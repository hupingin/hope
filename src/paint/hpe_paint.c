#include "paint/hpe_paint.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "paint/hpe_svg.h"
#include "base/hpe_util.h"
#include "render/hpe_image.h"
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
    /* accumulated scroll offset of every enclosing scroll container, added to
     * the page offset: descendants of a scrolled element move with it */
    float elem_scroll;
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
    return doc_y - scroll - ctx->elem_scroll + ctx->dev_y;
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
    r.y = snap_y(r.y - scroll - ctx->elem_scroll + ctx->dev_y);
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

/**
 * @brief 把索引夹到 `[0, n-1]` / Clamp an index to `[0, n-1]`
 *
 * @param[in]     i 待夹取的索引 / Index to clamp
 * @param[in]     n 有效元素个数 / Number of valid elements
 *
 * @return 夹取后的索引 / The clamped index
 *
 * @note 供双线性采样处理越界样本；`n <= 0` 时返回 0 / Used by bilinear sampling for out-of-range taps; returns 0 when `n <= 0`
 *
 * @see blit_image
 */
static int clamp_idx(int i, int n)
{
    if (n <= 0)
        return 0;
    if (i < 0)
        return 0;
    if (i > n - 1)
        return n - 1;
    return i;
}

/**
 * @brief 把目标矩形夹到像素缓冲的裁剪矩形 / Clamp a destination rect to the pixmap's clip
 *
 * @param[in]     pm 目标像素缓冲 / Target pixel buffer
 * @param[in,out] x0 目标左边界，就地夹取 / Destination left edge, clamped in place
 * @param[in,out] y0 目标上边界，就地夹取 / Destination top edge, clamped in place
 * @param[in,out] w 目标宽度，就地夹取 / Destination width, clamped in place
 * @param[in,out] h 目标高度，就地夹取 / Destination height, clamped in place
 *
 * @return 夹取后仍有面积返回 1，否则 0 / 1 when the clamped rect still has area, otherwise 0
 *
 * @note 与 `hpe_fill_rect_i` 的夹取方式一致 / The clamping matches `hpe_fill_rect_i`
 *
 * @see blit_image
 */
static int clamp_dest_to_clip(HpePixmap *pm, int *x0, int *y0, int *w, int *h)
{
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
    return *w > 0 && *h > 0;
}

/**
 * @brief 2×2 双线性采样（按样本 alpha 加权）/ 2×2 bilinear sample, weighted by each sample's alpha
 *
 * @param[in]     row0 上行像素 / Upper row of pixels
 * @param[in]     row1 下行像素 / Lower row of pixels
 * @param[in]     ix0 左列索引（已夹取）/ Left column index, already clamped
 * @param[in]     ix1 右列索引（已夹取）/ Right column index, already clamped
 * @param[in]     fx 水平小数 / Horizontal fraction
 * @param[in]     fy 垂直小数 / Vertical fraction
 * @param[out]    out_r 加权 R 累加 / Alpha-weighted R sum
 * @param[out]    out_g 加权 G 累加 / Alpha-weighted G sum
 * @param[out]    out_b 加权 B 累加 / Alpha-weighted B sum
 * @param[out]    out_w 权重和（即输出 alpha，0..255）/ Weight sum, which is the output alpha in 0..255
 *
 * @note 每个样本的权重乘以其自身 alpha，透明边缘不会被拖向黑色 / Each sample's weight is scaled by its own alpha so transparent edges are not dragged towards black
 *
 * @see blit_image
 */
static void sample_bilinear(const uint32_t *row0, const uint32_t *row1, int ix0, int ix1, float fx,
                            float fy, float *out_r, float *out_g, float *out_b, float *out_w)
{
    uint32_t p00 = row0[ix0], p10 = row0[ix1], p01 = row1[ix0], p11 = row1[ix1];
    float w00 = (1.0f - fx) * (1.0f - fy) * (float)((p00 >> 24) & 0xFFu);
    float w10 = fx * (1.0f - fy) * (float)((p10 >> 24) & 0xFFu);
    float w01 = (1.0f - fx) * fy * (float)((p01 >> 24) & 0xFFu);
    float w11 = fx * fy * (float)((p11 >> 24) & 0xFFu);

    *out_r = (float)((p00 >> 16) & 0xFFu) * w00 + (float)((p10 >> 16) & 0xFFu) * w10 +
             (float)((p01 >> 16) & 0xFFu) * w01 + (float)((p11 >> 16) & 0xFFu) * w11;
    *out_g = (float)((p00 >> 8) & 0xFFu) * w00 + (float)((p10 >> 8) & 0xFFu) * w10 +
             (float)((p01 >> 8) & 0xFFu) * w01 + (float)((p11 >> 8) & 0xFFu) * w11;
    *out_b = (float)(p00 & 0xFFu) * w00 + (float)(p10 & 0xFFu) * w10 + (float)(p01 & 0xFFu) * w01 +
             (float)(p11 & 0xFFu) * w11;
    *out_w = w00 + w10 + w01 + w11;
}

/**
 * @brief 把一个反预乘样本混合到目标像素 / Blend an un-premultiplied sample onto a destination pixel
 *
 * @param[in,out] dst 目标像素 / Destination pixel
 * @param[in]     r 加权 R 累加 / Alpha-weighted R sum
 * @param[in]     g 加权 G 累加 / Alpha-weighted G sum
 * @param[in]     b 加权 B 累加 / Alpha-weighted B sum
 * @param[in]     wsum 权重和 / Weight sum
 *
 * @note 双线性权重和为 1，故 `wsum` 恰为 0..255 的输出 alpha；颜色先按它反预乘再合成，全不透明时直接覆盖 / The bilinear weights sum to 1, so `wsum` is exactly the output alpha in 0..255; the colour is un-premultiplied by it before compositing, and an opaque result overwrites directly
 *
 * @see sample_bilinear blit_image
 */
