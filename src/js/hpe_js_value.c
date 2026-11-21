/* Hope UI Engine - javascript values, objects, prototype chain, exceptions */
#include "js/hpe_js_internal.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- arena ---------------- */

#define JS_BLOCK_SIZE (512 * 1024)

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

char *hpe_js_strndup(HpeJs *js, const char *s, size_t n)
{
    char *p = (char *)hpe_js_alloc(js, n + 1);
    if (n)
        memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

char *hpe_js_strdup(HpeJs *js, const char *s)
{
    return hpe_js_strndup(js, s, s ? strlen(s) : 0);
}

/* grows an arena-allocated array: allocates a new region and copies (the old
 * region becomes arena garbage until the arena is released) */
static void *arena_grow(HpeJs *js, void *ptr, size_t old_count, size_t new_count, size_t elem)
{
    void *np = hpe_js_alloc(js, new_count * elem);
    if (ptr && old_count)
        memcpy(np, ptr, old_count * elem);
    return np;
}

static void *arena_realloc(HpeJs *js, void *ptr, size_t n, size_t elem)
{
    void *np = hpe_js_alloc(js, n * elem);
    if (ptr)
        memcpy(np, ptr, n * elem);
    return np;
}

/* ---------------- values ---------------- */

JsValue js_undef(void)
{
    JsValue v;
    memset(&v, 0, sizeof(v));
    v.t = JS_UNDEF;
    return v;
}

JsValue js_null(void)
{
    JsValue v = js_undef();
    v.t = JS_NULL;
    return v;
}

JsValue js_bool(int b)
{
    JsValue v = js_undef();
    v.t = JS_BOOL;
    v.num = b ? 1.0 : 0.0;
    return v;
}

JsValue js_num(double x)
{
    JsValue v = js_undef();
    v.t = JS_NUM;
    v.num = x;
    return v;
}

JsValue js_string_len(HpeJs *js, const char *s, size_t n)
{
    JsValue v = js_undef();
    v.t = JS_STR;
    v.str = hpe_js_strndup(js, s ? s : "", n);
    return v;
}

JsValue js_string(HpeJs *js, const char *s)
{
    return js_string_len(js, s, s ? strlen(s) : 0);
}

JsValue js_objval(JsObj *o)
{
    JsValue v = js_undef();
    v.t = JS_OBJ;
    v.obj = o;
    return v;
}

/* ---------------- plain objects ---------------- */

JsObj *js_object_raw(HpeJs *js)
{
    JsObj *o = (JsObj *)hpe_js_alloc(js, sizeof(JsObj));
    o->kind = JS_OBJ_PLAIN;
    o->class_name = "Object";
    return o;
}

JsObj *js_object_new(HpeJs *js)
{
    JsObj *o = js_object_raw(js);
    o->proto = js->object_proto;
    return o;
}

JsObj *js_array_new(HpeJs *js)
{
    JsObj *o = js_object_raw(js);
    o->kind = JS_OBJ_ARRAY;
    o->class_name = "Array";
    o->proto = js->array_proto ? js->array_proto : js->object_proto;
    js_obj_set(js, o, "length", js_num(0.0));
    return o;
}

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

JsValue js_make_host(HpeJs *js, void *ptr, int type, const HpeJsHostVt *vt)
{
    return js_objval(js_host_new(js, ptr, type, vt));
}

void *js_host_ptr(JsValue v, int type)
{
    if (v.t == JS_OBJ && v.obj && v.obj->kind == JS_OBJ_HOST && v.obj->host_type == type)
        return v.obj->host_ptr;
    return NULL;
}

JsObj *js_promise_new(HpeJs *js)
{
    JsObj *o = js_object_raw(js);
    o->kind = JS_OBJ_PROMISE;
    o->class_name = "Promise";
    o->proto = js->promise_proto;
    o->promise_state = 0;
    return o;
}

JsObj *js_module_ns_new(HpeJs *js, JsModule *mod)
{
    JsObj *o = js_object_raw(js);
    o->kind = JS_OBJ_MODULE;
    o->class_name = "Module";
    o->proto = NULL;
    o->module = mod;
    return o;
}

/* ---------------- property access ---------------- */

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

int js_obj_own_has(JsObj *obj, const char *key)
{
    return js_obj_own_prop(obj, key) != NULL;
}

int js_obj_has(JsObj *obj, const char *key)
{
    return js_obj_find_prop(obj, key) != NULL;
}

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

int js_obj_prop_count(JsObj *obj)
{
    return obj ? obj->prop_count : 0;
}

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
        if (obj->props[i].enumerable)
            keys[n++] = obj->props[i].key;
    }
    keys[n] = NULL;
    *out = keys;
    *count = n;
    return n;
}

