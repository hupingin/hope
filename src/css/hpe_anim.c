/* Hope UI Engine - CSS transition / animation parsing and interpolation */
#include "hpe_anim.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/hpe_util.h"

/* ---------------- small token helpers ---------------- */

/**
 * @brief 跳过空白字符 / Skip whitespace characters
 */
static const char *anim_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    return p;
}

/**
 * @brief 取一个 token 的长度（括号组算一个整体）/ Measure one token (a parenthesised group counts as one)
 *
 * @details 停在顶层逗号或空白；进入 `(` 后忽略内部的逗号与空白，直到配对的 `)`，
 *          使 `cubic-bezier(0.1, 0.7, 1, 1)` 保持为单个 token。
 *          Stops at a top-level comma or whitespace; inside `(` inner commas and spaces
 *          are ignored up to the matching `)`, keeping `cubic-bezier(0.1, 0.7, 1, 1)`
 *          a single token.
 */
static int anim_word_len(const char *p)
{
    int depth = 0;
    int n = 0;
    while (p[n]) {
        char c = p[n];
        if (depth == 0 && (c == ',' || c == ' ' || c == '\t' || c == '\n' || c == '\r'))
            break;
        if (c == '(')
            depth++;
        else if (c == ')' && depth > 0)
            depth--;
        n++;
    }
    return n;
}

/**
 * @brief 大小写不敏感地比较 token 与关键字 / Compare a token with a keyword, case-insensitively
 */
static int anim_word_is(const char *p, int len, const char *kw)
{
    size_t n = strlen(kw);
    if ((int)n != len)
        return 0;
    return hpe_str_ieq_n(p, kw, n);
}

/**
 * @brief token 是否为时间值 / Test whether a token is a time value
 */
static int anim_word_is_time(const char *p, int len)
{
    int i = 0;
    if (len >= 3 && (p[len - 2] == 'm' || p[len - 2] == 'M') &&
        (p[len - 1] == 's' || p[len - 1] == 'S'))
        len -= 2;
    else if (len >= 2 && (p[len - 1] == 's' || p[len - 1] == 'S'))
        len -= 1;
    else
        return 0;
    if (p[i] == '+' || p[i] == '-')
        i++;
    if (i >= len)
        return 0;
    for (; i < len; i++) {
        if ((p[i] < '0' || p[i] > '9') && p[i] != '.')
            return 0;
    }
    return 1;
}

/* ---------------- property name mapping ---------------- */

/**
 * @brief 把 CSS 属性名映射为可插值属性 id / Map a CSS property name to an animatable property id
 *
 * @param[in] name 属性名（小写）/ Property name, lower case
 * @param[in] len 名字长度 / Name length
 *
 * @return `HpeAnimProp` id；`all` 返回 `HPE_AP_ALL`，未知返回 `HPE_AP_NONE` / An
 *         `HpeAnimProp` id; `all` yields `HPE_AP_ALL`, unknown names `HPE_AP_NONE`
 */
int hpe_anim_prop_from_name(const char *name, size_t len)
{
    static const struct {
        const char *name;
        int prop;
    } k_map[] = {
        {"all", HPE_AP_ALL},   {"none", HPE_AP_NONE},
        {"color", HPE_AP_COLOR}, {"background-color", HPE_AP_BACKGROUND_COLOR},
        {"border-color", HPE_AP_BORDER_COLOR}, {"opacity", HPE_AP_OPACITY},
        {"width", HPE_AP_WIDTH}, {"height", HPE_AP_HEIGHT},
        {"margin-top", HPE_AP_MARGIN_TOP}, {"margin-right", HPE_AP_MARGIN_RIGHT},
        {"margin-bottom", HPE_AP_MARGIN_BOTTOM}, {"margin-left", HPE_AP_MARGIN_LEFT},
        {"padding-top", HPE_AP_PADDING_TOP}, {"padding-right", HPE_AP_PADDING_RIGHT},
        {"padding-bottom", HPE_AP_PADDING_BOTTOM}, {"padding-left", HPE_AP_PADDING_LEFT},
        {"top", HPE_AP_INSET_TOP}, {"right", HPE_AP_INSET_RIGHT},
        {"bottom", HPE_AP_INSET_BOTTOM}, {"left", HPE_AP_INSET_LEFT},
        {"inset-top", HPE_AP_INSET_TOP}, {"inset-right", HPE_AP_INSET_RIGHT},
        {"inset-bottom", HPE_AP_INSET_BOTTOM}, {"inset-left", HPE_AP_INSET_LEFT},
        {"font-size", HPE_AP_FONT_SIZE}, {"letter-spacing", HPE_AP_LETTER_SPACING},
        {"line-height", HPE_AP_LINE_HEIGHT}, {"font-weight", HPE_AP_FONT_WEIGHT},
        {"border-radius", HPE_AP_BORDER_RADIUS}, {"transform", HPE_AP_TRANSFORM},
    };
    size_t i;

    for (i = 0; i < sizeof(k_map) / sizeof(k_map[0]); i++) {
        if (strlen(k_map[i].name) == len && hpe_str_ieq_n(name, k_map[i].name, len))
            return k_map[i].prop;
    }
    return HPE_AP_NONE;
}

/**
 * @brief 判断属性是否影响几何 / Test whether a property affects geometry
 *
 * @details 宽高、内外边距、inset、字号、行高、字距与圆角在布局期消费，动画进行中需要
 *           重建盒树；颜色、透明度、字重与 transform 只需重算样式重绘。
 *           Width/height, margins, padding, insets, font-size, line-height,
 *           letter-spacing and border-radius are consumed during layout, so an active
 *           animation on them needs a box-tree rebuild; colours, opacity, font-weight
 *           and transform only need a restyle.
 */