static void blend_pixel(uint32_t *dst, float r, float g, float b, float wsum)
{
    uint32_t sa;
    float inv, dr, dg, db;

    if (wsum <= 0.0f)
        return;
    sa = (uint32_t)(wsum + 0.5f);
    if (sa == 0)
        return;
    if (sa > 255u)
        sa = 255u;
    r = r / wsum;
    g = g / wsum;
    b = b / wsum;
    if (sa >= 255u) {
        *dst = 0xFF000000u | ((uint32_t)(r + 0.5f) << 16) | ((uint32_t)(g + 0.5f) << 8) |
               (uint32_t)(b + 0.5f);
        return;
    }
    dr = (float)((*dst >> 16) & 0xFFu);
    dg = (float)((*dst >> 8) & 0xFFu);
    db = (float)(*dst & 0xFFu);
    inv = 255.0f - (float)sa;
    *dst = 0xFF000000u |
           ((uint32_t)((r * (float)sa + dr * inv) / 255.0f + 0.5f) << 16) |
           ((uint32_t)((g * (float)sa + dg * inv) / 255.0f + 0.5f) << 8) |
           (uint32_t)((b * (float)sa + db * inv) / 255.0f + 0.5f);
}

/**
 * @brief 把一个解码图像按目标矩形双线性重采样混合到像素缓冲 / Composite a decoded image onto the pixel buffer with bilinear resampling
 *
 * @details 源图像按源矩形 `sx0..sx1 × sy0..sy1` 双线性采样到目标矩形
 *          `dx..dx+dw × dy..dy+dh`，逐像素按源 alpha 混合，并裁剪到 `pm` 的裁剪矩形。
 *          采样权重各自乘上样本自身的 alpha，因此透明边缘不会被拖向黑色。
 *          The source is bilinearly sampled from `sx0..sx1 × sy0..sy1` into the destination rect
 *          `dx..dx+dw × dy..dy+dh`, blended per pixel by source alpha and clipped to `pm`'s clip
 *          rectangle. Each sample's weight is multiplied by its own alpha, so transparent edges
 *          are not dragged towards black.
 *
 * @param[in,out] pm 目标像素缓冲 / Destination pixel buffer
 * @param[in]     img 源图像（32bpp 0xAARRGGBB，自上而下）/ Source image (32bpp 0xAARRGGBB, top-down)
 * @param[in]     dx 目标矩形左边界（设备坐标）/ Destination left edge in device coordinates
 * @param[in]     dy 目标矩形上边界（设备坐标）/ Destination top edge in device coordinates
 * @param[in]     dw 目标矩形宽度 / Destination width
 * @param[in]     dh 目标矩形高度 / Destination height
 * @param[in]     sx0 源矩形左边界（像素）/ Source left edge in pixels
 * @param[in]     sy0 源矩形上边界（像素）/ Source top edge in pixels
 * @param[in]     sx1 源矩形右边界（像素，不含）/ Source right edge in pixels, exclusive
 * @param[in]     sy1 源矩形下边界（像素，不含）/ Source bottom edge in pixels, exclusive
 *
 * @note 目标或源矩形为空、图像无像素时直接返回 / Returns immediately for an empty destination or source rect or an image with no pixels
 *
 * @see sample_bilinear blend_pixel paint_image_box paint_background_image
 */
static void blit_image(HpePixmap *pm, const HpeImage *img, float dx, float dy, float dw, float dh,
                       float sx0, float sy0, float sx1, float sy1)
{
    int x0, y0, w, h, px, py;
    float sw = sx1 - sx0, sh = sy1 - sy0;

    if (!pm || !pm->pixels || !img || !img->pixels || dw <= 0.0f || dh <= 0.0f)
        return;
    if (sw <= 0.0f || sh <= 0.0f)
        return;

    x0 = (int)floorf(dx);
    y0 = (int)floorf(dy);
    w = (int)ceilf(dw);
    h = (int)ceilf(dh);
    if (!clamp_dest_to_clip(pm, &x0, &y0, &w, &h))
        return;

    for (py = 0; py < h; py++) {
        /* sample at pixel centres: the destination pixel's centre maps to the
         * source coordinate `sy0 + (y + 0.5) / dh * sh - 0.5` in image pixels */
        float sy = sy0 + (((float)(y0 + py) - dy) + 0.5f) / dh * sh - 0.5f;
        int iy = (int)floorf(sy);
        float fy = sy - (float)iy;
        uint32_t *dst_row = pm->pixels + (size_t)(y0 + py) * (size_t)pm->width + (size_t)x0;
        const uint32_t *row0 = img->pixels + (size_t)clamp_idx(iy, img->height) * (size_t)img->width;
        const uint32_t *row1 =
            img->pixels + (size_t)clamp_idx(iy + 1, img->height) * (size_t)img->width;

        for (px = 0; px < w; px++) {
            float sx = sx0 + (((float)(x0 + px) - dx) + 0.5f) / dw * sw - 0.5f;
            int ix = (int)floorf(sx);
            float fx = sx - (float)ix;
            float r, g, b, wsum;

            sample_bilinear(row0, row1, clamp_idx(ix, img->width), clamp_idx(ix + 1, img->width),
                            fx, fy, &r, &g, &b, &wsum);
            blend_pixel(&dst_row[px], r, g, b, wsum);
        }
    }
}

static float clamp01(float v)
{
    if (v < 0.0f)
        return 0.0f;
    if (v > 1.0f)
        return 1.0f;
    return v;
}

/**
 * @brief 按 `object-fit` 求图像的目标尺寸 / Solve the image's destination size under `object-fit`
 *
 * @param[in]     fit 生效的 `object-fit` / The effective `object-fit`
 * @param[in]     iw 图像固有宽 / Intrinsic image width
 * @param[in]     ih 图像固有高 / Intrinsic image height
 * @param[in]     cw 内容盒宽 / Content-box width
 * @param[in]     ch 内容盒高 / Content-box height
 * @param[out]    out_w 目标宽 / Destination width
 * @param[out]    out_h 目标高 / Destination height
 *
 * @note `fill` 拉伸铺满，`none` 保持固有尺寸，`cover`/`contain` 等比缩放，`scale-down` 取较小者 / `fill` stretches, `none` keeps the intrinsic size, `cover`/`contain` scale proportionally, `scale-down` takes the smaller
 *
 * @see object_fit_rects
 */
