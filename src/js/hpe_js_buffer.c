/* Hope UI Engine - ArrayBuffer, DataView and typed arrays
 *
 * One `HpeBufData` is the backing store: a raw byte array shared by an
 * `ArrayBuffer` and every DataView or typed-array view built over it. Views
 * hold a pointer to that store plus a byte offset, so a write through one view
 * is immediately visible through the others, exactly as the spec requires.
 *
 * Typed arrays are modelled as **host objects** (`JS_OBJ_HOST`) rather than a
 * new JsObjKind. The host vtable answers indexed reads and writes straight out
 * of the bytes, while a shadow property array keeps `length`, `byteLength`,
 * `byteOffset`, `buffer` and the indices in sync so plain property lookup,
 * `Object.keys` and for-of still work. Keeping the bytes out of JS values is
 * what makes `arr[0] = 1.7` truncate instead of storing 1.7.
 *
 * Vtable identity (rather than `host_type`) tags the three object kinds, which
 * keeps this module collision-free with the DOM bridge.
 *
 * Scope: no `resizable` ArrayBuffers, no `transfer`, no `Atomics`, and
 * `BigInt64Array`/`BigUint64Array` store raw 64-bit words.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "js/hpe_js_internal.h"

/* ---------------- small helpers ---------------- */

static JsValue b_arg(JsValue *a, int argc, int i)
{
    return (a && i < argc) ? a[i] : js_undef();
}

static void b_throw(HpeJs *js, const char *msg)
{
    js_throw_value(js, js_make_error(js, "TypeError", msg));
}

/* ---------------- element kinds ---------------- */

typedef struct {
    const char *name;
    int bytes; /* element size in bytes */
    int is_float;
    int is_bigint;
    int is_signed;
} TaInfo;

static const TaInfo k_ta[k_ta_count] = {
    {"Int8Array", 1, 0, 0, 1},         {"Uint8Array", 1, 0, 0, 0},
    {"Uint8ClampedArray", 1, 0, 0, 0}, {"Int16Array", 2, 0, 0, 1},
    {"Uint16Array", 2, 0, 0, 0},       {"Int32Array", 4, 0, 0, 1},
    {"Uint32Array", 4, 0, 0, 0},       {"Float32Array", 4, 1, 0, 1},
    {"Float64Array", 8, 1, 0, 1},      {"BigInt64Array", 8, 0, 1, 1},
    {"BigUint64Array", 8, 0, 1, 0},
};

/* ---------------- shared byte store ---------------- */

typedef struct {
    unsigned char *bytes;
    size_t len;
    int detached;
} HpeBufData;

/* Owned by an ArrayBuffer host object; views point at the same store. */
typedef struct {
    HpeBufData *fd;
} HpeBufBox;

static HpeBufData *buf_data_new(HpeJs *js, size_t n)
{
    HpeBufData *d = (HpeBufData *)hpe_js_alloc(js, sizeof(HpeBufData));
    d->bytes = (unsigned char *)hpe_js_alloc(js, n ? n : 1);
    d->len = n;
    d->detached = 0;
    return d;
}

/* ---------------- element read / write ---------------- */

static double ta_read_float(const unsigned char *p, const TaInfo *t)
{
    if (t->bytes == 4) {
        float f;
        memcpy(&f, p, 4);
        return (double)f;
    }
    {
        double d;
        memcpy(&d, p, 8);
        return d;
    }
}

static void ta_write_float(unsigned char *p, const TaInfo *t, double v)
{
    if (t->bytes == 4) {
        float f = (float)v;
        memcpy(p, &f, 4);
    } else {
        memcpy(p, &v, 8);
    }
}

/**
 * @brief 把数值截断并归约到整数元素位宽 / Truncate a number and reduce it to the element's bit width
 *
 * @param[in] t 元素种类 / Element kind
 * @param[in] v 待转换的数值 / The number to convert
 *
 * @return `v mod 2^bits` 的无符号表示 / The unsigned residue of `v` modulo 2^bits
 *
 * @note NaN 与 ±Infinity 归零；负数按补码意义上的模归约，与规范的 ToUintN 一致 / NaN and ±Infinity become zero; a negative value reduces modulo 2^bits in the two's-complement sense, matching ToUintN
 */
static unsigned long long ta_int_store(const TaInfo *t, double v)
{
    double mod, r;
    int bits = t->bytes * 8;
    if (v != v || isinf(v))
        return 0;
    mod = ldexp(1.0, bits);
    r = fmod(trunc(v), mod);
    if (r < 0)
        r += mod;
    if (r >= mod)
        r = mod - 1.0;
    return (unsigned long long)r;
}

static int ta_hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return 0;
}

/**
 * @brief 把一个 BigInt 写进 64 位元素 / Store a BigInt into a 64-bit element
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] p 目标字节地址（8 字节小端）/ Target bytes, 8 little-endian
 * @param[in]     v 待写入的值 / The value to store
 * @param[in]     is_signed 有符号元素为 1 / 1 for a signed element
 *
 * @note 先经 `BigInt.asIntN/asUintN(64, …)` 回绕，再按十六进制串取低 64 位，绕过 `double` 的精度与符号问题 / Wraps through `BigInt.asIntN/asUintN(64, …)` then takes the low 64 bits from a hex string, sidestepping `double` precision and sign issues
 */
static void ta_store_big(HpeJs *js, unsigned char *p, JsValue v, int is_signed)
{
    JsValue w = js_bigint_asn(js, 64.0, v, is_signed);
    int neg = js_bigint_sign(w) < 0;
    int i;
    /* The radix form carries a leading '-', so take the magnitude's digits and
     * then write two's complement by hand; js_bigint_asn already wrapped the
     * value, so |w| < 2^64 and the borrow below stays inside the field. */
    if (neg)
        w = js_bigint_neg(js, w);
    {
        char *hex = js_bigint_to_radix(js, w, 16);
        int len = (int)strlen(hex);
        unsigned int borrow = neg ? 1u : 0u;
        for (i = 0; i < 8; i++) {
            int idx = len - 2 - 2 * i;
            int hi = idx >= 0 ? ta_hexval(hex[idx]) : 0;
            int lo = idx + 1 >= 0 ? ta_hexval(hex[idx + 1]) : 0;
            unsigned int byte = (unsigned int)((hi << 4) | lo);
            if (neg) {
                unsigned int t = (~byte & 0xFFu) + borrow;
                byte = t & 0xFFu;
                borrow = t >> 8;
            }
            p[i] = (unsigned char)byte;
        }
    }
}

