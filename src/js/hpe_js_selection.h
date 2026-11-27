/* Hope UI Engine - JavaScript Selection / Range bridge (v0.24) */
#ifndef HPE_JS_SELECTION_H
#define HPE_JS_SELECTION_H

#include "dom/hpe_dom.h"
#include "js/hpe_js.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 引擎侧选区端点访问接口 / Engine-side selection endpoint access
 *
 * @note 由 hope.c 注册；JS 侧的 Selection / Range 只经此接口读写页面选区。
 *       Registered by hope.c; the JS Selection / Range objects read and write
 *       the page selection only through this interface.
 */
typedef struct HpeJsSelApi {
    void *ud; /**< 透传的用户数据（引擎实例）/ Opaque user data (engine) */
    void (*get)(void *ud, HpeDomNode **anchor, int *anchor_off, HpeDomNode **focus,
                int *focus_off);
    void (*set)(void *ud, HpeDomNode *anchor, int anchor_off, HpeDomNode *focus, int focus_off);
    void (*clear)(void *ud);
} HpeJsSelApi;

/**
 * @brief 注册引擎侧选区接口 / Register the engine-side selection interface
 */
void hpe_js_sel_set_api(const HpeJsSelApi *api);

/**
 * @brief 安装 `document.createRange()` 与 `window.getSelection()` / Install the DOM selection API
 */
void hpe_js_sel_install(HpeJs *js);

#ifdef __cplusplus
}
#endif

#endif