int hpe_anim_prop_is_layout(int prop)
{
    switch (prop) {
    case HPE_AP_WIDTH:
    case HPE_AP_HEIGHT:
    case HPE_AP_MARGIN_TOP:
    case HPE_AP_MARGIN_RIGHT:
    case HPE_AP_MARGIN_BOTTOM:
    case HPE_AP_MARGIN_LEFT:
    case HPE_AP_PADDING_TOP:
    case HPE_AP_PADDING_RIGHT:
    case HPE_AP_PADDING_BOTTOM:
    case HPE_AP_PADDING_LEFT:
    case HPE_AP_INSET_TOP:
    case HPE_AP_INSET_RIGHT:
    case HPE_AP_INSET_BOTTOM:
    case HPE_AP_INSET_LEFT:
    case HPE_AP_FONT_SIZE:
    case HPE_AP_LETTER_SPACING:
    case HPE_AP_LINE_HEIGHT:
    case HPE_AP_BORDER_RADIUS:
        return 1;
    default:
        return 0;
    }
}

/* ---------------- easing ---------------- */

/**
 * @brief 求三次贝塞尔 x(t) / Evaluate a cubic bezier x(t)
 */
static float bezier_axis(float x1, float x2, float t)
{
    float u = 1.0f - t;
    return u * u * u * x1 * 0.0f + 3.0f * u * u * t * x1 + 3.0f * u * t * t * x2 + t * t * t;
}

/**
 * @brief 解贝塞尔参数 t 使 x(t)=x / Solve the bezier parameter t with x(t) = x
 *
 * @details 先牛顿迭代 8 次，未收敛时二分 32 次（x 单调，安全）。
 *          8 Newton iterations first, then a 32-step bisection (x is monotonic, so this
 *          is safe) when Newton fails to converge.
 */
static float bezier_solve_t(float x1, float x2, float x)
{
    float t = x;
    int i;
    for (i = 0; i < 8; i++) {
        float cx = bezier_axis(x1, x2, t) - x;
        float d = 3.0f * (1.0f - t) * (1.0f - t) * x1 + 6.0f * (1.0f - t) * t * (x2 - x1) +
                  3.0f * t * t * (1.0f - x2);
        if (fabsf(cx) < 1e-5f)
            return t;
        if (fabsf(d) < 1e-6f)
            break;
        t -= cx / d;
        if (t < 0.0f)
            t = 0.0f;
        if (t > 1.0f)
            t = 1.0f;
    }
    {
        float lo = 0.0f, hi = 1.0f;
        for (i = 0; i < 32; i++) {
            float mid = (lo + hi) * 0.5f;
            if (bezier_axis(x1, x2, mid) < x)
                lo = mid;
            else
                hi = mid;
        }
        return (lo + hi) * 0.5f;
    }
}

/**
 * @brief 求缓动函数在 t 处的进度 / Evaluate an easing function at t
 *
 * @details 关键字展开为预置贝塞尔曲线；`steps` 把 t 量化为 n 段（start 规则用左端点）；
 *           cubic-bezier 先解 t 再代 y。
 *           Keywords expand to preset bezier curves; `steps` quantises t into n
 *           intervals (the start rule uses the left edge); cubic-bezier solves t then
 *           evaluates y.
 */
float hpe_easing_solve(const HpeEasing *e, float t)
{
    static const HpeEasing k_presets[] = {
        {HPE_EASING_LINEAR, 0, 0, 0, 0, 0, 0},
        {HPE_EASING_CUBIC, 0.25f, 0.1f, 0.25f, 1.0f, 0, 0},     /* ease */
        {HPE_EASING_CUBIC, 0.42f, 0.0f, 1.0f, 1.0f, 0, 0},      /* ease-in */
        {HPE_EASING_CUBIC, 0.0f, 0.0f, 0.58f, 1.0f, 0, 0},      /* ease-out */
        {HPE_EASING_CUBIC, 0.42f, 0.0f, 0.58f, 1.0f, 0, 0},     /* ease-in-out */
    };

    if (e->kind == HPE_EASING_STEPS) {
        /* jump-start already holds the first step at t=0; both directions land
         * exactly on 0/1 at the domain ends */
        int n = e->steps > 0 ? e->steps : 1;
        if (t >= 1.0f)
            return 1.0f;
        if (t <= 0.0f)
            return e->steps_start ? 1.0f / (float)n : 0.0f;
    }
    if (t <= 0.0f)
        return 0.0f;
    if (t >= 1.0f)
        return 1.0f;
    if (e->kind == HPE_EASING_LINEAR)
        return t;
    if (e->kind == HPE_EASING_STEPS) {
        int n = e->steps > 0 ? e->steps : 1;
        float q = t * (float)n;
        if (e->steps_start)
            return ceilf(q - 1e-4f) / (float)n;
        return floorf(q) / (float)n;
    }
    if (e->kind == HPE_EASING_CUBIC)
        return bezier_axis(e->y1, e->y2, bezier_solve_t(e->x1, e->x2, t));
    return hpe_easing_solve(&k_presets[e->kind], t);
}

/**
 * @brief 解析缓动函数 / Parse an easing function
 *
 * @details 依次尝试五个关键字、`cubic-bezier(x1,y1,x2,y2)` 与 `steps(n, start|end)`；
 *          未知值回退 `ease`。
 *          The five keywords are tried first, then `cubic-bezier(x1,y1,x2,y2)` and
 *          `steps(n, start|end)`; unknown values fall back to `ease`.
 */
