/* Hope UI Engine - Intl built-in (ES subset)
 *
 * Layout: `Intl` is a plain namespace object holding three constructors
 * (`NumberFormat`, `DateTimeFormat`, `Collator`). Every constructor returns an
 * ordinary object whose *resolved* options are stored as `_`-prefixed data
 * properties; the prototype methods read them back, so an instance is a plain
 * object and needs no extra JsObjKind.
 *
 * Scope: this is a formatting-focused subset, not a CLDR port. There is one
 * locale pattern set (en-US) and a handful of currency symbols. Locale tags are
 * accepted, validated for shape, and echoed back by `resolvedOptions()`.
 *
 * Two spec details are deliberately simplified and called out where they occur:
 *   - `format` is a method, not a getter returning a bound function, so
 *     `var f = nf.format; f(1)` throws instead of using the captured instance.
 *   - Only `timeZone: "UTC"` (and the default local == UTC of this engine) is
 *     honoured; `Date` has no timezone database (see design doc §7.4).
 */
#include "js/hpe_js_internal.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---------------- small helpers ---------------- */

/**
 * @brief 取第 i 个实参，越界返回 `undefined` / Fetch the i-th argument, yielding `undefined` out of range
 *
 * @param[in,out] a 实参数组 / Actual argument array
 * @param[in]     argc 实参个数 / Argument count
 * @param[in]     i 下标 / Index
 *
 * @return 第 i 个实参或 `undefined` / The argument or `undefined`
 */
static JsValue i_arg(JsValue *a, int argc, int i)
{
    return i < argc ? a[i] : js_undef();
}

/**
 * @brief 在对象上定义一个原生方法 / Define a native method on an object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] target 目标对象 / Target object
 * @param[in]     name 方法名 / Method name
 * @param[in]     fn 原生实现 / Native implementation
 * @param[in,out] ud 透传的用户数据 / Opaque user data
 */
static void i_def(HpeJs *js, JsObj *target, const char *name, JsNativeFn fn, void *ud)
{
    js_obj_set(js, target, name, js_objval(js_native_new(js, fn, ud)));
}

/**
 * @brief 在命名空间上定义一个构造器 / Define a constructor on a namespace object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] ns 命名空间对象（`Intl`）/ The namespace object (`Intl`)
 * @param[in]     name 构造器名 / Constructor name
 * @param[in]     fn 原生构造实现 / Native constructor body
 * @param[in,out] proto 该构造器的 `prototype` 对象，同时作为 `ud` 透传 / The constructor's `prototype`, also threaded through as `ud`
 *
 * @note 构造器把 `proto` 当作用户数据，以便实例化时接上原型链 / The constructor receives `proto` as user data so instances can be linked to it
 */
static void i_ctor(HpeJs *js, JsObj *ns, const char *name, JsNativeFn fn, JsObj *proto)
{
    JsObj *f = js_native_new(js, fn, proto);
    f->name = hpe_js_strdup(js, name);
    js_obj_set(js, f, "name", js_string(js, name));
    js_obj_set(js, f, "length", js_num(0));
    js_obj_set(js, f, "prototype", js_objval(proto));
    js_obj_set(js, proto, "constructor", js_objval(f));
    js_obj_set(js, ns, name, js_objval(f));
}

/**
 * @brief 创建一个挂在指定原型下的普通实例 / Create a plain instance under the given prototype
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] ud 构造时透传的原型对象 / The prototype threaded through construction
 * @param[in]     cls 类名 / Class name
 *
 * @return 新建的实例 / The new instance
 */
static JsObj *i_instance(HpeJs *js, void *ud, const char *cls)
{
    JsObj *o = js_object_raw(js);
    o->kind = JS_OBJ_PLAIN;
    o->class_name = cls;
    o->proto = (JsObj *)ud;
    return o;
}

/**
 * @brief 读一个选项属性 / Read one option property
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     opts 选项对象（可为非对象）/ The options value, possibly not an object
 * @param[in]     name 选项名 / Option name
 *
 * @return 选项值，缺失时为 `undefined` / The option value, or `undefined`
 */
static JsValue i_opt_get(HpeJs *js, JsValue opts, const char *name)
{
    if (opts.t != JS_OBJ || !opts.obj)
        return js_undef();
    return js_obj_get(js, opts.obj, name);
}

/**
 * @brief 读一个字符串选项 / Read a string option
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     opts 选项对象 / The options value
 * @param[in]     name 选项名 / Option name
 * @param[in]     dflt 缺失或类型不符时的回退值 / Fallback when missing or of the wrong type
 *
 * @return 选项字符串或回退值 / The option string or the fallback
 */
static const char *i_opt_str(HpeJs *js, JsValue opts, const char *name, const char *dflt)
{
    JsValue v = i_opt_get(js, opts, name);
    if (v.t != JS_STR || !v.str)
        return dflt;
    return v.str;
}

/**
 * @brief 读一个整数选项 / Read an integer option
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     opts 选项对象 / The options value
 * @param[in]     name 选项名 / Option name
 * @param[in]     dflt 缺失时的回退值 / Fallback when missing
 *
 * @return 选项数值或回退值 / The option number or the fallback
 */
static int i_opt_int(HpeJs *js, JsValue opts, const char *name, int dflt)
{
    JsValue v = i_opt_get(js, opts, name);
    if (v.t == JS_UNDEF || v.t == JS_NULL)
        return dflt;
    return (int)js_to_number(js, v);
}

