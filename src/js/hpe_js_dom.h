/* Hope UI Engine - javascript DOM bridge */
#ifndef HPE_JS_DOM_H
#define HPE_JS_DOM_H

#include "dom/hpe_dom.h"
#include "js/hpe_js.h"

enum {
    HPE_HOST_DOCUMENT = 1,
    HPE_HOST_ELEMENT = 2,
    HPE_HOST_STYLE = 3,
    HPE_HOST_FRAGMENT = 4,
    /* a MutationObserver host object (its own methods live as own properties) */
    HPE_HOST_MUTATION_OBSERVER = 5,
    /* an element's `dataset` (accesses `data-*` attributes on the element) */
    HPE_HOST_DATASET = 6,
    /* a `getComputedStyle` result (reads the laid-out box style) */
    HPE_HOST_COMPUTED = 7,
    /* a Text node (nodeType 3, `data`/`textContent`, tree links) */
    HPE_HOST_TEXT = 8,
    /* a `localStorage` / `sessionStorage` area (keys are properties) */
    HPE_HOST_STORAGE = 9
};

/**
 * @brief 注册 `document` 宿主对象及其虚表 / Register the `document` host object and its vtables
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] document 文档根 DOM 节点 / The document root DOM node
 *
 * @note 同时注册元素与样式表的属性拦截虚表 / Also registers the element and stylesheet property-access vtables
 *
 * @see hpe_js_dom_node_value
 */
void hpe_js_dom_install(HpeJs *js, HpeDomNode *document);

/**
 * @brief 把 DOM 节点包装成脚本可见的宿主值 / Wrap a DOM node in the host value scripts see
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] node 待包装的 DOM 节点 / The DOM node to wrap
 *
 * @return 携带元素或片段虚表的宿主值 / The host value carrying the element or fragment vtable
 *
 * @note MutationObserver 桥用它把完全可用的节点作为记录 `target` 交回脚本 / The MutationObserver bridge uses it to hand a fully usable node back as a record's `target`
 */
JsValue hpe_js_dom_node_value(HpeJs *js, HpeDomNode *node);

/**
 * @brief 向目标节点及其祖先派发事件（冒泡）/ Dispatch an event to a target node and its ancestors with bubbling
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] target 事件目标节点 / The event target node
 * @param[in]     type 事件类型名（如 `click`）/ The event type name, such as `click`
 * @param[in]     clientX 客户端 X 坐标 / Client X coordinate
 * @param[in]     clientY 客户端 Y 坐标 / Client Y coordinate
 *
 * @return 有处理函数被调用返回 1 / 1 when any handler was invoked
 *
 * @note 传给处理函数的事件对象为 `{ type, clientX, clientY, target, currentTarget, preventDefault }` / Handlers receive an event object shaped like `{ type, clientX, clientY, target, currentTarget, preventDefault }`
 */
int hpe_js_dom_dispatch(HpeJs *js, HpeDomNode *target, const char *type, int clientX,
                        int clientY);

/**
 * @brief 事件初始化数据 / Extra data carried by an event
 *
 * @note 只有该事件类型相关的字段会被读入事件对象；其余保持默认 0 / Only the fields relevant to the event type are copied onto the event object; the rest stay at 0
 */
typedef struct HpeJsEventInit {
    int client_x;        /* pointer events */
    int client_y;
    int delta_x;         /* wheel */
    int delta_y;
    int delta_mode;      /* 0 = pixels, 1 = lines, 2 = pages */
    int key_code;        /* keyboard: the legacy numeric code */
    int ctrl;            /* modifier flags, keyboard and pointer alike */
    int shift;
    int alt;
    int meta;
    int repeat;          /* keyboard: 1 while the key is held down */
    int no_bubble;       /* 1 to keep the event on its target only (a `bubbles: false` Event) */
    const char *key;     /* keyboard: the printable key name (`a`, `Enter`) */
    const char *code;    /* keyboard: the physical key code (`KeyA`) */
    HpeDomNode *related; /* mouseover/mouseout: where the pointer came from or went to */
} HpeJsEventInit;

