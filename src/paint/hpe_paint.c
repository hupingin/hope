#include "paint/hpe_paint.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "paint/hpe_svg.h"
#include "base/hpe_util.h"
#include "render/hpe_raster.h"

/**
 * @brief 把设备 y 吸附到 1/64 像素栅格 / Snap a device y to a 1/64 px grid
 *
 * @param[in]     y 设备 y 坐标 / Device y coordinate
 *
 * @return 吸附后的 y / The snapped y
 *
 * @note 布局会累积浮点误差，1/64 的倍数可精确表示，因此平移后的行与重绘的行总能一致 / Layout accumulates fractional error; multiples of 1/64 are exactly representable so a shifted copy of a row and a repainted row always agree
 */
static float snap_y(float y)
{
    return (float)((double)(int)(y * 64.0f + (y >= 0.0f ? 0.5f : -0.5f))) / 64.0f;
}

/* A paint pass maps document coordinates to device coordinates as
 *   device_x = doc_x + dev_x
 *   device_y = doc_y - scroll + dev_y
 * `dev_x`/`dev_y` are non-zero only inside an opacity group (which paints into
 * a scratch pixmap anchored at the box's top-left) and later by transforms. */
typedef struct PaintCtx {
    float dev_x, dev_y;
    int group_depth; /* opacity/transform-group nesting, to cap pathological recursion */
} PaintCtx;

#define MAX_OPACITY_DEPTH 3

/**
 * @brief 把文档 y 映射为设备 y / Map a document y to device y
 *
 * @param[in]     ctx 绘制上下文 / Paint context
 * @param[in]     doc_y 文档 y / Document y
 * @param[in]     scroll 纵向滚动偏移 / Vertical scroll offset
 *
 * @return 设备 y / Device y
 *
 * @note 映射关系为 `doc_y - scroll + dev_y` / The mapping is `doc_y - scroll + dev_y`
 */
static float ctx_dev_y(PaintCtx *ctx, float doc_y, float scroll)
{
    return doc_y - scroll + ctx->dev_y;
}

/**
 * @brief 求盒边框盒的设备矩形 / Device rect of a box's border box
 *
 * @param[in]     ctx 绘制上下文 / Paint context
 * @param[in]     box 盒 / Box
 * @param[in]     scroll 纵向滚动偏移 / Vertical scroll offset
 *
 * @return 设备坐标矩形，y 已吸附 / Device-space rect with y snapped
 */
static HpeRect box_device_rect(PaintCtx *ctx, HpeBox *box, float scroll)
{
    HpeRect r = box->border_box;
    r.x += ctx->dev_x;
    r.y = snap_y(r.y - scroll + ctx->dev_y);
    return r;
}

/* ------------------------------------------------------------------ */
/* list markers                                                        */
/* ------------------------------------------------------------------ */

/**
 * @brief 判断盒是否为 `ul`/`ol` 下的 `<li>` / Test whether a box is an `<li>` inside a `ul`/`ol`
 *
 * @param[in]     box 盒 / Box
 *
 * @return 是列表项返回 1，否则 0 / 1 for a list item, else 0
 */
static int is_list_item(HpeBox *box)
{
    return box->node && box->node->tag && !strcmp(box->node->tag, "li") && box->parent &&
           box->parent->node && box->parent->node->tag &&
           (!strcmp(box->parent->node->tag, "ul") || !strcmp(box->parent->node->tag, "ol"));
}

/**
 * @brief 求 `<li>` 在其 `<ol>` 中的序号 / 1-based position of an `<li>` among its `<ol>` siblings
 *
 * @param[in]     box 列表项盒 / List item box
 *
 * @return 之前的 `<li>` 兄弟个数（0 起）/ Number of preceding `<li>` siblings, zero based
 *
 * @note 父级不是 `<ol>` 时返回 0 / Returns 0 when the parent is not an `<ol>`
 */
static int list_index(HpeBox *box)
{
    HpeBox *sib;
    int index = 0;
    if (!box->parent || !box->parent->node || strcmp(box->parent->node->tag, "ol") != 0)
        return 0;
    for (sib = box->parent->first_child; sib && sib != box; sib = sib->next)
        if (sib->node && sib->node->tag && !strcmp(sib->node->tag, "li"))
            index++;
    return index;
}

/**
 * @brief 求实际生效的 list-style-type / Effective `list-style-type` for a list item
 *
 * @param[in]     box 列表项盒 / List item box
 *
 * @return 生效的列表标记类型 / The effective list marker type
 *
 * @note 盒树只继承固定的属性列表，项自身未声明时从父盒读取 / The box tree inherits only a fixed property list, so the value is read from the parent when the item declares none
 */
static HpeListStyleType effective_list_type(HpeBox *box)
{
    if (box->style.set2 & HPE_PROP2_LIST_STYLE_TYPE)
        return box->style.list_style_type;
    if (box->parent)
        return box->parent->style.list_style_type;
    return HPE_LIST_DISC;
}

/**
 * @brief 绘制方形列表标记 / Paint a square list marker
 *
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     box 列表项盒 / List item box
 * @param[in]     marker_x 标记锚点 X / Marker anchor X
 * @param[in]     baseline 基线设备 y / Baseline device y
 *
 * @note 对应 `list-style-type: square`，尺寸取字号的 0.32 / Corresponds to `list-style-type: square` with a size of 0.32 times the font size
 */