/**
 * @brief 读一个布尔选项 / Read a boolean option
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     opts 选项对象 / The options value
 * @param[in]     name 选项名 / Option name
 * @param[in]     dflt 缺失时的回退值 / Fallback when missing
 *
 * @return 选项真值或回退值 / The option truthiness or the fallback
 */
static int i_opt_flag(HpeJs *js, JsValue opts, const char *name, int dflt)
{
    JsValue v = i_opt_get(js, opts, name);
    if (v.t == JS_UNDEF)
        return dflt;
    return js_truthy(v);
}

/**
 * @brief 追加格式化输出并在容量内安全截断 / Append formatted text, truncating safely at the capacity
 *
 * @param[in,out] buf 输出缓冲 / Output buffer
 * @param[in]     cap 缓冲容量 / Buffer capacity
 * @param[in]     len 已写入的字节数 / Bytes written so far
 * @param[in]     fmt `printf` 风格格式串 / A `printf`-style format string
 *
 * @return 追加后的长度 / The length after appending
 */
static int i_append(char *buf, size_t cap, int len, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (len < 0 || (size_t)len >= cap)
        return len < 0 ? 0 : (int)cap - 1;
    va_start(ap, fmt);
    n = vsnprintf(buf + len, cap - (size_t)len, fmt, ap);
    va_end(ap);
    if (n < 0)
        return len;
    return len + n;
}

/**
 * @brief 校验并规范化语言标签（仅做形状检查）/ Validate and normalise a language tag (shape check only)
 *
 * @param[in]     tag 输入的标签 / The input tag
 * @param[in,out] out 接收规范化结果 / Receives the normalised tag
 * @param[in]     cap 输出容量 / Output capacity
 *
 * @return 合法返回 1，否则返回 0 / 1 when well formed, otherwise 0
 *
 * @note 只接受 `字母 字母{1,7} ('-' 字母数字{1,8})*` 的形状；语言子标签转小写、其余保持原样 /
 *       Accepts `alpha alpha{1,7} ('-' alnum{1,8})*`; the language subtag is lowercased and the rest kept as-is
 */
static int i_locale_tag(const char *tag, char *out, size_t cap)
{
    size_t i = 0, o = 0;
    int alnum;
    if (!tag || !*tag)
        return 0;
    while (tag[i] && tag[i] != '-' && tag[i] != '_') {
        if (!((tag[i] >= 'a' && tag[i] <= 'z') || (tag[i] >= 'A' && tag[i] <= 'Z')))
            return 0;
        if (o + 1 >= cap)
            return 0;
        out[o++] = (char)((tag[i] >= 'A' && tag[i] <= 'Z') ? tag[i] - 'A' + 'a' : tag[i]);
        i++;
    }
    if (o < 2 || o > 8)
        return 0;
    while (tag[i]) {
        if (tag[i] != '-' && tag[i] != '_')
            return 0;
        i++;
        alnum = 0;
        if (o + 1 >= cap)
            return 0;
        out[o++] = '-';
        while (tag[i] && tag[i] != '-' && tag[i] != '_' && alnum < 8) {
            if (o + 1 >= cap)
                return 0;
            out[o++] = tag[i++];
            alnum++;
        }
        if (alnum == 0)
            return 0;
    }
    out[o] = '\0';
    return 1;
}

/**
 * @brief 取第一个语言标签 / Take the first language tag
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     v `locales` 实参（字符串或数组）/ The `locales` argument, a string or an array
 * @param[in,out] out 接收标签 / Receives the tag
 * @param[in]     cap 输出容量 / Output capacity
 *
 * @return 成功返回 1，未给出或形状非法返回 0 / 1 on success, 0 when absent or malformed
 */
static int i_locale_arg(HpeJs *js, JsValue v, char *out, size_t cap)
{
    if (v.t == JS_STR && v.str)
        return i_locale_tag(v.str, out, cap);
    if (v.t == JS_OBJ && v.obj) {
        JsValue first = js_obj_get(js, v.obj, "0");
        if (first.t == JS_STR && first.str)
            return i_locale_tag(first.str, out, cap);
    }
    return 0;
}

/**
 * @brief 把 `locales` 实参解析成一个标签值 / Resolve the `locales` argument into a tag value
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     locs `locales` 实参 / The `locales` argument
 *
 * @return 规范化后的标签；缺省或形状非法时为 `"en-US"` / The normalised tag, or `"en-US"` when absent or malformed
 *
 * @note 引擎只带一套 en-US 模式，因此这里只回显请求的标签 / The engine carries a single en-US pattern set, so the tag is echoed back
 */
static JsValue i_locale_value(HpeJs *js, JsValue locs)
{
    char tag[48];
    if (i_locale_arg(js, locs, tag, sizeof(tag)))
        return js_string(js, tag);
    return js_string(js, "en-US");
}

/**
 * @brief 读回格式化器实例上的 locale / Read the locale back off a formatter instance
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] o 格式化器实例，可为 NULL / The formatter instance, possibly `NULL`
 *
 * @return 标签字符串，缺失时为 `"en-US"` / The tag, defaulting to `"en-US"`
 */
static JsValue i_loc_out(HpeJs *js, JsObj *o)
{
    JsValue v = o ? js_obj_get(js, o, "_loc") : js_undef();
    if (v.t != JS_STR || !v.str)
        return js_string(js, "en-US");
    return v;
}

/* ---------------- NumberFormat ---------------- */