/**
 * @brief 把一个 JS 值写进 TypedArray 元素 / Store a JS value into one typed-array element
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] p 目标字节地址 / Target byte address
 * @param[in]     kind 元素种类 / Element kind
 * @param[in]     v 待写入的 JS 值 / The value to store
 */
static void ta_store(HpeJs *js, unsigned char *p, int kind, JsValue v)
{
    const TaInfo *t = &k_ta[kind];
    unsigned long long u;
    int i;
    if (t->is_bigint) {
        if (v.t != JS_BIGINT) {
            js_throw_value(
                js, js_make_error(js, "TypeError", "Cannot convert a Number to a BigInt"));
            return;
        }
        ta_store_big(js, p, v, t->is_signed);
        return;
    }
    if (kind == TA_UINT8C) {
        double n = js_to_number(js, v);
        double r = (n != n) ? 0.0 : nearbyint(n);
        p[0] = (unsigned char)(r < 0.0 ? 0 : (r > 255.0 ? 255 : r));
        return;
    }
    if (t->is_float) {
        ta_write_float(p, t, js_to_number(js, v));
        return;
    }
    u = ta_int_store(t, js_to_number(js, v));
    for (i = 0; i < t->bytes; i++)
        p[i] = (unsigned char)((u >> (8 * i)) & 0xFF);
}

/**
 * @brief 无符号 64 位字节转 BigInt / Box unsigned 64-bit bytes as a BigInt
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     p 源字节地址（8 字节小端）/ Source bytes, 8 little-endian
 *
 * @return 无符号 BigInt / The unsigned BigInt
 *
 * @note 走十六进制串而非 `js_bigint_from_i64`，后者把最高位置位解读为负数 / Uses a hex string because `js_bigint_from_i64` reads a set top bit as negative
 */
static JsValue ta_load_bigu(HpeJs *js, const unsigned char *p)
{
    char hex[17];
    int i;
    for (i = 0; i < 8; i++)
        snprintf(hex + i * 2, 3, "%02x", p[7 - i]);
    hex[16] = '\0';
    return js_bigint_from_str(js, hex, 16);
}

/**
 * @brief 从 TypedArray 元素读出一个 JS 值 / Load one typed-array element as a JS value
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     p 源字节地址 / Source byte address
 * @param[in]     kind 元素种类 / Element kind
 *
 * @return BigInt 元素返回 `JS_BIGINT`，其余返回 `JS_NUM` / `JS_BIGINT` for BigInt elements, `JS_NUM` otherwise
 */
static JsValue ta_load(HpeJs *js, const unsigned char *p, int kind)
{
    const TaInfo *t = &k_ta[kind];
    unsigned long long u = 0;
    int i;
    if (t->is_bigint) {
        if (t->is_signed) {
            long long raw;
            memcpy(&raw, p, 8);
            return js_bigint_from_i64(js, raw);
        }
        return ta_load_bigu(js, p);
    }
    if (t->is_float)
        return js_num(ta_read_float(p, t));
    for (i = t->bytes - 1; i >= 0; i--)
        u = (u << 8) | p[i];
    if (t->is_signed && t->bytes < 4) {
        int bits = t->bytes * 8;
        if ((u >> (bits - 1)) & 1ull) {
            unsigned long long mask = (1ull << bits) - 1ull;
            return js_num((double)(long long)(u | ~mask));
        }
    }
    if (t->is_signed && t->bytes == 4)
        return js_num((double)(int)(unsigned int)u);
    return js_num((double)u);
}

/* ---------------- view descriptor ---------------- */

typedef struct {
    HpeBufData *fd; /* shared store */
    HpeBufBox *box; /* the owning ArrayBuffer's box, for subarray */
    JsObj *self;    /* the object owning this view */
    JsObj *buffer;  /* the ArrayBuffer this view belongs to */
    int kind;       /* TaKind, or -1 for a DataView */
    size_t offset;  /* byte offset into the store */
    size_t length;  /* element count (typed arrays) / byte count (DataView) */
} TaView;

/* Constructor payload: a native function carries one `void *`, so the element
 * kind and the prototype travel together in one allocation. */
typedef struct {
    int kind;
    JsObj *proto;
} TaCtor;

static int view_byte_len(const TaView *w)
{
    if (w->kind < 0)
        return (int)w->length;
    return (int)(w->length * (size_t)k_ta[w->kind].bytes);
}

/* the three vtable identities are defined below, next to their hooks */
static const HpeJsHostVt g_abuf_vt;
static const HpeJsHostVt g_ta_vt;
static const HpeJsHostVt g_dv_vt;

static TaView *ta_view_of(JsValue thisv)
{
    if (thisv.t != JS_OBJ || !thisv.obj || thisv.obj->vt != &g_ta_vt)
        return NULL;
    return (TaView *)thisv.obj->host_ptr;
}

static TaView *dv_view_of(JsValue thisv)
{
    if (thisv.t != JS_OBJ || !thisv.obj || thisv.obj->vt != &g_dv_vt)
        return NULL;
    return (TaView *)thisv.obj->host_ptr;
}

static HpeBufBox *abuf_box_of(JsValue v)
{
    if (v.t != JS_OBJ || !v.obj || v.obj->vt != &g_abuf_vt)
        return NULL;
    return (HpeBufBox *)v.obj->host_ptr;
}

/* ---------------- shadow properties ---------------- */