static void paint_marker_square(HpePixmap *pm, HpeBox *box, float marker_x, float baseline)
{
    float size = box->style.font_size * 0.32f;
    hpe_fill_rect(pm, hpe_rect(marker_x - size - 3.0f,
                               baseline - box->style.font_size * 0.55f, size, size),
                  box->style.color);
}

/**
 * @brief 绘制空心圆列表标记 / Paint a hollow circle list marker
 *
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     box 列表项盒 / List item box
 * @param[in]     marker_x 标记锚点 X / Marker anchor X
 * @param[in]     baseline 基线设备 y / Baseline device y
 *
 * @note 对应 `list-style-type: circle` / Corresponds to `list-style-type: circle`
 */
static void paint_marker_circle(HpePixmap *pm, HpeBox *box, float marker_x, float baseline)
{
    float r = box->style.font_size * 0.18f;
    hpe_stroke_round_rect(pm, hpe_rect(marker_x - r * 2.0f - 3.0f,
                                       baseline - box->style.font_size * 0.55f, r * 2.0f,
                                       r * 2.0f),
                          r, 1.0f, box->style.color);
}

/**
 * @brief 绘制列表项的标记 / Paint a list item's marker
 *
 * @param[in]     ctx 绘制上下文 / Paint context
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     box 列表项盒 / List item box
 * @param[in]     scroll 纵向滚动偏移 / Vertical scroll offset
 *
 * @note 支持 decimal、square、circle、disc；`list-style-type:none` 与非列表项直接返回 / Supports decimal, square, circle and disc; returns immediately for `list-style-type:none` and non-list boxes
 */
