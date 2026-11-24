#include "layout/hpe_layout.h"

#include <math.h>
#include <string.h>

#include "base/hpe_util.h"

/* ---------------- layout context ---------------- */

typedef struct {
    float vw, vh;         /* viewport size; used as the initial containing block */
    float root_font_size; /* the root element's font size, for `rem` */
} LayoutCtx;

/**
 * @brief 取样式对应的字体对象 / Fetch the font object for a style
 *
 * @param[in]     st 计算样式 / Computed style
 *
 * @return 匹配字号/字重/字体族的字体 / Font matching the size, weight and family
 */

static HpeFont *font_for(const HpeStyle *st)
{
    return hpe_font_get(st->font_size, st->font_weight, st->family);
}

/**
 * @brief 解析长度值（含 em/rem/vw/vh/ch 等相对单位）/ Resolve a length value including relative units like `em`/`rem`/`vw`/`vh`/`ch`
 *
 * @param[in]     ctx 布局上下文，可为 NULL / Layout context, may be NULL
 * @param[in]     st 计算样式 / Computed style
 * @param[in]     len 待解析的长度 / Length to resolve
 * @param[in]     base 百分比等相对长度所依据的基准 / Basis for percentages and other relative lengths
 *
 * @return 解析后的像素长度 / Resolved length in pixels
 *
 * @note `ch` 需要元素自身字体中 `0` 字形的步进宽度 / `ch` needs the advance of the `0` glyph in the element's own font
 */
static float resolve_len(const LayoutCtx *ctx, const HpeStyle *st, HpeLength len, float base)
{
    float ch = 0.0f;
    if (len.unit == HPE_LEN_CH) {
        HpeFont *f = font_for(st);
        if (f)
            ch = hpe_font_advance(f, '0');
    }
    return hpe_len_resolve_full(len, base, st->font_size, ctx ? ctx->root_font_size : 0.0f,
                                ctx ? ctx->vw : 0.0f, ctx ? ctx->vh : 0.0f, ch);
}

/**
 * @brief 解析盒子边（padding/border/尺寸）长度并截断负值 / Resolve a box-side length, clamping negatives to zero
 *
 * @param[in]     ctx 布局上下文 / Layout context
 * @param[in]     st 计算样式 / Computed style
 * @param[in]     len 待解析的长度 / Length to resolve
 * @param[in]     base 相对长度基准 / Basis for relative lengths
 *
 * @return 非负的像素长度 / Non-negative length in pixels
 */
static float side_px(const LayoutCtx *ctx, const HpeStyle *st, HpeLength len, float base)
{
    float v = resolve_len(ctx, st, len, base);
    return v < 0.0f ? 0.0f : v;
}

/**
 * @brief 解析外边距长度（可为负，`auto` 视为 0）/ Resolve a margin length, negative allowed, `auto` as 0
 *
 * @param[in]     ctx 布局上下文 / Layout context
 * @param[in]     st 计算样式 / Computed style
 * @param[in]     len 外边距长度 / Margin length
 * @param[in]     base 相对长度基准 / Basis for relative lengths
 *
 * @return 像素外边距 / Margin in pixels
 *
 * @note `auto` 由块级/flex 流程自行分配后写回具体像素值 / The block and flex passes resolve `auto` themselves and write a concrete px length back
 */
static float margin_px(const LayoutCtx *ctx, const HpeStyle *st, HpeLength len, float base)
{
    if (len.unit == HPE_LEN_AUTO)
        return 0.0f;
    return resolve_len(ctx, st, len, base);
}

/**
 * @brief 求样式对应的行高 / Resolve the line height of a style
 *
 * @param[in]     st 计算样式 / Computed style
 * @param[in]     font 字体对象 / Font object
 *
 * @return 行高（像素）/ Line height in pixels
 *
 * @note `line-height` 为负时按字号比例换算，为 0 时回退到字体自身的行高 / A negative `line-height` is a factor of the font size; 0 falls back to the font's own line height
 */
static float style_line_height(const HpeStyle *st, HpeFont *font)
{
    if (st->line_height < 0.0f)
        return -st->line_height * st->font_size;
    if (st->line_height > 0.0f)
        return st->line_height;
    return hpe_font_line_height(font);
}

/**
 * @brief 判断码点是否落在 CJK 范围 / Test whether a code point lies in a CJK range
 *
 * @param[in]     cp Unicode 码点 / Unicode code point
 *
 * @return 属于 CJK 返回 1，否则 0 / 1 for a CJK code point, else 0
 */