static void object_fit_size(HpeObjectFit fit, int iw, int ih, float cw, float ch, float *out_w,
                            float *out_h)
{
    float scale;
    if (fit == HPE_OBJECT_FIT_FILL) {
        *out_w = cw;
        *out_h = ch;
        return;
    }
    {
        float sx = cw / (float)iw, sy = ch / (float)ih;
        if (fit == HPE_OBJECT_FIT_NONE)
            scale = 1.0f;
        else if (fit == HPE_OBJECT_FIT_COVER)
            scale = sx > sy ? sx : sy;
        else /* contain, and the smaller branch of scale-down */
            scale = sx < sy ? sx : sy;
        if (fit == HPE_OBJECT_FIT_SCALE_DOWN && scale > 1.0f)
            scale = 1.0f;
    }
    *out_w = (float)iw * scale;
    *out_h = (float)ih * scale;
}

/**
 * @brief 按 `object-fit` 求图像在内容盒内的目标矩形与源矩形 / Compute the destination and source rects of an image under `object-fit`
 *
 * @details 按 `object-fit` 与 `object-position` 计算：`fill` 拉伸铺满，`contain` 等比缩放并完整放入，
 *          `cover` 等比缩放并铺满（裁掉溢出），`none` 保持固有尺寸，`scale-down` 取 `none` 与 `contain` 中较小者。
 *          Computed from `object-fit` and `object-position`: `fill` stretches, `contain` scales
 *          down to fit entirely, `cover` scales to cover (clipping the overflow), `none` keeps
 *          the intrinsic size and `scale-down` takes the smaller of `none` and `contain`.
 *
 * @param[in]     fit 生效的 `object-fit` / The effective `object-fit`
 * @param[in]     pos 生效的 `object-position`（可能未声明）/ The effective `object-position`, possibly undeclared
 * @param[in]     iw 图像固有宽 / Intrinsic image width
 * @param[in]     ih 图像固有高 / Intrinsic image height
 * @param[in]     cw 内容盒宽 / Content-box width
 * @param[in]     ch 内容盒高 / Content-box height
 * @param[out]    out_x 目标矩形左边界（相对内容盒）/ Destination left edge, relative to the content box
 * @param[out]    out_y 目标矩形上边界（相对内容盒）/ Destination top edge, relative to the content box
 * @param[out]    out_w 目标矩形宽 / Destination width
 * @param[out]    out_h 目标矩形高 / Destination height
 * @param[out]    sx0 源矩形左边界（像素）/ Source left edge in pixels
 * @param[out]    sy0 源矩形上边界（像素）/ Source top edge in pixels
 * @param[out]    sx1 源矩形右边界（像素，不含）/ Source right edge in pixels, exclusive
 * @param[out]    sy1 源矩形下边界（像素，不含）/ Source bottom edge in pixels, exclusive
 *
 * @note `contain`/`cover` 保持图像宽高比，因此目标矩形可能小于或大于内容盒 / `contain` and `cover` preserve the aspect ratio, so the destination rect may be smaller or larger than the content box
 *
 * @see blit_image paint_image_box
 */
static void object_fit_rects(HpeObjectFit fit, const HpeBgPos *pos, int iw, int ih, float cw,
                             float ch, float *out_x, float *out_y, float *out_w, float *out_h,
                             float *sx0, float *sy0, float *sx1, float *sy1)
{
    float dw, dh, ox = 0.5f, oy = 0.5f;

    *sx0 = 0.0f;
    *sy0 = 0.0f;
    *sx1 = (float)iw;
    *sy1 = (float)ih;

    if (iw <= 0 || ih <= 0 || cw <= 0.0f || ch <= 0.0f) {
        *out_x = 0.0f;
        *out_y = 0.0f;
        *out_w = cw > 0.0f ? cw : 0.0f;
        *out_h = ch > 0.0f ? ch : 0.0f;
        return;
    }

    object_fit_size(fit, iw, ih, cw, ch, &dw, &dh);

    /* `object-position` defaults to 50% 50% (centred) */
    if (pos && pos->present) {
        ox = pos->x_pct ? pos->x * 0.01f : (cw != dw ? pos->x / (cw - dw) : 0.0f);
        oy = pos->y_pct ? pos->y * 0.01f : (ch != dh ? pos->y / (ch - dh) : 0.0f);
        ox = clamp01(ox);
        oy = clamp01(oy);
    }
    *out_x = (cw - dw) * ox;
    *out_y = (ch - dh) * oy;
    *out_w = dw;
    *out_h = dh;
}

/**
 * @brief 把 `<img>` 的解码内容绘制进内容盒 / Paint a decoded `<img>` into its content box
 *
 * @param[in]     ctx 绘制上下文 / Paint context
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     box 已排版的 `<img>` 盒 / The laid-out `<img>` box
 * @param[in]     scroll 纵向滚动偏移 / Vertical scroll offset
 *
 * @note 内容盒为空、`src` 缺失或解码失败时静默跳过 / Silently skipped when the content box is empty, `src` is missing or the decode failed
 *
 * @see blit_image object_fit_rects
 */
