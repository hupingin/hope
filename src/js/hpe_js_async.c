/* Hope UI Engine - async function suspension (design §12.8)
 *
 * An async function call returns a promise and runs its body on a coroutine,
 * exactly like a generator. The body runs synchronously on the way in, parks at
 * each `await`, and a microtask resumes it once the awaited promise settles. So
 * `await` is genuinely non-blocking: the caller's promise is handed back before
 * the body finishes, and `try`/`catch` around an `await` works because the C
 * frames stay alive across the suspend.
 *
 * Why not just drain the job queue inside `await`? An async body that awaits
 * another async function would need that other body's own `await` resolved by
 * the very same drain, so the drain would have to run the whole dependency tree
 * to completion before `await` returned — exactly the synchronous behaviour this
 * module replaces.
 *
 * The suspend/resume plumbing reuses the Promise reaction machinery rather than
 * inventing a second queue: parking an `await` registers a native reaction whose
 * handler is "resume that coroutine". When the promise settles, the ordinary
 * microtask drain calls the handler, which switches back into the coroutine.
 *
 * Exceptions use the same trick as generators: `js->try_top` and `js->jmp` are
 * swapped per fiber (`hpe_js_fiber_enter` / `hpe_js_fiber_leave`), and an error
 * raised inside the body is caught at the coroutine entry and re-thrown on the
 * caller's side.
 */
#include "js/hpe_js_internal.h"

#include "platform/hpe_platform.h"

/* coroutine stack: an async body recurses through the interpreter just like a
 * generator body, so the budget is the same. */
#define ASYNC_STACK_BYTES (256u * 1024u)

/**
 * @brief 一个 async 调用的挂起状态 / The suspended state of one async call
 *
 * @note `state` 取 0=起始、1=运行中、2=已结束；`settled` 保证结果只交给 promise 一次。
 *       `state` is 0 at start, 1 while running and 2 once finished; `settled` makes sure the
 *       result is handed to the promise at most once.
 */
typedef struct HpeJsAsync {
    HpeJs *js;
    JsObj *promise; /* the promise handed to the caller / 交给调用方的 promise */
    JsObj *fn;      /* the async function / async 函数 */
    JsEnv *closure; /* captured environment / 捕获的环境 */
    JsValue this_v;
    JsValue *args;
    int argc;
    HpeCoro *coro; /* NULL once the body finished / 函数体结束后为 NULL */
    int state;     /* 0 = start, 1 = running, 2 = finished / 起始/运行中/已结束 */
    int settled;   /* the result has been given to `promise` / 结果已交给 promise */
    int has_error; /* an exception is parked in `error` / `error` 中存有异常 */
    JsValue error;
    JsValue ret;
    /* what the `await` that parked us should do when it wakes */
    JsValue resume_value;  /* the settled value / 定局值 */
    int resume_throw;      /* 1 = rethrow inside the body (rejection) / 1 表示在体内重抛 */
    /* per-fiber exception context: see `HpeJsFiberCtx` for why this is needed */
    HpeJsFiberCtx fib;
} HpeJsAsync;

static JsValue async_handler_ok(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud);
static JsValue async_handler_err(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud);

/**
 * @brief 取回当前正在执行的 async 调用 / Fetch the async call currently executing
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @return 当前 async 调用，不在其中时为 `NULL` / The current async call, or `NULL` outside one
 */
static HpeJsAsync *async_active(HpeJs *js)
{
    return (HpeJsAsync *)js->async_active;
}

/**
 * @brief 在协程入口捕获函数体的结果或异常 / Capture the body's result or exception at the coroutine entry
 *
 * @param[in,out] ud 指向 `HpeJsAsync` / Points at the `HpeJsAsync`
 *
 * @note 函数体正常返回后把返回值存进 `ret`；抛错时先从 `js->thrown` 取回异常再记录。
 *       A normal return stores the value in `ret`; an exception is lifted out of `js->thrown`.
 */
static void async_body(void *ud)
{
    HpeJsAsync *a = (HpeJsAsync *)ud;
    HpeJs *js = a->js;
    JsValue out;

    if (setjmp(js->jmp) == 0) {
        out = js_run_async_body(js, a->fn, a->closure, a->this_v, a->args, a->argc);
        a->ret = out;
    } else {
        a->has_error = 1;
        a->error = js->thrown;
        js->thrown = js_undef();
    }
    a->state = 2;
}

