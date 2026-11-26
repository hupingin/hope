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
    JS_OBJ_DATE,
    JS_OBJ_PROXY,
    JS_OBJ_WEAKMAP,
    JS_OBJ_ITER,
    JS_OBJ_GEN
} JsObjKind;

/* typed-array element kinds. The order is the installation order and indexes
 * HpeJs.ta_proto, so it must stay in sync with the name table in
 * hpe_js_buffer.c. */
typedef enum {
    TA_INT8 = 0,
    TA_UINT8,
    TA_UINT8C,
    TA_INT16,
    TA_UINT16,
    TA_INT32,
    TA_UINT32,
    TA_FLOAT32,
    TA_FLOAT64,
    TA_BIGINT64,
    TA_BIGUINT64,
    k_ta_count
} TaKind;

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
    /* JS_OBJ_PROXY: the wrapped target lives in `payload_obj` and the trap table
     * (a plain object built from the handler argument) in `payload`. The target
     * keeps the proxy's prototype so `instanceof` and `Object.getPrototypeOf`
     * behave as if the proxy were the target itself. */
    struct JsObj *proxy_target;
    struct JsObj *proxy_handler;
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

/**
 * @brief 从 arena 分配 8 字节对齐的已清零内存 / Allocate 8-byte-aligned zeroed memory from the arena
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     size 请求字节数，向上对齐到 8 的倍数 / Requested byte count, rounded up to a multiple of 8
 *
 * @return 指向已清零内存的指针 / Pointer to the zeroed memory
 *
 * @note 分配的内存不能单独释放，随解释器销毁整体回收 / Allocations cannot be freed individually and are reclaimed when the interpreter is destroyed
 */

void *hpe_js_alloc(HpeJs *js, size_t size);

/**
 * @brief 在 arena 中复制指定长度的字符串 / Copy a string of the given length into the arena
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     s 源字符串，可为 `NULL` / Source string, may be `NULL`
 * @param[in]     n 复制的字节数 / Number of bytes to copy
 *
 * @return 以 NUL 结尾的新字符串 / The new NUL-terminated string
 */
char *hpe_js_strndup(HpeJs *js, const char *s, size_t n);

/**
 * @brief 在 arena 中复制以 NUL 结尾的字符串 / Duplicate a NUL-terminated string into the arena
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     s 源字符串，可为 `NULL` / Source string, may be `NULL`
 *
 * @return 复制的以 NUL 结尾的字符串 / The duplicated NUL-terminated string
 */
char *hpe_js_strdup(HpeJs *js, const char *s);

/**
 * @brief 构造 `undefined` 值 / Build an `undefined` value
 *
 * @return 类型为 `JS_UNDEF` 的值 / A value of type `JS_UNDEF`
 */

JsValue js_undef(void);

/**
 * @brief 构造 `null` 值 / Build a `null` value
 *
 * @return 类型为 `JS_NULL` 的值 / A value of type `JS_NULL`
 */
JsValue js_null(void);

/**
 * @brief 由 C 布尔值构造 JS 布尔值 / Build a JS boolean from a C boolean
 *
 * @param[in]     b 非零为 `true` / Non-zero means `true`
 *
 * @return 类型为 `JS_BOOL` 的值 / A value of type `JS_BOOL`
 */
JsValue js_bool(int b);

/**
 * @brief 构造 JS 数值 / Build a JS number
 *
 * @param[in]     v 数值（可为 `NaN`/`Infinity`）/ Numeric value, may be `NaN` or `Infinity`
 *
 * @return 类型为 `JS_NUM` 的值 / A value of type `JS_NUM`
 */
JsValue js_num(double v);

/**
 * @brief 由以 NUL 结尾字符串构造 JS 字符串 / Build a JS string from a NUL-terminated string
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     s 源字符串，可为 `NULL` / Source string, may be `NULL`
 *
 * @return 类型为 `JS_STR` 的值 / A value of type `JS_STR`
 */
JsValue js_string(HpeJs *js, const char *s);

/**
 * @brief 由定长字节构造 JS 字符串 / Build a JS string from a byte range
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     s 源字节，`NULL` 视为空 / Source bytes, `NULL` treated as empty
 * @param[in]     n 字节数 / Byte count
 *
 * @return 类型为 `JS_STR` 的值 / A value of type `JS_STR`
 */
JsValue js_string_len(HpeJs *js, const char *s, size_t n);

/**
 * @brief 把对象指针包装成 JS 值 / Wrap an object pointer as a JS value
 *
 * @param[in,out] o 目标对象 / Target object
 *
 * @return 类型为 `JS_OBJ` 的值 / A value of type `JS_OBJ`
 */
JsValue js_objval(JsObj *o);

/**
 * @brief 创建一个原型为 `Object.prototype` 的普通对象 / Create a plain object whose prototype is `Object.prototype`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @return 新普通对象 / The new plain object
 */
JsObj *js_object_new(HpeJs *js);            /* plain object (proto = Object.prototype) */

/**
 * @brief 分配一个无原型的普通对象 / Allocate a plain object with no prototype
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @return 新对象（类名 `Object`、`proto` 为 `NULL`）/ The new object with class `Object` and a `NULL` prototype
 *
 * @note 需要原型链时改用 `js_object_new` / Use `js_object_new` when a prototype is wanted
 */
JsObj *js_object_raw(HpeJs *js);            /* plain object with no prototype */

/**
 * @brief 创建一个 JS 数组对象 / Create a JS array object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @return 类名为 `Array`、`length` 为 0 的新数组 / The new array with class `Array` and `length` 0
 */
JsObj *js_array_new(HpeJs *js);             /* array with element accessors */

/**
 * @brief 由 AST 函数节点创建用户函数对象 / Create a user function object from an AST function node
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] code 函数声明的 AST 节点 / AST node of the function declaration
 * @param[in,out] closure 创建时捕获的词法环境 / Lexical environment captured at creation
 *
 * @return 新的函数对象 / The new function object
 *
 * @note 非箭头函数还会生成带 `constructor` 的 `.prototype` 对象 / Non-arrow functions also get a `.prototype` object carrying `constructor`
 */
JsObj *js_func_new(HpeJs *js, struct JsNode *code, JsEnv *closure);

/**
 * @brief 创建原生（C 实现）函数对象 / Create a native (C-implemented) function object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     fn 原生函数指针 / Native function pointer
 * @param[in,out] ud 透传给原生函数的用户数据 / User data passed through to the native function
 *
 * @return 类名为 `Function` 的原生函数对象 / The new native function object with class `Function`
 */
JsObj *js_native_new(HpeJs *js, JsNativeFn fn, void *ud);

/**
 * @brief 创建宿主对象，把 C 指针暴露给脚本 / Create a host object exposing a C pointer to scripts
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] ptr 宿主指针 / Host pointer
 * @param[in]     type 宿主类型标记（`HPE_HOST_*`）/ Host type tag
 * @param[in]     vt 可选的属性拦截虚表 / Optional property-access vtable
 *
 * @return 类名为 `Host` 的宿主对象 / The new host object with class `Host`
 *
 * @see js_make_host js_host_ptr
 */
