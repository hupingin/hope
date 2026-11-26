/* Hope UI Engine - javascript values, objects, prototype chain, exceptions */
#include "js/hpe_js_internal.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- arena ---------------- */

#define JS_BLOCK_SIZE (512 * 1024)

/**
 * @brief 向 arena 追加一个新的内存块 / Append a new memory block to the arena
 *
 * @details 块首挂在 `js->blocks` 链头部，实际可用区紧跟在 `JsBlock` 头之后。
 *          The block is pushed onto the head of the `js->blocks` chain and its usable area follows the `JsBlock` header.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     size 请求的块容量，小于 `JS_BLOCK_SIZE` 时按 512 KiB 申请 / Requested block size, raised to 512 KiB when smaller
 *
 * @note 分配失败直接打印到 stderr 并 `abort()` / Allocation failure is reported to stderr and the process is aborted
 */
static void block_new(HpeJs *js, size_t size)
{
    JsBlock *b;
    if (size < JS_BLOCK_SIZE)
        size = JS_BLOCK_SIZE;
    b = (JsBlock *)malloc(sizeof(JsBlock) + size);
    if (!b) {
        fprintf(stderr, "hope-js: out of memory\n");
        abort();
    }
    b->next = js->blocks;
    b->mem = (char *)(b + 1);
    b->used = 0;
    b->size = size;
    js->blocks = b;
}

/**
 * @brief 从 arena 分配 8 字节对齐的已清零内存 / Allocate 8-byte-aligned zeroed memory from the arena
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     size 请求字节数，向上对齐到 8 的倍数 / Requested byte count, rounded up to a multiple of 8
 *
 * @return 指向已清零内存的指针 / Pointer to the zeroed memory
 *
 * @note 当前块剩余空间不足时自动追加新块；分配的内存不能单独释放，随 arena 整体回收 / A new block is appended when the current one is full; individual allocations cannot be freed and are reclaimed with the whole arena
 */
void *hpe_js_alloc(HpeJs *js, size_t size)
{
    JsBlock *b = js->blocks;
    size = (size + 7u) & ~(size_t)7u;
    if (!b || b->used + size > b->size) {
        block_new(js, size);
        b = js->blocks;
    }
    {
        void *p = b->mem + b->used;
        b->used += size;
        memset(p, 0, size);
        return p;
    }
}

/**
 * @brief 在 arena 中复制指定长度的字符串 / Copy a string of the given length into the arena
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     s 源字符串，可为 `NULL` / Source string, may be `NULL`
 * @param[in]     n 复制的字节数 / Number of bytes to copy
 *
 * @return 以 NUL 结尾的新字符串 / The new NUL-terminated string
 *
 * @note `s` 为 `NULL` 时按空串处理，结果始终以 NUL 结尾 / `NULL` is treated as the empty string and the result is always NUL-terminated
 */
