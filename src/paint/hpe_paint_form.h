/* Hope UI Engine - form control painting (v0.23)
 *
 * Replaced form controls (input non-button types, select, progress, meter)
 * paint their control chrome here; the background and border come from the
 * ordinary box painting (UA or author CSS), this module draws the content:
 * value text / placeholder / password dots, checkbox tick, radio dot, range
 * track + thumb, colour swatch, file inner button, select arrow, progress /
 * meter fills and the editing caret of the focused text control.
 */
#ifndef HPE_PAINT_FORM_H
#define HPE_PAINT_FORM_H

#include "layout/hpe_layout.h"
#include "render/hpe_pixmap.h"

/**
 * @brief 记录当前聚焦的可编辑控件与插入符位置（绘制插入符用）/ Record the focused editable control and its caret (for caret painting)
 *
 * @param[in] node 焦点控件，`NULL` 表示无 / The focused control, `NULL` when none
 * @param[in] caret 插入符在值内的字节偏移 / Caret byte offset within the value
 *
 * @note 引擎在每帧绘制前调用；指针只在本帧读取 / The engine calls this before every
 *       paint; the pointer is read during the frame only
 */
void hpe_paint_form_set_focus(const HpeDomNode *node, int caret);

/**
 * @brief 绘制一个替换型表单控件的控件内容 / Paint one replaced form control's content
 *
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     box 控件盒 / The control box
 * @param[in]     bb 设备坐标边框盒 / The border box in device coordinates
 */
void hpe_paint_form_control(HpePixmap *pm, const HpeBox *box, HpeRect bb);

#endif /* HPE_PAINT_FORM_H */