void hpe_anim_parse_easing(const char *v, HpeEasing *out)
{
    memset(out, 0, sizeof(*out));
    out->kind = HPE_EASING_EASE;
    if (!v)
        return;
    if (anim_word_is(v, (int)strlen(v), "linear")) {
        out->kind = HPE_EASING_LINEAR;
    } else if (anim_word_is(v, (int)strlen(v), "ease-in")) {
        out->kind = HPE_EASING_EASE_IN;
    } else if (anim_word_is(v, (int)strlen(v), "ease-out")) {
        out->kind = HPE_EASING_EASE_OUT;
    } else if (anim_word_is(v, (int)strlen(v), "ease-in-out")) {
        out->kind = HPE_EASING_EASE_IN_OUT;
    } else if (anim_word_is(v, (int)strlen(v), "ease")) {
        out->kind = HPE_EASING_EASE;
    } else if (!strncmp(v, "cubic-bezier(", 13)) {
        out->kind = HPE_EASING_CUBIC;
        if (sscanf(v + 13, "%f , %f , %f , %f", &out->x1, &out->y1, &out->x2, &out->y2) != 4)
            out->kind = HPE_EASING_EASE;
    } else if (!strncmp(v, "steps(", 6)) {
        char tail[16] = "end";
        size_t tn;
        out->kind = HPE_EASING_STEPS;
        out->steps = 1;
        if (sscanf(v + 6, "%d , %15s", &out->steps, tail) < 1)
            out->steps = 1;
        /* `%s` captures the closing paren (and maybe `;`): strip them */
        tn = strlen(tail);
        while (tn > 0 && (tail[tn - 1] == ')' || tail[tn - 1] == ';'))
            tail[--tn] = '\0';
        out->steps_start = anim_word_is(tail, (int)tn, "start");
    }
}

/**
 * @brief 把时间值解析成秒 / Parse a CSS time value into seconds
 */
void hpe_anim_parse_time(const char *v, float *out)
{
    char tmp[32];
    size_t n;
    *out = 0.0f;
    if (!v || !*v)
        return;
    n = (size_t)anim_word_len(v);
    if (n >= sizeof(tmp))
        return;
    memcpy(tmp, v, n);
    tmp[n] = '\0';
    *out = (float)atof(tmp);
    /* `500ms` -> 0.5s; a bare unitless number counts as seconds */
    if (n >= 2 && tmp[n - 1] == 's' && (tmp[n - 2] == 'm' || tmp[n - 2] == 'M'))
        *out /= 1000.0f;
}

/* ---------------- transition parsing ---------------- */

/**
 * @brief 把一个 transition 列表项写进样式槽位 / Write one transition list item into a style slot
 */
static void trans_store(HpeStyle *d, int i, int prop, float dur, float delay,
                        const HpeEasing *ease)
{
    d->transitions[i].prop = prop;
    d->transitions[i].duration = dur;
    d->transitions[i].delay = delay;
    d->transitions[i].easing = *ease;
    if (i >= d->transition_count)
        d->transition_count = i + 1;
    d->set2 |= HPE_PROP2_TRANSITION;
}

/**
 * @brief 把当前 token 复制进缓冲区并加 NUL / Copy the current token into a NUL-terminated buffer
 *
 * @return 写入的字符数 / Number of characters written
 */
static int anim_tok(const char *q, int wl, char *buf, int cap)
{
    int n = wl < cap - 1 ? wl : cap - 1;
    memcpy(buf, q, (size_t)n);
    buf[n] = '\0';
    return n;
}

/**
 * @brief 把时间 token 依次填进 duration 再 delay / Fill a time token into duration then delay
 */
static void anim_tok_time(const char *q, int wl, int *times, float *dur, float *delay)
{
    char buf[32];
    float sec;
    anim_tok(q, wl, buf, (int)sizeof(buf));
    hpe_anim_parse_time(buf, &sec);
    if (*times == 0)
        *dur = sec;
    else
        *delay = sec;
    (*times)++;
}

/**
 * @brief 处理 transition 项里的缓动函数括号组 / Handle an easing function group in a transition item
 */
static void trans_tok_ease_group(const char *q, int wl, HpeEasing *ease)
{
    char buf[96];
    anim_tok(q, wl, buf, (int)sizeof(buf));
    hpe_anim_parse_easing(buf, ease);
}

/**
 * @brief 处理 transition 项里的普通标识符 / Handle a plain identifier in a transition item
 *
 * @details 先试缓动关键字（它们不可能是属性名），再查动画属性名表。
 *          Easing keywords are tried first (they cannot be property names), then the
 *          animated property name table.
 */
static void trans_tok_ident(const char *q, int wl, HpeEasing *ease, int *prop)
{
    char buf[48];
    int n = anim_tok(q, wl, buf, (int)sizeof(buf));
    if (anim_word_is(buf, n, "linear") || anim_word_is(buf, n, "ease") ||
        anim_word_is(buf, n, "ease-in") || anim_word_is(buf, n, "ease-out") ||
        anim_word_is(buf, n, "ease-in-out"))
        hpe_anim_parse_easing(buf, ease);
    else {
        int id = hpe_anim_prop_from_name(buf, (size_t)n);
        if (id != HPE_AP_NONE)
            *prop = id;
    }
}

