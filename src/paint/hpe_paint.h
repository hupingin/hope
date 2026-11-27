/* Hope UI Engine - painting */
#ifndef HPE_PAINT_H
#define HPE_PAINT_H

#include "layout/hpe_layout.h"
#include "render/hpe_pixmap.h"

/* ---------- text selection highlight ---------- */

/**
 * @brief 一个文本盒的选中字节区间 / Selected byte range of one text box
 */
typedef struct HpePaintSelEntry {
    const struct HpeBox *box; /* 选区命中的文本盒 / The selected text box */
    int start, end;           /* box->text 内的选中字节区间 [start, end) / Byte range within box->text */
} HpePaintSelEntry;

/**
 * @brief 本帧选区描述：文档序排列的逐盒区间表 / This frame's selection: per-box ranges in document order
 */
typedef struct HpePaintSel {
    const HpePaintSelEntry *entries; /* 区间表，可为 NULL / Range table, may be NULL */
    int count;                       /* 区间个数 / Number of entries */
} HpePaintSel;

/**
 * @brief 设置绘制阶段的选区描述（下一帧起生效）/ Set the paint-time selection descriptor (effective from the next frame)
 *
 * @param[in]     sel 选区描述，NULL 清除 / The selection descriptor, NULL to clear
 *
 * @note 指针由调用方持有且须在绘制期间保持有效；引擎在每次重绘前重建并传入 / The pointer is caller-owned and must stay valid during painting; the engine rebuilds and passes it before every repaint
 */
void hpe_paint_set_selection(const HpePaintSel *sel);

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

/**
 * @brief 绘制页面级纵向滚动条 / Paint the page-level vertical scrollbar
 *
 * @details 视口右缘的一条 `width` 宽槽：轨道浅灰，滑块深灰、按 `scroll_y / max_scroll`
 *          在 `[0, track_h - thumb_h]` 内线性投影，滑块高 = 视口高 ×（视口/内容）。
 *          无溢出（`content_h <= view_h`）时直接返回不绘。
 *          A `width`-wide gutter down the viewport's right edge: a light track with a darker
 *          thumb projected linearly into `[0, track_h - thumb_h]` by `scroll_y / max_scroll`;
 *          the thumb's height is the viewport/content ratio. Returns without drawing when the
 *          content does not overflow (`content_h <= view_h`).
 *
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     view_top 视口顶边的设备 y / Device y of the viewport's top edge
 * @param[in]     view_h 视口高度（像素）/ Viewport height in pixels
 * @param[in]     scroll_y 当前滚动偏移 / Current scroll offset
 * @param[in]     content_h 内容总高 / Total content height
 * @param[in]     width 滚动条宽度（像素），不大于 0 时不绘 / Scrollbar width in pixels; nothing is drawn when not positive
 *
 * @note 纯 chrome：调用方应在内容绘制并解除裁剪后于右缘覆盖调用 / Pure chrome: the caller draws it over the right edge after the content is painted and the clip is lifted
 *
 * @see hpe_paint_document
 */
void hpe_paint_page_scrollbar(HpePixmap *pm, float view_top, float view_h, float scroll_y,
                              float content_h, float width);

#endif /* HPE_PAINT_H */