/**
 * @brief 把三位分组符插入整数部分 / Insert the thousands separator into an integer part
 *
 * @param[in]     ip 整数部分文本（不含符号）/ Integer-part text without a sign
 * @param[in,out] out 接收分组结果 / Receives the grouped text
 * @param[in]     cap 输出容量 / Output capacity
 */
static void nf_group(const char *ip, char *out, size_t cap)
{
    int ilen = (int)strlen(ip), i, o = 0;
    for (i = 0; i < ilen; i++) {
        if (i > 0 && (ilen - i) % 3 == 0 && (size_t)o + 2 < cap)
            out[o++] = ',';
        if ((size_t)o + 1 < cap)
            out[o++] = ip[i];
    }
    out[o] = '\0';
}

/**
 * @brief 把非负数取整并拆成整数部分与小数部分文本 / Round a non-negative value and split it into integer and fraction text
 *
 * @param[in]     av 非负数值 / The non-negative value
 * @param[in]     minf 最少小数位 / Minimum fraction digits
 * @param[in]     maxf 最多小数位 / Maximum fraction digits
 * @param[in,out] ip 接收整数部分 / Receives the integer part
 * @param[in]     icap `ip` 容量 / Capacity of `ip`
 * @param[in,out] fp 接收小数部分（无多余尾零，可为空串）/ Receives the fraction, without redundant trailing zeros; may be empty
 * @param[in]     fcap `fp` 容量 / Capacity of `fp`
 *
 * @note 小数位先用 `%.*f` 四舍五入，再自尾部去零直到 `minf` 位 / The value is first rounded with `%.*f`, then trailing zeros are stripped down to `minf`
 */
static void nf_parts(double av, int minf, int maxf, char *ip, size_t icap, char *fp, size_t fcap)
{
    char raw[96];
    char *dot;
    int flen;
    if (maxf < 0)
        maxf = 0;
    if (maxf > 20)
        maxf = 20;
    if (minf < 0)
        minf = 0;
    if (minf > maxf)
        minf = maxf;
    snprintf(raw, sizeof(raw), "%.*f", maxf, av);
    dot = strchr(raw, '.');
    if (!dot) {
        snprintf(ip, icap, "%s", raw);
        fp[0] = '\0';
        return;
    }
    *dot = '\0';
    snprintf(ip, icap, "%s", raw);
    flen = (int)strlen(dot + 1);
    while (flen > minf && dot[flen] == '0')
        dot[flen--] = '\0';
    snprintf(fp, fcap, "%s", dot + 1);
}

/**
 * @brief 常见货币代码到符号的映射 / Map the common currency codes to their symbols
 *
 * @param[in] code 三位货币代码 / Three-letter currency code
 *
 * @return 货币符号；未知代码原样返回 / The symbol, or the code itself when unknown
 */
static const char *nf_currency_symbol(const char *code)
{
    static const struct {
        const char *code;
        const char *sym;
    } tbl[] = {
        { "USD", "$" },  { "EUR", "\xE2\x82\xAC" }, { "GBP", "\xC2\xA3" },
        { "JPY", "\xC2\xA5" }, { "CNY", "\xC2\xA5" }, { "KRW", "\xE2\x82\xA9" },
        { "RUB", "\xE2\x82\xBD" }, { "INR", "\xE2\x82\xB9" }, { "BRL", "R$" },
        { "AUD", "A$" }, { "CAD", "CA$" }, { "HKD", "HK$" }, { "CHF", "CHF " },
    };
    size_t i;
    for (i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++)
        if (!strcmp(tbl[i].code, code))
            return tbl[i].sym;
    return code;
}

/**
 * @brief `new Intl.NumberFormat(locales, options)` / `new Intl.NumberFormat(locales, options)`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 忽略 / Ignored
 * @param[in,out] a 实参数组 / Argument array
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 构造时透传的原型对象 / The prototype threaded through construction
 *
 * @return 新建的格式化器 / The new formatter
 *
 * @note 解析 `style` / `currency` / `minimumFractionDigits` / `maximumFractionDigits` / `useGrouping`；
 *       币种默认两位小数，百分比默认零位 / Resolves `style` / `currency` / `minimumFractionDigits` /
 *       `maximumFractionDigits` / `useGrouping`; currency defaults to two fraction digits and percent to zero
 */
static JsValue native_numberformat_ctor(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsValue opts = i_arg(a, argc, 1);
    const char *style = i_opt_str(js, opts, "style", "decimal");
    const char *code = i_opt_str(js, opts, "currency", "USD");
    int is_cur = !strcmp(style, "currency");
    int is_pct = !strcmp(style, "percent");
    int minf = i_opt_int(js, opts, "minimumFractionDigits", is_cur ? 2 : 0);
    int maxf = i_opt_int(js, opts, "maximumFractionDigits", is_cur ? 2 : (is_pct ? 0 : 3));
    JsObj *o = i_instance(js, ud, "Intl.NumberFormat");
    HPE_UNUSED(thisv);
    if (maxf < minf)
        maxf = minf;
    js_obj_set(js, o, "_loc", i_locale_value(js, i_arg(a, argc, 0)));
    js_obj_set(js, o, "_minf", js_num(minf));
    js_obj_set(js, o, "_maxf", js_num(maxf));
    js_obj_set(js, o, "_group", js_num(i_opt_flag(js, opts, "useGrouping", 1)));
    js_obj_set(js, o, "_pct", js_num(is_pct));
    js_obj_set(js, o, "_cur", js_num(is_cur));
    js_obj_set(js, o, "_code", js_string(js, code));
    js_obj_set(js, o, "_sym", js_string(js, is_cur ? nf_currency_symbol(code) : ""));
    return js_objval(o);
}

