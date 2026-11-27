/* Hope UI Engine - in-page text selection (drag select, double-click word,
 * select-all, copy to clipboard, paint-time highlight map)
 *
 * Selection endpoints are byte offsets inside DOM text nodes, so they survive
 * relayouts (the box tree is rebuilt but DOM nodes stay alive). A stale
 * endpoint (its node removed) simply stops matching during the map walk, which
 * degrades to an empty selection instead of crashing.
 */
#include "hope_internal.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "base/hpe_util.h"
#include "platform/hpe_platform.h"

/* multi-click recognition window: 0.4 s within 4 px of the previous press */
#define HPE_SEL_CLICK_DT 0.4
#define HPE_SEL_CLICK_D2 4

/**
 * @brief 判断字节是否为词分隔空白 / Test whether a byte is a word-separating space
 *
 * @param[in]     c 字节值 / Byte value
 *
 * @return 是分隔符返回 1，否则 0 / 1 when separating, otherwise 0
 *
 * @note 只匹配 ASCII 空白；UTF-8 多字节序列的续字节（>=0x80）按词内字符处理 / Only ASCII spaces match; UTF-8 continuation bytes (>=0x80) count as word characters
 */
static int sel_is_sep(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
}

/**
 * @brief 判断两个选区端点是否相同 / Test whether two selection endpoints are identical
 *
 * @param[in]     a 端点甲 / Endpoint A
 * @param[in]     b 端点乙 / Endpoint B
 *
 * @return 相同返回 1，否则 0 / 1 when identical, otherwise 0
 */
static int sel_pos_eq(const HpeSelPos *a, const HpeSelPos *b)
{
    return a->node == b->node && a->off == b->off;
}

/**
 * @brief 查询是否存在非折叠的页面选区 / Test whether a non-collapsed page selection exists
 *
 * @param[in]     engine 引擎实例 / Engine instance
 *
 * @return 有选区返回 1，否则 0 / 1 when a selection exists, otherwise 0
 */
int hpe_sel_has(const HpeEngine *engine)
{
    if (!engine || !engine->sel_anchor.node || !engine->sel_focus.node)
        return 0;
    return !sel_pos_eq(&engine->sel_anchor, &engine->sel_focus);
}

/**
 * @brief 求包含文档点的最深层带行盒 / Deepest line-bearing box containing a document point, in DFS order
 *
 * @param[in]     box 起始盒，递归子树 / Starting box, recursing into its subtree
 * @param[in]     x 文档 x / Document x
 * @param[in]     y 文档 y / Document y
 *
 * @return 命中的带行盒，无命中返回 NULL / The hit line-bearing box, NULL on a miss
 *
 * @note 文本实际挂在块盒的行上（文本盒自身无几何），因此按「有行的盒」命中；子树优先，与绘制次序一致 / Text lives on the block box's lines (text boxes carry no geometry), so the test targets line-bearing boxes; children are tested first, matching paint order
 */
static HpeBox *sel_hit_box(HpeBox *box, float x, float y)
{
    HpeBox *child, *r;
    if (!box)
        return NULL;
    for (child = box->first_child; child; child = child->next) {
        r = sel_hit_box(child, x, y);
        if (r)
            return r;
    }
    if (box->line_count > 0 &&
        x >= box->border_box.x && x <= box->border_box.x + box->border_box.w &&
        y >= box->border_box.y && y <= box->border_box.y + box->border_box.h)
        return box;
    return NULL;
}

/**
 * @brief 求一个 run 的文本所属的文本盒 / The text box a run's text belongs to
 *
 * @param[in]     box 起始盒，递归子树 / Starting box, recursing into its subtree
 * @param[in]     run 文本 run / The text run
 *
 * @return 拥有该 run 文本的文本盒，无则 NULL / The text box owning the run's text, NULL when none
 *
 * @note 行挂在块盒上而 run->text 指向文本盒文本；字面空格 run（折叠空格）不属于任何文本盒，会返回 NULL / Lines live on block boxes while run->text points into a text box; synthetic literal-space runs (collapsed spaces) belong to none and yield NULL
 */