JsObj *js_host_new(HpeJs *js, void *ptr, int type, const HpeJsHostVt *vt);

/**
 * @brief 创建一个处于 pending 状态的 Promise / Create a Promise in the pending state
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @return 类名为 `Promise` 的新对象 / The new object with class `Promise`
 */
JsObj *js_promise_new(HpeJs *js);

/**
 * @brief 创建模块命名空间对象 / Create a module namespace object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] mod 该命名空间所属的模块 / The module this namespace belongs to
 *
 * @return 类名为 `Module`、无原型的对象 / The new object with class `Module` and no prototype
 */
JsObj *js_module_ns_new(HpeJs *js, JsModule *mod);

/**
 * @brief 为数组对象挂上数组原型方法 / Attach the array prototype methods to an array object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] arr 目标数组对象 / Target array object
 *
 * @note 由 `js_array_new` 调用，整进程只安装一次 / Called from `js_array_new` and installed only once per process
 */
void js_array_install_methods(HpeJs *js, JsObj *arr);

/**
 * @brief 沿原型链查找属性记录 / Find a property record along the prototype chain
 *
 * @param[in,out] obj 起始对象 / Starting object
 * @param[in]     key 键名 / Key name
 *
 * @return 命中的属性指针，未找到返回 `NULL` / Pointer to the property, or `NULL` when not found
 */
JsProp *js_obj_find_prop(JsObj *obj, const char *key); /* walks the prototype chain */

/**
 * @brief 读取属性值（含原型链）/ Read a property value, walking the prototype chain
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] obj 目标对象 / Target object
 * @param[in]     key 属性键名 / Property key
 *
 * @return 属性值；未找到或属性为访问器时返回 `undefined` / The value, or `undefined` when absent or when the property is an accessor
 *
 * @note 访问器属性需由 `js_get_prop()` 调用读取 / Accessor properties must be read through `js_get_prop()`
 */
JsValue js_obj_get(HpeJs *js, JsObj *obj, const char *key);

/**
 * @brief 设置对象属性，不存在则创建 / Set an object property, creating it when absent
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] obj 目标对象 / Target object
 * @param[in]     key 属性键名 / Property key
 * @param[in]     v 要写入的值 / Value to store
 *
 * @note 数组的 `length` 与 `Symbol.*` 前缀键会被置为非枚举 / Array `length` and `Symbol.*`-prefixed keys are marked non-enumerable
 */
void js_obj_set(HpeJs *js, JsObj *obj, const char *key, JsValue v);

/**
 * @brief 为属性安装 getter/setter 访问器 / Install getter/setter accessors on a property
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] obj 目标对象 / Target object
 * @param[in]     key 属性键名 / Property key
 * @param[in]     getter 读取器函数值 / Getter function value
 * @param[in]     setter 写入器函数值 / Setter function value
 *
 * @note 传入 `undefined` 的一侧保持不变 / A side passed as `undefined` is left unchanged
 */
void js_obj_set_accessor(HpeJs *js, JsObj *obj, const char *key, JsValue getter, JsValue setter);

/**
 * @brief 判断对象（含原型链）是否有某属性 / Test whether an object has a property, prototype chain included
 *
 * @param[in,out] obj JS 对象 / JS object
 * @param[in]     key 键名 / Key name
 *
 * @return 自身或原型链上存在返回 1 / 1 when found on the object or its prototype chain
 */
int js_obj_has(JsObj *obj, const char *key);

/**
 * @brief 判断对象自身是否有某属性 / Test whether an object has an own property
 *
 * @param[in,out] obj JS 对象 / JS object
 * @param[in]     key 键名 / Key name
 *
 * @return 自身存在该属性返回 1 / 1 when the own property exists
 */
int js_obj_own_has(JsObj *obj, const char *key);

/**
 * @brief 删除对象的自身属性 / Delete an own property of an object
 *
 * @param[in,out] js JS 运行时上下文（未使用）/ JS runtime context (unused)
 * @param[in,out] obj 目标对象 / Target object
 * @param[in]     key 属性键名 / Property key
 *
 * @return 总是返回 1 / Always returns 1
 */
int js_obj_delete(HpeJs *js, JsObj *obj, const char *key);

/**
 * @brief 收集对象的可枚举自身键名 / Collect the enumerable own keys of an object
 *
 * @param[in,out] obj JS 对象，可为 `NULL` / JS object, may be `NULL`
 * @param[out]    out 接收以 `NULL` 结尾的键名数组 / Receives a `NULL`-terminated key array
 * @param[out]    count 键名个数 / Receives the number of keys
 *
 * @return 键名个数 / The number of keys
 *
 * @note 键名数组用 `malloc` 分配，调用方需 `free` / The array is `malloc`ed and must be freed by the caller
 */
int js_obj_own_keys(JsObj *obj, char ***out, int *count);

/**
 * @brief 取对象自身的符号键标识 / Read an object's own symbol-key identities
 *
 * @param[in,out] obj 目标对象 / Target object
 * @param[out]    out 接收符号标识数组（`malloc`，调用方 `free`）/ Receives the symbol ids (`malloc`ed, freed by the caller)
 * @param[out]    count 接收个数 / Receives the count
 *
 * @return 符号个数 / The number of symbol keys
 */
int js_obj_own_symbols(JsObj *obj, long long **out, int *count);

/**
 * @brief 返回对象自身属性个数 / Return the number of own properties
 *
 * @param[in,out] obj JS 对象，可为 `NULL` / JS object, may be `NULL`
 *
 * @return 自身属性个数 / The own property count
 */
int js_obj_prop_count(JsObj *obj);

/**
 * @brief 把数值格式化为 JS 风格字符串 / Format a number as a JS-style string
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     v 待格式化的数值 / Number to format
 *
 * @return arena 中的字符串表示 / The string representation in the arena
 *
 * @note 整数值用 `%lld`，其余用 `%.14g`；`NaN`/`Infinity` 有专门拼写 / Integer values use `%lld` and others `%.14g`; `NaN` and `Infinity` have dedicated spellings
 */
char *js_num_to_string(HpeJs *js, double v);

/**
 * @brief 数值/语义上的 `ToString` 抽象操作 / The abstract `ToString` operation
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     v 任意 JS 值 / Any JS value
 *
 * @return arena 中的字符串表示 / The string representation in the arena
 */
char *js_to_string(HpeJs *js, JsValue v);

/**
 * @brief 对象到原始值的 `ToPrimitive`（default 提示）/ `ToPrimitive` on an object with the default hint
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     v 待转换的值 / Value to convert
 *
 * @return 原始值；非对象时原样返回 / The primitive value, or the input unchanged when it is not an object
 *
 * @note 先调用自身 `valueOf()` 再 `toString()`，够支撑 `obj == 1` 这类宽松比较 / Invokes the object's own `valueOf()` then `toString()`, enough for loose comparisons such as `obj == 1`
 */
