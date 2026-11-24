/* Hope UI Engine - javascript interpreter internals (shared by core + dom bridge)
 *
 * The engine keeps the public surface in hpe_js.h and puts everything the
 * core interpreter, the timer queue and the DOM bridge need to share here.
 */
#ifndef HPE_JS_INTERNAL_H
#define HPE_JS_INTERNAL_H

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>

#include "base/hpe_util.h"
#include "js/hpe_js.h"

typedef enum {
    JS_OBJ_PLAIN,
    JS_OBJ_ARRAY,
    JS_OBJ_FUNC,
    JS_OBJ_NATIVE,
    JS_OBJ_HOST,
    JS_OBJ_PROMISE,
    JS_OBJ_REGEX,
    JS_OBJ_MODULE,
    JS_OBJ_ERROR,
    JS_OBJ_MAP,
    JS_OBJ_ARGUMENTS,
    JS_OBJ_DATE
} JsObjKind;

/* function flags: mirrored in JsObj.fn_flags and, for AST function nodes, in
 * JsNode.op (the parser and interpreter share the bit layout) */
enum {
    FN_ARROW = 1,
    FN_ASYNC = 2,
    FN_GEN = 4,
    FN_STRICT = 8,
    FN_CTOR = 16,
    FN_BOUND = 32,
    FN_CLASS = 64
};

/* object-literal member kinds (JsNode.op of an object pair) */
enum {
    JS_PROP_DATA = 0,
    JS_PROP_GET,
    JS_PROP_SET,
    JS_PROP_METHOD,
    JS_PROP_SHORTHAND,
    JS_PROP_PROTO      /* __proto__: value */
};

/* class member kinds (JsNode.op of a class member) */
enum {
    JS_METHOD = 0,
    JS_METHOD_GET,
    JS_METHOD_SET,
    JS_METHOD_STATIC,
    JS_METHOD_STATIC_GET,
    JS_METHOD_STATIC_SET,
    JS_MEMBER_FIELD,
    JS_MEMBER_STATIC_FIELD,
    JS_METHOD_CTOR,
    JS_STATIC_BLOCK
};

#include "js/hpe_js.h"

typedef struct JsProp {
    char *key;
    JsValue val;
    JsValue getter;
    JsValue setter;
    int enumerable;
} JsProp;

typedef struct JsEnv JsEnv;
typedef struct JsNode JsNode;
typedef struct JsBlock JsBlock;
typedef struct JsReaction JsReaction;
typedef struct JsJob JsJob;
typedef struct JsModule JsModule;
typedef struct HpeJs HpeJs;

struct JsObj {
    JsObjKind kind;
    JsProp *props;
    int prop_count;
    size_t prop_cap;
    struct JsObj *proto;
    const char *class_name;
    /* function */
    struct JsNode *code;
    JsEnv *closure;
    char *name;
    int fn_flags;
    JsValue bound_this;
    JsValue *bound_args;
    int bound_argc;
    struct JsObj *bound_target; /* the function a bound function wraps */
    struct JsObj *home; /* [[HomeObject]]: super lookup base for methods */
    struct JsNode *class_def; /* JS_N_CLASS node this function was created from */
    int length_hint;
    /* JS_OBJ_DATE: milliseconds since the Unix epoch (NaN = invalid date).
     * Named generically because plain objects borrow it for internal scalars. */
    double num_val;
    /* native */
    JsNativeFn native;
    void *native_ud;
    /* host */
    void *host_ptr;
    int host_type;
    const struct HpeJsHostVt *vt;
    /* iteration protocol source (Set / NodeList style objects) */
    struct JsObj *items;
    /* opaque per-kind payload (regex program, map/set backing) */
    void *payload;
    struct JsObj *payload_obj;
    /* promise */
    int promise_state; /* 0 pending, 1 fulfilled, 2 rejected */
    JsValue promise_value;
    JsReaction *reactions;
    JsReaction *reactions_tail;
    /* module namespace */
    struct JsModule *module;
};

struct JsReaction {
    JsValue on_ok;
    JsValue on_err;
    struct JsObj *result;
    struct JsReaction *next;
};

/* microtask kinds */
enum { JOB_CALL = 0, JOB_SETTLE, JOB_REACTION, JOB_THENABLE };

struct JsJob {
    int kind;
    JsValue fn;
    JsValue arg;
    struct JsObj *promise;
    int state;
    JsValue value;
    JsReaction *reaction;
    struct JsJob *next;
};

typedef struct JsTryFrame {
    jmp_buf jmp;
    struct JsTryFrame *prev;
} JsTryFrame;

