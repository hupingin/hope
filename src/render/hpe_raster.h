/* Hope UI Engine - software rasterizer primitives */
#ifndef HPE_RASTER_H
#define HPE_RASTER_H

#include "base/hpe_types.h"
#include "platform/hpe_platform.h"
#include "render/hpe_pixmap.h"

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
void hpe_fill_rect(HpePixmap *pm, HpeRect rect, HpeColor color);

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
void hpe_fill_rect_i(HpePixmap *pm, int x, int y, int w, int h, HpeColor color);

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
void hpe_fill_round_rect(HpePixmap *pm, HpeRect rect, float radius, HpeColor color);

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
 * @see hpe_fill_round_rect
 */
void hpe_stroke_round_rect(HpePixmap *pm, HpeRect rect, float radius, float width, HpeColor color);

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
 * @param[in]     color 颜色 / Colour
 *
 * @note `(x, y)` 是基线上的笔位置，掩码自身的 `baseline` 用于定位顶部 / `(x, y)` is the pen position on the baseline and the mask's own `baseline` is used to locate the top
 */
void hpe_draw_text_mask(HpePixmap *pm, int x, int y, const HpeTextMask *mask, HpeColor color);

/* one colour stop of a gradient */
typedef struct HpeRasterStop {
    float pos; /* 0..1, clamped */
    HpeColor color;
} HpeRasterStop;

/**
 * @brief 按参数 t 在色标列表中采样颜色 / Sample the colour of a stop list at parameter t
 *
 * @param[in]     stops 色标数组 / Colour stop array
 * @param[in]     stop_count 色标个数 / Number of colour stops
 * @param[in]     t 采样位置 0..1 / Sample position in 0..1
 * @param[out]    out 采样得到的颜色 / Receives the sampled colour
 *
 * @note CSS 背景渐变与 SVG 渐变绘制共用的入口 / Shared entry point for CSS background gradients and the SVG gradient painter
 *
 * @see hpe_fill_linear_gradient
 */
void hpe_sample_stops(const HpeRasterStop *stops, int stop_count, float t, HpeColor *out);

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
 * @note 色标按位置扫描采样，无需预先排序 / Stops are sampled by a position scan and need not be pre-sorted
 *
 * @see hpe_fill_round_rect_gradient hpe_sample_stops
 */
void hpe_fill_linear_gradient(HpePixmap *pm, HpeRect rect, float angle_deg,
                              const HpeRasterStop *stops, int stop_count);

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
                                  const HpeRasterStop *stops, int stop_count);

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
void hpe_fill_shadow(HpePixmap *pm, HpeRect rect, float radius, float blur, HpeColor color);

#endif /* HPE_RASTER_H */
