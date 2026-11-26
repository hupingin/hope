/* Hope UI Engine - Date built-in (ES subset)
 *
 * Layout: a Date instance is a JS_OBJ_DATE object whose `num_val` holds the
 * time value in milliseconds since the Unix epoch (UTC). NaN means an invalid
 * date, matching the spec's [[DateValue]].
 *
 * The wall clock comes from the platform layer (hpe_time_epoch_ms). Date is
 * the only builtin that talks to the platform, so everything else stays
 * deterministic and headless-testable; tests pin values through the
 * constructor and the UTC setters instead of "now".
 *
 * Local time is resolved per instant through hpe_time_utc_offset_minutes,
 * which reports the offset the system's timezone rules had in force at that
 * moment. This is what makes `getHours()` follow DST transitions and the
 * historical rule changes of the configured zone instead of assuming UTC.
 */
#include "js/hpe_js_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "platform/hpe_platform.h"

#define MS_PER_DAY 86400000.0
#define MS_PER_HOUR 3600000.0
#define MS_PER_MIN 60000.0
#define MS_PER_SEC 1000.0

static JsValue d_arg(JsValue *a, int argc, int i);
static double local_to_utc(double local_ms);

/**
 * @brief 把纪元天数转换为公历年月日 / Convert a day count since the epoch to a civil date
 *
 * @param[in]     z 自 1970-01-01 起的天数 / Day count since 1970-01-01
 * @param[out]    year 公历年 / Receives the year
 * @param[out]    month 公历月（1..12）/ Receives the month, 1..12
 * @param[out]    day 公历日 / Receives the day of month
 *
 * @note 采用 Howard Hinnant 的 civil 算法（自 0000-03-01 起的纪元划分）/ Uses Howard Hinnant's civil algorithm with the era anchored at 0000-03-01
 */

static void days_to_civil(long long z, int *year, int *month, int *day)
{
    long long era, doe, yoe, y, doy, mp, d, m;
    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2)
        y++;
    *year = (int)y;
    *month = (int)m;
    *day = (int)d;
}

/**
 * @brief 把公历年月日转换为纪元天数 / Convert a civil date to a day count since the epoch
 *
 * @param[in]     y 公历年 / Year
 * @param[in]     m 公历月（1..12）/ Month, 1..12
 * @param[in]     d 公历日 / Day of month
 *
 * @return 自 1970-01-01 起的天数 / Day count since 1970-01-01
 *
 * @note 与 `days_to_civil` 互为逆运算 / The inverse of `days_to_civil`
 */