static HpeBox *sel_run_owner(HpeBox *box, const HpeTextRun *run)
{
    HpeBox *child, *r;
    if (!box || !run->text)
        return NULL;
    if (box->kind == HPE_BOX_TEXT && box->node && box->text &&
        run->text >= box->text && run->text + run->len <= box->text + box->text_len)
        return box;
    for (child = box->first_child; child; child = child->next) {
        r = sel_run_owner(child, run);
        if (r)
            return r;
    }
    return NULL;
}

/**
 * @brief 把 run 内的 x 映射为字节偏移（按码点边界取整） / Map an x inside a run to a byte offset (snapped to code-point boundaries)
 *
 * @param[in]     run 文本 run / The text run
 * @param[in]     x 文档 x / Document x
 * @param[in]     fx run 起点文档 x / Document x of the run's start
 *
 * @return run 内的字节偏移 / Byte offset within the run
 *
 * @note 逐码点累加步进，x 落在字形前半取其起点、后半取其终点 / Accumulates advances per code point; the first half of a glyph snaps to its start, the second half to its end
 */
static int sel_run_boff(const HpeTextRun *run, float x, float fx)
{
    const char *p = run->text;
    const char *end = run->text + run->len;
    float acc = 0.0f;
    int boff = 0;
    while (p < end) {
        uint32_t cp;
        int k = hpe_utf8_next(p, &cp);
        float w = k > 0 ? hpe_font_advance(run->font, cp) : 0.0f;
        if (x <= fx + acc + w * 0.5f)
            break;
        acc += w;
        boff += k > 0 ? k : 1;
        p += k > 0 ? k : 1;
    }
    return boff;
}

/**
 * @brief 求盒内点的插入偏移（按码点边界取整） / The caret byte offset for a point inside a box (snapped to code-point boundaries)
 *
 * @param[in]     box 带行盒 / The line-bearing box
 * @param[in]     x 文档 x / Document x
 * @param[in]     y 文档 y / Document y
 * @param[out]    out 接收端点（节点 + 字节偏移）/ Receives the endpoint (node + byte offset)
 *
 * @return 定位成功返回 1，否则 0 / 1 when resolved, otherwise 0
 *
 * @note 先选包含 y 的行带（无则取最后一行），再把 x 映射到最近码点边界；行尾之外折到行末；字面空格 run 借助前后文回退定位 / The line band containing y is picked first (last line as fallback), then x maps to the nearest code-point boundary; beyond the line end clamps to it; literal space runs fall back to their neighbours
 */
static int sel_caret_at(HpeBox *box, float x, float y, HpeSelPos *out)
{
    HpeLine *line = NULL;
    int i, r;
    for (i = 0; i < box->line_count; i++) {
        line = &box->lines[i];
        if (y < line->y + line->height && line->run_count > 0)
            break;
    }
    if (!line)
        return 0;
    for (r = 0; r < line->run_count; r++) {
        HpeTextRun *run = &line->runs[r];
        HpeBox *owner;
        float fx;
        if (run->atomic || !run->font || !run->text)
            continue;
        fx = box->content_box.x + run->x;
        if (x > fx + run->width)
            continue;
        owner = sel_run_owner(box, run);
        if (!owner || !owner->text)
            continue;
        out->node = owner->node;
        out->off = (int)(run->text - owner->text) + sel_run_boff(run, x, fx);
        if (out->off > owner->text_len)
            out->off = owner->text_len;
        return 1;
    }
    /* past the last positioned run: clamp to the line's final real text */
    for (r = line->run_count - 1; r >= 0; r--) {
        HpeBox *owner = sel_run_owner(box, &line->runs[r]);
        if (owner && owner->text) {
            out->node = owner->node;
            out->off = (int)(line->runs[r].text - owner->text) + line->runs[r].len;
            if (out->off > owner->text_len)
                out->off = owner->text_len;
            return 1;
        }
    }
    return 0;
}