/**
 * @brief 取格式化器的数值选项 / Read a numeric option back off a formatter
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] o 格式化器实例 / The formatter instance
 * @param[in]     name 选项属性名 / The option property name
 *
 * @return 该选项的数值 / The option as a number
 */
static int nf_get_int(HpeJs *js, JsObj *o, const char *name)
{
    return (int)js_to_number(js, js_obj_get(js, o, name));
}

/**
 * @brief `Intl.NumberFormat.prototype.format(n)` / `Intl.NumberFormat.prototype.format(n)`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 格式化器实例 / The formatter instance
 * @param[in,out] a 实参数组，`a[0]` 为待格式化数值 / Argument array; `a[0]` is the value to format
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 本地化后的字符串 / The localised string
 *
 * @note 百分数先乘 100 再加 `%`；货币符号恒为前缀；`NaN`/`±Infinity` 走各自的短形式 / Percent multiplies by
 *       100 before appending `%`; the currency symbol is always a prefix; `NaN`/`±Infinity` use their short forms
 */
static JsValue nf_format(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    char ip[64], fp[64], grp[96], out[192];
    const char *sym;
    JsObj *o;
    double v;
    int minf, maxf, pct, len = 0;
    HPE_UNUSED(ud);
    if (thisv.t != JS_OBJ || !thisv.obj)
        js_throw_value(
            js, js_make_error(js, "TypeError", "Intl.NumberFormat.format called on incompatible receiver"));
    o = thisv.obj;
    v = js_to_number(js, i_arg(a, argc, 0));
    minf = nf_get_int(js, o, "_minf");
    maxf = nf_get_int(js, o, "_maxf");
    pct = nf_get_int(js, o, "_pct");
    sym = js_to_string(js, js_obj_get(js, o, "_sym"));
    if (v != v)
        return js_string(js, "NaN");
    if (pct)
        v *= 100.0;
    if (v < 0)
        len = i_append(out, sizeof(out), 0, "-");
    nf_parts(v < 0 ? -v : v, minf, maxf, ip, sizeof(ip), fp, sizeof(fp));
    if (nf_get_int(js, o, "_group"))
        nf_group(ip, grp, sizeof(grp));
    else
        snprintf(grp, sizeof(grp), "%s", ip);
    if (isinf(v))
        len = i_append(out, sizeof(out), len, "%s\xE2\x88\x9E", sym);
    else
        len = i_append(out, sizeof(out), len, "%s%s%s%s", sym, grp, fp[0] ? "." : "", fp);
    if (pct)
        i_append(out, sizeof(out), len, "%%");
    return js_string(js, out);
}

/**
 * @brief `Intl.NumberFormat.prototype.resolvedOptions()` / `Intl.NumberFormat.prototype.resolvedOptions()`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 格式化器实例 / The formatter instance
 * @param[in,out] a 未使用 / Unused
 * @param[in]     argc 未使用 / Unused
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 描述已解析选项的对象 / An object describing the resolved options
 */
static JsValue nf_resolved(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *r = js_object_new(js);
    JsObj *o = thisv.obj;
    const char *style = "decimal";
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    if (!o)
        return js_objval(r);
    if (nf_get_int(js, o, "_cur"))
        style = "currency";
    else if (nf_get_int(js, o, "_pct"))
        style = "percent";
    js_obj_set(js, r, "locale", i_loc_out(js, o));
    js_obj_set(js, r, "numberingSystem", js_string(js, "latn"));
    js_obj_set(js, r, "style", js_string(js, style));
    if (!strcmp(style, "currency"))
        js_obj_set(js, r, "currency", js_obj_get(js, o, "_code"));
    js_obj_set(js, r, "minimumIntegerDigits", js_num(1));
    js_obj_set(js, r, "minimumFractionDigits", js_obj_get(js, o, "_minf"));
    js_obj_set(js, r, "maximumFractionDigits", js_obj_get(js, o, "_maxf"));
    js_obj_set(js, r, "useGrouping", js_bool(nf_get_int(js, o, "_group")));
    return js_objval(r);
}

/* ---------------- DateTimeFormat ---------------- */

/* style codes shared by every component option: 0 absent, 1 numeric,
 * 2 two-digit, 3 short, 4 long, 5 narrow */
#define DT_NONE 0
#define DT_NUM 1
#define DT_2DIG 2
#define DT_SHORT 3
#define DT_LONG 4
#define DT_NARROW 5

/**
 * @brief 把组件选项的取值映射成样式码 / Map a component-option value onto a style code
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     opts 选项对象 / The options value
 * @param[in]     name 选项名 / Option name
 *
 * @return 样式码；缺失时返回 `DT_NONE` / The style code, `DT_NONE` when absent
 */
static int dt_style(HpeJs *js, JsValue opts, const char *name)
{
    JsValue v = i_opt_get(js, opts, name);
    if (v.t != JS_STR || !v.str)
        return DT_NONE;
    if (!strcmp(v.str, "2-digit"))
        return DT_2DIG;
    if (!strcmp(v.str, "short"))
        return DT_SHORT;
    if (!strcmp(v.str, "long"))
        return DT_LONG;
    if (!strcmp(v.str, "narrow"))
        return DT_NARROW;
    return DT_NUM;
}