static void paint_image_box(PaintCtx *ctx, HpePixmap *pm, HpeBox *box, float scroll)
{
    HpeStyle *bs = &box->style;
    const char *src = box->node ? hpe_dom_attr(box->node, "src") : NULL;
    const HpeImage *img;
    char path[1024];
    float x, y, w, h, sx0, sy0, sx1, sy1;

    if (!src || !src[0])
        return;
    if (box->content_box.w <= 0.0f || box->content_box.h <= 0.0f)
        return;
    if (!hpe_image_resolve_url(src, path, sizeof(path)))
        return;
    img = hpe_image_cache_get(hpe_image_shared_cache(), path, HPE_IMG_UNKNOWN, -1, -1);
    if (!img || !img->pixels)
        return;

    object_fit_rects(bs->object_fit, &bs->object_pos, img->width, img->height, box->content_box.w,
                     box->content_box.h, &x, &y, &w, &h, &sx0, &sy0, &sx1, &sy1);
    {
        /* `object-fit: cover` / `none` may overflow the content box, which the
         * replaced element clips: narrow the clip for the duration of the blit
         * and restore it afterwards (a `set` would drop an outer clip). */
        int saved[4];
        HpeRect clip;
        saved[0] = pm->clip_x0;
        saved[1] = pm->clip_y0;
        saved[2] = pm->clip_x1;
        saved[3] = pm->clip_y1;
        clip = box->content_box;
        clip.x += ctx->dev_x;
        clip.y = ctx_dev_y(ctx, box->content_box.y, scroll);
        hpe_pixmap_intersect_clip(pm, clip);
        blit_image(pm, img, box->content_box.x + ctx->dev_x + x,
                   ctx_dev_y(ctx, box->content_box.y, scroll) + y, w, h, sx0, sy0, sx1, sy1);
        pm->clip_x0 = saved[0];
        pm->clip_y0 = saved[1];
        pm->clip_x1 = saved[2];
        pm->clip_y1 = saved[3];
    }
}

/* ------------------------------------------------------------------ */
/* background / borders                                                */
/* ------------------------------------------------------------------ */

/**
 * @brief 求 `background-size` 展开后的图像尺寸 / Resolve a background layer's used pixel size
 *
 * @details `auto` 保持固有尺寸；`cover`/`contain` 按与 `object-fit` 相同的等比规则铺满或放入；
 *          单值只给宽时高按固有比例推出。
 *          `auto` keeps the intrinsic size, `cover`/`contain` use the same aspect-preserving
 *          rules as `object-fit`, and a single-value form derives the height from the ratio.
 *
 * @param[in]     bg 背景放置参数（`background-position` / `background-size`）/ Background placement parameters
 * @param[in]     iw 图像固有宽 / Intrinsic image width
 * @param[in]     ih 图像固有高 / Intrinsic image height
 * @param[in]     bw 定位区宽 / Positioning-area width
 * @param[in]     bh 定位区高 / Positioning-area height
 * @param[out]    out_w 计算后的图像宽 / The resulting image width
 * @param[out]    out_h 计算后的图像高 / The resulting image height
 *
 * @see paint_background_image
 */
static void bg_layer_size(const HpeBgPos *bg, int iw, int ih, float bw, float bh, float *out_w,
                          float *out_h)
{
    float w = (float)iw, h = (float)ih;

    if (iw <= 0 || ih <= 0 || bw <= 0.0f || bh <= 0.0f) {
        *out_w = bw > 0.0f ? bw : 0.0f;
        *out_h = bh > 0.0f ? bh : 0.0f;
        return;
    }
    if (bg->present && bg->size_cover) {
        float s = (bw / (float)iw) > (bh / (float)ih) ? (bw / (float)iw) : (bh / (float)ih);
        w = (float)iw * s;
        h = (float)ih * s;
    } else if (bg->present && bg->size_contain) {
        float s = (bw / (float)iw) < (bh / (float)ih) ? (bw / (float)iw) : (bh / (float)ih);
        w = (float)iw * s;
        h = (float)ih * s;
    } else if (bg->present && !bg->size_auto) {
        int have_x = bg->size_x != 0.0f || bg->sx_pct;
        int have_y = bg->size_y != 0.0f || bg->sy_pct;
        if (have_x)
            w = bg->sx_pct ? bw * bg->size_x * 0.01f : bg->size_x;
        if (have_y)
            h = bg->sy_pct ? bh * bg->size_y * 0.01f : bg->size_y;
        /* a single value keeps the intrinsic ratio */
        if (have_x && !have_y && bg->size_x != 0.0f)
            h = (float)ih * (w / (float)iw);
        else if (have_y && !have_x && bg->size_y != 0.0f)
            w = (float)iw * (h / (float)ih);
    }
    *out_w = w;
    *out_h = h;
}

/**
 * @brief 求 `background-repeat` 在两个方向上的平铺次数 / Compute the tile counts for both axes under `background-repeat`
 *
 * @param[in]     repeat 生效的 `background-repeat` / The effective `background-repeat`
 * @param[in]     area_w 定位区宽 / Positioning-area width
 * @param[in]     area_h 定位区高 / Positioning-area height
 * @param[in]     ox 图层左上角 x / Layer top-left x
 * @param[in]     oy 图层左上角 y / Layer top-left y
 * @param[in]     w 图层宽 / Layer width
 * @param[in]     h 图层高 / Layer height
 * @param[out]    out_nx 横向平铺次数 / Horizontal tile count
 * @param[out]    out_ny 纵向平铺次数 / Vertical tile count
 *
 * @note 平铺次数上限 4096，避免小图铺满大盒时爆炸 / The tile count is capped at 4096 so a tiny image over a huge box cannot explode
 *
 * @see paint_background_image
 */
static void bg_tile_counts(HpeBgRepeat repeat, float area_w, float area_h, float ox, float oy,
                           float w, float h, int *out_nx, int *out_ny)
{
    int nx = 1, ny = 1;
    if (repeat == HPE_BG_REPEAT_REPEAT || repeat == HPE_BG_REPEAT_X) {
        nx = (int)ceilf((area_w - ox) / w) + 1;
        if (nx < 1)
            nx = 1;
    }
    if (repeat == HPE_BG_REPEAT_REPEAT || repeat == HPE_BG_REPEAT_Y) {
        ny = (int)ceilf((area_h - oy) / h) + 1;
        if (ny < 1)
            ny = 1;
    }
    if (nx > 4096)
        nx = 4096;
    if (ny > 4096)
        ny = 4096;
    *out_nx = nx;
    *out_ny = ny;
}

