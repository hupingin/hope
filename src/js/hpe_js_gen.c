/* Hope UI Engine - lazy generators (design §12.7)
 *
 * A generator function call must not run the body. It builds a JS_OBJ_GEN object
 * holding the function, the captured closure and the arguments, plus a suspended
 * coroutine. `next()` resumes that coroutine, which runs the body on its own
 * stack until a `yield` parks it again. Because the body's C frames stay alive
 * across the suspend, `yield` can appear inside loops, `try` blocks and nested
 * calls — and an endless `while (true) yield i++` costs nothing until pulled.
 *
 * The coroutine itself lives in the platform layer (hpe_coro_*) because Win32
 * fibers are the only portable-enough way to switch stacks in pure C here.
 *
 * Exceptions are the reason this cannot be done with `setjmp`-only tricks: the
 * interpreter throws with `longjmp`, which cannot cross a stack switch. So an
 * error thrown *inside* a generator is caught at the coroutine entry, stored on
 * the generator, and re-thrown on the resumer's side by `gen_next`. That keeps
 * `try`/`catch` working in both directions.
 */
#include "js/hpe_js_internal.h"

#include <stdio.h>
#include <string.h>

#include "platform/hpe_platform.h"

/* coroutine stack: generators recurse through the interpreter, so this needs to
 * be generous. 256 KiB handles a few hundred nested generator frames. */
#define GEN_STACK_BYTES (256u * 1024u)

/**
 * @brief 把一个协程的异常上下文换入/换出 / Swap a coroutine's exception context in and out
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] ctx 协程的上下文块 / The coroutine's context block
 *
 * @see HpeJsFiberCtx
 */
void hpe_js_fiber_enter(HpeJs *js, HpeJsFiberCtx *ctx)
{
    ctx->resumer_try_top = js->try_top;
    memcpy(ctx->resumer_jmp, js->jmp, sizeof(jmp_buf));
    ctx->resumer_depth = js->call_depth;
    js->try_top = ctx->coro_try_top;
    memcpy(js->jmp, ctx->coro_jmp, sizeof(jmp_buf));
    js->call_depth = 0; /* the coroutine stack starts its own depth budget */
}

/**
 * @brief 切出协程时保存协程侧上下文并还原调用方侧 / Save the coroutine context and restore the resumer's on the way out
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] ctx 协程的上下文块 / The coroutine's context block
 *
 * @see hpe_js_fiber_enter
 */
void hpe_js_fiber_leave(HpeJs *js, HpeJsFiberCtx *ctx)
{
    ctx->coro_try_top = js->try_top;
    memcpy(ctx->coro_jmp, js->jmp, sizeof(jmp_buf));
    js->try_top = ctx->resumer_try_top;
    memcpy(js->jmp, ctx->resumer_jmp, sizeof(jmp_buf));
    js->call_depth = ctx->resumer_depth;
}

/**
 * @brief 一个生成器的挂起状态 / The suspended state of one generator
 *
 * @note 生命期由 arena 管理；协程栈由平台层负责释放 / Lifetime is arena-managed; the coroutine
 *       stack is released by the platform layer
 */
typedef struct HpeJsGen {
    HpeJs *js;
    JsObj *self;      /* the JS_OBJ_GEN object handed to script / 交给脚本的生成器对象 */
    JsObj *fn;        /* the generator function / 生成器函数 */
    JsEnv *closure;   /* captured environment / 捕获的环境 */
    JsValue this_v;
    JsValue *args;
    int argc;
    HpeCoro *coro;    /* NULL once the body has run to completion / 函数体跑完后为 NULL */
    int state;        /* 0 = suspended at start, 1 = running, 2 = finished / 起始/运行中/已结束 */
    int started;      /* has the coroutine been resumed at least once / 是否已启动 */
    JsValue sent;     /* value passed to `next(v)` / `next(v)` 传入的值 */
    JsValue yielded;  /* value produced by the last `yield` / 上次 `yield` 产出的值 */
    int has_error;    /* an exception is parked in `error` / `error` 中存有异常 */
    JsValue error;
    JsValue ret;      /* the body's return value / 函数体的返回值 */
    /* `throw(v)` parks the exception here; the coroutine re-raises it right after
     * resuming so the body's own `try` frames (which live on the coroutine stack)
     * can catch it. */
    int has_pending_throw;
    JsValue pending_throw;
    /* `return(v)` unwinds the body the same way, but with a distinguished value
     * that `exec_try_catch` refuses to catch, so `finally` blocks still run. */
    int has_pending_return;
    JsValue pending_return;
    /* the value a `yield*` delegate returned, handed back to the delegating body */
    JsValue delegate_ret;
    /* per-fiber exception context: see `HpeJsFiberCtx` for why this is needed */
    HpeJsFiberCtx fib;
} HpeJsGen;

