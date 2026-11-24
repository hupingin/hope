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

/* ---------------- ast helpers ---------------- */

JsNode *js_node_new(HpeJs *js, int kind, int line)
{
    JsNode *n = (JsNode *)hpe_js_alloc(js, sizeof(JsNode));
    n->kind = kind;
    n->line = line;
    return n;
}

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

/* ---------------- small helpers ---------------- */

static JsValue bind_native(HpeJs *js, JsNativeFn fn, void *ud)
{
    return js_objval(js_native_new(js, fn, ud));
}

static JsValue array_get_index(HpeJs *js, JsObj *arr, int i)
{
    char key[24];
    snprintf(key, sizeof(key), "%d", i);
    return js_obj_get(js, arr, key);
}

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

static void array_push_value(HpeJs *js, JsObj *arr, JsValue v)
{
    int len = (int)js_to_number(js, js_obj_get(js, arr, "length"));
    array_set_index(js, arr, len, v);
}

static int array_length(HpeJs *js, JsObj *arr)
{
    double len = js_to_number(js, js_obj_get(js, arr, "length"));
    if (len != len || len < 0.0 || len > 1e9)
        return 0;
    return (int)len;
}

static JsValue array_of(HpeJs *js, JsValue *v, int n)
{
    JsObj *a = js_array_new(js);
    int i;
    for (i = 0; i < n; i++)
        array_push_value(js, a, v[i]);
    return js_objval(a);
}

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

/* ---------------- microtask queue ---------------- */

void js_job_push(HpeJs *js, JsJob *job)
{
    job->next = NULL;
    if (js->jobs_tail)
        js->jobs_tail->next = job;
    else
        js->jobs_head = job;
    js->jobs_tail = job;
}

static JsJob *job_new(HpeJs *js, int kind)
{
    JsJob *j = (JsJob *)hpe_js_alloc(js, sizeof(JsJob));
    j->kind = kind;
    j->fn = js_undef();
    j->arg = js_undef();
    j->value = js_undef();
    return j;
}

/* queues fn(arg) to run as a microtask (JOB_CALL). MutationObserver uses this to
 * deliver records asynchronously, but any host binding may. The queue is drained
 * by the interpreter after the current statement/script and by hpe_js_call(). */
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

static JsValue promise_then(HpeJs *js, JsObj *p, JsValue on_ok, JsValue on_err)
{
    JsObj *next = js_promise_new(js);
    reaction_push(js, p, on_ok, on_err, next);
    hpe_js_drain_jobs(js);
    return js_objval(next);
}

/* ---------------- iterators ---------------- */

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

static JsValue native_iter_self(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HPE_UNUSED(js);
    HPE_UNUSED(args);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    return thisv;
}

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

/* ---------------- primitives: property access ---------------- */

static JsObj *proto_of_string(HpeJs *js)
{
    return js->string_proto;
}

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

static JsValue proto_lookup(HpeJs *js, JsObj *proto, JsValue thisv, const char *prop, int line)
{
    JsProp *p = proto ? js_obj_find_prop(proto, prop) : NULL;
    if (!p)
        return js_undef();
    if (p->getter.t != JS_UNDEF)
        return call_value(js, NULL, p->getter, thisv, NULL, 0, line);
    return p->val;
}

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

JsValue js_get_prop(HpeJs *js, JsEnv *env, JsValue base, const char *prop, int line)
{
    return member_get(js, env, base, prop, line);
}

void js_set_prop(HpeJs *js, JsEnv *env, JsValue base, const char *prop, JsValue value, int line)
{
    member_set(js, env, base, prop, value, line);
}

/* ---------------- calling ---------------- */

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

/* generators materialise their yields eagerly into an array (design §12.7) */
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

static JsValue run_async(HpeJs *js, JsObj *fn, JsEnv *closure, JsValue this_v, JsValue *args,
                         int argc)
{
    JsObj *p = js_promise_new(js);
    JsValue out = run_function_body(js, fn, closure, this_v, args, argc);
    promise_resolve_with(js, p, out);
    hpe_js_drain_jobs(js);
    return js_objval(p);
}

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

JsValue js_invoke(HpeJs *js, JsValue fn, JsValue thisv, JsValue *args, int argc, int line)
{
    return call_value(js, NULL, fn, thisv, args, argc, line);
}

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

/* ---------------- destructuring ---------------- */

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

/* Destructuring is implemented twice — once for declarations (`declare != 0`,
 * routed through bind_target) and once for assignments — but the traversal is
 * identical. `apply` is the single leaf operation; the helpers below share all
 * the structural walking. The walkers take a 4-argument callback, so the
 * declaration form of bind_pattern (which also carries `declare`) is reached
 * through these two adapters. */
static void pattern_bind_decl(HpeJs *js, JsEnv *env, JsNode *pat, JsValue value)
{
    bind_pattern(js, env, pat, value, 1);
}

static void pattern_bind_plain(HpeJs *js, JsEnv *env, JsNode *pat, JsValue value)
{
    bind_pattern(js, env, pat, value, 0);
}

/* `[a, b, ...rest]` over an iterable value */
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

/* `{a, b: c, ...rest}` over an object value; `exclude` (may be NULL) collects
 * the keys already destructured so a following `...rest` can skip them */
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

/* a `= default` pattern substitutes the default only for `undefined` */
static JsValue pattern_default_value(HpeJs *js, JsEnv *env, JsNode *pat, JsValue value)
{
    if (value.t == JS_UNDEF)
        return eval_expr(js, env, pat->kids[1]);
    return value;
}

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

