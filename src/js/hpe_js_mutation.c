/* Hope UI Engine - MutationObserver (DOM change notification)
 *
 * Implements the subset of the W3C DOM MutationObserver interface that fits a
 * local-file engine:
 *
 *   var mo = new MutationObserver(callback);
 *   mo.observe(target, { childList: true, attributes: true, characterData: true,
 *                        subtree: true, attributeOldValue: true,
 *                        characterDataOldValue: true, attributeFilter: [...] });
 *   mo.disconnect();
 *   var records = mo.takeRecords();
 *
 * The DOM layer reports every change through hpe_dom.h's mutation sinks; this
 * module turns those reports into MutationRecord objects and queues one
 * delivery microtask per observer, mirroring the specification's "queue a
 * mutation observer microtask" step (so callbacks run asynchronously).
 */
#include "js/hpe_js_mutation.h"

#include <stdio.h>
#include <string.h>

#include "dom/hpe_dom.h"
#include "js/hpe_js_dom.h"
#include "js/hpe_js_internal.h"

/* one pending record, held until the delivery microtask runs */
typedef struct JsMutationRecord {
    int type; /* HPE_MUT_* */
    HpeDomNode *target;
    char *attribute_name;
    char *old_value;
    struct JsMutationRecord *next;
} JsMutationRecord;

typedef struct JsMutationObserver {
    JsValue callback;
    HpeDomNode *target; /* the observed node */
    int child_list;
    int attributes;
    int character_data;
    int subtree;
    int attr_old_value;
    int char_old_value;
    char **attr_filter;
    int attr_filter_count;
    JsMutationRecord *head;
    JsMutationRecord *tail;
    int queued; /* a delivery microtask is pending */
    struct JsMutationObserver *next; /* the js-wide observer list */
} JsMutationObserver;

/**
 * @brief 取（必要时创建）解释器的 MutationObserver 状态 / Get, creating on demand, the interpreter's MutationObserver state
 *
 * @param[in,out] js JS 运行时上下文，可为 `NULL` / JS runtime context, may be `NULL`
 *
 * @return 解释器级状态块；`js` 为 `NULL` 时返回 `NULL` / The per-interpreter state block, or `NULL` when `js` is `NULL`
 *
 * @note 状态块从 arena 分配并清零，首次调用时缓存到 `js->mutation_state` / The block is arena-allocated, zeroed and cached in `js->mutation_state` on first use
 */

HpeJsMutationState *hpe_js_mutation_state(HpeJs *js)
{
    HpeJsMutationState *st;
    if (!js)
        return NULL;
    if (js->mutation_state)
        return js->mutation_state;
    st = (HpeJsMutationState *)hpe_js_alloc(js, sizeof(HpeJsMutationState));
    memset(st, 0, sizeof(*st));
    js->mutation_state = st;
    return st;
}

/**
 * @brief 分配一条待投递的变更记录 / Allocate a pending mutation record
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @return 已清零的新记录 / The new zeroed record
 */

static JsMutationRecord *record_new(HpeJs *js)
{
    JsMutationRecord *r = (JsMutationRecord *)hpe_js_alloc(js, sizeof(JsMutationRecord));
    memset(r, 0, sizeof(*r));
    return r;
}

/**
 * @brief 把记录追加到观察者的待投递链表尾 / Append a record to the tail of the observer's pending list
 *
 * @param[in,out] o 目标观察者 / Target observer
 * @param[in,out] r 要入队的记录 / The record to enqueue
 *
 * @note 链表为空时同时设置头指针 / Sets the head pointer too when the list was empty
 */
static void record_push(JsMutationObserver *o, JsMutationRecord *r)
{
    if (o->tail)
        o->tail->next = r;
    else
        o->head = r;
    o->tail = r;
}

/**
 * @brief 判断观察者是否监听了该节点 / Test whether the observer watches a node
 *
 * @param[in,out] o 观察者 / The observer
 * @param[in,out] target 变更发生的节点 / The node that changed
 *
 * @return 命中返回 1 / 1 when the node is watched
 *
 * @note 未开启 `subtree` 时只匹配观察目标本身，否则沿 `parent` 向上查找 / Without `subtree` only the observed node itself matches; otherwise ancestors are walked via `parent`
 */
static int observes_node(JsMutationObserver *o, HpeDomNode *target)
{
    HpeDomNode *n = target;
    if (!o->target)
        return 0;
    for (; n; n = n->parent) {
        if (n == o->target)
            return 1;
        if (!o->subtree)
            return 0;
    }
    return 0;
}