/**
 * @brief 把窗口坐标解析为选区端点 / Resolve a window point into a selection endpoint
 *
 * @param[in]     engine 引擎实例 / Engine instance
 * @param[in]     x X 坐标（窗口坐标）/ X coordinate in window space
 * @param[in]     y Y 坐标（窗口坐标）/ Y coordinate in window space
 * @param[out]    out 接收端点 / Receives the endpoint
 *
 * @return 命中文本返回 1，否则 0 / 1 when text was hit, otherwise 0
 *
 * @note 窗口 y 先减地址栏高度再加滚动偏移得到文档坐标 / The window y is first reduced by the chrome height then offset by the scroll to get document coordinates
 */
static int sel_hit_test(HpeEngine *engine, int x, int y, HpeSelPos *out)
{
    HpeBox *box;
    float cy = (float)y - (float)engine->chrome_h;
    if (cy < 0.0f)
        return 0;
    box = sel_hit_box(engine->root, (float)x, cy + engine->scroll_y);
    if (!box)
        return 0;
    if (getenv("HPE_TRACE_SEL"))
        fprintf(stderr, "selhit: block lines=%d box->text=\"%.16s\"\n", box->line_count,
                box->text ? box->text : "(null)");
    return sel_caret_at(box, (float)x, cy + engine->scroll_y, out);
}

/**
 * @brief 求偏移所在词的字节区间 / The byte range of the word around an offset
 *
 * @param[in]     text 文本 / The text
 * @param[in]     len 文本字节长度 / Byte length of the text
 * @param[in]     off 起始偏移 / The starting offset
 * @param[out]    ws 接收词首 / Receives the word start
 * @param[out]    we 接收词尾 / Receives the word end
 *
 * @note 偏移落在分隔符上时选中该分隔符；两端均按码点边界扩展 / A separator under the offset selects that separator; both ends expand on code-point boundaries
 */
static void sel_word_range(const char *text, int len, int off, int *ws, int *we)
{
    int s = off, e = off;
    while (s > 0 && !sel_is_sep((unsigned char)text[s - 1]))
        s--;
    while (e < len && !sel_is_sep((unsigned char)text[e]))
        e++;
    if (s == e && e < len) { /* the offset sits on a separator: take it */
        uint32_t cp;
        int k = hpe_utf8_next(text + e, &cp);
        e += k > 0 ? k : 1;
    }
    *ws = s;
    *we = e;
}

/**
 * @brief 求一个 DOM 文本节点的当前文本长度 / The current text length of a DOM text node
 *
 * @param[in]     node 文本节点 / The text node
 *
 * @return 字节长度，空节点返回 0 / Byte length, 0 for an empty node
 */
static int sel_node_len(const HpeDomNode *node)
{
    return node->text ? (int)strlen(node->text) : 0;
}

/**
 * @brief 求文本盒所属的块级祖先 / The block-level ancestor of a text box
 *
 * @param[in]     box 起始盒 / The starting box
 *
 * @return 最近的块盒或原子行内块盒，无则 NULL / The nearest block or atomic inline-block box, NULL when none
 *
 * @note 供复制时判断「跨块换行」 / Used by copy to decide block-boundary newlines
 */
static const HpeBox *sel_block_of(const HpeBox *box)
{
    for (; box; box = box->parent)
        if (box->kind == HPE_BOX_BLOCK || box->kind == HPE_BOX_INLINE_BLOCK)
            return box;
    return NULL;
}

/**
 * @brief 处理页面上的指针按下（开始/扩展/双击选词） / Handle a pointer press on the page (start/extend/double-click word select)
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     x X 坐标（窗口坐标）/ X coordinate in window space
 * @param[in]     y Y 坐标（窗口坐标）/ Y coordinate in window space
 * @param[in]     mods 修饰键掩码，`HPE_MOD_SHIFT` 扩展既有选区 / Modifier mask; `HPE_MOD_SHIFT` extends an existing selection
 *
 * @return 选区状态发生变化返回 1，否则 0 / 1 when the selection changed, otherwise 0
 *
 * @note Shift+点击把焦点端点移到点击处；未命中文本时折叠既有选区；双击选中光标处的词并允许继续拖拽扩展整词 / Shift+click moves the focus endpoint to the click; a miss collapses the selection; a double click selects the word and keeps dragging to extend by words
 */
