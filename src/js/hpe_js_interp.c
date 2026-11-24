/* Hope UI Engine - javascript tree-walking interpreter
 *
 * Covers: closures, `this` (call/apply/bind, arrow capture), prototype chains,
 * class / extends / super / new / instanceof, try/catch/finally/throw with
 * Error objects, destructuring, spread, default & rest parameters, template
 * literals, switch/labels, bitwise operators, optional chaining / nullish
 * coalescing, getters & setters, Promise + async/await, generators (eager
 * materialisation - see design doc §12.7) and ES modules.
 */
#include "js/hpe_js_internal.h"
#include "js/hpe_js_mutation.h"

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/hpe_platform.h"

/* maximum capture slots a single regex match writes into `caps`
 * (group 0 plus up to (RE_CAPS-1)/2 capturing groups) */
#define RE_CAPS 40

/**
 * @brief 分配并初始化一个 AST 节点 / Allocate and initialise an AST node
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     kind 节点种类（`JS_N_*`）/ Node kind (`JS_N_*`)
 * @param[in]     line 源码行号（用于报错）/ Source line number used for error reporting
 *
 * @return 指向新节点（已置位 kind 与 line，其余字段清零）的指针 / Pointer to the fresh node, with kind and line set and all other fields zeroed
 *
 * @note 节点由运行时分配器管理，随 `HpeJs` 一同释放 / The node is owned by the runtime allocator and freed with the `HpeJs`
 *
 * @see js_node_add
 */

JsNode *js_node_new(HpeJs *js, int kind, int line)
{
    JsNode *n = (JsNode *)hpe_js_alloc(js, sizeof(JsNode));
    n->kind = kind;
    n->line = line;
    return n;
}

/**
 * @brief 把一个子节点追加到父节点的子节点表 / Append a child node to a parent's child list
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] parent 接收子节点的父节点 / Parent that receives the child
 * @param[in,out] child 要追加的子节点 / Child node to append
 *
 * @note 子节点表按倍增策略扩容，旧内容会被拷贝 / The child array grows by doubling and copies the existing entries
 *
 * @see js_node_new
 */
void js_node_add(HpeJs *js, JsNode *parent, JsNode *child)
{
    if ((size_t)parent->kid_count + 1 > parent->kid_cap) {
        size_t ncap = parent->kid_cap ? parent->kid_cap * 2 : 4;
        while (ncap < (size_t)parent->kid_count + 1)
            ncap *= 2;
        {
            JsNode **nk = (JsNode **)hpe_js_alloc(js, ncap * sizeof(JsNode *));
            if (parent->kids && parent->kid_count)
                memcpy(nk, parent->kids, (size_t)parent->kid_count * sizeof(JsNode *));
            parent->kids = nk;
            parent->kid_cap = ncap;
        }
    }
    parent->kids[parent->kid_count++] = child;
}

/* ---------------- flow control ---------------- */

enum { FLOW_NONE = 0, FLOW_RETURN, FLOW_BREAK, FLOW_CONTINUE };

typedef struct {
    int action;
    JsValue ret;
    const char *label;
} Flow;

/**
 * @brief 构造一个表示“继续顺序执行”的 Flow 状态 / Build a Flow state meaning normal sequential execution
 *
 * @return action 为 `FLOW_NONE`、返回值 undefined、标签为 `NULL` 的 Flow / A Flow with action `FLOW_NONE`, an undefined return value and no label
 */
static Flow flow_none(void)
{
    Flow f;
    f.action = FLOW_NONE;
    f.ret = js_undef();
    f.label = NULL;
    return f;
}

/* ---------------- forward declarations ---------------- */

static JsValue eval_expr(HpeJs *js, JsEnv *env, JsNode *n);
static Flow exec_stmt(HpeJs *js, JsEnv *env, JsNode *n);
static JsValue call_value(HpeJs *js, JsEnv *env, JsValue callee, JsValue thisv, JsValue *args,
                          int argc, int line);
static Flow export_stmt(HpeJs *js, JsEnv *env, JsNode *n);
static Flow import_stmt(HpeJs *js, JsEnv *env, JsNode *n);
static JsValue super_call(HpeJs *js, JsEnv *env, JsNode *n);
static JsValue eval_super_member(HpeJs *js, JsEnv *env, JsNode *n);
/* runs a loop node; `labels` holds the labels of the enclosing labelled
 * statements (outermost first, may be NULL) so that `continue label` /
 * `break label` resolve to this loop */
static Flow exec_loop(HpeJs *js, JsEnv *env, JsNode *n, const char *const *labels, int nlabels);
static int is_loop_node(JsNode *n);
static int utf8_strlen_n(const char *s, size_t bytes);
/* builtins defined near the end of this file, forward-declared for the installers */
static JsValue set_add(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud);
static JsValue native_set_ctor(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud);
static JsValue map_size_fn(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud);
static JsValue symbol_brand(HpeJs *js, const char *desc);
static size_t utf8_offset(const char *s, int i);
static JsValue string_replace(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud);
static JsValue regex_make_result(HpeJs *js, const char *s, int *caps, int total);

/* maximum capture slots a single regex match writes into `caps` (group 0 + pairs) */
static JsValue utf8_char_at(HpeJs *js, const char *s, int i);
static int utf8_len(const char *s);
static JsValue run_function_body(HpeJs *js, JsObj *fn, JsEnv *closure, JsValue this_v, JsValue *args,
                                 int argc);
static void bind_pattern(HpeJs *js, JsEnv *env, JsNode *pat, JsValue value, int declare);
static void assign_pattern(HpeJs *js, JsEnv *env, JsNode *pat, JsValue value);
/* shared destructuring walkers: `apply` is the leaf bind/assign operation */
typedef void (*PatternApply)(HpeJs *js, JsEnv *env, JsNode *pat, JsValue value);
static void pattern_each_array(HpeJs *js, JsEnv *env, JsNode *pat, JsValue value, PatternApply apply);
static void pattern_each_object(HpeJs *js, JsEnv *env, JsNode *pat, JsValue value, JsObj *exclude,
                                PatternApply apply);
static JsValue member_get(HpeJs *js, JsEnv *env, JsValue base, const char *prop, int line);
static void member_set(HpeJs *js, JsEnv *env, JsValue base, const char *prop, JsValue value,
                       int line);
static JsValue class_eval(HpeJs *js, JsEnv *env, JsNode *n);
static JsValue make_iterable(HpeJs *js, JsObj *items);
static JsValue promise_then(HpeJs *js, JsObj *p, JsValue on_ok, JsValue on_err);

/**
 * @brief 把一个原生 C 函数包装成可调用的 JS 函数值 / Wrap a native C function into a callable JS function value
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     fn 原生函数指针 / Native function pointer
 * @param[in,out] ud 透传给原生函数的用户数据 / Opaque user data forwarded to the native function
 *
 * @return 装箱后的对象值 / The boxed object value
 */

static JsValue bind_native(HpeJs *js, JsNativeFn fn, void *ud)
{
    return js_objval(js_native_new(js, fn, ud));
}

/**
 * @brief 按下标读取数组元素 / Read an array element by index
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] arr 目标数组对象 / Target array object
 * @param[in]     i 元素下标 / Element index
 *
 * @return 下标对应的值；不存在时为 undefined / The value at the index, or undefined when absent
 *
 * @note 下标先转成十进制字符串再走属性查找 / The index is rendered as a decimal string and then looked up as a property
 */
static JsValue array_get_index(HpeJs *js, JsObj *arr, int i)
{
    char key[24];
    snprintf(key, sizeof(key), "%d", i);
    return js_obj_get(js, arr, key);
}

/**
 * @brief 按下标写入数组元素并同步 length / Write an array element by index, updating length
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] arr 目标数组对象 / Target array object
 * @param[in]     i 元素下标 / Element index
 * @param[in]     v 写入的值 / Value to store
 *
 * @note 当写入位置越过当前长度时把 `length` 提升为 `i + 1` / When the index goes past the current end, `length` is bumped to `i + 1`
 */
static void array_set_index(HpeJs *js, JsObj *arr, int i, JsValue v)
{
    char key[24];
    double len;
    snprintf(key, sizeof(key), "%d", i);
    js_obj_set(js, arr, key, v);
    len = js_to_number(js, js_obj_get(js, arr, "length"));
    if ((double)(i + 1) > len)
        js_obj_set(js, arr, "length", js_num((double)(i + 1)));
}

/**
 * @brief 把值推到数组末尾 / Append a value to the end of an array
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] arr 目标数组对象 / Target array object
 * @param[in]     v 要追加的值 / Value to append
 *
 * @note 等价于 `array_set_index(arr, length, v)` / Equivalent to `array_set_index(arr, length, v)`
 *
 * @see array_set_index
 */
static void array_push_value(HpeJs *js, JsObj *arr, JsValue v)
{
    int len = (int)js_to_number(js, js_obj_get(js, arr, "length"));
    array_set_index(js, arr, len, v);
}

/**
 * @brief 安全地读取数组长度 / Read an array length defensively
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] arr 目标数组对象 / Target array object
 *
 * @return 数组的整数长度 / The integer length of the array
 * @retval     0 `length` 为 NaN、负数或超过 1e9 时返回 0 / Returned when `length` is NaN, negative or above 1e9
 */
static int array_length(HpeJs *js, JsObj *arr)
{
    double len = js_to_number(js, js_obj_get(js, arr, "length"));
    if (len != len || len < 0.0 || len > 1e9)
        return 0;
    return (int)len;
}

/**
 * @brief 用一段 C 数组构造 JS 数组 / Build a JS array from a C array
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] v 元素值数组 / Array of element values
 * @param[in]     n 元素个数 / Number of elements to copy
 *
 * @return 包含全部元素的新数组值 / A new array value holding all elements
 */
static JsValue array_of(HpeJs *js, JsValue *v, int n)
{
    JsObj *a = js_array_new(js);
    int i;
    for (i = 0; i < n; i++)
        array_push_value(js, a, v[i]);
    return js_objval(a);
}

/**
 * @brief 判断字符串是否全部由十进制数字组成 / Test whether a string consists solely of decimal digits
 *
 * @param[in]     s 待检查的字符串 / String to inspect
 *
 * @return 全为数字且非空返回 1，否则返回 0 / 1 when non-empty and all digits, otherwise 0
 * @retval     0 为空指针、空串或含非数字字符 / `NULL`, empty, or containing a non-digit
 *
 * @note 空串视为不通过，与数组下标判定语义配合使用 / An empty string fails, matching how array-index keys are detected
 */
static int is_digits(const char *s)
{
    if (!s || !*s)
        return 0;
    while (*s) {
        if (*s < '0' || *s > '9')
            return 0;
        s++;
    }
    return 1;
}

/**
 * @brief 按 UTF-8 码点统计字符串长度 / Count a string's length in UTF-8 code points
 *
 * @param[in]     s 待统计的字符串 / String to measure
 *
 * @return 码点个数 / Number of code points
 *
 * @note 空指针按空串处理；非法的续字节按单字节前进 / A `NULL` pointer is treated as empty and an invalid sequence advances one byte
 */
static int utf8_len(const char *s)
{
    const char *p = s ? s : "";
    int n = 0;
    while (*p) {
        uint32_t cp;
        int k = hpe_utf8_next(p, &cp);
        p += k > 0 ? k : 1;
        n++;
    }
    return n;
}

/**
 * @brief 取字符串第 i 个码点的单字符子串 / Take the single-character substring at code-point index i
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     s 源字符串 / Source string
 * @param[in]     i 码点下标（从 0 起）/ Code-point index, zero based
 *
 * @return 该位置的单个码点字符串；越界时返回 undefined / The one-code-point string at that position, or undefined when out of range
 */
static JsValue utf8_char_at(HpeJs *js, const char *s, int i)
{
    const char *p = s ? s : "";
    int k = 0;
    while (k < i && *p) {
        uint32_t cp;
        p += hpe_utf8_next(p, &cp);
        k++;
    }
    if (!*p)
        return js_undef();
    {
        uint32_t cp;
        int n = hpe_utf8_next(p, &cp);
        return js_string_len(js, p, (size_t)n);
    }
}

/**
 * @brief 把作业追加到微任务队列尾部 / Append a job to the tail of the microtask queue
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] job 待入队的作业（其 next 会被清零）/ Job to enqueue, whose next pointer is cleared
 *
 * @note 仅做链表串接，不执行作业 / Only links the list; the job is not executed here
 *
 * @see hpe_js_drain_jobs
 */

void js_job_push(HpeJs *js, JsJob *job)
{
    job->next = NULL;
    if (js->jobs_tail)
        js->jobs_tail->next = job;
    else
        js->jobs_head = job;
    js->jobs_tail = job;
}

/**
 * @brief 分配一个微任务作业 / Allocate a microtask job
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     kind 作业种类（`JOB_*`）/ Job kind (`JOB_*`)
 *
 * @return 指向新作业的指针，fn/arg/value 均初始化为 undefined / Pointer to the new job whose fn/arg/value start as undefined
 *
 * @note 作业由运行时分配器托管 / The job is owned by the runtime allocator
 */
static JsJob *job_new(HpeJs *js, int kind)
{
    JsJob *j = (JsJob *)hpe_js_alloc(js, sizeof(JsJob));
    j->kind = kind;
    j->fn = js_undef();
    j->arg = js_undef();
    j->value = js_undef();
    return j;
}

/**
 * @brief 把 fn(arg) 排为微任务 / Queue fn(arg) to run as a microtask
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     fn 要调用的函数值 / Function value to call
 * @param[in]     arg 传给 fn 的单个实参 / Single argument passed to fn
 *
 * @note 以 `JOB_CALL` 入队，由解释器在当前语句/脚本之后以及 `hpe_js_call()` 中排空 / Enqueued as a `JOB_CALL` and drained after the current statement or script and by `hpe_js_call()`；`js` 为 `NULL` 时直接返回
 *
 * @see hpe_js_drain_jobs
 */
void hpe_js_queue_microtask(HpeJs *js, JsValue fn, JsValue arg)
{
    JsJob *j;
    if (!js)
        return;
    j = job_new(js, JOB_CALL);
    j->fn = fn;
    j->arg = arg;
    js_job_push(js, j);
}

/**
 * @brief 为 Promise 注册一对反应回调 / Register a pair of reaction callbacks on a promise
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] p 目标 Promise 对象 / Target promise object
 * @param[in]     on_ok 兑现回调 / Fulfilment callback
 * @param[in]     on_err 拒绝回调 / Rejection callback
 * @param[in,out] result 接收回调结果的后续 Promise（可为 `NULL`）/ Downstream promise receiving the handler result, may be `NULL`
 *
 * @note Promise 尚未定局时回调挂到链表上；已定局则立即生成 `JOB_REACTION` 入队 / If the promise is still pending the callbacks are linked; if already settled a `JOB_REACTION` job is enqueued at once
 */
static void reaction_push(HpeJs *js, JsObj *p, JsValue on_ok, JsValue on_err, JsObj *result)
{
    JsReaction *r = (JsReaction *)hpe_js_alloc(js, sizeof(JsReaction));
    r->on_ok = on_ok;
    r->on_err = on_err;
    r->result = result;
    if (!p->promise_state) {
        if (p->reactions_tail)
            p->reactions_tail->next = r;
        else
            p->reactions = r;
        p->reactions_tail = r;
        return;
    }
    {
        JsJob *j = job_new(js, JOB_REACTION);
        j->reaction = r;
        j->state = p->promise_state;
        j->value = p->promise_value;
        js_job_push(js, j);
    }
}

/**
 * @brief 把 Promise 置为已兑现或已拒绝并派发其反应 / Settle a promise and schedule its reactions
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] p 目标 Promise 对象 / Target promise object
 * @param[in]     state 新状态（1 兑现、2 拒绝）/ New state (1 fulfilled, 2 rejected)
 * @param[in]     value 随状态携带的结果值 / Result value carried with the state
 *
 * @note 只有仍处于 pending 的 Promise 会被改变，重复调用无效；已挂载的回调会被摘链并各生成一个 `JOB_REACTION` / Only a still-pending promise is changed, later calls are no-ops; attached reactions are detached and each becomes a `JOB_REACTION`
 */
void js_settle_promise(HpeJs *js, JsObj *p, int state, JsValue value)
{
    JsReaction *r, *next;
    if (!p || p->kind != JS_OBJ_PROMISE || p->promise_state)
        return;
    p->promise_state = state;
    p->promise_value = value;
    r = p->reactions;
    p->reactions = p->reactions_tail = NULL;
    for (; r; r = next) {
        JsJob *j;
        next = r->next;
        j = job_new(js, JOB_REACTION);
        j->reaction = r;
        j->state = state;
        j->value = value;
        js_job_push(js, j);
    }
}

/* resolve a promise with `v`, chaining onto it when it is thenable */
static void promise_resolve_with
(HpeJs *js, JsObj *p, JsValue v);
static JsValue native_promise_resolver(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud);

typedef struct {
    JsObj *promise;
    int reject;
} PromiseCap;

/**
 * @brief 按 thenable 规则用值 v 兑现 Promise p / Fulfil promise p with v following thenable rules
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] p 待兑现的 Promise 对象 / Promise object to fulfil
 * @param[in]     v 兑现值 / Fulfilment value
 *
 * @note v 为 p 自身时报 TypeError（链式循环）；v 为其他 Promise 时链接其状态，pending 时通过 `native_promise_resolver` 回调转发；否则直接兑现 / A TypeError is raised when v is p itself (chaining cycle); another promise is chained, pending ones being forwarded via `native_promise_resolver`; otherwise p fulfils directly
 */
static void promise_resolve_with(HpeJs *js, JsObj *p, JsValue v)
{
    if (v.t == JS_OBJ && v.obj == p) {
        js_settle_promise(js, p, 2, js_make_error(js, "TypeError", "chaining cycle detected"));
        return;
    }
    if (v.t == JS_OBJ && v.obj && v.obj->kind == JS_OBJ_PROMISE) {
        JsObj *src = v.obj;
        if (src->promise_state == 1) {
            js_settle_promise(js, p, 1, src->promise_value);
            return;
        }
        if (src->promise_state == 2) {
            js_settle_promise(js, p, 2, src->promise_value);
            return;
        }
        {
            PromiseCap *ok = (PromiseCap *)hpe_js_alloc(js, sizeof(PromiseCap));
            PromiseCap *no = (PromiseCap *)hpe_js_alloc(js, sizeof(PromiseCap));
            ok->promise = p;
            ok->reject = 0;
            no->promise = p;
            no->reject = 1;
            reaction_push(js, src, bind_native(js, native_promise_resolver, ok),
                          bind_native(js, native_promise_resolver, no), NULL);
        }
        return;
    }
    js_settle_promise(js, p, 1, v);
}

/**
 * @brief Promise 构造器内部使用的 resolve/reject 原生回调 / Native resolve/reject callback used inside the Promise constructor
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 忽略（返回 undefined）/ Ignored
 * @param[in,out] args 首个元素为传入值 / First element is the incoming value
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 指向 `PromiseCap` 的捕获块 / Pointer to the `PromiseCap` capture block
 *
 * @return 总是返回 undefined / Always returns undefined
 *
 * @note 无实参时按 undefined 处理；`cap->reject` 为真则直接拒绝，否则走 `promise_resolve_with` / A missing argument is treated as undefined; `cap->reject` rejects directly, otherwise `promise_resolve_with` is used
 */
static JsValue native_promise_resolver(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    PromiseCap *cap = (PromiseCap *)ud;
    JsValue v = argc > 0 ? args[0] : js_undef();
    HPE_UNUSED(thisv);
    if (cap->reject)
        js_settle_promise(js, cap->promise, 2, v);
    else
        promise_resolve_with(js, cap->promise, v);
    return js_undef();
}

/**
 * @brief 执行单个 Promise 反应并把结果接入后续 Promise / Run one promise reaction and feed its result to the downstream promise
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] r 要执行的反应 / Reaction to run
 * @param[in]     state 触发该反应的状态（1 兑现、2 拒绝）/ State that triggered the reaction (1 fulfilled, 2 rejected)
 * @param[in]     value 传入处理器的值 / Value passed to the handler
 *
 * @note 处理器不可调用时把状态与值原样传递下去；`result` 为 `NULL` 时丢弃处理器返回值 / When the handler is not callable the state and value pass straight through; with `result == NULL` the handler's return value is discarded
 */
static void run_reaction(HpeJs *js, JsReaction *r, int state, JsValue value)
{
    JsValue handler = (state == 1) ? r->on_ok : r->on_err;
    if (!js_is_callable(handler)) {
        if (r->result)
            js_settle_promise(js, r->result, state, value);
        return;
    }
    {
        JsValue args[1];
        JsValue out;
        args[0] = value;
        out = call_value(js, NULL, handler, js_undef(), args, 1, 0);
        if (r->result)
            promise_resolve_with(js, r->result, out);
    }
}

/**
 * @brief 排空微任务队列 / Drain the microtask queue
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @note 重入保护：`jobs_running` 为真时立即返回；本轮期间新入队的作业会一并处理 / Re-entrancy guarded by `jobs_running`; jobs enqueued during the pass are also processed
 *
 * @see js_job_push
 */
void hpe_js_drain_jobs(HpeJs *js)
{
    if (!js || js->jobs_running)
        return;
    js->jobs_running = 1;
    while (js->jobs_head) {
        JsJob *job = js->jobs_head;
        js->jobs_head = job->next;
        if (!js->jobs_head)
            js->jobs_tail = NULL;
        job->next = NULL;
        if (job->kind == JOB_CALL) {
            JsValue args[1];
            int n = 0;
            if (job->arg.t != JS_UNDEF) {
                args[0] = job->arg;
                n = 1;
            }
            call_value(js, NULL, job->fn, job->value, n ? args : NULL, n, 0);
        } else if (job->kind == JOB_SETTLE) {
            js_settle_promise(js, job->promise, job->state, job->value);
        } else if (job->kind == JOB_REACTION) {
            run_reaction(js, job->reaction, job->state, job->value);
        }
    }
    js->jobs_running = 0;
}

/**
 * @brief 为 Promise 挂上 then 回调并返回新 Promise / Attach then callbacks to a promise and return a new promise
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] p 源 Promise 对象 / Source promise object
 * @param[in]     on_ok 兑现回调 / Fulfilment callback
 * @param[in]     on_err 拒绝回调 / Rejection callback
 *
 * @return 由回调结果兑现的新 Promise 值 / A new promise value settled from the callback result
 *
 * @note 注册后立即调用 `hpe_js_drain_jobs()` / `hpe_js_drain_jobs()` is called right after registration
 */
static JsValue promise_then(HpeJs *js, JsObj *p, JsValue on_ok, JsValue on_err)
{
    JsObj *next = js_promise_new(js);
    reaction_push(js, p, on_ok, on_err, next);
    hpe_js_drain_jobs(js);
    return js_objval(next);
}

/**
 * @brief 迭代器对象的 next 方法 / The `next` method of an iterator object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 忽略 / Ignored
 * @param[in,out] args 忽略 / Ignored
 * @param[in]     argc 忽略 / Ignored
 * @param[in,out] ud 指向迭代器对象自身的指针 / Pointer to the iterator object itself
 *
 * @return `{ done, value }` 结果对象 / A `{ done, value }` result object
 *
 * @note 游标存放在对象的 `_i` 上；越界时返回 `done: true` 且 value 为 undefined / The cursor lives in the object's `_i`; past the end it returns `done: true` with value undefined
 */

static JsValue native_iter_next(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    JsObj *self = (JsObj *)ud;
    int i = (int)js_to_number(js, js_obj_get(js, self, "_i"));
    JsObj *items = self->items;
    JsObj *res = js_object_new(js);
    int len = items ? array_length(js, items) : 0;
    HPE_UNUSED(thisv);
    HPE_UNUSED(args);
    HPE_UNUSED(argc);
    if (i >= len) {
        js_obj_set(js, res, "done", js_bool(1));
        js_obj_set(js, res, "value", js_undef());
        return js_objval(res);
    }
    js_obj_set(js, self, "_i", js_num((double)(i + 1)));
    js_obj_set(js, res, "done", js_bool(0));
    js_obj_set(js, res, "value", array_get_index(js, items, i));
    return js_objval(res);
}

/**
 * @brief 返回 this 的 `Symbol.iterator` 方法 / The `Symbol.iterator` method returning `this`
 *
 * @param[in,out] js 忽略 / Ignored
 * @param[in]     thisv 迭代器自身 / The iterator itself
 * @param[in,out] args 忽略 / Ignored
 * @param[in]     argc 忽略 / Ignored
 * @param[in,out] ud 忽略 / Ignored
 *
 * @return 原样返回 `thisv` / Returns `thisv` unchanged
 *
 * @note 使迭代器自身可迭代 / Makes the iterator itself iterable
 */
static JsValue native_iter_self(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HPE_UNUSED(js);
    HPE_UNUSED(args);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    return thisv;
}

/**
 * @brief 把一个元素数组包装成可迭代的迭代器对象 / Wrap an element array into an iterable iterator object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] items 被迭代的元素数组 / Element array being iterated
 *
 * @return 带有 `next` 与 `Symbol.iterator` 的迭代器对象值 / An iterator object value carrying `next` and `Symbol.iterator`
 *
 * @note 对象的 `_i` 游标从 0 起，`class_name` 为 `Iterator` / The object's `_i` cursor starts at 0 and its `class_name` is `Iterator`
 */
static JsValue make_iterable(HpeJs *js, JsObj *items)
{
    JsObj *o = js_object_raw(js);
    o->kind = JS_OBJ_PLAIN;
    o->class_name = "Iterator";
    o->proto = js->object_proto;
    o->items = items;
    js_obj_set(js, o, "_i", js_num(0.0));
    js_obj_set(js, o, "next", bind_native(js, native_iter_next, o));
    js_obj_set(js, o, "Symbol.iterator", bind_native(js, native_iter_self, NULL));
    return js_objval(o);
}

/**
 * @brief 取字符串原型对象 / Get the string prototype object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @return `String.prototype` 对象 / The `String.prototype` object
 */

static JsObj *proto_of_string(HpeJs *js)
{
    return js->string_proto;
}

/**
 * @brief 按原始值类型选择对应的原型对象 / Pick the prototype matching a primitive value's type
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     base 参与属性查找的原始值 / Primitive value whose member is being resolved
 *
 * @return 字符串/数字/布尔值分别返回对应原型，其余返回 `Object.prototype` / The matching prototype for string, number and boolean, otherwise `Object.prototype`
 */
static JsObj *proto_for(HpeJs *js, JsValue base)
{
    switch (base.t) {
    case JS_STR:
        return js->string_proto;
    case JS_NUM:
        return js->number_proto;
    case JS_BOOL:
        return js->bool_proto;
    default:
        return js->object_proto;
    }
}

/**
 * @brief 在原型对象上查找属性并处理取值器 / Look up a property on a prototype and invoke its getter
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] proto 原型对象（可为 `NULL`）/ Prototype object, may be `NULL`
 * @param[in]     thisv 调用取值器时的 this 值 / The `this` value for the getter
 * @param[in]     prop 属性名 / Property name
 * @param[in]     line 出错时报告的行号 / Line number reported on error
 *
 * @return 属性值；属性缺失时返回 undefined / The property value, or undefined when the property is absent
 *
 * @note 存在 getter 时以 `thisv` 调用它，否则返回属性槽的值 / When a getter exists it is called with `thisv`; otherwise the property slot value is returned
 */
static JsValue proto_lookup(HpeJs *js, JsObj *proto, JsValue thisv, const char *prop, int line)
{
    JsProp *p = proto ? js_obj_find_prop(proto, prop) : NULL;
    if (!p)
        return js_undef();
    if (p->getter.t != JS_UNDEF)
        return call_value(js, NULL, p->getter, thisv, NULL, 0, line);
    return p->val;
}

/**
 * @brief 通用的成员读取，覆盖原始值、宿主对象与对象属性 / General member read covering primitives, host objects and object properties
 *
 * @details 字符串上 `length` 返回码点数，纯数字键返回对应码点字符，其余走 `String.prototype`。数字与布尔值走各自原型。宿主对象先尝试其虚表的 `get`。模块对象在自身无该属性时回退到命名空间对象。
 *          On strings `length` yields the code-point count and an all-digit key yields the matching code point, everything else going through `String.prototype`. Numbers and booleans use their own prototypes. Host objects first try their vtable's `get`. A module object falls back to its namespace object when the property is absent.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境（当前实现未使用）/ Lexical environment, unused by the current implementation
 * @param[in]     base 参与属性查找的值 / Value whose member is being resolved
 * @param[in]     prop 属性名 / Property name
 * @param[in]     line 出错时报告的行号 / Line number reported on error
 *
 * @return 属性值；查不到时返回 undefined / The property value, or undefined when not found
 *
 * @note 对 `null` / `undefined` 报错 `cannot read property` / Raises `cannot read property` for `null` / `undefined`
 */
static JsValue member_get(HpeJs *js, JsEnv *env, JsValue base, const char *prop, int line)
{
    HPE_UNUSED(env);
    if (base.t == JS_STR) {
        if (!strcmp(prop, "length"))
            return js_num((double)utf8_len(base.str));
        if (is_digits(prop))
            return utf8_char_at(js, base.str, atoi(prop));
        return proto_lookup(js, js->string_proto, base, prop, line);
    }
    if (base.t == JS_NUM || base.t == JS_BOOL)
        return proto_lookup(js, proto_for(js, base), base, prop, line);
    if (base.t == JS_OBJ && base.obj) {
        JsObj *o = base.obj;
        if (o->kind == JS_OBJ_HOST && o->vt && o->vt->get) {
            JsValue out = js_undef();
            if (o->vt->get(js, o->host_ptr, prop, &out))
                return out;
        }
        {
            JsProp *p = js_obj_find_prop(o, prop);
            if (!p && o->kind == JS_OBJ_PLAIN && o->items)
                p = NULL;
            if (p) {
                if (p->getter.t != JS_UNDEF)
                    return call_value(js, NULL, p->getter, base, NULL, 0, line);
                return p->val;
            }
        }
        if (o->kind == JS_OBJ_MODULE && o->module && o->module->ns && o->module->ns != o)
            return js_obj_get(js, o->module->ns, prop);
        return js_undef();
    }
    if (base.t == JS_NULL || base.t == JS_UNDEF)
        js_error(js, line, "cannot read property '%s' of %s", prop,
                 base.t == JS_NULL ? "null" : "undefined");
    return js_undef();
}

/**
 * @brief 通用的成员写入，覆盖宿主对象、访问器与数组长度 / General member write covering host objects, accessors and array length
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境（当前实现未使用）/ Lexical environment, unused by the current implementation
 * @param[in]     base 写入目标值 / Value being written to
 * @param[in]     prop 属性名 / Property name
 * @param[in]     value 写入的值 / Value to store
 * @param[in]     line 出错时报告的行号 / Line number reported on error
 *
 * @note 宿主对象的写入交给其虚表 `set`；有 setter 时以 base 为 this 调用；数组的纯数字键会同步 `length`。对原始值写入静默忽略，对 `null` / `undefined` 报错 / Host writes go to the vtable `set`; a setter is called with base as `this`; a digit key on an array updates `length`. Writes to primitives are silently ignored and `null` / `undefined` raise an error
 */
static void member_set(HpeJs *js, JsEnv *env, JsValue base, const char *prop, JsValue value,
                       int line)
{
    HPE_UNUSED(env);
    if (base.t == JS_OBJ && base.obj) {
        JsObj *o = base.obj;
        JsProp *p;
        if (o->kind == JS_OBJ_HOST) {
            if (o->vt && o->vt->set && o->vt->set(js, o->host_ptr, prop, value))
                return;
            return;
        }
        p = js_obj_find_prop(o, prop);
        if (p && p->setter.t != JS_UNDEF) {
            JsValue args[1];
            args[0] = value;
            call_value(js, NULL, p->setter, base, args, 1, line);
            return;
        }
        if (o->kind == JS_OBJ_ARRAY && is_digits(prop)) {
            double len = js_to_number(js, js_obj_get(js, o, "length"));
            int idx = atoi(prop);
            if ((double)(idx + 1) > len)
                js_obj_set(js, o, "length", js_num((double)idx + 1.0));
        }
        js_obj_set(js, o, prop, value);
        return;
    }
    if (base.t == JS_STR || base.t == JS_NUM || base.t == JS_BOOL)
        return;
    if (base.t == JS_NULL || base.t == JS_UNDEF)
        js_error(js, line, "cannot set property '%s' of %s", prop,
                 base.t == JS_NULL ? "null" : "undefined");
}

/**
 * @brief 对外暴露的成员读取入口 / Public member-read entry point
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in]     base 参与属性查找的值 / Value whose member is being resolved
 * @param[in]     prop 属性名 / Property name
 * @param[in]     line 出错时报告的行号 / Line number reported on error
 *
 * @return 属性值 / The property value
 *
 * @note 直接转发给 `member_get` / Forwards straight to `member_get`
 *
 * @see member_get
 */
JsValue js_get_prop(HpeJs *js, JsEnv *env, JsValue base, const char *prop, int line)
{
    return member_get(js, env, base, prop, line);
}

/**
 * @brief 对外暴露的成员写入入口 / Public member-write entry point
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in]     base 写入目标值 / Value being written to
 * @param[in]     prop 属性名 / Property name
 * @param[in]     value 写入的值 / Value to store
 * @param[in]     line 出错时报告的行号 / Line number reported on error
 *
 * @note 直接转发给 `member_set` / Forwards straight to `member_set`
 *
 * @see member_set
 */
void js_set_prop(HpeJs *js, JsEnv *env, JsValue base, const char *prop, JsValue value, int line)
{
    member_set(js, env, base, prop, value, line);
}

/**
 * @brief 构造函数内部的 arguments 对象 / Build the `arguments` object for a function call
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 实参个数 / Argument count
 *
 * @return 包含全部实参与 `length` 的 `Arguments` 对象值 / An `Arguments` object value holding all arguments plus `length`
 *
 * @note 元素以十进制字符串为键存放，原型为 `Object.prototype` / Elements are stored under decimal string keys with `Object.prototype` as prototype
 */

static JsValue make_arguments_object(HpeJs *js, JsValue *args, int argc)
{
    JsObj *o = js_object_raw(js);
    int i;
    o->kind = JS_OBJ_ARGUMENTS;
    o->class_name = "Arguments";
    o->proto = js->object_proto;
    for (i = 0; i < argc; i++) {
        char key[24];
        snprintf(key, sizeof(key), "%d", i);
        js_obj_set(js, o, key, args[i]);
    }
    js_obj_set(js, o, "length", js_num((double)argc));
    return js_objval(o);
}

/**
 * @brief 在新建的函数环境中执行函数体 / Execute a function body in a fresh function environment
 *
 * @details 非箭头函数会绑定 `this` 并定义 `arguments`；有 [[HomeObject]] 时一并写入环境。形参按顺序绑定，支持默认值（`undefined` 时求值）与 rest 参数（收集剩余实参为数组）。函数名在环境内未定义时以函数自身回填（便于递归）。
 *          A non-arrow function binds `this` and defines `arguments`; a [[HomeObject]] is carried into the environment when present. Parameters bind in order, supporting defaults (evaluated for `undefined`) and rest (remaining arguments gathered into an array). The function name is back-filled with the function itself when not already defined, enabling recursion.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] fn 函数对象（其 `code` 给出形参与函数体）/ Function object whose `code` supplies the parameters and body
 * @param[in,out] closure 闭包捕获的外层环境 / Outer environment captured by the closure
 * @param[in]     this_v 调用时的 this 值 / The `this` value of the call
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 实参个数 / Argument count
 *
 * @return `return` 流携带的返回值，否则为 undefined / The value carried by a `return` flow, otherwise undefined
 *
 * @note 由 `call_value` / `run_generator` / `run_async` 等调用 / Called from `call_value`, `run_generator`, `run_async` and friends
 */
static JsValue run_function_body(HpeJs *js, JsObj *fn, JsEnv *closure, JsValue this_v, JsValue *args,
                                 int argc)
{
    JsNode *code = fn->code;
    JsNode *params = code->kids[0];
    JsNode *body = code->kids[1];
    JsEnv *fenv = js_env_new(js, closure);
    Flow flow;
    int i;

    if (!(fn->fn_flags & FN_ARROW)) {
        fenv->has_this = 1;
        fenv->thisv = this_v;
        js_env_define(js, fenv, "arguments", make_arguments_object(js, args, argc));
    }
    if (fn->home) {
        fenv->has_home = 1;
        fenv->home = fn->home;
    }
    for (i = 0; i < params->kid_count; i++) {
        JsNode *p = params->kids[i];
        JsValue v = i < argc ? args[i] : js_undef();
        if (!p)
            continue;
        if (p->kind == JS_N_PAT_REST) {
            JsObj *rest = js_array_new(js);
            int k;
            for (k = i; k < argc; k++)
                array_push_value(js, rest, args[k]);
            bind_pattern(js, fenv, p->kids[0], js_objval(rest), 1);
            break;
        }
        if (p->kind == JS_N_PAT_DEFAULT) {
            if (v.t == JS_UNDEF)
                v = eval_expr(js, fenv, p->kids[1]);
            bind_pattern(js, fenv, p->kids[0], v, 1);
            continue;
        }
        bind_pattern(js, fenv, p, v, 1);
    }
    if (fn->name && *fn->name && !js_env_lookup(fenv, fn->name))
        js_env_define(js, fenv, fn->name, js_objval(fn));

    flow = exec_stmt(js, fenv, body);
    return flow.action == FLOW_RETURN ? flow.ret : js_undef();
}

/**
 * @brief 以急切求值方式运行生成器函数 / Run a generator function by materialising its yields eagerly
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] fn 生成器函数对象 / Generator function object
 * @param[in,out] closure 闭包捕获的外层环境 / Outer environment captured by the closure
 * @param[in]     this_v 调用时的 this 值 / The `this` value of the call
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 实参个数 / Argument count
 *
 * @return 包裹已收集产出值的可迭代对象 / An iterable wrapping the collected yielded values
 *
 * @note 所有 yield 立即收集到数组（设计文档 §12.7），暂停后恢复的 `yield_sink` 会被还原 / All yields are collected into an array up front (design §12.7); the previous `yield_sink` is restored afterwards
 */
static JsValue run_generator(HpeJs *js, JsObj *fn, JsEnv *closure, JsValue this_v, JsValue *args,
                             int argc)
{
    JsObj *collected = js_array_new(js);
    JsObj *prev = js->yield_sink;
    js->yield_sink = collected;
    run_function_body(js, fn, closure, this_v, args, argc);
    js->yield_sink = prev;
    return make_iterable(js, collected);
}

/**
 * @brief 为实例求值并安装类字段 / Evaluate and install a class's instance fields on an object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] ctor 类构造函数（其 `class_def` 给出类节点）/ Class constructor whose `class_def` gives the class node
 * @param[in]     instance 接收字段的实例 / Instance receiving the fields
 *
 * @note 在实例环境中求值各字段初始化器，无初始化器的字段置 undefined；类节点缺失或成员表为空时直接返回 / Initialisers are evaluated in a per-instance environment and a field without initialiser becomes undefined; returns early when the class node is missing or the member list is empty
 */
static void apply_class_fields(HpeJs *js, JsObj *ctor, JsValue instance)
{
    JsNode *cls = ctor->class_def;
    JsNode *members;
    JsEnv *fenv;
    int i;
    if (!cls || cls->kind != JS_N_CLASS || cls->kid_count < 2)
        return;
    members = cls->kids[1];
    if (!members)
        return;
    fenv = js_env_new(js, ctor->closure);
    fenv->has_this = 1;
    fenv->thisv = instance;
    if (ctor->home) {
        fenv->has_home = 1;
        fenv->home = ctor->home;
    }
    for (i = 0; i < members->kid_count; i++) {
        JsNode *m = members->kids[i];
        if (m && m->op == JS_MEMBER_FIELD) {
            /* kids[0] = initialiser (may be NULL), kids[1] = computed key */
            JsValue v = (m->kid_count > 0 && m->kids[0]) ? eval_expr(js, fenv, m->kids[0])
                                                        : js_undef();
            member_set(js, fenv, instance, m->str, v, m->line);
        }
    }
}

/**
 * @brief 以返回 Promise 的方式运行异步函数 / Run an async function, returning a promise
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] fn 异步函数对象 / Async function object
 * @param[in,out] closure 闭包捕获的外层环境 / Outer environment captured by the closure
 * @param[in]     this_v 调用时的 this 值 / The `this` value of the call
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 实参个数 / Argument count
 *
 * @return 由函数体返回值兑现的 Promise 值 / A promise value fulfilled from the body's return value
 *
 * @note 创建 Promise 后运行函数体并用 `promise_resolve_with` 接入结果，随后排空作业队列 / Creates the promise, runs the body and wires the result through `promise_resolve_with`, then drains the job queue
 */
static JsValue run_async(HpeJs *js, JsObj *fn, JsEnv *closure, JsValue this_v, JsValue *args,
                         int argc)
{
    JsObj *p = js_promise_new(js);
    JsValue out = run_function_body(js, fn, closure, this_v, args, argc);
    promise_resolve_with(js, p, out);
    hpe_js_drain_jobs(js);
    return js_objval(p);
}

/**
 * @brief 按函数值的种类分派调用 / Dispatch a call according to the kind of function value
 *
 * @details 不可调用时报错。绑定函数（`FN_BOUND`）会把已绑定实参与本次实参拼接后递归调用目标，`this` 取绑定值（非 undefined 时）。原生函数直接调用其 C 指针；生成器走 `run_generator`，异步函数走 `run_async`，普通函数走 `run_function_body`。
 *          Errors out when not callable. A bound function (`FN_BOUND`) concatenates its bound arguments with the new ones and recurses into the target, taking `this` from the bound value when it is not undefined. Native functions call their C pointer directly; generators go through `run_generator`, async functions through `run_async` and plain functions through `run_function_body`.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境（当前实现未使用）/ Lexical environment, unused by the current implementation
 * @param[in]     callee 被调用的函数值 / Function value being called
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 实参个数 / Argument count
 * @param[in]     line 出错时报告的行号 / Line number reported on error
 *
 * @return 调用结果值 / The call result value
 *
 * @see js_invoke
 */
static JsValue call_value(HpeJs *js, JsEnv *env, JsValue callee, JsValue thisv, JsValue *args,
                          int argc, int line)
{
    JsObj *fn;
    HPE_UNUSED(env);
    if (!js_is_callable(callee))
        js_error(js, line, "value is not a function");
    fn = callee.obj;
    if (fn->fn_flags & FN_BOUND) {
        JsValue *all;
        int i;
        JsObj *target = fn->bound_target;
        JsValue this_for_call = fn->bound_this.t == JS_UNDEF ? thisv : fn->bound_this;
        if (!target)
            js_error(js, line, "bound function target missing");
        all = (JsValue *)hpe_js_alloc(js, (size_t)(fn->bound_argc + argc + 1) * sizeof(JsValue));
        for (i = 0; i < fn->bound_argc; i++)
            all[i] = fn->bound_args[i];
        for (i = 0; i < argc; i++)
            all[fn->bound_argc + i] = args[i];
        return call_value(js, NULL, js_objval(target), this_for_call, all, fn->bound_argc + argc,
                          line);
    }
    if (fn->kind == JS_OBJ_NATIVE)
        return fn->native(js, thisv, args, argc, fn->native_ud);
    if (fn->kind == JS_OBJ_FUNC) {
        if (fn->fn_flags & FN_GEN)
            return run_generator(js, fn, fn->closure, thisv, args, argc);
        if (fn->fn_flags & FN_ASYNC)
            return run_async(js, fn, fn->closure, thisv, args, argc);
        return run_function_body(js, fn, fn->closure, thisv, args, argc);
    }
    js_error(js, line, "value is not a function");
    return js_undef();
}

/**
 * @brief 以一个显式 this 调用函数值 / Call a function value with an explicit `this`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     fn 被调用的函数值 / Function value to call
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 实参个数 / Argument count
 * @param[in]     line 出错时报告的行号 / Line number reported on error
 *
 * @return 调用结果值 / The call result value
 *
 * @note 以 `env` 为 `NULL` 转发给 `call_value` / Forwards to `call_value` with a `NULL` environment
 *
 * @see call_value
 */
JsValue js_invoke(HpeJs *js, JsValue fn, JsValue thisv, JsValue *args, int argc, int line)
{
    return call_value(js, NULL, fn, thisv, args, argc, line);
}

/**
 * @brief 运行构造函数体并按需安装类字段 / Run a constructor body, applying class fields as appropriate
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] ctor 构造函数对象 / Constructor function object
 * @param[in]     this_v 此构造调用中的 this 值（新建实例或被派生的实例）/ The `this` value of this construction (the new instance or the derived instance)
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 实参个数 / Argument count
 * @param[in]     derived 派生类构造函数为 1 / 1 for a derived-class constructor
 *
 * @return 构造函数体返回的值 / The value returned by the constructor body
 *
 * @note 基类在函数体之前安装字段，派生类在函数体（即 `super()`）之后安装 / A base class installs fields before the body while a derived class installs them after the body, i.e. after `super()`
 */
static JsValue run_ctor_body(HpeJs *js, JsObj *ctor, JsValue this_v, JsValue *args, int argc,
                             int derived)
{
    JsValue out;
    if (!derived)
        apply_class_fields(js, ctor, this_v);
    out = run_function_body(js, ctor, ctor->closure, this_v, args, argc);
    if (derived)
        apply_class_fields(js, ctor, this_v);
    return out;
}

/**
 * @brief 以 new 语义构造对象 / Construct an object with `new` semantics
 *
 * @details 不可调用时报错。原生构造函数以其 C 指针调用，`this` 为 undefined。绑定函数拼接实参后递归构造目标。其余情况取构造函数的 `prototype` 作为新实例原型，用类名设置 `class_name` 并运行构造函数体；构造函数体返回对象则采用该对象，否则返回新实例。
 *          Errors out when not callable. A native constructor is invoked through its C pointer with an undefined `this`. Bound functions concatenate arguments and recurse. Otherwise the constructor's `prototype` becomes the instance prototype, `class_name` is set from the function name and the constructor body runs; an object returned by the body wins, otherwise the fresh instance is returned.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     callee 构造函数值 / Constructor value
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 实参个数 / Argument count
 * @param[in]     line 出错时报告的行号 / Line number reported on error
 *
 * @return 新建（或构造函数返回的）对象值 / The newly created object value, or the object returned by the constructor
 */
JsValue js_construct(HpeJs *js, JsValue callee, JsValue *args, int argc, int line)
{
    JsObj *fn;
    if (!js_is_callable(callee))
        js_error(js, line, "value is not a constructor");
    fn = callee.obj;
    if (fn->kind == JS_OBJ_NATIVE)
        return fn->native(js, js_undef(), args, argc, fn->native_ud);
    if (fn->fn_flags & FN_BOUND) {
        JsValue *all;
        int i;
        JsObj *target = fn->bound_target;
        if (!target)
            js_error(js, line, "bound function target missing");
        all = (JsValue *)hpe_js_alloc(js, (size_t)(fn->bound_argc + argc + 1) * sizeof(JsValue));
        for (i = 0; i < fn->bound_argc; i++)
            all[i] = fn->bound_args[i];
        for (i = 0; i < argc; i++)
            all[fn->bound_argc + i] = args[i];
        return js_construct(js, js_objval(target), all, fn->bound_argc + argc, line);
    }
    {
        JsValue proto_v = member_get(js, NULL, js_objval(fn), "prototype", line);
        JsObj *instance = js_object_raw(js);
        JsValue out;
        int derived = (fn->class_def && fn->home && fn->home->proto);
        instance->proto = (proto_v.t == JS_OBJ) ? proto_v.obj : js->object_proto;
        instance->class_name = fn->name ? fn->name : "Object";
        out = run_ctor_body(js, fn, js_objval(instance), args, argc, derived);
        if (out.t == JS_OBJ && out.obj)
            return out;
        return js_objval(instance);
    }
}

/**
 * @brief 把值绑定或赋值到一个绑定目标 / Bind or assign a value to a single binding target
 *
 * @details 标识符目标在声明模式下走 `js_env_declare`；赋值模式下写已存在的槽位，槽位不存在时严格模式报 `is not defined`、非严格模式写入全局环境。非标识符目标转交给 `bind_pattern` 或 `assign_pattern`。
 *          An identifier target uses `js_env_declare` when declaring; when assigning it writes the existing slot, and a missing slot raises `is not defined` in strict mode or defines it on the global environment otherwise. A non-identifier target is handed to `bind_pattern` or `assign_pattern`.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] id 目标标识符节点或解构模式 / Identifier node or destructuring pattern
 * @param[in]     v 被绑定或赋值的值 / Value being bound or assigned
 * @param[in]     declare 声明式绑定为 1，纯赋值为 0 / 1 for a declarative binding, 0 for a plain assignment
 */

static void bind_target(HpeJs *js, JsEnv *env, JsNode *id, JsValue v, int declare)
{
    if (id->kind == JS_N_IDENT) {
        if (declare)
            js_env_declare(js, env, id->str, v);
        else {
            JsValue *slot = js_env_lookup(env, id->str);
            if (slot)
                *slot = v;
            else if (env->strict)
                js_error(js, id->line, "%s is not defined", id->str);
            else
                js_env_define(js, js->global_env, id->str, v);
        }
        return;
    }
    if (declare)
        bind_pattern(js, env, id, v, 1);
    else
        assign_pattern(js, env, id, v);
}

/**
 * @brief 声明式解构的叶子操作适配器 / Leaf adapter for declarative destructuring
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] pat 解构模式节点 / Destructuring pattern node
 * @param[in]     value 被解构的值 / Value being destructured
 *
 * @note 以 `declare = 1` 调用 `bind_pattern`，使解构走声明路径 / Calls `bind_pattern` with `declare = 1` so the walk takes the declaration path
 *
 * @see bind_pattern
 */
static void pattern_bind_decl(HpeJs *js, JsEnv *env, JsNode *pat, JsValue value)
{
    bind_pattern(js, env, pat, value, 1);
}

/**
 * @brief 赋值式解构的叶子操作适配器 / Leaf adapter for plain destructuring
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] pat 解构模式节点 / Destructuring pattern node
 * @param[in]     value 被解构的值 / Value being destructured
 *
 * @note 以 `declare = 0` 调用 `bind_pattern`，使解构走赋值路径 / Calls `bind_pattern` with `declare = 0` so the walk takes the assignment path
 *
 * @see bind_pattern
 */
static void pattern_bind_plain(HpeJs *js, JsEnv *env, JsNode *pat, JsValue value)
{
    bind_pattern(js, env, pat, value, 0);
}

/**
 * @brief 按数组模式逐项解构可迭代值 / Destructure an iterable element by element against an array pattern
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] pat 数组模式节点 / Array pattern node
 * @param[in]     value 被解构的可迭代值 / Iterable value being destructured
 * @param[in,out] apply 对每个叶子目标执行绑定/赋值的回调 / Callback performing the bind or assign for each leaf target
 *
 * @note 空位（elision）跳过对应下标；遇到 rest 元素时把从该下标起的所有元素收集成数组后交给回调并结束遍历 / Elisions skip their index; a rest element gathers everything from that index into an array, is handed to the callback, and the walk ends
 */
static void pattern_each_array(HpeJs *js, JsEnv *env, JsNode *pat, JsValue value,
                               void (*apply)(HpeJs *, JsEnv *, JsNode *, JsValue))
{
    int i, n = pat->kid_count;

    for (i = 0; i < n; i++) {
        JsNode *el = pat->kids[i];
        if (!el)
            continue; /* elision: skip this index */
        if (el->kind == JS_N_PAT_REST) {
            JsObj *rest = js_array_new(js);
            int k, len = js_iter_len(js, value);
            for (k = i; k < len; k++)
                array_push_value(js, rest, js_iter_at(js, value, k));
            apply(js, env, el->kids[0], js_objval(rest));
            break;
        }
        apply(js, env, el, js_iter_at(js, value, i));
    }
}

/**
 * @brief 按对象模式逐键解构对象值 / Destructure an object value key by key against an object pattern
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] pat 对象模式节点 / Object pattern node
 * @param[in]     value 被解构的对象值 / Object value being destructured
 * @param[in,out] exclude 收集已解构键的集合（可为 `NULL`）/ Set collecting already destructured keys, may be `NULL`
 * @param[in,out] apply 对每个叶子目标执行绑定/赋值的回调 / Callback performing the bind or assign for each leaf target
 *
 * @note rest 元素收集除 `exclude` 中键以外的全部自有可枚举键；带默认值的无源属性 `{a = 1}` 在此先播种 undefined，交由 `JS_N_PAT_DEFAULT` 分支替换 / A rest element collects every own key except those in `exclude`; a defaulted property with no source `{a = 1}` is seeded with undefined here and replaced by the `JS_N_PAT_DEFAULT` arm
 */
static void pattern_each_object(HpeJs *js, JsEnv *env, JsNode *pat, JsValue value, JsObj *exclude,
                                void (*apply)(HpeJs *, JsEnv *, JsNode *, JsValue))
{
    int i;

    for (i = 0; i < pat->kid_count; i++) {
        JsNode *pair = pat->kids[i];
        if (!pair)
            continue;
        if (pair->kind == JS_N_PAT_REST) {
            JsObj *rest = js_object_new(js);
            if (value.t == JS_OBJ && value.obj) {
                char **keys;
                int nk, k;
                js_obj_own_keys(value.obj, &keys, &nk);
                for (k = 0; k < nk; k++) {
                    if (exclude && js_obj_own_has(exclude, keys[k]))
                        continue;
                    js_obj_set(js, rest, keys[k],
                               member_get(js, env, value, keys[k], pair->line));
                }
                free(keys);
            }
            apply(js, env, pair->kids[0], js_objval(rest));
            continue;
        }
        /* `{a}` stores the key in pair->str; a bare `{a: b}` nests under kids[0] */
        if (pair->str && pair->str[0]) {
            JsValue v = member_get(js, env, value, pair->str, pair->line);
            if (exclude)
                js_obj_set(js, exclude, pair->str, js_bool(1));
            apply(js, env, pair->kids[0], v);
        } else if (pair->kind == JS_N_ASSIGN) {
            /* `{a = 1}` with no source property: the default is applied by the
             * caller's JS_N_PAT_DEFAULT arm, so seed an undefined value here */
            apply(js, env, pair->kids[0], js_undef());
        }
    }
}

/**
 * @brief 在值为 undefined 时求值解构默认值 / Evaluate a destructuring default when the value is undefined
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] pat 带默认值的模式节点（默认表达式在 `kids[1]`）/ Pattern node carrying the default, whose expression is in `kids[1]`
 * @param[in]     value 待检查的值 / Value being inspected
 *
 * @return 值非 undefined 时原样返回，否则返回默认表达式的求值结果 / The value itself when not undefined, otherwise the evaluated default
 *
 * @note 只有 `undefined` 触发默认值，`null` 不触发 / Only `undefined` triggers the default; `null` does not
 */
static JsValue pattern_default_value(HpeJs *js, JsEnv *env, JsNode *pat, JsValue value)
{
    if (value.t == JS_UNDEF)
        return eval_expr(js, env, pat->kids[1]);
    return value;
}

/**
 * @brief 按声明或赋值语义解构一个模式 / Destructure a pattern in declaration or assignment mode
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] pat 解构模式节点（可为 `NULL`）/ Destructuring pattern node, may be `NULL`
 * @param[in]     value 被解构的值 / Value being destructured
 * @param[in]     declare 声明式绑定为 1，赋值为 0 / 1 for a declarative binding, 0 for an assignment
 *
 * @note 标识符走 `bind_target`；默认值先经 `pattern_default_value`；数组与对象模式分别走 `pattern_each_array` / `pattern_each_object`（声明模式下经 `pattern_bind_decl` 适配器）；无法识别的模式回退到 `assign_pattern` / Identifiers go to `bind_target`; defaults pass through `pattern_default_value`; array and object patterns go to `pattern_each_array` / `pattern_each_object` (via the `pattern_bind_decl` adapter when declaring); anything else falls back to `assign_pattern`
 */
static void bind_pattern(HpeJs *js, JsEnv *env, JsNode *pat, JsValue value, int declare)
{
    if (!pat)
        return;
    switch (pat->kind) {
    case JS_N_IDENT:
        bind_target(js, env, pat, value, declare);
        return;
    case JS_N_PAT_DEFAULT:
        bind_pattern(js, env, pat->kids[0], pattern_default_value(js, env, pat, value), declare);
        return;
    case JS_N_PAT_REST:
        bind_pattern(js, env, pat->kids[0], value, declare);
        return;
    case JS_N_PAT_ARR:
        pattern_each_array(js, env, pat, value, declare ? pattern_bind_decl : pattern_bind_plain);
        return;
    case JS_N_PAT_OBJ:
        pattern_each_object(js, env, pat, value, js_object_new(js),
                            declare ? pattern_bind_decl : pattern_bind_plain);
        return;
    default:
        assign_pattern(js, env, pat, value);
        return;
    }
}

/**
 * @brief 向成员表达式目标赋值 / Assign a value to a member-expression target
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] target 成员目标节点（`a.b` 或 `a[b]`）/ Member target node (`a.b` or `a[b]`)
 * @param[in]     value 写入的值 / Value to store
 *
 * @note 点式取 `target->str`，下标式对键表达式求值后转字符串，最终经 `member_set` 写入 / A dot form uses `target->str`, an index form evaluates and stringifies the key, and the write goes through `member_set`
 */
static void assign_member(HpeJs *js, JsEnv *env, JsNode *target, JsValue value)
{
    JsValue base = eval_expr(js, env, target->kids[0]);
    char *prop = target->op == JS_OP_DOT
                     ? target->str
                     : js_to_string(js, eval_expr(js, env, target->kids[1]));
    member_set(js, env, base, prop, value, target->line);
}

/**
 * @brief 按赋值语义解构一个模式 / Destructure a pattern in assignment mode
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] pat 解构模式节点（可为 `NULL`）/ Destructuring pattern node, may be `NULL`
 * @param[in]     value 被解构的值 / Value being destructured
 *
 * @note 标识符经 `bind_target`（`declare = 0`）写入，成员目标经 `assign_member`；数组与对象模式分别走 `pattern_each_array` / `pattern_each_object`（不收集排除键）；其余模式静默忽略 / Identifiers are written through `bind_target` with `declare = 0` and member targets through `assign_member`; array and object patterns use `pattern_each_array` / `pattern_each_object` (without an exclusion set); any other pattern is silently ignored
 */
static void assign_pattern(HpeJs *js, JsEnv *env, JsNode *pat, JsValue value)
{
    if (!pat)
        return;
    switch (pat->kind) {
    case JS_N_IDENT:
        bind_target(js, env, pat, value, 0);
        return;
    case JS_N_MEMBER:
        assign_member(js, env, pat, value);
        return;
    case JS_N_PAT_DEFAULT:
        assign_pattern(js, env, pat->kids[0], pattern_default_value(js, env, pat, value));
        return;
    case JS_N_PAT_REST:
        assign_pattern(js, env, pat->kids[0], value);
        return;
    case JS_N_PAT_ARR:
        pattern_each_array(js, env, pat, value, assign_pattern);
        return;
    case JS_N_PAT_OBJ:
        pattern_each_object(js, env, pat, value, NULL, assign_pattern);
        return;
    default:
        return;
    }
}

/**
 * @brief 按 ToInt32 规则把值转换为 32 位有符号整数 / Convert a value to a signed 32-bit integer per ToInt32
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     v 待转换的值 / Value to convert
 *
 * @return 转换结果，已对 2^32 取模并解释为有符号数 / The result, reduced modulo 2^32 and interpreted as signed
 * @retval     0 值为 NaN 或无穷时返回 0 / Returned for NaN or an infinity
 */

static int32_t to_i32(HpeJs *js, JsValue v)
{
    double d = js_to_number(js, v);
    if (d != d || isinf(d))
        return 0;
    return (int32_t)(int64_t)fmod(trunc(d), 4294967296.0);
}

/**
 * @brief 按 ToUint32 规则把值转换为 32 位无符号整数 / Convert a value to an unsigned 32-bit integer per ToUint32
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     v 待转换的值 / Value to convert
 *
 * @return 转换结果，已对 2^32 取模到 `[0, 2^32)` / The result, reduced modulo 2^32 into `[0, 2^32)`
 * @retval     0 值为 NaN 或无穷时返回 0 / Returned for NaN or an infinity
 */
static uint32_t to_u32(HpeJs *js, JsValue v)
{
    double d = js_to_number(js, v);
    if (d != d || isinf(d))
        return 0;
    return (uint32_t)(int64_t)(fmod(trunc(d), 4294967296.0) + 4294967296.0);
}

/**
 * @brief 实现 instanceof 运算符 / Implement the `instanceof` operator
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     obj 待检查的左操作数 / Left-hand operand being tested
 * @param[in]     ctor 右操作数（构造函数值）/ Right-hand operand, the constructor value
 * @param[in]     line 出错时报告的行号 / Line number reported on error
 *
 * @return 在原型链上找到构造函数原型返回 true，否则返回 false / `true` when the constructor prototype is found on the chain, otherwise `false`
 *
 * @note 左操作数非对象返回 false；右操作数不可调用或其 `prototype` 非对象时报错 / A non-object left operand yields `false`; an uncallable right operand or a non-object `prototype` raises an error
 */
static JsValue js_instanceof(HpeJs *js, JsValue obj, JsValue ctor, int line)
{
    JsObj *proto;
    JsObj *o;
    JsValue pv;
    if (!js_is_callable(ctor))
        js_error(js, line, "right-hand side of 'instanceof' is not callable");
    if (obj.t != JS_OBJ || !obj.obj)
        return js_bool(0);
    pv = member_get(js, NULL, ctor, "prototype", line);
    proto = (pv.t == JS_OBJ) ? pv.obj : NULL;
    if (!proto)
        js_error(js, line, "function has a non-object prototype in instanceof");
    for (o = obj.obj->proto; o; o = o->proto)
        if (o == proto)
            return js_bool(1);
    return js_bool(0);
}

/**
 * @brief 把两个操作数转成字符串并拼接 / Stringify both operands and concatenate them
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     l 左操作数 / Left operand
 * @param[in]     r 右操作数 / Right operand
 *
 * @return 拼接得到的新字符串值 / The concatenated string value
 *
 * @note 结果长度按两者的 UTF-8 字节数计算 / The result length is the sum of the two UTF-8 byte counts
 */
static JsValue bin_concat(HpeJs *js, JsValue l, JsValue r)
{
    char *ls = js_to_string(js, l);
    char *rs = js_to_string(js, r);
    size_t ll = strlen(ls), rl = strlen(rs);
    char *out = (char *)hpe_js_alloc(js, ll + rl + 1);

    memcpy(out, ls, ll);
    memcpy(out + ll, rs, rl);
    out[ll + rl] = '\0';
    return js_string_len(js, out, ll + rl);
}

/**
 * @brief 求值逻辑与、逻辑或与空值合并并短路 / Evaluate `&&`, `||` and `??` with short-circuiting
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 二元表达式节点（子表达式在 kids 上）/ Binary expression node whose sub-expressions live in its kids
 * @param[in]     op 运算符（`JS_OP_AND` / `JS_OP_OR` / `JS_OP_NULLISH`）/ Operator (`JS_OP_AND` / `JS_OP_OR` / `JS_OP_NULLISH`)
 *
 * @return 决定结果的原始操作数，而非强制转换后的布尔值 / The deciding operand itself rather than a coerced boolean
 *
 * @note `??` 只在左值为 `null` 或 `undefined` 时才求右值 / `??` evaluates the right side only for `null` or `undefined`
 */
static JsValue eval_logical(HpeJs *js, JsEnv *env, JsNode *n, int op)
{
    JsValue l = eval_expr(js, env, n->kids[0]);

    if (op == JS_OP_AND)
        return js_truthy(l) ? eval_expr(js, env, n->kids[1]) : l;
    if (op == JS_OP_OR)
        return js_truthy(l) ? l : eval_expr(js, env, n->kids[1]);
    /* nullish coalescing: only null and undefined fall through */
    return (l.t == JS_NULL || l.t == JS_UNDEF) ? eval_expr(js, env, n->kids[1]) : l;
}

/**
 * @brief 求值比较运算符 / Evaluate the relational operators
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] n 比较表达式节点（决定具体运算符）/ Comparison node determining the operator
 * @param[in]     l 左操作数 / Left operand
 * @param[in]     r 右操作数 / Right operand
 *
 * @return `<` `>` `<=` `>=` 的布尔结果 / The boolean result of `<`, `>`, `<=` or `>=`
 *
 * @note 两侧均为字符串时按字典序 `strcmp` 比较，否则按数值比较；任一操作数为 NaN 时结果为 false / Two strings compare lexicographically via `strcmp`, otherwise numerically; a NaN operand always yields `false`
 */
static JsValue eval_relational(HpeJs *js, JsNode *n, JsValue l, JsValue r)
{
    int op = n->op;
    int res;

    if (l.t == JS_STR && r.t == JS_STR) {
        int c = strcmp(l.str ? l.str : "", r.str ? r.str : "");
        res = op == JS_OP_LT ? c < 0 : op == JS_OP_GT ? c > 0 : op == JS_OP_LE ? c <= 0 : c >= 0;
        return js_bool(res);
    }
    {
        double a = js_to_number(js, l), b = js_to_number(js, r);
        if (a != a || b != b)
            return js_bool(0);
        res = op == JS_OP_LT ? a < b : op == JS_OP_GT ? a > b : op == JS_OP_LE ? a <= b : a >= b;
    }
    return js_bool(res);
}

/**
 * @brief 求值算术、位与移位运算符 / Evaluate arithmetic, bitwise and shift operators
 *
 * @details 两侧通过 ToNumber 转数值。取模对除数为 0 返回 NaN（避免 C 的未定义行为），幂用 `pow`。位运算与移位先经 `to_i32` / `to_u32` 规约，移位量对 32 取模（`& 31`）。
 *          Both sides are converted with ToNumber. Remainder yields NaN for a zero divisor to avoid C undefined behaviour and exponentiation uses `pow`. Bitwise and shift operations normalise through `to_i32` / `to_u32`, with the shift amount taken modulo 32 (`& 31`).
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] n 算式节点（`op` 决定具体运算）/ Expression node whose `op` selects the operation
 * @param[in]     l 左操作数 / Left operand
 * @param[in]     r 右操作数 / Right operand
 *
 * @return 运算结果数値；运算符未覆盖时返回 undefined / The numeric result, or undefined for an unsupported operator
 */
static JsValue eval_arithmetic(HpeJs *js, JsNode *n, JsValue l, JsValue r)
{
    switch (n->op) {
    case JS_OP_SUB:
        return js_num(js_to_number(js, l) - js_to_number(js, r));
    case JS_OP_MUL:
        return js_num(js_to_number(js, l) * js_to_number(js, r));
    case JS_OP_DIV:
        return js_num(js_to_number(js, l) / js_to_number(js, r));
    case JS_OP_MOD: {
        double b = js_to_number(js, r); /* x % 0 is NaN, unlike C's UB */
        return js_num(b == 0.0 ? NAN : fmod(js_to_number(js, l), b));
    }
    case JS_OP_EXP:
        return js_num(pow(js_to_number(js, l), js_to_number(js, r)));
    case JS_OP_BAND:
        return js_num((double)(to_i32(js, l) & to_i32(js, r)));
    case JS_OP_BOR:
        return js_num((double)(to_i32(js, l) | to_i32(js, r)));
    case JS_OP_BXOR:
        return js_num((double)(to_i32(js, l) ^ to_i32(js, r)));
    case JS_OP_SHL:
        return js_num((double)(to_i32(js, l) << (to_u32(js, r) & 31)));
    case JS_OP_SHR:
        return js_num((double)(to_i32(js, l) >> (to_u32(js, r) & 31)));
    case JS_OP_USHR:
        return js_num((double)(to_u32(js, l) >> (to_u32(js, r) & 31)));
    default:
        return js_undef();
    }
}

/**
 * @brief 实现 in 运算符 / Implement the `in` operator
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] n 运算表达式节点（提供行号）/ Expression node supplying the line number
 * @param[in]     l 左操作数（属性键）/ Left operand, the property key
 * @param[in]     r 右操作数（目标对象）/ Right operand, the target object
 *
 * @return 对象拥有该属性时返回 true / `true` when the object owns the property
 *
 * @note 右操作数非对象时报错；整数值键按无小数形式格式化后再查找 / A non-object right operand raises an error; an integral numeric key is formatted without a fractional part before lookup
 */
static JsValue eval_in_operator(HpeJs *js, JsNode *n, JsValue l, JsValue r)
{
    if (r.t != JS_OBJ || !r.obj)
        js_error(js, n->line, "cannot use 'in' operator on a non-object");
    if (l.t == JS_NUM && l.num == (double)(long long)l.num) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld", (long long)l.num);
        return js_bool(js_obj_has(r.obj, buf));
    }
    return js_bool(js_obj_has(r.obj, js_to_string(js, l)));
}

/**
 * @brief 实现二元加号 / Implement the binary plus operator
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     l 左操作数 / Left operand
 * @param[in]     r 右操作数 / Right operand
 *
 * @return 任一操作数为字符串时返回拼接结果，否则返回数值和 / The concatenation when either side is a string, otherwise the numeric sum
 *
 * @note 对象先经 ToPrimitive 转换，转换后若任一侧为字符串仍走拼接 / Objects are coerced with ToPrimitive and still concatenate when either coerced side is a string
 */
static JsValue bin_add(HpeJs *js, JsValue l, JsValue r)
{
    if (l.t == JS_STR || r.t == JS_STR)
        return bin_concat(js, l, r);
    if ((l.t == JS_OBJ && l.obj) || (r.t == JS_OBJ && r.obj)) {
        JsValue lp = js_primitive_of(js, l);
        JsValue rp = js_primitive_of(js, r);
        if (lp.t == JS_STR || rp.t == JS_STR)
            return bin_concat(js, lp, rp);
        return js_num(js_to_number(js, lp) + js_to_number(js, rp));
    }
    return js_num(js_to_number(js, l) + js_to_number(js, r));
}

/**
 * @brief 分派并求值二元表达式 / Dispatch and evaluate a binary expression
 *
 * @details 逻辑与/或/空值合并先交给 `eval_logical`（不求值右操作数）。`instanceof` 先求左值再求右值交给 `js_instanceof`。相等类运算符用 `js_values_equal`（严格与否由运算符决定）。`in` 走 `eval_in_operator`，比较走 `eval_relational`，算术/位/移位走 `eval_arithmetic`，`+` 走 `bin_add`。未识别的运算符报错。
 *          Logical and/or/nullish go to `eval_logical` first, without evaluating the right operand. `instanceof` evaluates the left then the right and calls `js_instanceof`. Equality operators use `js_values_equal`, strictness determined by the operator. `in` uses `eval_in_operator`, comparisons use `eval_relational`, arithmetic/bitwise/shift use `eval_arithmetic` and `+` uses `bin_add`. An unrecognised operator raises an error.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 二元表达式节点 / Binary expression node
 *
 * @return 表达式求值结果 / The evaluated expression value
 */
static JsValue eval_bin(HpeJs *js, JsEnv *env, JsNode *n)
{
    int op = n->op;
    JsValue l;
    JsValue r;

    if (op == JS_OP_AND || op == JS_OP_OR || op == JS_OP_NULLISH)
        return eval_logical(js, env, n, op);

    l = eval_expr(js, env, n->kids[0]);
    if (op == JS_OP_INSTANCEOF) {
        r = eval_expr(js, env, n->kids[1]);
        return js_instanceof(js, l, r, n->line);
    }
    r = eval_expr(js, env, n->kids[1]);
    switch (op) {
    case JS_OP_ADD:
        return bin_add(js, l, r);
    case JS_OP_LT:
    case JS_OP_GT:
    case JS_OP_LE:
    case JS_OP_GE:
        return eval_relational(js, n, l, r);
    case JS_OP_EQ:
        return js_bool(js_values_equal(js, l, r, 0));
    case JS_OP_NEQ:
        return js_bool(!js_values_equal(js, l, r, 0));
    case JS_OP_SEQ:
        return js_bool(js_values_equal(js, l, r, 1));
    case JS_OP_SNEQ:
        return js_bool(!js_values_equal(js, l, r, 1));
    case JS_OP_IN:
        return eval_in_operator(js, n, l, r);
    case JS_OP_SUB:
    case JS_OP_MUL:
    case JS_OP_DIV:
    case JS_OP_MOD:
    case JS_OP_EXP:
    case JS_OP_BAND:
    case JS_OP_BOR:
    case JS_OP_BXOR:
    case JS_OP_SHL:
    case JS_OP_SHR:
    case JS_OP_USHR:
        return eval_arithmetic(js, n, l, r);
    default:
        js_error(js, n->line, "unsupported binary operator");
        return js_undef();
    }
}

/**
 * @brief 读取一个可作为左值的目标的当前值 / Read the current value of an assignable target
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] target 目标节点（标识符或成员表达式）/ Target node (identifier or member expression)
 *
 * @return 目标的当前值；标识符未定义或其他目标类型时返回 undefined / The current value, or undefined for an undefined identifier or other target kinds
 *
 * @note 成员目标会先对基对象与键表达式求值再走 `member_get` / A member target evaluates its base and key expressions and goes through `member_get`
 */
static JsValue read_target(HpeJs *js, JsEnv *env, JsNode *target)
{
    if (target->kind == JS_N_IDENT) {
        JsValue *slot = js_env_lookup(env, target->str);
        return slot ? *slot : js_undef();
    }
    if (target->kind == JS_N_MEMBER) {
        JsValue base = eval_expr(js, env, target->kids[0]);
        char *prop = target->op == JS_OP_DOT
                         ? target->str
                         : js_to_string(js, eval_expr(js, env, target->kids[1]));
        return member_get(js, env, base, prop, target->line);
    }
    return js_undef();
}

/**
 * @brief 向一个可作为左值的目标写入值 / Write a value to an assignable target
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] target 目标节点 / Target node
 * @param[in]     value 写入的值 / Value to store
 *
 * @note 标识符写入已存在的槽位，槽位缺失时严格模式报 `is not defined`、否则定义到全局环境；成员目标经 `member_set` 写入；其余目标转交 `assign_pattern` / An identifier writes its existing slot, raising `is not defined` in strict mode or defining it globally otherwise; member targets write through `member_set`; other targets go to `assign_pattern`
 */
static void write_target(HpeJs *js, JsEnv *env, JsNode *target, JsValue value)
{
    if (target->kind == JS_N_IDENT) {
        JsValue *slot = js_env_lookup(env, target->str);
        if (slot)
            *slot = value;
        else if (env->strict)
            js_error(js, target->line, "%s is not defined", target->str);
        else
            js_env_define(js, js->global_env, target->str, value);
        return;
    }
    if (target->kind == JS_N_MEMBER) {
        JsValue base = eval_expr(js, env, target->kids[0]);
        char *prop = target->op == JS_OP_DOT
                         ? target->str
                         : js_to_string(js, eval_expr(js, env, target->kids[1]));
        member_set(js, env, base, prop, value, target->line);
        return;
    }
    assign_pattern(js, env, target, value);
}

/**
 * @brief 求值复合赋值运算符 / Evaluate a compound assignment operator
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] n 赋值表达式节点（`op` 决定运算符）/ Assignment node whose `op` selects the operator
 * @param[in]     old 左值当前值 / Current value of the target
 * @param[in]     rhs 右操作数 / Right-hand operand
 *
 * @return 运算结果值 / The result value
 *
 * @note `+=` 在任一侧为字符串时拼接，其余运算符与 `eval_bin` 语义一致；未识别的赋值运算符报错 / `+=` concatenates when either side is a string and the rest match the `eval_bin` semantics; an unrecognised assignment operator raises an error
 */
static JsValue eval_compound(HpeJs *js, JsNode *n, JsValue old, JsValue rhs)
{
    double a, b;

    switch (n->op) {
    case JS_OP_ADDA:
        if (old.t == JS_STR || rhs.t == JS_STR)
            return bin_concat(js, old, rhs);
        return js_num(js_to_number(js, old) + js_to_number(js, rhs));
    case JS_OP_SUBA:
        return js_num(js_to_number(js, old) - js_to_number(js, rhs));
    case JS_OP_MULA:
        return js_num(js_to_number(js, old) * js_to_number(js, rhs));
    case JS_OP_DIVA:
        return js_num(js_to_number(js, old) / js_to_number(js, rhs));
    case JS_OP_MODA:
        a = js_to_number(js, old);
        b = js_to_number(js, rhs);
        return js_num(b == 0.0 ? NAN : fmod(a, b));
    case JS_OP_EXPA:
        return js_num(pow(js_to_number(js, old), js_to_number(js, rhs)));
    case JS_OP_LSHA:
        return js_num((double)(to_i32(js, old) << (to_u32(js, rhs) & 31)));
    case JS_OP_RSHA:
        return js_num((double)(to_i32(js, old) >> (to_u32(js, rhs) & 31)));
    case JS_OP_URSHA:
        return js_num((double)(to_u32(js, old) >> (to_u32(js, rhs) & 31)));
    case JS_OP_BANDA:
        return js_num((double)(to_i32(js, old) & to_i32(js, rhs)));
    case JS_OP_BORA:
        return js_num((double)(to_i32(js, old) | to_i32(js, rhs)));
    case JS_OP_BXORA:
        return js_num((double)(to_i32(js, old) ^ to_i32(js, rhs)));
    default:
        js_error(js, n->line, "unsupported assignment operator");
        return js_undef();
    }
}

/**
 * @brief 求值逻辑赋值运算符 / Evaluate the logical assignment operators
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 赋值表达式节点（区分 `&&=` / `||=` / `??=`）/ Assignment node distinguishing `&&=`, `||=` and `??=`
 * @param[in]     old 左值当前值 / Current value of the target
 *
 * @return 未触发赋值时返回原值，否则返回写入的新值 / The untouched value when the assignment is skipped, otherwise the newly written value
 *
 * @note `&&=` 仅在原值为假值时跳过，`||=` 仅在真值时跳过，`??=` 仅在非空值时跳过 / `&&=` skips only for a falsy value, `||=` only for a truthy one and `??=` only for a non-nullish one
 */
static JsValue eval_shortassign(HpeJs *js, JsEnv *env, JsNode *n, JsValue old)
{
    int skip;

    if (n->op == JS_OP_ANDASSIGN)
        skip = !js_truthy(old);
    else if (n->op == JS_OP_ORASSIGN)
        skip = js_truthy(old);
    else
        skip = (old.t != JS_NULL && old.t != JS_UNDEF);
    if (skip)
        return old;
    {
        JsValue value = eval_expr(js, env, n->kids[1]);
        write_target(js, env, n->kids[0], value);
        return value;
    }
}

/**
 * @brief 分派并求值赋值表达式 / Dispatch and evaluate an assignment expression
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 赋值表达式节点 / Assignment expression node
 *
 * @return 赋给目标的值 / The value assigned to the target
 *
 * @note 简单赋值（`=`）对标识符/成员目标走 `write_target`，解构目标走 `assign_pattern`；逻辑赋值走 `eval_shortassign`；其余复合赋值先 `read_target` 再 `eval_compound`，最后 `write_target` / A simple `=` uses `write_target` for identifier or member targets and `assign_pattern` for destructuring; logical assignments use `eval_shortassign`; other compound assignments read with `read_target`, combine with `eval_compound` and finally `write_target`
 */
static JsValue eval_assign(HpeJs *js, JsEnv *env, JsNode *n)
{
    JsNode *target = n->kids[0];

    if (n->op == JS_OP_SET) {
        JsValue value = eval_expr(js, env, n->kids[1]);
        if (target->kind == JS_N_IDENT || target->kind == JS_N_MEMBER)
            write_target(js, env, target, value);
        else
            assign_pattern(js, env, target, value); /* destructuring target */
        return value;
    }
    if (n->op == JS_OP_ANDASSIGN || n->op == JS_OP_ORASSIGN || n->op == JS_OP_NULLASSIGN)
        return eval_shortassign(js, env, n, read_target(js, env, target));
    {
        JsValue value = eval_compound(js, n, read_target(js, env, target),
                                      eval_expr(js, env, n->kids[1]));
        write_target(js, env, target, value);
        return value;
    }
}

/**
 * @brief 由函数节点创建方法函数值 / Create a method function value from a function node
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 创建该方法时捕获的环境 / Environment captured when the method is created
 * @param[in,out] fn_node 函数声明节点 / Function declaration node
 *
 * @return 方法函数值 / The method function value
 *
 * @note 函数标志取自节点的 `op`，节点有名称时写入函数的 `name` / Function flags come from the node's `op` and the node's name, when present, is written into the function's `name`
 */

static JsValue make_method(HpeJs *js, JsEnv *env, JsNode *fn_node)
{
    JsObj *fn = js_func_new(js, fn_node, env);
    fn->fn_flags = fn_node->op;
    if (fn_node->str)
        js_obj_set(js, fn, "name", js_string(js, fn_node->str));
    return js_objval(fn);
}

/**
 * @brief 创建方法并记录其 [[HomeObject]] / Create a method and record its [[HomeObject]]
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 创建该方法时捕获的环境 / Environment captured when the method is created
 * @param[in,out] fn_node 函数声明节点 / Function declaration node
 * @param[in,out] home 方法所属对象（供 `super.x` 解析）/ Object the method belongs to, used to resolve `super.x`
 *
 * @return 带 `home` 的方法函数值 / The method function value with `home` set
 *
 * @note 在 `make_method` 基础上补充 home 指针 / Adds the home pointer on top of `make_method`
 *
 * @see make_method
 */
static JsValue make_method_home(HpeJs *js, JsEnv *env, JsNode *fn_node, JsObj *home)
{
    JsValue v = make_method(js, env, fn_node);
    if (v.t == JS_OBJ && v.obj)
        v.obj->home = home;
    return v;
}

/* exports (JS_METHOD_* in hpe_js_internal.h) */
enum {
    K_CTOR = 0,
    K_METHOD,
    K_GET,
    K_SET,
    K_STATIC_METHOD,
    K_STATIC_GET,
    K_STATIC_SET,
    K_FIELD,
    K_STATIC_FIELD,
    K_STATIC_BLOCK
};

/**
 * @brief 判定类成员的种类 / Determine a class member's kind
 *
 * @details 块节点为静态块（`K_STATIC_BLOCK`）。非函数节点按 `op` 是 `JS_MEMBER_STATIC_FIELD` 与否区分静态字段或实例字段。函数节点按 `num` 中保存的 `JS_METHOD_*` 返回构造器、get/set、静态方法/get/set，缺省为普通方法。
 *          A block node is a static block (`K_STATIC_BLOCK`). A non-function node is a static or instance field depending on whether `op` is `JS_MEMBER_STATIC_FIELD`. A function node returns constructor, getter/setter or static method/getter/setter according to the `JS_METHOD_*` stored in `num`, defaulting to a plain method.
 *
 * @param[in,out] m 类成员节点 / Class member node
 *
 * @return `K_*` 成员种类常量 / The `K_*` member-kind constant
 */
static int class_member_kind(JsNode *m)
{
    if (m->kind == JS_N_BLOCK)
        return K_STATIC_BLOCK;
    if (m->kind != JS_N_FUNC) {
        return (m->op == JS_MEMBER_STATIC_FIELD) ? K_STATIC_FIELD : K_FIELD;
    }
    switch ((int)m->num) {
    case JS_METHOD_CTOR:
        return K_CTOR;
    case JS_METHOD_GET:
        return K_GET;
    case JS_METHOD_SET:
        return K_SET;
    case JS_METHOD_STATIC:
        return K_STATIC_METHOD;
    case JS_METHOD_STATIC_GET:
        return K_STATIC_GET;
    case JS_METHOD_STATIC_SET:
        return K_STATIC_SET;
    default:
        return K_METHOD;
    }
}

/**
 * @brief 判断成员种类是否安装到构造函数上 / Test whether a member kind installs on the constructor
 *
 * @param[in]     kind `class_member_kind` 返回的成员种类 / Member kind returned by `class_member_kind`
 *
 * @return 静态方法/get/set 或静态字段返回 1，其余返回 0 / 1 for a static method, getter, setter or field, otherwise 0
 *
 * @note 静态成员挂到构造函数上，其余挂到原型上 / Static members land on the constructor, everything else on the prototype
 *
 * @see class_member_kind
 */
static int class_is_static_kind(int kind)
{
    return kind == K_STATIC_METHOD || kind == K_STATIC_GET || kind == K_STATIC_SET ||
           kind == K_STATIC_FIELD;
}

/**
 * @brief 按成员种类把一个类成员安装到原型或构造函数 / Install one class member onto the prototype or the constructor according to its kind
 *
 * @details 构造器成员自身不在此安装。方法与静态方法以 `make_method_home` 安装；get/set（含静态）通过 `js_obj_set_accessor` 安装；静态字段求值初始化器后写入构造函数；静态块在新建的、`this` 指向构造函数的环境中执行。实例字段在此跳过，留待每实例 `apply_class_fields` 处理。
 *          The constructor member itself is not installed here. Methods and static methods install via `make_method_home`; getters and setters (including statics) install via `js_obj_set_accessor`; a static field evaluates its initialiser and writes the constructor; a static block runs in a fresh environment whose `this` is the constructor. Instance fields are skipped here and handled per instance by `apply_class_fields`.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] m 类成员节点 / Class member node
 * @param[in,out] proto 类原型对象 / Class prototype object
 * @param[in,out] ctor 类构造函数对象 / Class constructor object
 */
static void class_install_member(HpeJs *js, JsEnv *env, JsNode *m, JsObj *proto, JsObj *ctor)
{
    int kind = class_member_kind(m);
    JsObj *home = class_is_static_kind(kind) ? ctor : proto;

    switch (kind) {
    case K_CTOR:
        break; /* the constructor is installed by class_eval itself */
    case K_METHOD:
    case K_STATIC_METHOD:
        js_obj_set(js, home, m->str, make_method_home(js, env, m, home));
        break;
    case K_GET:
    case K_STATIC_GET:
        js_obj_set_accessor(js, home, m->str, make_method_home(js, env, m, home), js_undef());
        break;
    case K_SET:
    case K_STATIC_SET:
        js_obj_set_accessor(js, home, m->str, js_undef(), make_method_home(js, env, m, home));
        break;
    case K_STATIC_FIELD: {
        JsValue v = (m->kid_count > 0 && m->kids[0]) ? eval_expr(js, env, m->kids[0]) : js_undef();
        js_obj_set(js, home, m->str, v);
        break;
    }
    case K_STATIC_BLOCK: {
        /* `static { ... }` runs with `this` bound to the constructor */
        JsEnv *senv = js_env_new(js, env);
        senv->has_this = 1;
        senv->thisv = js_objval(ctor);
        exec_stmt(js, senv, m);
        break;
    }
    default:
        break; /* instance fields are applied per instance, not here */
    }
}

/**
 * @brief 在类体成员中查找显式构造函数节点 / Find the explicit constructor node among the class body members
 *
 * @param[in,out] members 类体成员节点（可为 `NULL`）/ Class body member node, may be `NULL`
 *
 * @return 构造函数节点指针；没有显式构造函数时返回 `NULL` / Pointer to the constructor node, or `NULL` when the class declares none
 *
 * @note 只识别种类为 `K_CTOR` 的函数节点 / Only function nodes whose kind is `K_CTOR` are recognised
 */
static JsNode *class_find_ctor_node(JsNode *members)
{
    int i;

    if (!members)
        return NULL;
    for (i = 0; i < members->kid_count; i++) {
        JsNode *m = members->kids[i];

        if (m && m->kind == JS_N_FUNC && class_member_kind(m) == K_CTOR)
            return m;
    }
    return NULL;
}

/**
 * @brief 为没有显式构造函数的类合成一个构造函数 / Synthesise a constructor for a class that declares none
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 类声明节点 / Class declaration node
 * @param[in,out] proto 类原型对象 / Class prototype object
 *
 * @return 合成的构造函数对象 / The synthesised constructor object
 *
 * @note 合成的函数带 `FN_CLASS | FN_CTOR`、空形参与空函数体，名称取类名（缺省 `constructor`），`home` 指向原型，`class_def` 指向类节点，使 `new` 与 `super()` 仍有可运行对象 / The synthetic function carries `FN_CLASS | FN_CTOR`, empty parameters and body, the class name (defaulting to `constructor`), `home` pointing at the prototype and `class_def` at the class node, so that `new` and `super()` still have something to run
 */
static JsObj *class_synth_ctor(HpeJs *js, JsEnv *env, JsNode *n, JsObj *proto)
{
    JsNode *fn_node = js_node_new(js, JS_N_FUNC, n->line);
    JsNode *params = js_node_new(js, JS_N_BLOCK, n->line);
    JsNode *body = js_node_new(js, JS_N_BLOCK, n->line);
    JsObj *ctor;

    fn_node->op = FN_CLASS | FN_CTOR;
    fn_node->str = n->str ? n->str : "constructor";
    js_node_add(js, fn_node, params);
    js_node_add(js, fn_node, body);
    ctor = js_func_new(js, fn_node, env);
    ctor->class_def = n;
    ctor->home = proto;
    return ctor;
}

/**
 * @brief 安装类体中的全部成员 / Install every member of a class body
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] members 类体成员节点（可为 `NULL`）/ Class body member node, may be `NULL`
 * @param[in,out] proto 类原型对象 / Class prototype object
 * @param[in,out] ctor 类构造函数对象 / Class constructor object
 *
 * @note 跳过构造器成员（由 `class_eval` 自行安装）与空槽位，其余交给 `class_install_member` / The constructor member (installed by `class_eval` itself) and empty slots are skipped, the rest go to `class_install_member`
 *
 * @see class_install_member
 */
static void class_install_members(HpeJs *js, JsEnv *env, JsNode *members, JsObj *proto,
                                  JsObj *ctor)
{
    int i;

    if (!members)
        return;
    for (i = 0; i < members->kid_count; i++) {
        JsNode *m = members->kids[i];

        if (!m || class_member_kind(m) == K_CTOR)
            continue;
        class_install_member(js, env, m, proto, ctor);
    }
}

/**
 * @brief 求值类声明并构造原型与构造函数 / Evaluate a class declaration, building its prototype and constructor
 *
 * @details 有父类表达式时求值并判断派生关系（父类 `prototype` 为对象即为派生）。原型以类名命名，派生时链接到父类原型，否则链接 `Object.prototype`。显式构造函数节点存在时用它建函数，否则合成一个。随后设置构造函数的 `name`、`prototype` 与原型上的 `constructor`，派生类把构造函数的 `proto` 接到父类上以实现静态继承，最后安装全部成员；类名在词法环境中声明。
 *          A superclass expression, when present, is evaluated and the class counts as derived when the superclass `prototype` is an object. The prototype is named after the class and links either to the superclass prototype or to `Object.prototype`. An explicit constructor node is used when present, otherwise one is synthesised. The constructor's `name`, `prototype` and the prototype's `constructor` are then set, a derived class links its constructor's `proto` to the superclass for static inheritance, all members are installed, and the class name is declared in the lexical environment.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 类声明节点 / Class declaration node
 *
 * @return 类构造函数值 / The class constructor value
 */
static JsValue class_eval(HpeJs *js, JsEnv *env, JsNode *n)
{
    JsNode *members = n->kid_count > 1 ? n->kids[1] : NULL;
    JsValue super_v = n->kids[0] ? eval_expr(js, env, n->kids[0]) : js_undef();
    JsObj *proto = js_object_raw(js);
    JsObj *ctor = NULL;
    JsNode *ctor_node;
    int derived = (super_v.t == JS_OBJ && super_v.obj &&
                   member_get(js, NULL, super_v, "prototype", n->line).t == JS_OBJ);

    proto->class_name = n->str ? n->str : "Object";
    proto->proto = derived ? member_get(js, NULL, super_v, "prototype", n->line).obj
                           : js->object_proto;

    ctor_node = class_find_ctor_node(members);
    if (ctor_node) {
        ctor = js_func_new(js, ctor_node, env);
        ctor->fn_flags = ctor_node->op;
        ctor->class_def = n;
        ctor->home = proto;
    } else {
        ctor = class_synth_ctor(js, env, n, proto);
    }
    ctor->name = n->str ? n->str : "constructor";
    ctor->class_def = n;
    ctor->home = proto;
    js_obj_set(js, ctor, "name", js_string(js, ctor->name));
    js_obj_set(js, ctor, "prototype", js_objval(proto));
    js_obj_set(js, proto, "constructor", js_objval(ctor));
    if (derived)
        ctor->proto = super_v.obj; /* static inheritance */

    class_install_members(js, env, members, proto, ctor);
    if (n->str)
        js_env_declare(js, env, n->str, js_objval(ctor));
    return js_objval(ctor);
}

/* ---------------- expressions ---------------- */

/**
 * @brief 求值对象字面量 / Evaluate an object literal
 *
 * @details 展开属性（`JS_N_SPREAD`）把源对象的全部自有键拷贝过来；`__proto__` 属性设置 `proto`。属性键取自节点的 `str`，取值/方法/访问器由 `op` 区分：get/set 经 `js_obj_set_accessor`，方法经 `make_method`，普通属性求值（带默认值形式取其表达式分支）。
 *          A spread property (`JS_N_SPREAD`) copies every own key from the source object and a `__proto__` property sets `proto`. The key comes from the node's `str` and `op` selects a value, method or accessor: get/set via `js_obj_set_accessor`, methods via `make_method`, and a plain property is evaluated (a defaulted form takes its expression branch).
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 对象字面量节点 / Object literal node
 *
 * @return 新建的对象值 / The newly created object value
 */
static JsValue object_literal(HpeJs *js, JsEnv *env, JsNode *n)
{
    JsObj *o = js_object_new(js);
    int i;
    for (i = 0; i < n->kid_count; i++) {
        JsNode *pair = n->kids[i];
        char *key;
        if (!pair)
            continue;
        if (pair->kind == JS_N_SPREAD) {
            JsValue src = eval_expr(js, env, pair->kids[0]);
            if (src.t == JS_OBJ && src.obj) {
                char **keys;
                int nk, k;
                js_obj_own_keys(src.obj, &keys, &nk);
                for (k = 0; k < nk; k++)
                    js_obj_set(js, o, keys[k], member_get(js, env, src, keys[k], pair->line));
                free(keys);
            }
            continue;
        }
        if (pair->op == JS_PROP_PROTO) {
            JsValue p = eval_expr(js, env, pair->kids[0]);
            if (p.t == JS_OBJ)
                o->proto = p.obj;
            continue;
        }
        /* a computed key is stored as a synthetic JS_N_BLOCK child marker:
         * the parser puts the key expression as kids[1] for computed names and
         * leaves kids[0] as the value/method. */
        key = pair->str ? pair->str : "";
        if (pair->op == JS_PROP_GET)
            js_obj_set_accessor(js, o, key, make_method(js, env, pair->kids[0]), js_undef());
        else if (pair->op == JS_PROP_SET)
            js_obj_set_accessor(js, o, key, js_undef(), make_method(js, env, pair->kids[0]));
        else if (pair->op == JS_PROP_METHOD)
            js_obj_set(js, o, key, make_method(js, env, pair->kids[0]));
        else {
            JsNode *val = pair->kids[0];
            if (val && val->kind == JS_N_PAT_DEFAULT)
                val = val->kids[1];
            js_obj_set(js, o, key, eval_expr(js, env, val));
        }
    }
    return js_objval(o);
}

/**
 * @brief 求值模板字面量 / Evaluate a template literal
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 模板字面量节点（`kids[0]` 为字面块表，其后为插值表达式）/ Template literal node whose `kids[0]` holds the literal chunks and whose following kids are the interpolations
 *
 * @return 拼接后的字符串值 / The concatenated string value
 *
 * @note 先按字节预估总长并一次分配，再逐块拷贝字面量与其后的插值（插值经 `js_to_string` 转换）/ The total byte length is computed up front and allocated once, then each literal chunk is copied followed by its interpolation stringified with `js_to_string`
 */
static JsValue template_eval(HpeJs *js, JsEnv *env, JsNode *n)
{
    JsNode *chunks = n->kids[0];
    int nchunks = chunks ? chunks->kid_count : 0;
    int nexpr = n->kid_count - 1;
    size_t total = 1;
    char *out;
    size_t o = 0;
    int i;
    for (i = 0; i < nchunks; i++)
        total += strlen(chunks->kids[i]->str ? chunks->kids[i]->str : "");
    for (i = 0; i < nexpr; i++)
        total += strlen(js_to_string(js, eval_expr(js, env, n->kids[i + 1])));
    out = (char *)hpe_js_alloc(js, total + 8);
    for (i = 0; i < nchunks; i++) {
        const char *s = chunks->kids[i]->str ? chunks->kids[i]->str : "";
        size_t sl = strlen(s);
        memcpy(out + o, s, sl);
        o += sl;
        if (i < nexpr) {
            char *s2 = js_to_string(js, eval_expr(js, env, n->kids[i + 1]));
            size_t sl2 = strlen(s2);
            memcpy(out + o, s2, sl2);
            o += sl2;
        }
    }
    out[o] = '\0';
    return js_string_len(js, out, o);
}

/**
 * @brief 求值成员访问表达式 / Evaluate a member access expression
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 成员访问节点 / Member access node
 *
 * @return 成员值；可选链在基值为 `null`/`undefined` 时返回 undefined / The member value, or undefined when an optional chain meets a `null` / `undefined` base
 *
 * @note 点式取 `str`，下标式对键表达式求值后转字符串；`num != 0` 标记可选链短路 / A dot form uses `str`, an index form evaluates and stringifies the key, and `num != 0` marks an optional chain that may short-circuit
 *
 * @see member_get
 */
static JsValue eval_member(HpeJs *js, JsEnv *env, JsNode *n)
{
    JsValue base = eval_expr(js, env, n->kids[0]);
    char *prop = n->op == JS_OP_DOT ? n->str : js_to_string(js, eval_expr(js, env, n->kids[1]));
    if (n->num != 0 && (base.t == JS_NULL || base.t == JS_UNDEF))
        return js_undef(); /* optional chain */
    return member_get(js, env, base, prop, n->line);
}

/**
 * @brief 求值 `super.x` 成员访问 / Evaluate a `super.x` member access
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n super 成员访问节点 / `super` member access node
 *
 * @return 父原型上的成员值；无属性名时返回父原型对象；缺失时返回 undefined / The member value from the superclass prototype, the prototype object itself when no name is given, or undefined when absent
 *
 * @note 沿环境链寻找带 [[HomeObject]] 的作用域，取其 `home->proto` 与 `this`；找不到 home 时报 `'super' keyword unexpected here`；取值器以该 `this` 调用 / Walks the environment chain for the scope carrying a [[HomeObject]], using its `home->proto` and `this`; a missing home raises `'super' keyword unexpected here` and a getter is called with that `this`
 */
static JsValue eval_super_member(HpeJs *js, JsEnv *env, JsNode *n)
{
    JsObj *home = NULL;
    JsValue thisv = js_undef();
    JsObj *proto;
    JsEnv *e = env;
    while (e && !e->has_home)
        e = e->parent;
    if (e) {
        home = e->home;
        thisv = e->thisv;
    }
    if (!home)
        js_error(js, n->line, "'super' keyword unexpected here");
    proto = home->proto;
    if (!n->str)
        return proto ? js_objval(proto) : js_undef();
    if (!proto)
        return js_undef();
    {
        JsProp *p = js_obj_find_prop(proto, n->str);
        if (!p)
            return js_undef();
        if (p->getter.t != JS_UNDEF)
            return call_value(js, NULL, p->getter, thisv, NULL, 0, n->line);
        return p->val;
    }
}

/* calls super(...) inside a derived constructor: runs the parent constructor
 * against the instance that is already bound to `this` */
/* calls super(...) inside a derived constructor: runs the parent constructor
 * against the instance that is already bound to `this` */
/* growable argument vector shared by call / new / super(...) */
typedef struct {
    JsValue *items;
    int count;
    int cap;
} ArgList;

/**
 * @brief 初始化一个可变实参向量 / Initialise a growable argument vector
 *
 * @param[out]    al 待初始化的实参向量 / Argument vector to initialise
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     cap 初始容量（小于 1 时按 1 处理）/ Initial capacity, clamped up to 1
 *
 * @note count 归零，缓冲区由运行时分配器分配 / The count is reset to zero and the buffer comes from the runtime allocator
 */
static void args_init(ArgList *al, HpeJs *js, int cap)
{
    if (cap < 1)
        cap = 1;
    al->cap = cap;
    al->count = 0;
    al->items = (JsValue *)hpe_js_alloc(js, (size_t)cap * sizeof(JsValue));
}

/**
 * @brief 向实参向量追加一个值 / Append one value to an argument vector
 *
 * @param[in,out] al 目标实参向量 / Target argument vector
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     v 要追加的值 / Value to append
 *
 * @note 容量不足时按 `+8` 增长并拷贝已有元素 / When full the capacity grows by 8 and existing elements are copied
 */
static void args_push(ArgList *al, HpeJs *js, JsValue v)
{
    if (al->count >= al->cap) {
        int ncap = al->cap + 8;
        JsValue *bigger = (JsValue *)hpe_js_alloc(js, (size_t)ncap * sizeof(JsValue));
        memcpy(bigger, al->items, (size_t)al->count * sizeof(JsValue));
        al->items = bigger;
        al->cap = ncap;
    }
    al->items[al->count++] = v;
}

/**
 * @brief 求值调用/构造节点的实参并展开 spread / Evaluate a call or construct node's arguments, flattening spreads
 *
 * @param[out]    al 接收实参的向量 / Vector receiving the arguments
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 调用或构造节点 / Call or construct node
 * @param[in]     first 起始子节点下标（跳过被调函数节点）/ Index of the first child to evaluate, skipping the callee node
 *
 * @note spread 实参通过迭代协议（`js_iter_len` / `js_iter_at`）逐个展开后压入，使调用与构造共用同一条路径 / A spread argument is expanded through the iteration protocol (`js_iter_len` / `js_iter_at`), letting calls and constructs share one code path
 */
static void args_build(ArgList *al, HpeJs *js, JsEnv *env, JsNode *n, int first)
{
    int i;
    for (i = first; i < n->kid_count; i++) {
        JsNode *a = n->kids[i];
        if (a && a->kind == JS_N_SPREAD) {
            JsValue v = eval_expr(js, env, a->kids[0]);
            int k, len = js_iter_len(js, v);
            for (k = 0; k < len; k++)
                args_push(al, js, js_iter_at(js, v, k));
        } else {
            args_push(al, js, eval_expr(js, env, a));
        }
    }
}

/**
 * @brief 解析被调函数及其 this 绑定 / Resolve a call's callee together with its `this` binding
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 调用节点（`op == 1` 表示可选调用）/ Call node, where `op == 1` marks an optional call
 * @param[in,out] callee_node 被调函数节点 / Callee node
 * @param[out]    callee 解析出的被调函数值 / Resolved callee value
 * @param[out]    thisv 对应的 this 值 / Matching `this` value
 *
 * @return 0 表示可选调用在 `null`/`undefined` 上短路，1 表示可以继续调用 / 0 when an optional call short-circuits on `null` / `undefined`, 1 when the call may proceed
 *
 * @note 方法调用把 `this` 绑到基对象；`super.m()` 把 `this` 绑到类的自身实例（经 `js_env_this_env`）/ A method call binds `this` to the base object；`super.m()` binds `this` to the class's own instance via `js_env_this_env`
 */
static int eval_callee(HpeJs *js, JsEnv *env, JsNode *n, JsNode *callee_node, JsValue *callee,
                       JsValue *thisv)
{
    if (callee_node->kind == JS_N_MEMBER) {
        JsValue base = eval_expr(js, env, callee_node->kids[0]);
        char *prop = callee_node->op == JS_OP_DOT
                         ? callee_node->str
                         : js_to_string(js, eval_expr(js, env, callee_node->kids[1]));
        if (n->op == 1 && (base.t == JS_NULL || base.t == JS_UNDEF))
            return 0;
        *callee = member_get(js, env, base, prop, callee_node->line);
        *thisv = base;
    } else if (callee_node->kind == JS_N_SUPER_MEMBER) {
        JsEnv *te = js_env_this_env(env);
        *callee = eval_super_member(js, env, callee_node);
        *thisv = te ? te->thisv : js_undef();
    } else {
        *callee = eval_expr(js, env, callee_node);
    }
    if (n->op == 1 && (callee->t == JS_NULL || callee->t == JS_UNDEF))
        return 0;
    return 1;
}

/**
 * @brief 在派生构造函数中执行 `super(...)` / Perform a `super(...)` call inside a derived constructor
 *
 * @details 沿环境链查找带 [[HomeObject]] 的作用域以取得 `home` 与 `this`。取 `home->proto` 上的 `constructor` 作为父构造函数，若不可调用则报错。用 `args_build` 求值实参，再以 `derived = 1` 运行父构造函数体（因此父类字段在 `super()` 之后才安装到该实例上）。
 *          The environment chain is walked for the scope carrying a [[HomeObject]] to obtain `home` and `this`. The `constructor` on `home->proto` is taken as the parent constructor and an error is raised when it is not callable. Arguments are evaluated with `args_build` and the parent body runs with `derived = 1`, so the parent's fields are installed on the instance only after `super()`.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n super 调用节点 / `super` call node
 *
 * @return 父构造函数返回的对象；未返回对象时返回当前 `this` 实例 / The object returned by the parent constructor, or the current `this` instance when it returns none
 */
static JsValue super_call(HpeJs *js, JsEnv *env, JsNode *n)
{
    JsEnv *e = env;
    JsObj *home = NULL;
    JsValue this_v = js_undef();
    JsValue parent_v;
    JsValue out;
    ArgList al;

    while (e && !e->has_home)
        e = e->parent;
    if (e) {
        home = e->home;
        this_v = e->thisv;
    }
    if (!home || !home->proto)
        js_error(js, n->line, "'super' constructor call outside of a derived class");

    parent_v = js_obj_get(js, home->proto, "constructor");
    if (!js_is_callable(parent_v))
        js_error(js, n->line, "super class has no constructor");

    args_init(&al, js, n->kid_count > 0 ? n->kid_count : 1);
    args_build(&al, js, env, n, 0);
    out = run_ctor_body(js, parent_v.obj, this_v, al.items, al.count, 1);
    if (out.t == JS_OBJ)
        return out;
    return this_v;
}

/**
 * @brief 求值一次函数调用表达式 / Evaluate a call expression
 *
 * @details 若被调节点是 `super(...)` 则转交 `super_call`；否则先由 `eval_callee` 解析出被调值与 `this`，再以第 1 个子节点起的实参调用 `call_value`。
 *          When the callee node is a `super(...)` call it is delegated to `super_call`; otherwise `eval_callee` resolves the callee and `this`, after which `call_value` is invoked with the arguments starting at child 1.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 节点 / Node
 *
 * @return 调用的结果值 / The result value of the call
 *
 * @note `eval_callee` 解析失败时返回 `undefined` / Returns `undefined` when `eval_callee` cannot resolve the callee
 *
 * @see eval_callee call_value eval_new
 */
static JsValue eval_call(HpeJs *js, JsEnv *env, JsNode *n)
{
    JsNode *callee_node = n->kids[0];
    JsValue callee = js_undef();
    JsValue thisv = js_undef();
    ArgList al;

    if (callee_node->kind == JS_N_SUPER_CALL)
        return super_call(js, env, callee_node);
    if (!eval_callee(js, env, n, callee_node, &callee, &thisv))
        return js_undef();
    args_init(&al, js, n->kid_count > 1 ? n->kid_count : 1);
    args_build(&al, js, env, n, 1);
    return call_value(js, env, callee, thisv, al.items, al.count, n->line);
}

/**
 * @brief 求值 `new` 构造表达式 / Evaluate a `new` construction expression
 *
 * @details 被调节点是 `JS_N_CALL` 时按 `new X(args)` 解析被调值与实参；否则被调值直接取自成员表达式或普通表达式，且不带实参调用 `js_construct`。
 *          When the callee node is a `JS_N_CALL` it is treated as `new X(args)`; otherwise the callee comes from a member or plain expression and `js_construct` is called with no arguments.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 节点 / Node
 *
 * @return 构造出的对象值 / The constructed object value
 *
 * @see js_construct eval_call
 */
static JsValue eval_new(HpeJs *js, JsEnv *env, JsNode *n)
{
    JsNode *callee_node = n->kids[0];
    JsValue callee;
    ArgList al;

    if (callee_node->kind == JS_N_CALL) {
        callee = eval_expr(js, env, callee_node->kids[0]);
        args_init(&al, js, callee_node->kid_count > 1 ? callee_node->kid_count : 1);
        args_build(&al, js, env, callee_node, 1);
        return js_construct(js, callee, al.items, al.count, n->line);
    }
    if (callee_node->kind == JS_N_MEMBER)
        callee = eval_member(js, env, callee_node);
    else
        callee = eval_expr(js, env, callee_node);
    return js_construct(js, callee, NULL, 0, n->line);
}

/**
 * @brief 把运行时值映射为 `typeof` 结果名 / Map a runtime value to its `typeof` result name
 *
 * @param[in]     v 值 / Value
 *
 * @return 类型名字符串 / The type-name string
 * @retval "undefined" `JS_UNDEF` 或未知类型 / `JS_UNDEF` or an unknown type
 * @retval "object" `JS_NULL` 或非可调用对象 / `JS_NULL` or a non-callable object
 * @retval "boolean" 布尔值 / A boolean value
 * @retval "number" 数值 / A number
 * @retval "string" 字符串 / A string
 * @retval "function" 可调用对象（函数）/ A callable object (function)
 *
 * @note `null` 按 JS 语义返回 `"object"` / `null` yields `"object"` per JS semantics
 */
static const char *typeof_name(JsValue v)
{
    switch (v.t) {
    case JS_UNDEF:
        return "undefined";
    case JS_NULL:
        return "object";
    case JS_BOOL:
        return "boolean";
    case JS_NUM:
        return "number";
    case JS_STR:
        return "string";
    case JS_OBJ:
        return js_is_callable(v) ? "function" : "object";
    default:
        return "undefined";
    }
}

/**
 * @brief 求值 `delete` 操作符 / Evaluate the `delete` operator
 *
 * @details 成员形式 `obj.prop` / `obj[key]` 会先求基对象再删除属性；标识符形式 `delete someVar` 把当前绑定的槽位置为 `undefined`。
 *          The member form `obj.prop` / `obj[key]` evaluates the base and removes the property; the identifier form `delete someVar` overwrites the found binding slot with `undefined`.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] target 被删除目标节点（成员或标识符）/ The delete target node (member or identifier)
 *
 * @return 始终返回 `true` / Always returns `true`
 *
 * @note 删除不可配置属性的失败被报告为成功，符合解释器假定的非严格模式行为 / A failed delete of a non-configurable property is reported as success, matching the sloppy-mode behaviour assumed by the interpreter
 */
static JsValue eval_delete(HpeJs *js, JsEnv *env, JsNode *target)
{
    if (target->kind == JS_N_MEMBER) {
        JsValue base = eval_expr(js, env, target->kids[0]);
        char *prop = target->op == JS_OP_DOT
                         ? target->str
                         : js_to_string(js, eval_expr(js, env, target->kids[1]));
        if (base.t == JS_OBJ && base.obj)
            js_obj_delete(js, base.obj, prop);
        return js_bool(1);
    }
    if (target->kind == JS_N_IDENT) {
        JsValue *slot = js_env_lookup(env, target->str);
        if (slot)
            *slot = js_undef();
    }
    return js_bool(1);
}

/**
 * @brief 求值 `!`、`void`、`-`、`+`、`~` 一元运算 / Evaluate the `!`, `void`, `-`, `+` and `~` unary operators
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] n 承载运算符的一元节点 / The unary node carrying the operator
 * @param[in]     v 运算数 / The operand
 *
 * @return 一元运算结果 / The unary operation result
 *
 * @note `void` 丢弃操作数并返回 `undefined`；`~` 先转 32 位有符号整数再取反 / `void` discards the operand and returns `undefined`; `~` converts to a 32-bit signed integer before complementing
 */
static JsValue eval_unary_arithmetic(HpeJs *js, JsNode *n, JsValue v)
{
    if (n->op == JS_OP_NOT)
        return js_bool(!js_truthy(v));
    if (n->op == JS_OP_VOID)
        return js_undef();
    if (n->op == JS_OP_NEG)
        return js_num(-js_to_number(js, v));
    if (n->op == JS_OP_POS)
        return js_num(js_to_number(js, v));
    if (n->op == JS_OP_BNOT)
        return js_num((double)(~to_i32(js, v)));
    return js_undef();
}

/**
 * @brief 求值数组字面量（含展开元素）/ Evaluate an array literal including spread elements
 *
 * @details 逐个子节点求值推入新数组；空槽位（`NULL` 子节点）推入 `undefined`，`JS_N_SPREAD` 子节点对其操作数迭代后逐项展开推入。
 *          Each child is evaluated and pushed into a new array; a hole (`NULL` child) pushes `undefined`, while a `JS_N_SPREAD` child iterates its operand and spreads the items.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 节点 / Node
 *
 * @return 新建的数组对象 / The newly created array object
 */
static JsValue eval_array_literal(HpeJs *js, JsEnv *env, JsNode *n)
{
    JsObj *arr = js_array_new(js);
    int i;

    for (i = 0; i < n->kid_count; i++) {
        JsNode *item = n->kids[i];
        if (!item) {
            array_push_value(js, arr, js_undef());
            continue;
        }
        if (item->kind == JS_N_SPREAD) {
            JsValue v = eval_expr(js, env, item->kids[0]);
            int k, len = js_iter_len(js, v);
            for (k = 0; k < len; k++)
                array_push_value(js, arr, js_iter_at(js, v, k));
        } else {
            array_push_value(js, arr, eval_expr(js, env, item));
        }
    }
    return js_objval(arr);
}

/**
 * @brief 求值 `await` 表达式 / Evaluate an `await` expression
 *
 * @details 先求子表达式；若结果是 promise 则先泵空微任务作业队列，失败态（state 为 2）抛出其值，否则返回其兑现值。
 *          The operand is evaluated first; when the result is a promise the microtask queue is drained, its value is thrown when rejected (state 2), and its fulfilment value is returned otherwise.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 节点 / Node
 *
 * @return 兑现值或原值 / The fulfilment value or the operand itself
 *
 * @note 非 promise 值原样返回 / A non-promise value is returned unchanged
 *
 * @see eval_yield
 */
static JsValue eval_await(HpeJs *js, JsEnv *env, JsNode *n)
{
    JsValue v = eval_expr(js, env, n->kids[0]);

    if (v.t == JS_OBJ && v.obj && v.obj->kind == JS_OBJ_PROMISE) {
        hpe_js_drain_jobs(js);
        if (v.obj->promise_state == 2)
            js_throw_value(js, v.obj->promise_value);
        return v.obj->promise_value;
    }
    return v;
}

/**
 * @brief 求值 `yield` / `yield*` / Evaluate `yield` / `yield*`
 *
 * @details 先求操作数（无操作数则取 `undefined`）；写入 `js->yield_sink`：委托形式迭代操作数逐项追加，非委托形式追加单个值。
 *          The operand is evaluated (`undefined` when absent); the value is written to `js->yield_sink`: the delegate form iterates the operand and appends each item, the plain form appends the single value.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 节点 / Node
 * @param[in]     delegate 非 0 表示 `yield*` 委托迭代 / Non-zero for `yield*` delegation
 *
 * @return 恒为 `undefined` / Always `undefined`
 *
 * @see eval_yield_node eval_await
 */
static JsValue eval_yield(HpeJs *js, JsEnv *env, JsNode *n, int delegate)
{
    JsValue v = n->kids[0] ? eval_expr(js, env, n->kids[0]) : js_undef();

    if (js->yield_sink) {
        if (delegate) {
            int k, len = js_iter_len(js, v);
            for (k = 0; k < len; k++)
                array_push_value(js, js->yield_sink, js_iter_at(js, v, k));
        } else {
            array_push_value(js, js->yield_sink, v);
        }
    }
    return js_undef();
}

/**
 * @brief 解析标识符引用 / Resolve an identifier reference
 *
 * @details 先在词法环境中查找最近绑定；未命中再查全局对象属性（使 `Math`、`undefined` 等可解析）；都没有则抛出 ReferenceError。
 *          The innermost lexical binding is looked up first; on a miss the global object's properties are consulted (so `Math`, `undefined` etc. resolve); when both fail a ReferenceError is raised.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 节点 / Node
 *
 * @return 绑定的值 / The bound value
 *
 * @see eval_this
 */
static JsValue eval_ident(HpeJs *js, JsEnv *env, JsNode *n)
{
    JsValue *slot = js_env_lookup(env, n->str);

    if (slot)
        return *slot;
    if (js_obj_has(js->global_obj, n->str))
        return js_obj_get(js, js->global_obj, n->str);
    js_error(js, n->line, "%s is not defined", n->str);
    return js_undef();
}

/**
 * @brief 解析 `this` / Resolve `this`
 *
 * @details 沿环境链向上找到最近绑定过 `this` 的环境；到顶仍未找到时回退为全局对象。
 *          The environment chain is walked up to the nearest environment that bound `this`; falling off the top falls back to the global object.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 *
 * @return 当前 `this` 值 / The current `this` value
 *
 * @see eval_this_node
 */
static JsValue eval_this(HpeJs *js, JsEnv *env)
{
    JsEnv *e = env;

    while (e && !e->has_this)
        e = e->parent;
    if (!e)
        return js_objval(js->global_obj);
    return e->thisv;
}

/**
 * @brief 求值逗号表达式 / Evaluate the comma operator
 *
 * @details 从左到右依次求值每个子节点，返回最后一个的值（此前的副作用生效）。
 *          Each child is evaluated left to right and the last value is returned (earlier side effects take effect).
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 节点 / Node
 *
 * @return 最后一个子表达式的值 / The value of the last sub-expression
 */
static JsValue eval_seq(HpeJs *js, JsEnv *env, JsNode *n)
{
    JsValue v = js_undef();
    int i;

    for (i = 0; i < n->kid_count; i++)
        v = eval_expr(js, env, n->kids[i]);
    return v;
}

/**
 * @brief 求值 `++` / `--` 自增自减 / Evaluate `++` / `--`
 *
 * @details 读取目标旧值、按运算符加减 1、写回目标；前缀返回新值，后缀返回旧值。
 *          The old target value is read, +/- 1 applied, and written back; prefix forms return the new value and postfix forms the old one.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 节点 / Node
 *
 * @return 前缀为新值、后缀为旧值 / The new value for prefix forms, the old value for postfix forms
 *
 * @see read_target write_target
 */
static JsValue eval_update(HpeJs *js, JsEnv *env, JsNode *n)
{
    JsNode *target = n->kids[0];
    JsValue old = read_target(js, env, target);
    double onum = js_to_number(js, old);
    int is_inc = (n->op == JS_OP_PREINC || n->op == JS_OP_POSTINC);
    int is_pre = (n->op == JS_OP_PREINC || n->op == JS_OP_PREDEC);
    double nnum = is_inc ? onum + 1.0 : onum - 1.0;

    write_target(js, env, target, js_num(nnum));
    return js_num(is_pre ? nnum : onum);
}

/**
 * @brief 一元运算分派（`delete` / `typeof` 及其余）/ Dispatch the unary operators (`delete` / `typeof` and the rest)
 *
 * @details `delete` 转 `eval_delete`；`typeof` 求值操作数后经 `typeof_name` 转字符串；其余运算符求值操作数后交 `eval_unary_arithmetic`。
 *          `delete` goes to `eval_delete`; `typeof` evaluates the operand and stringifies it through `typeof_name`; the remaining operators evaluate the operand and defer to `eval_unary_arithmetic`.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 节点 / Node
 *
 * @return 一元表达式结果 / The unary expression result
 *
 * @see eval_delete typeof_name eval_unary_arithmetic
 */
static JsValue eval_unary(HpeJs *js, JsEnv *env, JsNode *n)
{
    if (n->op == JS_OP_DELETE)
        return eval_delete(js, env, n->kids[0]);
    if (n->op == JS_OP_TYPEOF)
        return js_string(js, typeof_name(eval_expr(js, env, n->kids[0])));
    return eval_unary_arithmetic(js, n, eval_expr(js, env, n->kids[0]));
}

/**
 * @brief 求值字面量节点 / Evaluate a literal node
 *
 * @details 按节点种类构造：字符串取 `s`（`NULL` 视为空串）、数值取 `n->num`、`true`/`false` 取布尔，其余（`null`）返回 `null`。
 *          Constructs by node kind: a string takes `s` (`NULL` treated as empty), a number takes `n->num`, `true`/`false` yield booleans, and anything else (`null`) yields `null`.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] n 字面量节点 / The literal node
 * @param[in]     s 字符串字面量内容（可为 `NULL`）/ The string literal content (may be `NULL`)
 *
 * @return 构造出的字面量值 / The constructed literal value
 */
static JsValue eval_literal(HpeJs *js, JsNode *n, const char *s)
{
    if (n->kind == JS_N_STR)
        return js_string(js, s ? s : "");
    if (n->kind == JS_N_NUM)
        return js_num(n->num);
    if (n->kind == JS_N_TRUE)
        return js_bool(1);
    if (n->kind == JS_N_FALSE)
        return js_bool(0);
    return js_null();
}

typedef JsValue (*EvalFn)(HpeJs *js, JsEnv *env, JsNode *n);

/**
 * @brief `this` 节点的求值入口 / Evaluator entry for a `this` node
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 被忽略的节点（仅为签名一致）/ The ignored node (kept for signature uniformity)
 *
 * @return 当前 `this` 值 / The current `this` value
 *
 * @see eval_this
 */
static JsValue eval_this_node(HpeJs *js, JsEnv *env, JsNode *n)
{
    HPE_UNUSED(n);
    return eval_this(js, env);
}

/**
 * @brief 求值三元条件表达式 / Evaluate a ternary conditional expression
 *
 * @details 以第 0 个子节点作条件，真则取第 1 个子节点、假则取第 2 个子节点。
 *          Child 0 is the condition; child 1 is taken when truthy and child 2 otherwise.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 节点 / Node
 *
 * @return 所选分支的值 / The value of the selected branch
 */
static JsValue eval_ternary(HpeJs *js, JsEnv *env, JsNode *n)
{
    return js_truthy(eval_expr(js, env, n->kids[0])) ? eval_expr(js, env, n->kids[1])
                                                     : eval_expr(js, env, n->kids[2]);
}

/**
 * @brief 求值正则字面量节点 / Evaluate a regex literal node
 *
 * @details 解析器把模式与其标志打包进同一缓冲：模式起始于 `n->str`，标志串位于 `pat + strlen(pat) + 1`。
 *          The parser packs the pattern and its flags into one buffer: the pattern starts at `n->str` and the flag string sits at `pat + strlen(pat) + 1`.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 节点 / Node
 *
 * @return 新建的正则对象 / The newly created regex object
 *
 * @see js_regex_new
 */
static JsValue eval_regex_node(HpeJs *js, JsEnv *env, JsNode *n)
{
    /* the parser packs the pattern and its flags into one buffer */
    const char *pat = n->str ? n->str : "";

    HPE_UNUSED(env);
    return js_regex_new(js, pat, pat + strlen(pat) + 1);
}

/**
 * @brief 求值展开节点（取内层表达式）/ Evaluate a spread node (evaluates the inner expression)
 *
 * @details 展开语义由外层容器处理，此处只求值其唯一子表达式。
 *          Spread semantics are handled by the enclosing container, so only the single sub-expression is evaluated here.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 节点 / Node
 *
 * @return 被展开表达式的值 / The value of the spread operand
 */
static JsValue eval_spread(HpeJs *js, JsEnv *env, JsNode *n)
{
    return eval_expr(js, env, n->kids[0]);
}

/**
 * @brief `yield` 节点的求值入口 / Evaluator entry for a `yield` node
 *
 * @details 以 `n->op == 1` 判定是否为 `yield*` 委托形式并转交 `eval_yield`。
 *          Whether the node is the `yield*` delegate form is decided by `n->op == 1` and forwarded to `eval_yield`.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 节点 / Node
 *
 * @return 恒为 `undefined` / Always `undefined`
 *
 * @see eval_yield
 */
static JsValue eval_yield_node(HpeJs *js, JsEnv *env, JsNode *n)
{
    return eval_yield(js, env, n, n->op == 1);
}

/**
 * @brief 空表达式求值，返回 `undefined` / Evaluate an empty expression, yielding `undefined`
 *
 * @details 所有入参均被忽略，用于表达空语句体。
 *          All inputs are ignored; used to represent an empty statement body.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 节点 / Node
 *
 * @return 恒为 `undefined` / Always `undefined`
 */
static JsValue eval_empty_expr(HpeJs *js, JsEnv *env, JsNode *n)
{
    HPE_UNUSED(js);
    HPE_UNUSED(env);
    HPE_UNUSED(n);
    return js_undef();
}

/* node kind -> evaluator. Literal kinds are resolved before this table so this
 * one only carries nodes that need evaluation. */
static const struct {
    int kind;
    EvalFn fn;
} k_eval_table[] = {
    { JS_N_IDENT, eval_ident },          { JS_N_THIS, eval_this_node },
    { JS_N_ARRAY, eval_array_literal },  { JS_N_OBJECT, object_literal },
    { JS_N_TEMPLATE, template_eval },    { JS_N_BIN, eval_bin },
    { JS_N_UN, eval_unary },             { JS_N_TERNARY, eval_ternary },
    { JS_N_SEQ, eval_seq },              { JS_N_ASSIGN, eval_assign },
    { JS_N_MEMBER, eval_member },        { JS_N_SUPER_MEMBER, eval_super_member },
    { JS_N_SUPER_CALL, super_call },     { JS_N_CALL, eval_call },
    { JS_N_NEW, eval_new },              { JS_N_REGEX, eval_regex_node },
    { JS_N_AWAIT, eval_await },          { JS_N_YIELD, eval_yield_node },
    { JS_N_FUNC, make_method },          { JS_N_SPREAD, eval_spread },
    { JS_N_UPDATE, eval_update },        { JS_N_CLASS, class_eval },
    { JS_N_BLOCK, eval_empty_expr },
};

/**
 * @brief 表达式求值总入口 / General entry for expression evaluation
 *
 * @details 字面量节点（数值/字符串/真/假/空）统一由 `eval_literal` 处理，其余按 `k_eval_table` 按种类分派到相应求值函数。
 *          Literal nodes (number/string/true/false/null) all go through `eval_literal`; the rest are dispatched by kind through `k_eval_table` to their evaluator.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 节点 / Node
 *
 * @return 表达式结果值 / The expression result value
 *
 * @note `n` 为 `NULL` 时返回 `undefined`；无匹配节点种类时报错 / Returns `undefined` when `n` is `NULL`; raises an error when no node kind matches
 */
static JsValue eval_expr(HpeJs *js, JsEnv *env, JsNode *n)
{
    size_t i;

    if (!n)
        return js_undef();

    /* literal nodes carry no side effects, so they share one entry point */
    switch (n->kind) {
    case JS_N_NUM:
    case JS_N_STR:
    case JS_N_TRUE:
    case JS_N_FALSE:
    case JS_N_NULL:
        return eval_literal(js, n, n->str);
    default:
        break;
    }

    for (i = 0; i < sizeof(k_eval_table) / sizeof(k_eval_table[0]); i++) {
        if (k_eval_table[i].kind == n->kind)
            return k_eval_table[i].fn(js, env, n);
    }
    js_error(js, n->line, "unsupported expression node %d", n->kind);
    return js_undef();
}
/* ---------------- statements ---------------- */

/**
 * @brief 提升函数声明与 `var` 绑定到函数/程序作用域 / Hoist function declarations and `var` bindings into the function/program scope
 *
 * @details 函数声明直接定义为其求值结果；`var` 仅提取其中的标识符并预置为 `undefined`；块、标签、循环、try、switch、export 等容器节点递归下探。
 *          Function declarations are defined outright to their evaluated result; `var` only extracts the identifiers and pre-defines them as `undefined`; container nodes such as blocks, labels, loops, try, switch and export are descended recursively.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 节点 / Node
 *
 * @note 使声明之前的调用可工作（函数作用域提升）/ Enables calls before the declaration (function-scoped hoisting）
 */
static void hoist_decls(HpeJs *js, JsEnv *env, JsNode *n)
{
    int i;
    if (!n)
        return;
    switch (n->kind) {
    case JS_N_FUNC:
        if (n->str && *n->str)
            js_env_define(js, env, n->str, js_objval(js_func_new(js, n, env)));
        return;
    case JS_N_VAR:
        if (n->str && !strcmp(n->str, "var")) {
            for (i = 0; i < n->kid_count; i++) {
                JsNode *one = n->kids[i];
                JsNode *id = NULL;
                if (!one)
                    continue;
                /* `var a = 1` wraps the binding in an assignment; the `for (var a
                 * of ...)` form stores the naked binding as the single child */
                if (one->kind == JS_N_IDENT)
                    id = one;
                else if (one->kid_count > 0 && one->kids[0])
                    id = one->kids[0];
                if (id && id->kind == JS_N_IDENT && id->str)
                    js_env_define(js, env, id->str, js_undef());
            }
        }
        return;
    case JS_N_BLOCK:
    case JS_N_LABEL:
    case JS_N_IF:
    case JS_N_WHILE:
    case JS_N_DO:
    case JS_N_FOR:
    case JS_N_FORIN:
    case JS_N_FOROF:
    case JS_N_TRY:
    case JS_N_SWITCH:
    case JS_N_EXPORT:
        for (i = 0; i < n->kid_count; i++)
            hoist_decls(js, env, n->kids[i]);
        return;
    default:
        return;
    }
}

/**
 * @brief 执行变量声明语句 / Execute a variable declaration statement
 *
 * @details 逐项处理声明：有初始化表达式则求值，标识符声明调用 `js_env_declare`，解构模式交 `bind_pattern`。
 *          Each declarator is processed: the initialiser is evaluated when present, an identifier declaration calls `js_env_declare`, and a destructuring pattern is delegated to `bind_pattern`.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 节点 / Node
 *
 * @note 源码中 `var` 与非 `var` 分支当前都调用 `js_env_declare`（行为一致）/ In the source both the `var` and non-`var` branches currently call `js_env_declare` (behaviour is identical)
 */
static void exec_var_decl(HpeJs *js, JsEnv *env, JsNode *n)
{
    int i;
    for (i = 0; i < n->kid_count; i++) {
        JsNode *one = n->kids[i];
        JsValue v = js_undef();
        if (!one || !one->kids[0])
            continue;
        if (one->kids[1])
            v = eval_expr(js, env, one->kids[1]);
        if (one->kids[0]->kind == JS_N_IDENT) {
            if (n->str && !strcmp(n->str, "var"))
                js_env_declare(js, env, one->kids[0]->str, v);
            else
                js_env_declare(js, env, one->kids[0]->str, v);
        } else {
            bind_pattern(js, env, one->kids[0], v, 1);
        }
    }
}

/**
 * @brief 顺序执行语句块 / Execute a block of statements in order
 *
 * @details 逐条执行子语句，一旦产生非 `FLOW_NONE` 的控制流立即返回该流。
 *          Children are executed one by one and control flow that is not `FLOW_NONE` is returned immediately.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 节点 / Node
 *
 * @return 块结束或中断时的控制流 / The control flow at block end or on interruption
 */
static Flow exec_block(HpeJs *js, JsEnv *env, JsNode *n)
{
    Flow flow = flow_none();
    int i;
    for (i = 0; i < n->kid_count; i++) {
        flow = exec_stmt(js, env, n->kids[i]);
        if (flow.action != FLOW_NONE)
            return flow;
    }
    return flow;
}

/**
 * @brief 判断节点是否为循环语句 / Test whether a node is a loop statement
 *
 * @param[in,out] n 节点 / Node
 *
 * @return 是 `WHILE`/`DO`/`FOR`/`FOROF`/`FORIN` 之一返回 1 / 1 for one of `WHILE`/`DO`/`FOR`/`FOROF`/`FORIN`
 *
 * @note `n` 为 `NULL` 时返回 0 / Returns 0 when `n` is `NULL`
 */

static int is_loop_node(JsNode *n)
{
    if (!n)
        return 0;
    return n->kind == JS_N_WHILE || n->kind == JS_N_DO || n->kind == JS_N_FOR ||
           n->kind == JS_N_FOROF || n->kind == JS_N_FORIN;
}

/**
 * @brief 在标签名数组中查找匹配 / Look for a matching name in a label array
 *
 * @param[in]     labels 标签名数组 / Array of label names
 * @param[in]     nlabels 标签个数 / Number of labels
 * @param[in]     name 要匹配的标签名 / Label name to match
 *
 * @return 找到返回 1，否则 0 / 1 when found, 0 otherwise
 *
 * @note `name` 为 `NULL` 时直接返回 0 / Returns 0 immediately when `name` is `NULL`
 */
static int label_matches(const char *const *labels, int nlabels, const char *name)
{
    int i;
    if (!name)
        return 0;
    for (i = 0; i < nlabels; i++)
        if (labels[i] && !strcmp(labels[i], name))
            return 1;
    return 0;
}

/**
 * @brief 把循环体内产生的控制流折算为循环动作码 / Reduce control flow produced in a loop body to a loop action code
 *
 * @details `FLOW_NONE` 返回 0；`FLOW_RETURN` 原样写入 `*out` 并返回 2；`FLOW_BREAK` 无标签或标签匹配本循环时消费为 `flow_none` 并返回 1，否则写 `*out` 返回 2；`FLOW_CONTINUE` 匹配时返回 0 继续迭代，否则写 `*out` 返回 2。
 *          `FLOW_NONE` yields 0; `FLOW_RETURN` is copied to `*out` as 2; `FLOW_BREAK` is consumed (yielding `flow_none`) as 1 when unlabelled or matching this loop, otherwise written to `*out` as 2; `FLOW_CONTINUE` yields 0 while matching and otherwise writes `*out` as 2.
 *
 * @param[in]     f 循环体执行产生的控制流 / Control flow produced by the loop body
 * @param[in]     labels 标签数组 / Label array
 * @param[in]     nlabels 标签个数 / Number of labels
 * @param[out]    out 需向调用者传播的控制流 / Control flow to propagate to the caller
 *
 * @return 动作码 / The action code
 * @retval     0 继续迭代 / Keep iterating
 * @retval     1 离开本循环 / Leave this loop
 * @retval     2 把 `*out` 传播给调用者 / Propagate `*out` to the caller
 */
static int loop_flow(Flow f, const char *const *labels, int nlabels, Flow *out)
{
    if (f.action == FLOW_NONE)
        return 0;
    if (f.action == FLOW_RETURN) {
        *out = f;
        return 2;
    }
    if (f.action == FLOW_BREAK) {
        if (!f.label || label_matches(labels, nlabels, f.label)) {
            *out = flow_none();
            return 1;
        }
        *out = f;
        return 2;
    }
    /* FLOW_CONTINUE */
    if (!f.label || label_matches(labels, nlabels, f.label))
        return 0;
    *out = f;
    return 2;
}

/**
 * @brief 为一次循环迭代绑定循环变量 / Bind the loop variable for one iteration
 *
 * @details 目标为 `let`/`const` 声明时新建迭代环境并写入 `*iter_env`；`var` 或裸赋值目标直接写透到外层作用域。
 *          When the target is a `let`/`const` declaration a fresh iteration environment is created and written to `*iter_env`; a `var` or bare assignment target writes through to the enclosing scope.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] target 循环头目标（声明或绑定模式）/ The loop-head target (declaration or binding pattern)
 * @param[in]     v 值 / Value
 * @param[out]    iter_env 仅供每次迭代使用的环境（仅限 `let`/`const`）/ out: Per-iteration environment (only for `let`/`const`)
 *
 * @see loop_body_step bind_pattern
 */
static void loop_bind(HpeJs *js, JsEnv *env, JsNode *target, JsValue v, JsEnv **iter_env)
{
    int is_decl = target && target->kind == JS_N_VAR;
    JsNode *pat = is_decl ? target->kids[0] : target;
    if (is_decl && target->str && strcmp(target->str, "var") != 0) {
        *iter_env = js_env_new(js, env);
        bind_pattern(js, *iter_env, pat, v, 1);
    } else {
        bind_pattern(js, env, pat, v, !is_decl);
    }
}

/**
 * @brief 绑定循环变量并执行一次循环体 / Bind the loop variable and run one loop body iteration
 *
 * @details 有循环头目标时先经 `loop_bind` 绑定值，再以所得迭代环境执行循环体语句，最后把其控制流经 `loop_flow` 折算。
 *          When a loop-head target exists it is bound through `loop_bind`, the body statement is then executed in the resulting iteration environment, and its control flow is finally reduced through `loop_flow`.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] target 循环头目标（可为 `NULL`）/ The loop-head target (may be `NULL`)
 * @param[in]     v 本次迭代要绑定的值 / The value to bind for this iteration
 * @param[in,out] body 循环体语句节点 / The loop body statement node
 * @param[in]     labels 标签数组 / Label array
 * @param[in]     nlabels 标签个数 / Number of labels
 * @param[out]    out 需向调用者传播的控制流 / Control flow to propagate to the caller
 *
 * @return 循环动作码（0 继续、1 跳出、2 传播）/ The loop action code (0 keep, 1 break, 2 propagate)
 *
 * @see loop_bind loop_flow
 */
static int loop_body_step(HpeJs *js, JsEnv *env, JsNode *target, JsValue v, JsNode *body,
                          const char *const *labels, int nlabels, Flow *out)
{
    JsEnv *iterv = env;
    Flow f;

    if (target)
        loop_bind(js, env, target, v, &iterv);
    f = exec_stmt(js, iterv, body);
    return loop_flow(f, labels, nlabels, out);
}

/**
 * @brief 执行 `while` 循环 / Execute a `while` loop
 *
 * @details 先求条件子节点（kid 0），为真则执行循环体（kid 1）；动作码 1 结束、2 向上传播。
 *          Child 0 is the condition; while truthy the body (child 1) runs, with action code 1 ending the loop and 2 propagating upward.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 节点 / Node
 * @param[in]     labels 本循环挂载的标签名数组 / Label names attached to this loop
 * @param[in]     nlabels 标签个数 / Number of labels
 * @param[out]    out 需向调用者传播的控制流 / Control flow to propagate to the caller
 *
 * @return 循环结束或传播的控制流 / The control flow at loop end or to propagate
 *
 * @see exec_loop
 */
static Flow exec_while(HpeJs *js, JsEnv *env, JsNode *n, const char *const *labels, int nlabels,
                       Flow *out)
{
    while (js_truthy(eval_expr(js, env, n->kids[0]))) {
        int act = loop_body_step(js, env, NULL, js_undef(), n->kids[1], labels, nlabels, out);
        if (act == 1)
            return flow_none();
        if (act == 2)
            return *out;
    }
    return flow_none();
}

/**
 * @brief 执行 `do...while` 循环 / Execute a `do...while` loop
 *
 * @details 先执行循环体（kid 0），再求条件（kid 1）决定是否继续；动作码 1 结束、2 向上传播。
 *          The body (child 0) runs first, then the condition (child 1) decides whether to continue; action code 1 ends the loop and 2 propagates upward.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 节点 / Node
 * @param[in]     labels 本循环挂载的标签名数组 / Label names attached to this loop
 * @param[in]     nlabels 标签个数 / Number of labels
 * @param[out]    out 需向调用者传播的控制流 / Control flow to propagate to the caller
 *
 * @return 循环结束或传播的控制流 / The control flow at loop end or to propagate
 *
 * @see exec_loop
 */
static Flow exec_do(HpeJs *js, JsEnv *env, JsNode *n, const char *const *labels, int nlabels,
                    Flow *out)
{
    do {
        int act = loop_body_step(js, env, NULL, js_undef(), n->kids[0], labels, nlabels, out);
        if (act == 1)
            return flow_none();
        if (act == 2)
            return *out;
    } while (js_truthy(eval_expr(js, env, n->kids[1])));
    return flow_none();
}

/**
 * @brief 执行 `for` 循环 / Execute a `for` loop
 *
 * @details 子节点依次为初始化、条件、更新与循环体；当初始化是 `let`/`const` 声明时新建作用域使每次迭代闭包各自持有副本，`var` 与裸表达式留在外层；每轮先测条件，执行循环体并折算控制流，再执行更新。
 *          Children are init, test, update and body; when init is a `let`/`const` declaration a fresh scope is created so each iteration closes over its own copy, while `var` and bare expressions stay outer; each round tests the condition, runs the body, reduces its flow, then runs the update.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 节点 / Node
 * @param[in]     labels 本循环挂载的标签名数组 / Label names attached to this loop
 * @param[in]     nlabels 标签个数 / Number of labels
 * @param[out]    out 需向调用者传播的控制流 / Control flow to propagate to the caller
 *
 * @return 循环结束或传播的控制流 / The control flow at loop end or to propagate
 *
 * @see exec_loop exec_var_decl
 */
static Flow exec_for(HpeJs *js, JsEnv *env, JsNode *n, const char *const *labels, int nlabels,
                     Flow *out)
{
    JsNode *init = n->kids[0];
    JsNode *test = n->kids[1];
    JsNode *update = n->kids[2];
    JsNode *body = n->kids[3];
    /* `let` / `const` loop heads get a fresh scope so each iteration closes
     * over its own copy; `var` (and bare expressions) stay in the outer scope */
    JsEnv *lenv = env;

    if (init && init->kind == JS_N_VAR && init->str && strcmp(init->str, "var") != 0)
        lenv = js_env_new(js, env);
    if (init) {
        if (init->kind == JS_N_VAR)
            exec_var_decl(js, lenv, init);
        else
            eval_expr(js, lenv, init);
    }
    for (;;) {
        Flow f;
        int act;

        if (test && !js_truthy(eval_expr(js, lenv, test)))
            break;
        f = exec_stmt(js, lenv, body);
        act = loop_flow(f, labels, nlabels, out);
        if (act == 1)
            return flow_none();
        if (act == 2)
            return *out;
        if (update)
            eval_expr(js, lenv, update);
    }
    return flow_none();
}

/**
 * @brief 执行 `for...of` 循环 / Execute a `for...of` loop
 *
 * @details 对第 1 个子节点求值得到可迭代表，取其长度后按“绑定循环变量 + 执行循环体（kid 2）”逐项迭代。
 *          Child 1 is evaluated into an iterable, its length taken, and the loop iterates item by item while binding the loop variable and running the body (child 2).
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 节点 / Node
 * @param[in]     labels 本循环挂载的标签名数组 / Label names attached to this loop
 * @param[in]     nlabels 标签个数 / Number of labels
 * @param[out]    out 需向调用者传播的控制流 / Control flow to propagate to the caller
 *
 * @return 循环结束或传播的控制流 / The control flow at loop end or to propagate
 *
 * @see exec_loop loop_body_step
 */
static Flow exec_forof(HpeJs *js, JsEnv *env, JsNode *n, const char *const *labels, int nlabels,
                       Flow *out)
{
    JsValue it = eval_expr(js, env, n->kids[1]);
    int len = js_iter_len(js, it), i;

    for (i = 0; i < len; i++) {
        int act = loop_body_step(js, env, n->kids[0], js_iter_at(js, it, i), n->kids[2], labels,
                                 nlabels, out);
        if (act == 1)
            return flow_none();
        if (act == 2)
            return *out;
    }
    return flow_none();
}

/**
 * @brief 对字符串执行 `for...in`（枚举字符下标）/ Run `for...in` over a string (enumerates character indices)
 *
 * @details 以字符串长度为界逐项迭代，把下标格式化为十进制字符串后作为键绑定循环变量并执行循环体。
 *          Iterates up to the string length, formatting each index as a decimal string that is bound as the key before running the body.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 节点 / Node
 * @param[in]     obj 被枚举的字符串值 / The string value being enumerated
 * @param[in]     labels 本循环挂载的标签名数组 / Label names attached to this loop
 * @param[in]     nlabels 标签个数 / Number of labels
 * @param[out]    out 需向调用者传播的控制流 / Control flow to propagate to the caller
 *
 * @return 循环结束或传播的控制流 / The control flow at loop end or to propagate
 *
 * @see exec_forin
 */
static Flow exec_forin_string(HpeJs *js, JsEnv *env, JsNode *n, JsValue obj,
                              const char *const *labels, int nlabels, Flow *out)
{
    int len = js_iter_len(js, obj), i;

    for (i = 0; i < len; i++) {
        char buf[32];
        int act;

        snprintf(buf, sizeof(buf), "%d", i);
        act = loop_body_step(js, env, n->kids[0], js_string(js, buf), n->kids[2], labels, nlabels,
                             out);
        if (act == 1)
            return flow_none();
        if (act == 2)
            return *out;
    }
    return flow_none();
}

/**
 * @brief 对对象执行 `for...in`（遍历自身可枚举键）/ Run `for...in` over an object (walks own enumerable keys)
 *
 * @details 取对象自身可枚举键数组后逐键绑定并执行循环体；键数组在每条退出路径（含提前 break/return）都会释放。
 *          After fetching the object's own enumerable keys, each key is bound and the body run; the key array is freed on every exit path, including an early break/return.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 节点 / Node
 * @param[in]     obj 被枚举的对象值 / The object value being enumerated
 * @param[in]     labels 本循环挂载的标签名数组 / Label names attached to this loop
 * @param[in]     nlabels 标签个数 / Number of labels
 * @param[out]    out 需向调用者传播的控制流 / Control flow to propagate to the caller
 *
 * @return 循环结束或传播的控制流 / The control flow at loop end or to propagate
 *
 * @see exec_forin
 */
static Flow exec_forin_object(HpeJs *js, JsEnv *env, JsNode *n, JsValue obj,
                              const char *const *labels, int nlabels, Flow *out)
{
    char **keys;
    int nk = 0, k;

    js_obj_own_keys(obj.obj, &keys, &nk);
    for (k = 0; k < nk; k++) {
        int act = loop_body_step(js, env, n->kids[0], js_string(js, keys[k]), n->kids[2], labels,
                                 nlabels, out);
        if (act != 0) {
            free(keys);
            return act == 1 ? flow_none() : *out;
        }
    }
    free(keys);
    return flow_none();
}

/**
 * @brief 分派 `for...in` 到字符串或对象实现 / Dispatch `for...in` to the string or object implementation
 *
 * @details 第 1 个子节点求值后：字符串转 `exec_forin_string`，对象转 `exec_forin_object`，其余返回空控制流。
 *          After evaluating child 1, strings go to `exec_forin_string`, objects to `exec_forin_object`, and anything else returns an empty flow.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 节点 / Node
 * @param[in]     labels 本循环挂载的标签名数组 / Label names attached to this loop
 * @param[in]     nlabels 标签个数 / Number of labels
 * @param[out]    out 需向调用者传播的控制流 / Control flow to propagate to the caller
 *
 * @return 循环结束或传播的控制流 / The control flow at loop end or to propagate
 *
 * @see exec_forin_string exec_forin_object
 */
static Flow exec_forin(HpeJs *js, JsEnv *env, JsNode *n, const char *const *labels, int nlabels,
                       Flow *out)
{
    JsValue obj = eval_expr(js, env, n->kids[1]);

    if (obj.t == JS_STR)
        return exec_forin_string(js, env, n, obj, labels, nlabels, out);
    if (obj.t == JS_OBJ && obj.obj)
        return exec_forin_object(js, env, n, obj, labels, nlabels, out);
    return flow_none();
}

/**
 * @brief 统一循环执行器 / Unified loop executor
 *
 * @details 按节点种类把 `WHILE`/`DO`/`FOR`/`FOROF`/`FORIN` 分派到各自实现，并携带本循环的标签链供 `break`/`continue` 消费。
 *          Dispatches `WHILE`/`DO`/`FOR`/`FOROF`/`FORIN` to their respective implementations by node kind, carrying this loop's label chain for `break`/`continue` to consume.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 节点 / Node
 * @param[in]     labels 本循环挂载的标签名数组 / Label names attached to this loop
 * @param[in]     nlabels 标签个数 / Number of labels
 *
 * @return 循环结束或传播的控制流 / The control flow at loop end or to propagate
 *
 * @note 无法识别的节点种类返回空控制流 / An unrecognised node kind returns an empty flow
 *
 * @see exec_while exec_do exec_for exec_forof exec_forin
 */
static Flow exec_loop(HpeJs *js, JsEnv *env, JsNode *n, const char *const *labels, int nlabels)
{
    Flow out = flow_none();

    switch (n->kind) {
    case JS_N_WHILE:
        return exec_while(js, env, n, labels, nlabels, &out);
    case JS_N_DO:
        return exec_do(js, env, n, labels, nlabels, &out);
    case JS_N_FOR:
        return exec_for(js, env, n, labels, nlabels, &out);
    case JS_N_FOROF:
        return exec_forof(js, env, n, labels, nlabels, &out);
    case JS_N_FORIN:
        return exec_forin(js, env, n, labels, nlabels, &out);
    default:
        return flow_none();
    }
}

/**
 * @brief 在自带着陆点下执行 `try` 块 / Run the `try` block under its own landing pad
 *
 * @details 把新 `JsTryFrame` 压入 `js->try_top`；`setjmp` 返回 0 时执行块语句并把其控制流写入 `*flow`，否则置 `*pending` 为 1 并通过 `*thrown` 取回 `js->thrown`（同时复位）。
 *          A new `JsTryFrame` is pushed onto `js->try_top`; when `setjmp` returns 0 the block runs and its flow is written to `*flow`, otherwise `*pending` is set to 1 and `js->thrown` is retrieved through `*thrown` (and reset).
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] stmts 语句节点 / Statement node
 * @param[in,out] block_env 执行块所用的环境（为 `NULL` 时用 `env`）/ The environment used for the block (`env` when `NULL`)
 * @param[out]    flow 块正常完成时的控制流 / Control flow when the block completes normally
 * @param[out]    pending 置 1 表示捕获到抛出 / Set to 1 when a throw was caught
 * @param[out]    thrown 捕获到的抛出值 / The caught thrown value
 *
 * @note `setjmp` 保留在本帧内，使 `longjmp` 能落回此处 / `setjmp` is kept inside this frame so that `longjmp` can land here
 *
 * @see exec_try_catch exec_try_finally
 */
static void exec_try_guard(HpeJs *js, JsEnv *env, JsNode *stmts, JsEnv *block_env, Flow *flow,
                           int *pending, JsValue *thrown)
{
    JsTryFrame f;
    f.prev = js->try_top;
    js->try_top = &f;
    if (setjmp(f.jmp) == 0) {
        *flow = exec_block(js, block_env ? block_env : env, stmts);
    } else {
        *pending = 1;
        *thrown = js->thrown;
        js->thrown = js_undef();
    }
    js->try_top = f.prev;
}

/**
 * @brief 执行 `catch` 处理块 / Run the `catch` handler
 *
 * @details 新建捕获作用域，把抛出值绑定到 catch 模式（若有），并在 `js->in_catch` 计数内执行处理体；若处理体内又抛出则 `*pending` 置 1 并通过 `*rethrown` 取回。
 *          A catch scope is created, the thrown value is bound to the catch pattern (when present), and the handler body runs with `js->in_catch` incremented; a throw inside the handler sets `*pending` to 1 and retrieves the value through `*rethrown`.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] cc catch 子句节点 `[pattern, body]` / The catch clause node `[pattern, body]`
 * @param[in]     thrown 由 `try` 块抛出的值 / The value thrown by the `try` block
 * @param[out]    flow 处理体正常完成时的控制流 / Control flow when the handler completes normally
 * @param[out]    pending 置 1 表示处理体内再次抛出 / Set to 1 when the handler throws again
 * @param[out]    rethrown 处理体重新抛出的值 / The value re-thrown by the handler
 *
 * @see exec_try_guard
 */
static void exec_try_catch(HpeJs *js, JsEnv *env, JsNode *cc, JsValue thrown, Flow *flow,
                           int *pending, JsValue *rethrown)
{
    JsEnv *cenv = js_env_new(js, env);
    JsTryFrame f;

    *pending = 0;
    f.prev = js->try_top;
    js->try_top = &f;
    if (setjmp(f.jmp) == 0) {
        if (cc->kids[0])
            bind_pattern(js, cenv, cc->kids[0], thrown, 1);
        js->in_catch++;
        *flow = exec_block(js, cenv, cc->kids[1]);
        js->in_catch--;
    } else {
        *pending = 1;
        *rethrown = js->thrown;
        js->thrown = js_undef();
    }
    js->try_top = f.prev;
}

/**
 * @brief 执行 `finally` 块 / Run the `finally` block
 *
 * @details 正常完成时仅当 `finally` 自身产生非 `FLOW_NONE` 的突然完成才覆盖 `*flow` 并清除 `*pending`；若 `finally` 内抛出则置 `*pending` 为 1 并通过 `*rethrown` 取回，从而取消在途抛出。
 *          On normal completion the pending flow is overridden and `*pending` cleared only when `finally` itself completes abruptly with a non-`FLOW_NONE` action; a throw inside `finally` sets `*pending` to 1 and retrieves the value through `*rethrown`, cancelling an in-flight throw.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] fb `finally` 块节点 / The `finally` block node
 * @param[in,out] flow 待覆盖的控制流 / Control flow that may be overridden
 * @param[out]    pending 置 1 表示 `finally` 内抛出 / Set to 1 when `finally` throws
 * @param[out]    rethrown `finally` 内抛出的值 / The value thrown inside `finally`
 *
 * @see exec_try_guard exec_try
 */
static void exec_try_finally(HpeJs *js, JsEnv *env, JsNode *fb, Flow *flow, int *pending,
                             JsValue *rethrown)
{
    JsTryFrame f;
    f.prev = js->try_top;
    js->try_top = &f;
    if (setjmp(f.jmp) == 0) {
        Flow ff = exec_block(js, env, fb);
        if (ff.action != FLOW_NONE) {
            *flow = ff;
            *pending = 0;
        }
    } else {
        *pending = 1;
        *rethrown = js->thrown;
        js->thrown = js_undef();
    }
    js->try_top = f.prev;
}

/**
 * @brief 执行 `try` / `catch` / `finally` 语句 / Execute a `try` / `catch` / `finally` statement
 *
 * @details 先在着陆点下执行 `try` 块；有待处理抛出且有 catch 子句时执行 catch；有 finally 时执行 finally；最后若仍待处理则经 `js_throw_value` 重新抛出。
 *          The `try` block runs under a landing pad first; when a throw is pending and a catch clause exists the handler runs; a `finally` block runs when present; anything still pending is re-thrown through `js_throw_value`.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 节点 / Node
 *
 * @return 语句完成后的控制流 / The control flow after the statement completes
 *
 * @see exec_try_guard exec_try_catch exec_try_finally
 */
static Flow exec_try(HpeJs *js, JsEnv *env, JsNode *n)
{
    JsNode *cc = n->kid_count > 1 ? n->kids[1] : NULL;
    JsNode *fb = n->kid_count > 2 ? n->kids[2] : NULL;
    Flow flow = flow_none();
    JsValue thrown = js_undef();
    int pending = 0;

    exec_try_guard(js, env, n->kids[0], NULL, &flow, &pending, &thrown);
    if (pending && cc)
        exec_try_catch(js, env, cc, thrown, &flow, &pending, &thrown);
    if (fb)
        exec_try_finally(js, env, fb, &flow, &pending, &thrown);
    if (pending)
        js_throw_value(js, thrown);
    return flow;
}

/**
 * @brief 执行 `switch` 语句 / Execute a `switch` statement
 *
 * @details 以严格相等（`js_values_equal` 最后参数为 1）在 `switch` 作用域中定位入口子句，未命中则用 `default`；随后从入口子句起顺序执行、向后贯穿，直到无标签 `break` 或 `return`/`throw`。
 *          The entry clause is located with strict equality (`js_values_equal` with last argument 1) inside a `switch` scope, falling back to `default`; execution then proceeds from that clause and falls through until an unlabelled `break` or a `return`/`throw`.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 节点 / Node
 *
 * @return 语句完成后的控制流 / The control flow after the statement completes
 *
 * @note 带标签的 `break` 会向外传播，而非终止本 `switch` / A labelled `break` propagates outward instead of ending this `switch`
 */
static Flow exec_switch(HpeJs *js, JsEnv *env, JsNode *n)
{
    Flow none = flow_none();
    JsValue test = eval_expr(js, env, n->kids[0]);
    JsEnv *senv = js_env_new(js, env);
    int i, matched = -1, default_idx = -1;

    for (i = 1; i < n->kid_count; i++) {
        JsNode *clause = n->kids[i];
        JsNode *label = clause->kids[0];

        if (!label) {
            default_idx = i;
            continue;
        }
        if (matched < 0 && js_values_equal(js, test, eval_expr(js, senv, label), 1))
            matched = i;
    }
    if (matched < 0)
        matched = default_idx;
    if (matched < 0)
        return none;
    for (i = matched; i < n->kid_count; i++) {
        JsNode *clause = n->kids[i];
        int k;

        for (k = 1; k < clause->kid_count; k++) {
            Flow f = exec_stmt(js, senv, clause->kids[k]);

            if (f.action == FLOW_RETURN)
                return f;
            /* an unlabelled break leaves the switch itself */
            if (f.action == FLOW_BREAK) {
                if (f.label)
                    return f;
                return none;
            }
        }
    }
    return none;
}

/**
 * @brief 执行带标签的语句 / Execute a labelled statement
 *
 * @details 内层为循环节点时，向上收集整条标签链（至多 32 个）并把该链交给 `exec_loop`，使内层 `continue 标签`/`break 标签` 正确定位；否则执行内层语句，并把指定本标签的 `break` 就地消费。
 *          When the inner statement is a loop node the whole label chain is collected upward (up to 32) and handed to `exec_loop`, letting an inner `continue label`/`break label` target that loop; otherwise the inner statement runs and a `break` naming this label is consumed here.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 节点 / Node
 *
 * @return 语句完成后的控制流 / The control flow after the statement completes
 *
 * @see is_loop_node exec_loop
 */
static Flow exec_label(HpeJs *js, JsEnv *env, JsNode *n)
{
    JsNode *inner = n->kids[0];

    if (is_loop_node(inner)) {
        const char *stack[32];
        int depth = 0;
        JsNode *cur = n;

        while (cur && cur->kind == JS_N_LABEL && depth < 32) {
            stack[depth++] = cur->str;
            cur = cur->kids[0];
        }
        return exec_loop(js, env, cur, stack, depth);
    }
    {
        Flow f = exec_stmt(js, env, inner);

        /* a `break` naming this label is consumed here */
        if (f.action == FLOW_BREAK && f.label && n->str && !strcmp(f.label, n->str)) {
            f.action = FLOW_NONE;
            f.label = NULL;
        }
        return f;
    }
}

/**
 * @brief 执行 `return` 语句 / Execute a `return` statement
 *
 * @details 构造 `FLOW_RETURN` 控制流并求值返回表达式（缺省为 `undefined`）；若返回值是 promise 则泵空作业队列，使观察该 promise 的 `.then` 回调先运行。
 *          A `FLOW_RETURN` flow is built with the return expression evaluated (`undefined` when absent); when the result is a promise the job queue is drained so `.then` callbacks observing it run first.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 节点 / Node
 *
 * @return 承载返回值的 `FLOW_RETURN` 控制流 / The `FLOW_RETURN` flow carrying the return value
 */
static Flow exec_return(HpeJs *js, JsEnv *env, JsNode *n)
{
    Flow flow = flow_none();

    flow.action = FLOW_RETURN;
    flow.ret = n->kids[0] ? eval_expr(js, env, n->kids[0]) : js_undef();
    if (flow.ret.t == JS_OBJ && flow.ret.obj && flow.ret.obj->kind == JS_OBJ_PROMISE)
        hpe_js_drain_jobs(js);
    return flow;
}

/**
 * @brief 执行 `if` 语句 / Execute an `if` statement
 *
 * @details 条件（kid 0）为真执行 then 分支（kid 1），否则在存在 else 分支（kid 2）时执行之。
 *          The then branch (child 1) runs when the condition (child 0) is truthy, otherwise the else branch (child 2) runs when present.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 节点 / Node
 *
 * @return 所选分支的控制流，无分支时为空控制流 / The selected branch's flow, or an empty flow when no branch runs
 */
static Flow exec_if(HpeJs *js, JsEnv *env, JsNode *n)
{
    Flow flow = flow_none();

    if (js_truthy(eval_expr(js, env, n->kids[0])))
        return exec_stmt(js, env, n->kids[1]);
    if (n->kid_count > 2)
        return exec_stmt(js, env, n->kids[2]);
    return flow;
}

/**
 * @brief 执行 `throw` 语句 / Execute a `throw` statement
 *
 * @details 求值抛出表达式后经 `js_throw_value` 抛向最近的活跃 try 帧，并用 `longjmp` 展开。
 *          The thrown expression is evaluated and raised through `js_throw_value` to the nearest active try frame, unwinding with `longjmp`.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 节点 / Node
 *
 * @return 恒为空控制流（不会正常返回）/ Always an empty flow (never returns normally)
 *
 * @see exec_try_guard
 */
static Flow exec_throw(HpeJs *js, JsEnv *env, JsNode *n)
{
    js_throw_value(js, eval_expr(js, env, n->kids[0]));
    return flow_none();
}

/**
 * @brief 执行表达式语句 / Execute an expression statement
 *
 * @details 求值子表达式；若结果为 promise 则泵空作业队列以促使处理器落定。
 *          The sub-expression is evaluated; when the result is a promise the job queue is drained so its handlers settle.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 节点 / Node
 *
 * @return 恒为空控制流 / Always an empty flow
 */
static Flow exec_expr_stmt(HpeJs *js, JsEnv *env, JsNode *n)
{
    JsValue v = eval_expr(js, env, n->kids[0]);

    if (v.t == JS_OBJ && v.obj && v.obj->kind == JS_OBJ_PROMISE)
        hpe_js_drain_jobs(js);
    return flow_none();
}

/**
 * @brief 执行 `break` / `continue`，构造对应控制流 / Execute `break` / `continue` by building the corresponding flow
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] n 节点 / Node
 * @param[in]     action 控制流动作（`FLOW_BREAK` 或 `FLOW_CONTINUE`）/ The flow action (`FLOW_BREAK` or `FLOW_CONTINUE`)
 *
 * @return 带动作码与标签的控制流 / The flow carrying the action code and label
 *
 * @note `js` 参数未被使用 / The `js` parameter is unused
 */
static Flow exec_break(HpeJs *js, JsNode *n, int action)
{
    Flow flow = flow_none();

    HPE_UNUSED(js);
    flow.action = action;
    flow.label = n->str;
    return flow;
}

/**
 * @brief 空语句执行器，返回空控制流 / No-op statement executor returning an empty flow
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 节点 / Node
 *
 * @return 恒为空控制流 / Always an empty flow
 */
static Flow exec_noop(HpeJs *js, JsEnv *env, JsNode *n)
{
    HPE_UNUSED(js);
    HPE_UNUSED(env);
    HPE_UNUSED(n);
    return flow_none();
}

/* statement kind -> executor; nodes that evaluate a subexpression as part of
 * their own bookkeeping are handled before this table */
static const struct {
    int kind;
    Flow (*fn)(HpeJs *js, JsEnv *env, JsNode *n);
} k_stmt_table[] = {
    { JS_N_SWITCH, exec_switch }, { JS_N_TRY, exec_try },
    { JS_N_RETURN, exec_return }, { JS_N_LABEL, exec_label },
    { JS_N_EXPORT, export_stmt }, { JS_N_IMPORT, import_stmt },
    { JS_N_IF, exec_if },         { JS_N_THROW, exec_throw },
    { JS_N_EXPR, exec_expr_stmt }, { JS_N_EMPTY, exec_noop },
};

/**
 * @brief 语句执行总入口 / General entry for statement execution
 *
 * @details 块语句新建块级环境并先提升声明；`var`/函数/类各自处理；循环统一转 `exec_loop`；`break`/`continue` 构造控制流；其余按 `k_stmt_table` 按种类分派。
 *          Block statements create a block scope and hoist declarations first; `var`/function/class are handled individually; loops all go through `exec_loop`; `break`/`continue` build flows; the rest are dispatched by kind through `k_stmt_table`.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 节点 / Node
 *
 * @return 语句执行产生的控制流 / The control flow produced by the statement
 *
 * @note `n` 为 `NULL` 或无匹配种类时返回空控制流 / Returns an empty flow when `n` is `NULL` or no kind matches
 */
static Flow exec_stmt(HpeJs *js, JsEnv *env, JsNode *n)
{
    size_t i;

    if (!n)
        return flow_none();

    switch (n->kind) {
    case JS_N_BLOCK: {
        JsEnv *benv = js_env_new(js, env);
        hoist_decls(js, benv, n);
        return exec_block(js, benv, n);
    }
    case JS_N_VAR:
        exec_var_decl(js, env, n);
        return flow_none();
    case JS_N_FUNC:
        js_env_declare(js, env, n->str ? n->str : "", js_objval(js_func_new(js, n, env)));
        return flow_none();
    case JS_N_CLASS:
        class_eval(js, env, n);
        return flow_none();
    case JS_N_WHILE:
    case JS_N_DO:
    case JS_N_FOR:
    case JS_N_FOROF:
    case JS_N_FORIN:
        return exec_loop(js, env, n, NULL, 0);
    case JS_N_BREAK:
        return exec_break(js, n, FLOW_BREAK);
    case JS_N_CONTINUE:
        return exec_break(js, n, FLOW_CONTINUE);
    default:
        break;
    }
    for (i = 0; i < sizeof(k_stmt_table) / sizeof(k_stmt_table[0]); i++) {
        if (k_stmt_table[i].kind == n->kind)
            return k_stmt_table[i].fn(js, env, n);
    }
    return flow_none();
}
/* ---------------- es modules ---------------- */

/* module states stored in JsModule.evaluated */
enum { MOD_PENDING = 0, MOD_RUNNING = 1, MOD_DONE = 2 };

/**
 * @brief 按路径查找已登记的模块 / Find an already registered module by path
 *
 * @details 线性遍历 `js->modules` 链表，比较 `path` 字段。
 *          The `js->modules` list is walked linearly comparing the `path` field.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     path 文件路径 / File path
 *
 * @return 命中的模块记录，未找到为 `NULL` / The matching module record, or `NULL` when not found
 *
 * @see module_load
 */
static JsModule *module_find(HpeJs *js, const char *path)
{
    JsModule *m;
    for (m = js->modules; m; m = m->next)
        if (!strcmp(m->path, path))
            return m;
    return NULL;
}

/**
 * @brief 解析模块说明符并返回模块记录 / Resolve a module specifier and return its module record
 *
 * @details 安装了宿主加载器（`js->loader.load`）时优先由它解析路径与源码，否则从本地文件系统读取：以 `/` 开头为绝对路径，相对说明符与 `from_dir` 拼接，裸说明符报错；命中已登记模块则直接复用。
 *          When a host loader is installed (`js->loader.load`) it resolves the path and source first; otherwise the module is read from the local filesystem: a leading `/` is absolute, a relative specifier is joined with `from_dir`, and a bare specifier is an error; an already registered module is reused directly.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     spec 模块说明符 / Module specifier
 * @param[in]     from_dir 发起导入的模块所在目录 / Directory of the importing module
 *
 * @return 模块记录 / The module record
 *
 * @note 无法解析时经 `js_error` 报错（不返回）/ Raises through `js_error` when resolution fails (does not return)
 *
 * @see module_find module_run
 */
static JsModule *module_load(HpeJs *js, const char *spec, const char *from_dir)
{
    char path[1024];
    char *src = NULL;
    JsModule *m;

    if (!spec || !*spec)
        js_error(js, 0, "invalid module specifier");
    if (js->loader.load) {
        char *rpath = NULL;
        if (!js->loader.load(js->loader.ud, spec, from_dir, &rpath, &src) || !rpath)
            js_error(js, 0, "cannot resolve module '%s'", spec);
        m = module_find(js, rpath);
        if (m)
            return m;
        {
            m = (JsModule *)malloc(sizeof(JsModule));
            memset(m, 0, sizeof(*m));
            m->path = rpath;
            m->src = src;
            m->next = js->modules;
            js->modules = m;
        }
        return m;
    }
    if (spec[0] != '/' && spec[0] != '.')
        js_error(js, 0, "cannot resolve bare module specifier '%s'", spec);
    if (spec[0] == '/')
        snprintf(path, sizeof(path), "%s", spec);
    else
        hpe_path_join(path, sizeof(path), from_dir ? from_dir : ".", spec);
    m = module_find(js, path);
    if (m)
        return m;
    src = hpe_read_file(path, NULL);
    if (!src)
        js_error(js, 0, "cannot open module '%s'", path);
    m = (JsModule *)malloc(sizeof(JsModule));
    memset(m, 0, sizeof(*m));
    m->path = hpe_strdup(path);
    m->src = src;
    m->next = js->modules;
    js->modules = m;
    return m;
}

static void module_fill_ns(HpeJs *js, JsModule *m, JsNode *prog);

/**
 * @brief 求值模块体（仅一次）并返回其命名空间对象 / Evaluate a module body (once) and return its namespace object
 *
 * @details 以 `MOD_RUNNING`/`MOD_DONE` 状态防止重复求值；解析源码后建立严格模式环境与命名空间对象，切换 `current_module`/`current_dir`，先执行所有 `import`，再提升声明，随后顺序执行其余语句，最后经 `module_fill_ns` 填充命名空间。
 *          The `MOD_RUNNING`/`MOD_DONE` states prevent re-evaluation; after parsing the source a strict-mode environment and namespace object are created, `current_module`/`current_dir` are switched, all `import`s are executed first, declarations are hoisted, the remaining statements are executed in order, and the namespace is finally populated through `module_fill_ns`.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] m 模块记录 / Module record
 *
 * @return 已完成求值的模块记录 / The fully evaluated module record
 *
 * @note 模块恒为严格模式；执行到 `FLOW_NONE` 之外的控制流即中止 / Modules are always strict; execution stops at any flow other than `FLOW_NONE`
 *
 * @see module_load module_fill_ns import_stmt
 */
static JsModule *module_run(HpeJs *js, JsModule *m)
{
    JsNode *prog;
    JsModule *prev_mod;
    const char *prev_dir;
    int i;

    if (m->evaluated == MOD_DONE || m->evaluated == MOD_RUNNING)
        return m;
    m->evaluated = MOD_RUNNING;
    prog = js_parse_module(js, m->src ? m->src : "");
    m->src = NULL;
    if (prog->op == 1)
        prog->op = 1;
    m->env = js_env_new(js, js->global_env);
    m->env->strict = 1; /* modules are always strict */
    m->ns = js_module_ns_new(js, m);
    m->ns->proto = NULL;

    prev_mod = js->current_module;
    prev_dir = js->current_dir;
    js->current_module = m;
    js->current_dir = m->path;

    /* imports run before the body (module hoisting) */
    for (i = 0; i < prog->kid_count; i++)
        if (prog->kids[i] && prog->kids[i]->kind == JS_N_IMPORT)
            import_stmt(js, m->env, prog->kids[i]);
    hoist_decls(js, m->env, prog);
    for (i = 0; i < prog->kid_count; i++) {
        JsNode *st = prog->kids[i];
        if (!st || st->kind == JS_N_IMPORT)
            continue;
        if (exec_stmt(js, m->env, st).action != FLOW_NONE)
            break;
    }
    module_fill_ns(js, m, prog);

    js->current_module = prev_mod;
    js->current_dir = prev_dir;
    m->is_script = 0;
    m->evaluated = MOD_DONE;
    return m;
}

/**
 * @brief 在模块命名空间对象上定义导出 / Define an export on the module namespace object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] m 模块记录 / Module record
 * @param[in]     name 导出名 / Export name
 * @param[in]     v 导出值 / Export value
 *
 * @see ns_fill_local ns_fill_default
 */
static void ns_define(HpeJs *js, JsModule *m, const char *name, JsValue v)
{
    js_obj_set(js, m->ns, name, v);
}

/**
 * @brief 把单个本地声明的绑定发布到命名空间 / Publish one local declaration's binding into the namespace
 *
 * @details 函数/类声明按名字在环境中取槽位写入；`var` 声明则逐项提取标识符并写入各自的槽位值。
 *          A function/class declaration looks its name up in the environment and writes the slot value; a `var` declaration extracts each identifier and writes its slot value.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] m 模块记录 / Module record
 * @param[in,out] env 模块作用域环境 / The module scope environment
 * @param[in,out] d 声明节点（函数/类/`var`）/ The declaration node (function/class/`var`)
 *
 * @see ns_fill_local
 */
static void ns_define_local_decl(HpeJs *js, JsModule *m, JsEnv *env, JsNode *d)
{
    if (d->kind == JS_N_FUNC || d->kind == JS_N_CLASS) {
        JsValue *slot = d->str ? js_env_lookup(env, d->str) : NULL;
        if (d->str && slot)
            ns_define(js, m, d->str, *slot);
    } else if (d->kind == JS_N_VAR) {
        int k;
        for (k = 0; k < d->kid_count; k++) {
            JsNode *one = d->kids[k];
            if (one && one->kids[0] && one->kids[0]->kind == JS_N_IDENT) {
                JsValue *slot = js_env_lookup(env, one->kids[0]->str);
                if (slot)
                    ns_define(js, m, one->kids[0]->str, *slot);
            }
        }
    }
}

/**
 * @brief 处理 `export <declaration>` 并发布本地导出 / Handle `export <declaration>` and publish the local export
 *
 * @details op==0 形式：取第 0 个子节点交 `ns_define_local_decl`，把其中声明的名字从环境复制进命名空间。
 *          The op==0 form: child 0 is passed to `ns_define_local_decl`, copying each declared name from the environment into the namespace.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] m 模块记录 / Module record
 * @param[in,out] env 模块作用域环境 / The module scope environment
 * @param[in,out] ex 导出节点 / The export node
 *
 * @see ns_define_local_decl module_fill_ns
 */
static void ns_fill_local(HpeJs *js, JsModule *m, JsEnv *env, JsNode *ex)
{
    JsNode *d = ex->kids[0];
    if (d)
        ns_define_local_decl(js, m, env, d);
}

/**
 * @brief 处理 `export default`，从 `*default*` 槽位取值 / Handle `export default` by reading the `*default*` slot
 *
 * @details op==1 形式：读取合成绑定 `*default*`，写入命名空间的 `default` 键。
 *          The op==1 form: the synthetic `*default*` binding is read and written to the namespace's `default` key.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] m 模块记录 / Module record
 * @param[in,out] env 模块作用域环境 / The module scope environment
 *
 * @see export_stmt module_fill_ns
 */
static void ns_fill_default(HpeJs *js, JsModule *m, JsEnv *env)
{
    JsValue *slot = js_env_lookup(env, "*default*");
    ns_define(js, m, "default", slot ? *slot : js_undef());
}

/**
 * @brief 处理 `export { a, b as c } [from "src"]` 再导出 / Handle `export { a, b as c } [from "src"]` re-exports
 *
 * @details op==2 形式：若带来源字符串则先运行源模块；随后逐对处理，导出名取对的第 0 个子节点名（缺省用对名），值来自源模块命名空间的原始名或本环境绑定。
 *          The op==2 form: when a source string is present its module is run first; each pair is then processed with the export name taken from the pair's child 0 (falling back to the pair name), and the value taken from the source namespace's original name or a local binding.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] m 模块记录 / Module record
 * @param[in,out] env 模块作用域环境 / The module scope environment
 * @param[in,out] ex 导出节点 / The export node
 *
 * @see ns_fill_star module_fill_ns
 */
static void ns_fill_reexport(HpeJs *js, JsModule *m, JsEnv *env, JsNode *ex)
{
    JsNode *list = ex->kids[0];
    JsNode *src_expr = ex->kid_count > 1 ? ex->kids[1] : NULL;
    JsModule *from = NULL;
    int k;

    if (src_expr && src_expr->kind == JS_N_STR)
        from = module_run(js, module_load(js, src_expr->str, js->current_dir));
    for (k = 0; list && k < list->kid_count; k++) {
        JsNode *pair = list->kids[k];
        const char *exported = (pair->kids[0] && pair->kids[0]->str) ? pair->kids[0]->str
                                                                    : pair->str;
        if (from)
            ns_define(js, m, exported, js_obj_get(js, from->ns, pair->str));
        else {
            JsValue *slot = js_env_lookup(env, pair->str);
            ns_define(js, m, exported, slot ? *slot : js_undef());
        }
    }
}

/**
 * @brief 处理 `export * from "src"`（透传除 `default` 外的所有键）/ Handle `export * from "src"` (re-exports every key except `default`)
 *
 * @details op==3 形式：运行源模块后遍历其命名空间自身键，当 `ex->num` 非零时跳过 `default`，其余逐个写入本命名空间。
 *          The op==3 form: the source module is run, its namespace's own keys are enumerated, `default` is skipped when `ex->num` is non-zero, and the rest are written into this namespace.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] m 模块记录 / Module record
 * @param[in,out] ex 导出节点 / The export node
 *
 * @note 源模块不可解析时直接返回不导出任何键 / Returns without exporting any key when the source cannot be resolved
 *
 * @see ns_fill_reexport
 */
static void ns_fill_star(HpeJs *js, JsModule *m, JsNode *ex)
{
    JsModule *from = (ex->kids[0] && ex->kids[0]->kind == JS_N_STR)
                         ? module_run(js, module_load(js, ex->kids[0]->str, js->current_dir))
                         : NULL;
    char **keys;
    int nk, k;

    if (!from)
        return;
    js_obj_own_keys(from->ns, &keys, &nk);
    for (k = 0; k < nk; k++) {
        if (ex->num && !strcmp(keys[k], "default"))
            continue;
        ns_define(js, m, keys[k], js_obj_get(js, from->ns, keys[k]));
    }
    free(keys);
}

/**
 * @brief 把各 `export` 节点分派到对应的命名空间填充器 / Dispatch each `export` node to its kind-specific namespace filler
 *
 * @details 遍历程序子节点，按 `ex->op`（0 本地、1 默认、2 再导出、3 星号）选择 `ns_fill_*`。
 *          Program children are walked and `ns_fill_*` is selected by `ex->op` (0 local, 1 default, 2 re-export, 3 star).
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] m 模块记录 / Module record
 * @param[in,out] prog 程序根节点 / Program root node
 *
 * @see ns_fill_local ns_fill_default ns_fill_reexport ns_fill_star
 */
static void module_fill_ns(HpeJs *js, JsModule *m, JsNode *prog)
{
    JsEnv *env = m->env;
    int i;
    for (i = 0; i < prog->kid_count; i++) {
        JsNode *ex = prog->kids[i];
        if (!ex || ex->kind != JS_N_EXPORT)
            continue;
        switch (ex->op) {
        case 0: ns_fill_local(js, m, env, ex); break;
        case 1: ns_fill_default(js, m, env); break;
        case 2: ns_fill_reexport(js, m, env, ex); break;
        case 3: ns_fill_star(js, m, ex); break;
        default: break;
        }
    }
}

/**
 * @brief 执行 `export` 语句 / Execute an `export` statement
 *
 * @details op==0 转交内层声明语句执行；op==1 求值默认导出（函数直接建函数对象并绑定名字、类走 `class_eval`、其余求值）后写入 `*default*` 槽位；op==2/3 迁入按需运行来源模块。
 *          op==0 forwards to the inner declaration statement; op==1 evaluates the default export (functions are turned into function objects and bound by name, classes go through `class_eval`, everything else is evaluated) and writes it to the `*default*` slot; op==2/3 additionally run the source module on demand.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 节点 / Node
 *
 * @return 语句完成后的控制流（通常为空）/ The control flow after the statement completes (usually empty)
 *
 * @see module_fill_ns ns_fill_default
 */
static Flow export_stmt(HpeJs *js, JsEnv *env, JsNode *n)
{
    Flow flow = flow_none();
    if (n->op == 0) {
        JsNode *d = n->kids[0];
        if (d)
            return exec_stmt(js, env, d);
        return flow;
    }
    if (n->op == 1) {
        JsNode *d = n->kids[0];
        JsValue v = js_undef();
        if (!d)
            return flow;
        if (d->kind == JS_N_FUNC) {
            v = js_objval(js_func_new(js, d, env));
            if (d->str && *d->str)
                js_env_declare(js, env, d->str, v);
        } else if (d->kind == JS_N_CLASS) {
            v = class_eval(js, env, d);
        } else {
            v = eval_expr(js, env, d);
        }
        js_env_declare(js, env, "*default*", v);
        return flow;
    }
    if (n->op == 2 && n->kid_count > 1 && n->kids[1] && n->kids[1]->kind == JS_N_STR)
        module_run(js, module_load(js, n->kids[1]->str, js->current_dir));
    else if (n->op == 3 && n->kids[0] && n->kids[0]->kind == JS_N_STR)
        module_run(js, module_load(js, n->kids[0]->str, js->current_dir));
    return flow;
}

/**
 * @brief 执行 `import` 语句 / Execute an `import` statement
 *
 * @details op==1 为副作用导入，仅运行来源模块；否则解析来源说明符后运行该模块，并逐项绑定：命名空间导入（op 2）绑整个 `ns`、默认导入（op 1）取 `default`、具名导入取原始名（缺省用本地名）。
 *          op==1 is a side-effect import that only runs the source module; otherwise the source module is run and each item is bound: a namespace import (op 2) binds the whole `ns`, a default import (op 1) takes `default`, and a named import takes the original name (falling back to the local name).
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 词法环境 / Lexical environment
 * @param[in,out] n 节点 / Node
 *
 * @return 语句完成后的控制流（通常为空）/ The control flow after the statement completes (usually empty)
 *
 * @see module_run module_load
 */
static Flow import_stmt(HpeJs *js, JsEnv *env, JsNode *n)
{
    JsModule *m;
    Flow flow = flow_none();
    if (n->op == 1) {
        if (n->kids[0] && n->kids[0]->kind == JS_N_STR)
            module_run(js, module_load(js, n->kids[0]->str, js->current_dir));
        return flow;
    }
    if (n->kid_count < 2)
        return flow;
    {
        JsNode *list = n->kids[0];
        JsNode *spec = n->kids[1];
        int i;
        if (!spec || spec->kind != JS_N_STR)
            return flow;
        m = module_run(js, module_load(js, spec->str, js->current_dir));
        for (i = 0; list && i < list->kid_count; i++) {
            JsNode *pair = list->kids[i];
            if (!pair)
                continue;
            if (pair->op == 2) {
                js_env_declare(js, env, pair->str, js_objval(m->ns));
            } else if (pair->op == 1) {
                js_env_declare(js, env, pair->str, js_obj_get(js, m->ns, "default"));
            } else {
                const char *src = (pair->kids[0] && pair->kids[0]->str) ? pair->kids[0]->str
                                                                       : pair->str;
                js_env_declare(js, env, pair->str, js_obj_get(js, m->ns, src));
            }
        }
    }
    return flow;
}

/**
 * @brief 把原生函数指针包装成 JS 值 / Wrap a native function pointer into a JS value
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     fn 原生函数指针 / Native function pointer
 * @param[in,out] ud 透传给宿主的用户数据 / Opaque user data passed through to the host
 *
 * @return 承载该原生函数的 JS 值 / The JS value carrying the native function
 *
 * @see def_method
 */

static JsValue mk_native(HpeJs *js, JsNativeFn fn, void *ud)
{
    return js_objval(js_native_new(js, fn, ud));
}

/**
 * @brief 按下标取实参，越界返回 `undefined` / Fetch the argument at an index, yielding `undefined` when out of range
 *
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 实参个数 / Number of actual arguments
 * @param[in]     i 实参下标 / Argument index
 *
 * @return 该位置的实参或 `undefined` / The argument at that position, or `undefined`
 */
static JsValue arg_at(JsValue *args, int argc, int i)
{
    return i < argc ? args[i] : js_undef();
}

/**
 * @brief 在目标对象上定义原生方法 / Define a native method on the target object
 *
 * @details 先用 `mk_native` 包装原生函数，再以 `name` 为键写入 `target`。
 *          The native function is wrapped with `mk_native` and written to `target` under `name`.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] target 目标对象 / Target object
 * @param[in]     name 方法名 / Method name
 * @param[in]     fn 原生函数指针 / Native function pointer
 * @param[in,out] ud 透传给宿主的用户数据 / Opaque user data passed through to the host
 *
 * @see mk_native def_value
 */
static void def_method(HpeJs *js, JsObj *target, const char *name, JsNativeFn fn, void *ud)
{
    js_obj_set(js, target, name, mk_native(js, fn, ud));
}

/**
 * @brief 在目标对象上定义取值 / Define a value on the target object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] target 目标对象 / Target object
 * @param[in]     name 属性名 / Property name
 * @param[in]     v 属性值 / Property value
 *
 * @see def_method
 */
static void def_value(HpeJs *js, JsObj *target, const char *name, JsValue v)
{
    js_obj_set(js, target, name, v);
}

/**
 * @brief 读取字符串实参，缺失时返回回退值 / Read a string argument, returning a fallback when absent
 *
 * @details 下标越界直接返回 `fallback`，否则以 `js_to_string` 转换该实参。
 *          An out-of-range index returns `fallback` directly; otherwise the argument is converted with `js_to_string`.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 实参个数 / Number of actual arguments
 * @param[in]     i 实参下标 / Argument index
 * @param[in]     fallback 缺失时的回退字符串 / The fallback string when absent
 *
 * @return 参数字符串或回退值 / The argument string or the fallback
 */
static const char *arg_cstr(HpeJs *js, JsValue *args, int argc, int i, const char *fallback)
{
    if (i >= argc)
        return fallback;
    return js_to_string(js, args[i]);
}

/**
 * @brief 把值数组打包成 JS 数组 / Pack a value array into a JS array
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] v 值数组 / Value array
 * @param[in]     n 元素个数 / Number of elements
 *
 * @return 新建的 JS 数组 / The newly created JS array
 *
 * @see array_of
 */
static JsValue list_of(HpeJs *js, JsValue *v, int n)
{
    return array_of(js, v, n);
}

/**
 * @brief 取得 `Array.prototype` 方法的接收者（须类数组）/ Get the receiver of an `Array.prototype` method (must be array-like)
 *
 * @details 接收者为对象时返回该对象；为字符串时返回 `NULL`（调用方另行处理）；其余返回 `NULL`。
 *          Returns the object when the receiver is an object, `NULL` when it is a string (handled separately by the caller), and `NULL` otherwise.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 *
 * @return 接收对象，或 `NULL` / The receiver object, or `NULL`
 */
static JsObj *this_like(HpeJs *js, JsValue thisv)
{
    if (thisv.t == JS_OBJ && thisv.obj)
        return thisv.obj;
    if (thisv.t == JS_STR)
        return NULL;
    HPE_UNUSED(js);
    return NULL;
}

/**
 * @brief 实现 `console.log` 等输出 / Implement `console.log` and friends
 *
 * @details 按 `ud` 选择输出流（非 `NULL` 写 `stderr`，否则 `stdout`），实参以空格分隔打印后补换行。
 *          The output stream is chosen by `ud` (`stderr` when non-`NULL`, `stdout` otherwise); arguments are printed space-separated followed by a newline.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 恒为 `undefined` / Always `undefined`
 *
 * @note `this` 未被使用 / `this` is unused
 */

static JsValue native_console(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    FILE *out = ud ? stderr : stdout;
    int i;
    HPE_UNUSED(thisv);
    for (i = 0; i < argc; i++) {
        char *s = js_to_string(js, args[i]);
        if (i)
            fputc(' ', out);
        fputs(s ? s : "", out);
    }
    fputc('\n', out);
    return js_undef();
}

/**
 * @brief 实现 `alert`（委托给 console 输出）/ Implement `alert` (delegates to the console output)
 *
 * @details 以 `ud` 为 `NULL` 调用 `native_console`，即写入 `stdout`。
 *          Calls `native_console` with a `NULL` `ud`, i.e. writing to `stdout`.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 恒为 `undefined` / Always `undefined`
 *
 * @see native_console
 */
static JsValue native_alert(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    return native_console(js, thisv, args, argc, NULL);
}

/* ---------------- Math ---------------- */

#define MATH1(name, expr)                                                                      \
    static JsValue name(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)           \
    {                                                                                          \
        double x = js_to_number(js, arg_at(args, argc, 0));                                     \
        HPE_UNUSED(thisv);                                                                     \
        HPE_UNUSED(ud);                                                                        \
        return js_num(expr);                                                                   \
    }

MATH1(math_abs, fabs(x))
MATH1(math_ceil, ceil(x))
MATH1(math_floor, floor(x))
MATH1(math_trunc, trunc(x))
MATH1(math_sqrt, x < 0 ? NAN : sqrt(x))
MATH1(math_cbrt, cbrt(x))
MATH1(math_exp, exp(x))
MATH1(math_log, log(x))
MATH1(math_log2, log2(x))
MATH1(math_log10, log10(x))
MATH1(math_sin, sin(x))
MATH1(math_cos, cos(x))
MATH1(math_tan, tan(x))
MATH1(math_asin, asin(x))
MATH1(math_acos, acos(x))
MATH1(math_atan, atan(x))
MATH1(math_sinh, sinh(x))
MATH1(math_cosh, cosh(x))
MATH1(math_tanh, tanh(x))
MATH1(math_sign, x != x ? NAN : (x > 0 ? 1.0 : (x < 0 ? -1.0 : x)))

/**
 * @brief 实现 `Math.round` / Implement `Math.round`
 *
 * @details 按 JS 语义把 `.5` 向 +∞ 取整；`NaN` 与 `±Infinity` 原样返回。
 *          Rounds `.5` towards +∞ per JS semantics; `NaN` and `±Infinity` are returned unchanged.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 取整后的数值 / The rounded number
 */
static JsValue math_round(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    double x = js_to_number(js, arg_at(args, argc, 0));
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    /* JS rounds .5 towards +infinity */
    return js_num(x != x || isinf(x) ? x : floor(x + 0.5));
}

/**
 * @brief 实现 `Math.pow`（底数、指数）/ Implement `Math.pow` (base and exponent)
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 幂运算结果 / The power result
 */
static JsValue math_pow(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    return js_num(pow(js_to_number(js, arg_at(args, argc, 0)),
                      js_to_number(js, arg_at(args, argc, 1))));
}

/**
 * @brief 实现 `Math.atan2`（y, x）/ Implement `Math.atan2` (y, x)
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 反正切角度（弧度）/ The arc-tangent angle in radians
 */
static JsValue math_atan2(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    return js_num(atan2(js_to_number(js, arg_at(args, argc, 0)),
                        js_to_number(js, arg_at(args, argc, 1))));
}

/**
 * @brief 实现 `Math.hypot`（各实参平方和的平方根）/ Implement `Math.hypot` (square root of the sum of squared arguments)
 *
 * @details 逐个实参转数值后累加其平方，最后开方。
 *          Each argument is converted to a number, its square accumulated, and the square root taken at the end.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 欧氏范数 / The Euclidean norm
 */
static JsValue math_hypot(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    double acc = 0.0;
    int i;
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    for (i = 0; i < argc; i++) {
        double x = js_to_number(js, args[i]);
        acc += x * x;
    }
    return js_num(sqrt(acc));
}

/**
 * @brief 实现 `Math.max` / `Math.min` / Implement `Math.max` / `Math.min`
 *
 * @details `ud` 非 `NULL` 时为取最大（初值 -∞），否则取最小（初值 +∞）；任一实参为 `NaN` 立即返回 `NaN`。
 *          A non-`NULL` `ud` selects the maximum (initial -∞), otherwise the minimum (initial +∞); any `NaN` argument returns `NaN` immediately.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 非 `NULL` 取最大、`NULL` 取最小 / Non-`NULL` for maximum, `NULL` for minimum
 *
 * @return 极值 / The extreme value
 */
static JsValue math_minmax(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    double best = ud ? -INFINITY : INFINITY;
    int i;
    HPE_UNUSED(thisv);
    for (i = 0; i < argc; i++) {
        double x = js_to_number(js, args[i]);
        if (x != x)
            return js_num(NAN);
        best = ud ? (x > best ? x : best) : (x < best ? x : best);
    }
    return js_num(best);
}

/**
 * @brief 实现 `Math.random`（xorshift64 伪随机）/ Implement `Math.random` (xorshift64 pseudo-random)
 *
 * @details 首次调用以挂钟毫秒种下静态状态（或 1 保证非零），随后做 xorshift64 并用高 53 位映射到 `[0,1)`。
 *          The first call seeds the static state from epoch milliseconds (OR-ed with 1 to stay non-zero); a xorshift64 step then maps its top 53 bits into `[0,1)`.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return `[0, 1)` 区间内的随机数 / A random number in `[0, 1)`
 *
 * @note 不用全局 `rand()`，避免干扰其他调用方；所有入参均被忽略 / Uses no global `rand()` so other callers are undisturbed; all inputs are ignored
 */
static JsValue math_random(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    static unsigned long long state = 0;
    HPE_UNUSED(js);
    HPE_UNUSED(thisv);
    HPE_UNUSED(args);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    if (state == 0) {
        /* seed once from the wall clock so successive runs differ */
        state = (unsigned long long)hpe_time_epoch_ms() | 1ULL;
    }
    /* xorshift64: cheap, no global rand() state to disturb other callers */
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return js_num((double)(state >> 11) / 9007199254740992.0); /* 53 random bits */
}

/**
 * @brief 实现 `Math.clz32`（32 位前导零计数）/ Implement `Math.clz32` (32-bit leading-zero count)
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 前导零个数；入参为 0 时返回 32 / The leading-zero count; 32 when the argument is 0
 */
static JsValue math_clz32(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    uint32_t x = (uint32_t)js_to_number(js, arg_at(a, argc, 0));
    int n = 0;
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    if (x == 0)
        return js_num(32);
    while (!(x & 0x80000000u)) {
        x <<= 1;
        n++;
    }
    return js_num((double)n);
}

/**
 * @brief 实现 `Math.imul`（32 位有符号整数乘法）/ Implement `Math.imul` (32-bit signed integer multiplication)
 *
 * @details 把两个实参先截为 32 位无符号相乘，再按 32 位有符号解释结果。
 *          Both arguments are truncated to 32-bit unsigned values for the multiply, and the product is reinterpreted as 32-bit signed.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 32 位有符号乘积 / The 32-bit signed product
 */
static JsValue math_imul(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    int32_t x = (int32_t)(uint32_t)js_to_number(js, arg_at(a, argc, 0));
    int32_t y = (int32_t)(uint32_t)js_to_number(js, arg_at(a, argc, 1));
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    return js_num((double)(int32_t)((uint32_t)x * (uint32_t)y));
}

/**
 * @brief 实现 `Math.fround`（舍入为单精度）/ Implement `Math.fround` (round to single precision)
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 单精度舍入后的数值 / The value rounded to single precision
 */
static JsValue math_fround(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    float f = (float)js_to_number(js, arg_at(a, argc, 0));
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    return js_num((double)f);
}

/* the hyperbolic/inverse/log1p family: one implementation per name, dispatched
 * by the `ud` selector so the install table stays compact */
enum { MF_LOG1P, MF_EXPM1, MF_ACOSH, MF_ASINH, MF_ATANH };

/**
 * @brief `log1p`/`expm1`/`acosh`/`asinh`/`atanh` 共用实现 / Shared implementation for `log1p`/`expm1`/`acosh`/`asinh`/`atanh`
 *
 * @details 由 `ud` 选择算法，对第 0 个实参求值；`log1p` 对零保留符号（±0）。
 *          The algorithm is selected by `ud` and applied to argument 0; `log1p` preserves the sign of zero (±0).
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 算法选择子 / The algorithm selector
 *
 * @return 所选函数的结果 / The result of the selected function
 *
 * @note 选择子无法识别时返回 `NaN` / Returns `NaN` for an unrecognised selector
 */
static JsValue math_extra(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    double x = js_to_number(js, arg_at(a, argc, 0));
    int which = (int)(intptr_t)ud;
    HPE_UNUSED(thisv);
    switch (which) {
    case MF_LOG1P:
        return js_num(x == 0.0 ? (x < 0 ? -0.0 : 0.0) : log1p(x));
    case MF_EXPM1:
        return js_num(expm1(x));
    case MF_ACOSH:
        return js_num(acosh(x));
    case MF_ASINH:
        return js_num(asinh(x));
    case MF_ATANH:
        return js_num(atanh(x));
    default:
        return js_num(NAN);
    }
}

/**
 * @brief 把字符转换为进制数字值 / Convert a character to its radix digit value
 *
 * @details `0`-`9`、`a`-`z`、`A`-`Z` 分别映射为 0-35 的对应值。
 *          `0`-`9`, `a`-`z` and `A`-`Z` map to their corresponding values in 0-35.
 *
 * @param[in]     c 字符 / Character
 *
 * @return 数字值；非法字符返回 -1 / The digit value; -1 for a non-digit
 *
 * @see native_parse_int uri_hex
 */

static int radix_digit(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z')
        return c - 'A' + 10;
    return -1;
}

/**
 * @brief 实现全局 `parseInt` / Implement the global `parseInt`
 *
 * @details 跳过前导空白与正负号；进制为 0 或 16 且带 `0x`/`0X` 前缀时按 16 处理；进制为 0 时默认 10；进制不在 2..36 返回 `NaN`；按进制累加直至首个非法字符，无任何数字则返回 `NaN`。
 *          Leading whitespace and a sign are skipped; a `0x`/`0X` prefix with radix 0 or 16 is treated as base 16; a radix of 0 defaults to 10; a radix outside 2..36 yields `NaN`; digits are accumulated until the first invalid character, and `NaN` is returned when no digit was seen.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 解析出的整数（可能为负）或 `NaN` / The parsed integer (possibly negative), or `NaN`
 */
static JsValue native_parse_int(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    char *s = js_to_string(js, arg_at(args, argc, 0));
    int radix = argc > 1 ? (int)js_to_number(js, args[1]) : 0;
    const char *p = s ? s : "";
    int sign = 1, any = 0;
    double acc = 0.0;
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    if (*p == '+' || *p == '-') {
        sign = (*p == '-') ? -1 : 1;
        p++;
    }
    if ((radix == 0 || radix == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        radix = 16;
        p += 2;
    }
    if (radix == 0)
        radix = 10;
    if (radix < 2 || radix > 36)
        return js_num(NAN);
    for (; *p; p++) {
        int d = radix_digit((unsigned char)*p);
        if (d < 0 || d >= radix)
            break;
        acc = acc * radix + d;
        any = 1;
    }
    return js_num(any ? acc * sign : NAN);
}

/**
 * @brief 实现全局 `parseFloat` / Implement the global `parseFloat`
 *
 * @details 跳过前导空白；识别 `Infinity`/`+Infinity`/`-Infinity` 与 `0x` 十六进制形式，否则用 `strtod` 解析，未消费任何字符则返回 `NaN`。
 *          Leading whitespace is skipped; `Infinity`/`+Infinity`/`-Infinity` and `0x` hexadecimal forms are recognised, otherwise `strtod` parses the number and `NaN` is returned when nothing was consumed.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 解析出的浮点数或 `NaN` / The parsed floating-point number, or `NaN`
 */
static JsValue native_parse_float(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    char *s = js_to_string(js, arg_at(args, argc, 0));
    const char *p = s ? s : "";
    char *end = NULL;
    double d;
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    if (!strncmp(p, "Infinity", 8) || !strncmp(p, "+Infinity", 9))
        return js_num(INFINITY);
    if (!strncmp(p, "-Infinity", 9))
        return js_num(-INFINITY);
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
        return js_num((double)(long long)strtoull(p, NULL, 16));
    d = strtod(p, &end);
    if (end == p)
        return js_num(NAN);
    return js_num(d);
}

/**
 * @brief 实现全局 `isNaN` / Implement the global `isNaN`
 *
 * @details 把第 0 个实参转为数值后判断其是否等于自身。
 *          Argument 0 is converted to a number and tested for self-inequality.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 为 `NaN` 返回 `true`，否则 `false` / `true` for `NaN`, otherwise `false`
 */
static JsValue native_is_nan(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    double d;
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    d = js_to_number(js, arg_at(args, argc, 0));
    return js_bool(d != d);
}

/**
 * @brief 实现全局 `isFinite` / Implement the global `isFinite`
 *
 * @details 转为数值后判断其既非 `NaN` 也非无穷。
 *          After conversion the number is tested for being neither `NaN` nor infinite.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 有限返回 `true`，否则 `false` / `true` for a finite number, otherwise `false`
 */
static JsValue native_is_finite(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    double d;
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    d = js_to_number(js, arg_at(args, argc, 0));
    return js_bool(d == d && !isinf(d));
}

/* ---------------- URI helpers ---------------- */

static const char *uri_unreserved = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.!~*'()";

/**
 * @brief 实现 `encodeURIComponent` / Implement `encodeURIComponent`
 *
 * @details 逐字节处理：未保留字符原样输出，其余编码为 `%XX`；当 `ud` 非 `NULL`（`encodeURI` 模式）时另将 `;/?:@&=+$,#` 与 `!` 视作可保留。
 *          Bytes are processed one by one: unreserved characters pass through and the rest become `%XX`; when `ud` is non-`NULL` (`encodeURI` mode) `;/?:@&=+$,#` and `!` are also treated as unreserved.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 非 `NULL` 采用 `encodeURI` 的保留集 / Non-`NULL` selects the `encodeURI` reserved set
 *
 * @return 百分号编码后的字符串 / The percent-encoded string
 */
static JsValue native_encode_uri_component(HpeJs *js, JsValue thisv, JsValue *args, int argc,
                                           void *ud)
{
    char *s = js_to_string(js, arg_at(args, argc, 0));
    const unsigned char *p = (const unsigned char *)(s ? s : "");
    size_t cap = strlen((const char *)p) * 3 + 1;
    char *out = (char *)hpe_js_alloc(js, cap);
    size_t o = 0;
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    for (; *p; p++) {
        if (strchr(uri_unreserved, *p) || (ud && (strchr(";/?:@&=+$,#", *p) || *p == '!')))
            out[o++] = (char)*p;
        else
            o += (size_t)snprintf(out + o, cap - o, "%%%02X", *p);
    }
    out[o] = '\0';
    return js_string_len(js, out, o);
}

/**
 * @brief 把十六进制字符转换为数值 / Convert a hexadecimal character to its value
 *
 * @details `0`-`9`、`a`-`f`、`A`-`F` 分别映射为 0-15。
 *          `0`-`9`, `a`-`f` and `A`-`F` map to 0-15.
 *
 * @param[in]     c 字符 / Character
 *
 * @return 十六进制数值；非法字符返回 -1 / The hexadecimal value; -1 for a non-hex character
 *
 * @see native_decode_uri_component radix_digit
 */
static int uri_hex(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/**
 * @brief 实现全局 `decodeURIComponent`：把 `%XX` 还原为字节 / Implement global `decodeURIComponent`: turn `%XX` escapes back into bytes
 *
 * @details 逐字节扫描输入串，遇到合法的 `%` 加两位十六进制时解码为单个字节，否则原样拷贝。解码结果按字节直接构造字符串，因此多字节 UTF-8 序列会被还原成原始字符。
 *          The input is scanned byte by byte; a valid `%` followed by two hex digits is decoded to one byte, anything else is copied verbatim. The result is built as a byte string, so multi-byte UTF-8 sequences are reassembled.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 解码后的字符串；输入无法转换时按空串处理 / The decoded string; treated as empty when the input cannot be converted
 */
static JsValue native_decode_uri_component(HpeJs *js, JsValue thisv, JsValue *args, int argc,
                                           void *ud)
{
    char *s = js_to_string(js, arg_at(args, argc, 0));
    const char *p = s ? s : "";
    size_t cap = strlen(p) + 1;
    char *out = (char *)hpe_js_alloc(js, cap);
    size_t o = 0;
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    for (; *p; p++) {
        if (*p == '%' && p[1] && p[2]) {
            int h = uri_hex((unsigned char)p[1]), l = uri_hex((unsigned char)p[2]);
            if (h >= 0 && l >= 0) {
                out[o++] = (char)(h * 16 + l);
                p += 2;
                continue;
            }
        }
        out[o++] = *p;
    }
    out[o] = '\0';
    return js_string_len(js, out, o);
}

/**
 * @brief 把 `thisv` 当作可读写的普通对象取出 / Take `thisv` as a plain mutable object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 *
 * @return 普通对象指针；类型不是带实体的普通对象时返回 `NULL` / The plain object pointer, or `NULL` when it is not a plain object with storage
 */

static JsObj *str_this(HpeJs *js, JsValue thisv)
{
    HPE_UNUSED(js);
    if (thisv.t == JS_OBJ && thisv.obj && thisv.obj->kind == JS_OBJ_PLAIN && thisv.obj->items)
        return thisv.obj;
    HPE_UNUSED(thisv);
    return NULL;
}

/**
 * @brief 把参数强制转换为字符串 / Coerce an argument to a string
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     a 待转换的实参 / Argument to convert
 *
 * @return 转换后的以 NUL 结尾的字符串，由 arena 持有 / The converted NUL-terminated string, owned by the arena
 */
static const char *str_arg(HpeJs *js, JsValue a)
{
    return js_to_string(js, a);
}

/**
 * @brief 构造字符串值，空指针按空串处理 / Build a string value, treating a null pointer as the empty string
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     s 源字符串，可为 `NULL` / Source string, may be `NULL`
 *
 * @return 指向该文本的 JS 字符串值 / A JS string value wrapping the text
 */
static JsValue strv(HpeJs *js, const char *s)
{
    return js_string(js, s ? s : "");
}

/**
 * @brief 实现 `String.prototype.charAt`：按码点下标取单字符 / Implement `String.prototype.charAt`: read one character at a code-point index
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 该下标的单字符字符串；下标越界时为空串 / The single-character string at that index, or the empty string when out of range
 */
static JsValue string_charAt(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    char *s = js_to_string(js, thisv);
    int i = (int)js_to_number(js, arg_at(a, argc, 0));
    HPE_UNUSED(ud);
    if (i < 0 || i >= utf8_len(s))
        return strv(js, "");
    return utf8_char_at(js, s, i);
}

/**
 * @brief 实现 `String.prototype.charCodeAt`：按码点下标取码点值 / Implement `String.prototype.charCodeAt`: read the code point at an index
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 该下标的 Unicode 码点；下标越界时返回 `NaN` / The Unicode code point at that index, or `NaN` when out of range
 */
static JsValue string_charCodeAt(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    char *s = js_to_string(js, thisv);
    int i = (int)js_to_number(js, arg_at(a, argc, 0));
    const char *p = s;
    int k = 0;
    HPE_UNUSED(ud);
    while (k < i && *p) {
        uint32_t cp;
        p += hpe_utf8_next(p, &cp);
        k++;
    }
    if (k != i || !*p)
        return js_num(NAN);
    {
        uint32_t cp;
        hpe_utf8_next(p, &cp);
        return js_num((double)cp);
    }
}

/**
 * @brief 实现 `String.prototype.indexOf`：从前往后查找子串 / Implement `String.prototype.indexOf`: find a substring searching forward
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 命中处的码点下标；未命中或起点越界时返回 -1 / The code-point index of the match, or -1 when absent or the start is out of range
 */
static JsValue string_indexOf(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    char *s = str_arg(js, thisv);
    char *needle = str_arg(js, arg_at(a, argc, 0));
    char *hit;
    HPE_UNUSED(ud);
    if (argc > 1) {
        int from = (int)js_to_number(js, a[1]);
        if (from < 0)
            from = 0;
        if (from > utf8_len(s))
            return js_num(-1);
        {
            const char *p = s;
            int k = 0;
            while (k < from && *p) {
                uint32_t cp;
                p += hpe_utf8_next(p, &cp);
                k++;
            }
            hit = strstr(p, needle);
            if (!hit)
                return js_num(-1);
            return js_num((double)(from + (int)utf8_strlen_n(p, (size_t)(hit - p))));
        }
    }
    hit = strstr(s, needle);
    if (!hit)
        return js_num(-1);
    return js_num((double)utf8_strlen_n(s, (size_t)(hit - s)));
}

/**
 * @brief 实现 `String.prototype.lastIndexOf`：从后往前查找子串 / Implement `String.prototype.lastIndexOf`: find a substring searching backward
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 最后一次命中的码点下标；未命中时返回 -1；空子串返回字符串长度 / The code-point index of the last match, -1 when absent, or the length for an empty needle
 */
static JsValue string_lastIndexOf(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    char *s = str_arg(js, thisv);
    char *needle = str_arg(js, arg_at(a, argc, 0));
    size_t nl = strlen(needle), sl = strlen(s), i;
    HPE_UNUSED(ud);
    HPE_UNUSED(argc);
    if (nl == 0)
        return js_num((double)utf8_strlen_n(s, sl));
    if (nl > sl)
        return js_num(-1);
    for (i = sl - nl + 1; i-- > 0;) {
        if (!strncmp(s + i, needle, nl))
            return js_num((double)utf8_strlen_n(s, i));
    }
    return js_num(-1);
}

/**
 * @brief 求第 `i` 个码点对应的字节偏移 / Byte offset of the `i`-th code point
 *
 * @param[in]     s 以 NUL 结尾的字符串 / NUL-terminated string
 * @param[in]     i 码点下标 / Code-point index
 *
 * @return 该码点在串中的字节偏移；`i` 超过长度时返回字符串的字节长度 / The byte offset of that code point, or the byte length when `i` runs past the end
 */
static size_t utf8_offset(const char *s, int i)
{
    const char *p = s;
    int k = 0;
    while (k < i && *p) {
        uint32_t cp;
        p += hpe_utf8_next(p, &cp);
        k++;
    }
    return (size_t)(p - s);
}

/**
 * @brief 实现 `String.prototype.slice`：按可负的下标区间取子串 / Implement `String.prototype.slice`: extract a substring from a possibly negative range
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 区间内的子串 / The substring within the range
 */
static JsValue string_slice(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    char *s = str_arg(js, thisv);
    int len = utf8_len(s);
    int start = argc > 0 ? (int)js_to_number(js, a[0]) : 0;
    int end = argc > 1 && a[1].t != JS_UNDEF ? (int)js_to_number(js, a[1]) : len;
    size_t b0, b1;
    HPE_UNUSED(ud);
    if (start < 0)
        start += len;
    if (end < 0)
        end += len;
    if (start < 0)
        start = 0;
    if (end > len)
        end = len;
    if (end < start)
        end = start;
    b0 = utf8_offset(s, start);
    b1 = utf8_offset(s, end);
    return js_string_len(js, s + b0, b1 - b0);
}

/**
 * @brief 实现 `String.prototype.substring`：取 `[start, end)` 子串并把负值夹到 0 / Implement `String.prototype.substring`: take the `[start, end)` substring clamping negatives to 0
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @note `start` 大于 `end` 时两者自动交换 / `start` and `end` are swapped when `start` exceeds `end`
 */
static JsValue string_substring(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    char *s = str_arg(js, thisv);
    int len = utf8_len(s);
    int start = argc > 0 ? (int)js_to_number(js, a[0]) : 0;
    int end = argc > 1 && a[1].t != JS_UNDEF ? (int)js_to_number(js, a[1]) : len;
    size_t b0, b1;
    HPE_UNUSED(ud);
    if (start < 0)
        start = 0;
    if (end < 0)
        end = 0;
    if (start > len)
        start = len;
    if (end > len)
        end = len;
    if (start > end) {
        int t = start;
        start = end;
        end = t;
    }
    b0 = utf8_offset(s, start);
    b1 = utf8_offset(s, end);
    return js_string_len(js, s + b0, b1 - b0);
}

/**
 * @brief 实现 `String.prototype.substr`：从起点取指定长度 / Implement `String.prototype.substr`: take a given length from a start offset
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 从 `start` 起长度至多 `count` 的子串 / The substring of at most `count` characters starting at `start`
 */
static JsValue string_substr(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    char *s = str_arg(js, thisv);
    int len = utf8_len(s);
    int start = argc > 0 ? (int)js_to_number(js, a[0]) : 0;
    int count = argc > 1 && a[1].t != JS_UNDEF ? (int)js_to_number(js, a[1]) : len;
    size_t b0, b1;
    HPE_UNUSED(ud);
    if (start < 0)
        start = len + start > 0 ? len + start : 0;
    if (start > len)
        start = len;
    if (count < 0)
        count = 0;
    if (start + count > len)
        count = len - start;
    b0 = utf8_offset(s, start);
    b1 = utf8_offset(s, start + count);
    return js_string_len(js, s + b0, b1 - b0);
}

/**
 * @brief 实现 `String.prototype.toUpperCase`：仅对 ASCII 字母转大写 / Implement `String.prototype.toUpperCase`: uppercase ASCII letters only
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 转换后的新字符串 / The converted new string
 */
static JsValue string_toUpperCase(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    char *s = str_arg(js, thisv);
    char *out = hpe_js_strdup(js, s);
    size_t i;
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    for (i = 0; out[i]; i++)
        if (out[i] >= 'a' && out[i] <= 'z')
            out[i] = (char)(out[i] - 'a' + 'A');
    return js_string(js, out);
}

/**
 * @brief 实现 `String.prototype.toLowerCase`：仅对 ASCII 字母转小写 / Implement `String.prototype.toLowerCase`: lowercase ASCII letters only
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 转换后的新字符串 / The converted new string
 */
static JsValue string_toLowerCase(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    char *s = str_arg(js, thisv);
    char *out = hpe_js_strdup(js, s);
    size_t i;
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    for (i = 0; out[i]; i++)
        if (out[i] >= 'A' && out[i] <= 'Z')
            out[i] = (char)(out[i] - 'A' + 'a');
    return js_string(js, out);
}

/**
 * @brief 实现 `String.prototype.trim`：去掉两端的 ASCII 空白 / Implement `String.prototype.trim`: strip ASCII whitespace from both ends
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 去空白后的子串 / The trimmed substring
 */
static JsValue string_trim(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    char *s = str_arg(js, thisv);
    const char *b = s, *e = s + strlen(s);
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    while (*b && (*b == ' ' || *b == '\t' || *b == '\n' || *b == '\r' || *b == '\f' || *b == '\v'))
        b++;
    while (e > b && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r' ||
                     e[-1] == '\f' || e[-1] == '\v'))
        e--;
    return js_string_len(js, b, (size_t)(e - b));
}

/**
 * @brief 实现 `padStart`/`padEnd`：用填充串把字符串补到目标长度 / Implement `padStart`/`padEnd`: pad a string to a target length with a fill string
 *
 * @details 目标长度按码点计。`ud` 非空时往左侧填充（padStart），为空时往右侧填充（padEnd）；需要补的字符数按填充串字节数循环取用。
 *          The target length is counted in code points. A non-null `ud` pads on the left (padStart), null pads on the right (padEnd); the missing characters are taken cyclically from the fill string.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 补长后的字符串；无需补长时原样返回 / The padded string, returned unchanged when no padding is needed
 */
static JsValue string_pad(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    char *s = str_arg(js, thisv);
    int target = argc > 0 ? (int)js_to_number(js, a[0]) : 0;
    char *pad = argc > 1 ? js_to_string(js, a[1]) : " ";
    int cur = utf8_len(s);
    int fill, plen, i, o = 0;
    char *out;
    HPE_UNUSED(ud);
    if (target <= cur || !pad || !*pad)
        return js_string(js, s);
    fill = target - cur;
    plen = (int)strlen(pad);
    out = (char *)hpe_js_alloc(js, (size_t)cur * 4 + (size_t)fill * (size_t)plen + 8);
    if (ud) { /* padStart */
        for (i = 0; o < fill * plen; i = (i + 1) % plen)
            out[o++] = pad[i];
    }
    memcpy(out + o, s, strlen(s));
    o += (int)strlen(s);
    if (!ud) { /* padEnd */
        for (i = 0; o < fill * plen + (int)strlen(s); i = (i + 1) % plen)
            out[o++] = pad[i];
    }
    out[o] = '\0';
    return js_string_len(js, out, (size_t)o);
}

/**
 * @brief 实现 `String.prototype.repeat`：把字符串重复若干次 / Implement `String.prototype.repeat`: repeat the string a given number of times
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 重复后的新字符串 / The repeated new string
 *
 * @warning `count` 为负时抛出 `Invalid count value` / Throws `Invalid count value` when the count is negative
 */
static JsValue string_repeat(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    char *s = str_arg(js, thisv);
    int n = argc > 0 ? (int)js_to_number(js, a[0]) : 0;
    size_t sl = strlen(s);
    char *out;
    int i;
    HPE_UNUSED(ud);
    if (n < 0)
        js_error(js, 0, "Invalid count value");
    out = (char *)hpe_js_alloc(js, sl * (size_t)n + 1);
    for (i = 0; i < n; i++)
        memcpy(out + (size_t)i * sl, s, sl);
    out[sl * (size_t)n] = '\0';
    return js_string_len(js, out, sl * (size_t)n);
}

/**
 * @brief 只裁剪字符串的一端：`ud` 选择裁右端还是左端 / Trim only one end of the string: `ud` selects right end versus left end
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 非空裁右端（trimEnd），为空裁左端（trimStart）/ Non-null trims the right end (trimEnd), null the left end (trimStart)
 *
 * @return 裁剪后的子串 / The trimmed substring
 */
static JsValue string_trim_side(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    char *s = str_arg(js, thisv);
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    if (ud) {
        size_t n = strlen(s);
        while (n > 0 && hpe_is_space(s[n - 1]))
            n--;
        return js_string_len(js, s, n);
    } else {
        size_t i = 0;
        while (s[i] && hpe_is_space(s[i]))
            i++;
        return js_string(js, s + i);
    }
}

/* replaces every occurrence of a substring (or of a global regex) */
/**
 * @brief 确保缓冲区还能再放下 `need` 字节，不够则翻倍扩容 / Ensure the buffer holds `need` more bytes, doubling its capacity when short
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] out 当前输出缓冲区，扩容时会被替换 / Current output buffer, replaced when grown
 * @param[in]     o 已写入的字节数 / Number of bytes already written
 * @param[in,out] cap 缓冲区容量，扩容后写回新值 / Buffer capacity, updated in place when grown
 * @param[in]     need 本次还需要写入的字节数 / Number of additional bytes required
 *
 * @return 可能已扩容的缓冲区指针 / The buffer pointer, possibly after growth
 *
 * @note 容量足够时原样返回，不做任何分配 / Returns the buffer untouched when capacity suffices
 */
static char *repl_grow(HpeJs *js, char *out, size_t o, size_t *cap, size_t need)
{
    if (o + need + 1 <= *cap)
        return out;
    {
        size_t ncap = *cap * 2;
        char *bigger;

        while (ncap < o + need + 1)
            ncap *= 2;
        bigger = (char *)hpe_js_alloc(js, ncap);
        memcpy(bigger, out, o);
        out = bigger;
        *cap = ncap;
    }
    return out;
}

/**
 * @brief 把替换参数取成字符串；若为函数则先回调求值 / Resolve a replacement argument to a string, calling it first when it is a function
 *
 * @details 替换值为可调用对象时，以 `(match, offset, input)` 三个实参调用，并把返回值转成字符串；否则直接转字符串。
 *          When the replacement is callable it is invoked with `(match, offset, input)` and the result is stringified; otherwise the value itself is stringified.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     rep 替换值 / Replacement value
 * @param[in]     n 节点 / Node
 * @param[in]     s 以 NUL 结尾的字符串 / NUL-terminated string
 * @param[in]     hit 命中的节点 / Hit node
 *
 * @return 替换文本 / The replacement text
 */
static char *repl_value(HpeJs *js, JsValue rep, const char *n, const char *s, const char *hit)
{
    if (js_is_callable(rep)) {
        JsValue args[3];

        args[0] = js_string(js, n);
        args[1] = js_num((double)(hit - s));
        args[2] = js_string(js, s);
        return js_to_string(js, call_value(js, NULL, rep, js_undef(), args, 3, 0));
    }
    return js_to_string(js, rep);
}

/**
 * @brief 空查找串：在每两个字符之间插入替换文本 / Empty search string: insert the replacement between every pair of characters
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     s 以 NUL 结尾的字符串 / NUL-terminated string
 * @param[in]     rep 替换值 / Replacement value
 *
 * @return 插入后的新字符串 / The resulting new string
 */
static JsValue replaceall_empty(HpeJs *js, const char *s, JsValue rep)
{
    char *rs = js_to_string(js, rep);
    size_t rl = strlen(rs), sl = strlen(s), o;
    char *out = (char *)hpe_js_alloc(js, (sl + 1) * (rl + 1) + 1);
    const char *p;

    memcpy(out, rs, rl);
    o = rl;
    for (p = s; *p; p++) {
        out[o++] = *p;
        memcpy(out + o, rs, rl);
        o += rl;
    }
    out[o] = '\0';
    return js_string_len(js, out, o);
}

/**
 * @brief 逐处替换非空子串的所有出现 / Replace every occurrence of a non-empty substring
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     s 源文本 / Source text
 * @param[in]     n 非空的查找子串 / Non-empty search substring
 * @param[in]     rep 替换值 / Replacement value
 *
 * @return 替换后的新字符串 / The resulting new string
 *
 * @note 输出缓冲区按需经 `repl_grow` 翻倍扩容 / The output buffer is grown on demand through `repl_grow`
 */
static JsValue replaceall_plain(HpeJs *js, const char *s, const char *n, JsValue rep)
{
    size_t nl = strlen(n), sl = strlen(s), o = 0, cap = sl + (sl / nl + 1) * 8 + 1;
    char *out = (char *)hpe_js_alloc(js, cap);
    const char *p = s;

    for (;;) {
        const char *hit = strstr(p, n);
        char *rs;
        size_t seg, rslen;

        if (!hit) {
            seg = strlen(p);
            memcpy(out + o, p, seg);
            o += seg;
            break;
        }
        out = repl_grow(js, out, o, &cap, (size_t)(hit - p));
        memcpy(out + o, p, (size_t)(hit - p));
        o += (size_t)(hit - p);
        rs = repl_value(js, rep, n, s, hit);
        rslen = strlen(rs);
        out = repl_grow(js, out, o, &cap, rslen);
        memcpy(out + o, rs, rslen);
        o += rslen;
        p = hit + nl;
    }
    out[o] = '\0';
    return js_string_len(js, out, o);
}

/**
 * @brief 实现 `String.prototype.replaceAll`：替换全部匹配 / Implement `String.prototype.replaceAll`: replace all matches
 *
 * @details 查找值为正则时复用 `string.replace` 的全局路径；空查找串走逐字符插入；否则逐处替换。
 *          A regex search string reuses the global path of `string.replace`; an empty needle inserts between characters; otherwise every occurrence is replaced.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 替换后的新字符串 / The resulting new string
 */
static JsValue string_replaceAll(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    char *s = js_to_string(js, thisv);
    JsValue needle = arg_at(a, argc, 0);
    JsValue rep = arg_at(a, argc, 1);
    char *n;

    HPE_UNUSED(ud);
    /* a regex needle reuses string.replace, whose global path already loops */
    if (needle.t == JS_OBJ && needle.obj && needle.obj->kind == JS_OBJ_REGEX)
        return string_replace(js, thisv, a, argc, NULL);
    n = js_to_string(js, needle);
    if (*n == '\0')
        return replaceall_empty(js, s, rep);
    return replaceall_plain(js, s, n, rep);
}

/**
 * @brief 实现 `String.prototype.codePointAt`：取下标处码点的完整值 / Implement `String.prototype.codePointAt`: read the full code point at an index
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 该下标的完整 Unicode 码点；下标越界时返回 `undefined` / The full Unicode code point at that index, or `undefined` when out of range
 */
static JsValue string_codePointAt(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    char *s = js_to_string(js, thisv);
    int i = argc > 0 ? (int)js_to_number(js, a[0]) : 0;
    const char *p = s;
    int k = 0;
    uint32_t cp;
    HPE_UNUSED(ud);
    while (k < i && *p)
        p += hpe_utf8_next(p, &cp), k++;
    if (!*p)
        return js_undef();
    hpe_utf8_next(p, &cp);
    return js_num((double)cp);
}

/**
 * @brief 实现 `String.prototype.localeCompare`：按码元比较返回 -1/0/1 / Implement `String.prototype.localeCompare`: compare code units returning -1/0/1
 *
 * @details 引擎没有本地化排序表，因此退化为普通 `strcmp` 比较。
 *          The engine has no collation tables, so this degrades to a plain `strcmp` comparison.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 小于返回 -1、相等返回 0、大于返回 1 / -1 when smaller, 0 when equal, 1 when larger
 */
static JsValue string_localeCompare(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    char *s = js_to_string(js, thisv);
    char *o = js_to_string(js, arg_at(a, argc, 0));
    int c = strcmp(s, o);
    HPE_UNUSED(ud);
    return js_num(c < 0 ? -1.0 : c > 0 ? 1.0 : 0.0);
}

/**
 * @brief 实现 `String.prototype.matchAll`：收集全局正则的全部匹配 / Implement `String.prototype.matchAll`: collect all matches of a global regex
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 由各次匹配结果数组组成的数组 / An array of per-match result arrays
 */
static JsValue string_matchAll(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    char *s = js_to_string(js, thisv);
    JsValue needle = arg_at(a, argc, 0);
    JsObj *out = js_array_new(js);
    HPE_UNUSED(ud);
    if (needle.t == JS_OBJ && needle.obj && needle.obj->kind == JS_OBJ_REGEX) {
        HpeRegex *re = (HpeRegex *)needle.obj->payload;
        int total = hpe_regex_group_count(re);
        int off = 0;
        if (!re)
            return js_objval(out);
        for (;;) {
            int caps[RE_CAPS];
            int m, i;
            for (i = 0; i < RE_CAPS; i++)
                caps[i] = -1;
            m = hpe_regex_search(re, s, off, caps, RE_CAPS);
            if (m < 0)
                break;
            if (caps[1] < 0)
                caps[1] = m;
            array_push_value(js, out, regex_make_result(js, s, caps, total));
            off = caps[1] > caps[0] ? caps[1] : caps[0] + 1;
            if ((size_t)off > strlen(s))
                break;
        }
    }
    return js_objval(out);
}

/**
 * @brief 把 `thisv` 当作数组对象取出 / Take `thisv` as an array object
 *
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 *
 * @return 数组对象指针；`thisv` 不是对象时返回 `NULL` / The object pointer, or `NULL` when `thisv` is not an object
 */

static JsObj *arr_of(JsValue thisv)
{
    return (thisv.t == JS_OBJ && thisv.obj) ? thisv.obj : NULL;
}

/**
 * @brief 用分隔符把数组元素连接成字符串 / Join the elements of an array into a string using a separator
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] src 源数组对象 / Source array object
 * @param[in]     sep 元素之间的分隔符 / Separator inserted between elements
 *
 * @return 连接后的字符串值 / The joined string value
 *
 * @note `undefined` 与 `null` 元素按空串处理，输出缓冲按需翻倍扩容 / `undefined` and `null` elements become the empty string and the output buffer doubles on demand
 */
static JsValue array_join_impl(HpeJs *js, JsObj *src, const char *sep)
{
    char buf[8];
    size_t cap = 64, o = 0;
    char *out = (char *)hpe_js_alloc(js, cap);
    int i, len = array_length(js, src);
    size_t sepl = strlen(sep);
    for (i = 0; i < len; i++) {
        JsValue e = array_get_index(js, src, i);
        char *s = NULL;
        size_t sl;
        if (i)
            s = NULL;
        if (e.t != JS_UNDEF && e.t != JS_NULL)
            s = js_to_string(js, e);
        sl = s ? strlen(s) : 0;
        if (o + sl + sepl + 2 > cap) {
            while (o + sl + sepl + 2 > cap)
                cap *= 2;
            {
                char *nb = (char *)hpe_js_alloc(js, cap);
                memcpy(nb, out, o);
                out = nb;
            }
        }
        if (i && sepl) {
            memcpy(out + o, sep, sepl);
            o += sepl;
        }
        if (sl) {
            memcpy(out + o, s, sl);
            o += sl;
        }
    }
    out[o] = '\0';
    HPE_UNUSED(buf);
    return js_string_len(js, out, o);
}

/**
 * @brief 实现 `Array.prototype.join`：用分隔符连接元素 / Implement `Array.prototype.join`: join elements with a separator
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 连接后的字符串；`thisv` 非数组时为空串 / The joined string, or the empty string when `thisv` is not an array
 */
static JsValue array_join(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *src = arr_of(thisv);
    char *sep = argc > 0 && a[0].t != JS_UNDEF ? js_to_string(js, a[0]) : ",";
    HPE_UNUSED(ud);
    if (!src)
        return js_string(js, "");
    return array_join_impl(js, src, sep ? sep : ",");
}

/**
 * @brief 实现 `Array.prototype.push`：在尾部追加元素 / Implement `Array.prototype.push`: append elements at the end
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 追加后的新长度 / The new length after appending
 */
static JsValue array_push(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *src = arr_of(thisv);
    int i;
    HPE_UNUSED(ud);
    if (!src)
        return js_num(0);
    for (i = 0; i < argc; i++)
        array_push_value(js, src, a[i]);
    return js_num((double)array_length(js, src));
}

/**
 * @brief 实现 `Array.prototype.pop`：弹出并返回尾元素 / Implement `Array.prototype.pop`: remove and return the last element
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 被移除的尾元素；数组为空时返回 `undefined` / The removed last element, or `undefined` when the array is empty
 */
static JsValue array_pop(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *src = arr_of(thisv);
    int len;
    JsValue v;
    char key[24];
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    if (!src)
        return js_undef();
    len = array_length(js, src);
    if (len == 0)
        return js_undef();
    v = array_get_index(js, src, len - 1);
    snprintf(key, sizeof(key), "%d", len - 1);
    js_obj_delete(js, src, key);
    js_obj_set(js, src, "length", js_num((double)(len - 1)));
    return v;
}

/**
 * @brief 实现 `Array.prototype.shift`：移除并返回首元素 / Implement `Array.prototype.shift`: remove and return the first element
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 被移除的首元素；数组为空时返回 `undefined` / The removed first element, or `undefined` when the array is empty
 *
 * @note 剩余元素整体前移一位 / The remaining elements are shifted one slot toward the front
 */
static JsValue array_shift(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *src = arr_of(thisv);
    int len, i;
    JsValue first;
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    if (!src)
        return js_undef();
    len = array_length(js, src);
    if (len == 0)
        return js_undef();
    first = array_get_index(js, src, 0);
    for (i = 1; i < len; i++)
        array_set_index(js, src, i - 1, array_get_index(js, src, i));
    {
        char key[24];
        snprintf(key, sizeof(key), "%d", len - 1);
        js_obj_delete(js, src, key);
    }
    js_obj_set(js, src, "length", js_num((double)(len - 1)));
    return first;
}

/**
 * @brief 实现 `Array.prototype.unshift`：在头部插入元素 / Implement `Array.prototype.unshift`: insert elements at the front
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 插入后的新长度 / The new length after insertion
 */
static JsValue array_unshift(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *src = arr_of(thisv);
    int len, i;
    HPE_UNUSED(ud);
    if (!src)
        return js_num(0);
    len = array_length(js, src);
    for (i = len - 1; i >= 0; i--)
        array_set_index(js, src, i + argc, array_get_index(js, src, i));
    for (i = 0; i < argc; i++)
        array_set_index(js, src, i, a[i]);
    return js_num((double)(len + argc));
}

/**
 * @brief 实现 `Array.prototype.slice`：按可负区间截取新数组 / Implement `Array.prototype.slice`: copy a possibly negative range into a new array
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 由区间元素组成的新数组 / A new array holding the range elements
 */
static JsValue array_slice(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *src = arr_of(thisv);
    JsObj *out = js_array_new(js);
    int len, start, end, i;
    HPE_UNUSED(ud);
    if (!src)
        return js_objval(out);
    len = array_length(js, src);
    start = argc > 0 && a[0].t != JS_UNDEF ? (int)js_to_number(js, a[0]) : 0;
    end = argc > 1 && a[1].t != JS_UNDEF ? (int)js_to_number(js, a[1]) : len;
    if (start < 0)
        start += len;
    if (end < 0)
        end += len;
    if (start < 0)
        start = 0;
    if (end > len)
        end = len;
    for (i = start; i < end; i++)
        array_push_value(js, out, array_get_index(js, src, i));
    return js_objval(out);
}

/**
 * @brief 实现 `Array.prototype.splice`：删除并可选插入元素 / Implement `Array.prototype.splice`: delete and optionally insert elements
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 由被删除元素组成的新数组 / A new array holding the removed elements
 *
 * @note 从第 2 个实参起作为要插入的元素，插入到删除位置 / Arguments from the second onward are inserted at the removal position
 */
static JsValue array_splice(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *src = arr_of(thisv);
    JsObj *removed = js_array_new(js);
    int len, start, del, i;
    JsValue *tail;
    int tailn;
    HPE_UNUSED(ud);
    if (!src)
        return js_objval(removed);
    len = array_length(js, src);
    start = argc > 0 ? (int)js_to_number(js, a[0]) : 0;
    if (start < 0)
        start += len;
    if (start < 0)
        start = 0;
    if (start > len)
        start = len;
    del = argc > 1 ? (int)js_to_number(js, a[1]) : len - start;
    if (del < 0)
        del = 0;
    if (start + del > len)
        del = len - start;
    for (i = 0; i < del; i++)
        array_push_value(js, removed, array_get_index(js, src, start + i));
    tailn = len - start - del;
    tail = (JsValue *)hpe_js_alloc(js, (size_t)(tailn + 1) * sizeof(JsValue));
    for (i = 0; i < tailn; i++)
        tail[i] = array_get_index(js, src, start + del + i);
    js_obj_set(js, src, "length", js_num((double)start));
    for (i = 2; i < argc; i++)
        array_push_value(js, src, a[i]);
    for (i = 0; i < tailn; i++)
        array_push_value(js, src, tail[i]);
    return js_objval(removed);
}

/**
 * @brief 实现 `Array.prototype.concat`：拼接自身与各实参 / Implement `Array.prototype.concat`: concatenate the receiver and all arguments
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 拼接后的新数组 / The concatenated new array
 *
 * @note 数组实参逐个展开，非数组实参整体作为单个元素 / Array arguments are spread, other arguments are appended as single elements
 */
static JsValue array_concat(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *out = js_array_new(js);
    int i;
    JsObj *src = arr_of(thisv);
    HPE_UNUSED(ud);
    if (src) {
        int len = array_length(js, src), k;
        for (k = 0; k < len; k++)
            array_push_value(js, out, array_get_index(js, src, k));
    }
    for (i = 0; i < argc; i++) {
        if (a[i].t == JS_OBJ && a[i].obj && a[i].obj->kind == JS_OBJ_ARRAY) {
            int len = array_length(js, a[i].obj), k;
            for (k = 0; k < len; k++)
                array_push_value(js, out, array_get_index(js, a[i].obj, k));
        } else {
            array_push_value(js, out, a[i]);
        }
    }
    return js_objval(out);
}

/**
 * @brief 实现 `Array.prototype.indexOf`：从前往后查找元素 / Implement `Array.prototype.indexOf`: search for an element forward
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 命中元素的下标；未命中时返回 -1 / The index of the matching element, or -1 when not found
 */
static JsValue array_indexOf(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *src = arr_of(thisv);
    int len, from = argc > 1 ? (int)js_to_number(js, a[1]) : 0, i;
    JsValue needle = arg_at(a, argc, 0);
    HPE_UNUSED(ud);
    if (!src)
        return js_num(-1);
    len = array_length(js, src);
    if (from < 0)
        from += len;
    if (from < 0)
        from = 0;
    for (i = from; i < len; i++)
        if (js_values_equal(js, array_get_index(js, src, i), needle, 1))
            return js_num((double)i);
    return js_num(-1);
}

/**
 * @brief 实现 `Array.prototype.lastIndexOf`：从后往前查找元素 / Implement `Array.prototype.lastIndexOf`: search for an element backward
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 最后一次命中的下标；未命中时返回 -1 / The index of the last match, or -1 when not found
 */
static JsValue array_lastIndexOf(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *src = arr_of(thisv);
    int len, from, i;
    JsValue needle = arg_at(a, argc, 0);
    HPE_UNUSED(ud);
    if (!src)
        return js_num(-1);
    len = array_length(js, src);
    from = argc > 1 ? (int)js_to_number(js, a[1]) : len - 1;
    if (from < 0)
        from += len;
    if (from >= len)
        from = len - 1;
    for (i = from; i >= 0; i--)
        if (js_values_equal(js, array_get_index(js, src, i), needle, 1))
            return js_num((double)i);
    return js_num(-1);
}

/**
 * @brief 实现 `Array.prototype.includes`：判断是否包含某值 / Implement `Array.prototype.includes`: test whether a value is present
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 命中返回 `true`，否则返回 `false` / `true` when present, otherwise `false`
 *
 * @note 同时按 `SameValueZero` 处理 `NaN` / `NaN` is matched following `SameValueZero`
 */
static JsValue array_includes(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *src = arr_of(thisv);
    int len, i;
    JsValue needle = arg_at(a, argc, 0);
    HPE_UNUSED(ud);
    if (!src)
        return js_bool(0);
    len = array_length(js, src);
    for (i = 0; i < len; i++) {
        JsValue e = array_get_index(js, src, i);
        if (js_values_equal(js, e, needle, 1) || (e.t == JS_NUM && needle.t == JS_NUM && e.num != e.num && needle.num != needle.num))
            return js_bool(1);
    }
    return js_bool(0);
}

/* generic iteration helper: mode picks the return shape */
enum {
    ARR_EACH = 0,
    ARR_MAP = 1,
    ARR_FILTER = 2,
    ARR_SOME = 4,
    ARR_EVERY = 5,
    ARR_FIND = 6,
    ARR_FINDIDX = 7
};

/**
 * @brief 数组遍历公共骨架：按模式决定回调与返回形态 / Shared array-iteration skeleton: the mode selects the callback and result shape
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] src 源数组对象 / Source array object
 * @param[in]     cb 逐元素回调 / Per-element callback
 * @param[in]     this_arg 回调调用时的 this / `this` value for the callback
 * @param[in]     mode 遍历模式（`ARR_*` 枚举）/ Iteration mode (`ARR_*` enum)
 *
 * @return 随 `mode` 变化：新数组、布尔、元素值、下标或 `undefined` / Varies with `mode`: a new array, a boolean, an element, an index, or `undefined`
 *
 * @note 回调以 `(value, index, array)` 三个实参调用 / The callback is invoked with `(value, index, array)`
 */
static JsValue arr_iterate(HpeJs *js, JsObj *src, JsValue cb, JsValue this_arg, int mode)
{
    int len = array_length(js, src), i;
    JsObj *out = NULL;
    if (mode == ARR_MAP || mode == ARR_FILTER)
        out = js_array_new(js);
    for (i = 0; i < len; i++) {
        JsValue v = array_get_index(js, src, i);
        JsValue args[3];
        JsValue r;
        args[0] = v;
        args[1] = js_num((double)i);
        args[2] = js_objval(src);
        r = call_value(js, NULL, cb, this_arg, args, 3, 0);
        if (mode == ARR_MAP) {
            array_push_value(js, out, r);
        } else if (mode == ARR_FILTER) {
            if (js_truthy(r))
                array_push_value(js, out, v);
        } else if (mode == ARR_SOME) {
            if (js_truthy(r))
                return js_bool(1);
        } else if (mode == ARR_EVERY) {
            if (!js_truthy(r))
                return js_bool(0);
        } else if (mode == ARR_FIND) {
            if (js_truthy(r))
                return v;
        } else if (mode == ARR_FINDIDX) {
            if (js_truthy(r))
                return js_num((double)i);
        }
    }
    if (mode == ARR_SOME)
        return js_bool(0);
    if (mode == ARR_EVERY)
        return js_bool(1);
    if (mode == ARR_FIND)
        return js_undef();
    if (mode == ARR_FINDIDX)
        return js_num(-1);
    if (out)
        return js_objval(out);
    return js_undef();
}

/**
 * @brief 实现 `Array.prototype.forEach`：逐元素调用回调 / Implement `Array.prototype.forEach`: invoke a callback per element
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 始终返回 `undefined` / Always returns `undefined`
 *
 * @note 回调不可调用时直接返回 / Returns immediately when the callback is not callable
 */
static JsValue array_forEach(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *src = arr_of(thisv);
    HPE_UNUSED(ud);
    if (!src || !js_is_callable(arg_at(a, argc, 0)))
        return js_undef();
    return arr_iterate(js, src, a[0], arg_at(a, argc, 1), ARR_EACH);
}

/**
 * @brief 实现 `Array.prototype.map`：把回调结果收集为新数组 / Implement `Array.prototype.map`: collect callback results into a new array
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 映射后的新数组；回调不可调用时为空数组 / The mapped new array, or an empty array when the callback is not callable
 */
static JsValue array_map(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *src = arr_of(thisv);
    HPE_UNUSED(ud);
    if (!src)
        return js_objval(js_array_new(js));
    if (!js_is_callable(arg_at(a, argc, 0)))
        return js_objval(js_array_new(js));
    return arr_iterate(js, src, a[0], arg_at(a, argc, 1), ARR_MAP);
}

/**
 * @brief 实现 `Array.prototype.filter`：保留回调为真的元素 / Implement `Array.prototype.filter`: keep the elements whose callback is truthy
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 筛选后的新数组 / The filtered new array
 */
static JsValue array_filter(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *src = arr_of(thisv);
    HPE_UNUSED(ud);
    if (!src)
        return js_objval(js_array_new(js));
    if (!js_is_callable(arg_at(a, argc, 0)))
        return js_objval(js_array_new(js));
    return arr_iterate(js, src, a[0], arg_at(a, argc, 1), ARR_FILTER);
}

/**
 * @brief 实现 `Array.prototype.some`：是否存在回调为真的元素 / Implement `Array.prototype.some`: test whether any element passes the callback
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 任一为真返回 `true`，否则返回 `false` / `true` when any element passes, otherwise `false`
 */
static JsValue array_some(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *src = arr_of(thisv);
    HPE_UNUSED(ud);
    if (!src || !js_is_callable(arg_at(a, argc, 0)))
        return js_bool(0);
    return arr_iterate(js, src, a[0], arg_at(a, argc, 1), ARR_SOME);
}

/**
 * @brief 实现 `Array.prototype.every`：是否所有元素都通过回调 / Implement `Array.prototype.every`: test whether all elements pass the callback
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 全部通过返回 `true`，否则返回 `false` / `true` when all pass, otherwise `false`
 */
static JsValue array_every(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *src = arr_of(thisv);
    HPE_UNUSED(ud);
    if (!src || !js_is_callable(arg_at(a, argc, 0)))
        return js_bool(1);
    return arr_iterate(js, src, a[0], arg_at(a, argc, 1), ARR_EVERY);
}

/**
 * @brief 实现 `Array.prototype.find`：返回首个通过回调的元素 / Implement `Array.prototype.find`: return the first element passing the callback
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 首个命中的元素；无命中时返回 `undefined` / The first matching element, or `undefined` when none matches
 */
static JsValue array_find(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *src = arr_of(thisv);
    HPE_UNUSED(ud);
    if (!src || !js_is_callable(arg_at(a, argc, 0)))
        return js_undef();
    return arr_iterate(js, src, a[0], arg_at(a, argc, 1), ARR_FIND);
}

/**
 * @brief 实现 `Array.prototype.findIndex`：返回首个通过回调的下标 / Implement `Array.prototype.findIndex`: return the index of the first element passing the callback
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 首个命中的下标；无命中时返回 -1 / The first matching index, or -1 when none matches
 */
static JsValue array_findIndex(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *src = arr_of(thisv);
    HPE_UNUSED(ud);
    if (!src || !js_is_callable(arg_at(a, argc, 0)))
        return js_num(-1);
    return arr_iterate(js, src, a[0], arg_at(a, argc, 1), ARR_FINDIDX);
}

/**
 * @brief 实现 `Array.prototype.reduce`：从左到右累积 / Implement `Array.prototype.reduce`: accumulate from left to right
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 最终累积值 / The final accumulator value
 *
 * @warning 回调不可调用或空数组且无初值时抛出错误 / Throws when the callback is not callable, or on an empty array without an initial value
 */
static JsValue array_reduce(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *src = arr_of(thisv);
    int len, i = 0;
    JsValue acc;
    HPE_UNUSED(ud);
    if (!src || !js_is_callable(arg_at(a, argc, 0)))
        js_error(js, 0, "reduce: callback is not a function");
    len = array_length(js, src);
    if (argc > 1) {
        acc = a[1];
    } else {
        while (i < len && array_get_index(js, src, i).t == JS_UNDEF)
            i++;
        if (i >= len)
            js_error(js, 0, "reduce of empty array with no initial value");
        acc = array_get_index(js, src, i++);
    }
    for (; i < len; i++) {
        JsValue args[4];
        args[0] = acc;
        args[1] = array_get_index(js, src, i);
        args[2] = js_num((double)i);
        args[3] = js_objval(src);
        acc = call_value(js, NULL, a[0], js_undef(), args, 4, 0);
    }
    return acc;
}

/**
 * @brief 实现 `Array.prototype.reduceRight`：从右到左累积 / Implement `Array.prototype.reduceRight`: accumulate from right to left
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 最终累积值 / The final accumulator value
 *
 * @warning 回调不可调用或空数组且无初值时抛出错误 / Throws when the callback is not callable, or on an empty array without an initial value
 */
static JsValue array_reduceRight(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *src = arr_of(thisv);
    int len, i;
    JsValue acc;
    HPE_UNUSED(ud);
    if (!src || !js_is_callable(arg_at(a, argc, 0)))
        js_error(js, 0, "reduceRight: callback is not a function");
    len = array_length(js, src);
    i = len - 1;
    if (argc > 1) {
        acc = a[1];
    } else {
        while (i >= 0 && array_get_index(js, src, i).t == JS_UNDEF)
            i--;
        if (i < 0)
            js_error(js, 0, "reduce of empty array with no initial value");
        acc = array_get_index(js, src, i--);
    }
    for (; i >= 0; i--) {
        JsValue args[4];
        args[0] = acc;
        args[1] = array_get_index(js, src, i);
        args[2] = js_num((double)i);
        args[3] = js_objval(src);
        acc = call_value(js, NULL, a[0], js_undef(), args, 4, 0);
    }
    return acc;
}

/**
 * @brief 实现 `findLast`/`findLastIndex`：从右往左找首个命中 / Implement `findLast`/`findLastIndex`: find the first hit scanning from the right
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 非空返回元素（findLast），为空返回下标（findLastIndex）/ Non-null returns the element (findLast), null returns the index (findLastIndex)
 *
 * @return 命中的元素或其下标，无命中时分别为 `undefined` 或 -1 / The matching element or its index, or `undefined` / -1 when none matches
 */
static JsValue array_find_last(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *src = arr_of(thisv);
    JsValue cb = arg_at(a, argc, 0);
    int len, i;
    if (!src || !js_is_callable(cb))
        js_error(js, 0, "findLast: callback is not a function");
    len = array_length(js, src);
    for (i = len - 1; i >= 0; i--) {
        JsValue v = array_get_index(js, src, i);
        JsValue args[3];
        args[0] = v;
        args[1] = js_num((double)i);
        args[2] = js_objval(src);
        if (js_truthy(call_value(js, NULL, cb, js_undef(), args, 3, 0)))
            return ud ? v : js_num((double)i);
    }
    return ud ? js_undef() : js_num(-1.0);
}

/**
 * @brief 实现 `Array.prototype.copyWithin`：区间内就地搬运元素 / Implement `Array.prototype.copyWithin`: copy an element range in place
 *
 * @details 把 `[from, end)` 拷贝到 `target` 起的位置，区间重叠时反向拷贝以免覆盖源数据。
 *          Copies `[from, end)` to start at `target`, copying backwards when the ranges overlap so source data is not clobbered.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 被修改的数组本身 / The modified array itself
 */
static JsValue array_copyWithin(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *src = arr_of(thisv);
    int len, to, from, end, count, i;
    HPE_UNUSED(ud);
    if (!src)
        return thisv;
    len = array_length(js, src);
    to = argc > 0 ? (int)js_to_number(js, a[0]) : 0;
    from = argc > 1 ? (int)js_to_number(js, a[1]) : 0;
    end = argc > 2 && a[2].t != JS_UNDEF ? (int)js_to_number(js, a[2]) : len;
    if (to < 0)
        to += len;
    if (from < 0)
        from += len;
    if (end < 0)
        end += len;
    if (to < 0)
        to = 0;
    if (from < 0)
        from = 0;
    if (end > len)
        end = len;
    count = end - from;
    if (count > len - to)
        count = len - to;
    if (count <= 0)
        return thisv;
    if (from < to && to < from + count) {
        for (i = count - 1; i >= 0; i--) /* copy backwards to avoid clobbering */
            array_set_index(js, src, to + i, array_get_index(js, src, from + i));
    } else {
        for (i = 0; i < count; i++)
            array_set_index(js, src, to + i, array_get_index(js, src, from + i));
    }
    return thisv;
}

/**
 * @brief 实现 `Array.prototype.entries`：产出 `[下标, 元素]` 对的可迭代对象 / Implement `Array.prototype.entries`: an iterable of `[index, value]` pairs
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 可迭代的结果对象 / An iterable result object
 */
static JsValue array_entries(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *src = arr_of(thisv);
    JsObj *items = js_array_new(js);
    int i, len;
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    if (!src)
        return make_iterable(js, items);
    len = array_length(js, src);
    for (i = 0; i < len; i++) {
        JsValue pair[2];
        pair[0] = js_num((double)i);
        pair[1] = array_get_index(js, src, i);
        array_push_value(js, items, array_of(js, pair, 2));
    }
    return make_iterable(js, items);
}

/**
 * @brief `Array.prototype[Symbol.iterator]`：直接返回数组自身 / `Array.prototype[Symbol.iterator]`: return the array itself
 *
 * @details 取迭代长度与元素的下标访问已经能遍历数组元素，因此直接返回 `this` 即可支持 for-of 与展开。
 *          Length and indexed access already walk array elements, so returning `this` supports for-of and spread directly.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 未改动的 `thisv` / `thisv` unchanged
 */
static JsValue array_iter_self(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(js);
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    return thisv;
}

/**
 * @brief 实现 `Array.prototype.reverse`：就地反转元素顺序 / Implement `Array.prototype.reverse`: reverse the elements in place
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 被修改的数组本身 / The modified array itself
 */
static JsValue array_reverse(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *src = arr_of(thisv);
    int len, i;
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    if (!src)
        return thisv;
    len = array_length(js, src);
    for (i = 0; i < len / 2; i++) {
        JsValue t = array_get_index(js, src, i);
        array_set_index(js, src, i, array_get_index(js, src, len - 1 - i));
        array_set_index(js, src, len - 1 - i, t);
    }
    return thisv;
}

/**
 * @brief 默认排序比较：按字符串字典序，`undefined` 排在末尾 / Default sort comparison: lexicographic by string, with `undefined` ordered last
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     x 左参与比较的值 / Left value to compare
 * @param[in]     y 右参与比较的值 / Right value to compare
 * @param[out]    undef 置 1 表示本次比较涉及 `undefined` / Set to 1 when this comparison involved `undefined`
 *
 * @return 负、零、正分别表示 x 小于、等于、大于 y / Negative, zero, or positive as x is less than, equal to, or greater than y
 */
static int default_cmp(HpeJs *js, JsValue x, JsValue y, int *undef)
{
    *undef = 0;
    if (x.t == JS_UNDEF && y.t == JS_UNDEF)
        return 0;
    if (x.t == JS_UNDEF) {
        *undef = 1;
        return 1;
    }
    if (y.t == JS_UNDEF) {
        *undef = 1;
        return 1;
    }
    {
        char *xs = js_to_string(js, x);
        char *ys = js_to_string(js, y);
        return strcmp(xs, ys);
    }
}

/**
 * @brief 实现 `Array.prototype.sort`：就地稳定排序 / Implement `Array.prototype.sort`: sort in place, stably
 *
 * @details 使用插入排序以保证稳定性；提供了比较函数时用其返回值定序，返回 `NaN` 视为相等；否则走 `default_cmp`。
 *          Insertion sort is used for stability; a supplied comparator decides the order with `NaN` treated as equal; otherwise `default_cmp` is used.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 被修改的数组本身 / The modified array itself
 */
static JsValue array_sort(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *src = arr_of(thisv);
    int len, i, j;
    JsValue cmp = arg_at(a, argc, 0);
    int has_cmp = js_is_callable(cmp);
    HPE_UNUSED(ud);
    if (!src)
        return thisv;
    len = array_length(js, src);
    for (i = 0; i < len; i++) { /* insertion sort: stable + tiny code */
        for (j = i; j > 0; j--) {
            JsValue x = array_get_index(js, src, j - 1);
            JsValue y = array_get_index(js, src, j);
            int c;
            if (has_cmp) {
                JsValue args[2];
                args[0] = x;
                args[1] = y;
                c = (int)js_to_number(js, call_value(js, NULL, cmp, js_undef(), args, 2, 0));
                if (c != c)
                    c = 0;
            } else {
                int undef = 0;
                c = default_cmp(js, x, y, &undef);
                if (undef)
                    c = (x.t == JS_UNDEF) ? 1 : -1;
            }
            if (c > 0) {
                array_set_index(js, src, j - 1, y);
                array_set_index(js, src, j, x);
            } else {
                break;
            }
        }
    }
    return thisv;
}

/**
 * @brief 把数组按指定深度展平并追加到输出数组 / Flatten an array to a given depth, appending into an output array
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] out 接收展平结果的数组 / Array receiving the flattened elements
 * @param[in,out] src 待展平的源数组 / Source array to flatten
 * @param[in]     depth 剩余可展平深度，为 0 时不再下钻 / Remaining flatten depth, no recursion when 0
 *
 * @note 深度大于 0 且元素为数组时递归下钻，否则原样追加 / Recurses when depth is positive and an element is an array, otherwise appends it verbatim
 */
static void flat_into(HpeJs *js, JsObj *out, JsObj *src, int depth)
{
    int len = array_length(js, src), i;
    for (i = 0; i < len; i++) {
        JsValue v = array_get_index(js, src, i);
        if (depth > 0 && v.t == JS_OBJ && v.obj && v.obj->kind == JS_OBJ_ARRAY)
            flat_into(js, out, v.obj, depth - 1);
        else
            array_push_value(js, out, v);
    }
}

/**
 * @brief 实现 `Array.prototype.flat`：按深度展平数组 / Implement `Array.prototype.flat`: flatten an array to a given depth
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 展平后的新数组 / The flattened new array
 */
static JsValue array_flat(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *src = arr_of(thisv);
    JsObj *out = js_array_new(js);
    int depth = argc > 0 ? (int)js_to_number(js, a[0]) : 1;
    HPE_UNUSED(ud);
    if (src)
        flat_into(js, out, src, depth);
    return js_objval(out);
}

/**
 * @brief 实现 `Array.prototype.flatMap`：先映射再按深度 1 展平 / Implement `Array.prototype.flatMap`: map then flatten to depth 1
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 映射并展平后的新数组 / The mapped and flattened new array
 */
static JsValue array_flatMap(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsValue mapped = array_map(js, thisv, a, argc, ud);
    JsObj *out = js_array_new(js);
    if (mapped.t == JS_OBJ && mapped.obj)
        flat_into(js, out, mapped.obj, 1);
    return js_objval(out);
}

/**
 * @brief 实现 `Array.prototype.toString`：等同于无分隔符参数的 `join` / Implement `Array.prototype.toString`: equivalent to `join` with the default separator
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 连接后的字符串 / The joined string
 */
static JsValue array_toString(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    return array_join(js, thisv, a, argc, ud);
}

/**
 * @brief 实现 `Array.prototype.at`：支持下标的负值取元素 / Implement `Array.prototype.at`: read an element with possibly negative index
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 该下标处的元素；越界时返回 `undefined` / The element at that index, or `undefined` when out of range
 */
static JsValue array_at(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *src = arr_of(thisv);
    int len, i;
    HPE_UNUSED(ud);
    if (!src)
        return js_undef();
    len = array_length(js, src);
    i = argc > 0 ? (int)js_to_number(js, a[0]) : 0;
    if (i < 0)
        i += len;
    if (i < 0 || i >= len)
        return js_undef();
    return array_get_index(js, src, i);
}

/**
 * @brief 实现 `Array.prototype.fill`：把区间元素填成同一个值 / Implement `Array.prototype.fill`: set a range of elements to one value
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 被修改的数组本身 / The modified array itself
 */
static JsValue array_fill(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *src = arr_of(thisv);
    int len, start, end, i;
    JsValue v = arg_at(a, argc, 0);
    HPE_UNUSED(ud);
    if (!src)
        return thisv;
    len = array_length(js, src);
    start = argc > 1 ? (int)js_to_number(js, a[1]) : 0;
    end = argc > 2 && a[2].t != JS_UNDEF ? (int)js_to_number(js, a[2]) : len;
    if (start < 0)
        start += len;
    if (end < 0)
        end += len;
    if (start < 0)
        start = 0;
    if (end > len)
        end = len;
    for (i = start; i < end; i++)
        array_set_index(js, src, i, v);
    return thisv;
}

/**
 * @brief 实现 `Array.prototype.keys`：产出下标数组 / Implement `Array.prototype.keys`: produce the index array
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 由 0 到长度减一组成的数组 / An array of 0 through length - 1
 */
static JsValue array_keys(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *src = arr_of(thisv);
    JsObj *out = js_array_new(js);
    int len, i;
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    if (!src)
        return js_objval(out);
    len = array_length(js, src);
    for (i = 0; i < len; i++)
        array_push_value(js, out, js_num((double)i));
    return js_objval(out);
}

/**
 * @brief 实现 `Array.prototype.values`：产出元素数组 / Implement `Array.prototype.values`: produce the value array
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 按顺序复制出的元素数组 / An array of the elements in order
 */
static JsValue array_values(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *src = arr_of(thisv);
    JsObj *out = js_array_new(js);
    int len, i;
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    if (!src)
        return js_objval(out);
    len = array_length(js, src);
    for (i = 0; i < len; i++)
        array_push_value(js, out, array_get_index(js, src, i));
    return js_objval(out);
}

/**
 * @brief 实现 `Array.from`：由可迭代对象构造数组并可选映射 / Implement `Array.from`: build an array from an iterable, optionally mapping
 *
 * @details 遍历源对象的迭代长度；若传入映射函数则对每个元素以 `(value, index)` 调用并采用其返回值。
 *          Walks the source's iteration length; when a map function is given each element is passed through it as `(value, index)`.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 新建的数组 / The newly built array
 */
static JsValue native_array_from(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *out = js_array_new(js);
    JsValue src = arg_at(a, argc, 0);
    JsValue fn = arg_at(a, argc, 1);
    int i, n;
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    n = js_iter_len(js, src);
    for (i = 0; i < n; i++) {
        JsValue v = js_iter_at(js, src, i);
        if (js_is_callable(fn)) {
            JsValue args[2];
            args[0] = v;
            args[1] = js_num((double)i);
            v = call_value(js, NULL, fn, js_undef(), args, 2, 0);
        }
        array_push_value(js, out, v);
    }
    return js_objval(out);
}

/**
 * @brief 实现 `Array.of`：用全部实参构造数组 / Implement `Array.of`: build an array from all arguments
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 由实参组成的数组 / An array built from the arguments
 */
static JsValue native_array_of(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    return array_of(js, a, argc);
}

/**
 * @brief 实现 `Array.isArray`：判断是否为数组对象 / Implement `Array.isArray`: test whether a value is an array object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 是数组返回 `true`，否则返回 `false` / `true` when it is an array, otherwise `false`
 */
static JsValue native_array_is_array(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsValue v = arg_at(a, argc, 0);
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    return js_bool(v.t == JS_OBJ && v.obj && v.obj->kind == JS_OBJ_ARRAY);
}

/**
 * @brief 数组方法安装占位：当前为空实现 / Placeholder for array-method installation: currently a no-op
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] arr 目标数组对象 / Target array object
 *
 * @note 两个参数均未被使用，保留以维持调用点稳定 / Both parameters are unused and kept only to keep the call site stable
 */
void js_array_install_methods(HpeJs *js, JsObj *arr)
{
    HPE_UNUSED(js);
    HPE_UNUSED(arr);
}

/**
 * @brief 实现 `Object.keys`：取对象的自有可枚举键 / Implement `Object.keys`: own enumerable keys of an object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 键名组成的数组；非对象参数为空数组 / An array of key names, or an empty array for a non-object argument
 */
static JsValue object_keys(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsValue v = arg_at(a, argc, 0);
    JsObj *out = js_array_new(js);
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    if (v.t == JS_OBJ && v.obj) {
        if (v.obj->kind == JS_OBJ_ARRAY) {
            int len = array_length(js, v.obj), i;
            for (i = 0; i < len; i++) {
                char buf[24];
                snprintf(buf, sizeof(buf), "%d", i);
                array_push_value(js, out, js_string(js, buf));
            }
        } else {
            char **keys;
            int nk, i;
            js_obj_own_keys(v.obj, &keys, &nk);
            for (i = 0; i < nk; i++)
                array_push_value(js, out, js_string(js, keys[i]));
            free(keys);
        }
    }
    return js_objval(out);
}

/**
 * @brief 实现 `Object.values`：取对象的自有可枚举值 / Implement `Object.values`: own enumerable values of an object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 属性值组成的数组 / An array of property values
 */
static JsValue object_values(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsValue v = arg_at(a, argc, 0);
    JsObj *out = js_array_new(js);
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    if (v.t == JS_OBJ && v.obj) {
        char **keys;
        int nk, i;
        js_obj_own_keys(v.obj, &keys, &nk);
        for (i = 0; i < nk; i++)
            array_push_value(js, out, member_get(js, NULL, v, keys[i], 0));
        free(keys);
    }
    return js_objval(out);
}

/**
 * @brief 实现 `Object.entries`：取 `[键, 值]` 对数组 / Implement `Object.entries`: an array of `[key, value]` pairs
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 由键值对数组组成的数组 / An array of key/value pair arrays
 */
static JsValue object_entries(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsValue v = arg_at(a, argc, 0);
    JsObj *out = js_array_new(js);
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    if (v.t == JS_OBJ && v.obj) {
        char **keys;
        int nk, i;
        js_obj_own_keys(v.obj, &keys, &nk);
        for (i = 0; i < nk; i++) {
            JsValue pair[2];
            pair[0] = js_string(js, keys[i]);
            pair[1] = member_get(js, NULL, v, keys[i], 0);
            array_push_value(js, out, array_of(js, pair, 2));
        }
        free(keys);
    }
    return js_objval(out);
}

/**
 * @brief 实现 `Object.assign`：把各来源对象属性拷入目标 / Implement `Object.assign`: copy properties from sources into the target
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 目标对象；目标不是对象时原样返回 / The target object, returned unchanged when it is not an object
 *
 * @note 只处理对象来源，非对象来源被跳过 / Only object sources are processed; others are skipped
 */
static JsValue object_assign(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsValue target = arg_at(a, argc, 0);
    int i;
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    if (target.t != JS_OBJ || !target.obj)
        return target;
    for (i = 1; i < argc; i++) {
        JsValue src = a[i];
        if (src.t != JS_OBJ || !src.obj)
            continue;
        {
            char **keys;
            int nk, k;
            js_obj_own_keys(src.obj, &keys, &nk);
            for (k = 0; k < nk; k++)
                member_set(js, NULL, target, keys[k], member_get(js, NULL, src, keys[k], 0), 0);
            free(keys);
        }
    }
    return target;
}

/**
 * @brief 实现 `Object.freeze`：把对象标记为冻结 / Implement `Object.freeze`: mark an object as frozen
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 被冻结的对象 / The frozen object
 *
 * @note 仅设置内部状态位，实际仍可读写 / Only an internal state flag is set; the object remains readable and writable
 */
static JsValue object_freeze(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsValue v = arg_at(a, argc, 0);
    HPE_UNUSED(js);
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    if (v.t == JS_OBJ && v.obj)
        v.obj->num_val = 3; /* sealed | frozen */
    return v;
}

/**
 * @brief 实现 `Object.create`：以给定原型新建对象 / Implement `Object.create`: create an object with a given prototype
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 新对象 / The new object
 *
 * @note 原型参数非对象时新对象的原型为 `NULL` / The new object's prototype is `NULL` when the argument is not an object
 */
static JsValue object_create(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsValue proto = arg_at(a, argc, 0);
    JsObj *o = js_object_raw(js);
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    o->proto = (proto.t == JS_OBJ) ? proto.obj : NULL;
    return js_objval(o);
}

/**
 * @brief 实现 `Object.defineProperty`：写入数据属性或访问器属性 / Implement `Object.defineProperty`: install a data or accessor property
 *
 * @details 描述符含 `get`/`set` 时安装访问器，否则取 `value` 写入数据属性。
 *          When the descriptor provides `get`/`set` an accessor is installed; otherwise `value` is written as a data property.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 目标对象；目标不是对象时原样返回 / The target object, returned unchanged when it is not an object
 */
static JsValue object_define_property(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsValue target = arg_at(a, argc, 0);
    JsValue key = arg_at(a, argc, 1);
    JsValue desc = arg_at(a, argc, 2);
    char *k;
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    if (target.t != JS_OBJ || !target.obj)
        return target;
    k = js_to_string(js, key);
    if (desc.t == JS_OBJ && desc.obj) {
        JsValue g = member_get(js, NULL, desc, "get", 0);
        JsValue s = member_get(js, NULL, desc, "set", 0);
        if (g.t != JS_UNDEF || s.t != JS_UNDEF)
            js_obj_set_accessor(js, target.obj, k, g, s);
        else
            member_set(js, NULL, target, k, member_get(js, NULL, desc, "value", 0), 0);
    } else {
        member_set(js, NULL, target, k, js_undef(), 0);
    }
    return target;
}

/**
 * @brief 实现 `Object.getOwnPropertyDescriptor`：读取属性的描述符 / Implement `Object.getOwnPropertyDescriptor`: read a property's descriptor
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 含 `value`/`writable` 或 `get`/`set` 及 `enumerable` 的描述符对象；属性不存在时返回 `undefined` / A descriptor object with `value`/`writable` or `get`/`set` plus `enumerable`, or `undefined` when the property is absent
 */
static JsValue object_get_property_descriptor(HpeJs *js, JsValue thisv, JsValue *a, int argc,
                                               void *ud)
{
    JsValue target = arg_at(a, argc, 0);
    JsValue key = arg_at(a, argc, 1);
    JsObj *o;
    JsProp *p;
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    if (target.t != JS_OBJ || !target.obj)
        return js_undef();
    o = js_object_new(js);
    p = js_obj_own_prop(target.obj, js_to_string(js, key));
    if (!p)
        return js_undef();
    if (p->getter.t != JS_UNDEF || p->setter.t != JS_UNDEF) {
        js_obj_set(js, o, "get", p->getter);
        js_obj_set(js, o, "set", p->setter);
    } else {
        js_obj_set(js, o, "value", p->val);
        js_obj_set(js, o, "writable", js_bool(1));
    }
    js_obj_set(js, o, "enumerable", js_bool(p->enumerable));
    return js_objval(o);
}

/**
 * @brief 实现 `Object.hasOwn`：判断是否含自有属性 / Implement `Object.hasOwn`: test whether an object has an own property
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 包含返回 `true`，否则返回 `false` / `true` when present, otherwise `false`
 */
static JsValue object_has_own(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsValue v = arg_at(a, argc, 0);
    JsValue k = arg_at(a, argc, 1);
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    if (v.t != JS_OBJ || !v.obj)
        return js_bool(0);
    return js_bool(js_obj_own_has(v.obj, js_to_string(js, k)));
}

/**
 * @brief 实现 `Object.getPrototypeOf`：读取对象的原型 / Implement `Object.getPrototypeOf`: read an object's prototype
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 原型对象；无原型或参数非对象时返回 `null` / The prototype object, or `null` when there is none or the argument is not an object
 */
static JsValue object_get_prototype_of(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsValue v = arg_at(a, argc, 0);
    HPE_UNUSED(js);
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    if (v.t == JS_OBJ && v.obj && v.obj->proto)
        return js_objval(v.obj->proto);
    return js_null();
}

/**
 * @brief 实现 `Object.setPrototypeOf`：设置对象的原型 / Implement `Object.setPrototypeOf`: set an object's prototype
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 传入的对象本身 / The object passed in
 *
 * @note 原型参数非对象时置为 `NULL` / The prototype becomes `NULL` when the argument is not an object
 */
static JsValue object_set_prototype_of(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsValue v = arg_at(a, argc, 0);
    JsValue p = arg_at(a, argc, 1);
    HPE_UNUSED(js);
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    if (v.t == JS_OBJ && v.obj)
        v.obj->proto = (p.t == JS_OBJ) ? p.obj : NULL;
    return v;
}

/**
 * @brief 实现 `Object.prototype.toString`：返回 `[object 类名]` 形式 / Implement `Object.prototype.toString`: return the `[object Class]` form
 *
 * @details `undefined`/`null` 单独处理，原始值按 `String`/`Number`/`Boolean` 归类，对象取 `class_name`（`Module` 归为 `Object`）。
 *          `undefined`/`null` are handled specially, primitives map to `String`/`Number`/`Boolean`, and objects use `class_name` (`Module` collapses to `Object`).
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 形如 `[object Class]` 的字符串 / A string of the form `[object Class]`
 */
static JsValue object_this_toString(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    char buf[64];
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    if (thisv.t == JS_UNDEF)
        return js_string(js, "[object Undefined]");
    if (thisv.t == JS_NULL)
        return js_string(js, "[object Null]");
    if (thisv.t != JS_OBJ || !thisv.obj) {
        const char *cn = thisv.t == JS_STR   ? "String"
                         : thisv.t == JS_NUM ? "Number"
                         : thisv.t == JS_BOOL ? "Boolean"
                                              : "Object";
        snprintf(buf, sizeof(buf), "[object %s]", cn);
        return js_string(js, buf);
    }
    {
        const char *cn = thisv.obj->class_name ? thisv.obj->class_name : "Object";
        if (!strcmp(cn, "Module"))
            cn = "Object";
        snprintf(buf, sizeof(buf), "[object %s]", cn);
        return js_string(js, buf);
    }
}

/**
 * @brief 实现 `Object.prototype.valueOf`：返回 `this` 本身 / Implement `Object.prototype.valueOf`: return `this` as-is
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 未改动的 `thisv` / `thisv` unchanged
 */
static JsValue object_this_valueOf(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(js);
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    return thisv;
}

/**
 * @brief 实现 `Object.fromEntries`：由 `[键, 值]` 列表构造对象 / Implement `Object.fromEntries`: build an object from a list of `[key, value]` pairs
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 新建的对象 / The newly built object
 */
static JsValue object_from_entries(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *out = js_object_new(js);
    JsValue src = arg_at(a, argc, 0);
    int i, n;
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    n = js_iter_len(js, src);
    for (i = 0; i < n; i++) {
        JsValue entry = js_iter_at(js, src, i);
        JsValue k = js_iter_at(js, entry, 0);
        JsValue v = js_iter_at(js, entry, 1);
        js_obj_set(js, out, js_to_string(js, k), v);
    }
    return js_objval(out);
}

/**
 * @brief 实现 `Object.getOwnPropertyNames`：列出含不可枚举的自有键 / Implement `Object.getOwnPropertyNames`: list own keys including non-enumerable ones
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 自有属性名组成的数组 / An array of own property names
 *
 * @note 数组参数会先补上下标键，再列出显式属性 / For an array argument the index keys are listed first, followed by explicit properties
 */
static JsValue object_get_own_property_names(HpeJs *js, JsValue thisv, JsValue *a, int argc,
                                             void *ud)
{
    JsValue v = arg_at(a, argc, 0);
    JsObj *out = js_array_new(js);
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    if (v.t == JS_OBJ && v.obj) {
        int i;
        if (v.obj->kind == JS_OBJ_ARRAY) {
            int len = array_length(js, v.obj);
            for (i = 0; i < len; i++) {
                char buf[24];
                snprintf(buf, sizeof(buf), "%d", i);
                array_push_value(js, out, js_string(js, buf));
            }
        }
        for (i = 0; i < v.obj->prop_count; i++)
            array_push_value(js, out, js_string(js, v.obj->props[i].key));
    }
    return js_objval(out);
}

/**
 * @brief 实现 `Object.seal`：把对象标记为封闭 / Implement `Object.seal`: mark an object as sealed
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 被封闭的对象 / The sealed object
 *
 * @note 仅设置内部状态位，属性属性未被真正锁定 / Only an internal state flag is set; attributes are not actually locked
 */
static JsValue object_seal(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsValue v = arg_at(a, argc, 0);
    HPE_UNUSED(js);
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    if (v.t == JS_OBJ && v.obj)
        v.obj->num_val = 1; /* sealed */
    return v;
}

/**
 * @brief 实现 `Object.isSealed`：判断对象是否被封闭 / Implement `Object.isSealed`: test whether an object is sealed
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 已封闭或非对象返回 `true`，否则 `false` / `true` when sealed or not an object, otherwise `false`
 */
static JsValue object_is_sealed(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsValue v = arg_at(a, argc, 0);
    HPE_UNUSED(js);
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    return js_bool(!(v.t == JS_OBJ && v.obj) || (v.obj->num_val != 0.0));
}

/**
 * @brief 实现 `Object.isFrozen`：判断对象是否被冻结 / Implement `Object.isFrozen`: test whether an object is frozen
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 已冻结或非对象返回 `true`，否则 `false` / `true` when frozen or not an object, otherwise `false`
 */
static JsValue object_is_frozen(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsValue v = arg_at(a, argc, 0);
    HPE_UNUSED(js);
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    return js_bool(!(v.t == JS_OBJ && v.obj) || (v.obj->num_val >= 3.0));
}

/**
 * @brief 实现 `Number.prototype.valueOf`：取出 `this` 的数值 / Implement `Number.prototype.valueOf`: extract the numeric value of `this`
 *
 * @details 原始数字直接返回；包装对象读取内部 `__prim`；其余情况按数值强转。
 *          A primitive number is returned as-is; a wrapper object reads its internal `__prim`; anything else is coerced to a number.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 数值结果 / The numeric result
 */

static JsValue number_this_valueOf(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    if (thisv.t == JS_NUM)
        return thisv;
    if (thisv.t == JS_OBJ && thisv.obj) {
        JsValue prim = member_get(js, NULL, thisv, "__prim", 0);
        if (prim.t == JS_NUM)
            return prim;
    }
    return js_num(js_to_number(js, thisv));
}

/**
 * @brief 实现 `Number.prototype.toString`：按指定进制转字符串 / Implement `Number.prototype.toString`: format a number in a given radix
 *
 * @details 十进制或缺省走常规数字转字符串；`2..36` 内的整数在 `±1e15` 范围内手工按位转换，其余回退到十进制。
 *          Decimal or no radix uses the ordinary number formatter; integers within `±1e15` and a radix of `2..36` are converted digit by digit, anything else falls back to decimal.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 格式化后的字符串 / The formatted string
 */
static JsValue number_this_toString(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    double v = js_to_number(js, thisv);
    int radix = argc > 0 ? (int)js_to_number(js, a[0]) : 10;
    HPE_UNUSED(ud);
    if (radix == 10 || radix == 0)
        return js_string(js, js_num_to_string(js, v));
    if (radix >= 2 && radix <= 36 && v == floor(v) && v >= -1e15 && v < 1e15) {
        static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
        char buf[80];
        int o = 0, neg = v < 0;
        unsigned long long u = (unsigned long long)(neg ? -v : v);
        if (u == 0)
            buf[o++] = '0';
        while (u) {
            buf[o++] = digits[u % (unsigned)radix];
            u /= (unsigned)radix;
        }
        if (neg)
            buf[o++] = '-';
        {
            int i;
            for (i = 0; i < o / 2; i++) {
                char t = buf[i];
                buf[i] = buf[o - 1 - i];
                buf[o - 1 - i] = t;
            }
        }
        buf[o] = '\0';
        return js_string_len(js, buf, (size_t)o);
    }
    return js_string(js, js_num_to_string(js, v));
}

/**
 * @brief 实现 `Number.prototype.toFixed`：保留指定小数位 / Implement `Number.prototype.toFixed`: format with a fixed number of fraction digits
 *
 * @details 小数位数裁剪到 `0..20`；`NaN` 与无穷回退到常规数字字符串。
 *          The fraction-digit count is clamped to `0..20`; `NaN` and infinities fall back to the ordinary number string.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 定点格式的字符串 / The fixed-point formatted string
 */
static JsValue number_toFixed(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    double v = js_to_number(js, thisv);
    int digits = argc > 0 ? (int)js_to_number(js, a[0]) : 0;
    char buf[128];
    HPE_UNUSED(ud);
    if (digits < 0)
        digits = 0;
    if (digits > 20)
        digits = 20;
    if (v != v || isinf(v))
        return js_string(js, js_num_to_string(js, v));
    snprintf(buf, sizeof(buf), "%.*f", digits, v);
    return js_string(js, buf);
}

/**
 * @brief 实现 `Number.prototype.toPrecision`：按有效数字位格式化 / Implement `Number.prototype.toPrecision`: format with a given number of significant digits
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 格式化后的字符串；未给参数时等同常规数字字符串 / The formatted string, or the ordinary number string when no argument is given
 */
static JsValue number_toPrecision(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    double v = js_to_number(js, thisv);
    char buf[128];
    HPE_UNUSED(ud);
    if (argc == 0 || a[0].t == JS_UNDEF)
        return js_string(js, js_num_to_string(js, v));
    snprintf(buf, sizeof(buf), "%.*g", (int)js_to_number(js, a[0]), v);
    return js_string(js, buf);
}

/**
 * @brief Number.prototype.toExponential：把数字格式化为科学计数法 / Number.prototype.toExponential: format a number in exponential notation
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 被转换的数字接收者 / The numeric receiver being converted
 * @param[in,out] a 实参数组，`a[0]` 为小数位数 / Argument array; `a[0]` is the fraction-digit count
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 格式化后的字符串 / The formatted string
 *
 * @note 数字为 NaN 或 ±Infinity 时回退到 js_num_to_string；未给位数时用 `%e` 默认精度 / NaN and ±Infinity fall back to js_num_to_string; without a digit count the default `%e` precision is used
 */
static JsValue number_toExponential(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    double v = js_to_number(js, thisv);
    char buf[128];
    HPE_UNUSED(ud);
    if (v != v || isinf(v))
        return js_string(js, js_num_to_string(js, v));
    if (argc > 0 && a[0].t != JS_UNDEF)
        snprintf(buf, sizeof(buf), "%.*e", (int)js_to_number(js, a[0]), v);
    else
        snprintf(buf, sizeof(buf), "%e", v);
    return js_string(js, buf);
}

/**
 * @brief Number.prototype.toLocaleString：按本地化形式返回数字（无 Intl，等于普通数值串）/ Number.prototype.toLocaleString: return the number in locale form (no Intl, so it is the plain numeric string)
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 实参数组，忽略 / Argument array; ignored
 * @param[in]     argc 实参个数，忽略 / Argument count; ignored
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 数字的普通字符串形式 / The number as a plain string
 *
 * @note 引擎未实现 Intl，不做千分位分组 / The engine has no Intl database and performs no grouping
 */
static JsValue number_toLocaleString(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    double v = js_to_number(js, thisv);
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    return js_string(js, js_num_to_string(js, v));
}

/**
 * @brief Number.isInteger：判断参数是否为整数值 / Number.isInteger: test whether the argument is an integer value
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 忽略 / Ignored
 * @param[in,out] a 实参数组，`a[0]` 为待测值 / Argument array; `a[0]` is the value under test
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 是整数值返回 true / true when the value is an integer
 *
 * @note 仅当类型为 `number`、非 NaN、非 ±Infinity 且 `floor(v)==v` 时为真 / True only when the type is `number`, not NaN, not ±Infinity and `floor(v)==v`
 */

static JsValue number_is_integer(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsValue v = arg_at(a, argc, 0);
    HPE_UNUSED(js);
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    return js_bool(v.t == JS_NUM && v.num == v.num && v.num != INFINITY && v.num != -INFINITY &&
                   floor(v.num) == v.num);
}

/**
 * @brief Number.isFinite：判断参数是否为有限数字 / Number.isFinite: test whether the argument is a finite number
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 忽略 / Ignored
 * @param[in,out] a 实参数组，`a[0]` 为待测值 / Argument array; `a[0]` is the value under test
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 是有限数字返回 true / true when the value is a finite number
 *
 * @note 与全局 isFinite 不同，不做类型转换：非 `number` 一律为假 / Unlike the global isFinite this performs no coercion: anything that is not a `number` is false
 */
static JsValue number_is_finite_fn(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsValue v = arg_at(a, argc, 0);
    HPE_UNUSED(js);
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    return js_bool(v.t == JS_NUM && v.num == v.num && v.num != INFINITY && v.num != -INFINITY);
}

/**
 * @brief Number.isNaN：判断参数是否为数值 NaN / Number.isNaN: test whether the argument is the numeric NaN
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 忽略 / Ignored
 * @param[in,out] a 实参数组，`a[0]` 为待测值 / Argument array; `a[0]` is the value under test
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 是 NaN 返回 true / true when the value is NaN
 *
 * @note 仅当类型为 `number` 且 `v != v` 时为真，不做类型转换 / True only when the type is `number` and `v != v`, with no coercion
 */
static JsValue number_is_nan_fn(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsValue v = arg_at(a, argc, 0);
    HPE_UNUSED(js);
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    return js_bool(v.t == JS_NUM && v.num != v.num);
}

/**
 * @brief Number.isSafeInteger：判断参数是否为安全整数 / Number.isSafeInteger: test whether the argument is a safe integer
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 忽略 / Ignored
 * @param[in,out] a 实参数组，`a[0]` 为待测值 / Argument array; `a[0]` is the value under test
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 是安全整数返回 true / true when the value is a safe integer
 *
 * @note 要求类型为 `number`、`floor(v)==v` 且 |v| ≤ 2^53-1 / Requires type `number`, `floor(v)==v` and |v| ≤ 2^53-1
 */
static JsValue number_is_safe_integer(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsValue v = arg_at(a, argc, 0);
    HPE_UNUSED(js);
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    return js_bool(v.t == JS_NUM && floor(v.num) == v.num && fabs(v.num) <= 9007199254740991.0);
}

static JsValue number_this_toString_radix(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud);

/**
 * @brief Boolean.prototype.valueOf：取布尔接收者的原始布尔值 / Boolean.prototype.valueOf: return the primitive boolean of the receiver
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 实参数组，忽略 / Argument array; ignored
 * @param[in]     argc 实参个数，忽略 / Argument count; ignored
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 原始布尔值 / The primitive boolean value
 *
 * @note 接收者已是布尔则直接返回；是对象时取内部 `__prim`；否则对接收者做数值转换后判非零 / Returns the receiver when already boolean, reads the internal `__prim` for objects, otherwise coerces the receiver to a number and tests it against zero
 */
static JsValue bool_this_valueOf(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(ud);
    if (thisv.t == JS_BOOL)
        return thisv;
    if (thisv.t == JS_OBJ && thisv.obj) {
        JsValue prim = member_get(js, NULL, thisv, "__prim", 0);
        if (prim.t == JS_BOOL)
            return prim;
    }
    return js_bool(js_to_number(js, thisv) != 0.0);
}

/**
 * @brief Boolean.prototype.toString：返回 `true` 或 `false` / Boolean.prototype.toString: return `true` or `false`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 实参数组，忽略 / Argument array; ignored
 * @param[in]     argc 实参个数，忽略 / Argument count; ignored
 * @param[in,out] ud 未使用 / Unused
 *
 * @return `true` 或 `false` 字符串 / The string `true` or `false`
 *
 * @note 基于 js_truthy 判定接收者的真值 / The receiver's truthiness is decided by js_truthy
 */
static JsValue bool_this_toString(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(js);
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    return js_string(js, js_truthy(thisv) ? "true" : "false");
}

/**
 * @brief Number.prototype.toString 的基数版本（占位实现）/ Radix-aware Number.prototype.toString (placeholder implementation)
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 忽略 / Ignored
 * @param[in,out] a 实参数组，忽略 / Argument array; ignored
 * @param[in]     argc 实参个数，忽略 / Argument count; ignored
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 目前固定返回 `undefined` / Currently always returns `undefined`
 *
 * @note 该版本尚未实现，所有入参均被忽略 / This version is not implemented yet and ignores all arguments
 */
static JsValue number_this_toString_radix(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(js);
    HPE_UNUSED(thisv);
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    return js_undef();
}

/* ---------------- JSON ---------------- */

typedef struct {
    char *s;
    size_t len;
    size_t cap;
} JSBuf;

/**
 * @brief 初始化字符串输出缓冲 / Initialise a string output buffer
 *
 * @details 容量固定为 256 字节并分配堆内存，`len` 清零，缓冲区置为空串 / The capacity starts at 256 bytes, heap memory is allocated, `len` is zeroed and the buffer is set to the empty string.
 *
 * @param[in,out] b 待初始化的字符串缓冲 / String buffer to initialise
 *
 * @note 使用完毕必须调用 jsbuf_free 释放 / The caller must release it with jsbuf_free
 */
static void jsbuf_init(JSBuf *b)
{
    b->cap = 256;
    b->len = 0;
    b->s = (char *)malloc(b->cap);
    if (b->s)
        b->s[0] = '\0';
}

/**
 * @brief 向字符串缓冲追加指定字节数 / Append bytes to a string buffer
 *
 * @details 缓冲不足时容量反复翻倍再 realloc，随后拷贝 `n` 字节并补写结尾 NUL / When the buffer is too small the capacity is doubled repeatedly and realloc'd, then `n` bytes are copied and a trailing NUL is written.
 *
 * @param[in,out] b 目标字符串缓冲 / Target string buffer
 * @param[in]     s 要追加的字节 / Bytes to append
 * @param[in]     n 要追加的字节数（可为 0）/ Number of bytes to append (may be 0)
 *
 * @note 缓冲未成功分配（`b->s` 为 `NULL`）时直接返回 / Returns immediately when the buffer was never allocated (`b->s` is `NULL`)
 */
static void jsbuf_add(JSBuf *b, const char *s, size_t n)
{
    if (!b->s)
        return;
    if (b->len + n + 1 > b->cap) {
        while (b->len + n + 1 > b->cap)
            b->cap *= 2;
        b->s = (char *)realloc(b->s, b->cap);
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = '\0';
}

/**
 * @brief 释放字符串输出缓冲 / Free a string output buffer
 *
 * @param[in,out] b 要释放的字符串缓冲 / String buffer to release
 *
 * @note 释放后把 `b->s` 置为 `NULL`，可安全重复调用 / After freeing, `b->s` is set to `NULL` so repeated calls are safe
 */
static void jsbuf_free(JSBuf *b)
{
    free(b->s);
    b->s = NULL;
}

/**
 * @brief 按 JSON 语法给字符串加引号并转义 / Quote and escape a string per the JSON grammar
 *
 * @details 处理 `"`、`\` 与 `\n \r \t \b \f` 等控制字符；其余小于 0x20 的字节转成 `\u00xx`，高位 UTF-8 字节原样输出 / Handles `"`, `\` and the control escapes `\n \r \t \b \f`; other bytes below 0x20 become `\u00xx` and multi-byte UTF-8 is copied verbatim.
 *
 * @param[in,out] b JSON 输出缓冲 / JSON output buffer
 * @param[in]     s 要转义的字符串，`NULL` 视作空串 / String to escape; `NULL` is treated as the empty string
 */
static void json_quote(JSBuf *b, const char *s)
{
    const unsigned char *p = (const unsigned char *)(s ? s : "");
    jsbuf_add(b, "\"", 1);
    for (; *p; p++) {
        switch (*p) {
        case '"':
            jsbuf_add(b, "\\\"", 2);
            break;
        case '\\':
            jsbuf_add(b, "\\\\", 2);
            break;
        case '\n':
            jsbuf_add(b, "\\n", 2);
            break;
        case '\r':
            jsbuf_add(b, "\\r", 2);
            break;
        case '\t':
            jsbuf_add(b, "\\t", 2);
            break;
        case '\b':
            jsbuf_add(b, "\\b", 2);
            break;
        case '\f':
            jsbuf_add(b, "\\f", 2);
            break;
        default:
            if (*p < 0x20) {
                char tmp[8];
                snprintf(tmp, sizeof(tmp), "\\u%04x", *p);
                jsbuf_add(b, tmp, 6);
            } else {
                jsbuf_add(b, (const char *)p, 1);
            }
        }
    }
    jsbuf_add(b, "\"", 1);
}

/**
 * @brief 判断值是否为函数或原生对象 / Test whether a value is a function or a native object
 *
 * @param[in]     v 待检查的值 / Value to inspect
 *
 * @return 是函数或原生对象返回 1，否则返回 0 / 1 for a function or native object, otherwise 0
 *
 * @note 这类值不可序列化，JSON.stringify 时按 `null` 处理 / Such values are not serialisable and become `null` in JSON.stringify
 */
static int json_is_function(JsValue v)
{
    return v.t == JS_OBJ && v.obj &&
           (v.obj->kind == JS_OBJ_FUNC || v.obj->kind == JS_OBJ_NATIVE);
}

/* recursive value writer; forward-declared for the container helpers below */
static int json_write(HpeJs *js, JSBuf *b, JsValue v, int depth);

/**
 * @brief 把数组序列化为 `[e, ...]` / Serialise an array as `[e, ...]`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] b JSON 输出缓冲 / JSON output buffer
 * @param[in,out] o 要序列化的数组对象 / The array object to serialise
 * @param[in]     depth 当前递归深度 / Current recursion depth
 *
 * @note 按索引逐个写出，包括空洞 / Elements are written index by index, holes included
 */
static void json_write_array(HpeJs *js, JSBuf *b, JsObj *o, int depth)
{
    int len = array_length(js, o), i;

    jsbuf_add(b, "[", 1);
    for (i = 0; i < len; i++) {
        if (i)
            jsbuf_add(b, ",", 1);
        json_write(js, b, array_get_index(js, o, i), depth + 1);
    }
    jsbuf_add(b, "]", 1);
}

/**
 * @brief 把对象序列化为 `{"k":v, ...}` / Serialise an object as `{"k":v, ...}`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] b JSON 输出缓冲 / JSON output buffer
 * @param[in]     v 被序列化的对象值 / The object value being serialised
 * @param[in,out] o 对象本体 / The underlying object
 * @param[in]     depth 当前递归深度 / Current recursion depth
 *
 * @note 遍历自有键，跳过值为 `undefined` 或函数的属性 / Walks own keys and skips properties whose value is `undefined` or a function
 */
static void json_write_object(HpeJs *js, JSBuf *b, JsValue v, JsObj *o, int depth)
{
    char **keys;
    int nk, i, first = 1;

    js_obj_own_keys(o, &keys, &nk);
    jsbuf_add(b, "{", 1);
    for (i = 0; i < nk; i++) {
        JsValue pv = member_get(js, NULL, v, keys[i], 0);

        if (pv.t == JS_UNDEF || json_is_function(pv))
            continue;
        if (!first)
            jsbuf_add(b, ",", 1);
        first = 0;
        json_quote(b, keys[i]);
        jsbuf_add(b, ":", 1);
        json_write(js, b, pv, depth + 1);
    }
    free(keys);
    jsbuf_add(b, "}", 1);
}

/**
 * @brief 把 Date 实例序列化为其 toJSON() 结果 / Serialise a Date instance as its toJSON() result
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] b JSON 输出缓冲 / JSON output buffer
 * @param[in]     v Date 值 / The Date value
 * @param[in]     depth 当前递归深度 / Current recursion depth
 *
 * @return 成功的状态码，透传自 json_write / A status code propagated from json_write
 *
 * @note 存在可调用的 `toJSON` 时调用并继续写出，否则直接输出 `js_to_string(v)` 的带引号形式 / When a callable `toJSON` exists it is invoked and its result written; otherwise the quoted `js_to_string(v)` is emitted
 */
static int json_write_date(HpeJs *js, JSBuf *b, JsValue v, int depth)
{
    JsValue j = member_get(js, NULL, v, "toJSON", 0);

    if (js_is_callable(j))
        return json_write(js, b, call_value(js, NULL, j, v, NULL, 0, 0), depth + 1);
    json_quote(b, js_to_string(js, v));
    return 1;
}

/**
 * @brief 递归写出任意值的 JSON 表示 / Recursively write the JSON representation of any value
 *
 * @details `undefined`/`null` 写 `null`；布尔写 `true`/`false`；数字写数值串，NaN 与 ±Infinity 写 `null`；字符串加引号；对象按数组、函数、Date、普通对象分派；其他类型统一写 `null` / `undefined`/`null` become `null`; booleans become `true`/`false`; numbers are written as numeric strings with NaN and ±Infinity as `null`; strings are quoted; objects dispatch to array, function, Date or plain-object handling; everything else writes `null`.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] b JSON 输出缓冲 / JSON output buffer
 * @param[in]     v 待序列化的值 / Value to serialise
 * @param[in]     depth 当前递归深度 / Current recursion depth
 *
 * @return 写出成功返回 1；深度超过 64 层返回 0 以中断循环引用 / 1 on success; returns 0 once the depth exceeds 64 to break out of circular references
 * @retval     0 递归过深，判定为循环结构 / Recursion too deep, treated as a circular structure
 * @retval     1 已写出 / Written
 */
static int json_write(HpeJs *js, JSBuf *b, JsValue v, int depth)
{
    if (depth > 64)
        return 0;
    switch (v.t) {
    case JS_UNDEF:
    case JS_NULL:
        jsbuf_add(b, "null", 4);
        return 1;
    case JS_BOOL: {
        const char *t = v.num != 0.0 ? "true" : "false";
        jsbuf_add(b, t, strlen(t));
        return 1;
    }
    case JS_NUM:
        if (v.num != v.num || isinf(v.num)) {
            jsbuf_add(b, "null", 4); /* NaN and Infinity are not valid JSON */
            return 1;
        }
        {
            char *s = js_num_to_string(js, v.num);
            jsbuf_add(b, s, strlen(s));
        }
        return 1;
    case JS_STR:
        json_quote(b, v.str);
        return 1;
    case JS_OBJ: {
        JsObj *o = v.obj;

        if (!o)
            break;
        if (o->kind == JS_OBJ_ARRAY) {
            json_write_array(js, b, o, depth);
            return 1;
        }
        if (json_is_function(v))
            break;
        if (o->kind == JS_OBJ_DATE)
            return json_write_date(js, b, v, depth);
        json_write_object(js, b, v, o, depth);
        return 1;
    }
    default:
        break;
    }
    jsbuf_add(b, "null", 4);
    return 1;
}

/**
 * @brief JSON.stringify：把首个参数序列化为 JSON 字符串 / JSON.stringify: serialise the first argument to a JSON string
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 忽略 / Ignored
 * @param[in,out] a 实参数组，`a[0]` 为待序列化值 / Argument array; `a[0]` is the value to serialise
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 未使用 / Unused
 *
 * @return JSON 字符串；参数为 `undefined` 时返回 `undefined` / The JSON string, or `undefined` when the argument is `undefined`
 *
 * @note 检测到循环结构（json_write 返回 0）时抛出 “Converting circular structure to JSON” / Throws “Converting circular structure to JSON” when json_write signals a circular structure with 0
 */
static JsValue json_stringify(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JSBuf b;
    JsValue v = arg_at(a, argc, 0);
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    if (v.t == JS_UNDEF)
        return js_undef();
    jsbuf_init(&b);
    if (!json_write(js, &b, v, 0)) {
        jsbuf_free(&b);
        js_error(js, 0, "Converting circular structure to JSON");
    }
    {
        JsValue out = js_string_len(js, b.s ? b.s : "", b.len);
        jsbuf_free(&b);
        return out;
    }
}

/* ---------------- JSON.parse */

typedef struct {
    HpeJs *js;
    const char *p;
} JsonP;

/**
 * @brief 跳过 JSON 文本中的空白字符 / Skip whitespace in the JSON text
 *
 * @param[in,out] j JSON 解析游标 / JSON parser cursor
 *
 * @note 空白包括空格、制表、换行与回车 / Whitespace covers space, tab, newline and carriage return
 */
static void json_ws(JsonP *j)
{
    while (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')
        j->p++;
}

static JsValue json_parse_value(JsonP *j);

/**
 * @brief 把 JSON 解码临时缓冲容量翻倍 / Double the capacity of the JSON decode scratch buffer
 *
 * @param[in,out] j JSON 解析游标 / JSON parser cursor
 * @param[in,out] buf 解码缓冲指针 / Decode scratch buffer pointer
 * @param[in,out] cap 缓冲容量 / Buffer capacity
 * @param[in]     n 已写入的字节数，拷贝时保留 / Bytes already written, preserved by the copy
 */
static void json_str_grow(JsonP *j, char **buf, size_t *cap, size_t n)
{
    char *nb = (char *)hpe_js_alloc(j->js, *cap * 2);
    memcpy(nb, *buf, n);
    *buf = nb;
    *cap *= 2;
}

/**
 * @brief 把 `\uXXXX` 转义解码为 UTF-8 / Decode a `\uXXXX` escape to UTF-8
 *
 * @details 最多读取四个十六进制数字并推进游标，随后把码点编码为 UTF-8 追加到缓冲 / Reads up to four hex digits while advancing the cursor, then appends the UTF-8 encoding of the code point.
 *
 * @param[in,out] j JSON 解析游标 / JSON parser cursor
 * @param[in,out] buf 解码缓冲指针 / Decode scratch buffer pointer
 * @param[in,out] cap 缓冲容量 / Buffer capacity
 * @param[in,out] n 已写入字节计数 / Bytes-written counter
 *
 * @note 代理对不做合并，单个 `\uD800` 会原样单独编码 / Surrogate pairs are not combined; a lone `\uD800` is encoded on its own
 */
static void json_str_unicode(JsonP *j, char **buf, size_t *cap, size_t *n)
{
    unsigned v = 0;
    int k;
    for (k = 0; k < 4 && *j->p; k++, j->p++) {
        int d = uri_hex((unsigned char)*j->p);
        if (d < 0)
            break;
        v = v * 16 + (unsigned)d;
    }
    if (*n + 6 > *cap)
        json_str_grow(j, buf, cap, *n);
    *n += (size_t)hpe_utf8_encode(v, *buf + *n);
}

/**
 * @brief 解析单字符转义序列 / Resolve a single-character escape sequence
 *
 * @param[in]     e 反斜杠后的转义字符 / The escape character following the backslash
 * @param[in]     c 转义字符本身，作为默认回退 / The escape character itself, used as the fallback
 *
 * @return 转义对应的实际字符 / The actual character the escape stands for
 *
 * @note 只识别 `n t r b f`；其余情况原样返回第二个参数，配合宽容解析器不报错 / Only `n t r b f` are recognised; anything else returns the second argument unchanged so the tolerant parser never rejects it
 */
static char json_escape_simple(char e, char c)
{
    switch (e) {
    case 'n':
        return '\n';
    case 't':
        return '\t';
    case 'r':
        return '\r';
    case 'b':
        return '\b';
    case 'f':
        return '\f';
    default:
        return c;
    }
}

/**
 * @brief 解析一个 JSON 字符串字面量 / Parse one JSON string literal
 *
 * @param[in,out] j JSON 解析游标 / JSON parser cursor
 *
 * @return 解析出的 JS 字符串 / The parsed JS string
 *
 * @note 处理 `\uXXXX` 与单字符转义；未闭合引号时以当前缓冲内容结束 / Handles `\uXXXX` and single-character escapes; unterminated quotes end at the current buffer content
 */
static JsValue json_parse_string(JsonP *j)
{
    char *buf;
    size_t cap = 64, n = 0;
    j->p++; /* opening quote */
    buf = (char *)hpe_js_alloc(j->js, cap);
    while (*j->p && *j->p != '"') {
        char c = *j->p++;
        if (c == '\\' && *j->p) {
            char e = *j->p++;
            if (e == 'u') {
                json_str_unicode(j, &buf, &cap, &n);
                continue;
            }
            c = json_escape_simple(e, c);
        }
        if (n + 2 > cap)
            json_str_grow(j, &buf, &cap, n);
        buf[n++] = c;
    }
    if (*j->p == '"')
        j->p++;
    buf[n] = '\0';
    return js_string_len(j->js, buf, n);
}

/**
 * @brief 解析一个 JSON 数组 / Parse a JSON array
 *
 * @param[in,out] j JSON 解析游标 / JSON parser cursor
 *
 * @return 解析出的 JS 数组对象 / The parsed JS array object
 *
 * @note 元素之间以 `,` 分隔，遇到 `]` 结束 / Elements are separated by `,` and terminated by `]`
 */
static JsValue json_parse_array(JsonP *j)
{
    JsObj *arr = js_array_new(j->js);
    j->p++; /* [ */
    json_ws(j);
    if (*j->p == ']') {
        j->p++;
        return js_objval(arr);
    }
    for (;;) {
        array_push_value(j->js, arr, json_parse_value(j));
        json_ws(j);
        if (*j->p == ',') {
            j->p++;
            json_ws(j);
            continue;
        }
        if (*j->p == ']')
            j->p++;
        break;
    }
    return js_objval(arr);
}

/**
 * @brief 解析一个 JSON 对象 / Parse a JSON object
 *
 * @param[in,out] j JSON 解析游标 / JSON parser cursor
 *
 * @return 解析出的 JS 普通对象 / The parsed JS plain object
 *
 * @note 键必须是带引号的字符串，否则抛出 “JSON.parse: expected a property name” / Keys must be quoted strings, otherwise “JSON.parse: expected a property name” is thrown
 */
static JsValue json_parse_object(JsonP *j)
{
    JsObj *o = js_object_new(j->js);
    j->p++; /* { */
    json_ws(j);
    if (*j->p == '}') {
        j->p++;
        return js_objval(o);
    }
    for (;;) {
        JsValue key;
        JsValue v;
        json_ws(j);
        if (*j->p != '"')
            js_error(j->js, 0, "JSON.parse: expected a property name");
        key = json_parse_string(j);
        json_ws(j);
        if (*j->p == ':')
            j->p++;
        json_ws(j);
        v = json_parse_value(j);
        js_obj_set(j->js, o, key.str, v);
        json_ws(j);
        if (*j->p == ',') {
            j->p++;
            continue;
        }
        if (*j->p == '}')
            j->p++;
        break;
    }
    return js_objval(o);
}

/**
 * @brief 递归下降解析一个 JSON 值 / Recursively parse one JSON value
 *
 * @details 先跳过空白，再按首字符分派：`{`→对象、`[`→数组、`"`→字符串、`true`/`false`/`null` 字面量，其余交给 `strtod` 解析数字（含负数与小数）/ Leading whitespace is skipped, then the first character dispatches: `{` to object, `[` to array, `"` to string, the `true`/`false`/`null` literals, and everything else to `strtod` for numbers (including negatives and fractions).
 *
 * @param[in,out] j JSON 解析游标 / JSON parser cursor
 *
 * @return 解析出的 JS 值 / The parsed JS value
 *
 * @note 数字无法解析时抛出 “JSON.parse: unexpected token”；本函数是整个 JSON.parse 的递归入口，构成对象/数组的互相递归 / Throws “JSON.parse: unexpected token” when the number cannot be parsed; this is the recursion entry point of the whole JSON.parse and mutually recurses with the object and array parsers
 */
static JsValue json_parse_value(JsonP *j)
{
    json_ws(j);
    if (*j->p == '{')
        return json_parse_object(j);
    if (*j->p == '[')
        return json_parse_array(j);
    if (*j->p == '"')
        return json_parse_string(j);
    if (!strncmp(j->p, "true", 4)) {
        j->p += 4;
        return js_bool(1);
    }
    if (!strncmp(j->p, "false", 5)) {
        j->p += 5;
        return js_bool(0);
    }
    if (!strncmp(j->p, "null", 4)) {
        j->p += 4;
        return js_null();
    }
    {
        char *end = NULL;
        double d = strtod(j->p, &end);
        if (end == j->p)
            js_error(j->js, 0, "JSON.parse: unexpected token");
        j->p = end;
        return js_num(d);
    }
}

/**
 * @brief JSON.parse：把首个参数解析为 JS 值 / JSON.parse: parse the first argument into a JS value
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 忽略 / Ignored
 * @param[in,out] a 实参数组，`a[0]` 为待解析文本 / Argument array; `a[0]` is the text to parse
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 解析得到的 JS 值 / The parsed JS value
 *
 * @note 入参先做字符串转换，随后交给 json_parse_value / The argument is coerced to a string before being handed to json_parse_value
 */
static JsValue json_parse(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsonP j;
    JsValue v;
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    j.js = js;
    j.p = js_to_string(js, arg_at(a, argc, 0));
    v = json_parse_value(&j);
    return v;
}

/**
 * @brief 构造一个 RegExp 对象 / Create a RegExp object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     pattern 模式串，`NULL` 视作空串 / Pattern string; `NULL` is treated as the empty string
 * @param[in]     flags 标志串，`NULL` 视作空串 / Flag string; `NULL` is treated as the empty string
 *
 * @return 新建的 RegExp 值 / The newly created RegExp value
 *
 * @note 编译模式、设置标志，并写入 `source`、`flags`、`lastIndex` 属性 / Compiles the pattern, applies the flags and installs the `source`, `flags` and `lastIndex` properties
 */

static JsValue regex_obj_new(HpeJs *js, const char *pattern, const char *flags)
{
    JsObj *o = js_object_raw(js);
    o->kind = JS_OBJ_REGEX;
    o->class_name = "RegExp";
    o->proto = js->regex_proto;
    o->payload = hpe_regex_compile(js, pattern ? pattern : "");
    hpe_regex_set_flags((HpeRegex *)o->payload, flags);
    js_obj_set(js, o, "source", js_string(js, pattern ? pattern : ""));
    js_obj_set(js, o, "flags", js_string(js, flags ? flags : ""));
    js_obj_set(js, o, "lastIndex", js_num(0.0));
    return js_objval(o);
}

/* capture buffer shared by RegExp#exec/test and String#match/replace/split */

static int regex_is_global(JsValue thisv){
    JsValue fl;
    const char *s;
    if (thisv.t != JS_OBJ || !thisv.obj)
        return 0;
    fl = js_get_prop(NULL, NULL, thisv, "flags", 0);
    if (fl.t != JS_STR || !fl.str)
        return 0;
    for (s = fl.str; *s; s++)
        if (*s == 'g')
            return 1;
    return 0;
}

/**
 * @brief 读取正则对象的 lastIndex / Read a regex object's lastIndex
 *
 * @param[in]     thisv 正则对象 / The regex object
 *
 * @return 非负的 lastIndex 整数（缺省或非正时为 0）/ The non-negative lastIndex as an int, or 0 when missing or not positive
 */
static int regex_last_index(JsValue thisv)
{
    JsValue li;
    double d;
    if (thisv.t != JS_OBJ || !thisv.obj)
        return 0;
    li = js_get_prop(NULL, NULL, thisv, "lastIndex", 0);
    d = li.t == JS_NUM ? li.num : 0.0;
    return d > 0 ? (int)d : 0;
}

/**
 * @brief 写回正则对象的 lastIndex / Write back a regex object's lastIndex
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 正则对象 / The regex object
 * @param[in]     v 新的 lastIndex 值 / The new lastIndex value
 *
 * @note 接收者不是对象时不做任何处理 / Does nothing when the receiver is not an object
 */
static void regex_set_last_index(HpeJs *js, JsValue thisv, int v)
{
    if (thisv.t == JS_OBJ && thisv.obj)
        js_obj_set(js, thisv.obj, "lastIndex", js_num((double)v));
}

/**
 * @brief 由捕获下标构造正则匹配结果数组 / Build a regex match-result array from capture indices
 *
 * @details 首元素为整体匹配子串；其后每个捕获组对应一个元素（未参与匹配时为 `undefined`）；再附加 `index`、`input`、`groups` 属性 / The first element is the whole match, each capture group follows (as `undefined` when unmatched), and `index`, `input` and `groups` are attached as extra properties.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     s 被匹配的输入字符串 / The input string that was matched
 * @param[in,out] caps 捕获下标数组，`caps[0]`/`caps[1]` 为整体匹配起止 / Capture index array; `caps[0]`/`caps[1]` bound the whole match
 * @param[in]     total 捕获组个数 / Number of capture groups
 *
 * @return 匹配结果数组 / The match-result array
 *
 * @note `caps[0]` 被钳制到 ≥0，`caps[1]` 被钳制到字符串长度 / `caps[0]` is clamped to ≥0 and `caps[1]` to the string length
 */
static JsValue regex_make_result(HpeJs *js, const char *s, int *caps, int total)
{
    JsObj *out = js_array_new(js);
    size_t sl = strlen(s);
    int g;
    if (caps[0] < 0)
        caps[0] = 0;
    if (caps[1] < 0 || caps[1] > (int)sl)
        caps[1] = (int)sl;
    array_push_value(js, out, js_string_len(js, s + caps[0], (size_t)(caps[1] - caps[0])));
    for (g = 0; g < total; g++) {
        int bi = 2 + g * 2, ei = bi + 1;
        if (bi >= RE_CAPS || caps[bi] < 0 || caps[ei] < caps[bi])
            array_push_value(js, out, js_undef());
        else
            array_push_value(js, out,
                             js_string_len(js, s + caps[bi], (size_t)(caps[ei] - caps[bi])));
    }
    js_obj_set(js, out, "index", js_num((double)caps[0]));
    js_obj_set(js, out, "input", js_string(js, s));
    js_obj_set(js, out, "groups", js_undef());
    return js_objval(out);
}

/**
 * @brief RegExp.prototype.test：测试输入是否含匹配 / RegExp.prototype.test: test whether the input contains a match
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 正则对象接收者 / The regex receiver
 * @param[in,out] a 实参数组，`a[0]` 为被测试文本 / Argument array; `a[0]` is the text under test
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 存在匹配返回 true，否则 false / true when a match exists, otherwise false
 *
 * @note 全局正则从 lastIndex 起搜，失败时把 lastIndex 复位为 0，成功时更新为匹配起点 / A global regex searches from lastIndex, resets it to 0 on failure and stores the match start on success
 */
static JsValue regex_test(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HpeRegex *re = (thisv.t == JS_OBJ && thisv.obj) ? (HpeRegex *)thisv.obj->payload : NULL;
    char *s = js_to_string(js, arg_at(a, argc, 0));
    int caps[RE_CAPS];
    int start, m;
    int global = regex_is_global(thisv);
    HPE_UNUSED(ud);
    if (!re)
        return js_bool(0);
    start = global ? regex_last_index(thisv) : 0;
    m = hpe_regex_search(re, s, start, caps, RE_CAPS);
    if (m < 0) {
        if (global)
            regex_set_last_index(js, thisv, 0);
        return js_bool(0);
    }
    if (global)
        regex_set_last_index(js, thisv, m);
    return js_bool(1);
}

/**
 * @brief RegExp.prototype.exec：执行一次匹配并返回结果 / RegExp.prototype.exec: perform one match and return the result
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 正则对象接收者 / The regex receiver
 * @param[in,out] a 实参数组，`a[0]` 为输入文本 / Argument array; `a[0]` is the input text
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 匹配结果数组；未匹配返回 `null` / The match-result array, or `null` when there is no match
 *
 * @note 全局正则维护并更新 lastIndex；结果由 regex_make_result 生成 / A global regex maintains and updates lastIndex; the result is produced by regex_make_result
 */
static JsValue regex_exec(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HpeRegex *re = (thisv.t == JS_OBJ && thisv.obj) ? (HpeRegex *)thisv.obj->payload : NULL;
    char *s = js_to_string(js, arg_at(a, argc, 0));
    int caps[RE_CAPS];
    int start, m;
    int global = regex_is_global(thisv);
    HPE_UNUSED(ud);
    if (!re)
        return js_null();
    start = global ? regex_last_index(thisv) : 0;
    m = hpe_regex_search(re, s, start, caps, RE_CAPS);
    if (m < 0) {
        if (global)
            regex_set_last_index(js, thisv, 0);
        return js_null();
    }
    if (global)
        regex_set_last_index(js, thisv, m);
    return regex_make_result(js, s, caps, hpe_regex_group_count(re));
}

/**
 * @brief 由正则字面量创建 RegExp 对象 / Create a RegExp object from a regex literal
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     pattern 模式串 / Pattern string
 * @param[in]     flags 标志串 / Flag string
 *
 * @return 与 `new RegExp` 等价的 RegExp 值 / A RegExp value equivalent to `new RegExp`
 *
 * @note 内部直接转发到 regex_obj_new / Internally forwards straight to regex_obj_new
 *
 * @see regex_obj_new
 */
JsValue js_regex_new(HpeJs *js, const char *pattern, const char *flags)
{
    return regex_obj_new(js, pattern, flags);
}

/**
 * @brief RegExp.prototype.toString：返回 `/模式/标志` 形式 / RegExp.prototype.toString: return the `/pattern/flags` form
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 正则对象接收者 / The regex receiver
 * @param[in,out] a 实参数组，忽略 / Argument array; ignored
 * @param[in]     argc 实参个数，忽略 / Argument count; ignored
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 形如 `/pat/flags` 的字符串 / A string of the form `/pat/flags`
 *
 * @note `source` 与 `flags` 缺失时按空串处理 / Missing `source` and `flags` are treated as empty strings
 */
static JsValue regex_toString(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsValue src = member_get(js, NULL, thisv, "source", 0);
    JsValue fl = member_get(js, NULL, thisv, "flags", 0);
    char *pat = js_to_string(js, src.t == JS_UNDEF ? js_string(js, "") : src);
    char *flags = js_to_string(js, fl.t == JS_UNDEF ? js_string(js, "") : fl);
    size_t pl = strlen(pat), fln = strlen(flags);
    char *out = (char *)hpe_js_alloc(js, pl + fln + 3);
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    out[0] = '/';
    memcpy(out + 1, pat, pl);
    out[1 + pl] = '/';
    memcpy(out + 2 + pl, flags, fln);
    out[2 + pl + fln] = '\0';
    return js_string_len(js, out, pl + fln + 2);
}

/**
 * @brief 展开替换模板中的 `$$`、`$&`、`$1`..`$9`、``$` ``、`$'` / Expand `$$`, `$&`, `$1`..`$9`, ``$` `` and `$'` in a replacement template
 *
 * @details `$$` 产生字面 `$`；`$&` 整个匹配；`$n` 第 n 组（超出范围或未匹配则跳过）；`` $` `` 匹配前的文本；`$'` 匹配后的文本；其他 `$x` 原样输出 `$` / `$$` yields a literal `$`; `$&` is the whole match; `$n` is capture n (skipped when out of range or unmatched); `` $` `` is the text before the match; `$'` is the text after it; any other `$x` emits a literal `$`.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] b 输出缓冲 / Output buffer
 * @param[in]     rep 替换模板（以 NUL 结尾）/ The replacement template, NUL-terminated
 * @param[in]     s 被替换的原始输入 / The original input being replaced
 * @param[in,out] caps 捕获下标数组 / Capture index array
 * @param[in]     total 捕获组个数 / Number of capture groups
 *
 * @note 本函数只处理模板；回调形式的替换由 repl_append_match 处理 / This handles the template form only; callback-style replacement is handled by repl_append_match
 */
static void repl_expand(HpeJs *js, JSBuf *b, const char *rep, const char *s, int *caps, int total)
{
    size_t sl = strlen(s);
    const char *p = rep;
    while (*p) {
        if (*p != '$') {
            jsbuf_add(b, p, 1);
            p++;
            continue;
        }
        p++;
        if (*p == '$') {
            jsbuf_add(b, "$", 1);
            p++;
        } else if (*p == '&') {
            int e = caps[1] < 0 ? caps[0] : caps[1];
            jsbuf_add(b, s + caps[0], (size_t)(e - caps[0]));
            p++;
        } else if (*p == '`') {
            jsbuf_add(b, s, (size_t)caps[0]);
            p++;
        } else if (*p == '\'') {
            int e = caps[1] < 0 ? caps[0] : caps[1];
            if ((size_t)e < sl)
                jsbuf_add(b, s + e, sl - (size_t)e);
            p++;
        } else if (*p >= '1' && *p <= '9') {
            int g = *p - '0';
            int bi = g * 2; /* group g -> caps[2g]/caps[2g+1] */
            if (g <= total && bi + 1 < RE_CAPS && caps[bi] >= 0 && caps[bi + 1] >= caps[bi])
                jsbuf_add(b, s + caps[bi], (size_t)(caps[bi + 1] - caps[bi]));
            p++;
        } else {
            jsbuf_add(b, "$", 1);
        }
    }
    HPE_UNUSED(js);
}

/**
 * @brief 为替换回调构造实参数组 / Build the argument array for a replacement callback
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     s 被替换的原始输入 / The original input being replaced
 * @param[in,out] caps 捕获下标数组 / Capture index array
 * @param[in]     total 捕获组个数 / Number of capture groups
 *
 * @return 长度为 total+3 的实参数组：整体匹配、各捕获组（未匹配为 `undefined`）、偏移、输入字符串 / An argument array of length total+3: the whole match, each capture group (`undefined` when unmatched), the offset, then the input string
 */
static JsValue *repl_call_args(HpeJs *js, const char *s, int *caps, int total)
{
    JsValue *args = (JsValue *)hpe_js_alloc(js, (size_t)(total + 3) * sizeof(JsValue));
    int i;

    args[0] = js_string_len(js, s + caps[0], (size_t)(caps[1] - caps[0]));
    for (i = 0; i < total; i++) {
        int bi = 2 + i * 2, ei = bi + 1;

        args[i + 1] = (bi + 1 < RE_CAPS && caps[bi] >= 0 && caps[ei] >= caps[bi])
                          ? js_string_len(js, s + caps[bi], (size_t)(caps[ei] - caps[bi]))
                          : js_undef();
    }
    args[total + 1] = js_num((double)caps[0]);
    args[total + 2] = js_string(js, s);
    return args;
}

/**
 * @brief 把单次匹配的替换结果追加到输出缓冲 / Append the replacement for one match to the output buffer
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] b 输出缓冲 / Output buffer
 * @param[in]     rep 替换值，可为回调 / The replacement value, possibly a callback
 * @param[in]     rep_s 替换模板字符串（rep 非回调时使用）/ The replacement template string, used when `rep` is not callable
 * @param[in]     s 被替换的原始输入 / The original input being replaced
 * @param[in,out] caps 捕获下标数组 / Capture index array
 * @param[in]     total 捕获组个数 / Number of capture groups
 *
 * @note `rep` 可调用时以其返回值经字符串转换后追加，否则按模板展开 / When `rep` is callable its result is coerced to a string and appended; otherwise the template is expanded
 */
static void repl_append_match(HpeJs *js, JSBuf *b, JsValue rep, const char *rep_s, const char *s,
                              int *caps, int total)
{
    if (js_is_callable(rep)) {
        JsValue *args = repl_call_args(js, s, caps, total);
        JsValue r = call_value(js, NULL, rep, js_undef(), args, total + 3, 0);
        char *rpc = js_to_string(js, r);

        jsbuf_add(b, rpc, strlen(rpc));
    } else {
        repl_expand(js, b, rep_s, s, caps, total);
    }
}

/**
 * @brief 用正则替换：非全局只替换首个匹配 / Replace with a RegExp needle: only the first match unless global
 *
 * @details 每次搜索后写入匹配前的文本、追加替换结果；全局正则会持续前进，空匹配时前移一个字节以推进 / Before each match the preceding text is copied and the replacement appended; a global regex keeps advancing, and an empty match moves forward one byte to make progress.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     s 原始字符串 / The original string
 * @param[in]     needle 正则对象 / The RegExp needle
 * @param[in]     rep 替换值（字符串或回调）/ The replacement (a string or a callback)
 *
 * @return 替换后的新字符串 / The resulting string
 *
 * @note 正则未编译成功时原样返回输入 / Returns the input unchanged when the regex was not compiled
 */
static JsValue string_replace_regex(HpeJs *js, const char *s, JsValue needle, JsValue rep)
{
    HpeRegex *re = (HpeRegex *)needle.obj->payload;
    int total, global, off;
    JSBuf b;
    char *rep_s = NULL;

    if (!re)
        return js_string(js, s);
    total = hpe_regex_group_count(re);
    global = regex_is_global(needle);
    if (!js_is_callable(rep))
        rep_s = js_to_string(js, rep);
    jsbuf_init(&b);
    off = 0;
    for (;;) {
        int caps[RE_CAPS];
        int m, i;

        for (i = 0; i < RE_CAPS; i++)
            caps[i] = -1;
        m = hpe_regex_search(re, s, off, caps, RE_CAPS);
        if (m < 0) {
            jsbuf_add(&b, s + off, strlen(s + off));
            break;
        }
        if (caps[1] < 0)
            caps[1] = m; /* empty match: use the search start as the end */
        jsbuf_add(&b, s + off, (size_t)(caps[0] - off));
        repl_append_match(js, &b, rep, rep_s, s, caps, total);
        /* advance past the match; an empty match advances one character to make
         * progress without looping forever */
        if (caps[1] > caps[0]) {
            off = caps[1];
        } else if ((size_t)off < strlen(s)) {
            jsbuf_add(&b, s + off, 1);
            off++;
        } else {
            break;
        }
        if (!global)
            break;
    }
    {
        JsValue out = js_string(js, b.s ? b.s : "");
        jsbuf_free(&b);
        return out;
    }
}

/**
 * @brief 用普通字符串替换：只替换第一个出现 / Replace with a plain-string needle: only the first occurrence
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     s 原始字符串 / The original string
 * @param[in]     needle 查找串（经字符串转换）/ The needle, coerced to a string
 * @param[in]     rep 替换值（字符串或回调）/ The replacement (a string or a callback)
 *
 * @return 替换后的新字符串；未命中时原样返回 / The resulting string, or the input unchanged when not found
 *
 * @note 回调形式接收 (matched, offset, input) 三个实参 / The callback form receives the arguments (matched, offset, input)
 */
static JsValue string_replace_plain(HpeJs *js, const char *s, JsValue needle, JsValue rep)
{
    char *n = js_to_string(js, needle);
    char *hit = strstr(s, n);
    size_t out_len;
    char *out;
    char *rep_s;

    if (!hit)
        return js_string(js, s);
    if (js_is_callable(rep)) {
        JsValue args[3];
        JsValue r;

        args[0] = js_string(js, n);
        args[1] = js_num((double)(hit - s));
        args[2] = js_string(js, s);
        r = call_value(js, NULL, rep, js_undef(), args, 3, 0);
        rep_s = js_to_string(js, r);
    } else {
        rep_s = js_to_string(js, rep);
    }
    out_len = strlen(s) + strlen(rep_s) + 1;
    out = (char *)hpe_js_alloc(js, out_len);
    memcpy(out, s, (size_t)(hit - s));
    memcpy(out + (hit - s), rep_s, strlen(rep_s));
    strcpy(out + (hit - s) + strlen(rep_s), hit + strlen(n));
    return js_string(js, out);
}

/**
 * @brief String.prototype.replace：按分隔类型分派替换 / String.prototype.replace: dispatch the replacement by needle type
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 接收字符串 / The receiver string
 * @param[in,out] a 实参数组：`a[0]` 查找目标、`a[1]` 替换值 / Argument array: `a[0]` is the needle, `a[1]` the replacement
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 替换后的新字符串 / The resulting string
 *
 * @note `needle` 为 RegExp 时走 string_replace_regex，否则走 string_replace_plain / A RegExp `needle` goes to string_replace_regex, anything else to string_replace_plain
 */
static JsValue string_replace(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    char *s = js_to_string(js, thisv);
    JsValue needle = arg_at(a, argc, 0);
    JsValue rep = arg_at(a, argc, 1);

    HPE_UNUSED(ud);
    if (needle.t == JS_OBJ && needle.obj && needle.obj->kind == JS_OBJ_REGEX)
        return string_replace_regex(js, s, needle, rep);
    return string_replace_plain(js, s, needle, rep);
}

/**
 * @brief 全局正则：收集所有不重叠匹配 / Global regex: gather every non-overlapping match
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] re 已编译的正则 / The compiled regex
 * @param[in]     s 输入字符串 / The input string
 *
 * @return 匹配子串数组；一个都没有时返回 `null` / An array of matched substrings, or `null` when there is none
 *
 * @note 推进位置在空匹配时按起点加一，避免死循环 / The advance position is start+1 for an empty match so the loop cannot spin
 */
static JsValue match_regex_global(HpeJs *js, HpeRegex *re, const char *s)
{
    JsObj *out = js_array_new(js);
    int off = 0, any = 0;
    for (;;) {
        int caps[RE_CAPS];
        int m, i;
        for (i = 0; i < RE_CAPS; i++)
            caps[i] = -1;
        m = hpe_regex_search(re, s, off, caps, RE_CAPS);
        if (m < 0)
            break;
        if (caps[1] < 0)
            caps[1] = m;
        array_push_value(js, out,
                         js_string_len(js, s + caps[0], (size_t)(caps[1] - caps[0])));
        any = 1;
        off = caps[1] > caps[0] ? caps[1] : caps[0] + 1;
        if ((size_t)off > strlen(s))
            break;
    }
    return any ? js_objval(out) : js_null();
}

/**
 * @brief 非全局正则：返回单个匹配结果对象 / Non-global regex: return a single match-result object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] re 已编译的正则 / The compiled regex
 * @param[in]     s 输入字符串 / The input string
 * @param[in]     total 捕获组个数 / Number of capture groups
 *
 * @return 匹配结果数组（含捕获组）；未匹配返回 `null` / The match-result array with capture groups, or `null` when unmatched
 *
 * @note 始终从位置 0 开始搜索 / The search always starts at offset 0
 */
static JsValue match_regex_once(HpeJs *js, HpeRegex *re, const char *s, int total)
{
    int caps[RE_CAPS];
    int i, m;
    for (i = 0; i < RE_CAPS; i++)
        caps[i] = -1;
    m = hpe_regex_search(re, s, 0, caps, RE_CAPS);
    if (m < 0)
        return js_null();
    if (caps[1] < 0)
        caps[1] = m;
    return regex_make_result(js, s, caps, total);
}

/**
 * @brief 普通字符串匹配：模拟匹配数组（含 index/input）/ Plain-string match: emulate the match array with index/input
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     s 输入字符串 / The input string
 * @param[in]     needle 查找串（经字符串转换）/ The needle, coerced to a string
 *
 * @return 匹配数组 / The match array
 *
 * @note 未命中时返回带 `index: -1` 的空数组 / When not found it returns an empty array carrying `index: -1`
 */
static JsValue match_plain(HpeJs *js, const char *s, JsValue needle)
{
    char *n = js_to_string(js, needle);
    char *hit = strstr(s, n);
    JsObj *out = js_array_new(js);

    if (!hit) {
        js_obj_set(js, out, "index", js_num(-1));
        return js_objval(out);
    }
    array_push_value(js, out, js_string(js, n));
    js_obj_set(js, out, "index", js_num((double)(hit - s)));
    js_obj_set(js, out, "input", js_string(js, s));
    return js_objval(out);
}

/**
 * @brief String.prototype.match：按分隔类型分派 / String.prototype.match: dispatch by needle type
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 接收字符串 / The receiver string
 * @param[in,out] a 实参数组，`a[0]` 为查找目标 / Argument array; `a[0]` is the needle
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 匹配结果：正则全局为数组、非全局为结果对象、普通串为匹配数组 / The match: an array for a global regex, a result object for a non-global one, or a match array for a plain string
 */
static JsValue string_match(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    char *s = js_to_string(js, thisv);
    JsValue needle = arg_at(a, argc, 0);
    HPE_UNUSED(ud);
    if (needle.t == JS_OBJ && needle.obj && needle.obj->kind == JS_OBJ_REGEX) {
        HpeRegex *re = (HpeRegex *)needle.obj->payload;
        int total = hpe_regex_group_count(re);
        if (!re)
            return js_null();
        if (regex_is_global(needle))
            return match_regex_global(js, re, s);
        return match_regex_once(js, re, s, total);
    }
    return match_plain(js, s, needle);
}

/**
 * @brief String.prototype.search：返回首个匹配的下标 / String.prototype.search: return the index of the first match
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 接收字符串 / The receiver string
 * @param[in,out] a 实参数组，`a[0]` 为查找目标 / Argument array; `a[0]` is the needle
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 首个匹配的字节下标；未找到返回 -1 / The byte offset of the first match, or -1 when not found
 *
 * @note 正则忽略其全局标志，始终从 0 开始搜索 / The regex's global flag is ignored and the search always starts at 0
 */
static JsValue string_search(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    char *s = js_to_string(js, thisv);
    JsValue needle = arg_at(a, argc, 0);
    HPE_UNUSED(ud);
    if (needle.t == JS_OBJ && needle.obj && needle.obj->kind == JS_OBJ_REGEX) {
        HpeRegex *re = (HpeRegex *)needle.obj->payload;
        int caps[2];
        caps[0] = caps[1] = 0;
        if (!re)
            return js_num(-1);
        return js_num(hpe_regex_search(re, s, 0, caps, 2) >= 0 ? (double)caps[0] : -1.0);
    }
    {
        char *n = js_to_string(js, needle);
        char *hit = strstr(s, n);
        return js_num(hit ? (double)(hit - s) : -1.0);
    }
}

/* ---------------- Symbol (minimal) ---------------- */

static int symbol_counter = 0;

static JsValue symbol_toString(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud);

/**
 * @brief Symbol(...)：创建一个最小可用的 Symbol 对象 / Symbol(...): create a minimal Symbol object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 忽略 / Ignored
 * @param[in,out] a 实参数组，`a[0]` 为描述文本 / Argument array; `a[0]` is the description
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 新的 Symbol 对象 / The new Symbol object
 *
 * @note 对象带 `description`（形如 `Symbol(desc)`）、`toString`，并以自增计数器作为内部标识 / The object carries `description` (of the form `Symbol(desc)`), `toString`, and an auto-incrementing counter as its internal identity
 */
static JsValue native_symbol(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *o = js_object_raw(js);
    char buf[48];
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    o->kind = JS_OBJ_PLAIN;
    o->class_name = "Symbol";
    o->proto = js->object_proto;
    snprintf(buf, sizeof(buf), "Symbol(%s)",
             argc > 0 && a[0].t != JS_UNDEF ? js_to_string(js, a[0]) : "");
    js_obj_set(js, o, "description", js_string(js, buf));
    js_obj_set(js, o, "toString", mk_native(js, symbol_toString, NULL));
    o->payload = (void *)(intptr_t)(++symbol_counter);
    return js_objval(o);
}

/**
 * @brief Symbol.prototype.toString：返回 symbol 的描述 / Symbol.prototype.toString: return the symbol's description
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv Symbol 对象接收者 / The Symbol receiver
 * @param[in,out] a 实参数组，忽略 / Argument array; ignored
 * @param[in]     argc 实参个数，忽略 / Argument count; ignored
 * @param[in,out] ud 未使用 / Unused
 *
 * @return `description` 字符串；缺失时返回 `Symbol()` / The `description` string, or `Symbol()` when absent
 */
static JsValue symbol_toString(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsValue d = member_get(js, NULL, thisv, "description", 0);
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    return d.t == JS_STR ? d : js_string(js, "Symbol()");
}

static JsValue map_set_impl(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud);

static JsValue map_get_impl(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud);

static JsValue native_map_ctor(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud);
static JsValue set_add(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud);
static JsValue native_set_ctor(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud);
static JsValue function_ctor(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud);

/**
 * @brief new Map(iterable)：创建 Map 并可选地由键值对初始化 / new Map(iterable): create a Map, optionally initialised from key/value pairs
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 忽略 / Ignored
 * @param[in,out] a 实参数组，`a[0]` 为可迭代的键值对集合 / Argument array; `a[0]` is an iterable of key/value pairs
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 新建的 Map 对象 / The new Map object
 *
 * @note 键以内部数组 `` _keys `` 保存，值以 `vN` 属性按同一索引存放；初始化时逐对调用 map_set_impl / Keys live in an internal `_keys` array while values are stored under `vN` properties at the same index; initialisation pairs are fed one by one into map_set_impl
 */
static JsValue native_map_ctor(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *o = js_object_raw(js);
    o->kind = JS_OBJ_MAP;
    o->class_name = "Map";
    o->proto = js->map_proto;
    o->payload_obj = js_array_new(js);
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    js_obj_set(js, o, "_keys", js_objval(o->payload_obj));
    if (argc > 0 && js_is_callable(a[0]) == 0 && js_iter_len(js, a[0]) > 0) {
        int n = js_iter_len(js, a[0]), i;
        for (i = 0; i < n; i++) {
            JsValue pair = js_iter_at(js, a[0], i);
            JsValue kv[2];
            kv[0] = js_iter_at(js, pair, 0);
            kv[1] = js_iter_at(js, pair, 1);
            map_set_impl(js, js_objval(o), kv, 2, NULL);
        }
    }
    return js_objval(o);
}

/**
 * @brief 在 Map 的键数组中查找键的下标 / Locate the index of a key in a Map's key array
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] o Map 对象 / The Map object
 * @param[in]     key 要查找的键 / The key to look up
 * @param[in]     create 占位参数，本实现未使用 / Placeholder; unused by this implementation
 *
 * @return 命中返回下标，否则返回 -1（以数值形式）/ The matching index, otherwise -1, both as numbers
 *
 * @note 比较使用 js_values_equal 的 SameValueZero 语义 / Comparison uses js_values_equal with SameValueZero semantics
 */
static JsValue map_locate(HpeJs *js, JsObj *o, JsValue key, int create)
{
    JsObj *keys = o->payload_obj;
    int i, len;
    HPE_UNUSED(create);
    if (!keys)
        return js_undef();
    len = array_length(js, keys);
    for (i = 0; i < len; i++)
        if (js_values_equal(js, array_get_index(js, keys, i), key, 1))
            return js_num((double)i);
    return js_num(-1);
}

/**
 * @brief Map 内部 set：写入键值对 / Map internal set: store a key/value pair
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv Map 对象接收者 / The Map receiver
 * @param[in,out] a 实参数组：`a[0]` 键、`a[1]` 值 / Argument array: `a[0]` is the key, `a[1]` the value
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 未使用 / Unused
 *
 * @return Map 本身，便于链式调用 / The Map itself, for chaining
 *
 * @note 键已存在则覆盖 `vN`；否则把键追加到 `` _keys `` 并写入新槽 / An existing key overwrites `vN`; otherwise the key is appended to `_keys` and a fresh slot written
 *
 * @see map_get_impl
 */
static JsValue map_set_impl(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *o = (thisv.t == JS_OBJ) ? thisv.obj : NULL;
    JsValue key = arg_at(a, argc, 0);
    JsValue val = arg_at(a, argc, 1);
    JsValue idx;
    HPE_UNUSED(ud);
    if (!o || o->kind != JS_OBJ_MAP)
        return thisv;
    idx = map_locate(js, o, key, 1);
    if (idx.t == JS_NUM && idx.num >= 0) {
        char buf[24];
        snprintf(buf, sizeof(buf), "v%d", (int)idx.num);
        js_obj_set(js, o, buf, val);
    } else {
        int n = array_length(js, o->payload_obj);
        char buf[24];
        array_push_value(js, o->payload_obj, key);
        snprintf(buf, sizeof(buf), "v%d", n);
        js_obj_set(js, o, buf, val);
    }
    return thisv;
}

/**
 * @brief Map 内部 get：按键取值 / Map internal get: read the value for a key
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv Map 对象接收者 / The Map receiver
 * @param[in,out] a 实参数组，`a[0]` 为键 / Argument array; `a[0]` is the key
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 键对应的值；不存在时返回 `undefined` / The value for the key, or `undefined` when absent
 */
static JsValue map_get_impl(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *o = (thisv.t == JS_OBJ) ? thisv.obj : NULL;
    JsValue key = arg_at(a, argc, 0);
    JsValue idx;
    char buf[24];
    HPE_UNUSED(ud);
    if (!o || o->kind != JS_OBJ_MAP)
        return js_undef();
    idx = map_locate(js, o, key, 0);
    if (idx.t != JS_NUM || idx.num < 0)
        return js_undef();
    snprintf(buf, sizeof(buf), "v%d", (int)idx.num);
    return member_get(js, NULL, thisv, buf, 0);
}

/**
 * @brief Map.prototype.has：判断键是否存在 / Map.prototype.has: test whether a key exists
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv Map 对象接收者 / The Map receiver
 * @param[in,out] a 实参数组，`a[0]` 为键 / Argument array; `a[0]` is the key
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 存在返回 true，否则 false / true when present, otherwise false
 */
static JsValue map_has(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *o = (thisv.t == JS_OBJ) ? thisv.obj : NULL;
    JsValue idx;
    HPE_UNUSED(ud);
    if (!o || o->kind != JS_OBJ_MAP)
        return js_bool(0);
    idx = map_locate(js, o, arg_at(a, argc, 0), 0);
    return js_bool(idx.t == JS_NUM && idx.num >= 0);
}

/**
 * @brief Map.prototype.delete：删除键并压缩内部存储 / Map.prototype.delete: remove a key and compact the internal storage
 *
 * @details 删除对应的 `vN` 槽后会前移其后的键，并同步搬移值属性、更新长度、清掉末尾多余槽 / After removing the `vN` slot it shifts the following keys forward, moves the matching value properties, updates the length and drops the now-extra trailing slot.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv Map 对象接收者 / The Map receiver
 * @param[in,out] a 实参数组，`a[0]` 为键 / Argument array; `a[0]` is the key
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 确实删除返回 true，键不存在返回 false / true when a key was removed, false when it was absent
 */
static JsValue map_delete(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *o = (thisv.t == JS_OBJ) ? thisv.obj : NULL;
    JsValue idx;
    HPE_UNUSED(ud);
    if (!o || o->kind != JS_OBJ_MAP)
        return js_bool(0);
    idx = map_locate(js, o, arg_at(a, argc, 0), 0);
    if (idx.t == JS_NUM && idx.num >= 0) {
        int i = (int)idx.num;
        char buf[24];
        snprintf(buf, sizeof(buf), "v%d", i);
        js_obj_delete(js, o, buf);
        {
            int n = array_length(js, o->payload_obj), k;
            for (k = i; k < n - 1; k++)
                array_set_index(js, o->payload_obj, k, array_get_index(js, o->payload_obj, k + 1));
            js_obj_set(js, o->payload_obj, "length", js_num((double)(n - 1)));
            for (k = i; k < n - 1; k++) {
                char vb[24], nb[24];
                snprintf(vb, sizeof(vb), "v%d", k);
                snprintf(nb, sizeof(nb), "v%d", k + 1);
                js_obj_set(js, o, vb, member_get(js, NULL, thisv, nb, 0));
            }
            snprintf(buf, sizeof(buf), "v%d", n - 1);
            js_obj_delete(js, o, buf);
        }
        return js_bool(1);
    }
    return js_bool(0);
}

/**
 * @brief Map.prototype.clear：清空所有键值对 / Map.prototype.clear: drop every key/value pair
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv Map 对象接收者 / The Map receiver
 * @param[in,out] a 实参数组，忽略 / Argument array; ignored
 * @param[in]     argc 实参个数，忽略 / Argument count; ignored
 * @param[in,out] ud 未使用 / Unused
 *
 * @return `undefined` / `undefined`
 *
 * @note 以新的空键数组替换内部 `` _keys `` 并清空 `items` / Replaces the internal `_keys` with a fresh empty array and clears `items`
 */
static JsValue map_clear(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *o = (thisv.t == JS_OBJ) ? thisv.obj : NULL;
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    if (o && o->kind == JS_OBJ_MAP) {
        o->payload_obj = js_array_new(js);
        o->items = NULL;
    }
    return js_undef();
}

/**
 * @brief Map.prototype.size 取值器：返回元素个数 / Map.prototype.size getter: return the entry count
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv Map 对象接收者 / The Map receiver
 * @param[in,out] a 实参数组，忽略 / Argument array; ignored
 * @param[in]     argc 实参个数，忽略 / Argument count; ignored
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 键数组的长度；非 Map 时为 0 / The length of the key array, or 0 for a non-Map
 *
 * @note 作为属性访问器调用，非 Map 接收者返回 0 / Invoked as a property accessor; a non-Map receiver yields 0
 */
static JsValue map_size_get(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *keys;
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    if (thisv.t != JS_OBJ || thisv.obj->kind != JS_OBJ_MAP)
        return js_num(0);
    keys = thisv.obj->payload_obj;
    return js_num(keys ? (double)array_length(js, keys) : 0.0);
}

/**
 * @brief Map.prototype.forEach：按插入顺序遍历键值对 / Map.prototype.forEach: visit every entry in insertion order
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv Map 对象接收者 / The Map receiver
 * @param[in,out] a 实参数组，`a[0]` 为回调 / Argument array; `a[0]` is the callback
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 未使用 / Unused
 *
 * @return `undefined` / `undefined`
 *
 * @note 回调实参依次为（值、键、Map 本身）；回调不可调用时不做任何事 / The callback receives (value, key, the Map itself); nothing happens when it is not callable
 */
static JsValue map_forEach(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *o = (thisv.t == JS_OBJ) ? thisv.obj : NULL;
    int i, n;
    JsValue cb = arg_at(a, argc, 0);
    HPE_UNUSED(ud);
    if (!o || o->kind != JS_OBJ_MAP || !js_is_callable(cb))
        return js_undef();
    n = array_length(js, o->payload_obj);
    for (i = 0; i < n; i++) {
        JsValue args[3];
        char buf[24];
        args[0] = member_get(js, NULL, thisv, (snprintf(buf, sizeof(buf), "v%d", i), buf), 0);
        args[1] = array_get_index(js, o->payload_obj, i);
        args[2] = thisv;
        call_value(js, NULL, cb, js_undef(), args, 3, 0);
    }
    return js_undef();
}

static JsValue map_forEach(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud);
static JsValue map_size_fn(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud);

/**
 * @brief Error 及各类错误构造器：创建错误对象 / The Error and subclass constructors: create an error object
 *
 * @details 根据 `ud` 传入的错误类型名选择对应的原型（TypeError/RangeError/SyntaxError/ReferenceError，其余落到 Error 原型），设置 `name`，并在有首参时设置 `message`（否则补齐空串）/ The error type name in `ud` selects the right prototype (TypeError/RangeError/SyntaxError/ReferenceError, otherwise the Error prototype), `name` is set, and `message` is set from the first argument or defaulted to the empty string.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 已构造的实例（`[[Construct]]` 路径），否则新建 / An already-created instance on the `[[Construct]]` path, otherwise a new object is made
 * @param[in,out] a 实参数组，`a[0]` 为错误消息 / Argument array; `a[0]` is the error message
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 错误类型名（如 `TypeError`）/ The error type name, e.g. `TypeError`
 *
 * @return 错误对象 / The error object
 *
 * @note 已存在的自有 `name`/`message` 属性不会被覆盖 / Pre-existing own `name`/`message` properties are not overwritten
 */

static JsValue error_ctor(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    const char *name = ud ? (const char *)ud : "Error";
    JsObj *proto =
        !strcmp(name, "TypeError")        ? js->type_error_proto
        : !strcmp(name, "RangeError")     ? js->range_error_proto
        : !strcmp(name, "SyntaxError")    ? js->syntax_error_proto
        : !strcmp(name, "ReferenceError") ? js->ref_error_proto
                                          : js->error_proto;
    JsObj *o;
    HPE_UNUSED(thisv);
    if (thisv.t == JS_OBJ && thisv.obj && thisv.obj->kind == JS_OBJ_ERROR) {
        o = thisv.obj; /* [[Construct]] with an already-created instance */
    } else {
        o = js_object_raw(js);
        o->kind = JS_OBJ_ERROR;
        o->class_name = name;
        o->proto = proto;
    }
    if (!js_obj_own_has(o, "name"))
        js_obj_set(js, o, "name", js_string(js, name));
    if (argc > 0 && a[0].t != JS_UNDEF)
        js_obj_set(js, o, "message", js_string(js, js_to_string(js, a[0])));
    else if (!js_obj_own_has(o, "message"))
        js_obj_set(js, o, "message", js_string(js, ""));
    return js_objval(o);
}

/**
 * @brief Error.prototype.toString：返回错误的字符串形式 / Error.prototype.toString: return the string form of an error
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 错误对象接收者 / The error receiver
 * @param[in,out] a 实参数组，忽略 / Argument array; ignored
 * @param[in]     argc 实参个数，忽略 / Argument count; ignored
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 错误对象的字符串表示 / The string representation of the error
 *
 * @note 直接走 js_to_string，`name`/`message` 的拼接由该转换负责 / Delegates to js_to_string, which handles the `name`/`message` concatenation
 */
static JsValue error_this_toString(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    return js_string(js, js_to_string(js, thisv));
}

/* ---------------- Promise ---------------- */

typedef struct {
    JsObj *promise;
    int settled;
} PromiseHooks;

typedef struct {
    JsObj *results;
    JsObj *out;
    int index;
    int *pending;
} AllCtx;

/**
 * @brief Promise 构造器暴露的 resolve 钩子 / The resolve hook exposed by the Promise constructor
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 忽略 / Ignored
 * @param[in,out] a 实参数组，`a[0]` 为兑现值 / Argument array; `a[0]` is the fulfilment value
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud PromiseHooks 钩子上下文 / The `PromiseHooks` hook context
 *
 * @return `undefined` / `undefined`
 *
 * @note 通过 `settled` 标志保证只生效一次，随后交给 promise_resolve_with / The `settled` flag makes it fire only once before delegating to promise_resolve_with
 */
static JsValue promise_hook_resolve(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    PromiseHooks *h = (PromiseHooks *)ud;
    HPE_UNUSED(thisv);
    if (h && !h->settled) {
        h->settled = 1;
        promise_resolve_with(js, h->promise, arg_at(a, argc, 0));
    }
    return js_undef();
}

/**
 * @brief Promise 构造器暴露的 reject 钩子 / The reject hook exposed by the Promise constructor
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 忽略 / Ignored
 * @param[in,out] a 实参数组，`a[0]` 为拒绝原因 / Argument array; `a[0]` is the rejection reason
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud PromiseHooks 钩子上下文 / The `PromiseHooks` hook context
 *
 * @return `undefined` / `undefined`
 *
 * @note 通过 `settled` 标志保证只生效一次，随后以状态 2 调用 js_settle_promise / The `settled` flag makes it fire only once before calling js_settle_promise with state 2
 */
static JsValue promise_hook_reject(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    PromiseHooks *h = (PromiseHooks *)ud;
    HPE_UNUSED(thisv);
    if (h && !h->settled) {
        h->settled = 1;
        js_settle_promise(js, h->promise, 2, arg_at(a, argc, 0));
    }
    return js_undef();
}

/**
 * @brief Promise.all 的单个兑现回调 / The per-item fulfilment callback of Promise.all
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 忽略 / Ignored
 * @param[in,out] a 实参数组，`a[0]` 为该项的兑现值 / Argument array; `a[0]` is this item's fulfilment value
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud AllCtx 聚合上下文 / The `AllCtx` aggregation context
 *
 * @return `undefined` / `undefined`
 *
 * @note 把结果写入对应下标，待计数减到 0 时以结果数组兑现输出 Promise / Stores the result at its index and, once the pending count reaches 0, fulfils the output promise with the results array
 */
static JsValue all_hook_ok(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    AllCtx *c = (AllCtx *)ud;
    HPE_UNUSED(thisv);
    array_set_index(js, c->results, c->index, arg_at(a, argc, 0));
    if (c->pending && --*c->pending == 0)
        js_settle_promise(js, c->out, 1, js_objval(c->results));
    return js_undef();
}

/**
 * @brief Promise.all 的单个拒绝回调 / The per-item rejection callback of Promise.all
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 忽略 / Ignored
 * @param[in,out] a 实参数组，`a[0]` 为拒绝原因 / Argument array; `a[0]` is the rejection reason
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud AllCtx 聚合上下文 / The `AllCtx` aggregation context
 *
 * @return `undefined` / `undefined`
 *
 * @note 任意一项拒绝即立即以该原因拒绝输出 Promise / Any single rejection immediately rejects the output promise with that reason
 */
static JsValue all_hook_err(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    AllCtx *c = (AllCtx *)ud;
    HPE_UNUSED(thisv);
    js_settle_promise(js, c->out, 2, arg_at(a, argc, 0));
    return js_undef();
}

/**
 * @brief new Promise(executor)：同步执行执行器并返回 Promise / new Promise(executor): run the executor synchronously and return a promise
 *
 * @details 创建（或复用已有的）Promise 对象，构造 resolve/reject 钩子并立即调用 executor；随后泵出微任务队列以推进已挂起的反应 / A promise is created (or an existing one reused), resolve/reject hooks are built and the executor is called immediately; the microtask queue is then drained to advance any pending reactions.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 已构造的 Promise 实例（`[[Construct]]` 路径），否则新建 / An already-created promise on the `[[Construct]]` path, otherwise a new one is made
 * @param[in,out] a 实参数组，`a[0]` 为 executor 函数 / Argument array; `a[0]` is the executor function
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 未使用 / Unused
 *
 * @return Promise 对象 / The promise object
 *
 * @note 缺少可调用的 executor 时抛出 “Promise resolver is not a function” / Throws “Promise resolver is not a function” when the executor is missing or not callable
 */
static JsValue promise_ctor_native(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *p;
    PromiseHooks *h;
    HPE_UNUSED(ud);
    if (thisv.t == JS_OBJ && thisv.obj && thisv.obj->kind == JS_OBJ_PROMISE)
        p = thisv.obj;
    else
        p = js_promise_new(js);
    if (argc < 1 || !js_is_callable(a[0]))
        js_error(js, 0, "Promise resolver is not a function");
    h = (PromiseHooks *)hpe_js_alloc(js, sizeof(PromiseHooks));
    h->promise = p;
    h->settled = 0;
    {
        JsValue args[2];
        args[0] = mk_native(js, promise_hook_resolve, h);
        args[1] = mk_native(js, promise_hook_reject, h);
        call_value(js, NULL, a[0], js_undef(), args, 2, 0);
    }
    hpe_js_drain_jobs(js);
    return js_objval(p);
}

/**
 * @brief Promise.prototype.then：注册兑现与拒绝回调 / Promise.prototype.then: register fulfilment and rejection callbacks
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv Promise 接收者 / The promise receiver
 * @param[in,out] a 实参数组：`a[0]` onFulfilled、`a[1]` onRejected / Argument array: `a[0]` onFulfilled, `a[1]` onRejected
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 新的下游 Promise / A new downstream promise
 *
 * @note 接收者不是对象时抛出 “Promise.prototype.then called on a non-promise” / Throws “Promise.prototype.then called on a non-promise” when the receiver is not an object
 */
static JsValue promise_proto_then(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(ud);
    if (thisv.t != JS_OBJ || !thisv.obj)
        js_error(js, 0, "Promise.prototype.then called on a non-promise");
    return promise_then(js, thisv.obj, arg_at(a, argc, 0), arg_at(a, argc, 1));
}

/**
 * @brief Promise.prototype.catch：等价于 `then(undefined, onRejected)` / Promise.prototype.catch: equivalent to `then(undefined, onRejected)`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv Promise 接收者 / The promise receiver
 * @param[in,out] a 实参数组，`a[0]` 为拒绝回调 / Argument array; `a[0]` is the rejection callback
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 新的下游 Promise / A new downstream promise
 */
static JsValue promise_proto_catch(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(ud);
    return promise_then(js, thisv.obj, js_undef(), arg_at(a, argc, 0));
}

/**
 * @brief Promise.prototype.finally：兑现与拒绝时都调用同一回调 / Promise.prototype.finally: invoke the same callback on both settlement paths
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv Promise 接收者 / The promise receiver
 * @param[in,out] a 实参数组，`a[0]` 为回调 / Argument array; `a[0]` is the callback
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 新的下游 Promise / A new downstream promise
 *
 * @note 实现为 `then(onF, onF)`，回调返回值不影响下游结果 / Implemented as `then(onF, onF)`; the callback's return value does not affect the downstream result
 */
static JsValue promise_proto_finally(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(ud);
    return promise_then(js, thisv.obj, arg_at(a, argc, 0), arg_at(a, argc, 0));
}

/**
 * @brief Promise.resolve：把值包装成已解决的 Promise / Promise.resolve: wrap a value into a resolved promise
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 忽略 / Ignored
 * @param[in,out] a 实参数组，`a[0]` 为待解决的值 / Argument array; `a[0]` is the value to resolve
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 已有的 Promise 原样返回，否则新建 / An existing promise is returned as is, otherwise a new one is created
 *
 * @note 遇到带 `then` 的 thenable 时以 resolve/reject 钩子调用其 `then`，否则直接兑现 / A thenable with a `then` method has its `then` called with the resolve/reject hooks; otherwise the promise is fulfilled directly
 */
static JsValue promise_static_resolve(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsValue v = arg_at(a, argc, 0);
    JsObj *p;
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    if (v.t == JS_OBJ && v.obj && v.obj->kind == JS_OBJ_PROMISE)
        return v;
    p = js_promise_new(js);
    if (v.t == JS_OBJ && v.obj && js_obj_has(v.obj, "then")) {
        PromiseHooks *h = (PromiseHooks *)hpe_js_alloc(js, sizeof(PromiseHooks));
        JsValue args[2];
        h->promise = p;
        h->settled = 0;
        args[0] = mk_native(js, promise_hook_resolve, h);
        args[1] = mk_native(js, promise_hook_reject, h);
        call_value(js, NULL, member_get(js, NULL, v, "then", 0), v, args, 2, 0);
    } else {
        promise_resolve_with(js, p, v);
    }
    return js_objval(p);
}

/**
 * @brief Promise.reject：创建已拒绝的 Promise / Promise.reject: create an already-rejected promise
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 忽略 / Ignored
 * @param[in,out] a 实参数组，`a[0]` 为拒绝原因 / Argument array; `a[0]` is the rejection reason
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 已拒绝的 Promise / The rejected promise
 *
 * @note 以状态 2 调用 js_settle_promise 立即结算 / Settles immediately via js_settle_promise with state 2
 */
static JsValue promise_static_reject(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *p = js_promise_new(js);
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    js_settle_promise(js, p, 2, arg_at(a, argc, 0));
    return js_objval(p);
}

/**
 * @brief Promise.all：等待全部兑现后以结果数组兑现 / Promise.all: fulfil with an array once every input fulfils
 *
 * @details 先按输入长度预置结果数组；长度为 0 时立即以空数组兑现；否则逐个把已解析的 Promise 通过反应钩子挂到 all_hook_ok/all_hook_err 上，最后泵出微任务队列 / The results array is pre-sized to the input length and fulfilled at once with an empty array when that length is 0; otherwise each resolved promise is hooked to all_hook_ok/all_hook_err and the microtask queue is drained.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 忽略 / Ignored
 * @param[in,out] a 实参数组，`a[0]` 为可迭代的 Promise 集合 / Argument array; `a[0]` is an iterable of promises
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 聚合结果 Promise / The aggregate promise
 *
 * @note 任一输入拒绝即整体拒绝 / Rejects as a whole as soon as any input rejects
 */
static JsValue promise_static_all(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *out = js_promise_new(js);
    JsObj *results;
    int n = js_iter_len(js, arg_at(a, argc, 0));
    int i;
    int *pending;
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    results = js_array_new(js);
    for (i = 0; i < n; i++)
        array_push_value(js, results, js_undef());
    if (n == 0) {
        js_settle_promise(js, out, 1, js_objval(results));
        return js_objval(out);
    }
    pending = (int *)hpe_js_alloc(js, sizeof(int));
    *pending = n;
    for (i = 0; i < n; i++) {
        JsValue it = js_iter_at(js, arg_at(a, argc, 0), i);
        JsValue settled = promise_static_resolve(js, js_undef(), &it, 1, NULL);
        AllCtx *ctx;
        if (settled.t != JS_OBJ || !settled.obj)
            continue;
        ctx = (AllCtx *)hpe_js_alloc(js, sizeof(AllCtx));
        ctx->results = results;
        ctx->out = out;
        ctx->index = i;
        ctx->pending = pending;
        reaction_push(js, settled.obj, mk_native(js, all_hook_ok, ctx),
                      mk_native(js, all_hook_err, ctx), NULL);
    }
    hpe_js_drain_jobs(js);
    return js_objval(out);
}

/**
 * @brief Promise.race：以首个结算的输入结果结算 / Promise.race: settle with the first input to settle
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 忽略 / Ignored
 * @param[in,out] a 实参数组，`a[0]` 为可迭代的 Promise 集合 / Argument array; `a[0]` is an iterable of promises
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 竞赛结果 Promise / The race promise
 *
 * @note 每个输入共用一个 resolve/reject 钩子上下文，谁先结算谁生效 / Every input shares one resolve/reject hook context, so whichever settles first wins
 */
static JsValue promise_static_race(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *out = js_promise_new(js);
    int n = js_iter_len(js, arg_at(a, argc, 0));
    int i;
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    for (i = 0; i < n; i++) {
        JsValue it = js_iter_at(js, arg_at(a, argc, 0), i);
        JsValue settled = promise_static_resolve(js, js_undef(), &it, 1, NULL);
        PromiseHooks *h;
        if (settled.t != JS_OBJ || !settled.obj)
            continue;
        h = (PromiseHooks *)hpe_js_alloc(js, sizeof(PromiseHooks));
        h->promise = out;
        h->settled = 0;
        reaction_push(js, settled.obj, mk_native(js, promise_hook_resolve, h),
                      mk_native(js, promise_hook_reject, h), NULL);
    }
    hpe_js_drain_jobs(js);
    return js_objval(out);
}

/**
 * @brief Function.prototype.call：以指定 this 立即调用 / Function.prototype.call: invoke immediately with the given `this`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 被调用的函数 / The function being invoked
 * @param[in,out] a 实参数组：`a[0]` 作为 this，其后为实参 / Argument array: `a[0]` becomes `this`, the rest are arguments
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 被调函数的返回值 / The callee's return value
 *
 * @note 接收者不可调用时抛出 “Function.prototype.call called on a non-function” / Throws “Function.prototype.call called on a non-function” when the receiver is not callable
 */

static JsValue function_call(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(ud);
    if (!js_is_callable(thisv))
        js_error(js, 0, "Function.prototype.call called on a non-function");
    return call_value(js, NULL, thisv, arg_at(a, argc, 0), argc > 1 ? a + 1 : NULL,
                      argc > 1 ? argc - 1 : 0, 0);
}

/**
 * @brief Function.prototype.apply：以数组展开实参调用 / Function.prototype.apply: invoke with arguments spread from an array
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 被调用的函数 / The function being invoked
 * @param[in,out] a 实参数组：`a[0]` 作为 this、`a[1]` 为实参集合 / Argument array: `a[0]` becomes `this` and `a[1]` is the argument collection
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 被调函数的返回值 / The callee's return value
 *
 * @note 实参集合按可迭代对象逐个取出；缺省或非对象时视为无实参 / The collection is iterated item by item; a missing or non-object value means no arguments
 */
static JsValue function_apply(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsValue arr = arg_at(a, argc, 1);
    int n = 0, i;
    JsValue *args = NULL;
    HPE_UNUSED(ud);
    if (!js_is_callable(thisv))
        js_error(js, 0, "Function.prototype.apply called on a non-function");
    if (arr.t == JS_OBJ && arr.obj) {
        n = js_iter_len(js, arr);
        args = (JsValue *)hpe_js_alloc(js, (size_t)(n + 1) * sizeof(JsValue));
        for (i = 0; i < n; i++)
            args[i] = js_iter_at(js, arr, i);
    }
    return call_value(js, NULL, thisv, arg_at(a, argc, 0), args, n, 0);
}

/**
 * @brief Function.prototype.bind：创建预绑定 this 与部分实参的函数 / Function.prototype.bind: create a function with a bound `this` and leading arguments
 *
 * @details 新建函数对象，复制目标的 kind、native 入口、用户数据与标志并追加 `FN_BOUND`，记录绑定目标、绑定的 this 与前缀实参（`a[1]` 起），名称固定为 `bound` / A new function object copies the target's kind, native entry, user data and flags, adds `FN_BOUND`, records the bound target, the bound `this` and the leading arguments (`a[1]` onward), and is always named `bound`.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 被绑定的函数 / The function being bound
 * @param[in,out] a 实参数组：`a[0]` 绑定为 this，其后为前缀实参 / Argument array: `a[0]` is bound as `this`, the rest become leading arguments
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 绑定后的新函数对象 / The resulting bound function object
 *
 * @note 接收者不是函数或原生对象时抛出 “Function.prototype.bind called on a non-function” / Throws “Function.prototype.bind called on a non-function” when the receiver is neither a function nor a native object
 */
static JsValue function_bind(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *target = (thisv.t == JS_OBJ) ? thisv.obj : NULL;
    JsObj *bound;
    int i, n;
    HPE_UNUSED(ud);
    if (!target || (target->kind != JS_OBJ_FUNC && target->kind != JS_OBJ_NATIVE))
        js_error(js, 0, "Function.prototype.bind called on a non-function");
    bound = js_object_raw(js);
    bound->kind = target->kind;
    bound->class_name = "Function";
    bound->proto = js->func_proto;
    bound->native = target->native;
    bound->native_ud = target->native_ud;
    bound->fn_flags = target->fn_flags | FN_BOUND;
    bound->bound_target = target;
    bound->bound_this = arg_at(a, argc, 0);
    bound->name = hpe_js_strdup(js, "bound");
    n = argc > 1 ? argc - 1 : 0;
    bound->bound_args = (JsValue *)hpe_js_alloc(js, (size_t)(n + 1) * sizeof(JsValue));
    for (i = 0; i < n; i++)
        bound->bound_args[i] = a[i + 1];
    bound->bound_argc = n;
    js_obj_set(js, bound, "name", js_string(js, "bound"));
    return js_objval(bound);
}

/**
 * @brief Function.prototype.toString：返回原生函数占位文本 / Function.prototype.toString: return the native-function placeholder text
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 忽略 / Ignored
 * @param[in,out] a 实参数组，忽略 / Argument array; ignored
 * @param[in]     argc 实参个数，忽略 / Argument count; ignored
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 固定字符串 `function () { [native code] }` / The fixed string `function () { [native code] }`
 *
 * @note 不还原真实函数源码 / The real function source is not reconstructed
 */
static JsValue function_this_toString(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(thisv);
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    return js_string(js, "function () { [native code] }");
}

/**
 * @brief new Function(...params, body)：动态编译函数 / new Function(...params, body): compile a function from source
 *
 * @details 把形参逐个用 `,` 连接、末尾接上函数体，拼装成 `(function(a,b){body})` 后交给表达式解析器，取回内层函数节点并构造函数对象 / Parameter strings are joined with `,` and followed by the body to form `(function(a,b){body})`, which the expression parser turns into the inner function node used to build the function object.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 忽略 / Ignored
 * @param[in,out] a 实参数组：最后一个元素为函数体，前面均为形参名 / Argument array: the last element is the body, all preceding ones are parameter names
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 新编译的函数对象 / The newly compiled function object
 *
 * @note 解析失败时回退为一个空参数的匿名空函数 / When parsing fails it falls back to an anonymous function with no parameters and an empty body
 */
static JsValue function_ctor(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JSBuf src;
    int i;
    const char *body = "";
    JsNode *prog, *fn_node = NULL;
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    jsbuf_init(&src);
    jsbuf_add(&src, "(function(", 10);
    for (i = 0; i < argc - 1; i++) {
        const char *p;
        if (i)
            jsbuf_add(&src, ",", 1);
        p = js_to_string(js, a[i]);
        jsbuf_add(&src, p, strlen(p));
    }
    jsbuf_add(&src, "){", 2);
    if (argc > 0)
        body = js_to_string(js, a[argc - 1]);
    jsbuf_add(&src, body, strlen(body));
    jsbuf_add(&src, "})", 2);
    prog = js_parse_expression(js, src.s ? src.s : "(function(){})");
    jsbuf_free(&src);
    if (prog && prog->kid_count > 0) {
        JsNode *inner = prog->kids[0];
        if (inner && inner->kind == JS_N_FUNC)
            fn_node = inner;
    }
    if (!fn_node) {
        JsNode *f = js_node_new(js, JS_N_FUNC, 0);
        JsNode *params = js_node_new(js, JS_N_BLOCK, 0);
        JsNode *b = js_node_new(js, JS_N_BLOCK, 0);
        f->str = "anonymous";
        js_node_add(js, f, params);
        js_node_add(js, f, b);
        return js_objval(js_func_new(js, f, js->global_env));
    }
    return js_objval(js_func_new(js, fn_node, js->global_env));
}

/**
 * @brief String(x)：把参数转换为字符串 / String(x): convert the argument to a string
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 忽略 / Ignored
 * @param[in,out] a 实参数组，`a[0]` 为待转换值 / Argument array; `a[0]` is the value to convert
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 字符串结果；无实参时返回空串 / The resulting string, or the empty string when called with no arguments
 */

static JsValue native_string_ctor(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    if (argc == 0)
        return js_string(js, "");
    return js_string(js, js_to_string(js, a[0]));
}

/**
 * @brief String.fromCharCode(...codes)：由 UTF-16 码元构造字符串 / String.fromCharCode(...codes): build a string from UTF-16 code units
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 忽略 / Ignored
 * @param[in,out] a 实参数组，每个元素为一个码元 / Argument array; each element is one code unit
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 拼接后的字符串 / The concatenated string
 *
 * @note 每个实参取低 16 位（`& 0xFFFF`）后编码为 UTF-8；BMP 以上的码位需由调用方用代理对表示 / Each argument is masked to its low 16 bits (`& 0xFFFF`) before UTF-8 encoding; code points above the BMP must be supplied as surrogate pairs by the caller
 */
static JsValue string_from_char_code(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JSBuf b;
    int i;
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    jsbuf_init(&b);
    for (i = 0; i < argc; i++) {
        char enc[4];
        uint32_t cp = (uint32_t)((long long)js_to_number(js, a[i]) & 0xFFFF);
        int n = hpe_utf8_encode(cp, enc);
        jsbuf_add(&b, enc, (size_t)n);
    }
    {
        JsValue out = js_string_len(js, b.s ? b.s : "", b.len);
        jsbuf_free(&b);
        return out;
    }
}

/**
 * @brief String.fromCodePoint(...cp)：由码点构造字符串 / String.fromCodePoint(...cp): build a string from code points
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 忽略 / Ignored
 * @param[in,out] a 实参数组，每个元素为一个码点 / Argument array; each element is one code point
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 拼接后的字符串 / The concatenated string
 *
 * @note 码点为 NaN、负数或大于 0x10FFFF 时抛出 `RangeError` / Throws a `RangeError` when a code point is NaN, negative or greater than 0x10FFFF
 */
static JsValue string_from_code_point(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JSBuf b;
    int i;
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    jsbuf_init(&b);
    for (i = 0; i < argc; i++) {
        char enc[4];
        double d = js_to_number(js, a[i]);
        int n;
        if (d != d || d < 0 || d > 0x10FFFF)
            js_throw_value(js, js_make_error(js, "RangeError", "Invalid code point"));
        n = hpe_utf8_encode((uint32_t)d, enc);
        jsbuf_add(&b, enc, (size_t)n);
    }
    {
        JsValue out = js_string_len(js, b.s ? b.s : "", b.len);
        jsbuf_free(&b);
        return out;
    }
}

/**
 * @brief Number(x)：把参数转换为数值 / Number(x): convert the argument to a number
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 忽略 / Ignored
 * @param[in,out] a 实参数组，`a[0]` 为待转换值 / Argument array; `a[0]` is the value to convert
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 数值结果；无实参时返回 0 / The numeric result, or 0 when called with no arguments
 */
static JsValue native_number_ctor(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    return js_num(argc == 0 ? 0.0 : js_to_number(js, a[0]));
}

/**
 * @brief Boolean(x)：把参数转换为布尔值 / Boolean(x): convert the argument to a boolean
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 忽略 / Ignored
 * @param[in,out] a 实参数组，`a[0]` 为待转换值 / Argument array; `a[0]` is the value to convert
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 布尔结果；按 js_truthy 判定 / The boolean result, decided by js_truthy
 *
 * @note 无实参时返回 false / Returns false when called with no arguments
 */
static JsValue native_boolean_ctor(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(js);
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    return js_bool(argc > 0 && js_truthy(a[0]));
}

/**
 * @brief Object(x)：包装或创建对象 / Object(x): wrap or create an object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 忽略 / Ignored
 * @param[in,out] a 实参数组，`a[0]` 为待包装值 / Argument array; `a[0]` is the value to wrap
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 有实参时原样返回该值；无实参时返回新建的空对象 / The argument returned as is when present, otherwise a freshly created empty object
 *
 * @note 本实现不区分原始值包装与对象透传，两者都原样返回 / This implementation does not distinguish primitive wrapping from object passthrough and returns the argument unchanged in both cases
 */
static JsValue native_object_ctor(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    if (argc > 0 && a[0].t == JS_OBJ)
        return a[0];
    if (argc > 0)
        return a[0];
    return js_objval(js_object_new(js));
}

/**
 * @brief Array(...args)：按长度或元素构造数组 / Array(...args): build an array from a length or from elements
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 忽略 / Ignored
 * @param[in,out] a 实参数组，`a[0]` 为唯一数值时视作长度 / Argument array; a single numeric `a[0]` is treated as the length
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 新数组 / The new array
 *
 * @note 单个数值实参时长度取 `floor` 且不小于 0，元素为空槽；否则各实参依次成为元素 / With a single numeric argument the length is `floor`'d and clamped to ≥0 leaving holes; otherwise the arguments become the elements
 */
static JsValue native_array_ctor(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    if (argc == 1 && a[0].t == JS_NUM) {
        JsObj *arr = js_array_new(js);
        double len = a[0].num >= 0 ? floor(a[0].num) : 0;
        js_obj_set(js, arr, "length", js_num(len));
        return js_objval(arr);
    }
    return array_of(js, a, argc);
}

/**
 * @brief Symbol.iterator 的取值函数：返回一个空对象 / The Symbol.iterator accessor: return an empty object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 忽略 / Ignored
 * @param[in,out] a 实参数组，忽略 / Argument array; ignored
 * @param[in]     argc 实参个数，忽略 / Argument count; ignored
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 新建的空对象 / A freshly created empty object
 *
 * @note 仅用于让 `obj[Symbol.iterator]` 的取值/存在性检查成立，不产生真实迭代器 / Only serves to make a `obj[Symbol.iterator]` read or existence check succeed; no real iterator is produced
 */

static JsValue native_iter_symbol(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(thisv);
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    return js_objval(js_object_new(js));
}

/**
 * @brief 统计字节区间内的 UTF-8 码点数 / Count the UTF-8 code points within a byte range
 *
 * @param[in]     s 以 NUL 结尾的字符串 / NUL-terminated string
 * @param[in]     bytes 限制的字节数 / Byte limit
 *
 * @return 区间内的码点个数 / The number of code points in the range
 *
 * @note 遇到 NUL 或达到 `bytes` 即停止 / Stops at a NUL or once `bytes` is reached
 */
static int utf8_strlen_n(const char *s, size_t bytes)
{
    size_t i = 0;
    int n = 0;
    while (i < bytes && s[i]) {
        uint32_t cp;
        int k = hpe_utf8_next(s + i, &cp);
        (void)cp;
        i += (size_t)k;
        n++;
    }
    return n;
}

/**
 * @brief 用正则分隔符切分，并按规范插入捕获组 / Split on a RegExp separator, interleaving capture groups per spec
 *
 * @details 在每次匹配处切出前一段作为元素，随后把每个捕获组依次插入结果，再推进搜索；`limit` 限制元素总数 / Each match contributes the preceding segment as an element followed by every capture group in turn, then the search advances; `limit` caps the total number of elements.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[out]    out 接收切分结果的数组 / The array receiving the split result
 * @param[in]     s 被切分的字符串 / The string being split
 * @param[in]     sep 正则分隔符 / The RegExp separator
 * @param[in]     limit 元素个数上限，负数表示不限 / Maximum element count; negative means unlimited
 *
 * @note 空匹配时前进一个字节以避免死循环 / An empty match advances one byte to avoid spinning
 */
static void string_split_regex(HpeJs *js, JsObj *out, const char *s, JsValue sep, int limit)
{
    HpeRegex *re = (HpeRegex *)sep.obj->payload;
    int off = 0, n = 0, m, total = hpe_regex_group_count(re);
    size_t sl = strlen(s);

    for (;;) {
        int caps[RE_CAPS];
        int i;

        for (i = 0; i < RE_CAPS; i++)
            caps[i] = -1;
        m = re ? hpe_regex_search(re, s, off, caps, RE_CAPS) : -1;
        if (m < 0) {
            if (limit < 0 || n < limit)
                array_push_value(js, out, js_string(js, s + off));
            break;
        }
        if (caps[1] < 0)
            caps[1] = m;
        if (limit >= 0 && n >= limit)
            break;
        array_push_value(js, out, js_string_len(js, s + off, (size_t)(caps[0] - off)));
        n++;
        for (i = 0; i < total; i++) {
            int bi = 2 + i * 2, ei = bi + 1;

            if (limit >= 0 && n >= limit)
                break;
            if (bi + 1 < RE_CAPS && caps[bi] >= 0 && caps[ei] >= caps[bi])
                array_push_value(js, out,
                                 js_string_len(js, s + caps[bi], (size_t)(caps[ei] - caps[bi])));
            else
                array_push_value(js, out, js_undef());
            n++;
        }
        if (caps[1] > caps[0])
            off = caps[1];
        else
            off = caps[0] + 1; /* empty match: advance one byte */
        if ((size_t)off > sl)
            break;
    }
}

/**
 * @brief 用普通字符串分隔符切分 / Split on a plain string separator
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[out]    out 接收切分结果的数组 / The array receiving the split result
 * @param[in]     s 被切分的字符串 / The string being split
 * @param[in]     sep 分隔符（经字符串转换）/ The separator, coerced to a string
 * @param[in]     limit 元素个数上限，负数表示不限 / Maximum element count; negative means unlimited
 *
 * @note 分隔符为空串时按码点逐字切分 / An empty separator splits the string into code points
 */
static void string_split_plain(HpeJs *js, JsObj *out, const char *s, JsValue sep, int limit)
{
    char *sep_s = js_to_string(js, sep);
    size_t sl = strlen(sep_s);
    const char *p = s;
    int n = 0;

    if (sl == 0) {
        int len = utf8_len(s), i;

        for (i = 0; i < len; i++) {
            if (limit >= 0 && n >= limit)
                break;
            array_push_value(js, out, utf8_char_at(js, s, i));
            n++;
        }
        return;
    }
    for (;;) {
        const char *hit;

        if (limit >= 0 && n >= limit)
            break;
        hit = strstr(p, sep_s);
        if (!hit) {
            array_push_value(js, out, js_string(js, p));
            break;
        }
        array_push_value(js, out, js_string_len(js, p, (size_t)(hit - p)));
        n++;
        p = hit + sl;
    }
}

/**
 * @brief String.prototype.split：按分隔符切分字符串 / String.prototype.split: split a string by a separator
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 接收字符串 / The receiver string
 * @param[in,out] a 实参数组：`a[0]` 分隔符、`a[1]` 上限 / Argument array: `a[0]` is the separator, `a[1]` the limit
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 切分结果数组 / The array of split parts
 *
 * @note 上限为 0 时返回空数组；分隔符为 `undefined` 时整串作为唯一元素 / A limit of 0 yields an empty array; an `undefined` separator makes the whole string the only element
 */
static JsValue string_split(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    char *s = str_arg(js, thisv);
    JsObj *out = js_array_new(js);
    JsValue sep = arg_at(a, argc, 0);
    int limit = argc > 1 && a[1].t != JS_UNDEF ? (int)js_to_number(js, a[1]) : -1;

    HPE_UNUSED(ud);
    if (limit == 0)
        return js_objval(out);
    if (sep.t == JS_UNDEF) {
        /* no separator: the whole string is the single element */
        array_push_value(js, out, js_string(js, s));
        return js_objval(out);
    }
    if (sep.t == JS_OBJ && sep.obj && sep.obj->kind == JS_OBJ_REGEX)
        string_split_regex(js, out, s, sep, limit);
    else
        string_split_plain(js, out, s, sep, limit);
    return js_objval(out);
}

/**
 * @brief String.prototype.concat：依次拼接实参并返回新串 / String.prototype.concat: append the arguments and return a new string
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 接收字符串 / The receiver string
 * @param[in,out] a 实参数组，按顺序拼接 / Argument array, appended in order
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 拼接后的新字符串 / The concatenated string
 *
 * @note 每个实参都经 str_arg 做字符串转换；缓冲区按需成倍扩容 / Every argument is coerced by str_arg; the buffer grows by doubling as needed
 */
static JsValue string_concat(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    char *s = str_arg(js, thisv);
    size_t len = strlen(s);
    size_t cap = len + 16;
    char *out = (char *)hpe_js_alloc(js, cap);
    int i;
    HPE_UNUSED(ud);
    memcpy(out, s, len);
    out[len] = '\0';
    for (i = 0; i < argc; i++) {
        char *piece = str_arg(js, a[i]);
        size_t pl = strlen(piece);
        if (len + pl + 1 > cap) {
            size_t ncap = (len + pl + 1) * 2;
            char *nb = (char *)hpe_js_alloc(js, ncap);
            memcpy(nb, out, len);
            out = nb;
            cap = ncap;
        }
        memcpy(out + len, piece, pl);
        len += pl;
        out[len] = '\0';
    }
    return js_string_len(js, out, len);
}

/**
 * @brief String.prototype.includes：判断从 pos 起是否包含子串 / `String.prototype.includes`: test whether the string contains the substring from `pos` on
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 实参数组：`a[0]` 为查找子串，`a[1]` 为起始位置 / Argument array: `a[0]` is the search substring, `a[1]` the start position
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 包含返回 true、否则 false / `true` when found, `false` otherwise
 *
 * @note 起始位置按码点计算并夹到 [0,total]；超过总长直接返回 false / The start index is in code points, clamped to [0,total]; past the end it returns `false`
 */
static JsValue string_includes(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    char *s = str_arg(js, thisv);
    char *needle = str_arg(js, arg_at(a, argc, 0));
    int pos = argc > 1 ? (int)js_to_number(js, a[1]) : 0;
    int total = utf8_len(s);
    HPE_UNUSED(ud);
    if (pos < 0)
        pos = 0;
    if (pos > total)
        return js_bool(0);
    return js_bool(strstr(s + utf8_offset(s, pos), needle) != NULL);
}

/**
 * @brief String.prototype.startsWith：判断从 pos 起是否以子串开头 / `String.prototype.startsWith`: test whether the string starts with the substring at `pos`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 实参数组：`a[0]` 为前缀子串，`a[1]` 为起始位置 / Argument array: `a[0]` is the prefix, `a[1]` the start position
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 是指定前缀返回 true、否则 false / `true` when it is the given prefix, `false` otherwise
 *
 * @note 起始位置为负时按 0 处理；比较按字节长度进行 / A negative start is clamped to 0; comparison uses the byte length
 */
static JsValue string_starts_with(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    char *s = str_arg(js, thisv);
    char *needle = str_arg(js, arg_at(a, argc, 0));
    int pos = argc > 1 ? (int)js_to_number(js, a[1]) : 0;
    size_t nl = strlen(needle);
    HPE_UNUSED(ud);
    if (pos < 0)
        pos = 0;
    return js_bool(strncmp(s + utf8_offset(s, pos), needle, nl) == 0);
}

/**
 * @brief String.prototype.endsWith：判断指定结尾位置之前是否以子串结尾 / `String.prototype.endsWith`: test whether the string ends with the substring before the given end index
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 实参数组：`a[0]` 为后缀子串，`a[1]` 为结尾位置（缺省为串长）/ Argument array: `a[0]` is the suffix, `a[1]` the end position (defaults to the length)
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 以指定子串结尾返回 true、否则 false / `true` when it ends with the given suffix, `false` otherwise
 *
 * @note 结尾位置按码点计算并夹到 [0,total]；不足子串长度时返回 false / The end index is in code points, clamped to [0,total]; `false` when shorter than the suffix
 */
static JsValue string_ends_with(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    char *s = str_arg(js, thisv);
    char *needle = str_arg(js, arg_at(a, argc, 0));
    int total = utf8_len(s);
    int end = argc > 1 && a[1].t != JS_UNDEF ? (int)js_to_number(js, a[1]) : total;
    size_t nl = strlen(needle);
    size_t byte_end;
    HPE_UNUSED(ud);
    if (end < 0)
        end = 0;
    if (end > total)
        end = total;
    byte_end = (size_t)utf8_offset(s, end);
    if (byte_end < nl)
        return js_bool(0);
    return js_bool(memcmp(s + byte_end - nl, needle, nl) == 0);
}

/**
 * @brief String.prototype.at：按下标返回一个码点字符 / `String.prototype.at`: return one code-point character by index
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 实参数组：`a[0]` 为下标 / Argument array: `a[0]` is the index
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 该位置的字符；越界返回 undefined / The character at that position, or `undefined` when out of range
 *
 * @note 负下标从串尾回绕；下标按码点计算 / A negative index counts from the end; indices are in code points
 */
static JsValue string_at(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    char *s = str_arg(js, thisv);
    int total = utf8_len(s);
    int i = (int)js_to_number(js, arg_at(a, argc, 0));
    HPE_UNUSED(ud);
    if (i < 0)
        i += total;
    if (i < 0 || i >= total)
        return js_undef();
    return utf8_char_at(js, s, i);
}

static JsValue set_add(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud);
static JsValue native_set_ctor(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud);
static JsValue map_size_fn(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud);
static JsValue string_iter(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud);
static JsValue native_regexp_ctor(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud);
static JsValue object_is_value(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud);
static JsValue function_ctor(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud);

/**
 * @brief Object.is：按 SameValue 语义比较两个值 / `Object.is`: compare two values with SameValue semantics
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 实参数组：`a[0]`、`a[1]` 为待比较的两个值 / Argument array: `a[0]` and `a[1]` are the two values to compare
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 相同返回 true、否则 false / `true` when the two values are the same, `false` otherwise
 *
 * @note NaN 视为相等，+0 与 -0 视为不等，其余按严格相等 / `NaN` equals `NaN`, `+0` differs from `-0`, everything else uses strict equality
 */
static JsValue object_is_value(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsValue x = arg_at(a, argc, 0);
    JsValue y = arg_at(a, argc, 1);
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    if (x.t == JS_NUM && y.t == JS_NUM) {
        if (x.num != x.num && y.num != y.num)
            return js_bool(1);
        if (x.num == 0.0 && y.num == 0.0)
            return js_bool(signbit(x.num) == signbit(y.num));
    }
    return js_bool(js_values_equal(js, x, y, 1));
}

/**
 * @brief RegExp 构造器：新建正则对象 / The `RegExp` constructor: create a regular-expression object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 实参数组：`a[0]` 为模式（可为已有正则），`a[1]` 为标志串 / Argument array: `a[0]` is the pattern (may be an existing regex), `a[1]` the flag string
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 新的正则对象；传入正则且未给标志时原样返回 / The new regex object; when given a regex and no flags, that same object
 */
static JsValue native_regexp_ctor(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsValue pat = arg_at(a, argc, 0);
    JsValue fl = arg_at(a, argc, 1);
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    if (pat.t == JS_OBJ && pat.obj && pat.obj->kind == JS_OBJ_REGEX && fl.t == JS_UNDEF)
        return pat;
    {
        char *p = js_to_string(js, pat.t == JS_UNDEF ? js_string(js, "") : pat);
        char *f = fl.t == JS_UNDEF ? "" : js_to_string(js, fl);
        JsValue o = regex_obj_new(js, p, f);
        if (pat.t == JS_OBJ && pat.obj && pat.obj->kind == JS_OBJ_REGEX)
            o.obj->payload = pat.obj->payload;
        return o;
    }
}

/**
 * @brief 把值转成 Set 内部使用的字符串键 / Convert a value into the string key used inside a `Set`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     v 待转换的值 / Value to convert
 *
 * @return 堆分配的键字符串 / A heap-allocated key string
 *
 * @note 按类型加前缀以避免冲突（如布尔 `b:true`、对象按指针 `o:%p`）；字符串与数字直接用其字符串形式 / A per-type prefix avoids collisions (`b:true` for booleans, `o:%p` for objects); strings and numbers use their string form
 */

static char *set_key(HpeJs *js, JsValue v)
{
    if (v.t == JS_STR)
        return js_to_string(js, v);
    if (v.t == JS_NUM)
        return hpe_js_strdup(js, js_num_to_string(js, v.num));
    if (v.t == JS_BOOL)
        return hpe_js_strdup(js, v.num != 0.0 ? "b:true" : "b:false");
    if (v.t == JS_UNDEF)
        return hpe_js_strdup(js, "u:");
    if (v.t == JS_NULL)
        return hpe_js_strdup(js, "n:");
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "o:%p", (void *)v.obj);
        return hpe_js_strdup(js, buf);
    }
}

/**
 * @brief Set 构造器：新建 Set（可选从可迭代对象初始化）/ The `Set` constructor: create a `Set`, optionally seeded from an iterable
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 实参数组：`a[0]` 为初始可迭代对象 / Argument array: `a[0]` is the initial iterable
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 新的 Set 对象 / The new `Set` object
 *
 * @note 内部用普通对象（`payload_obj`）以字符串键存储成员 / Members are stored in a plain object (`payload_obj`) keyed by string
 */
static JsValue native_set_ctor(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *o = js_object_raw(js);
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    o->kind = JS_OBJ_MAP;
    o->class_name = "Set";
    o->proto = js->set_proto;
    o->payload_obj = js_array_new(js);
    if (argc > 0 && js_iter_len(js, a[0]) > 0) {        int n = js_iter_len(js, a[0]), i;
        for (i = 0; i < n; i++) {
            JsValue v = js_iter_at(js, a[0], i);
            JsValue one[1];
            one[0] = v;
            set_add(js, js_objval(o), one, 1, NULL);
        }
    }
    return js_objval(o);
}

/**
 * @brief Set.prototype.add：向集合加入一个值 / `Set.prototype.add`: add a value to the set
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 实参数组：`a[0]` 为待加入的值 / Argument array: `a[0]` is the value to add
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 返回 this（集合本身），便于链式调用 / Returns `this` (the set itself) for chaining
 *
 * @note 已存在则不改动；无有效集合时原样返回 this / An existing member is left untouched; with no valid set, `this` is returned as-is
 */
static JsValue set_add(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *o = (thisv.t == JS_OBJ) ? thisv.obj : NULL;
    JsValue v = arg_at(a, argc, 0);
    char *k;
    HPE_UNUSED(ud);
    if (!o || !o->payload_obj)
        return thisv;
    k = set_key(js, v);
    if (!js_obj_own_has(o->payload_obj, k))
        js_obj_set(js, o->payload_obj, k, v);
    return thisv;
}

/**
 * @brief Set.prototype.has：判断集合是否含某值 / `Set.prototype.has`: test whether the set contains a value
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 实参数组：`a[0]` 为待查询的值 / Argument array: `a[0]` is the value to look up
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 包含返回 true、否则 false / `true` when present, `false` otherwise
 */
static JsValue set_has(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *o = (thisv.t == JS_OBJ) ? thisv.obj : NULL;
    HPE_UNUSED(ud);
    if (!o || !o->payload_obj)
        return js_bool(0);
    return js_bool(js_obj_own_has(o->payload_obj, set_key(js, arg_at(a, argc, 0))));
}

/**
 * @brief Set.prototype.delete：从集合删除某值 / `Set.prototype.delete`: remove a value from the set
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 实参数组：`a[0]` 为待删除的值 / Argument array: `a[0]` is the value to remove
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 确实删除返回 true、否则 false / `true` when a member was removed, `false` otherwise
 */
static JsValue set_delete(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *o = (thisv.t == JS_OBJ) ? thisv.obj : NULL;
    HPE_UNUSED(ud);
    if (!o || !o->payload_obj)
        return js_bool(0);
    return js_bool(js_obj_delete(js, o->payload_obj, set_key(js, arg_at(a, argc, 0))));
}

/**
 * @brief Set.prototype.clear：清空集合 / `Set.prototype.clear`: remove all members from the set
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 返回 undefined / Returns `undefined`
 *
 * @note 实现上是更换一个新的空存储数组 / Implemented by swapping in a fresh empty backing array
 */
static JsValue set_clear(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *o = (thisv.t == JS_OBJ) ? thisv.obj : NULL;
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    if (o)
        o->payload_obj = js_array_new(js);
    return js_undef();
}

/**
 * @brief Set.prototype.values/keys：返回成员值的可迭代对象 / `Set.prototype.values`/`keys`: return an iterable over member values
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 含全部成员值的可迭代对象 / An iterable over all member values
 *
 * @note 按内部键顺序遍历；无有效集合时返回空迭代器 / Walks the internal key order; an empty iterator when there is no valid set
 */
static JsValue set_values(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *o = (thisv.t == JS_OBJ) ? thisv.obj : NULL;
    JsObj *items = js_array_new(js);
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    if (o && o->payload_obj) {
        char **keys;
        int nk, i;
        js_obj_own_keys(o->payload_obj, &keys, &nk);
        for (i = 0; i < nk; i++)
            array_push_value(js, items, js_obj_get(js, o->payload_obj, keys[i]));
        free(keys);
    }
    return make_iterable(js, items);
}

static JsValue set_foreach(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{    JsObj *o = (thisv.t == JS_OBJ) ? thisv.obj : NULL;
    JsValue cb = arg_at(a, argc, 0);
    HPE_UNUSED(ud);
    if (o && o->payload_obj && js_is_callable(cb)) {
        char **keys;
        int nk, i;
        js_obj_own_keys(o->payload_obj, &keys, &nk);
        for (i = 0; i < nk; i++) {
            JsValue v = js_obj_get(js, o->payload_obj, keys[i]);
            JsValue args[2];
            args[0] = v;
            args[1] = v;
            call_value(js, NULL, cb, js_undef(), args, 2, 0);
        }
        free(keys);
    }
    return js_undef();
}

/**
 * @brief Map/Set 的 size 取值器：返回元素个数 / The `size` getter for `Map`/`Set`: return the element count
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 元素个数（数字）/ The element count as a number
 *
 * @note Set 数键个数，Map 数存储数组长度 / For a `Set` it counts the keys, for a `Map` the backing array length
 */
static JsValue map_size_fn(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *o = (thisv.t == JS_OBJ) ? thisv.obj : NULL;
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    if (!o)
        return js_num(0);
    if (o->class_name && !strcmp(o->class_name, "Set")) {
        int n = 0;
        char **keys;
        if (o->payload_obj) {
            js_obj_own_keys(o->payload_obj, &keys, &n);
            free(keys);
        }
        return js_num((double)n);
    }
    return js_num(o->payload_obj ? (double)array_length(js, o->payload_obj) : 0.0);
}

/**
 * @brief 校验 thisv 是否为 Map/Set 对象并取出对象指针 / Validate that `thisv` is a `Map`/`Set` object and hand back its object pointer
 *
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[out]    out 成功后接收对象指针 / On success receives the object pointer
 *
 * @return 是 Map/Set 返回 1、否则 0 / 1 when it is a `Map`/`Set`, otherwise 0
 */

static int map_kind(JsValue thisv, JsObj **out)
{
    JsObj *o = (thisv.t == JS_OBJ) ? thisv.obj : NULL;
    if (!o || o->kind != JS_OBJ_MAP)
        return 0;
    *out = o;
    return 1;
}

/**
 * @brief Map.prototype.keys：返回键的可迭代对象 / `Map.prototype.keys`: return an iterable over the keys
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 键的可迭代对象 / An iterable over the keys
 *
 * @note 从存储数组按下标取键；非 Map 返回空迭代器 / Keys are read by index from the backing array; an empty iterator for non-maps
 */
static JsValue map_keys(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *o;
    JsObj *items = js_array_new(js);
    int i, n;
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    if (!map_kind(thisv, &o))
        return make_iterable(js, items);
    n = array_length(js, o->payload_obj);
    for (i = 0; i < n; i++)
        array_push_value(js, items, array_get_index(js, o->payload_obj, i));
    return make_iterable(js, items);
}

/**
 * @brief Map.prototype.values：返回值的可迭代对象 / `Map.prototype.values`: return an iterable over the values
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 值的可迭代对象 / An iterable over the values
 *
 * @note 按内部 `v<i>` 属性取各值；非 Map 返回空迭代器 / Values are read via the internal `v<i>` members; an empty iterator for non-maps
 */
static JsValue map_values(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *o;
    JsObj *items = js_array_new(js);
    int i, n;
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    if (!map_kind(thisv, &o))
        return make_iterable(js, items);
    n = array_length(js, o->payload_obj);
    for (i = 0; i < n; i++) {
        char buf[24];
        snprintf(buf, sizeof(buf), "v%d", i);
        array_push_value(js, items, member_get(js, NULL, thisv, buf, 0));
    }
    return make_iterable(js, items);
}

/**
 * @brief Map.prototype.entries：返回 [键,值] 二元组的可迭代对象 / `Map.prototype.entries`: return an iterable of `[key, value]` pairs
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 二元组数组的可迭代对象 / An iterable of two-element arrays
 *
 * @note 每项为 `[k, v]`；非 Map 返回空迭代器 / Each item is `[k, v]`; an empty iterator for non-maps
 */
static JsValue map_entries(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *o;
    JsObj *items = js_array_new(js);
    int i, n;
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    if (!map_kind(thisv, &o))
        return make_iterable(js, items);
    n = array_length(js, o->payload_obj);
    for (i = 0; i < n; i++) {
        char buf[24];
        JsValue pair[2];
        pair[0] = array_get_index(js, o->payload_obj, i);
        snprintf(buf, sizeof(buf), "v%d", i);
        pair[1] = member_get(js, NULL, thisv, buf, 0);
        array_push_value(js, items, array_of(js, pair, 2));
    }
    return make_iterable(js, items);
}

/**
 * @brief Set.prototype.entries：返回 [值,值] 二元组的可迭代对象 / `Set.prototype.entries`: return an iterable of `[value, value]` pairs
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 二元组数组的可迭代对象 / An iterable of two-element arrays
 *
 * @note 按规范每项的两元素相同；非 Set 返回空迭代器 / Per spec both elements of each pair are the same value; an empty iterator for non-sets
 */
static JsValue set_entries(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *o = (thisv.t == JS_OBJ) ? thisv.obj : NULL;
    JsObj *items = js_array_new(js);
    int i, n;
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    if (!o || o->kind != JS_OBJ_MAP)
        return make_iterable(js, items);
    n = js_iter_len(js, thisv);
    for (i = 0; i < n; i++) {
        JsValue pair[2];
        JsValue v = js_iter_at(js, thisv, i);
        pair[0] = v;
        pair[1] = v;
        array_push_value(js, items, array_of(js, pair, 2));
    }
    return make_iterable(js, items);
}

/**
 * @brief Map/Set 的 `[Symbol.iterator]`：返回对象自身 / `[Symbol.iterator]` for `Map`/`Set`: return the object itself
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 调用时的 this 值原样返回 / The `this` value is returned unchanged
 *
 * @note 迭代行为由 `js_iter_len`/`js_iter_at` 提供，共享的 `next` 遍历器驱动它 / Iterator behaviour is supplied by `js_iter_len`/`js_iter_at` and driven by the shared `next` walker
 */
static JsValue map_set_iter_self(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(js);
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    return thisv;
}

static JsValue symbol_brand(HpeJs *js, const char *desc){
    JsObj *o = js_object_raw(js);
    o->kind = JS_OBJ_PLAIN;
    o->class_name = "Symbol";
    o->proto = js->symbol_proto;
    js_obj_set(js, o, "description", js_string(js, desc));
    js_obj_set(js, o, "toString", mk_native(js, symbol_toString, NULL));
    return js_objval(o);
}

/**
 * @brief 字符串的 `[Symbol.iterator]`：按码点遍历 / `[Symbol.iterator]` on a string: iterate by code point
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] a 操作数 / Operand
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 各码点字符的可迭代对象 / An iterable over the per-code-point characters
 */
static JsValue string_iter(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    char *s = js_to_string(js, thisv);
    JsObj *items = js_array_new(js);
    int len = utf8_len(s), i;
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    for (i = 0; i < len; i++)
        array_push_value(js, items, utf8_char_at(js, s, i));
    return make_iterable(js, items);
}

/**
 * @brief 创建一个挂到指定原型下的新原型对象 / Create a new prototype object chained under the given prototype
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     cls 原型对象的类名（如 `Array`）/ Class name of the prototype object, e.g. `Array`
 * @param[in,out] object_proto 该原型的父原型（可为 NULL）/ Parent prototype, may be `NULL`
 *
 * @return 新建的原型对象 / The newly created prototype object
 */

static JsObj *fresh_proto(HpeJs *js, const char *cls, JsObj *object_proto)
{
    JsObj *p = js_object_raw(js);
    p->proto = object_proto;
    p->class_name = cls;
    return p;
}

/**
 * @brief 创建原生构造器并注册为全局 / Create a native constructor and register it as a global
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     name 构造器名（同时用作全局属性名）/ Constructor name (also used as the global property name)
 * @param[in]     fn 原生函数实现 / Native function implementation
 * @param[in,out] ud 透传给原生函数的用户数据 / User data passed through to the native function
 * @param[in,out] proto 构造器的 `prototype` 指向的原型对象（可为 NULL）/ The prototype object the constructor's `prototype` points at, may be `NULL`
 *
 * @return 新建的构造器函数值 / The newly created constructor function value
 *
 * @note 同时设置函数的 `name`，并在提供 proto 时设置 `prototype` 属性 / Also sets the function's `name` and, when a prototype is given, its `prototype` property
 */
static JsValue make_ctor(HpeJs *js, const char *name, JsNativeFn fn, void *ud, JsObj *proto)
{
    JsObj *f = js_native_new(js, fn, ud);
    f->name = hpe_js_strdup(js, name);
    js_obj_set(js, f, "name", js_string(js, name));
    if (proto)
        js_obj_set(js, f, "prototype", js_objval(proto));
    js_obj_set(js, js->global_obj, name, js_objval(f));
    return js_objval(f);
}

/**
 * @brief 创建各内建类型的原型对象并接好原型链 / Create the prototype objects of the builtin types and wire up the prototype chain
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @note 内建原型的父原型为 `Object.prototype`，三个错误原型继承 `Error.prototype` / Builtin prototypes inherit from `Object.prototype`; the three error prototypes inherit from `Error.prototype`
 */
static void init_prototypes(HpeJs *js)
{
    js->object_proto = fresh_proto(js, "Object", NULL);
    js->func_proto = fresh_proto(js, "Function", js->object_proto);
    js->array_proto = fresh_proto(js, "Array", js->object_proto);
    js->string_proto = fresh_proto(js, "String", js->object_proto);
    js->number_proto = fresh_proto(js, "Number", js->object_proto);
    js->bool_proto = fresh_proto(js, "Boolean", js->object_proto);
    js->promise_proto = fresh_proto(js, "Promise", js->object_proto);
    js->regex_proto = fresh_proto(js, "RegExp", js->object_proto);
    js->error_proto = fresh_proto(js, "Error", js->object_proto);
    js->type_error_proto = fresh_proto(js, "TypeError", js->error_proto);
    js->range_error_proto = fresh_proto(js, "RangeError", js->error_proto);
    js->syntax_error_proto = fresh_proto(js, "SyntaxError", js->error_proto);
    js->ref_error_proto = fresh_proto(js, "ReferenceError", js->error_proto);
    js->map_proto = fresh_proto(js, "Map", js->object_proto);
    js->set_proto = fresh_proto(js, "Set", js->object_proto);
    js->symbol_proto = fresh_proto(js, "Symbol", js->object_proto);
    js->date_proto = fresh_proto(js, "Date", js->object_proto);
}

/**
 * @brief 给 `Object.prototype` 安装方法 / Install the methods on `Object.prototype`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @note 安装 `toString`、`valueOf`、`hasOwnProperty`、`propertyIsEnumerable` / Installs `toString`, `valueOf`, `hasOwnProperty` and `propertyIsEnumerable`
 */
static void install_object_proto(HpeJs *js)
{
    JsObj *p = js->object_proto;
    def_method(js, p, "toString", object_this_toString, NULL);
    def_method(js, p, "valueOf", object_this_valueOf, NULL);
    def_method(js, p, "hasOwnProperty", object_has_own, NULL);
    def_method(js, p, "propertyIsEnumerable", object_has_own, NULL);
}

/**
 * @brief 给 `Function.prototype` 安装方法 / Install the methods on `Function.prototype`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @note 安装 `call`、`apply`、`bind`、`toString` / Installs `call`, `apply`, `bind` and `toString`
 */
static void install_function_proto(HpeJs *js)
{
    JsObj *p = js->func_proto;
    def_method(js, p, "call", function_call, NULL);
    def_method(js, p, "apply", function_apply, NULL);
    def_method(js, p, "bind", function_bind, NULL);
    def_method(js, p, "toString", function_this_toString, NULL);
}

/**
 * @brief 给 `Array.prototype` 安装全部数组方法 / Install all array methods on `Array.prototype`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @note 覆盖增删改、遍历、查找、排序与 `keys`/`values`/`entries`，并设置 `Symbol.iterator` 为自迭代 / Covers mutation, iteration, searching, sorting plus `keys`/`values`/`entries`, and sets `Symbol.iterator` to self-iteration
 */
static void install_array_proto(HpeJs *js)
{
    JsObj *p = js->array_proto;
    def_method(js, p, "push", array_push, NULL);
    def_method(js, p, "pop", array_pop, NULL);
    def_method(js, p, "shift", array_shift, NULL);
    def_method(js, p, "unshift", array_unshift, NULL);
    def_method(js, p, "slice", array_slice, NULL);
    def_method(js, p, "splice", array_splice, NULL);
    def_method(js, p, "concat", array_concat, NULL);
    def_method(js, p, "join", array_join, NULL);
    def_method(js, p, "indexOf", array_indexOf, NULL);
    def_method(js, p, "lastIndexOf", array_lastIndexOf, NULL);
    def_method(js, p, "includes", array_includes, NULL);
    def_method(js, p, "forEach", array_forEach, NULL);
    def_method(js, p, "map", array_map, NULL);
    def_method(js, p, "filter", array_filter, NULL);
    def_method(js, p, "some", array_some, NULL);
    def_method(js, p, "every", array_every, NULL);
    def_method(js, p, "find", array_find, NULL);
    def_method(js, p, "findIndex", array_findIndex, NULL);
    def_method(js, p, "findLast", array_find_last, (void *)1);
    def_method(js, p, "findLastIndex", array_find_last, NULL);
    def_method(js, p, "reduce", array_reduce, NULL);
    def_method(js, p, "reduceRight", array_reduceRight, NULL);
    def_method(js, p, "reverse", array_reverse, NULL);
    def_method(js, p, "sort", array_sort, NULL);
    def_method(js, p, "flat", array_flat, NULL);
    def_method(js, p, "flatMap", array_flatMap, NULL);
    def_method(js, p, "fill", array_fill, NULL);
    def_method(js, p, "copyWithin", array_copyWithin, NULL);
    def_method(js, p, "at", array_at, NULL);
    def_method(js, p, "keys", array_keys, NULL);
    def_method(js, p, "values", array_values, NULL);
    def_method(js, p, "entries", array_entries, NULL);
    def_method(js, p, "toString", array_toString, NULL);
    js_obj_set(js, p, "Symbol.iterator", mk_native(js, array_iter_self, NULL));
}

/**
 * @brief 给 `String.prototype` 安装全部字符串方法 / Install all string methods on `String.prototype`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @note 包含大小写、修剪、填充、分割替换、匹配与 `includes`/`startsWith`/`endsWith`/`at`，并设置 `Symbol.iterator` 为按码点迭代 / Includes case conversion, trimming, padding, split/replace, matching and `includes`/`startsWith`/`endsWith`/`at`, and sets `Symbol.iterator` to code-point iteration
 */
static void install_string_proto(HpeJs *js)
{
    JsObj *p = js->string_proto;
    JsObj *iter = js_object_raw(js);
    def_method(js, p, "charAt", string_charAt, NULL);
    def_method(js, p, "charCodeAt", string_charCodeAt, NULL);
    def_method(js, p, "indexOf", string_indexOf, NULL);
    def_method(js, p, "lastIndexOf", string_lastIndexOf, NULL);
    def_method(js, p, "slice", string_slice, NULL);
    def_method(js, p, "substring", string_substring, NULL);
    def_method(js, p, "substr", string_substr, NULL);
    def_method(js, p, "toUpperCase", string_toUpperCase, NULL);
    def_method(js, p, "toLowerCase", string_toLowerCase, NULL);
    def_method(js, p, "trim", string_trim, NULL);
    def_method(js, p, "trimStart", string_trim_side, NULL);
    def_method(js, p, "trimEnd", string_trim_side, (void *)1);
    def_method(js, p, "trimLeft", string_trim_side, NULL);
    def_method(js, p, "trimRight", string_trim_side, (void *)1);
    def_method(js, p, "padStart", string_pad, NULL);
    def_method(js, p, "padEnd", string_pad, (void *)1);
    def_method(js, p, "repeat", string_repeat, NULL);
    def_method(js, p, "split", string_split, NULL);
    def_method(js, p, "replace", string_replace, NULL);
    def_method(js, p, "replaceAll", string_replaceAll, NULL);
    def_method(js, p, "match", string_match, NULL);
    def_method(js, p, "matchAll", string_matchAll, NULL);
    def_method(js, p, "search", string_search, NULL);
    def_method(js, p, "codePointAt", string_codePointAt, NULL);
    def_method(js, p, "localeCompare", string_localeCompare, NULL);
    def_method(js, p, "concat", string_concat, NULL);
    def_method(js, p, "includes", string_includes, NULL);
    def_method(js, p, "startsWith", string_starts_with, NULL);
    def_method(js, p, "endsWith", string_ends_with, NULL);
    def_method(js, p, "at", string_at, NULL);
    /* Symbol.iterator on strings: a method object stored under its symbolic key */
    iter->class_name = "Iterator";
    iter->proto = js->object_proto;
    js_obj_set(js, p, "Symbol.iterator", mk_native(js, string_iter, NULL));
}

/**
 * @brief 给 `Number.prototype` 安装方法 / Install the methods on `Number.prototype`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @note 安装 `toString`、`valueOf`、`toFixed`、`toPrecision`、`toExponential`、`toLocaleString` / Installs `toString`, `valueOf`, `toFixed`, `toPrecision`, `toExponential` and `toLocaleString`
 */
static void install_number_proto(HpeJs *js)
{
    JsObj *p = js->number_proto;
    def_method(js, p, "toString", number_this_toString, NULL);
    def_method(js, p, "valueOf", number_this_valueOf, NULL);
    def_method(js, p, "toFixed", number_toFixed, NULL);
    def_method(js, p, "toPrecision", number_toPrecision, NULL);
    def_method(js, p, "toExponential", number_toExponential, NULL);
    def_method(js, p, "toLocaleString", number_toLocaleString, NULL);
}

/**
 * @brief 给 `Boolean.prototype` 安装方法 / Install the methods on `Boolean.prototype`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @note 安装 `toString` 与 `valueOf` / Installs `toString` and `valueOf`
 */
static void install_bool_proto(HpeJs *js)
{
    JsObj *p = js->bool_proto;
    def_method(js, p, "toString", bool_this_toString, NULL);
    def_method(js, p, "valueOf", bool_this_valueOf, NULL);
}

/**
 * @brief 给 `RegExp.prototype` 安装方法 / Install the methods on `RegExp.prototype`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @note 安装 `test`、`exec`、`toString` / Installs `test`, `exec` and `toString`
 */
static void install_regex_proto(HpeJs *js)
{
    JsObj *p = js->regex_proto;
    def_method(js, p, "test", regex_test, NULL);
    def_method(js, p, "exec", regex_exec, NULL);
    def_method(js, p, "toString", regex_toString, NULL);
}

/**
 * @brief 给错误原型族安装方法与 `name`/`message` 默认值 / Install methods and default `name`/`message` on the error prototype family
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @note `Error.prototype` 得到 `toString`；各子错误原型设置各自的 `name` / `Error.prototype` gains `toString`; each derived error prototype gets its own `name`
 */
static void install_error_protos(HpeJs *js)
{
    JsObj *p = js->error_proto;
    def_method(js, p, "toString", error_this_toString, NULL);
    def_value(js, js->type_error_proto, "name", js_string(js, "TypeError"));
    def_value(js, js->range_error_proto, "name", js_string(js, "RangeError"));
    def_value(js, js->syntax_error_proto, "name", js_string(js, "SyntaxError"));
    def_value(js, js->ref_error_proto, "name", js_string(js, "ReferenceError"));
    def_value(js, p, "name", js_string(js, "Error"));
    def_value(js, p, "message", js_string(js, ""));
}

/**
 * @brief 给 `Promise.prototype` 安装方法 / Install the methods on `Promise.prototype`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @note 安装 `then`、`catch`、`finally` / Installs `then`, `catch` and `finally`
 */
static void install_promise_proto(HpeJs *js)
{
    JsObj *p = js->promise_proto;
    def_method(js, p, "then", promise_proto_then, NULL);
    def_method(js, p, "catch", promise_proto_catch, NULL);
    def_method(js, p, "finally", promise_proto_finally, NULL);
}

/**
 * @brief 给 `Map.prototype` 与 `Set.prototype` 安装方法与 size 取值器 / Install methods and the `size` getter on `Map.prototype` and `Set.prototype`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @note 两者均设 `Symbol.iterator` 为自迭代；Set 的 `keys`/`values` 共用值迭代器 / Both get `Symbol.iterator` set to self-iteration; the `Set` `keys`/`values` share the value iterator
 */
static void install_map_proto(HpeJs *js)
{
    JsObj *p = js->map_proto;
    JsObj *sp = js->set_proto;
    def_method(js, p, "set", map_set_impl, NULL);
    def_method(js, p, "get", map_get_impl, NULL);
    def_method(js, p, "has", map_has, NULL);
    def_method(js, p, "delete", map_delete, NULL);
    def_method(js, p, "clear", map_clear, NULL);
    def_method(js, p, "forEach", map_forEach, NULL);
    def_method(js, p, "keys", map_keys, NULL);
    def_method(js, p, "values", map_values, NULL);
    def_method(js, p, "entries", map_entries, NULL);
    js_obj_set(js, p, "Symbol.iterator", mk_native(js, map_set_iter_self, NULL));
    js_obj_set_accessor(js, p, "size", mk_native(js, map_size_fn, NULL), js_undef());
    if (sp) {
        js_obj_set_accessor(js, sp, "size", mk_native(js, map_size_fn, NULL), js_undef());
        def_method(js, sp, "add", set_add, NULL);
        def_method(js, sp, "has", set_has, NULL);
        def_method(js, sp, "delete", set_delete, NULL);
        def_method(js, sp, "clear", set_clear, NULL);
        def_method(js, sp, "forEach", set_foreach, NULL);
        def_method(js, sp, "values", set_values, NULL);
        def_method(js, sp, "keys", set_values, NULL);
        def_method(js, sp, "entries", set_entries, NULL);
        js_obj_set(js, sp, "Symbol.iterator", mk_native(js, map_set_iter_self, NULL));
    }
}

/**
 * @brief 从全局对象按名取回构造器对象 / Look up a constructor object on the global object by name
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     name 构造器名 / Constructor name
 *
 * @return 全局对象上该名的对象指针，不存在则为 NULL / The object pointer stored under that global name, or `NULL`
 */
static JsObj *ctor_of(HpeJs *js, const char *name)
{
    return js_obj_get(js, js->global_obj, name).obj;
}

/**
 * @brief 安装 `Object` 构造器与其静态方法 / Install the `Object` constructor and its static methods
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @note 静态方法含 `keys`/`values`/`entries`/`assign`/`create`/`defineProperty` 等，`is` 单独用 `object_is_value` 注册 / Statics include `keys`/`values`/`entries`/`assign`/`create`/`defineProperty` etc.; `is` is registered separately with `object_is_value`
 */
static void install_object_ctor(HpeJs *js)
{
    JsObj *oc;

    make_ctor(js, "Object", native_object_ctor, NULL, js->object_proto);
    oc = ctor_of(js, "Object");
    def_method(js, oc, "keys", object_keys, NULL);
    def_method(js, oc, "values", object_values, NULL);
    def_method(js, oc, "entries", object_entries, NULL);
    def_method(js, oc, "fromEntries", object_from_entries, NULL);
    def_method(js, oc, "getOwnPropertyNames", object_get_own_property_names, NULL);
    def_method(js, oc, "assign", object_assign, NULL);
    def_method(js, oc, "freeze", object_freeze, NULL);
    def_method(js, oc, "seal", object_seal, NULL);
    def_method(js, oc, "isSealed", object_is_sealed, NULL);
    def_method(js, oc, "isFrozen", object_is_frozen, NULL);
    def_method(js, oc, "create", object_create, NULL);
    def_method(js, oc, "defineProperty", object_define_property, NULL);
    def_method(js, oc, "getOwnPropertyDescriptor", object_get_property_descriptor, NULL);
    def_method(js, oc, "hasOwn", object_has_own, NULL);
    def_method(js, oc, "getPrototypeOf", object_get_prototype_of, NULL);
    def_method(js, oc, "setPrototypeOf", object_set_prototype_of, NULL);
    def_method(js, oc, "is", NULL, NULL);
    js_obj_set(js, oc, "is", mk_native(js, object_is_value, NULL));
}

/**
 * @brief 安装 `Array` 构造器与其静态方法 / Install the `Array` constructor and its static methods
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @note 安装 `from`、`of`、`isArray` / Installs `from`, `of` and `isArray`
 */
static void install_array_ctor(HpeJs *js)
{
    JsObj *ac;

    make_ctor(js, "Array", native_array_ctor, NULL, js->array_proto);
    ac = ctor_of(js, "Array");
    def_method(js, ac, "from", native_array_from, NULL);
    def_method(js, ac, "of", native_array_of, NULL);
    def_method(js, ac, "isArray", native_array_is_array, NULL);
}

/**
 * @brief 安装 `String` 构造器与其静态方法 / Install the `String` constructor and its static methods
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @note 安装 `fromCharCode` 与 `fromCodePoint` / Installs `fromCharCode` and `fromCodePoint`
 */
static void install_string_ctor(HpeJs *js)
{
    JsObj *sc;

    make_ctor(js, "String", native_string_ctor, NULL, js->string_proto);
    sc = ctor_of(js, "String");
    def_method(js, sc, "fromCharCode", string_from_char_code, NULL);
    def_method(js, sc, "fromCodePoint", string_from_code_point, NULL);
}

/**
 * @brief 安装 `Number` 构造器、静态方法与常量 / Install the `Number` constructor, its static methods and constants
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @note 含 `isInteger`/`isFinite`/`isNaN`/`isSafeInteger`、`parseInt`/`parseFloat` 及 `MAX_SAFE_INTEGER`/`NaN` 等常量 / Includes `isInteger`/`isFinite`/`isNaN`/`isSafeInteger`, `parseInt`/`parseFloat` and constants such as `MAX_SAFE_INTEGER`/`NaN`
 */
static void install_number_ctor(HpeJs *js)
{
    JsObj *nc;

    make_ctor(js, "Number", native_number_ctor, NULL, js->number_proto);
    nc = ctor_of(js, "Number");
    def_method(js, nc, "isInteger", number_is_integer, NULL);
    def_method(js, nc, "isFinite", number_is_finite_fn, NULL);
    def_method(js, nc, "isNaN", number_is_nan_fn, NULL);
    def_method(js, nc, "isSafeInteger", number_is_safe_integer, NULL);
    def_method(js, nc, "parseInt", native_parse_int, NULL);
    def_method(js, nc, "parseFloat", native_parse_float, NULL);
    def_value(js, nc, "MAX_SAFE_INTEGER", js_num(9007199254740991.0));
    def_value(js, nc, "MIN_SAFE_INTEGER", js_num(-9007199254740991.0));
    def_value(js, nc, "MAX_VALUE", js_num(1.7976931348623157e308));
    def_value(js, nc, "MIN_VALUE", js_num(5e-324));
    def_value(js, nc, "POSITIVE_INFINITY", js_num(INFINITY));
    def_value(js, nc, "NEGATIVE_INFINITY", js_num(-INFINITY));
    def_value(js, nc, "NaN", js_num(NAN));
    def_value(js, nc, "EPSILON", js_num(2.220446049250313e-16));
}

/**
 * @brief 安装 `Error` 及其子类构造器 / Install the `Error` constructor and its subclasses
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @note 含 `Error`、`TypeError`、`RangeError`、`SyntaxError`、`ReferenceError`，均用 `error_ctor` 并以名字作 `ud` / Covers `Error`, `TypeError`, `RangeError`, `SyntaxError` and `ReferenceError`, all using `error_ctor` with the name as `ud`
 */
static void install_error_ctors(HpeJs *js)
{
    make_ctor(js, "Error", error_ctor, (void *)"Error", js->error_proto);
    make_ctor(js, "TypeError", error_ctor, (void *)"TypeError", js->type_error_proto);
    make_ctor(js, "RangeError", error_ctor, (void *)"RangeError", js->range_error_proto);
    make_ctor(js, "SyntaxError", error_ctor, (void *)"SyntaxError", js->syntax_error_proto);
    make_ctor(js, "ReferenceError", error_ctor, (void *)"ReferenceError", js->ref_error_proto);
}

/**
 * @brief 安装 `Promise` 构造器与其静态方法 / Install the `Promise` constructor and its static methods
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @note 安装 `resolve`、`reject`、`all`、`race`、`allSettled` / Installs `resolve`, `reject`, `all`, `race` and `allSettled`
 */
static void install_promise_ctor(HpeJs *js)
{
    JsObj *pc;

    make_ctor(js, "Promise", promise_ctor_native, NULL, js->promise_proto);
    pc = ctor_of(js, "Promise");
    def_method(js, pc, "resolve", promise_static_resolve, NULL);
    def_method(js, pc, "reject", promise_static_reject, NULL);
    def_method(js, pc, "all", promise_static_all, NULL);
    def_method(js, pc, "race", promise_static_race, NULL);
    def_method(js, pc, "allSettled", promise_static_all, NULL);
}

/**
 * @brief 在 `Set` 构造器对象上安装静态方法 / Install static methods on the `Set` constructor object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @note 按规范 `values`/`keys`/`entries` 均指向值迭代器 / Per spec `values`/`keys`/`entries` all point at the value iterator
 */
static void install_set_statics(HpeJs *js)
{
    JsObj *sc = ctor_of(js, "Set");

    def_method(js, sc, "add", set_add, NULL);
    def_method(js, sc, "has", set_has, NULL);
    def_method(js, sc, "delete", set_delete, NULL);
    def_method(js, sc, "clear", set_clear, NULL);
    def_method(js, sc, "forEach", set_foreach, NULL);
    /* `values`/`keys`/`entries` all share the value iterator per the spec */
    def_value(js, sc, "values", mk_native(js, set_values, NULL));
    def_value(js, sc, "keys", mk_native(js, set_values, NULL));
    def_value(js, sc, "entries", mk_native(js, set_values, NULL));
}

/**
 * @brief 安装 `Map` 与 `Set` 构造器 / Install the `Map` and `Set` constructors
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @note 随后调用 `install_set_statics` 补上 Set 的静态方法 / Followed by `install_set_statics` to add the `Set` statics
 */
static void install_map_set_ctors(HpeJs *js)
{
    make_ctor(js, "Map", native_map_ctor, NULL, js->map_proto);
    make_ctor(js, "Set", native_set_ctor, NULL, js->set_proto);
    install_set_statics(js);
}

/**
 * @brief 安装 `Symbol` 构造器、内置符号与方法 / Install the `Symbol` constructor, well-known symbols and methods
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @note 注册 `iterator`/`toStringTag`/`asyncIterator` 内置符号，并把 `for` 与原型 `toString` 接上 / Registers the `iterator`/`toStringTag`/`asyncIterator` symbols and wires up `for` and the prototype `toString`
 */
static void install_symbol_ctor(HpeJs *js)
{
    JsObj *sc;

    make_ctor(js, "Symbol", native_symbol, NULL, js->symbol_proto);
    sc = ctor_of(js, "Symbol");
    def_value(js, sc, "iterator", symbol_brand(js, "Symbol.iterator"));
    def_value(js, sc, "toStringTag", symbol_brand(js, "Symbol.toStringTag"));
    def_value(js, sc, "asyncIterator", symbol_brand(js, "Symbol.asyncIterator"));
    def_method(js, sc, "for", native_symbol, NULL);
    def_method(js, js->symbol_proto, "toString", symbol_toString, NULL);
}

/* one row per `Math` function whose signature is `(x)` or `(x,y)`; the last
 * argument threads the `Math.max` / `Math.min` selection flag through. */
typedef struct MathFn {
    const char *name;
    JsNativeFn fn;
    void *ud;
} MathFn;

/**
 * @brief 按表把 Math 函数安装到 Math 对象上 / Install the `Math` functions onto the math object from a table
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] math 目标 `Math` 对象 / Target `Math` object
 *
 * @note 表中最后一项 `ud` 用来区分 `Math.max`/`Math.min`；`log1p` 等扩展函数用 `MF_*` 选择码作 `ud` / The trailing `ud` distinguishes `Math.max`/`Math.min`; extra functions like `log1p` carry an `MF_*` selector as `ud`
 */
static void install_math_fn_table(HpeJs *js, JsObj *math)
{
    size_t i;
    static const MathFn fns[] = {
        { "abs", math_abs, NULL },     { "ceil", math_ceil, NULL },
        { "floor", math_floor, NULL }, { "trunc", math_trunc, NULL },
        { "round", math_round, NULL }, { "sqrt", math_sqrt, NULL },
        { "cbrt", math_cbrt, NULL },   { "exp", math_exp, NULL },
        { "log", math_log, NULL },     { "log2", math_log2, NULL },
        { "log10", math_log10, NULL }, { "sin", math_sin, NULL },
        { "cos", math_cos, NULL },     { "tan", math_tan, NULL },
        { "asin", math_asin, NULL },   { "acos", math_acos, NULL },
        { "atan", math_atan, NULL },   { "atan2", math_atan2, NULL },
        { "sinh", math_sinh, NULL },   { "cosh", math_cosh, NULL },
        { "tanh", math_tanh, NULL },   { "sign", math_sign, NULL },
        { "pow", math_pow, NULL },     { "hypot", math_hypot, NULL },
        { "min", math_minmax, NULL },  { "max", math_minmax, (void *)1 },
        { "random", math_random, NULL },
        { "clz32", math_clz32, NULL }, { "imul", math_imul, NULL },
        { "fround", math_fround, NULL },
        { "log1p", math_extra, (void *)(intptr_t)MF_LOG1P },
        { "expm1", math_extra, (void *)(intptr_t)MF_EXPM1 },
        { "acosh", math_extra, (void *)(intptr_t)MF_ACOSH },
        { "asinh", math_extra, (void *)(intptr_t)MF_ASINH },
        { "atanh", math_extra, (void *)(intptr_t)MF_ATANH },
    };

    for (i = 0; i < sizeof(fns) / sizeof(fns[0]); i++)
        def_method(js, math, fns[i].name, fns[i].fn, fns[i].ud);
}

/**
 * @brief 把数学常量安装到 Math 对象上 / Install the mathematical constants onto the math object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] math 目标 `Math` 对象 / Target `Math` object
 *
 * @note 含 `PI`、`E`、`LN2`、`LN10`、`SQRT2`、`SQRT1_2` / Includes `PI`, `E`, `LN2`, `LN10`, `SQRT2` and `SQRT1_2`
 */
static void install_math_consts(HpeJs *js, JsObj *math)
{
    def_value(js, math, "PI", js_num(3.141592653589793));
    def_value(js, math, "E", js_num(2.718281828459045));
    def_value(js, math, "LN2", js_num(0.6931471805599453));
    def_value(js, math, "LN10", js_num(2.302585092994046));
    def_value(js, math, "SQRT2", js_num(1.4142135623730951));
    def_value(js, math, "SQRT1_2", js_num(0.7071067811865476));
}

/**
 * @brief 创建并注册全局 `Math` 对象 / Create and register the global `Math` object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @note 先建对象并设类名，再装函数表与常量 / Builds the object, sets its class name, then installs the function table and constants
 */
static void install_math(HpeJs *js)
{
    JsObj *math = js_object_new(js);

    math->class_name = "Math";
    install_math_fn_table(js, math);
    install_math_consts(js, math);
    js_obj_set(js, js->global_obj, "Math", js_objval(math));
}

/**
 * @brief 创建并注册全局 `JSON` 对象 / Create and register the global `JSON` object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @note 安装 `stringify` 与 `parse` / Installs `stringify` and `parse`
 */
static void install_json(HpeJs *js)
{
    JsObj *json = js_object_new(js);

    json->class_name = "JSON";
    def_method(js, json, "stringify", json_stringify, NULL);
    def_method(js, json, "parse", json_parse, NULL);
    js_obj_set(js, js->global_obj, "JSON", js_objval(json));
}

/**
 * @brief 在全局对象上安装全局函数与常量 / Install the global functions and constants on the global object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @note 含 `parseInt`/`parseFloat`/`isNaN`/`isFinite`、URI 编解码、`NaN`/`Infinity`/`undefined`；`encodeURI` 复用组件编解码并带保留字符标志 / Includes `parseInt`/`parseFloat`/`isNaN`/`isFinite`, the URI codecs and `NaN`/`Infinity`/`undefined`; `encodeURI` reuses the component codec with the reserved-chars flag
 */
static void install_global_functions(HpeJs *js)
{
    JsObj *g = js->global_obj;

    def_value(js, g, "parseInt", mk_native(js, native_parse_int, NULL));
    def_value(js, g, "parseFloat", mk_native(js, native_parse_float, NULL));
    def_value(js, g, "isNaN", mk_native(js, native_is_nan, NULL));
    def_value(js, g, "isFinite", mk_native(js, native_is_finite, NULL));
    def_value(js, g, "encodeURIComponent", mk_native(js, native_encode_uri_component, NULL));
    def_value(js, g, "decodeURIComponent", mk_native(js, native_decode_uri_component, NULL));
    /* `encodeURI` reuses the component codec with the "keep reserved chars" flag */
    def_value(js, g, "encodeURI", mk_native(js, native_encode_uri_component, (void *)1));
    def_value(js, g, "decodeURI", mk_native(js, native_decode_uri_component, NULL));
    def_value(js, g, "NaN", js_num(NAN));
    def_value(js, g, "Infinity", js_num(INFINITY));
    def_value(js, g, "undefined", js_undef());
}

/**
 * @brief 创建并注册全局 `console` 对象 / Create and register the global `console` object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @note 各方法共用 `native_console`，`ud` 非空表示写到 stderr / All methods share `native_console`; a non-NULL `ud` selects stderr
 */
static void install_console(HpeJs *js)
{
    static const MathFn methods[] = {
        { "log", native_console, NULL },    { "info", native_console, NULL },
        { "debug", native_console, NULL },  { "warn", native_console, (void *)1 },
        { "error", native_console, (void *)1 },
        { "assert", native_console, NULL }, { "trace", native_console, (void *)1 },
        { "dir", native_console, NULL },
    };
    JsObj *console = js_object_new(js);
    size_t i;

    console->class_name = "Console";
    for (i = 0; i < sizeof(methods) / sizeof(methods[0]); i++)
        def_method(js, console, methods[i].name, methods[i].fn, methods[i].ud);
    js_obj_set(js, js->global_obj, "console", js_objval(console));
}

/**
 * @brief 把全部内建构造函数与对象装到全局对象上 / Install all builtin constructors and objects onto the global object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @note 含各构造器、`Math`/`JSON`/`console`、`alert` 与 `globalThis`；日期由 `hpe_js_date_install` 单独安装 / Covers the constructors, `Math`/`JSON`/`console`, `alert` and `globalThis`; `Date` is installed separately by `hpe_js_date_install`
 */
static void install_global_object(HpeJs *js)
{
    JsValue global = js_objval(js->global_obj);

    install_object_ctor(js);
    make_ctor(js, "Function", function_ctor, NULL, js->func_proto);
    install_array_ctor(js);
    install_string_ctor(js);
    install_number_ctor(js);
    make_ctor(js, "Boolean", native_boolean_ctor, NULL, js->bool_proto);
    install_error_ctors(js);
    install_promise_ctor(js);
    make_ctor(js, "RegExp", native_regexp_ctor, NULL, js->regex_proto);
    install_map_set_ctors(js);
    install_symbol_ctor(js);
    install_math(js);
    install_json(js);
    hpe_js_date_install(js);
    install_global_functions(js);
    install_console(js);
    def_value(js, js->global_obj, "alert", mk_native(js, native_alert, NULL));
    def_value(js, js->global_obj, "globalThis", global);
    HPE_UNUSED(global);
}

/**
 * @brief 创建并初始化一个 JS 运行时实例 / Create and initialise a JS runtime instance
 *
 * @return 成功返回新实例；内存分配失败返回 NULL / The new instance, or `NULL` when allocation fails
 *
 * @note 依次初始化各原型、全局对象/环境、内建对象与定时器 / Initialises the prototypes, global object/environment, builtins and timers in order
 *
 * @see hpe_js_destroy
 */

HpeJs *hpe_js_create(void)
{
    HpeJs *js = (HpeJs *)calloc(1, sizeof(HpeJs));
    if (!js)
        return NULL;
    js->error = 0;
    js->timer_cap = 0;
    js->strict = 0;
    init_prototypes(js);
    install_object_proto(js);
    install_function_proto(js);
    install_array_proto(js);
    install_string_proto(js);
    install_number_proto(js);
    install_bool_proto(js);
    install_regex_proto(js);
    install_error_protos(js);
    install_promise_proto(js);
    install_map_proto(js);
    {
        /* Error()/new Error() set their prototype through the constructor, but a
         * thrown plain object still needs a sane `.name`. */
        JsValue e = js_make_error(js, "Error", "");
        HPE_UNUSED(e);
    }
    js->global_obj = js_object_new(js);
    js->global_env = js_env_new(js, NULL);
    js->global_env->has_this = 1;
    js->global_env->thisv = js_objval(js->global_obj);
    install_global_object(js);
    hpe_js_timers_install(js);
    return js;
}

/**
 * @brief 销毁 JS 运行时并释放其资源 / Destroy a JS runtime and release its resources
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @note 先卸载 DOM 变更监听，再释放块、定时器与模块链表；`js` 为 NULL 时直接返回 / Uninstalls the DOM mutation sink first, then frees blocks, timers and the module list; returns immediately when `js` is `NULL`
 *
 * @see hpe_js_create
 */
void hpe_js_destroy(HpeJs *js)
{
    JsBlock *b;
    JsModule *m;
    if (!js)
        return;
    /* drop the DOM mutation sink: the js arena (and its observer state) is about
     * to be freed, so the DOM must stop forwarding to it */
    hpe_js_mutation_uninstall(js);
    b = js->blocks;
    while (b) {
        JsBlock *next = b->next;
        free(b);
        b = next;
    }
    /* timers and modules are malloc'd */
    if (js->timers)
        free(js->timers);
    m = js->modules;
    while (m) {
        JsModule *next = m->next;
        if (m->path)
            free(m->path);
        if (m->src)
            free(m->src);
        free(m);
        m = next;
    }
    free(js);
}

/**
 * @brief 把宿主对象暴露为全局变量 / Expose a host object as a global
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     name 全局变量名（如 `document`）/ Global variable name, e.g. `document`
 * @param[in,out] ptr 宿主对象指针 / Host object pointer
 * @param[in]     type 宿主对象类型标记 / Host object type tag
 * @param[in]     vt 宿主对象虚表 / Host object vtable
 *
 * @note 以宿主值形式写入全局对象 / Writes a host value onto the global object
 */
void hpe_js_set_global_host(HpeJs *js, const char *name, void *ptr, int type,
                            const HpeJsHostVt *vt)
{
    js_obj_set(js, js->global_obj, name, js_make_host(js, ptr, type, vt));
}

/**
 * @brief `hpe_js_eval`/`hpe_js_eval_module` 的公共实现 / Shared implementation behind `hpe_js_eval` and `hpe_js_eval_module`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     source 源文本 / Source text
 * @param[in]     module 非 0 时按 ES 模块解析执行 / Non-zero parses and runs the source as an ES module
 * @param[out]    errbuf 失败时的错误信息缓冲区 / Error-message buffer filled on failure
 * @param[in]     errbuf_size 错误缓冲区容量 / Error buffer capacity
 *
 * @return 成功返回 0、失败返回 -1 / 0 on success, -1 on failure
 *
 * @note 用 try 帧捕获异常并写入 errbuf；模块模式下先建立模块环境与命名空间，执行后统一抽取微任务 / Uses a try frame to catch exceptions into `errbuf`; in module mode it builds the module environment and namespace first, then drains microtasks after execution
 */
static int eval_common(HpeJs *js, const char *source, int module, char *errbuf, size_t errbuf_size)
{
    JsNode *prog;
    JsTryFrame frame;
    int i;

    if (!js)
        return -1;
    if (js->thrown.t != JS_UNDEF)
        js->thrown = js_undef();
    frame.prev = js->try_top;
    js->try_top = &frame;
    if (setjmp(frame.jmp) != 0) {
        js->try_top = frame.prev;
        if (errbuf && errbuf_size) {
            char *s = js_to_string(js, js->thrown);
            snprintf(errbuf, errbuf_size, "%s", s ? s : "error");
        }
        js->thrown = js_undef();
        js->in_catch = 0;
        return -1;
    }
    prog = module ? js_parse_module(js, source) : js_parse_program(js, source);
    {
        JsEnv *env = js->global_env;
        if (module) {
            JsModule *m = (JsModule *)malloc(sizeof(JsModule));
            memset(m, 0, sizeof(*m));
            m->path = hpe_js_strdup(js, "<eval>");
            m->env = env;
            m->ns = js_module_ns_new(js, m);
            m->ns->proto = NULL;
            js->current_module = m;
        }
        hoist_decls(js, env, prog);
        for (i = 0; i < prog->kid_count; i++) {
            if (exec_stmt(js, env, prog->kids[i]).action != FLOW_NONE)
                break;
        }
    }
    hpe_js_drain_jobs(js);
    js->try_top = frame.prev;
    js->in_catch = 0;
    return 0;
}

/**
 * @brief 以脚本方式执行一段源码 / Execute a piece of source as a script
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     source 源文本 / Source text
 * @param[out]    errbuf 失败时的错误信息缓冲区 / Error-message buffer filled on failure
 * @param[in]     errbuf_size 错误缓冲区容量 / Error buffer capacity
 *
 * @return 成功返回 0、失败返回 -1 / 0 on success, -1 on failure
 *
 * @note 等价于以非模块方式调用 `eval_common` / Equivalent to calling `eval_common` in non-module mode
 *
 * @see eval_common
 */
int hpe_js_eval(HpeJs *js, const char *source, char *errbuf, size_t errbuf_size)
{
    return eval_common(js, source, 0, errbuf, errbuf_size);
}

/**
 * @brief 以 ES 模块方式执行一段源码 / Execute a piece of source as an ES module
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     source 源文本 / Source text
 * @param[in]     dir 解析相对导入的基准目录（可为空）/ Base directory for resolving relative imports, may be empty
 * @param[out]    errbuf 失败时的错误信息缓冲区 / Error-message buffer filled on failure
 * @param[in]     errbuf_size 错误缓冲区容量 / Error buffer capacity
 *
 * @return 成功返回 0、失败返回 -1 / 0 on success, -1 on failure
 *
 * @note `dir` 非空时先更新当前目录，再以模块方式调用 `eval_common`；命名空间可经 import 访问 / When `dir` is non-empty it is set as the current directory first; the namespace is reachable through imports
 *
 * @see eval_common
 */
int hpe_js_eval_module(HpeJs *js, const char *source, const char *dir, char *errbuf,
                       size_t errbuf_size)
{
    if (dir && *dir)
        hpe_js_set_current_dir(js, dir);
    return eval_common(js, source, 1, errbuf, errbuf_size);
}

/**
 * @brief 从 C 侧按值调用一个可调用对象 / Call a callable value from C
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     callee 被调用的可调用值（如事件处理函数）/ The callable value to call, e.g. an event handler
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 实参个数 / Argument count
 *
 * @return 成功返回 0、运行时错误返回 -1 / 0 on success, -1 on runtime error
 *
 * @note 运行时错误打印到 stderr；`argc` 为 0 时 `thisv`/`args` 可为 NULL；调用后抽取微任务 / Runtime errors go to stderr; `thisv`/`args` may be `NULL` when `argc` is 0; microtasks are drained after the call
 */
int hpe_js_call(HpeJs *js, JsValue callee, JsValue thisv, JsValue *args, int argc)
{
    JsTryFrame frame;
    if (!js)
        return -1;
    frame.prev = js->try_top;
    js->try_top = &frame;
    if (setjmp(frame.jmp) != 0) {
        js->try_top = frame.prev;
        fprintf(stderr, "hope: uncaught exception in callback: %s\n",
                js_to_string(js, js->thrown));
        js->thrown = js_undef();
        js->in_catch = 0;
        return -1;
    }
    call_value(js, NULL, callee, thisv, args, argc, 0);
    hpe_js_drain_jobs(js);
    js->try_top = frame.prev;
    js->in_catch = 0;
    return 0;
}

/**
 * @brief 从 C 侧调用可调用对象并返回其结果值 / Call a callable value from C and return its result value
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     callee 被调用的可调用值 / The callable value to call
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 实参个数 / Argument count
 *
 * @return 调用结果值；`js` 为空或 `callee` 不可调用时返回 undefined / The call result, or `undefined` when `js` is `NULL` or `callee` is not callable
 *
 * @note 不捕获异常，与 `hpe_js_call` 不同 / Unlike `hpe_js_call`, it does not catch exceptions
 */
JsValue hpe_js_call_value(HpeJs *js, JsValue callee, JsValue thisv, JsValue *args, int argc)
{
    if (!js || !js_is_callable(callee))
        return js_undef();
    return call_value(js, NULL, callee, thisv, args, argc, 0);
}

/**
 * @brief 注册用于解析 `import` 的模块加载器 / Register the module loader used to resolve `import` specifiers
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     loader 模块加载器回调（为 NULL 时清除并置零）/ Module loader callback; `NULL` clears it
 */
void hpe_js_set_module_loader(HpeJs *js, const HpeJsModuleLoader *loader)
{
    if (!js)
        return;
    if (loader)
        js->loader = *loader;
    else
        memset(&js->loader, 0, sizeof(js->loader));
}

/**
 * @brief 设置解析相对模块说明符的基准目录 / Set the base directory relative module specifiers resolve against
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     dir 基准目录 / Base directory
 *
 * @note 仅保存指针，不复制字符串 / Only the pointer is stored, the string is not copied
 */
void hpe_js_set_current_dir(HpeJs *js, const char *dir)
{
    if (js)
        js->current_dir = dir;
}

/**
 * @brief 取回全局对象的原始对象指针 / Get the raw object pointer of the global object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @return 全局对象指针；`js` 为空时返回 NULL / The global object, or `NULL` when `js` is `NULL`
 */
JsObj *hpe_js_global_object(HpeJs *js)
{
    return js ? js->global_obj : NULL;
}