static void assign_member(HpeJs *js, JsEnv *env, JsNode *target, JsValue value)
{
    JsValue base = eval_expr(js, env, target->kids[0]);
    char *prop = target->op == JS_OP_DOT
                     ? target->str
                     : js_to_string(js, eval_expr(js, env, target->kids[1]));
    member_set(js, env, base, prop, value, target->line);
}

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

/* ---------------- operators ---------------- */

static int32_t to_i32(HpeJs *js, JsValue v)
{
    double d = js_to_number(js, v);
    if (d != d || isinf(d))
        return 0;
    return (int32_t)(int64_t)fmod(trunc(d), 4294967296.0);
}

static uint32_t to_u32(HpeJs *js, JsValue v)
{
    double d = js_to_number(js, v);
    if (d != d || isinf(d))
        return 0;
    return (uint32_t)(int64_t)(fmod(trunc(d), 4294967296.0) + 4294967296.0);
}

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

/* `a + b` where at least one string-converted operand is a string */
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

/* `&&`, `||` and `??` short-circuit and yield the deciding operand itself
 * rather than a coerced boolean */
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

/* `<`, `>`, `<=`, `>=`; string operands compare lexicographically, everything
 * else numerically, and a NaN operand always yields false */
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

/* arithmetic, bitwise and shift operators that reduce both operands to numbers */
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

/* `x in obj`: integer keys are formatted without a fractional part */
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

/* `+`: string concatenation when either side is (or degrades to) a string,
 * otherwise numeric addition. Objects are coerced via ToPrimitive first. */
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

/* `a += b` etc.: the same operators as eval_bin, applied to the read-back lvalue
 * and the RHS. `+=` concatenates when either operand is a string. */
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

/* `a ??= b` / `||=` / `&&=`: assign only when the current value is nullish /
 * falsy / truthy respectively, and yield the untouched value otherwise */
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
/* ---------------- classes ---------------- */

static JsValue make_method(HpeJs *js, JsEnv *env, JsNode *fn_node)
{
    JsObj *fn = js_func_new(js, fn_node, env);
    fn->fn_flags = fn_node->op;
    if (fn_node->str)
        js_obj_set(js, fn, "name", js_string(js, fn_node->str));
    return js_objval(fn);
}

/* like make_method, but records the [[HomeObject]] so `super.x` resolves */
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

/* the parser stores the method kind in JsNode.num and the function flags in op */
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

/* the four `static` member kinds install on the constructor; the rest go on the
 * prototype */
static int class_is_static_kind(int kind)
{
    return kind == K_STATIC_METHOD || kind == K_STATIC_GET || kind == K_STATIC_SET ||
           kind == K_STATIC_FIELD;
}

/* bind one class member onto the prototype or the constructor, depending on the
 * member kind */
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

/* find the explicit `constructor(...)` member, or NULL when the class body
 * omits one */
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

/* a class without an explicit constructor gets a synthetic one so that `new`
 * and `super()` still have a function object to run */
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

/* key resolution for object literals: computed keys carry the key expression
 * as kids[1] when kids[0] is the value (the parser never stores a key kind) */
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

static JsValue eval_member(HpeJs *js, JsEnv *env, JsNode *n)
{
    JsValue base = eval_expr(js, env, n->kids[0]);
    char *prop = n->op == JS_OP_DOT ? n->str : js_to_string(js, eval_expr(js, env, n->kids[1]));
    if (n->num != 0 && (base.t == JS_NULL || base.t == JS_UNDEF))
        return js_undef(); /* optional chain */
    return member_get(js, env, base, prop, n->line);
}

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

static void args_init(ArgList *al, HpeJs *js, int cap)
{
    if (cap < 1)
        cap = 1;
    al->cap = cap;
    al->count = 0;
    al->items = (JsValue *)hpe_js_alloc(js, (size_t)cap * sizeof(JsValue));
}

/* geometric-ish growth (+8) keeps the inner spread loop cheap */
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

/* evaluates the argument nodes from `first` onward, flattening spread elements
 * through the iterator protocol so calls and constructors share one code path */
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

/* resolves the callee and its `this` binding. A method call binds `this` to the
 * base object; `super.m()` binds the class's own `this`. Returns 0 only when an
 * optional call (`?.`) short-circuits on a null/undefined callee. */
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

/* `typeof` result name for a runtime value, per the JS type table */
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

/* `delete obj.prop` / `delete obj[key]` / `delete someVar`: a failed delete of
 * a non-configurable property is reported as success, matching the sloppy-mode
 * behaviour the rest of the interpreter assumes. */
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

/* the four unary operators that are not `delete`/`typeof` */
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

/* `[...a, b]`: spreads iterate the operand, holes and plain elements push as-is */
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

/* `await expr`: settle the promise job queue, then unwrap or rethrow */
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

/* `yield` / `yield*`: values are collected into the driver's yield sink */
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

/* resolve an identifier: the innermost binding, then the global object's
 * properties (so `Math`, `undefined` etc. resolve), else a ReferenceError */
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

/* `this` resolves to the nearest enclosing environment that bound one, falling
 * back to the global object at the top level */
static JsValue eval_this(HpeJs *js, JsEnv *env)
{
    JsEnv *e = env;

    while (e && !e->has_this)
        e = e->parent;
    if (!e)
        return js_objval(js->global_obj);
    return e->thisv;
}