/**
 * @brief 解析 transition 列表的一项 / Parse one item of a transition list
 *
 * @details 扫描项内 token：时间第一个为 duration、第二个为 delay；缓动关键字与
 *          `cubic-bezier(` / `steps(` 开头的括号组为 timing function；其余标识符查
 *          属性名表。
 *          Tokens inside the item are scanned: the first time is the duration and the
 *          second the delay; easing keywords and `cubic-bezier(` / `steps(` groups set
 *          the timing function; any remaining identifier is looked up in the property
 *          name table.
 */
static int trans_item(const char *p, int len, HpeTransition *t)
{
    const char *end = p + len;
    const char *q = p;
    int seen = 0, times = 0;
    float dur = 0.0f, delay = 0.0f;
    HpeEasing ease;
    int prop = HPE_AP_ALL;

    hpe_anim_parse_easing("ease", &ease);
    while (q < end) {
        int wl;
        q = anim_ws(q);
        if (q >= end || *q == ',')
            break;
        wl = anim_word_len(q);
        if (wl > len)
            break;
        if (anim_word_is_time(q, wl))
            anim_tok_time(q, wl, &times, &dur, &delay);
        else if (wl > 0 && (hpe_str_ieq_n(q, "cubic", 5) || hpe_str_ieq_n(q, "steps", 5)))
            trans_tok_ease_group(q, wl, &ease);
        else if (!strchr(q, '('))
            trans_tok_ident(q, wl, &ease, &prop);
        seen = 1;
        q += wl;
    }
    if (!seen)
        return 0;
    t->prop = prop;
    t->duration = dur;
    t->delay = delay;
    t->easing = ease;
    return 1;
}

/**
 * @brief 取下一个顶层逗号分隔的段 / Take the next top-level comma-separated segment
 *
 * @details 括号深度感知：`cubic-bezier(0.1, 0.7, 1, 1)` 内部的逗号不算分隔符。
 *          Parenthesis-depth aware: commas inside `cubic-bezier(0.1, 0.7, 1, 1)` are not
 *          separators.
 *
 * @param[in] p 段起点 / Segment start
 *
 * @return 段末尾（顶层逗号或串尾）/ Segment end (top-level comma or end of string)
 */
static const char *anim_next_segment(const char *p)
{
    int depth = 0;
    while (*p) {
        if (*p == ',' && depth == 0)
            break;
        if (*p == '(')
            depth++;
        else if (*p == ')' && depth > 0)
            depth--;
        p++;
    }
    return p;
}

/**
 * @brief 解析 transition 简写 / Parse the `transition` shorthand
 */
void hpe_anim_parse_transition_shorthand(const char *value, HpeStyle *d)
{
    const char *p = anim_ws(value);
    int slot = 0;

    d->transition_count = 0;
    d->set2 |= HPE_PROP2_TRANSITION;
    if (!strcmp(value, "none"))
        return;
    while (*p && slot < HPE_TRANS_MAX) {
        const char *seg = anim_next_segment(p);
        HpeTransition t;
        if (seg > p && trans_item(p, (int)(seg - p), &t))
            trans_store(d, slot++, t.prop, t.duration, t.delay, &t.easing);
        p = (*seg == ',') ? anim_ws(seg + 1) : seg;
    }
}

/**
 * @brief 解析 transition 长属性 / Parse a `transition-*` longhand
 */
void hpe_anim_parse_transition_longhand(const char *prop, const char *value, HpeStyle *d)
{
    const char *p = anim_ws(value);
    int slot = 0;
    int is_prop = !strcmp(prop, "transition-property");
    int is_dur = !strcmp(prop, "transition-duration");
    int is_ease = !strcmp(prop, "transition-timing-function");
    int is_delay = !strcmp(prop, "transition-delay");

    d->set2 |= HPE_PROP2_TRANSITION;
    if (is_prop && !strcmp(value, "none")) {
        d->transition_count = 0;
        return;
    }
    while (*p && slot < HPE_TRANS_MAX) {
        const char *seg = anim_next_segment(p);
        char buf[96];
        int n;
        if (seg == p) {
            p = (*seg == ',') ? anim_ws(seg + 1) : seg;
            continue;
        }
        n = (int)(seg - p);
        n = n < (int)sizeof(buf) - 1 ? n : (int)sizeof(buf) - 1;
        memcpy(buf, p, (size_t)n);
        buf[n] = '\0';
        if (is_dur || is_delay) {
            float sec;
            hpe_anim_parse_time(buf, &sec);
            if (is_dur)
                d->transitions[slot].duration = sec;
            else
                d->transitions[slot].delay = sec;
        } else if (is_ease) {
            hpe_anim_parse_easing(buf, &d->transitions[slot].easing);
        } else {
            int id = hpe_anim_prop_from_name(buf, (size_t)n);
            d->transitions[slot].prop = id;
        }
        if (slot >= d->transition_count)
            d->transition_count = slot + 1;
        slot++;
        p = (*seg == ',') ? anim_ws(seg + 1) : seg;
    }
}

/* ---------------- animation parsing ---------------- */

/**
 * @brief 把一个 animation 列表项写进样式槽位 / Write one animation list item into a style slot
 */
static void anim_store(HpeStyle *d, int i, const HpeAnimDecl *a)
{
    d->anims[i] = *a;
    if (i >= d->anim_count)
        d->anim_count = i + 1;
    d->set2 |= HPE_PROP2_ANIMATION;
}

/**
 * @brief 构造一个默认的 animation 声明 / Build a default animation declaration
 */
static HpeAnimDecl anim_decl_default(void)
{
    HpeAnimDecl a;
    memset(&a, 0, sizeof(a));
    hpe_anim_parse_easing("ease", &a.easing);
    a.duration = 0.0f;
    a.iter_count = 1.0f;
    a.direction = HPE_ANIM_DIR_NORMAL;
    a.fill = HPE_ANIM_FILL_NONE;
    return a;
}