/**
 * @brief 判断属性名是否通过 `attributeFilter` 过滤 / Test whether an attribute name passes the `attributeFilter`
 *
 * @param[in,out] o 观察者 / The observer
 * @param[in]     name 变更的属性名，可为 `NULL` / The changed attribute name, may be `NULL`
 *
 * @return 通过返回 1 / 1 when the attribute passes
 *
 * @note 未设置过滤表时全部通过；设置了过滤表但属性名为 `NULL` 则不通过 / Everything passes without a filter list; with one, a `NULL` name fails
 */
static int attr_passes_filter(JsMutationObserver *o, const char *name)
{
    int i;
    if (!o->attr_filter_count)
        return 1;
    if (!name)
        return 0;
    for (i = 0; i < o->attr_filter_count; i++)
        if (o->attr_filter[i] && !strcmp(o->attr_filter[i], name))
            return 1;
    return 0;
}

/**
 * @brief 把内部记录转换为 MutationRecord 对象 / Convert an internal record into a MutationRecord object
 *
 * @details `type` 由记录种类映射为 `attributes`/`characterData`/`childList`；`oldValue` 仅在观察者开启对应 `*OldValue` 时给出，否则为 `null`。
 *          The record kind maps `type` to `attributes`, `characterData` or `childList`; `oldValue` is only present when the observer enabled the matching `*OldValue` option, otherwise it is `null`.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] o 所属观察者（决定是否带 `oldValue`）/ The owning observer, which decides whether `oldValue` is included
 * @param[in,out] r 待转换的记录 / The record to convert
 *
 * @return 形如 `{ type, target, attributeName, oldValue }` 的普通对象 / A plain object with `type`, `target`, `attributeName` and `oldValue`
 */

static JsValue record_to_object(HpeJs *js, JsMutationObserver *o, JsMutationRecord *r)
{
    JsObj *obj = js_object_new(js);
    const char *type = r->type == HPE_MUT_ATTR ? "attributes"
                       : r->type == HPE_MUT_TEXT ? "characterData"
                                                 : "childList";
    js_obj_set(js, obj, "type", js_string(js, type));
    js_obj_set(js, obj, "target", hpe_js_dom_node_value(js, r->target));
    if (r->type == HPE_MUT_ATTR) {
        js_obj_set(js, obj, "attributeName",
                   js_string(js, r->attribute_name ? r->attribute_name : ""));
        if (o->attr_old_value && r->old_value)
            js_obj_set(js, obj, "oldValue", js_string(js, r->old_value));
        else
            js_obj_set(js, obj, "oldValue", js_null());
    } else if (r->type == HPE_MUT_TEXT) {
        if (o->char_old_value && r->old_value)
            js_obj_set(js, obj, "oldValue", js_string(js, r->old_value));
        else
            js_obj_set(js, obj, "oldValue", js_null());
    } else {
        js_obj_set(js, obj, "oldValue", js_null());
    }
    return js_objval(obj);
}

/**
 * @brief 取出观察者的全部待投递记录并清空队列 / Take all pending records of an observer, clearing its queue
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] o 目标观察者 / Target observer
 *
 * @return 记录对象构成的数组 / An array of record objects
 *
 * @note 取出后头尾指针清零并同步写入 `length` / The head and tail pointers are cleared and `length` is written
 */
static JsValue take_records_value(HpeJs *js, JsMutationObserver *o)
{
    JsObj *arr = js_array_new(js);
    JsMutationRecord *r = o->head;
    char key[32];
    int n = 0;
    o->head = o->tail = NULL;
    for (; r; r = r->next) {
        snprintf(key, sizeof(key), "%d", n++);
        js_obj_set(js, arr, key, record_to_object(js, o, r));
    }
    js_obj_set(js, arr, "length", js_num((double)n));
    return js_objval(arr);
}

/**
 * @brief 投递微任务体：把已入队记录交给回调 / The delivery microtask body: hand the queued records to the callback
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 `this`（未使用）/ The `this` of the call (unused)
 * @param[in,out] args 实参数组（未使用）/ Argument array (unused)
 * @param[in]     argc 参数个数（未使用）/ Argument count (unused)
 * @param[in,out] ud 透传的观察者指针 / The observer pointer passed through
 *
 * @return 始终返回 `undefined` / Always returns `undefined`
 *
 * @note 先清除 `queued` 标志；无待投递记录时不做任何事，否则以记录数组为唯一实参调用回调 / The `queued` flag is cleared first; with no pending records it returns, otherwise the callback is invoked with the records array as its only argument
 */