JsValue js_primitive_of(HpeJs *js, JsValue v);

/**
 * @brief 语义上的 `ToNumber` 抽象操作 / The abstract `ToNumber` operation
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     v 任意 JS 值 / Any JS value
 *
 * @return 转换后的 double / The converted double
 *
 * @note `undefined`→NaN、`null`→0、空串→0、含非法尾随的字符串→NaN / `undefined` gives NaN, `null` gives 0, the empty string gives 0 and invalid trailing text gives NaN
 */
double js_to_number(HpeJs *js, JsValue v);

/**
 * @brief 语义上的 `ToBoolean` 抽象操作 / The abstract `ToBoolean` operation
 *
 * @param[in]     v 任意 JS 值 / Any JS value
 *
 * @return 真值返回 1，假值返回 0 / 1 for truthy values, 0 for falsy ones
 */
int js_truthy(JsValue v);

/**
 * @brief 按严格或宽松语义比较两个值 / Compare two values with strict or loose semantics
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     a 左操作数 / Left operand
 * @param[in]     b 右操作数 / Right operand
 * @param[in]     strict 严格比较为 1（`===`）/ 1 for strict comparison (`===`)
 *
 * @return 相等返回 1 / 1 when the values are equal
 *
 * @note 宽松模式做数值/字符串互转、布尔转数，并把 `null` 与 `undefined` 视为相等 / Loose mode coerces number and string, converts booleans and treats `null` as equal to `undefined`
 */
int js_values_equal(HpeJs *js, JsValue a, JsValue b, int strict);

/**
 * @brief 判断值是否可调用 / Test whether a value is callable
 *
 * @param[in]     v 任意 JS 值 / Any JS value
 *
 * @return 函数或原生函数返回 1 / 1 for functions and native functions
 */
int js_is_callable(JsValue v);

/**
 * @brief 判断值是否为 thenable / Test whether a value is thenable
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     v 任意 JS 值 / Any JS value
 *
 * @return Promise 或带可调用 `then` 的对象返回 1 / 1 for a Promise or an object with a callable `then`
 */
int js_is_thenable(HpeJs *js, JsValue v);

/**
 * @brief 查找对象自身的属性记录 / Look up an own property record of an object
 *
 * @param[in,out] obj JS 对象，可为 `NULL` / JS object, may be `NULL`
 * @param[in]     key 键名，可为 `NULL` / Key name, may be `NULL`
 *
 * @return 命中的属性指针，未找到返回 `NULL` / Pointer to the property, or `NULL` when absent
 *
 * @note 不走原型链 / The prototype chain is not walked
 */
struct JsProp *js_obj_own_prop(JsObj *obj, const char *key);

/**
 * @brief `[[Get]]`：读取成员并执行访问器 / `[[Get]]`: read a member honouring accessors
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 当前词法环境 / The current lexical environment
 * @param[in]     base 成员访问的基值 / The base value of the member access
 * @param[in]     prop 属性名 / The property name
 * @param[in]     line 出错时报告的行号 / The line number reported on error
 *
 * @return 读取到的属性值 / The property value read
 *
 * @note 读取 getter 并处理原型链；解释器的成员表达式与 DOM 桥都走它 / Invokes getters and follows the prototype chain; both the interpreter's member expressions and the DOM bridge use it
 *
 * @see js_set_prop
 */
JsValue js_get_prop(HpeJs *js, JsEnv *env, JsValue base, const char *prop, int line);

/**
 * @brief `[[Set]]`：写入成员并执行访问器 / `[[Set]]`: write a member honouring accessors
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 当前词法环境 / The current lexical environment
 * @param[in]     base 成员访问的基值 / The base value of the member access
 * @param[in]     prop 属性名 / The property name
 * @param[in]     value 要写入的值 / Value to write
 * @param[in]     line 出错时报告的行号 / The line number reported on error
 *
 * @note 写入 setter 并处理原型链 / Invokes setters and follows the prototype chain
 *
 * @see js_get_prop
 */
void js_set_prop(HpeJs *js, JsEnv *env, JsValue base, const char *prop, JsValue value, int line);

/**
 * @brief 格式化并抛出一个 SyntaxError / Format and throw a SyntaxError
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     line 行号，大于 0 时前缀 `line N:` / Line number, prefixed as `line N:` when positive
 * @param[in]     fmt `printf` 风格格式串 / `printf`-style format string
 *
 * @note 变参经 `vsnprintf` 渲染；本函数不返回 / Variadic arguments are rendered with `vsnprintf`; the function does not return
 */

void js_error(HpeJs *js, int line, const char *fmt, ...); /* throws an Error */

/**
 * @brief 抛出值并展开到最近的 try 帧 / Throw a value and unwind to the innermost try frame
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     v 要抛出的值 / Value to throw
 *
 * @note 记录到 `js->thrown` 后 `longjmp`，无 try 帧时回退到 `js->jmp` / The value is stored in `js->thrown` before `longjmp`, falling back to `js->jmp` when there is no try frame
 */
void js_throw_value(HpeJs *js, JsValue v);

/**
 * @brief 创建 Error 对象 / Create an Error object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     name 错误类名（如 `TypeError`）/ Error class name such as `TypeError`
 * @param[in]     msg 错误消息，可为 `NULL` / Error message, may be `NULL`
 *
 * @return 新的 Error 值 / The new Error value
 *
 * @note 按类名选择专用原型，并写入 `name` 与 `message` / The class name selects the dedicated prototype and `name` and `message` are set
 */
JsValue js_make_error(HpeJs *js, const char *name, const char *msg);

/**
 * @brief 从 C 侧调用可调用的值 / Invoke a callable value from C
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     fn 被调用的函数值 / The function value to call
 * @param[in]     thisv 调用时的 `this` 值 / The `this` value of the call
 * @param[in,out] args 实参数组，`argc` 为 0 时可为 `NULL` / Argument array, may be `NULL` when `argc` is 0
 * @param[in]     argc 参数个数 / Argument count
 * @param[in]     line 出错时报告的行号 / The line number reported on error
 *
 * @return 调用的返回值 / The call's return value
 *
 * @note 异常经 `longjmp` 向外传播 / Exceptions propagate outwards via `longjmp`
 */
JsValue js_invoke(HpeJs *js, JsValue fn, JsValue thisv, JsValue *args, int argc, int line);

/**
 * @brief 以 `new` 方式构造对象 / Construct an object with `new`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     fn 构造函数值 / The constructor value
 * @param[in,out] args 实参数组，`argc` 为 0 时可为 `NULL` / Argument array, may be `NULL` when `argc` is 0
 * @param[in]     argc 参数个数 / Argument count
 * @param[in]     line 出错时报告的行号 / The line number reported on error
 *
 * @return 新建实例，或构造函数体返回的对象 / The new instance, or the object returned by the constructor body
 *
 * @note 构造体显式返回对象时以该对象为准 / An object explicitly returned by the body takes precedence
 */
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

