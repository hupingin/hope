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
 * Local time == UTC here: the engine has no timezone database, so getHours()
 * and getUTCHours() agree. This is called out in the design doc.
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

/* ---------------- calendar conversion (Howard Hinnant's civil algorithms) ------ */

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

/* weekday: 0 = Sunday, matching Date.getDay() */
static int day_of_week(long long days)
{
    long long wd = (days + 4) % 7; /* 1970-01-01 was a Thursday */
    if (wd < 0)
        wd += 7;
    return (int)wd;
}

static int date_value(HpeJs *js, JsValue thisv, double *out)
{
    if (thisv.t != JS_OBJ || !thisv.obj || thisv.obj->kind != JS_OBJ_DATE) {
        js_error(js, 0, "Date.prototype method called on a non-Date");
        return 0;
    }
    *out = thisv.obj->num_val;
    return 1;
}

static double now_ms(void)
{
    return hpe_time_epoch_ms();
}

/* ---------------- string parsing (ISO 8601 + a few common forms) ---------------- */

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
/* consumes YYYY[-/]MM[-/]DD, filling whichever components are present */
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

/* consumes HH[:MM[:SS[.fff]]] */
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

/* consumes Z or +HH[:MM] / -HH[:MM]; returns the sign (0 = UTC/absent) */
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

/* Parses the subset of the date-time string grammar tests rely on:
 *   2024-01-02            2024-01-02T03:04:05.678Z
 *   2024-01-02T03:04:05+08:00              2024/01/02 03:04:05
 * Returns NaN when nothing recognisable is found. */
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

/* ---------------- constructor ---------------- */

static JsObj *date_alloc(HpeJs *js, double ms)
{
    JsObj *o = js_object_raw(js);
    o->kind = JS_OBJ_DATE;
    o->class_name = "Date";
    o->proto = js->date_proto;
    o->num_val = ms;
    return o;
}

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
        return js_objval(date_alloc(js, t));
    }
}

/* ---------------- getters ---------------- */

typedef struct {
    int y, mo, d, wd, h, mi, se, ms;
} DateParts;

static int split_time(HpeJs *js, JsValue thisv, DateParts *p)
{
    double ms;
    long long days, rem;
    if (!date_value(js, thisv, &ms))
        return 0;
    if (ms != ms) {
        memset(p, 0, sizeof(*p));
        return 0;
    }
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
    return 1;
}

static JsValue date_get_full_year(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    DateParts p;
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    if (!split_time(js, thisv, &p))
        return js_num(NAN);
    return js_num((double)p.y);
}

static JsValue date_get_month(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    DateParts p;
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    if (!split_time(js, thisv, &p))
        return js_num(NAN);
    return js_num((double)(p.mo - 1));
}

static JsValue date_get_date(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    DateParts p;
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    if (!split_time(js, thisv, &p))
        return js_num(NAN);
    return js_num((double)p.d);
}

static JsValue date_get_day(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    DateParts p;
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    if (!split_time(js, thisv, &p))
        return js_num(NAN);
    return js_num((double)p.wd);
}

static JsValue date_get_hours(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    DateParts p;
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    if (!split_time(js, thisv, &p))
        return js_num(NAN);
    return js_num((double)p.h);
}

static JsValue date_get_minutes(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    DateParts p;
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    if (!split_time(js, thisv, &p))
        return js_num(NAN);
    return js_num((double)p.mi);
}

static JsValue date_get_seconds(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    DateParts p;
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    if (!split_time(js, thisv, &p))
        return js_num(NAN);
    return js_num((double)p.se);
}

static JsValue date_get_ms(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    DateParts p;
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    if (!split_time(js, thisv, &p))
        return js_num(NAN);
    return js_num((double)p.ms);
}

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

static JsValue date_get_tz_offset(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(js);
    HPE_UNUSED(thisv);
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    return js_num(0); /* no timezone database: local time is UTC */
}

/* ---------------- setters ---------------- */

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

/* shared body for the field setters. `first_field` is the field index the first
 * argument writes to: setFullYear=0 (y,m,d), setMonth=1 (m,d), setDate=2, ...,
 * setMilliseconds=6. Unsupplied fields keep their current value, and
 * out-of-range values roll over exactly as the spec requires. */
static JsValue set_fields(HpeJs *js, JsValue thisv, JsValue *a, int argc, int first_field)
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
    if (!split_time(js, thisv, &p))
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
    thisv.obj->num_val = t;
    return js_num(t);
}

static JsValue date_set_full_year(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(ud);
    return set_fields(js, thisv, a, argc, 0);
}

static JsValue date_set_month(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(ud);
    return set_fields(js, thisv, a, argc, 1);
}

static JsValue date_set_date(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(ud);
    return set_fields(js, thisv, a, argc, 2);
}