int hpe_sel_mouse_down(HpeEngine *engine, int x, int y, int mods)
{
    HpeSelPos pos, old_a, old_f;
    int hit;
    if (!engine || !engine->root)
        return 0;
    old_a = engine->sel_anchor;
    old_f = engine->sel_focus;
    engine->sel_drag = 0;
    hit = sel_hit_test(engine, x, y, &pos);
    if (hit && (mods & HPE_MOD_SHIFT) && hpe_sel_has(engine)) {
        engine->sel_focus = pos;
        engine->sel_drag = 1;
        return !sel_pos_eq(&old_f, &pos);
    }
    if (!hit) {
        engine->sel_anchor.node = engine->sel_focus.node = NULL;
        engine->sel_anchor.off = engine->sel_focus.off = 0;
        return (old_a.node || old_f.node) ? 1 : 0;
    }
    {
        double now = hpe_time_seconds();
        if (now - engine->sel_click_t < HPE_SEL_CLICK_DT &&
            abs(x - engine->sel_click_x) < HPE_SEL_CLICK_D2 &&
            abs(y - engine->sel_click_y) < HPE_SEL_CLICK_D2)
            engine->sel_click_count++;
        else
            engine->sel_click_count = 1;
        engine->sel_click_t = now;
        engine->sel_click_x = x;
        engine->sel_click_y = y;
    }
    if (engine->sel_click_count >= 2) { /* double/triple click: whole word */
        HpeDomNode *node = pos.node;
        int ws, we;
        sel_word_range(node->text ? node->text : "", sel_node_len(node), pos.off, &ws, &we);
        engine->sel_anchor.node = engine->sel_focus.node = node;
        engine->sel_anchor.off = ws;
        engine->sel_focus.off = we;
    } else {
        engine->sel_anchor = pos;
        engine->sel_focus = pos;
    }
    engine->sel_drag = 1;
    return !sel_pos_eq(&old_a, &engine->sel_anchor) || !sel_pos_eq(&old_f, &engine->sel_focus);
}

/**
 * @brief 拖选进行中处理指针移动（扩展焦点端点） / Handle a pointer move while dragging (extends the focus endpoint)
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     x X 坐标（窗口坐标）/ X coordinate in window space
 * @param[in]     y Y 坐标（窗口坐标）/ Y coordinate in window space
 *
 * @return 选区变化返回 1，否则 0 / 1 when the selection changed, otherwise 0
 *
 * @note 只有按下后开始的拖拽才扩展；未命中文本时保持原焦点 / Only a drag started by a press extends; a miss keeps the previous focus
 */
int hpe_sel_mouse_move(HpeEngine *engine, int x, int y)
{
    HpeSelPos pos;
    if (!engine || !engine->sel_drag || !engine->root)
        return 0;
    if (!sel_hit_test(engine, x, y, &pos))
        return 0;
    if (sel_pos_eq(&pos, &engine->sel_focus))
        return 0;
    engine->sel_focus = pos;
    return 1;
}

/**
 * @brief 结束拖选 / End a selection drag
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 */
void hpe_sel_mouse_up(HpeEngine *engine)
{
    if (engine)
        engine->sel_drag = 0;
}

/**
 * @brief 清除页面选区 / Clear the page selection
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @return 原有选区被清除返回 1，本无选区返回 0 / 1 when an existing selection was cleared, 0 when there was none
 */
int hpe_sel_clear(HpeEngine *engine)
{
    if (!engine || !(engine->sel_anchor.node || engine->sel_focus.node))
        return 0;
    engine->sel_anchor.node = engine->sel_focus.node = NULL;
    engine->sel_anchor.off = engine->sel_focus.off = 0;
    engine->sel_drag = 0;
    return 1;
}

/**
 * @brief 求深度优先序下的第一个文本盒 / The first text box in DFS order
 *
 * @param[in]     box 起始盒 / Starting box
 *
 * @return 第一个带节点的文本盒，无则 NULL / The first node-backed text box, NULL when none
 */