/**
 * @brief 新建一个词法环境 / Create a lexical environment
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] parent 父环境，可为 `NULL` / Parent environment, may be `NULL`
 *
 * @return 新环境；`strict` 继承父环境或全局设置 / The new environment whose `strict` inherits from the parent or the global setting
 */
JsEnv *js_env_new(HpeJs *js, JsEnv *parent);

/**
 * @brief 沿环境链查找变量槽位 / Look up a variable slot along the environment chain
 *
 * @param[in,out] env 起始环境 / Starting environment
 * @param[in]     name 变量名 / Variable name
 *
 * @return 变量值槽位指针，未绑定返回 `NULL` / Pointer to the variable's value slot, or `NULL` when unbound
 */
JsValue *js_env_lookup(JsEnv *env, const char *name);

/**
 * @brief 在当前环境定义变量（已存在则赋值）/ Define a variable in the current environment, assigning when it already exists
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 目标环境 / Target environment
 * @param[in]     name 变量名 / Variable name
 * @param[in]     v 初始值 / Initial value
 *
 * @note 仅在当前环境建新槽位 / A new slot is added to this environment only
 */
void js_env_define(HpeJs *js, JsEnv *env, const char *name, JsValue v);

/**
 * @brief 在当前作用域做全新绑定（let/const/class）/ Create a fresh binding in the current scope (let/const/class)
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 目标环境 / Target environment
 * @param[in]     name 变量名 / Variable name
 * @param[in]     v 初始值 / Initial value
 *
 * @note 同名绑定已存在时直接改写其值，从而遮蔽外层 / A same-named binding is overwritten, shadowing the outer one
 */
void js_env_declare(HpeJs *js, JsEnv *env, const char *name, JsValue v);

/**
 * @brief 找到持有 `this` 绑定的最近环境 / Find the nearest environment that owns a `this` binding
 *
 * @param[in,out] env 起始环境 / Starting environment
 *
 * @return 最近持有 `this` 的环境，找不到返回 `NULL` / The nearest environment with `this`, or `NULL` when none exists
 */
JsEnv *js_env_this_env(JsEnv *env);

/**
 * @brief `[[Get]]`：读取成员并执行访问器 / `[[Get]]`: read a member honouring accessors
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 当前词法环境 / The current lexical environment
 * @param[in]     base 成员访问的基值 / The base value of the member access
 * @param[in]     prop 属性名 / The property name
 * @param[in]     line 出错时报告的行号 / The line number reported on error
 *
 * @return 读取到的属性值 / The property value read
 *
 * @note 读取 getter 并处理原型链；解释器的成员表达式与 DOM 桥都走它 / Invokes getters and follows the prototype chain; both the interpreter's member expressions and the DOM bridge use it
 *
 * @see js_set_prop
 */
JsValue js_get_prop(HpeJs *js, JsEnv *env, JsValue base, const char *prop, int line);

/**
 * @brief `[[Set]]`：写入成员并执行访问器 / `[[Set]]`: write a member honouring accessors
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 当前词法环境 / The current lexical environment
 * @param[in]     base 成员访问的基值 / The base value of the member access
 * @param[in]     prop 属性名 / The property name
 * @param[in]     value 要写入的值 / Value to write
 * @param[in]     line 出错时报告的行号 / The line number reported on error
 *
 * @note 写入 setter 并处理原型链 / Invokes setters and follows the prototype chain
 *
 * @see js_get_prop
 */
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
    JS_N_BIGINT /* `num` holds the radix, `str` the digits */,
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

/**
 * @brief 新建一个 AST 节点 / Create an AST node
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     kind 节点种类（`JS_N_*`）/ Node kind, one of `JS_N_*`
 * @param[in]     line 源码行号 / Source line number
 *
 * @return 新的 AST 节点 / The new AST node
 */
JsNode *js_node_new(HpeJs *js, int kind, int line);

/**
 * @brief 把子节点追加到父 AST 节点 / Append a child node to a parent AST node
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] parent 父节点 / Parent node
 * @param[in,out] child 要追加的子节点 / Child node to append
 */
void js_node_add(HpeJs *js, JsNode *parent, JsNode *child);

/**
 * @brief 把源码解析为脚本程序 / Parse source text into a script program
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     source 脚本源码 / Script source text
 *
 * @return 程序根 AST 节点 / The program root AST node
 *
 * @see js_parse_module
 */
JsNode *js_parse_program(HpeJs *js, const char *source);

/**
 * @brief 把源码解析为 ES 模块 / Parse source text as an ES module
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     source 模块源码 / Module source text
 *
 * @return 程序根 AST 节点 / The program root AST node
 *
 * @note 允许 `import`/`export` 语法 / `import` and `export` syntax are allowed
 *
 * @see js_parse_program
 */
JsNode *js_parse_module(HpeJs *js, const char *source);

/**
 * @brief 把源码解析为单个表达式 / Parse source text as a single expression
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     source 表达式源码 / Expression source text
 *
 * @return 表达式 AST 节点 / The expression AST node
 */
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
    /* global symbol registry backing `Symbol.for` / `Symbol.keyFor` */
    JsObj *symbol_registry;
    JsObj *bigint_proto;
    /* binary data: ArrayBuffer plus the typed-array views built over it */
    JsObj *arraybuffer_proto;
    JsObj *dataview_proto;
    JsObj **ta_proto; /* [k_ta_count], indexed by TaKind */
    JsObj *ta_proto_base;
    JsObj *date_proto;
    JsObj *weakmap_proto;
    JsObj *weakset_proto;
    JsObj *weakref_proto;
    JsObj *finreg_proto;
    /* generator support. `yield_sink` is the old eager-mode collector and stays
     * only so a nested eager run still has somewhere to append; a real generator
     * instead parks here while its fiber is suspended. */
    JsObj *yield_sink;
    /* the coroutine backing a running generator, NULL outside one */
    struct HpeJsGen *gen_active;
    /* suspended generator waiting to be resumed, set by `yield` */
    struct HpeJsGen *gen_waiting;
    /* Unique marker object for `Generator.prototype.return`. Resuming with a
     * return completion must unwind the body so `finally` blocks run, but must
     * NOT be catchable by a user `catch`, so it travels as a distinguished
     * object that `exec_try_catch` recognises and passes through. */
    JsObj *gen_ret_sentinel;
    /* microtask queue (Promise jobs and then() reactions) */
    JsJob *jobs_head;
    JsJob *jobs_tail;
    int jobs_running;
    /* timer queues (setTimeout / setInterval / requestAnimationFrame) */
    struct JsTimer *timers;
    int timer_count;
    size_t timer_cap;
    int next_timer_id;
    /* monotonic creation counter: a pump snapshots it so callbacks queued from
     * inside another callback do not run until the next pump */
    int next_timer_seq;
    /* HTML timer nesting level of the callback currently on the stack, 0 when
     * no timer callback is running. A timer created from inside a callback
     * inherits this + 1, and past level 5 the 4 ms floor applies. */
    int timer_level;
    /* Set while a native constructor body runs, so it can tell `new X()` from
     * a plain call; the two need different results (`this` vs an error). */
    int in_ctor;
    /* modules */
    JsModule *modules;
    JsModule *current_module; /* module being evaluated (import/export target) */
    HpeJsModuleLoader loader;
    const char *current_dir; /* directory of the module currently being evaluated */
    const char *entry_path;  /* host-supplied path of the entry module (import.meta.url) */
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