/**
 * @brief 平铺绘制一个背景图层 / Tile one background layer across the positioning area
 *
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     img 源图像 / Source image
 * @param[in]     bb 定位区（边框盒）设备矩形 / Positioning area (border box) in device coordinates
 * @param[in]     w 图层宽 / Layer width
 * @param[in]     h 图层高 / Layer height
 * @param[in]     ox 图层左上角 x / Layer top-left x
 * @param[in]     oy 图层左上角 y / Layer top-left y
 * @param[in]     nx 横向平铺次数 / Horizontal tile count
 * @param[in]     ny 纵向平铺次数 / Vertical tile count
 *
 * @note `background-clip` 默认边框盒，因此整体裁剪在定位区之内 / `background-clip` defaults to the border box, so the whole layer is clipped to the positioning area
 *
 * @see blit_image bg_tile_counts
 */
static void bg_blit_tiles(HpePixmap *pm, const HpeImage *img, HpeRect bb, float w, float h, float ox,
                          float oy, int nx, int ny)
{
    int saved[4];
    int tile_x, tile_y;

    saved[0] = pm->clip_x0;
    saved[1] = pm->clip_y0;
    saved[2] = pm->clip_x1;
    saved[3] = pm->clip_y1;
    hpe_pixmap_intersect_clip(pm, bb);
    for (tile_y = 0; tile_y < ny; tile_y++) {
        for (tile_x = 0; tile_x < nx; tile_x++) {
            float dx = bb.x + ox + (float)tile_x * w;
            float dy = bb.y + oy + (float)tile_y * h;
            if (dx > bb.x + bb.w || dy > bb.y + bb.h)
                continue;
            blit_image(pm, img, dx, dy, w, h, 0.0f, 0.0f, (float)img->width, (float)img->height);
        }
    }
    pm->clip_x0 = saved[0];
    pm->clip_y0 = saved[1];
    pm->clip_x1 = saved[2];
    pm->clip_y1 = saved[3];
}

/**
 * @brief 绘制 `background-image: url(...)` 图层 / Paint a `background-image: url(...)` layer
 *
 * @details 按 `background-position` 求图层左上角，按 `background-repeat` 平铺，逐块调用 `blit_image`。
 *          The layer's top-left comes from `background-position` and tiling follows
 *          `background-repeat`, with every tile drawn by `blit_image`.
 *
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     bs 计算样式 / Computed style
 * @param[in]     bb 盒的设备矩形（定位区）/ Device rect of the box, the positioning area
 *
 * @note 解码失败或尺寸退化时静默跳过 / Silently skipped when the decode fails or the layer size degenerates
 *
 * @see bg_layer_size bg_tile_counts bg_blit_tiles paint_background
 */
static void paint_background_image(HpePixmap *pm, HpeStyle *bs, HpeRect bb)
{
    const HpeImage *img;
    char path[1024];
    float w, h, ox, oy;
    int nx, ny;

    if (!bs->background_image.url[0])
        return;
    if (!hpe_image_resolve_url(bs->background_image.url, path, sizeof(path)))
        return;
    img = hpe_image_cache_get(hpe_image_shared_cache(), path, HPE_IMG_UNKNOWN, -1, -1);
    if (!img || !img->pixels)
        return;
    if (bb.w <= 0.0f || bb.h <= 0.0f)
        return;

    bg_layer_size(&bs->bg_pos, img->width, img->height, bb.w, bb.h, &w, &h);
    if (w <= 0.0f || h <= 0.0f)
        return;

    /* `background-position` defaults to 0% 0% (top-left) */
    if (bs->bg_pos.present) {
        ox = bs->bg_pos.x_pct ? (bb.w - w) * bs->bg_pos.x * 0.01f : bs->bg_pos.x;
        oy = bs->bg_pos.y_pct ? (bb.h - h) * bs->bg_pos.y * 0.01f : bs->bg_pos.y;
    } else {
        ox = 0.0f;
        oy = 0.0f;
    }

    bg_tile_counts(bs->bg_repeat, bb.w, bb.h, ox, oy, w, h, &nx, &ny);
    bg_blit_tiles(pm, img, bb, w, h, ox, oy, nx, ny);
}

/**
 * @brief 绘制盒背景 / Paint a box's background
 *
 * @param[in]     bs 计算样式 / Computed style
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     bb 盒的设备矩形 / Device rect of the box
 * @param[in]     radius 圆角半径 / Corner radius
 *
 * @note 背景色先铺底，再叠渐变或外链图像图层；radius > 0.5 时纯色走圆角填充 / The background colour is laid down first, then a gradient or external-image layer on top; a solid fill uses the rounded-rect path when `radius > 0.5`
 */
static void paint_background(HpeStyle *bs, HpePixmap *pm, HpeRect bb, float radius)
{
    /* the colour is the bottom layer; an image / gradient sits above it */
    if (bs->background.a > 0) {
        if (radius > 0.5f)
            hpe_fill_round_rect(pm, bb, radius, bs->background);
        else
            hpe_fill_rect(pm, bb, bs->background);
    }

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
    if (bs->background_image.kind == HPE_BG_URL)
        paint_background_image(pm, bs, bb);
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
    sub.elem_scroll = ctx->elem_scroll;
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
/* Trace a box's paint call when HOPE_TRACE_PAINT is set. */
static void paint_trace_box(HpeBox *box, HpeStyle *bs, HpeRect bb)
{
    if (!getenv("HOPE_TRACE_PAINT"))
        return;
    fprintf(stderr, "paint %s<%s> bb=(%.2f,%.2f %.2fx%.2f) bg=%02x%02x%02x%02x runs=%d\n",
            box->kind == HPE_BOX_TEXT ? "T" : (box->kind == HPE_BOX_INLINE ? "I" : "B"),
            box->node && box->node->tag ? box->node->tag : "?", bb.x, bb.y, bb.w, bb.h,
            bs->background.a, bs->background.r, bs->background.g, bs->background.b,
            box->line_count);
}

/* Blit the glyph runs of every line whose band intersects the clip. */
static void paint_box_line_runs(PaintCtx *ctx, HpePixmap *pm, HpeBox *box, float own_scroll)
{
    int i, r;
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
            float fx;
            int x, y;
            /* atomic inline-block runs are painted via child recursion, not here */
            if (run->atomic || !run->font)
                continue;
            fx = box->content_box.x + ctx->dev_x + run->x;
            x = (int)floorf(fx);
            y = (int)floorf(baseline);
            if (run->color.a == 0 || x > pm->clip_x1)
                continue;
            /* sub-pixel positioning: the quantised fractional phase is baked
             * into the cached mask, so the glyphs keep their AA gradient */
            mask = hpe_font_mask_phase(run->font, run->text, run->len, fx - (float)x);
            if (!mask)
                continue;
            hpe_draw_text_mask(pm, x, y, mask, run->color);
        }
    }
}