/* the comma operator: evaluate left to right, yield the last value */
static JsValue eval_seq(HpeJs *js, JsEnv *env, JsNode *n)
{
    JsValue v = js_undef();
    int i;

    for (i = 0; i < n->kid_count; i++)
        v = eval_expr(js, env, n->kids[i]);
    return v;
}

/* `++`/`--`: read, compute, write back, and yield either the old (postfix) or
 * the new (prefix) value */
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

static JsValue eval_unary(HpeJs *js, JsEnv *env, JsNode *n)
{
    if (n->op == JS_OP_DELETE)
        return eval_delete(js, env, n->kids[0]);
    if (n->op == JS_OP_TYPEOF)
        return js_string(js, typeof_name(eval_expr(js, env, n->kids[0])));
    return eval_unary_arithmetic(js, n, eval_expr(js, env, n->kids[0]));
}

/* one arm of the expression dispatch: a literal-valued node kind */
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

static JsValue eval_this_node(HpeJs *js, JsEnv *env, JsNode *n)
{
    HPE_UNUSED(n);
    return eval_this(js, env);
}

static JsValue eval_ternary(HpeJs *js, JsEnv *env, JsNode *n)
{
    return js_truthy(eval_expr(js, env, n->kids[0])) ? eval_expr(js, env, n->kids[1])
                                                     : eval_expr(js, env, n->kids[2]);
}

static JsValue eval_regex_node(HpeJs *js, JsEnv *env, JsNode *n)
{
    /* the parser packs the pattern and its flags into one buffer */
    const char *pat = n->str ? n->str : "";

    HPE_UNUSED(env);
    return js_regex_new(js, pat, pat + strlen(pat) + 1);
}

static JsValue eval_spread(HpeJs *js, JsEnv *env, JsNode *n)
{
    return eval_expr(js, env, n->kids[0]);
}

static JsValue eval_yield_node(HpeJs *js, JsEnv *env, JsNode *n)
{
    return eval_yield(js, env, n, n->op == 1);
}

/* `{ }` as an expression yields undefined; used for the empty statement body */
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

/* hoists function declarations and `var` bindings into the function/program
 * scope so calls before the declaration work (function-scoped hoisting) */
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

/* ---------------- loops (label aware) ----------------
 * `break` / `continue` are implemented by unwinding a Flow value through the
 * statement executor. A loop consumes the flows that target itself, i.e. an
 * unlabelled break/continue, or a label that names one of the labels attached
 * to this very loop; anything else is passed outward to an enclosing loop.
 */

static int is_loop_node(JsNode *n)
{
    if (!n)
        return 0;
    return n->kind == JS_N_WHILE || n->kind == JS_N_DO || n->kind == JS_N_FOR ||
           n->kind == JS_N_FOROF || n->kind == JS_N_FORIN;
}

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

/* 0 = keep iterating, 1 = leave the loop, 2 = propagate `*out` to the caller */
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

/* `for (let x of ...)` / `for (const x of ...)` bind per iteration; a `var`
 * (or a bare assignment target) writes through to the enclosing scope */
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

/* bind the loop variable (when the loop form has one), run one body statement
 * and fold its Flow into `out`; returns the loop_flow act code
 * (0 = keep going, 1 = break, 2 = propagate) */
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

/* `for (k in str)` enumerates the character indices as strings */
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

/* `for (k in obj)` walks own enumerable keys; the key array must be freed on
 * every exit path, including an early break/return */
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

/* runs one try/catch/finally block under its own landing pad. On a caught throw
 * returns with *pending=1 and the value in *thrown; otherwise writes the block's
 * flow into *flow. Keeping setjmp inside its own frame lets the longjmp land here. */
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

/* establishes the catch scope, binds the thrown value to the pattern and runs
 * the handler body */
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

/* finally overrides the pending flow only when it itself completes abruptly,
 * which also cancels an in-flight throw */
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

/* `switch` uses strict equality to find the entry clause, then falls through
 * the remaining clauses until `break` (unlabelled) or a return/throw */
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

/* `label: stmt`. When the labelled statement is a loop the whole label chain is
 * collected so `continue a` / `break a` from inside target that loop. */
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

/* `return expr`: the promise job queue is drained so `.then` callbacks that
 * observe the returned promise run before the caller resumes */
static Flow exec_return(HpeJs *js, JsEnv *env, JsNode *n)
{
    Flow flow = flow_none();

    flow.action = FLOW_RETURN;
    flow.ret = n->kids[0] ? eval_expr(js, env, n->kids[0]) : js_undef();
    if (flow.ret.t == JS_OBJ && flow.ret.obj && flow.ret.obj->kind == JS_OBJ_PROMISE)
        hpe_js_drain_jobs(js);
    return flow;
}

/* `if (t) a else b`: both branches are ordinary statements */
static Flow exec_if(HpeJs *js, JsEnv *env, JsNode *n)
{
    Flow flow = flow_none();

    if (js_truthy(eval_expr(js, env, n->kids[0])))
        return exec_stmt(js, env, n->kids[1]);
    if (n->kid_count > 2)
        return exec_stmt(js, env, n->kids[2]);
    return flow;
}

/* `throw expr` unwinds to the nearest active try frame */
static Flow exec_throw(HpeJs *js, JsEnv *env, JsNode *n)
{
    js_throw_value(js, eval_expr(js, env, n->kids[0]));
    return flow_none();
}