/**
 * @brief 把控制权交给 async 函数体 / Hand control to the async body
 *
 * @param[in,out] a async 调用状态 / The async call state
 *
 * @return 仍在运行返回 1，已结束返回 0 / 1 when still live, 0 when finished
 *
 * @note 与生成器同理：`async_active` 与异常上下文都按「进入谁就指向谁、出来还原」处理。
 *       As with generators, `async_active` and the exception context are set on entry and
 *       restored on the way out.
 */
static int async_resume(HpeJsAsync *a)
{
    HpeJs *js = a->js;
    void *prev;
    int r;

    if (!a->coro)
        return 0;
    prev = js->async_active;
    hpe_js_fiber_enter(js, &a->fib);
    js->async_active = a;
    a->state = 1;
    r = hpe_coro_resume(a->coro);
    hpe_js_fiber_leave(js, &a->fib);
    js->async_active = prev;
    return r;
}

/**
 * @brief 把 async 调用的结果接到其 promise 上 / Wire the async call's result into its promise
 *
 * @param[in,out] a async 调用状态 / The async call state
 *
 * @note 只生效一次：抛错则拒绝 promise，否则以返回值兑现。
 *       Fires at most once: an error rejects the promise, otherwise the return value fulfils it.
 */
static void async_settle(HpeJsAsync *a)
{
    if (a->settled)
        return;
    a->settled = 1;
    if (a->has_error)
        js_settle_promise(a->js, a->promise, 2, a->error);
    else
        js_resolve_promise(a->js, a->promise, a->ret);
}

/**
 * @brief 反应处理器共用部分：恢复协程并在结束后定局 / Shared reaction handler: resume the coroutine and settle once done
 *
 * @param[in,out] a async 调用状态 / The async call state
 * @param[in]     value 反应携带的定局值 / The settled value carried by the reaction
 * @param[in]     is_err 非 0 表示拒绝（需在体内重抛）/ Non-zero for a rejection (rethrow inside the body)
 *
 * @note 协程可能在恢复后再次挂起（下一条 `await`），此时不能定局；只有 `state == 2`
 *       才交付结果。重抛由 `hpe_js_async_park` 在协程栈上完成。
 *       The coroutine may park again on resume (the next `await`), in which case nothing is
 *       settled; only `state == 2` publishes the result. The rethrow itself is done by
 *       `hpe_js_async_park`, on the coroutine stack.
 */
static void async_resumed_from(HpeJsAsync *a, JsValue value, int is_err)
{
    a->resume_value = value;
    a->resume_throw = is_err;
    async_resume(a);
    if (a->state == 2)
        async_settle(a);
}

/**
 * @brief 兑现侧的 `await` 续体 / The fulfilment-side `await` continuation
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 忽略 / Ignored
 * @param[in,out] args 首个元素为兑现值 / First element is the fulfilment value
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 指向 `HpeJsAsync` / Points at the `HpeJsAsync`
 *
 * @return 总是返回 undefined（反应结果被丢弃）/ Always undefined (the reaction result is discarded)
 */
static JsValue async_handler_ok(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HPE_UNUSED(js);
    HPE_UNUSED(thisv);
    async_resumed_from((HpeJsAsync *)ud, argc > 0 ? args[0] : js_undef(), 0);
    return js_undef();
}

/**
 * @brief 拒绝侧的 `await` 续体 / The rejection-side `await` continuation
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 忽略 / Ignored
 * @param[in,out] args 首个元素为拒绝原因 / First element is the rejection reason
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 指向 `HpeJsAsync` / Points at the `HpeJsAsync`
 *
 * @return 总是返回 undefined（反应结果被丢弃）/ Always undefined (the reaction result is discarded)
 */
static JsValue async_handler_err(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HPE_UNUSED(js);
    HPE_UNUSED(thisv);
    async_resumed_from((HpeJsAsync *)ud, argc > 0 ? args[0] : js_undef(), 1);
    return js_undef();
}

/**
 * @brief 创建一个 async 调用并立即返回其 promise / Create an async call and return its promise at once
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] fn async 函数对象 / The async function object
 * @param[in,out] closure 闭包捕获的环境 / The captured environment
 * @param[in]     this_v 调用时的 this 值 / The `this` value of the call
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 实参个数 / Argument count
 *
 * @return 立即返回的 promise / The promise returned immediately
 *
 * @note 函数体**同步**跑到第一个 `await` 或结束（与规范一致），随后调用方拿到的
 *       promise 在函数体结束时定局。整段没有任何 `await` 时，promise 一返回就已定局。
 *       The body runs **synchronously** up to the first `await` or completion (as the spec
 *       requires); the promise settles when the body finishes. With no `await` at all the
 *       promise is already settled by the time it is returned.
 */