static void paint_list_marker(PaintCtx *ctx, HpePixmap *pm, HpeBox *box, float scroll)
{
    static const char bullet[] = "\xE2\x80\xA2"; /* U+2022 */
    HpeListStyleType type;
    HpeFont *font;
    const HpeTextMask *mask;
    float baseline, marker_x, base_x;

    if (!is_list_item(box) || box->line_count == 0)
        return;
    type = effective_list_type(box);
    if (type == HPE_LIST_NONE)
        return;
    font = hpe_font_get(box->style.font_size, box->style.font_weight, box->style.family);
    if (!font)
        return;
    baseline = snap_y(ctx_dev_y(ctx, box->lines[0].y + box->lines[0].baseline, scroll));
    base_x = box->content_box.x + ctx->dev_x;
    marker_x = (box->style.list_style_position == HPE_LIST_INSIDE) ? base_x + 6.0f
                                                                   : base_x - 18.0f;

    switch (type) {
    case HPE_LIST_DECIMAL: {
        char num[16];
        int n = snprintf(num, sizeof(num), "%d.", list_index(box) + 1);
        float adv = hpe_font_measure(font, num, n);
        mask = hpe_font_mask(font, num, n);
        if (mask)
            hpe_draw_text_mask(pm, (int)floorf(marker_x - adv), (int)floorf(baseline), mask,
                               box->style.color);
        break;
    }
    case HPE_LIST_SQUARE:
        paint_marker_square(pm, box, marker_x, baseline);
        break;
    case HPE_LIST_CIRCLE:
        paint_marker_circle(pm, box, marker_x, baseline);
        break;
    default: /* disc: a U+2022 bullet, left-aligned at the marker anchor */
        mask = hpe_font_mask(font, bullet, 3);
        if (mask)
            hpe_draw_text_mask(pm, (int)floorf(marker_x), (int)floorf(baseline), mask,
                               box->style.color);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* text decoration                                                     */
/* ------------------------------------------------------------------ */

/**
 * @brief 绘制文本装饰线 / Paint text decoration lines
 *
 * @param[in]     ctx 绘制上下文 / Paint context
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     box 含文本行的盒 / Box holding the text lines
 * @param[in]     scroll 纵向滚动偏移 / Vertical scroll offset
 * @param[in]     dec 装饰位掩码 / Decoration bit mask
 *
 * @note 下划线、上划线、删除线各画一条 1px 线，覆盖行内非原子 run 的水平范围 / Draws one 1px line each for underline, overline and line-through spanning the horizontal extent of the line's non-atomic runs
 */
static void paint_text_decorations(PaintCtx *ctx, HpePixmap *pm, HpeBox *box, float scroll,
                                   HpeTextDecoration dec)
{
    int i, r;
    if (dec == HPE_TD_NONE || box->line_count == 0)
        return;
    for (i = 0; i < box->line_count; i++) {
        HpeLine *line = &box->lines[i];
        float baseline = snap_y(ctx_dev_y(ctx, line->y + line->baseline, scroll));
        float run_x0 = 1e9f, run_x1 = -1e9f;
        for (r = 0; r < line->run_count; r++) {
            HpeTextRun *run = &line->runs[r];
            float x0, x1;
            if (run->atomic || !run->font || run->color.a == 0)
                continue;
            x0 = box->content_box.x + ctx->dev_x + run->x;
            x1 = x0 + run->width;
            if (x0 < run_x0)
                run_x0 = x0;
            if (x1 > run_x1)
                run_x1 = x1;
        }
        if (run_x1 <= run_x0)
            continue;
        if (dec & HPE_TD_UNDERLINE)
            hpe_fill_rect(pm, hpe_rect(run_x0, baseline + 1.0f, run_x1 - run_x0, 1.0f),
                          box->style.color);
        if (dec & HPE_TD_OVERLINE)
            hpe_fill_rect(pm,
                          hpe_rect(run_x0, ctx_dev_y(ctx, line->y, scroll), run_x1 - run_x0, 1.0f),
                          box->style.color);
        if (dec & HPE_TD_LINE_THROUGH)
            hpe_fill_rect(pm,
                          hpe_rect(run_x0, baseline - box->style.font_size * 0.32f,
                                   run_x1 - run_x0, 1.0f),
                          box->style.color);
    }
}

/* ------------------------------------------------------------------ */
/* shadows                                                             */
/* ------------------------------------------------------------------ */

/**
 * @brief 绘制盒的 box-shadow / Paint a box's `box-shadow`
 *
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     box 盒 / Box
 * @param[in]     bb 盒的设备矩形 / Device rect of the box
 * @param[in]     radius 圆角半径 / Corner radius
 *
 * @note 阴影矩形由偏移与 spread 从 bb 推出，无效或全透明时跳过 / The shadow rect is derived from `bb` by the offset and spread; skipped when invalid or fully transparent
 */
static void paint_box_shadow(HpePixmap *pm, HpeBox *box, HpeRect bb, float radius)
{
    HpeShadow *sh = &box->style.box_shadow;
    HpeRect r;
    if (!sh->valid || sh->color.a == 0)
        return;
    r = hpe_rect(bb.x + sh->ox - sh->spread, bb.y + sh->oy - sh->spread, bb.w + sh->spread * 2.0f,
                 bb.h + sh->spread * 2.0f);
    hpe_fill_shadow(pm, r, radius > 0.0f ? radius : 0.0f, sh->blur, sh->color);
}

/**
 * @brief 绘制文本的 text-shadow / Paint a box's `text-shadow`
 *
 * @param[in]     ctx 绘制上下文 / Paint context
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     box 含文本行的盒 / Box holding the text lines
 * @param[in]     scroll 纵向滚动偏移 / Vertical scroll offset
 *
 * @note 逐个非原子 run 按阴影偏移绘制字形掩码 / Draws the glyph mask of every non-atomic run offset by the shadow
 */
static void paint_text_shadow(PaintCtx *ctx, HpePixmap *pm, HpeBox *box, float scroll)
{
    HpeShadow *sh = &box->style.text_shadow;
    int i, r;
    if (!sh->valid || sh->color.a == 0)
        return;
    for (i = 0; i < box->line_count; i++) {
        HpeLine *line = &box->lines[i];
        float baseline = snap_y(ctx_dev_y(ctx, line->y + line->baseline, scroll));
        for (r = 0; r < line->run_count; r++) {
            HpeTextRun *run = &line->runs[r];
            const HpeTextMask *mask;
            if (run->atomic || !run->font)
                continue;
            mask = hpe_font_mask(run->font, run->text, run->len);
            if (!mask)
                continue;
            hpe_draw_text_mask(pm, (int)floorf(box->content_box.x + ctx->dev_x + run->x + sh->ox),
                               (int)floorf(baseline + sh->oy), mask, sh->color);
        }
    }
}

/* ------------------------------------------------------------------ */
/* background / borders                                                */
/* ------------------------------------------------------------------ */

/**
 * @brief 绘制盒背景 / Paint a box's background
 *
 * @param[in]     bs 计算样式 / Computed style
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     bb 盒的设备矩形 / Device rect of the box
 * @param[in]     radius 圆角半径 / Corner radius
 *
 * @note 线性渐变背景优先；否则纯色填充，radius > 0.5 时走圆角填充 / A linear-gradient background wins; otherwise a solid fill, using the rounded-rect path when `radius > 0.5`
 */
static void paint_background(HpeStyle *bs, HpePixmap *pm, HpeRect bb, float radius)
{
    if (bs->background_image.kind == HPE_BG_LINEAR_GRADIENT) {
        HpeRasterStop stops[HPE_MAX_GRADIENT_STOPS];
        int i;
        for (i = 0; i < bs->background_image.stop_count; i++) {
            stops[i].pos = bs->background_image.stops[i].pos;
            stops[i].color = bs->background_image.stops[i].color;
        }
        hpe_fill_round_rect_gradient(pm, bb, radius, bs->background_image.angle_deg, stops,
                                     bs->background_image.stop_count);
        return;
    }
    if (bs->background.a > 0) {
        if (radius > 0.5f)
            hpe_fill_round_rect(pm, bb, radius, bs->background);
        else
            hpe_fill_rect(pm, bb, bs->background);
    }
}

/**
 * @brief 绘制四边边框 / Paint the four border bands
 *
 * @param[in]     bs 计算样式 / Computed style
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     bb 盒的设备矩形 / Device rect of the box
 * @param[in]     radius 圆角半径 / Corner radius
 *
 * @note 有圆角时退化为单环并取最不透明的边框色；无有效边框时直接返回 / With rounded corners it collapses to a single ring using the most opaque border colour; returns early when no border is effective
 */
static void paint_borders(HpeStyle *bs, HpePixmap *pm, HpeRect bb, float radius)
{
    float bt = bs->border_w[0], br = bs->border_w[1];
    float bbw = bs->border_w[2], bl = bs->border_w[3];
    float maxb = 0.0f;
    int i;
    int any = 0;

    for (i = 0; i < 4; i++) {
        if (bs->border_w[i] > maxb)
            maxb = bs->border_w[i];
        if (bs->border_w[i] > 0.0f && bs->border_colors[i].a > 0)
            any = 1;
    }
    if (!any)
        return;

    if (radius > 0.5f && maxb > 0.0f) {
        HpeColor best = bs->border_colors[0];
        for (i = 1; i < 4; i++)
            if (bs->border_colors[i].a > best.a)
                best = bs->border_colors[i];
        if (best.a > 0)
            hpe_stroke_round_rect(pm, bb, radius, maxb, best);
        return;
    }

    if (bt > 0.0f && bs->border_colors[0].a > 0)
        hpe_fill_rect(pm, hpe_rect(bb.x, bb.y, bb.w, bt), bs->border_colors[0]);
    if (bbw > 0.0f && bs->border_colors[2].a > 0)
        hpe_fill_rect(pm, hpe_rect(bb.x, bb.y + bb.h - bbw, bb.w, bbw), bs->border_colors[2]);
    if (bl > 0.0f && bs->border_colors[3].a > 0)
        hpe_fill_rect(pm, hpe_rect(bb.x, bb.y + bt, bl, bb.h - bt - bbw), bs->border_colors[3]);
    if (br > 0.0f && bs->border_colors[1].a > 0)
        hpe_fill_rect(pm, hpe_rect(bb.x + bb.w - br, bb.y + bt, br, bb.h - bt - bbw),
                      bs->border_colors[1]);
}

/* ------------------------------------------------------------------ */
/* the box walk                                                        */
/* ------------------------------------------------------------------ */

static void paint_box_inner(PaintCtx *ctx, HpePixmap *pm, HpeBox *box, float scroll);

/* Paints `box` (and its subtree) into a scratch pixmap and blends the result
 * back with a per-pixel alpha that is `opacity` uniformly multiplied on top of
 * each source pixel's own alpha.
 *
 * A subtree painted directly into the destination would let a child blend with
 * a *sibling* it should never see (both share the page background otherwise),
 * and would leak the group's opacity into every descendant separately; the
 * scratch buffer composites the subtree in isolation first. `bbox` is the
 * device-space region to capture; `opacity` is 1.0 when only a transform is
 * being applied. */

/**
 * @brief 把离屏矩形夹紧到目标裁剪区 / Clamp a scratch rect to the destination clip
 *
 * @param[in]     pm 目标像素缓冲 / Destination pixel buffer
 * @param[in,out] x0 离屏左边界，被夹紧 / Scratch left edge, clamped in place
 * @param[in,out] y0 离屏上边界，被夹紧 / Scratch top edge, clamped in place
 * @param[in,out] w 离屏宽度，被夹紧 / Scratch width, clamped in place
 * @param[in,out] h 离屏高度，被夹紧 / Scratch height, clamped in place
 *
 * @return 夹紧后仍非空返回 1，否则 0 / 1 when the clamped rect is still non-empty, else 0
 */
static int group_clip_scratch(HpePixmap *pm, int *x0, int *y0, int *w, int *h)
{
    /* clip the scratch to what the destination can accept */
    if (*x0 < pm->clip_x0) {
        *w -= pm->clip_x0 - *x0;
        *x0 = pm->clip_x0;
    }
    if (*y0 < pm->clip_y0) {
        *h -= pm->clip_y0 - *y0;
        *y0 = pm->clip_y0;
    }
    if (*x0 + *w > pm->clip_x1)
        *w = pm->clip_x1 - *x0;
    if (*y0 + *h > pm->clip_y1)
        *h = pm->clip_y1 - *y0;
    return (*w > 0 && *h > 0);
}

/**
 * @brief 把离屏子树结果合成回目标 / Composite a scratch subtree result back onto the destination
 *
 * @param[in,out] pm 目标像素缓冲 / Destination pixel buffer
 * @param[in]     scratch 承载子树的离屏缓冲 / Scratch buffer holding the subtree
 * @param[in]     x0 离屏左边界 / Scratch left edge
 * @param[in]     y0 离屏上边界 / Scratch top edge
 * @param[in]     w 离屏宽度 / Scratch width
 * @param[in]     h 离屏高度 / Scratch height
 * @param[in]     opacity 不透明度 0..1 / Opacity in 0..1
 * @param[in]     xf 变换矩阵，可为 `NULL` / Transform matrix, may be `NULL`
 * @param[in]     anchor_x 变换锚点 X / Transform anchor X
 * @param[in]     anchor_y 变换锚点 Y / Transform anchor Y
 *
 * @note 有变换时把目标像素逆映射回离屏空间；源 alpha 乘以 opacity 后按 255 定点混合 / With a transform each destination pixel is inverse-mapped into scratch space; source alpha is scaled by `opacity` and blended in 255 fixed point
 */
static void group_composite(HpePixmap *pm, const HpePixmap *scratch, int x0, int y0, int w, int h,
                            float opacity, HpeTransform *xf, int anchor_x, int anchor_y)
{
    /* the transform is applied about the box's top-left corner, which is
     * where the scratch origin was anchored (see paint_box) */
    int use_xf = xf && !xf->identity;
    float ox = (float)anchor_x - (float)x0;
    float oy = (float)anchor_y - (float)y0;
    int px, py;

    for (py = 0; py < h; py++) {
        uint32_t *dst_row = pm->pixels + (size_t)(y0 + py) * (size_t)pm->width + (size_t)x0;
        for (px = 0; px < w; px++) {
            uint32_t s;
            uint32_t sa, a, inv, sr, sg, sb, dr, dg, db;
            if (use_xf) {
                /* inverse-map the destination pixel back into scratch space */
                float rx = (float)px - ox, ry = (float)py - oy;
                float det = xf->a * xf->d - xf->b * xf->c;
                float ux, uy;
                int ix, iy;
                if (fabsf(det) < 1e-9f)
                    continue;
                ux = (xf->d * rx - xf->c * ry) / det;
                uy = (-xf->b * rx + xf->a * ry) / det;
                ix = (int)floorf(ux + ox);
                iy = (int)floorf(uy + oy);
                if (ix < 0 || iy < 0 || ix >= w || iy >= h)
                    continue;
                s = scratch->pixels[(size_t)iy * (size_t)w + (size_t)ix];
            } else {
                s = scratch->pixels[(size_t)py * (size_t)w + (size_t)px];
            }
            sa = (s >> 24) & 0xFFu;
            if (sa == 0)
                continue;
            a = (uint32_t)((float)sa * opacity + 0.5f);
            if (a == 0)
                continue;
            dr = (dst_row[px] >> 16) & 0xFFu;
            dg = (dst_row[px] >> 8) & 0xFFu;
            db = dst_row[px] & 0xFFu;
            sr = (s >> 16) & 0xFFu;
            sg = (s >> 8) & 0xFFu;
            sb = s & 0xFFu;
            inv = 255u - a;
            dst_row[px] = 0xFF000000u | (((sr * a + dr * inv) / 255u) << 16) |
                          (((sg * a + dg * inv) / 255u) << 8) | ((sb * a + db * inv) / 255u);
        }
    }
}

/**
 * @brief 把盒子树绘入离屏缓冲再按不透明度合成回来 / Paint a box's subtree into a scratch pixmap and blend it back with an opacity
 *
 * @param[in]     ctx 上层绘制上下文 / Enclosing paint context
 * @param[in,out] pm 目标像素缓冲 / Destination pixel buffer
 * @param[in]     box 分组根盒 / Group root box
 * @param[in]     scroll 纵向滚动偏移 / Vertical scroll offset
 * @param[in]     bbox 要捕获的设备区域 / Device-space region to capture
 * @param[in]     opacity 整组不透明度 0..1 / Group opacity in 0..1
 * @param[in]     xf 变换矩阵，可为 `NULL` / Transform matrix, may be `NULL`
 * @param[in]     anchor_x 变换锚点 X / Transform anchor X
 * @param[in]     anchor_y 变换锚点 Y / Transform anchor Y
 *
 * @note 先在离屏内孤立合成子树，避免子盒与不应见到的兄弟混合、也避免把整组不透明度逐个泄漏给后代；仅应用变换时 opacity 为 1.0 / The subtree is composited in isolation first so a child cannot blend with a sibling it should never see and the group opacity is not leaked into each descendant; `opacity` is 1.0 when only a transform is applied
 */
static void paint_group(PaintCtx *ctx, HpePixmap *pm, HpeBox *box, float scroll, HpeRect bbox,
                        float opacity, HpeTransform *xf, int anchor_x, int anchor_y)
{
    HpePixmap scratch;
    PaintCtx sub;
    int pad = 2;
    int x0, y0, w, h;

    if (opacity <= 0.0f)
        return; /* fully transparent: nothing to paint */

    if (box->style.box_shadow.valid) {
        HpeShadow *sh = &box->style.box_shadow;
        float bleed = sh->blur + sh->spread + fabsf(sh->ox) + fabsf(sh->oy);
        pad += (int)ceilf(bleed);
    }
    x0 = (int)floorf(bbox.x) - pad;
    y0 = (int)floorf(bbox.y) - pad;
    w = (int)ceilf(bbox.w) + pad * 2;
    h = (int)ceilf(bbox.h) + pad * 2;
    if (w <= 0 || h <= 0)
        return;
    if (!group_clip_scratch(pm, &x0, &y0, &w, &h))
        return;

    memset(&scratch, 0, sizeof(scratch));
    scratch.width = w;
    scratch.height = h;
    scratch.stride = w * 4;
    scratch.pixels = (uint32_t *)hpe_calloc((size_t)w * (size_t)h, sizeof(uint32_t));
    hpe_pixmap_reset_clip(&scratch);

    /* shift the device origin so the box's top-left lands at the scratch origin */
    sub.dev_x = ctx->dev_x - (float)x0;
    sub.dev_y = ctx->dev_y - (float)y0;
    sub.group_depth = ctx->group_depth + 1;
    paint_box_inner(&sub, &scratch, box, scroll);

    group_composite(pm, &scratch, x0, y0, w, h, opacity, xf, anchor_x, anchor_y);
    hpe_free(scratch.pixels);
}

/**
 * @brief 求变换后盒的轴对齐包围盒 / Axis-aligned bounds of a transformed box
 *
 * @param[in]     bb 变换前的盒矩形 / Box rect before the transform
 * @param[in]     xf 变换矩阵 / Transform matrix
 *
 * @return 变换后四角的轴对齐包围盒 / Axis-aligned bounds of the four transformed corners
 *
 * @note 恒等变换时原样返回；旋转/倾斜会把盒变成四边形 / Returns the input unchanged for an identity transform; rotate and skew turn the box into a quad
 */
static HpeRect transform_bounds(HpeRect bb, HpeTransform *xf)
{
    float cx[4], cy[4];
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    int i;
    if (xf->identity)
        return bb;
    cx[0] = bb.x;
    cy[0] = bb.y;
    cx[1] = bb.x + bb.w;
    cy[1] = bb.y;
    cx[2] = bb.x;
    cy[2] = bb.y + bb.h;
    cx[3] = bb.x + bb.w;
    cy[3] = bb.y + bb.h;
    for (i = 0; i < 4; i++) {
        /* the transform is about the box's top-left corner */
        float lx = cx[i] - bb.x, ly = cy[i] - bb.y;
        float x = bb.x + (xf->a * lx + xf->c * ly + xf->e);
        float y = bb.y + (xf->b * lx + xf->d * ly + xf->f);
        if (x < minx)
            minx = x;
        if (y < miny)
            miny = y;
        if (x > maxx)
            maxx = x;
        if (y > maxy)
            maxy = y;
    }
    return hpe_rect(minx, miny, maxx - minx, maxy - miny);
}

/**
 * @brief 绘制一个盒及其子树 / Paint a box and its subtree
 *
 * @param[in]     ctx 绘制上下文 / Paint context
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     box 待绘制的盒 / Box to paint
 * @param[in]     scroll 纵向滚动偏移 / Vertical scroll offset
 *
 * @note `display:none` 跳过；需要分组（透明度或变换）时走 paint_group，否则直接画内部 / `display:none` is skipped; a box needing a group (opacity or transform) goes through paint_group, otherwise its inner content is painted directly
 */
static void paint_box(PaintCtx *ctx, HpePixmap *pm, HpeBox *box, float scroll)
{
    HpeStyle *bs = &box->style;
    int needs_group;
    if (bs->display == HPE_DISPLAY_NONE)
        return;
    /* `visibility: hidden` suppresses this box's own painting but not its
     * subtree: a descendant may set `visibility: visible` to reappear, so we
     * still recurse (paint_box_inner skips the self-painting when hidden). */
    needs_group = (bs->opacity < 1.0f) || !bs->transform.identity;
    if (needs_group && ctx->group_depth < MAX_OPACITY_DEPTH) {
        HpeRect bb = box_device_rect(ctx, box, scroll);
        float radius = bs->border_radius;
        HpeRect bbox = bb;
        if (radius < 0.0f) {
            float side = bb.w < bb.h ? bb.w : bb.h;
            radius = side * (-radius) * 0.01f;
        }
        if (!bs->transform.identity)
            bbox = transform_bounds(bb, &bs->transform);
        paint_group(ctx, pm, box, scroll, bbox, bs->opacity < 1.0f ? bs->opacity : 1.0f,
                    &bs->transform, (int)floorf(bb.x), (int)floorf(bb.y));
        return;
    }
    paint_box_inner(ctx, pm, box, scroll);
}

/**
 * @brief 绘制盒自身的视觉内容 / Paint a box's own visuals
 *
 * @param[in]     ctx 绘制上下文 / Paint context
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     box 盒 / Box
 * @param[in]     own_scroll 本盒的滚动偏移 / The box's own scroll offset
 * @param[in]     bb 盒的设备矩形 / Device rect of the box
 * @param[in]     radius 圆角半径，负值表示百分比 / Corner radius, negative meaning a percentage
 *
 * @note 依次绘制阴影、背景、边框、列表标记、文本阴影、文本 run 与装饰；原子行内 run 由子盒递归绘制 / Paints the shadow, background, borders, list marker, text shadow, text runs and decorations; atomic inline runs are painted via child recursion
 */
static void paint_box_own_content(PaintCtx *ctx, HpePixmap *pm, HpeBox *box, float own_scroll,
                                  HpeRect bb, float radius)
{
    HpeStyle *bs = &box->style;
    int i, r;

    if (bs->box_shadow.valid)
        paint_box_shadow(pm, box, bb, radius);
    paint_background(bs, pm, bb, radius);
    paint_borders(bs, pm, bb, radius);

    if (getenv("HOPE_TRACE_PAINT"))
        fprintf(stderr, "paint %s<%s> bb=(%.2f,%.2f %.2fx%.2f) bg=%02x%02x%02x%02x runs=%d\n",
                box->kind == HPE_BOX_TEXT ? "T" : (box->kind == HPE_BOX_INLINE ? "I" : "B"),
                box->node && box->node->tag ? box->node->tag : "?", bb.x, bb.y, bb.w, bb.h,
                bs->background.a, bs->background.r, bs->background.g, bs->background.b,
                box->line_count);

    paint_list_marker(ctx, pm, box, own_scroll);
    if (bs->text_shadow.valid)
        paint_text_shadow(ctx, pm, box, own_scroll);

    for (i = 0; i < box->line_count; i++) {
        HpeLine *line = &box->lines[i];
        float baseline = snap_y(ctx_dev_y(ctx, line->y + line->baseline, own_scroll));
        float line_top = snap_y(ctx_dev_y(ctx, line->y, own_scroll));
        float line_bottom = line_top + line->height;

        if (line_bottom < (float)pm->clip_y0 || line_top > (float)pm->clip_y1)
            continue;

        for (r = 0; r < line->run_count; r++) {
            HpeTextRun *run = &line->runs[r];
            const HpeTextMask *mask;
            int x, y;
            /* atomic inline-block runs are painted via child recursion, not here */
            if (run->atomic || !run->font)
                continue;
            x = (int)floorf(box->content_box.x + ctx->dev_x + run->x);
            y = (int)floorf(baseline);
            if (run->color.a == 0 || x > pm->clip_x1)
                continue;
            mask = hpe_font_mask(run->font, run->text, run->len);
            if (!mask)
                continue;
            hpe_draw_text_mask(pm, x, y, mask, run->color);
        }
    }
    if (bs->text_decoration != HPE_TD_NONE)
        paint_text_decorations(ctx, pm, box, own_scroll, bs->text_decoration);
}

/**
 * @brief 按 z-index 升序插入分层盒序列 / Insert a box into the z-order stack by ascending z-index
 *
 * @param[in,out] stacked 分层盒指针数组 / Stacked box pointer array
 * @param[in]     n 数组当前长度 / Current array length
 *
 * @note 对末尾元素做插入排序；长度不足 2 时无操作 / Insertion sort on the last element; no-op when the length is below 2
 */
static void stacked_insert_sorted(HpeBox **stacked, int n)
{
    HpeBox *key;
    int j;
    if (n <= 1)
        return;
    key = stacked[n - 1];
    j = n - 2;
    while (j >= 0 && stacked[j]->style.z_index > key->style.z_index) {
        stacked[j + 1] = stacked[j];
        j--;
    }
    stacked[j + 1] = key;
}

/**
 * @brief 绘制子盒子树 / Paint the child subtree
 *
 * @param[in]     ctx 绘制上下文 / Paint context
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     box 父盒 / Parent box
 * @param[in]     scroll 纵向滚动偏移 / Vertical scroll offset
 *
 * @note 静态子盒按顺序绘制，定位子盒按 z-index 升序叠在上层 / Static children paint in order; positioned children paint on top in ascending z-index
 */
static void paint_box_children(PaintCtx *ctx, HpePixmap *pm, HpeBox *box, float scroll)
{
    HpeBox **stacked = NULL;
    size_t sc = 0, scap = 0;
    HpeBox *child;
    int r;

    for (child = box->first_child; child; child = child->next) {
        if (child->style.position == HPE_POS_STATIC) {
            paint_box(ctx, pm, child, scroll);
        } else {
            stacked = (HpeBox **)hpe_grow(stacked, &scap, sc + 1, sizeof(HpeBox *));
            stacked[sc++] = child;
            stacked_insert_sorted(stacked, (int)sc);
        }
    }
    for (r = 0; r < (int)sc; r++)
        paint_box(ctx, pm, stacked[r], scroll);
    if (stacked)
        hpe_free(stacked);
}

/**
 * @brief 按 overflow 裁剪后绘制子盒 / Paint the children clipped per `overflow`
 *
 * @param[in]     ctx 绘制上下文 / Paint context
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     box 父盒 / Parent box
 * @param[in]     scroll 纵向滚动偏移 / Vertical scroll offset
 * @param[in]     own_scroll 本盒的滚动偏移 / The box's own scroll offset
 *
 * @note hidden/scroll/auto 会为子树建立裁剪矩形；元素级滚动尚未实现，scroll/auto 对超出内容表现为 hidden / `hidden`/`scroll`/`auto` establish a clip rect; element-level scrolling is not modelled, so `scroll`/`auto` behave like `hidden` for overflow
 */
static void paint_box_clipped_children(PaintCtx *ctx, HpePixmap *pm, HpeBox *box, float scroll,
                                       float own_scroll)
{
    HpeStyle *bs = &box->style;
    int had_clip = 0;
    int saved[4] = {0, 0, 0, 0};
    if (bs->overflow_x != HPE_OVERFLOW_VISIBLE || bs->overflow_y != HPE_OVERFLOW_VISIBLE) {
        HpeRect clip = box->pad_box;
        clip.x += ctx->dev_x;
        clip.y = snap_y(ctx_dev_y(ctx, box->pad_box.y, own_scroll));
        saved[0] = pm->clip_x0;
        saved[1] = pm->clip_y0;
        saved[2] = pm->clip_x1;
        saved[3] = pm->clip_y1;
        hpe_pixmap_intersect_clip(pm, clip);
        had_clip = 1;
    }

    paint_box_children(ctx, pm, box, scroll);

    if (had_clip) {
        pm->clip_x0 = saved[0];
        pm->clip_y0 = saved[1];
        pm->clip_x1 = saved[2];
        pm->clip_y1 = saved[3];
    }
}

/**
 * @brief 绘制盒自身内容与已裁剪的子盒 / Paint a box's own content and its clipped children
 *
 * @param[in]     ctx 绘制上下文 / Paint context
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     box 盒 / Box
 * @param[in]     scroll 纵向滚动偏移 / Vertical scroll offset
 *
 * @note 固定定位盒不随滚动，其子树同样使用零滚动；`visibility:hidden` 只隐藏本盒自身视觉，子树仍递归 / Fixed boxes never scroll and their subtrees share the same zero offset; `visibility:hidden` hides only this box's own visuals while the subtree is still recursed
 */
static void paint_box_inner(PaintCtx *ctx, HpePixmap *pm, HpeBox *box, float scroll)
{
    HpeStyle *bs = &box->style;
    HpeRect bb;
    /* fixed boxes are laid out in viewport coordinates: never scroll them,
     * and their subtrees inherit the same (zero) scroll offset */
    float own_scroll = bs->position == HPE_POS_FIXED ? 0.0f : scroll;
    float radius = bs->border_radius;
    /* `visibility: hidden` hides this box's own visuals (background, borders,
     * text, decorations) but the box still occupies space and its subtree is
     * still painted: descendants may set `visibility: visible` to reappear. */
    int self_visible = bs->visibility != HPE_VISIBILITY_HIDDEN;

    bb = box->border_box;
    bb.x += ctx->dev_x;
    bb.y = snap_y(ctx_dev_y(ctx, box->border_box.y, own_scroll));

    if (radius < 0.0f) { /* percentage: solved against the border box */
        float side = bb.w < bb.h ? bb.w : bb.h;
        radius = side * (-radius) * 0.01f;
    }

    if (self_visible)
        paint_box_own_content(ctx, pm, box, own_scroll, bb, radius);

    if (box->node && box->node->tag && !strcmp(box->node->tag, "svg")) {
        if (self_visible)
            hpe_svg_paint(pm, box, bb.y);
        return; /* SVG content is painted from the DOM, not from boxes */
    }

    paint_box_clipped_children(ctx, pm, box, scroll, own_scroll);
}

/**
 * @brief 绘制整个文档 / Paint the whole document
 *
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     root 盒子树根，可为 `NULL` / Box tree root, may be `NULL`
 * @param[in]     scroll_y 纵向滚动偏移 / Vertical scroll offset
 * @param[in]     page_background 页面背景色 / Page background colour
 * @param[in]     origin_y 页面视口顶边的设备 y / Device y of the page viewport's top edge
 *
 * @note 先用背景色清屏，再把内容裁剪在 [origin_y, 像素缓冲高) 之内，避免画到浏览器外框上 / Clears with the background colour first, then clips content to `[origin_y, pixmap height)` so it never paints over browser chrome
 */
void hpe_paint_document(HpePixmap *pm, HpeBox *root, float scroll_y, HpeColor page_background,
                        float origin_y)
{
    PaintCtx ctx;
    if (!pm || !pm->pixels)
        return;
    hpe_pixmap_reset_clip(pm);
    hpe_pixmap_clear(pm, page_background);
    if (root) {
        ctx.dev_x = 0.0f;
        ctx.dev_y = origin_y;
        ctx.group_depth = 0;
        /* keep page content below the browser chrome */
        hpe_pixmap_set_clip(pm, hpe_rect(0.0f, origin_y, (float)pm->width,
                                         (float)pm->height - origin_y));
        paint_box(&ctx, pm, root, scroll_y);
        hpe_pixmap_reset_clip(pm);
    }
}

/**
 * @brief 只重绘裁剪区域内的文档 / Repaint only the document inside a clip region
 *
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     root 盒子树根，可为 `NULL` / Box tree root, may be `NULL`
 * @param[in]     scroll_y 纵向滚动偏移 / Vertical scroll offset
 * @param[in]     page_background 页面背景色 / Page background colour
 * @param[in]     clip 设备坐标下的裁剪矩形 / Clip rect in device coordinates
 * @param[in]     origin_y 页面视口顶边的设备 y / Device y of the page viewport's top edge
 *
 * @note 供滚动损伤跟踪使用：只用背景色擦除 clip，再重绘与该区域相交的文档；clip 会被抬到 origin_y 之下 / Used by scroll damage tracking: erases only `clip` with the background, then repaints the document intersected with it; the clip is raised below `origin_y`
 */
void hpe_paint_region(HpePixmap *pm, HpeBox *root, float scroll_y, HpeColor page_background,
                      HpeRect clip, float origin_y)
{
    PaintCtx ctx;
    if (!pm || !pm->pixels || hpe_rect_empty(clip))
        return;
    /* never paint into the chrome strip */
    if (clip.y < origin_y) {
        float bottom = clip.y + clip.h;
        clip.y = origin_y;
        clip.h = bottom - origin_y;
        if (clip.h <= 0.0f)
            return;
    }
    hpe_pixmap_set_clip(pm, clip);
    hpe_fill_rect(pm, clip, page_background);
    if (root) {
        ctx.dev_x = 0.0f;
        ctx.dev_y = origin_y;
        ctx.group_depth = 0;
        paint_box(&ctx, pm, root, scroll_y);
    }
    hpe_pixmap_reset_clip(pm);
}
