/* Hope UI Engine - MutationObserver (DOM change notification) */
#ifndef HPE_JS_MUTATION_H
#define HPE_JS_MUTATION_H

#include "js/hpe_js.h"

/* per-interpreter observer bookkeeping; one allocation, owned by the js arena */
typedef struct HpeJsMutationState {
    void *observers;      /* JsMutationObserver list (opaque) */
    HpeJs *js;
    int sink_installed;   /* the DOM mutation sink is registered */
} HpeJsMutationState;

/**
 * @brief 取（必要时创建）解释器的观察者登记状态 / Get, creating on demand, the interpreter's observer bookkeeping state
 *
 * @param[in,out] js JS 运行时上下文，可为 `NULL` / JS runtime context, may be `NULL`
 *
 * @return 解释器级状态块；`js` 为 `NULL` 时返回 `NULL` / The per-interpreter state block, or `NULL` when `js` is `NULL`
 *
 * @note 状态块由 arena 分配，字段含观察者链表、所属 `js` 与 `sink_installed` / The arena-allocated block holds the observer list, the owning `js` and `sink_installed`
 */
HpeJsMutationState *hpe_js_mutation_state(HpeJs *js);

/**
 * @brief 注册 DOM 变更接收器并安装全局 `MutationObserver` / Register the DOM mutation sink and install the global `MutationObserver`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @see hpe_js_mutation_uninstall hpe_js_mutation_install_ctor
 */
void hpe_js_mutation_install(HpeJs *js);

/**
 * @brief 注销 DOM 变更接收器 / Unregister the DOM mutation sink
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @note 由 `hpe_js_mutation_install` 注册的接收器在此摘除 / Removes the sink registered by `hpe_js_mutation_install`
 *
 * @see hpe_js_mutation_install
 */
void hpe_js_mutation_uninstall(HpeJs *js);

/**
 * @brief 安装全局 `MutationObserver` 构造器 / Install the global `MutationObserver` constructor
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @see hpe_js_mutation_install
 */
void hpe_js_mutation_install_ctor(HpeJs *js);

#endif /* HPE_JS_MUTATION_H */