/**
 * @brief 派发一个带附加数据的事件 / Dispatch an event carrying extra data
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] target 事件目标节点 / The event target node
 * @param[in]     type 事件类型名 / The event type name
 * @param[in]     init 附加数据；传 `NULL` 表示全默认 / Extra data, or `NULL` for all defaults
 *
 * @return 有处理函数被调用返回 1 / 1 when any handler was invoked
 *
 * @note 事件流与 `hpe_js_dom_dispatch` 一致；键盘事件用 `key`/`code`/`keyCode` 与修饰键，
 *       滚轮事件用 `deltaX`/`deltaY`/`deltaMode`，进出事件用 `relatedTarget`
 *       The flow matches `hpe_js_dom_dispatch`; keyboard events carry `key`/`code`/`keyCode` and the modifiers, wheel events carry `deltaX`/`deltaY`/`deltaMode`, and enter/leave events carry `relatedTarget`
 *
 * @see hpe_js_dom_dispatch
 */
int hpe_js_dom_dispatch_ex(HpeJs *js, HpeDomNode *target, const char *type,
                           const HpeJsEventInit *init);

/**
 * @brief 只在目标节点上触发、不传播的事件 / Dispatch an event on the target only, without propagation
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] target 事件目标节点 / The event target node
 * @param[in]     type 事件类型名 / The event type name
 * @param[in]     init 附加数据；传 `NULL` 表示全默认 / Extra data, or `NULL` for all defaults
 *
 * @return 有处理函数被调用返回 1 / 1 when any handler was invoked
 *
 * @note `mouseenter` / `mouseleave` 不冒泡，用它派发；`relatedTarget` 照常写入 / `mouseenter` and `mouseleave` do not bubble, so they go through here; `relatedTarget` is still recorded
 */
int hpe_js_dom_dispatch_self(HpeJs *js, HpeDomNode *target, const char *type,
                             const HpeJsEventInit *init);

/**
 * @brief 注册焦点变化回调 / Register the focus-change callback
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     fn 回调；`el.focus()` / `el.blur()` 时以目标节点（或 NULL 表示失焦）调用 / The callback, invoked with the target node (or NULL to blur) when `el.focus()` / `el.blur()` runs
 * @param[in,out] ud 传给回调的用户数据 / User data handed to the callback
 *
 * @note 引擎用它把 `:focus` 状态同步到真实焦点并触发重排 / The engine uses it to mirror `:focus` onto the real focus and trigger a relayout
 */
void hpe_js_dom_set_focus_hook(HpeJs *js, void (*fn)(void *ud, HpeDomNode *node), void *ud);

/**
 * @brief 注册“立即排版”回调 / Register the layout-now callback
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     fn 回调；几何读取器（`offsetWidth` 等）在读值前调用它 / The callback invoked by the geometry getters (`offsetWidth` and friends) before they read a value
 * @param[in,out] ud 传给回调的用户数据 / User data handed to the callback
 *
 * @note 浏览器在这一类读取上会强制布局；引擎借此把脚本刚建、尚未参与排版的元素也量到尺寸 / Browsers force layout on these reads; this lets the engine measure elements a script just built and that have not taken part in a layout yet
 */
void hpe_js_dom_set_layout_hook(HpeJs *js, void (*fn)(void *ud), void *ud);

/**
 * @brief 注册视口查询回调 / Register the viewport-query callback
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     fn 回调；填入视口尺寸与滚动偏移 / The callback, which fills in the viewport size and scroll offsets
 * @param[in,out] ud 传给回调的用户数据 / User data handed to the callback
 *
 * @note `getBoundingClientRect` 要用它把文档坐标换算成视口坐标 / `getBoundingClientRect` uses it to map document coordinates onto the viewport
 */
void hpe_js_dom_set_viewport_hook(HpeJs *js,
                                  void (*fn)(void *ud, float *w, float *h, float *sx, float *sy),
                                  void *ud);

/**
 * @brief 查询当前拥有 `:focus` 的元素 / Query the element that currently owns `:focus`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @return 焦点元素，无焦点时返回 `NULL` / The focused element, or `NULL` when nothing is focused
 */
HpeDomNode *hpe_js_dom_active_element(HpeJs *js);

#endif /* HPE_JS_DOM_H */
