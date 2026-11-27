#include "render/hpe_pixmap.h"

#include <math.h>
#include <string.h>

#include "base/hpe_util.h"

/**
 * @brief 初始化像素缓冲并按需分配像素数组 / Initialise a pixel buffer and allocate its pixel array
 *
 * @details 宽高小于等于 0 时被夹为 0，此时不会分配像素数组而把 `pixels` 置为 `NULL`。
 *          Non-positive dimensions are clamped to 0, in which case no pixel array is allocated and `pixels` is left `NULL`.
 *
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     width 缓冲宽度（像素），非正数夹为 0 / Buffer width in pixels, clamped to 0 when non-positive
 * @param[in]     height 缓冲高度（像素），非正数夹为 0 / Buffer height in pixels, clamped to 0 when non-positive
 *
 * @note 会以默认全幅范围重置裁剪矩形 / The clip rectangle is reset to the full buffer
 *
 * @see hpe_pixmap_free hpe_pixmap_resize
 */
void hpe_pixmap_init(HpePixmap *pm, int width, int height)
{
    if (!pm)
        return;
    pm->width = width > 0 ? width : 0;
    pm->height = height > 0 ? height : 0;
    pm->stride = pm->width * 4;
    pm->alpha_record = 0;
    pm->pixels = pm->width && pm->height
                     ? (uint32_t *)hpe_calloc((size_t)pm->width * (size_t)pm->height,
                                              sizeof(uint32_t))
                     : NULL;
    hpe_pixmap_reset_clip(pm);
}

/**
 * @brief 把像素缓冲调整为新的宽高 / Resize the pixel buffer to new dimensions
 *
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     width 新宽度（像素）/ New width in pixels
 * @param[in]     height 新高度（像素）/ New height in pixels
 *
 * @note 尺寸未变时直接返回；否则释放旧像素再走 `hpe_pixmap_init`，旧内容不保留 / Returns immediately when the size already matches; otherwise frees the old pixels and re-initialises, so previous content is lost
 *
 * @see hpe_pixmap_init hpe_pixmap_free
 */
void hpe_pixmap_resize(HpePixmap *pm, int width, int height)
{
    if (!pm)
        return;
    if (width == pm->width && height == pm->height)
        return;
    hpe_pixmap_free(pm);
    hpe_pixmap_init(pm, width, height);
}

/**
 * @brief 释放像素缓冲占用的内存 / Release the memory held by a pixel buffer
 *
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 *
 * @note 释放后宽、高、步长与 `pixels` 全部清零，可安全重复调用 / Width, height, stride and `pixels` are all cleared, so repeated calls are safe
 *
 * @see hpe_pixmap_init
 */
void hpe_pixmap_free(HpePixmap *pm)
{
    if (!pm)
        return;
    if (pm->pixels)
        hpe_free(pm->pixels);
    pm->pixels = NULL;
    pm->width = pm->height = pm->stride = 0;
}

/**
 * @brief 用纯色填充整个像素缓冲 / Fill the whole pixel buffer with a solid colour
 *
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     color 填充颜色，按 `0xAARRGGBB` 写入 / Fill colour, stored as `0xAARRGGBB`
 *
 * @note 忽略裁剪矩形，直接覆盖全部像素；无像素数组时不做任何事 / The clip rectangle is ignored and every pixel is written; does nothing when there is no pixel array
 */
void hpe_pixmap_clear(HpePixmap *pm, HpeColor color)
{
    uint32_t c;
    size_t count, i;
    if (!pm || !pm->pixels)
        return;
    c = hpe_argb(color);
    count = (size_t)pm->width * (size_t)pm->height;
    for (i = 0; i < count; i++)
        pm->pixels[i] = c;
}

/**
 * @brief 把裁剪矩形重置为整个缓冲 / Reset the clip rectangle to the whole buffer
 *
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 *
 * @see hpe_pixmap_set_clip hpe_pixmap_intersect_clip
 */
void hpe_pixmap_reset_clip(HpePixmap *pm)
{
    if (!pm)
        return;
    pm->clip_x0 = 0;
    pm->clip_y0 = 0;
    pm->clip_x1 = pm->width;
    pm->clip_y1 = pm->height;
}

/**
 * @brief 设置裁剪矩形，替换当前裁剪范围 / Set the clip rectangle, replacing the current clip
 *
 * @details 矩形先按左/上边界 `floor`、右/下边界 `ceil` 取整，再夹到缓冲范围内；空矩形会退化为零宽或零高。
 *          The rectangle is rounded with `floor` on the top/left edges and `ceil` on the bottom/right edges, then clamped to the buffer; a degenerate rectangle collapses to zero width or height.
 *
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     rect 新的裁剪矩形（用户空间坐标）/ New clip rectangle in user-space coordinates
 *
 * @note 与 `hpe_pixmap_intersect_clip` 不同，这里会丢弃原有的裁剪范围 / Unlike `hpe_pixmap_intersect_clip`, the previous clip is discarded
 *
 * @see hpe_pixmap_intersect_clip hpe_pixmap_reset_clip
 */