/**
 * @brief 把属性名解析为非负数组下标 / Parse a property name into a non-negative array index
 *
 * @param[in] prop 属性名 / Property name
 * @param[out] out 接收下标 / Receives the index
 *
 * @return 是合法下标返回 1 / 1 when the name is a valid index
 *
 * @note 拒绝空串、前导零与超过 9 位的数字 / Rejects the empty string, leading zeros and names longer than 9 digits
 */
static int index_of(const char *prop, int *out)
{
    int i = 0;
    if (!prop || !prop[0])
        return 0;
    for (; prop[i]; i++) {
        if (prop[i] < '0' || prop[i] > '9')
            return 0;
        if (i > 9)
            return 0;
    }
    if (i > 1 && prop[0] == '0')
        return 0;
    *out = atoi(prop);
    return 1;
}

/**
 * @brief 重建 TypedArray 的下标影子属性 / Rebuild a typed array's indexed shadow properties
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] w 视图描述 / The view descriptor
 *
 * @note 属性表让 `Object.keys`、for-of 与基于属性的取值路径都能看到元素，而真正的字节仍只存在于缓冲区 / The property table lets `Object.keys`, for-of and property-based reads see elements while the real bytes stay in the buffer
 */
static void ta_sync_indices(HpeJs *js, TaView *w)
{
    size_t i;
    char key[24];
    for (i = 0; i < w->length; i++) {
        snprintf(key, sizeof(key), "%u", (unsigned)i);
        js_obj_set(js, w->self, key,
                   ta_load(js, w->fd->bytes + w->offset + i * (size_t)k_ta[w->kind].bytes,
                           w->kind));
    }
}

static void ta_sync_meta(HpeJs *js, TaView *w)
{
    js_obj_set(js, w->self, "length", js_num((double)w->length));
    js_obj_set(js, w->self, "byteLength", js_num((double)view_byte_len(w)));
    js_obj_set(js, w->self, "byteOffset", js_num((double)w->offset));
    js_obj_set(js, w->self, "BYTES_PER_ELEMENT",
               js_num(w->kind >= 0 ? k_ta[w->kind].bytes : 1));
    js_obj_set(js, w->self, "buffer", js_objval(w->buffer));
}

/* ---------------- indexed access ---------------- */

/**
 * @brief 写入一个 TypedArray 元素并刷新其影子属性 / Store one typed-array element and refresh its shadow property
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] w 视图描述 / The view descriptor
 * @param[in]     i 元素下标 / Element index
 * @param[in]     v 待写入的值 / The value to store
 *
 * @return 总是返回 1 / Always 1
 */
static int ta_set_index(HpeJs *js, TaView *w, int i, JsValue v)
{
    char key[24];
    unsigned char *p = w->fd->bytes + w->offset + (size_t)i * (size_t)k_ta[w->kind].bytes;
    ta_store(js, p, w->kind, v);
    snprintf(key, sizeof(key), "%d", i);
    js_obj_set(js, w->self, key, ta_load(js, p, w->kind));
    return 1;
}

/**
 * @brief 把起止参数归一化为 `[start, end)` / Normalise a start/end argument pair
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     a 实参数组 / Actual argument array
 * @param[in]     argc 实参个数 / Argument count
 * @param[in]     len 长度上界 / Length upper bound
 * @param[out]    s 接收起点 / Receives the start
 * @param[out]    e 接收终点 / Receives the end
 *
 * @note 负值从尾部计数，越界夹紧到 `[0, len]`；`end` 缺省为 `len` / Negative values count from the end and out-of-range values clamp to `[0, len]`; a missing end means `len`
 */
static void norm_range(HpeJs *js, JsValue *a, int argc, int len, int *s, int *e)
{
    int b = (argc > 0 && a[0].t != JS_UNDEF) ? (int)js_to_number(js, a[0]) : 0;
    int en = len;
    if (argc > 1 && a[1].t != JS_UNDEF) {
        double dv = js_to_number(js, a[1]);
        if (dv == dv)
            en = (int)dv;
    }
    if (b < 0)
        b += len;
    if (en < 0)
        en += len;
    if (b < 0)
        b = 0;
    if (en > len)
        en = len;
    if (en < b)
        en = b;
    *s = b;
    *e = en;
}

/* ---------------- host vtables ---------------- */

static const HpeJsHostVt g_abuf_vt;
static const HpeJsHostVt g_ta_vt;
static const HpeJsHostVt g_dv_vt;

/**
 * @brief 读取 ArrayBuffer 的元数据 / Read an ArrayBuffer's metadata
 *
 * @param[in,out] js JS 运行时上下文（未使用）/ JS runtime context (unused)
 * @param[in]     target 缓冲区盒子（即宿主指针）/ The buffer box, i.e. the host pointer
 * @param[in]     prop 属性名 / Property name
 * @param[in,out] out 接收属性值 / Receives the value
 *
 * @return 属性被处理返回 1 / 1 when the property was handled
 */
static int abuf_vt_get(HpeJs *js, void *target, const char *prop, JsValue *out)
{
    HpeBufBox *b = (HpeBufBox *)target;
    HPE_UNUSED(js);
    if (!b)
        return 0;
    if (!strcmp(prop, "byteLength")) {
        *out = js_num((double)b->fd->len);
        return 1;
    }
    return 0;
}

/**
 * @brief TypedArray 的索引取值钩子 / Indexed-read hook for a typed array
 *
 * @param[in,out] js JS 运行时上下文（未使用）/ JS runtime context (unused)
 * @param[in]     target 视图描述（即宿主指针）/ The view descriptor, i.e. the host pointer
 * @param[in]     prop 属性名 / Property name
 * @param[in,out] out 接收属性值 / Receives the value
 *
 * @return 属性被处理返回 1 / 1 when the property was handled
 *
 * @note 元数据（`length`、`byteOffset` 等）由影子属性提供，此处只负责数字下标；越界或已分离时返回 0 / Metadata such as `length` and `byteOffset` comes from the shadow properties, so only numeric indices are served here; an out-of-range or detached read yields 0
 */