char *hpe_js_strndup(HpeJs *js, const char *s, size_t n)
{
    char *p = (char *)hpe_js_alloc(js, n + 1);
    if (n)
        memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

/**
 * @brief 在 arena 中复制以 NUL 结尾的字符串 / Duplicate a NUL-terminated string into the arena
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     s 源字符串，可为 `NULL` / Source string, may be `NULL`
 *
 * @return 复制的以 NUL 结尾的字符串 / The duplicated NUL-terminated string
 *
 * @note `s` 为 `NULL` 时产生空串 / A `NULL` input yields the empty string
 */
char *hpe_js_strdup(HpeJs *js, const char *s)
{
    return hpe_js_strndup(js, s, s ? strlen(s) : 0);
}

/**
 * @brief 扩容 arena 数组：另分配并拷贝旧元素 / Grow an arena-allocated array by allocating anew and copying the old elements
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] ptr 旧数组，可为 `NULL` / The old array, may be `NULL`
 * @param[in]     old_count 旧元素个数 / Previous element count
 * @param[in]     new_count 新元素个数 / New element count
 * @param[in]     elem 单个元素字节数 / Byte size of one element
 *
 * @return 新数组的起始地址 / Start of the new array
 *
 * @note 旧区域成为 arena 垃圾，待 arena 释放时一并回收 / The old region becomes arena garbage and is reclaimed when the arena is released
 */
static void *arena_grow(HpeJs *js, void *ptr, size_t old_count, size_t new_count, size_t elem)
{
    void *np = hpe_js_alloc(js, new_count * elem);
    if (ptr && old_count)
        memcpy(np, ptr, old_count * elem);
    return np;
}

/**
 * @brief 构造 `undefined` 值 / Build an `undefined` value
 *
 * @return 类型为 `JS_UNDEF` 的值 / A value of type `JS_UNDEF`
 */

JsValue js_undef(void)
{
    JsValue v;
    memset(&v, 0, sizeof(v));
    v.t = JS_UNDEF;
    return v;
}

/**
 * @brief 构造 `null` 值 / Build a `null` value
 *
 * @return 类型为 `JS_NULL` 的值 / A value of type `JS_NULL`
 */
JsValue js_null(void)
{
    JsValue v = js_undef();
    v.t = JS_NULL;
    return v;
}

/**
 * @brief 由 C 布尔值构造 JS 布尔值 / Build a JS boolean from a C boolean
 *
 * @param[in]     b 非零为 `true` / Non-zero means `true`
 *
 * @return 类型为 `JS_BOOL` 的值 / A value of type `JS_BOOL`
 */
JsValue js_bool(int b)
{
    JsValue v = js_undef();
    v.t = JS_BOOL;
    v.num = b ? 1.0 : 0.0;
    return v;
}

/**
 * @brief 构造 JS 数值 / Build a JS number
 *
 * @param[in]     x 数值（可为 `NaN`/`Infinity`）/ Numeric value, may be `NaN` or `Infinity`
 *
 * @return 类型为 `JS_NUM` 的值 / A value of type `JS_NUM`
 */
JsValue js_num(double x)
{
    JsValue v = js_undef();
    v.t = JS_NUM;
    v.num = x;
    return v;
}

/**
 * @brief 由定长字节构造 JS 字符串 / Build a JS string from a byte range
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     s 源字节，`NULL` 视为空 / Source bytes, `NULL` treated as empty
 * @param[in]     n 字节数 / Byte count
 *
 * @return 类型为 `JS_STR` 的值 / A value of type `JS_STR`
 *
 * @note 字符串拷贝进 arena，不持有 `s` / The bytes are copied into the arena; `s` is not retained
 */
JsValue js_string_len(HpeJs *js, const char *s, size_t n)
{
    JsValue v = js_undef();
    v.t = JS_STR;
    v.str = hpe_js_strndup(js, s ? s : "", n);
    return v;
}

/**
 * @brief 由以 NUL 结尾字符串构造 JS 字符串 / Build a JS string from a NUL-terminated string
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     s 源字符串，可为 `NULL` / Source string, may be `NULL`
 *
 * @return 类型为 `JS_STR` 的值 / A value of type `JS_STR`
 */
JsValue js_string(HpeJs *js, const char *s)
{
    return js_string_len(js, s, s ? strlen(s) : 0);
}

/**
 * @brief 把对象指针包装成 JS 值 / Wrap an object pointer as a JS value
 *
 * @param[in,out] o 目标对象 / Target object
 *
 * @return 类型为 `JS_OBJ` 的值 / A value of type `JS_OBJ`
 */
JsValue js_objval(JsObj *o)
{
    JsValue v = js_undef();
    v.t = JS_OBJ;
    v.obj = o;
    return v;
}

/**
 * @brief 分配一个无原型的普通对象 / Allocate a plain object with no prototype
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @return 新对象（`kind=JS_OBJ_PLAIN`、类名 `Object`、`proto` 为 `NULL`）/ The new object with `kind=JS_OBJ_PLAIN`, class `Object` and a `NULL` prototype
 *
 * @see js_object_new
 */

JsObj *js_object_raw(HpeJs *js)
{
    JsObj *o = (JsObj *)hpe_js_alloc(js, sizeof(JsObj));
    o->kind = JS_OBJ_PLAIN;
    o->class_name = "Object";
    return o;
}

/**
 * @brief 创建一个原型为 `Object.prototype` 的普通对象 / Create a plain object whose prototype is `Object.prototype`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @return 新普通对象 / The new plain object
 *
 * @see js_object_raw
 */
JsObj *js_object_new(HpeJs *js)
{
    JsObj *o = js_object_raw(js);
    o->proto = js->object_proto;
    return o;
}

/**
 * @brief 创建一个 JS 数组对象 / Create a JS array object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @return 新数组（类名 `Array`，`length` 初始化为 0）/ The new array with class `Array` and `length` set to 0
 *
 * @note 原型取 `Array.prototype`，缺失时回退到 `Object.prototype` / The prototype is `Array.prototype`, falling back to `Object.prototype`
 */
JsObj *js_array_new(HpeJs *js)
{
    JsObj *o = js_object_raw(js);
    o->kind = JS_OBJ_ARRAY;
    o->class_name = "Array";
    o->proto = js->array_proto ? js->array_proto : js->object_proto;
    js_obj_set(js, o, "length", js_num(0.0));
    return o;
}

/**
 * @brief 由 AST 函数节点创建用户函数对象 / Create a user function object from an AST function node
 *
 * @details 从形参列表（遇到剩余参数为止）推出 `length`，写入 `name`；非箭头函数还会生成带 `constructor` 的 `.prototype` 对象。
 *          `length` is derived from the parameter list up to the first rest parameter and `name` is set; non-arrow functions also get a `.prototype` object carrying `constructor`.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] code 函数声明的 AST 节点（取自 `JsNode`）/ AST node of the function declaration
 * @param[in,out] closure 创建时捕获的词法环境 / Lexical environment captured at creation
 *
 * @return 新的函数对象 / The new function object
 */
JsObj *js_func_new(HpeJs *js, struct JsNode *code, JsEnv *closure)
{
    JsObj *o = js_object_raw(js);
    o->kind = JS_OBJ_FUNC;
    o->class_name = "Function";
    o->proto = js->func_proto;
    o->code = code;
    o->closure = closure;
    o->name = code ? code->str : NULL;
    o->fn_flags = code ? code->op : 0;
    js_obj_set(js, o, "name", js_string(js, o->name ? o->name : ""));
    if (code && code->kids && code->kids[0]) {
        JsNode *params = code->kids[0];
        int i, n = 0;
        for (i = 0; i < params->kid_count; i++) {
            if (params->kids[i] && params->kids[i]->kind == JS_N_PAT_REST)
                break;
            n++;
        }
        o->length_hint = n;
        js_obj_set(js, o, "length", js_num((double)n));
    }
    /* ordinary functions carry a `.prototype` object for `new` / instanceof */
    if (!(o->fn_flags & FN_ARROW)) {
        JsObj *proto = js_object_raw(js);
        proto->proto = js->object_proto;
        js_obj_set(js, proto, "constructor", js_objval(o));
        js_obj_set(js, o, "prototype", js_objval(proto));
    }
    return o;
}

/**
 * @brief 创建原生（C 实现）函数对象 / Create a native (C-implemented) function object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     fn 原生函数指针 / Native function pointer
 * @param[in,out] ud 透传给原生函数的用户数据 / User data passed through to the native function
 *
 * @return 类名为 `Function` 的原生函数对象 / The new native function object with class `Function`
 */
JsObj *js_native_new(HpeJs *js, JsNativeFn fn, void *ud)
{
    JsObj *o = js_object_raw(js);
    o->kind = JS_OBJ_NATIVE;
    o->class_name = "Function";
    o->proto = js->func_proto;
    o->native = fn;
    o->native_ud = ud;
    return o;
}

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
JsObj *js_host_new(HpeJs *js, void *ptr, int type, const HpeJsHostVt *vt)
{
    JsObj *o = js_object_raw(js);
    o->kind = JS_OBJ_HOST;
    o->class_name = "Host";
    o->proto = js->object_proto;
    o->host_ptr = ptr;
    o->host_type = type;
    o->vt = vt;
    return o;
}

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
 * @see js_host_new
 */
JsValue js_make_host(HpeJs *js, void *ptr, int type, const HpeJsHostVt *vt)
{
    return js_objval(js_host_new(js, ptr, type, vt));
}

/**
 * @brief 取回宿主对象内的 C 指针 / Retrieve the C pointer held by a host object
 *
 * @param[in]     v 待检查的值 / Value to inspect
 * @param[in]     type 期望的宿主类型标记 / Expected host type tag
 *
 * @return 类型匹配时返回宿主指针，否则返回 `NULL` / The host pointer when the type matches, otherwise `NULL`
 */
void *js_host_ptr(JsValue v, int type)
{
    if (v.t == JS_OBJ && v.obj && v.obj->kind == JS_OBJ_HOST && v.obj->host_type == type)
        return v.obj->host_ptr;
    return NULL;
}

/**
 * @brief 创建一个处于 pending 状态的 Promise / Create a Promise in the pending state
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @return 类名为 `Promise`、`promise_state` 为 0 的对象 / The new object with class `Promise` and `promise_state` 0
 */
JsObj *js_promise_new(HpeJs *js)
{
    JsObj *o = js_object_raw(js);
    o->kind = JS_OBJ_PROMISE;
    o->class_name = "Promise";
    o->proto = js->promise_proto;
    o->promise_state = 0;
    return o;
}

/**
 * @brief 创建模块命名空间对象 / Create a module namespace object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] mod 该命名空间所属的模块 / The module this namespace belongs to
 *
 * @return 类名为 `Module`、无原型的对象 / The new object with class `Module` and no prototype
 */
JsObj *js_module_ns_new(HpeJs *js, JsModule *mod)
{
    JsObj *o = js_object_raw(js);
    o->kind = JS_OBJ_MODULE;
    o->class_name = "Module";
    o->proto = NULL;
    o->module = mod;
    return o;
}

/**
 * @brief 查找对象自身的属性记录 / Look up an own property record of an object
 *
 * @param[in,out] obj JS 对象，可为 `NULL` / JS object, may be `NULL`
 * @param[in]     key 键名，可为 `NULL` / Key name, may be `NULL`
 *
 * @return 命中的属性指针，未找到返回 `NULL` / Pointer to the property, or `NULL` when absent
 *
 * @note 只查自身属性，不沿原型链 / Only own properties are searched; the prototype chain is not walked
 */

JsProp *js_obj_own_prop(JsObj *obj, const char *key)
{
    int i;
    if (!obj || !key)
        return NULL;
    for (i = 0; i < obj->prop_count; i++) {
        if (obj->props[i].key[0] == key[0] && strcmp(obj->props[i].key, key) == 0)
            return &obj->props[i];
    }
    return NULL;
}

/**
 * @brief 沿原型链查找属性记录 / Find a property record along the prototype chain
 *
 * @param[in,out] obj 起始对象 / Starting object
 * @param[in]     key 键名 / Key name
 *
 * @return 命中的属性指针，未找到返回 `NULL` / Pointer to the property, or `NULL` when not found
 *
 * @note 原型链最多上溯 100 层，防止自引用造成死循环 / The chain is walked at most 100 levels to guard against cycles
 */
JsProp *js_obj_find_prop(JsObj *obj, const char *key)
{
    JsObj *o = obj;
    int depth = 0;
    while (o && depth++ < 100) {
        JsProp *p = js_obj_own_prop(o, key);
        if (p)
            return p;
        o = o->proto;
    }
    return NULL;
}

/**
 * @brief 判断对象自身是否有某属性 / Test whether an object has an own property
 *
 * @param[in,out] obj JS 对象 / JS object
 * @param[in]     key 键名 / Key name
 *
 * @return 自身存在该属性返回 1 / 1 when the own property exists
 */
int js_obj_own_has(JsObj *obj, const char *key)
{
    return js_obj_own_prop(obj, key) != NULL;
}

/**
 * @brief 判断对象（含原型链）是否有某属性 / Test whether an object has a property, prototype chain included
 *
 * @param[in,out] obj JS 对象 / JS object
 * @param[in]     key 键名 / Key name
 *
 * @return 自身或原型链上存在返回 1 / 1 when found on the object or its prototype chain
 */
int js_obj_has(JsObj *obj, const char *key)
{
    return js_obj_find_prop(obj, key) != NULL;
}

/**
 * @brief 为对象追加一条新属性记录 / Append a new property record to an object
 *
 * @details 属性数组按需倍增扩容；新记录的值为 `undefined`、enumerable 为 1。
 *          The property array doubles on demand; the new record starts with an `undefined` value and `enumerable` 1.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] obj 目标对象 / Target object
 * @param[in]     key 新属性键名 / Key of the new property
 *
 * @return 新属性记录 / The new property record
 */
static JsProp *obj_add_prop(HpeJs *js, JsObj *obj, const char *key)
{
    JsProp *p;
    if ((size_t)obj->prop_count + 1 > obj->prop_cap) {
        size_t ncap = obj->prop_cap ? obj->prop_cap * 2 : 8;
        while (ncap < (size_t)obj->prop_count + 1)
            ncap *= 2;
        obj->props = (JsProp *)arena_grow(js, obj->props, (size_t)obj->prop_count, ncap,
                                          sizeof(JsProp));
        obj->prop_cap = ncap;
    }
    p = &obj->props[obj->prop_count++];
    p->key = hpe_js_strdup(js, key);
    p->val = js_undef();
    p->getter = js_undef();
    p->setter = js_undef();
    p->enumerable = 1;
    return p;
}

/**
 * @brief 设置对象属性，不存在则创建 / Set an object property, creating it when absent
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] obj 目标对象，可为 `NULL` / Target object, may be `NULL`
 * @param[in]     key 属性键名，可为 `NULL` / Property key, may be `NULL`
 * @param[in]     v 要写入的值 / Value to store
 *
 * @note 数组的 `length` 与 `Symbol.*` 前缀键会被置为非枚举 / Array `length` and `Symbol.*`-prefixed keys are marked non-enumerable
 */
void js_obj_set(HpeJs *js, JsObj *obj, const char *key, JsValue v)
{
    JsProp *p;
    if (!obj || !key)
        return;
    p = js_obj_own_prop(obj, key);
    if (!p)
        p = obj_add_prop(js, obj, key);
    p->val = v;
    /* array `length` and the symbol-iterator hooks stay hidden from for-in */
    if ((obj->kind == JS_OBJ_ARRAY && !strcmp(key, "length")) || !strncmp(key, "Symbol.", 7))
        p->enumerable = 0;
}

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
void js_obj_set_accessor(HpeJs *js, JsObj *obj, const char *key, JsValue getter, JsValue setter)
{
    JsProp *p = js_obj_own_prop(obj, key);
    if (!p)
        p = obj_add_prop(js, obj, key);
    if (getter.t != JS_UNDEF)
        p->getter = getter;
    if (setter.t != JS_UNDEF)
        p->setter = setter;
}

/**
 * @brief 删除对象的自身属性 / Delete an own property of an object
 *
 * @param[in,out] js JS 运行时上下文（未使用）/ JS runtime context (unused)
 * @param[in,out] obj 目标对象，可为 `NULL` / Target object, may be `NULL`
 * @param[in]     key 属性键名，可为 `NULL` / Property key, may be `NULL`
 *
 * @return 总是返回 1 / Always returns 1
 *
 * @note 删除时用 `memmove` 保持其余属性的相对顺序 / Remaining properties keep their order via `memmove`
 */
int js_obj_delete(HpeJs *js, JsObj *obj, const char *key)
{
    int i;
    HPE_UNUSED(js);
    if (!obj || !key)
        return 1;
    for (i = 0; i < obj->prop_count; i++) {
        if (!strcmp(obj->props[i].key, key)) {
            memmove(&obj->props[i], &obj->props[i + 1],
                    (size_t)(obj->prop_count - i - 1) * sizeof(JsProp));
            obj->prop_count--;
            return 1;
        }
    }
    return 1;
}

/**
 * @brief 返回对象自身属性个数 / Return the number of own properties
 *
 * @param[in,out] obj JS 对象，可为 `NULL` / JS object, may be `NULL`
 *
 * @return 自身属性个数，`obj` 为 `NULL` 时返回 0 / The own property count, or 0 when `obj` is `NULL`
 */
int js_obj_prop_count(JsObj *obj)
{
    return obj ? obj->prop_count : 0;
}

/**
 * @brief 收集对象的可枚举自身键名 / Collect the enumerable own keys of an object
 *
 * @param[in,out] obj JS 对象，可为 `NULL` / JS object, may be `NULL`
 * @param[out]    out 接收以 `NULL` 结尾的键名数组 / Receives a `NULL`-terminated key array
 * @param[out]    count 键名个数 / Receives the number of keys
 *
 * @return 键名个数 / The number of keys
 *
 * @note 键名数组用 `malloc` 分配，调用方需 `free`；仅枚举 `enumerable` 为真的属性。符号键被排除，因为规范规定它们不进入 `Object.keys`/for-in / The array is `malloc`ed and must be freed by the caller; only `enumerable` properties are included. Symbol keys are excluded because the spec keeps them out of `Object.keys` and for-in
 */
int js_obj_own_keys(JsObj *obj, char ***out, int *count)
{
    char **keys;
    int i, n = 0;
    if (!obj) {
        *out = NULL;
        *count = 0;
        return 0;
    }
    keys = (char **)malloc(sizeof(char *) * (size_t)(obj->prop_count + 1));
    if (!keys) {
        *out = NULL;
        *count = 0;
        return 0;
    }
    for (i = 0; i < obj->prop_count; i++) {
        if (!obj->props[i].enumerable || js_key_is_symbol(obj->props[i].key))
            continue;
        keys[n++] = obj->props[i].key;
    }
    keys[n] = NULL;
    *out = keys;
    *count = n;
    return n;
}

/**
 * @brief 取对象自身的符号键 / Read an object's own symbol keys
 *
 * @param[in,out] obj 目标对象 / Target object
 * @param[out]    out 接收符号标识数组 / Receives the symbol identities
 * @param[out]    count 接收个数 / Receives the count
 *
 * @return 符号个数 / The number of symbol keys
 *
 * @note 数组用 `malloc` 分配，调用方需 `free`；仅供 `Object.getOwnPropertySymbols` 使用 / The array is `malloc`ed and must be freed; used only by `Object.getOwnPropertySymbols`
 */
int js_obj_own_symbols(JsObj *obj, long long **out, int *count)
{
    long long *ids;
    int i, n = 0;
    if (!obj) {
        *out = NULL;
        *count = 0;
        return 0;
    }
    ids = (long long *)malloc(sizeof(long long) * (size_t)(obj->prop_count + 1));
    if (!ids) {
        *out = NULL;
        *count = 0;
        return 0;
    }
    for (i = 0; i < obj->prop_count; i++)
        if (js_key_is_symbol(obj->props[i].key))
            ids[n++] = strtoll(obj->props[i].key + 2, NULL, 10);
    *out = ids;
    *count = n;
    return n;
}

/**
 * @brief 读取属性值（含原型链） / Read a property value, walking the prototype chain
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] obj 目标对象 / Target object
 * @param[in]     key 属性键名 / Property key
 *
 * @return 属性值；未找到或该属性是访问器时返回 `undefined` / The property value, or `undefined` when absent or when it is an accessor
 *
 * @note 访问器属性需由 `js_get_prop()` 调用读取 / Accessor properties must be read by calling `js_get_prop()`
 */
JsValue js_obj_get(HpeJs *js, JsObj *obj, const char *key)
{
    JsProp *p = js_obj_find_prop(obj, key);
    if (!p)
        return js_undef();
    if (p->getter.t == JS_OBJ)
        return js_undef(); /* accessor: js_get_prop() calls it */
    return p->val;
}

/**
 * @brief 把数值下标格式化为属性键字符串 / Format a numeric index as a property-key string
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     idx 数值下标（截断为整数）/ Numeric index, truncated to an integer
 *
 * @return arena 中的十进制键名字符串 / A decimal key string in the arena
 */
static char *key_from_index(HpeJs *js, double idx)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", (long long)idx);
    return hpe_js_strdup(js, buf);
}