/**
 * @brief 匹配 direction 关键字 / Match a direction keyword
 */
static int anim_kw_dir(const char *buf, int n, HpeAnimDecl *a)
{
    if (anim_word_is(buf, n, "normal"))
        a->direction = HPE_ANIM_DIR_NORMAL;
    else if (anim_word_is(buf, n, "reverse"))
        a->direction = HPE_ANIM_DIR_REVERSE;
    else if (anim_word_is(buf, n, "alternate"))
        a->direction = HPE_ANIM_DIR_ALTERNATE;
    else if (anim_word_is(buf, n, "alternate-reverse"))
        a->direction = HPE_ANIM_DIR_ALTERNATE_REVERSE;
    else
        return 0;
    return 1;
}

/**
 * @brief 匹配 fill-mode 关键字 / Match a fill-mode keyword
 */
static int anim_kw_fill(const char *buf, int n, HpeAnimDecl *a)
{
    if (anim_word_is(buf, n, "forwards"))
        a->fill = HPE_ANIM_FILL_FORWARDS;
    else if (anim_word_is(buf, n, "backwards"))
        a->fill = HPE_ANIM_FILL_BACKWARDS;
    else if (anim_word_is(buf, n, "both"))
        a->fill = HPE_ANIM_FILL_BOTH;
    else
        return 0;
    return 1;
}

/**
 * @brief 匹配 `none` / 缓动关键字 / play-state / 动画名 / Match `none`, easing keywords, play-state or the animation name
 *
 * @details `running` 是默认值、`paused` 已解析但播放暂停尚未实现；剩余标识符作为
 *          @keyframes 名（最多一个）。
 *          `running` is the default and `paused` is parsed but playback pause is not
 *          implemented yet; any leftover identifier becomes the @keyframes name (one at most).
 */
static int anim_kw_misc(const char *buf, int n, HpeAnimDecl *a, int *name_set)
{
    if (anim_word_is(buf, n, "none") && !*name_set) {
        a->name[0] = '\0';
        *name_set = 1;
    } else if (anim_word_is(buf, n, "linear") || anim_word_is(buf, n, "ease") ||
               anim_word_is(buf, n, "ease-in") || anim_word_is(buf, n, "ease-out") ||
               anim_word_is(buf, n, "ease-in-out") || anim_word_is(buf, n, "running") ||
               anim_word_is(buf, n, "paused")) {
        if (anim_word_is(buf, n, "linear"))
            a->easing.kind = HPE_EASING_LINEAR;
    } else if (!*name_set) {
        snprintf(a->name, sizeof(a->name), "%s", buf);
        *name_set = 1;
    } else
        return 0;
    return 1;
}

/**
 * @brief 处理 animation 项里的一个非时间 token / Handle one non-time token of an animation item
 */
static void anim_kw(const char *buf, int n, HpeAnimDecl *a, int *name_set)
{
    if (hpe_str_ieq_n(buf, "cubic", 5) || hpe_str_ieq_n(buf, "steps", 5))
        hpe_anim_parse_easing(buf, &a->easing);
    else if (strchr(buf, '('))
        ; /* an unknown function group: ignore (play-state is not a function) */
    else if (anim_word_is(buf, n, "infinite"))
        a->iter_infinite = 1;
    else if (buf[0] >= '0' && buf[0] <= '9') {
        a->iter_infinite = 0;
        a->iter_count = (float)atof(buf);
    } else if (!anim_kw_dir(buf, n, a) && !anim_kw_fill(buf, n, a))
        anim_kw_misc(buf, n, a, name_set);
}

/**
 * @brief 解析 animation 列表的一项 / Parse one item of an animation list
 *
 * @details 与 transition 项相同的 token 扫描；额外识别 `infinite`、纯数字（迭代次数）、
 *          direction / fill-mode / play-state 关键字；最后剩下的标识符作为 @keyframes 名。
 *          Token scanning matches the transition item; additionally `infinite` and bare
 *          numbers are the iteration count, direction / fill-mode / play-state keywords
 *          slot in, and the last leftover identifier becomes the @keyframes name.
 */
static int anim_item(const char *p, int len, HpeAnimDecl *a)
{
    const char *end = p + len;
    const char *q = p;
    int seen = 0, times = 0, name_set = 0;

    *a = anim_decl_default();
    while (q < end) {
        int wl;
        q = anim_ws(q);
        if (q >= end || *q == ',')
            break;
        wl = anim_word_len(q);
        if (wl > len)
            break;
        if (anim_word_is_time(q, wl)) {
            anim_tok_time(q, wl, &times, &a->duration, &a->delay);
        } else if (wl > 0 && *q != '"') {
            char buf[96];
            int n = anim_tok(q, wl, buf, (int)sizeof(buf));
            anim_kw(buf, n, a, &name_set);
        }
        seen = 1;
        q += wl;
    }
    return seen;
}

/**
 * @brief 解析 animation 简写 / Parse the `animation` shorthand
 */
void hpe_anim_parse_animation_shorthand(const char *value, HpeStyle *d)
{
    const char *p = anim_ws(value);
    int slot = 0;

    d->anim_count = 0;
    d->set2 |= HPE_PROP2_ANIMATION;
    if (!strcmp(value, "none"))
        return;
    while (*p && slot < HPE_ANIM_MAX) {
        const char *seg = anim_next_segment(p);
        HpeAnimDecl a;
        if (seg > p && anim_item(p, (int)(seg - p), &a))
            anim_store(d, slot++, &a);
        p = (*seg == ',') ? anim_ws(seg + 1) : seg;
    }
}