static int ta_vt_get(HpeJs *js, void *target, const char *prop, JsValue *out)
{
    TaView *w = (TaView *)target;
    int i;
    HPE_UNUSED(js);
    if (!w || w->fd->detached || !index_of(prop, &i))
        return 0;
    if (i < 0 || (size_t)i >= w->length)
        return 0;
    *out = ta_load(js, w->fd->bytes + w->offset + (size_t)i * (size_t)k_ta[w->kind].bytes,
                   w->kind);
    return 1;
}

/**
 * @brief TypedArray 的索引写入钩子 / Indexed-write hook for a typed array
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     target 视图描述（即宿主指针）/ The view descriptor, i.e. the host pointer
 * @param[in]     prop 属性名 / Property name
 * @param[in]     v 待写入的值 / The value to store
 *
 * @return 属性名是数字下标返回 1 / 1 when the name was a numeric index
 */
static int ta_vt_set(HpeJs *js, void *target, const char *prop, JsValue v)
{
    TaView *w = (TaView *)target;
    int i;
    if (!w || w->fd->detached || !index_of(prop, &i))
        return 0;
    if (i < 0 || (size_t)i >= w->length)
        return 1;
    ta_set_index(js, w, i, v);
    return 1;
}

static const HpeJsHostVt g_abuf_vt = {abuf_vt_get, NULL};
static const HpeJsHostVt g_ta_vt = {ta_vt_get, ta_vt_set};
static const HpeJsHostVt g_dv_vt = {NULL, NULL};

/* ---------------- ArrayBuffer ---------------- */

static JsValue abuf_make(HpeJs *js, size_t n)
{
    JsObj *o = js_host_new(js, NULL, 0, &g_abuf_vt);
    HpeBufBox *b = (HpeBufBox *)hpe_js_alloc(js, sizeof(HpeBufBox));
    b->fd = buf_data_new(js, n);
    o->host_ptr = b;
    o->class_name = "ArrayBuffer";
    o->proto = js->arraybuffer_proto;
    return js_objval(o);
}

/**
 * @brief 校验并解出视图所引用的 ArrayBuffer / Validate and unwrap the ArrayBuffer a view refers to
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     v 候选 ArrayBuffer / The candidate ArrayBuffer
 * @param[out]    len 接收字节长度，可为 `NULL` / Receives the byte length, may be `NULL`
 *
 * @return 缓冲区盒子；不是 ArrayBuffer 时抛 `TypeError` / The buffer box, throwing `TypeError` when `v` is not an ArrayBuffer
 */
static HpeBufBox *abuf_require(HpeJs *js, JsValue v, size_t *len)
{
    HpeBufBox *b = abuf_box_of(v);
    if (!b) {
        b_throw(js, "First argument must be an ArrayBuffer");
        return NULL;
    }
    if (len)
        *len = b->fd->len;
    return b;
}

/**
 * @brief 创建视图对象 / Create a view object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     box 共享缓冲区 / The shared buffer
 * @param[in]     bufv ArrayBuffer 值 / The ArrayBuffer value
 * @param[in]     kind `TaKind`，DataView 传 -1 / `TaKind`, or -1 for a DataView
 * @param[in]     proto 视图原型 / The view prototype
 * @param[in]     off 字节偏移 / Byte offset
 * @param[in]     count 元素个数（DataView 为字节数）/ Element count, or byte count for a DataView
 *
 * @return 新的视图对象值 / The new view object
 */
static JsValue view_make(HpeJs *js, HpeBufBox *box, JsValue bufv, int kind, JsObj *proto,
                         size_t off, size_t count)
{
    JsObj *o = js_host_new(js, NULL, 0, kind < 0 ? &g_dv_vt : &g_ta_vt);
    TaView *w = (TaView *)hpe_js_alloc(js, sizeof(TaView));
    w->fd = box->fd;
    w->box = box;
    w->self = o;
    w->buffer = bufv.obj;
    w->kind = kind;
    w->offset = off;
    w->length = count;
    o->host_ptr = w;
    o->proto = proto;
    o->class_name = kind < 0 ? "DataView" : k_ta[kind].name;
    if (kind >= 0) {
        ta_sync_indices(js, w);
        ta_sync_meta(js, w);
    } else {
        js_obj_set(js, o, "byteLength", js_num((double)count));
        js_obj_set(js, o, "byteOffset", js_num((double)off));
        js_obj_set(js, o, "buffer", bufv);
    }
    return js_objval(o);
}

static JsValue ab_ctor(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    double n = js_to_number(js, b_arg(a, argc, 0));
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    if (n != n || n < 0)
        js_throw_value(js, js_make_error(js, "RangeError", "Invalid array buffer length"));
    return abuf_make(js, (size_t)n);
}

static JsValue ab_is_view(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsValue v = b_arg(a, argc, 0);
    HPE_UNUSED(js);
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    return js_bool(v.t == JS_OBJ && v.obj && (v.obj->vt == &g_ta_vt || v.obj->vt == &g_dv_vt));
}

static JsValue ab_slice(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HpeBufBox *b = abuf_box_of(thisv);
    int s, e;
    HPE_UNUSED(ud);
    if (!b) {
        b_throw(js, "ArrayBuffer.prototype.slice called on a non-ArrayBuffer");
        return js_undef();
    }
    norm_range(js, a, argc, (int)b->fd->len, &s, &e);
    {
        JsValue out = abuf_make(js, (size_t)(e - s));
        memcpy(((HpeBufBox *)out.obj->host_ptr)->fd->bytes, b->fd->bytes + s, (size_t)(e - s));
        return out;
    }
}

/**
 * @brief 刷新 TypedArray 的影子属性 / Refresh a typed array's shadow properties
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 接收者 / The receiver
 * @param[in,out] a 实参数组（未使用）/ Argument array (unused)
 * @param[in]     argc 实参个数（未使用）/ Argument count (unused)
 * @param[in,out] ud 用户数据（未使用）/ User data (unused)
 *
 * @return `undefined` / `undefined`
 *
 * @note 通过 `buffer` 或另一个视图写入的字节已经生效，本方法只把结果同步进属性表 / Bytes written through `buffer` or another view already took effect; this only mirrors them into the property table
 */