/**
 * @brief 求可迭代值的元素个数 / Get the element count of an iterable value
 *
 * @details 字符串按码点计数；Set 用后备存储的键数、Map 用其 `length`、数组用元素的 `length`；类数组（NodeList、arguments 等）取 `length` 属性。
 *          Strings count code points; a Set uses the size of its backing store, a Map its `length`, an array the element `length`; array-likes such as NodeList or `arguments` use their `length`.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     v 字符串、数组、Map/Set 或类数组对象 / A string, array, Map/Set or array-like object
 *
 * @return 元素个数，无法迭代时返回 0 / The element count, or 0 when not iterable
 */

int js_iter_len(HpeJs *js, JsValue v)
{
    if (v.t == JS_STR) {
        const char *s = v.str ? v.str : "";
        int n = 0;
        while (*s) {
            uint32_t cp;
            s += hpe_utf8_next(s, &cp);
            n++;
        }
        return n;
    }
    if (v.t == JS_OBJ && v.obj) {
        /* a live collection iterator yields "source length - cursor" remaining
         * items, so for-of over an iterator object walks exactly the rest */
        if (v.obj->kind == JS_OBJ_ITER) {
            JsValue src = js_objval(v.obj->payload_obj);
            int cursor = (int)js_to_number(js, js_obj_get(js, v.obj, "_i"));
            int total = js_iter_len(js, src) - cursor;
            return total > 0 ? total : 0;
        }
        /* Map / Set iterate their backing store: a Map has one entry per key
         * (yielding [k, v] pairs), a Set yields the values it holds */
        if (v.obj->kind == JS_OBJ_MAP) {
            JsObj *o = v.obj;
            if (o->class_name && !strcmp(o->class_name, "Set")) {
                char **keys;
                int nk = 0;
                if (o->payload_obj)
                    js_obj_own_keys(o->payload_obj, &keys, &nk);
                if (o->payload_obj)
                    free(keys);
                return nk;
            }
            return o->payload_obj
                       ? (int)js_to_number(js, js_obj_get(js, o->payload_obj, "length"))
                       : 0;
        }
        if (v.obj->kind == JS_OBJ_ARRAY || v.obj->items)
            return (int)js_to_number(js, js_obj_get(js, v.obj->items ? v.obj->items : v.obj, "length"));
        /* array-likes (NodeList, arguments, querySelectorAll results) */
        if (js_obj_has(v.obj, "length"))
            return (int)js_to_number(js, js_obj_get(js, v.obj, "length"));
    }
    return 0;
}