/**
 * @brief animation-name 长属性：逗号列表直接映射到槽位 / `animation-name` longhand: comma list maps to slots
 */
static void anim_parse_names(const char *value, HpeStyle *d)
{
    const char *p = anim_ws(value);
    int slot = 0;

    d->anim_count = 0;
    while (*p && slot < HPE_ANIM_MAX) {
        int wl = anim_word_len(p);
        if (wl == 0 || *p == ',') {
            p++;
            continue;
        }
        if (!(wl == 4 && hpe_str_ieq_n(p, "none", 4))) {
            int n = wl < HPE_ANIM_NAME_MAX - 1 ? wl : HPE_ANIM_NAME_MAX - 1;
            memcpy(d->anims[slot].name, p, (size_t)n);
            d->anims[slot].name[n] = '\0';
        } else {
            d->anims[slot].name[0] = '\0';
        }
        if (slot >= d->anim_count)
            d->anim_count = slot + 1;
        slot++;
        p = anim_ws(p + wl);
        if (*p == ',')
            p = anim_ws(p + 1);
    }
}

/**
 * @brief 把一个长属性 token 写进槽位声明 / Write one longhand token into a slot declaration
 */
static void anim_long_token(const char *prop, const char *buf, int n, HpeAnimDecl *a)
{
    if (!strcmp(prop, "animation-duration")) {
        hpe_anim_parse_time(buf, &a->duration);
    } else if (!strcmp(prop, "animation-delay")) {
        hpe_anim_parse_time(buf, &a->delay);
    } else if (!strcmp(prop, "animation-timing-function")) {
        hpe_anim_parse_easing(buf, &a->easing);
    } else if (!strcmp(prop, "animation-iteration-count")) {
        a->iter_infinite = anim_word_is(buf, n, "infinite");
        if (!a->iter_infinite)
            a->iter_count = (float)atof(buf);
    } else if (!strcmp(prop, "animation-direction")) {
        a->direction = anim_word_is(buf, n, "reverse")
                           ? HPE_ANIM_DIR_REVERSE
                           : anim_word_is(buf, n, "alternate")
                                 ? HPE_ANIM_DIR_ALTERNATE
                                 : anim_word_is(buf, n, "alternate-reverse")
                                       ? HPE_ANIM_DIR_ALTERNATE_REVERSE
                                       : HPE_ANIM_DIR_NORMAL;
    } else if (!strcmp(prop, "animation-fill-mode")) {
        a->fill = anim_word_is(buf, n, "forwards")
                      ? HPE_ANIM_FILL_FORWARDS
                      : anim_word_is(buf, n, "backwards")
                            ? HPE_ANIM_FILL_BACKWARDS
                            : anim_word_is(buf, n, "both") ? HPE_ANIM_FILL_BOTH
                                                           : HPE_ANIM_FILL_NONE;
    }
}

/**
 * @brief 解析 animation 长属性 / Parse an `animation-*` longhand
 */
void hpe_anim_parse_animation_longhand(const char *prop, const char *value, HpeStyle *d)
{
    const char *p = anim_ws(value);
    int slot = 0;

    d->set2 |= HPE_PROP2_ANIMATION;
    if (!strcmp(prop, "animation-name")) {
        anim_parse_names(value, d);
        return;
    }
    while (*p && slot < HPE_ANIM_MAX) {
        int wl = anim_word_len(p);
        char buf[96];
        int n;
        if (wl == 0 || *p == ',') {
            p++;
            continue;
        }
        n = anim_tok(p, wl, buf, (int)sizeof(buf));
        anim_long_token(prop, buf, n, &d->anims[slot]);
        if (slot >= d->anim_count)
            d->anim_count = slot + 1;
        slot++;
        p = anim_ws(p + wl);
        if (*p == ',')
            p = anim_ws(p + 1);
    }
}

/* ---------------- @keyframes lookup ---------------- */

/**
 * @brief 按名查找样式表中的 @keyframes / Find @keyframes by name in a stylesheet
 */
HpeKeyframes *hpe_anim_find_keyframes(HpeStylesheet *sheet, const char *name)
{
    int i;
    if (!sheet)
        return NULL;
    for (i = 0; i < sheet->kf_count; i++) {
        if (!strcmp(sheet->kf[i].name, name))
            return &sheet->kf[i];
    }
    return NULL;
}

/**
 * @brief 按名找（或新建）一条空的 @keyframes / Find or create an empty @keyframes slot
 */
HpeKeyframes *hpe_anim_sheet_keyframes_slot(HpeStylesheet *sheet, const char *name)
{
    HpeKeyframes *kf = hpe_anim_find_keyframes(sheet, name);
    if (kf) {
        /* a later definition replaces the earlier one wholesale */
        int i;
        for (i = 0; i < kf->stop_count; i++) {
            if (kf->stops[i].decl)
                hpe_free(kf->stops[i].decl);
            kf->stops[i].decl = NULL;
        }
        kf->stop_count = 0;
        return kf;
    }
    sheet->kf = (HpeKeyframes *)hpe_grow(sheet->kf, &sheet->kf_cap,
                                         (size_t)sheet->kf_count + 1, sizeof(HpeKeyframes));
    if (!sheet->kf)
        return NULL;
    kf = &sheet->kf[sheet->kf_count++];
    memset(kf, 0, sizeof(*kf));
    snprintf(kf->name, sizeof(kf->name), "%s", name);
    return kf;
}