static JsValue ta_resync(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    TaView *w = ta_view_of(thisv);
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    if (w && !w->fd->detached)
        ta_sync_indices(js, w);
    return js_undef();
}

/* ---------------- typed arrays ---------------- */

/**
 * @brief 解析 TypedArray 构造实参并创建视图 / Resolve typed-array constructor arguments and create the view
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     kind 元素种类 / Element kind
 * @param[in,out] a 实参数组 / Argument array
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] proto 目标原型 / The target prototype
 *
 * @return 新的 TypedArray / The new typed array
 *
 * @note 首参为数字时按元素个数新建缓冲区；为 ArrayBuffer 时按偏移与长度建立共享视图；其余按可迭代对象或类数组填充 / A numeric first argument allocates a fresh buffer, an ArrayBuffer creates a shared view honouring offset and length, anything else is filled from an iterable or array-like
 */
static JsValue ta_build(HpeJs *js, int kind, JsValue *a, int argc, JsObj *proto)
{
    int esz = k_ta[kind].bytes;
    JsValue first = b_arg(a, argc, 0);
    size_t off = 0, count = 0;
    JsValue bufv;
    if (first.t != JS_OBJ || !first.obj) {
        double dn = (first.t == JS_UNDEF) ? 0.0 : js_to_number(js, first);
        if (dn != dn || dn < 0)
            js_throw_value(js, js_make_error(js, "RangeError", "Invalid typed array length"));
        count = (size_t)dn;
        bufv = abuf_make(js, count * (size_t)esz);
        return view_make(js, (HpeBufBox *)bufv.obj->host_ptr, bufv, kind, proto, 0, count);
    }
    if (abuf_box_of(first)) {
        size_t blen = 0;
        HpeBufBox *box = abuf_require(js, first, &blen);
        if (argc > 1 && a[1].t != JS_UNDEF)
            off = (size_t)js_to_number(js, a[1]);
        if (argc > 2 && a[2].t != JS_UNDEF)
            count = (size_t)js_to_number(js, a[2]);
        else
            count = off <= blen ? (blen - off) / (size_t)esz : 0;
        if (off + count * (size_t)esz > blen)
            js_throw_value(js, js_make_error(js, "RangeError", "Invalid typed array length"));
        return view_make(js, box, first, kind, proto, off, count);
    }
    {
        char **keys = NULL;
        int nk = 0, n, i;
        JsValue out;
        js_obj_own_keys(first.obj, &keys, &nk);
        free(keys);
        n = js_iter_len(js, first);
        if (n <= 0 && nk > 0)
            n = nk;
        count = (size_t)n;
        bufv = abuf_make(js, count * (size_t)esz);
        out = view_make(js, (HpeBufBox *)bufv.obj->host_ptr, bufv, kind, proto, 0, count);
        for (i = 0; i < n; i++) {
            TaView *w = (TaView *)out.obj->host_ptr;
            ta_store(js, w->fd->bytes + (size_t)i * (size_t)esz, kind, js_iter_at(js, first, i));
        }
        ta_sync_indices(js, (TaView *)out.obj->host_ptr);
        return out;
    }
}

static JsValue ta_ctor(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    TaCtor *c = (TaCtor *)ud;
    HPE_UNUSED(thisv);
    return ta_build(js, c->kind, a, argc, c->proto);
}

/**
 * @brief 构造 DataView / Construct a DataView
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] a 实参数组，`a[0]` 必须是 ArrayBuffer / Argument array; `a[0]` must be an ArrayBuffer
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 原型对象 / The prototype object
 *
 * @return 新的 DataView / The new DataView
 */
static JsValue dv_ctor(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    size_t blen = 0, off = 0, len;
    HpeBufBox *box = abuf_require(js, b_arg(a, argc, 0), &blen);
    HPE_UNUSED(thisv);
    if (!box)
        return js_undef();
    if (argc > 1 && a[1].t != JS_UNDEF)
        off = (size_t)js_to_number(js, a[1]);
    len = (argc > 2 && a[2].t != JS_UNDEF) ? (size_t)js_to_number(js, a[2]) : blen - off;
    if (off > blen || off + len > blen)
        js_throw_value(js, js_make_error(js, "RangeError", "Invalid DataView length"));
    return view_make(js, box, b_arg(a, argc, 0), -1, (JsObj *)ud, off, len);
}

/**
 * @brief 复制一段元素到新的 TypedArray / Copy a span of elements into a new typed array
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 接收者 / The receiver
 * @param[in,out] a `[start, end]` / `[start, end]`
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 用户数据（未使用）/ User data (unused)
 *
 * @return 装载副本的新 TypedArray / A new typed array holding the copy
 *
 * @note 与 `subarray` 不同，结果拥有自己的缓冲区，写入互不影响 / Unlike `subarray` the result owns its buffer, so the two never alias
 */
static JsValue ta_slice_m(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    TaView *w = ta_view_of(thisv);
    int s, e, i, esz;
    JsValue bufv, out;
    HPE_UNUSED(ud);
    if (!w)
        return js_undef();
    esz = k_ta[w->kind].bytes;
    norm_range(js, a, argc, (int)w->length, &s, &e);
    bufv = abuf_make(js, (size_t)(e - s) * (size_t)esz);
    out = view_make(js, (HpeBufBox *)bufv.obj->host_ptr, bufv, w->kind, thisv.obj->proto, 0,
                    (size_t)(e - s));
    for (i = s; i < e; i++) {
        TaView *d = (TaView *)out.obj->host_ptr;
        ta_store(js, d->fd->bytes + (size_t)(i - s) * (size_t)esz, w->kind,
                 ta_load(js, w->fd->bytes + w->offset + (size_t)i * (size_t)esz, w->kind));
    }
    ta_sync_indices(js, (TaView *)out.obj->host_ptr);
    return out;
}