JsValue js_obj_get(HpeJs *js, JsObj *obj, const char *key)
{
    JsProp *p = js_obj_find_prop(obj, key);
    if (!p)
        return js_undef();
    if (p->getter.t == JS_OBJ)
        return js_undef(); /* accessor: js_get_prop() calls it */
    return p->val;
}

/* numbers-as-keys helper: array index / length */
static char *key_from_index(HpeJs *js, double idx)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", (long long)idx);
    return hpe_js_strdup(js, buf);
}

/* ---------------- iterator helpers ---------------- */

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

/* i-th value of a Set's backing store (own enumerable values, insertion order) */
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
        if (v.obj->kind == JS_OBJ_MAP) {
            JsObj *o = v.obj;
            if (o->class_name && !strcmp(o->class_name, "Set"))
                return set_value_at(js, o, i);
            /* Map entry i -> [key, value]; values live as v<i> on the map */
            if (o->payload_obj) {
                JsObj *pair = js_array_new(js);
                char buf[24];
                js_obj_set(js, pair, "0", js_obj_get(js, o->payload_obj, key_from_index(js, (double)i)));
                snprintf(buf, sizeof(buf), "v%d", i);
                js_obj_set(js, pair, "1", js_obj_get(js, o, buf));
                js_obj_set(js, pair, "length", js_num(2.0));
                return js_objval(pair);
            }
            return js_undef();
        }
        {
            JsObj *src = v.obj->items ? v.obj->items : v.obj;
            char *key = key_from_index(js, (double)i);
            return js_obj_get(js, src, key);
        }
    }
    return js_undef();
}

/* ---------------- conversions ---------------- */

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