/* ---------------- value get / set / lerp ---------------- */

/**
 * @brief 判断两个长度是否可互相插值（同单位且都非 auto）/ Test whether two lengths can interpolate
 */
static int len_lerpable(const HpeLength *a, const HpeLength *b)
{
    return a->unit != HPE_LEN_AUTO && b->unit == a->unit;
}

/**
 * @brief 从计算样式取一个可插值属性的当前值 / Extract one animatable property's current value
 *
 * @details 按属性 id 把对应成员装入值快照；长度类属性为 `auto` 时 `valid` 置 0（不可插值）。
 *          The matching member is packed into the value snapshot per property id; length
 *          properties whose value is `auto` report `valid` 0 (not interpolable).
 */
int hpe_anim_value_get(const HpeStyle *s, int prop, HpeAnimValue *out)
{
    memset(out, 0, sizeof(*out));
    out->prop = prop;
    out->valid = 1;
    switch (prop) {
    case HPE_AP_COLOR: out->type = HPE_AV_COLOR; out->color = s->color; return 1;
    case HPE_AP_BACKGROUND_COLOR:
        out->type = HPE_AV_COLOR;
        out->color = s->background;
        return 1;
    case HPE_AP_BORDER_COLOR:
        out->type = HPE_AV_COLOR4;
        memcpy(out->colors, s->border_colors, sizeof(out->colors));
        return 1;
    case HPE_AP_OPACITY: out->type = HPE_AV_FLOAT; out->f = s->opacity; return 1;
    case HPE_AP_WIDTH: out->type = HPE_AV_LEN; out->len = s->width; break;
    case HPE_AP_HEIGHT: out->type = HPE_AV_LEN; out->len = s->height; break;
    case HPE_AP_MARGIN_TOP: case HPE_AP_MARGIN_RIGHT:
    case HPE_AP_MARGIN_BOTTOM: case HPE_AP_MARGIN_LEFT:
        out->type = HPE_AV_LEN;
        out->len = s->margin[prop - HPE_AP_MARGIN_TOP];
        break;
    case HPE_AP_PADDING_TOP: case HPE_AP_PADDING_RIGHT:
    case HPE_AP_PADDING_BOTTOM: case HPE_AP_PADDING_LEFT:
        out->type = HPE_AV_LEN;
        out->len = s->padding[prop - HPE_AP_PADDING_TOP];
        break;
    case HPE_AP_INSET_TOP: case HPE_AP_INSET_RIGHT:
    case HPE_AP_INSET_BOTTOM: case HPE_AP_INSET_LEFT:
        out->type = HPE_AV_LEN;
        out->len = s->inset[prop - HPE_AP_INSET_TOP];
        break;
    case HPE_AP_FONT_SIZE: out->type = HPE_AV_LEN; out->len = hpe_len_px(s->font_size); return 1;
    case HPE_AP_LETTER_SPACING: out->type = HPE_AV_LEN; out->len = s->letter_spacing; break;
    case HPE_AP_LINE_HEIGHT: out->type = HPE_AV_FLOAT; out->f = s->line_height; return 1;
    case HPE_AP_FONT_WEIGHT: out->type = HPE_AV_FLOAT; out->f = (float)s->font_weight; return 1;
    case HPE_AP_BORDER_RADIUS:
        out->type = HPE_AV_LEN;
        out->len = hpe_len_px(s->border_radius);
        return 1;
    case HPE_AP_TRANSFORM: out->type = HPE_AV_TRANSFORM; out->tf = s->transform; return 1;
    default:
        out->valid = 0;
        return 0;
    }
    out->valid = len_lerpable(&out->len, &out->len);
    return 1;
}

/**
 * @brief 把插值结果写回计算样式 / Write an interpolated value back into a computed style
 */
void hpe_anim_value_set(HpeStyle *s, int prop, const HpeAnimValue *v)
{
    switch (prop) {
    case HPE_AP_COLOR: s->color = v->color; return;
    case HPE_AP_BACKGROUND_COLOR: s->background = v->color; return;
    case HPE_AP_BORDER_COLOR:
        memcpy(s->border_colors, v->colors, sizeof(s->border_colors));
        return;
    case HPE_AP_OPACITY: s->opacity = v->f; return;
    case HPE_AP_WIDTH: s->width = v->len; return;
    case HPE_AP_HEIGHT: s->height = v->len; return;
    case HPE_AP_MARGIN_TOP: case HPE_AP_MARGIN_RIGHT:
    case HPE_AP_MARGIN_BOTTOM: case HPE_AP_MARGIN_LEFT:
        s->margin[prop - HPE_AP_MARGIN_TOP] = v->len;
        return;
    case HPE_AP_PADDING_TOP: case HPE_AP_PADDING_RIGHT:
    case HPE_AP_PADDING_BOTTOM: case HPE_AP_PADDING_LEFT:
        s->padding[prop - HPE_AP_PADDING_TOP] = v->len;
        return;
    case HPE_AP_INSET_TOP: case HPE_AP_INSET_RIGHT:
    case HPE_AP_INSET_BOTTOM: case HPE_AP_INSET_LEFT:
        s->inset[prop - HPE_AP_INSET_TOP] = v->len;
        return;
    case HPE_AP_FONT_SIZE: s->font_size = v->len.value; return;
    case HPE_AP_LETTER_SPACING: s->letter_spacing = v->len; return;
    case HPE_AP_LINE_HEIGHT: s->line_height = v->f; return;
    case HPE_AP_FONT_WEIGHT: s->font_weight = (int)(v->f + 0.5f); return;
    case HPE_AP_BORDER_RADIUS: s->border_radius = v->len.value; return;
    case HPE_AP_TRANSFORM: s->transform = v->tf; return;
    default: return;
    }
}

