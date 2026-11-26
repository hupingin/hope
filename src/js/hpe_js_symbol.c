/* Hope UI Engine - real Symbol primitive
 *
 * A symbol is a primitive type (`JS_SYMBOL`), not an object, so `typeof` and
 * `===` behave per spec: two `Symbol("k")` calls are distinct, and only
 * `Symbol.for("k")` hands back a shared one from the global registry.
 *
 * Property keys must reach the object model as `char *`, so a symbol-keyed
 * property is stored under a synthetic key of the form "\x01#<id>". The
 * leading `\x01` is what keeps those properties out of `Object.keys`, for-in,
 * `JSON.stringify` and object spread, matching the spec's rule that symbols
 * are never enumerated as string keys.
 *
 * Scope: no `Symbol.prototype[Symbol.toPrimitive]` hook plumbing beyond
 * `String()`/`description`/`toString`, and no `Object.getOwnPropertySymbols`.
 */
#include <stdio.h>
#include <string.h>

#include "js/hpe_js_internal.h"

struct HpeSymbol {
    long long id;
    char *desc; /* may be NULL for an anonymous symbol */
    int well_known; /* a built-in like Symbol.iterator: keyed by its own name */
};

static long long g_sym_next = 1;

/**
 * @brief 新建一个唯一符号 / Create a fresh unique symbol
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     desc 描述文本，可为 `NULL` / Description, may be `NULL`
 *
 * @return 类型为 `JS_SYMBOL` 的值 / A value of type `JS_SYMBOL`
 *
 * @note 每次调用都得到新的身份，这正是 `Symbol("k") !== Symbol("k")` 的来源 / Every call yields a new identity, which is exactly why `Symbol("k") !== Symbol("k")`
 */
JsValue js_symbol_new(HpeJs *js, const char *desc)
{
    HpeSymbol *s = (HpeSymbol *)hpe_js_alloc(js, sizeof(HpeSymbol));
    JsValue v;
    s->id = g_sym_next++;
    s->desc = desc ? hpe_js_strdup(js, desc) : NULL;
    s->well_known = 0;
    v.t = JS_SYMBOL;
    v.num = 0;
    v.str = NULL;
    v.obj = NULL;
    v.big = NULL;
    v.sym = s;
    return v;
}

/**
 * @brief 建立一个知名符号 / Create a well-known symbol
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     desc 符号名，形如 `"Symbol.iterator"` / The symbol name, e.g. `"Symbol.iterator"`
 *
 * @return 类型为 `JS_SYMBOL` 的值 / A value of type `JS_SYMBOL`
 *
 * @note 知名符号的属性键就是它自己的名字，因此内建代码书写 `obj["Symbol.iterator"]` 与用户书写 `obj[Symbol.iterator]` 命中同一槽位；匿名符号没有这样一个稳定键，只能走合成键 / A well-known symbol keys properties under its own name, so the built-in literal `obj["Symbol.iterator"]` and user code `obj[Symbol.iterator]` reach one slot; an ordinary symbol has no such stable name and uses a synthetic key instead
 */
JsValue js_symbol_well_known(HpeJs *js, const char *desc)
{
    JsValue v = js_symbol_new(js, desc);
    v.sym->well_known = 1;
    return v;
}

int js_is_symbol(JsValue v)
{
    return v.t == JS_SYMBOL && v.sym != NULL;
}

long long js_symbol_id(JsValue v)
{
    return js_is_symbol(v) ? v.sym->id : 0;
}

const char *js_symbol_desc(JsValue v)
{
    return js_is_symbol(v) ? v.sym->desc : NULL;
}

/**
 * @brief 把符号转成属性键 / Render a symbol as a property key
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     v 符号值 / The symbol value
 *
 * @return arena 中的合成键 `"\x01#<id>"` / A synthetic key in the arena, `"\x01#<id>"`
 */
char *js_symbol_key(HpeJs *js, JsValue v)
{
    char buf[32];
    if (js_is_symbol(v) && v.sym->well_known && v.sym->desc)
        return hpe_js_strdup(js, v.sym->desc);
    snprintf(buf, sizeof(buf), "\x01#%lld", js_symbol_id(v));
    return hpe_js_strdup(js, buf);
}

/**
 * @brief 判断属性键是否代表符号 / Test whether a property key represents a symbol
 *
 * @param[in] key 属性键 / Property key
 *
 * @return 是符号键返回 1 / 1 when the key stands for a symbol
 *
 * @note 该判定也是枚举时过滤符号属性的依据；知名符号用的是 `"Symbol.xxx"` 这样的自然名，普通符号用 `"\x01#<id>"` 合成名 / The same test filters symbol properties out of enumeration; well-known symbols use a natural name like `"Symbol.xxx"`, ordinary ones a synthetic `"\x01#<id>"`
 */
int js_key_is_symbol(const char *key)
{
    if (key == NULL)
        return 0;
    if (key[0] == '\x01' && key[1] == '#')
        return 1;
    return !strncmp(key, "Symbol.", 7);
}

/**
 * @brief 求符号的描述文本（形如 `Symbol(desc)`）/ Render a symbol's description text, of the form `Symbol(desc)`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     v 符号值 / The symbol value
 *
 * @return arena 中的字符串 / A string in the arena
 */
char *js_symbol_text(HpeJs *js, JsValue v)
{
    const char *d = js_symbol_desc(v);
    /* well-known symbols already carry their full "Symbol.xxx" name */
    if (js_is_symbol(v) && v.sym->well_known && d)
        return hpe_js_strdup(js, d);
    /* "Symbol(" + desc + ")" + NUL */
    {
        size_t n = 10 + (d ? strlen(d) : 0);
        char *out = (char *)hpe_js_alloc(js, n);
        snprintf(out, n, "Symbol(%s)", d ? d : "");
        return out;
    }
}