static void gen_body(void *ud);
static JsValue gen_next(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud);
static JsValue gen_return(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud);
static JsValue gen_self(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud);

/**
 * @brief 取 `ud` 里的生成器状态 / Fetch the generator state behind `ud`
 *
 * @param[in,out] ud 指向 `HpeJsGen` / Points at the `HpeJsGen`
 *
 * @return 生成器状态指针，`ud` 为空时返回 `NULL` / The generator state, or `NULL` for a null `ud`
 */
static HpeJsGen *gen_of(void *ud)
{
    return (HpeJsGen *)ud;
}

/**
 * @brief 构造 `{ value, done }` 结果对象 / Build a `{ value, done }` result object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     value 结果值 / The value
 * @param[in]     done 是否已完成 / Whether the generator is done
 *
 * @return 新建的结果对象 / The fresh result object
 */
static JsValue gen_result(HpeJs *js, JsValue value, int done)
{
    JsObj *o = js_object_new(js);
    js_obj_set(js, o, "value", value);
    js_obj_set(js, o, "done", js_bool(done));
    return js_objval(o);
}

/**
 * @brief 协程入口：在独立栈上运行函数体 / Coroutine entry: run the body on its own stack
 *
 * @param[in,out] ud 指向 `HpeJsGen` / Points at the `HpeJsGen`
 *
 * @note 函数体正常返回后把返回值存进 `ret`；若中途抛错，先从 `js->thrown` 取回异常再记录。
 *       `js->try_top` / `js->jmp` 由 `gen_resume` 在切换时交换，本函数不再自行改动。
 *       A normal return stores the value in `ret`; an exception is lifted out of `js->thrown`
 *       first. `js->try_top` / `js->jmp` are swapped by `gen_resume` around the switch, so this
 *       function leaves them alone.
 */
static void gen_body(void *ud)
{
    HpeJsGen *g = gen_of(ud);
    HpeJs *js = g->js;
    JsValue out;

    if (setjmp(js->jmp) == 0) {
        out = js_run_generator_body(js, g->fn, g->closure, g->this_v, g->args, g->argc);
        g->ret = out;
    } else if (js->thrown.t == JS_OBJ && js->thrown.obj == js->gen_ret_sentinel) {
        /* the return completion finished unwinding: it is the return value, not
         * an error */
        g->ret = g->pending_return;
        g->has_pending_return = 0;
        js->thrown = js_undef();
    } else {
        g->has_error = 1;
        g->error = js->thrown;
        js->thrown = js_undef();
    }
    g->state = 2;
}

/**
 * @brief 把控制权交给生成器体，直到它 yield 或结束 / Hand control to the body until it yields or finishes
 *
 * @param[in,out] g 生成器状态 / The generator state
 *
 * @return 仍在运行返回 1，已结束返回 0 / 1 when still live, 0 when finished
 *
 * @note 这里做两件事，都是为了让「一个生成器 = 一个独立栈」这件事对上层透明：
 *       1. `gen_active` 在此设置并还原，使 `yield*` 的嵌套按栈式语义工作（内层挂起时
 *          `gen_active` 指向内层，内层 yield 回外层后还原成外层）。
 *       2. `try_top` 与 `jmp` 在此**交换**：协程侧的暂存进 `fib.coro_*`，调用方侧的暂存进
 *          `fib.resumer_*`。这两个槽在 `HpeJs` 上是全局单例，不交换的话协程里的
 *          `longjmp` 会落到另一个栈上的帧，Windows 会直接以 `0xC0000028` 终止进程。
 *       交换在**每次**切换前后都做，因为协程可能在任意深的 `try` 里挂起。
 *       Two things happen here so "one generator = one stack" stays invisible above:
 *       1. `gen_active` is set/restored so `yield*` nests stack-wise.
 *       2. The exception context is swapped via `hpe_js_fiber_enter` / `hpe_js_fiber_leave`,
 *          since `js->try_top` and `js->jmp` are singletons on `HpeJs`.
 */