/**
 * @brief 把一个长度解算为像素（行内碎片用） / Resolve a length to pixels for an inline fragment
 *
 * @param[in] l 长度 / The length
 * @param[in] base 百分比基准（宿主内容宽度）/ Percentage basis (host content width)
 *
 * @return 像素值；非 px/percent 单位返回 0 / Pixel value; non-px/percent units yield 0
 *
 * @note em/rem 等字体相关单位在绘制期不可得，按 0 处理 / Font-relative units are unavailable at paint time and resolve to 0
 */
static float frag_len_px(HpeLength l, float base)
{
    if (l.unit == HPE_LEN_PX)
        return l.value;
    if (l.unit == HPE_LEN_PCT)
        return l.value * base / 100.0f;
    return 0.0f;
}

/**
 * @brief 求一个盒在盒树中的深度 / Depth of a box in the box tree
 *
 * @param[in] box 目标盒 / The box
 *
 * @return 根为 0 的祖先层数 / Number of ancestors, the root being 0
 */
static int frag_box_depth(HpeBox *box)
{
    int d = 0;
    while (box->parent) {
        d++;
        box = box->parent;
    }
    return d;
}

/**
 * @brief 按盒深度稳定排序一行碎片（祖先先画）/ Stable-sort a line's fragments by box depth (ancestors first)
 *
 * @param[in,out] frags 碎片数组，就地排序 / Fragment array, sorted in place
 * @param[in] n 碎片个数 / Fragment count
 *
 * @note 嵌套行内盒按闭标记顺序入列（内层先入），直接绘制会让外层背景盖住内层；
 *       稳定的插入排序保持同深度碎片按树序 / Nested inline boxes enqueue at close
 *       time (inner first), which would let the outer background overpaint the
 *       inner one; the stable insertion sort keeps same-depth fragments in tree order
 */
static void paint_frags_sort_by_depth(HpeInlineFrag *frags, int n)
{
    int i, j;
    for (i = 1; i < n; i++) {
        HpeInlineFrag key = frags[i];
        int kd = frag_box_depth(key.box);
        j = i - 1;
        while (j >= 0 && frag_box_depth(frags[j].box) > kd) {
            frags[j + 1] = frags[j];
            j--;
        }
        frags[j + 1] = key;
    }
}

/* Paint the per-line background and borders of every inline box that
 * contributed content to this block's lines. */
static void paint_box_inline_frags(PaintCtx *ctx, HpePixmap *pm, HpeBox *box, float own_scroll)
{
    int i, r;
    for (i = 0; i < box->line_count; i++) {
        HpeLine *line = &box->lines[i];
        float line_top = snap_y(ctx_dev_y(ctx, line->y, own_scroll));
        float line_bottom = line_top + line->height;

        if (line_bottom < (float)pm->clip_y0 || line_top > (float)pm->clip_y1)
            continue;

        paint_frags_sort_by_depth(line->frags, line->frag_count);
        for (r = 0; r < line->frag_count; r++) {
            HpeInlineFrag *fr = &line->frags[r];
            HpeStyle *bs = &fr->box->style;
            float base = box->content_box.w;
            float pT = frag_len_px(bs->padding[0], base), pR = frag_len_px(bs->padding[1], base);
            float pB = frag_len_px(bs->padding[2], base), pL = frag_len_px(bs->padding[3], base);
            float bT = bs->border_w[0], bR = bs->border_w[1];
            float bB = bs->border_w[2], bL = bs->border_w[3];
            int start = (fr->flags & 1) != 0, end = (fr->flags & 2) != 0;
            float x0 = box->content_box.x + ctx->dev_x + fr->x0 - (start ? pL + bL : 0.0f);
            float x1 = box->content_box.x + ctx->dev_x + fr->x1 + (end ? pR + bR : 0.0f);
            float y0 = ctx_dev_y(ctx, line->y + line->baseline - fr->ascent - pT - bT,
                                 own_scroll);
            float y1 = ctx_dev_y(ctx, line->y + line->baseline + fr->descent + pB + bB,
                                 own_scroll);
            HpeRect bb = hpe_rect(snap_y(x0), snap_y(y0), snap_y(x1) - snap_y(x0),
                                  snap_y(y1) - snap_y(y0));
            int any_b = (bT > 0.0f || bR > 0.0f || bB > 0.0f || bL > 0.0f);

            if (!any_b && bs->background.a == 0)
                continue;
            if (bs->background.a > 0)
                hpe_fill_rect(pm, bb, bs->background);
            if (bT > 0.0f && bs->border_colors[0].a > 0)
                hpe_fill_rect(pm, hpe_rect(bb.x, bb.y, bb.w, bT), bs->border_colors[0]);
            if (bB > 0.0f && bs->border_colors[2].a > 0)
                hpe_fill_rect(pm, hpe_rect(bb.x, bb.y + bb.h - bB, bb.w, bB),
                              bs->border_colors[2]);
            if (start && bL > 0.0f && bs->border_colors[3].a > 0)
                hpe_fill_rect(pm, hpe_rect(bb.x, bb.y, bL, bb.h), bs->border_colors[3]);
            if (end && bR > 0.0f && bs->border_colors[1].a > 0)
                hpe_fill_rect(pm, hpe_rect(bb.x + bb.w - bR, bb.y, bR, bb.h),
                              bs->border_colors[1]);
        }
    }
}

