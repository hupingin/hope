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

/**
 * @brief 创建并初始化一个 JS 解释器实例 / Create and initialise a JS interpreter instance
 *
 * @return 新的解释器实例；失败时返回 `NULL` / The new interpreter instance, or `NULL` on failure
 *
 * @note 实例内部自带 arena、全局对象与各内建原型，随后由 `hpe_js_destroy` 整体释放 / The instance owns its arena, global object and builtin prototypes, all released as a whole by `hpe_js_destroy`
 *
 * @see hpe_js_destroy
 */
HpeJs *hpe_js_create(void);

/**
 * @brief 销毁解释器并释放其全部内存 / Destroy an interpreter and release all its memory
 *
 * @param[in,out] js 要销毁的解释器实例 / The interpreter instance to destroy
 *
 * @note arena 整体释放，实例内一切对象随之失效 / The arena is freed wholesale, invalidating every object it owned
 *
 * @see hpe_js_create
 */
void hpe_js_destroy(HpeJs *js);

/**
 * @brief 把一个宿主对象暴露为全局变量 / Expose a host object as a global
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     name 全局变量名（如 `document`）/ The global name, such as `document`
 * @param[in,out] ptr 宿主对象指针 / The host object pointer
 * @param[in]     type 宿主类型标记（`HPE_HOST_*`）/ Host type tag
 * @param[in]     vt 可选的属性拦截虚表 / Optional property-access vtable
 *
 * @note 典型用法是把 `document` 挂到全局，使脚本可直接访问 / The typical use is exposing `document` so scripts can reach it directly
 *
 * @see hpe_js_dom_install
 */
void hpe_js_set_global_host(HpeJs *js, const char *name, void *ptr, int type,
                            const HpeJsHostVt *vt);

/**
 * @brief 执行一段脚本 / Run a script
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     source 脚本源码 / Script source text
 * @param[out]    errbuf 接收错误信息，可为 `NULL` / Receives the error message, may be `NULL`
 * @param[in]     errbuf_size `errbuf` 的容量（字节）/ Capacity of `errbuf` in bytes
 *
 * @return 成功返回 0，出错返回非 0 / 0 on success, non-zero on error
 *
 * @note 出错时把信息写入 `errbuf` / The error text is written to `errbuf` on failure
 */
int hpe_js_eval(HpeJs *js, const char *source, char *errbuf, size_t errbuf_size);

/**
 * @brief 按 ES 模块执行一段脚本 / Run a script as an ES module
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     source 模块源码 / Module source text
 * @param[in]     dir 解析相对 `import` 说明符的基准目录 / Base directory used to resolve relative `import` specifiers
 * @param[out]    errbuf 接收错误信息，可为 `NULL` / Receives the error message, may be `NULL`
 * @param[in]     errbuf_size `errbuf` 的容量（字节）/ Capacity of `errbuf` in bytes
 *
 * @return 成功返回 0，出错返回非 0 / 0 on success, non-zero on error
 *
 * @note 允许 `import`/`export` 语法；模块解析器由 `hpe_js_set_module_loader` 注册 / `import` and `export` syntax are allowed; the resolver is registered via `hpe_js_set_module_loader`
 *
 * @see hpe_js_set_module_loader hpe_js_set_current_dir
 */
int hpe_js_eval_module(HpeJs *js, const char *source, const char *dir, char *errbuf,
                       size_t errbuf_size);

/**
 * @brief 从 C 侧调用一个可调用的值 / Call a callable value from C
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     callee 被调用的值（如事件处理函数）/ The value to call, such as an event handler
 * @param[in]     thisv 调用时的 `this` 值 / The `this` value of the call
 * @param[in,out] args 实参数组，`argc` 为 0 时可为 `NULL` / Argument array, may be `NULL` when `argc` is 0
 * @param[in]     argc 参数个数 / Argument count
 *
 * @return 成功返回 0，运行时错误返回 -1 / 0 on success, -1 on a runtime error
 *
 * @note 运行时错误会报告到 stderr / Runtime errors are reported to stderr
 */
int hpe_js_call(HpeJs *js, JsValue callee, JsValue thisv, JsValue *args, int argc);

/**
 * @brief 执行到期定时器与一轮 rAF 回调 / Run due timers plus one round of rAF callbacks
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     now 当前时间（秒），与定时器到期时刻同基准 / Current time in seconds, on the same basis as the deadlines
 * @param[out]    fired 实际上执行的回调个数，可为 `NULL` / The number of callbacks that actually ran; may be `NULL`
 *
 * @return 实际执行的回调个数 / The number of callbacks that actually ran
 *
 * @note 宿主可据 `*fired` 决定是否重排重绘 / The embedder can decide whether to relayout and repaint from `*fired`
 */
int hpe_js_run_timers(HpeJs *js, double now, int *fired);

/**
 * @brief 注册 `import` 说明符的解析器 / Register the resolver for `import` specifiers
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     loader 模块加载器回调结构 / The module-loader callback structure
 */
void hpe_js_set_module_loader(HpeJs *js, const HpeJsModuleLoader *loader);

/**
 * @brief 设置相对说明符的基准目录 / Set the base directory for relative specifiers
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     dir 基准目录 / The base directory
 *
 * @note 相对 `import` 说明符以此目录为起点解析 / Relative `import` specifiers are resolved from this directory
 */
void hpe_js_set_current_dir(HpeJs *js, const char *dir);

#endif /* HPE_JS_H */