JsValue hpe_js_async_create(HpeJs *js, JsObj *fn, JsEnv *closure, JsValue this_v, JsValue *args,
                            int argc)
{
    HpeJsAsync *a = (HpeJsAsync *)hpe_js_alloc_typed(js, sizeof(*a), GC_T_ASYNC);
    int i;

    if (!js->gc_scan_async)
        js->gc_scan_async = hpe_js_async_gc_scan;
    a->js = js;
    a->fn = fn;
    a->closure = closure;
    a->this_v = this_v;
    a->argc = argc;
    a->error = js_undef();
    a->ret = js_undef();
    a->resume_value = js_undef();
    if (argc > 0) {
        a->args = (JsValue *)hpe_js_alloc(js, (size_t)argc * sizeof(JsValue));
        for (i = 0; i < argc; i++)
            a->args[i] = args[i];
    }
    a->promise = js_promise_new(js);
    a->coro = hpe_coro_create(async_body, a, ASYNC_STACK_BYTES);
    async_resume(a);
    if (a->state == 2)
        async_settle(a);
    return js_objval(a->promise);
}

/**
 * @brief `await` 的实现 / The implementation of `await`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     v 被 await 的值 / The awaited value
 *
 * @return 兑现值 / The fulfilment value
 *
 * @note 非 promise 值不挂起，直接返回；promise 已定局也不挂起（但拒绝时立即在协程栈上抛出）；
 *       仍为 pending 时把「恢复本协程」登记为该 promise 的反应，然后挂起协程。
 *       A non-promise does not park and is returned as-is; a settled promise does not park either
 *       (though a rejection throws on the coroutine stack right away). A still-pending promise
 *       gets "resume this coroutine" registered as its reaction, and then the coroutine parks.
 */
JsValue hpe_js_await(HpeJs *js, JsValue v)
{
    HpeJsAsync *a = async_active(js);

    if (!(v.t == JS_OBJ && v.obj && v.obj->kind == JS_OBJ_PROMISE)) {
        if (!js_is_thenable(js, v)) {
            /* not thenable: `await` still yields a microtask per the spec, but the value is
             * already known, so taking it here is observationally equivalent for our host */
            return v;
        }
        return v;
    }
    if (!a) {
        /* outside an async body there is no coroutine to park: drive the queue instead */
        hpe_js_drain_jobs(js);
        if (v.obj->promise_state == 2)
            js_throw_value(js, v.obj->promise_value);
        return v.obj->promise_value;
    }
    if (v.obj->promise_state == 2)
        js_throw_value(js, v.obj->promise_value);
    if (v.obj->promise_state == 1)
        return v.obj->promise_value;
    hpe_js_async_park(js, v.obj);
    return a->resume_value;
}

/**
 * @brief 挂起当前 async 体直到给定 promise 定局 / Park the current async body until the promise settles
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] p 被 await 的 promise（必须仍为 pending）/ The awaited promise, which must still be pending
 *
 * @note 实现是「把续体登记到 promise 上，再把协程切回调用方」。恢复时若 promise 被拒绝，
 *       在**协程栈上**抛出，使 `try`/`catch` 能接住。
 *       Parking means registering the continuation on the promise and switching back to the
 *       caller. On resume, a rejected promise rethrows **on the coroutine stack** so that
 *       `try`/`catch` can catch it.
 */
void hpe_js_async_park(HpeJs *js, JsObj *p)
{
    HpeJsAsync *a = async_active(js);

    if (!a)
        return;
    js_promise_on_settle(js, p, async_handler_ok, async_handler_err, a);
    hpe_coro_yield(a->coro);
    /* resumed: the handler left its disposition on `a` */
    if (a->resume_throw)
        js_throw_value(js, a->resume_value);
}

/**
 * @brief 供 GC 扫描一个挂起 async 的协程栈 / GC hook: scan a suspended async coroutine's stack
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] state async 状态块指针（GC_T_ASYNC 块的载荷）/ The async state pointer (payload of a GC_T_ASYNC chunk)
 *
 * @see hpe_js_gc_collect
 */
void hpe_js_async_gc_scan(HpeJs *js, void *state)
{
    HpeJsAsync *a = (HpeJsAsync *)state;
    void *lo, *hi;
    if (a && a->coro && hpe_coro_stack_range(a->coro, &lo, &hi))
        hpe_js_gc_scan_range(js, lo, hi);
}