static int gen_resume(HpeJsGen *g)
{
    HpeJs *js = g->js;
    HpeJsGen *prev;
    int r;

    if (!g->coro)
        return 0;
    prev = js->gen_active;
    hpe_js_fiber_enter(js, &g->fib);
    js->gen_active = g;
    g->state = 1;
    r = hpe_coro_resume(g->coro);
    hpe_js_fiber_leave(js, &g->fib);
    js->gen_active = prev;
    return r;
}

/**
 * @brief 恢复生成器函数体直到下一个 `yield` 或结束 / Resume the body until the next `yield` or completion
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     g 生成器状态 / The generator state
 * @param[in]     sent 传入生成器的值 / The value handed to the body
 *
 * @return `{ value, done }` 结果 / The `{ value, done }` result
 *
 * @note 已结束的生成器再被 `next` 只返回 `{ undefined, true }` / Calling `next` on a finished generator yields `{ undefined, true }`
 */
static JsValue gen_step(HpeJs *js, HpeJsGen *g, JsValue sent)
{
    JsValue out;

    if (!g || g->state == 2)
        return gen_result(js, js_undef(), 1);
    g->sent = sent;
    if (!g->started) {
        g->started = 1;
        g->yielded = js_undef();
    }
    gen_resume(g);
    if (g->has_error) {
        g->has_error = 0;
        js_throw_value(js, g->error);
    }
    if (g->state == 2)
        out = gen_result(js, g->ret, 1);
    else
        out = gen_result(js, g->yielded, 0);
    return out;
}

/**
 * @brief `Generator.prototype.next` / `Generator.prototype.next`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 忽略 / Ignored
 * @param[in,out] args 首个实参作为送入值 / The first argument is the value sent in
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 指向 `HpeJsGen` / Points at the `HpeJsGen`
 *
 * @return `{ value, done }` / The `{ value, done }` result
 */
static JsValue gen_next(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HPE_UNUSED(thisv);
    return gen_step(js, gen_of(ud), argc > 0 ? args[0] : js_undef());
}

/**
 * @brief `Generator.prototype.return`：提前结束生成器 / `Generator.prototype.return`: finish early
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 忽略 / Ignored
 * @param[in,out] args 首个实参作为返回值 / The first argument is the return value
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 指向 `HpeJsGen` / Points at the `HpeJsGen`
 *
 * @return `{ value, done: true }` / The `{ value, done: true }` result
 */
static JsValue gen_return(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HpeJsGen *g = gen_of(ud);
    JsValue v = argc > 0 ? args[0] : js_undef();
    HPE_UNUSED(thisv);
    if (!g || g->state == 2)
        return gen_result(js, v, 1);
    /* A never-started generator never enters its body, so there is no `finally`
     * to run: complete immediately. */
    if (g->state == 0 && !g->started) {
        if (g->coro)
            hpe_coro_destroy(g->coro);
        g->coro = NULL;
        g->state = 2;
        return gen_result(js, v, 1);
    }
    /* Otherwise unwind the body so its `finally` blocks run: park the value and
     * resume with the return-completion marker, which `exec_try_catch` refuses
     * to catch and `exec_try_finally` runs around. */
    g->pending_return = v;
    g->has_pending_return = 1;
    return gen_step(js, g, js_undef());
}

/**
 * @brief `Generator.prototype.throw`：向生成器体内抛错 / `Generator.prototype.throw`: raise inside the body
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 忽略 / Ignored
 * @param[in,out] args 首个实参作为异常值 / The first argument is the exception value
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 指向 `HpeJsGen` / Points at the `HpeJsGen`
 *
 * @return 未捕获时不会返回（异常继续外抛）/ Never returns when the body does not catch it
 *
 * @note 生成器体内 `try` 能接住；接不住就在调用方抛出 / A `try` inside the body catches it; otherwise it propagates to the caller
 */
static JsValue gen_throw(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HpeJsGen *g = gen_of(ud);
    JsValue v = argc > 0 ? args[0] : js_undef();
    HPE_UNUSED(thisv);
    if (!g || g->state == 2) {
        js_throw_value(js, v);
        return js_undef();
    }
    /* A start-suspended generator never runs its body when resumed with a throw:
     * per spec it goes straight to "completed" and the value is thrown out, so
     * the body's own `try` (which has not been entered) cannot catch it. */
    if (g->state == 0 && !g->started) {
        if (g->coro)
            hpe_coro_destroy(g->coro);
        g->coro = NULL;
        g->state = 2;
        js_throw_value(js, v);
        return js_undef();
    }
    /* Park the exception and resume: the body's `try` frames are on the
     * coroutine stack, so only a raise from *there* can reach them. */
    g->pending_throw = v;
    g->has_pending_throw = 1;
    return gen_step(js, g, js_undef());
}

