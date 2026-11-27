/* Hope UI Engine - javascript timer queue: setTimeout/setInterval/clear* and
 * requestAnimationFrame. Callbacks run from hpe_js_run_timers, driven by the
 * engine's tick (window WM_TIMER or the headless test hooks). */
#include "js/hpe_js_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/hpe_platform.h"

typedef struct JsTimer {
    int id;
    int seq;       /* creation order; a pump only runs entries queued before it started */
    int level;     /* HTML timer nesting level (0 for rAF, which has none) */
    int repeating; /* setInterval */
    int is_raf;    /* requestAnimationFrame: runs on every drain, no id */
    double due;    /* absolute hpe_time_seconds() deadline */
    double every;  /* repeating interval, seconds */
    JsValue fn;
} JsTimer;

/* HTML caps a timer's nesting depth at 5 and then enforces a 4 ms floor, which
 * is what stops a `setTimeout(f, 0)` self-recursion from starving the loop. */
#define HPE_TIMER_MAX_NESTING 5
#define HPE_TIMER_CLAMP_MS 4.0
/* upper bound on the callbacks one pump may run, so a self-rescheduling
 * zero-delay timer cannot hang the frame */
#define HPE_TIMER_MAX_PER_PUMP 10000

/**
 * @brief 把定时器表整体当作 GC 根扫描 / Expose the timer table to the GC as a conservative root
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @note JsTimer 布局对 GC 私有，故由本文件提供扫描；表内的回调 JsValue 借此保持可达 / The JsTimer layout is private to this file, so the scan lives here; callback JsValues stay reachable through it
 */
void hpe_js_timers_gc_scan(HpeJs *js)
{
    if (js->timers)
        hpe_js_gc_scan_range(js, js->timers,
                             (char *)js->timers + sizeof(JsTimer) * (size_t)js->timer_cap);
}

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
 * @note 定时器数组归 `realloc` 单独管理（不占 arena），因此 `hpe_js_destroy` 可以对它调用
 *       `free`；容量不足时按 2 倍增长，新槽位零初始化 / The timer array is `realloc`-managed on
 *       its own (it does not live in the arena) so `hpe_js_destroy` can `free` it; capacity
 *       doubles on demand and the new slot is zeroed
 */
static JsTimer *timer_grow(HpeJs *js)
{
    int n = js->timer_count;
    if ((size_t)(n + 1) > js->timer_cap) {
        size_t cap = js->timer_cap ? js->timer_cap * 2u : 8u;
        JsTimer *arr = (JsTimer *)realloc(js->timers, sizeof(JsTimer) * cap);
        if (!arr) {
            fprintf(stderr, "hope-js: out of memory\n");
            abort();
        }
        js->timers = arr;
        js->timer_cap = cap;
    }
    memset(&js->timers[n], 0, sizeof(JsTimer));
    js->timer_count = n + 1;
    return &js->timers[n];
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
 * @note 延迟小于 0 被夹为 0，且按秒换算成绝对到期时刻；新定时器的嵌套层数为「当前正在执行
 *       的回调层数 + 1」，层数超过 5 时按 HTML 规范施加 4 ms 下限，避免 `setTimeout(f, 0)`
 *       自递归饿死事件循环
 *       A negative delay is clamped to 0 and converted to an absolute deadline in seconds;
 *       a new timer's nesting level is the running callback's level plus one, and past level 5
 *       the HTML 4 ms floor applies, which stops a `setTimeout(f, 0)` self-recursion from
 *       starving the loop
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
    /* the clamp only applies to the timer variants, not to rAF */
    if (js->timer_level > HPE_TIMER_MAX_NESTING && ms < HPE_TIMER_CLAMP_MS)
        ms = HPE_TIMER_CLAMP_MS;
    t = timer_grow(js);
    t->id = ++js->next_timer_id;
    t->seq = ++js->next_timer_seq;
    t->level = js->timer_level + 1;
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
        t->seq = ++js->next_timer_seq;
        t->level = 0;
        t->repeating = 0;
        t->is_raf = 1;
        t->due = 0.0;
        t->every = 0.0;
        t->fn = args[0];
    }
    return js_undef();
}

/**
 * @brief 找一个可运行的回调 / Pick one runnable callback
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     now 当前时间（秒）/ Current time in seconds
 * @param[in]     limit 只看 `seq <= limit` 的定时器 / Only timers with `seq <= limit` are considered
 * @param[out]    index 接收选中定时器的下标 / Receives the index of the chosen timer
 *
 * @return 找到返回 1，否则 0 / 1 when one was found, otherwise 0
 *
 * @note rAF 优先于定时器；定时器按到期时刻取最早，同时到期的按下标（即创建顺序）/
 *       rAF outranks timers, and timers are taken earliest-deadline-first, with ties broken by index (creation order)
 */