/**
 * @brief 应用 `dateStyle` / `timeStyle` 快捷选项 / Apply the `dateStyle` / `timeStyle` shorthands
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     opts 选项对象 / The options value
 * @param[in,out] o 目标格式化器实例 / The formatter instance being built
 *
 * @note 只在对应的粒度尚未显式给出时生效 / Only fills in components that were not set explicitly
 */
static void dt_apply_styles(HpeJs *js, JsValue opts, JsObj *o)
{
    const char *ds = i_opt_str(js, opts, "dateStyle", NULL);
    const char *ts = i_opt_str(js, opts, "timeStyle", NULL);
    if (ds && !js_truthy(js_obj_get(js, o, "_y"))) {
        int full = !strcmp(ds, "full"), lng = full || !strcmp(ds, "long");
        js_obj_set(js, o, "_wd", js_num(full ? DT_LONG : (lng ? DT_SHORT : DT_NONE)));
        js_obj_set(js, o, "_y", js_num(lng ? DT_NUM : (strcmp(ds, "short") ? DT_NUM : DT_2DIG)));
        js_obj_set(js, o, "_mo", js_num(lng ? DT_LONG : (strcmp(ds, "short") ? DT_SHORT : DT_NUM)));
        js_obj_set(js, o, "_d", js_num(DT_NUM));
    }
    if (ts && !js_truthy(js_obj_get(js, o, "_h"))) {
        js_obj_set(js, o, "_h", js_num(DT_NUM));
        js_obj_set(js, o, "_mi", js_num(DT_2DIG));
        js_obj_set(js, o, "_se", js_num(strcmp(ts, "short") ? DT_2DIG : DT_NONE));
    }
}

/**
 * @brief `new Intl.DateTimeFormat(locales, options)` / `new Intl.DateTimeFormat(locales, options)`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 忽略 / Ignored
 * @param[in,out] a 实参数组 / Argument array
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 构造时透传的原型对象 / The prototype threaded through construction
 *
 * @return 新建的格式化器 / The new formatter
 *
 * @note 未给出任何日期/时间组件时按规范回退到「数值年 + 数值月 + 数值日」 / With no component options at all
 *       this falls back to numeric year + month + day, as the spec requires
 */
static JsValue native_datetime_ctor(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsValue opts = i_arg(a, argc, 1);
    JsObj *o = i_instance(js, ud, "Intl.DateTimeFormat");
    const char *tz = i_opt_str(js, opts, "timeZone", "UTC");
    HPE_UNUSED(thisv);
    js_obj_set(js, o, "_loc", i_locale_value(js, i_arg(a, argc, 0)));
    js_obj_set(js, o, "_tz", js_string(js, tz));
    js_obj_set(js, o, "_wd", js_num(dt_style(js, opts, "weekday")));
    js_obj_set(js, o, "_y", js_num(dt_style(js, opts, "year")));
    js_obj_set(js, o, "_mo", js_num(dt_style(js, opts, "month")));
    js_obj_set(js, o, "_d", js_num(dt_style(js, opts, "day")));
    js_obj_set(js, o, "_h", js_num(dt_style(js, opts, "hour")));
    js_obj_set(js, o, "_mi", js_num(dt_style(js, opts, "minute")));
    js_obj_set(js, o, "_se", js_num(dt_style(js, opts, "second")));
    dt_apply_styles(js, opts, o);
    if (!nf_get_int(js, o, "_y") && !nf_get_int(js, o, "_mo") && !nf_get_int(js, o, "_d") &&
        !nf_get_int(js, o, "_h") && !nf_get_int(js, o, "_mi") && !nf_get_int(js, o, "_se") &&
        !nf_get_int(js, o, "_wd")) {
        js_obj_set(js, o, "_y", js_num(DT_NUM));
        js_obj_set(js, o, "_mo", js_num(DT_NUM));
        js_obj_set(js, o, "_d", js_num(DT_NUM));
    }
    js_obj_set(js, o, "_h12", js_num(i_opt_flag(js, opts, "hour12", 1)));
    return js_objval(o);
}

/**
 * @brief 渲染两位或非补零的数值字段 / Render a numeric field, zero padded or not
 *
 * @param[in,out] buf 输出缓冲 / Output buffer
 * @param[in]     cap 缓冲容量 / Buffer capacity
 * @param[in]     len 已写入长度 / Bytes written so far
 * @param[in]     v 字段值 / The field value
 * @param[in]     style 字段样式码 / The field style code
 *
 * @return 追加后的长度 / The length after appending
 */
static int dt_num(char *buf, size_t cap, int len, int v, int style)
{
    if (style == DT_2DIG)
        return i_append(buf, cap, len, "%02d", v);
    return i_append(buf, cap, len, "%d", v);
}

/**
 * @brief 渲染日期部分 / Render the date portion
 *
 * @param[in]     f 已拆解的日期字段 / The split date fields
 * @param[in,out] o 格式化器实例（提供样式码）/ The formatter instance carrying the style codes
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] buf 输出缓冲 / Output buffer
 * @param[in]     cap 缓冲容量 / Buffer capacity
 * @param[in]     len 已写入长度 / Bytes written so far
 *
 * @return 追加后的长度 / The length after appending
 *
 * @note 采用 en-US 顺序：星期、月、日、年；月为 `short`/`long`/`narrow` 时输出月名并以 `月 日, 年` 组织 /
 *       Uses the en-US order (weekday, month, day, year); with a named month the shape becomes `Month D, YYYY`
 */