/* an expression statement: a promise result is drained so its handlers settle */
static Flow exec_expr_stmt(HpeJs *js, JsEnv *env, JsNode *n)
{
    JsValue v = eval_expr(js, env, n->kids[0]);

    if (v.t == JS_OBJ && v.obj && v.obj->kind == JS_OBJ_PROMISE)
        hpe_js_drain_jobs(js);
    return flow_none();
}

static Flow exec_break(HpeJs *js, JsNode *n, int action)
{
    Flow flow = flow_none();

    HPE_UNUSED(js);
    flow.action = action;
    flow.label = n->str;
    return flow;
}

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

static JsModule *module_find(HpeJs *js, const char *path)
{
    JsModule *m;
    for (m = js->modules; m; m = m->next)
        if (!strcmp(m->path, path))
            return m;
    return NULL;
}

/* resolves `spec` and returns the module record. A host loader takes priority;
 * otherwise the module is read from the local filesystem. */
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

/* evaluates the module body (once) and returns its namespace object */
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

static void ns_define(HpeJs *js, JsModule *m, const char *name, JsValue v)
{
    js_obj_set(js, m->ns, name, v);
}

/* publishes a single local declaration's binding into the namespace */
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

/* op==0: `export <declaration>` — each declared name is copied from the env */
static void ns_fill_local(HpeJs *js, JsModule *m, JsEnv *env, JsNode *ex)
{
    JsNode *d = ex->kids[0];
    if (d)
        ns_define_local_decl(js, m, env, d);
}

/* op==1: `export default <expr>` lives in the synthetic "*default*" slot */
static void ns_fill_default(HpeJs *js, JsModule *m, JsEnv *env)
{
    JsValue *slot = js_env_lookup(env, "*default*");
    ns_define(js, m, "default", slot ? *slot : js_undef());
}

/* op==2: `export { a, b as c } [from "src"]` — pairs may alias and re-source */
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

/* op==3: `export * from "src"` — re-export every key except `default` */
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

/* dispatches each `export` node to its kind-specific filler */
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

/* ---------------- builtin helpers ---------------- */

static JsValue mk_native(HpeJs *js, JsNativeFn fn, void *ud)
{
    return js_objval(js_native_new(js, fn, ud));
}

static JsValue arg_at(JsValue *args, int argc, int i)
{
    return i < argc ? args[i] : js_undef();
}

static void def_method(HpeJs *js, JsObj *target, const char *name, JsNativeFn fn, void *ud)
{
    js_obj_set(js, target, name, mk_native(js, fn, ud));
}

static void def_value(HpeJs *js, JsObj *target, const char *name, JsValue v)
{
    js_obj_set(js, target, name, v);
}

/* reads a string argument or returns a fallback */
static const char *arg_cstr(HpeJs *js, JsValue *args, int argc, int i, const char *fallback)
{
    if (i >= argc)
        return fallback;
    return js_to_string(js, args[i]);
}

static JsValue list_of(HpeJs *js, JsValue *v, int n)
{
    return array_of(js, v, n);
}

/* the receiver of an Array.prototype method (must be array-like) */
static JsObj *this_like(HpeJs *js, JsValue thisv)
{
    if (thisv.t == JS_OBJ && thisv.obj)
        return thisv.obj;
    if (thisv.t == JS_STR)
        return NULL;
    HPE_UNUSED(js);
    return NULL;
}

/* ---------------- console / alert ---------------- */

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

static JsValue math_round(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    double x = js_to_number(js, arg_at(args, argc, 0));
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    /* JS rounds .5 towards +infinity */
    return js_num(x != x || isinf(x) ? x : floor(x + 0.5));
}

static JsValue math_pow(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    return js_num(pow(js_to_number(js, arg_at(args, argc, 0)),
                      js_to_number(js, arg_at(args, argc, 1))));
}

static JsValue math_atan2(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    return js_num(atan2(js_to_number(js, arg_at(args, argc, 0)),
                        js_to_number(js, arg_at(args, argc, 1))));
}

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

static JsValue math_imul(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    int32_t x = (int32_t)(uint32_t)js_to_number(js, arg_at(a, argc, 0));
    int32_t y = (int32_t)(uint32_t)js_to_number(js, arg_at(a, argc, 1));
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    return js_num((double)(int32_t)((uint32_t)x * (uint32_t)y));
}

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

/* ---------------- parseInt / parseFloat / isNaN / isFinite ---------------- */

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

static JsValue native_is_nan(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    double d;
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    d = js_to_number(js, arg_at(args, argc, 0));
    return js_bool(d != d);
}

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
/* ---------------- String.prototype ---------------- */

static JsObj *str_this(HpeJs *js, JsValue thisv)
{
    HPE_UNUSED(js);
    if (thisv.t == JS_OBJ && thisv.obj && thisv.obj->kind == JS_OBJ_PLAIN && thisv.obj->items)
        return thisv.obj;
    HPE_UNUSED(thisv);
    return NULL;
}

static const char *str_arg(HpeJs *js, JsValue a)
{
    return js_to_string(js, a);
}

static JsValue strv(HpeJs *js, const char *s)
{
    return js_string(js, s ? s : "");
}

static JsValue string_charAt(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    char *s = js_to_string(js, thisv);
    int i = (int)js_to_number(js, arg_at(a, argc, 0));
    HPE_UNUSED(ud);
    if (i < 0 || i >= utf8_len(s))
        return strv(js, "");
    return utf8_char_at(js, s, i);
}

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