static HpeBox *sel_text_first(HpeBox *box)
{
    HpeBox *child, *r;
    if (!box)
        return NULL;
    if (box->kind == HPE_BOX_TEXT && box->node && box->text_len > 0)
        return box;
    for (child = box->first_child; child; child = child->next) {
        r = sel_text_first(child);
        if (r)
            return r;
    }
    return NULL;
}

/**
 * @brief 求深度优先序下的最后一个文本盒 / The last text box in DFS order
 *
 * @param[in]     box 起始盒，递归子树 / Starting box, recursing into its subtree
 * @param[in,out] last 追踪当前最靠后的命中 / Tracks the latest hit so far
 *
 * @return 最后一个带节点的文本盒，无则 NULL / The last node-backed text box, NULL when none
 *
 * @note HpeBox 的兄弟链是单向的，因此用正向遍历持续覆盖 last，等效反向深度优先 / The sibling chain is singly linked, so a forward walk that keeps overwriting `last` is equivalent to a reversed DFS
 */
static void sel_text_last(HpeBox *box, HpeBox **last)
{
    HpeBox *child;
    if (!box)
        return;
    if (box->kind == HPE_BOX_TEXT && box->node && box->text_len > 0)
        *last = box;
    for (child = box->first_child; child; child = child->next)
        sel_text_last(child, last);
}

/**
 * @brief 全选文档文本 / Select all document text
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @return 文档含文本且选区变化返回 1，否则 0 / 1 when the document has text and the selection changed, otherwise 0
 */
int hpe_sel_select_all(HpeEngine *engine)
{
    HpeBox *first, *last = NULL;
    if (!engine || !engine->root)
        return 0;
    first = sel_text_first(engine->root);
    if (!first)
        return 0;
    sel_text_last(engine->root, &last);
    if (!last)
        last = first;
    engine->sel_anchor.node = first->node;
    engine->sel_anchor.off = 0;
    engine->sel_focus.node = last->node;
    engine->sel_focus.off = last->text_len;
    engine->sel_drag = 0;
    return 1;
}

/**
 * @brief 向选区区间表追加一个条目 / Append one entry to the selection table
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     box 文本盒 / The text box
 * @param[in]     start 起始字节（含）/ Start byte, inclusive
 * @param[in]     end 结束字节（不含）/ End byte, exclusive
 *
 * @note 必要时扩容；越界或空区间直接忽略 / Grows when needed; out-of-range or empty ranges are ignored
 */
static void sel_map_push(HpeEngine *engine, const HpeBox *box, int start, int end)
{
    HpePaintSelEntry *e;
    if (start < 0 || end <= start)
        return;
    if (start > (int)box->text_len)
        return;
    if (end > box->text_len)
        end = box->text_len;
    if (end <= start)
        return;
    engine->sel_map = (HpePaintSelEntry *)hpe_grow(engine->sel_map, &engine->sel_map_cap,
                                                   (size_t)engine->sel_map_count + 1,
                                                   sizeof(HpePaintSelEntry));
    if (!engine->sel_map)
        return;
    e = &engine->sel_map[engine->sel_map_count++];
    e->box = box;
    e->start = start;
    e->end = end;
}

/**
 * @brief 判定锚点与焦点节点在文档序中的先后 / Which endpoint node comes first in document order
 *
 * @param[in]     root 盒子树根 / Box tree root
 * @param[in]     a 锚点 / The anchor
 * @param[in]     f 焦点 / The focus
 *
 * @return 锚点在前返回 1，焦点在前返回 -1，同节点或未同时找到返回 0 / 1 anchor first, -1 focus first, 0 same node or not both found
 */
static int sel_map_order(HpeBox *root, const HpeSelPos *a, const HpeSelPos *f)
{
    HpeBox *child;
    if (!root)
        return 0;
    if (root->kind == HPE_BOX_TEXT && root->node) {
        if (root->node == a->node && root->node == f->node)
            return 0;
        if (root->node == a->node)
            return 1;
        if (root->node == f->node)
            return -1;
    }
    for (child = root->first_child; child; child = child->next) {
        int r = sel_map_order(child, a, f);
        if (r)
            return r;
    }
    return 0;
}

