#include "hope.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/hpe_util.h"
#include "css/hpe_css.h"
#include "dom/hpe_dom.h"
#include "hope_internal.h"
#include "html/hpe_encoding.h"
#include "paint/hpe_svg.h"
#include "html/hpe_html.h"
#include "js/hpe_js.h"
#include "js/hpe_js_dom.h"
#include "layout/hpe_layout.h"
#include "paint/hpe_paint.h"
#include "platform/hpe_platform.h"

#define HPE_MAX_SCRIPTS 64

/* built-in "blank page" shown when the engine is started without a file */
static const char BLANK_PAGE[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>blank</title>"
    "<style>"
    "html,body{margin:0;padding:0;background:#ffffff;}"
    ".hint{margin:0;padding:56px 0 0 0;text-align:center;color:#b3bac4;"
    "font-family:sans-serif;font-size:14px;}"
    "</style></head><body>"
    "<p class=\"hint\">blank page \xE2\x80\x94 "
    "\xE5\x9C\xA8\xE4\xB8\x8A\xE6\x96\xB9\xE5\x9C\xB0\xE5\x9D\x80\xE6\xA0\x8F"
    "\xE8\xBE\x93\xE5\x85\xA5\xE6\x9C\xAC\xE5\x9C\xB0 HTML "
    "\xE6\x96\x87\xE4\xBB\xB6\xE8\xB7\xAF\xE5\xBE\x84\xEF\xBC\x8C"
    "\xE5\x9B\x9E\xE8\xBD\xA6\xE6\x89\x93\xE5\xBC\x80</p>"
    "</body></html>";

static int click_element_by_id(HpeEngine *engine, const char *id);
static void print_text_by_id(HpeEngine *engine, const char *id);

/* ---------------- stylesheets ---------------- */

/**
 * @brief 查找文档的 `<base href>` / Find the document's `<base href>`
 *
 * @param[in,out] node 开始查找的 DOM 节点，递归其子树 / DOM node to search from, recursing into its subtree
 *
 * @return 首个非空 `href` 的指针（由 DOM 拥有），未找到返回 `NULL` / Pointer to the first non-empty `href` owned by the DOM, `NULL` when none
 *
 * @note 深度优先遍历；按规范首个带非空 href 的 `<base>` 生效 / Depth-first; per spec the first `<base>` with a non-empty href wins
 */
static const char *find_base_href(HpeDomNode *node)
{
    HpeDomNode *child;
    for (child = node->first_child; child; child = child->next_sibling) {
        if (child->type == HPE_NODE_ELEMENT) {
            const char *base = NULL;
            if (strcmp(child->tag, "base") == 0)
                base = hpe_dom_attr(child, "href");
            if (base && *base)
                return base;
            {
                const char *found = find_base_href(child);
                if (found)
                    return found;
            }
        }
    }
    return NULL;
}

/**
 * @brief 把文档相对地址解析为绝对路径 / Resolve a document-relative reference to an absolute path
 *
 * @param[in,out] engine 引擎实例，提供 `<base href>` / Engine instance, supplies `<base href>`
 * @param[out]    out 接收绝对路径的缓冲区 / Buffer that receives the absolute path
 * @param[in]     out_size 输出缓冲区容量（字节）/ Capacity of the output buffer in bytes
 * @param[in]     dir 文档所在目录 / Directory of the document
 * @param[in]     rel 待解析的相对引用 / Relative reference to resolve
 *
 * @note 存在 `<base href>` 且 `rel` 非绝对时以 base 所在目录为基准，否则用文档目录 / Uses the base href's directory when a base is present and `rel` is not absolute, otherwise the document directory
 */
static void resolve_url(HpeEngine *engine, char *out, size_t out_size, const char *dir,
                        const char *rel)
{
    if (engine->base_href[0] && !hpe_path_is_absolute(rel)) {
        char basedir[HPE_MAX_PATH];
        hpe_path_dir(engine->base_href, basedir, sizeof(basedir));
        hpe_path_join(out, out_size, basedir, rel);
    } else {
        hpe_path_join(out, out_size, dir, rel);
    }
}

/**
 * @brief 把 rel 属性规范成小写、空格分隔的 token 串 / Normalise a rel attribute into a lowercase space-separated token string
 *
 * @param[in]     rel 待规范的 rel 属性值 / rel attribute value to normalise
 * @param[out]    out 接收规范串的缓冲区 / Buffer that receives the normalised string
 * @param[in]     out_size 输出缓冲区容量（字节）/ Capacity of the output buffer in bytes
 *
 * @note 合并连续空白、去首尾空白并转小写，使 `rel="ICON"` 与 `rel="shortcut icon"` 可预测比较 / Collapses runs of whitespace, trims it and lowercases, so `rel="ICON"` and `rel="shortcut icon"` compare predictably
 */
static void normalize_rel(const char *rel, char *out, size_t out_size)
{
    size_t o = 0;
    int last_space = 1;
    for (; rel && *rel && o + 1 < out_size; rel++) {
        if (hpe_is_space(*rel)) {
            if (!last_space) {
                out[o++] = ' ';
                last_space = 1;
            }
        } else {
            out[o++] = (char)hpe_ascii_lower((unsigned char)*rel);
            last_space = 0;
        }
    }
    while (o > 0 && out[o - 1] == ' ')
        o--;
    out[o] = '\0';
}

/**
 * @brief 判断 rel 串是否含指定 token / Test whether a rel string contains a given token
 *
 * @param[in]     rel 规范化的 rel token 串 / Normalised rel token string
 * @param[in]     token 要查找的 token（不区分大小写）/ Token to look for (case-insensitive)
 *
 * @return 含该 token 返回 1，否则 0 / 1 when the token is present, otherwise 0
 *
 * @note 按空格分词后逐段比较 / Splits on spaces and compares each field
 */
static int rel_has_token(const char *rel, const char *token)
{
    const char *p = rel;
    size_t tl = strlen(token);
    while (*p) {
        while (*p == ' ')
            p++;
        if (hpe_str_ieq_n(p, token, tl) && (p[tl] == ' ' || p[tl] == '\0'))
            return 1;
        while (*p && *p != ' ')
            p++;
    }
    return 0;
}

/**
 * @brief 递归收集 `<style>` 规则与 `<link rel=stylesheet>` 外链样式 / Recursively collect `<style>` rules and `<link rel=stylesheet>` external sheets
 *
 * @param[in,out] engine 引擎实例，规则累加进 `sheet` / Engine instance, rules are appended to `sheet`
 * @param[in,out] node 开始遍历的 DOM 节点 / DOM node to walk from
 * @param[in]     dir 解析外链 href 的基准目录 / Base directory used to resolve external hrefs
 *
 * @note 非 stylesheet 的 `<link>` 记入资源提示表（上限 `HPE_MAX_LINKS`）；外链读取失败打印到 stderr 后跳过 / Non-stylesheet `<link>`s are recorded as resource hints (capped at `HPE_MAX_LINKS`); a failed external read is reported on stderr and skipped
 */
static void collect_styles(HpeEngine *engine, HpeDomNode *node, const char *dir)
{
    HpeDomNode *child;

    for (child = node->first_child; child; child = child->next_sibling) {
        if (child->type != HPE_NODE_ELEMENT)
            continue;

        if (strcmp(child->tag, "style") == 0) {
            HpeDomNode *text = child->first_child;
            if (text && text->type == HPE_NODE_TEXT && text->text)
                hpe_css_parse(&engine->sheet, text->text, strlen(text->text));
        } else if (strcmp(child->tag, "link") == 0) {
            const char *rel = hpe_dom_attr(child, "rel");
            const char *href = hpe_dom_attr(child, "href");
            if (rel && href) {
                if (rel_has_token(rel, "stylesheet")) {
                    char full[HPE_MAX_PATH];
                    char *css;
                    resolve_url(engine, full, sizeof(full), dir, href);
                    css = hpe_read_file(full, NULL);
                    if (css) {
                        hpe_css_parse(&engine->sheet, css, strlen(css));
                        hpe_free(css);
                    } else {
                        fprintf(stderr, "hope: cannot open stylesheet %s\n", full);
                    }
                } else if (engine->link_count < HPE_MAX_LINKS) {
                    /* record non-stylesheet resource hints (icon/preload/...) */
                    int idx = engine->link_count++;
                    char tokens[24];
                    normalize_rel(rel, tokens, sizeof(tokens));
                    snprintf(engine->links[idx].rel, sizeof(engine->links[idx].rel), "%s",
                             tokens);
                    resolve_url(engine, engine->links[idx].href,
                                sizeof(engine->links[idx].href), dir, href);
                }
            }
        }
        collect_styles(engine, child, dir);
    }
}

/* A script can add/remove a <style>/<link> or rewrite a <style>'s text after the
 * first collection. The DOM sink below reports exactly those edits; a later
 * relayout then rebuilds the author rules. The sink fires before the sheet is
 * touched, so it only records "a sheet-affecting change happened" + a signature
 * of the last node involved; the real re-read happens in refresh_styles(). */
static HpeDomNode *g_style_sig;
static unsigned int g_style_rev;
static int g_style_dirty;

/**
 * @brief 判断节点是否会增删改样式表 / Test whether a node can add, remove or edit the stylesheet
 *
 * @param[in,out] n 待检查的 DOM 节点 / DOM node to inspect
 *
 * @return 是 `<style>`/`<link>` 元素或其文本子节点返回 1，否则 0 / 1 for a `<style>`/`<link>` element or its text child, otherwise 0
 */
static int is_style_host(HpeDomNode *n)
{
    if (!n)
        return 0;
    if (n->type == HPE_NODE_ELEMENT)
        return strcmp(n->tag, "style") == 0 || strcmp(n->tag, "link") == 0;
    if (n->type == HPE_NODE_TEXT && n->parent)
        return strcmp(n->parent->tag, "style") == 0;
    return 0;
}

/* ---------------- mutation classification ----------------
 * Every DOM change reports here. Two consumers share the record:
 *   - the author-sheet tracker (a <style>/<link> touched -> re-read the sheet)
 *   - the reflow tracker (how much layout/repaint the change needs)
 * The reflow level only ever ratchets up; flush_mutations() acts on it and
 * then resets. */

/**
 * @brief 按属性位集判定所需重排级别 / Classify the reflow level a property bit set requires
 *
 * @details 保守策略：除非每个变更位都确知为纯绘制，整组声明按 `HPE_REFLOW_LAYOUT` 处理 / Conservative: the whole declaration set is treated as `HPE_REFLOW_LAYOUT` unless every changed bit is known to be paint-only.
 *
 * @param[in]     set 变更涉及的属性位集 / Bit set of the changed properties
 * @param[in]     set2 变更涉及的第二组属性位集 / Second bit set of the changed properties
 *
 * @return 所需重排级别 `HPE_REFLOW_*` / The `HPE_REFLOW_*` level required
 * @retval HPE_REFLOW_LAYOUT 任一变更属性影响几何（盒度量/位置/流/排版）/ A changed property affects geometry (box metrics, position, flow, typography)
 * @retval HPE_REFLOW_PAINT 仅纯视觉属性（颜色/不透明度/阴影）/ Only purely visual properties (colour, opacity, shadow)
 * @retval HPE_REFLOW_NONE 无变更 / Nothing changed
 *
 * @see reflow_for_inline_style reflow_for_attr
 */
static int reflow_for_props(uint64_t set, uint64_t set2)
{
    /* geometry: box metrics, position, flow, typography (all change line boxes) */
    const uint64_t geom =
        HPE_PROP_DISPLAY | HPE_PROP_WIDTH | HPE_PROP_HEIGHT | HPE_PROP_MARGIN_TOP |
        HPE_PROP_MARGIN_RIGHT | HPE_PROP_MARGIN_BOTTOM | HPE_PROP_MARGIN_LEFT |
        HPE_PROP_PADDING_TOP | HPE_PROP_PADDING_RIGHT | HPE_PROP_PADDING_BOTTOM |
        HPE_PROP_PADDING_LEFT | HPE_PROP_BORDER_WIDTH | HPE_PROP_FONT_SIZE |
        HPE_PROP_FONT_WEIGHT | HPE_PROP_TEXT_ALIGN | HPE_PROP_LINE_HEIGHT |
        HPE_PROP_WHITE_SPACE | HPE_PROP_FONT_FAMILY | HPE_PROP_POSITION |
        HPE_PROP_INSET_TOP | HPE_PROP_INSET_RIGHT | HPE_PROP_INSET_BOTTOM |
        HPE_PROP_INSET_LEFT | HPE_PROP_Z_INDEX | HPE_PROP_FLOAT | HPE_PROP_CLEAR |
        HPE_PROP_BORDER_RADIUS | HPE_PROP_BORDER_TOP | HPE_PROP_BORDER_RIGHT |
        HPE_PROP_BORDER_BOTTOM | HPE_PROP_BORDER_LEFT | HPE_PROP_FLEX_DIR |
        HPE_PROP_ALIGN_ITEMS | HPE_PROP_JUSTIFY | HPE_PROP_GAP | HPE_PROP_FLEX_GROW |
        HPE_PROP_ALIGN_SELF | HPE_PROP_MIN_WIDTH;
    const uint64_t geom2 = HPE_PROP2_MIN_HEIGHT | HPE_PROP2_MAX_WIDTH | HPE_PROP2_MAX_HEIGHT |
                           HPE_PROP2_BOX_SIZING | HPE_PROP2_OVERFLOW_X | HPE_PROP2_OVERFLOW_Y |
                           HPE_PROP2_VISIBILITY | HPE_PROP2_LETTER_SPACING |
                           HPE_PROP2_WORD_SPACING | HPE_PROP2_TEXT_INDENT |
                           HPE_PROP2_VERTICAL_ALIGN | HPE_PROP2_LIST_STYLE_TYPE |
                           HPE_PROP2_LIST_STYLE_POSITION | HPE_PROP2_BACKGROUND_IMAGE |
                           HPE_PROP2_TRANSFORM;
    if ((set & geom) || (set2 & geom2))
        return HPE_REFLOW_LAYOUT;
    if (set || set2)
        return HPE_REFLOW_PAINT;
    return HPE_REFLOW_NONE;
}

/**
 * @brief 比较 style 属性新旧值得出重排级别 / Compare old and new `style` attribute values to get the reflow level
 *
 * @param[in]     old_value 变更前的 style 值，可为 NULL / Previous style value, may be NULL
 * @param[in]     new_value 变更后的 style 值，可为 NULL / New style value, may be NULL
 *
 * @return 所需重排级别 `HPE_REFLOW_*` / The `HPE_REFLOW_*` level required
 *
 * @note 两份解析后的声明位掩码完全一致时返回 `HPE_REFLOW_NONE` / Returns `HPE_REFLOW_NONE` when both parsed declaration sets carry identical bit masks
 *
 * @see reflow_for_props
 */
static int reflow_for_inline_style(const char *old_value, const char *new_value)
{
    HpeStyle a, b;
    a = hpe_style_default();
    b = hpe_style_default();
    if (old_value)
        hpe_css_parse_declarations(old_value, strlen(old_value), &a);
    if (new_value)
        hpe_css_parse_declarations(new_value, strlen(new_value), &b);
    if (a.set == b.set && a.set2 == b.set2)
        return HPE_REFLOW_NONE;
    return reflow_for_props(a.set ^ b.set, a.set2 ^ b.set2);
}

/**
 * @brief 判定某属性变更所需的重排级别 / Classify the reflow level an attribute change requires
 *
 * @param[in,out] target 属性所在 DOM 元素 / DOM element the attribute belongs to
 * @param[in]     name 属性名 / Attribute name
 * @param[in]     old_value 变更前的属性值 / Previous attribute value
 *
 * @return 所需重排级别 `HPE_REFLOW_*` / The `HPE_REFLOW_*` level required
 *
 * @note `<svg>` 元素（几何存于属性）与 `class`/`id` 变更按 `HPE_REFLOW_LAYOUT`；`style` 走内联比较；其余属性对本引擎无影响返回 `HPE_REFLOW_NONE` / `<svg>` (geometry lives in its attributes) and `class`/`id` changes yield `HPE_REFLOW_LAYOUT`; `style` goes through the inline comparison; other attributes are inert here and give `HPE_REFLOW_NONE`
 *
 * @see reflow_for_inline_style
 */
static int reflow_for_attr(HpeDomNode *target, const char *name, const char *old_value)
{
    const char *new_value;
    if (!target || !name)
        return HPE_REFLOW_LAYOUT;
    if (target->type == HPE_NODE_ELEMENT && target->tag && strcmp(target->tag, "svg") == 0)
        return HPE_REFLOW_LAYOUT; /* SVG geometry lives in attributes */
    /* `style` is the only attribute that reaches the cascade; class/id change
     * which rules match; everything else is inert for this engine. */
    if (hpe_str_ieq(name, "style")) {
        new_value = hpe_dom_attr(target, name);
        return reflow_for_inline_style(old_value, new_value);
    }
    if (hpe_str_ieq(name, "class") || hpe_str_ieq(name, "id"))
        return HPE_REFLOW_LAYOUT;
    return HPE_REFLOW_NONE;
}

/**
 * @brief 把待重排级别抬升到 `need` / Ratchet the pending reflow level up to `need`
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     need 要达到的重排级别 `HPE_REFLOW_*` / Reflow level to ratchet up to (`HPE_REFLOW_*`)
 *
 * @note 只抬升不下降；`hpe_engine_flush` 处理后统一清零 / Only ratchets up, never down; `hpe_engine_flush` resets it after acting
 *
 * @see hpe_engine_flush
 */
static void note_reflow(HpeEngine *engine, int need)
{
    if (!engine || need <= engine->reflow_need)
        return;
    engine->reflow_need = need;
}

/**
 * @brief 判断 `anc` 是否为 `node` 自身或其祖先 / Test whether `anc` is `node` itself or one of its ancestors
 *
 * @param[in,out] anc 候选祖先节点 / Candidate ancestor node
 * @param[in,out] node 被检查的节点 / Node being inspected
 *
 * @return 是返回 1，否则 0 / 1 when it is, otherwise 0
 */
static int dom_is_ancestor(HpeDomNode *anc, HpeDomNode *node)
{
    for (; node; node = node->parent)
        if (node == anc)
            return 1;
    return 0;
}

/**
 * @brief 记录需局部重建的盒子树对应的 DOM 节点 / Record the DOM node whose box subtree an incremental rebuild must replace
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in,out] node 变更涉及的 DOM 节点 / DOM node involved in the change
 *
 * @note 只保留最靠近根的外层候选，深入已记录子树内的变更不会缩小作用域；文本节点上溯到元素；无父的分离节点无盒信息被忽略 / Keeps the outermost candidate so a change nested inside an already-recorded subtree does not narrow the scope; a text node walks up to its element; a detached node (no parent) carries no box information and is ignored
 *
 * @see hpe_engine_flush
 */
static void note_reflow_node(HpeEngine *engine, HpeDomNode *node)
{
    if (!engine || !node)
        return;
    if (node->type == HPE_NODE_TEXT)
        node = node->parent;
    while (node && node->type != HPE_NODE_ELEMENT)
        node = node->parent;
    if (!node || node->type != HPE_NODE_ELEMENT || !node->parent)
        return; /* detached: no box to splice against (a full build covers it) */
    if (!engine->reflow_node || dom_is_ancestor(node, engine->reflow_node))
        engine->reflow_node = node;
}

/**
 * @brief DOM 变更汇聚点：分类重排并跟踪样式表编辑 / DOM mutation sink: classify the reflow and track stylesheet edits
 *
 * @param[in,out] ud 宿主回调透传的引擎实例 / Opaque user data, the engine instance
 * @param[in]     kind 变更种类 `HPE_MUT_*` / Mutation kind (`HPE_MUT_*`)
 * @param[in,out] target 变更的目标 DOM 节点 / DOM node the mutation targets
 * @param[in]     name 变更的属性名（`HPE_MUT_ATTR` 时）/ Attribute name (for `HPE_MUT_ATTR`)
 * @param[in]     old_value 变更前的旧值 / Previous value
 * @param[in,out] added 新增的节点 / Added node
 * @param[in,out] removed 被移除的节点 / Removed node
 *
 * @note 触及 `<style>`/`<link>` 会标记样式表脏并强制定界为全量布局（把 `reflow_node` 置空）；`HPE_MUT_CHILD_LIST` 与 `HPE_MUT_TEXT` 记为其父节点的局部重排 / Touching `<style>`/`<link>` marks the sheet dirty and forces an unbounded LAYOUT (clears `reflow_node`); `HPE_MUT_CHILD_LIST` and `HPE_MUT_TEXT` are recorded as a partial reflow of the parent
 *
 * @see refresh_styles hpe_engine_flush
 */
static void style_sink(void *ud, int kind, HpeDomNode *target, const char *name,
                       const char *old_value, HpeDomNode *added, HpeDomNode *removed)
{
    HpeEngine *engine = (HpeEngine *)ud;
    int need = HPE_REFLOW_NONE;

    /* ---- author-sheet tracking ---- */
    if (is_style_host(target) || is_style_host(added) || is_style_host(removed)) {
        g_style_dirty = 1;
        if (is_style_host(added))
            g_style_sig = added;
        else if (is_style_host(removed))
            g_style_sig = removed;
        else
            g_style_sig = target;
        g_style_rev++;
        need = HPE_REFLOW_LAYOUT;
    }

    /* ---- reflow classification + scope ---- */
    if (kind == HPE_MUT_CHILD_LIST) {
        /* a node entering or leaving the tree moves boxes around its parent;
         * `target` is that parent (the child-list owner) */
        if (added || removed) {
            note_reflow(engine, HPE_REFLOW_LAYOUT);
            note_reflow_node(engine, target);
        }
    } else if (kind == HPE_MUT_ATTR) {
        need = reflow_for_attr(target, name, old_value);
        note_reflow(engine, need);
        if (need == HPE_REFLOW_LAYOUT)
            note_reflow_node(engine, target);
    } else if (kind == HPE_MUT_TEXT) {
        /* a text edit changes a line box's content -> relayout of its parent */
        note_reflow(engine, HPE_REFLOW_LAYOUT);
        note_reflow_node(engine, target);
    }
    if (need)
        note_reflow(engine, need);
    if (engine) {
        engine->mutation_pending = 1;
        /* any <style>/<link> edit changes every box: no partial rebuild possible */
        if (is_style_host(target) || is_style_host(added) || is_style_host(removed))
            engine->reflow_node = NULL;
    }
}

/**
 * @brief `<style>`/`<link>` 变动后重建作者规则 / Rebuild the author rules after a `<style>`/`<link>` change
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @return 重建了样式表返回 1（调用方须重新级联），否则 0 / 1 when the sheet was rebuilt (the caller must re-cascade), otherwise 0
 *
 * @note 从干净表开始：`hpe_css_init` 先追加 UA 规则，再遍历（可能已被脚本编辑的）树收集作者规则；link 提示表重新扫描以免累积 / Starts from a clean sheet: `hpe_css_init` re-adds the user-agent rules, then the (possibly script-edited) tree is walked for author rules; the link hint list is re-scanned so it does not accumulate
 *
 * @see is_style_host collect_styles
 */
static int refresh_styles(HpeEngine *engine)
{
    char dir[HPE_MAX_PATH];
    if (!g_style_dirty)
        return 0;
    g_style_dirty = 0;

    /* start from a clean sheet: hpe_css_init appends the user agent rules, then
     * collect_styles walks the (possibly edited) tree for the author ones. */
    hpe_css_free(&engine->sheet);
    hpe_css_init(&engine->sheet);
    engine->author_rule_base = engine->sheet.rule_count;
    engine->link_count = 0; /* re-scan, so the hint list does not accumulate */
    hpe_path_dir(engine->path ? engine->path : ".", dir, sizeof(dir));
    collect_styles(engine, engine->doc, dir);
    engine->style_sig = g_style_sig;
    engine->style_rev = g_style_rev;
    return 1;
}

/**
 * @brief 按（可能已编辑的）DOM 重算每个盒的样式 / Recompute every box's style from the (possibly edited) DOM
 *
 * @param[in,out] box 起始盒，递归其子树 / Starting box, recursing into its subtree
 * @param[in,out] sheet 用于级联的样式表 / Stylesheet used for the cascade
 * @param[in]     parent 父盒的计算样式，可为 NULL / Parent box's computed style, may be NULL
 *
 * @note 只覆盖样式、不动几何；纯几何编辑因无相应属性而无操作，因而零代价 / Only styles are overwritten, geometry is untouched; a pure-geometry edit is a no-op with no such property, so it pays nothing
 *
 * @see hpe_engine_flush
 */
static void restyle_boxes(HpeBox *box, HpeStylesheet *sheet, const HpeStyle *parent)
{
    HpeBox *child;
    HpeStyle style;
    HpeStyle child_parent;

    if (!box)
        return;
    if (box->kind == HPE_BOX_TEXT) {
        if (parent) {
            hpe_css_compute(sheet, NULL, parent, &style);
            box->style = style;
        }
        return;
    }
    hpe_css_compute(sheet, box->node, parent, &style);
    box->style = style;
    child_parent = style;
    for (child = box->first_child; child; child = child->next)
        restyle_boxes(child, sheet, &child_parent);
}

/**
 * @brief 执行 LAYOUT 级 flush 的盒树重建 / Rebuild the box tree for a LAYOUT-level flush
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in,out] scope 局部重建的 DOM 作用域，NULL 表示整树重建 / DOM scope for a partial rebuild, NULL for a full rebuild
 *
 * @note 有有效 `scope` 且未设 `HPE_NO_PARTIAL` 时走 `hpe_layout_rebuild` 局部子树重建，否则整树重建；`HOPE_TRACE_REFLOW` 打印走了哪条路径 / Does a partial subtree rebuild via `hpe_layout_rebuild` when a valid `scope` exists and `HPE_NO_PARTIAL` is unset, otherwise a full rebuild; `HOPE_TRACE_REFLOW` traces which path ran
 *
 * @see hpe_engine_flush
 */
static void flush_rebuild_layout(HpeEngine *engine, HpeDomNode *scope)
{
    HpeBox *partial = NULL;

    if (scope && !getenv("HPE_NO_PARTIAL"))
        partial = hpe_layout_rebuild(engine->doc, &engine->sheet, (float)engine->width,
                                     (float)engine->height, engine->root, scope);
    if (partial) {
        if (getenv("HOPE_TRACE_REFLOW"))
            fprintf(stderr, "hope: flush -> LAYOUT (partial)\n");
    } else {
        if (getenv("HOPE_TRACE_REFLOW"))
            fprintf(stderr, "hope: flush -> LAYOUT (full)\n");
        if (engine->root) {
            hpe_layout_free(engine->root);
            engine->root = NULL;
        }
        engine->root = hpe_layout_build(engine->doc, &engine->sheet, (float)engine->width,
                                        (float)engine->height);
    }
    engine->content_height = hpe_layout_content_height(engine->root);
}

/**
 * @brief 把滚动偏移夹到 `[0, content_height - view_height]` / Clamp the scroll offset to `[0, content_height - view_height]`
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @see hpe_engine_scroll_by
 */
static void clamp_scroll(HpeEngine *engine)
{
    float max_scroll =
        HPE_MAX(0.0f, engine->content_height - (float)hpe_engine_view_height(engine));
    if (engine->scroll_y > max_scroll)
        engine->scroll_y = max_scroll;
    if (engine->scroll_y < 0.0f)
        engine->scroll_y = 0.0f;
}

/**
 * @brief 应用自上次以来记录的 DOM 变更 / Apply the DOM mutations recorded since the last call
 *
 * @details 惰性重排策略：LAYOUT 重建受影响的盒子树（作用域无界时整树）；PAINT 重算样式并重绘；NONE 完全跳过渲染。
 *          Lazy policy: LAYOUT rebuilds the affected box subtree (everything when unbounded); PAINT recomputes styles and repaints; NONE skips the render entirely.
 *
 * @param[in,out] engine 引擎实例，NULL 时直接返回 / Engine instance, returns early when NULL
 * @param[in]     force 非零请求全量重建（新加载或缩放）/ Non-zero requests a full rebuild (fresh load or resize)
 *
 * @note `HPE_FORCE_EAGER` 强制走急切全量路径以便与惰性结果对比；处理后清零 `reflow_need`/`reflow_node`/`mutation_pending` 并按需置 `HPE_DIRTY_FULL` / `HPE_FORCE_EAGER` forces the eager full path so the lazy result can be compared; afterwards `reflow_need`/`reflow_node`/`mutation_pending` are cleared and `HPE_DIRTY_FULL` is set as needed
 *
 * @see hpe_engine_relayout flush_rebuild_layout restyle_boxes
 */
void hpe_engine_flush(HpeEngine *engine, int force)
{
    int need;
    HpeDomNode *scope = NULL;

    if (!engine)
        return;
    if (force) {
        need = HPE_REFLOW_LAYOUT;
    } else {
        need = engine->reflow_need;
        scope = engine->reflow_node;
        if (refresh_styles(engine))
            need = HPE_REFLOW_LAYOUT; /* the sheet changed: scope is meaningless */
        /* diagnostic/benchmark aid: force the eager path so the lazy result can
         * be compared against a full rebuild */
        if (getenv("HPE_FORCE_EAGER") && need != HPE_REFLOW_NONE) {
            need = HPE_REFLOW_LAYOUT;
            scope = NULL;
        }
    }
    engine->reflow_need = HPE_REFLOW_NONE;
    engine->reflow_node = NULL;
    engine->mutation_pending = 0;
    if (!engine->doc)
        return;

    hpe_css_set_viewport(&engine->sheet, (float)engine->width,
                         (float)hpe_engine_view_height(engine));

    if (need == HPE_REFLOW_LAYOUT) {
        flush_rebuild_layout(engine, scope);
    } else if (need == HPE_REFLOW_PAINT && engine->root) {
        HpeStyle root_parent = hpe_style_default();
        if (getenv("HOPE_TRACE_REFLOW"))
            fprintf(stderr, "hope: flush -> PAINT (restyle only)\n");
        restyle_boxes(engine->root, &engine->sheet, &root_parent);
    } else {
        if (getenv("HOPE_TRACE_REFLOW"))
            fprintf(stderr, "hope: flush -> NONE\n");
        return; /* nothing changed: leave the existing frame alone */
    }

    clamp_scroll(engine);
    engine->dirty = HPE_DIRTY_FULL;
}

/**
 * @brief 取路径的最后一段 / Get the final path component
 *
 * @param[in]     path 文件路径 / File path
 *
 * @return 末尾的文件名部分，`path` 为 NULL 时返回空串 / The trailing file name, an empty string when `path` is NULL
 *
 * @note 同时识别 `/` 与 `\` 分隔符 / Handles both `/` and `\` separators
 */

static const char *path_basename(const char *path)
{
    const char *base = path;
    const char *p;
    if (!path)
        return "";
    for (p = path; *p; p++) {
        if (*p == '/' || *p == '\\')
            base = p + 1;
    }
    return base;
}

/**
 * @brief 刷新状态行文本 / Refresh the status-line text
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @note 汇总文件名、尺寸、是否含地址栏、加载/绘制耗时与滚动位置，并在末尾追加粘性加载错误（若有）/ Formats the file name, size, chrome flag, load/paint timings and scroll position, appending the sticky load error when present
 *
 * @see hpe_engine_status
 */
void hpe_engine_refresh_status(HpeEngine *engine)
{
    int n = snprintf(engine->status, sizeof(engine->status),
                     "%s  |  %dx%d%s  |  load %.1f ms  |  paint %.1f ms  |  scroll %.0f/%.0f",
                     engine->path ? path_basename(engine->path) : "(blank page)", engine->width,
                     engine->height, engine->chrome_h > 0 ? " +chrome" : "", engine->load_ms,
                     engine->render_ms, engine->scroll_y,
                     HPE_MAX(0.0f, engine->content_height - (float)hpe_engine_view_height(engine)));
    if (engine->error[0] && n > 0 && n < (int)sizeof(engine->status))
        snprintf(engine->status + n, sizeof(engine->status) - (size_t)n, "  |  ERR: %s",
                 engine->error);
}

/**
 * @brief 取页面视口高度（窗口高减地址栏）/ Get the page viewport height (window height minus chrome)
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @return 视口高度（像素），不小于 0；`engine` 为 NULL 时返回 0 / Viewport height in pixels, never below 0; 0 when `engine` is NULL
 */
int hpe_engine_view_height(HpeEngine *engine)
{
    int h;
    if (!engine)
        return 0;
    h = engine->height - engine->chrome_h;
    return h > 0 ? h : 0;
}

/**
 * @brief 递归打印盒树及行/文本段（调试）/ Recursively dump the box tree with lines and runs (debug)
 *
 * @param[in,out] box 起始盒，递归其子树 / Starting box, recursing into its subtree
 * @param[in]     depth 当前缩进层级 / Current indentation depth
 *
 * @note 输出到 stderr；`HOPE_DEBUG` 下由 `load_source` 在加载后调用，供无头诊断 / Writes to stderr; called by `load_source` after a load under `HOPE_DEBUG` for headless diagnostics
 *
 * @see dump_dom
 */
static void dump_boxes(HpeBox *box, int depth)
{
    HpeBox *child;
    int i;
    for (i = 0; i < depth; i++)
        fprintf(stderr, "  ");
    fprintf(stderr, "%s<%s> bb=(%.0f,%.0f %.0fx%.0f) cb=(%.0f,%.0f %.0fx%.0f) lines=%d fs=%.0f\n",
            box->kind == HPE_BOX_TEXT ? "T" : (box->kind == HPE_BOX_INLINE ? "I" : "B"),
            box->node && box->node->tag ? box->node->tag : "#text", box->border_box.x,
            box->border_box.y, box->border_box.w, box->border_box.h, box->content_box.x,
            box->content_box.y, box->content_box.w, box->content_box.h, box->line_count,
            box->style.font_size);
    for (i = 0; i < box->line_count; i++)
        fprintf(stderr, "        line y=%.0f h=%.0f w=%.0f runs=%d\n", box->lines[i].y,
                box->lines[i].height, box->lines[i].width, box->lines[i].run_count);
    for (i = 0; i < box->line_count; i++) {
        int ri;
        for (ri = 0; ri < box->lines[i].run_count; ri++) {
            HpeTextRun *run = &box->lines[i].runs[ri];
            if (run->atomic)
                fprintf(stderr, "          run x=%.1f w=%.1f <atomic>\n", run->x, run->width);
            else if (run->font)
                fprintf(stderr, "          run x=%.1f w=%.1f \"%.*s\" #%02x%02x%02x\n", run->x,
                        run->width, run->len > 24 ? 24 : run->len, run->text, run->color.r,
                        run->color.g, run->color.b);
        }
    }
    for (child = box->first_child; child; child = child->next)
        dump_boxes(child, depth + 1);
}

/**
 * @brief 收集子树的 `<script>` 元素 / Collect the `<script>` elements of a subtree
 *
 * @param[in,out] node 开始遍历的 DOM 节点 / DOM node to walk from
 * @param[out]    out 接收脚本元素的数组 / Array that receives the script elements
 * @param[in]     max 数组容量上限 / Maximum number of array entries
 * @param[in,out] count 已收集个数 / Number collected so far
 *
 * @note 按文档顺序填充，最多 `max` 个 / Fills in document order, at most `max` entries
 *
 * @see run_scripts
 */

static void collect_script_nodes(HpeDomNode *node, HpeDomNode **out, int max, int *count)
{
    HpeDomNode *child;
    for (child = node->first_child; child; child = child->next_sibling) {
        if (child->type != HPE_NODE_ELEMENT)
            continue;
        if (strcmp(child->tag, "script") == 0 && *count < max)
            out[(*count)++] = child;
        collect_script_nodes(child, out, max, count);
    }
}

/**
 * @brief 模块加载器：解析说明符并读取模块源码 / Module loader: resolve a specifier and read the module source
 *
 * @param[in,out] ud 宿主透传的基础目录 / Opaque user data, the base directory
 * @param[in]     spec 模块说明符 / Module specifier
 * @param[in]     from_dir 发起导入的模块所在目录，空则用 `ud` / Directory of the importing module, `ud` when empty
 * @param[out]    out_path 接收解析后的模块路径 / Receives the resolved module path
 * @param[out]    out_src 接收模块源码 / Receives the module source
 *
 * @return 加载成功返回 1，否则 0 / 1 when loaded, otherwise 0
 *
 * @note 无扩展名的说明符补 `.js`；裸说明符回退到文档目录；失败时不写出 `out_path` / Appends `.js` to an extension-less specifier; a bare specifier falls back to the document directory; `out_path` is not written on failure
 */
static int hope_module_load(void *ud, const char *spec, const char *from_dir, char **out_path,
                            char **out_src)
{
    char full[1024];
    const char *base = from_dir && *from_dir ? from_dir : (const char *)ud;
    size_t n;

    if (!spec || !*spec)
        return 0;
    /* bare specifiers fall back to the document directory */
    if (spec[0] == '.' || spec[0] == '/')
        hpe_path_join(full, sizeof(full), base ? base : ".", spec);
    else
        hpe_path_join(full, sizeof(full), base ? base : ".", spec);
    /* allow extension-less specifiers */
    n = strlen(full);
    if (n < 3 || strcmp(full + n - 3, ".js") != 0) {
        if (n + 4 < sizeof(full))
            strcpy(full + n, ".js");
    }
    *out_src = hpe_read_file(full, NULL);
    if (!*out_src)
        return 0;
    *out_path = hpe_strdup(full);
    return 1;
}

/**
 * @brief 读取单个 `<script>` 的源码 / Read one `<script>`'s source
 *
 * @param[in,out] node `<script>` 元素节点 / The `<script>` element node
 * @param[in]     dir 解析 `src` 的基准目录 / Base directory used to resolve `src`
 * @param[out]    is_module 置为非空表示 `type="module"` / Set non-NULL for `type="module"`
 *
 * @return 新分配的缓冲区（须 `hpe_free`），无内容返回 `NULL` / A freshly allocated buffer (free with `hpe_free`), `NULL` when there is nothing
 *
 * @note 有 `src` 时读文件，否则取内联原始文本 / Reads the `src` file when present, otherwise the inline raw text
 */
static char *script_source(HpeDomNode *node, const char *dir, const char **is_module)
{
    const char *src_attr = hpe_dom_attr(node, "src");
    const char *type_attr = hpe_dom_attr(node, "type");

    *is_module = type_attr && strcmp(type_attr, "module") == 0 ? "1" : NULL;
    if (src_attr && *src_attr) {
        char full[1024];
        char *code;
        hpe_path_join(full, sizeof(full), dir, src_attr);
        code = hpe_read_file(full, NULL);
        if (!code)
            fprintf(stderr, "hope: cannot open script %s\n", full);
        return code;
    }
    {
        HpeDomNode *text = node->first_child;
        if (text && text->type == HPE_NODE_TEXT && text->text && text->text[0])
            return hpe_strdup(text->text);
    }
    return NULL;
}

/**
 * @brief 在引擎 JS 上下文中求值一个已收集的 `<script>` / Evaluate one already-collected `<script>` in the engine's JS context
 *
 * @param[in,out] engine 引擎实例，提供 `js` / Engine instance, provides `js`
 * @param[in,out] node 要执行的 `<script>` 元素 / The `<script>` element to run
 * @param[in]     dir 脚本所在目录 / Directory of the script
 *
 * @note module 走 `hpe_js_eval_module`，否则 `hpe_js_eval`；错误打印到 stderr 且不中断后续脚本 / Modules go through `hpe_js_eval_module`, otherwise `hpe_js_eval`; an error is printed to stderr and does not stop later scripts
 *
 * @see run_scripts
 */
static void run_one_script(HpeEngine *engine, HpeDomNode *node, const char *dir)
{
    const char *is_module = NULL;
    char errbuf[256];
    char *code = script_source(node, dir, &is_module);
    int rc;

    if (!code)
        return;
    errbuf[0] = '\0';
    rc = is_module ? hpe_js_eval_module(engine->js, code, dir, errbuf, sizeof(errbuf))
                   : hpe_js_eval(engine->js, code, errbuf, sizeof(errbuf));
    if (rc != 0)
        fprintf(stderr, "hope: script error: %s\n", errbuf);
    hpe_free(code);
}

/**
 * @brief 重建 JS 上下文并执行文档内全部脚本 / Rebuild the JS context and run every script in the document
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @note 先销毁旧 JS 上下文；无脚本时不创建上下文直接返回；安装 DOM 绑定与模块加载器后按文档顺序执行（上限 `HPE_MAX_SCRIPTS`）/ Destroys the old JS context first; returns without creating one when there are no scripts; installs the DOM bindings and module loader, then runs them in document order (capped at `HPE_MAX_SCRIPTS`)
 *
 * @see run_one_script collect_script_nodes
 */
static void run_scripts(HpeEngine *engine)
{
    HpeDomNode *scripts[HPE_MAX_SCRIPTS];
    int count = 0;
    int i;
    char dir[1024];
    HpeJsModuleLoader loader;

    if (engine->js) {
        hpe_js_destroy(engine->js);
        engine->js = NULL;
    }
    collect_script_nodes(engine->doc, scripts, HPE_MAX_SCRIPTS, &count);
    if (count == 0)
        return;

    engine->js = hpe_js_create();
    hpe_js_dom_install(engine->js, engine->doc);

    hpe_path_dir(engine->path ? engine->path : ".", dir, sizeof(dir));
    hpe_js_set_current_dir(engine->js, dir);
    loader.load = hope_module_load;
    loader.ud = dir;
    hpe_js_set_module_loader(engine->js, &loader);

    for (i = 0; i < count; i++)
        run_one_script(engine, scripts[i], dir);
}

/**
 * @brief 递归打印 DOM 元素与文本节点（调试）/ Recursively dump DOM elements and text nodes (debug)
 *
 * @param[in,out] node 起始 DOM 节点 / Starting DOM node
 * @param[in]     depth 当前缩进层级 / Current indentation depth
 *
 * @note 元素打印标签与 class，文本最多打印 40 字节；输出到 stderr / Elements print their tag and class, text is truncated to 40 bytes; writes to stderr
 *
 * @see dump_boxes
 */

static void dump_dom(HpeDomNode *node, int depth)
{
    HpeDomNode *child;
    int i;
    for (child = node->first_child; child; child = child->next_sibling) {
        for (i = 0; i < depth; i++)
            fprintf(stderr, "  ");
        if (child->type == HPE_NODE_ELEMENT) {
            const char *cls = hpe_dom_attr(child, "class");
            fprintf(stderr, "<%s%s%s>\n", child->tag, cls ? " class=" : "", cls ? cls : "");
            dump_dom(child, depth + 1);
        } else if (child->type == HPE_NODE_TEXT) {
            int len = child->text ? (int)strlen(child->text) : 0;
            fprintf(stderr, "\"%.40s\" (%d bytes)\n", child->text ? child->text : "", len);
        }
    }
}

/**
 * @brief 急切重排：无条件全量重建盒树 / Eager relayout: unconditionally rebuild the whole box tree
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @note 等价于 `hpe_engine_flush(engine, 1)`；供加载、缩放、悬停、定时器等必须重排的路径使用，惰性策略在 `hpe_engine_flush` / Equivalent to `hpe_engine_flush(engine, 1)`; used by paths that must relayout (load, resize, hover, timers), while the lazy policy lives in `hpe_engine_flush`
 *
 * @see hpe_engine_flush
 */
void hpe_engine_relayout(HpeEngine *engine)
{
    /* the eager entry point (load, resize, hover, timers, dev hooks): it always
     * rebuilds. The lazy policy lives in hpe_engine_flush(), used by the event
     * paths that can prove a mutation is paint-only or absent. */
    hpe_engine_flush(engine, 1);
}

/**
 * @brief 创建引擎实例并初始化像素缓冲 / Create an engine instance and initialise its pixel buffer
 *
 * @param[in]     width 窗口宽度 / Window width
 * @param[in]     height 窗口高度 / Window height
 *
 * @return 新引擎实例 / The new engine instance
 *
 * @note 非正或缺失的宽高分别回退为 800×600 / Non-positive dimensions fall back to 800×600
 *
 * @see hpe_engine_destroy
 */

HpeEngine *hpe_engine_create(int width, int height)
{
    HpeEngine *engine = (HpeEngine *)hpe_calloc(1, sizeof(HpeEngine));
    engine->width = width > 0 ? width : 800;
    engine->height = height > 0 ? height : 600;
    hpe_pixmap_init(&engine->pm, engine->width, engine->height);
    snprintf(engine->status, sizeof(engine->status), "hope: ready");
    return engine;
}

/**
 * @brief 销毁引擎并释放全部资源 / Destroy the engine and release all resources
 *
 * @param[in,out] engine 引擎实例，NULL 时直接返回 / Engine instance, returns early when NULL
 *
 * @note 释放盒树、DOM、样式表、像素缓冲与 JS 上下文，并关闭字体子系统 / Frees the box tree, DOM, stylesheet, pixel buffer and JS context, and shuts the font subsystem down
 *
 * @see hpe_engine_create
 */
void hpe_engine_destroy(HpeEngine *engine)
{
    if (!engine)
        return;
    hpe_dom_clear_mutation_sinks();
    if (engine->root)
        hpe_layout_free(engine->root);
    if (engine->doc)
        hpe_dom_free(engine->doc);
    hpe_css_free(&engine->sheet);
    hpe_pixmap_free(&engine->pm);
    if (engine->js) {
        hpe_js_destroy(engine->js);
        engine->js = NULL;
    }
    if (engine->path)
        hpe_free(engine->path);
    hpe_font_shutdown();
    hpe_free(engine);
}

/**
 * @brief 拆掉上一份文档的全部产物，准备重新加载 / Tear down everything the previous document produced, ready for a new load
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     path 新文档路径，NULL 表示空白页 / New document path, NULL for the blank page
 *
 * @note 释放旧路径/盒树/DOM 并重建样式表（含 `hpe_css_init` 的 UA 规则）；`path` 为 NULL 时清空路径 / Frees the old path, box tree and DOM and rebuilds the sheet (with the user-agent rules from `hpe_css_init`); a NULL `path` clears the path
 *
 * @see load_source
 */
static void reset_document(HpeEngine *engine, const char *path)
{
    if (engine->path) {
        hpe_free(engine->path);
        engine->path = NULL;
    }
    if (path)
        engine->path = hpe_strdup(path);
    if (engine->root) {
        hpe_layout_free(engine->root);
        engine->root = NULL;
    }
    if (engine->doc) {
        hpe_dom_free(engine->doc);
        engine->doc = NULL;
    }
    hpe_css_free(&engine->sheet);
    hpe_css_init(&engine->sheet); /* user agent styles */
}

/**
 * @brief 解码原始字节、解析 DOM 并解析 `<base href>` / Decode the raw bytes, parse the DOM and resolve `<base href>`
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in,out] src 原始字节缓冲，函数接管并释放 / Raw byte buffer; ownership is taken and freed
 * @param[in]     size 缓冲区字节数 / Number of bytes in the buffer
 * @param[in]     path 文档路径，供相对引用重定基 / Document path, used to rebase relative references
 *
 * @note 按 BOM / `<meta charset>` / UTF-8 校验探测编码并转成 UTF-8 再解析；在收集作者样式前解析 base href，使相对引用正确重定基 / Detects the charset (BOM / `<meta charset>` / UTF-8 validation), decodes to UTF-8 before parsing, and resolves base href before author styles are collected so relative references rebase correctly
 *
 * @see reset_document install_styles
 */
static void parse_document(HpeEngine *engine, char *src, size_t size, const char *path)
{
    char dir[HPE_MAX_PATH];
    char *utf8;
    size_t declen = 0;
    HpeDocEncoding enc = HPE_ENC_UTF8;
    int from_meta = 0;
    const char *base;

    utf8 = hpe_html_decode(src, size, &declen, &enc, &from_meta);
    hpe_free(src);
    snprintf(engine->doc_encoding, sizeof(engine->doc_encoding), "%s", hpe_encoding_name(enc));
    engine->meta_charset = from_meta;
    engine->link_count = 0;
    engine->base_href[0] = '\0';

    engine->doc = hpe_html_parse(utf8, declen);
    hpe_free(utf8);

    hpe_path_dir(path ? path : ".", dir, sizeof(dir));
    base = find_base_href(engine->doc);
    if (base) {
        if (hpe_path_is_absolute(base))
            snprintf(engine->base_href, sizeof(engine->base_href), "%s", base);
        else
            hpe_path_join(engine->base_href, sizeof(engine->base_href), dir, base);
    }
}

/**
 * @brief 收集作者规则并安装 DOM 变更汇聚点 / Collect the author rules and install the DOM mutation sink
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     path 文档路径，用于解析外链 / Document path, used to resolve external links
 *
 * @note 记录 `author_rule_base` 与样式签名，并注册 `style_sink`，使脚本后续对 `<style>`/`<link>` 的改动保持同步 / Records `author_rule_base` and the style signature, and registers `style_sink` so later `<style>`/`<link>` edits by page scripts stay in sync
 *
 * @see collect_styles style_sink
 */
static void install_styles(HpeEngine *engine, const char *path)
{
    char dir[HPE_MAX_PATH];

    hpe_path_dir(path ? path : ".", dir, sizeof(dir));
    collect_styles(engine, engine->doc, dir);
    engine->author_rule_base = engine->sheet.rule_count;
    engine->style_sig = NULL;
    engine->style_rev = g_style_rev;
    g_style_dirty = 0;
    g_style_sig = NULL;
    hpe_dom_add_mutation_sink(style_sink, engine);
}

/**
 * @brief `HOPE_DEBUG` 下转储 DOM 与前若干条级联规则 / Under `HOPE_DEBUG`, dump the DOM and the first few cascade rules
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @note 未设置 `HOPE_DEBUG` 时直接返回；最多打印 40 条规则 / Returns immediately when `HOPE_DEBUG` is unset; at most 40 rules are printed
 *
 * @see dump_dom
 */
static void debug_dump_document(HpeEngine *engine)
{
    int ri;
    if (!getenv("HOPE_DEBUG"))
        return;
    dump_dom(engine->doc, 0);
    fprintf(stderr, "hope: %d css rules after UA+author\n", engine->sheet.rule_count);
    for (ri = 0; ri < engine->sheet.rule_count && ri < 40; ri++) {
        HpeRule *r = &engine->sheet.rules[ri];
        int si, di;
        fprintf(stderr, "  rule %d: %d selectors, %d decls, spec=%d\n", ri, r->selector_count,
                r->decl_count, r->selector_count ? r->selectors[0].specificity : -1);
        for (si = 0; si < r->selector_count; si++) {
            HpeSelector *s = &r->selectors[si];
            int pi;
            fprintf(stderr, "    sel:");
            for (pi = 0; pi < s->count; pi++) {
                HpeCompound *c = &s->parts[pi];
                fprintf(stderr, " [%s%s%s cls=%d]", c->tag ? c->tag : "*", c->id ? "#" : "",
                        c->id ? c->id : "", c->class_count);
                if (c->class_count)
                    fprintf(stderr, "(.%s)", c->classes[0]);
            }
            fprintf(stderr, "\n");
        }
        for (di = 0; di < r->decl_count; di++)
            fprintf(stderr, "    decl set=0x%llx font=%.1f\n",
                    (unsigned long long)r->decls[di].set, r->decls[di].font_size);
    }
}

/**
 * @brief 运行 `HOPE_TEST_*` 合成事件测试钩子 / Run the `HOPE_TEST_*` synthetic-event test hooks
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @note 读取环境变量：`HOPE_TEST_CLICK`（按 id 点击）、`HOPE_TEST_HOVER`（`x,y` 悬停）、`HOPE_TEST_WAIT`（在给定秒数内跑 tick）、`HOPE_TEST_TEXT`（按 id 打印文本）；在排版后调用一次，供无头 `--dump` 验证交互页 / Reads env vars: `HOPE_TEST_CLICK` (click by id), `HOPE_TEST_HOVER` (`x,y` hover), `HOPE_TEST_WAIT` (tick for the given seconds), `HOPE_TEST_TEXT` (print text by id); runs once after layout so headless `--dump` can verify interactive pages
 *
 * @see click_element_by_id print_text_by_id
 */
static void run_test_hooks(HpeEngine *engine)
{
    const char *test_click = getenv("HOPE_TEST_CLICK");
    const char *test_hover = getenv("HOPE_TEST_HOVER");
    const char *test_wait = getenv("HOPE_TEST_WAIT");
    const char *test_text = getenv("HOPE_TEST_TEXT");

    if (test_click && *test_click)
        click_element_by_id(engine, test_click);
    if (test_hover && *test_hover) {
        int hx, hy;
        if (sscanf(test_hover, "%d,%d", &hx, &hy) == 2)
            hpe_engine_mouse_move(engine, hx, hy);
    }
    if (test_wait && *test_wait) {
        double wait = atof(test_wait);
        double t0 = hpe_time_seconds();
        while (hpe_time_seconds() - t0 < wait)
            hpe_engine_tick(engine);
    }
    if (test_text && *test_text)
        print_text_by_id(engine, test_text);
}

/**
 * @brief 共享加载流水线 / Shared load pipeline
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in,out] src 原始字节缓冲，函数接管并释放 / Raw byte buffer; ownership is taken and freed
 * @param[in]     size 缓冲区字节数 / Number of bytes in the buffer
 * @param[in]     path 文档路径，NULL 表示内置空白页 / Document path, NULL for the built-in blank page
 *
 * @return 排版出盒树返回 1，否则 0 / 1 when a box tree was produced, otherwise 0
 *
 * @note 清错、重置、解析、收集样式，先排版一次（供脚本读取 `clientWidth`/`clientHeight`），跑脚本后再排版一次，最后跑测试钩子；记录加载耗时并刷新状态 / Clears the error, resets, parses and collects styles, lays out once (so scripts can read `clientWidth`/`clientHeight`), runs the scripts and lays out again, then runs the test hooks; records the load time and refreshes the status
 *
 * @see parse_document run_scripts
 */
static int load_source(HpeEngine *engine, char *src, size_t size, const char *path)
{
    double t0 = hpe_time_seconds();

    engine->error[0] = '\0'; /* a new document clears the previous load error */
    reset_document(engine, path);
    parse_document(engine, src, size, path);
    install_styles(engine, path);
    debug_dump_document(engine);

    engine->scroll_y = 0.0f;
    /* lay out once before running page scripts: they read clientWidth /
     * clientHeight (which force a layout in browsers) to size their output.
     * The second layout below picks up whatever the script changed. */
    hpe_engine_relayout(engine);
    run_scripts(engine);
    hpe_engine_relayout(engine);
    run_test_hooks(engine);

    if (getenv("HOPE_DEBUG") && engine->root)
        dump_boxes(engine->root, 0);
    engine->load_ms = (hpe_time_seconds() - t0) * 1000.0;
    hpe_engine_refresh_status(engine);
    return engine->root != NULL;
}

/**
 * @brief 加载并排版一个 HTML 文件 / Load and lay out an HTML file
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     path 要加载的文件路径 / Path of the file to load
 *
 * @return 加载成功返回 1，否则 0 / 1 when loaded, otherwise 0
 * @retval     1 已读取并排版 / The file was read and laid out
 * @retval     0 `engine`/`path` 为 NULL 或文件无法打开（错误写入状态行）/ `engine` or `path` is NULL, or the file could not be opened (the error is stored in the status line)
 *
 * @note 启用地址栏时把绝对路径写入地址栏 / Writes the absolute path into the address bar when chrome is enabled
 *
 * @see load_source hpe_engine_reload
 */
int hpe_engine_load_file(HpeEngine *engine, const char *path)
{
    char *src;
    size_t size = 0;
    char abs[2048];

    if (!engine || !path)
        return 0;

    src = hpe_read_file(path, &size);
    if (!src) {
        snprintf(engine->error, sizeof(engine->error), "cannot open %s", path);
        snprintf(engine->status, sizeof(engine->status), "hope: cannot open %s", path);
        return 0;
    }
    if (!load_source(engine, src, size, path))
        return 0;
    if (engine->chrome_h > 0) {
        hpe_path_absolute(abs, sizeof(abs), path);
        hpe_chrome_set_text(engine, abs);
    }
    return 1;
}

/**
 * @brief 加载内置空白页 / Load the built-in blank page
 *
 * @param[in,out] engine 引擎实例，NULL 时返回 0 / Engine instance, returns 0 when NULL
 *
 * @return 加载成功返回 1，否则 0 / 1 when loaded, otherwise 0
 *
 * @note 无文件参数启动时使用；启用地址栏时清空地址并失焦 / Used when the engine starts without a file argument; clears and blurs the address bar when chrome is enabled
 *
 * @see hpe_engine_load_file
 */
int hpe_engine_load_blank(HpeEngine *engine)
{
    char *src;
    if (!engine)
        return 0;
    src = hpe_strdup(BLANK_PAGE);
    if (!src)
        return 0;
    if (!load_source(engine, src, strlen(BLANK_PAGE), NULL))
        return 0;
    if (engine->chrome_h > 0) {
        hpe_chrome_set_text(engine, "");
        engine->addr_focus = 0;
    }
    return 1;
}

/**
 * @brief 重新加载当前文档 / Reload the current document
 *
 * @param[in,out] engine 引擎实例，NULL 时返回 0 / Engine instance, returns 0 when NULL
 *
 * @return 加载成功返回 1，否则 0 / 1 when reloaded, otherwise 0
 *
 * @note 无路径时加载内置空白页 / Loads the built-in blank page when there is no path
 *
 * @see hpe_engine_load_file hpe_engine_load_blank
 */
int hpe_engine_reload(HpeEngine *engine)
{
    if (!engine)
        return 0;
    if (!engine->path)
        return hpe_engine_load_blank(engine);
    return hpe_engine_load_file(engine, engine->path);
}

/**
 * @brief 调整窗口与像素缓冲尺寸并重排 / Resize the window and pixel buffer and relayout
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     width 新的窗口宽度 / New window width
 * @param[in]     height 新的窗口高度 / New window height
 *
 * @note 宽高非正或与当前完全一致时直接返回；否则重设缓冲、重排并刷新状态 / Returns early for non-positive or unchanged dimensions; otherwise resizes the buffer, relayouts and refreshes the status
 *
 * @see hpe_engine_relayout
 */
void hpe_engine_resize(HpeEngine *engine, int width, int height)
{
    if (!engine || width <= 0 || height <= 0)
        return;
    if (width == engine->width && height == engine->height)
        return;
    engine->width = width;
    engine->height = height;
    hpe_pixmap_resize(&engine->pm, width, height);
    hpe_engine_relayout(engine);
    hpe_engine_refresh_status(engine);
}

/**
 * @brief 按行滚动页面 / Scroll the page by a number of lines
 *
 * @param[in,out] engine 引擎实例，NULL 时直接返回 / Engine instance, returns early when NULL
 * @param[in]     lines 滚动的行数，可正可负 / Number of lines to scroll, may be negative
 *
 * @note 一行等于 `HPE_SCROLL_LINE_PX` 像素；夹到合法范围后取整到整像素，使损伤矩形路径与全量重绘逐字节一致；仅在未脏时置 `HPE_DIRTY_SCROLL` / One line is `HPE_SCROLL_LINE_PX` pixels; the offset is clamped then snapped to whole pixels so the damage-rect path stays byte-identical to a full repaint; only sets `HPE_DIRTY_SCROLL` when not already dirty
 *
 * @see hpe_engine_scroll_offset clamp_scroll
 */
void hpe_engine_scroll_by(HpeEngine *engine, float lines)
{
    float max_scroll;
    if (!engine)
        return;
    max_scroll =
        HPE_MAX(0.0f, engine->content_height - (float)hpe_engine_view_height(engine));
    engine->scroll_y += lines * HPE_SCROLL_LINE_PX;
    if (engine->scroll_y < 0.0f)
        engine->scroll_y = 0.0f;
    if (engine->scroll_y > max_scroll)
        engine->scroll_y = max_scroll;
    /* the renderer is pixel-based: snapping the offset to whole pixels keeps the
     * damage-rect path byte-identical to a full repaint at any scroll position
     * (an un-snapped 204.4px offset would round differently per path) */
    engine->scroll_y = floorf(engine->scroll_y + 0.5f);
    if (engine->dirty == HPE_DIRTY_NONE)
        engine->dirty = HPE_DIRTY_SCROLL;
    hpe_engine_refresh_status(engine);
}

/**
 * @brief 查询是否禁用损伤矩形滚动（`HPE_NO_DIRTY`）/ Test whether damage-rect scrolling is disabled (`HPE_NO_DIRTY`)
 *
 * @return 禁用为 1，否则 0 / 1 when disabled, otherwise 0
 *
 * @note 环境变量只读一次并缓存，作为基准对比用 / The environment variable is read once and cached, as a benchmark aid
 */
static int dirty_disabled(void)
{
    static int flag = -1;
    if (flag < 0)
        flag = getenv("HPE_NO_DIRTY") != NULL;
    return flag;
}

/**
 * @brief 收集 `position:fixed` 的盒 / Collect `position:fixed` boxes
 *
 * @param[in,out] box 起始盒，递归其子树 / Starting box, recursing into its subtree
 * @param[out]    out 接收 fixed 盒的数组 / Array that receives the fixed boxes
 * @param[in]     max 数组容量上限 / Maximum number of array entries
 * @param[in,out] count 已收集个数 / Number collected so far
 *
 * @note 递归子树，最多 `max` 个；供仅滚动的局部重绘使用 / Recurses the subtree, at most `max` entries; used by the scroll-only partial repaint
 *
 * @see render_scrolled
 */
static void collect_fixed_boxes(HpeBox *box, HpeBox **out, int max, int *count)
{
    HpeBox *child;
    for (child = box->first_child; child; child = child->next) {
        if (child->style.position == HPE_POS_FIXED && *count < max)
            out[(*count)++] = child;
        collect_fixed_boxes(child, out, max, count);
    }
}

/**
 * @brief 仅滚动的局部重绘 / Scroll-only repaint
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @note 先按滚动差竖直平移已有像素并重绘露出的条带；位移达到整屏则回退为 `hpe_paint_document` 全量；`fixed` 盒不随滚动移动，最后覆盖其区域 / Shifts the existing pixels by the scroll delta then repaints the exposed strip; a delta of a full viewport or more falls back to a full `hpe_paint_document`; `fixed` boxes do not move and their regions are repainted last
 *
 * @see hpe_engine_render collect_fixed_boxes
 */
static void render_scrolled(HpeEngine *engine)
{
    HpeColor bg = hpe_rgb(255, 255, 255);
    float dy = engine->scroll_y - engine->painted_scroll;
    int dyi = (int)(dy >= 0.0f ? dy + 0.5f : dy - 0.5f);
    int origin_y = engine->chrome_h;
    int view_h = hpe_engine_view_height(engine);
    HpeBox *fixed[64];
    int fc = 0, i;

    if (dyi == 0)
        return; /* sub-pixel remainder: keep accumulating for next time */
    if (dyi >= view_h || dyi <= -view_h) {
        hpe_paint_document(&engine->pm, engine->root, engine->scroll_y, bg, (float)origin_y);
        hpe_chrome_paint(engine);
        engine->painted_scroll = engine->scroll_y;
        return;
    }

    /* only the page viewport scrolls: the chrome strip stays pinned */
    hpe_pixmap_shift_v_rect(&engine->pm, dyi, origin_y, engine->height);
    engine->painted_scroll += (float)dyi;

    if (dyi > 0) {
        int strip_y = engine->height - dyi;
        if (strip_y < origin_y)
            strip_y = origin_y;
        hpe_paint_region(&engine->pm, engine->root, engine->painted_scroll, bg,
                         hpe_rect(0.0f, (float)strip_y, (float)engine->width,
                                  (float)(engine->height - strip_y)),
                         (float)origin_y);
    } else {
        hpe_paint_region(&engine->pm, engine->root, engine->painted_scroll, bg,
                         hpe_rect(0.0f, (float)origin_y, (float)engine->width,
                                  (float)(-dyi)),
                         (float)origin_y);
    }

    /* fixed boxes stay glued to the viewport: repaint their regions over the
     * shifted content (the region repaint replays the whole tree clipped) */
    collect_fixed_boxes(engine->root, fixed, (int)(sizeof(fixed) / sizeof(fixed[0])), &fc);
    for (i = 0; i < fc; i++) {
        HpeRect clip = fixed[i]->border_box;
        clip.y += (float)origin_y;
        hpe_paint_region(&engine->pm, engine->root, engine->painted_scroll, bg, clip,
                         (float)origin_y);
    }
}

/**
 * @brief 按脏标记把引擎绘制到像素缓冲 / Paint the engine into its pixel buffer according to the dirty flag
 *
 * @param[in,out] engine 引擎实例，NULL 时直接返回 / Engine instance, returns early when NULL
 *
 * @note 脏标记为 `HPE_DIRTY_NONE` 且已绘制过时整帧跳过（使无实际变更的 DOM mutation 免费）；`HPE_DIRTY_CHROME` 只重画地址栏，`HPE_DIRTY_SCROLL` 走 `render_scrolled`（`HPE_NO_DIRTY` 下跳过），否则整帧重绘；结束清零脏标记并记录绘制耗时 / Skips the whole frame when the flag is `HPE_DIRTY_NONE` and a frame exists (making a no-op mutation free); `HPE_DIRTY_CHROME` repaints only the bar, `HPE_DIRTY_SCROLL` goes through `render_scrolled` (skipped under `HPE_NO_DIRTY`), otherwise a full repaint; clears the flag and records the paint time
 *
 * @see render_scrolled hpe_engine_pixmap
 */
void hpe_engine_render(HpeEngine *engine)
{
    double t0;
    if (!engine)
        return;
    /* nothing changed since the last frame: skip the whole paint (this is what
     * makes a DOM mutation that turns out to be a no-op free) */
    if (engine->dirty == HPE_DIRTY_NONE && engine->painted) {
        if (getenv("HOPE_TRACE_REFLOW"))
            fprintf(stderr, "hope: render -> skipped (no damage)\n");
        return;
    }
    t0 = hpe_time_seconds();
    if (engine->dirty == HPE_DIRTY_CHROME && engine->painted)
        hpe_chrome_paint(engine);
    else if (engine->dirty == HPE_DIRTY_SCROLL && engine->root && !dirty_disabled())
        render_scrolled(engine);
    else {
        hpe_paint_document(&engine->pm, engine->root, engine->scroll_y, hpe_rgb(255, 255, 255),
                           (float)engine->chrome_h);
        hpe_chrome_paint(engine);
        engine->painted_scroll = engine->scroll_y;
        engine->painted = 1;
    }
    engine->dirty = HPE_DIRTY_NONE;
    engine->render_ms = (hpe_time_seconds() - t0) * 1000.0;
    hpe_engine_refresh_status(engine);
}

/**
 * @brief 取引擎的像素缓冲 / Get the engine's pixel buffer
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @return 像素缓冲指针，`engine` 为 NULL 时返回 `NULL` / Pointer to the pixel buffer, `NULL` when `engine` is NULL
 */
const HpePixmap *hpe_engine_pixmap(HpeEngine *engine)
{
    return engine ? &engine->pm : NULL;
}

/**
 * @brief 取状态行文本 / Get the status-line text
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @return 状态文本，`engine` 为 NULL 时返回空串 / The status text, an empty string when `engine` is NULL
 *
 * @see hpe_engine_refresh_status
 */
const char *hpe_engine_status(HpeEngine *engine)
{
    return engine ? engine->status : "";
}

/**
 * @brief 取当前纵向滚动偏移 / Get the current vertical scroll offset
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @return 滚动偏移（像素），`engine` 为 NULL 时返回 0 / Scroll offset in pixels, 0 when `engine` is NULL
 *
 * @see hpe_engine_scroll_by
 */
float hpe_engine_scroll_offset(HpeEngine *engine)
{
    return engine ? engine->scroll_y : 0.0f;
}

/**
 * @brief 取上次加载文档所用的字符编码 / Get the charset the last loaded document was decoded from
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @return 编码名（如 `utf-8`、`latin1`、`utf-16le`、`utf-16be`），`engine` 为 NULL 时返回空串 / The charset label (e.g. `utf-8`, `latin1`, `utf-16le`, `utf-16be`), an empty string when `engine` is NULL
 *
 * @note 直到下次加载前有效 / Valid until the next load
 */

const char *hpe_engine_document_encoding(HpeEngine *engine)
{
    return engine ? engine->doc_encoding : "";
}

/**
 * @brief 查询编码是否来自文档内 `<meta charset>` / Query whether the charset came from an in-document `<meta charset>`
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @return 来自 meta 返回 1，否则 0 / 1 when it came from meta, otherwise 0
 *
 * @note 与来自 BOM 或 UTF-8 校验的情况区分 / Distinguishes it from a BOM or UTF-8 validation
 */
int hpe_engine_meta_charset(HpeEngine *engine)
{
    return engine ? engine->meta_charset : 0;
}

/**
 * @brief 取解析后的 `<base href>` / Get the resolved `<base href>`
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @return 绝对基址，文档未声明时返回空串 / The absolute base URL, an empty string when the document declares none
 */
const char *hpe_engine_base_href(HpeEngine *engine)
{
    return engine ? engine->base_href : "";
}

/**
 * @brief 取非 stylesheet 的 `<link>` 资源数 / Get the number of non-stylesheet `<link>` resources
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @return 记录的资源提示个数（icon/preload/alternate 等），`engine` 为 NULL 时返回 0 / Number of recorded resource hints (icon/preload/alternate ...), 0 when `engine` is NULL
 *
 * @see hpe_engine_link_at
 */
int hpe_engine_link_count(HpeEngine *engine)
{
    return engine ? engine->link_count : 0;
}

/**
 * @brief 取某条资源提示的归一化 rel 与绝对 href / Get a resource hint's normalised rel token and resolved absolute href
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     index 资源提示下标 / Resource hint index
 * @param[out]    rel 接收归一化 rel token 串，可为 NULL / Receives the normalised rel token string, may be NULL
 * @param[out]    href 接收解析后的绝对地址，可为 NULL / Receives the resolved absolute URL, may be NULL
 *
 * @return `index` 有效返回 1，越界返回 0 / 1 for a valid `index`, 0 when out of range
 *
 * @note 越界时不写出任何输出 / Nothing is written when the index is out of range
 *
 * @see hpe_engine_link_count
 */
int hpe_engine_link_at(HpeEngine *engine, int index, const char **rel, const char **href)
{
    if (!engine || index < 0 || index >= engine->link_count)
        return 0;
    if (rel)
        *rel = engine->links[index].rel;
    if (href)
        *href = engine->links[index].href;
    return 1;
}

/* ---------------- hit testing & events ---------------- */

/**
 * @brief 在盒树中做命中测试，返回最深命中的盒 / Hit-test the box tree, returning the deepest matching box
 *
 * @param[in,out] box 起始盒，递归其子树 / Starting box, recursing into its subtree
 * @param[in]     x X 坐标（视口坐标）/ X coordinate in viewport space
 * @param[in]     y_vp Y 坐标（视口坐标）/ Y coordinate in viewport space
 * @param[in]     scroll 文档滚动偏移 / Document scroll offset
 *
 * @return 命中的盒，未命中返回 `NULL` / The hit box, `NULL` when nothing matched
 *
 * @note `fixed` 盒按视口坐标比较，普通流盒按文档坐标（视口 y 加滚动量）；子级优先，最深命中获胜 / `fixed` boxes compare in viewport coordinates, normal-flow boxes in document coordinates (viewport y plus scroll); children are tested first and the deepest match wins
 *
 * @see node_from_hit
 */
static HpeBox *hit_test_box(HpeBox *box, float x, float y_vp, float scroll)
{
    HpeBox *child;
    float ytest;
    if (!box)
        return NULL;
    ytest = box->style.position == HPE_POS_FIXED ? y_vp : y_vp + scroll;
    if (x < box->border_box.x || ytest < box->border_box.y ||
        x > box->border_box.x + box->border_box.w ||
        ytest > box->border_box.y + box->border_box.h)
        return NULL;
    for (child = box->first_child; child; child = child->next) {
        HpeBox *r = hit_test_box(child, x, y_vp, scroll);
        if (r)
            return r; /* deepest match wins */
    }
    return box;
}

/**
 * @brief 由窗口坐标求命中的 DOM 元素 / Resolve the DOM element under a window coordinate
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     x X 坐标（窗口坐标）/ X coordinate in window space
 * @param[in]     y Y 坐标（窗口坐标）/ Y coordinate in window space
 *
 * @return 命中的元素节点，未命中或落在地址栏内返回 `NULL` / The hit element, `NULL` when nothing matched or the point is inside the chrome strip
 *
 * @note 减去地址栏高度后做命中测试；`<svg>` 子树无盒，改由 SVG 模块判定具体图形；非元素节点上溯到元素 / Subtracts the chrome height before hit-testing; an `<svg>` has no boxes for its children so the SVG module decides the shape; non-element nodes walk up to their element
 *
 * @see hit_test_box hpe_engine_mouse_move
 */
static HpeDomNode *node_from_hit(HpeEngine *engine, int x, int y)
{
    HpeBox *hit;
    HpeDomNode *node;
    /* mouse coords are window coords: the page viewport starts below the chrome */
    int cy = y - engine->chrome_h;
    if (cy < 0)
        return NULL;
    hit = hit_test_box(engine->root, (float)x, (float)cy, engine->scroll_y);
    node = hit ? hit->node : NULL;
    /* an <svg> has no boxes for its children, so the deepest box is the element
     * itself: ask the SVG module which shape is under the pointer. */
    if (node && node->tag && !strcmp(node->tag, "svg")) {
        HpeDomNode *shape = hpe_svg_hit_test(hit, (float)x, (float)cy + engine->scroll_y);
        if (shape)
            node = shape;
    }
    while (node && node->type != HPE_NODE_ELEMENT)
        node = node->parent;
    return node;
}

/**
 * @brief 清除子树的 `:hover` 标记 / Clear the `:hover` flags of a subtree
 *
 * @param[in,out] node 起始 DOM 节点 / Starting DOM node
 *
 * @note 递归自身及其所有兄弟子树 / Recurses over the node and all its siblings
 *
 * @see hpe_engine_mouse_move
 */
static void clear_hover_flags(HpeDomNode *node)
{
    for (; node; node = node->next_sibling) {
        node->hovered = 0;
        if (node->first_child)
            clear_hover_flags(node->first_child);
    }
}

/**
 * @brief 处理指针移动：更新 `:hover` 并派发 `mousemove` / Handle a pointer move: update `:hover` and fire `mousemove`
 *
 * @param[in,out] engine 引擎实例，NULL 或无盒树时返回 0 / Engine instance, returns 0 when NULL or without a box tree
 * @param[in]     x X 坐标（窗口坐标）/ X coordinate in window space
 * @param[in]     y Y 坐标（窗口坐标）/ Y coordinate in window space
 *
 * @return 需要重绘返回 1（悬停变化或有监听器改动了页面），否则 0 / 1 when a repaint is needed (hover changed or a listener mutated the page), otherwise 0
 *
 * @note 地址栏拖选进行中时交给地址栏处理；悬停变化总是强制重排（`:hover` 是选择器），否则仅在监听器产生变更时惰性 flush / Hands off to the address bar while a drag selection is live; a hover change always forces a relayout (`:hover` is a selector), otherwise a listener-driven change is flushed lazily
 *
 * @see node_from_hit clear_hover_flags
 */
int hpe_engine_mouse_move(HpeEngine *engine, int x, int y)
{
    HpeDomNode *node;
    int hover_changed, fired = 0;

    if (!engine || !engine->root)
        return 0;
    /* a drag that started inside the address bar keeps receiving the moves, even
     * once the pointer leaves the strip */
    if (hpe_chrome_mouse_move(engine, x, y))
        return 1;
    node = node_from_hit(engine, x, y); /* NULL inside the chrome strip */

    hover_changed = node != engine->hover_node;
    if (hover_changed) {
        clear_hover_flags(engine->doc);
        if (node)
            node->hovered = 1;
        engine->hover_node = node;
    }

    if (engine->js && node)
        fired = hpe_js_dom_dispatch(engine->js, node, "mousemove", x, y - engine->chrome_h);

    /* a hover change always needs a fresh cascade (":hover" is a selector the
     * style resolver evaluates), so force; handlers may also have edited the DOM
     * -- the flush picks those up at whatever depth the sink recorded. */
    if (hover_changed) {
        hpe_engine_relayout(engine);
        hpe_engine_refresh_status(engine);
    } else if (fired && engine->reflow_need != HPE_REFLOW_NONE) {
        hpe_engine_flush(engine, 0);
        hpe_engine_refresh_status(engine);
    }
    return hover_changed || fired;
}

/**
 * @brief 泵到期定时器与 rAF 回调 / Drain due timers and rAF callbacks
 *
 * @param[in,out] engine 引擎实例，无 JS 上下文时返回 0 / Engine instance, returns 0 without a JS context
 *
 * @return 有回调运行返回 1，否则 0 / 1 when at least one callback ran, otherwise 0
 *
 * @note 有回调时惰性 flush 并刷新状态 / Lazily flushes and refreshes the status when a callback ran
 *
 * @see hpe_engine_flush
 */
int hpe_engine_tick(HpeEngine *engine)
{
    int fired = 0;
    if (!engine || !engine->js)
        return 0;
    hpe_js_run_timers(engine->js, hpe_time_seconds(), &fired);
    if (fired) {
        hpe_engine_flush(engine, 0); /* only relayout if a timer touched the DOM */
        hpe_engine_refresh_status(engine);
    }
    return fired;
}

/**
 * @brief 派发一次鼠标按下 / 抬起 / Deliver a mouse press/release
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     x X 坐标（窗口坐标）/ X coordinate in window space
 * @param[in]     y Y 坐标（窗口坐标）/ Y coordinate in window space
 * @param[in]     button 鼠标按键（0 = 左键）/ Mouse button (0 = left)
 * @param[in]     down 按下为 1、抬起为 0 / 1 when pressed, 0 when released
 *
 * @return 页面发生变更返回 1（引擎已重排，窗口应重绘），否则 0 / 1 when the page mutated (the engine already relaid-out and the window should repaint), otherwise 0
 *
 * @note 等价于修饰键掩码为 0 的 `hpe_engine_mouse_event_mods` / Equivalent to calling `hpe_engine_mouse_event_mods` with a zero modifier mask
 *
 * @see hpe_engine_mouse_event_mods
 */
int hpe_engine_mouse_event(HpeEngine *engine, int x, int y, int button, int down)
{
    return hpe_engine_mouse_event_mods(engine, x, y, button, down, 0);
}

/**
 * @brief 带修饰键地派发一次鼠标按下 / 抬起 / Deliver a mouse press/release with modifiers
 *
 * @param[in,out] engine 引擎实例，NULL 或无盒树时返回 0 / Engine instance, returns 0 when NULL or without a box tree
 * @param[in]     x X 坐标（窗口坐标）/ X coordinate in window space
 * @param[in]     y Y 坐标（窗口坐标）/ Y coordinate in window space
 * @param[in]     button 鼠标按键（当前未使用）/ Mouse button (currently unused)
 * @param[in]     down 按下为 1、抬起为 0 / 1 when pressed, 0 when released
 * @param[in]     mods 修饰键掩码 / Modifier mask
 *
 * @return 触发监听器或使地址栏失焦返回 1，否则 0 / 1 when a listener fired or the address bar was blurred, otherwise 0
 *
 * @note 落在地址栏条带内或拖选进行中时交给地址栏；点击页面使地址栏失焦；抬起时依次派发 `mousedown`/`mouseup`/`click`，并按记录的变更做惰性 flush / Points inside the chrome strip or an in-progress drag go to the address bar; clicking the page blurs the bar; a release dispatches `mousedown`/`mouseup`/`click` in turn and lazily flushes the recorded mutations
 *
 * @see hpe_engine_mouse_event
 */
int hpe_engine_mouse_event_mods(HpeEngine *engine, int x, int y, int button, int down, int mods)
{
    HpeDomNode *node;
    int fired = 0;
    int blurred = 0;
    int py;

    HPE_UNUSED(button);
    if (!engine || !engine->root)
        return 0;

    /* clicks inside the chrome strip focus the address bar instead of the page */
    if (engine->chrome_h > 0 && (y < engine->chrome_h || engine->addr_drag))
        return hpe_chrome_mouse(engine, x, y, down, mods);
    if (engine->chrome_h > 0 && engine->addr_focus) {
        engine->addr_focus = 0; /* clicking the page leaves the address bar */
        if (engine->dirty == HPE_DIRTY_NONE)
            engine->dirty = HPE_DIRTY_CHROME;
        blurred = 1;
    }
    if (!engine->js)
        return blurred;

    node = node_from_hit(engine, x, y);
    if (!node)
        return blurred;
    py = y - engine->chrome_h;

    if (down)
        fired = hpe_js_dom_dispatch(engine->js, node, "mousedown", x, py);
    else {
        if (hpe_js_dom_dispatch(engine->js, node, "mouseup", x, py))
            fired = 1;
        if (hpe_js_dom_dispatch(engine->js, node, "click", x, py))
            fired = 1;
    }

    if (fired) {
        /* the handlers may have changed the DOM; only do the work their
         * mutations call for (a click that toggles a colour skips layout) */
        hpe_engine_flush(engine, 0);
        hpe_engine_refresh_status(engine);
    }
    return fired || blurred;
}

/**
 * @brief 测试钩子：对指定 id 的元素触发一次点击 / Test hook: fire a click on the element with the given id
 *
 * @param[in,out] engine 引擎实例，无 JS 上下文时返回 0 / Engine instance, returns 0 without a JS context
 * @param[in]     id 目标元素的 id / id of the target element
 *
 * @return 找到并派发了事件返回 1，否则 0 / 1 when the element was found and the events dispatched, otherwise 0
 *
 * @note 由 `HOPE_TEST_CLICK` 使用；用广度优先遍历按 id 找元素，再依次派发 `mousedown`/`mouseup`/`click` 并 flush，无需窗口即可走真实事件路径 / Used by `HOPE_TEST_CLICK`; finds the element by id with a breadth-first walk, then dispatches `mousedown`/`mouseup`/`click` and flushes, exercising the real event path without a window
 *
 * @see run_test_hooks
 */
static int click_element_by_id(HpeEngine *engine, const char *id)
{
    HpeDomNode **queue;
    int qh = 0, qt = 0, qcap = 1024;
    HpeDomNode *found = NULL;
    HpeDomNode *n = engine->doc;

    if (!engine->js || !id || !*id)
        return 0;
    queue = (HpeDomNode **)malloc(sizeof(HpeDomNode *) * (size_t)qcap);
    if (!queue)
        return 0;
    queue[qt++] = n;
    while (qh < qt) {
        HpeDomNode *cur = queue[qh++];
        HpeDomNode *c;
        for (c = cur->first_child; c; c = c->next_sibling) {
            if (c->type == HPE_NODE_ELEMENT) {
                const char *aid = hpe_dom_attr(c, "id");
                if (aid && strcmp(aid, id) == 0) {
                    found = c;
                    break;
                }
                if (qt < qcap)
                    queue[qt++] = c;
            }
        }
        if (found)
            break;
    }
    free(queue);
    if (!found)
        return 0;
    hpe_js_dom_dispatch(engine->js, found, "mousedown", 0, 0);
    hpe_js_dom_dispatch(engine->js, found, "mouseup", 0, 0);
    hpe_js_dom_dispatch(engine->js, found, "click", 0, 0);
    hpe_engine_flush(engine, 0);
    hpe_engine_refresh_status(engine);
    return 1;
}

/**
 * @brief 测试钩子：把指定 id 元素的文本内容打印到 stdout / Test hook: print the text content of the element with the given id to stdout
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     id 目标元素的 id，空则直接返回 / id of the target element, returns early when empty
 *
 * @note 由 `HOPE_TEST_TEXT` 使用；深度优先收集子树文本后换行输出，找不到 id 时打印 `<no element id=...>`；供无头运行断言脚本对 DOM 的修改 / Used by `HOPE_TEST_TEXT`; concatenates the subtree's text and prints it followed by a newline, printing `<no element id=...>` when the id is absent; lets headless runs assert script-driven DOM updates
 *
 * @see run_test_hooks
 */
static void print_text_by_id(HpeEngine *engine, const char *id)
{
    HpeDomNode *stack[256];
    int sp = 0;
    HpeDomNode *n = engine->doc;

    if (!id || !*id)
        return;
    stack[sp++] = n;
    while (sp > 0) {
        HpeDomNode *cur = stack[--sp];
        HpeDomNode *c;
        if (cur->type == HPE_NODE_ELEMENT) {
            const char *aid = hpe_dom_attr(cur, "id");
            if (aid && strcmp(aid, id) == 0) {
                /* found: print own subtree text */
                HpeDomNode *tstack[256];
                int tsp = 0;
                tstack[tsp++] = cur;
                while (tsp > 0) {
                    HpeDomNode *t = tstack[--tsp];
                    HpeDomNode *tc;
                    if (t->type == HPE_NODE_TEXT && t->text)
                        fprintf(stdout, "%s", t->text);
                    for (tc = t->first_child; tc; tc = tc->next_sibling)
                        if (tsp < 256)
                            tstack[tsp++] = tc;
                }
                fprintf(stdout, "\n");
                return;
            }
        }
        for (c = cur->first_child; c; c = c->next_sibling)
            if (sp < 256)
                stack[sp++] = c;
    }
    fprintf(stdout, "<no element id=%s>\n", id);
}