static int timer_pick(HpeJs *js, double now, int limit, int *index)
{
    int i, best = -1;
    for (i = 0; i < js->timer_count; i++) {
        JsTimer *t = &js->timers[i];
        if (t->seq > limit)
            continue;
        if (t->is_raf) {
            *index = i;
            return 1;
        }
        if (t->due <= now && (best < 0 || t->due < js->timers[best].due))
            best = i;
    }
    if (best < 0)
        return 0;
    *index = best;
    return 1;
}

/**
 * @brief 从队列取出一个定时器（处理重复/一次性/rAF 的顺延与出队）/ Take a timer out of the queue
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] t 目标定时器（指向 `js->timers` 内部）/ Target timer, pointing into `js->timers`
 * @param[in]     now 当前时间（秒）/ Current time in seconds
 *
 * @return 该定时器的嵌套层数 / The timer's nesting level
 *
 * @note rAF 与一次性定时器立即出队；`setInterval` 留下并按 `every` 前移到期时刻，落后太多时
 *       直接跳到 `now + every` 而不补跑 / rAF and one-shot timers are removed at once; a
 *       `setInterval` stays and its deadline advances by `every`, jumping to `now + every`
 *       (rather than catching up) when it fell too far behind
 */
static int timer_take(HpeJs *js, JsTimer *t, double now)
{
    int level = t->level;
    if (t->is_raf) {
        timer_remove(js, t);
    } else if (t->repeating) {
        t->due += t->every;
        if (t->due <= now)
            t->due = now + t->every;
    } else {
        timer_remove(js, t);
    }
    return level;
}

/**
 * @brief 执行一个回调并处理其队列状态 / Run one callback and update its queue entry
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     i 目标定时器下标 / Index of the target timer
 * @param[in]     now 当前时间（秒）/ Current time in seconds
 *
 * @return 回调确实执行返回 1 / 1 when the callback actually ran
 *
 * @note 每执行一个回调就把微任务队列排空（HTML 事件循环的「一个任务 + 其后的微任务检查点」）；
 *       调用期间把 `timer_level` 提升到该定时器的层数，回调内再排定时器时即可触发 4 ms 钳制
 *       The microtask queue is drained after every callback, which is the HTML event loop's "one task plus its microtask checkpoint"; the nesting level is raised to this timer's level for the duration of the call, so a timer scheduled inside it sees the 4 ms clamp
 */
static int timer_run_one(HpeJs *js, int i, double now)
{
    JsTimer *t = &js->timers[i];
    JsValue fn = t->fn;
    int level = timer_take(js, t, now);
    int saved = js->timer_level;
    int ok;
    js->timer_level = level;
    ok = hpe_js_call(js, fn, js_undef(), NULL, 0) == 0;
    js->timer_level = saved;
    hpe_js_drain_jobs(js);
    return ok;
}

/**
 * @brief 执行到期定时器与 rAF 回调 / Run due timers and rAF callbacks
 *
 * @details 每轮挑一个「应当最先跑」的回调：rAF 优先，其次到期时刻最早的定时器；执行后立即
 *          排空微任务队列，再重新挑选，直到没有可跑的为止。这样一遍泵里，同一时刻到期的
 *          多个回调也会按创建顺序依次执行。
 *          Each round picks the single callback that should run next -- rAF first, then the
 *          earliest-due timer -- drains the microtask queue, then picks again until nothing is
 *          left. That way several callbacks due at the same instant still run in creation order.
 *
 * @param[in,out] js JS 运行时上下文，可为 `NULL` / JS runtime context, may be `NULL`
 * @param[in]     now 当前时间（秒），与定时器到期时刻同基准 / Current time in seconds, on the same basis as the deadlines
 * @param[out]    fired 置 1 表示确有回调执行（可有 NULL）/ Set to 1 when any callback actually ran; may be `NULL`
 *
 * @return 实际执行的回调个数 / The number of callbacks that actually ran
 *
 * @note 一个回调里新排的定时器会等到下一遍泵（它的 `seq` 大于本遍的快照上限），
 *       因此 rAF 回调里再调 `requestAnimationFrame` 不会在同一帧里被反复执行；
 *       单遍最多执行 `HPE_TIMER_MAX_PER_PUMP` 个回调，避免无限自递归挂住一帧
 *       A timer queued inside a callback waits for the next pump (its `seq` exceeds this
 *       pump's snapshot), so a rAF callback that re-arms with `requestAnimationFrame` is not
 *       run over and over inside one frame; a single pump runs at most
 *       `HPE_TIMER_MAX_PER_PUMP` callbacks so an infinite self-recursion cannot hang a frame
 */
int hpe_js_run_timers(HpeJs *js, double now, int *fired)
{
    int ran = 0, idx, limit;
    if (!js)
        return 0;
    limit = js->next_timer_seq; /* callbacks queued from here on wait for the next pump */
    while (ran < HPE_TIMER_MAX_PER_PUMP && timer_pick(js, now, limit, &idx)) {
        if (timer_run_one(js, idx, now))
            ran++;
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
