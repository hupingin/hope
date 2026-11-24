/* Hope UI Engine - basic types */
#ifndef HPE_TYPES_H
#define HPE_TYPES_H

#include <stddef.h>
#include <stdint.h>

typedef struct HpeRect {
    float x, y, w, h;
} HpeRect;

typedef struct HpeColor {
    uint8_t r, g, b, a;
} HpeColor;

/**
 * @brief 用四个通道分量构造颜色 / Build a colour from four channel components
 *
 * @param[in]     r 红色通道（0~255）/ Red channel, 0 to 255
 * @param[in]     g 绿色通道（0~255）/ Green channel, 0 to 255
 * @param[in]     b 蓝色通道（0~255）/ Blue channel, 0 to 255
 * @param[in]     a 不透明度通道（0 全透明，255 全不透明）/ Alpha channel, 0 fully transparent to 255 fully opaque
 *
 * @return 构造出的颜色 / The constructed colour
 *
 * @note 不做范围校验，超出 0~255 的值按 uint8_t 回绕 / Values outside 0 to 255 are not validated and wrap around as `uint8_t`
 *
 * @see hpe_rgb
 */
static inline HpeColor hpe_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    HpeColor c;
    c.r = r;
    c.g = g;
    c.b = b;
    c.a = a;
    return c;
}

/**
 * @brief 用红绿蓝三分量构造不透明颜色 / Build an opaque colour from red, green and blue
 *
 * @param[in]     r 红色通道（0~255）/ Red channel, 0 to 255
 * @param[in]     g 绿色通道（0~255）/ Green channel, 0 to 255
 * @param[in]     b 蓝色通道（0~255）/ Blue channel, 0 to 255
 *
 * @return alpha 固定为 255 的颜色 / The colour with alpha fixed at 255
 *
 * @note 等价于 `hpe_rgba(r, g, b, 255)` / Equivalent to `hpe_rgba(r, g, b, 255)`
 *
 * @see hpe_rgba
 */
static inline HpeColor hpe_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return hpe_rgba(r, g, b, 255);
}

/* generic font categories resolved from CSS font-family; the platform layer
 * maps each to a concrete typeface on the current OS. */
typedef enum HpeFontFamily {
    HPE_FONT_DEFAULT = 0, /* engine default (same as sans) */
    HPE_FONT_SERIF,
    HPE_FONT_SANS,
    HPE_FONT_MONO
} HpeFontFamily;

/**
 * @brief 用左上角坐标与宽高构造矩形 / Build a rectangle from its top-left corner and size
 *
 * @param[in]     x 左边界 X 坐标 / Left edge X coordinate
 * @param[in]     y 上边界 Y 坐标 / Top edge Y coordinate
 * @param[in]     w 宽度，可正可负 / Width, may be negative
 * @param[in]     h 高度，可正可负 / Height, may be negative
 *
 * @return 构造出的矩形 / The constructed rectangle
 *
 * @note 输入不做规范化，负宽高会原样保留 / Inputs are not normalised, negative widths and heights are kept as given
 *
 * @see hpe_rect_empty
 */
static inline HpeRect hpe_rect(float x, float y, float w, float h)
{
    HpeRect r;
    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    return r;
}

/**
 * @brief 判断矩形是否为空 / Test whether a rectangle is empty
 *
 * @param[in]     r 待检查的矩形，按值传入 / Rectangle to inspect, passed by value
 *
 * @return 宽或高小于等于 0 返回 1 / 1 when the width or the height is not positive
 *
 * @note 空矩形不覆盖任何像素，光栅化阶段会直接跳过 / An empty rectangle covers no pixel and is skipped by the rasteriser
 *
 * @see hpe_rect
 */
static inline int hpe_rect_empty(HpeRect r)
{
    return r.w <= 0.0f || r.h <= 0.0f;
}

#define HPE_UNUSED(x) ((void)(x))

#ifndef HPE_MAX
#define HPE_MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#ifndef HPE_MIN
#define HPE_MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#endif /* HPE_TYPES_H */