/**
 * @brief 格式化并抛出一个 SyntaxError / Format and throw a SyntaxError
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     line 行号，大于 0 时前缀 `line N:` / Line number, prefixed as `line N:` when positive
 * @param[in]     fmt `printf` 风格格式串 / `printf`-style format string
 *
 * @note 变参经 `vsnprintf` 渲染；本函数不返回 / Variadic arguments are rendered with `vsnprintf`; the function does not return
 */
void js_error(HpeJs *js, int line, const char *fmt, ...);

/**
 * @brief 创建宿主对象并包装为 JS 值 / Create a host object and wrap it as a JS value
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] ptr 宿主指针 / Host pointer
 * @param[in]     type 宿主类型标记（`HPE_HOST_*`）/ Host type tag
 * @param[in]     vt 可选的属性拦截虚表 / Optional property-access vtable
 *
 * @return 包装宿主对象的 `JS_OBJ` 值 / A `JS_OBJ` value wrapping the host object
 *
 * @see js_host_ptr
 */
JsValue js_make_host(HpeJs *js, void *ptr, int type, const HpeJsHostVt *vt);

/**
 * @brief 取回宿主对象内的 C 指针 / Retrieve the C pointer held by a host object
 *
 * @param[in]     v 待检查的值 / Value to inspect
 * @param[in]     type 期望的宿主类型标记 / Expected host type tag
 *
 * @return 类型匹配时返回宿主指针，否则返回 `NULL` / The host pointer when the type matches, otherwise `NULL`
 */
void *js_host_ptr(JsValue v, int type); /* NULL if not a host of that type */

/**
 * @brief 在全局对象上安装定时器与 rAF / Install the timer and rAF functions on the global object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 */
void hpe_js_timers_install(HpeJs *js);

/**
 * @brief 执行到期定时器与 rAF 回调 / Run due timers and rAF callbacks
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     now 当前时间（秒），与定时器到期时刻同基准 / Current time in seconds, on the same basis as the deadlines
 * @param[out]    fired 任一回调执行后置 1，可为 `NULL` / Set to 1 when any callback actually ran; may be `NULL`
 *
 * @return 实际执行的回调个数 / The number of callbacks that actually ran
 *
 * @note 宿主可据 `*fired` 决定是否重排重绘 / The embedder can decide whether to relayout and repaint from `*fired`
 */
int hpe_js_run_timers(HpeJs *js, double now, int *fired);

/**
 * @brief 运行排队的 Promise 微任务直到清空 / Run queued Promise microtasks to completion
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 */
void hpe_js_drain_jobs(HpeJs *js);

/**
 * @brief 把作业追加到微任务队列尾 / Append a job to the tail of the microtask queue
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] job 要入队的作业 / The job to enqueue
 */
void js_job_push(HpeJs *js, JsJob *job);

/**
 * @brief 把 `fn(arg)` 排为微任务 / Queue `fn(arg)` as a microtask
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     fn 要调用的函数值 / The function value to call
 * @param[in]     arg 传给函数的唯一实参 / The single argument passed to the function
 *
 * @note MutationObserver 用它异步投递记录，对应规范中观察者微任务的一步 / MutationObserver uses it to deliver records asynchronously, mirroring the spec's observer microtask step
 */
void hpe_js_queue_microtask(HpeJs *js, JsValue fn, JsValue arg);

/**
 * @brief 兑现或拒绝一个 Promise / Fulfil or reject a Promise
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] p 目标 Promise 对象 / Target Promise object
 * @param[in]     state 目标状态（1 兑现 / 2 拒绝）/ Target state: 1 fulfilled, 2 rejected
 * @param[in]     value 兑现值或拒绝原因 / The fulfilment value or rejection reason
 */
void js_settle_promise(HpeJs *js, JsObj *p, int state, JsValue value);

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
 */
void hpe_js_set_current_dir(HpeJs *js, const char *dir);

/**
 * @brief 创建 `Date.prototype` 并安装全局 `Date` / Build `Date.prototype` and install the global `Date`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 */
void hpe_js_date_install(HpeJs *js);

/**
 * @brief 拆解后的公历字段（供 `Intl.DateTimeFormat` 使用）/ Split Gregorian fields, used by `Intl.DateTimeFormat`
 *
 * @note 月份为 1..12，星期为 0（周日）..6，与 `DateParts` 一致 / The month is 1..12 and the weekday is 0
 *       (Sunday)..6, matching `DateParts`
 */
typedef struct HpeDateFields {
    int year;
    int month; /* 1..12 */
    int day;
    int weekday; /* 0 = Sunday */
    int hour;
    int minute;
    int second;
    int ms;
} HpeDateFields;

/**
 * @brief 把 Date 值拆成 UTC 日历字段 / Split a Date value into UTC calendar fields
 *
 * @param[in]     datev Date 值 / The Date value
 * @param[out]    out 接收拆解后的字段 / Receives the split fields
 *
 * @return 成功返回 1，非 Date 或无效日期返回 0 / 1 on success, 0 for a non-Date or an invalid date
 */
int hpe_js_date_fields(JsValue datev, HpeDateFields *out);

/**
 * @brief 创建 `Intl` 命名空间并挂在全局 / Build the `Intl` namespace and expose it globally
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @note 提供 `NumberFormat` / `DateTimeFormat` / `Collator` 及 `getCanonicalLocales` /
 *       Provides `NumberFormat` / `DateTimeFormat` / `Collator` plus `getCanonicalLocales`
 */
void hpe_js_intl_install(HpeJs *js);

/* regex (hpe_js_regex.c): compiled once per literal, matched on demand */typedef struct HpeRegex HpeRegex;
/**
 * @brief 编译正则模式串为匹配程序 / Compile a regex pattern into a matcher program
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     pattern 模式串，`NULL` 视为空模式 / Pattern string, `NULL` treated as empty
 *
 * @return arena 中新建的正则对象 / The new regex object in the arena
 *
 * @note 标志位初始为 0，需另行调用 `hpe_regex_set_flags` / Flags start at 0 and must be set later with `hpe_regex_set_flags`
 */
HpeRegex *hpe_regex_compile(HpeJs *js, const char *pattern);