static JsValue ta_subarray(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    TaView *w = ta_view_of(thisv);
    int s, e;
    HPE_UNUSED(ud);
    if (!w)
        return js_undef();
    norm_range(js, a, argc, (int)w->length, &s, &e);
    return view_make(js, w->box, js_objval(w->buffer), w->kind, thisv.obj->proto,
                     w->offset + (size_t)s * (size_t)k_ta[w->kind].bytes, (size_t)(e - s));
}

/**
 * @brief 在 TypedArray 内部复制元素 / Copy elements within a typed array
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 接收者 / The receiver
 * @param[in,out] a `[target, start, end]` / `[target, start, end]`
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 用户数据（未使用）/ User data (unused)
 *
 * @return `undefined` / `undefined`
 *
 * @note 单向复制；重叠区间按规范允许「就地覆写」的结果 / Copies forward only; an overlapping range gives the spec's in-place-overwrite result
 */
static JsValue ta_copy_within(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    TaView *w = ta_view_of(thisv);
    int len, from, to, i;
    HPE_UNUSED(ud);
    if (!w)
        return js_undef();
    len = (int)w->length;
    to = (int)js_to_number(js, b_arg(a, argc, 0));
    from = (int)js_to_number(js, b_arg(a, argc, 1));
    if (argc > 2 && a[2].t != JS_UNDEF)
        len = (int)js_to_number(js, a[2]);
    if (to < 0)
        to += (int)w->length;
    if (from < 0)
        from += (int)w->length;
    if (len < 0)
        len += (int)w->length;
    if (from < 0)
        from = 0;
    if (to < 0)
        to = 0;
    for (i = 0; i < len && from + i < (int)w->length && to + i < (int)w->length; i++) {
        JsValue v =
            ta_load(js, w->fd->bytes + w->offset + (size_t)(from + i) * (size_t)k_ta[w->kind].bytes,
                    w->kind);
        ta_store(js, w->fd->bytes + w->offset + (size_t)(to + i) * (size_t)k_ta[w->kind].bytes,
                 w->kind, v);
    }
    ta_sync_indices(js, w);
    return js_undef();
}

/* ---------------- DataView ---------------- */

static int dv_size_of(const char *name)
{
    if (!strcmp(name, "Int8"))
        return 1;
    if (!strcmp(name, "Uint8"))
        return 1;
    if (!strcmp(name, "Int16"))
        return 2;
    if (!strcmp(name, "Uint16"))
        return 2;
    if (!strcmp(name, "Int32"))
        return 4;
    if (!strcmp(name, "Uint32"))
        return 4;
    if (!strcmp(name, "Float32"))
        return 4;
    if (!strcmp(name, "Float64"))
        return 8;
    return 0;
}

/**
 * @brief 把字节串按 BigInt 解读 / Read bytes as a BigInt
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     p 源字节 / Source bytes
 * @param[in]     n 字节数 / Byte count
 * @param[in]     is_signed 有符号为 1 / 1 for a signed read
 *
 * @return 读出的 BigInt / The BigInt read
 */
static JsValue dv_load_big(HpeJs *js, const unsigned char *p, int n, int is_signed)
{
    char hex[17];
    int i;
    for (i = 0; i < n; i++)
        snprintf(hex + i * 2, 3, "%02x", p[n - 1 - i]);
    hex[n * 2] = '\0';
    {
        JsValue v = js_bigint_from_str(js, hex, 16);
        if (is_signed)
            v = js_bigint_asn(js, (double)(n * 8), v, 1);
        return v;
    }
}

/**
 * @brief 把 BigInt 写进字节串 / Write a BigInt into bytes
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] p 目标字节 / Target bytes
 * @param[in]     n 字节数 / Byte count
 * @param[in]     v 待写入的 BigInt / The BigInt to store
 */
static void dv_store_big(HpeJs *js, unsigned char *p, int n, JsValue v)
{
    JsValue w = js_bigint_asn(js, (double)(n * 8), v, 0);
    int neg = js_bigint_sign(w) < 0;
    int i;
    if (neg)
        w = js_bigint_neg(js, w);
    {
        char *hex = js_bigint_to_radix(js, w, 16);
        int len = (int)strlen(hex);
        unsigned int borrow = neg ? 1u : 0u;
        for (i = 0; i < n; i++) {
            int idx = len - 2 - 2 * i;
            int hi = idx >= 0 ? ta_hexval(hex[idx]) : 0;
            int lo = idx + 1 >= 0 ? ta_hexval(hex[idx + 1]) : 0;
            unsigned int byte = (unsigned int)((hi << 4) | lo);
            if (neg) {
                unsigned int t = (~byte & 0xFFu) + borrow;
                byte = t & 0xFFu;
                borrow = t >> 8;
            }
            p[i] = (unsigned char)byte;
        }
    }
}

/**
 * @brief 取得 DataView 上指定偏移的字节地址 / Resolve the byte address at an offset inside a DataView
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 接收者 / The receiver
 * @param[in]     idx 数据视图内的字节偏移 / Byte offset inside the view
 * @param[in]     width 访问宽度 / Access width
 *
 * @return 字节地址；越界时抛 `RangeError` 并返回 `NULL` / The byte address, or `NULL` after throwing `RangeError` when out of range
 */
static unsigned char *dv_at(HpeJs *js, JsValue thisv, double idx, int width)
{
    TaView *w = dv_view_of(thisv);
    if (!w) {
        b_throw(js, "DataView method called on an incompatible receiver");
        return NULL;
    }
    if (idx != idx || idx < 0 || idx + (double)width > (double)w->length) {
        js_throw_value(js, js_make_error(js, "RangeError", "Offset is outside the bounds"));
        return NULL;
    }
    return w->fd->bytes + w->offset + (size_t)idx;
}