/**
 * @brief 插值颜色与四边颜色 / Interpolate colours and four-side colours
 */
static void value_lerp_colors(const HpeAnimValue *a, const HpeAnimValue *b, float t,
                              HpeAnimValue *out)
{
    int i;
    out->color.r = (uint8_t)(a->color.r + (b->color.r - a->color.r) * t);
    out->color.g = (uint8_t)(a->color.g + (b->color.g - a->color.g) * t);
    out->color.b = (uint8_t)(a->color.b + (b->color.b - a->color.b) * t);
    out->color.a = (uint8_t)(a->color.a + (b->color.a - a->color.a) * t);
    for (i = 0; i < 4; i++) {
        out->colors[i].r = (uint8_t)(a->colors[i].r + (b->colors[i].r - a->colors[i].r) * t);
        out->colors[i].g = (uint8_t)(a->colors[i].g + (b->colors[i].g - a->colors[i].g) * t);
        out->colors[i].b = (uint8_t)(a->colors[i].b + (b->colors[i].b - a->colors[i].b) * t);
        out->colors[i].a = (uint8_t)(a->colors[i].a + (b->colors[i].a - a->colors[i].a) * t);
    }
}

/**
 * @brief 把 2D 仿射矩阵分解为平移/旋转/缩放 / Decompose a 2D affine matrix into translation / rotation / scale
 *
 * @details 行列式退化或存在明显斜切（c 与 -b 不符）时返回 0，调用方回退离散切换。
 *          Returns 0 on a degenerate determinant or visible skew (c not matching -b), so
 *          the caller falls back to a discrete swap.
 */
static int transform_decompose(const HpeTransform *tf, float *tx, float *ty, float *angle,
                               float *sx, float *sy)
{
    float det = tf->a * tf->d - tf->b * tf->c;
    float a = tf->a, b = tf->b;
    if (fabsf(a) < 1e-6f && fabsf(b) < 1e-6f)
        return 0;
    if (det < 1e-6f)
        return 0;
    if (fabsf(tf->c + tf->b) > 1e-3f)
        return 0; /* skew present: fall back to discrete */
    if (fabsf(a) < fabsf(b)) {
        float tmp = a;
        a = b;
        b = tmp;
    }
    *sx = sqrtf(a * a + b * b);
    *angle = atan2f(b, a);
    *sy = det / *sx;
    *tx = tf->e;
    *ty = tf->f;
    return 1;
}

/**
 * @brief 用平移/旋转/缩放重建仿射矩阵 / Rebuild an affine matrix from translation / rotation / scale
 */
static HpeTransform transform_compose(float tx, float ty, float angle, float sx, float sy)
{
    HpeTransform tf;
    float c = cosf(angle), s = sinf(angle);
    tf.a = c * sx;
    tf.b = s * sx;
    tf.c = -s * sy;
    tf.d = c * sy;
    tf.e = tx;
    tf.f = ty;
    tf.identity = fabsf(tf.a - 1.0f) < 1e-5f && fabsf(tf.b) < 1e-5f &&
                  fabsf(tf.c) < 1e-5f && fabsf(tf.d - 1.0f) < 1e-5f &&
                  fabsf(tf.e) < 1e-5f && fabsf(tf.f) < 1e-5f;
    return tf;
}

/**
 * @brief 插值两个 transform（分解插值，退化时离散）/ Interpolate two transforms (decomposed, discrete on degeneracy)
 */
static void value_lerp_transform(const HpeAnimValue *a, const HpeAnimValue *b, float t,
                                 HpeAnimValue *out)
{
    float tx1, ty1, an1, sx1, sy1, tx2, ty2, an2, sx2, sy2;

    if (transform_decompose(&a->tf, &tx1, &ty1, &an1, &sx1, &sy1) &&
        transform_decompose(&b->tf, &tx2, &ty2, &an2, &sx2, &sy2)) {
        float da = an2 - an1;
        while (da > 3.14159265f)
            da -= 6.28318531f;
        while (da < -3.14159265f)
            da += 6.28318531f;
        out->tf = transform_compose(tx1 + (tx2 - tx1) * t, ty1 + (ty2 - ty1) * t,
                                    an1 + da * t, sx1 + (sx2 - sx1) * t,
                                    sy1 + (sy2 - sy1) * t);
    } else if (t < 0.5f) {
        out->tf = a->tf; /* non-interpolable pair: discrete swap at the midpoint */
    } else {
        out->tf = b->tf;
    }
}

/**
 * @brief 在两个值之间插值 / Interpolate between two values
 */
int hpe_anim_value_lerp(const HpeAnimValue *a, const HpeAnimValue *b, float t, HpeAnimValue *out)
{
    if (!a->valid || !b->valid || a->type != b->type)
        return 0;
    *out = *b;
    switch (a->type) {
    case HPE_AV_LEN:
        if (a->len.unit != b->len.unit)
            return 0;
        out->len.value = a->len.value + (b->len.value - a->len.value) * t;
        return 1;
    case HPE_AV_COLOR:
    case HPE_AV_COLOR4:
        value_lerp_colors(a, b, t, out);
        return 1;
    case HPE_AV_FLOAT:
        out->f = a->f + (b->f - a->f) * t;
        return 1;
    case HPE_AV_TRANSFORM:
        value_lerp_transform(a, b, t, out);
        return 1;
    default:
        return 0;
    }
}