static void paint_box_own_content(PaintCtx *ctx, HpePixmap *pm, HpeBox *box, float own_scroll,
                                  HpeRect bb, float radius)
{
    HpeStyle *bs = &box->style;

    if (bs->box_shadow.valid)
        paint_box_shadow(pm, box, bb, radius);
    paint_background(bs, pm, bb, radius);
    paint_borders(bs, pm, bb, radius);
    /* a replaced element paints its decoded content inside the content box,
     * above the background and borders but below any generated text */
    if (box->is_replaced)
        paint_image_box(ctx, pm, box, own_scroll);

    paint_trace_box(box, bs, bb);

    paint_list_marker(ctx, pm, box, own_scroll);
    if (bs->text_shadow.valid)
        paint_text_shadow(ctx, pm, box, own_scroll);

    paint_box_inline_frags(ctx, pm, box, own_scroll);
    paint_box_line_runs(ctx, pm, box, own_scroll);
    if (bs->text_decoration != HPE_TD_NONE)
        paint_text_decorations(ctx, pm, box, own_scroll, bs->text_decoration);
}

/**
 * @brief 判定一个子盒的层叠批次 / Classify a child into its stacking batch
 *
 * @param[in] child 子盒 / The child box
 *
 * @return 0 = 负 z 定位盒；1 = 流内静态盒；2 = 浮动盒（附录 E 的 float 层）；
 *         3 = z:auto/0 定位盒或建立层叠上下文的分组盒（opacity < 1 / transform）；
 *         4 = 正 z 定位盒
 *         / 0 = negative-z positioned; 1 = in-flow static; 2 = floated box (the
 *         Appendix E float layer); 3 = z:auto/0 positioned or a grouping box that
 *         establishes a stacking context (opacity < 1 / transform); 4 = positive-z
 *         positioned
 *
 * @note opacity/transform 盒即使非定位也建立层叠上下文，因此像 z:0 一样参与
 *       排序而不是留在流内批次；浮动盒画在流内内容之后、定位盒之前 / A grouping
 *       box establishes a stacking context even when not positioned, so it sorts
 *       like z:0 instead of staying in-flow; floats paint after in-flow content
 *       and before positioned boxes
 */
static int stack_batch(HpeBox *child)
{
    HpeStyle *s = &child->style;
    int positioned = s->position != HPE_POS_STATIC;
    int grouped = (s->opacity < 1.0f) || !s->transform.identity;
    if (positioned && s->z_index < 0)
        return 0;
    if (positioned)
        return s->z_index > 0 ? 4 : 3;
    if (s->float_side != HPE_FLOAT_NONE)
        return 2;
    return grouped ? 3 : 1;
}

/**
 * @brief 按（批次，z-index）升序插入分层序列 / Insert into the stacked sequence by (batch, z-index)
 *
 * @param[in,out] stacked 分层盒指针数组 / Stacked box pointer array
 * @param[in,out] batch 平行的批次号数组 / Parallel batch number array
 * @param[in]     n 数组当前长度 / Current array length
 *
 * @note 稳定插入排序：同批次同 z 保持文档序（CSS 附录 E 的简化层序） / Stable
 *       insertion sort: equal batch and z keep document order (a simplified
 *       CSS Appendix E layering)
 */
static void stacked_insert_sorted(HpeBox **stacked, int *batch, int n)
{
    HpeBox *key;
    int keyb, j;
    if (n <= 1)
        return;
    key = stacked[n - 1];
    keyb = batch[n - 1];
    j = n - 2;
    while (j >= 0 && (batch[j] > keyb ||
                      (batch[j] == keyb && stacked[j]->style.z_index > key->style.z_index))) {
        stacked[j + 1] = stacked[j];
        batch[j + 1] = batch[j];
        j--;
    }
    stacked[j + 1] = key;
    batch[j + 1] = keyb;
}

/**
 * @brief 绘制子盒子树 / Paint the child subtree
 *
 * @note 层序：负 z 定位盒 → 流内静态盒（文档序）→ 浮动盒 → z:auto/0 定位盒与
 *       opacity/transform 分组盒（文档序）→ 正 z 定位盒（升序）。
 *       每个层叠上下文内部独立排序（与规范附录 E 的简化层序一致）
 *       / Layer order: negative-z positioned, in-flow statics (document order),
 *       floats, z:auto/0 positioned plus opacity/transform grouping boxes
 *       (document order), then positive-z positioned (ascending). Each stacking
 *       context sorts independently (a simplified CSS Appendix E layering)
 */
static void paint_box_children(PaintCtx *ctx, HpePixmap *pm, HpeBox *box, float scroll)
{
    HpeBox **stacked = NULL;
    int *batch = NULL;
    size_t sc = 0, scap_b = 0, scap_i = 0;
    HpeBox *child;
    int r;

    for (child = box->first_child; child; child = child->next) {
        int lv = stack_batch(child);
        if (lv == 1) {
            paint_box(ctx, pm, child, scroll);
            continue;
        }
        /* two independent growables: hpe_grow is a no-op once *cap >= need, so
         * sharing one cap counter would leave the second array NULL */
        stacked = (HpeBox **)hpe_grow(stacked, &scap_b, sc + 1, sizeof(HpeBox *));
        batch = (int *)hpe_grow(batch, &scap_i, sc + 1, sizeof(int));
        stacked[sc] = child;
        batch[sc] = lv;
        sc++;
        stacked_insert_sorted(stacked, batch, (int)sc);
    }
    for (r = 0; r < (int)sc; r++)
        paint_box(ctx, pm, stacked[r], scroll);
    if (stacked)
        hpe_free(stacked);
    if (batch)
        hpe_free(batch);
}