static JsValue dv_get(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    const char *name = (const char *)ud;
    int width = dv_size_of(name);
    int is_signed = !strncmp(name, "Int", 3) || !strncmp(name, "BigInt", 6);
    unsigned char *p = dv_at(js, thisv, js_to_number(js, b_arg(a, argc, 0)), width);
    unsigned long long u = 0;
    int i;
    if (!p)
        return js_undef();
    if (!strncmp(name, "Big", 3))
        return dv_load_big(js, p, width, is_signed);
    if (!strncmp(name, "Float", 5))
        return js_num(ta_read_float(p, &k_ta[width == 4 ? TA_FLOAT32 : TA_FLOAT64]));
    for (i = width - 1; i >= 0; i--)
        u = (u << 8) | p[i];
    if (is_signed) {
        int bits = width * 8;
        if (bits < 32 && ((u >> (bits - 1)) & 1ull))
            return js_num((double)(long long)(u | ~((1ull << bits) - 1ull)));
        if (bits == 32)
            return js_num((double)(int)(unsigned int)u);
    }
    return js_num((double)u);
}

static JsValue dv_set(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    const char *name = (const char *)ud;
    int width = dv_size_of(name);
    unsigned char *p = dv_at(js, thisv, js_to_number(js, b_arg(a, argc, 0)), width);
    JsValue v = b_arg(a, argc, 1);
    int i;
    if (!p)
        return js_undef();
    if (strstr(name, "Big")) {
        if (v.t != JS_BIGINT) {
            b_throw(js, "Cannot convert a Number to a BigInt");
            return js_undef();
        }
        dv_store_big(js, p, width, v);
        return js_undef();
    }
    if (!strncmp(name, "Float", 5)) {
        ta_write_float(p, &k_ta[width == 4 ? TA_FLOAT32 : TA_FLOAT64], js_to_number(js, v));
        return js_undef();
    }
    {
        unsigned long long u = ta_int_store(&k_ta[width == 1 ? TA_UINT8 : (width == 2 ? TA_UINT16 : TA_UINT32)],
                                            js_to_number(js, v));
        for (i = 0; i < width; i++)
            p[i] = (unsigned char)((u >> (8 * i)) & 0xFF);
    }
    return js_undef();
}

/* ---------------- set / fill ---------------- */

/**
 * @brief 把源数据写进 TypedArray / Copy source data into a typed array
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 接收者 / The receiver
 * @param[in,out] a `[source, offset]` / `[source, offset]`
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 用户数据（未使用）/ User data (unused)
 *
 * @return `undefined` / `undefined`
 *
 * @note 源可以是另一个 TypedArray（逐元素）或普通类数组（逐下标）；越界抛 `RangeError` / The source may be another typed array (element-wise) or a plain array-like (by index); overrunning the end throws `RangeError`
 */
static JsValue ta_set_m(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    TaView *w = ta_view_of(thisv);
    JsValue src = b_arg(a, argc, 0);
    int off = (argc > 1 && a[1].t != JS_UNDEF) ? (int)js_to_number(js, a[1]) : 0;
    int n, i;
    HPE_UNUSED(ud);
    if (!w)
        return js_undef();
    n = js_iter_len(js, src);
    if (n <= 0 && src.t == JS_OBJ && src.obj)
        n = (int)js_to_number(js, js_obj_get(js, src.obj, "length"));
    if (off < 0 || (size_t)(off + n) > w->length) {
        js_throw_value(js, js_make_error(js, "RangeError", "Offset is outside the bounds"));
        return js_undef();
    }
    for (i = 0; i < n; i++)
        ta_store(js, w->fd->bytes + w->offset + (size_t)(off + i) * (size_t)k_ta[w->kind].bytes,
                 w->kind, js_iter_at(js, src, i));
    ta_sync_indices(js, w);
    return js_undef();
}

/**
 * @brief 用一个值填满 TypedArray 的一段 / Fill a span of a typed array with one value
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 接收者 / The receiver
 * @param[in,out] a `[value, start, end]` / `[value, start, end]`
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 用户数据（未使用）/ User data (unused)
 *
 * @return 接收者本身 / The receiver itself
 */
static JsValue ta_fill_m(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    TaView *w = ta_view_of(thisv);
    int s, e, i;
    HPE_UNUSED(ud);
    if (!w)
        return js_undef();
    norm_range(js, a ? a + 1 : NULL, argc > 0 ? argc - 1 : 0, (int)w->length, &s, &e);
    for (i = s; i < e; i++)
        ta_store(js, w->fd->bytes + w->offset + (size_t)i * (size_t)k_ta[w->kind].bytes,
                 w->kind, b_arg(a, argc, 0));
    ta_sync_indices(js, w);
    return thisv;
}

/* ---------------- installation ---------------- */

/**
 * @brief 在对象上定义一个原生方法 / Define a native method on an object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] target 目标对象 / Target object
 * @param[in]     name 方法名 / Method name
 * @param[in]     fn 原生实现 / Native implementation
 * @param[in,out] ud 透传的用户数据 / Opaque user data
 * @param[in]     len 形参个数 / Declared argument count
 */
static void b_def(HpeJs *js, JsObj *target, const char *name, JsNativeFn fn, void *ud, int len)
{
    JsObj *f = js_native_new(js, fn, ud);
    f->name = hpe_js_strdup(js, name);
    f->length_hint = len;
    js_obj_set(js, f, "name", js_string(js, name));
    js_obj_set(js, f, "length", js_num((double)len));
    js_obj_set(js, target, name, js_objval(f));
}

/**
 * @brief 让 TypedArray 继承数组的通用方法 / Give typed arrays the generic array methods
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] proto 目标原型 / The target prototype
 *
 * @note `join` 只能借用（需要固定的 `this` 语义），`forEach`/`map` 等按属性读取同样有效，因为元素已同步为影子属性 / `join` is the only one that must be borrowed (its `this` semantics are fixed); `forEach`/`map` and friends work as-is because the elements are synced into shadow properties
 */
static void ta_borrow_array_methods(HpeJs *js, JsObj *proto)
{
    static const char *const names[] = {"join", "indexOf", "lastIndexOf", "forEach",
                                        "map",   "filter",    "reduce",     "some",
                                        "every", "find",      "findIndex",  "includes"};
    size_t i;
    if (!js->array_proto)
        return;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        JsValue f = js_obj_get(js, js->array_proto, names[i]);
        if (f.t == JS_OBJ)
            js_obj_set(js, proto, names[i], f);
    }
}

