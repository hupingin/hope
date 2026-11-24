/* Hope UI Engine - javascript timer queue: setTimeout/setInterval/clear* and
 * requestAnimationFrame. Callbacks run from hpe_js_run_timers, driven by the
 * engine's tick (window WM_TIMER or the headless test hooks). */
#include "js/hpe_js_internal.h"

#include <stdio.h>
#include <string.h>

#include "platform/hpe_platform.h"

typedef struct JsTimer {
    int id;
    int repeating; /* setInterval */
    int is_raf;    /* requestAnimationFrame: runs on every drain, no id */
    double due;    /* absolute hpe_time_seconds() deadline */
    double every;  /* repeating interval, seconds */
    JsValue fn;
} JsTimer;

/**
 * @brief 按 id 查找定时器 / Find a timer by its id
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     id 定时器 id / Timer id
 *
 * @return 命中的定时器指针，未找到返回 `NULL` / Pointer to the timer, or `NULL` when not found
 */
static JsTimer *timer_by_id(HpeJs *js, int id)
{
    int i;
    for (i = 0; i < js->timer_count; i++)
        if (js->timers[i].id == id)
            return &js->timers[i];
    return NULL;
}

/**
 * @brief 从队列中删除一个定时器 / Remove a timer from the queue
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] t 要移除的定时器（指向 `js->timers` 内部）/ The timer to remove, pointing into `js->timers`
 *
 * @note 用指针差定位下标，索引越界时静默返回；其余项用 `memmove` 前移 / The index is computed by pointer difference and out-of-range values return silently; later entries shift down via `memmove`
 */
static void timer_remove(HpeJs *js, JsTimer *t)
{
    int idx = (int)(t - js->timers);
    if (idx < 0 || idx >= js->timer_count)
        return;
    memmove(&js->timers[idx], &js->timers[idx + 1],
            sizeof(JsTimer) * (size_t)(js->timer_count - idx - 1));
    js->timer_count--;
}

/**
 * @brief 追加一个定时器槽位并返回其指针 / Append a timer slot and return a pointer to it
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @return 指向新追加的定时器槽位 / Pointer to the newly appended timer slot
 *
 * @note 在 arena 中重新分配更大的数组并拷贝旧项，`timer_count` 自增 / A larger array is arena-allocated and copied, and `timer_count` is incremented
 */
static JsTimer *timer_grow(HpeJs *js)
{
    JsTimer *old = js->timers;
    int n = js->timer_count;
    JsTimer *arr = (JsTimer *)hpe_js_alloc(js, sizeof(JsTimer) * (size_t)(n + 1));
    if (n > 0)
        memcpy(arr, old, sizeof(JsTimer) * (size_t)n);
    js->timers = arr;
    js->timer_count = n + 1;
    return &arr[n];
}

/**
 * @brief `setTimeout` / `setInterval` 的原生实现 / Native implementation of `setTimeout` and `setInterval`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 `this`（未使用）/ The `this` of the call (unused)
 * @param[in,out] args 实参数组：`[回调, 毫秒数]` / Argument array: `[callback, milliseconds]`
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 非 `NULL` 表示 `setInterval` / Non-`NULL` means `setInterval`
 *
 * @return 新定时器的 id；首参不是函数时返回 -1 / The new timer id, or -1 when the first argument is not a function
 *
 * @note 延迟小于 0 被夹为 0，且按秒换算成绝对到期时刻 / A negative delay is clamped to 0 and converted to an absolute deadline in seconds
 */
static JsValue native_set_timeout(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    double ms = argc > 1 ? js_to_number(js, args[1]) : 0.0;
    int repeating = ud != NULL;
    JsTimer *t;
    HPE_UNUSED(thisv);
    if (argc < 1 || args[0].t != JS_OBJ || args[0].obj->kind != JS_OBJ_FUNC)
        return js_num(-1);
    if (ms < 0.0)
        ms = 0.0;
    t = timer_grow(js);
    t->id = ++js->next_timer_id;
    t->repeating = repeating;
    t->is_raf = 0;
    t->every = ms / 1000.0;
    t->due = hpe_time_seconds() + t->every;
    t->fn = args[0];
    return js_num((double)t->id);
}

/**
 * @brief `clearTimeout` / `clearInterval` 的原生实现 / Native implementation of `clearTimeout` and `clearInterval`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 `this`（未使用）/ The `this` of the call (unused)
 * @param[in,out] args 实参数组：`[id]` / Argument array: `[id]`
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 用户数据（未使用，`setTimeout`/`setInterval` 共用同一实现）/ User data (unused; the same implementation backs both variants)
 *
 * @return 始终返回 `undefined` / Always returns `undefined`
 *
 * @note 无参数或 id 不存在时不做事 / Does nothing without an argument or when the id is unknown
 */