/**
 * @brief 按文档序遍历盒子树并生成选区区间 / Walk the box tree in document order emitting selected ranges
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     box 当前盒 / Current box
 * @param[in]     p1 文档序靠前的端点 / The document-first endpoint
 * @param[in]     p2 文档序靠后的端点 / The document-second endpoint
 * @param[in,out] state 0=未见到端点 1=仅见 p1 2=完毕 / 0 none seen, 1 only p1 seen, 2 done
 *
 * @note 完全覆盖的盒记 [0, text_len]；端点盒记部分区间 / Fully covered boxes record [0, text_len]; endpoint boxes record partial ranges
 */
static void sel_map_walk(HpeEngine *engine, HpeBox *box, const HpeSelPos *p1,
                         const HpeSelPos *p2, int *state)
{
    HpeBox *child;
    if (!box || *state == 2)
        return;
    for (child = box->first_child; child; child = child->next)
        sel_map_walk(engine, child, p1, p2, state);
    if (box->kind != HPE_BOX_TEXT || !box->node || box->text_len <= 0)
        return;
    if (*state == 0 && box->node == p1->node && box->node == p2->node) {
        int lo = p1->off < p2->off ? p1->off : p2->off;
        int hi = p1->off > p2->off ? p1->off : p2->off;
        sel_map_push(engine, box, lo, hi);
        *state = 2;
        return;
    }
    if (*state == 0 && box->node == p1->node) {
        sel_map_push(engine, box, p1->off, box->text_len);
        *state = 1;
        return;
    }
    if (*state == 1 && box->node == p2->node) {
        sel_map_push(engine, box, 0, p2->off);
        *state = 2;
        return;
    }
    if (*state == 1)
        sel_map_push(engine, box, 0, box->text_len);
}

/**
 * @brief 同节点选区：遍历盒子树找到该文本盒并记录区间 / Same-node selection: find the text box and record the range
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     box 当前盒，递归子树 / Current box, recursing into its subtree
 * @param[in]     node 两端点共同的文本节点 / The text node shared by both endpoints
 * @param[in]     lo 较小的字节偏移 / The smaller byte offset
 * @param[in]     hi 较大的字节偏移 / The larger byte offset
 */
static void sel_map_walk_same(HpeEngine *engine, HpeBox *box, const HpeDomNode *node, int lo,
                              int hi)
{
    HpeBox *child;
    if (!box)
        return;
    if (box->kind == HPE_BOX_TEXT && box->node == node) {
        sel_map_push(engine, box, lo, hi);
        return;
    }
    for (child = box->first_child; child; child = child->next)
        sel_map_walk_same(engine, child, node, lo, hi);
}

/**
 * @brief 重建逐盒选区区间表并交给绘制模块 / Rebuild the per-box selection table and hand it to the paint module
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @note 每次重绘页面前调用；两端点同节点时直接记单条区间，否则先判定先后再走一遍盒子树；选区失效（节点被移除）时自然产生空表 / Called before every page repaint; a same-node selection records one range directly, otherwise endpoint order is decided first and the tree walked once; a stale selection naturally yields an empty table
 */