/* ---------------- heap (arena, freed wholesale on destroy/reset) ---------------- */

void *hpe_js_alloc(HpeJs *js, size_t size);
char *hpe_js_strndup(HpeJs *js, const char *s, size_t n);
char *hpe_js_strdup(HpeJs *js, const char *s);

/* ---------------- values ---------------- */

JsValue js_undef(void);
JsValue js_null(void);
JsValue js_bool(int b);
JsValue js_num(double v);
JsValue js_string(HpeJs *js, const char *s);
JsValue js_string_len(HpeJs *js, const char *s, size_t n);
JsValue js_objval(JsObj *o);

JsObj *js_object_new(HpeJs *js);            /* plain object (proto = Object.prototype) */
JsObj *js_object_raw(HpeJs *js);            /* plain object with no prototype */
JsObj *js_array_new(HpeJs *js);             /* array with element accessors */
JsObj *js_func_new(HpeJs *js, struct JsNode *code, JsEnv *closure);
JsObj *js_native_new(HpeJs *js, JsNativeFn fn, void *ud);
JsObj *js_host_new(HpeJs *js, void *ptr, int type, const HpeJsHostVt *vt);
JsObj *js_promise_new(HpeJs *js);
JsObj *js_module_ns_new(HpeJs *js, JsModule *mod);

/* attaches the array prototype methods (called from js_array_new) */
void js_array_install_methods(HpeJs *js, JsObj *arr);

JsProp *js_obj_find_prop(JsObj *obj, const char *key); /* walks the prototype chain */
JsValue js_obj_get(HpeJs *js, JsObj *obj, const char *key);
void js_obj_set(HpeJs *js, JsObj *obj, const char *key, JsValue v);
void js_obj_set_accessor(HpeJs *js, JsObj *obj, const char *key, JsValue getter, JsValue setter);
int js_obj_has(JsObj *obj, const char *key);
int js_obj_own_has(JsObj *obj, const char *key);
int js_obj_delete(HpeJs *js, JsObj *obj, const char *key);
/* own enumerable string keys, in insertion order */
int js_obj_own_keys(JsObj *obj, char ***out, int *count);
int js_obj_prop_count(JsObj *obj);

/* conversions (return heap strings) */
char *js_num_to_string(HpeJs *js, double v);
char *js_to_string(HpeJs *js, JsValue v);
/* primitive value of an object (valueOf/toString), used by loose equality */
JsValue js_primitive_of(HpeJs *js, JsValue v);
double js_to_number(HpeJs *js, JsValue v);
int js_truthy(JsValue v);
int js_values_equal(HpeJs *js, JsValue a, JsValue b, int strict);
int js_is_callable(JsValue v);
int js_is_thenable(HpeJs *js, JsValue v);
/* own property record lookup (no prototype walk) */
struct JsProp *js_obj_own_prop(JsObj *obj, const char *key);
/* [[Get]]/[[Set]] honouring accessors and the prototype chain; used by the
 * interpreter for member expressions and by the DOM bridge */
JsValue js_get_prop(HpeJs *js, JsEnv *env, JsValue base, const char *prop, int line);
void js_set_prop(HpeJs *js, JsEnv *env, JsValue base, const char *prop, JsValue value, int line);

/* ---------------- errors / exceptions ---------------- */

void js_error(HpeJs *js, int line, const char *fmt, ...); /* throws an Error */
void js_throw_value(HpeJs *js, JsValue v);
JsValue js_make_error(HpeJs *js, const char *name, const char *msg);

/* invokes a callable value from C (exceptions propagate via longjmp) */
JsValue js_invoke(HpeJs *js, JsValue fn, JsValue thisv, JsValue *args, int argc, int line);
/* constructs `fn` with `new`: creates the instance, runs the body, keeps the
 * returned object when the body returns one */
JsValue js_construct(HpeJs *js, JsValue fn, JsValue *args, int argc, int line);

/* ---------------- environments ---------------- */

typedef struct JsVar {
    char *name;
    JsValue val;
} JsVar;

struct JsEnv {
    struct JsEnv *parent;
    JsVar *vars;
    int var_count;
    size_t var_cap;
    int has_this;  /* this env owns a `this` binding (non-arrow function body) */
    JsValue thisv;
    int has_home;  /* this env owns a [[HomeObject]] (method body) */
    struct JsObj *home;
    int strict;
};