static JsValue date_set_hours(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(ud);
    return set_fields(js, thisv, a, argc, 3);
}

static JsValue date_set_minutes(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(ud);
    return set_fields(js, thisv, a, argc, 4);
}

static JsValue date_set_seconds(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(ud);
    return set_fields(js, thisv, a, argc, 5);
}

static JsValue date_set_ms(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(ud);
    return set_fields(js, thisv, a, argc, 6);
}

/* ---------------- formatting ---------------- */

static const char *const day_names[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const month_names[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                             "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

static JsValue date_to_iso(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    DateParts p;
    char buf[64];
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    if (!split_time(js, thisv, &p))
        js_throw_value(js, js_make_error(js, "RangeError", "Invalid time value"));
    if (p.y >= 0 && p.y <= 9999)
        snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", p.y, p.mo, p.d, p.h,
                 p.mi, p.se, p.ms);
    else
        snprintf(buf, sizeof(buf), "%+07d-%02d-%02dT%02d:%02d:%02d.%03dZ", p.y, p.mo, p.d, p.h,
                 p.mi, p.se, p.ms);
    return js_string(js, buf);
}

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

static JsValue date_to_string(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    DateParts p;
    char buf[80];
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    if (!split_time(js, thisv, &p))
        return js_string(js, "Invalid Date");
    snprintf(buf, sizeof(buf), "%s %s %02d %04d %02d:%02d:%02d GMT+0000 (Coordinated Universal Time)",
             day_names[p.wd], month_names[p.mo - 1], p.d, p.y, p.h, p.mi, p.se);
    return js_string(js, buf);
}

static JsValue date_to_date_string(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    DateParts p;
    char buf[64];
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    if (!split_time(js, thisv, &p))
        return js_string(js, "Invalid Date");
    snprintf(buf, sizeof(buf), "%s %s %02d %04d", day_names[p.wd], month_names[p.mo - 1], p.d, p.y);
    return js_string(js, buf);
}

static JsValue date_to_time_string(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    DateParts p;
    char buf[32];
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    if (!split_time(js, thisv, &p))
        return js_string(js, "Invalid Date");
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d GMT+0000", p.h, p.mi, p.se);
    return js_string(js, buf);
}

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

/* en-US flavoured formatting, enough for UI labels without an Intl database */
static JsValue date_to_locale_date(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    DateParts p;
    char buf[48];
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    if (!split_time(js, thisv, &p))
        return js_string(js, "Invalid Date");
    snprintf(buf, sizeof(buf), "%d/%d/%d", p.mo, p.d, p.y);
    return js_string(js, buf);
}

static JsValue date_to_locale_time(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    DateParts p;
    char buf[32];
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    if (!split_time(js, thisv, &p))
        return js_string(js, "Invalid Date");
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", p.h, p.mi, p.se);
    return js_string(js, buf);
}

/* ---------------- statics ---------------- */

static JsValue date_static_now(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    HPE_UNUSED(thisv);
    HPE_UNUSED(a);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    return js_num(now_ms());
}

static JsValue date_static_parse(HpeJs *js, JsValue thisv, JsValue *a, int argc, void *ud)
{
    JsValue v = d_arg(a, argc, 0);
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    if (v.t == JS_OBJ && v.obj && v.obj->kind == JS_OBJ_DATE)
        return js_num(v.obj->num_val);
    return js_num(parse_date_string(js_to_string(js, v)));
}

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

/* local mirrors of the interpreter's builtin helpers (those are file-static) */
static JsValue d_arg(JsValue *a, int argc, int i)
{
    return i < argc ? a[i] : js_undef();
}

static void d_def(HpeJs *js, JsObj *target, const char *name, JsNativeFn fn)
{
    js_obj_set(js, target, name, js_objval(js_native_new(js, fn, NULL)));
}

/* getters/setters that behave identically in local and UTC time (no timezone
 * database here) alias the same native function object under the getUTC / setUTC
 * names */
static void def_alias(HpeJs *js, JsObj *p, const char *name, const char *src_name)
{
    JsProp *prop = js_obj_own_prop(p, src_name);
    if (prop)
        js_obj_set(js, p, name, prop->val);
}

typedef struct {
    const char *name;
    JsNativeFn fn;
} DateMethodDef;

static void date_def_methods(HpeJs *js, JsObj *p, const DateMethodDef *defs)
{
    int i;
    for (i = 0; defs[i].name; i++)
        d_def(js, p, defs[i].name, defs[i].fn);
}

static void date_def_aliases(HpeJs *js, JsObj *p, const char (*aliases)[2][24], int n)
{
    int i;
    for (i = 0; i < n; i++)
        def_alias(js, p, aliases[i][0], aliases[i][1]);
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
