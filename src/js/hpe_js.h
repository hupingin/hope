/* Hope UI Engine - minimal javascript interpreter (public api)
 *
 * Pure C, no external dependencies. Supports a practical subset:
 * var/let/const, functions & closures, objects, arrays, control flow,
 * operators, and host objects (the DOM bridge lives in hpe_js_dom.c).
 */
#ifndef HPE_JS_H
#define HPE_JS_H

#include <stddef.h>

typedef enum JsType {
    JS_UNDEF = 0,
    JS_NULL,
    JS_BOOL,
    JS_NUM,
    JS_STR,
    JS_OBJ
} JsType;

typedef struct JsObj JsObj;

typedef struct JsValue {
    JsType t;
    double num;
    char *str; /* JS_STR: heap-owned immutable string */
    JsObj *obj;
} JsValue;

typedef struct HpeJs HpeJs;

/* native function signature (used by the core builtins and by host bindings) */
typedef JsValue (*JsNativeFn)(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud);

/* host object vtable: lets the DOM bridge intercept property access */
typedef struct HpeJsHostVt {
    /* return 1 if the property was handled */
    int (*get)(HpeJs *js, void *target, const char *prop, JsValue *out);
    int (*set)(HpeJs *js, void *target, const char *prop, JsValue value);
} HpeJsHostVt;

/* module resolution callback: resolves+reads `spec` relative to `from_dir`;
 * returns 1 and fills *out_path / *out_src (both malloc'd, owned by the engine) */
typedef struct HpeJsModuleLoader {
    int (*load)(void *ud, const char *spec, const char *from_dir, char **out_path,
                char **out_src);
    void *ud;
} HpeJsModuleLoader;

HpeJs *hpe_js_create(void);
void hpe_js_destroy(HpeJs *js);
/* exposes a host object as a global (e.g. "document") */
void hpe_js_set_global_host(HpeJs *js, const char *name, void *ptr, int type,
                            const HpeJsHostVt *vt);
/* runs a script; returns 0 on success, fills errbuf on error */
int hpe_js_eval(HpeJs *js, const char *source, char *errbuf, size_t errbuf_size);
/* runs a script as an ES module (import/export allowed). `dir` is the base
 * directory used to resolve relative import specifiers. returns 0 on success. */
int hpe_js_eval_module(HpeJs *js, const char *source, const char *dir, char *errbuf,
                       size_t errbuf_size);
/* calls a value as a function from C (e.g. an event handler). returns 0 on success,
 * -1 on runtime error (reported to stderr). thisv/args may be NULL when argc == 0. */
int hpe_js_call(HpeJs *js, JsValue callee, JsValue thisv, JsValue *args, int argc);

/* timer pump: runs all due setTimeout/setInterval callbacks plus one round of
 * requestAnimationFrame callbacks. *fired (may be NULL) is set to the number of
 * timers actually executed, so the embedder can decide to relayout/repaint.
 * returns the same count. */
int hpe_js_run_timers(HpeJs *js, double now, int *fired);

/* module support: registers a resolver used for `import` specifiers, and sets
 * the base directory relative specifiers are resolved against. */
void hpe_js_set_module_loader(HpeJs *js, const HpeJsModuleLoader *loader);
void hpe_js_set_current_dir(HpeJs *js, const char *dir);

#endif /* HPE_JS_H */