/* byte offset of utf-16/codepoint index `i` */
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

/* trims one end of the string: `ud` selects end (non-NULL) vs start */
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
/* ensure `out` can hold `need` more bytes from `o`, doubling as needed */
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

/* `rep` as a string, calling a replacement callback with (match, offset, input) */
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

/* empty search string: insert the replacement between every character */
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

/* replaces every occurrence of a non-empty substring */
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

/* replaces every occurrence of a substring (or of a global regex) */
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

/* index of the codepoint that starts at byte offset i (UTF-8 aware) */
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

/* case-insensitive-ish comparison: locale collation is unavailable, so this is
 * a plain code-unit comparison returning -1/0/1 */
static JsValue string_localeCompare(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    char *s = js_to_string(js, thisv);
    char *o = js_to_string(js, arg_at(a, argc, 0));
    int c = strcmp(s, o);
    HPE_UNUSED(ud);
    return js_num(c < 0 ? -1.0 : c > 0 ? 1.0 : 0.0);
}

/* all matches of a global regex, as an array of match arrays */
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

/* ---------------- Array.prototype ---------------- */

static JsObj *arr_of(JsValue thisv)
{
    return (thisv.t == JS_OBJ && thisv.obj) ? thisv.obj : NULL;
}

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

static JsValue array_join(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *src = arr_of(thisv);
    char *sep = argc > 0 && a[0].t != JS_UNDEF ? js_to_string(js, a[0]) : ",";
    HPE_UNUSED(ud);
    if (!src)
        return js_string(js, "");
    return array_join_impl(js, src, sep ? sep : ",");
}

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

/* searches from the end; `fromIndex` counts back from the end when negative */
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

static JsValue array_forEach(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *src = arr_of(thisv);
    HPE_UNUSED(ud);
    if (!src || !js_is_callable(arg_at(a, argc, 0)))
        return js_undef();
    return arr_iterate(js, src, a[0], arg_at(a, argc, 1), ARR_EACH);
}

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

static JsValue array_some(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *src = arr_of(thisv);
    HPE_UNUSED(ud);
    if (!src || !js_is_callable(arg_at(a, argc, 0)))
        return js_bool(0);
    return arr_iterate(js, src, a[0], arg_at(a, argc, 1), ARR_SOME);
}

static JsValue array_every(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *src = arr_of(thisv);
    HPE_UNUSED(ud);
    if (!src || !js_is_callable(arg_at(a, argc, 0)))
        return js_bool(1);
    return arr_iterate(js, src, a[0], arg_at(a, argc, 1), ARR_EVERY);
}

static JsValue array_find(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *src = arr_of(thisv);
    HPE_UNUSED(ud);
    if (!src || !js_is_callable(arg_at(a, argc, 0)))
        return js_undef();
    return arr_iterate(js, src, a[0], arg_at(a, argc, 1), ARR_FIND);
}

static JsValue array_findIndex(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *src = arr_of(thisv);
    HPE_UNUSED(ud);
    if (!src || !js_is_callable(arg_at(a, argc, 0)))
        return js_num(-1);
    return arr_iterate(js, src, a[0], arg_at(a, argc, 1), ARR_FINDIDX);
}

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

/* searches from the end; `ud` selects findLast (non-NULL) vs findLastIndex */
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

/* copies [start,end) to `target`, handling overlapping ranges correctly */
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

/* [index, value] pairs, mirroring Object.entries */
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

/* Array.prototype[Symbol.iterator] hands back the array itself: js_iter_len /
 * js_iter_at already walk array elements, so for-of and spread work directly */
static JsValue array_iter_self(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(js);
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    return thisv;
}

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

static JsValue array_flatMap(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsValue mapped = array_map(js, thisv, a, argc, ud);
    JsObj *out = js_array_new(js);
    if (mapped.t == JS_OBJ && mapped.obj)
        flat_into(js, out, mapped.obj, 1);
    return js_objval(out);
}

static JsValue array_toString(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    return array_join(js, thisv, a, argc, ud);
}

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

/* Array.from(iterable, mapFn) */
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

static JsValue native_array_of(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    return array_of(js, a, argc);
}

static JsValue native_array_is_array(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsValue v = arg_at(a, argc, 0);
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    return js_bool(v.t == JS_OBJ && v.obj && v.obj->kind == JS_OBJ_ARRAY);
}

void js_array_install_methods(HpeJs *js, JsObj *arr)
{
    HPE_UNUSED(js);
    HPE_UNUSED(arr);
}

/* ---------------- Object ---------------- */
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

static JsValue object_create(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsValue proto = arg_at(a, argc, 0);
    JsObj *o = js_object_raw(js);
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    o->proto = (proto.t == JS_OBJ) ? proto.obj : NULL;
    return js_objval(o);
}

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

static JsValue object_this_valueOf(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(js);
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    return thisv;
}

/* Object.fromEntries([[k,v], ...]) — the inverse of Object.entries */
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

/* own string keys including hidden (non-enumerable) ones, unlike Object.keys */
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

/* the engine does not model property attributes, so seal/freeze only have to
 * round-trip through isSealed/isFrozen to keep feature-detection code happy */
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

static JsValue object_is_sealed(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsValue v = arg_at(a, argc, 0);
    HPE_UNUSED(js);
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    return js_bool(!(v.t == JS_OBJ && v.obj) || (v.obj->num_val != 0.0));
}