static long long civil_to_days(long long y, int m, int d)
{
    long long era, yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/**
 * @brief 由纪元天数求星期几 / Get the weekday from a day count since the epoch
 *
 * @param[in]     days 自 1970-01-01 起的天数 / Day count since 1970-01-01
 *
 * @return 星期几，0 表示周日 / The weekday with 0 meaning Sunday
 *
 * @note 与 `Date.getDay()` 的编号一致 / Matches the numbering of `Date.getDay()`
 */
static int day_of_week(long long days)
{
    long long wd = (days + 4) % 7; /* 1970-01-01 was a Thursday */
    if (wd < 0)
        wd += 7;
    return (int)wd;
}

/**
 * @brief 读取 Date 实例的时间值 / Read the time value of a Date instance
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 方法调用的 `this`，必须是 Date / The `this` of the call, which must be a Date
 * @param[out]    out 接收毫秒时间值 / Receives the millisecond time value
 *
 * @return 成功返回 1，`this` 不是 Date 时抛错并返回 0 / 1 on success; throws and returns 0 when `this` is not a Date
 */
static int date_value(HpeJs *js, JsValue thisv, double *out)
{
    if (thisv.t != JS_OBJ || !thisv.obj || thisv.obj->kind != JS_OBJ_DATE) {
        js_error(js, 0, "Date.prototype method called on a non-Date");
        return 0;
    }
    *out = thisv.obj->num_val;
    return 1;
}

/**
 * @brief 从平台层取当前毫秒时间戳 / Get the current millisecond timestamp from the platform layer
 *
 * @return 自 Unix 纪元起的毫秒数 / Milliseconds since the Unix epoch
 *
 * @note Date 是唯一访问平台的内建，其余内建保持确定性以便无头测试 / Date is the only builtin touching the platform so the rest stay deterministic for headless tests
 */
static double now_ms(void)
{
    return hpe_time_epoch_ms();
}

/**
 * @brief 读取定长十进制数字并转成整数 / Read a fixed-length decimal run and convert it to an integer
 *
 * @param[in]     s 输入字符串 / Input string
 * @param[in]     len 要读取的数字个数 / Number of digits to read
 * @param[out]    out 接收转换结果 / Receives the converted value
 *
 * @return 全部为数字返回 1，否则返回 0 / 1 when every character is a digit, otherwise 0
 */

static int digits_in(const char *s, int len, int *out)
{
    int i, v = 0;
    for (i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9')
            return 0;
        v = v * 10 + (s[i] - '0');
    }
    *out = v;
    return 1;
}

/* Parses the subset of the date-time string grammar tests rely on:
 *   2024-01-02            2024-01-02T03:04:05.678Z
 *   2024-01-02T03:04:05+08:00              2024/01/02 03:04:05
 * Returns NaN when nothing recognisable is found. */

/**
 * @brief 解析 `YYYY[-/]MM[-/]DD` 日期部分 / Parse a `YYYY[-/]MM[-/]DD` date portion
 *
 * @param[in]     p 解析游标 / Parse cursor
 * @param[out]    y 年 / Receives the year
 * @param[out]    mo 月 / Receives the month
 * @param[out]    d 日 / Receives the day
 *
 * @return 解析后的游标；年月非法时返回 `NULL` / The cursor after parsing, or `NULL` when year or month is malformed
 *
 * @note 日部分可省略，缺省时保持调用方给的初值 / The day part is optional and keeps the caller's initial value when absent
 */
static const char *parse_date_numeric(const char *p, int *y, int *mo, int *d)
{
    if (!digits_in(p, 4, y))
        return NULL;
    p += 4;
    if (*p != '-' && *p != '/')
        return p;
    p++;
    if (!digits_in(p, 2, mo))
        return NULL;
    p += 2;
    if (*p == '-' || *p == '/') {
        p++;
        if (!digits_in(p, 2, d))
            return NULL;
        p += 2;
    }
    return p;
}

/**
 * @brief 解析 `HH[:MM[:SS[.fff]]]` 时刻部分 / Parse an `HH[:MM[:SS[.fff]]]` clock portion
 *
 * @param[in]     p 解析游标 / Parse cursor
 * @param[out]    h 时 / Receives the hour
 * @param[out]    mi 分 / Receives the minute
 * @param[out]    se 秒 / Receives the second
 * @param[out]    ms 毫秒 / Receives the milliseconds
 *
 * @return 解析后的游标；时或分非法时返回 `NULL` / The cursor after parsing, or `NULL` when hour or minute is malformed
 *
 * @note 小数部分按位加权累加到 `*ms`，多余位数被忽略 / Fractional digits accumulate into `*ms` by place value; extra digits are ignored
 */
static const char *parse_date_clock(const char *p, int *h, int *mi, int *se, int *ms)
{
    if (!digits_in(p, 2, h))
        return NULL;
    p += 2;
    if (*p != ':')
        return p;
    p++;
    if (!digits_in(p, 2, mi))
        return NULL;
    p += 2;
    if (*p != ':')
        return p;
    p++;
    if (!digits_in(p, 2, se))
        return NULL;
    p += 2;
    if (*p == '.') {
        int scale = 100;
        p++;
        while (*p >= '0' && *p <= '9') {
            *ms += (*p - '0') * scale;
            scale /= 10;
            p++;
        }
    }
    return p;
}

/**
 * @brief 解析 `Z` 或 `±HH[:MM]` 时区部分 / Parse a `Z` or `±HH[:MM]` timezone portion
 *
 * @param[in]     p 解析游标 / Parse cursor
 * @param[out]    tzh 时区小时 / Receives the timezone hours
 * @param[out]    tzm 时区分钟 / Receives the timezone minutes
 * @param[out]    sign 时区符号（1 东 / -1 西 / 0 为 UTC 或缺省）/ Receives the sign: 1 east, -1 west, 0 for UTC or absent
 *
 * @return 解析后的游标；小时非法时返回 `NULL` / The cursor after parsing, or `NULL` when the hours are malformed
 *
 * @note 分钟内可省略冒号；无时区标记时不推进游标 / The colon before minutes is optional; the cursor does not advance when no zone marker is present
 */
static const char *parse_date_zone(const char *p, int *tzh, int *tzm, int *sign)
{
    if (*p == 'Z' || *p == 'z')
        return p + 1;
    if (*p != '+' && *p != '-')
        return p;
    *sign = (*p == '-') ? -1 : 1;
    p++;
    if (!digits_in(p, 2, tzh))
        return NULL;
    p += 2;
    if (*p == ':')
        p++;
    if (digits_in(p, 2, tzm))
        p += 2;
    return p;
}

/**
 * @brief 解析 ISO 8601 及常见日期的字符串形式 / Parse a date string in ISO 8601 and common forms
 *
 * @details 识别 `2024-01-02`、`2024-01-02T03:04:05.678Z`、`2024-01-02T03:04:05+08:00`、`2024/01/02 03:04:05` 等形式；`T`/空格后的时刻可省略，时区偏移会从结果中扣除。
 *          Accepts `2024-01-02`, `2024-01-02T03:04:05.678Z`, `2024-01-02T03:04:05+08:00` and `2024/01/02 03:04:05`; the clock after `T` or a space is optional and a timezone offset is subtracted from the result.
 *
 * @param[in]     s 待解析的日期字符串 / Date string to parse
 *
 * @return 毫秒时间值；无法识别时返回 NaN / The millisecond time value, or NaN when nothing recognisable is found
 *
 * @note 尾部存在无法识别的字符时按无效处理 / Trailing garbage makes the whole string invalid
 */
static double parse_date_string(const char *s)
{
    int y = 0, mo = 1, d = 1, h = 0, mi = 0, se = 0, ms = 0;
    int tzh = 0, tzm = 0, sign = 0;
    const char *p = s;
    long long days;
    double t;

    if (!p)
        return NAN;
    while (*p == ' ' || *p == '\t')
        p++;
    p = parse_date_numeric(p, &y, &mo, &d);
    if (!p)
        return NAN;
    if (*p == 'T' || *p == 't' || *p == ' ') {
        p++;
        p = parse_date_clock(p, &h, &mi, &se, &ms);
        if (!p)
            return NAN;
        p = parse_date_zone(p, &tzh, &tzm, &sign);
        if (!p)
            return NAN;
    }
    while (*p == ' ')
        p++;
    if (*p && *p != 'Z' && *p != 'z' && !(*p == '+' || *p == '-'))
        return NAN; /* trailing garbage -> invalid, like the spec's fallback rules */

    days = civil_to_days(y, mo, d);
    t = (double)days * MS_PER_DAY + (double)h * MS_PER_HOUR + (double)mi * MS_PER_MIN +
        (double)se * MS_PER_SEC + (double)ms;
    if (sign)
        t -= (double)sign * ((double)tzh * MS_PER_HOUR + (double)tzm * MS_PER_MIN);
    return t;
}

/**
 * @brief 分配一个 Date 对象 / Allocate a Date object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     ms 毫秒时间值（NaN 表示无效日期）/ The millisecond time value, NaN for an invalid date
 *
 * @return 类名为 `Date`、原型为 `Date.prototype` 的新对象 / The new object with class `Date` and prototype `Date.prototype`
 */

static JsObj *date_alloc(HpeJs *js, double ms)
{
    JsObj *o = js_object_raw(js);
    o->kind = JS_OBJ_DATE;
    o->class_name = "Date";
    o->proto = js->date_proto;
    o->num_val = ms;
    return o;
}

/**
 * @brief `Date` 构造函数 / The `Date` constructor
 *
 * @details 无参取当前时间；单参按字符串解析、Date 拷贝或数值；多参按 `Date(y,m,d,h,mi,s,ms)` 处理，月份 0 起、越界字段回卷，两位年份映射到 1900..1999。
 *          No argument uses the current time; a single argument is parsed as a string, copied from a Date or coerced from a number; multiple arguments follow `Date(y,m,d,h,mi,s,ms)` with zero-based months, roll-over fields and two-digit years mapped to 1900..1999.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 `this`（未使用）/ The `this` of the call (unused)
 * @param[in,out] a 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 用户数据（未使用）/ User data (unused)
 *
 * @return 新的 Date 实例 / The new Date instance
 *
 * @note 超出 `±275760` 年的年份直接得到无效日期 / An out-of-range year beyond `±275760` yields an invalid date
 */
static JsValue native_date_ctor(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    if (argc == 0)
        return js_objval(date_alloc(js, now_ms()));
    if (argc == 1) {
        JsValue v = a[0];
        if (v.t == JS_STR)
            return js_objval(date_alloc(js, parse_date_string(v.str)));
        if (v.t == JS_OBJ && v.obj && v.obj->kind == JS_OBJ_DATE)
            return js_objval(date_alloc(js, v.obj->num_val));
        return js_objval(date_alloc(js, js_to_number(js, v)));
    }
    {
        /* Date(y, m, d, h, mi, s, ms): month is 0-based, out-of-range fields roll over */
        double yv = js_to_number(js, d_arg(a, argc, 0));
        double mv = argc > 1 ? js_to_number(js, a[1]) : 0.0;
        double dv = argc > 2 ? js_to_number(js, a[2]) : 1.0;
        double hv = argc > 3 ? js_to_number(js, a[3]) : 0.0;
        double miv = argc > 4 ? js_to_number(js, a[4]) : 0.0;
        double sv = argc > 5 ? js_to_number(js, a[5]) : 0.0;
        double msv = argc > 6 ? js_to_number(js, a[6]) : 0.0;
        long long yi, mo;
        int mm;
        double t;
        if (yv != yv || fabs(yv) > 275760.0)
            return js_objval(date_alloc(js, NAN));
        yi = (long long)(yv >= 0 ? floor(yv) : ceil(yv));
        if (yi >= 0 && yi <= 99)
            yi += 1900; /* two-digit years map to 1900..1999 */
        mo = (long long)(mv >= 0 ? floor(mv) : ceil(mv));
        yi += mo / 12;
        mm = (int)(mo % 12);
        if (mm < 0) {
            mm += 12;
            yi--;
        }
        t = (double)civil_to_days(yi, mm + 1, 1) * MS_PER_DAY;
        t += (dv - 1.0) * MS_PER_DAY;
        t += hv * MS_PER_HOUR + miv * MS_PER_MIN + sv * MS_PER_SEC + msv;
        t = floor(t + (t >= 0 ? 0.5 : -0.5));
        /* the field form is local time, so shift it onto the UTC timeline */
        return js_objval(date_alloc(js, local_to_utc(t)));
    }
}

/* ---------------- getters ---------------- */

typedef struct {
    int y, mo, d, wd, h, mi, se, ms;
} DateParts;

/**
 * @brief 把 Date 的毫秒值拆成日期各字段 / Split a Date's millisecond value into calendar fields
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 方法调用的 `this` / The `this` of the call
 * @param[out]    p 接收拆解后的字段结构 / Receives the split field structure
 *
 * @return 成功返回 1；`this` 非 Date 或时间值无效返回 0 / 1 on success; 0 when `this` is not a Date or the time value is invalid
 *
 * @note 负数毫秒的取整做了收尾处理，字段值全部非负 / Negative milliseconds are normalised so every field ends up non-negative
 */
static void split_ms_fields(double ms, DateParts *p)
{
    long long days, rem;
    days = (long long)floor(ms / MS_PER_DAY);
    rem = (long long)ms - days * (long long)MS_PER_DAY;
    if (rem < 0) {
        rem += (long long)MS_PER_DAY;
        days--;
    }
    days_to_civil(days, &p->y, &p->mo, &p->d);
    p->wd = day_of_week(days);
    p->h = (int)(rem / 3600000);
    p->mi = (int)((rem / 60000) % 60);
    p->se = (int)((rem / 1000) % 60);
    p->ms = (int)(rem % 1000);
}

/**
 * @brief 某个时刻生效的时区信息 / The timezone in force at one instant
 *
 * @note `offset_min` 以「东为正」的分钟数表示，与 `getTimezoneOffset()` 的符号相反 / `offset_min` is minutes positive east, the opposite sign of `getTimezoneOffset()`
 */
typedef struct {
    int offset_min;
    int is_dst;
    char name[64];
} TzInfo;

/**
 * @brief 求某个 UTC 时刻生效的时区 / Resolve the timezone in force at a UTC instant
 *
 * @param[in]     ms 自 Unix 纪元起的毫秒 / Milliseconds since the Unix epoch
 * @param[out]    tzi 接收时区信息 / Receives the timezone information
 *
 * @note 偏移按**该时刻**的历史规则求解，因此夏令时切换前后会不同 / The offset follows the rules **in force at that instant**, so it differs across a DST transition
 */
static void tz_at(double ms, TzInfo *tzi)
{
    tzi->name[0] = '\0';
    tzi->is_dst = 0;
    tzi->offset_min = hpe_time_utc_offset_minutes(ms, tzi->name, sizeof(tzi->name), &tzi->is_dst);
}

/**
 * @brief 把本地墙钟毫秒换算成 UTC 毫秒 / Convert local wall-clock milliseconds to UTC milliseconds
 *
 * @param[in]     local_ms 按本地时间解释的墙钟毫秒 / Wall-clock milliseconds interpreted as local time
 *
 * @return 对应的 UTC 毫秒 / The matching UTC milliseconds
 *
 * @note 先按一次猜测求偏移，再用该偏移回算；跨越夏令时切换时二次求解收敛到正确一侧 / The offset is resolved from a first guess and then re-applied; around a DST transition the second resolution settles on the correct side
 */
static double local_to_utc(double local_ms)
{
    TzInfo t;
    tz_at(local_ms, &t);
    tz_at(local_ms - (double)t.offset_min * MS_PER_MIN, &t);
    return local_ms - (double)t.offset_min * MS_PER_MIN;
}

/**
 * @brief 把 Date 实例按**本地时间**拆成日历字段 / Split a Date instance into **local-time** calendar fields
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 方法调用的 `this` / The `this` of the call
 * @param[out]    p 接收拆解后的字段结构 / Receives the split field structure
 *
 * @return 成功返回 1；`this` 非 Date 或时间值无效返回 0 / 1 on success; 0 when `this` is not a Date or the time value is invalid
 *
 * @note 先把 UTC 毫秒加上**该时刻生效**的时区偏移再拆字段 / The offset **in force at that instant** is added to the UTC milliseconds before the fields are split
 */
static int split_time(HpeJs *js, JsValue thisv, DateParts *p)
{
    double ms;
    TzInfo t;
    if (!date_value(js, thisv, &ms))
        return 0;
    if (ms != ms) {
        memset(p, 0, sizeof(*p));
        return 0;
    }
    tz_at(ms, &t);
    split_ms_fields(ms + (double)t.offset_min * MS_PER_MIN, p);
    return 1;
}

/**
 * @brief 把 Date 实例拆成 UTC 日历字段 / Split a Date instance into UTC calendar fields
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 方法调用的 `this` / The `this` of the call
 * @param[out]    p 接收拆解后的字段结构 / Receives the split field structure
 *
 * @return 成功返回 1；`this` 非 Date 或时间值无效返回 0 / 1 on success; 0 when `this` is not a Date or the time value is invalid
 *
 * @note `getUTC*` 走本函数，与本地时间的 `split_time` 相对 / Backs `getUTC*`, the counterpart of the local-time `split_time`
 */
static int split_time_utc(HpeJs *js, JsValue thisv, DateParts *p)
{
    double ms;
    if (!date_value(js, thisv, &ms))
        return 0;
    if (ms != ms) {
        memset(p, 0, sizeof(*p));
        return 0;
    }
    split_ms_fields(ms, p);
    return 1;
}

/**
 * @brief 按 `ud` 选择本地或 UTC 拆分 / Split in local or UTC terms based on `ud`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 方法调用的 `this` / The `this` of the call
 * @param[out]    p 接收拆解后的字段结构 / Receives the split field structure
 * @param[in]     ud 非 `NULL` 表示 UTC 版本 / Non-`NULL` selects the UTC variant
 *
 * @return 成功返回 1 / 1 on success
 *
 * @note 同一份实现同时供给 `getHours` 与 `getUTCHours`，避免两份逐字段拷贝 / One implementation serves both `getHours` and `getUTCHours`, avoiding a second field-by-field copy
 */
static int split_like(HpeJs *js, JsValue thisv, DateParts *p, void *ud)
{
    return ud ? split_time_utc(js, thisv, p) : split_time(js, thisv, p);
}

/**
 * @brief 把 Date 实例拆成 UTC 日历字段 / Split a Date instance into UTC calendar fields
 *
 * @param[in]     datev Date 值 / The Date value
 * @param[out]    out 接收拆解后的字段 / Receives the split fields
 *
 * @return 成功返回 1，非 Date 或无效日期返回 0 / 1 on success, 0 for a non-Date or an invalid date
 *
 * @note 供 `Intl.DateTimeFormat` 复用，避免历法算法出现第二份副本 / Shared with `Intl.DateTimeFormat` so the civil
 *       algorithm has a single copy
 */
int hpe_js_date_fields(JsValue datev, HpeDateFields *out)
{
    DateParts p;
    if (datev.t != JS_OBJ || !datev.obj || datev.obj->kind != JS_OBJ_DATE)
        return 0;
    if (datev.obj->num_val != datev.obj->num_val)
        return 0;
    split_ms_fields(datev.obj->num_val, &p);
    out->year = p.y;
    out->month = p.mo;
    out->day = p.d;
    out->weekday = p.wd;
    out->hour = p.h;
    out->minute = p.mi;
    out->second = p.se;
    out->ms = p.ms;
    return 1;
}

/**
 * @brief `Date.prototype.getFullYear`：取四位年份 / `Date.prototype.getFullYear`: get the full year
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 方法调用的 `this` / The `this` of the call
 * @param[in,out] a 实参数组（未使用）/ Actual argument array (unused)
 * @param[in]     argc 参数个数（未使用）/ Argument count (unused)
 * @param[in,out] ud 用户数据（未使用）/ User data (unused)
 *
 * @return 年份数值，无效日期返回 NaN / The year, or NaN for an invalid date
 */
static JsValue date_get_full_year(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    DateParts p;
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    if (!split_like(js, thisv, &p, ud))
        return js_num(NAN);
    return js_num((double)p.y);
}

/**
 * @brief `Date.prototype.getMonth`：取月份（0 起）/ `Date.prototype.getMonth`: get the zero-based month
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 方法调用的 `this` / The `this` of the call
 * @param[in,out] a 实参数组（未使用）/ Actual argument array (unused)
 * @param[in]     argc 参数个数（未使用）/ Argument count (unused)
 * @param[in,out] ud 用户数据（未使用）/ User data (unused)
 *
 * @return 月份数值 0..11，无效日期返回 NaN / The month 0..11, or NaN for an invalid date
 */
static JsValue date_get_month(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    DateParts p;
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    if (!split_like(js, thisv, &p, ud))
        return js_num(NAN);
    return js_num((double)(p.mo - 1));
}

/**
 * @brief `Date.prototype.getDate`：取月内日 / `Date.prototype.getDate`: get the day of month
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 方法调用的 `this` / The `this` of the call
 * @param[in,out] a 实参数组（未使用）/ Actual argument array (unused)
 * @param[in]     argc 参数个数（未使用）/ Argument count (unused)
 * @param[in,out] ud 用户数据（未使用）/ User data (unused)
 *
 * @return 日数值，无效日期返回 NaN / The day of month, or NaN for an invalid date
 */
static JsValue date_get_date(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    DateParts p;
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    if (!split_like(js, thisv, &p, ud))
        return js_num(NAN);
    return js_num((double)p.d);
}

/**
 * @brief `Date.prototype.getDay`：取星期几 / `Date.prototype.getDay`: get the weekday
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 方法调用的 `this` / The `this` of the call
 * @param[in,out] a 实参数组（未使用）/ Actual argument array (unused)
 * @param[in]     argc 参数个数（未使用）/ Argument count (unused)
 * @param[in,out] ud 用户数据（未使用）/ User data (unused)
 *
 * @return 星期几 0..6（0 为周日），无效日期返回 NaN / The weekday 0..6 with 0 for Sunday, or NaN for an invalid date
 */
static JsValue date_get_day(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    DateParts p;
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    if (!split_like(js, thisv, &p, ud))
        return js_num(NAN);
    return js_num((double)p.wd);
}

/**
 * @brief `Date.prototype.getHours`：取小时 / `Date.prototype.getHours`: get the hour
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 方法调用的 `this` / The `this` of the call
 * @param[in,out] a 实参数组（未使用）/ Actual argument array (unused)
 * @param[in]     argc 参数个数（未使用）/ Argument count (unused)
 * @param[in,out] ud 用户数据（未使用）/ User data (unused)
 *
 * @return 小时数值，无效日期返回 NaN / The hour, or NaN for an invalid date
 */
static JsValue date_get_hours(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    DateParts p;
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    if (!split_like(js, thisv, &p, ud))
        return js_num(NAN);
    return js_num((double)p.h);
}

/**
 * @brief `Date.prototype.getMinutes`：取分钟 / `Date.prototype.getMinutes`: get the minute
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 方法调用的 `this` / The `this` of the call
 * @param[in,out] a 实参数组（未使用）/ Actual argument array (unused)
 * @param[in]     argc 参数个数（未使用）/ Argument count (unused)
 * @param[in,out] ud 用户数据（未使用）/ User data (unused)
 *
 * @return 分钟数值，无效日期返回 NaN / The minute, or NaN for an invalid date
 */
static JsValue date_get_minutes(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    DateParts p;
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    if (!split_like(js, thisv, &p, ud))
        return js_num(NAN);
    return js_num((double)p.mi);
}

/**
 * @brief `Date.prototype.getSeconds`：取秒 / `Date.prototype.getSeconds`: get the second
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 方法调用的 `this` / The `this` of the call
 * @param[in,out] a 实参数组（未使用）/ Actual argument array (unused)
 * @param[in]     argc 参数个数（未使用）/ Argument count (unused)
 * @param[in,out] ud 用户数据（未使用）/ User data (unused)
 *
 * @return 秒数值，无效日期返回 NaN / The second, or NaN for an invalid date
 */
static JsValue date_get_seconds(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    DateParts p;
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    if (!split_like(js, thisv, &p, ud))
        return js_num(NAN);
    return js_num((double)p.se);
}

/**
 * @brief `Date.prototype.getMilliseconds`：取毫秒 / `Date.prototype.getMilliseconds`: get the milliseconds
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 方法调用的 `this` / The `this` of the call
 * @param[in,out] a 实参数组（未使用）/ Actual argument array (unused)
 * @param[in]     argc 参数个数（未使用）/ Argument count (unused)
 * @param[in,out] ud 用户数据（未使用）/ User data (unused)
 *
 * @return 毫秒数值 0..999，无效日期返回 NaN / The milliseconds 0..999, or NaN for an invalid date
 */
static JsValue date_get_ms(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    DateParts p;
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    if (!split_like(js, thisv, &p, ud))
        return js_num(NAN);
    return js_num((double)p.ms);
}

/**
 * @brief `Date.prototype.getTime`：取毫秒时间值 / `Date.prototype.getTime`: get the millisecond time value
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 方法调用的 `this` / The `this` of the call
 * @param[in,out] a 实参数组（未使用）/ Actual argument array (unused)
 * @param[in]     argc 参数个数（未使用）/ Argument count (unused)
 * @param[in,out] ud 用户数据（未使用）/ User data (unused)
 *
 * @return 自 Unix 纪元起的毫秒数，无效日期返回 NaN / Milliseconds since the Unix epoch, or NaN for an invalid date
 */
static JsValue date_get_time(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    double ms;
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    if (!date_value(js, thisv, &ms))
        return js_num(NAN);
    return js_num(ms);
}

/**
 * @brief `Date.prototype.getTimezoneOffset`：取时区偏移 / `Date.prototype.getTimezoneOffset`: get the timezone offset
 *
 * @param[in,out] js JS 运行时上下文（未使用）/ JS runtime context (unused)
 * @param[in]     thisv 方法调用的 `this`（未使用）/ The `this` of the call (unused)
 * @param[in,out] a 实参数组（未使用）/ Actual argument array (unused)
 * @param[in]     argc 参数个数（未使用）/ Argument count (unused)
 * @param[in,out] ud 用户数据（未使用）/ User data (unused)
 *
 * @return 本地时间相对 UTC 的分钟数，西半球为正（与 `hpe_time_utc_offset_minutes` 反号）；无效日期返回 NaN / Minutes of local time behind UTC, positive west (the opposite sign of `hpe_time_utc_offset_minutes`); NaN for an invalid date
 *
 * @note 偏移按**该 Date 时刻**生效的历史时区规则求解，因此同一脚本里 1 月与 7 月的日期可能给出不同结果 / The offset follows the historical rules **in force at that instant**, so January and July dates may differ within one script
 */
static JsValue date_get_tz_offset(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    double ms;
    TzInfo t;
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    if (!date_value(js, thisv, &ms) || ms != ms)
        return js_num(NAN);
    tz_at(ms, &t);
    return js_num(-(double)t.offset_min);
}

/**
 * @brief `Date.prototype.setTime`：设置毫秒时间值 / `Date.prototype.setTime`: set the millisecond time value
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 方法调用的 `this`，必须是 Date / The `this` of the call, which must be a Date
 * @param[in,out] a 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 用户数据（未使用）/ User data (unused)
 *
 * @return 写入后的时间值 / The stored time value
 *
 * @note `this` 非 Date 时抛错并返回 NaN；缺参时写入 NaN / Throws and returns NaN when `this` is not a Date; a missing argument stores NaN
 */

static JsValue date_set_time(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    double ms;
    HPE_UNUSED(ud);
    if (thisv.t != JS_OBJ || !thisv.obj || thisv.obj->kind != JS_OBJ_DATE) {
        js_error(js, 0, "Date.prototype.setTime called on a non-Date");
        return js_num(NAN);
    }
    ms = argc > 0 ? js_to_number(js, a[0]) : NAN;
    thisv.obj->num_val = ms;
    return js_num(ms);
}

/**
 * @brief 各字段 setter 共用的实现 / Shared implementation for the field setters
 *
 * @details 未提供的字段保持当前值，越界值按规范回卷；`first_field` 为 0 且首参在 0..99 时按 1900+ 解释两位年份，结果四舍五入到整毫秒。
 *          Unsupplied fields keep their current value and out-of-range values roll over as the spec requires; when `first_field` is 0 and the first argument is 0..99 it is read as 1900+, and the result is rounded to whole milliseconds.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 方法调用的 `this`，必须是 Date / The `this` of the call, which must be a Date
 * @param[in,out] a 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 * @param[in]     first_field 首个实参写入的字段下标（0=年 … 6=毫秒）/ Field index the first argument writes to (0 = year … 6 = millisecond)
 * @param[in]     utc 非 0 时墙钟字段按 UTC 解释，否则按本地时间 / Non-zero takes the wall-clock fields as UTC, otherwise as local time
 *
 * @return 写入后的时间值 / The stored time value
 *
 * @note `this` 非 Date 时抛错并返回 NaN / Throws and returns NaN when `this` is not a Date
 *
 * @see date_set_full_year date_set_month date_set_date date_set_hours date_set_minutes date_set_seconds date_set_ms
 */
static JsValue set_fields(HpeJs *js, JsValue thisv, JsValue *a, int argc, int first_field, int utc)
{
    DateParts p;
    double f[7];
    long long yy;
    int mm, i;
    double t;
    if (thisv.t != JS_OBJ || !thisv.obj || thisv.obj->kind != JS_OBJ_DATE) {
        js_error(js, 0, "Date.prototype setter called on a non-Date");
        return js_num(NAN);
    }
    if (utc ? !split_time_utc(js, thisv, &p) : !split_time(js, thisv, &p))
        return js_num(NAN);

    f[0] = (double)p.y;
    f[1] = (double)(p.mo - 1);
    f[2] = (double)p.d;
    f[3] = (double)p.h;
    f[4] = (double)p.mi;
    f[5] = (double)p.se;
    f[6] = (double)p.ms;
    for (i = first_field; i < 7 && (i - first_field) < argc; i++)
        f[i] = js_to_number(js, a[i - first_field]);
    /* a two-digit year is only interpreted as 1900+ when it was supplied */
    if (first_field == 0 && argc > 0 && f[0] >= 0 && f[0] <= 99)
        f[0] += 1900;

    yy = (long long)(f[0] >= 0 ? floor(f[0]) : ceil(f[0]));
    {
        long long mo = (long long)(f[1] >= 0 ? floor(f[1]) : ceil(f[1]));
        yy += mo / 12;
        mm = (int)(mo % 12);
        if (mm < 0) {
            mm += 12;
            yy--;
        }
    }
    t = (double)civil_to_days(yy, mm + 1, 1) * MS_PER_DAY;
    t += (f[2] - 1.0) * MS_PER_DAY;
    t += f[3] * MS_PER_HOUR + f[4] * MS_PER_MIN + f[5] * MS_PER_SEC + f[6];
    t = floor(t + (t >= 0 ? 0.5 : -0.5));
    thisv.obj->num_val = utc ? t : local_to_utc(t);
    return js_num(thisv.obj->num_val);
}

/**
 * @brief `Date.prototype.setFullYear`：设置年（可带月日）/ `Date.prototype.setFullYear`: set the year, optionally with month and day
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 方法调用的 `this` / The `this` of the call
 * @param[in,out] a 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 非 `NULL` 表示 UTC 变体 / Non-`NULL` selects the UTC variant
 *
 * @return 写入后的时间值 / The stored time value
 *
 * @see set_fields
 */
static JsValue date_set_full_year(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    return set_fields(js, thisv, a, argc, 0, ud != NULL);
}

/**
 * @brief `Date.prototype.setMonth`：设置月（可带日）/ `Date.prototype.setMonth`: set the month, optionally with the day
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 方法调用的 `this` / The `this` of the call
 * @param[in,out] a 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 非 `NULL` 表示 UTC 变体 / Non-`NULL` selects the UTC variant
 *
 * @return 写入后的时间值 / The stored time value
 *
 * @see set_fields
 */
static JsValue date_set_month(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    return set_fields(js, thisv, a, argc, 1, ud != NULL);
}

/**
 * @brief `Date.prototype.setDate`：设置月内日 / `Date.prototype.setDate`: set the day of month
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 方法调用的 `this` / The `this` of the call
 * @param[in,out] a 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 非 `NULL` 表示 UTC 变体 / Non-`NULL` selects the UTC variant
 *
 * @return 写入后的时间值 / The stored time value
 *
 * @see set_fields
 */
static JsValue date_set_date(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    return set_fields(js, thisv, a, argc, 2, ud != NULL);
}

/**
 * @brief `Date.prototype.setHours`：设置时（可带分秒毫秒）/ `Date.prototype.setHours`: set the hour, optionally with minute, second and millisecond
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 方法调用的 `this` / The `this` of the call
 * @param[in,out] a 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 非 `NULL` 表示 UTC 变体 / Non-`NULL` selects the UTC variant
 *
 * @return 写入后的时间值 / The stored time value
 *
 * @see set_fields
 */
static JsValue date_set_hours(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    return set_fields(js, thisv, a, argc, 3, ud != NULL);
}

/**
 * @brief `Date.prototype.setMinutes`：设置分（可带秒毫秒）/ `Date.prototype.setMinutes`: set the minute, optionally with second and millisecond
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 方法调用的 `this` / The `this` of the call
 * @param[in,out] a 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 非 `NULL` 表示 UTC 变体 / Non-`NULL` selects the UTC variant
 *
 * @return 写入后的时间值 / The stored time value
 *
 * @see set_fields
 */
static JsValue date_set_minutes(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    return set_fields(js, thisv, a, argc, 4, ud != NULL);
}

/**
 * @brief `Date.prototype.setSeconds`：设置秒（可带毫秒）/ `Date.prototype.setSeconds`: set the second, optionally with milliseconds
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 方法调用的 `this` / The `this` of the call
 * @param[in,out] a 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 非 `NULL` 表示 UTC 变体 / Non-`NULL` selects the UTC variant
 *
 * @return 写入后的时间值 / The stored time value
 *
 * @see set_fields
 */
static JsValue date_set_seconds(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    return set_fields(js, thisv, a, argc, 5, ud != NULL);
}

/**
 * @brief `Date.prototype.setMilliseconds`：设置毫秒 / `Date.prototype.setMilliseconds`: set the milliseconds
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 方法调用的 `this` / The `this` of the call
 * @param[in,out] a 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 非 `NULL` 表示 UTC 变体 / Non-`NULL` selects the UTC variant
 *
 * @return 写入后的时间值 / The stored time value
 *
 * @see set_fields
 */
static JsValue date_set_ms(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    return set_fields(js, thisv, a, argc, 6, ud != NULL);
}

/* ---------------- formatting ---------------- */

static const char *const day_names[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const month_names[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                             "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

/**
 * @brief `Date.prototype.toISOString`：输出 ISO 8601 文本 / `Date.prototype.toISOString`: render an ISO 8601 string
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 方法调用的 `this` / The `this` of the call
 * @param[in,out] a 实参数组（未使用）/ Actual argument array (unused)
 * @param[in]     argc 参数个数（未使用）/ Argument count (unused)
 * @param[in,out] ud 用户数据（未使用）/ User data (unused)
 *
 * @return 形如 `2024-01-02T03:04:05.678Z` 的字符串 / A string such as `2024-01-02T03:04:05.678Z`
 *
 * @note 无效日期时抛出 `RangeError`；年份在 0..9999 外用带符号的 7 位年 / Throws a `RangeError` for an invalid date; years outside 0..9999 use a signed 7-digit year
 */
static JsValue date_to_iso(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    DateParts p;
    char buf[64];
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    memset(&p, 0, sizeof(p));
    if (!split_time_utc(js, thisv, &p))
        js_throw_value(js, js_make_error(js, "RangeError", "Invalid time value"));
    if (p.y >= 0 && p.y <= 9999)
        snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", p.y, p.mo, p.d, p.h,
                 p.mi, p.se, p.ms);
    else
        snprintf(buf, sizeof(buf), "%+07d-%02d-%02dT%02d:%02d:%02d.%03dZ", p.y, p.mo, p.d, p.h,
                 p.mi, p.se, p.ms);
    return js_string(js, buf);
}

/**
 * @brief `Date.prototype.toJSON`：无效日期返回 `null` / `Date.prototype.toJSON`: return `null` for an invalid date
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 方法调用的 `this` / The `this` of the call
 * @param[in,out] a 实参数组（未使用）/ Actual argument array (unused)
 * @param[in]     argc 参数个数（未使用）/ Argument count (unused)
 * @param[in,out] ud 用户数据（未使用）/ User data (unused)
 *
 * @return 有效时返回 `toISOString` 的结果，无效时返回 `null` / The `toISOString` result when valid, otherwise `null`
 */
static JsValue date_to_json(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    double ms;
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    if (!date_value(js, thisv, &ms))
        return js_null();
    if (ms != ms)
        return js_null();
    return date_to_iso(js, thisv, NULL, 0, NULL);
}

/**
 * @brief 把偏移分钟格式化成 `GMT+HHMM` / Format an offset in minutes as `GMT+HHMM`
 *
 * @param[out]    buf 接收格式化结果的缓冲 / Buffer receiving the formatted text
 * @param[in]     cap 缓冲容量（字节）/ Buffer capacity in bytes
 * @param[in]     offset_min 本地时间相对 UTC 的偏移分钟（东为正）/ Offset in minutes, positive east
 *
 * @note 分钟部分用绝对值，负偏移不会写成 `-01-30` / The minute part uses the absolute value so a negative offset never renders as `-01-30`
 */
static void fmt_gmt_offset(char *buf, size_t cap, int offset_min)
{
    int a = offset_min < 0 ? -offset_min : offset_min;
    snprintf(buf, cap, "GMT%c%02d%02d", offset_min < 0 ? '-' : '+', a / 60, a % 60);
}

/**
 * @brief `Date.prototype.toString`：输出人类可读日期 / `Date.prototype.toString`: render a human-readable date
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 方法调用的 `this` / The `this` of the call
 * @param[in,out] a 实参数组（未使用）/ Actual argument array (unused)
 * @param[in]     argc 参数个数（未使用）/ Argument count (unused)
 * @param[in,out] ud 用户数据（未使用）/ User data (unused)
 *
 * @return 形如 `Tue Jan 02 2024 11:04:05 GMT+0800 (China Standard Time)` 的字符串；无效日期返回 `Invalid Date` / A string like `Tue Jan 02 2024 11:04:05 GMT+0800 (China Standard Time)`, or `Invalid Date`
 *
 * @note 偏移与时区名取自该时刻生效的规则，无名称时省略括号部分 / The offset and name come from the rules in force at that instant; the parenthesised name is omitted when the platform reports none
 */
static JsValue date_to_string(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    DateParts p;
    TzInfo t;
    char buf[160], gmt[16];
    double ms = 0;
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    if (!split_like(js, thisv, &p, NULL))
        return js_string(js, "Invalid Date");
    date_value(js, thisv, &ms);
    tz_at(ms, &t);
    fmt_gmt_offset(gmt, sizeof(gmt), t.offset_min);
    if (t.name[0])
        snprintf(buf, sizeof(buf), "%s %s %02d %04d %02d:%02d:%02d %s (%s)", day_names[p.wd],
                 month_names[p.mo - 1], p.d, p.y, p.h, p.mi, p.se, gmt, t.name);
    else
        snprintf(buf, sizeof(buf), "%s %s %02d %04d %02d:%02d:%02d %s", day_names[p.wd],
                 month_names[p.mo - 1], p.d, p.y, p.h, p.mi, p.se, gmt);
    return js_string(js, buf);
}

/**
 * @brief `Date.prototype.toDateString`：只输出日期部分 / `Date.prototype.toDateString`: render only the date portion
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 方法调用的 `this` / The `this` of the call
 * @param[in,out] a 实参数组（未使用）/ Actual argument array (unused)
 * @param[in]     argc 参数个数（未使用）/ Argument count (unused)
 * @param[in,out] ud 用户数据（未使用）/ User data (unused)
 *
 * @return 形如 `Tue Jan 02 2024` 的字符串；无效日期返回 `Invalid Date` / A string like `Tue Jan 02 2024`, or `Invalid Date`
 */
static JsValue date_to_date_string(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    DateParts p;
    char buf[64];
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    if (!split_like(js, thisv, &p, ud))
        return js_string(js, "Invalid Date");
    snprintf(buf, sizeof(buf), "%s %s %02d %04d", day_names[p.wd], month_names[p.mo - 1], p.d, p.y);
    return js_string(js, buf);
}

/**
 * @brief `Date.prototype.toTimeString`：只输出时刻部分 / `Date.prototype.toTimeString`: render only the time portion
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 方法调用的 `this` / The `this` of the call
 * @param[in,out] a 实参数组（未使用）/ Actual argument array (unused)
 * @param[in]     argc 参数个数（未使用）/ Argument count (unused)
 * @param[in,out] ud 用户数据（未使用）/ User data (unused)
 *
 * @return 形如 `11:04:05 GMT+0800` 的字符串；无效日期返回 `Invalid Date` / A string like `11:04:05 GMT+0800`, or `Invalid Date`
 */
static JsValue date_to_time_string(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    DateParts p;
    TzInfo t;
    char buf[48], gmt[16];
    double ms = 0;
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    if (!split_like(js, thisv, &p, NULL))
        return js_string(js, "Invalid Date");
    date_value(js, thisv, &ms);
    tz_at(ms, &t);
    fmt_gmt_offset(gmt, sizeof(gmt), t.offset_min);
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d %s", p.h, p.mi, p.se, gmt);
    return js_string(js, buf);
}

/**
 * @brief `Date.prototype.valueOf`：取毫秒时间值 / `Date.prototype.valueOf`: get the millisecond time value
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 方法调用的 `this` / The `this` of the call
 * @param[in,out] a 实参数组（未使用）/ Actual argument array (unused)
 * @param[in]     argc 参数个数（未使用）/ Argument count (unused)
 * @param[in,out] ud 用户数据（未使用）/ User data (unused)
 *
 * @return 毫秒时间值，无效日期返回 NaN / The millisecond time value, or NaN for an invalid date
 */
static JsValue date_value_of(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    double ms;
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    if (!date_value(js, thisv, &ms))
        return js_num(NAN);
    return js_num(ms);
}

/**
 * @brief `Date.prototype.toLocaleDateString`：输出美式本地日期 / `Date.prototype.toLocaleDateString`: render a US-style local date
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 方法调用的 `this` / The `this` of the call
 * @param[in,out] a 实参数组（未使用）/ Actual argument array (unused)
 * @param[in]     argc 参数个数（未使用）/ Argument count (unused)
 * @param[in,out] ud 用户数据（未使用）/ User data (unused)
 *
 * @return 形如 `1/2/2024` 的字符串；无效日期返回 `Invalid Date` / A string like `1/2/2024`, or `Invalid Date`
 *
 * @note `toLocaleString` 也复用本函数 / `toLocaleString` reuses this function
 */
static JsValue date_to_locale_date(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    DateParts p;
    char buf[48];
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    if (!split_like(js, thisv, &p, ud))
        return js_string(js, "Invalid Date");
    snprintf(buf, sizeof(buf), "%d/%d/%d", p.mo, p.d, p.y);
    return js_string(js, buf);
}

/**
 * @brief `Date.prototype.toLocaleTimeString`：输出本地时刻 / `Date.prototype.toLocaleTimeString`: render a local time
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 方法调用的 `this` / The `this` of the call
 * @param[in,out] a 实参数组（未使用）/ Actual argument array (unused)
 * @param[in]     argc 参数个数（未使用）/ Argument count (unused)
 * @param[in,out] ud 用户数据（未使用）/ User data (unused)
 *
 * @return 形如 `03:04:05` 的字符串；无效日期返回 `Invalid Date` / A string like `03:04:05`, or `Invalid Date`
 */
static JsValue date_to_locale_time(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    DateParts p;
    char buf[32];
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    if (!split_like(js, thisv, &p, ud))
        return js_string(js, "Invalid Date");
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", p.h, p.mi, p.se);
    return js_string(js, buf);
}

/**
 * @brief `Date.now()`：取当前毫秒时间戳 / `Date.now()`: get the current millisecond timestamp
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 `this`（未使用）/ The `this` of the call (unused)
 * @param[in,out] a 实参数组（未使用）/ Actual argument array (unused)
 * @param[in]     argc 参数个数（未使用）/ Argument count (unused)
 * @param[in,out] ud 用户数据（未使用）/ User data (unused)
 *
 * @return 自 Unix 纪元起的毫秒数 / Milliseconds since the Unix epoch
 */

static JsValue date_static_now(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(thisv);
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    return js_num(now_ms());
}

/**
 * @brief `Date.parse()`：解析日期字符串 / `Date.parse()`: parse a date string
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 `this`（未使用）/ The `this` of the call (unused)
 * @param[in,out] a 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 用户数据（未使用）/ User data (unused)
 *
 * @return 毫秒时间值，无法解析返回 NaN / The millisecond time value, or NaN when unparseable
 *
 * @note 参数已是 Date 时直接取其时间值，否则先转字符串再解析 / A Date argument yields its value directly; anything else is stringified and parsed
 */
static JsValue date_static_parse(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsValue v = d_arg(a, argc, 0);
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    if (v.t == JS_OBJ && v.obj && v.obj->kind == JS_OBJ_DATE)
        return js_num(v.obj->num_val);
    return js_num(parse_date_string(js_to_string(js, v)));
}

/**
 * @brief `Date.UTC()`：把 UTC 字段合成毫秒时间值 / `Date.UTC()`: combine UTC fields into a millisecond time value
 *
 * @details 按 `(y, m, d, h, mi, s, ms)` 处理，月份 0 起、越界回卷；仅当提供了至少两个参数且年在 0..99 时按 1900+ 解释。
 *          Fields follow `(y, m, d, h, mi, s, ms)` with zero-based months and roll-over; a year in 0..99 is read as 1900+ only when at least two arguments were supplied.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 `this`（未使用）/ The `this` of the call (unused)
 * @param[in,out] a 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 用户数据（未使用）/ User data (unused)
 *
 * @return 毫秒时间值；无参数时返回 NaN / The millisecond time value, or NaN when called with no arguments
 */
static JsValue date_static_utc(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    double yv, mv, dv, hv, miv, sv, msv;
    long long yi;
    int mm;
    double t;
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    if (argc == 0)
        return js_num(NAN);
    yv = js_to_number(js, a[0]);
    mv = argc > 1 ? js_to_number(js, a[1]) : 0.0;
    dv = argc > 2 ? js_to_number(js, a[2]) : 1.0;
    hv = argc > 3 ? js_to_number(js, a[3]) : 0.0;
    miv = argc > 4 ? js_to_number(js, a[4]) : 0.0;
    sv = argc > 5 ? js_to_number(js, a[5]) : 0.0;
    msv = argc > 6 ? js_to_number(js, a[6]) : 0.0;
    yi = (long long)(yv >= 0 ? floor(yv) : ceil(yv));
    if (yi >= 0 && yi <= 99 && argc > 1)
        yi += 1900;
    {
        long long mo = (long long)(mv >= 0 ? floor(mv) : ceil(mv));
        yi += mo / 12;
        mm = (int)(mo % 12);
        if (mm < 0) {
            mm += 12;
            yi--;
        }
    }
    t = (double)civil_to_days(yi, mm + 1, 1) * MS_PER_DAY;
    t += (dv - 1.0) * MS_PER_DAY;
    t += hv * MS_PER_HOUR + miv * MS_PER_MIN + sv * MS_PER_SEC + msv;
    return js_num(floor(t + (t >= 0 ? 0.5 : -0.5)));
}

/* ---------------- install ---------------- */

/**
 * @brief 取第 i 个实参，越界返回 `undefined` / Get the i-th argument, returning `undefined` when out of range
 *
 * @param[in,out] a 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 * @param[in]     i 下标 / Index
 *
 * @return 第 i 个实参或 `undefined` / The i-th argument or `undefined`
 *
 * @note 解释器内建辅助的本文件私有副本 / A local copy of the interpreter's file-static builtin helper
 */
static JsValue d_arg(JsValue *a, int argc, int i)
{
    return i < argc ? a[i] : js_undef();
}

/**
 * @brief 在对象上定义一个原生方法属性 / Define a native method property on an object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] target 目标对象 / Target object
 * @param[in]     name 方法名 / Method name
 * @param[in]     fn 原生实现 / Native implementation
 *
 * @note 包装为 `js_native_new` 的原生函数并写入属性 / Wraps `fn` with `js_native_new` before storing it
 */
static void d_def(HpeJs *js, JsObj *target, const char *name, JsNativeFn fn)
{
    js_obj_set(js, target, name, js_objval(js_native_new(js, fn, NULL)));
}

/**
 * @brief 安装一个别名属性，可携带新的用户数据 / Install an alias property, optionally with fresh user data
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] p 目标原型对象 / Target prototype object
 * @param[in]     name 新键名 / The new key
 * @param[in]     src_name 被复制的原键名 / The source key to copy from
 * @param[in,out] ud 传给别名函数的用户数据；`NULL` 时与原函数完全共用 / User data for the alias; `NULL` shares the very same function object
 *
 * @note 原函数是原生函数且 `ud` 非 `NULL` 时新建一个函数对象，使 `getUTC*`/`setUTC*` 能走 UTC 分支；否则直接共享函数对象 / When the source is a native function and `ud` is non-`NULL` a fresh function object is made so `getUTC*`/`setUTC*` can take the UTC branch; otherwise the function object is shared as-is
 */
static void def_alias(HpeJs *js, JsObj *p, const char *name, const char *src_name, void *ud)
{
    JsProp *prop = js_obj_own_prop(p, src_name);
    if (!prop)
        return;
    if (ud && prop->val.t == JS_OBJ && prop->val.obj && prop->val.obj->kind == JS_OBJ_NATIVE)
        js_obj_set(js, p, name, js_objval(js_native_new(js, prop->val.obj->native, ud)));
    else
        js_obj_set(js, p, name, prop->val);
}

typedef struct {
    const char *name;
    JsNativeFn fn;
} DateMethodDef;

/**
 * @brief 批量安装一组 Date 原型方法 / Install a batch of Date prototype methods
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] p 目标原型对象 / Target prototype object
 * @param[in]     defs 以 `NULL` 名结尾的方法定义表 / Method definition table terminated by a `NULL` name
 *
 * @note 逐项调用 `d_def` 安装 / Each entry is installed through `d_def`
 */
static void date_def_methods(HpeJs *js, JsObj *p, const DateMethodDef *defs)
{
    int i;
    for (i = 0; defs[i].name; i++)
        d_def(js, p, defs[i].name, defs[i].fn);
}

/**
 * @brief 批量安装别名属性 / Install a batch of alias properties
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] p 目标原型对象 / Target prototype object
 * @param[in]     n 别名对数 / Number of alias pairs
 *
 * @note 表项形如 `{ 新名, 原名 }`；名字以 `UTC` 开头的按 UTC 变体安装，其余与原函数共享 / Entries are `{ new_name, source_name }`; a name starting with `UTC` is installed as the UTC variant and the rest share the source function
 */
static void date_def_aliases(HpeJs *js, JsObj *p, const char (*aliases)[2][24], int n)
{
    int i;
    for (i = 0; i < n; i++) {
        const char *name = aliases[i][0];
        void *ud = strstr(name, "UTC") ? (void *)1 : NULL;
        def_alias(js, p, name, aliases[i][1], ud);
    }
}

/* prototype methods; the getUTC* / setUTC* names alias these via alias_pairs */
static const DateMethodDef date_proto_methods[] = {
    { "getFullYear",        date_get_full_year },
    { "getMonth",           date_get_month },
    { "getDate",            date_get_date },
    { "getDay",             date_get_day },
    { "getHours",           date_get_hours },
    { "getMinutes",         date_get_minutes },
    { "getSeconds",         date_get_seconds },
    { "getMilliseconds",    date_get_ms },
    { "getTime",            date_get_time },
    { "getTimezoneOffset",  date_get_tz_offset },
    { "setTime",            date_set_time },
    { "setFullYear",        date_set_full_year },
    { "setMonth",           date_set_month },
    { "setDate",            date_set_date },
    { "setHours",           date_set_hours },
    { "setMinutes",         date_set_minutes },
    { "setSeconds",         date_set_seconds },
    { "setMilliseconds",    date_set_ms },
    { "toISOString",        date_to_iso },
    { "toJSON",             date_to_json },
    { "toString",           date_to_string },
    { "toDateString",       date_to_date_string },
    { "toTimeString",       date_to_time_string },
    { "toLocaleDateString", date_to_locale_date },
    { "toLocaleTimeString", date_to_locale_time },
    { "toLocaleString",     date_to_locale_date },
    { "valueOf",            date_value_of },
    { NULL, NULL }
};

static const char date_alias_pairs[][2][24] = {
    { "getUTCFullYear",     "getFullYear" },
    { "getUTCMonth",        "getMonth" },
    { "getUTCDate",         "getDate" },
    { "getUTCDay",          "getDay" },
    { "getUTCHours",        "getHours" },
    { "getUTCMinutes",      "getMinutes" },
    { "getUTCSeconds",      "getSeconds" },
    { "getUTCMilliseconds", "getMilliseconds" },
    { "setUTCFullYear",     "setFullYear" },
    { "setUTCMonth",        "setMonth" },
    { "setUTCDate",         "setDate" },
    { "setUTCHours",        "setHours" },
    { "setUTCMinutes",      "setMinutes" },
    { "setUTCSeconds",      "setSeconds" },
    { "setUTCMilliseconds", "setMilliseconds" },
    { "getYear",            "getFullYear" }
};

/**
 * @brief 创建 `Date.prototype` 并安装全局 `Date` / Build `Date.prototype` and install the global `Date`
 *
 * @details 安装原型方法及其 UTC 别名，构造全局构造函数（含 `now`/`parse`/`UTC` 静态方法）并写入 `global.Date`。
 *          Installs the prototype methods and their UTC aliases, builds the global constructor with its `now`/`parse`/`UTC` statics and sets `global.Date`.
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 */
void hpe_js_date_install(HpeJs *js)
{
    JsObj *p = js->date_proto;
    JsValue ctor;

    date_def_methods(js, p, date_proto_methods);
    date_def_aliases(js, p, date_alias_pairs,
                     (int)(sizeof(date_alias_pairs) / sizeof(date_alias_pairs[0])));

    ctor = js_objval(js_native_new(js, native_date_ctor, NULL));
    ctor.obj->name = hpe_js_strdup(js, "Date");
    js_obj_set(js, ctor.obj, "name", js_string(js, "Date"));
    js_obj_set(js, ctor.obj, "prototype", js_objval(p));
    d_def(js, ctor.obj, "now", date_static_now);
    d_def(js, ctor.obj, "parse", date_static_parse);
    d_def(js, ctor.obj, "UTC", date_static_utc);
    js_obj_set(js, js->global_obj, "Date", ctor);
}
