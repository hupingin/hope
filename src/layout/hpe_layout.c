#include "layout/hpe_layout.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/hpe_util.h"
#include "platform/hpe_platform.h"
#include "render/hpe_image.h"

/* ---------------- layout context ---------------- */

typedef struct {
    float vw, vh;         /* viewport size; used as the initial containing block */
    float root_font_size; /* the root element's font size, for `rem` */
    /* abspos boxes are collected during the ordinary pass and positioned in a
     * deferred pass: a containing block ancestor's pad_box is only final once
     * its own subtree (which includes this abspos box) has been laid out */
    HpeBox **abspos;
    int abspos_count, abspos_cap;
    int abspos_fixup; /* 1 while the deferred pass runs: position immediately */
} LayoutCtx;

/**
 * @brief 初始化布局上下文 / Initialize a layout context
 *
 * @param[out]    ctx 布局上下文 / Layout context
 * @param[in]     vw 视口宽度 / Viewport width
 * @param[in]     vh 视口高度，非正时取 4*vw / Viewport height, 4*vw when not positive
 * @param[in]     root_font 根元素字号，非正时取 16 / Root font size, 16 when not positive
 */
static void layout_ctx_init(LayoutCtx *ctx, float vw, float vh, float root_font)
{
    ctx->vw = vw;
    ctx->vh = vh > 0.0f ? vh : vw * 4.0f;
    ctx->root_font_size = root_font > 0.0f ? root_font : 16.0f;
    ctx->abspos = NULL;
    ctx->abspos_count = 0;
    ctx->abspos_cap = 0;
    ctx->abspos_fixup = 0;
}

static void layout_abspos(LayoutCtx *ctx, HpeBox *box); /* defined below */

/**
 * @brief 执行延迟的绝对定位 pass 并释放收集缓冲 / Run the deferred abspos pass and release the collection buffer
 *
 * @param[in,out] ctx 布局上下文 / Layout context
 *
 * @note 收集序即文档序（外层先于内层），因此嵌套 abspos 的包含块在补算时已定稿；
 *       补算期间新发现的 abspos 直接就地定位 / Collection order is document order (outer before
 *       inner), so nested abspos containing blocks are final by fixup time; abspos found during
 *       the fixup itself is positioned immediately
 */
static void abspos_flush(LayoutCtx *ctx)
{
    int i;
    ctx->abspos_fixup = 1;
    for (i = 0; i < ctx->abspos_count; i++)
        layout_abspos(ctx, ctx->abspos[i]);
    ctx->abspos_fixup = 0;
    free(ctx->abspos);
    ctx->abspos = NULL;
    ctx->abspos_count = 0;
    ctx->abspos_cap = 0;
}

/**
 * @brief 把一个 abspos 盒登记到延迟列表 / Queue an abspos box for the deferred pass
 *
 * @param[in,out] ctx 布局上下文 / Layout context
 * @param[in,out] box 待定位的流外盒 / The out-of-flow box to position later
 *
 * @note 缓冲扩容失败时退回立即定位，保证降级仍可见 / Falls back to immediate positioning when the buffer cannot grow
 */
static void abspos_queue(LayoutCtx *ctx, HpeBox *box)
{
    if (ctx->abspos_count == ctx->abspos_cap) {
        int ncap = ctx->abspos_cap ? ctx->abspos_cap * 2 : 16;
        HpeBox **ni = (HpeBox **)realloc(ctx->abspos, (size_t)ncap * sizeof(HpeBox *));
        if (!ni) {
            layout_abspos(ctx, box); /* degrade to the old immediate behaviour */
            return;
        }
        ctx->abspos = ni;
        ctx->abspos_cap = ncap;
    }
    ctx->abspos[ctx->abspos_count++] = box;
}

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
/* the grid pass sizes its `auto` columns from the items' max-content widths,
 * so it needs the table helper before the table section is defined */
static float table_cell_pref_w(LayoutCtx *ctx, HpeBox *cell);

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
 * @brief 为 `<img>` 元素构造替换盒 / Build the replaced box for an `<img>` element
 *
 * @details 解码 `src` 取得固有像素尺寸，把它记在盒上供排版求解 used 尺寸；作者通过 CSS
 *          或 `width`/`height` 属性声明的尺寸优先，属性值只在样式未声明对应长属性时生效。
 *          The `src` is decoded to obtain the intrinsic pixel size, which the box records so
 *          layout can solve the used size; an author size from CSS or the `width`/`height`
 *          attributes wins, with the attributes applied only when the corresponding longhand
 *          was not declared by any style rule.
 *
 * @param[in]     node `<img>` DOM 节点 / The `<img>` DOM node
 * @param[in]     style 计算样式 / Computed style
 *
 * @return 对应的盒 / The corresponding box
 *
 * @note 解码失败时盒仍有固有尺寸 0×0，CSS 尺寸仍可让它占位 / On a decode failure the box still has a 0x0 intrinsic size and a CSS size can still give it a box
 *
 * @see build_svg_box build_element
 */
static HpeBox *build_img_box(HpeDomNode *node, HpeStyle style)
{
    const char *src = hpe_dom_attr(node, "src");
    const char *aw = hpe_dom_attr(node, "width");
    const char *ah = hpe_dom_attr(node, "height");
    HpeBox *box;
    const HpeImage *img = NULL;
    char path[1024];

    /* an `<img>` is inline-level by default; an author `display: block` (or
     * `inline-block`) is honoured, and absolute/fixed positioning blockifies it
     * exactly like an ordinary element */
    if (style.position == HPE_POS_ABSOLUTE || style.position == HPE_POS_FIXED ||
        style.display == HPE_DISPLAY_BLOCK || style.display == HPE_DISPLAY_FLEX ||
        style.display == HPE_DISPLAY_GRID || style.display == HPE_DISPLAY_TABLE)
        box = box_new(HPE_BOX_BLOCK);
    else
        box = box_new(HPE_BOX_INLINE_BLOCK);

    box->node = node;
    box->is_replaced = 1;
    box->repl_css_w = style.width;
    box->repl_css_h = style.height;

    if (src && src[0] && hpe_image_resolve_url(src, path, sizeof(path)))
        img = hpe_image_cache_get(hpe_image_shared_cache(), path, HPE_IMG_UNKNOWN, -1, -1);
    if (img) {
        box->repl_w = img->width;
        box->repl_h = img->height;
    }

    /* the `width`/`height` attributes are presentational hints: they lose to any
     * authored CSS declaration, so only fill a longhand that is still `auto` */
    if (aw && aw[0] && style.width.unit == HPE_LEN_AUTO)
        box->repl_css_w = hpe_len_px((float)atof(aw));
    if (ah && ah[0] && style.height.unit == HPE_LEN_AUTO)
        box->repl_css_h = hpe_len_px((float)atof(ah));

    box->style = style;
    return box;
}

/**
 * @brief 为一个元素的 `::before` / `::after` 生成内容盒 / Build the generated-content box for `::before` / `::after`
 *
 * @details 依次求值两份样式：先把父选择器链上的伪元素规则套用一次（`hpe_css_compute_pseudo`），仅当 `content` 非 `none` 时生成盒；`attr(name)` 形式的 `content` 在此时从节点属性解析为文本。
 *          Two styles are evaluated in turn: the pseudo-element rules matching the parent are applied via `hpe_css_compute_pseudo`, a box is only built when `content` is not `none`, and an `attr(name)` `content` is resolved to text from the node's attribute at this point.
 *
 * @param[in]     node 宿主元素 / Host element
 * @param[in]     sheet 样式表 / Stylesheet
 * @param[in]     parent_style 宿主元素的计算样式 / The host element's computed style
 * @param[in]     which `HPE_PE_BEFORE` 或 `HPE_PE_AFTER` / `HPE_PE_BEFORE` or `HPE_PE_AFTER`
 *
 * @return 生成盒；`content` 为 `none`/`normal` 或伪元素不可见时返回 `NULL` / The generated box, or `NULL` when `content` is `none`/`normal` or the pseudo-element is not visible
 *
 * @note 生成盒沿用宿主元素的 `display`（行内级或块级），文本来自 `content` / The generated box reuses the host's `display` (inline or block level) and its text comes from `content`
 *
 * @see build_children build_element
 */