/**
 * @brief 生成器对象自身的 `Symbol.iterator` / The generator object's own `Symbol.iterator`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 生成器对象 / The generator object
 * @param[in,out] args 忽略 / Ignored
 * @param[in]     argc 忽略 / Ignored
 * @param[in,out] ud 忽略 / Ignored
 *
 * @return `thisv` 本身 / `thisv` itself
 */
static JsValue gen_self(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HPE_UNUSED(js);
    HPE_UNUSED(args);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    return thisv;
}

/**
 * @brief 创建一个惰性生成器 / Create a lazy generator
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] fn 生成器函数对象 / The generator function object
 * @param[in,out] closure 闭包捕获的环境 / The captured environment
 * @param[in]     this_v 调用时的 this 值 / The `this` value of the call
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 实参个数 / Argument count
 *
 * @return 生成器对象值 / The generator object value
 *
 * @note 函数体此时**不执行**，只建立协程；`args` 会复制进 arena，因此调用方的栈数组可以释放 / The body does **not** run yet; only the coroutine is built. `args` is copied into the arena so the caller's stack array may go away
 */
JsValue hpe_js_gen_create(HpeJs *js, JsObj *fn, JsEnv *closure, JsValue this_v, JsValue *args,
                          int argc)
{
    HpeJsGen *g = (HpeJsGen *)hpe_js_alloc_typed(js, sizeof(*g), GC_T_GEN);
    JsObj *o = js_object_raw(js);
    int i;

    if (!js->gc_scan_gen)
        js->gc_scan_gen = hpe_js_gen_gc_scan;

    g->js = js;
    g->fn = fn;
    g->closure = closure;
    g->this_v = this_v;
    g->argc = argc;
    g->sent = js_undef();
    g->yielded = js_undef();
    g->error = js_undef();
    g->ret = js_undef();
    if (argc > 0) {
        g->args = (JsValue *)hpe_js_alloc(js, (size_t)argc * sizeof(JsValue));
        for (i = 0; i < argc; i++)
            g->args[i] = args[i];
    }

    o->kind = JS_OBJ_GEN;
    o->class_name = "Generator";
    o->proto = js->object_proto;
    g->self = o;
    js_obj_set(js, o, "next", js_objval(js_native_new(js, gen_next, g)));
    js_obj_set(js, o, "return", js_objval(js_native_new(js, gen_return, g)));
    js_obj_set(js, o, "throw", js_objval(js_native_new(js, gen_throw, g)));
    js_obj_set(js, o, "Symbol.iterator", js_objval(js_native_new(js, gen_self, g)));
    o->payload = NULL;
    o->payload_obj = NULL;
    /* stash the state so the host side and GC teardown can find it */
    o->vt = NULL;
    o->native_ud = g;
    g->coro = hpe_coro_create(gen_body, g, GEN_STACK_BYTES);
    return js_objval(o);
}

/**
 * @brief 返回生成器状态（供迭代协议使用）/ Expose the generator state to the iteration protocol
 *
 * @param[in]     obj 生成器对象，可为 `NULL` / The generator object, may be `NULL`
 *
 * @return 状态指针；非生成器返回 `NULL` / The state pointer, or `NULL` for a non-generator
 */
HpeJsGen *hpe_js_gen_of(JsObj *obj)
{
    if (!obj || obj->kind != JS_OBJ_GEN)
        return NULL;
    return (HpeJsGen *)obj->native_ud;
}

/**
 * @brief 从生成器拉取下一项 / Pull the next item from a generator
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] obj 生成器对象 / The generator object
 * @param[out]    out 接收 `{ value, done }` / Receives the `{ value, done }` result
 *
 * @return 仍在运行返回 1；已结束返回 0 / 1 when still live, 0 when finished
 */
int hpe_js_gen_pull(HpeJs *js, JsObj *obj, JsValue *out)
{
    HpeJsGen *g = hpe_js_gen_of(obj);
    if (!g)
        return 0;
    *out = gen_step(js, g, js_undef());
    return g->state != 2;
}

/**
 * @brief 把一个生成器注册到 `HpeJs`，使 `yield` 能暂停它 / Register a generator so `yield` can park it
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] g 生成器状态 / The generator state
 */
void hpe_js_gen_enter(HpeJs *js, HpeJsGen *g)
{
    js->gen_active = g;
}