/**
 * @brief 取 Set 后备存储中第 i 个值 / Read the i-th value of a Set's backing store
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] o Set 对象 / The Set object
 * @param[in]     i 元素下标 / Element index
 *
 * @return 第 i 个可枚举值，越界或无后备存储时返回 `undefined` / The i-th enumerable value, or `undefined` when out of range or without a backing store
 */
static JsValue set_value_at(HpeJs *js, JsObj *o, int i)
{
    char **keys;
    int nk = 0;
    JsValue out = js_undef();
    if (!o->payload_obj)
        return js_undef();
    js_obj_own_keys(o->payload_obj, &keys, &nk);
    if (i >= 0 && i < nk)
        out = js_obj_get(js, o->payload_obj, keys[i]);
    free(keys);
    return out;
}

/**
 * @brief 由集合后备存储的键序取第 i 个值 / Read the i-th value in a collection's backing-store key order
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] o Set 风格的对象（含后备存储）/ A Set-style object with a backing store
 * @param[in]     i 元素下标 / Element index
 *
 * @return 该位置的值；越界时为 `undefined` / The value at that position, or `undefined` when out of range
 */
static JsValue set_backing_value_at(HpeJs *js, JsObj *o, int i)
{
    char **keys = NULL;
    int nk = 0;
    JsValue out = js_undef();
    if (!o->payload_obj)
        return js_undef();
    js_obj_own_keys(o->payload_obj, &keys, &nk);
    if (i >= 0 && i < nk)
        out = js_obj_get(js, o->payload_obj, keys[i]);
    free(keys);
    return out;
}

/**
 * @brief 读数组后备存储的第 i 个元素 / Read element i of an array backing store
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] arr 存储数组，可为 `NULL` / The storage array, may be `NULL`
 * @param[in]     i 元素下标 / Element index
 *
 * @return 该位置的元素；存储缺失时为 `undefined` / The element at that position, or `undefined` when there is no store
 */
static JsValue store_at(HpeJs *js, JsObj *arr, int i)
{
    if (!arr)
        return js_undef();
    return js_obj_get(js, arr, key_from_index(js, (double)i));
}

/**
 * @brief 按索引写入数组后备存储 / Write element i of an array backing store
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] arr 存储数组 / The storage array
 * @param[in]     i 元素下标 / Element index
 * @param[in]     v 写入的值 / Value to store
 *
 * @note 同时把 `length` 提升到 `i + 1`，与 `Array.prototype` 的语义一致 / `length` is raised to
 *       `i + 1`, matching `Array.prototype` semantics
 */
static void store_set(HpeJs *js, JsObj *arr, int i, JsValue v)
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
 * @brief 按迭代模式从 Map / Set 取一项 / Pull one item from a Map / Set according to an iteration mode
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     src 数据源（Map / Set）/ The source, a Map or Set
 * @param[in]     i 元素下标 / Element index
 * @param[in]     mode 模式：1 键、2 值、3 `[键,值]`、4 `[值,值]` / Mode: 1 keys, 2 values, 3 `[key, value]`, 4 `[value, value]`
 *
 * @return 按模式换算出的项 / The item mapped through the mode
 *
 * @note 模式 3/4 返回新构造的二元数组；模式 2 用于 Set 时等同于按值迭代 / Modes 3 and 4 build a fresh
 *       two-element array; mode 2 over a Set is plain value iteration
 */
JsValue js_iter_source_item(HpeJs *js, JsValue src, int i, int mode)
{
    JsObj *o = (src.t == JS_OBJ) ? src.obj : NULL;
    JsValue v;
    if (!o)
        return js_undef();
    if (o->class_name && !strcmp(o->class_name, "Set")) {
        v = set_backing_value_at(js, o, i);
        if (mode == 4) {
            JsObj *pair = js_array_new(js);
            store_set(js, pair, 0, v);
            store_set(js, pair, 1, v);
            return js_objval(pair);
        }
        return v;
    }
    if (mode == 1)
        return store_at(js, o->payload_obj, i);
    if (mode == 2 || mode == 3) {
        JsObj *pair = js_array_new(js);
        char buf[24];
        JsValue key = store_at(js, o->payload_obj, i);
        JsValue val;
        snprintf(buf, sizeof(buf), "v%d", i);
        val = js_obj_get(js, o, buf);
        if (mode == 2)
            return val;
        store_set(js, pair, 0, key);
        store_set(js, pair, 1, val);
        return js_objval(pair);
    }
    return js_undef();
}

/**
 * @brief 取可迭代值第 i 个元素 / Get the i-th element of an iterable value
 *
 * @details 字符串按码点返回单码点子串；Set 返回其第 i 个值；Map 返回 `[键, 值]` 二元数组；数组与类数组用键名下标读取。
 *          A string yields the single-code-point substring; a Set yields its i-th value; a Map yields a `[key, value]` pair; arrays and array-likes are read by numeric key.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     v 可迭代值 / The iterable value
 * @param[in]     i 元素下标 / Element index
 *
 * @return 第 i 个元素，越界或不可迭代时返回 `undefined` / The i-th element, or `undefined` when out of range or not iterable
 */