JsEnv *js_env_new(HpeJs *js, JsEnv *parent);
JsValue *js_env_lookup(JsEnv *env, const char *name);
void js_env_define(HpeJs *js, JsEnv *env, const char *name, JsValue v);
/* defines a fresh binding in exactly this scope (let/const/class) */
void js_env_declare(HpeJs *js, JsEnv *env, const char *name, JsValue v);
JsEnv *js_env_this_env(JsEnv *env);
/* [[Get]]/[[Set]] honouring accessors and the prototype chain */
JsValue js_get_prop(HpeJs *js, JsEnv *env, JsValue base, const char *prop, int line);
void js_set_prop(HpeJs *js, JsEnv *env, JsValue base, const char *prop, JsValue value, int line);

/* ---------------- ast ---------------- */

typedef struct JsNode {
    int kind;
    int op;
    double num;
    char *str;
    struct JsNode **kids;
    int kid_count;
    size_t kid_cap;
    int line;
} JsNode;

/* statement / expression kinds */
enum {
    JS_N_BLOCK, JS_N_VAR, JS_N_FUNC, JS_N_IF, JS_N_WHILE, JS_N_DO, JS_N_FOR,
    JS_N_RETURN, JS_N_BREAK, JS_N_CONTINUE, JS_N_EXPR, JS_N_EMPTY,
    JS_N_FOROF, JS_N_FORIN, JS_N_SWITCH, JS_N_TRY, JS_N_THROW, JS_N_LABEL,
    JS_N_CLASS, JS_N_EXPORT, JS_N_IMPORT,
    /* expressions */
    JS_N_NUM, JS_N_STR, JS_N_IDENT, JS_N_TRUE, JS_N_FALSE, JS_N_NULL,
    JS_N_ARRAY, JS_N_OBJECT, JS_N_BIN, JS_N_UN, JS_N_ASSIGN, JS_N_TERNARY,
    JS_N_CALL, JS_N_MEMBER, JS_N_UPDATE, JS_N_REGEX, JS_N_NEW, JS_N_SPREAD,
    JS_N_THIS, JS_N_SUPER_CALL, JS_N_SUPER_MEMBER, JS_N_YIELD, JS_N_AWAIT,
    JS_N_SEQ, JS_N_FUNC_EXPR, JS_N_ARROW, JS_N_TEMPLATE, JS_N_TAGGED,
    /* patterns (destructuring) */
    JS_N_PAT_ARR, JS_N_PAT_OBJ, JS_N_PAT_DEFAULT, JS_N_PAT_REST,
    JS_N_OPTCHAIN /* placeholder for an optional-chain link */
};

/* binary / unary / assignment operators */
enum {
    JS_OP_ADD, JS_OP_SUB, JS_OP_MUL, JS_OP_DIV, JS_OP_MOD, JS_OP_EXP,
    JS_OP_EQ, JS_OP_NEQ, JS_OP_SEQ, JS_OP_SNEQ,
    JS_OP_LT, JS_OP_GT, JS_OP_LE, JS_OP_GE,
    JS_OP_AND, JS_OP_OR, JS_OP_NULLISH, JS_OP_NOT, JS_OP_NEG, JS_OP_POS, JS_OP_TYPEOF,
    JS_OP_BAND, JS_OP_BOR, JS_OP_BXOR, JS_OP_SHL, JS_OP_SHR, JS_OP_USHR,
    JS_OP_BNOT, JS_OP_VOID, JS_OP_DELETE, JS_OP_IN, JS_OP_INSTANCEOF,
    /* assignment */
    JS_OP_SET, JS_OP_ADDA, JS_OP_SUBA, JS_OP_MULA, JS_OP_DIVA, JS_OP_MODA,
    JS_OP_EXPA, JS_OP_LSHA, JS_OP_RSHA, JS_OP_URSHA,
    JS_OP_BANDA, JS_OP_BORA, JS_OP_BXORA,
    JS_OP_ANDASSIGN, JS_OP_ORASSIGN, JS_OP_NULLASSIGN,
    /* update */
    JS_OP_PREINC, JS_OP_PREDEC, JS_OP_POSTINC, JS_OP_POSTDEC,
    /* member access */
    JS_OP_DOT, JS_OP_INDEX
};

JsNode *js_node_new(HpeJs *js, int kind, int line);
void js_node_add(HpeJs *js, JsNode *parent, JsNode *child);

/* parser (hpe_js_parser.c) */
JsNode *js_parse_program(HpeJs *js, const char *source);
JsNode *js_parse_module(HpeJs *js, const char *source);
JsNode *js_parse_expression(HpeJs *js, const char *source);

/* ---------------- interpreter state ---------------- */