static JsValue object_is_frozen(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsValue v = arg_at(a, argc, 0);
    HPE_UNUSED(js);
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    return js_bool(!(v.t == JS_OBJ && v.obj) || (v.obj->num_val >= 3.0));
}

/* ---------------- Boolean / Number ---------------- */

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

/* digits after the point, or scientific notation when `ud` is set (exponential) */
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

/* no Intl database: this is toString with a plain grouping-free numeric form */
static JsValue number_toLocaleString(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    double v = js_to_number(js, thisv);
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    return js_string(js, js_num_to_string(js, v));
}

/* ---- Number statics ---- */

static JsValue number_is_integer(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsValue v = arg_at(a, argc, 0);
    HPE_UNUSED(js);
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    return js_bool(v.t == JS_NUM && v.num == v.num && v.num != INFINITY && v.num != -INFINITY &&
                   floor(v.num) == v.num);
}

static JsValue number_is_finite_fn(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsValue v = arg_at(a, argc, 0);
    HPE_UNUSED(js);
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    return js_bool(v.t == JS_NUM && v.num == v.num && v.num != INFINITY && v.num != -INFINITY);
}

static JsValue number_is_nan_fn(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsValue v = arg_at(a, argc, 0);
    HPE_UNUSED(js);
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    return js_bool(v.t == JS_NUM && v.num != v.num);
}

static JsValue number_is_safe_integer(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsValue v = arg_at(a, argc, 0);
    HPE_UNUSED(js);
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    return js_bool(v.t == JS_NUM && floor(v.num) == v.num && fabs(v.num) <= 9007199254740991.0);
}

static JsValue number_this_toString_radix(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud);

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

static JsValue bool_this_toString(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(js);
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    return js_string(js, js_truthy(thisv) ? "true" : "false");
}

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

static void jsbuf_init(JSBuf *b)
{
    b->cap = 256;
    b->len = 0;
    b->s = (char *)malloc(b->cap);
    if (b->s)
        b->s[0] = '\0';
}

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

static void jsbuf_free(JSBuf *b)
{
    free(b->s);
    b->s = NULL;
}

/* quotes and escapes one string per the JSON grammar */
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

/* a function or native object is not serialisable and becomes null */
static int json_is_function(JsValue v)
{
    return v.t == JS_OBJ && v.obj &&
           (v.obj->kind == JS_OBJ_FUNC || v.obj->kind == JS_OBJ_NATIVE);
}

/* recursive value writer; forward-declared for the container helpers below */
static int json_write(HpeJs *js, JSBuf *b, JsValue v, int depth);

/* arrays serialise as `[e, ...]` with every hole at its index */
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

/* objects serialise as `{"k":v, ...}`, skipping undefined/function values */
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

/* Date instances serialise as their toJSON() (an ISO-8601 string) */
static int json_write_date(HpeJs *js, JSBuf *b, JsValue v, int depth)
{
    JsValue j = member_get(js, NULL, v, "toJSON", 0);

    if (js_is_callable(j))
        return json_write(js, b, call_value(js, NULL, j, v, NULL, 0, 0), depth + 1);
    json_quote(b, js_to_string(js, v));
    return 1;
}

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