static JsValue observer_deliver(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    JsMutationObserver *o = (JsMutationObserver *)ud;
    JsValue recs, cbargs[1], cbthis;
    HPE_UNUSED(thisv);
    HPE_UNUSED(args);
    HPE_UNUSED(argc);
    if (!o)
        return js_undef();
    o->queued = 0;
    if (!o->head)
        return js_undef();
    recs = take_records_value(js, o);
    cbargs[0] = recs;
    cbthis = js_undef();
    hpe_js_call(js, o->callback, cbthis, cbargs, 1);
    return js_undef();
}

/**
 * @brief 为一个观察者排入投递微任务 / Queue a delivery microtask for an observer
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] o 目标观察者 / Target observer
 *
 * @note 已有待投递微任务时直接返回；否则置 `queued` 标志并投递 `observer_deliver` / Returns immediately when a delivery is already pending, otherwise sets `queued` and queues `observer_deliver`
 *
 * @see observer_deliver
 */
static void queue_delivery(HpeJs *js, JsMutationObserver *o)
{
    JsObj *fn;
    if (o->queued)
        return;
    o->queued = 1;
    fn = js_native_new(js, observer_deliver, o);
    hpe_js_queue_microtask(js, js_objval(fn), js_undef());
}

/**
 * @brief DOM 变更接收器：为匹配的观察者生成记录 / DOM mutation sink: create records for the observers that match
 *
 * @details 遍历解释器的观察者链：子节点变更要求 `childList` 且命中节点，属性变更额外要求通过属性过滤，字符数据变更要求 `characterData`；命中者建一条记录并入队投递微任务。
 *          Walks the interpreter's observer list: a child-list change requires `childList` and a watched node, an attribute change also needs to pass the filter, and a character-data change requires `characterData`; a hit builds a record and queues a delivery microtask.
 *
 * @param[in,out] ud 透传的 JS 上下文 / The JS context passed through
 * @param[in]     kind 变更种类（`HPE_MUT_*`）/ Mutation kind, one of `HPE_MUT_*`
 * @param[in,out] target 发生变更的 DOM 节点 / The DOM node that changed
 * @param[in]     name 属性名（属性变更时）/ The attribute name for attribute mutations
 * @param[in]     old_value 变更前的旧值，可为 `NULL` / The previous value, may be `NULL`
 * @param[in,out] added 新增节点（未使用）/ The added node (unused)
 * @param[in,out] removed 被移除节点（未使用）/ The removed node (unused)
 *
 * @see queue_delivery record_push
 */

static void mutation_bridge(void *ud, int kind, HpeDomNode *target, const char *name,
                            const char *old_value, HpeDomNode *added, HpeDomNode *removed)
{
    HpeJs *js = (HpeJs *)ud;
    HpeJsMutationState *st = hpe_js_mutation_state(js);
    JsMutationObserver *o;
    HPE_UNUSED(added);
    HPE_UNUSED(removed);

    if (!st)
        return;
    for (o = st->observers; o; o = o->next) {
        int match = 0;
        JsMutationRecord *r;

        if (kind == HPE_MUT_CHILD_LIST)
            match = o->child_list && observes_node(o, target);
        else if (kind == HPE_MUT_ATTR)
            match = o->attributes && observes_node(o, target) && attr_passes_filter(o, name);
        else if (kind == HPE_MUT_TEXT)
            match = o->character_data && observes_node(o, target);
        if (!match)
            continue;

        r = record_new(js);
        r->type = kind;
        r->target = target;
        if (kind == HPE_MUT_ATTR && name)
            r->attribute_name = hpe_js_strdup(js, name);
        if (old_value)
            r->old_value = hpe_js_strdup(js, old_value);
        record_push(o, r);
        queue_delivery(js, o);
    }
}

/**
 * @brief 注册 DOM 变更接收器 / Register the DOM mutation sink
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @note 每个解释器只注册一次（由 `sink_installed` 保证），接收器经 `hpe_dom_add_mutation_sink` 挂到 DOM 层 / Registers once per interpreter guarded by `sink_installed`, attaching the bridge through `hpe_dom_add_mutation_sink`
 */