void hpe_pixmap_set_clip(HpePixmap *pm, HpeRect rect)
{
    int x0, y0, x1, y1;
    if (!pm)
        return;
    x0 = (int)floorf(rect.x);
    y0 = (int)floorf(rect.y);
    x1 = (int)ceilf(rect.x + rect.w);
    y1 = (int)ceilf(rect.y + rect.h);
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > pm->width)
        x1 = pm->width;
    if (y1 > pm->height)
        y1 = pm->height;
    if (x1 < x0)
        x1 = x0;
    if (y1 < y0)
        y1 = y0;
    pm->clip_x0 = x0;
    pm->clip_y0 = y0;
    pm->clip_x1 = x1;
    pm->clip_y1 = y1;
}

/**
 * @brief 把当前裁剪矩形收窄为与给定矩形的交集 / Narrow the current clip to its intersection with a rectangle
 *
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     rect 用于收窄裁剪的矩形（用户空间坐标）/ Rectangle to narrow the clip with, in user-space coordinates
 *
 * @note 嵌套裁剪（如破损区域内的 `<svg>` 视口）必须用它，`hpe_pixmap_set_clip` 会替换掉外层裁剪 / Required for nested clipping such as an `<svg>` viewport inside a damaged region, since `hpe_pixmap_set_clip` would replace the outer clip
 *
 * @see hpe_pixmap_set_clip hpe_pixmap_reset_clip
 */
void hpe_pixmap_intersect_clip(HpePixmap *pm, HpeRect rect)
{
    int x0, y0, x1, y1;
    if (!pm)
        return;
    x0 = (int)floorf(rect.x);
    y0 = (int)floorf(rect.y);
    x1 = (int)ceilf(rect.x + rect.w);
    y1 = (int)ceilf(rect.y + rect.h);
    if (x0 > pm->clip_x0)
        pm->clip_x0 = x0;
    if (y0 > pm->clip_y0)
        pm->clip_y0 = y0;
    if (x1 < pm->clip_x1)
        pm->clip_x1 = x1;
    if (y1 < pm->clip_y1)
        pm->clip_y1 = y1;
    if (pm->clip_x1 < pm->clip_x0)
        pm->clip_x1 = pm->clip_x0;
    if (pm->clip_y1 < pm->clip_y0)
        pm->clip_y1 = pm->clip_y0;
}

/**
 * @brief 把全部像素行纵向平移若干像素 / Shift every pixel row vertically by a number of pixels
 *
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     dy 纵向位移，正数表示内容上移 / Vertical shift, positive scrolls content up
 *
 * @return 实际应用的位移，已夹到 ±高度 / The shift actually applied, clamped to plus or minus the height
 *
 * @note 空出的行保留旧像素，需由调用方重绘 / Vacated rows keep stale pixels and must be repainted by the caller
 *
 * @see hpe_pixmap_shift_v_rect
 */
int hpe_pixmap_shift_v(HpePixmap *pm, int dy)
{
    if (!pm)
        return 0;
    return hpe_pixmap_shift_v_rect(pm, dy, 0, pm->height);
}

/**
 * @brief 只平移行区间 [y0, y1) 的像素 / Shift only the rows in [y0, y1)
 *
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     dy 纵向位移，正数表示内容上移 / Vertical shift, positive scrolls content up
 * @param[in]     y0 起始行（含），小于 0 时夹为 0 / First row, inclusive; clamped to 0 when negative
 * @param[in]     y1 结束行（不含），超过高度时夹到高度 / Row past the last one; clamped to the height
 *
 * @return 实际应用的位移，已夹到 ±区间行数 / The shift actually applied, clamped to plus or minus the row count
 *
 * @note 让地址栏等浏览器外框保持固定、只让下方页面内容滚动 / Lets browser chrome such as the address bar stay pinned while the page content below scrolls
 *
 * @see hpe_pixmap_shift_v
 */
int hpe_pixmap_shift_v_rect(HpePixmap *pm, int dy, int y0, int y1)
{
    size_t row_bytes;
    int rows;
    unsigned char *base;

    if (!pm || !pm->pixels || dy == 0)
        return 0;
    if (y0 < 0)
        y0 = 0;
    if (y1 > pm->height)
        y1 = pm->height;
    rows = y1 - y0;
    if (rows <= 0)
        return 0;
    if (dy > rows)
        dy = rows;
    if (dy < -rows)
        dy = -rows;
    row_bytes = (size_t)pm->stride;
    base = (unsigned char *)pm->pixels + (size_t)y0 * row_bytes;
    if (dy > 0)
        memmove(base, base + (size_t)dy * row_bytes, ((size_t)rows - (size_t)dy) * row_bytes);
    else
        memmove(base + (size_t)(-dy) * row_bytes, base, ((size_t)rows + (size_t)dy) * row_bytes);
    return dy;
}