static HpeBox *build_pseudo_box(HpeDomNode *node, HpeStylesheet *sheet, const HpeStyle *parent_style,
                                int which)
{
    HpeStyle style;
    HpeBox *box;
    const char *txt;

    hpe_css_compute_pseudo(sheet, node, parent_style, which, &style);
    if (!style.has_content)
        return NULL;
    txt = style.content;
    if (style.attr_content[0]) {
        const char *av = hpe_dom_attr(node, style.attr_content);
        char joined[HPE_CONTENT_MAX];
        snprintf(joined, sizeof(joined), "%s%s", txt, av ? av : "");
        snprintf(style.content, sizeof(style.content), "%s", joined);
        style.attr_content[0] = '\0';
        txt = style.content;
    }
    if (!txt[0])
        return NULL;
    /* A generated box carries text directly, so it has to be a text box: the
     * inline walker only turns HPE_BOX_TEXT into text runs, and letting an
     * anonymous inline carry the text would leak its style onto the siblings.
     * Block-level content is wrapped in a block box holding that text box. */
    if (style.display == HPE_DISPLAY_BLOCK || style.display == HPE_DISPLAY_FLEX ||
        style.display == HPE_DISPLAY_GRID || style.display == HPE_DISPLAY_TABLE) {
        HpeBox *outer = box_new(HPE_BOX_BLOCK);
        HpeBox *inner = box_new(HPE_BOX_TEXT);
        outer->node = node;
        outer->style = style;
        inner->node = node;
        inner->style = style;
        inner->text = hpe_strdup(txt);
        inner->text_len = (int)strlen(txt);
        inner->owns_text = 1;
        box_append(outer, inner);
        return outer;
    }
    box = box_new(HPE_BOX_TEXT);
    box->node = node;
    box->style = style;
    box->text = hpe_strdup(txt); /* owned; released by hpe_layout_free */
    box->text_len = (int)strlen(txt);
    box->owns_text = 1;
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
 * @note 元素子节点递归建盒，文本子节点生成文本盒；首尾各尝试一次 `::before` / `::after` 生成盒 / Element children recurse, text children become text boxes, and a `::before` / `::after` generated box is attempted at the head and tail
 *
 * @see build_pseudo_box build_element
 */
static void build_children(HpeBox *box, HpeDomNode *node, HpeStylesheet *sheet,
                           const HpeStyle *style)
{
    HpeDomNode *child;
    HpeBox *gen;
    gen = build_pseudo_box(node, sheet, style, HPE_PE_BEFORE);
    if (gen)
        box_append(box, gen);
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
    gen = build_pseudo_box(node, sheet, style, HPE_PE_AFTER);
    if (gen)
        box_append(box, gen);
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

    /* flex / grid items blockify: an inline child of a flex or grid container
     * is a block box (CSS Flexbox §4, CSS Grid §6), which is also what keeps
     * its text from being dropped by the flex/grid pass. */
    if (parent_style &&
        (parent_style->display == HPE_DISPLAY_FLEX || parent_style->display == HPE_DISPLAY_GRID) &&
        style.display == HPE_DISPLAY_INLINE)
        style.display = HPE_DISPLAY_BLOCK;

    if (!strcmp(node->tag, "svg"))
        return build_svg_box(node, style);
    if (!strcmp(node->tag, "img"))
        return build_img_box(node, style);

    /* absolutely / fixed positioned boxes blockify their display;
     * floated boxes are laid out as atomic inline-level (shrink-to-fit) boxes.
     * Every table-internal display (table, row-group, row, cell, caption) is
     * block-level here: the table pass gives them their real geometry. */
    if (style.position == HPE_POS_ABSOLUTE || style.position == HPE_POS_FIXED)
        box = box_new(HPE_BOX_BLOCK);
    else if (style.float_side != HPE_FLOAT_NONE || style.display == HPE_DISPLAY_INLINE_BLOCK)
        box = box_new(HPE_BOX_INLINE_BLOCK);
    else if (style.display == HPE_DISPLAY_INLINE)
        box = box_new(HPE_BOX_INLINE);
    else
        box = box_new(HPE_BOX_BLOCK);

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
    HpeBox *inline_box; /* non-NULL => inline open/close marker (`open` flag) */
    int open;           /* 1 = inline box opens here, 0 = closes here */
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
 * @brief 向段落追加一个行内盒开/闭标记 / Append an inline open/close marker to a paragraph
 *
 * @param[in,out] para 段落 / Paragraph
 * @param[in]     box 行内盒 / The inline box
 * @param[in]     open 1=开始，0=结束 / 1 to open, 0 to close
 */
static void para_add_inline(HpeParagraph *para, HpeBox *box, int open)
{
    HpeSegment *seg;
    para->items = (HpeSegment *)hpe_grow(para->items, &para->cap, (size_t)para->count + 1,
                                         sizeof(HpeSegment));
    seg = &para->items[para->count++];
    memset(seg, 0, sizeof(*seg));
    seg->inline_box = box;
    seg->open = open;
}

/**
 * @brief 收集行内级内容到段落 / Collect inline-level content into a paragraph
 *
 * @param[in]     ctx 布局上下文 / Layout context
 * @param[in,out] box 待收集的盒 / Box to collect
 * @param[in]     inner_w 内容区宽度 / Inner width
 * @param[out]    para 接收内容的段落 / Paragraph receiving the content
 *
 * @note 行内块盒在此先行排版以便行构建器得知其尺寸；浮动盒被跳过；行内盒发出
 *       开/闭标记供行内碎片记录 / Inline-block boxes are laid out here so the line
 *       builder knows their size; floated boxes are skipped; inline boxes emit
 *       open/close markers for inline fragment recording
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
    if (box->kind == HPE_BOX_INLINE)
        para_add_inline(para, box, 1);
    for (child = box->first_child; child; child = child->next)
        collect_inline(ctx, child, inner_w, para);
    if (box->kind == HPE_BOX_INLINE)
        para_add_inline(para, box, 0);
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
 * @brief 判定一个盒是否建立块格式化上下文（BFC）/ Test whether a box establishes a block formatting context
 *
 * @param[in] box 目标盒 / The box to test
 *
 * @return 建立返回 1，否则 0 / 1 when it establishes a BFC, otherwise 0
 *
 * @note 浮动、绝对/固定定位、行内块、表格单元与 `overflow` 非 `visible` 的块容器
 *       均建立 BFC / Floats, absolutely/fixed positioned boxes, inline-blocks, table
 *       cells and block containers with a non-visible `overflow` all establish one
 */
static int establishes_bfc(HpeBox *box)
{
    const HpeStyle *s = &box->style;
    if (s->float_side != HPE_FLOAT_NONE)
        return 1;
    if (s->position == HPE_POS_ABSOLUTE || s->position == HPE_POS_FIXED)
        return 1;
    if (box->kind == HPE_BOX_INLINE_BLOCK)
        return 1;
    if (s->display == HPE_DISPLAY_TABLE_CELL || s->display == HPE_DISPLAY_INLINE_BLOCK)
        return 1;
    if (s->overflow_x != HPE_OVERFLOW_VISIBLE || s->overflow_y != HPE_OVERFLOW_VISIBLE)
        return 1;
    return 0;
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
 * @note 结果夹紧到块自身的内容盒；任何建立 BFC 的祖先（浮动、行内块、
 *       `overflow` 非 visible、定位、表格单元）都会终止避让继承 / The result is clamped to the block's own content box; any BFC-establishing ancestor (float, inline-block, non-visible overflow, positioned, table cell) stops the walk
 */
static void band_edges_inherited(HpeBox *block, float y0, float y1, float *out_left,
                                 float *out_right)
{
    HpeBox *b;
    float L = block->content_box.x;
    float R = block->content_box.x + block->content_box.w;
    /* a BFC root walls off floats that live outside it: its line boxes dodge
     * only the floats of its own band (handled by the paragraph state), never
     * the bands of ancestors */
    if (establishes_bfc(block)) {
        *out_left = L;
        *out_right = R;
        return;
    }
    b = block->parent;
    while (b) {
        if (b->floats.count > 0) {
            float l, r;
            band_edges(&b->floats, b->content_box.x, b->content_box.w, y0, y1, &l, &r);
            if (l > L)
                L = l;
            if (r < R)
                R = r;
        }
        if (establishes_bfc(b))
            break; /* any BFC boundary stops the inherited float avoidance */
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
        for (r = 0; r < line->frag_count; r++) {
            line->frags[r].x0 += offset;
            line->frags[r].x1 += offset;
        }
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
    /* stack of inline boxes currently open, for per-line fragment recording */
    HpeBox *open_b[24];
    float open_x0[24];    /* fragment left edge of the current line */
    float open_carry[24]; /* own left advance folded into pad_carry (no line yet) */
    int open_started[24]; /* 1 once the box has emitted its first fragment */
    int open_n;
    float pad_carry;      /* inline pad/border advances made while no line is open */
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
    int i;
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
    /* inline pad/border shifts that happened before any line existed */
    ps->x += ps->pad_carry;
    ps->pad_carry = 0.0f;
    /* continuing inline boxes restart their fragment at the new line start;
     * boxes whose own left advance went into pad_carry back off that amount
     * so their fragment starts at the border-box edge */
    for (i = 0; i < ps->open_n; i++) {
        if (ps->open_started[i])
            ps->open_x0[i] = ps->x;
        else
            ps->open_x0[i] = ps->x - ps->open_carry[i];
    }
    ps->line_idx = line_begin(ps->block);
}

/**
 * @brief 为行内盒在当前行记录一个碎片 / Record one inline-box fragment on the open line
 *
 * @param[in,out] ps 段落排版状态 / Paragraph layout state
 * @param[in]     box 行内盒 / The inline box
 * @param[in]     x0 碎片内容左缘 / Fragment content left edge
 * @param[in]     x1 碎片内容右缘 / Fragment content right edge
 * @param[in]     flags 1=盒从此行开始，2=盒在此行结束 / 1 = box starts on this line, 2 = box ends here
 */
static void para_frag_push(ParaState *ps, HpeBox *box, float x0, float x1, int flags)
{
    HpeLine *line = &ps->block->lines[ps->line_idx];
    HpeInlineFrag *fr;
    HpeFont *f;
    if (x1 <= x0 + 0.01f)
        return;
    line->frags = (HpeInlineFrag *)hpe_grow(line->frags, &line->frag_cap,
                                            (size_t)line->frag_count + 1,
                                            sizeof(HpeInlineFrag));
    fr = &line->frags[line->frag_count++];
    fr->box = box;
    fr->x0 = x0;
    fr->x1 = x1;
    fr->flags = flags;
    f = font_for(&box->style);
    fr->ascent = hpe_font_ascent(f);
    fr->descent = hpe_font_descent(f);
}

/**
 * @brief 处理一个行内盒开标记：入栈并推进左内边距/边框 / Handle an inline open marker: push and advance the left pad/border
 *
 * @param[in,out] ps 段落排版状态 / Paragraph layout state
 * @param[in]     box 行内盒 / The inline box
 */
static void para_inline_open(ParaState *ps, HpeBox *box)
{
    const HpeStyle *st = &box->style;
    float adv;
    if (ps->open_n >= 24)
        return;
    ps->open_b[ps->open_n] = box;
    ps->open_x0[ps->open_n] = ps->x;
    ps->open_started[ps->open_n] = 0;
    adv = margin_px(ps->ctx, st, st->margin[3], ps->inner_w) +
          side_px(ps->ctx, st, st->padding[3], ps->inner_w) +
          (st->border_w[3] > 0.0f ? st->border_w[3] : 0.0f);
    if (ps->line_idx >= 0) {
        ps->open_carry[ps->open_n] = 0.0f;
        ps->x += adv;
    } else {
        ps->open_carry[ps->open_n] = adv;
        ps->pad_carry += adv;
    }
    ps->open_n++;
}

/**
 * @brief 处理一个行内盒闭标记：记录碎片、弹出并推进右内边距/边框 / Handle an inline close marker: emit the fragment, pop and advance the right pad/border
 *
 * @param[in,out] ps 段落排版状态 / Paragraph layout state
 * @param[in]     box 行内盒 / The inline box
 */
static void para_inline_close(ParaState *ps, HpeBox *box)
{
    const HpeStyle *st;
    float adv;
    int i = ps->open_n - 1, k;
    while (i >= 0 && ps->open_b[i] != box)
        i--;
    if (i < 0)
        return;
    if (ps->line_idx >= 0)
        para_frag_push(ps, box, ps->open_x0[i], ps->x, ps->open_started[i] ? 2 : 3);
    st = &box->style;
    adv = side_px(ps->ctx, st, st->padding[1], ps->inner_w) +
          (st->border_w[1] > 0.0f ? st->border_w[1] : 0.0f) +
          margin_px(ps->ctx, st, st->margin[1], ps->inner_w);
    if (ps->line_idx >= 0)
        ps->x += adv;
    else
        ps->pad_carry += adv;
    for (k = i; k < ps->open_n - 1; k++) {
        ps->open_b[k] = ps->open_b[k + 1];
        ps->open_x0[k] = ps->open_x0[k + 1];
        ps->open_started[k] = ps->open_started[k + 1];
    }
    ps->open_n--;
}

/**
 * @brief 关闭当前行并推进纵向游标 / Close the current line and advance the vertical cursor
 *
 * @param[in,out] ps 段落排版状态 / Paragraph layout state
 */
static void para_close_line(ParaState *ps)
{
    int i;
    /* every still-open inline box gets a fragment for the line being closed */
    for (i = 0; i < ps->open_n; i++) {
        para_frag_push(ps, ps->open_b[i], ps->open_x0[i], ps->x,
                       ps->open_started[i] ? 0 : 1);
        ps->open_started[i] = 1;
    }
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
        else if (seg->inline_box)
            seg->open ? para_inline_open(&ps, seg->inline_box)
                      : para_inline_close(&ps, seg->inline_box);
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
 * @brief 求解替换元素的 used 内容尺寸并写回样式 / Solve a replaced element's used content size and write it back into the style
 *
 * @details 按 CSS 替换元素尺寸规则求解：两个轴都 `auto` 时用固有尺寸；只有一个轴有声明值时另一轴按
 *          固有宽高比缩放；两个轴都有声明值时原样使用。声明值按元素自身的 `box-sizing` 换算到内容盒，
 *          固有尺寸天然就是内容盒尺寸。求解结果写回 `style.width`/`style.height`（px 内容盒值）并把
 *          `box-sizing` 归一为 `content-box`，使后续的 `solve_content_width` / `block_resolve_height`
 *          直接采用；原始声明保存在 `repl_css_*`，因此每次排版重算都幂等。
 *          The CSS replaced-element sizing rules apply: with both axes `auto` the intrinsic size is
 *          used, with one axis declared the other scales by the intrinsic ratio, and with both
 *          declared they are used verbatim. A declared value is converted to the content box per the
 *          element's own `box-sizing`, while the intrinsic size already is a content-box size. The
 *          result is written into `style.width`/`style.height` as px content-box values with
 *          `box-sizing` normalised to `content-box`, so `solve_content_width` and
 *          `block_resolve_height` adopt it directly; the original declaration lives on in
 *          `repl_css_*`, which keeps a re-run idempotent.
 *
 * @param[in]     ctx 布局上下文 / Layout context
 * @param[in,out] box 替换元素盒 / The replaced-element box
 * @param[in]     cb_w 包含块宽度（解析百分比宽度）/ Containing-block width, the basis for percentage widths
 * @param[in]     cb_h 包含块高度（解析百分比高度）/ Containing-block height, the basis for percentage heights
 *
 * @note 固有尺寸为 0 时视为不会有固有宽高比，缺少声明的轴取 0 / A zero intrinsic size means no usable ratio, so an undeclared axis becomes 0
 *
 * @see layout_inline_block layout_block
 */
/**
 * @brief 求替换元素的可用内容尺寸（已扣除盒模型外扩）/ Resolve a replaced element's available content size (border-box padding removed)
 *
 * @param[in]     ctx 布局上下文 / Layout context
 * @param[in]     st 样式 / Style
 * @param[in]     bw 声明的 width（-1 表示 auto）/ Declared width, -1 for auto
 * @param[in]     bh 声明的 height（-1 表示 auto）/ Declared height, -1 for auto
 * @param[in]     cb_w 包含块宽度 / Containing-block width
 * @param[in]     cb_h 包含块高度 / Containing-block height
 * @param[in,out] w 输入声明的 width，输出 content-box width / In: declared width; out: content-box width
 * @param[in,out] h 输入声明的 height，输出 content-box height / In: declared height; out: content-box height
 *
 * @note `box-sizing: border-box` 时从声明值里减去外扩 / Under `box-sizing: border-box` the padding+border is subtracted from the declared value
 *
 * @see replaced_used_size
 */
static void repl_resolve_declared(LayoutCtx *ctx, HpeStyle *st, float bw, float bh, float cb_w,
                                  float cb_h, float *w, float *h)
{
    float extra_w =
        side_px(ctx, st, st->padding[3], cb_w) + side_px(ctx, st, st->padding[1], cb_w) +
        (st->border_w[3] > 0.0f ? st->border_w[3] : 0.0f) +
        (st->border_w[1] > 0.0f ? st->border_w[1] : 0.0f);
    float extra_h =
        side_px(ctx, st, st->padding[0], cb_h) + side_px(ctx, st, st->padding[2], cb_h) +
        (st->border_w[0] > 0.0f ? st->border_w[0] : 0.0f) +
        (st->border_w[2] > 0.0f ? st->border_w[2] : 0.0f);
    if (st->box_sizing == HPE_BOX_SIZING_BORDER) {
        if (bw >= 0.0f)
            bw -= extra_w;
        if (bh >= 0.0f)
            bh -= extra_h;
    }
    *w = bw;
    *h = bh;
}

/**
 * @brief 按固有宽高比补齐缺失的那一维 / Fill the missing dimension from the intrinsic aspect ratio
 *
 * @param[in]  iw 固有宽 / Intrinsic width
 * @param[in]  ih 固有高 / Intrinsic height
 * @param[in]  bw 声明的 width（-1 表示 auto）/ Declared width, -1 for auto
 * @param[in]  bh 声明的 height（-1 表示 auto）/ Declared height, -1 for auto
 * @param[out] w 求得的使用宽 / Resolved used width
 * @param[out] h 求得的使用高 / Resolved used height
 *
 * @note 两维都为 auto 时直接用固有尺寸 / With both dimensions auto the intrinsic size is used
 *
 * @see replaced_used_size
 */
static void repl_solve_size(float iw, float ih, float bw, float bh, float *w, float *h)
{
    if (bw < 0.0f && bh < 0.0f) {
        *w = iw;
        *h = ih;
    } else if (bw < 0.0f) {
        *h = bh;
        *w = (ih > 0.0f) ? bh * (iw / ih) : iw;
    } else if (bh < 0.0f) {
        *w = bw;
        *h = (iw > 0.0f) ? bw * (ih / iw) : ih;
    } else {
        *w = bw;
        *h = bh;
    }
}

static void replaced_used_size(LayoutCtx *ctx, HpeBox *box, float cb_w, float cb_h)
{
    HpeStyle *st = &box->style;
    float iw = (float)box->repl_w;
    float ih = (float)box->repl_h;
    float bw = resolve_len(ctx, st, box->repl_css_w, cb_w); /* -1 = auto */
    float bh = resolve_len(ctx, st, box->repl_css_h, cb_h);
    float min_w, max_w, min_h, max_h, w, h;

    repl_resolve_declared(ctx, st, bw, bh, cb_w, cb_h, &bw, &bh);
    repl_solve_size(iw, ih, bw, bh, &w, &h);

    /* min/max describe the content box here (box-sizing was normalised away) */
    min_w = resolve_len(ctx, st, st->min_width, cb_w);
    max_w = resolve_len(ctx, st, st->max_width, cb_w);
    min_h = resolve_len(ctx, st, st->min_height, cb_h);
    max_h = resolve_len(ctx, st, st->max_height, cb_h);
    if (min_w > 0.0f && w < min_w)
        w = min_w;
    if (max_w > 0.0f && w > max_w)
        w = max_w;
    if (min_h > 0.0f && h < min_h)
        h = min_h;
    if (max_h > 0.0f && h > max_h)
        h = max_h;
    if (w < 0.0f)
        w = 0.0f;
    if (h < 0.0f)
        h = 0.0f;

    st->width = hpe_len_px(w);
    st->height = hpe_len_px(h);
    st->box_sizing = HPE_BOX_SIZING_CONTENT;
    box->repl_used_w = w;
    box->repl_used_h = h;
}

/**
 * @brief 排版一个行内块盒 / Lay out an inline-block box
 *
 * @param[in]     ctx 布局上下文 / Layout context
 * @param[in,out] box 行内块盒 / Inline-block box
 * @param[in]     parent_inner_w 父内容区宽度 / Parent inner width
 *
 * @note 有显式宽度时按该宽度排版，否则收缩适应；替换元素用固有尺寸 / Uses an explicit width when present, otherwise shrink-to-fit; a replaced element uses its intrinsic size
 */
static void layout_inline_block(LayoutCtx *ctx, HpeBox *box, float parent_inner_w)
{
    HpeStyle *st = &box->style;
    float extra = side_px(ctx, st, st->padding[3], parent_inner_w) +
                  side_px(ctx, st, st->padding[1], parent_inner_w) +
                  (st->border_w[3] > 0.0f ? st->border_w[3] : 0.0f) +
                  (st->border_w[1] > 0.0f ? st->border_w[1] : 0.0f);

    /* A replaced element (an `<img>`) takes its used size from the intrinsic
     * size and the author's declaration, so solve it before the ordinary
     * width / shrink-to-fit paths (which would measure an empty content box).
     * `layout_block` also runs the same solver, keeping both entry points
     * consistent. */
    if (box->is_replaced) {
        layout_block(ctx, box, 0.0f, 0.0f, parent_inner_w, -1.0f);
        return;
    }

    if (st->width.unit != HPE_LEN_AUTO) {
        /* `width` is the content box for content-box sizing and the border box
         * for border-box; layout_block applies that interpretation itself.
         * Convert to px first: layout_block would otherwise resolve the same
         * percentage a second time against this already-resolved width. */
        float w = resolve_len(ctx, st, st->width, parent_inner_w);
        HpeLength saved_w = st->width;
        if (w < 0.0f)
            w = 0.0f;
        st->width = hpe_len_px(w);
        layout_block(ctx, box, 0.0f, 0.0f, w, -1.0f);
        st->width = saved_w;
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
 * @brief 求绝对定位盒的 margin 盒左缘 / Compute the margin-box left edge of an absolutely positioned box
 *
 * @param[in]     ctx 布局上下文 / Layout context
 * @param[in]     st 计算样式 / Computed style
 * @param[in]     cb 包含块矩形 / Containing block rect
 * @param[in]     mL 左外边距 / Left margin
 * @param[in]     mR 右外边距 / Right margin
 * @param[in]     hasL 左 inset 非 auto 为 1 / 1 when the left inset is not auto
 * @param[in]     L 左 inset / Left inset
 * @param[in]     hasR 右 inset 非 auto 为 1 / 1 when the right inset is not auto
 * @param[in]     R 右 inset / Right inset
 * @param[in]     w used 内容宽 / Used content width
 *
 * @return margin 盒左缘（layout_block 的 x 语义）/ The margin-box left edge (layout_block's x)
 *
 * @note 右锚定按规范从 margin 盒右缘推算：须扣除外边距、水平内边距与边框，
 *       否则 border 盒会越过 inset 位置 / The right anchor derives from the margin-box
 *       right edge: horizontal margins, padding and border are subtracted, or the border
 *       box would overrun the inset position
 */
static float abspos_anchor_x(LayoutCtx *ctx, HpeStyle *st, HpeRect cb, float mL, float mR,
                             int hasL, float L, int hasR, float R, float w)
{
    float pad_lr = side_px(ctx, st, st->padding[3], cb.w) +
                   side_px(ctx, st, st->padding[1], cb.w);
    float bor_lr = st->border_w[3] + st->border_w[1];
    if (hasL)
        return cb.x + L;
    if (hasR)
        return cb.x + cb.w - R - mR - pad_lr - bor_lr - w;
    return cb.x + mL;
}

/**
 * @brief 相对包含块放置一个绝对/固定定位盒 / Position an absolutely/fixed positioned box against its containing block
 *
 * @param[in]     ctx 布局上下文 / Layout context
 * @param[in,out] box 定位盒 / Positioned box
 *
 * @note 固定定位以视口为包含块；同时给出上下 inset 且高度 auto 时拉伸到包含块；
 *       宽度在此一次性解算后换算成 px，避免 layout_block 的百分比路径把
 *       `width:50%` 以 used 宽度为基准再解一次 / Fixed positioning uses the viewport as
 *       containing block; with both vertical insets and an auto height the box stretches.
 *       The width is resolved once here and converted to px so layout_block's percentage
 *       path cannot resolve `width: 50%` against the used width a second time
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
    float x = abspos_anchor_x(ctx, st, cb, mL, mR, hasL, L, hasR, R, w);
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

    {
        HpeLength saved_w = st->width;
        st->width = hpe_len_px(w);
        layout_block(ctx, box, x, ypass, w, cb.h);
        st->width = saved_w;
    }

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
 * Flexbox layout on both main axes, with multi-line wrapping.
 * Implemented: `row` / `row-reverse` / `column` / `column-reverse`,
 * `flex-wrap` (nowrap / wrap / wrap-reverse), `order`-sorted items,
 * `flex-basis` / `flex-grow` / `flex-shrink` resolution, `gap`,
 * `justify-content` (start / center / end / space-between / space-around),
 * `align-items` / `align-self` (stretch / flex-start / center / flex-end /
 * baseline) and auto margins (the `margin-left:auto` push idiom).
 * Anonymous flex items are not modelled by the box tree, so inline and text
 * children are skipped. */

typedef struct {
    HpeBox *box;
    float m[4];        /* resolved margins (auto sides are distributed later) */
    float w;           /* resolved border-box main size on a row */
    float basis;       /* flex base size used for shrink scaling */
    float outer_main;  /* margin-box main size, drives wrapping */
    float outer_h;     /* margin-box height once the item is laid out */
    float grow, shrink;
    int order;
    int auto_ml, auto_mr;
} FlexItem;

/* one flex line: a contiguous item range plus its resolved extents */
typedef struct {
    int start;  /* index of the line's first item */
    int count;  /* number of items on the line */
    float main; /* summed outer main sizes including gaps */
    float cross;/* resolved cross size */
    float lead; /* justify-content leading offset on the main axis */
    float extra;/* justify-content extra spacing between items */
} FlexLine;

/**
 * @brief 求单个 flex item 的交叉轴偏移 / Cross-axis offset for one flex item
 *
 * @param[in]     align 对齐方式 / Alignment
 * @param[in]     cross 交叉轴尺寸 / Cross-axis size
 * @param[in]     outer item 的外尺寸 / Item outer size
 *
 * @return 交叉轴偏移 / Cross-axis offset
 *
 * @note `stretch`/`baseline` 的数值偏移由调用方先行处理，此处退化为 `flex-start` / `stretch` and `baseline` offsets are handled by the caller, so they degrade to `flex-start` here
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
 * @brief 取容器给某个 item 生效的 `align-self` / Resolve the effective `align-self` for one item
 *
 * @param[in]     st 容器样式 / Container style
 * @param[in]     cs item 样式 / Item style
 *
 * @return 生效的对齐方式 / The alignment in effect
 *
 * @note item 的 `align-self != auto` 优先，否则回落容器的 `align-items` / A non-`auto` item `align-self` wins, otherwise the container `align-items` applies
 */
static int flex_effective_align(const HpeStyle *st, const HpeStyle *cs)
{
    return cs->align_self != HPE_ALIGN_ITEMS_AUTO ? cs->align_self : st->align_items;
}

/**
 * @brief 按 `order` 稳定排序 flex item / Stably sort flex items by `order`
 *
 * @param[in,out] items flex item 数组 / Flex item array
 * @param[in]     n item 个数 / Number of items
 *
 * @note 插入排序天然稳定，同 `order` 的项保持文档顺序 / Insertion sort is stable, so equal `order` values keep their document order
 */
static void flex_sort_items(FlexItem *items, int n)
{
    int i, j;
    for (i = 1; i < n; i++) {
        FlexItem key = items[i];
        j = i - 1;
        while (j >= 0 && items[j].order > key.order) {
            items[j + 1] = items[j];
            j--;
        }
        items[j + 1] = key;
    }
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
 * @brief 求一个 flex item 的 `flex-basis` 解析后的主轴尺寸 / Resolve one flex item's `flex-basis` main size
 *
 * @param[in]     ctx 布局上下文 / Layout context
 * @param[in,out] child item 对应的子盒 / Child box of the item
 * @param[in]     inner_w 容器内容区宽度 / Container inner width
 *
 * @return 主轴基础尺寸（`flex-basis:auto` 时回落收缩适应宽度）/ The base main size, falling back to the shrink-to-fit width for `flex-basis:auto`
 *
 * @note 非 `auto` 的 `flex-basis` 覆盖 `width` / A non-`auto` `flex-basis` overrides `width`
 */
static float flex_item_basis(LayoutCtx *ctx, HpeBox *child, float inner_w)
{
    HpeStyle *cs = &child->style;
    float w;
    if (cs->flex_basis.unit == HPE_LEN_AUTO)
        return flex_item_width(ctx, child, inner_w);
    w = resolve_len(ctx, cs, cs->flex_basis, inner_w);
    if (cs->box_sizing != HPE_BOX_SIZING_BORDER) {
        w += side_px(ctx, cs, cs->padding[3], inner_w) +
             side_px(ctx, cs, cs->padding[1], inner_w) +
             (cs->border_w[3] > 0.0f ? cs->border_w[3] : 0.0f) +
             (cs->border_w[1] > 0.0f ? cs->border_w[1] : 0.0f);
    }
    if (w < 0.0f)
        w = 0.0f;
    return w > inner_w ? inner_w : w;
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
        it->shrink = cs->flex_shrink;
        it->order = cs->order;
        it->basis = flex_item_basis(ctx, child, inner_w);
        it->w = it->basis;
        it->outer_main = it->w + it->m[1] + it->m[3];
    }
    flex_sort_items(items, n);
    *out_n = n;
    return items;
}

/* auto margins soak up the free space before justify-content runs */
static void flex_row_absorb_automargins(FlexItem *items, int n, float free_space, int autos)
{
    float share = free_space / (float)autos;
    int i;
    for (i = 0; i < n; i++) {
        /* the resolved value has to reach the item's own layout pass, which
         * resolves `auto` margins to zero on its own */
        if (items[i].auto_ml) {
            items[i].m[3] = share;
            items[i].box->style.margin[3] = hpe_len_px(share);
        }
        if (items[i].auto_mr) {
            items[i].m[1] = share;
            items[i].box->style.margin[1] = hpe_len_px(share);
        }
    }
}

/* negative free space is taken back in proportion to shrink × basis */
static void flex_row_apply_shrink(FlexItem *items, int n, float free_space, float shrink_sum)
{
    int i;
    for (i = 0; i < n; i++) {
        float scaled = items[i].shrink * items[i].basis;
        if (scaled > 0.0f) {
            items[i].w += free_space * scaled / shrink_sum;
            if (items[i].w < 0.0f)
                items[i].w = 0.0f;
        }
    }
}

static void flex_row_free_space(HpeStyle *st, FlexItem *items, int n, float free_space,
                                float grow_sum, float shrink_sum, int autos, float *out_lead,
                                float *out_extra)
{
    int i;
    *out_lead = 0.0f;
    *out_extra = 0.0f;
    if (free_space > 0.0f && autos > 0) {
        flex_row_absorb_automargins(items, n, free_space, autos);
        return;
    }
    if (free_space > 0.0f && grow_sum > 0.0f) {
        for (i = 0; i < n; i++) {
            if (items[i].grow > 0.0f)
                items[i].w += free_space * items[i].grow / grow_sum;
        }
        return;
    }
    if (free_space < 0.0f && shrink_sum > 0.0f) {
        flex_row_apply_shrink(items, n, free_space, shrink_sum);
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
 * @brief 把 item 序列切成若干 flex 行 / Split the item sequence into flex lines
 *
 * @param[in]     items flex item 数组 / Flex item array
 * @param[in]     n item 个数 / Number of items
 * @param[in]     limit 主轴可用尺寸 / Available main-axis size
 * @param[in]     gap item 间距 / Gap between items
 * @param[in]     wrap 是否允许换行 / Whether wrapping is allowed
 * @param[out]    out_nl 切出的行数 / Number of resulting lines
 *
 * @return 新分配的行数组，调用方负责释放 / Newly allocated line array owned by the caller
 *
 * @note `wrap` 为 0（nowrap）时全部 item 落在唯一一行 / With `wrap` 0 (nowrap) every item lands on a single line
 */
static FlexLine *flex_build_lines(const FlexItem *items, int n, float limit, float gap, int wrap,
                                  int *out_nl)
{
    FlexLine *lines = NULL;
    size_t cap = 0;
    int nl = 0, i = 0;

    if (!wrap)
        limit = 0.0f; /* no break: everything stays on line 0 */
    while (i < n) {
        int j = i;
        float sum = 0.0f;
        while (j < n) {
            float cand = (j == i) ? items[j].outer_main : sum + gap + items[j].outer_main;
            if (wrap && j > i && limit > 0.0f && cand > limit)
                break;
            sum = cand;
            j++;
        }
        lines = (FlexLine *)hpe_grow(lines, &cap, (size_t)nl + 1, sizeof(FlexLine));
        lines[nl].start = i;
        lines[nl].count = j - i;
        lines[nl].main = sum;
        lines[nl].cross = 0.0f;
        lines[nl].lead = 0.0f;
        lines[nl].extra = 0.0f;
        nl++;
        i = j;
    }
    *out_nl = nl;
    return lines;
}

/**
 * @brief 求一个 item 的首行基线相对其外边距盒顶部的距离 / Baseline offset of an item's first line from its margin-box top
 *
 * @param[in]     it flex item / Flex item
 *
 * @return 基线偏移；无文本行时回落为外边距盒高度 / The baseline offset, falling back to the margin-box height when the item has no text lines
 */
static float flex_item_baseline(const FlexItem *it)
{
    HpeBox *b = it->box;
    if (b->line_count > 0)
        return (b->lines[0].y + b->lines[0].baseline) - (b->border_box.y - it->m[0]);
    return it->outer_h;
}

/**
 * @brief 按交叉轴对齐规则平移单个 flex item / Translate one flex item according to its cross-axis alignment
 *
 * @param[in]     st 容器样式 / Container style
 * @param[in,out] it flex item / Flex item
 * @param[in]     cross 行交叉轴尺寸 / The line's cross size
 * @param[in]     baseline 行内最高基线 / The line's highest baseline
 *
 * @note 仅做整体平移，不触发二次排版 / It only translates the whole box, never re-laying it out
 */
static void flex_place_item(const HpeStyle *st, FlexItem *it, float cross, float baseline)
{
    int align = flex_effective_align(st, &it->box->style);
    float dy;
    if (align == HPE_ALIGN_ITEMS_BASELINE)
        dy = baseline - flex_item_baseline(it);
    else
        dy = flex_cross_offset(align, cross, it->outer_h);
    if (dy != 0.0f)
        box_shift(it->box, 0.0f, dy);
}

/**
 * @brief 求一行内所有基线对齐项的基准 / Resolve the baseline shared by a line's baseline-aligned items
 *
 * @param[in]     st 容器样式 / Container style
 * @param[in]     items flex item 数组 / Flex item array
 * @param[in]     line 当前行 / Current line
 *
 * @return 行内最高基线 / The highest baseline on the line
 */
static float flex_line_baseline(const HpeStyle *st, const FlexItem *items, const FlexLine *line)
{
    float max_baseline = 0.0f;
    int i;
    for (i = 0; i < line->count; i++) {
        const FlexItem *it = &items[line->start + i];
        if (flex_effective_align(st, &it->box->style) == HPE_ALIGN_ITEMS_BASELINE) {
            float bl = flex_item_baseline(it);
            if (bl > max_baseline)
                max_baseline = bl;
        }
    }
    return max_baseline;
}

/**
 * @brief 排版一个横向（row/row-reverse）flex 容器 / Lay out a horizontal (row / row-reverse) flex container
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
 * @note 两趟：先测量各行交叉轴尺寸，再落位并对齐 / Two passes: measure each line's cross size, then place and align
 */
/* With `align-items:stretch` an item without an explicit height fills the line
 * box, exactly like a block with `height:auto` filling its containing block. */
static void flex_row_stretch_line(HpeStyle *st, FlexLine *ln, FlexItem *items)
{
    int k;
    for (k = 0; k < ln->count; k++) {
        FlexItem *it = &items[ln->start + k];
        int align = flex_effective_align(st, &it->box->style);
        if (it->box->style.height.unit == HPE_LEN_AUTO && align == HPE_ALIGN_ITEMS_STRETCH) {
            float target = ln->cross - it->m[0] - it->m[2];
            if (target > it->box->border_box.h) {
                it->box->border_box.h = target;
                it->box->content_box.h = target;
                it->box->pad_box.h = target;
            }
            it->outer_h = ln->cross;
        }
    }
}

/* Pass 1 for one row line: settle the line's main sizes and justify offsets,
 * lay its items out once to learn the line's cross extent, then apply
 * `align-items: stretch` for items without an explicit height. */
static void flex_row_measure_line(LayoutCtx *ctx, HpeBox *box, FlexLine *ln, FlexItem *items,
                                  float inner_w, float inner_h, float own_h, int nl)
{
    HpeStyle *st = &box->style;
    int reverse = st->row_reverse;
    float grow_sum = 0.0f, shrink_sum = 0.0f, free_space;
    int autos = 0, k;
    float cursor;

    for (k = 0; k < ln->count; k++) {
        int id = ln->start + k;
        grow_sum += items[id].grow;
        shrink_sum += items[id].shrink * items[id].basis;
        autos += items[id].auto_ml + items[id].auto_mr;
    }
    free_space = inner_w - ln->main;
    flex_row_free_space(st, &items[ln->start], ln->count, free_space, grow_sum, shrink_sum, autos,
                        &ln->lead, &ln->extra);
    cursor = reverse ? (box->content_box.x + inner_w - ln->lead)
                     : (box->content_box.x + ln->lead);
    for (k = 0; k < ln->count; k++) {
        FlexItem *it = &items[ln->start + k];
        float x;
        it->outer_main = it->w + it->m[1] + it->m[3];
        if (reverse) {
            /* `-reverse` only flips where main-start sits; the items still run
             * in DOM order, so the first item hugs the right edge */
            cursor -= it->outer_main;
            x = cursor;
            cursor -= st->gap + ln->extra;
        } else {
            x = cursor;
            cursor += it->outer_main + st->gap + ln->extra;
        }
        layout_block(ctx, it->box, x, box->content_box.y, it->outer_main, inner_h);
        it->outer_h = it->m[0] + it->box->border_box.h + it->m[2];
        if (it->outer_h > ln->cross)
            ln->cross = it->outer_h;
    }
    /* a definite container height sets the line's cross size for a single line,
     * which is what lets `align-items:stretch` fill it */
    if (own_h > 0.0f && nl == 1 && own_h > ln->cross)
        ln->cross = own_h;
    flex_row_stretch_line(st, ln, items);
}

/* Pass 2: translate each line to its final cross offset and align its items. */
static void flex_row_place_lines(HpeBox *box, FlexLine *lines, int nl, FlexItem *items,
                                 int wrap_reverse)
{
    HpeStyle *st = &box->style;
    float cursor_y = box->content_box.y;
    int li, k;
    for (li = 0; li < nl; li++) {
        int idx = wrap_reverse ? (nl - 1 - li) : li;
        FlexLine *ln = &lines[idx];
        float base = box->content_box.y;
        float dyl = cursor_y - base;
        float bl = flex_line_baseline(st, items, ln);
        for (k = 0; k < ln->count; k++) {
            if (dyl != 0.0f)
                box_shift(items[ln->start + k].box, 0.0f, dyl);
            flex_place_item(st, &items[ln->start + k], ln->cross, bl);
        }
        cursor_y += ln->cross + st->gap;
    }
}

static float flex_layout_row(LayoutCtx *ctx, HpeBox *box, FlexItem *items, int n, float inner_w,
                             float inner_h, float own_h)
{
    HpeStyle *st = &box->style;
    FlexLine *lines;
    int nl = 0, li;
    float total_cross = 0.0f;

    lines = flex_build_lines(items, n, inner_w, st->gap, st->flex_wrap != 0, &nl);
    for (li = 0; li < nl; li++)
        flex_row_measure_line(ctx, box, &lines[li], items, inner_w, inner_h, own_h, nl);
    for (li = 0; li < nl; li++)
        total_cross += lines[li].cross;
    if (nl > 1)
        total_cross += st->gap * (float)(nl - 1);

    flex_row_place_lines(box, lines, nl, items, st->flex_wrap == 2);

    hpe_free(lines);
    return own_h > total_cross ? own_h : total_cross;
}

/**
 * @brief 排版一个纵向（column/column-reverse）flex 容器 / Lay out a vertical (column / column-reverse) flex container
 *
 * @param[in]     ctx 布局上下文 / Layout context
 * @param[in,out] box flex 容器盒 / Flex container box
 * @param[in,out] items flex item 数组 / Flex item array
 * @param[in]     n item 个数 / Number of items
 * @param[in]     inner_w 内容区宽度 / Inner width
 * @param[in]     inner_h 内容区高度基准 / Inner height basis
 * @param[in]     own_h 容器自身高度，未定为负 / Container's own height, negative when unset
 *
 * @return 容器的交叉轴（宽度、按列累计）尺寸 / The container's cross-axis extent
 *
 * @note 主轴为纵向，故先横向量出每个 item 的高度再按 `own_h` 分行 / The main axis is vertical, so each item is measured first and then split into lines against `own_h`
 */
/* A column's cross axis is horizontal: a `stretch` item without an explicit
 * width fills the container, and any item wider than the container is capped. */
static void flex_col_resolve_widths(HpeStyle *st, FlexItem *items, int n, float inner_w)
{
    int i;
    for (i = 0; i < n; i++) {
        FlexItem *it = &items[i];
        int align = flex_effective_align(st, &it->box->style);
        float avail = inner_w - it->m[1] - it->m[3];
        if (avail < 0.0f)
            avail = 0.0f;
        if (align == HPE_ALIGN_ITEMS_STRETCH && it->box->style.width.unit == HPE_LEN_AUTO)
            it->w = avail;
        else if (it->w > avail)
            it->w = avail;
    }
}

/* Measure each column item's main (height) size at its resolved width. */
static void flex_col_measure_items(LayoutCtx *ctx, HpeBox *box, FlexItem *items, int n,
                                   float inner_h)
{
    int i;
    for (i = 0; i < n; i++) {
        FlexItem *it = &items[i];
        layout_block(ctx, it->box, box->content_box.x + it->m[3], box->content_box.y,
                     it->w + it->m[1] + it->m[3], inner_h);
        it->outer_h = it->m[0] + it->box->border_box.h + it->m[2];
        it->outer_main = it->outer_h;
    }
}

/* On a column the main sizes can grow/shrink against a definite container
 * height. */
static void flex_col_flex_main(HpeStyle *st, FlexItem *items, int n, float own_h)
{
    float total = 0.0f, grow_sum = 0.0f, shrink_sum = 0.0f, free_space;
    int i;
    if (own_h <= 0.0f)
        return;
    for (i = 0; i < n; i++) {
        total += items[i].outer_main;
        grow_sum += items[i].grow;
        shrink_sum += items[i].shrink * items[i].basis;
    }
    if (n > 1)
        total += st->gap * (float)(n - 1);
    free_space = own_h - total;
    for (i = 0; i < n; i++) {
        FlexItem *it = &items[i];
        float delta = 0.0f;
        if (free_space > 0.0f && grow_sum > 0.0f && it->grow > 0.0f)
            delta = free_space * it->grow / grow_sum;
        else if (free_space < 0.0f && shrink_sum > 0.0f && it->shrink > 0.0f)
            delta = free_space * (it->shrink * it->basis) / shrink_sum;
        if (delta == 0.0f)
            continue;
        it->box->border_box.h += delta;
        it->box->content_box.h += delta;
        it->box->pad_box.h += delta;
        it->outer_h += delta;
        it->outer_main = it->outer_h;
    }
}

/* Size each line's cross extent: a definite container width sets a single line,
 * otherwise the widest item's outer width does. */
static void flex_col_line_cross(FlexLine *lines, int nl, FlexItem *items, float inner_w)
{
    int li;
    for (li = 0; li < nl; li++) {
        FlexLine *ln = &lines[li];
        float max_w = 0.0f;
        int k;
        if (inner_w > 0.0f && nl == 1)
            ln->cross = inner_w;
        for (k = 0; k < ln->count; k++) {
            FlexItem *it = &items[ln->start + k];
            float ow = it->w + it->m[1] + it->m[3];
            if (ow > max_w)
                max_w = ow;
        }
        if (max_w > ln->cross)
            ln->cross = max_w;
    }
}

/* Stretch the auto-width, stretch-aligned items of a column line to the line's
 * cross size before they are placed. */
static void flex_col_stretch_line(HpeStyle *st, FlexLine *ln, FlexItem *items)
{
    int k;
    for (k = 0; k < ln->count; k++) {
        FlexItem *it = &items[ln->start + k];
        if (it->box->style.align_self != HPE_ALIGN_ITEMS_AUTO &&
            st->align_items != HPE_ALIGN_ITEMS_STRETCH)
            continue;
        if (flex_effective_align(st, &it->box->style) == HPE_ALIGN_ITEMS_STRETCH &&
            it->box->style.width.unit == HPE_LEN_AUTO) {
            it->w = ln->cross - it->m[1] - it->m[3];
            if (it->w < 0.0f)
                it->w = 0.0f;
        }
    }
}

static float flex_layout_column(LayoutCtx *ctx, HpeBox *box, FlexItem *items, int n, float inner_w,
                                float inner_h, float own_h)
{
    HpeStyle *st = &box->style;
    FlexLine *lines;
    int nl = 0, i, k;
    float cross_total = 0.0f, cursor_x;
    int wrap_reverse = st->flex_wrap == 2;
    int reverse = st->row_reverse;

    flex_col_resolve_widths(st, items, n, inner_w);
    flex_col_measure_items(ctx, box, items, n, inner_h);
    flex_col_flex_main(st, items, n, own_h);

    lines = flex_build_lines(items, n, own_h > 0.0f ? own_h : 0.0f, st->gap, st->flex_wrap != 0,
                             &nl);
    flex_col_line_cross(lines, nl, items, inner_w);

    cursor_x = box->content_box.x;
    for (i = 0; i < nl; i++) {
        int idx = wrap_reverse ? (nl - 1 - i) : i;
        FlexLine *ln = &lines[idx];
        float cursor_y = reverse ? (box->content_box.y + (own_h > 0.0f ? own_h : ln->main))
                                 : box->content_box.y;
        flex_col_stretch_line(st, ln, items);
        for (k = 0; k < ln->count; k++) {
            FlexItem *it = &items[ln->start + k];
            int align = flex_effective_align(st, &it->box->style);
            float dx = flex_cross_offset(align, ln->cross, it->w + it->m[1] + it->m[3]);
            float y;
            if (reverse) {
                cursor_y -= it->outer_main;
                y = cursor_y;
                cursor_y -= st->gap;
            } else {
                y = cursor_y;
                cursor_y += it->outer_main + st->gap;
            }
            layout_block(ctx, it->box, cursor_x + dx, y, it->w + it->m[1] + it->m[3], inner_h);
            it->outer_h = it->m[0] + it->box->border_box.h + it->m[2];
        }
        cursor_x += ln->cross + st->gap;
        cross_total += ln->cross + st->gap;
    }
    if (cross_total > 0.0f)
        cross_total -= st->gap;

    hpe_free(lines);
    return cross_total;
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
        result = flex_layout_column(ctx, box, items, n, inner_w, inner_h, own_h);
    else
        result = flex_layout_row(ctx, box, items, n, inner_w, inner_h, own_h);
    if (items)
        hpe_free(items);
    return result;
}

/* ---------------- grid ----------------
 *
 * A grid container has an explicit track list for the columns (and optionally
 * the rows). Items are placed either explicitly (`grid-column` / `grid-row`)
 * or by the auto-placement cursor, which walks the grid in row-major order and
 * takes the next free slot wide enough for the item. Track sizing first gives
 * every `px` / `%` track its resolved size and every `auto` track the largest
 * preferred width of the items in it; the remaining space is then shared among
 * the `fr` tracks in proportion to their factors. Rows are sized from their
 * items' heights, so an implicit row (`grid-auto-rows`) simply wraps its
 * content. */

#define HPE_GRID_MAX_ITEMS 256
/* the row dimension has to be bounded separately: a grid may have far more
 * implicit rows than its column template has tracks */
#define HPE_GRID_MAX_ROWS 64

typedef struct {
    HpeBox *box;
    int col, row;         /* 0-based start cell */
    int colspan, rowspan; /* at least 1 */
} GridItem;

/**
 * @brief 取一个网格 item 的跨列/跨行数 / Resolve a grid item's column/row span
 *
 * @details 按 `grid-column` / `grid-row` 的「起始线 + 结束线」求跨数；任一端缺失时按 1 计。
 *          The span comes from the `grid-column` / `grid-row` start and end lines; a missing end means one track.
 *
 * @param[in] start 起始线（1 基线，0 = auto，负值 = `span N`）/ Start line, 1-based (0 = auto, negative = `span N`)
 * @param[in] end 结束线（同上）/ End line, same encoding
 *
 * @return 跨数（至少 1）/ The span, at least 1
 *
 * @note 负值把 `span N` 直接作为跨数 / A negative value is taken as the `span N` length
 */
static int grid_span_from_lines(int start, int end)
{
    if (end < 0)
        return -end;
    if (start < 0)
        return -start;
    if (start > 0 && end > start)
        return end - start;
    return 1;
}

/**
 * @brief 取一个网格 item 的起始线 / Resolve a grid item's start line
 *
 * @param[in] start 声明值 / Declared value
 *
 * @return 0 基线的起始格；`auto` 或 `span` 时返回 -1 表示「交给自动放置」/ The 0-based start cell, or -1 for "let auto-placement decide"
 */
static int grid_start_cell(int start)
{
    if (start <= 0)
        return -1;
    return start - 1;
}

/**
 * @brief 解算网格轨道尺寸 / Resolve the grid track sizes
 *
 * @details 第一遍给每个 `px` / `%` 轨道定值、`auto` 轨道取内容首选宽度；随后把
 *           `avail - 已用 - 间距` 按 `fr` 因子分配给弹性轨道。`fr` 轨道为 0 时退化为 `auto`。
 *          Pass one fixes the `px` / `%` tracks and gives every `auto` track its content's preferred width; the leftover after gaps is then distributed among the `fr` tracks by factor.
 *
 * @param[in,out] spec 轨道表 / Track list
 * @param[in]     avail 可用主轴尺寸 / Available main-axis size
 * @param[in]     gap 轨道间距 / Track gap
 * @param[in]     auto_hint 每个 `auto` 轨道的提示宽度（内容首选宽）/ Hint width for each `auto` track
 * @param[out]    out 每个轨道的最终尺寸 / Final size of every track
 *
 * @return 轨道总数 / The number of tracks
 */
static int grid_resolve_tracks(const HpeGridSpec *spec, float avail, float gap,
                               const float *auto_hint, float *out)
{
    float used = 0.0f, fr_sum = 0.0f;
    float gaps;
    int i, n = spec->count;

    for (i = 0; i < n; i++) {
        const HpeGridTrack *t = &spec->tracks[i];
        float w = 0.0f;
        if (t->kind == 0) /* px */
            w = t->value;
        else if (t->kind == 3) /* % */
            w = avail * t->value / 100.0f;
        else if (t->kind == 2) /* auto */
            w = auto_hint ? auto_hint[i] : 0.0f;
        else /* fr */
            fr_sum += t->value > 0.0f ? t->value : 1.0f;
        out[i] = w;
        used += w;
    }
    gaps = n > 1 ? gap * (float)(n - 1) : 0.0f;
    if (fr_sum > 0.0f) {
        float free_w = avail - used - gaps;
        if (free_w < 0.0f)
            free_w = 0.0f;
        for (i = 0; i < n; i++) {
            if (spec->tracks[i].kind == 1)
                out[i] = free_w * (spec->tracks[i].value > 0.0f ? spec->tracks[i].value : 1.0f) /
                         fr_sum;
        }
    }
    return n;
}

/**
 * @brief 布局一个网格容器 / Lay out a grid container
 *
 * @param[in]     ctx 布局上下文 / Layout context
 * @param[in,out] box 网格容器盒 / Grid container box
 * @param[in]     inner_w 内容区宽度 / Inner width
 * @param[in]     inner_h 内容区高度基准 / Inner height basis
 *
 * @return 网格内容区高度 / The grid's content height
 *
 * @note 未显式指定列时按「有多少 item 就排多少列」单行铺开 / With no explicit `grid-template-columns` the items are laid out in a single row
 */
/* Everything layout_grid accumulates across its passes. */
typedef struct {
    GridItem items[HPE_GRID_MAX_ITEMS];
    float col_w[HPE_GRID_MAX_TRACKS];
    float row_h[HPE_GRID_MAX_ROWS];
    float row_top[HPE_GRID_MAX_ROWS + 1];
    float auto_hint[HPE_GRID_MAX_TRACKS];
    int n, ncols, nrows;
    float gap, x0, y0, inner_w, inner_h;
} GridState;

/* Collect the in-flow block-level children as grid items. Returns 0 when there
 * is nothing to lay out. */
static int grid_collect_items(HpeBox *box, GridState *g)
{
    HpeBox *child;
    g->n = 0;
    for (child = box->first_child; child && g->n < HPE_GRID_MAX_ITEMS; child = child->next) {
        HpeStyle *cs = &child->style;
        GridItem *it;
        if (cs->position == HPE_POS_ABSOLUTE || cs->position == HPE_POS_FIXED)
            continue;
        /* only block-level children participate; stray inline/text nodes (the
         * whitespace between the tags) are not grid items */
        if (child->kind != HPE_BOX_BLOCK && child->kind != HPE_BOX_INLINE_BLOCK)
            continue;
        it = &g->items[g->n];
        it->box = child;
        it->col = -1;
        it->row = -1;
        it->colspan = grid_span_from_lines(cs->grid_col_start, cs->grid_col_end);
        it->rowspan = grid_span_from_lines(cs->grid_row_start, cs->grid_row_end);
        if (it->colspan > HPE_GRID_MAX_TRACKS)
            it->colspan = HPE_GRID_MAX_TRACKS;
        if (it->rowspan > HPE_GRID_MAX_TRACKS)
            it->rowspan = HPE_GRID_MAX_TRACKS;
        g->n++;
    }
    return g->n > 0;
}

/* Explicit column grid, or one column per item when no template is given. */
static void grid_resolve_ncols(HpeStyle *st, GridState *g)
{
    int i;
    if (st->grid_cols.present) {
        g->ncols = st->grid_cols.count;
        return;
    }
    g->ncols = 0;
    for (i = 0; i < g->n; i++)
        g->ncols += g->items[i].colspan;
    if (g->ncols > HPE_GRID_MAX_TRACKS)
        g->ncols = HPE_GRID_MAX_TRACKS;
    if (g->ncols < 1)
        g->ncols = 1;
}

/* Auto-placement: honour explicit positions, then fill the gaps.
 * `taken[r][c]` records occupancy; the row dimension is bounded by
 * HPE_GRID_MAX_ROWS so an over-long column template cannot overflow. Returns 0
 * when placement hit a wall (the caller then just stops adding rows). */
/* Mark the cells an item occupies in the occupancy grid. */
static void grid_mark_taken(unsigned char taken[][HPE_GRID_MAX_TRACKS], int col, int row,
                            int colspan, int rowspan)
{
    int r, c;
    for (r = row; r < row + rowspan && r < HPE_GRID_MAX_ROWS; r++)
        for (c = col; c < col + colspan && c < HPE_GRID_MAX_TRACKS; c++)
            taken[r][c] = 1;
}

/* One axis fixed, the other auto: scan that axis for a free run, advancing the
 * auto cursors, and report the chosen cell in *out_c / *out_r. On a no-room
 * break the item still lands on the last tried cell, as the reference does. */
/* Advance the auto cursor one step along the free axis. Returns 1 when the
 * scan has exhausted every axis and the caller should stop probing. */
static int grid_slide_cursor(const GridState *g, const GridItem *it, int col_flow, int flow_rows,
                             int cc, int rr, int *cursor_c, int *cursor_r)
{
    if (it->row >= 0) {
        /* row pinned: slide along the columns */
        if (cc + 1 + it->colspan > g->ncols)
            return 1;
        *cursor_c = cc + 1;
    } else if (it->col >= 0) {
        /* column pinned: slide down the rows */
        if (rr + 1 + it->rowspan > HPE_GRID_MAX_ROWS)
            return 1;
        *cursor_r = rr + 1;
    } else if (col_flow) {
        /* fully auto, column flow: walk down then wrap right */
        if (rr + 1 + it->rowspan > flow_rows) {
            *cursor_r = 0;
            if (cc + 1 + it->colspan > HPE_GRID_MAX_TRACKS)
                return 1;
            *cursor_c = cc + 1;
        } else {
            *cursor_r = rr + 1;
        }
    } else {
        /* fully auto: row-major walk */
        if (cc + 1 + it->colspan > g->ncols) {
            *cursor_c = 0;
            if (rr + 1 + it->rowspan > HPE_GRID_MAX_ROWS)
                return 1;
            *cursor_r = rr + 1;
        } else {
            *cursor_c = cc + 1;
        }
    }
    return 0;
}

static void grid_find_slot(const GridState *g, const GridItem *it,
                           unsigned char taken[][HPE_GRID_MAX_TRACKS], int col_flow, int flow_rows,
                           int *cursor_c, int *cursor_r, int *out_c, int *out_r)
{
    int cc, rr;
    for (;;) {
        int r, c, ok = 1;
        cc = it->col >= 0 ? it->col : *cursor_c;
        rr = it->row >= 0 ? it->row : *cursor_r;
        if (cc < 0)
            cc = 0;
        if (rr < 0)
            rr = 0;
        if (cc + it->colspan > g->ncols)
            ok = 0;
        if (rr + it->rowspan > HPE_GRID_MAX_ROWS)
            ok = 0;
        if (ok) {
            for (r = rr; r < rr + it->rowspan; r++)
                for (c = cc; c < cc + it->colspan; c++)
                    if (taken[r][c])
                        ok = 0;
        }
        if (ok)
            break;
        if (grid_slide_cursor(g, it, col_flow, flow_rows, cc, rr, cursor_c, cursor_r))
            break;
    }
    *out_c = cc;
    *out_r = rr;
}

/* Advance the auto cursor past the item just placed. */
static void grid_advance_cursor(const GridState *g, const GridItem *it, int col_flow, int flow_rows,
                                int cc, int rr, int *cursor_c, int *cursor_r)
{
    if (col_flow) {
        *cursor_r = rr + it->rowspan;
        *cursor_c = cc;
        if (*cursor_r >= flow_rows) {
            *cursor_r = 0;
            *cursor_c = cc + it->colspan;
        }
    } else {
        *cursor_c = cc + it->colspan;
        if (*cursor_c >= g->ncols) {
            *cursor_c = 0;
            *cursor_r = rr + it->rowspan;
        } else {
            *cursor_r = rr;
        }
    }
}

static int grid_autoplace(GridState *g, HpeStyle *st)
{
    unsigned char taken[HPE_GRID_MAX_ROWS][HPE_GRID_MAX_TRACKS];
    int cursor_c = 0, cursor_r = 0;
    /* `grid-auto-flow: column` walks down a column before moving right, so the
     * row count comes from the row template (an implicit row is added only when
     * no template is given). */
    int col_flow = st->grid_auto_flow == 1;
    int flow_rows = st->grid_rows.present ? st->grid_rows.count : 1;
    int i;
    if (flow_rows < 1)
        flow_rows = 1;
    if (flow_rows > HPE_GRID_MAX_ROWS)
        flow_rows = HPE_GRID_MAX_ROWS;
    memset(taken, 0, sizeof(taken));
    for (i = 0; i < g->n; i++) {
        HpeStyle *cs = &g->items[i].box->style;
        int sc = grid_start_cell(cs->grid_col_start);
        int sr = grid_start_cell(cs->grid_row_start);
        int cc, rr;
        if (sc >= 0)
            g->items[i].col = sc;
        if (sr >= 0)
            g->items[i].row = sr;
        /* an explicit position on both axes needs no scanning */
        if (g->items[i].col >= 0 && g->items[i].row >= 0) {
            grid_mark_taken(taken, g->items[i].col, g->items[i].row, g->items[i].colspan,
                            g->items[i].rowspan);
            continue;
        }
        grid_find_slot(g, &g->items[i], taken, col_flow, flow_rows, &cursor_c, &cursor_r, &cc, &rr);
        g->items[i].col = cc;
        g->items[i].row = rr;
        grid_mark_taken(taken, cc, rr, g->items[i].colspan, g->items[i].rowspan);
        grid_advance_cursor(g, &g->items[i], col_flow, flow_rows, cc, rr, &cursor_c, &cursor_r);
    }
    return 1;
}

/* Row track sizes from the template; `auto` / `fr` rows start at 0 and grow to
 * fit their items later. nrows is settled from the placed items. */
static void grid_row_tracks(HpeStyle *st, GridState *g)
{
    int i, r;
    g->nrows = 1;
    for (i = 0; i < g->n; i++) {
        int end = g->items[i].row + g->items[i].rowspan;
        if (end > g->nrows)
            g->nrows = end;
    }
    if (g->nrows > HPE_GRID_MAX_ROWS)
        g->nrows = HPE_GRID_MAX_ROWS;
    memset(g->row_h, 0, sizeof(g->row_h));
    if (st->grid_rows.present) {
        int given = st->grid_rows.count < g->nrows ? st->grid_rows.count : g->nrows;
        for (r = 0; r < given; r++) {
            const HpeGridTrack *t = &st->grid_rows.tracks[r];
            if (t->kind == 0)
                g->row_h[r] = t->value;
            else if (t->kind == 3)
                g->row_h[r] = g->inner_h * t->value / 100.0f;
        }
    }
}

/* Hint each auto column with its single-column items' preferred width, then
 * resolve the column tracks. */
static void grid_col_tracks(LayoutCtx *ctx, HpeStyle *st, GridState *g)
{
    int i, c;
    for (c = 0; c < g->ncols; c++)
        g->auto_hint[c] = 0.0f;
    for (i = 0; i < g->n; i++) {
        float pw;
        if (g->items[i].col < 0 || g->items[i].colspan != 1)
            continue;
        pw = table_cell_pref_w(ctx, g->items[i].box);
        if (g->items[i].col < HPE_GRID_MAX_TRACKS && pw > g->auto_hint[g->items[i].col])
            g->auto_hint[g->items[i].col] = pw;
    }
    if (st->grid_cols.present) {
        grid_resolve_tracks(&st->grid_cols, g->inner_w, g->gap, g->auto_hint, g->col_w);
    } else {
        /* no explicit template: share the width equally between the columns */
        float share =
            (g->inner_w - (g->ncols > 1 ? g->gap * (float)(g->ncols - 1) : 0.0f)) / (float)g->ncols;
        for (c = 0; c < g->ncols; c++)
            g->col_w[c] = share > 0.0f ? share : 0.0f;
    }
    if (st->grid_cols.present && g->ncols > st->grid_cols.count) {
        /* implicit columns beyond the template get the leftover share */
        float extra = g->inner_w / (float)g->ncols;
        for (c = st->grid_cols.count; c < g->ncols; c++)
            g->col_w[c] = extra > 0.0f ? extra : 0.0f;
    }
}

/* Row track sizes: every row gets the tallest single-row item in it, then a
 * spanning item pushes its rows when it needs more room. */
static void grid_measure_rows(LayoutCtx *ctx, GridState *g)
{
    int i, r, k;
    for (r = 0; r < g->nrows; r++) {
        for (i = 0; i < g->n; i++) {
            float cw = 0.0f, h;
            if (g->items[i].row != r || g->items[i].rowspan != 1)
                continue;
            for (k = 0; k < g->items[i].colspan && g->items[i].col + k < g->ncols; k++)
                cw += g->col_w[g->items[i].col + k] + (k ? g->gap : 0.0f);
            layout_block(ctx, g->items[i].box, g->x0, g->y0, cw > 0.0f ? cw : g->inner_w,
                         g->inner_h);
            h = g->items[i].box->border_box.h;
            if (h > g->row_h[r])
                g->row_h[r] = h;
        }
    }
    for (i = 0; i < g->n; i++) {
        float cw = 0.0f, have = 0.0f, need;
        int last = g->items[i].row + g->items[i].rowspan - 1;
        if (g->items[i].rowspan <= 1)
            continue;
        if (last >= g->nrows)
            last = g->nrows - 1;
        for (k = 0; k < g->items[i].colspan && g->items[i].col + k < g->ncols; k++)
            cw += g->col_w[g->items[i].col + k] + (k ? g->gap : 0.0f);
        layout_block(ctx, g->items[i].box, g->x0, g->y0, cw > 0.0f ? cw : g->inner_w, g->inner_h);
        need = g->items[i].box->border_box.h;
        for (k = g->items[i].row; k <= last; k++)
            have += g->row_h[k] + (k > g->items[i].row ? g->gap : 0.0f);
        if (need > have) {
            float add = (need - have) / (float)(last - g->items[i].row + 1);
            for (k = g->items[i].row; k <= last; k++)
                g->row_h[k] += add;
        }
    }
}

/* align-content: compute the row offsets, then shift / spread the whole grid
 * inside the container. */
static void grid_align_content(HpeStyle *st, GridState *g)
{
    float total_h, extra;
    int r;
    g->row_top[0] = g->y0;
    for (r = 1; r < g->nrows; r++)
        g->row_top[r] = g->row_top[r - 1] + g->row_h[r - 1] + g->gap;
    total_h = g->row_top[g->nrows - 1] + g->row_h[g->nrows - 1] - g->y0;
    extra = g->inner_h - total_h;
    if (extra <= 0.0f)
        return;
    if (st->align_content == HPE_JUSTIFY_CENTER) {
        for (r = 0; r < g->nrows; r++)
            g->row_top[r] += extra * 0.5f;
    } else if (st->align_content == HPE_JUSTIFY_END) {
        for (r = 0; r < g->nrows; r++)
            g->row_top[r] += extra;
    } else if (st->align_content == HPE_JUSTIFY_SPACE_BETWEEN && g->nrows > 1) {
        float add = extra / (float)(g->nrows - 1);
        for (r = 0; r < g->nrows; r++)
            g->row_top[r] += add * (float)r;
    } else if (st->align_content == HPE_JUSTIFY_SPACE_AROUND && g->nrows > 0) {
        float unit = extra / (float)g->nrows;
        for (r = 0; r < g->nrows; r++)
            g->row_top[r] += unit * ((float)r + 0.5f);
    }
}

/* Place pass: layout each item in its cell, then apply justify-items
 * (stretch by default). */
static void grid_place_items(LayoutCtx *ctx, HpeStyle *st, GridState *g)
{
    float col_x[HPE_GRID_MAX_TRACKS];
    float x = g->x0;
    int i, c;
    for (c = 0; c < g->ncols; c++) {
        col_x[c] = x;
        x += g->col_w[c] + g->gap;
    }
    for (i = 0; i < g->n; i++) {
        float cx, cw = 0.0f, ch;
        int k, last = g->items[i].row + g->items[i].rowspan - 1;
        if (g->items[i].col + g->items[i].colspan > g->ncols)
            g->items[i].colspan = g->ncols - g->items[i].col;
        if (g->items[i].colspan < 1)
            g->items[i].colspan = 1;
        if (last >= g->nrows)
            last = g->nrows - 1;
        for (k = 0; k < g->items[i].colspan; k++)
            cw += g->col_w[g->items[i].col + k] + (k ? g->gap : 0.0f);
        cx = col_x[g->items[i].col];
        ch = g->row_top[last] + g->row_h[last] - g->row_top[g->items[i].row];
        if (ch < 0.0f)
            ch = 0.0f;
        layout_block(ctx, g->items[i].box, cx, g->row_top[g->items[i].row], cw,
                     ch > 0.0f ? ch : g->inner_h);
        /* justify-items / align-items: stretch by default */
        if (g->items[i].box->style.width.unit == HPE_LEN_AUTO &&
            st->justify_items != HPE_ALIGN_ITEMS_CENTER &&
            st->justify_items != HPE_ALIGN_ITEMS_FLEX_END) {
            if (cw > g->items[i].box->border_box.w) {
                float grow = cw - g->items[i].box->border_box.w;
                g->items[i].box->content_box.w += grow;
                g->items[i].box->pad_box.w += grow;
                g->items[i].box->border_box.w += grow;
            }
        } else if (st->justify_items == HPE_ALIGN_ITEMS_CENTER) {
            float dx = (cw - g->items[i].box->border_box.w) * 0.5f;
            g->items[i].box->content_box.x += dx;
            g->items[i].box->pad_box.x += dx;
            g->items[i].box->border_box.x += dx;
        } else if (st->justify_items == HPE_ALIGN_ITEMS_FLEX_END) {
            float dx = cw - g->items[i].box->border_box.w;
            g->items[i].box->content_box.x += dx;
            g->items[i].box->pad_box.x += dx;
            g->items[i].box->border_box.x += dx;
        }
    }
}

/**
 * @brief 布局一个网格容器 / Lay out a grid container
 *
 * @param[in]     ctx 布局上下文 / Layout context
 * @param[in,out] box 网格容器盒 / Grid container box
 * @param[in]     inner_w 内容区宽度 / Inner width
 * @param[in]     inner_h 内容区高度基准 / Inner height basis
 *
 * @return 网格内容区高度 / The grid's content height
 *
 * @note 未显式指定列时按「有多少 item 就排多少列」单行铺开 / With no explicit `grid-template-columns` the items are laid out in a single row
 */
static float layout_grid(LayoutCtx *ctx, HpeBox *box, float inner_w, float inner_h)
{
    HpeStyle *st = &box->style;
    GridState g;

    memset(&g, 0, sizeof(g));
    g.gap = st->grid_gap;
    g.x0 = box->content_box.x;
    g.y0 = box->content_box.y;
    g.inner_w = inner_w;
    g.inner_h = inner_h;

    if (!grid_collect_items(box, &g))
        return 0.0f;
    grid_resolve_ncols(st, &g);
    grid_autoplace(&g, st);
    grid_row_tracks(st, &g);
    grid_col_tracks(ctx, st, &g);
    grid_measure_rows(ctx, &g);
    grid_align_content(st, &g);
    grid_place_items(ctx, st, &g);
    return g.row_top[g.nrows - 1] + g.row_h[g.nrows - 1] - g.y0;
}

/* ---------------- tables ----------------
 *
 * A table is laid out in three steps: collect the rows (descending through
 * row groups), resolve the column widths from the cells' preferred widths, and
 * finally place every cell in its column so the table can size its rows. The
 * automatic table layout algorithm is approximated by the classic "maximum of
 * the columns' preferred widths, then distribute the leftover proportionally"
 * rule, which reproduces the reference rendering for content-shaped tables.
 * `border-collapse` is not modelled: cells keep their own borders and the
 * table's `border-spacing` (default 0) separates them. */

#define HPE_TABLE_MAX_COLS 32

typedef struct {
    HpeBox **rows;
    int count;
    size_t cap;
} TableRows;

typedef struct {
    float *w;
    int count;
} TableCols;

/* one occupied cell of the grid: the box, its column span and the row index
 * it starts at (used to carry a rowspan's height forward). */
typedef struct {
    HpeBox *box;
    int col;
    int colspan;
    int rowspan;
} TableSlot;

/* the resolved grid geometry for one row: where each cell starts and which
 * columns it covers; `n` counts the slots actually present in the row. */
typedef struct {
    TableSlot slots[HPE_TABLE_MAX_COLS];
    int n;
} TableRowSlots;

/**
 * @brief 收集表格中的行盒（穿过行组）/ Collect a table's row boxes, descending through row groups
 *
 * @param[in]     box 表格或行组盒 / Table or row-group box
 * @param[in,out] out 行盒列表 / Row-box list
 *
 * @note 只收集 `table-row`，行组递归其子 / Only `table-row` boxes are collected while row groups recurse into their children
 */
static void table_collect_rows(HpeBox *box, TableRows *out)
{
    HpeBox *child;
    for (child = box->first_child; child; child = child->next) {
        HpeDisplay d = child->style.display;
        if (d == HPE_DISPLAY_TABLE_ROW) {
            out->rows = (HpeBox **)hpe_grow(out->rows, &out->cap, (size_t)out->count + 1,
                                             sizeof(HpeBox *));
            out->rows[out->count++] = child;
        } else if (d == HPE_DISPLAY_TABLE_ROW_GROUP) {
            table_collect_rows(child, out);
        }
    }
}

/**
 * @brief 收集一行的单元格盒 / Collect the cell boxes of one row
 *
 * @param[in]     row 行盒 / Row box
 * @param[out]    out 单元格数组 / Cell array
 * @param[in]     max 数组容量 / Array capacity
 *
 * @return 单元格个数（不超过 `max`）/ Number of cells, capped at `max`
 */
static int table_row_cells(HpeBox *row, HpeBox **out, int max)
{
    HpeBox *c;
    int n = 0;
    for (c = row->first_child; c && n < max; c = c->next) {
        if (c->style.display == HPE_DISPLAY_TABLE_CELL)
            out[n++] = c;
    }
    return n;
}

/**
 * @brief 读取单元格的正整数属性 / Read a positive integer cell attribute
 *
 * @param[in] cell 单元格盒 / Cell box
 * @param[in] name 属性名 / Attribute name
 *
 * @return 属性值（至少 1）；缺失或非法时返回 1 / The value, clamped to at least 1; 1 when missing or malformed
 */
static int table_attr_span(HpeBox *cell, const char *name)
{
    const char *v = cell->node ? hpe_dom_attr(cell->node, name) : NULL;
    int n = 0;
    if (!v)
        return 1;
    while (*v >= '0' && *v <= '9') {
        n = n * 10 + (*v - '0');
        v++;
    }
    if (n < 1)
        n = 1;
    return n;
}

/**
 * @brief 把一行的单元格放进网格槽位 / Place a row's cells into grid slots
 *
 * @details 依次按 `colspan` 宽度寻找下一个未被占用的列起点；`rowspan` 只记录，
 *           其高度由被跨越行分摊（本实现按「起始行独自撑开」近似）。
 *          Each cell takes the next free column run as wide as its `colspan`;
 *          `rowspan` is recorded and charged to the starting row.
 *
 * @param[in]     row 行盒 / Row box
 * @param[in,out] out 槽位结果 / Slot result
 * @param[in]     covered 该行已被上方 `rowspan` 占用的列掩码 / Columns already taken by a rowspan from above
 *
 * @note 超过 `HPE_TABLE_MAX_COLS` 的单元被丢弃 / Cells beyond `HPE_TABLE_MAX_COLS` are dropped
 */
static void table_slots_for_row(HpeBox *row, TableRowSlots *out, const unsigned char *covered)
{
    HpeBox *cells[HPE_TABLE_MAX_COLS];
    unsigned char used[HPE_TABLE_MAX_COLS];
    int nc = table_row_cells(row, cells, HPE_TABLE_MAX_COLS);
    int c, k;
    memset(used, 0, sizeof(used));
    /* seed with the columns a rowspan from above already occupies */
    for (k = 0; k < HPE_TABLE_MAX_COLS; k++)
        used[k] = covered[k] ? 1 : 0;
    out->n = 0;
    for (c = 0; c < nc && out->n < HPE_TABLE_MAX_COLS; c++) {
        TableSlot *s = &out->slots[out->n];
        int col = 0;
        int cs = table_attr_span(cells[c], "colspan");
        if (cs < 1)
            cs = 1;
        if (cs > HPE_TABLE_MAX_COLS)
            cs = HPE_TABLE_MAX_COLS;
        /* the first column run wide enough for this cell's span */
        while (col + cs <= HPE_TABLE_MAX_COLS) {
            int free_run = 1;
            for (k = 0; k < cs; k++)
                if (used[col + k])
                    free_run = 0;
            if (free_run)
                break;
            col++;
        }
        if (col + cs > HPE_TABLE_MAX_COLS)
            col = HPE_TABLE_MAX_COLS - cs;
        for (k = 0; k < cs; k++)
            used[col + k] = 1;
        s->box = cells[c];
        s->col = col;
        s->colspan = cs;
        s->rowspan = table_attr_span(cells[c], "rowspan");
        out->n++;
    }
}

/**
 * @brief 求一个单元格的首选（最大内容）宽度 / Resolve a cell's preferred (max-content) width
 *
 * @param[in]     ctx 布局上下文 / Layout context
 * @param[in,out] cell 单元格盒 / Cell box
 *
 * @return 边框盒首选宽度 / Preferred border-box width
 *
 * @note 显式宽度按 box-sizing 折算内边距与边框 / An explicit width accounts for padding and border per `box-sizing`
 */
static float table_cell_pref_w(LayoutCtx *ctx, HpeBox *cell)
{
    HpeStyle *cs = &cell->style;
    float extra = side_px(ctx, cs, cs->padding[3], ctx->vw) +
                  side_px(ctx, cs, cs->padding[1], ctx->vw) +
                  (cs->border_w[3] > 0.0f ? cs->border_w[3] : 0.0f) +
                  (cs->border_w[1] > 0.0f ? cs->border_w[1] : 0.0f);
    if (cs->width.unit != HPE_LEN_AUTO) {
        float w = resolve_len(ctx, cs, cs->width, ctx->vw);
        if (cs->box_sizing != HPE_BOX_SIZING_BORDER)
            w += extra;
        return w;
    }
    return measure_preferred_w(ctx, cell);
}

/**
 * @brief 解算表格的列宽 / Resolve a table's column widths
 *
 * @param[in]     ctx 布局上下文 / Layout context
 * @param[in,out] cols 列宽数组 / Column-width array
 * @param[in]     ncols 列数 / Number of columns
 * @param[in]     avail 表格内容区可用宽度 / Available content width of the table
 * @param[in]     spacing 列间距（`border-spacing`）/ Column spacing (`border-spacing`)
 *
 * @note 先取各列所有单元格首选宽度的最大值，再把剩余（或不足）空间按比例分配 / Each column first takes the maximum of its cells' preferred widths, after which the leftover, or the shortfall, is spread proportionally
 */
static void table_resolve_cols(LayoutCtx *ctx, TableCols *cols, int ncols, float avail,
                               float spacing)
{
    float total = 0.0f;
    int i;
    float gaps = ncols > 1 ? spacing * (float)(ncols - 1) : 0.0f;

    for (i = 0; i < ncols; i++) {
        if (cols->w[i] < 0.0f)
            cols->w[i] = 0.0f;
        total += cols->w[i];
    }
    if (ncols == 0)
        return;
    if (total + gaps <= 0.0f) {
        /* no intrinsic widths at all: share the space equally */
        float share = (avail - gaps) / (float)ncols;
        for (i = 0; i < ncols; i++)
            cols->w[i] = share > 0.0f ? share : 0.0f;
        return;
    }
    if (total + gaps < avail) {
        float extra = (avail - gaps - total) / total;
        for (i = 0; i < ncols; i++)
            cols->w[i] += cols->w[i] * extra;
    } else if (total + gaps > avail) {
        float scale = (avail - gaps) / total;
        if (scale < 0.0f)
            scale = 0.0f;
        for (i = 0; i < ncols; i++)
            cols->w[i] *= scale;
    }
    (void)ctx;
}

/**
 * @brief 求一个网格槽位的总宽度 / Resolve the total width of a grid slot
 *
 * @param[in] cols 已解算的列宽 / Resolved column widths
 * @param[in] spacing 列间距 / Column spacing
 * @param[in] ncols 列数 / Column count
 * @param[in] s 槽位 / Slot
 *
 * @return 该槽位跨越的列宽之和（含内部间距）/ Sum of the spanned columns plus the inner spacing
 */
static float table_slots_width(const TableCols *cols, float spacing, int ncols,
                               const TableSlot *s)
{
    float w = 0.0f;
    int k;
    for (k = 0; k < s->colspan && s->col + k < ncols; k++)
        w += cols->w[s->col + k] + (k ? spacing : 0.0f);
    return w;
}

/**
 * @brief 布局一个表格根盒 / Lay out a table root box
 *
 * @param[in]     ctx 布局上下文 / Layout context
 * @param[in,out] box 表格盒 / Table box
 * @param[in]     inner_w 内容区宽度 / Inner width
 * @param[in]     inner_h 内容区高度基准 / Inner height basis
 *
 * @return 表格内容区高度 / The table's content height
 *
 * @note 单元格宽度按列统一，行高取该行单元格的最大外高 / Cell widths are unified per column and a row's height is the maximum outer height of its cells
 */
/* Everything the table passes share. */
typedef struct {
    HpeBox *box;
    float inner_w, inner_h;
    float spacing, spacing_v;
    TableRows rows;
    TableRowSlots *slots;
    TableCols cols;
    int ncols;
} TableCtx;

/* Pass 1: place every cell in a grid slot and derive the column count.
 * Returns 0 when the table has no rows or no cells. */
static int table_place_slots(TableCtx *t)
{
    int r, i;
    t->ncols = 0;
    t->slots = (TableRowSlots *)hpe_calloc((size_t)t->rows.count, sizeof(TableRowSlots));
    for (r = 0; r < t->rows.count; r++) {
        unsigned char covered[HPE_TABLE_MAX_COLS];
        int k;
        memset(covered, 0, sizeof(covered));
        /* mark the columns spanned by rowspans started on earlier rows */
        for (i = 0; i < r; i++) {
            int s;
            for (s = 0; s < t->slots[i].n; s++) {
                TableSlot *p = &t->slots[i].slots[s];
                if (p->rowspan > r - i) {
                    for (k = p->col; k < p->col + p->colspan && k < HPE_TABLE_MAX_COLS; k++)
                        covered[k] = 1;
                }
            }
        }
        table_slots_for_row(t->rows.rows[r], &t->slots[r], covered);
        for (i = 0; i < t->slots[r].n; i++) {
            int end = t->slots[r].slots[i].col + t->slots[r].slots[i].colspan;
            if (end > t->ncols)
                t->ncols = end;
        }
    }
    if (t->ncols == 0)
        return 0;
    if (t->ncols > HPE_TABLE_MAX_COLS)
        t->ncols = HPE_TABLE_MAX_COLS;
    return 1;
}

/* Pass 2: per-column preferred width, splitting a spanning cell.
 * A cell that spans n columns contributes its preferred width divided by n
 * to each of those columns, which is what the automatic algorithm does
 * when distributing a spanning cell's minimum. */
static void table_col_pref_widths(LayoutCtx *ctx, TableCtx *t)
{
    int r, i, c;
    t->cols.w = (float *)hpe_calloc((size_t)t->ncols, sizeof(float));
    t->cols.count = t->ncols;
    for (c = 0; c < t->ncols; c++)
        t->cols.w[c] = -1.0f;
    for (r = 0; r < t->rows.count; r++) {
        for (i = 0; i < t->slots[r].n; i++) {
            TableSlot *s = &t->slots[r].slots[i];
            float pw = table_cell_pref_w(ctx, s->box) / (float)s->colspan;
            int k;
            for (k = 0; k < s->colspan && s->col + k < t->ncols; k++)
                if (pw > t->cols.w[s->col + k])
                    t->cols.w[s->col + k] = pw;
        }
    }
    /* a span's total still wins over its parts' individual maxima */
    for (r = 0; r < t->rows.count; r++) {
        for (i = 0; i < t->slots[r].n; i++) {
            TableSlot *s = &t->slots[r].slots[i];
            float need, have = 0.0f;
            int k;
            if (s->colspan <= 1)
                continue;
            for (k = 0; k < s->colspan && s->col + k < t->ncols; k++)
                have += t->cols.w[s->col + k] + (k ? t->spacing : 0.0f);
            need = table_cell_pref_w(ctx, s->box);
            if (need > have) {
                float add = (need - have) / (float)s->colspan;
                for (k = 0; k < s->colspan && s->col + k < t->ncols; k++)
                    t->cols.w[s->col + k] += add;
            }
        }
    }
    table_resolve_cols(ctx, &t->cols, t->ncols, t->inner_w, t->spacing);
}

/* Row heights: measure each cell at its final column width; layout_block with a
 * provisional y = 0 gives the natural height without committing the geometry,
 * so the row height is known before final placement. A spanning cell then
 * expands the rows it covers when its own height exceeds their combined band. */
static void table_measure_rows(LayoutCtx *ctx, TableCtx *t, float *row_h)
{
    int r, i;
    for (r = 0; r < t->rows.count; r++) {
        for (i = 0; i < t->slots[r].n; i++) {
            TableSlot *s = &t->slots[r].slots[i];
            float cw = table_slots_width(&t->cols, t->spacing, t->ncols, s);
            if (s->rowspan <= 1) {
                layout_block(ctx, s->box, 0.0f, 0.0f, cw, t->inner_h);
                if (s->box->border_box.h > row_h[r])
                    row_h[r] = s->box->border_box.h;
            }
        }
    }
    for (r = 0; r < t->rows.count; r++) {
        for (i = 0; i < t->slots[r].n; i++) {
            TableSlot *s = &t->slots[r].slots[i];
            float cw, have = 0.0f, need;
            int k, last = r + s->rowspan - 1;
            if (s->rowspan <= 1)
                continue;
            if (last >= t->rows.count)
                last = t->rows.count - 1;
            cw = table_slots_width(&t->cols, t->spacing, t->ncols, s);
            layout_block(ctx, s->box, 0.0f, 0.0f, cw, t->inner_h);
            need = s->box->border_box.h;
            for (k = r; k <= last; k++)
                have += row_h[k] + (k > r ? t->spacing_v : 0.0f);
            if (need > have) {
                float add = (need - have) / (float)(last - r + 1);
                for (k = r; k <= last; k++)
                    row_h[k] += add;
            }
        }
    }
}

/* Pass 3: final placement. A cell always fills the column band it occupies,
 * exactly as a browser does, and a vertically stretched cell fills its row band
 * so backgrounds and borders tile without gaps. */
static void table_place_cells(LayoutCtx *ctx, TableCtx *t, const float *row_h, const float *row_top)
{
    int r, c, i;
    for (r = 0; r < t->rows.count; r++) {
        for (i = 0; i < t->slots[r].n; i++) {
            TableSlot *s = &t->slots[r].slots[i];
            float cx = t->box->content_box.x;
            float cw = table_slots_width(&t->cols, t->spacing, t->ncols, s);
            int last = r + s->rowspan - 1;
            float ch;
            for (c = 0; c < s->col; c++)
                cx += t->cols.w[c] + t->spacing;
            if (last >= t->rows.count)
                last = t->rows.count - 1;
            ch = row_top[last] + row_h[last] - row_top[r];
            layout_block(ctx, s->box, cx, row_top[r], cw, ch > 0.0f ? ch : t->inner_h);
            if (cw > s->box->border_box.w) {
                float grow = cw - s->box->border_box.w;
                s->box->content_box.w += grow;
                s->box->pad_box.w += grow;
                s->box->border_box.w += grow;
            }
            if (s->box->style.height.unit == HPE_LEN_AUTO && ch > 0.0f) {
                float got = s->box->border_box.h;
                if (ch > got) {
                    float grow = ch - got;
                    s->box->content_box.h += grow;
                    s->box->pad_box.h += grow;
                    s->box->border_box.h += grow;
                }
            }
        }
    }
}

/* Lay the caption and every row out, returning the content bottom. */
static float table_layout_rows(LayoutCtx *ctx, TableCtx *t, HpeBox *caption)
{
    float y = t->box->content_box.y;
    float *row_h = (float *)hpe_calloc((size_t)t->rows.count, sizeof(float));
    float *row_top = (float *)hpe_calloc((size_t)t->rows.count + 1, sizeof(float));
    int r;
    if (caption) {
        layout_block(ctx, caption, t->box->content_box.x, y, t->inner_w, t->inner_h);
        y += caption->border_box.h;
    }
    table_measure_rows(ctx, t, row_h);
    /* row top edges: the first row sits at the content top, later rows are
     * separated by `border-spacing` (the vertical component) */
    row_top[0] = y;
    for (r = 1; r < t->rows.count; r++)
        row_top[r] = row_top[r - 1] + row_h[r - 1] + t->spacing_v;
    table_place_cells(ctx, t, row_h, row_top);
    y = row_top[t->rows.count - 1] + row_h[t->rows.count - 1];
    hpe_free(row_top);
    hpe_free(row_h);
    return y;
}

static float layout_table(LayoutCtx *ctx, HpeBox *box, float inner_w, float inner_h)
{
    TableCtx t;
    HpeBox *caption = NULL;
    float y;

    memset(&t, 0, sizeof(t));
    t.box = box;
    t.inner_w = inner_w;
    t.inner_h = inner_h;
    t.spacing = box->style.border_spacing_h;
    t.spacing_v = box->style.border_spacing_v;

    /* a caption is laid out above the rows and spans the whole table */
    for (caption = box->first_child; caption; caption = caption->next)
        if (caption->style.display == HPE_DISPLAY_TABLE_CAPTION)
            break;

    table_collect_rows(box, &t.rows);
    if (t.rows.count == 0) {
        if (t.rows.rows)
            hpe_free(t.rows.rows);
        return 0.0f;
    }
    if (!table_place_slots(&t)) {
        hpe_free(t.slots);
        hpe_free(t.rows.rows);
        return 0.0f;
    }
    table_col_pref_widths(ctx, &t);
    y = table_layout_rows(ctx, &t, caption);
    {
        float h = y - box->content_box.y;
        if (h < 0.0f)
            h = 0.0f;
        hpe_free(t.cols.w);
        hpe_free(t.slots);
        hpe_free(t.rows.rows);
        return h;
    }
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
    /* a block-level child honours `clear`: drop its margin box below every
     * same-side float recorded in this container's band */
    if (child->style.clear_side != HPE_CLEAR_NONE) {
        float clear_y = float_clear_offset(&box->floats, (int)child->style.clear_side, ypass);
        if (clear_y > ypass)
            ypass = clear_y;
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
/* Non-block-flow containers (flex / table / grid) hand their inner layout to the
 * dedicated routine. Returns 1 when the display was handled here. */
static int block_flow_special_display(LayoutCtx *ctx, HpeBox *box, float inner_w,
                                      float inner_h_base, int *out_had_block,
                                      float *out_prev_bottom, float *out_bottom)
{
    HpeStyle *st = &box->style;
    float inner_size;
    if (st->display == HPE_DISPLAY_FLEX)
        inner_size = layout_flex(ctx, box, inner_w, inner_h_base);
    else if (st->display == HPE_DISPLAY_TABLE)
        inner_size = layout_table(ctx, box, inner_w, inner_h_base);
    else if (st->display == HPE_DISPLAY_GRID)
        inner_size = layout_grid(ctx, box, inner_w, inner_h_base);
    else
        return 0;
    *out_had_block = 0;
    *out_prev_bottom = 0.0f;
    *out_bottom = box->content_box.y + inner_size;
    return 1;
}

/* One in-flow child: floats leave the flow, block children run their own flow,
 * everything else joins the inline paragraph being built. */
static void block_flow_child(LayoutCtx *ctx, HpeBox *box, HpeBox *child, float inner_w,
                             float inner_h_base, const float m[4], const float b[4], int no_top_sep,
                             BlockFlow *fl, HpeParagraph *para)
{
    if (child->style.float_side != HPE_FLOAT_NONE) {
        /* floats leave the flow: text laid out before them stays before */
        if (child->style.position != HPE_POS_ABSOLUTE &&
            child->style.position != HPE_POS_FIXED) {
            flush_paragraph(ctx, box, para, &fl->cur_y, inner_w);
            place_float(ctx, box, child, &fl->cur_y);
        }
        return;
    }
    if (child->kind == HPE_BOX_BLOCK) {
        /* out-of-flow boxes are positioned later, against their containing block */
        if (child->style.position == HPE_POS_ABSOLUTE || child->style.position == HPE_POS_FIXED)
            return;
        flush_paragraph(ctx, box, para, &fl->cur_y, inner_w);
        block_flow_block_child(ctx, box, child, inner_w, inner_h_base, m, b, no_top_sep, fl);
    } else {
        collect_inline(ctx, child, inner_w, para);
    }
}

static float block_flow_children(LayoutCtx *ctx, HpeBox *box, float inner_w, float inner_h_base,
                                 const float m[4], const float b[4], int no_top_sep,
                                 int *out_had_block, float *out_prev_bottom)
{
    HpeParagraph para;
    HpeBox *child;
    BlockFlow fl;
    float special_bottom;

    if (block_flow_special_display(ctx, box, inner_w, inner_h_base, out_had_block, out_prev_bottom,
                                   &special_bottom))
        return special_bottom;

    fl.cur_y = box->content_box.y;
    fl.prev_bottom = 0.0f;
    fl.first = 1;
    fl.had_block = 0;
    memset(&para, 0, sizeof(para));
    for (child = box->first_child; child; child = child->next)
        block_flow_child(ctx, box, child, inner_w, inner_h_base, m, b, no_top_sep, &fl, &para);
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
             child->style.position == HPE_POS_FIXED)) {
            /* deferred pass: position immediately; normal pass: queue */
            if (ctx->abspos_fixup)
                layout_abspos(ctx, child);
            else
                abspos_queue(ctx, child);
        }
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
/* Re-derive the border and padding boxes from the settled content box. */
static void block_box_rects(HpeBox *box, float inner_w, float inner_h, const float p[4],
                            const float b[4])
{
    box->border_box = hpe_rect(box->content_box.x - b[3] - p[3],
                               box->content_box.y - b[0] - p[0],
                               inner_w + p[3] + p[1] + b[3] + b[1],
                               inner_h + p[0] + p[2] + b[0] + b[2]);
    box->pad_box = hpe_rect(box->content_box.x - p[3], box->content_box.y - p[0],
                            inner_w + p[3] + p[1], inner_h + p[0] + p[2]);
}

/* Element-level scroll container. A classic (non-overlay) scrollbar takes inline
 * space out of the content box, and `overflow:auto` only pays that cost once the
 * content is known to overflow -- browsers respond by laying the subtree out a
 * second time, which is exactly what the narrowing re-run below models.
 * `overflow:scroll` reserves unconditionally. */
static void block_scroll_container(LayoutCtx *ctx, HpeBox *box, float avail_width, float avail_height,
                                   const float m[4], const float p[4], const float b[4], int no_top_sep,
                                   int no_bottom_sep, int *had_block, float *prev_bottom,
                                   float *inner_w, float *inner_h)
{
    HpeStyle *st = &box->style;
    int vscroll, always;
    float natural_h, port, over, gutter;
    if (st->overflow_y != HPE_OVERFLOW_SCROLL && st->overflow_y != HPE_OVERFLOW_AUTO &&
        st->overflow_x != HPE_OVERFLOW_SCROLL && st->overflow_x != HPE_OVERFLOW_AUTO)
        return;

    vscroll = st->overflow_y == HPE_OVERFLOW_SCROLL || st->overflow_y == HPE_OVERFLOW_AUTO;
    gutter = st->scrollbar_width;
    always = st->overflow_y == HPE_OVERFLOW_SCROLL;
    natural_h = box->scroll_content_h;

    box->is_scroll_container = 1;
    if (vscroll && gutter > 0.0f && (always || natural_h > box->pad_box.h)) {
        float cur_y, nw = *inner_w - gutter;
        if (nw < 0.0f)
            nw = 0.0f;
        /* re-run the child layout in the narrowed content box; every child
         * block frees its own line runs on entry, so this is idempotent */
        block_reset_lines(box);
        box->content_box.w = nw;
        *inner_w = nw;
        cur_y = block_flow_children(ctx, box, nw, avail_height, m, b, no_top_sep, had_block,
                                    prev_bottom);
        natural_h = cur_y - box->content_box.y;
        if (natural_h < 0.0f)
            natural_h = 0.0f;
        box->scroll_content_h = natural_h;
        *inner_h = block_content_height(ctx, box, natural_h, cur_y, *prev_bottom, *had_block,
                                        no_bottom_sep, avail_width, avail_height, m, p, b);
        box->content_box.h = *inner_h;
        block_box_rects(box, nw, *inner_h, p, b);
    }
    port = box->pad_box.h;
    over = box->scroll_content_h - port;
    box->scroll_port_h = port;
    box->scroll_max = over > 0.0f ? over : 0.0f;
    if (box->scroll_y > box->scroll_max)
        box->scroll_y = box->scroll_max;
    if (box->scroll_y < 0.0f)
        box->scroll_y = 0.0f;
}

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
    /* a replaced element solves its used content size from the intrinsic size
     * and the author's declaration before the ordinary width path runs */
    if (box->is_replaced)
        replaced_used_size(ctx, box, avail_width, avail_height);
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
    /* the natural content extent is what a scroll container has to be able to
     * reach; `block_content_height` may then clamp it to the explicit height. */
    {
        float natural = cur_y - box->content_box.y;
        if (natural < 0.0f)
            natural = 0.0f;
        box->scroll_content_h = natural;
    }
    inner_h = block_content_height(ctx, box, cur_y - box->content_box.y, cur_y, prev_bottom,
                                   had_block, no_bottom_sep, avail_width, avail_height, m, p, b);
    box->content_box.h = inner_h;
    block_box_rects(box, inner_w, inner_h, p, b);

    block_scroll_container(ctx, box, avail_width, avail_height, m, p, b, no_top_sep, no_bottom_sep,
                           &had_block, &prev_bottom, &inner_w, &inner_h);

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
    layout_ctx_init(&ctx, vw, vh, root->style.font_size);
    if (root->kind == HPE_BOX_BLOCK)
        layout_block(&ctx, root, 0.0f, 0.0f, vw, ctx.vh);
    abspos_flush(&ctx);
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
    ctx.abspos = NULL;
    ctx.abspos_count = 0;
    ctx.abspos_cap = 0;
    ctx.abspos_fixup = 0;

    root_parent = hpe_style_default();
    root = build_element(html, sheet, &root_parent);
    if (!root)
        return NULL;
    /* `rem` resolves against the root element's font size */
    if (root->style.font_size > 0.0f)
        ctx.root_font_size = root->style.font_size;
    if (root->kind == HPE_BOX_BLOCK)
        layout_block(&ctx, root, 0.0f, 0.0f, viewport_width, ctx.vh);
    abspos_flush(&ctx);
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
        if (box->lines[i].frags)
            hpe_free(box->lines[i].frags);
    }
    if (box->lines)
        hpe_free(box->lines);
    if (box->floats.items)
        hpe_free(box->floats.items);
    if (box->owns_text && box->text)
        hpe_free((void *)box->text);
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