void hpe_js_mutation_install(HpeJs *js)
{
    HpeJsMutationState *st = hpe_js_mutation_state(js);
    if (!st || st->sink_installed)
        return;
    st->sink_installed = 1;
    st->js = js;
    hpe_dom_add_mutation_sink(mutation_bridge, js);
}

/**
 * @brief 注销 DOM 变更接收器 / Unregister the DOM mutation sink
 *
 * @param[in,out] js JS 运行时上下文，可为 `NULL` / JS runtime context, may be `NULL`
 *
 * @note 未注册时不做任何事，否则摘除接收器并清 `sink_installed` / Does nothing when not installed, otherwise removes the sink and clears `sink_installed`
 */
void hpe_js_mutation_uninstall(HpeJs *js)
{
    HpeJsMutationState *st = js ? js->mutation_state : NULL;
    if (!st || !st->sink_installed)
        return;
    hpe_dom_remove_mutation_sink(mutation_bridge, js);
    st->sink_installed = 0;
}

/**
 * @brief `MutationObserver.prototype.observe`：开始监听目标节点 / `MutationObserver.prototype.observe`: start watching a target node
 *
 * @details 目标须为 `HPE_HOST_ELEMENT` 或 `HPE_HOST_FRAGMENT` 宿主值；从选项对象读取 `childList`/`attributes`/`characterData`/`subtree`/`attributeOldValue`/`characterDataOldValue` 与 `attributeFilter`。
 *          The target must be a `HPE_HOST_ELEMENT` or `HPE_HOST_FRAGMENT` host value; the options object supplies `childList`, `attributes`, `characterData`, `subtree`, `attributeOldValue`, `characterDataOldValue` and `attributeFilter`.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 `this`（未使用）/ The `this` of the call (unused)
 * @param[in,out] args 实参数组：`[目标节点, 选项对象]` / Argument array: `[target node, options object]`
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 透传的观察者指针 / The observer pointer passed through
 *
 * @return 始终返回 `undefined` / Always returns `undefined`
 *
 * @note 缺少选项对象时只记录目标不设任何监听开关；过滤器条目经 `ToString` 复制 / Without an options object only the target is recorded and no watch flags are set; filter entries are copied as strings
 */

static JsValue observer_observe(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    JsMutationObserver *o = (JsMutationObserver *)ud;
    HpeDomNode *node;
    JsValue opts;
    HPE_UNUSED(thisv);
    if (!o || argc < 1)
        return js_undef();
    node = (HpeDomNode *)js_host_ptr(args[0], HPE_HOST_ELEMENT);
    if (!node)
        node = (HpeDomNode *)js_host_ptr(args[0], HPE_HOST_FRAGMENT);
    if (!node)
        return js_undef();
    o->target = node;
    if (argc < 2 || args[1].t != JS_OBJ || !args[1].obj)
        return js_undef();
    opts = args[1];
    o->child_list = js_truthy(js_obj_get(js, opts.obj, "childList"));
    o->attributes = js_truthy(js_obj_get(js, opts.obj, "attributes"));
    o->character_data = js_truthy(js_obj_get(js, opts.obj, "characterData"));
    o->subtree = js_truthy(js_obj_get(js, opts.obj, "subtree"));
    o->attr_old_value = js_truthy(js_obj_get(js, opts.obj, "attributeOldValue"));
    o->char_old_value = js_truthy(js_obj_get(js, opts.obj, "characterDataOldValue"));
    {
        JsValue filter = js_obj_get(js, opts.obj, "attributeFilter");
        if (filter.t == JS_OBJ && filter.obj) {
            JsValue len = js_obj_get(js, filter.obj, "length");
            int n = (int)js_to_number(js, len), i;
            if (n < 0)
                n = 0;
            o->attr_filter = (char **)hpe_js_alloc(js, sizeof(char *) * (size_t)(n ? n : 1));
            o->attr_filter_count = 0;
            for (i = 0; i < n; i++) {
                char key[16];
                snprintf(key, sizeof(key), "%d", i);
                o->attr_filter[o->attr_filter_count++] =
                    hpe_js_strdup(js, js_to_string(js, js_obj_get(js, filter.obj, key)));
            }
        }
    }
    return js_undef();
}