static int dt_date_part(const HpeDateFields *f, JsObj *o, HpeJs *js, char *buf, size_t cap, int len)
{
    static const char *const mon_long[12] = { "January", "February", "March",     "April",
                                              "May",     "June",     "July",      "August",
                                              "September", "October", "November", "December" };
    static const char *const mon_short[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                               "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    static const char *const wd_long[7] = { "Sunday",   "Monday", "Tuesday", "Wednesday",
                                            "Thursday", "Friday", "Saturday" };
    static const char *const wd_short[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    int wd = nf_get_int(js, o, "_wd");
    int y = nf_get_int(js, o, "_y");
    int mo = nf_get_int(js, o, "_mo");
    int d = nf_get_int(js, o, "_d");
    if (wd)
        len = i_append(buf, cap, len, "%s, ", wd >= DT_SHORT ? wd_long[f->weekday] : wd_short[f->weekday]);
    if (mo >= DT_SHORT) {
        len = i_append(buf, cap, len, "%s %d", mo == DT_LONG ? mon_long[f->month - 1] : mon_short[f->month - 1], f->day);
    } else if (d || y) {
        if (mo)
            len = dt_num(buf, cap, len, f->month, mo);
        if (d) {
            if (mo)
                len = i_append(buf, cap, len, "/");
            len = dt_num(buf, cap, len, f->day, d);
        }
    }
    if (y) {
        if (mo >= DT_SHORT)
            len = i_append(buf, cap, len, ", ");
        else if (mo || d)
            len = i_append(buf, cap, len, "/");
        len = dt_num(buf, cap, len, y == DT_2DIG ? f->year % 100 : f->year, y);
    }
    return len;
}

/**
 * @brief 渲染时间部分 / Render the time portion
 *
 * @param[in]     f 已拆解的日期字段 / The split date fields
 * @param[in,out] o 格式化器实例（提供样式码与 `hour12`）/ The formatter instance carrying the style codes and `hour12`
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] buf 输出缓冲 / Output buffer
 * @param[in]     cap 缓冲容量 / Buffer capacity
 * @param[in]     len 已写入长度 / Bytes written so far
 *
 * @return 追加后的长度 / The length after appending
 */
static int dt_time_part(const HpeDateFields *f, JsObj *o, HpeJs *js, char *buf, size_t cap, int len)
{
    int h = nf_get_int(js, o, "_h");
    int mi = nf_get_int(js, o, "_mi");
    int se = nf_get_int(js, o, "_se");
    int hour = f->hour;
    if (nf_get_int(js, o, "_h12")) {
        const char *ap = hour < 12 ? "AM" : "PM";
        hour = hour % 12;
        if (hour == 0)
            hour = 12;
        len = dt_num(buf, cap, len, hour, h);
        if (mi)
            len = i_append(buf, cap, len, ":%02d", f->minute);
        if (se)
            len = i_append(buf, cap, len, ":%02d", f->second);
        return i_append(buf, cap, len, " %s", ap);
    }
    len = dt_num(buf, cap, len, hour, h);
    if (mi)
        len = i_append(buf, cap, len, ":%02d", f->minute);
    if (se)
        len = i_append(buf, cap, len, ":%02d", f->second);
    return len;
}

/**
 * @brief `Intl.DateTimeFormat.prototype.format(date)` / `Intl.DateTimeFormat.prototype.format(date)`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 格式化器实例 / The formatter instance
 * @param[in,out] a 实参数组，`a[0]` 为 Date 或毫秒值 / Argument array; `a[0]` is a Date or a millisecond value
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 本地化后的日期时间字符串 / The localised date-time string
 *
 * @note 只有日期部分与时间部分都存在时才插入 `, ` 分隔 / The `, ` separator is only inserted when both a date and a
 *       time portion are present
 */
static JsValue dt_format(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    char buf[192];
    HpeDateFields f;
    JsValue arg = i_arg(a, argc, 0);
    JsObj *o;
    int len = 0;
    HPE_UNUSED(ud);
    if (thisv.t != JS_OBJ || !thisv.obj)
        js_throw_value(js, js_make_error(js, "TypeError",
                                         "Intl.DateTimeFormat.format called on incompatible receiver"));
    o = thisv.obj;
    if (arg.t != JS_OBJ || !arg.obj || !hpe_js_date_fields(arg, &f)) {
        double ms = js_to_number(js, arg);
        JsObj *d = js_object_raw(js);
        d->kind = JS_OBJ_DATE;
        d->num_val = ms;
        if (arg.t != JS_OBJ && !hpe_js_date_fields(js_objval(d), &f))
            return js_string(js, "Invalid Date");
        if (arg.t == JS_OBJ)
            return js_string(js, "Invalid Date");
    }
    buf[0] = '\0';
    if (nf_get_int(js, o, "_wd") || nf_get_int(js, o, "_y") || nf_get_int(js, o, "_mo") ||
        nf_get_int(js, o, "_d"))
        len = dt_date_part(&f, o, js, buf, sizeof(buf), len);
    if (nf_get_int(js, o, "_h") || nf_get_int(js, o, "_mi") || nf_get_int(js, o, "_se")) {
        if (len > 0)
            len = i_append(buf, sizeof(buf), len, ", ");
        len = dt_time_part(&f, o, js, buf, sizeof(buf), len);
    }
    return js_string(js, buf);
}

/**
 * @brief `Intl.DateTimeFormat.prototype.resolvedOptions()` / `Intl.DateTimeFormat.prototype.resolvedOptions()`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 格式化器实例 / The formatter instance
 * @param[in,out] a 未使用 / Unused
 * @param[in]     argc 未使用 / Unused
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 描述已解析选项的对象 / An object describing the resolved options
 */
static JsValue dt_resolved(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *r = js_object_new(js);
    JsObj *o = thisv.obj;
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    js_obj_set(js, r, "locale", i_loc_out(js, o));
    js_obj_set(js, r, "calendar", js_string(js, "gregory"));
    js_obj_set(js, r, "numberingSystem", js_string(js, "latn"));
    js_obj_set(js, r, "timeZone", o ? js_obj_get(js, o, "_tz") : js_string(js, "UTC"));
    js_obj_set(js, r, "hour12", js_bool(nf_get_int(js, o, "_h12")));
    return js_objval(r);
}

/* ---------------- Collator ---------------- */

/**
 * @brief 按 locale 敏感度比较两个字符串 / Compare two strings at the configured collation sensitivity
 *
 * @param[in]     x 左操作数 / Left operand
 * @param[in]     y 右操作数 / Right operand
 * @param[in]     sens 敏感度：0 = base，1 = accent，2 = variant / Sensitivity: 0 = base, 1 = accent, 2 = variant
 *
 * @return 负数 / 0 / 正数 / Negative, zero or positive
 *
 * @note `base` 忽略大小写，`accent` 与 `variant` 在这里等价于按码元比较——引擎没有重音折叠表 / `base` folds case;
 *       `accent` and `variant` compare code units, because the engine carries no accent-folding table
 */
static int co_compare_str(const char *x, const char *y, int sens)
{
    if (sens > 0)
        return strcmp(x, y);
    while (*x && *y) {
        unsigned char a = (unsigned char)*x, b = (unsigned char)*y;
        if (a >= 'A' && a <= 'Z')
            a = (unsigned char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z')
            b = (unsigned char)(b - 'A' + 'a');
        if (a != b)
            return a < b ? -1 : 1;
        x++;
        y++;
    }
    if (*x == *y)
        return 0;
    return *x ? 1 : -1;
}

/**
 * @brief `new Intl.Collator(locales, options)` / `new Intl.Collator(locales, options)`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 忽略 / Ignored
 * @param[in,out] a 实参数组 / Argument array
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 构造时透传的原型对象 / The prototype threaded through construction
 *
 * @return 新建的比较器 / The new collator
 *
 * @note 解析 `sensitivity`（默认 `variant`）、`numeric` 与 `ignorePunctuation`；后两者仅记录在
 *       `resolvedOptions()` 中 / Resolves `sensitivity` (default `variant`), `numeric` and `ignorePunctuation`;
 *       the latter two are only echoed by `resolvedOptions()`
 */
static JsValue native_collator_ctor(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsValue opts = i_arg(a, argc, 1);
    const char *sens = i_opt_str(js, opts, "sensitivity", "variant");
    JsObj *o = i_instance(js, ud, "Intl.Collator");
    HPE_UNUSED(thisv);
    js_obj_set(js, o, "_loc", i_locale_value(js, i_arg(a, argc, 0)));
    js_obj_set(js, o, "_sens", js_string(js, sens));
    js_obj_set(js, o, "_sensc", js_num(!strcmp(sens, "base") ? 0 : (!strcmp(sens, "accent") ? 1 : 2)));
    js_obj_set(js, o, "_num", js_num(i_opt_flag(js, opts, "numeric", 0)));
    js_obj_set(js, o, "_punc", js_num(i_opt_flag(js, opts, "ignorePunctuation", 0)));
    return js_objval(o);
}

/**
 * @brief `Intl.Collator.prototype.compare(a, b)` / `Intl.Collator.prototype.compare(a, b)`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 比较器实例 / The collator instance
 * @param[in,out] a 实参数组，`a[0]`/`a[1]` 为待比较字符串 / Argument array; `a[0]` and `a[1]` are the operands
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 负数、0 或正数 / A negative number, zero or a positive number
 */
static JsValue co_compare(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    const char *x = js_to_string(js, i_arg(a, argc, 0));
    const char *y = js_to_string(js, i_arg(a, argc, 1));
    int r;
    HPE_UNUSED(ud);
    if (thisv.t != JS_OBJ || !thisv.obj)
        js_throw_value(js, js_make_error(js, "TypeError",
                                         "Intl.Collator.compare called on incompatible receiver"));
    r = co_compare_str(x, y, nf_get_int(js, thisv.obj, "_sensc"));
    return js_num(r < 0 ? -1.0 : (r > 0 ? 1.0 : 0.0));
}

/**
 * @brief `Intl.Collator.prototype.resolvedOptions()` / `Intl.Collator.prototype.resolvedOptions()`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 比较器实例 / The collator instance
 * @param[in,out] a 未使用 / Unused
 * @param[in]     argc 未使用 / Unused
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 描述已解析选项的对象 / An object describing the resolved options
 */
static JsValue co_resolved(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *r = js_object_new(js);
    JsObj *o = thisv.obj;
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    js_obj_set(js, r, "locale", i_loc_out(js, o));
    js_obj_set(js, r, "usage", js_string(js, "sort"));
    js_obj_set(js, r, "sensitivity", o ? js_obj_get(js, o, "_sens") : js_string(js, "variant"));
    js_obj_set(js, r, "ignorePunctuation", js_bool(o ? nf_get_int(js, o, "_punc") : 0));
    js_obj_set(js, r, "collation", js_string(js, "default"));
    js_obj_set(js, r, "numeric", js_bool(o ? nf_get_int(js, o, "_num") : 0));
    js_obj_set(js, r, "caseFirst", js_string(js, "false"));
    return js_objval(r);
}

/* ---------------- namespace statics ---------------- */

/**
 * @brief `supportedLocalesOf(locales)`：回显请求的标签 / `supportedLocalesOf(locales)`: echo the requested tags
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 忽略 / Ignored
 * @param[in,out] a 实参数组，`a[0]` 为标签或标签数组 / Argument array; `a[0]` is a tag or an array of tags
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 新建数组，元素为形状合法的标签 / A new array holding every well-formed tag
 *
 * @note 引擎只有一套 en-US 模式，因此「支持」等价于「形状合法」 / The engine ships one en-US pattern set, so
 *       "supported" reduces to "well formed"
 */
static JsValue intl_supported_locales(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsObj *out = js_array_new(js);
    JsValue in = i_arg(a, argc, 0);
    int n = 0, i, count = 0;
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    if (in.t == JS_STR)
        n = 1;
    else if (in.t == JS_OBJ && in.obj)
        n = (int)js_to_number(js, js_obj_get(js, in.obj, "length"));
    for (i = 0; i < n; i++) {
        char tag[48];
        JsValue item = in.t == JS_STR ? in : js_obj_get(js, in.obj, js_num_to_string(js, (double)i));
        if (item.t != JS_STR || !item.str || !i_locale_tag(item.str, tag, sizeof(tag)))
            continue;
        js_obj_set(js, out, js_num_to_string(js, (double)count), js_string(js, tag));
        count++;
    }
    js_obj_set(js, out, "length", js_num(count));
    return js_objval(out);
}

/**
 * @brief `Intl.getCanonicalLocales(locales)` / `Intl.getCanonicalLocales(locales)`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 忽略 / Ignored
 * @param[in,out] a 实参数组，`a[0]` 为标签或标签数组 / Argument array; `a[0]` is a tag or an array of tags
 * @param[in]     argc 实参个数 / Argument count
 * @param[in,out] ud 未使用 / Unused
 *
 * @return 规范化后的标签数组 / An array of canonicalised tags
 *
 * @note 规范化只做语言子标签小写与下划线转连字符 / Canonicalisation only lowercases the language subtag and turns
 *       underscores into hyphens
 */
static JsValue intl_canonical_locales(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsValue in = i_arg(a, argc, 0);
    char tag[48];
    JsObj *out;
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    if (in.t == JS_STR) {
        out = js_array_new(js);
        if (i_locale_tag(in.str, tag, sizeof(tag))) {
            js_obj_set(js, out, "0", js_string(js, tag));
            js_obj_set(js, out, "length", js_num(1));
        }
        return js_objval(out);
    }
    return intl_supported_locales(js, thisv, a, argc, ud);
}

/* ---------------- install ---------------- */

/**
 * @brief 创建三个格式化原型对象 / Create the three formatter prototype objects
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[out]    nfp 接收 `NumberFormat.prototype` / Receives `NumberFormat.prototype`
 * @param[out]    dtp 接收 `DateTimeFormat.prototype` / Receives `DateTimeFormat.prototype`
 * @param[out]    cop 接收 `Collator.prototype` / Receives `Collator.prototype`
 */
static void intl_make_protos(HpeJs *js, JsObj **nfp, JsObj **dtp, JsObj **cop)
{
    *nfp = js_object_new(js);
    (*nfp)->class_name = "Intl.NumberFormat";
    *dtp = js_object_new(js);
    (*dtp)->class_name = "Intl.DateTimeFormat";
    *cop = js_object_new(js);
    (*cop)->class_name = "Intl.Collator";
}

/**
 * @brief 把 `Intl` 命名空间安装到全局对象 / Install the `Intl` namespace on the global object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @note 三个构造器各自带 `supportedLocalesOf` 静态度；`Intl` 本身还提供 `getCanonicalLocales` /
 *       Each constructor carries a static `supportedLocalesOf`; `Intl` itself also exposes `getCanonicalLocales`
 */
void hpe_js_intl_install(HpeJs *js)
{
    JsObj *intl = js_object_new(js);
    JsObj *nfp, *dtp, *cop;
    intl->class_name = "Intl";
    intl_make_protos(js, &nfp, &dtp, &cop);

    i_def(js, nfp, "format", nf_format, NULL);
    i_def(js, nfp, "resolvedOptions", nf_resolved, NULL);
    i_def(js, dtp, "format", dt_format, NULL);
    i_def(js, dtp, "resolvedOptions", dt_resolved, NULL);
    i_def(js, cop, "compare", co_compare, NULL);
    i_def(js, cop, "resolvedOptions", co_resolved, NULL);

    i_ctor(js, intl, "NumberFormat", native_numberformat_ctor, nfp);
    i_ctor(js, intl, "DateTimeFormat", native_datetime_ctor, dtp);
    i_ctor(js, intl, "Collator", native_collator_ctor, cop);
    i_def(js, js_obj_get(js, intl, "NumberFormat").obj, "supportedLocalesOf", intl_supported_locales, NULL);
    i_def(js, js_obj_get(js, intl, "DateTimeFormat").obj, "supportedLocalesOf", intl_supported_locales,
          NULL);
    i_def(js, js_obj_get(js, intl, "Collator").obj, "supportedLocalesOf", intl_supported_locales, NULL);

    i_def(js, intl, "getCanonicalLocales", intl_canonical_locales, NULL);
    js_obj_set(js, js->global_obj, "Intl", js_objval(intl));
}