/**
 * @brief 绘制元素级纵向滚动条 / Paint an element's vertical scrollbar
 *
 * @details 轨道覆盖元素 padding box 的右侧 `scrollbar-width` 宽条带，滑块高度按
 *           「可见高 / 内容高」的比例算出，位置按当前滚动偏移线性投影。
 *          The track covers a `scrollbar-width` strip down the padding box's right edge; the thumb's height is the visible/content ratio and its position is the scroll offset projected linearly.
 *
 * @param[in]     ctx 绘制上下文 / Paint context
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     box 滚动容器盒 / Scroll-container box
 * @param[in]     own_scroll 本盒所在层的滚动偏移 / The enclosing layer's scroll offset
 *
 * @note 宽度取 `scrollbar-width`（初始值 `auto` = 12px，`none` = 0 即不绘），保证最小滑块长度 20px / The width comes from `scrollbar-width` (initial `auto` = 12px, `none` = 0 so nothing is drawn) and the thumb is at least 20px long
 */
static void paint_box_scrollbar(PaintCtx *ctx, HpePixmap *pm, HpeBox *box, float own_scroll)
{
    float sw = box->style.scrollbar_width;
    float port_h = box->scroll_port_h;
    float content_h = port_h + box->scroll_max;
    HpeRect track, thumb;
    float thumb_h, pos;

    if (!(sw > 0.0f) || !(port_h > 0.0f) || !(content_h > 0.0f))
        return;
    track = box->pad_box;
    track.x += ctx->dev_x;
    track.y = snap_y(ctx_dev_y(ctx, box->pad_box.y, own_scroll));
    track.x = track.x + track.w - sw;
    track.w = sw;

    thumb_h = port_h * (port_h / content_h);
    if (thumb_h < 20.0f)
        thumb_h = 20.0f;
    if (thumb_h > port_h)
        thumb_h = port_h;
    pos = box->scroll_max > 0.0f ? (box->scroll_y / box->scroll_max) * (port_h - thumb_h) : 0.0f;

    /* the track is a light gutter, the thumb a darker slab inset by one pixel */
    hpe_fill_rect(pm, track, hpe_rgba(240, 240, 240, 255));
    thumb = track;
    thumb.y += pos;
    thumb.h = thumb_h;
    thumb.x += 1.0f;
    thumb.w -= 2.0f;
    if (thumb.h > 2.0f)
        thumb.h -= 2.0f;
    hpe_fill_rect(pm, thumb, hpe_rgba(170, 170, 170, 255));
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
 * @note hidden/scroll/auto 会为子树建立裁剪矩形；`scroll`/`auto` 且内容溢出时，子树
 *       按本盒的 `scroll_y` 整体上移，并在裁剪解除后于右缘补绘滚动条
 *       / `hidden`/`scroll`/`auto` establish a clip rect; when `scroll`/`auto` content overflows, the subtree shifts by the box's `scroll_y` and a scrollbar is drawn at the right edge after the clip is lifted
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

    /* descendants of a scrolled element shift up by its own offset */
    ctx->elem_scroll += box->scroll_y;
    paint_box_children(ctx, pm, box, scroll);
    ctx->elem_scroll -= box->scroll_y;

    if (had_clip) {
        pm->clip_x0 = saved[0];
        pm->clip_y0 = saved[1];
        pm->clip_x1 = saved[2];
        pm->clip_y1 = saved[3];
    }
    /* the scrollbar is chrome: drawn after the clip is lifted, on top of the
     * content, only when the container actually overflows */
    if (box->is_scroll_container && box->scroll_max > 0.0f)
        paint_box_scrollbar(ctx, pm, box, own_scroll);
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
        ctx.elem_scroll = 0.0f;
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

/**
 * @brief 绘制页面级纵向滚动条 / Paint the page-level vertical scrollbar
 *
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     view_top 视口顶边的设备 y / Device y of the viewport's top edge
 * @param[in]     view_h 视口高度（像素）/ Viewport height in pixels
 * @param[in]     scroll_y 当前滚动偏移 / Current scroll offset
 * @param[in]     content_h 内容总高 / Total content height
 * @param[in]     width 滚动条宽度（像素），不大于 0 时不绘 / Scrollbar width in pixels; nothing is drawn when not positive
 *
 * @note 纯 chrome：在右缘覆盖绘制，须在内容绘制并解除裁剪后调用 / Pure chrome: drawn over the right edge, called after the content is painted and the clip is lifted
 */
void hpe_paint_page_scrollbar(HpePixmap *pm, float view_top, float view_h, float scroll_y,
                              float content_h, float width)
{
    HpeRect track, thumb;
    float max_scroll, thumb_h, pos;

    if (!pm || !pm->pixels || !(width > 0.0f) || !(view_h > 0.0f))
        return;
    /* no overflow: no scrollbar at all (matching a browser's overlay behaviour) */
    if (!(content_h > view_h))
        return;

    max_scroll = content_h - view_h;
    track = hpe_rect((float)pm->width - width, view_top, width, view_h);

    thumb_h = view_h * (view_h / content_h);
    if (thumb_h < 20.0f)
        thumb_h = 20.0f;
    if (thumb_h > view_h)
        thumb_h = view_h;
    if (scroll_y < 0.0f)
        scroll_y = 0.0f;
    if (scroll_y > max_scroll)
        scroll_y = max_scroll;
    pos = max_scroll > 0.0f ? (scroll_y / max_scroll) * (view_h - thumb_h) : 0.0f;

    /* the track is a light gutter, the thumb a darker slab inset by one pixel,
     * both painted with the 1/64 device grid so scrolled repaints stay exact */
    track.y = snap_y(track.y);
    hpe_fill_rect(pm, track, hpe_rgba(240, 240, 240, 255));
    thumb = track;
    thumb.y += pos;
    thumb.h = thumb_h;
    thumb.x += 1.0f;
    thumb.w -= 2.0f;
    if (thumb.h > 2.0f)
        thumb.h -= 2.0f;
    if (thumb.w < 1.0f)
        thumb.w = 1.0f;
    hpe_fill_rect(pm, thumb, hpe_rgba(170, 170, 170, 255));
}