static const char *obj_class_name(JsObj *o)
{
    return (o && o->class_name) ? o->class_name : "Object";
}

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
    case JS_STR:
        return v.str ? v.str : hpe_js_strdup(js, "");
    case JS_OBJ:
        if (v.obj && v.obj->kind == JS_OBJ_ARRAY) {
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
        if (v.obj && v.obj->kind == JS_OBJ_FUNC)
            return hpe_js_strdup(js, "function");
        if (v.obj && v.obj->class_name && !strcmp(v.obj->class_name, "Symbol")) {
            /* symbols key properties by their description string, so a computed
             * access `obj[Symbol.iterator]` reaches the same slot that
             * `obj["Symbol.iterator"]` writes */
            JsProp *d = js_obj_find_prop(v.obj, "description");
            if (d && d->val.t == JS_STR)
                return d->val.str ? d->val.str : hpe_js_strdup(js, "");
            return hpe_js_strdup(js, "Symbol()");
        }
        if (v.obj && v.obj->kind == JS_OBJ_DATE) {
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
        if (v.obj && v.obj->kind == JS_OBJ_ERROR) {
            JsValue nm = js_obj_get(js, v.obj, "name");
            JsValue ms = js_obj_get(js, v.obj, "message");
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
        if (v.obj) {
            char buf[64];
            const char *cn = obj_class_name(v.obj);
            if (!strcmp(cn, "Object") || !strcmp(cn, "Module"))
                return hpe_js_strdup(js, "[object Object]");
            snprintf(buf, sizeof(buf), "[object %s]", cn);
            return hpe_js_strdup(js, buf);
        }
        return hpe_js_strdup(js, "[object Object]");
    default:
        return hpe_js_strdup(js, "undefined");
    }
}

/* parses a numeric literal out of the head of `s`; returns 0 when no digits */
static int parse_number_prefix(const char *s, double *out, const char **end)
{
    char buf[64];
    size_t n = 0;
    const char *p = s;
    int sign = 1;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    if (*p == '+' || *p == '-') {
        sign = (*p == '-') ? -1 : 1;
        p++;
    }
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        char *e = NULL;
        unsigned long long v = strtoull(p + 2, &e, 16);
        *out = (double)(long long)v * (double)sign;
        *end = e;
        return 1;
    }
    if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) {
        unsigned long long v = 0;
        const char *q = p + 2;
        while (*q == '0' || *q == '1') {
            v = v * 2 + (unsigned)(*q - '0');
            q++;
        }
        *out = (double)(long long)v * (double)sign;
        *end = q;
        return 1;
    }
    if (p[0] == '0' && (p[1] == 'o' || p[1] == 'O')) {
        unsigned long long v = 0;
        const char *q = p + 2;
        while (*q >= '0' && *q <= '7') {
            v = v * 8 + (unsigned)(*q - '0');
            q++;
        }
        *out = (double)(long long)v * (double)sign;
        *end = q;
        return 1;
    }
    if (!(*p >= '0' && *p <= '9') && !(*p == '.' && p[1] >= '0' && p[1] <= '9')) {
        *end = s;
        return 0;
    }
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
    case JS_STR:
        return v.str && v.str[0];
    case JS_OBJ:
        return 1;
    default:
        return 0;
    }
}

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

int js_is_callable(JsValue v)
{
    if (v.t != JS_OBJ || !v.obj)
        return 0;
    return v.obj->kind == JS_OBJ_FUNC || v.obj->kind == JS_OBJ_NATIVE;
}

/* ToPrimitive with the "default" hint: an object's own valueOf()/toString()
 * wins, otherwise the class-based string form. Enough for `obj == 1`. */
JsValue js_primitive_of(HpeJs *js, JsValue v)
{
    JsProp *p;
    if (v.t != JS_OBJ || !v.obj)
        return v;
    if (v.obj->kind == JS_OBJ_ARRAY || v.obj->kind == JS_OBJ_PROMISE)
        return js_string(js, js_to_string(js, v));
    p = js_obj_find_prop(v.obj, "valueOf");
    if (p && p->getter.t == JS_UNDEF && js_is_callable(p->val)) {
        JsValue r = js_invoke(js, p->val, v, NULL, 0, 0);
        if (r.t != JS_OBJ)
            return r;
    }
    p = js_obj_find_prop(v.obj, "toString");
    if (p && p->getter.t == JS_UNDEF && js_is_callable(p->val)) {
        JsValue r = js_invoke(js, p->val, v, NULL, 0, 0);
        if (r.t != JS_OBJ)
            return r;
    }
    return js_string(js, js_to_string(js, v));
}

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

/* ---------------- errors / exceptions ---------------- */

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

void js_throw_value(HpeJs *js, JsValue v)
{
    js->thrown = v;
    /* unwind to the innermost active try frame (installed by exec_try /
     * hpe_js_eval / hpe_js_call); js->jmp is only a last-resort safety net */
    if (js->try_top)
        longjmp(js->try_top->jmp, 1);
    longjmp(js->jmp, 1);
}

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

/* ---------------- environments ---------------- */

JsEnv *js_env_new(HpeJs *js, JsEnv *parent)
{
    JsEnv *env = (JsEnv *)hpe_js_alloc(js, sizeof(JsEnv));
    env->parent = parent;
    env->strict = parent ? parent->strict : js->strict;
    return env;
}

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

/* defines a fresh binding in exactly this scope (let/const/class), shadowing
 * an outer one */
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

JsEnv *js_env_this_env(JsEnv *env)
{
    while (env && !env->has_this)
        env = env->parent;
    return env;
}