struct JsModule {
    char *path;
    char *src;      /* pending source (cleared after parsing) */
    JsObj *ns;      /* namespace object holding the exports */
    JsEnv *env;
    int evaluated;
    int is_script;
    struct JsModule *next;
};

/* HpeJsModuleLoader is declared in the public header (hpe_js.h) */

struct HpeJs {
    JsEnv *global_env;
    JsObj *global_obj;
    /* bump arena */
    JsBlock *blocks;
    /* error handling */
    jmp_buf jmp;
    int error;
    char errbuf[256];
    /* exception machinery */
    JsValue thrown;
    JsTryFrame *try_top;
    int in_catch;
    /* prototypes */
    JsObj *object_proto;
    JsObj *array_proto;
    JsObj *func_proto;
    JsObj *string_proto;
    JsObj *number_proto;
    JsObj *bool_proto;
    JsObj *promise_proto;
    JsObj *regex_proto;
    JsObj *error_proto;
    JsObj *type_error_proto;
    JsObj *range_error_proto;
    JsObj *syntax_error_proto;
    JsObj *ref_error_proto;
    JsObj *set_proto;
    JsObj *map_proto;
    JsObj *symbol_proto;
    JsObj *date_proto;
    /* generator support: collect `yield` values eagerly (see design doc §12.7) */
    JsObj *yield_sink;
    /* microtask queue (Promise jobs and then() reactions) */
    JsJob *jobs_head;
    JsJob *jobs_tail;
    int jobs_running;
    /* timer queues (setTimeout / setInterval / requestAnimationFrame) */
    struct JsTimer *timers;
    int timer_count;
    size_t timer_cap;
    int next_timer_id;
    /* modules */
    JsModule *modules;
    JsModule *current_module; /* module being evaluated (import/export target) */
    HpeJsModuleLoader loader;
    const char *current_dir; /* directory of the module currently being evaluated */
    int strict;              /* global "use strict" */
    int array_methods_done;  /* Array.prototype methods installed once */
    void *mutation_state;    /* HpeJsMutationState (see hpe_js_mutation.h) */
};

struct JsBlock {
    JsBlock *next;
    size_t used;
    size_t size;
    char *mem;
};

void js_error(HpeJs *js, int line, const char *fmt, ...);

/* native helpers for bindings */
JsValue js_make_host(HpeJs *js, void *ptr, int type, const HpeJsHostVt *vt);
void *js_host_ptr(JsValue v, int type); /* NULL if not a host of that type */

/* timer queues (hpe_js_timers.c) */
void hpe_js_timers_install(HpeJs *js);
/* drains due timers and the rAF queue; *fired (may be NULL) is 1 when any
 * callback actually ran and the page should be re-laid-out/repainted */
int hpe_js_run_timers(HpeJs *js, double now, int *fired);

/* microtasks (hpe_js_interp.c): runs queued Promise jobs to completion */
void hpe_js_drain_jobs(HpeJs *js);
void js_job_push(HpeJs *js, JsJob *job);
/* queues `fn(arg)` as a microtask (a JOB_CALL job). used by MutationObserver to
 * deliver records asynchronously, like the spec's observer microtask. */
void hpe_js_queue_microtask(HpeJs *js, JsValue fn, JsValue arg);
void js_settle_promise(HpeJs *js, JsObj *p, int state, JsValue value);
void hpe_js_set_module_loader(HpeJs *js, const HpeJsModuleLoader *loader);
void hpe_js_set_current_dir(HpeJs *js, const char *dir);

/* Date built-in (hpe_js_date.c): creates Date.prototype and installs the global */
void hpe_js_date_install(HpeJs *js);

/* regex (hpe_js_regex.c): compiled once per literal, matched on demand */typedef struct HpeRegex HpeRegex;
HpeRegex *hpe_regex_compile(HpeJs *js, const char *pattern);
/* matches `subject` starting at `start`; on success fills the group spans and
 * returns the end offset of the match, otherwise -1 */
int hpe_regex_search(HpeRegex *re, const char *subject, int start, int *captures, int ncaps);
int hpe_regex_group_count(HpeRegex *re);
/* applies /i /m /s flags parsed from a regex literal */
void hpe_regex_set_flags(HpeRegex *re, const char *flags);
JsValue js_regex_new(HpeJs *js, const char *pattern, const char *flags);

/* iterator helpers (hpe_js_interp.c) — used by spread / for-of / Array.from */
int js_iter_len(HpeJs *js, JsValue v);
JsValue js_iter_at(HpeJs *js, JsValue v, int i);

#endif /* HPE_JS_INTERNAL_H */
