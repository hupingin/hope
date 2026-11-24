/* Hope UI Engine - SVG painter (subset), see hpe_svg.c */
#ifndef HPE_SVG_H
#define HPE_SVG_H

#include "render/hpe_pixmap.h"

struct HpeBox;

/**
 * @brief 绘制 `<svg>` 子树 / Paint the `<svg>` subtree
 *
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     box `<svg>` 盒 / The `<svg>` box
 * @param[in]     y_dev 盒子顶边的设备 y / Device y of the box's top edge
 *
 * @note 内容直接来自 DOM，不经过盒子树 / The content is painted straight from the DOM rather than from boxes
 */
void hpe_svg_paint(HpePixmap *pm, struct HpeBox *box, float y_dev);

/**
 * @brief 对 `<svg>` 子树做命中测试 / Hit test an `<svg>` subtree
 *
 * @param[in]     box `<svg>` 盒 / The `<svg>` box
 * @param[in]     x 文档坐标 X / Document-space X
 * @param[in]     y 文档坐标 Y / Document-space Y
 *
 * @return 最深的包含该点的 SVG 元素（`<rect>` 上的点击会冒泡到其 `<g>`），未命中返回 `NULL` / The deepest SVG element containing the point (a click on a `<rect>` bubbles up to its `<g>`), or `NULL`
 *
 * @note 路径目前尚未参与命中测试 / Paths are not hit tested yet
 */
struct HpeDomNode *hpe_svg_hit_test(struct HpeBox *box, float x, float y);

#endif /* HPE_SVG_H */