/* ---------------- global registry ---------------- */

JsValue js_symbol_for(HpeJs *js, const char *key)
{
    JsValue existing;
    if (!js->symbol_registry) {
        js->symbol_registry = js_object_raw(js);
        js->symbol_registry->proto = NULL;
    }
    existing = js_obj_get(js, js->symbol_registry, key ? key : "");
    if (js_is_symbol(existing))
        return existing;
    {
        JsValue s = js_symbol_new(js, key);
        js_obj_set(js, js->symbol_registry, key ? key : "", s);
        return s;
    }
}

JsValue js_symbol_key_for(HpeJs *js, JsValue v)
{
    JsValue got;
    if (!js_is_symbol(v) || !v.sym->desc || !js->symbol_registry)
        return js_undef();
    got = js_obj_get(js, js->symbol_registry, v.sym->desc);
    return js_values_equal(js, got, v, 1) ? js_string(js, v.sym->desc) : js_undef();
}

/* ---------------- native surface ---------------- */

static JsValue sym_arg(JsValue *a, int argc, int i)
{
    return (a && i < argc) ? a[i] : js_undef();
}

static JsValue native_symbol(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsValue d = sym_arg(a, argc, 0);
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    return js_symbol_new(js, d.t == JS_UNDEF ? NULL : js_to_string(js, d));
}

static JsValue sym_for(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsValue k = sym_arg(a, argc, 0);
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    if (k.t == JS_SYMBOL)
        js_throw_value(js, js_make_error(js, "TypeError", "Cannot convert a Symbol to a string"));
    return js_symbol_for(js, js_to_string(js, k));
}

static JsValue sym_key_for(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    return js_symbol_key_for(js, sym_arg(a, argc, 0));
}

/**
 * @brief 取接收符号（拒绝其他类型）/ Require a symbol receiver
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 接收者 / The receiver
 *
 * @return 该符号；不是符号时抛 `TypeError` 并返回 `undefined` / The symbol, or `undefined` after throwing `TypeError`
 *
 * @note 通过 `Object(sym)` 得到的包装对象会经 `js_primitive_of` 解回符号 / A wrapper from `Object(sym)` unwraps back through `js_primitive_of`
 */
static JsValue sym_this(HpeJs *js, JsValue thisv)
{
    if (js_is_symbol(thisv))
        return thisv;
    if (thisv.t == JS_OBJ && thisv.obj) {
        JsValue p = js_primitive_of(js, thisv);
        if (js_is_symbol(p))
            return p;
    }
    js_throw_value(js, js_make_error(js, "TypeError", "Not a symbol"));
    return js_undef();
}

static JsValue sym_to_string(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    return js_string(js, js_symbol_text(js, sym_this(js, thisv)));
}

static JsValue sym_value_of(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    return sym_this(js, thisv);
}

/**
 * @brief `Symbol.prototype.description` 的取值器 / Getter for `Symbol.prototype.description`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 接收者 / The receiver
 * @param[in,out] a 实参数组（未使用）/ Argument array (unused)
 * @param[in]     argc 实参个数（未使用）/ Argument count (unused)
 * @param[in,out] ud 用户数据（未使用）/ User data (unused)
 *
 * @return 描述文本；匿名符号返回 `undefined` / The description, or `undefined` for an anonymous symbol
 */
static JsValue sym_get_desc(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    const char *d;
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    d = js_symbol_desc(sym_this(js, thisv));
    return d ? js_string(js, d) : js_undef();
}

static void def_fn(HpeJs *js, JsObj *target, const char *name, JsNativeFn fn, int len)
{
    JsObj *f = js_native_new(js, fn, NULL);
    f->name = hpe_js_strdup(js, name);
    f->length_hint = len;
    js_obj_set(js, f, "name", js_string(js, name));
    js_obj_set(js, f, "length", js_num((double)len));
    js_obj_set(js, target, name, js_objval(f));
}

void hpe_js_symbol_install(HpeJs *js)
{
    JsObj *proto = js_object_raw(js);
    JsObj *ctor;
    JsValue desc_getter;
    static const char *const names[] = {
        "iterator", "asyncIterator", "toStringTag", "hasInstance", "isConcatSpreadable",
        "species",  "match",         "matchAll",    "replace",     "search",
        "split",    "toPrimitive",   "unscopables",
    };
    size_t i;
    proto->proto = js->object_proto;
    proto->class_name = "Symbol";
    js->symbol_proto = proto;
    ctor = js_native_new(js, native_symbol, NULL);
    ctor->name = "Symbol";
    js_obj_set(js, ctor, "name", js_string(js, "Symbol"));
    js_obj_set(js, ctor, "length", js_num(0));
    js_obj_set(js, ctor, "prototype", js_objval(proto));
    js_obj_set(js, proto, "constructor", js_objval(ctor));
    def_fn(js, ctor, "for", sym_for, 1);
    def_fn(js, ctor, "keyFor", sym_key_for, 1);
    def_fn(js, proto, "toString", sym_to_string, 0);
    def_fn(js, proto, "valueOf", sym_value_of, 0);
    desc_getter = js_objval(js_native_new(js, sym_get_desc, NULL));
    js_obj_set_accessor(js, proto, "description", desc_getter, js_undef());
    js_obj_set(js, js->global_obj, "Symbol", js_objval(ctor));
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char buf[40];
        snprintf(buf, sizeof(buf), "Symbol.%s", names[i]);
        js_obj_set(js, ctor, names[i], js_symbol_well_known(js, buf));
    }
}