/**
 * @brief 结束对生成器的注册 / Clear the generator registration
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 */
void hpe_js_gen_leave(HpeJs *js)
{
    js->gen_active = NULL;
}

/**
 * @brief 取出当前正在执行的生成器 / Fetch the generator currently running
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @return 生成器状态；不在生成器体内时为 `NULL` / The generator state, or `NULL` outside a generator body
 */
HpeJsGen *hpe_js_gen_active(HpeJs *js)
{
    return js->gen_active;
}

/**
 * @brief 记录本次 `yield` 产出的值 / Record the value produced by the current `yield`
 *
 * @param[in,out] js 忽略 / Ignored
 * @param[in,out] g 生成器状态 / The generator state
 * @param[in]     value 产出的值 / The produced value
 */
void hpe_js_gen_set_yielded(HpeJs *js, HpeJsGen *g, JsValue value)
{
    HPE_UNUSED(js);
    if (g)
        g->yielded = value;
}

/**
 * @brief 取回 `yield` 表达式的值（`next(v)` 送入的值）/ Retrieve the `yield` expression's value
 *
 * @param[in,out] js 忽略 / Ignored
 * @param[in,out] g 生成器状态 / The generator state
 *
 * @return 送入的值；`g` 为空时返回 `undefined` / The value sent in, or `undefined` for a null `g`
 */
JsValue hpe_js_gen_get_sent(HpeJs *js, HpeJsGen *g)
{
    return g ? g->sent : js_undef();
}

/**
 * @brief 取走 `throw(v)` 停放的异常 / Take the exception parked by `throw(v)`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] g 生成器状态 / The generator state
 * @param[out]    out 接收异常值 / Receives the exception value
 *
 * @return 有待抛异常返回 1 / 1 when there was one
 *
 * @note 用返回值而不是「是否为 undefined」判定，因为 `throw undefined` 是合法的 / Uses a return
 *       flag rather than an undefined check, since `throw undefined` is legal
 */
int hpe_js_gen_take_pending_throw(HpeJs *js, HpeJsGen *g, JsValue *out)
{
    HPE_UNUSED(js);
    if (!g || !g->has_pending_throw)
        return 0;
    g->has_pending_throw = 0;
    *out = g->pending_throw;
    g->pending_throw = js_undef();
    return 1;
}

/**
 * @brief 取走 `return(v)` 停放的返回完成值 / Take the return completion parked by `return(v)`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] g 生成器状态 / The generator state
 * @param[out]    out 接收一个私有的「返回完成」标记对象 / Receives a private return-completion marker
 *
 * @return 有待处理返回完成时返回 1 / 1 when there was one
 *
 * @note 返回值是 `js->gen_ret_sentinel` 而非用户给的值本身；真正的返回值留在 `pending_return` 里，
 *       由 `gen_body` 在展开结束时取回（因为展开过程可能被 `finally` 覆盖）。
 *       The value returned is `js->gen_ret_sentinel`, not the user's value; the real value stays in
 *       `pending_return` and is recovered by `gen_body` once unwinding finishes, since a `finally`
 *       may override it.
 */
int hpe_js_gen_take_pending_return(HpeJs *js, HpeJsGen *g, JsValue *out)
{
    if (!g || !g->has_pending_return)
        return 0;
    g->has_pending_return = 0;
    *out = js_objval(js->gen_ret_sentinel);
    return 1;
}

/**
 * @brief 取生成器挂起所需的协程 / Fetch the coroutine a generator parks on
 *
 * @param[in,out] js 忽略 / Ignored
 * @param[in,out] g 生成器状态 / The generator state
 *
 * @return 协程句柄；`g` 为空时返回 `NULL` / The coroutine handle, or `NULL` for a null `g`
 */
void *hpe_js_gen_coro(HpeJs *js, HpeJsGen *g)
{
    HPE_UNUSED(js);
    return g ? g->coro : NULL;
}

/**
 * @brief 供 GC 扫描一个挂起生成器的协程栈 / GC hook: scan a suspended generator's coroutine stack
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] state 生成器状态块指针（GC_T_GEN 块的载荷）/ The generator state pointer (payload of a GC_T_GEN chunk)
 *
 * @see hpe_js_gc_collect
 */
void hpe_js_gen_gc_scan(HpeJs *js, void *state)
{
    HpeJsGen *g = (HpeJsGen *)state;
    void *lo, *hi;
    if (g && g->coro && hpe_coro_stack_range(g->coro, &lo, &hi))
        hpe_js_gc_scan_range(js, lo, hi);
}
