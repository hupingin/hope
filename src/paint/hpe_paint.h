/* Hope UI Engine - painting */
#ifndef HPE_PAINT_H
#define HPE_PAINT_H

#include "layout/hpe_layout.h"
#include "render/hpe_pixmap.h"

/**
 * @brief 绘制整个文档 / Paint the whole document
 *
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     root 盒子树根，可为 `NULL` / Box tree root, may be `NULL`
 * @param[in]     scroll_y 纵向滚动偏移 / Vertical scroll offset
 * @param[in]     page_background 页面背景色 / Page background colour
 * @param[in]     origin_y 页面视口顶边的设备 y / Device y of the page viewport's top edge
 *
 * @note 内容被裁剪在 [origin_y, 像素缓冲高) 之内，避免画到浏览器外框上 / Content is clipped to `[origin_y, pixmap height)` so it never paints over browser chrome
 */
void hpe_paint_document(HpePixmap *pm, HpeBox *root, float scroll_y, HpeColor page_background,
                        float origin_y);

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
 * @note 供滚动损伤跟踪使用，clip 与页面视口求交 / Used by scroll damage tracking; the clip is intersected with the page viewport
 */
void hpe_paint_region(HpePixmap *pm, HpeBox *root, float scroll_y, HpeColor page_background,
                      HpeRect clip, float origin_y);

#endif /* HPE_PAINT_H */
