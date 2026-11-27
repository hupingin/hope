/* Hope UI Engine - <details>/<dialog> painting (v0.26)
 *
 * Draws the disclosure triangle for <summary> and the modal backdrop for an
 * open modal <dialog>. Box backgrounds/borders come from ordinary painting.
 */
#ifndef HPE_PAINT_DETAILS_H
#define HPE_PAINT_DETAILS_H

#include "layout/hpe_layout.h"
#include "render/hpe_pixmap.h"

/**
 * @brief 记录当前模态 `<dialog>` 节点供绘制使用 / Record the current modal `<dialog>` for painting
 *
 * @param[in] node 模态对话框节点或 `NULL` / The modal dialog node, or `NULL`
 *
 * @note 引擎在每帧绘制前调用；指针只在本帧读取 / The engine calls this before each frame; the pointer is read during that frame only
 */
void hpe_paint_set_modal_dialog(HpeDomNode *node);

/**
 * @brief 读取当前登记的模态 `<dialog>` / Read the registered modal `<dialog>`
 *
 * @return 模态对话框节点，无则 `NULL` / The modal dialog node, or `NULL`
 */
HpeDomNode *hpe_paint_modal_dialog(void);

/**
 * @brief 绘制 `<summary>` 的展开指示三角 / Paint the disclosure triangle of a `<summary>`
 *
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     box summary 盒 / The summary box
 * @param[in]     bb 设备坐标边框盒 / The border box in device coordinates
 * @param[in]     open 所属 details 是否展开 / Whether the owning details is open
 *
 * @note 三角画在 `padding-left` 预留的左侧空档 / The triangle is drawn in the gap reserved by `padding-left`
 */
void hpe_paint_summary_marker(HpePixmap *pm, const HpeBox *box, HpeRect bb, int open);

/**
 * @brief 绘制模态 `<dialog>` 的背景遮罩 / Paint the backdrop of a modal `<dialog>`
 *
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     y 视口内容区顶边的设备 y / Device y of the viewport content top edge
 *
 * @note 半透明黑色覆盖整个内容区；在非模态绘制阶段之前调用 / Translucent black covers the whole content area; called before the non-modal boxes are painted
 */
void hpe_paint_dialog_backdrop(HpePixmap *pm, float y);

#endif /* HPE_PAINT_DETAILS_H */
