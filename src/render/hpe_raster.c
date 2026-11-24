#include "render/hpe_raster.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/hpe_util.h"

/**
 * @brief 把源色按 alpha 混合到目标色上 / Blend a source colour onto a destination colour
 *
 * @param[in]     dst 目标像素 `0x00RRGGBB` / Destination pixel `0x00RRGGBB`
 * @param[in]     src 源颜色 `0x00RRGGBB` / Source colour `0x00RRGGBB`
 * @param[in]     alpha 混合系数 0~255 / Blend factor 0..255
 *
 * @return 不透明的混合结果 `0xFFRRGGBB` / The blended opaque pixel `0xFFRRGGBB`
 *
 * @note alpha 为 0 原样返回目标，大于等于 255 直接取源色 / alpha of 0 returns the destination untouched; alpha of 255 or more returns the source outright
 *
 * @see blend_px_pm
 */
static inline uint32_t blend(uint32_t dst, uint32_t src, uint32_t alpha)
{
    uint32_t inv, dr, dg, db, sr, sg, sb, r, g, b;
    if (alpha == 0)
        return dst;
    if (alpha >= 255)
        return (src & 0x00FFFFFFu) | 0xFF000000u;
    inv = 255u - alpha;
    dr = (dst >> 16) & 0xFFu;
    dg = (dst >> 8) & 0xFFu;
    db = dst & 0xFFu;
    sr = (src >> 16) & 0xFFu;
    sg = (src >> 8) & 0xFFu;
    sb = src & 0xFFu;
    r = (sr * alpha + dr * inv) / 255u;
    g = (sg * alpha + dg * inv) / 255u;
    b = (sb * alpha + db * inv) / 255u;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

/**
 * @brief 用纯色填充一个矩形 / Fill a rectangle with a solid colour
 *
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     rect 矩形区域（用户空间坐标）/ Rectangle in user-space coordinates
 * @param[in]     color 颜色 / Colour
 *
 * @note 坐标按左/上 `floor`、右/下 `ceil` 取整后交给 `hpe_fill_rect_i` / The rectangle is rounded with `floor` on the top/left edges and `ceil` on the bottom/right edges before being passed to `hpe_fill_rect_i`
 *
 * @see hpe_fill_rect_i
 */
void hpe_fill_rect(HpePixmap *pm, HpeRect rect, HpeColor color)
{
    int x0, y0, x1, y1;
    if (!pm || !pm->pixels || color.a == 0)
        return;
    x0 = (int)floorf(rect.x);
    y0 = (int)floorf(rect.y);
    x1 = (int)ceilf(rect.x + rect.w);
    y1 = (int)ceilf(rect.y + rect.h);
    hpe_fill_rect_i(pm, x0, y0, x1 - x0, y1 - y0, color);
}

/**
 * @brief 按整数坐标用纯色填充矩形 / Fill an integer-coordinate rectangle with a solid colour
 *
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     x 矩形左边界 / Rectangle left edge
 * @param[in]     y 矩形上边界 / Rectangle top edge
 * @param[in]     w 矩形宽度 / Rectangle width
 * @param[in]     h 矩形高度 / Rectangle height
 * @param[in]     color 颜色 / Colour
 *
 * @note 矩形会夹到当前裁剪范围内，全不透明时按行整体写入，否则逐像素混合 / The rectangle is clamped to the current clip; a fully opaque fill writes whole rows while a translucent one blends per pixel
 *
 * @see hpe_fill_rect
 */
void hpe_fill_rect_i(HpePixmap *pm, int x, int y, int w, int h, HpeColor color)
{
    int row, col;
    uint32_t src;

    if (!pm || !pm->pixels || color.a == 0 || w <= 0 || h <= 0)
        return;

    /* clamp to the damage-rect clip: moving an edge inward must shrink the
     * extent, never grow it (a rect crossing the clip's top/left edge). */
    if (x < pm->clip_x0) {
        w -= pm->clip_x0 - x;
        x = pm->clip_x0;
    }
    if (y < pm->clip_y0) {
        h -= pm->clip_y0 - y;
        y = pm->clip_y0;
    }
    if (x + w > pm->clip_x1)
        w = pm->clip_x1 - x;
    if (y + h > pm->clip_y1)
        h = pm->clip_y1 - y;
    if (w <= 0 || h <= 0)
        return;

    src = hpe_argb(color);
    if (color.a == 255) {
        uint32_t *row_ptr;
        int i;
        for (row = y; row < y + h; row++) {
            row_ptr = pm->pixels + (size_t)row * (size_t)pm->width + x;
            for (i = 0; i < w; i++)
                row_ptr[i] = src;
        }
        return;
    }

    for (row = y; row < y + h; row++) {
        uint32_t *row_ptr = pm->pixels + (size_t)row * (size_t)pm->width + x;
        for (col = 0; col < w; col++)
            row_ptr[col] = blend(row_ptr[col], src, color.a);
    }
}

/**
 * @brief 把 8 位覆盖率掩码合成到像素缓冲 / Composite an 8-bit coverage mask onto the pixel buffer
 *
 * @details 掩码逐行逐列按覆盖率与颜色 alpha 相乘后混合，超出裁剪范围的像素跳过。
 *          Each mask pixel is blended after multiplying its coverage by the colour alpha; pixels outside the clip are skipped.
 *
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     x 笔位置 X（设备像素）/ Pen X position in device pixels
 * @param[in]     y 基线 Y（设备像素）/ Baseline Y in device pixels
 * @param[in]     mask 字形覆盖率掩码 / Glyph coverage mask
 * @param[in]     color 文字颜色 / Text colour
 *
 * @note `(x, y)` 是基线上的笔位置，掩码自身的 `baseline` 用于定位顶部 / `(x, y)` is the pen position on the baseline and the mask's own `baseline` is used to locate the top
 */
void hpe_draw_text_mask(HpePixmap *pm, int x, int y, const HpeTextMask *mask, HpeColor color)
{
    int row, col;
    int top;
    uint32_t src;

    if (!pm || !pm->pixels || !mask || !mask->coverage || color.a == 0)
        return;

    top = y - mask->baseline;
    src = hpe_argb(color);

    for (row = 0; row < mask->height; row++) {
        int dy = top + row;
        const unsigned char *cov_row;
        uint32_t *dst_row;
        if (dy < pm->clip_y0 || dy >= pm->clip_y1)
            continue;
        cov_row = mask->coverage + (size_t)row * (size_t)mask->width;
        dst_row = pm->pixels + (size_t)dy * (size_t)pm->width;
        for (col = 0; col < mask->width; col++) {
            int dx = x + col;
            uint32_t a;
            if (dx < pm->clip_x0 || dx >= pm->clip_x1)
                continue;
            if (!cov_row[col])
                continue;
            a = ((uint32_t)cov_row[col] * color.a) / 255u;
            dst_row[dx] = blend(dst_row[dx], src, a);
        }
    }
}

/**
 * @brief 按覆盖率把一个颜色混合到单个像素 / Blend a colour into a single pixel by coverage
 *
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     x 像素 X 坐标 / Pixel X coordinate
 * @param[in]     y 像素 Y 坐标 / Pixel Y coordinate
 * @param[in]     c 要混合的颜色 / Colour to blend
 * @param[in]     cov 覆盖率 0..1，会自动夹取 / Coverage in 0..1, automatically clamped
 *
 * @note 越界、被裁剪或覆盖率非正的像素直接跳过；写入结果始终为不透明 / Pixels out of bounds, clipped away or with non-positive coverage are skipped; the written pixel is always opaque
 *
 * @see blend
 */

static void blend_px_pm(HpePixmap *pm, int x, int y, HpeColor c, float cov)
{
    uint32_t *dst, d;
    float a, ia, dr, dg, db;

    if (!pm || !pm->pixels)
        return;
    if (x < pm->clip_x0 || x >= pm->clip_x1 || y < pm->clip_y0 || y >= pm->clip_y1)
        return;
    if (x < 0 || y < 0 || x >= pm->width || y >= pm->height)
        return;
    if (cov <= 0.0f)
        return;
    if (cov > 1.0f)
        cov = 1.0f;
    a = cov * ((float)c.a / 255.0f);
    if (a <= 0.0f)
        return;
    dst = &pm->pixels[(size_t)y * (size_t)pm->width + (size_t)x];
    d = *dst;
    dr = (float)((d >> 16) & 0xFFu);
    dg = (float)((d >> 8) & 0xFFu);
    db = (float)(d & 0xFFu);
    ia = 1.0f - a;
    *dst = 0xFF000000u |
           ((uint32_t)((float)c.r * a + dr * ia + 0.5f) << 16) |
           ((uint32_t)((float)c.g * a + dg * ia + 0.5f) << 8) |
           (uint32_t)((float)c.b * a + db * ia + 0.5f);
}

/**
 * @brief 求像素中心到圆角矩形轮廓的有符号距离 / Compute the signed distance from a pixel centre to a rounded-rectangle outline
 *
 * @param[in]     px 像素中心 X 坐标 / Pixel centre X
 * @param[in]     py 像素中心 Y 坐标 / Pixel centre Y
 * @param[in]     cx 矩形中心 X / Rectangle centre X
 * @param[in]     cy 矩形中心 Y / Rectangle centre Y
 * @param[in]     hw 矩形半宽 / Rectangle half width
 * @param[in]     hh 矩形半高 / Rectangle half height
 * @param[in]     r 圆角半径 / Corner radius
 *
 * @return 有符号距离，矩形内部为负 / Signed distance, negative inside the rectangle
 */
static float rrect_sdf(float px, float py, float cx, float cy, float hw, float hh, float r)
{
    float dx = fabsf(px - cx) - (hw - r);
    float dy = fabsf(py - cy) - (hh - r);
    float ax = dx > 0.0f ? dx : 0.0f;
    float ay = dy > 0.0f ? dy : 0.0f;
    return sqrtf(ax * ax + ay * ay) - r;
}

/**
 * @brief 按圆角矩形的距离场逐像素绘制填充或描边 / Rasterise a rounded rectangle pixel by pixel from its distance field
 *
 * @details 半径会夹到半宽/半高之内；描边时覆盖率来自到环带 `[-width, 0]` 的距离，否则来自到轮廓的距离。
 *          The radius is clamped to the half width and half height; for a stroke the coverage comes from the distance to the band `[-width, 0]`, otherwise from the distance to the outline.
 *
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     rect 目标矩形（用户空间坐标）/ Target rectangle in user-space coordinates
 * @param[in]     r 圆角半径 / Corner radius
 * @param[in]     width 描边宽度；填充时忽略 / Stroke width, ignored when filling
 * @param[in]     color 颜色 / Colour
 * @param[in]     stroke 非 0 表示画描边环，0 表示实心填充 / Non-zero draws a stroke ring, 0 fills solid
 *
 * @note 绘制范围在矩形四周各外扩 1 像素以容纳抗锯齿边 / The scan range extends one pixel beyond the rectangle on every side to accommodate the anti-aliased edge
 *
 * @see rrect_sdf blend_px_pm
 */
static void rrect_span(HpePixmap *pm, HpeRect rect, float r, float width, HpeColor color,
                       int stroke)
{
    int x0, y0, x1, y1, px, py;
    float cx = rect.x + rect.w * 0.5f, cy = rect.y + rect.h * 0.5f;
    float hw = rect.w * 0.5f, hh = rect.h * 0.5f;

    if (rect.w <= 0.0f || rect.h <= 0.0f)
        return;
    if (r > hw)
        r = hw;
    if (r > hh)
        r = hh;
    x0 = (int)floorf(rect.x) - 1;
    y0 = (int)floorf(rect.y) - 1;
    x1 = (int)ceilf(rect.x + rect.w) + 1;
    y1 = (int)ceilf(rect.y + rect.h) + 1;
    for (py = y0; py < y1; py++) {
        for (px = x0; px < x1; px++) {
            float d = rrect_sdf((float)px + 0.5f, (float)py + 0.5f, cx, cy, hw, hh, r);
            if (stroke) {
                /* the ring spans d in [-width, 0]; distance to that band is
                 * |d + width/2| - width/2 */
                float band = fabsf(d + width * 0.5f) - width * 0.5f;
                blend_px_pm(pm, px, py, color, 0.5f - band);
            } else {
                blend_px_pm(pm, px, py, color, 0.5f - d);
            }
        }
    }
}

/**
 * @brief 用纯色填充抗锯齿圆角矩形 / Fill an anti-aliased rounded rectangle with a solid colour
 *
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     rect 目标矩形（用户空间坐标）/ Target rectangle in user-space coordinates
 * @param[in]     radius 圆角半径 / Corner radius
 * @param[in]     color 颜色 / Colour
 *
 * @note 半径不超过 0.5 时退化为直角矩形，直接调用 `hpe_fill_rect` / A radius of 0.5 or less degenerates to a square rectangle via `hpe_fill_rect`
 *
 * @see hpe_stroke_round_rect hpe_fill_rect
 */
void hpe_fill_round_rect(HpePixmap *pm, HpeRect rect, float radius, HpeColor color)
{
    if (radius <= 0.5f) {
        hpe_fill_rect(pm, rect, color);
        return;
    }
    rrect_span(pm, rect, radius, 0.0f, color, 0);
}

/**
 * @brief 用纯色画圆角矩形的内描边环 / Stroke an inner border ring of a rounded rectangle
 *
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     rect 目标矩形（用户空间坐标）/ Target rectangle in user-space coordinates
 * @param[in]     radius 圆角半径 / Corner radius
 * @param[in]     width 描边宽度 / Stroke width
 * @param[in]     color 颜色 / Colour
 *
 * @note 宽度非正时不做任何事；半径不超过 0.5 时改为画四条直角边带 / Does nothing for a non-positive width; a radius of 0.5 or less draws four axis-aligned bands instead
 *
 * @see hpe_fill_round_rect rrect_span
 */
void hpe_stroke_round_rect(HpePixmap *pm, HpeRect rect, float radius, float width, HpeColor color)
{
    if (width <= 0.0f)
        return;
    if (radius <= 0.5f) {
        /* square corners: four axis-aligned bands */
        hpe_fill_rect(pm, hpe_rect(rect.x, rect.y, rect.w, width), color);
        hpe_fill_rect(pm, hpe_rect(rect.x, rect.y + rect.h - width, rect.w, width), color);
        hpe_fill_rect(pm, hpe_rect(rect.x, rect.y, width, rect.h), color);
        hpe_fill_rect(pm, hpe_rect(rect.x + rect.w - width, rect.y, width, rect.h), color);
        return;
    }
    rrect_span(pm, rect, radius, width, color, 1);
}

/**
 * @brief 按参数 t 在色标列表中采样颜色 / Sample the colour of a stop list at parameter t
 *
 * @details t 落在首个色标之前或末个色标之后时取端点颜色，否则在相邻两色标间按位置线性插值。
 *          A t before the first stop or after the last one takes the end colour; otherwise the colour is linearly interpolated between the two neighbouring stops.
 *
 * @param[in]     stops 色标数组 / Colour stop array
 * @param[in]     n 色标个数；非正时写出全透明黑 / Number of stops, writing transparent black when non-positive
 * @param[in]     t 采样位置 0..1 / Sample position in 0..1
 * @param[out]    out 采样得到的颜色 / Receives the sampled colour
 *
 * @note 两色标位置重合时按前一个色标取色 / Coincident stops take the earlier stop's colour
 */

static void stop_sample(const HpeRasterStop *stops, int n, float t, HpeColor *out)
{
    int i;
    float prev_pos, span;
    if (n <= 0) {
        *out = hpe_rgba(0, 0, 0, 0);
        return;
    }
    if (n == 1 || t <= stops[0].pos) {
        *out = stops[0].color;
        return;
    }
    if (t >= stops[n - 1].pos) {
        *out = stops[n - 1].color;
        return;
    }
    for (i = 1; i < n; i++) {
        if (t <= stops[i].pos) {
            float f;
            prev_pos = stops[i - 1].pos;
            span = stops[i].pos - prev_pos;
            f = span > 1e-6f ? (t - prev_pos) / span : 0.0f;
            out->r = (uint8_t)(stops[i - 1].color.r +
                               (stops[i].color.r - stops[i - 1].color.r) * f);
            out->g = (uint8_t)(stops[i - 1].color.g +
                               (stops[i].color.g - stops[i - 1].color.g) * f);
            out->b = (uint8_t)(stops[i - 1].color.b +
                               (stops[i].color.b - stops[i - 1].color.b) * f);
            out->a = (uint8_t)(stops[i - 1].color.a +
                               (stops[i].color.a - stops[i - 1].color.a) * f);
            return;
        }
    }
    *out = stops[n - 1].color;
}

/**
 * @brief 按参数 t 在色标列表中采样颜色 / Sample the colour of a stop list at parameter t
 *
 * @param[in]     stops 色标数组 / Colour stop array
 * @param[in]     stop_count 色标个数 / Number of colour stops
 * @param[in]     t 采样位置 0..1 / Sample position in 0..1
 * @param[out]    out 采样得到的颜色 / Receives the sampled colour
 *
 * @note CSS 背景渐变与 SVG 渐变绘制共用的入口，实现在 `stop_sample` / Shared entry point for CSS background gradients and the SVG gradient painter, implemented by `stop_sample`
 *
 * @see stop_sample
 */
void hpe_sample_stops(const HpeRasterStop *stops, int stop_count, float t, HpeColor *out)
{
    stop_sample(stops, stop_count, t, out);
}

/**
 * @brief 在矩形内填充线性渐变 / Fill a rectangle with a linear gradient
 *
 * @details 渐变轴由 `angle_deg` 决定，方向向量为 `(sin a, -cos a)`，渐变线长度按 `
 *          w*sin a
 *          +
 *          h*cos a
 *          ` 覆盖到四角。
 *          The gradient axis comes from `angle_deg` with direction vector `(sin a, -cos a)`; the gradient line is sized as `
 *          w*sin a
 *          +
 *          h*cos a
 *          ` so the corners are covered.
 *
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     rect 目标矩形（用户空间坐标）/ Target rectangle in user-space coordinates
 * @param[in]     angle_deg 渐变角度（度），0 向上、90 向右 / Gradient angle in degrees, 0 is upward and 90 rightward
 * @param[in]     stops 色标数组 / Colour stop array
 * @param[in]     stop_count 色标个数 / Number of colour stops
 *
 * @note 色标按位置扫描采样，无需预先排序；全透明色标跳过 / Stops are sampled by a position scan so they need not be pre-sorted; fully transparent stops are skipped
 *
 * @see hpe_fill_round_rect_gradient hpe_sample_stops
 */
void hpe_fill_linear_gradient(HpePixmap *pm, HpeRect rect, float angle_deg,
                              const HpeRasterStop *stops, int stop_count)
{
    int x0, y0, x1, y1, px, py;
    float rad = angle_deg * 3.14159265f / 180.0f;
    float dx = sinf(rad), dy = -cosf(rad);
    float cx = rect.x + rect.w * 0.5f, cy = rect.y + rect.h * 0.5f;
    float len = fabsf(rect.w * dx) + fabsf(rect.h * dy);
    float t0 = cx * dx + cy * dy - len * 0.5f;

    if (!pm || !pm->pixels || stop_count <= 0 || len <= 0.0f)
        return;
    x0 = (int)floorf(rect.x);
    y0 = (int)floorf(rect.y);
    x1 = (int)ceilf(rect.x + rect.w);
    y1 = (int)ceilf(rect.y + rect.h);
    if (x0 < pm->clip_x0)
        x0 = pm->clip_x0;
    if (y0 < pm->clip_y0)
        y0 = pm->clip_y0;
    if (x1 > pm->clip_x1)
        x1 = pm->clip_x1;
    if (y1 > pm->clip_y1)
        y1 = pm->clip_y1;
    if (x0 >= x1 || y0 >= y1)
        return;

    for (py = y0; py < y1; py++) {
        uint32_t *row = pm->pixels + (size_t)py * (size_t)pm->width;
        for (px = x0; px < x1; px++) {
            float proj = ((float)px + 0.5f) * dx + ((float)py + 0.5f) * dy;
            float t = (proj - t0) / len;
            HpeColor c;
            if (t < 0.0f)
                t = 0.0f;
            if (t > 1.0f)
                t = 1.0f;
            stop_sample(stops, stop_count, t, &c);
            if (c.a == 0)
                continue;
            row[px] = blend(row[px], hpe_argb(c), c.a);
        }
    }
}

/* gradient clipped to a rounded rect: reuse the coverage of the filled rrect */
/**
 * @brief 把线性渐变填满一块临时像素缓冲 / Fill a scratch pixel buffer with the linear gradient
 *
 * @param[in,out] local 临时像素缓冲 / Scratch pixel buffer
 * @param[in]     dx 渐变方向向量 X 分量 / X component of the gradient direction
 * @param[in]     dy 渐变方向向量 Y 分量 / Y component of the gradient direction
 * @param[in]     stops 色标数组 / Colour stop array
 * @param[in]     stop_count 色标个数 / Number of colour stops
 *
 * @note 渐变线以缓冲区中心为原点、按其尺寸覆盖，写入的颜色保留 alpha / The gradient line is centred on the buffer and sized to cover it; the written colours keep their alpha
 *
 * @see gradient_composite_rrect
 */
static void gradient_fill_scratch(HpePixmap *local, float dx, float dy, const HpeRasterStop *stops,
                                  int stop_count)
{
    int w = local->width, h = local->height;
    float cx = (float)w * 0.5f, cy = (float)h * 0.5f;
    float len = fabsf((float)w * dx) + fabsf((float)h * dy);
    float t0 = cx * dx + cy * dy - len * 0.5f;
    int px, py;
    for (py = 0; py < h; py++) {
        uint32_t *row = local->pixels + (size_t)py * (size_t)w;
        for (px = 0; px < w; px++) {
            float proj = ((float)px + 0.5f) * dx + ((float)py + 0.5f) * dy;
            float t = len > 0.0f ? (proj - t0) / len : 0.0f;
            HpeColor c;
            if (t < 0.0f)
                t = 0.0f;
            if (t > 1.0f)
                t = 1.0f;
            stop_sample(stops, stop_count, t, &c);
            row[px] = hpe_argb(c);
        }
    }
}

/**
 * @brief 按圆角矩形覆盖率把临时渐变合成到目标缓冲 / Composite the scratch gradient onto the target through the rounded-rect coverage
 *
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     local 渐变临时缓冲（源）/ Scratch gradient buffer, the source
 * @param[in]     rect 目标圆角矩形（用户空间坐标）/ Target rounded rectangle in user-space coordinates
 * @param[in]     radius 圆角半径 / Corner radius
 *
 * @note 覆盖率来自与实心填充相同的距离场，采样坐标相对矩形左上角偏移 1 像素 / Coverage comes from the same distance field the solid fill uses, sampling with a one-pixel offset from the rectangle's top-left corner
 *
 * @see rrect_sdf gradient_fill_scratch
 */
static void gradient_composite_rrect(HpePixmap *pm, const HpePixmap *local, HpeRect rect,
                                     float radius)
{
    int w = local->width, h = local->height;
    float hw = rect.w * 0.5f, hh = rect.h * 0.5f;
    float rcx = rect.x + hw, rcy = rect.y + hh;
    float r = radius;
    int x0, y0, x1, y1, px, py;
    if (r > hw)
        r = hw;
    if (r > hh)
        r = hh;
    x0 = (int)floorf(rect.x) - 1;
    y0 = (int)floorf(rect.y) - 1;
    x1 = (int)ceilf(rect.x + rect.w) + 1;
    y1 = (int)ceilf(rect.y + rect.h) + 1;
    for (py = y0; py < y1; py++) {
        for (px = x0; px < x1; px++) {
            float d = rrect_sdf((float)px + 0.5f, (float)py + 0.5f, rcx, rcy, hw, hh, r);
            float cov = 0.5f - d;
            int sx, sy;
            HpeColor c;
            uint32_t p;
            if (cov <= 0.0f)
                continue;
            if (cov > 1.0f)
                cov = 1.0f;
            sx = px - (int)floorf(rect.x) + 1;
            sy = py - (int)floorf(rect.y) + 1;
            if (sx < 0 || sy < 0 || sx >= w || sy >= h)
                continue;
            p = local->pixels[(size_t)sy * (size_t)w + (size_t)sx];
            c.r = (uint8_t)((p >> 16) & 0xFFu);
            c.g = (uint8_t)((p >> 8) & 0xFFu);
            c.b = (uint8_t)(p & 0xFFu);
            c.a = (uint8_t)((p >> 24) & 0xFFu);
            blend_px_pm(pm, px, py, c, cov);
        }
    }
}

/**
 * @brief 在圆角矩形内填充线性渐变 / Fill a rounded rectangle with a linear gradient
 *
 * @details 先把渐变画进与圆角矩形同尺寸的临时缓冲，再按圆角矩形的距离场覆盖率合成回目标缓冲。
 *          The gradient is first painted into a scratch buffer the size of the rounded rectangle, then composited back through the rounded-rectangle distance-field coverage.
 *
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     rect 目标矩形（用户空间坐标）/ Target rectangle in user-space coordinates
 * @param[in]     radius 圆角半径 / Corner radius
 * @param[in]     angle_deg 渐变角度（度），0 向上、90 向右 / Gradient angle in degrees, 0 is upward and 90 rightward
 * @param[in]     stops 色标数组 / Colour stop array
 * @param[in]     stop_count 色标个数 / Number of colour stops
 *
 * @note 半径不超过 0.5 时退化为普通线性渐变，直接调用 `hpe_fill_linear_gradient` / A radius of 0.5 or less degenerates to a plain linear gradient via `hpe_fill_linear_gradient`
 *
 * @see hpe_fill_linear_gradient hpe_fill_round_rect
 */
void hpe_fill_round_rect_gradient(HpePixmap *pm, HpeRect rect, float radius, float angle_deg,
                                  const HpeRasterStop *stops, int stop_count)
{
    HpePixmap local;
    float rad = angle_deg * 3.14159265f / 180.0f;
    float dx = sinf(rad), dy = -cosf(rad);
    int w, h;

    if (!pm || !pm->pixels || stop_count <= 0)
        return;
    if (radius <= 0.5f) {
        hpe_fill_linear_gradient(pm, rect, angle_deg, stops, stop_count);
        return;
    }
    if (rect.w <= 0.0f || rect.h <= 0.0f)
        return;

    /* Paint the gradient into a scratch pixmap the size of the rrect bounds so
     * the rounded coverage math stays in one place, then composite it back with
     * the same SDF coverage the solid fill uses. */
    w = (int)ceilf(rect.w) + 2;
    h = (int)ceilf(rect.h) + 2;
    if (w <= 0 || h <= 0)
        return;
    memset(&local, 0, sizeof(local));
    local.width = w;
    local.height = h;
    local.stride = w * 4;
    local.pixels = (uint32_t *)hpe_calloc((size_t)w * (size_t)h, sizeof(uint32_t));

    gradient_fill_scratch(&local, dx, dy, stops, stop_count);
    gradient_composite_rrect(pm, &local, rect, radius);
    hpe_free(local.pixels);
}

/**
 * @brief 在矩形周围绘制柔和投影 / Draw a soft drop shadow around a rectangle
 *
 * @details 用若干层同心圆角矩形（alpha 由外向内递增）近似高斯模糊，最内层保持完全不透明。
 *          A gaussian blur is approximated by concentric rounded rectangles whose alpha increases inward, with the innermost layer fully opaque.
 *
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     rect 投影矩形（用户空间坐标）/ The rectangle casting the shadow, in user-space coordinates
 * @param[in]     radius 圆角半径 / Corner radius
 * @param[in]     blur 模糊半径 / Blur radius
 * @param[in]     color 颜色 / Colour
 *
 * @note 模糊半径小于 1 时退化为一次实心圆角矩形填充；层数夹在 1~24 / A blur below 1 degenerates to a single solid rounded-rectangle fill; the ring count is clamped to 1..24
 *
 * @see hpe_fill_round_rect
 */
void hpe_fill_shadow(HpePixmap *pm, HpeRect rect, float radius, float blur, HpeColor color)
{
    int steps, i;
    if (!pm || !pm->pixels || color.a == 0)
        return;
    if (blur < 1.0f) {
        hpe_fill_round_rect(pm, rect, radius, color);
        return;
    }
    steps = (int)(blur * 0.5f);
    if (steps < 1)
        steps = 1;
    if (steps > 24)
        steps = 24;
    /* draw from the outside in, so the opaque core lands on top */
    for (i = steps; i >= 0; i--) {
        float t = (float)i / (float)steps;
        float grow = blur * t;
        float a = (1.0f - t) * (1.0f - t);
        HpeColor c = color;
        HpeRect r = hpe_rect(rect.x - grow, rect.y - grow, rect.w + grow * 2.0f,
                             rect.h + grow * 2.0f);
        c.a = (uint8_t)((float)color.a * a);
        if (i == 0)
            c.a = color.a;
        if (c.a == 0)
            continue;
        hpe_fill_round_rect(pm, r, radius + grow, c);
    }
}