/**
 * @brief 从指定位置起搜索主题串 / Search the subject starting from a given offset
 *
 * @param[in,out] re 已编译的正则对象 / A compiled regex object
 * @param[in]     subject 主题串，`NULL` 视为空串 / Subject string, `NULL` treated as empty
 * @param[in]     start 起始字节偏移 / Start byte offset
 * @param[out]    captures 接收捕获组的字节偏移数组 / Receives the capture byte-offset array
 * @param[in]     ncaps `captures` 的可用长度 / Capacity of `captures`
 *
 * @return 匹配则返回匹配结束后的字节偏移，否则返回 -1 / The byte offset just past the match, or -1 when nothing matches
 *
 * @note `captures[0]/[1]` 为整段匹配，`[2+2g]/[3+2g]` 为第 g 个捕获组；未参与匹配的组置 -1 / `captures[0]`/`[1]` describe the whole match and `[2+2g]`/`[3+2g]` the g-th group, with -1 for groups that did not participate
 */
int hpe_regex_search(HpeRegex *re, const char *subject, int start, int *captures, int ncaps);

/**
 * @brief 返回正则的捕获组个数 / Return the number of capture groups
 *
 * @param[in,out] re 正则对象，可为 `NULL` / Regex object, may be `NULL`
 *
 * @return 捕获组个数，`re` 为 `NULL` 时返回 0 / The capture group count, or 0 when `re` is `NULL`
 */
int hpe_regex_group_count(HpeRegex *re);

/**
 * @brief 取指定捕获组的名字 / Get a capture group's name
 *
 * @param[in] re 已编译的正则对象，可为 `NULL` / A compiled regex object, may be `NULL`
 * @param[in] index 捕获组序号（1 起）/ Capture index, 1-based
 *
 * @return 组名字符串，无名或越界时返回 `NULL` / The group name, or `NULL` when unnamed or out of range
 */
const char *hpe_regex_group_name(HpeRegex *re, int index);

/**
 * @brief 取正则对象的标志位掩码 / Get a regex object's flag bit mask
 *
 * @param[in] re 已编译的正则对象，可为 `NULL` / A compiled regex object, may be `NULL`
 *
 * @return 标志位掩码（`i`=1、`m`=2、`s`=4、`u`=8、`y`=16、`d`=32），`re` 为 `NULL` 时返回 0 /
 *         The flag bit mask, or 0 when `re` is `NULL`
 */
int hpe_regex_flags(HpeRegex *re);

/**
 * @brief 由标志串设置 `/i /m /s` 开关 / Apply the `/i /m /s` switches from a flag string
 *
 * @param[in,out] re 目标正则对象 / Target regex object
 * @param[in]     flags 标志串 / Flag string
 *
 * @note `i` 忽略大小写、`m` 多行、`s` 让 `.` 匹配换行 / `i` ignores case, `m` enables multi-line and `s` lets `.` match newlines
 */
void hpe_regex_set_flags(HpeRegex *re, const char *flags);

/**
 * @brief 由模式串与标志创建正则值 / Create a regex value from a pattern and flags
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     pattern 模式串 / Pattern string
 * @param[in]     flags 标志串 / Flag string
 *
 * @return 包装正则对象的 JS 值 / The JS value wrapping the regex object
 *
 * @see hpe_regex_compile hpe_regex_set_flags
 */
JsValue js_regex_new(HpeJs *js, const char *pattern, const char *flags);

/**
 * @brief 求可迭代值的元素个数 / Get the element count of an iterable value
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     v 字符串、数组、Map/Set 或类数组对象 / A string, array, Map/Set or array-like object
 *
 * @return 元素个数，无法迭代时返回 0 / The element count, or 0 when not iterable
 *
 * @note 供展开运算符、`for-of` 与 `Array.from` 使用 / Used by spread, `for-of` and `Array.from`
 *
 * @see js_iter_at
 */
int js_iter_len(HpeJs *js, JsValue v);

/**
 * @brief 取可迭代值第 i 个元素 / Get the i-th element of an iterable value
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     v 可迭代值 / The iterable value
 * @param[in]     i 元素下标 / Element index
 *
 * @return 第 i 个元素，越界或不可迭代时返回 `undefined` / The i-th element, or `undefined` when out of range or not iterable
 *
 * @see js_iter_len
 */
JsValue js_iter_at(HpeJs *js, JsValue v, int i);

/**
 * @brief 按迭代模式从 Map / Set 取一项 / Pull one item from a Map / Set according to an iteration mode
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     src 数据源（Map / Set）/ The source, a Map or Set
 * @param[in]     i 元素下标 / Element index
 * @param[in]     mode 模式：1 键、2 值、3 `[键,值]`、4 `[值,值]` / Mode: 1 keys, 2 values, 3 `[key, value]`, 4 `[value, value]`
 *
 * @return 按模式换算出的项 / The item mapped through the mode
 *
 * @note 迭代器对象（`JS_OBJ_ITER`）的 `next`/展开共用此函数 / Shared by a collection iterator's `next`
 *       and by spread
 *
 * @see js_iter_at
 */
JsValue js_iter_source_item(HpeJs *js, JsValue src, int i, int mode);

/* ---------------- BigInt ---------------- */

/**
 * @brief 判断一个值是否为 BigInt 原语 / Test whether a value is a BigInt primitive
 *
 * @param[in] v 值 / Value
 *
 * @return 是 BigInt 返回 1，否则 0 / 1 when the value is a BigInt, otherwise 0
 */
int js_is_bigint(JsValue v);

/**
 * @brief 由 `long long` 构造 BigInt / Build a BigInt from a `long long`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     v 源数值 / Source value
 *
 * @return 新的 BigInt 值 / The new BigInt value
 */
JsValue js_bigint_from_i64(HpeJs *js, long long v);

/**
 * @brief 由指定进制的数字串构造 BigInt / Build a BigInt from a digit string in the given radix
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     digits 数字串（不含符号与进制前缀）/ Digit string, without sign or radix prefix
 * @param[in]     radix 进制（2..36）/ The radix (2..36)
 *
 * @return 新的 BigInt 值 / The new BigInt value
 *
 * @note 非数字字符被忽略，因此调用方应先做语法校验 / Non-digit characters are skipped, so callers should validate the syntax first
 */
JsValue js_bigint_from_str(HpeJs *js, const char *digits, int radix);

/**
 * @brief 按进制把 BigInt 转成字符串 / Render a BigInt in the given radix
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     v BigInt 值 / A BigInt value
 * @param[in]     radix 进制（2..36）/ The radix (2..36)
 *
 * @return arena 中的字符串 / A string in the arena
 *
 * @note 进制越界时抛出 `RangeError` / Throws a `RangeError` when the radix is out of range
 */
char *js_bigint_to_radix(HpeJs *js, JsValue v, int radix);

/**
 * @brief 判断 BigInt 是否为零 / Test whether a BigInt is zero
 *
 * @param[in] v BigInt 值 / A BigInt value
 *
 * @return 为零返回 1 / 1 when the value is zero
 */