static void ta_install_protos(HpeJs *js)
{
    b_def(js, js->arraybuffer_proto, "slice", ab_slice, NULL, 2);
    b_def(js, js->dataview_proto, "getInt8", dv_get, (void *)"Int8", 1);
    b_def(js, js->dataview_proto, "getUint8", dv_get, (void *)"Uint8", 1);
    b_def(js, js->dataview_proto, "getInt16", dv_get, (void *)"Int16", 1);
    b_def(js, js->dataview_proto, "getUint16", dv_get, (void *)"Uint16", 1);
    b_def(js, js->dataview_proto, "getInt32", dv_get, (void *)"Int32", 1);
    b_def(js, js->dataview_proto, "getUint32", dv_get, (void *)"Uint32", 1);
    b_def(js, js->dataview_proto, "getFloat32", dv_get, (void *)"Float32", 1);
    b_def(js, js->dataview_proto, "getFloat64", dv_get, (void *)"Float64", 1);
    b_def(js, js->dataview_proto, "getBigInt64", dv_get, (void *)"BigInt64", 1);
    b_def(js, js->dataview_proto, "getBigUint64", dv_get, (void *)"BigUint64", 1);
    b_def(js, js->dataview_proto, "setInt8", dv_set, (void *)"Int8", 2);
    b_def(js, js->dataview_proto, "setUint8", dv_set, (void *)"Uint8", 2);
    b_def(js, js->dataview_proto, "setInt16", dv_set, (void *)"Int16", 2);
    b_def(js, js->dataview_proto, "setUint16", dv_set, (void *)"Uint16", 2);
    b_def(js, js->dataview_proto, "setInt32", dv_set, (void *)"Int32", 2);
    b_def(js, js->dataview_proto, "setUint32", dv_set, (void *)"Uint32", 2);
    b_def(js, js->dataview_proto, "setFloat32", dv_set, (void *)"Float32", 2);
    b_def(js, js->dataview_proto, "setFloat64", dv_set, (void *)"Float64", 2);
    b_def(js, js->dataview_proto, "setBigInt64", dv_set, (void *)"BigInt64", 2);
    b_def(js, js->dataview_proto, "setBigUint64", dv_set, (void *)"BigUint64", 2);
    b_def(js, js->ta_proto_base, "slice", ta_slice_m, NULL, 2);
    b_def(js, js->ta_proto_base, "subarray", ta_subarray, NULL, 2);
    b_def(js, js->ta_proto_base, "copyWithin", ta_copy_within, NULL, 2);
    b_def(js, js->ta_proto_base, "set", ta_set_m, NULL, 2);
    b_def(js, js->ta_proto_base, "fill", ta_fill_m, NULL, 1);
    b_def(js, js->ta_proto_base, "resync", ta_resync, NULL, 0);
    ta_borrow_array_methods(js, js->ta_proto_base);
}

static void ta_install_ctors(HpeJs *js)
{
    JsObj *ab = js_native_new(js, ab_ctor, NULL);
    JsObj *dv = js_native_new(js, dv_ctor, js->dataview_proto);
    int k;
    ab->name = "ArrayBuffer";
    js_obj_set(js, ab, "name", js_string(js, "ArrayBuffer"));
    js_obj_set(js, ab, "length", js_num(1));
    js_obj_set(js, ab, "prototype", js_objval(js->arraybuffer_proto));
    js_obj_set(js, js->arraybuffer_proto, "constructor", js_objval(ab));
    b_def(js, ab, "isView", ab_is_view, NULL, 1);
    js_obj_set(js, js->global_obj, "ArrayBuffer", js_objval(ab));
    dv->name = "DataView";
    js_obj_set(js, dv, "name", js_string(js, "DataView"));
    js_obj_set(js, dv, "length", js_num(1));
    js_obj_set(js, dv, "prototype", js_objval(js->dataview_proto));
    js_obj_set(js, js->dataview_proto, "constructor", js_objval(dv));
    js_obj_set(js, js->global_obj, "DataView", js_objval(dv));
    for (k = 0; k < k_ta_count; k++) {
        TaCtor *c = (TaCtor *)hpe_js_alloc(js, sizeof(TaCtor));
        JsObj *proto = js_object_raw(js);
        JsObj *f;
        c->kind = k;
        c->proto = proto;
        proto->proto = js->ta_proto_base;
        proto->class_name = k_ta[k].name;
        f = js_native_new(js, ta_ctor, c);
        f->name = (char *)k_ta[k].name;
        js_obj_set(js, f, "name", js_string(js, k_ta[k].name));
        js_obj_set(js, f, "length", js_num(3));
        js_obj_set(js, f, "prototype", js_objval(proto));
        js_obj_set(js, f, "BYTES_PER_ELEMENT", js_num(k_ta[k].bytes));
        js_obj_set(js, proto, "constructor", js_objval(f));
        js_obj_set(js, proto, "BYTES_PER_ELEMENT", js_num(k_ta[k].bytes));
        js_obj_set(js, js->global_obj, k_ta[k].name, js_objval(f));
    }
}

void hpe_js_buffer_install(HpeJs *js)
{
    JsObj *abuf = js_object_raw(js);
    JsObj *ta_base = js_object_raw(js);
    JsObj *dv_proto = js_object_raw(js);
    js->arraybuffer_proto = abuf;
    abuf->proto = js->object_proto;
    ta_base->proto = js->object_proto;
    dv_proto->proto = js->object_proto;
    js->ta_proto_base = ta_base;
    js->dataview_proto = dv_proto;
    js->ta_proto = (JsObj **)hpe_js_alloc(js, sizeof(JsObj *) * (size_t)k_ta_count);
    ta_install_protos(js);
    ta_install_ctors(js);
}

int hpe_js_is_typed_array(JsValue v)
{
    return v.t == JS_OBJ && v.obj && v.obj->vt == &g_ta_vt;
}
