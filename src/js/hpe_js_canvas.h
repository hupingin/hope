/* Hope UI Engine - JavaScript canvas 2D bridge (v0.27) */
#ifndef HPE_JS_CANVAS_H
#define HPE_JS_CANVAS_H

#include "dom/hpe_dom.h"
#include "js/hpe_js.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 把 canvas 2D 桥接入 JS 运行时 / Install the canvas 2D bridge into the JS runtime
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @note 注册 `HTMLCanvasElement.getContext('2d')` 所需的元素方法绑定；实际由
 *       `hpe_js_dom` 的方法表统一暴露 / Registers the element method binding used by
 *       `getContext('2d')`, exposed through the `hpe_js_dom` method table
 */
void hpe_js_canvas_install(HpeJs *js);

/**
 * @brief `getContext()` 的元素方法实现 / Element-method implementation of `getContext()`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] node 元素节点 / Element node
 * @param[in,out] args 实参数组 / Argument array
 * @param[in]     argc 参数个数 / Argument count
 *
 * @return 2D 上下文宿主对象，或 `null`（非 canvas / 不支持的上下文类型）/ The 2D context host, or `null`
 */
JsValue hpe_js_canvas_get_context(HpeJs *js, HpeDomNode *node, JsValue *args, int argc);

#ifdef __cplusplus
}
#endif

#endif /* HPE_JS_CANVAS_H */