int js_bigint_is_zero(JsValue v);

/**
 * @brief 取 BigInt 的符号 / Read the sign of a BigInt
 *
 * @param[in] v BigInt 值 / A BigInt value
 *
 * @return -1、0 或 1 / -1, 0 or 1
 */
int js_bigint_sign(JsValue v);

/**
 * @brief 把 BigInt 近似转成 double / Convert a BigInt to a double, approximately
 *
 * @param[in] v BigInt 值 / A BigInt value
 *
 * @return 最接近的 double；溢出为 ±Infinity / The nearest double, or ±Infinity on overflow
 */
double js_bigint_to_number(JsValue v);

/**
 * @brief BigInt 加/减 / BigInt addition and subtraction
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     l 左操作数 / Left operand
 * @param[in]     r 右操作数 / Right operand
 * @param[in]     sub 非零表示做减法 / Non-zero to subtract
 *
 * @return 结果 / The result
 */
JsValue js_bigint_add(HpeJs *js, JsValue l, JsValue r, int sub);

/**
 * @brief BigInt 乘法 / BigInt multiplication
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     l 左操作数 / Left operand
 * @param[in]     r 右操作数 / Right operand
 *
 * @return 积 / The product
 */
JsValue js_bigint_mul(HpeJs *js, JsValue l, JsValue r);

/**
 * @brief BigInt 除法或取模 / BigInt division or remainder
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     l 被除数 / Dividend
 * @param[in]     r 除数 / Divisor
 * @param[in]     want_rem 非零表示取模 / Non-zero to take the remainder
 *
 * @return 商或余数 / The quotient or remainder
 *
 * @note 除数为零时抛出 `RangeError` / Throws a `RangeError` when the divisor is zero
 */
JsValue js_bigint_divmod(HpeJs *js, JsValue l, JsValue r, int want_rem);

/**
 * @brief BigInt 求负 / Negate a BigInt
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     v 操作数 / The operand
 *
 * @return `-v` / `-v`
 */
JsValue js_bigint_neg(HpeJs *js, JsValue v);

/**
 * @brief 比较两个 BigInt / Compare two BigInts
 *
 * @param[in] l 左操作数 / Left operand
 * @param[in] r 右操作数 / Right operand
 *
 * @return 负数、0 或正数 / A negative number, zero or a positive number
 */
int js_bigint_cmp(JsValue l, JsValue r);

/**
 * @brief BigInt 幂运算 / BigInt exponentiation
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     l 底数 / Base
 * @param[in]     r 指数 / Exponent
 *
 * @return `l ** r` / `l ** r`
 *
 * @note 指数为负时抛出 `RangeError` / Throws a `RangeError` for a negative exponent
 */
JsValue js_bigint_pow(HpeJs *js, JsValue l, JsValue r);

/**
 * @brief BigInt 按位与/或/异或 / BigInt bitwise and / or / xor
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     op `JS_OP_BAND` / `JS_OP_BOR` / `JS_OP_BXOR` / Operator
 * @param[in]     l 左操作数 / Left operand
 * @param[in]     r 右操作数 / Right operand
 *
 * @return 结果 / The result
 *
 * @note 按无限位二进制补码语义计算 / Computed with infinite-width two's-complement semantics
 */
JsValue js_bigint_bitop(HpeJs *js, int op, JsValue l, JsValue r);

/**
 * @brief BigInt 按位取反 / Bitwise complement of a BigInt
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     v 操作数 / The operand
 *
 * @return `~v` / `~v`
 */
JsValue js_bigint_not(HpeJs *js, JsValue v);

/**
 * @brief BigInt 移位 / Shift a BigInt
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     l 被移位的值 / The value being shifted
 * @param[in]     r 移位量 / Shift count
 * @param[in]     left 非零表示左移 / Non-zero for a left shift
 *
 * @return 结果 / The result
 *
 * @note 右移按算术移位（向下取整）语义 / A right shift follows arithmetic (floor) semantics
 */
JsValue js_bigint_shift(HpeJs *js, JsValue l, JsValue r, int left);

/**
 * @brief `BigInt.asIntN` / `BigInt.asUintN` 的共用实现 / Shared body of `BigInt.asIntN` and `BigInt.asUintN`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     bits 位宽 / Bit width
 * @param[in]     v 被截断的 BigInt / The value to truncate
 * @param[in]     signedp 非零表示有符号截断 / Non-zero for a signed truncation
 *
 * @return 截断后的 BigInt / The truncated BigInt
 *
 * @note 位宽非法（NaN、负数）时抛出 `RangeError` / Throws a `RangeError` for an invalid bit width
 */
JsValue js_bigint_asn(HpeJs *js, double bits, JsValue v, int signedp);

/**
 * @brief 安装 ArrayBuffer、DataView 与全部 TypedArray / Install ArrayBuffer, DataView and every TypedArray
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @note 由全局安装流程调用一次；TypedArray 原型按 `TaKind` 下标存入 `js->ta_proto` / Called once by the global installer; typed-array prototypes land in `js->ta_proto`, indexed by `TaKind`
 */
void hpe_js_buffer_install(HpeJs *js);

/**
 * @brief 判断是否为本引擎的 TypedArray / Test whether a value is one of this engine's typed arrays
 *
 * @param[in] v 待测值 / The value to test
 *
 * @return 是 TypedArray 返回 1 / 1 when it is a typed array
 *
 * @note `Array.isArray` 与 `js_values_equal` 用它排除 TypedArray / `Array.isArray` and `js_values_equal` use this to exclude typed arrays
 */
int hpe_js_is_typed_array(JsValue v);

/* ---------------- real Symbol primitive ---------------- */

/**
 * @brief 新建一个唯一符号 / Create a fresh unique symbol
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     desc 描述文本，可为 `NULL` / Description, may be `NULL`
 *
 * @return 类型为 `JS_SYMBOL` 的值 / A value of type `JS_SYMBOL`
 */
JsValue js_symbol_new(HpeJs *js, const char *desc);
JsValue js_symbol_well_known(HpeJs *js, const char *desc);

/**
 * @brief 判断一个值是否为符号 / Test whether a value is a symbol
 *
 * @param[in] v 待测值 / The value to test
 *
 * @return 是符号返回 1 / 1 when it is a symbol
 */
int js_is_symbol(JsValue v);

/**
 * @brief 取符号的唯一标识 / Read a symbol's unique identity
 *
 * @param[in] v 符号值 / The symbol value
 *
 * @return 自增标识；非符号返回 0 / The auto-incremented identity, or 0 for a non-symbol
 */
long long js_symbol_id(JsValue v);

/**
 * @brief 取符号的描述文本 / Read a symbol's description
 *
 * @param[in] v 符号值 / The symbol value
 *
 * @return 描述指针，可为 `NULL` / The description pointer, possibly `NULL`
 */
const char *js_symbol_desc(JsValue v);