static JsValue native_clear_timer(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    JsTimer *t;
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    if (argc < 1)
        return js_undef();
    t = timer_by_id(js, (int)js_to_number(js, args[0]));
    if (t)
        timer_remove(js, t);
    return js_undef();
}

/**
 * @brief `requestAnimationFrame` 的原生实现 / Native implementation of `requestAnimationFrame`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 `this`（未使用）/ The `this` of the call (unused)
 * @param[in,out] args 实参数组：`[回调]` / Argument array: `[callback]`
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 用户数据（未使用）/ User data (unused)
 *
 * @return 始终返回 `undefined` / Always returns `undefined`
 *
 * @note 入队时 id 为 0、`is_raf` 置 1、到期时刻为 0，因此每轮泵都会执行 / The queued entry has id 0, `is_raf` set and a zero deadline, so it runs on every pump round
 */
static JsValue native_request_animation_frame(HpeJs *js, JsValue thisv, JsValue *args, int argc,
                                              void *ud)
{
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    HPE_UNUSED(argc);
    if (argc < 1 || args[0].t != JS_OBJ || args[0].obj->kind != JS_OBJ_FUNC)
        return js_undef();
    timer_grow(js);
    {
        JsTimer *t = &js->timers[js->timer_count - 1];
        t->id = 0;
        t->repeating = 0;
        t->is_raf = 1;
        t->due = 0.0;
        t->every = 0.0;
        t->fn = args[0];
    }
    return js_undef();
}

/**
 * @brief 执行到期定时器与 rAF 回调 / Run due timers and rAF callbacks
 *
 * @details 倒序遍历队列：rAF 与到期的非重复定时器执行后出队，重复定时器顺延到下一个到期时刻；回调经 `hpe_js_call` 调用，返回 0 说明正常执行才计数。
 *          The queue is walked backwards; rAF entries and due one-shot timers are removed after running while repeating timers advance to their next deadline; callbacks run through `hpe_js_call` and only a zero return counts as having run.
 *
 * @param[in,out] js JS 运行时上下文，可为 `NULL` / JS runtime context, may be `NULL`
 * @param[in]     now 当前时间（秒），与定时器到期时刻同基准 / Current time in seconds, on the same basis as the deadlines
 * @param[out]    fired 置 1 表示确有回调执行（可有 NULL）/ Set to 1 when any callback actually ran; may be `NULL`
 *
 * @return 实际执行的回调个数 / The number of callbacks that actually ran
 */
int hpe_js_run_timers(HpeJs *js, double now, int *fired)
{
    int i, ran = 0;
    if (!js)
        return 0;
    for (i = js->timer_count - 1; i >= 0; i--) {
        JsTimer *t = &js->timers[i];
        if (t->is_raf || t->due <= now) {
            JsValue fn = t->fn;
            int repeating = t->repeating;
            int is_raf = t->is_raf;
            if (is_raf) {
                timer_remove(js, t);
            } else if (repeating) {
                t->due += t->every;
                if (t->due <= now)
                    t->due = now + t->every;
            } else {
                timer_remove(js, t);
            }
            if (hpe_js_call(js, fn, js_undef(), NULL, 0) == 0)
                ran++;
        }
    }
    if (fired)
        *fired = ran > 0;
    return ran;
}

/**
 * @brief 在全局对象上安装定时器与 rAF / Install the timer and rAF functions on the global object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @note 安装 `setTimeout`、`setInterval`、`clearTimeout`、`clearInterval` 与 `requestAnimationFrame` / Installs `setTimeout`, `setInterval`, `clearTimeout`, `clearInterval` and `requestAnimationFrame`
 */
void hpe_js_timers_install(HpeJs *js)
{
    js_obj_set(js, js->global_obj, "setTimeout",
               js_objval(js_native_new(js, native_set_timeout, NULL)));
    js_obj_set(js, js->global_obj, "setInterval",
               js_objval(js_native_new(js, native_set_timeout, (void *)1)));
    js_obj_set(js, js->global_obj, "clearTimeout",
               js_objval(js_native_new(js, native_clear_timer, NULL)));
    js_obj_set(js, js->global_obj, "clearInterval",
               js_objval(js_native_new(js, native_clear_timer, (void *)1)));
    js_obj_set(js, js->global_obj, "requestAnimationFrame",
               js_objval(js_native_new(js, native_request_animation_frame, NULL)));
}
