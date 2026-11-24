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

#endif /* HPE_JS_INTERNAL_H */