JsValue js_iter_at(HpeJs *js, JsValue v, int i)
{
    if (v.t == JS_STR) {
        const char *p = v.str ? v.str : "";
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
    if (v.t == JS_OBJ && v.obj) {
        if (v.obj->kind == JS_OBJ_ITER) {
            /* a collection iterator forwards the mode it was built with, and
             * indexes from its cursor, so spread and destructuring see the same
             * items as next() */
            int cursor = (int)js_to_number(js, js_obj_get(js, v.obj, "_i"));
            int total = js_iter_len(js, v);
            if (i < 0 || i >= total)
                return js_undef();
            return js_iter_source_item(js, js_objval(v.obj->payload_obj), cursor + i,
                                       v.obj->length_hint);
        }
        if (v.obj->kind == JS_OBJ_MAP) {
            JsObj *o = v.obj;
            if (o->class_name && !strcmp(o->class_name, "Set"))
                return set_value_at(js, o, i);
            return js_iter_source_item(js, v, i, 3);
        }
        {
            JsObj *src = v.obj->items ? v.obj->items : v.obj;
            char *key = key_from_index(js, (double)i);
            return js_obj_get(js, src, key);
        }
    }
    return js_undef();
}

/**
 * @brief 把数值格式化为 JS 风格字符串 / Format a number as a JS-style string
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     v 待格式化的数值 / Number to format
 *
 * @return arena 中的字符串：`NaN`、`Infinity`、`-Infinity`、`0` 或十进制表示 / A string in the arena: `NaN`, `Infinity`, `-Infinity`, `0` or the decimal form
 *
 * @note 整数值用 `%lld`，其余用 `%.14g` / Integer values use `%lld`, others use `%.14g`
 */

char *js_num_to_string(HpeJs *js, double v)
{
    char buf[64];
    if (v != v)
        return hpe_js_strdup(js, "NaN");
    if (v == 0.0)
        return hpe_js_strdup(js, "0");
    if (isinf(v))
        return hpe_js_strdup(js, v > 0 ? "Infinity" : "-Infinity");
    if (v == (double)(long long)v && v > -1e15 && v < 1e15)
        snprintf(buf, sizeof(buf), "%lld", (long long)v);
    else
        snprintf(buf, sizeof(buf), "%.14g", v);
    return hpe_js_strdup(js, buf);
}

/**
 * @brief 取对象的类名，缺省为 `Object` / Get an object's class name, defaulting to `Object`
 *
 * @param[in,out] o JS 对象，可为 `NULL` / JS object, may be `NULL`
 *
 * @return 类名字符串 / The class-name string
 */
static const char *obj_class_name(JsObj *o)
{
    return (o && o->class_name) ? o->class_name : "Object";
}

/**
 * @brief 数组的 `toString`：逗号连接元素 / `Array#toString`: join elements with commas
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     v 要转换的数组值 / The array value to convert
 *
 * @return arena 中的逗号连接字符串 / The comma-joined string in the arena
 *
 * @note `null`/`undefined` 元素被当作空洞跳过（不产生空槽分隔） / `null` and `undefined` elements are treated as holes and skipped
 */
static char *js_array_to_string(HpeJs *js, JsValue v)
{
    int i, len = js_iter_len(js, v);
    size_t total = 1;
    char *out, *p;
    for (i = 0; i < len; i++) {
        JsValue e = js_iter_at(js, v, i);
        total += strlen(e.t == JS_UNDEF || e.t == JS_NULL ? "" : js_to_string(js, e)) + 1;
    }
    out = (char *)hpe_js_alloc(js, total);
    p = out;
    for (i = 0; i < len; i++) {
        JsValue e = js_iter_at(js, v, i);
        char *s;
        size_t sl;
        if (e.t == JS_UNDEF || e.t == JS_NULL)
            continue;
        s = js_to_string(js, e);
        sl = strlen(s);
        if (p != out)
            *p++ = ',';
        memcpy(p, s, sl);
        p += sl;
    }
    *p = '\0';
    return out;
}

/**
 * @brief 把 Symbol 对象转为描述串 / Convert a Symbol object to its description string
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] o Symbol 对象 / The Symbol object
 *
 * @return `description` 属性字符串，缺省为 `Symbol()` / The `description` property string, or `Symbol()` when absent
 *
 * @note Symbol 用描述串给属性做键，因此 `obj[Symbol.iterator]` 与 `obj["Symbol.iterator"]` 命中同一槽位 / Symbols key properties by description, so both access forms reach the same slot
 */
static char *js_symbol_to_string(HpeJs *js, JsObj *o)
{
    /* symbols key properties by their description string, so a computed
     * access `obj[Symbol.iterator]` reaches the same slot that
     * `obj["Symbol.iterator"]` writes */
    JsProp *d = js_obj_find_prop(o, "description");
    if (d && d->val.t == JS_STR)
        return d->val.str ? d->val.str : hpe_js_strdup(js, "");
    return hpe_js_strdup(js, "Symbol()");
}

/**
 * @brief 用 Date 的原型 `toString` 渲染日期 / Render a Date via its prototype `toString`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     v Date 值 / The Date value
 *
 * @return 调用 `toString()` 得到的字符串；失败时返回 `Invalid Date` / The string returned by `toString()`, or `Invalid Date` on failure
 *
 * @note `String(date)` 走字符串提示的 ToPrimitive，因此优先生效原型上的 `toString` / `String(date)` uses the string-hint ToPrimitive, so the prototype `toString` wins
 */
static char *js_date_to_string(HpeJs *js, JsValue v)
{
    /* String(date) uses ToPrimitive with the string hint, so the
     * prototype's toString() wins (it renders "Invalid Date" for NaN) */
    JsProp *p = js_obj_find_prop(v.obj, "toString");
    if (p && p->getter.t == JS_UNDEF && js_is_callable(p->val)) {
        JsValue r = js_invoke(js, p->val, v, NULL, 0, 0);
        if (r.t == JS_STR)
            return r.str ? r.str : hpe_js_strdup(js, "");
    }
    return hpe_js_strdup(js, "Invalid Date");
}

/**
 * @brief 把 Error 对象格式化为 `name: message` / Format an Error object as `name: message`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] o Error 对象 / The Error object
 *
 * @return arena 中的错误描述串 / The error description string in the arena
 *
 * @note `name` 缺省为 `Error`；`message` 为空时只输出名字 / `name` defaults to `Error`; when `message` is empty only the name is emitted
 */
static char *js_error_to_string(HpeJs *js, JsObj *o)
{
    JsValue nm = js_obj_get(js, o, "name");
    JsValue ms = js_obj_get(js, o, "message");
    char *ns = js_to_string(js, nm.t == JS_UNDEF ? js_string(js, "Error") : nm);
    char *mes = js_to_string(js, ms.t == JS_UNDEF ? js_string(js, "") : ms);
    size_t n = strlen(ns), m = strlen(mes);
    char *out = (char *)hpe_js_alloc(js, n + m + 4);
    memcpy(out, ns, n);
    if (m) {
        out[n] = ':';
        out[n + 1] = ' ';
        memcpy(out + n + 2, mes, m);
        out[n + m + 2] = '\0';
    } else {
        out[n] = '\0';
    }
    return out;
}

/**
 * @brief 生成对象的 `[object Class]` 形式串 / Produce the `[object Class]` form for an object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] o JS 对象 / JS object
 *
 * @return `[object Object]` 或 `[object <类名>]` / `[object Object]` or `[object <class>]`
 *
 * @note 类名为 `Object` 或 `Module` 时统一输出 `[object Object]` / `Object` and `Module` both render as `[object Object]`
 */
static char *js_obj_to_string(HpeJs *js, JsObj *o)
{
    char buf[64];
    const char *cn = obj_class_name(o);
    if (!strcmp(cn, "Object") || !strcmp(cn, "Module"))
        return hpe_js_strdup(js, "[object Object]");
    snprintf(buf, sizeof(buf), "[object %s]", cn);
    return hpe_js_strdup(js, buf);
}

/**
 * @brief 按对象种类分派 `ToString` / Dispatch `ToString` by object kind
 *
 * @details 数组→逗号连接、函数→`function`、Symbol→描述串、Date→其 `toString`、Error→`name: message`，其余走 `[object Class]`。
 *          Arrays join with commas, functions render as `function`, Symbols as their description, Dates via `toString`, Errors as `name: message`, everything else as `[object Class]`.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     v 待转换对象值 / Object value to convert
 *
 * @return arena 中的字符串表示 / The string representation in the arena
 */
static char *js_object_value_to_string(HpeJs *js, JsValue v)
{
    if (!v.obj)
        return hpe_js_strdup(js, "[object Object]");
    if (v.obj->kind == JS_OBJ_ARRAY)
        return js_array_to_string(js, v);
    if (v.obj->kind == JS_OBJ_FUNC)
        return hpe_js_strdup(js, "function");
    if (v.obj->class_name && !strcmp(v.obj->class_name, "Symbol"))
        return js_symbol_to_string(js, v.obj);
    if (v.obj->kind == JS_OBJ_DATE)
        return js_date_to_string(js, v);
    if (v.obj->kind == JS_OBJ_ERROR)
        return js_error_to_string(js, v.obj);
    return js_obj_to_string(js, v.obj);
}

/**
 * @brief 数值/语义上的 `ToString` 抽象操作 / The abstract `ToString` operation
 *
 * @details `undefined`→`undefined`、`null`→`null`、布尔→`true`/`false`、数值走 `js_num_to_string`、字符串原样返回、对象走对象分派。
 *          `undefined` maps to `undefined`, `null` to `null`, booleans to their literal form, numbers through `js_num_to_string`, strings are returned as-is and objects are dispatched by kind.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     v 任意 JS 值 / Any JS value
 *
 * @return arena 中的字符串表示 / The string representation in the arena
 */
char *js_to_string(HpeJs *js, JsValue v)
{
    switch (v.t) {
    case JS_UNDEF:
        return hpe_js_strdup(js, "undefined");
    case JS_NULL:
        return hpe_js_strdup(js, "null");
    case JS_BOOL:
        return hpe_js_strdup(js, v.num != 0.0 ? "true" : "false");
    case JS_NUM:
        return js_num_to_string(js, v.num);
    case JS_BIGINT:
        return js_bigint_to_radix(js, v, 10);
    case JS_SYMBOL: {
        /* String(sym) is allowed, but every implicit string coercion is not;
         * js_to_string is the implicit path, so it must refuse. */
        js_throw_value(js, js_make_error(js, "TypeError", "Cannot convert a Symbol value to a string"));
        return hpe_js_strdup(js, "");
    }
    case JS_STR:
        return v.str ? v.str : hpe_js_strdup(js, "");
    case JS_OBJ:
        return js_object_value_to_string(js, v);
    default:
        return hpe_js_strdup(js, "undefined");
    }
}

/**
 * @brief 解析 `0x` 十六进制字面前缀 / Parse a `0x` hexadecimal literal prefix
 *
 * @param[in]     p 指向 `0x` 起始位置的输入串 / Input string pointing at the `0x` prefix
 * @param[in]     sign 符号（1 或 -1）/ Sign (1 or -1)
 * @param[out]    out 解析出的带符号数值 / Receives the signed numeric value
 * @param[out]    end 解析结束的输入位置 / Receives the input position after parsing
 *
 * @return 总是返回 1 / Always returns 1
 */
static int parse_hex_prefix(const char *p, int sign, double *out, const char **end)
{
    char *e = NULL;
    unsigned long long v = strtoull(p + 2, &e, 16);
    *out = (double)(long long)v * (double)sign;
    *end = e;
    return 1;
}

/**
 * @brief 解析 `0b`/`0o` 低进制数字串 / Parse a `0b`/`0o` low-radix digit run
 *
 * @param[in]     digits 紧跟在前缀后的数字串 / The digit run right after the prefix
 * @param[in]     base 进制（2 或 8）/ Radix (2 or 8)
 * @param[in]     sign 符号（1 或 -1）/ Sign (1 or -1)
 * @param[out]    out 解析出的带符号数值 / Receives the signed numeric value
 * @param[out]    end 解析结束的输入位置 / Receives the input position after parsing
 *
 * @return 总是返回 1 / Always returns 1
 *
 * @note 遇到超出进制的数字即停止 / Stops at the first digit not valid for the radix
 */
static int parse_radix_prefix(const char *digits, int base, int sign, double *out,
                              const char **end)
{
    unsigned long long v = 0;
    const char *q = digits;
    for (;;) {
        int d;
        if (!(*q >= '0' && *q <= '9'))
            break;
        d = *q - '0';
        if (d >= base)
            break;
        v = v * (unsigned)base + (unsigned)d;
        q++;
    }
    *out = (double)(long long)v * (double)sign;
    *end = q;
    return 1;
}

/**
 * @brief 解析十进制字面前缀（含小数与指数）/ Parse a decimal literal prefix including fraction and exponent
 *
 * @param[in]     p 指向数字起始位置的输入串 / Input string pointing at the first digit
 * @param[in]     sign 符号（1 或 -1）/ Sign (1 or -1)
 * @param[out]    out 解析出的带符号数值 / Receives the signed numeric value
 * @param[out]    end 解析结束的输入位置 / Receives the input position after parsing
 *
 * @return 总是返回 1 / Always returns 1
 *
 * @note 拷贝进 64 字节临时缓冲后用 `strtod` 转换 / The run is copied into a 64-byte scratch buffer and converted with `strtod`
 */
static int parse_decimal_prefix(const char *p, int sign, double *out, const char **end)
{
    char buf[64];
    size_t n = 0;
    while ((*p >= '0' && *p <= '9') || *p == '.') {
        if (n < sizeof(buf) - 1)
            buf[n++] = *p;
        p++;
    }
    if ((*p == 'e' || *p == 'E') && n < sizeof(buf) - 3) {
        buf[n++] = *p++;
        if (*p == '+' || *p == '-')
            buf[n++] = *p++;
        while (*p >= '0' && *p <= '9' && n < sizeof(buf) - 1)
            buf[n++] = *p++;
    }
    buf[n] = '\0';
    *out = strtod(buf, NULL) * (double)sign;
    *end = p;
    return 1;
}

/**
 * @brief 解析字符串头部的数值字面量 / Parse a numeric literal from the head of a string
 *
 * @details 先跳过空白，识别可选符号，再按 `0x`/`0b`/`0o` 前缀选择进制解析，其余走十进制。
 *          Leading whitespace is skipped, an optional sign is consumed, then the `0x`/`0b`/`0o` prefixes pick a radix and everything else falls through to decimal.
 *
 * @param[in]     s 输入字符串 / Input string
 * @param[out]    out 解析出的数值 / Receives the numeric value
 * @param[out]    end 解析结束的输入位置 / Receives the input position after parsing
 *
 * @return 成功解析出数字返回 1，否则返回 0（`*end` 置为 `s`）/ 1 when digits were parsed, otherwise 0 with `*end` set to `s`
 */
static int parse_number_prefix(const char *s, double *out, const char **end)
{
    const char *p = s;
    int sign = 1;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    if (*p == '+' || *p == '-') {
        sign = (*p == '-') ? -1 : 1;
        p++;
    }
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
        return parse_hex_prefix(p, sign, out, end);
    if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B'))
        return parse_radix_prefix(p + 2, 2, sign, out, end);
    if (p[0] == '0' && (p[1] == 'o' || p[1] == 'O'))
        return parse_radix_prefix(p + 2, 8, sign, out, end);
    if (!(*p >= '0' && *p <= '9') && !(*p == '.' && p[1] >= '0' && p[1] <= '9')) {
        *end = s;
        return 0;
    }
    return parse_decimal_prefix(p, sign, out, end);
}

/**
 * @brief 语义上的 `ToNumber` 抽象操作 / The abstract `ToNumber` operation
 *
 * @details `undefined`→NaN、`null`→0、布尔→0/1、数值原样；字符串解析前缀，空串为 0、含非法尾随为 NaN；对象先 `ToPrimitive` 再递归。
 *          `undefined` gives NaN, `null` gives 0, booleans give 0/1 and numbers pass through; strings parse a prefix, empty strings yield 0 and invalid trailing text yields NaN; objects are primitived first and then converted recursively.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     v 任意 JS 值 / Any JS value
 *
 * @return 转换后的 double / The converted double
 */
double js_to_number(HpeJs *js, JsValue v)
{
    switch (v.t) {
    case JS_UNDEF:
        return NAN;
    case JS_NULL:
        return 0.0;
    case JS_BOOL:
        return v.num != 0.0 ? 1.0 : 0.0;
    case JS_NUM:
        return v.num;
    case JS_BIGINT:
        js_throw_value(js, js_make_error(js, "TypeError", "Cannot convert a BigInt value to a number"));
        return 0.0;
    case JS_SYMBOL:
        js_throw_value(js, js_make_error(js, "TypeError", "Cannot convert a Symbol value to a number"));
        return 0.0;
    case JS_STR: {
        const char *s = v.str ? v.str : "";
        double d;
        const char *end;
        if (!parse_number_prefix(s, &d, &end)) {
            /* "  12abc" -> NaN; "" -> 0 */
            const char *p = s;
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
                p++;
            return (*p == '\0') ? 0.0 : NAN;
        }
        return d;
    }
    case JS_OBJ: {
        /* ToPrimitive("number") : valueOf/toString override the class form */
        JsValue p = js_primitive_of(js, v);
        if (p.t == JS_OBJ)
            return NAN;
        if (p.t == JS_STR)
            return js_to_number(js, p);
        return js_to_number(js, p);
    }
    default:
        return NAN;
    }
}

/**
 * @brief 语义上的 `ToBoolean` 抽象操作 / The abstract `ToBoolean` operation
 *
 * @param[in]     v 任意 JS 值 / Any JS value
 *
 * @return 真值返回 1，假值返回 0 / 1 for truthy values, 0 for falsy ones
 *
 * @note falsy 为 `undefined`、`null`、`false`、`0`、`NaN`、空串；对象恒为真 / Falsy values are `undefined`, `null`, `false`, `0`, `NaN` and the empty string; objects are always truthy
 */
int js_truthy(JsValue v)
{
    switch (v.t) {
    case JS_UNDEF:
    case JS_NULL:
        return 0;
    case JS_BOOL:
        return v.num != 0.0;
    case JS_NUM:
        return v.num != 0.0 && v.num == v.num;
    case JS_BIGINT:
        return !js_bigint_is_zero(v);
    case JS_SYMBOL:
        return 1;
    case JS_STR:
        return v.str && v.str[0];
    case JS_OBJ:
        return 1;
    default:
        return 0;
    }
}

/**
 * @brief BigInt 与另一类型之间的宽松相等 / Loose equality between a BigInt and another type
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     a 左操作数 / Left operand
 * @param[in]     b 右操作数 / Right operand
 *
 * @return 相等返回 1 / 1 when the values are equal
 *
 * @note 与数值比较时按数学值比较；与字符串比较时先把字符串转成 BigInt（整数串才可比）；与布尔值比较先转成数值 /
 *       Numbers compare mathematically; a string is converted to a BigInt first (only integer strings
 *       can match); a boolean is converted to a number first
 */
static int bigint_loose_equal(HpeJs *js, JsValue a, JsValue b)
{
    const JsValue *bi = a.t == JS_BIGINT ? &a : &b;
    const JsValue *other = a.t == JS_BIGINT ? &b : &a;
    if (other->t == JS_BIGINT)
        return js_bigint_cmp(*bi, *other) == 0;
    if (other->t == JS_NUM)
        return js_bigint_to_number(*bi) == other->num;
    if (other->t == JS_BOOL || other->t == JS_NULL || other->t == JS_UNDEF)
        return 0;
    if (other->t == JS_STR)
        return js_bigint_cmp(*bi, js_bigint_from_str(js, other->str ? other->str : "", 10)) == 0;
    if (other->t == JS_OBJ)
        return js_values_equal(js, *bi, js_primitive_of(js, *other), 0);
    return 0;
}

/**
 * @brief 按严格或宽松语义比较两个值 / Compare two values with strict or loose semantics
 *
 * @details 严格模式要求类型一致；宽松模式做数值/字符串互转、布尔转数、`null`≡`undefined`，对象与原始值比较时先求其原始值。
 *          Strict mode requires matching types; loose mode coerces number/string, converts booleans to numbers, treats `null` and `undefined` as equal and primitives an object before comparing it.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     a 左操作数 / Left operand
 * @param[in]     b 右操作数 / Right operand
 * @param[in]     strict 严格比较为 1（`===`）/ 1 for strict comparison (`===`)
 *
 * @return 相等返回 1 / 1 when the values are equal
 */
int js_values_equal(HpeJs *js, JsValue a, JsValue b, int strict)
{
    if (a.t == b.t) {
        switch (a.t) {
        case JS_UNDEF:
        case JS_NULL:
            return 1;
        case JS_BOOL:
        case JS_NUM:
            return a.num == b.num;
        case JS_BIGINT:
            return js_bigint_cmp(a, b) == 0;
        case JS_SYMBOL:
            return a.sym == b.sym;
        case JS_STR:
            return strcmp(a.str ? a.str : "", b.str ? b.str : "") == 0;
        case JS_OBJ:
            return a.obj == b.obj;
        default:
            return 0;
        }
    }
    if (strict)
        return 0;
    /* loose equality */
    if (a.t == JS_SYMBOL || b.t == JS_SYMBOL)
        return 0; /* a symbol only ever equals itself, which the strict path handled */
    if (a.t == JS_BIGINT || b.t == JS_BIGINT)
        return bigint_loose_equal(js, a, b);
    if (a.t == JS_NUM && b.t == JS_STR)
        return a.num == js_to_number(js, b);
    if (a.t == JS_STR && b.t == JS_NUM)
        return js_to_number(js, a) == b.num;
    if (a.t == JS_BOOL || b.t == JS_BOOL)
        return js_values_equal(js, js_num(js_to_number(js, a)), js_num(js_to_number(js, b)), 0);
    if ((a.t == JS_UNDEF || a.t == JS_NULL) && (b.t == JS_UNDEF || b.t == JS_NULL))
        return 1;
    if (a.t == JS_OBJ && b.t != JS_OBJ)
        return js_values_equal(js, js_primitive_of(js, a), b, 0);
    if (b.t == JS_OBJ && a.t != JS_OBJ)
        return js_values_equal(js, a, js_primitive_of(js, b), 0);
    return 0;
}

/**
 * @brief 判断值是否可调用 / Test whether a value is callable
 *
 * @param[in]     v 任意 JS 值 / Any JS value
 *
 * @return 函数或原生函数返回 1 / 1 for functions and native functions
 */
int js_is_callable(JsValue v)
{
    if (v.t != JS_OBJ || !v.obj)
        return 0;
    if (v.obj->kind == JS_OBJ_PROXY)
        return v.obj->proxy_target != NULL && js_is_callable(js_objval(v.obj->proxy_target));
    return v.obj->kind == JS_OBJ_FUNC || v.obj->kind == JS_OBJ_NATIVE;
}

/**
 * @brief 对象到原始值的 `ToPrimitive`（default 提示）/ `ToPrimitive` on an object with the default hint
 *
 * @details 数组与 Promise 直接走字符串形式；其余先调用自身 `valueOf()`，再 `toString()`，返回非对象结果即采用。
 *          Arrays and Promises go straight through the string form; otherwise the object's own `valueOf()` then `toString()` are invoked and the first non-object result is used.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     v 待转换的值 / Value to convert
 *
 * @return 原始值；非对象或无法转换时返回其字符串形式 / The primitive value, or the string form when the input is not an object or conversion failed
 *
 * @note 只够支撑 `obj == 1` 这类宽松比较 / Sufficient for loose comparisons such as `obj == 1`
 */
int js_primitive_of_checked(HpeJs *js, JsValue v, JsValue *out)
{
    if (v.t != JS_OBJ || !v.obj) {
        *out = v;
        return 1;
    }
    if (v.obj->kind == JS_OBJ_ARRAY || v.obj->kind == JS_OBJ_PROMISE) {
        *out = js_string(js, js_to_string(js, v));
        return 1;
    }
    {
        JsProp *p = js_obj_find_prop(v.obj, "valueOf");
        if (p && p->getter.t == JS_UNDEF && js_is_callable(p->val)) {
            JsValue r = js_invoke(js, p->val, v, NULL, 0, 0);
            if (r.t != JS_OBJ) {
                *out = r;
                return 1;
            }
        }
    }
    {
        JsProp *p = js_obj_find_prop(v.obj, "toString");
        if (p && p->getter.t == JS_UNDEF && js_is_callable(p->val)) {
            JsValue r = js_invoke(js, p->val, v, NULL, 0, 0);
            if (r.t != JS_OBJ) {
                *out = r;
                return 1;
            }
        }
    }
    *out = js_string(js, js_to_string(js, v));
    return 1;
}

JsValue js_primitive_of(HpeJs *js, JsValue v)
{
    JsValue out;
    js_primitive_of_checked(js, v, &out);
    return out;
}

/**
 * @brief 判断值是否为 thenable / Test whether a value is thenable
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     v 任意 JS 值 / Any JS value
 *
 * @return Promise 或带可调用 `then` 的对象返回 1 / 1 for a Promise or an object with a callable `then`
 */
int js_is_thenable(HpeJs *js, JsValue v)
{
    if (v.t != JS_OBJ || !v.obj)
        return 0;
    if (v.obj->kind == JS_OBJ_PROMISE)
        return 1;
    if (js_is_callable(js_obj_get(js, v.obj, "then")))
        return 1;
    return 0;
}

/**
 * @brief 创建 Error 对象 / Create an Error object
 *
 * @details 按 `name` 选择 `TypeError`/`RangeError`/`SyntaxError`/`ReferenceError` 的专用原型，其余回退到 `Error.prototype`，并写入 `name`、`message`。
 *          The class name picks the dedicated `TypeError`/`RangeError`/`SyntaxError`/`ReferenceError` prototype, otherwise `Error.prototype`; `name` and `message` are set.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     name 错误类名（如 `TypeError`）/ Error class name such as `TypeError`
 * @param[in]     msg 错误消息，可为 `NULL` / Error message, may be `NULL`
 *
 * @return 新的 Error 值 / The new Error value
 */

JsValue js_make_error(HpeJs *js, const char *name, const char *msg)
{
    JsObj *o = js_object_raw(js);
    JsObj *proto = js->error_proto;
    o->kind = JS_OBJ_ERROR;
    o->class_name = name;
    if (!strcmp(name, "TypeError"))
        proto = js->type_error_proto;
    else if (!strcmp(name, "RangeError"))
        proto = js->range_error_proto;
    else if (!strcmp(name, "SyntaxError"))
        proto = js->syntax_error_proto;
    else if (!strcmp(name, "ReferenceError"))
        proto = js->ref_error_proto;
    o->proto = proto;
    js_obj_set(js, o, "name", js_string(js, name));
    js_obj_set(js, o, "message", js_string(js, msg ? msg : ""));
    return js_objval(o);
}

/**
 * @brief 抛出值并展开到最近的 try 帧 / Throw a value and unwind to the innermost try frame
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     v 要抛出的值 / Value to throw
 *
 * @note 记录到 `js->thrown` 后 `longjmp` 到 `js->try_top`；无 try 帧时回退到 `js->jmp` 兜底 / The value is stored in `js->thrown` before `longjmp`-ing to `js->try_top`, falling back to `js->jmp` when there is no try frame
 */
void js_throw_value(HpeJs *js, JsValue v)
{
    js->thrown = v;
    /* unwind to the innermost active try frame (installed by exec_try /
     * hpe_js_eval / hpe_js_call); js->jmp is only a last-resort safety net */
    if (js->try_top)
        longjmp(js->try_top->jmp, 1);
    longjmp(js->jmp, 1);
}

/**
 * @brief 格式化并抛出一个 SyntaxError / Format and throw a SyntaxError
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     line 行号，大于 0 时前缀 `line N:` / Line number, prefixed as `line N:` when positive
 * @param[in]     fmt `printf` 风格格式串 / `printf`-style format string
 *
 * @note 变参经 `vsnprintf` 写入 224 字节缓冲，过长会截断 / Variadic arguments are rendered into a 224-byte buffer and may be truncated
 */
void js_error(HpeJs *js, int line, const char *fmt, ...)
{
    va_list ap;
    char msg[224];
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (line > 0) {
        char buf[256];
        snprintf(buf, sizeof(buf), "line %d: %s", line, msg);
        js_throw_value(js, js_make_error(js, "SyntaxError", buf));
    }
    js_throw_value(js, js_make_error(js, "SyntaxError", msg));
}

/**
 * @brief 新建一个词法环境 / Create a lexical environment
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] parent 父环境，可为 `NULL` / Parent environment, may be `NULL`
 *
 * @return 新环境；`strict` 继承父环境或全局设置 / The new environment whose `strict` inherits from the parent or the global setting
 */

JsEnv *js_env_new(HpeJs *js, JsEnv *parent)
{
    JsEnv *env = (JsEnv *)hpe_js_alloc(js, sizeof(JsEnv));
    env->parent = parent;
    env->strict = parent ? parent->strict : js->strict;
    return env;
}

/**
 * @brief 沿环境链查找变量槽位 / Look up a variable slot along the environment chain
 *
 * @param[in,out] env 起始环境 / Starting environment
 * @param[in]     name 变量名 / Variable name
 *
 * @return 变量值槽位指针，未绑定返回 `NULL` / Pointer to the variable's value slot, or `NULL` when unbound
 */
JsValue *js_env_lookup(JsEnv *env, const char *name)
{
    int i;
    while (env) {
        for (i = 0; i < env->var_count; i++) {
            if (env->vars[i].name[0] == name[0] && strcmp(env->vars[i].name, name) == 0)
                return &env->vars[i].val;
        }
        env = env->parent;
    }
    return NULL;
}

/**
 * @brief 在当前环境定义变量（已存在则赋值）/ Define a variable in the current environment, assigning when it already exists
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 目标环境 / Target environment
 * @param[in]     name 变量名 / Variable name
 * @param[in]     v 初始值 / Initial value
 *
 * @note 仅在当前环境建新槽位，不写入外层环境 / A new slot is added to this environment only; outer environments are not touched
 */
void js_env_define(HpeJs *js, JsEnv *env, const char *name, JsValue v)
{
    JsValue *slot = js_env_lookup(env, name);
    if (slot) {
        *slot = v;
        return;
    }
    if ((size_t)env->var_count + 1 > env->var_cap) {
        size_t ncap = env->var_cap ? env->var_cap * 2 : 8;
        while (ncap < (size_t)env->var_count + 1)
            ncap *= 2;
        env->vars = (JsVar *)arena_grow(js, env->vars, (size_t)env->var_count, ncap,
                                        sizeof(JsVar));
        env->var_cap = ncap;
    }
    env->vars[env->var_count].name = hpe_js_strdup(js, name);
    env->vars[env->var_count].val = v;
    env->var_count++;
}

/**
 * @brief 在当前作用域做全新绑定（let/const/class）/ Create a fresh binding in the current scope (let/const/class)
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] env 目标环境 / Target environment
 * @param[in]     name 变量名 / Variable name
 * @param[in]     v 初始值 / Initial value
 *
 * @note 同名绑定已存在时直接改写其值，从而遮蔽外层 / When a same-named binding exists it is overwritten, shadowing the outer one
 */
void js_env_declare(HpeJs *js, JsEnv *env, const char *name, JsValue v)
{
    int i;
    for (i = 0; i < env->var_count; i++) {
        if (!strcmp(env->vars[i].name, name)) {
            env->vars[i].val = v;
            return;
        }
    }
    js_env_define(js, env, name, v);
}

/**
 * @brief 找到持有 `this` 绑定的最近环境 / Find the nearest environment that owns a `this` binding
 *
 * @param[in,out] env 起始环境 / Starting environment
 *
 * @return 最近持有 `this` 的环境，找不到返回 `NULL` / The nearest environment with `this`, or `NULL` when none exists
 */
JsEnv *js_env_this_env(JsEnv *env)
{
    while (env && !env->has_this)
        env = env->parent;
    return env;
}