static int is_cjk(uint32_t cp)
{
    return (cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
           (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
           (cp >= 0x20000 && cp <= 0x3FFFF);
}

/**
 * @brief 把四边外边距解析进数组 / Resolve the four margin sides into an array
 *
 * @param[in]     ctx 布局上下文 / Layout context
 * @param[in]     st 计算样式 / Computed style
 * @param[in]     base 相对长度基准 / Basis for relative lengths
 * @param[out]    m 接收 top/right/bottom/left 外边距 / Receives margins in top/right/bottom/left order
 */
static void resolve_margins(const LayoutCtx *ctx, const HpeStyle *st, float base, float m[4])
{
    int i;
    for (i = 0; i < 4; i++)
        m[i] = margin_px(ctx, st, st->margin[i], base);
}

/**
 * @brief 按 CSS 2.1 §8.3.1 折叠两个相邻垂直外边距 / Collapse two adjoining vertical margins per CSS 2.1 §8.3.1
 *
 * @param[in]     a 第一个外边距 / First margin
 * @param[in]     b 第二个外边距 / Second margin
 *
 * @return 折叠后的外边距 / Collapsed margin
 *
 * @note 同为正经取较大者，同为负取更负者，异号取和 / Both positive takes the larger, both negative the more negative, mixed signs their sum
 */
static float collapse_margins(float a, float b)
{
    if (a >= 0.0f && b >= 0.0f)
        return a > b ? a : b;
    if (a < 0.0f && b < 0.0f)
        return a < b ? a : b;
    return a + b;
}

/* spacing between two adjacent glyphs of a run; `sp` carries the resolved
 * letter/word spacing of the run's style (see LayoutCtx-adjacent helpers). */
typedef struct {
    float letter; /* letter-spacing: added after every glyph */
    float word;   /* word-spacing: added after every inter-word space */
} TextSpacing;

/**
 * @brief 解析 letter-spacing 与 word-spacing / Resolve `letter-spacing` and `word-spacing`
 *
 * @param[in]     ctx 布局上下文 / Layout context
 * @param[in]     st 计算样式 / Computed style
 * @param[in]     base 相对长度基准 / Basis for relative lengths
 *
 * @return 字形间距参数 / Glyph spacing parameters
 *
 * @note 负 letter-spacing 不支持，按 0 处理 / Negative tracking is not modelled and is clamped to 0
 */
static TextSpacing text_spacing(const LayoutCtx *ctx, const HpeStyle *st, float base)
{
    TextSpacing sp;
    sp.letter = resolve_len(ctx, st, st->letter_spacing, base);
    sp.word = resolve_len(ctx, st, st->word_spacing, base);
    if (sp.letter < 0.0f)
        sp.letter = 0.0f; /* negative tracking is not modelled */
    return sp;
}

/**
 * @brief 在文档坐标中平移一个盒及其整棵子树 / Translate a box and its whole subtree in document space
 *
 * @param[in,out] box 待平移的盒 / Box to translate
 * @param[in]     dx X 方向偏移 / Delta X
 * @param[in]     dy Y 方向偏移 / Delta Y
 *
 * @note 同时平移三个盒矩形与所有行的 y / Shifts all three box rects and every line's y as well
 */
static void box_shift(HpeBox *box, float dx, float dy)
{
    HpeBox *c;
    int i;
    box->border_box.x += dx;
    box->border_box.y += dy;
    box->content_box.x += dx;
    box->content_box.y += dy;
    box->pad_box.x += dx;
    box->pad_box.y += dy;
    for (i = 0; i < box->line_count; i++)
        box->lines[i].y += dy;
    for (c = box->first_child; c; c = c->next)
        box_shift(c, dx, dy);
}

/**
 * @brief 新建一个零初始化的布局盒 / Create a zero-initialised layout box
 *
 * @param[in]     kind 盒类型 / Box kind
 *
 * @return 新分配的盒 / Newly allocated box
 */
static HpeBox *box_new(HpeBoxKind kind)
{
    HpeBox *box = (HpeBox *)hpe_calloc(1, sizeof(HpeBox));
    box->kind = kind;
    return box;
}

/**
 * @brief 把一个盒追加为父盒的最后一个子盒 / Append a box as the parent's last child
 *
 * @param[in,out] parent 父盒 / Parent box
 * @param[in,out] child 待追加的子盒 / Child box to append
 */
static void box_append(HpeBox *parent, HpeBox *child)
{
    child->parent = parent;
    if (parent->last_child) {
        parent->last_child->next = child;
        parent->last_child = child;
    } else {
        parent->first_child = parent->last_child = child;
    }
}

/* forward declarations (functions are mutually recursive) */
static float layout_block(LayoutCtx *ctx, HpeBox *box, float x, float y, float avail_width,
                          float avail_height);
static void layout_inline_block(LayoutCtx *ctx, HpeBox *box, float parent_inner_w);
static float measure_preferred_w(LayoutCtx *ctx, HpeBox *box);

/* ---------------- box tree ---------------- */

static HpeBox *build_element(HpeDomNode *node, HpeStylesheet *sheet,
                             const HpeStyle *parent_style);

/**
 * @brief 为 `<svg>` 元素构造替换块盒 / Build the replaced block box for an `<svg>` element
 *
 * @param[in]     node `<svg>` DOM 节点 / The `<svg>` DOM node
 * @param[in]     style 计算样式 / Computed style
 *
 * @return 对应的块盒 / The corresponding block box
 *
 * @note 固有尺寸取自 `width`/`height` 属性；子树由 hpe_svg_paint 直接从 DOM 绘制，不建盒 / Intrinsic size comes from the `width`/`height` attributes; the subtree is painted from the DOM by hpe_svg_paint instead of being turned into boxes
 */
static HpeBox *build_svg_box(HpeDomNode *node, HpeStyle style)
{
    const char *aw = hpe_dom_attr(node, "width");
    const char *ah = hpe_dom_attr(node, "height");
    float w = aw ? (float)atof(aw) : 0.0f;
    float h = ah ? (float)atof(ah) : 0.0f;
    HpeBox *box;
    if (w > 0.0f)
        style.width = hpe_len_px(w);
    if (h > 0.0f)
        style.height = hpe_len_px(h);
    box = box_new(HPE_BOX_BLOCK);
    box->node = node;
    box->style = style;
    return box;
}

/**
 * @brief 递归构建节点的子盒 / Recursively build the child boxes of a node
 *
 * @param[in,out] box 父盒 / Parent box
 * @param[in]     node 父 DOM 节点 / Parent DOM node
 * @param[in]     sheet 样式表 / Stylesheet
 * @param[in]     style 父元素样式 / Parent style
 *
 * @note 元素子节点递归建盒，文本子节点生成文本盒 / Element children recurse; text children become text boxes
 */
static void build_children(HpeBox *box, HpeDomNode *node, HpeStylesheet *sheet,
                           const HpeStyle *style)
{
    HpeDomNode *child;
    for (child = node->first_child; child; child = child->next_sibling) {
        HpeBox *cb = NULL;
        if (child->type == HPE_NODE_ELEMENT) {
            cb = build_element(child, sheet, style);
        } else if (child->type == HPE_NODE_TEXT) {
            if (child->text && child->text[0]) {
                HpeStyle tstyle;
                hpe_css_compute(sheet, NULL, style, &tstyle);
                cb = box_new(HPE_BOX_TEXT);
                cb->node = child;
                cb->style = tstyle;
                cb->text = child->text;
                cb->text_len = (int)strlen(child->text);
            }
        }
        if (cb)
            box_append(box, cb);
    }
}

/**
 * @brief 为一个元素构建盒子树 / Build the box subtree for one element
 *
 * @param[in]     node 元素 DOM 节点 / Element DOM node
 * @param[in]     sheet 样式表 / Stylesheet
 * @param[in]     parent_style 父元素样式 / Parent style
 *
 * @return 根盒；`display:none` 时返回 `NULL` / The root box, or `NULL` for `display:none`
 *
 * @note 绝对/固定定位盒块化，浮动盒作为原子行内级（收缩适应）盒处理 / Absolutely and fixed positioned boxes blockify; floated boxes become atomic inline-level shrink-to-fit boxes
 */
static HpeBox *build_element(HpeDomNode *node, HpeStylesheet *sheet, const HpeStyle *parent_style)
{
    HpeStyle style;
    HpeBox *box;

    hpe_css_compute(sheet, node, parent_style, &style);
    if (style.display == HPE_DISPLAY_NONE)
        return NULL;

    /* flex items blockify: an inline child of a flex container is a block box
     * (CSS Flexbox §4 "Flex Items"), which is also what keeps its text from
     * being dropped by the flex pass. */
    if (parent_style && parent_style->display == HPE_DISPLAY_FLEX &&
        style.display == HPE_DISPLAY_INLINE)
        style.display = HPE_DISPLAY_BLOCK;

    if (!strcmp(node->tag, "svg"))
        return build_svg_box(node, style);

    /* absolutely / fixed positioned boxes blockify their display;
     * floated boxes are laid out as atomic inline-level (shrink-to-fit) boxes */
    if (style.position == HPE_POS_ABSOLUTE || style.position == HPE_POS_FIXED)
        box = box_new(HPE_BOX_BLOCK);
    else if (style.float_side != HPE_FLOAT_NONE || style.display == HPE_DISPLAY_INLINE_BLOCK)
        box = box_new(HPE_BOX_INLINE_BLOCK);
    else if (style.display == HPE_DISPLAY_BLOCK || style.display == HPE_DISPLAY_FLEX)
        box = box_new(HPE_BOX_BLOCK);
    else
        box = box_new(HPE_BOX_INLINE);

    box->node = node;
    box->style = style;
    build_children(box, node, sheet, &style);
    return box;
}

/* ---------------- inline content ---------------- */

typedef struct {
    const char *text;
    int len;
    const HpeStyle *style;
    int hard_break;
    HpeBox *atomic_box; /* non-NULL => atomic inline-level (inline-block) box */
} HpeSegment;

typedef struct {
    HpeSegment *items;
    int count;
    size_t cap;
} HpeParagraph;

/**
 * @brief 向段落追加一个文本片段 / Append a text segment to a paragraph
 *
 * @param[in,out] para 段落 / Paragraph
 * @param[in]     text 片段文本 / Segment text
 * @param[in]     len 文本字节长度 / Segment byte length
 * @param[in]     style 片段样式 / Segment style
 * @param[in]     br 硬换行标志 / Hard-break flag
 */
static void para_add(HpeParagraph *para, const char *text, int len, const HpeStyle *style, int br)
{
    HpeSegment *seg;
    para->items = (HpeSegment *)hpe_grow(para->items, &para->cap, (size_t)para->count + 1,
                                         sizeof(HpeSegment));
    seg = &para->items[para->count++];
    memset(seg, 0, sizeof(*seg));
    seg->text = text;
    seg->len = len;
    seg->style = style;
    seg->hard_break = br;
}

/**
 * @brief 向段落追加一个原子行内盒片段 / Append an atomic inline-level segment to a paragraph
 *
 * @param[in,out] para 段落 / Paragraph
 * @param[in]     box 原子行内块盒 / Atomic inline-block box
 */
static void para_add_atomic(HpeParagraph *para, HpeBox *box)
{
    HpeSegment *seg;
    para->items = (HpeSegment *)hpe_grow(para->items, &para->cap, (size_t)para->count + 1,
                                         sizeof(HpeSegment));
    seg = &para->items[para->count++];
    memset(seg, 0, sizeof(*seg));
    seg->atomic_box = box;
}

/**
 * @brief 收集行内级内容到段落 / Collect inline-level content into a paragraph
 *
 * @param[in]     ctx 布局上下文 / Layout context
 * @param[in,out] box 待收集的盒 / Box to collect
 * @param[in]     inner_w 内容区宽度 / Inner width
 * @param[out]    para 接收内容的段落 / Paragraph receiving the content
 *
 * @note 行内块盒在此先行排版以便行构建器得知其尺寸；浮动盒被跳过 / Inline-block boxes are laid out here so the line builder knows their size; floated boxes are skipped
 */
static void collect_inline(LayoutCtx *ctx, HpeBox *box, float inner_w, HpeParagraph *para)
{
    HpeBox *child;
    if (box->kind == HPE_BOX_TEXT) {
        para_add(para, box->text, box->text_len, &box->style, 0);
        return;
    }
    /* floated boxes are placed by the block container, not inlined */
    if (box->style.float_side != HPE_FLOAT_NONE)
        return;
    if (box->kind == HPE_BOX_INLINE_BLOCK) {
        layout_inline_block(ctx, box, inner_w);
        para_add_atomic(para, box);
        return;
    }
    if (box->node && strcmp(box->node->tag, "br") == 0) {
        para_add(para, NULL, 0, &box->style, 1);
        return;
    }
    for (child = box->first_child; child; child = child->next)
        collect_inline(ctx, child, inner_w, para);
}

/**
 * @brief 在块中开启一条新行 / Open a new line in a block
 *
 * @param[in,out] block 块盒 / Block box
 *
 * @return 新行的下标 / Index of the new line
 *
 * @note 必要时扩容行数组 / Grows the line array when needed
 */

static int line_begin(HpeBox *block)
{
    block->lines = (HpeLine *)hpe_grow(block->lines, &block->line_cap,
                                       (size_t)block->line_count + 1, sizeof(HpeLine));
    memset(&block->lines[block->line_count], 0, sizeof(HpeLine));
    return block->line_count++;
}

/**
 * @brief 向行追加一个文本 run / Append a text run to a line
 *
 * @param[in,out] block 块盒 / Block box
 * @param[in]     idx 行下标 / Line index
 * @param[in]     text run 文本 / Run text
 * @param[in]     len 文本长度 / Text length
 * @param[in]     st run 样式 / Run style
 * @param[in]     font 字体 / Font
 * @param[in]     x 相对内容盒的 X 位置 / X relative to the content box
 * @param[in]     width run 宽度 / Run width
 * @param[in]     ascent 上伸部 / Ascent
 * @param[in]     descent 下伸部 / Descent
 * @param[in]     line_height 行高 / Line height
 *
 * @note 颜色取自 run 样式 / The colour is taken from the run style
 */
static void line_add_run(HpeBox *block, int idx, const char *text, int len, const HpeStyle *st,
                         HpeFont *font, float x, float width, float ascent, float descent,
                         float line_height)
{
    HpeLine *line = &block->lines[idx];
    HpeTextRun *run;

    line->runs = (HpeTextRun *)hpe_grow(line->runs, &line->run_cap,
                                        (size_t)line->run_count + 1, sizeof(HpeTextRun));
    run = &line->runs[line->run_count++];
    memset(run, 0, sizeof(*run));
    run->text = text;
    run->len = len;
    run->font = font;
    run->color = st->color;
    run->x = x;
    run->width = width;
    run->ascent = ascent;
    run->descent = descent;
    run->line_height = line_height;
}

/* ---------------- float bands ---------------- */

/**
 * @brief 求区间内浮动态带留给行盒的水平范围 / Compute the horizontal span a float band leaves for line boxes in a y-range
 *
 * @param[in]     band 浮动态带 / Float band
 * @param[in]     cb_x 包含块内容区左边界 / Content-box left of the container
 * @param[in]     cb_w 包含块内容区宽度 / Content-box width of the container
 * @param[in]     y0 区间上边界 / Span top
 * @param[in]     y1 区间下边界 / Span bottom
 * @param[out]    out_left 可用左边界 / Available left edge
 * @param[out]    out_right 可用右边界 / Available right edge
 */
static void band_edges(HpeFloatBand *band, float cb_x, float cb_w, float y0, float y1,
                       float *out_left, float *out_right)
{
    float L = cb_x, R = cb_x + cb_w;
    int i;
    for (i = 0; i < band->count; i++) {
        HpeFloatBox *f = &band->items[i];
        if (f->bottom <= y0 || f->top >= y1)
            continue;
        if (f->side == HPE_FLOAT_LEFT && f->right > L)
            L = f->right;
        if (f->side == HPE_FLOAT_RIGHT && f->left < R)
            R = f->left;
    }
    *out_left = L;
    *out_right = R;
}

/**
 * @brief 沿同一 BFC 的祖先链合并浮动态带范围 / Merge float band extents along the ancestor chain of the same BFC
 *
 * @param[in]     block 目标块盒 / Target block box
 * @param[in]     y0 区间上边界 / Span top
 * @param[in]     y1 区间下边界 / Span bottom
 * @param[out]    out_left 可用左边界 / Available left edge
 * @param[out]    out_right 可用右边界 / Available right edge
 *
 * @note 结果夹紧到块自身的内容盒；浮动祖先本身即是 BFC，遍历到此为止 / The result is clamped to the block's own content box; a floated ancestor is itself a BFC, so the walk stops there
 */
static void band_edges_inherited(HpeBox *block, float y0, float y1, float *out_left,
                                 float *out_right)
{
    HpeBox *b = block->parent;
    float L = block->content_box.x;
    float R = block->content_box.x + block->content_box.w;
    while (b) {
        if (b->floats.count > 0) {
            float l, r;
            band_edges(&b->floats, b->content_box.x, b->content_box.w, y0, y1, &l, &r);
            if (l > L)
                L = l;
            if (r < R)
                R = r;
        }
        if (b->style.float_side != HPE_FLOAT_NONE)
            break; /* floated boxes establish a BFC */
        b = b->parent;
    }
    if (L < block->content_box.x)
        L = block->content_box.x;
    if (R > block->content_box.x + block->content_box.w)
        R = block->content_box.x + block->content_box.w;
    if (R < L)
        R = L;
    *out_left = L;
    *out_right = R;
}

/* computes one line's final metrics, applies text-align inside the float-aware
 * available span, positions atomic boxes, and advances *row_y */

/**
 * @brief 从行的 runs 度量行高、基线与右边界 / Measure a line's height, baseline and right edge from its runs
 *
 * @param[in]     line 行 / Line
 * @param[out]    out_h 行高 / Line height
 * @param[out]    out_base 基线深度 / Baseline depth
 * @param[out]    out_w 右边界 / Right edge
 *
 * @note 原子盒以盒底为基线；无字体的 run 被跳过 / Atomic boxes sit on their bottom edge; runs without a font are skipped
 */
static void line_measure(HpeLine *line, float *out_h, float *out_base, float *out_w)
{
    float height = 0.0f, baseline = 0.0f, width = 0.0f;
    int r;
    for (r = 0; r < line->run_count; r++) {
        HpeTextRun *run = &line->runs[r];
        float run_base;
        if (run->atomic) {
            run_base = run->ascent; /* baseline at the box bottom */
        } else {
            float extra;
            if (!run->font)
                continue;
            extra = run->line_height - (run->ascent + run->descent);
            if (extra < 0.0f)
                extra = 0.0f;
            run_base = run->ascent + extra * 0.5f;
        }
        if (run->line_height > height)
            height = run->line_height;
        if (run_base > baseline)
            baseline = run_base;
        if (run->x + run->width > width)
            width = run->x + run->width;
    }
    *out_h = height;
    *out_base = baseline;
    *out_w = width;
}

/**
 * @brief 按 vertical-align 放置行上的原子行内盒 / Position the line's atomic inline boxes per `vertical-align`
 *
 * @param[in]     ctx 布局上下文 / Layout context
 * @param[in,out] block 块盒 / Block box
 * @param[in,out] line 行 / Line
 * @param[in]     inner_w 内容区宽度 / Inner width
 *
 * @note 默认 `baseline` 使盒底对齐行基线，其余关键字相对行盒解析 / The default `baseline` drops the box so its bottom sits on the line baseline; other keywords resolve against the line box
 */
static void line_place_atomics(LayoutCtx *ctx, HpeBox *block, HpeLine *line, float inner_w)
{
    int r;
    for (r = 0; r < line->run_count; r++) {
        HpeTextRun *run = &line->runs[r];
        HpeBox *ab;
        float bx, by;
        if (!run->atomic)
            continue;
        ab = run->atomic;
        bx = block->content_box.x + run->x + margin_px(ctx, &ab->style, ab->style.margin[3], inner_w);
        switch (ab->style.vertical_align) {
        case HPE_VALIGN_TOP:
        case HPE_VALIGN_TEXT_TOP:
            by = line->y;
            break;
        case HPE_VALIGN_BOTTOM:
        case HPE_VALIGN_TEXT_BOTTOM:
            by = line->y + line->height - ab->border_box.h;
            break;
        case HPE_VALIGN_MIDDLE:
            by = line->y + (line->height - ab->border_box.h) * 0.5f;
            break;
        default: /* baseline */
            by = line->y + line->baseline - ab->border_box.h;
            break;
        }
        box_shift(ab, bx - ab->border_box.x, by - ab->border_box.y);
    }
}

/**
 * @brief 确定一行的最终度量并应用 text-align / Finalise one line's metrics and apply `text-align`
 *
 * @param[in]     ctx 布局上下文 / Layout context
 * @param[in,out] block 块盒 / Block box
 * @param[in]     li 行下标 / Line index
 * @param[in]     inner_w 内容区宽度 / Inner width
 * @param[in]     band 浮动态带 / Float band
 * @param[in,out] row_y 纵向游标，按行高推进 / Vertical cursor, advanced by the line height
 *
 * @note 空行使用块字体的行高、基线取行高的 0.8 / An empty line takes the block font's line height with a baseline at 0.8 of it
 */
static void finalize_line(LayoutCtx *ctx, HpeBox *block, int li, float inner_w, HpeFloatBand *band,
                          float *row_y)
{
    HpeLine *line = &block->lines[li];
    float height, baseline, width;
    float line_x0, line_x1, offset = 0.0f;
    int r;

    line_measure(line, &height, &baseline, &width);
    if (line->run_count == 0) {
        HpeFont *f = font_for(&block->style);
        height = style_line_height(&block->style, f);
        baseline = height * 0.8f;
    }

    line->width = width;
    line->height = height;
    line->baseline = baseline;
    line->y = *row_y;

    /* recompute the available span with the now-known line height */
    {
        float L, R;
        band_edges_inherited(block, *row_y, *row_y + height, &L, &R);
        line_x0 = L - block->content_box.x;
        line_x1 = R - block->content_box.x;
    }

    if (block->style.text_align == HPE_ALIGN_CENTER)
        offset = (line_x1 - line_x0 - width) * 0.5f;
    else if (block->style.text_align == HPE_ALIGN_RIGHT)
        offset = line_x1 - line_x0 - width;
    if (offset > 0.0f) {
        for (r = 0; r < line->run_count; r++)
            line->runs[r].x += offset;
    }

    line_place_atomics(ctx, block, line, inner_w);
    *row_y += height;
}

/* running state while laying out one paragraph into a block's lines */
typedef struct {
    LayoutCtx *ctx;
    HpeBox *block;
    HpeFloatBand *band;
    float inner_w;
    float row_y;
    int line_idx;
    float x;              /* pen position relative to the content box */
    float line_x0, line_x1; /* float-aware available span of the open line */
    int pending_space;    /* a collapsed space waits to be emitted before the next word */
    float indent;
    int indent_pending;   /* text-indent shifts only the first line */
} ParaState;

/**
 * @brief 开启一条新行并固定其浮动感知的可用范围 / Open a new line and fix its float-aware available span
 *
 * @param[in,out] ps 段落排版状态 / Paragraph layout state
 * @param[in]     lh_est 估算行高 / Estimated line height
 *
 * @note 待应用的首行缩进在此前移笔位 / A pending first-line indent shifts the pen position here
 */
static void para_open_line(ParaState *ps, float lh_est)
{
    float L, R;
    band_edges_inherited(ps->block, ps->row_y, ps->row_y + lh_est, &L, &R);
    ps->line_x0 = L - ps->block->content_box.x;
    ps->line_x1 = R - ps->block->content_box.x;
    if (ps->line_x1 < ps->line_x0)
        ps->line_x1 = ps->line_x0;
    ps->x = ps->line_x0;
    if (ps->indent_pending) {
        ps->x += ps->indent;
        ps->indent_pending = 0;
    }
    ps->line_idx = line_begin(ps->block);
}

/**
 * @brief 关闭当前行并推进纵向游标 / Close the current line and advance the vertical cursor
 *
 * @param[in,out] ps 段落排版状态 / Paragraph layout state
 */
static void para_close_line(ParaState *ps)
{
    finalize_line(ps->ctx, ps->block, ps->line_idx, ps->inner_w, ps->band, &ps->row_y);
    ps->line_idx = -1;
}

/**
 * @brief 放置一个原子行内盒，必要时换行 / Place one atomic inline box, wrapping when it does not fit
 *
 * @param[in,out] ps 段落排版状态 / Paragraph layout state
 * @param[in,out] ab 原子行内块盒 / Atomic inline-block box
 *
 * @note 为其预留左边距 + 边框盒宽 + 右边距 / Reserves its left margin plus border-box width plus right margin
 */
static void para_place_atomic(ParaState *ps, HpeBox *ab)
{
    float mL = margin_px(ps->ctx, &ab->style, ab->style.margin[3], ps->inner_w);
    float mR = margin_px(ps->ctx, &ab->style, ab->style.margin[1], ps->inner_w);
    float bh = ab->border_box.h;
    float total = mL + ab->border_box.w + mR;
    int rc;

    if (ps->x > ps->line_x0 + 0.01f && ps->x + total > ps->line_x1 + 0.5f) {
        para_close_line(ps);
        ps->pending_space = 0;
    }
    if (ps->line_idx < 0)
        para_open_line(ps, bh);
    line_add_run(ps->block, ps->line_idx, NULL, 0, &ab->style, NULL, ps->x, total, bh, 0.0f, bh);
    rc = ps->block->lines[ps->line_idx].run_count - 1;
    ps->block->lines[ps->line_idx].runs[rc].atomic = ab;
    ps->x += total;
}

/**
 * @brief 以一个硬换行结束当前行 / End the current line with a hard break
 *
 * @param[in,out] ps 段落排版状态 / Paragraph layout state
 * @param[in]     style 换行所在样式（用于空行行高）/ Style of the break, used for an empty line's height
 */
static void para_hard_break(ParaState *ps, const HpeStyle *style)
{
    if (ps->line_idx < 0)
        para_open_line(ps, style_line_height(style, font_for(style)));
    para_close_line(ps);
    ps->pending_space = 0;
}

/**
 * @brief 放置一个空白字形 / Place one whitespace glyph
 *
 * @param[in,out] ps 段落排版状态 / Paragraph layout state
 * @param[in]     seg 所属片段 / Owning segment
 * @param[in]     font 字体 / Font
 * @param[in]     preserve 保留空白模式为 1 / 1 in a preserve-whitespace mode
 * @param[in]     lh 行高 / Line height
 * @param[in]     asc 上伸部 / Ascent
 * @param[in]     desc 下伸部 / Descent
 * @param[in]     sp 字形间距 / Glyph spacing
 * @param[in]     cp 空白码点 / Whitespace code point
 *
 * @note 非保留模式下折叠为 pending_space，由下一个单词输出 / In non-preserve modes the space collapses into `pending_space`, emitted by the next word
 */
static void para_place_space(ParaState *ps, const HpeSegment *seg, HpeFont *font, int preserve,
                             float lh, float asc, float desc, TextSpacing sp, uint32_t cp)
{
    float space_w;
    if (!preserve) {
        ps->pending_space = 1;
        return;
    }
    if (ps->line_idx < 0)
        para_open_line(ps, lh);
    space_w = hpe_font_advance(font, ' ') + (cp == ' ' ? sp.word : 0.0f) + sp.letter;
    line_add_run(ps->block, ps->line_idx, " ", 1, seg->style, font, ps->x, space_w, asc, desc, lh);
    ps->x += space_w;
}

/**
 * @brief 度量一个非空白文本簇（单个 CJK 字或一段非空白非 CJK 字符）/ Measure one non-space cluster: a lone CJK char or a run of non-space non-CJK chars
 *
 * @param[in]     font 字体 / Font
 * @param[in]     p 起始指针 / Start pointer
 * @param[in]     end 文本结束指针 / Text end pointer
 * @param[in]     letter_spacing 字间距 / Letter spacing
 * @param[out]    out_w 簇宽度 / Cluster width
 *
 * @return 指向簇后一字节的指针 / Pointer just past the cluster
 *
 * @note 遇到空白、换行或 CJK 字符即停止 / Stops at whitespace, newline or a CJK character
 */
static const char *text_measure_cluster(HpeFont *font, const char *p, const char *end,
                                        float letter_spacing, float *out_w)
{
    uint32_t cp;
    int n = hpe_utf8_next(p, &cp);
    float w = 0.0f;

    if (n <= 0) {
        *out_w = 0.0f;
        return p;
    }
    if (is_cjk(cp)) {
        *out_w = hpe_font_advance(font, cp) + letter_spacing;
        return p + n;
    }
    while (p < end) {
        uint32_t c2;
        int n2 = hpe_utf8_next(p, &c2);
        if (n2 <= 0)
            break;
        if (c2 == ' ' || c2 == '\n' || c2 == '\r' || c2 == '\t' || c2 == 0x00A0 || is_cjk(c2))
            break;
        w += hpe_font_advance(font, c2) + letter_spacing;
        p += n2;
    }
    *out_w = w;
    return p;
}

/**
 * @brief 输出一个已度量的文本簇，应用待处理空格并在放不下时换行 / Emit one measured cluster, applying a pending collapsed space and wrapping when it no longer fits
 *
 * @param[in,out] ps 段落排版状态 / Paragraph layout state
 * @param[in]     seg 所属片段 / Owning segment
 * @param[in]     font 字体 / Font
 * @param[in]     asc 上伸部 / Ascent
 * @param[in]     desc 下伸部 / Descent
 * @param[in]     lh 行高 / Line height
 * @param[in]     preserve 保留空白模式为 1 / 1 in a preserve-whitespace mode
 * @param[in]     wrap 允许换行为 1 / 1 to allow wrapping
 * @param[in]     sp 字形间距 / Glyph spacing
 * @param[in]     chunk 簇文本 / Cluster text
 * @param[in]     chunk_len 簇字节长度 / Cluster byte length
 * @param[in]     chunk_w 簇宽度 / Cluster width
 */
static void para_emit_word(ParaState *ps, const HpeSegment *seg, HpeFont *font, float asc,
                           float desc, float lh, int preserve, int wrap, TextSpacing sp,
                           const char *chunk, int chunk_len, float chunk_w)
{
    float space_w = (!preserve && ps->pending_space && ps->x > ps->line_x0 + 0.01f)
                        ? hpe_font_advance(font, ' ') + sp.word + sp.letter
                        : 0.0f;

    if (wrap && ps->x > ps->line_x0 + 0.01f && ps->x + space_w + chunk_w > ps->line_x1 + 0.5f) {
        para_close_line(ps);
        space_w = 0.0f;
        ps->pending_space = 0;
    }
    if (ps->line_idx < 0)
        para_open_line(ps, lh);
    if (space_w > 0.0f) {
        line_add_run(ps->block, ps->line_idx, " ", 1, seg->style, font, ps->x, space_w, asc,
                     desc, lh);
        ps->x += space_w;
    }
    line_add_run(ps->block, ps->line_idx, chunk, chunk_len, seg->style, font, ps->x, chunk_w,
                 asc, desc, lh);
    ps->x += chunk_w;
    ps->pending_space = 0;
}

/**
 * @brief 按 white-space 模式排版一个文本片段 / Lay out one text segment per its `white-space` mode
 *
 * @param[in,out] ps 段落排版状态 / Paragraph layout state
 * @param[in]     seg 待排版片段 / Segment to lay out
 *
 * @note 源文本中的换行在 pre/pre-wrap/pre-line 下断行 / Source newlines break the line under pre, pre-wrap and pre-line
 */
static void para_place_text(ParaState *ps, const HpeSegment *seg)
{
    HpeFont *font = font_for(seg->style);
    float lh = style_line_height(seg->style, font);
    float asc = hpe_font_ascent(font);
    float desc = hpe_font_descent(font);
    HpeWhiteSpace ws = seg->style->white_space;
    TextSpacing sp = text_spacing(ps->ctx, seg->style, ps->inner_w);
    int preserve = (ws == HPE_WS_PRE || ws == HPE_WS_PRE_WRAP);
    int wrap = (ws == HPE_WS_NORMAL || ws == HPE_WS_PRE_WRAP || ws == HPE_WS_PRE_LINE);
    int break_nl = (ws == HPE_WS_PRE || ws == HPE_WS_PRE_WRAP || ws == HPE_WS_PRE_LINE);
    const char *p = seg->text;
    const char *end = seg->text + seg->len;

    while (p < end) {
        uint32_t cp;
        int n = hpe_utf8_next(p, &cp);
        int is_ws = (cp == ' ' || cp == '\t' || cp == '\r' || cp == 0x00A0);
        int is_nl = (cp == '\n');
        const char *chunk;
        float chunk_w = 0.0f;

        if (n <= 0)
            break;

        /* source newlines break the line in pre / pre-wrap / pre-line */
        if (is_nl && break_nl) {
            if (ps->line_idx < 0)
                para_open_line(ps, lh);
            para_close_line(ps);
            ps->x = 0.0f;
            ps->pending_space = 0;
            p += n;
            continue;
        }

        if (is_ws || (is_nl && !break_nl)) {
            para_place_space(ps, seg, font, preserve, lh, asc, desc, sp, cp);
            p += n;
            continue;
        }

        chunk = p;
        p = text_measure_cluster(font, p, end, sp.letter, &chunk_w);
        para_emit_word(ps, seg, font, asc, desc, lh, preserve, wrap, sp, chunk, (int)(p - chunk),
                       chunk_w);
    }
}

/**
 * @brief 把累积的段落排版成行 / Lay the accumulated paragraph out into lines
 *
 * @param[in]     ctx 布局上下文 / Layout context
 * @param[in,out] block 块盒 / Block box
 * @param[in,out] para 段落，排完后清空 / Paragraph, emptied afterwards
 * @param[in,out] cur_y 纵向游标 / Vertical cursor
 * @param[in]     inner_w 内容区宽度 / Inner width
 *
 * @note 段落为空时直接返回 / Returns immediately when the paragraph is empty
 */
static void flush_paragraph(LayoutCtx *ctx, HpeBox *block, HpeParagraph *para, float *cur_y,
                            float inner_w)
{
    ParaState ps;
    int i;

    if (para->count == 0)
        return;

    memset(&ps, 0, sizeof(ps));
    ps.ctx = ctx;
    ps.block = block;
    ps.band = &block->floats;
    ps.inner_w = inner_w;
    ps.row_y = *cur_y;
    ps.line_idx = -1;
    ps.line_x0 = 0.0f;
    ps.line_x1 = inner_w;
    ps.indent = resolve_len(ctx, &block->style, block->style.text_indent, inner_w);
    ps.indent_pending = (ps.indent != 0.0f);

    for (i = 0; i < para->count; i++) {
        HpeSegment *seg = &para->items[i];
        if (seg->atomic_box)
            para_place_atomic(&ps, seg->atomic_box);
        else if (seg->hard_break)
            para_hard_break(&ps, seg->style);
        else
            para_place_text(&ps, seg);
    }

    if (ps.line_idx >= 0)
        finalize_line(ctx, block, ps.line_idx, inner_w, ps.band, &ps.row_y);
    *cur_y = ps.row_y;
    para->count = 0;
}

/* ---------------- block layout ---------------- */

/**
 * @brief 排版一个行内块盒 / Lay out an inline-block box
 *
 * @param[in]     ctx 布局上下文 / Layout context
 * @param[in,out] box 行内块盒 / Inline-block box
 * @param[in]     parent_inner_w 父内容区宽度 / Parent inner width
 *
 * @note 有显式宽度时按该宽度排版，否则收缩适应 / Uses an explicit width when present, otherwise shrink-to-fit
 */
static void layout_inline_block(LayoutCtx *ctx, HpeBox *box, float parent_inner_w)
{
    HpeStyle *st = &box->style;
    float extra = side_px(ctx, st, st->padding[3], parent_inner_w) +
                  side_px(ctx, st, st->padding[1], parent_inner_w) +
                  (st->border_w[3] > 0.0f ? st->border_w[3] : 0.0f) +
                  (st->border_w[1] > 0.0f ? st->border_w[1] : 0.0f);

    if (st->width.unit != HPE_LEN_AUTO) {
        /* `width` is the content box for content-box sizing and the border box
         * for border-box; layout_block applies that interpretation itself. */
        float w = resolve_len(ctx, st, st->width, parent_inner_w);
        if (w < 0.0f)
            w = 0.0f;
        layout_block(ctx, box, 0.0f, 0.0f, w, -1.0f);
        return;
    }

    {
        float pref = measure_preferred_w(ctx, box);
        float mL = margin_px(ctx, st, st->margin[3], parent_inner_w);
        float mR = margin_px(ctx, st, st->margin[1], parent_inner_w);
        /* layout_block subtracts the horizontal margins from `avail`; add them
         * back so the shrink-to-fit content keeps its measured natural width */
        float w = pref + mL + mR;
        float mn = resolve_len(ctx, st, st->min_width, parent_inner_w);
        float mx = resolve_len(ctx, st, st->max_width, parent_inner_w);
        if (w > parent_inner_w)
            w = parent_inner_w;
        if (mx > 0.0f && w > mx)
            w = mx;
        if (mn > 0.0f && w < mn)
            w = mn;
        if (w < 0.0f)
            w = 0.0f;
        (void)extra;
        layout_block(ctx, box, 0.0f, 0.0f, w, -1.0f);
    }
}

/**
 * @brief 测量内在（收缩适应）边框盒宽度 / Measure the natural shrink-to-fit border-box width
 *
 * @param[in]     ctx 布局上下文 / Layout context
 * @param[in,out] box 待测量的盒 / Box to measure
 *
 * @return 内在边框盒宽度 / Intrinsic border-box width
 *
 * @note 以极大可用宽度排版以免折行，再取真实内容范围 / Lays out with a huge available width so text does not wrap, then takes the real content extent
 */
static float measure_preferred_w(LayoutCtx *ctx, HpeBox *box)
{
    layout_block(ctx, box, 0.0f, 0.0f, 1.0e6f, -1.0f);
    /* the block pass clamps to the huge avail; take the real content extent instead */
    {
        float content_w = 0.0f;
        int i;
        for (i = 0; i < box->line_count; i++)
            if (box->lines[i].width > content_w)
                content_w = box->lines[i].width;
        {
            HpeBox *c;
            for (c = box->first_child; c; c = c->next) {
                if (c->kind == HPE_BOX_BLOCK || c->kind == HPE_BOX_INLINE_BLOCK) {
                    float cw = c->border_box.x + c->border_box.w - box->content_box.x;
                    if (cw > content_w)
                        content_w = cw;
                }
            }
        }
        box->content_box.w = content_w;
        box->border_box.w = content_w + side_px(ctx, &box->style, box->style.padding[3], content_w) +
                            side_px(ctx, &box->style, box->style.padding[1], content_w) +
                            box->style.border_w[3] + box->style.border_w[1];
    }
    return box->border_box.w;
}

/**
 * @brief 求最近定位祖先的 padding 盒 / Find the nearest positioned ancestor's padding box
 *
 * @param[in]     ctx 布局上下文 / Layout context
 * @param[in]     box 目标盒 / Target box
 *
 * @return 包含块矩形；无定位祖先时返回视口矩形 / The containing block rect, or the viewport when no positioned ancestor exists
 */
static HpeRect find_containing_block(LayoutCtx *ctx, HpeBox *box)
{
    HpeBox *p = box->parent;
    while (p) {
        if (p->style.position != HPE_POS_STATIC)
            return p->pad_box;
        p = p->parent;
    }
    return hpe_rect(0.0f, 0.0f, ctx->vw, ctx->vh);
}

/* positions an absolutely/fixed positioned box relative to its containing block */
/**
 * @brief 解析一条定位 inset 边，`auto` 返回 0 / Resolve one positioning inset edge, returning 0 for `auto`
 *
 * @param[in]     ctx 布局上下文 / Layout context
 * @param[in]     st 计算样式 / Computed style
 * @param[in]     side 边序号 / Side index
 * @param[in]     ref 解析基准长度 / Reference length
 *
 * @return 该边的像素偏移，`auto` 时为 0 / Pixel offset of the edge, 0 when `auto`
 */
static float abspos_inset(LayoutCtx *ctx, HpeStyle *st, int side, float ref)
{
    if (st->inset[side].unit == HPE_LEN_AUTO)
        return 0.0f;
    return resolve_len(ctx, st, st->inset[side], ref);
}

/**
 * @brief 求绝对定位盒的 used 宽度 / Compute the used width of an absolutely positioned box
 *
 * @param[in]     ctx 布局上下文 / Layout context
 * @param[in,out] box 定位盒 / Positioned box
 * @param[in]     cb 包含块矩形 / Containing block rect
 * @param[in]     mL 左边距 / Left margin
 * @param[in]     mR 右边距 / Right margin
 * @param[in]     hasL 左侧 inset 非 auto 为 1 / 1 when the left inset is not `auto`
 * @param[in]     L 左侧 inset / Left inset
 * @param[in]     hasR 右侧 inset 非 auto 为 1 / 1 when the right inset is not `auto`
 * @param[in]     R 右侧 inset / Right inset
 *
 * @return used 宽度（非负）/ Used width, clamped to non-negative
 *
 * @note 依次取显式宽度、两侧 inset 拉伸、收缩适应 / Prefers an explicit width, then stretching between both insets, then shrink-to-fit
 */
static float abspos_width(LayoutCtx *ctx, HpeBox *box, HpeRect cb, float mL, float mR,
                          int hasL, float L, int hasR, float R)
{
    HpeStyle *st = &box->style;
    float w;
    if (st->width.unit != HPE_LEN_AUTO) {
        w = resolve_len(ctx, st, st->width, cb.w);
    } else if (hasL && hasR) {
        w = cb.w - L - R - mL - mR;
    } else {
        float avail = cb.w;
        if (hasL)
            avail = cb.w - L - mL - mR;
        else if (hasR)
            avail = cb.w - R - mL - mR;
        w = measure_preferred_w(ctx, box);
        if (w > avail)
            w = avail;
    }
    return w < 0.0f ? 0.0f : w;
}

/**
 * @brief 相对包含块放置一个绝对/固定定位盒 / Position an absolutely/fixed positioned box against its containing block
 *
 * @param[in]     ctx 布局上下文 / Layout context
 * @param[in,out] box 定位盒 / Positioned box
 *
 * @note 固定定位以视口为包含块；同时给出上下 inset 且高度 auto 时拉伸到包含块 / Fixed positioning uses the viewport as containing block; when both top and bottom insets are given and the height is `auto` the box stretches to the containing block
 */
static void layout_abspos(LayoutCtx *ctx, HpeBox *box)
{
    HpeStyle *st = &box->style;
    HpeRect cb = (st->position == HPE_POS_FIXED) ? hpe_rect(0.0f, 0.0f, ctx->vw, ctx->vh)
                                                : find_containing_block(ctx, box);
    float mL = margin_px(ctx, st, st->margin[3], cb.w);
    float mR = margin_px(ctx, st, st->margin[1], cb.w);
    float mT = margin_px(ctx, st, st->margin[0], cb.h);
    float mB = margin_px(ctx, st, st->margin[2], cb.h);
    int hasL = (st->inset[3].unit != HPE_LEN_AUTO);
    int hasR = (st->inset[1].unit != HPE_LEN_AUTO);
    int hasT = (st->inset[0].unit != HPE_LEN_AUTO);
    int hasB = (st->inset[2].unit != HPE_LEN_AUTO);
    float L = abspos_inset(ctx, st, 3, cb.w);
    float R = abspos_inset(ctx, st, 1, cb.w);
    float T = abspos_inset(ctx, st, 0, cb.h);
    float B = abspos_inset(ctx, st, 2, cb.h);
    float w = abspos_width(ctx, box, cb, mL, mR, hasL, L, hasR, R);
    float x = hasL ? cb.x + L
                   : (hasR ? cb.x + cb.w - R - w : cb.x + mL);
    float ypass = hasT ? cb.y + T
                       : (hasB ? cb.y : cb.y + mT); /* hasB: fixed up after measuring */
    int forced_h = 0;

    /* `inset: T ... B` with an auto height stretches to the containing block */
    if (hasT && hasB && st->height.unit == HPE_LEN_AUTO) {
        float h = cb.h - T - B - mT - mB - side_px(ctx, st, st->padding[0], cb.w) -
                  side_px(ctx, st, st->padding[2], cb.w) - st->border_w[0] - st->border_w[2];
        st->height = hpe_len_px(h > 0.0f ? h : 0.0f);
        forced_h = 1;
    }

    layout_block(ctx, box, x, ypass, w, cb.h);

    if (hasB && !hasT) {
        float margin_h = mT + box->border_box.h + mB;
        float target_top = cb.y + cb.h - B - margin_h; /* margin-box top */
        box_shift(box, 0.0f, target_top - (box->border_box.y - mT));
    }
    if (forced_h)
        st->height = hpe_len_auto();
}

/* places a floated box into its container's float band: sizes it, dodges
 * existing floats downward (honouring `clear`), then records the band extent. */

/**
 * @brief 向浮动态带追加一段浮动态记录 / Append one float extent to the band
 *
 * @param[in,out] band 浮动态带 / Float band
 * @param[in]     fbox 浮动盒 / Float box
 * @param[in]     top 上边界（文档坐标）/ Top edge in document coordinates
 * @param[in]     bottom 下边界（文档坐标）/ Bottom edge in document coordinates
 * @param[in]     left 左边界（文档坐标）/ Left edge in document coordinates
 * @param[in]     right 右边界（文档坐标）/ Right edge in document coordinates
 * @param[in]     side 浮动侧 / Float side
 */
static void float_band_push(HpeFloatBand *band, HpeBox *fbox, float top, float bottom,
                            float left, float right, int side)
{
    band->items = (HpeFloatBox *)hpe_grow(band->items, &band->cap, (size_t)band->count + 1,
                                          sizeof(HpeFloatBox));
    band->items[band->count].box = fbox;
    band->items[band->count].top = top;
    band->items[band->count].bottom = bottom;
    band->items[band->count].left = left;
    band->items[band->count].right = right;
    band->items[band->count].side = side;
    band->count++;
}

/**
 * @brief 按 clear 把 y 推到既有浮动之下 / Push y below every same-side float already present, per `clear`
 *
 * @param[in]     band 浮动态带 / Float band
 * @param[in]     clear_side 清除侧 / Clear side
 * @param[in]     y 初始 y / Initial y
 *
 * @return 清除后的 y / Adjusted y
 */
static float float_clear_offset(HpeFloatBand *band, int clear_side, float y)
{
    int i;
    for (i = 0; i < band->count; i++) {
        HpeFloatBox *f = &band->items[i];
        int hit = (clear_side == HPE_CLEAR_BOTH) ||
                  (clear_side == HPE_CLEAR_LEFT && f->side == HPE_FLOAT_LEFT) ||
                  (clear_side == HPE_CLEAR_RIGHT && f->side == HPE_FLOAT_RIGHT);
        if (hit && f->bottom > y)
            y = f->bottom;
    }
    return y;
}

/**
 * @brief 求与区间重叠浮动的最低底边 / Lowest bottom of the floats overlapping a vertical span
 *
 * @param[in]     band 浮动态带 / Float band
 * @param[in]     y 区间上边界 / Span top
 * @param[in]     h 区间高度 / Span height
 *
 * @return 重叠浮动的最低底边；无重叠时为 -1 / Lowest overlapping bottom, or -1 when none overlap
 */
static float float_dodge_below(HpeFloatBand *band, float y, float h)
{
    float next = -1.0f;
    int i;
    for (i = 0; i < band->count; i++) {
        HpeFloatBox *f = &band->items[i];
        if (f->bottom > y && f->top < y + h && f->bottom > next)
            next = f->bottom;
    }
    return next;
}

/**
 * @brief 把浮动盒放入容器的浮动态带 / Place a floated box into its container's float band
 *
 * @param[in]     ctx 布局上下文 / Layout context
 * @param[in,out] container 容器盒 / Container box
 * @param[in,out] fbox 浮动盒 / Float box
 * @param[in,out] cur_y 容器纵向游标 / Container vertical cursor
 *
 * @note 先定尺寸并遵守 `clear`，必要时下移到既有浮动之下，再记录浮动范围 / Sizes the box honouring `clear`, dodges downward past existing floats when it does not fit, then records the band extent
 */
static void place_float(LayoutCtx *ctx, HpeBox *container, HpeBox *fbox, float *cur_y)
{
    HpeStyle *st = &fbox->style;
    HpeFloatBand *band = &container->floats;
    float inner_w = container->content_box.w;
    float mL = margin_px(ctx, st, st->margin[3], inner_w);
    float mR = margin_px(ctx, st, st->margin[1], inner_w);
    float mT = margin_px(ctx, st, st->margin[0], inner_w);
    float mB = margin_px(ctx, st, st->margin[2], inner_w);
    float y = float_clear_offset(band, st->clear_side, *cur_y);
    float w, h;

    layout_inline_block(ctx, fbox, inner_w); /* shrink-to-fit or explicit width */
    w = fbox->border_box.w + mL + mR;
    h = fbox->border_box.h + mT + mB;
    if (w > inner_w)
        w = inner_w; /* a float never exceeds its container */

    for (;;) {
        float L, R, x, next;
        band_edges_inherited(container, y, y + h, &L, &R);
        if (R - L >= w - 0.5f || band->count == 0) {
            x = (st->float_side == HPE_FLOAT_RIGHT) ? R - w : L;
            if (x < container->content_box.x)
                x = container->content_box.x;
            float_band_push(band, fbox, y, y + h, x, x + w, st->float_side);
            box_shift(fbox, x + mL - fbox->border_box.x, y + mT - fbox->border_box.y);
            return;
        }
        /* does not fit beside the current floats: retry below the lowest one */
        next = float_dodge_below(band, y, h);
        if (next < 0.0f) {
            /* floats ended above: nothing more to dodge (container-wide box) */
            float_band_push(band, fbox, y, y + h, L, L + w, st->float_side);
            box_shift(fbox, L + mL - fbox->border_box.x, y + mT - fbox->border_box.y);
            return;
        }
        y = next;
    }
}


/* ---------------- flex containers ----------------
 *
 * Single-line (nowrap) flex layout, which is what the reference pages use.
 * Implemented: row and column main axes, shrink-to-fit item sizing, `gap`,
 * `align-items` / `align-self`, `justify-content`, auto margins (the
 * `margin-left:auto` push idiom) and `flex-grow` distribution of positive free
 * space. `stretch` degrades to `flex-start` because item heights are driven by
 * their content, and text children are skipped: anonymous flex items are not
 * modelled by the box tree. */

typedef struct {
    HpeBox *box;
    float m[4];    /* resolved margins (auto sides are distributed later) */
    float w;       /* target border-box size on the main axis */
    float outer_h; /* margin-box height, known once the item is laid out */
    float grow;
    int auto_ml, auto_mr;
} FlexItem;

/**
 * @brief 求单个 flex item 的交叉轴偏移 / Cross-axis offset for one flex item
 *
 * @param[in]     align 对齐方式 / Alignment
 * @param[in]     cross 交叉轴尺寸 / Cross-axis size
 * @param[in]     outer item 的外尺寸 / Item outer size
 *
 * @return 交叉轴偏移 / Cross-axis offset
 *
 * @note `stretch` 与不支持的 `baseline` 退化为 `flex-start` / `stretch` and the unsupported `baseline` degrade to `flex-start`
 */
static float flex_cross_offset(int align, float cross, float outer)
{
    if (align == HPE_ALIGN_ITEMS_CENTER)
        return (cross - outer) * 0.5f;
    if (align == HPE_ALIGN_ITEMS_FLEX_END)
        return cross - outer;
    return 0.0f; /* flex-start, stretch and the unsupported baseline */
}

/**
 * @brief 分配 flex 行主轴上的正剩余空间 / Distribute positive free space on a flex row's main axis
 *
 * @param[in]     st 容器样式 / Container style
 * @param[in,out] items flex item 数组 / Flex item array
 * @param[in]     n item 个数 / Number of items
 * @param[in]     free_space 剩余空间 / Free space
 * @param[in]     grow_sum flex-grow 之和 / Sum of flex-grow factors
 * @param[in]     autos auto 外边距个数 / Number of auto margins
 * @param[out]    out_lead 主轴前导偏移 / Leading offset on the main axis
 * @param[out]    out_extra item 间额外间距 / Extra spacing between items
 *
 * @note 顺序为 auto 外边距、flex-grow、justify-content / The order is auto margins, then flex-grow, then `justify-content`
 */
static void flex_row_free_space(HpeStyle *st, FlexItem *items, int n, float free_space,
                                float grow_sum, int autos, float *out_lead, float *out_extra)
{
    int i;
    *out_lead = 0.0f;
    *out_extra = 0.0f;
    if (free_space <= 0.0f)
        return;
    if (autos > 0) {
        /* auto margins soak up the free space before justify-content runs */
        float share = free_space / (float)autos;
        for (i = 0; i < n; i++) {
            /* the resolved value has to reach the item's own layout pass,
             * which resolves `auto` margins to zero on its own */
            if (items[i].auto_ml) {
                items[i].m[3] = share;
                items[i].box->style.margin[3] = hpe_len_px(share);
            }
            if (items[i].auto_mr) {
                items[i].m[1] = share;
                items[i].box->style.margin[1] = hpe_len_px(share);
            }
        }
        return;
    }
    if (grow_sum > 0.0f) {
        for (i = 0; i < n; i++) {
            if (items[i].grow > 0.0f)
                items[i].w += free_space * items[i].grow / grow_sum;
        }
        return;
    }
    if (st->justify == HPE_JUSTIFY_CENTER)
        *out_lead = free_space * 0.5f;
    else if (st->justify == HPE_JUSTIFY_END)
        *out_lead = free_space;
    else if (st->justify == HPE_JUSTIFY_SPACE_BETWEEN && n > 1)
        *out_extra = free_space / (float)(n - 1);
    else if (st->justify == HPE_JUSTIFY_SPACE_AROUND && n > 0) {
        *out_lead = free_space / (float)(n * 2);
        *out_extra = free_space / (float)n;
    }
}

/**
 * @brief 在交叉轴上对齐各行 flex item / Align each flex item on the cross axis
 *
 * @param[in]     st 容器样式 / Container style
 * @param[in,out] items flex item 数组 / Flex item array
 * @param[in]     n item 个数 / Number of items
 * @param[in]     cross 交叉轴尺寸 / Cross-axis size
 *
 * @return 交叉轴尺寸 / The cross-axis size
 *
 * @note 每个 item 的 `align-self` 优先于容器的 `align-items` / Each item's `align-self` takes precedence over the container's `align-items`
 */
static float flex_row_align(const HpeStyle *st, FlexItem *items, int n, float cross)
{
    int i;
    for (i = 0; i < n; i++) {
        FlexItem *it = &items[i];
        int align = it->box->style.align_self != HPE_ALIGN_ITEMS_AUTO
                        ? it->box->style.align_self
                        : st->align_items;
        float dy = flex_cross_offset(align, cross, it->outer_h);
        if (dy != 0.0f)
            box_shift(it->box, 0.0f, dy);
    }
    return cross;
}

/**
 * @brief 排版一条单行（nowrap）flex 行 / Lay out a single-line (nowrap) flex row
 *
 * @param[in]     ctx 布局上下文 / Layout context
 * @param[in,out] box flex 容器盒 / Flex container box
 * @param[in,out] items flex item 数组 / Flex item array
 * @param[in]     n item 个数 / Number of items
 * @param[in]     inner_w 内容区宽度 / Inner width
 * @param[in]     inner_h 内容区高度基准 / Inner height basis
 * @param[in]     own_h 容器自身高度，未定为负 / Container's own height, negative when unset
 *
 * @return 容器的交叉轴（高度）尺寸 / The container's cross-axis (height) size
 */
static float layout_flex_row(LayoutCtx *ctx, HpeBox *box, FlexItem *items, int n, float inner_w,
                             float inner_h, float own_h)
{
    HpeStyle *st = &box->style;
    float total = 0.0f, free_space, line_h = 0.0f, cross, cursor, lead, extra;
    float grow_sum = 0.0f;
    int i, autos = 0;

    for (i = 0; i < n; i++) {
        total += items[i].w + items[i].m[1] + items[i].m[3];
        autos += items[i].auto_ml + items[i].auto_mr;
        grow_sum += items[i].grow;
    }
    if (n > 1)
        total += st->gap * (float)(n - 1);
    free_space = inner_w - total;

    flex_row_free_space(st, items, n, free_space, grow_sum, autos, &lead, &extra);

    /* pass 1: lay the items out along the main axis to collect cross sizes */
    cursor = box->content_box.x + lead;
    for (i = 0; i < n; i++) {
        FlexItem *it = &items[i];
        float avail = it->w + it->m[1] + it->m[3];
        /* layout_block() takes the margin box origin and adds the margins itself */
        layout_block(ctx, it->box, cursor, box->content_box.y, avail, inner_h);
        it->outer_h = it->m[0] + it->box->border_box.h + it->m[2];
        if (it->outer_h > line_h)
            line_h = it->outer_h;
        cursor += it->w + it->m[1] + it->m[3] + st->gap + extra;
    }

    cross = own_h > line_h ? own_h : line_h;
    return flex_row_align(st, items, n, cross);
}

/**
 * @brief 排版一条单行 flex 列 / Lay out a single-line flex column
 *
 * @param[in]     ctx 布局上下文 / Layout context
 * @param[in,out] box flex 容器盒 / Flex container box
 * @param[in,out] items flex item 数组 / Flex item array
 * @param[in]     n item 个数 / Number of items
 * @param[in]     inner_w 内容区宽度 / Inner width
 * @param[in]     inner_h 内容区高度基准 / Inner height basis
 * @param[in]     own_h 容器自身高度，未定为负 / Container's own height, negative when unset
 *
 * @return 容器的交叉轴（高度）尺寸 / The container's cross-axis (height) size
 *
 * @note 列的主轴为纵向，交叉轴水平 / The column's main axis is vertical and its cross axis horizontal
 */
static float layout_flex_column(LayoutCtx *ctx, HpeBox *box, FlexItem *items, int n, float inner_w,
                                float inner_h, float own_h)
{
    HpeStyle *st = &box->style;
    float cursor = box->content_box.y;
    int i;

    for (i = 0; i < n; i++) {
        FlexItem *it = &items[i];
        float avail = it->w + it->m[1] + it->m[3];
        int align = it->box->style.align_self != HPE_ALIGN_ITEMS_AUTO
                        ? it->box->style.align_self
                        : st->align_items;
        /* the cross axis is horizontal for a column: offset the margin box */
        float dx = flex_cross_offset(align, inner_w, avail);
        layout_block(ctx, it->box, box->content_box.x + dx, cursor, avail, inner_h);
        it->outer_h = it->m[0] + it->box->border_box.h + it->m[2];
        cursor += it->outer_h + st->gap;
    }
    return own_h > cursor - box->content_box.y ? own_h : cursor - box->content_box.y;
}

/**
 * @brief 求一个 flex item 的主轴（边框盒）尺寸 / Resolve one flex item's main-axis (border-box) size
 *
 * @param[in]     ctx 布局上下文 / Layout context
 * @param[in,out] child item 对应的子盒 / Child box of the item
 * @param[in]     inner_w 容器内容区宽度 / Container inner width
 *
 * @return 主轴尺寸（非负，夹紧到 min/max 与容器宽度）/ Main-axis size, non-negative, clamped to min/max and the container width
 *
 * @note 显式宽度按 box-sizing 加内边距/边框，否则收缩适应 / An explicit width adds padding/border for content-box sizing, otherwise shrink-to-fit
 */
static float flex_item_width(LayoutCtx *ctx, HpeBox *child, float inner_w)
{
    HpeStyle *cs = &child->style;
    float w;
    if (cs->width.unit != HPE_LEN_AUTO) {
        float extra = side_px(ctx, cs, cs->padding[3], inner_w) +
                      side_px(ctx, cs, cs->padding[1], inner_w) +
                      (cs->border_w[3] > 0.0f ? cs->border_w[3] : 0.0f) +
                      (cs->border_w[1] > 0.0f ? cs->border_w[1] : 0.0f);
        w = resolve_len(ctx, cs, cs->width, inner_w);
        if (cs->box_sizing != HPE_BOX_SIZING_BORDER)
            w += extra;
    } else {
        w = measure_preferred_w(ctx, child);
    }
    if (cs->min_width.unit != HPE_LEN_AUTO) {
        float mw = resolve_len(ctx, cs, cs->min_width, inner_w);
        if (mw > w)
            w = mw;
    }
    if (cs->max_width.unit != HPE_LEN_AUTO) {
        float mw = resolve_len(ctx, cs, cs->max_width, inner_w);
        if (mw > 0.0f && w > mw)
            w = mw;
    }
    if (w > inner_w)
        w = inner_w;
    return w < 0.0f ? 0.0f : w;
}

/**
 * @brief 收集流内块级/行内块子盒为 flex items / Collect the in-flow block and inline-block children as flex items
 *
 * @param[in]     ctx 布局上下文 / Layout context
 * @param[in]     box flex 容器盒 / Flex container box
 * @param[in]     inner_w 内容区宽度 / Inner width
 * @param[out]    out_n 收集到的 item 个数 / Number of collected items
 *
 * @return 新分配的 item 数组，调用方负责释放 / Newly allocated item array owned by the caller
 *
 * @note 跳过流外（绝对/固定）与行内/文本子盒 / Out-of-flow boxes and inline/text children are skipped
 */
static FlexItem *flex_collect_items(LayoutCtx *ctx, HpeBox *box, float inner_w, int *out_n)
{
    FlexItem *items = NULL;
    size_t cap = 0;
    int n = 0;
    HpeBox *child;

    for (child = box->first_child; child; child = child->next) {
        HpeStyle *cs = &child->style;
        FlexItem *it;
        if (cs->position == HPE_POS_ABSOLUTE || cs->position == HPE_POS_FIXED)
            continue; /* out of flow: positioned against the containing block later */
        if (child->kind != HPE_BOX_BLOCK && child->kind != HPE_BOX_INLINE_BLOCK)
            continue; /* inline/text children would need anonymous flex items */
        items = (FlexItem *)hpe_grow(items, &cap, (size_t)n + 1, sizeof(FlexItem));
        it = &items[n++];
        memset(it, 0, sizeof(*it));
        it->box = child;
        it->auto_ml = cs->margin[3].unit == HPE_LEN_AUTO;
        it->auto_mr = cs->margin[1].unit == HPE_LEN_AUTO;
        it->m[0] = margin_px(ctx, cs, cs->margin[0], inner_w);
        it->m[1] = margin_px(ctx, cs, cs->margin[1], inner_w);
        it->m[2] = margin_px(ctx, cs, cs->margin[2], inner_w);
        it->m[3] = margin_px(ctx, cs, cs->margin[3], inner_w);
        it->grow = cs->flex_grow > 0.0f ? cs->flex_grow : 0.0f;
        it->w = flex_item_width(ctx, child, inner_w);
    }
    *out_n = n;
    return items;
}

/**
 * @brief 排版一个 flex 容器 / Lay out a flex container
 *
 * @param[in]     ctx 布局上下文 / Layout context
 * @param[in,out] box flex 容器盒 / Flex container box
 * @param[in]     inner_w 内容区宽度 / Inner width
 * @param[in]     inner_h 内容区高度基准 / Inner height basis
 *
 * @return 容器的高度 / The container's height
 *
 * @note 无 flex item 时仅返回自身高度；主轴方向取自 `flex-direction` / Returns just the own height when there are no items; the main axis comes from `flex-direction`
 */
static float layout_flex(LayoutCtx *ctx, HpeBox *box, float inner_w, float inner_h)
{
    HpeStyle *st = &box->style;
    FlexItem *items;
    int n = 0;
    float own_h = -1.0f, result;

    if (st->height.unit != HPE_LEN_AUTO) {
        float h = resolve_len(ctx, st, st->height, inner_w);
        if (st->height.unit == HPE_LEN_PCT && inner_h > 0.0f)
            h = resolve_len(ctx, st, st->height, inner_h);
        if (h >= 0.0f)
            own_h = h;
    }

    items = flex_collect_items(ctx, box, inner_w, &n);
    if (n == 0) {
        if (items)
            hpe_free(items);
        return own_h > 0.0f ? own_h : 0.0f;
    }
    if (st->flex_dir == HPE_FLEX_COLUMN)
        result = layout_flex_column(ctx, box, items, n, inner_w, inner_h, own_h);
    else
        result = layout_flex_row(ctx, box, items, n, inner_w, inner_h, own_h);
    if (items)
        hpe_free(items);
    return result;
}

/**
 * @brief 求解块盒的内容宽度 / Solve a block box's content width
 *
 * @param[in]     ctx 布局上下文 / Layout context
 * @param[in]     st 计算样式 / Computed style
 * @param[in]     cb_width 包含块宽度 / Containing block width
 * @param[in]     avail_width 去掉水平外边距后的可用宽度 / Available width left after the horizontal margins
 * @param[in]     p 四边内边距 / Four padding sides
 * @param[in]     b 四边边框宽度 / Four border widths
 *
 * @return 内容宽度（非负）/ Content width, clamped to non-negative
 *
 * @note `box-sizing:border-box` 时显式宽度按边框盒解释，需减去内边距与边框 / With `box-sizing:border-box` an explicit width refers to the border box, so padding and borders are subtracted
 */
static float solve_content_width(const LayoutCtx *ctx, HpeStyle *st, float cb_width,
                                 float avail_width, const float p[4], const float b[4])
{
    float border_box_extra = p[3] + p[1] + b[3] + b[1];
    float inner_w;
    float min_w, max_w;

    if (st->width.unit == HPE_LEN_AUTO) {
        inner_w = avail_width - border_box_extra;
    } else {
        inner_w = resolve_len(ctx, st, st->width, cb_width);
        if (st->box_sizing == HPE_BOX_SIZING_BORDER)
            inner_w -= border_box_extra;
    }

    /* `min-width`/`max-width` describe the content box for content-box sizing
     * and the border box for border-box sizing. */
    min_w = resolve_len(ctx, st, st->min_width, cb_width);
    max_w = resolve_len(ctx, st, st->max_width, cb_width);
    if (st->box_sizing == HPE_BOX_SIZING_BORDER) {
        if (min_w > 0.0f)
            min_w -= border_box_extra;
        if (max_w > 0.0f)
            max_w -= border_box_extra;
    }
    if (min_w > 0.0f && inner_w < min_w)
        inner_w = min_w;
    if (max_w > 0.0f && inner_w > max_w)
        inner_w = max_w;
    if (inner_w < 0.0f)
        inner_w = 0.0f;
    return inner_w;
}

/**
 * @brief 清空盒的行列表与浮动态带 / Reset a box's line list and float band
 *
 * @param[in,out] box 块盒 / Block box
 *
 * @note 盒可能被多次排版（收缩适应测量、flex 定尺寸），每次都要从干净状态开始 / A box can be laid out more than once (shrink-to-fit measurement, flex sizing), so it starts from a clean state every time
 */
static void block_reset_lines(HpeBox *box)
{
    int li;
    for (li = 0; li < box->line_count; li++) {
        if (box->lines[li].runs)
            hpe_free(box->lines[li].runs);
    }
    box->line_count = 0;
    box->floats.count = 0;
}

/**
 * @brief 解析外边距/内边距/边框，并报告上下边缘是否无边框内边距 / Resolve margins, paddings and borders; report whether the top/bottom edges have neither border nor padding
 *
 * @param[in]     ctx 布局上下文 / Layout context
 * @param[in]     st 计算样式 / Computed style
 * @param[in]     avail_width 可用宽度 / Available width
 * @param[out]    m 四边外边距（可负）/ Four margin sides, may be negative
 * @param[out]    p 四边内边距 / Four padding sides
 * @param[out]    b 四边边框宽度 / Four border widths
 * @param[out]    no_top_sep 上边缘无边框内边距为 1 / 1 when the top edge has no border or padding
 * @param[out]    no_bottom_sep 下边缘无边框内边距为 1 / 1 when the bottom edge has no border or padding
 *
 * @note 无分隔的相邻边缘允许子外边距折叠穿透 / Edges without a separator let child margins collapse through
 */
static void block_resolve_edges(LayoutCtx *ctx, HpeStyle *st, float avail_width, float m[4],
                                float p[4], float b[4], int *no_top_sep, int *no_bottom_sep)
{
    /* margins may be negative: an overhang pulls the box out of its container */
    m[0] = margin_px(ctx, st, st->margin[0], avail_width);
    m[1] = margin_px(ctx, st, st->margin[1], avail_width);
    m[2] = margin_px(ctx, st, st->margin[2], avail_width);
    m[3] = margin_px(ctx, st, st->margin[3], avail_width);
    p[0] = side_px(ctx, st, st->padding[0], avail_width);
    p[1] = side_px(ctx, st, st->padding[1], avail_width);
    p[2] = side_px(ctx, st, st->padding[2], avail_width);
    p[3] = side_px(ctx, st, st->padding[3], avail_width);
    b[0] = st->border_w[0] > 0.0f ? st->border_w[0] : 0.0f;
    b[1] = st->border_w[1] > 0.0f ? st->border_w[1] : 0.0f;
    b[2] = st->border_w[2] > 0.0f ? st->border_w[2] : 0.0f;
    b[3] = st->border_w[3] > 0.0f ? st->border_w[3] : 0.0f;
    *no_top_sep = (b[0] <= 0.0f && p[0] <= 0.0f);
    *no_bottom_sep = (b[2] <= 0.0f && p[2] <= 0.0f);
}

/**
 * @brief 分配水平 auto 外边距以居中定宽盒 / Distribute horizontal auto margins to centre a fixed-width box
 *
 * @param[in]     ctx 布局上下文 / Layout context
 * @param[in]     st 计算样式 / Computed style
 * @param[in]     avail_width 可用宽度 / Available width
 * @param[in,out] m 四边外边距，auto 侧被写回 / Four margin sides; the auto sides are written back
 * @param[in]     p 四边内边距 / Four padding sides
 * @param[in]     b 四边边框宽度 / Four border widths
 *
 * @note 宽度为 `auto` 或无 auto 侧时不做处理 / Does nothing when the width is `auto` or neither side is auto
 */
static void block_auto_margins(LayoutCtx *ctx, HpeStyle *st, float avail_width, float m[4],
                               const float p[4], const float b[4])
{
    int auto_left, auto_right;
    float outer, free_space;
    if (st->width.unit == HPE_LEN_AUTO)
        return;
    auto_left = st->margin[3].unit == HPE_LEN_AUTO;
    auto_right = st->margin[1].unit == HPE_LEN_AUTO;
    if (!auto_left && !auto_right)
        return;
    outer = resolve_len(ctx, st, st->width, avail_width) + p[3] + p[1] + b[3] + b[1];
    free_space = avail_width - outer;
    if (free_space <= 0.0f)
        return;
    if (auto_left && auto_right) {
        m[3] = free_space * 0.5f;
        m[1] = free_space * 0.5f;
    } else if (auto_left) {
        m[3] = free_space;
    } else {
        m[1] = free_space;
    }
}

/* running block-flow state, carrying the vertical-margin-collapse variables */
typedef struct {
    float cur_y;
    float prev_bottom;
    int first;
    int had_block;
} BlockFlow;

/**
 * @brief 放置并排版一个流内块级子盒 / Position and lay out one in-flow block child
 *
 * @param[in]     ctx 布局上下文 / Layout context
 * @param[in,out] box 父块盒 / Parent block box
 * @param[in,out] child 子盒 / Child box
 * @param[in]     inner_w 父内容区宽度 / Parent inner width
 * @param[in]     inner_h_base 内容区高度基准 / Inner height basis
 * @param[in]     m 父的四边外边距 / Parent's four margin sides
 * @param[in]     b 父的四边边框宽度 / Parent's four border widths
 * @param[in]     no_top_sep 父上边缘无分隔为 1 / 1 when the parent's top edge has no separator
 * @param[in,out] fl 块级流状态 / Block flow state
 *
 * @note 首个无上分隔的子盒与父的上外边距折叠 / The first child on a parent without a top separator collapses its top margin with the parent's
 */
static void block_flow_block_child(LayoutCtx *ctx, HpeBox *box, HpeBox *child, float inner_w,
                                   float inner_h_base, const float m[4], const float b[4],
                                   int no_top_sep, BlockFlow *fl)
{
    float cm[4];
    float ypass;
    resolve_margins(ctx, &child->style, inner_w, cm);
    if (fl->first) {
        if (no_top_sep)
            ypass = box->content_box.y - b[0] + collapse_margins(m[0], cm[0]) - cm[0];
        else
            ypass = fl->cur_y;
    } else {
        float prev_border_bottom = fl->cur_y - fl->prev_bottom;
        float gap = collapse_margins(fl->prev_bottom, cm[0]);
        ypass = prev_border_bottom + gap - cm[0];
    }
    fl->cur_y = ypass;
    fl->cur_y += layout_block(ctx, child, box->content_box.x, fl->cur_y, inner_w, inner_h_base);
    fl->prev_bottom = cm[2];
    fl->had_block = 1;
    fl->first = 0;
}

/**
 * @brief 运行流内子项（块流或 flex）并返回内容底边 / Run the in-flow children (block flow or flex), returning the content bottom
 *
 * @param[in]     ctx 布局上下文 / Layout context
 * @param[in,out] box 块盒 / Block box
 * @param[in]     inner_w 内容区宽度 / Inner width
 * @param[in]     inner_h_base 内容区高度基准 / Inner height basis
 * @param[in]     m 本盒的四边外边距 / This box's four margin sides
 * @param[in]     b 本盒的四边边框宽度 / This box's four border widths
 * @param[in]     no_top_sep 上边缘无分隔为 1 / 1 when the top edge has no separator
 * @param[out]    out_had_block 是否出现过块级子项 / Whether a block child was seen
 * @param[out]    out_prev_bottom 最后一个子项的下外边距 / The last child's bottom margin
 *
 * @return 内容底边的文档 y / Content bottom in document coordinates
 *
 * @note flex 容器走 flex 分支；浮动子盒离开常规流 / A flex container takes the flex branch; floated children leave the flow
 */
static float block_flow_children(LayoutCtx *ctx, HpeBox *box, float inner_w, float inner_h_base,
                                 const float m[4], const float b[4], int no_top_sep,
                                 int *out_had_block, float *out_prev_bottom)
{
    HpeStyle *st = &box->style;
    HpeParagraph para;
    HpeBox *child;
    BlockFlow fl;

    if (st->display == HPE_DISPLAY_FLEX) {
        *out_had_block = 0;
        *out_prev_bottom = 0.0f;
        return box->content_box.y + layout_flex(ctx, box, inner_w, inner_h_base);
    }

    fl.cur_y = box->content_box.y;
    fl.prev_bottom = 0.0f;
    fl.first = 1;
    fl.had_block = 0;
    memset(&para, 0, sizeof(para));
    for (child = box->first_child; child; child = child->next) {
        if (child->style.float_side != HPE_FLOAT_NONE) {
            /* floats leave the flow: text laid out before them stays before */
            if (child->style.position != HPE_POS_ABSOLUTE &&
                child->style.position != HPE_POS_FIXED) {
                flush_paragraph(ctx, box, &para, &fl.cur_y, inner_w);
                place_float(ctx, box, child, &fl.cur_y);
            }
            continue;
        }
        if (child->kind == HPE_BOX_BLOCK) {
            /* out-of-flow boxes are positioned later, against their containing block */
            if (child->style.position == HPE_POS_ABSOLUTE ||
                child->style.position == HPE_POS_FIXED)
                continue;
            flush_paragraph(ctx, box, &para, &fl.cur_y, inner_w);
            block_flow_block_child(ctx, box, child, inner_w, inner_h_base, m, b, no_top_sep, &fl);
        } else {
            collect_inline(ctx, child, inner_w, &para);
        }
    }
    flush_paragraph(ctx, box, &para, &fl.cur_y, inner_w);
    if (para.items)
        hpe_free(para.items);
    *out_had_block = fl.had_block;
    *out_prev_bottom = fl.prev_bottom;
    return fl.cur_y;
}

/**
 * @brief 求块盒的内容高度 / Resolve a block box's content height
 *
 * @param[in]     ctx 布局上下文 / Layout context
 * @param[in]     st 计算样式 / Computed style
 * @param[in]     content_h 内容范围高度 / Content extent height
 * @param[in]     avail_width 可用宽度 / Available width
 * @param[in]     avail_height 可用高度（百分比高度基准）/ Available height, the basis for percentage heights
 * @param[in]     p 四边内边距 / Four padding sides
 * @param[in]     b 四边边框宽度 / Four border widths
 *
 * @return 内容高度（非负）/ Content height, clamped to non-negative
 *
 * @note 显式高度优先于内容范围，再受 min/max 夹紧 / An explicit height wins over the content extent and is then clamped by min/max
 */
static float block_resolve_height(LayoutCtx *ctx, HpeStyle *st, float content_h,
                                  float avail_width, float avail_height, const float p[4],
                                  const float b[4])
{
    float inner_h = content_h;
    float explicit_h = -1.0f;
    float border_box_v = p[0] + p[2] + b[0] + b[2];
    float mn, mx;

    if (st->height.unit == HPE_LEN_PX || st->height.unit == HPE_LEN_EM ||
        st->height.unit == HPE_LEN_REM || st->height.unit == HPE_LEN_VH ||
        st->height.unit == HPE_LEN_VW || st->height.unit == HPE_LEN_CH) {
        explicit_h = resolve_len(ctx, st, st->height, avail_width);
    } else if (st->height.unit == HPE_LEN_PCT && avail_height > 0.0f) {
        explicit_h = resolve_len(ctx, st, st->height, avail_height);
    }
    if (explicit_h >= 0.0f) {
        if (st->box_sizing == HPE_BOX_SIZING_BORDER)
            explicit_h -= border_box_v;
        inner_h = explicit_h;
    }
    /* min/max height always describe the content box here */
    mn = resolve_len(ctx, st, st->min_height, avail_height);
    mx = resolve_len(ctx, st, st->max_height, avail_height);
    if (st->box_sizing == HPE_BOX_SIZING_BORDER) {
        if (mn > 0.0f)
            mn -= border_box_v;
        if (mx > 0.0f)
            mx -= border_box_v;
    }
    if (mn > 0.0f && inner_h < mn)
        inner_h = mn;
    if (mx > 0.0f && inner_h > mx)
        inner_h = mx;
    return inner_h < 0.0f ? 0.0f : inner_h;
}

/**
 * @brief 应用 relative 定位偏移 / Apply the `relative` positioning offset
 *
 * @param[in]     ctx 布局上下文 / Layout context
 * @param[in,out] box 块盒 / Block box
 * @param[in]     avail_width 解析基准宽度 / Reference width for resolution
 *
 * @note 只处理 `position:relative`；须在 abspos 流程之前执行，使后代的包含块使用已平移的 padding 盒 / Only `position:relative` is handled; must run before the abspos pass so descendant containing blocks use the shifted padding box
 */
static void block_apply_relative_offset(LayoutCtx *ctx, HpeBox *box, float avail_width)
{
    HpeStyle *st = &box->style;
    float ox = 0.0f, oy = 0.0f;
    if (st->position != HPE_POS_RELATIVE)
        return;
    if (st->inset[3].unit != HPE_LEN_AUTO)
        ox = resolve_len(ctx, st, st->inset[3], avail_width);
    else if (st->inset[1].unit != HPE_LEN_AUTO)
        ox = -resolve_len(ctx, st, st->inset[1], avail_width);
    if (st->inset[0].unit != HPE_LEN_AUTO)
        oy = resolve_len(ctx, st, st->inset[0], avail_width);
    else if (st->inset[2].unit != HPE_LEN_AUTO)
        oy = -resolve_len(ctx, st, st->inset[2], avail_width);
    if (ox != 0.0f || oy != 0.0f)
        box_shift(box, ox, oy);
}

/**
 * @brief 放置流外的绝对/固定定位子盒 / Position the out-of-flow absolutely/fixed positioned children
 *
 * @param[in]     ctx 布局上下文 / Layout context
 * @param[in,out] box 块盒 / Block box
 *
 * @note 流外后代相对本盒（已平移的）padding 盒定位 / Out-of-flow descendants position against this box's (possibly shifted) padding box
 */
static void block_position_abspos_children(LayoutCtx *ctx, HpeBox *box)
{
    HpeBox *child;
    for (child = box->first_child; child; child = child->next) {
        if (child->kind == HPE_BOX_BLOCK &&
            (child->style.position == HPE_POS_ABSOLUTE ||
             child->style.position == HPE_POS_FIXED))
            layout_abspos(ctx, child);
    }
}

/**
 * @brief 求内容盒高度：先覆盖浮动，再应用显式/min/max / Content-box height: content extent covering floats, then explicit/min/max
 *
 * @param[in]     ctx 布局上下文 / Layout context
 * @param[in,out] box 块盒 / Block box
 * @param[in]     content_h 内容范围高度 / Content extent height
 * @param[in]     cur_y 流内游标 / In-flow cursor
 * @param[in]     prev_bottom 最后一个子项的下外边距 / The last child's bottom margin
 * @param[in]     had_block 曾出现块级子项为 1 / 1 when a block child was seen
 * @param[in]     no_bottom_sep 下边缘无分隔为 1 / 1 when the bottom edge has no separator
 * @param[in]     avail_width 可用宽度 / Available width
 * @param[in]     avail_height 可用高度 / Available height
 * @param[in]     m 本盒的四边外边距 / This box's four margin sides
 * @param[in]     p 四边内边距 / Four padding sides
 * @param[in]     b 四边边框宽度 / Four border widths
 *
 * @return 内容盒高度 / Content-box height
 *
 * @note 块容器延伸以覆盖其浮动子项 / A block container extends to cover its floated children
 */
static float block_content_height(LayoutCtx *ctx, HpeBox *box, float content_h, float cur_y,
                                  float prev_bottom, int had_block, int no_bottom_sep,
                                  float avail_width, float avail_height, const float m[4],
                                  const float p[4], const float b[4])
{
    int fi;
    /* a block container extends to cover its floated children */
    for (fi = 0; fi < box->floats.count; fi++) {
        float fb = box->floats.items[fi].bottom - box->content_box.y;
        if (fb > content_h)
            content_h = fb;
    }
    if (had_block && no_bottom_sep)
        content_h = (cur_y - prev_bottom - box->content_box.y) + collapse_margins(prev_bottom, m[2]);
    return block_resolve_height(ctx, &box->style, content_h, avail_width, avail_height, p, b);
}

/**
 * @brief 排版一个块盒 / Lay out a block box
 *
 * @param[in]     ctx 布局上下文 / Layout context
 * @param[in,out] box 待排版的块盒 / Block box to lay out
 * @param[in]     x 边框盒左上角文档坐标 X / Document X of the margin-box origin
 * @param[in]     y 边框盒左上角文档坐标 Y / Document Y of the margin-box origin
 * @param[in]     avail_width 可用宽度（margin box）/ Available width, the margin box
 * @param[in]     avail_height 可用高度 / Available height
 *
 * @return 本盒的外侧高度（上外边距 + 边框盒高 + 下外边距）/ The box's outer height (top margin plus border-box height plus bottom margin)
 *
 * @note 负责解析尺寸、排版子项、求解内容高，并放置流外后代 / Resolves the sizes, lays out the children, solves the content height and positions out-of-flow descendants
 */
static float layout_block(LayoutCtx *ctx, HpeBox *box, float x, float y, float avail_width,
                          float avail_height)
{
    HpeStyle *st = &box->style;
    float m[4], p[4], b[4], inner_w;
    int no_top_sep, no_bottom_sep;
    float cur_y, inner_h;
    int had_block = 0;
    float prev_bottom = 0.0f;

    block_reset_lines(box);
    block_resolve_edges(ctx, st, avail_width, m, p, b, &no_top_sep, &no_bottom_sep);
    block_auto_margins(ctx, st, avail_width, m, p, b);

    /* `avail_width` is the margin box; the content width is solved from what is
     * left after the horizontal margins (which may be negative). Percentages
     * still resolve against the full containing block width. */
    inner_w = solve_content_width(ctx, st, avail_width, avail_width - m[3] - m[1], p, b);

    box->content_box.x = x + m[3] + b[3] + p[3];
    box->content_box.y = y + m[0] + b[0] + p[0];
    box->content_box.w = inner_w;

    cur_y = block_flow_children(ctx, box, inner_w, avail_height, m, b, no_top_sep, &had_block,
                                &prev_bottom);
    inner_h = block_content_height(ctx, box, cur_y - box->content_box.y, cur_y, prev_bottom,
                                   had_block, no_bottom_sep, avail_width, avail_height, m, p, b);
    box->content_box.h = inner_h;

    box->border_box = hpe_rect(box->content_box.x - b[3] - p[3],
                               box->content_box.y - b[0] - p[0],
                               inner_w + p[3] + p[1] + b[3] + b[1],
                               inner_h + p[0] + p[2] + b[0] + b[2]);
    box->pad_box = hpe_rect(box->content_box.x - p[3], box->content_box.y - p[0],
                            inner_w + p[3] + p[1], inner_h + p[0] + p[2]);

    block_apply_relative_offset(ctx, box, avail_width);
    block_position_abspos_children(ctx, box);

    {
        float outer_bottom = (had_block && no_bottom_sep)
                                 ? collapse_margins(prev_bottom, m[2])
                                 : m[2];
        return m[0] + box->border_box.h + outer_bottom;
    }
}

static void bind_boxes(HpeBox *box);

/* ---------------- incremental rebuild ----------------
 * After a DOM mutation we need not re-walk the whole document: the stylesheet is
 * unchanged, so a box can only differ from the previous pass if it lies inside
 * the mutated node's subtree. The rebuild below reuses the entire box tree and
 * replaces just the subtree rooted at the mutated node, then replays the
 * ordinary geometry pass -- which is where every position is decided, so the
 * result is identical to a full build (siblings re-flow because the pass runs in
 * full; the reused boxes keep their styles, which cannot have changed).
 */

/**
 * @brief 把盒插入父盒子链表中 ref 之后 / Insert a box after `ref` in a parent's child list
 *
 * @param[in,out] parent 父盒 / Parent box
 * @param[in]     ref 参照盒；`NULL` 表示插到表头 / Reference box, `NULL` inserts at the head
 * @param[in,out] box 待插入的盒 / Box to insert
 *
 * @note 顺序与整体构建一致，因为 ref 是生成盒的最近前置 DOM 兄弟 / Produces the same order as a full build, since `ref` is the nearest preceding DOM sibling that generated a box
 */
static void box_insert_after(HpeBox *parent, HpeBox *ref, HpeBox *box)
{
    box->parent = parent;
    if (!ref) {
        box->next = parent->first_child;
        parent->first_child = box;
        if (!parent->last_child)
            parent->last_child = box;
        return;
    }
    box->next = ref->next;
    ref->next = box;
    if (parent->last_child == ref)
        parent->last_child = box;
}

/**
 * @brief 就地替换某 DOM 节点对应的盒子子树 / Replace the box subtree of one DOM node in place
 *
 * @param[in]     dom 发生变更的 DOM 节点 / The mutated DOM node
 * @param[in]     sheet 样式表 / Stylesheet
 *
 * @return 成功返回 1；无法就地替换时返回 0 / 1 on success, 0 when an in-place replacement is not possible
 *
 * @note 只重建 dom 自身子树；文本节点因无标签返回 0，交给整体构建 / Only `dom`'s own subtree is rebuilt; text nodes return 0 because they have no tag for build_element
 */
static int rebuild_box_in_place(HpeDomNode *dom, HpeStylesheet *sheet)
{
    HpeDomNode *pdom;
    HpeBox *pbox, *old, *fresh, *ref, *c;
    HpeDomNode *psib;

    if (!dom || !dom->parent)
        return 0;
    pdom = dom->parent;
    pbox = (HpeBox *)pdom->box;
    if (!pbox)
        return 0; /* the parent is not rendered (e.g. inside display:none) */
    if (dom->type == HPE_NODE_TEXT)
        return 0; /* text nodes have no tag for build_element: rebuild the parent */

    /* find and unlink the old box for `dom`, remembering its predecessor */
    ref = NULL;
    old = NULL;
    for (c = pbox->first_child; c; c = c->next) {
        if (c->node == dom) {
            old = c;
            break;
        }
        ref = c;
    }
    if (old) {
        if (ref)
            ref->next = old->next;
        else
            pbox->first_child = old->next;
        if (pbox->last_child == old)
            pbox->last_child = ref;
        hpe_layout_free(old);
    } else {
        /* a newly inserted node: anchor on the box of the nearest preceding DOM
         * sibling, so DOM order is preserved */
        ref = NULL;
        for (psib = pdom->first_child; psib && psib != dom; psib = psib->next_sibling)
            if (psib->box)
                ref = (HpeBox *)psib->box;
        if (psib != dom)
            ref = NULL;
    }

    fresh = build_element(dom, sheet, &pbox->style);
    if (fresh)
        box_insert_after(pbox, ref, fresh);
    return 1;
}

/**
 * @brief 对既有盒子树重跑几何排版 / Re-run the geometry pass over an existing box tree
 *
 * @param[in,out] root 盒子树根 / Box tree root
 * @param[in]     vw 视口宽度 / Viewport width
 * @param[in]     vh 视口高度；非正时取 4*vw / Viewport height, defaulting to 4*vw when not positive
 *
 * @note 不做任何重建，只重算几何并回填反向指针 / Rebuilds nothing; only recomputes geometry and rebinds back pointers
 */
static void relayout_existing(HpeBox *root, float vw, float vh)
{
    LayoutCtx ctx;
    ctx.vw = vw;
    ctx.vh = vh > 0.0f ? vh : vw * 4.0f;
    ctx.root_font_size = root->style.font_size > 0.0f ? root->style.font_size : 16.0f;
    if (root->kind == HPE_BOX_BLOCK)
        layout_block(&ctx, root, 0.0f, 0.0f, vw, ctx.vh);
    bind_boxes(root);
}

/**
 * @brief DOM 变更后的增量重建 / Incremental rebuild after a DOM mutation
 *
 * @param[in]     document 文档根节点 / Document root node
 * @param[in]     sheet 样式表 / Stylesheet
 * @param[in]     viewport_width 视口宽度 / Viewport width
 * @param[in]     viewport_height 视口高度 / Viewport height
 * @param[in,out] old_root 上一轮的盒子树 / Previous pass's box tree
 * @param[in]     reflow_root 子树发生变更的最高 DOM 节点 / Highest DOM node whose subtree changed
 *
 * @return 成功时返回复用的 old_root；输入不允许局部重建时返回 `NULL` / The reused `old_root` on success, or `NULL` when the inputs do not permit a partial pass
 *
 * @note 变更子树之外的盒原样复用，只重建从根到 reflow_root 的路径；几何结果与 hpe_layout_build 一致 / Boxes outside the changed subtree are reused verbatim; only the path from the root down to `reflow_root` is rebuilt, and the geometry matches hpe_layout_build
 */
HpeBox *hpe_layout_rebuild(HpeDomNode *document, HpeStylesheet *sheet, float viewport_width,
                           float viewport_height, HpeBox *old_root, HpeDomNode *reflow_root)
{
    HpeDomNode *html;
    /* contract: return the (reused) root on success, or NULL when the inputs do
     * not permit a partial pass -- `old_root` is left untouched either way, so
     * the caller can fall back to a full build without leaking it. */
    if (!document || !old_root || !reflow_root)
        return NULL;
    html = hpe_dom_find_tag(document, "html");
    if (!html || old_root->node != html)
        return NULL;
    /* only an element with a rendered parent can be spliced in place; anything
     * else (the root itself, a detached node, a text node) needs a full build */
    if (reflow_root == html || reflow_root->type != HPE_NODE_ELEMENT)
        return NULL;
    if (!rebuild_box_in_place(reflow_root, sheet))
        return NULL;
    relayout_existing(old_root, viewport_width, viewport_height);
    return old_root;
}

/**
 * @brief 为文档构建并排版盒子树 / Build and lay out the box tree for a document
 *
 * @param[in]     document 文档根节点 / Document root node
 * @param[in]     sheet 样式表 / Stylesheet
 * @param[in]     viewport_width 视口宽度 / Viewport width
 * @param[in]     viewport_height 视口高度；非正时取 4*viewport_width / Viewport height, defaulting to 4*viewport_width when not positive
 *
 * @return 盒子树根；无文档或 `<html>` 时返回 `NULL` / Box tree root, or `NULL` when there is no document or no `<html>`
 *
 * @note 视口即初始包含块，`height:100%` 与 abspos `bottom:` 都相对它解析 / The viewport is the initial containing block, so `height:100%` and abspos `bottom:` resolve against it
 */
HpeBox *hpe_layout_build(HpeDomNode *document, HpeStylesheet *sheet, float viewport_width,
                         float viewport_height)
{
    HpeDomNode *html;
    HpeBox *root;
    HpeStyle root_parent;
    LayoutCtx ctx;

    if (!document)
        return NULL;
    html = hpe_dom_find_tag(document, "html");
    if (!html)
        return NULL;

    ctx.vw = viewport_width;
    /* the initial containing block is the viewport: `height: 100%` on html/body
     * and abspos `bottom:` both resolve against it */
    ctx.vh = viewport_height > 0.0f ? viewport_height : viewport_width * 4.0f;
    ctx.root_font_size = 16.0f; /* refined once the root style is known */

    root_parent = hpe_style_default();
    root = build_element(html, sheet, &root_parent);
    if (!root)
        return NULL;
    /* `rem` resolves against the root element's font size */
    if (root->style.font_size > 0.0f)
        ctx.root_font_size = root->style.font_size;
    if (root->kind == HPE_BOX_BLOCK)
        layout_block(&ctx, root, 0.0f, 0.0f, viewport_width, ctx.vh);
    bind_boxes(root);
    return root;
}

/**
 * @brief 回填元素到盒的反向指针 / Fill in element-to-box back pointers
 *
 * @param[in,out] box 盒子树根 / Box tree root
 *
 * @note 脚本读取 clientWidth/clientHeight 时依赖该指针，须在每次（重新）排版后重建 / Scripts rely on it for clientWidth/clientHeight, so it is rebuilt after every (re)layout
 */
static void bind_boxes(HpeBox *box)
{
    HpeBox *child;
    if (!box)
        return;
    if (box->node)
        box->node->box = box;
    for (child = box->first_child; child; child = child->next)
        bind_boxes(child);
}


/**
 * @brief 释放整棵盒子树 / Free an entire box tree
 *
 * @param[in,out] box 待释放的盒；可为 `NULL` / Box to free, may be `NULL`
 *
 * @note 递归释放子盒、各行 runs、行数组与浮动态带，并清空对应 DOM 节点的反向指针 / Recursively frees children, each line's runs, the line array and the float band, clearing the matching DOM back pointer
 */
void hpe_layout_free(HpeBox *box)
{
    HpeBox *child, *next;
    int i;
    if (!box)
        return;
    for (child = box->first_child; child; child = next) {
        next = child->next;
        hpe_layout_free(child);
    }
    if (box->node && box->node->box == box)
        box->node->box = NULL;
    for (i = 0; i < box->line_count; i++) {
        if (box->lines[i].runs)
            hpe_free(box->lines[i].runs);
    }
    if (box->lines)
        hpe_free(box->lines);
    if (box->floats.items)
        hpe_free(box->floats.items);
    hpe_free(box);
}

/**
 * @brief 求根盒的总内容高度 / Total content height of the root box
 *
 * @param[in]     root 根盒 / Root box
 *
 * @return 根盒边框盒底边的文档 y；root 为 `NULL` 时返回 0 / Document y of the root's border-box bottom, or 0 when `root` is `NULL`
 */
float hpe_layout_content_height(HpeBox *root)
{
    if (!root)
        return 0.0f;
    return root->border_box.y + root->border_box.h;
}