/**
 * @brief `MutationObserver.prototype.disconnect`：停止监听并丢弃待投递记录 / `MutationObserver.prototype.disconnect`: stop watching and drop pending records
 *
 * @param[in,out] js JS 运行时上下文（未使用）/ JS runtime context (unused)
 * @param[in]     thisv 调用时的 `this`（未使用）/ The `this` of the call (unused)
 * @param[in,out] args 实参数组（未使用）/ Argument array (unused)
 * @param[in]     argc 参数个数（未使用）/ Argument count (unused)
 * @param[in,out] ud 透传的观察者指针 / The observer pointer passed through
 *
 * @return 始终返回 `undefined` / Always returns `undefined`
 *
 * @note 清空目标、待投递记录链与 `queued` 标志 / Clears the target, the pending record list and the `queued` flag
 */
static JsValue observer_disconnect(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    JsMutationObserver *o = (JsMutationObserver *)ud;
    HPE_UNUSED(js);
    HPE_UNUSED(thisv);
    HPE_UNUSED(args);
    HPE_UNUSED(argc);
    if (o) {
        o->target = NULL;
        o->head = o->tail = NULL;
        o->queued = 0;
    }
    return js_undef();
}

/**
 * @brief `MutationObserver.prototype.takeRecords`：取走并清空待投递记录 / `MutationObserver.prototype.takeRecords`: take and clear the pending records
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 `this`（未使用）/ The `this` of the call (unused)
 * @param[in,out] args 实参数组（未使用）/ Argument array (unused)
 * @param[in]     argc 参数个数（未使用）/ Argument count (unused)
 * @param[in,out] ud 透传的观察者指针 / The observer pointer passed through
 *
 * @return 记录对象数组 / An array of record objects
 *
 * @note 观察者指针为空时返回空数组 / Returns an empty array when the observer pointer is `NULL`
 *
 * @see take_records_value
 */
static JsValue observer_take_records(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    JsMutationObserver *o = (JsMutationObserver *)ud;
    HPE_UNUSED(thisv);
    HPE_UNUSED(args);
    HPE_UNUSED(argc);
    if (!o)
        return js_objval(js_array_new(js));
    return take_records_value(js, o);
}

/**
 * @brief `MutationObserver` 构造函数 / The `MutationObserver` constructor
 *
 * @details 分配并清零观察者结构，首参存为回调，注册到解释器状态链，并把 `observe`/`disconnect`/`takeRecords` 作为自身属性挂上。
 *          The observer struct is allocated and zeroed, the first argument is stored as the callback, the observer is linked into the interpreter state and `observe`, `disconnect` and `takeRecords` are attached as own properties.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 `this`（未使用）/ The `this` of the call (unused)
 * @param[in,out] args 实参数组：`[回调]` / Argument array: `[callback]`
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 用户数据（未使用）/ User data (unused)
 *
 * @return 新的 MutationObserver 宿主值 / The new MutationObserver host value
 */
static JsValue observer_ctor(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HpeJsMutationState *st = hpe_js_mutation_state(js);
    JsMutationObserver *o = (JsMutationObserver *)hpe_js_alloc(js, sizeof(JsMutationObserver));
    JsObj *obj;
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    memset(o, 0, sizeof(*o));
    o->callback = argc > 0 ? args[0] : js_undef();
    obj = js_host_new(js, o, HPE_HOST_MUTATION_OBSERVER, NULL);
    if (st) {
        o->next = st->observers;
        st->observers = o;
    }
    /* attach the methods as ordinary own properties */
    js_obj_set(js, obj, "observe", js_objval(js_native_new(js, observer_observe, o)));
    js_obj_set(js, obj, "disconnect", js_objval(js_native_new(js, observer_disconnect, o)));
    js_obj_set(js, obj, "takeRecords", js_objval(js_native_new(js, observer_take_records, o)));
    return js_objval(obj);
}

/**
 * @brief 安装全局 `MutationObserver` 构造器 / Install the global `MutationObserver` constructor
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @note `js` 为 `NULL` 时直接返回；否则把带 `name` 的原生函数写入 `global.MutationObserver` / Returns early when `js` is `NULL`; otherwise the native function with a `name` property is stored in `global.MutationObserver`
 */
void hpe_js_mutation_install_ctor(HpeJs *js)
{
    JsObj *fn;
    if (!js)
        return;
    fn = js_native_new(js, observer_ctor, NULL);
    js_obj_set(js, fn, "name", js_string(js, "MutationObserver"));
    js_obj_set(js, js->global_obj, "MutationObserver", js_objval(fn));
}