void hpe_sel_prepare(HpeEngine *engine)
{
    int state = 0, dir;
    if (!engine)
        return;
    engine->sel_map_count = 0;
    dir = 0;
    if (hpe_sel_has(engine) && engine->root) {
        if (engine->sel_anchor.node == engine->sel_focus.node) {
            int lo = engine->sel_anchor.off < engine->sel_focus.off ? engine->sel_anchor.off
                                                                    : engine->sel_focus.off;
            int hi = engine->sel_anchor.off > engine->sel_focus.off ? engine->sel_anchor.off
                                                                    : engine->sel_focus.off;
            sel_map_walk_same(engine, engine->root, engine->sel_anchor.node, lo, hi);
        } else {
            dir = sel_map_order(engine->root, &engine->sel_anchor, &engine->sel_focus);
            if (dir != 0) {
                const HpeSelPos *p1 = dir > 0 ? &engine->sel_anchor : &engine->sel_focus;
                const HpeSelPos *p2 = dir > 0 ? &engine->sel_focus : &engine->sel_anchor;
                sel_map_walk(engine, engine->root, p1, p2, &state);
            }
        }
    }
    engine->sel_ps.entries = engine->sel_map;
    engine->sel_ps.count = engine->sel_map_count;
    hpe_paint_set_selection(engine->sel_map_count ? &engine->sel_ps : NULL);
    if (getenv("HPE_TRACE_SEL")) {
        int i;
        fprintf(stderr, "sel: a=node#%s off=%d f=node#%s off=%d entries=%d\n",
                engine->sel_anchor.node && engine->sel_anchor.node->text
                    ? engine->sel_anchor.node->text : "?",
                engine->sel_anchor.off,
                engine->sel_focus.node && engine->sel_focus.node->text
                    ? engine->sel_focus.node->text : "?",
                engine->sel_focus.off, engine->sel_map_count);
        for (i = 0; i < engine->sel_map_count && i < 6; i++)
            fprintf(stderr, "  [%d] len=%d [%d,%d) \"%.20s\"\n", i,
                    engine->sel_map[i].box->text_len, engine->sel_map[i].start,
                    engine->sel_map[i].end, engine->sel_map[i].box->text);
    }
}

/**
 * @brief 无条件丢弃选区状态（换页时调用，不触发重绘） / Drop the selection state unconditionally (on page load, no repaint)
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @note 旧文档的 DOM 节点即将销毁，端点引用必须清空 / The old document's DOM nodes are about to be freed, so the endpoints must be cleared
 *
 * @see hpe_sel_shutdown
 */
void hpe_sel_reset(HpeEngine *engine)
{
    if (!engine)
        return;
    engine->sel_anchor.node = engine->sel_focus.node = NULL;
    engine->sel_anchor.off = engine->sel_focus.off = 0;
    engine->sel_drag = 0;
    engine->sel_map_count = 0;
}

/**
 * @brief 释放选区模块的缓冲 / Free the selection module's buffers
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 */
void hpe_sel_shutdown(HpeEngine *engine)
{
    if (!engine)
        return;
    hpe_free(engine->sel_map);
    engine->sel_map = NULL;
    engine->sel_map_count = 0;
    engine->sel_map_cap = 0;
    hpe_sel_reset(engine);
}

/**
 * @brief 把选区文本写入剪贴板 / Copy the selected text to the clipboard
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @return 写入成功返回 1，无选区或文本为空返回 0 / 1 when written, 0 without a selection or when the text is empty
 *
 * @note 以重绘用区间表为副本来源（文档序），块级祖先变化处补换行，与浏览器复制行为一致 / Copies from the paint-time range table (document order), inserting a newline where the block ancestor changes, matching browser copy behaviour
 */
int hpe_sel_copy(HpeEngine *engine)
{
    char *buf = NULL;
    size_t cap = 0, used = 0;
    const HpeBox *prev_block = NULL;
    int i, ok;
    if (!engine || !engine->root || !hpe_sel_has(engine))
        return 0;
    hpe_sel_prepare(engine);
    for (i = 0; i < engine->sel_map_count; i++) {
        const HpePaintSelEntry *e = &engine->sel_map[i];
        const HpeBox *blk = sel_block_of((HpeBox *)e->box);
        size_t need;
        if (prev_block && blk != prev_block) {
            buf = (char *)hpe_grow(buf, &cap, used + 2, 1);
            if (buf)
                buf[used++] = '\n';
        }
        prev_block = blk;
        need = used + (size_t)(e->end - e->start) + 2;
        buf = (char *)hpe_grow(buf, &cap, need, 1);
        if (!buf)
            break;
        memcpy(buf + used, e->box->text + e->start, (size_t)(e->end - e->start));
        used += (size_t)(e->end - e->start);
    }
    ok = 0;
    if (buf) {
        buf[used] = '\0';
        if (used > 0)
            ok = hpe_clipboard_set_text(buf);
        hpe_free(buf);
    }
    return ok;
}
