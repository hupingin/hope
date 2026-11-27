/* Hope UI Engine - <details>/<summary> and <dialog> semantics (see hope_details.c) */
#ifndef HOPE_DETAILS_H
#define HOPE_DETAILS_H

#include "dom/hpe_dom.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 判断节点是否为 `<summary>` 元素 / Test whether a node is a `<summary>` element
 *
 * @param[in] node DOM 节点 / DOM node
 *
 * @return 是 `<summary>` 返回 1，否则 0 / 1 for a `<summary>`, else 0
 */
int hpe_details_is_summary(const HpeDomNode *node);

/**
 * @brief 找到 `<summary>`（或其子孙）所属的 `<details>` / Find the `<details>` owning a summary
 *
 * @param[in] node 起始 DOM 节点 / Starting DOM node
 *
 * @return 所属 `<details>` 节点，找不到返回 `NULL` / The owning `<details>`, or `NULL`
 *
 * @note 从 `node` 沿父链上溯，先定位 `<summary>`，再取其直接父 `<details>` / Walks up from `node` to a `<summary>`, then takes its direct `<details>` parent
 */
HpeDomNode *hpe_details_owner(HpeDomNode *node);

/**
 * @brief 读取 `<details>` 的展开状态 / Read the open state of a `<details>`
 *
 * @param[in] details `<details>` 节点 / A `<details>` node
 *
 * @return 带 `open` 属性返回 1，否则 0 / 1 when the `open` attribute is present
 */
int hpe_details_is_open(const HpeDomNode *details);

/**
 * @brief 切换 `<details>` 的 `open` 属性 / Toggle the `open` attribute of a `<details>`
 *
 * @param[in,out] details `<details>` 节点 / A `<details>` node
 *
 * @return 切换前为展开返回 1，否则 0 / 1 when it was open before the toggle
 *
 * @note 直接改 DOM 属性，重排由 mutation sink 负责 / Mutates the DOM attribute directly; reflow is driven by the mutation sink
 */
int hpe_details_toggle(HpeDomNode *details);

/**
 * @brief 判断节点是否为 `<dialog>` 元素 / Test whether a node is a `<dialog>` element
 *
 * @param[in] node DOM 节点 / DOM node
 *
 * @return 是 `<dialog>` 返回 1，否则 0 / 1 for a `<dialog>`, else 0
 */
int hpe_dialog_is(const HpeDomNode *node);

/**
 * @brief 设置当前模态 `<dialog>` / Set the current modal `<dialog>`
 *
 * @param[in] dialog 模态对话框节点或 `NULL` / The modal dialog node, or `NULL`
 */
void hpe_dialog_set_modal(HpeDomNode *dialog);

/**
 * @brief 读取当前模态 `<dialog>` / Read the current modal `<dialog>`
 *
 * @return 模态对话框节点，无则 `NULL` / The modal dialog node, or `NULL`
 */
HpeDomNode *hpe_dialog_modal(void);

#ifdef __cplusplus
}
#endif

#endif /* HOPE_DETAILS_H */