/**
 * @brief 把符号转成属性键 / Render a symbol as a property key
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     v 符号值 / The symbol value
 *
 * @return 合成键 `"\x01#<id>"` / The synthetic key `"\x01#<id>"`
 */
char *js_symbol_key(HpeJs *js, JsValue v);

/**
 * @brief 判断属性键是否代表符号 / Test whether a property key represents a symbol
 *
 * @param[in] key 属性键 / Property key
 *
 * @return 是符号键返回 1 / 1 when the key stands for a symbol
 */
int js_key_is_symbol(const char *key);

/**
 * @brief 取符号的 `Symbol(desc)` 文本 / Render a symbol's `Symbol(desc)` text
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     v 符号值 / The symbol value
 *
 * @return arena 中的字符串 / A string in the arena
 */
char *js_symbol_text(HpeJs *js, JsValue v);

/**
 * @brief 在全局注册表中查取或新建符号 / Look up or create a symbol in the global registry
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     key 注册键 / The registry key
 *
 * @return 该键对应的共享符号 / The shared symbol for that key
 */
JsValue js_symbol_for(HpeJs *js, const char *key);

/**
 * @brief 反向查询注册键 `/` `Symbol.keyFor` / Reverse registry lookup for `Symbol.keyFor`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     v 符号值 / The symbol value
 *
 * @return 注册键字符串；未注册时返回 `undefined` / The registry key, or `undefined` when unregistered
 */
JsValue js_symbol_key_for(HpeJs *js, JsValue v);

/**
 * @brief 安装 `Symbol` 构造器、原型与全部知名符号 / Install the `Symbol` constructor, prototype and every well-known symbol
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 */
void hpe_js_symbol_install(HpeJs *js);

/* ---------------- lazy generators (hpe_js_gen.c) ---------------- */

typedef struct HpeJsGen HpeJsGen;

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
 * @return 生成器对象值；创建协程失败时返回一个已结束的生成器 / The generator object; a failed coroutine yields an already-finished generator
 *
 * @note 函数体此时不执行，只建立协程 / The body does not run yet; only the coroutine is built
 */
JsValue hpe_js_gen_create(HpeJs *js, JsObj *fn, JsEnv *closure, JsValue this_v, JsValue *args,
                          int argc);

/**
 * @brief 取对象背后的生成器状态 / Fetch the generator state behind an object
 *
 * @param[in] obj 待检查的对象，可为 `NULL` / The object to inspect, may be `NULL`
 *
 * @return 生成器状态；非生成器对象返回 `NULL` / The generator state, or `NULL` when the object is not a generator
 */
HpeJsGen *hpe_js_gen_of(JsObj *obj);

/**
 * @brief 从生成器拉取下一项 / Pull the next item from a generator
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] obj 生成器对象 / The generator object
 * @param[out]    out 接收 `{ value, done }` / Receives the `{ value, done }` result
 *
 * @return 仍在运行返回 1；已结束返回 0 / 1 when still live, 0 when finished
 */
int hpe_js_gen_pull(HpeJs *js, JsObj *obj, JsValue *out);

/**
 * @brief 登记当前正在执行的生成器 / Register the generator currently running
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] g 生成器状态 / The generator state
 */
void hpe_js_gen_enter(HpeJs *js, HpeJsGen *g);

/**
 * @brief 取出当前正在执行的生成器 / Fetch the generator currently running
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @return 生成器状态；不在生成器体内时为 `NULL` / The generator state, or `NULL` outside a generator body
 */
HpeJsGen *hpe_js_gen_active(HpeJs *js);

/**
 * @brief 记录本次 `yield` 产出的值 / Record the value produced by the current `yield`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] g 生成器状态 / The generator state
 * @param[in]     value 产出的值 / The produced value
 */
void hpe_js_gen_set_yielded(HpeJs *js, HpeJsGen *g, JsValue value);

/**
 * @brief 取回 `yield` 表达式的值（`next(v)` 送入的值）/ Retrieve the `yield` expression's value
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] g 生成器状态 / The generator state
 *
 * @return 送入的值 / The value sent in
 */
JsValue hpe_js_gen_get_sent(HpeJs *js, HpeJsGen *g);

/**
 * @brief 取生成器挂起所需的协程 / Fetch the coroutine a generator parks on
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] g 生成器状态 / The generator state
 *
 * @return 协程句柄 / The coroutine handle
 */
void *hpe_js_gen_coro(HpeJs *js, HpeJsGen *g);

/**
 * @brief 清除当前生成器登记 / Clear the current generator registration
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 */
void hpe_js_gen_leave(HpeJs *js);

/**
 * @brief 让当前生成器在 `yield` 处挂起 / Park the current generator at a `yield`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     value 本次产出的值 / The value produced this time
 *
 * @note 只在 `gen_active` 非空时有效；返回时 `sent` 为 `next(v)` 送入的值 / Only meaningful while `gen_active` is set; on return `sent` holds what `next(v)` passed in
 */
void hpe_js_gen_suspend(HpeJs *js, JsValue value);

/**
 * @brief 取走 `throw(v)` 停放的异常 / Take the exception parked by `throw(v)`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] g 生成器状态 / The generator state
 * @param[out]    out 接收异常值 / Receives the exception value
 *
 * @return 有待抛异常返回 1 / 1 when there was one
 *
 * @note 由 `yield` 恢复点在**协程栈上**调用 / Called by the `yield` resume point, **on the coroutine stack**
 */
int hpe_js_gen_take_pending_throw(HpeJs *js, HpeJsGen *g, JsValue *out);

/**
 * @brief 取走 `return(v)` 停放的返回完成标记 / Take the return-completion marker parked by `return(v)`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] g 生成器状态 / The generator state
 * @param[out]    out 接收 `js->gen_ret_sentinel` / Receives `js->gen_ret_sentinel`
 *
 * @return 有待处理返回完成时返回 1 / 1 when there was one
 *
 * @note 由 `yield` 恢复点在**协程栈上**调用 / Called by the `yield` resume point, **on the coroutine stack**
 */
int hpe_js_gen_take_pending_return(HpeJs *js, HpeJsGen *g, JsValue *out);

/**
 * @brief 在独立栈上运行生成器函数体 / Run a generator body on its own stack
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] fn 生成器函数对象 / The generator function object
 * @param[in,out] closure 捕获的环境 / The captured environment
 * @param[in]     this_v 调用时的 this 值 / The `this` value of the call
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 实参个数 / Argument count
 *
 * @return 函数体的返回值 / The body's return value
 *
 * @note 由 `hpe_js_gen.c` 的协程入口调用，实现留在解释器里以便访问静态执行器 / Called from the coroutine entry in `hpe_js_gen.c`; the implementation lives in the interpreter so it can reach the static executor
 */
JsValue js_run_generator_body(HpeJs *js, JsObj *fn, JsEnv *closure, JsValue this_v, JsValue *args,
                              int argc);

#endif /* HPE_JS_INTERNAL_H */