static void json_ws(JsonP *j)
{
    while (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')
        j->p++;
}

static JsValue json_parse_value(JsonP *j);

/* doubles the decode scratch buffer, preserving the `n` bytes already written */
static void json_str_grow(JsonP *j, char **buf, size_t *cap, size_t n)
{
    char *nb = (char *)hpe_js_alloc(j->js, *cap * 2);
    memcpy(nb, *buf, n);
    *buf = nb;
    *cap *= 2;
}

/* decodes one \uXXXX sequence to UTF-8 (left unpaired; \uD800 pairs are not
 * combined), advancing j->p past up to four hex digits */
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

/* the single-character escapes; anything else keeps the raw backslash, matching
 * the tolerant parser that never rejects an unknown escape */
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

/* ---------------- RegExp ---------------- */

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

static void regex_set_last_index(HpeJs *js, JsValue thisv, int v)
{
    if (thisv.t == JS_OBJ && thisv.obj)
        js_obj_set(js, thisv.obj, "lastIndex", js_num((double)v));
}

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

/* regex literal path (/pattern/flags): build the same object as new RegExp */
JsValue js_regex_new(HpeJs *js, const char *pattern, const char *flags)
{
    return regex_obj_new(js, pattern, flags);
}

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

/* expands $$ $& $1..$9 $` $' in a replacement template */
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

/* build the argument list for a replacement callback: the whole match, each
 * capture group (undefined when unmatched), the offset, then the input string */
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

/* append one match's replacement: either a callback result or the expanded
 * `rep_s` template */
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

/* replace with a RegExp needle; walks the input when the regex is global */
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

/* replace with a plain-string needle: only the first occurrence is replaced */
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

/* string.replace(needle, replacement) */
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

/* global regex: collect every non-overlapping match into an array, or null */
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

/* non-global regex: a single match result object (with capture groups), or null */
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

/* plain-string needle: emulate the match array (with index/input) or null */
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

/* string.match(needle): regex -> match array/result; string -> match array */
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

/* ---------------- Error ---------------- */

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

static JsValue all_hook_ok(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    AllCtx *c = (AllCtx *)ud;
    HPE_UNUSED(thisv);
    array_set_index(js, c->results, c->index, arg_at(a, argc, 0));
    if (c->pending && --*c->pending == 0)
        js_settle_promise(js, c->out, 1, js_objval(c->results));
    return js_undef();
}

static JsValue all_hook_err(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    AllCtx *c = (AllCtx *)ud;
    HPE_UNUSED(thisv);
    js_settle_promise(js, c->out, 2, arg_at(a, argc, 0));
    return js_undef();
}

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

static JsValue promise_proto_then(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(ud);
    if (thisv.t != JS_OBJ || !thisv.obj)
        js_error(js, 0, "Promise.prototype.then called on a non-promise");
    return promise_then(js, thisv.obj, arg_at(a, argc, 0), arg_at(a, argc, 1));
}

static JsValue promise_proto_catch(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(ud);
    return promise_then(js, thisv.obj, js_undef(), arg_at(a, argc, 0));
}

static JsValue promise_proto_finally(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(ud);
    return promise_then(js, thisv.obj, arg_at(a, argc, 0), arg_at(a, argc, 0));
}

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

static JsValue promise_static_reject(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *p = js_promise_new(js);
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    js_settle_promise(js, p, 2, arg_at(a, argc, 0));
    return js_objval(p);
}

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

/* ---------------- Function.prototype ---------------- */

static JsValue function_call(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(ud);
    if (!js_is_callable(thisv))
        js_error(js, 0, "Function.prototype.call called on a non-function");
    return call_value(js, NULL, thisv, arg_at(a, argc, 0), argc > 1 ? a + 1 : NULL,
                      argc > 1 ? argc - 1 : 0, 0);
}

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

static JsValue function_this_toString(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(thisv);
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    return js_string(js, "function () { [native code] }");
}

/* new Function("a", "b", "return a + b") — compile the joined source as a function */
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

/* ---------------- global constructors ---------------- */

static JsValue native_string_ctor(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    if (argc == 0)
        return js_string(js, "");
    return js_string(js, js_to_string(js, a[0]));
}

/* String.fromCharCode(...codes): each argument is one UTF-16 code unit; values
 * above BMP are combined from surrogate pairs by the caller, so we just encode
 * the code point each argument names */
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

static JsValue native_number_ctor(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    return js_num(argc == 0 ? 0.0 : js_to_number(js, a[0]));
}

static JsValue native_boolean_ctor(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(js);
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    return js_bool(argc > 0 && js_truthy(a[0]));
}

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

/* ---------------- native iterator plumbing ---------------- */

static JsValue native_iter_symbol(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(thisv);
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    return js_objval(js_object_new(js));
}

/* codepoints in the first `bytes` bytes of `s` */
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

/* split on a RegExp separator, interleaving the capture groups per spec */
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

/* split on a plain separator; an empty separator splits into codepoints */
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

/* ---- Set (backed by a plain object keyed by a string form of the value) ---- */

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

static JsValue set_has(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *o = (thisv.t == JS_OBJ) ? thisv.obj : NULL;
    HPE_UNUSED(ud);
    if (!o || !o->payload_obj)
        return js_bool(0);
    return js_bool(js_obj_own_has(o->payload_obj, set_key(js, arg_at(a, argc, 0))));
}

static JsValue set_delete(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *o = (thisv.t == JS_OBJ) ? thisv.obj : NULL;
    HPE_UNUSED(ud);
    if (!o || !o->payload_obj)
        return js_bool(0);
    return js_bool(js_obj_delete(js, o->payload_obj, set_key(js, arg_at(a, argc, 0))));
}

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

/* ---- Map / Set iteration (yields through js_iter_len/js_iter_at, so
 * for-of, spread, Array.from and destructuring all consume them) ---- */

static int map_kind(JsValue thisv, JsObj **out)
{
    JsObj *o = (thisv.t == JS_OBJ) ? thisv.obj : NULL;
    if (!o || o->kind != JS_OBJ_MAP)
        return 0;
    *out = o;
    return 1;
}

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

/* Set.prototype.entries() yields [v, v] pairs (the spec's shape); keys/values
 * are the plain value iterator already installed as set_values */
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

/* `[Symbol.iterator]` on a Map/Set returns the object itself, whose iterator
 * behaviour is provided by js_iter_len/js_iter_at; the shared `next` walker
 * drives it. values() is the default iterator for both, per spec. */
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

/* strings iterate by codepoint */
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

/* ---------------- prototype wiring + global install ---------------- */

static JsObj *fresh_proto(HpeJs *js, const char *cls, JsObj *object_proto)
{
    JsObj *p = js_object_raw(js);
    p->proto = object_proto;
    p->class_name = cls;
    return p;
}

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

/* Object.prototype */
static void install_object_proto(HpeJs *js)
{
    JsObj *p = js->object_proto;
    def_method(js, p, "toString", object_this_toString, NULL);
    def_method(js, p, "valueOf", object_this_valueOf, NULL);
    def_method(js, p, "hasOwnProperty", object_has_own, NULL);
    def_method(js, p, "propertyIsEnumerable", object_has_own, NULL);
}

/* Function.prototype */
static void install_function_proto(HpeJs *js)
{
    JsObj *p = js->func_proto;
    def_method(js, p, "call", function_call, NULL);
    def_method(js, p, "apply", function_apply, NULL);
    def_method(js, p, "bind", function_bind, NULL);
    def_method(js, p, "toString", function_this_toString, NULL);
}

/* Array.prototype */
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

/* String.prototype */
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

/* Number / Boolean */
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

static void install_bool_proto(HpeJs *js)
{
    JsObj *p = js->bool_proto;
    def_method(js, p, "toString", bool_this_toString, NULL);
    def_method(js, p, "valueOf", bool_this_valueOf, NULL);
}

/* RegExp.prototype / Error family / Promise / Map */
static void install_regex_proto(HpeJs *js)
{
    JsObj *p = js->regex_proto;
    def_method(js, p, "test", regex_test, NULL);
    def_method(js, p, "exec", regex_exec, NULL);
    def_method(js, p, "toString", regex_toString, NULL);
}

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

static void install_promise_proto(HpeJs *js)
{
    JsObj *p = js->promise_proto;
    def_method(js, p, "then", promise_proto_then, NULL);
    def_method(js, p, "catch", promise_proto_catch, NULL);
    def_method(js, p, "finally", promise_proto_finally, NULL);
}

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

/* install statics on the constructor object looked up by name in `global` */
static JsObj *ctor_of(HpeJs *js, const char *name)
{
    return js_obj_get(js, js->global_obj, name).obj;
}

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

static void install_array_ctor(HpeJs *js)
{
    JsObj *ac;

    make_ctor(js, "Array", native_array_ctor, NULL, js->array_proto);
    ac = ctor_of(js, "Array");
    def_method(js, ac, "from", native_array_from, NULL);
    def_method(js, ac, "of", native_array_of, NULL);
    def_method(js, ac, "isArray", native_array_is_array, NULL);
}

static void install_string_ctor(HpeJs *js)
{
    JsObj *sc;

    make_ctor(js, "String", native_string_ctor, NULL, js->string_proto);
    sc = ctor_of(js, "String");
    def_method(js, sc, "fromCharCode", string_from_char_code, NULL);
    def_method(js, sc, "fromCodePoint", string_from_code_point, NULL);
}

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

static void install_error_ctors(HpeJs *js)
{
    make_ctor(js, "Error", error_ctor, (void *)"Error", js->error_proto);
    make_ctor(js, "TypeError", error_ctor, (void *)"TypeError", js->type_error_proto);
    make_ctor(js, "RangeError", error_ctor, (void *)"RangeError", js->range_error_proto);
    make_ctor(js, "SyntaxError", error_ctor, (void *)"SyntaxError", js->syntax_error_proto);
    make_ctor(js, "ReferenceError", error_ctor, (void *)"ReferenceError", js->ref_error_proto);
}

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

static void install_map_set_ctors(HpeJs *js)
{
    make_ctor(js, "Map", native_map_ctor, NULL, js->map_proto);
    make_ctor(js, "Set", native_set_ctor, NULL, js->set_proto);
    install_set_statics(js);
}

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

static void install_math_consts(HpeJs *js, JsObj *math)
{
    def_value(js, math, "PI", js_num(3.141592653589793));
    def_value(js, math, "E", js_num(2.718281828459045));
    def_value(js, math, "LN2", js_num(0.6931471805599453));
    def_value(js, math, "LN10", js_num(2.302585092994046));
    def_value(js, math, "SQRT2", js_num(1.4142135623730951));
    def_value(js, math, "SQRT1_2", js_num(0.7071067811865476));
}

static void install_math(HpeJs *js)
{
    JsObj *math = js_object_new(js);

    math->class_name = "Math";
    install_math_fn_table(js, math);
    install_math_consts(js, math);
    js_obj_set(js, js->global_obj, "Math", js_objval(math));
}

static void install_json(HpeJs *js)
{
    JsObj *json = js_object_new(js);

    json->class_name = "JSON";
    def_method(js, json, "stringify", json_stringify, NULL);
    def_method(js, json, "parse", json_parse, NULL);
    js_obj_set(js, js->global_obj, "JSON", js_objval(json));
}

/* `parseInt` / `isNaN` / URI codecs shared by the global object and `Number` */
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

/* `console` methods all route to native_console; `ud` selects the stderr stream */
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

/* ---------------- public api ---------------- */

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

void hpe_js_set_global_host(HpeJs *js, const char *name, void *ptr, int type,
                            const HpeJsHostVt *vt)
{
    js_obj_set(js, js->global_obj, name, js_make_host(js, ptr, type, vt));
}

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

int hpe_js_eval(HpeJs *js, const char *source, char *errbuf, size_t errbuf_size)
{
    return eval_common(js, source, 0, errbuf, errbuf_size);
}

/* runs a module and returns 0/-1 (the namespace is reachable through imports) */
int hpe_js_eval_module(HpeJs *js, const char *source, const char *dir, char *errbuf,
                       size_t errbuf_size)
{
    if (dir && *dir)
        hpe_js_set_current_dir(js, dir);
    return eval_common(js, source, 1, errbuf, errbuf_size);
}

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

JsValue hpe_js_call_value(HpeJs *js, JsValue callee, JsValue thisv, JsValue *args, int argc)
{
    if (!js || !js_is_callable(callee))
        return js_undef();
    return call_value(js, NULL, callee, thisv, args, argc, 0);
}

void hpe_js_set_module_loader(HpeJs *js, const HpeJsModuleLoader *loader)
{
    if (!js)
        return;
    if (loader)
        js->loader = *loader;
    else
        memset(&js->loader, 0, sizeof(js->loader));
}

void hpe_js_set_current_dir(HpeJs *js, const char *dir)
{
    if (js)
        js->current_dir = dir;
}

JsObj *hpe_js_global_object(HpeJs *js)
{
    return js ? js->global_obj : NULL;
}


