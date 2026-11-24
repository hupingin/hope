#include "css/hpe_css.h"

#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* forward declarations used by the public selector-matching entry point */
static int selector_matches(const HpeSelector *sel, const HpeDomNode *node);
/* recursive stylesheet walker; @media blocks re-enter it with a deeper stack */
static void css_parse_into(HpeStylesheet *sheet, const char *css, size_t length,
                           HpeMediaQuery *mq_stack, int mq_depth);

/* ---------------- defaults ---------------- */

/*
 * Box-model longhands: lengths, per-side defaults and the identity transform.
 * Those whose CSS initial value is already zero are left to the caller's
 * memset, so only non-zero initials are written here.
 */
static void style_init_box(HpeStyle *s)
{
    int i;

    s->background = hpe_rgba(0, 0, 0, 0);
    s->border_color = hpe_rgba(0, 0, 0, 0);
    s->width = hpe_len_auto();
    s->height = hpe_len_auto();
    s->min_width = hpe_len_auto();
    s->min_height = hpe_len_auto();
    s->max_width = hpe_len_auto();
    s->max_height = hpe_len_auto();
    for (i = 0; i < 4; i++) {
        s->margin[i] = hpe_len_px(0.0f);
        s->padding[i] = hpe_len_px(0.0f);
        s->border_colors[i] = hpe_rgba(0, 0, 0, 0);
        s->inset[i] = hpe_len_auto();
    }
    s->letter_spacing = hpe_len_px(0.0f);
    s->word_spacing = hpe_len_px(0.0f);
    s->text_indent = hpe_len_px(0.0f);
    /* 2-D identity: the off-diagonals stay zero from the memset above */
    s->transform.a = 1.0f;
    s->transform.d = 1.0f;
    s->transform.identity = 1;
}

/* enum- and scalar-valued longhands whose CSS initial value is not zero */
static void style_init_scalars(HpeStyle *s)
{
    s->color = hpe_rgb(0, 0, 0);
    s->display = HPE_DISPLAY_INLINE;
    s->font_size = 16.0f;   /* CSS `medium` */
    s->font_weight = 400;   /* CSS `normal` */
    s->line_height = 0.0f;  /* 0 means `normal` */
    s->family = HPE_FONT_DEFAULT;
    s->flex_dir = HPE_FLEX_ROW;
    s->align_items = HPE_ALIGN_ITEMS_STRETCH;
    s->justify = HPE_JUSTIFY_START;
    s->align_self = HPE_ALIGN_ITEMS_AUTO;
    s->text_align = HPE_ALIGN_LEFT;
    s->white_space = HPE_WS_NORMAL;
    s->position = HPE_POS_STATIC;
    s->z_auto = 1;
    s->box_sizing = HPE_BOX_SIZING_CONTENT;
    s->overflow_x = HPE_OVERFLOW_VISIBLE;
    s->overflow_y = HPE_OVERFLOW_VISIBLE;
    s->opacity = 1.0f;
    s->visibility = HPE_VISIBILITY_VISIBLE;
    s->cursor = HPE_CURSOR_AUTO;
    s->text_decoration = HPE_TD_NONE;
    s->vertical_align = HPE_VALIGN_BASELINE;
    s->list_style_type = HPE_LIST_DISC;
    s->list_style_position = HPE_LIST_OUTSIDE;
    s->background_image.kind = HPE_BG_NONE;
}

HpeStyle hpe_style_default(void)
{
    HpeStyle s;

    memset(&s, 0, sizeof(s));
    style_init_box(&s);
    style_init_scalars(&s);
    return s;
}

/* ---------------- value helpers ---------------- */

static int split_ws(char *s, char **out, int max)
{
    int n = 0;
    int depth = 0;
    char *p = s;
    while (*p) {
        while (*p && hpe_is_space(*p))
            p++;
        if (!*p)
            break;
        if (n < max)
            out[n++] = p;
        /* keep `func(a, b, c)` whitespace inside the parens as one token */
        depth = 0;
        while (*p) {
            if (*p == '(')
                depth++;
            else if (*p == ')') {
                if (depth > 0)
                    depth--;
            } else if (hpe_is_space(*p) && depth <= 0) {
                break;
            }
            p++;
        }
        if (*p)
            *p++ = '\0';
    }
    return n;
}

static int parse_plain_number(const char *s, float *out)
{
    char *end = NULL;
    float v;
    if (!s || !*s)
        return 0;
    v = (float)strtod(s, &end);
    if (end == s)
        return 0;
    while (end && *end && hpe_is_space(*end))
        end++;
    if (end && *end)
        return 0;
    *out = v;
    return 1;
}

static void set_sides(HpeStyle *d, const char *value, int is_margin){
    char buf[256];
    char *parts[4];
    int n, i;
    static const uint64_t bits[4][4] = {
        {HPE_PROP_MARGIN_TOP, HPE_PROP_MARGIN_RIGHT, HPE_PROP_MARGIN_BOTTOM, HPE_PROP_MARGIN_LEFT},
        {HPE_PROP_PADDING_TOP, HPE_PROP_PADDING_RIGHT, HPE_PROP_PADDING_BOTTOM,
         HPE_PROP_PADDING_LEFT},
        {0, 0, 0, 0},
        {0, 0, 0, 0}};
    HpeLength vals[4];

    snprintf(buf, sizeof(buf), "%s", value ? value : "0");
    n = split_ws(buf, parts, 4);
    if (n <= 0)
        return;
    for (i = 0; i < 4; i++) {
        const char *v = parts[i < n ? i : n - 1];
        vals[i] = hpe_len_parse(v);
    }
    if (n < 4) {
        /* expand: 1 value -> all, 2 -> tb/lr, 3 -> t/lr/b */
        HpeLength t = vals[0];
        HpeLength r = n > 1 ? vals[1] : t;
        HpeLength b = n > 2 ? vals[2] : t;
        HpeLength l = n > 1 ? vals[1] : t;
        vals[0] = t;
        vals[1] = r;
        vals[2] = b;
        vals[3] = l;
    }
    for (i = 0; i < 4; i++) {
        if (is_margin)
            d->margin[i] = vals[i];
        else
            d->padding[i] = vals[i];
        d->set |= bits[is_margin ? 0 : 1][i];
    }
}

/* ---------------- gradient / shadow / transform helpers ---------------- */

/* parses a hex number-ish angle/number token; returns 1 on success */
static int parse_float_token(const char *s, float *out)
{
    char *end = NULL;
    float v;
    if (!s || !*s)
        return 0;
    v = (float)strtod(s, &end);
    if (end == s)
        return 0;
    while (*end && hpe_is_space(*end))
        end++;
    if (*end && strcmp(end, "deg") != 0 && strcmp(end, "px") != 0 &&
        strcmp(end, "%") != 0 && strcmp(end, "rad") != 0 && strcmp(end, "turn") != 0)
        return 0;
    if (*end == '\0')
        ; /* plain number */
    else if (!strcmp(end, "rad"))
        v = v * 180.0f / 3.14159265f;
    else if (!strcmp(end, "turn"))
        v = v * 360.0f;
    *out = v;
    return 1;
}

/* splits `body` on commas at paren-depth 0 (colour functions carry their own
 * commas); writes up to `max` segment pointers and returns how many it found */
static int split_top_level(char *body, char **seg, int max)
{
    int depth = 0;
    int nseg = 0;
    char *start = body;
    char *q;

    for (q = body; *q; q++) {
        if (*q == '(')
            depth++;
        else if (*q == ')')
            depth--;
        else if (*q == ',' && depth == 0) {
            *q = '\0';
            if (nseg < max)
                seg[nseg++] = start;
            start = q + 1;
        }
    }
    if (nseg < max)
        seg[nseg++] = start;
    return nseg;
}

/* maps a `to <side>` keyword or an angle token to degrees; returns 1 when the
 * token was a direction (so it must not also be treated as a colour stop) */
static int gradient_direction(const char *tok, float *angle)
{
    if (hpe_starts_with_i(tok, "to ")) {
        char *dir = hpe_trim((char *)tok + 3);
        if (hpe_str_ieq(dir, "top"))
            *angle = 0.0f;
        else if (hpe_str_ieq(dir, "bottom"))
            *angle = 180.0f;
        else if (hpe_str_ieq(dir, "left"))
            *angle = 270.0f;
        else if (hpe_str_ieq(dir, "right"))
            *angle = 90.0f;
        else if (hpe_str_ieq(dir, "top right") || hpe_str_ieq(dir, "right top"))
            *angle = 45.0f;
        else if (hpe_str_ieq(dir, "bottom right") || hpe_str_ieq(dir, "right bottom"))
            *angle = 135.0f;
        else if (hpe_str_ieq(dir, "top left") || hpe_str_ieq(dir, "left top"))
            *angle = 315.0f;
        else if (hpe_str_ieq(dir, "bottom left") || hpe_str_ieq(dir, "left bottom"))
            *angle = 225.0f;
        return 1;
    }
    {
        float a;
        if (parse_float_token(tok, &a) &&
            (strstr(tok, "deg") || strstr(tok, "rad") || strstr(tok, "turn"))) {
            *angle = a;
            return 1;
        }
    }
    return 0;
}

/* parses `<color> [<pos>]`; returns 1 when the token held a colour */
static int parse_gradient_stop(const char *tok, HpeColor *c, float *pos)
{
    char buf[128];
    char *sp;
    float pv;

    *pos = -1.0f;
    snprintf(buf, sizeof(buf), "%s", tok);
    sp = buf;
    while (*sp && !hpe_is_space(*sp))
        sp++;
    if (*sp) {
        *sp = '\0';
        if (parse_float_token(hpe_trim(sp + 1), &pv))
            *pos = (pv > 1.0f || strstr(hpe_trim(sp + 1), "%")) ? pv / 100.0f : pv;
    }
    return hpe_color_parse(hpe_trim(buf), c);
}

/* fills any unset stop positions by even distribution across [0,1] */
static void distribute_gradient_stops(HpeBackground *bg, int count)
{
    int i;
    if (bg->stops[0].pos < 0.0f)
        bg->stops[0].pos = 0.0f;
    if (bg->stops[count - 1].pos < 0.0f)
        bg->stops[count - 1].pos = 1.0f;
    for (i = 1; i < count - 1; i++) {
        if (bg->stops[i].pos < 0.0f)
            bg->stops[i].pos = (float)i / (float)(count - 1);
    }
}

/* parses a `linear-gradient(...)` body into stops; returns the stop count */
static int collect_gradient_stops(char *body, HpeBackground *bg, float *angle)
{
    char *seg[16];
    int nseg = split_top_level(body, seg, 16);
    int count = 0;
    int i;

    for (i = 0; i < nseg; i++) {
        char *tok = hpe_trim(seg[i]);
        HpeColor c;
        float pos;
        if (!*tok)
            continue;
        if (gradient_direction(tok, angle))
            continue;
        if (!parse_gradient_stop(tok, &c, &pos))
            continue;
        if (count < HPE_MAX_GRADIENT_STOPS) {
            bg->stops[count].color = c;
            bg->stops[count].pos = pos;
            count++;
        }
    }
    return count;
}

/* parses `linear-gradient(...)`; only the 2-stop / n-stop colour-list form with
 * an optional leading direction (`to right`, `to bottom`, `Ndeg`) is supported. */
static int parse_linear_gradient(const char *value, HpeBackground *bg)
{
    const char *lp, *rp;
    int count;
    float angle = 180.0f; /* css default: to bottom */

    bg->kind = HPE_BG_NONE;
    bg->stop_count = 0;
    bg->angle_deg = 180.0f;
    if (!value)
        return 0;
    while (*value && hpe_is_space(*value))
        value++;
    if (!hpe_starts_with_i(value, "linear-gradient"))
        return 0;
    lp = strchr(value, '(');
    if (!lp)
        return 0;
    rp = strrchr(lp, ')');
    if (!rp || rp <= lp)
        return 0;

    {
        char *body = hpe_strndup(lp + 1, (size_t)(rp - lp - 1));
        count = collect_gradient_stops(body, bg, &angle);
        hpe_free(body);
    }
    if (count < 2)
        return 0;
    distribute_gradient_stops(bg, count);
    bg->kind = HPE_BG_LINEAR_GRADIENT;
    bg->angle_deg = angle;
    bg->stop_count = count;
    return 1;
}

/* parses `[ox oy blur spread] color` (colour may lead or trail) */
static int parse_shadow(const char *value, HpeShadow *out)
{
    char buf[256];
    char *parts[8];
    float nums[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    int nnums = 0;
    HpeColor c = hpe_rgba(0, 0, 0, 160);
    int n, i, got_color = 0;

    memset(out, 0, sizeof(*out));
    if (!value)
        return 0;
    snprintf(buf, sizeof(buf), "%s", value);
    n = split_ws(buf, parts, 8);
    for (i = 0; i < n; i++) {
        HpeColor cc;
        float v;
        if (hpe_color_parse(parts[i], &cc)) {
            c = cc;
            got_color = 1;
        } else if (parse_float_token(parts[i], &v) && nnums < 4) {
            nums[nnums++] = v;
        }
    }
    if (nnums < 2)
        return 0;
    HPE_UNUSED(got_color);
    out->ox = nums[0];
    out->oy = nums[1];
    out->blur = nums[2];
    out->spread = nums[3];
    out->color = c;
    out->valid = 1;
    return 1;
}

static void mat_mul(HpeTransform *dst, const HpeTransform *m, const HpeTransform *n)
{
    /* dst = m * n (apply n then m) */
    float a = m->a * n->a + m->c * n->b;
    float b = m->b * n->a + m->d * n->b;
    float c = m->a * n->c + m->c * n->d;
    float d = m->b * n->c + m->d * n->d;
    float e = m->a * n->e + m->c * n->f + m->e;
    float f = m->b * n->e + m->d * n->f + m->f;
    dst->a = a;
    dst->b = b;
    dst->c = c;
    dst->d = d;
    dst->e = e;
    dst->f = f;
    dst->identity = (a == 1.0f && b == 0.0f && c == 0.0f && d == 1.0f && e == 0.0f && f == 0.0f);
}

/* builds the affine matrix for one transform function name + its two args */
static void transform_from_fn(const char *fn, float v1, float v2, HpeTransform *m)
{
    m->a = 1.0f;
    m->b = 0.0f;
    m->c = 0.0f;
    m->d = 1.0f;
    m->e = 0.0f;
    m->f = 0.0f;

    if (!strcmp(fn, "translate"))
        m->e = v1, m->f = v2;
    else if (!strcmp(fn, "translatex"))
        m->e = v1;
    else if (!strcmp(fn, "translatey"))
        m->f = v1;
    else if (!strcmp(fn, "scale"))
        m->a = m->d = v1, m->b = m->c = 0.0f, m->e = m->f = 0.0f;
    else if (!strcmp(fn, "scalex"))
        m->a = v1;
    else if (!strcmp(fn, "scaley"))
        m->d = v1;
    else if (!strcmp(fn, "rotate")) {
        float r = v1 * 3.14159265f / 180.0f;
        m->a = cosf(r);
        m->b = sinf(r);
        m->c = -sinf(r);
        m->d = cosf(r);
    } else if (!strcmp(fn, "skewx")) {
        float r = v1 * 3.14159265f / 180.0f;
        m->c = tanf(r);
    } else if (!strcmp(fn, "skewy")) {
        float r = v1 * 3.14159265f / 180.0f;
        m->b = tanf(r);
    }
    /* "matrix" is not supported: leave the identity */
    m->identity =
        (m->a == 1.0f && m->b == 0.0f && m->c == 0.0f && m->d == 1.0f && m->e == 0.0f &&
         m->f == 0.0f);
}

/* applies one `name(args)` transform to the accumulator */
static void apply_transform_fn(const char *fn, const char *body, HpeTransform *acc)
{
    char *copy = hpe_strdup(body);
    float v1 = 0.0f, v2 = 0.0f;
    char *comma = strchr(copy, ',');
    HpeTransform m;

    if (comma) {
        *comma = '\0';
        parse_float_token(hpe_trim(comma + 1), &v2);
    }
    parse_float_token(hpe_trim(copy), &v1);
    transform_from_fn(fn, v1, v2, &m);
    if (!m.identity)
        mat_mul(acc, acc, &m);
    hpe_free(copy);
}

/* parses a comma/space separated transform function list; only 2d functions */
static void parse_transform(const char *value, HpeTransform *out)
{
    HpeTransform acc;
    const char *p = value;

    acc.a = 1.0f;
    acc.b = 0.0f;
    acc.c = 0.0f;
    acc.d = 1.0f;
    acc.e = 0.0f;
    acc.f = 0.0f;
    acc.identity = 1;
    if (!value || !strcmp(value, "none")) {
        *out = acc;
        return;
    }
    while (*p) {
        const char *lp;
        const char *rp;
        char fn[32];
        size_t flen;
        size_t i;

        while (*p && (hpe_is_space(*p) || *p == ','))
            p++;
        if (!*p)
            break;
        lp = strchr(p, '(');
        if (!lp)
            break;
        rp = strchr(lp, ')');
        if (!rp)
            break;
        flen = (size_t)(lp - p);
        if (flen >= sizeof(fn))
            flen = sizeof(fn) - 1;
        memcpy(fn, p, flen);
        fn[flen] = '\0';
        for (i = 0; fn[i]; i++)
            fn[i] = (char)hpe_ascii_lower((unsigned char)fn[i]);
        {
            char *body = hpe_strndup(lp + 1, (size_t)(rp - lp - 1));
            apply_transform_fn(fn, body, &acc);
            hpe_free(body);
        }
        p = rp + 1;
    }
    *out = acc;
}

/* ---------------- property appliers ----------------
 * Each applier writes one property (or one shorthand family) into a style.
 * They are registered by name in k_prop_table and dispatched by apply_property,
 * so adding a property is a data change, and every applier stays small enough
 * to be read and reasoned about on its own. */

/* split `value` on whitespace and return the last token that parses as a colour
 * (handles shorthands such as `1px solid #ccc` where the colour is trailing). */
static int last_color_token(const char *value, HpeColor *out)
{
    char buf[256];
    char *parts[8];
    int n, i;

    snprintf(buf, sizeof(buf), "%s", value);
    n = split_ws(buf, parts, 8);
    for (i = n - 1; i >= 0; i--) {
        if (hpe_color_parse(parts[i], out))
            return 1;
    }
    return 0;
}

/* CSS expands 1..4 border-colour values by repetition (T R B L) */
static void expand_border_colors(HpeStyle *d, const HpeColor *vals, int got)
{
    static const int pick4[4] = {0, 1, 2, 3};
    static const int pick3[4] = {0, 1, 2, 1};
    static const int pick2[4] = {0, 1, 0, 1};
    static const int pick1[4] = {0, 0, 0, 0};
    const int *pick = got == 1 ? pick1 : (got == 2 ? pick2 : (got == 3 ? pick3 : pick4));
    int i;

    for (i = 0; i < 4; i++) {
        d->border_colors[i] = vals[pick[i]];
        d->set2 |= HPE_PROP2_BORDER_TOP_COLOR << i;
    }
    d->border_color = vals[0]; /* keep the shorthand default in sync */
    d->set |= HPE_PROP_BORDER_COLOR;
}

/* width applies to every side when `side` < 0, otherwise to that side only */
static void set_border_width(HpeStyle *d, float w, int side, uint64_t side_bit)
{
    if (side >= 0) {
        d->border_w[side] = w;
        d->set |= side_bit;
    } else {
        int k;
        for (k = 0; k < 4; k++)
            d->border_w[k] = w;
        d->set |= HPE_PROP_BORDER_WIDTH;
    }
}

static HpeFontFamily classify_font_family(const char *name)
{
    if (strstr(name, "mono") || !strcmp(name, "courier") || strstr(name, "consolas"))
        return HPE_FONT_MONO;
    if (strstr(name, "serif") || strstr(name, "times") || strstr(name, "georgia") ||
        strstr(name, "simsun") || strstr(name, "song") || strstr(name, "宋"))
        return HPE_FONT_SERIF;
    if (strstr(name, "sans") || strstr(name, "yahei") || strstr(name, "arial") ||
        strstr(name, "helvetica") || strstr(name, "segoe") || strstr(name, "黑") ||
        strstr(name, "微软雅黑") || strstr(name, "hei"))
        return HPE_FONT_SANS;
    return HPE_FONT_DEFAULT;
}

/* strips surrounding quotes and lowercases a family token in place */
static void normalize_family_name(char *name)
{
    size_t len = strlen(name);
    size_t j;

    if (len >= 2 && name[0] == '"' && name[len - 1] == '"') {
        memmove(name, name + 1, len - 2);
        name[len - 2] = '\0';
        len -= 2;
    }
    for (j = 0; name[j]; j++)
        name[j] = (char)hpe_ascii_lower((unsigned char)name[j]);
}

static HpeOverflow overflow_keyword(const char *tok)
{
    if (!strcmp(tok, "hidden"))
        return HPE_OVERFLOW_HIDDEN;
    if (!strcmp(tok, "scroll"))
        return HPE_OVERFLOW_SCROLL;
    if (!strcmp(tok, "auto"))
        return HPE_OVERFLOW_AUTO;
    return HPE_OVERFLOW_VISIBLE;
}

/* ---------------- appliers ---------------- */

static void apply_display(HpeStyle *d, const char *value)
{
    if (!strcmp(value, "none"))
        d->display = HPE_DISPLAY_NONE;
    else if (!strcmp(value, "inline-block"))
        d->display = HPE_DISPLAY_INLINE_BLOCK;
    else if (!strcmp(value, "flex") || !strcmp(value, "inline-flex"))
        /* inline-flex is blockified: the engine has no inline-level flex container */
        d->display = HPE_DISPLAY_FLEX;
    else if (!strcmp(value, "inline"))
        d->display = HPE_DISPLAY_INLINE;
    else
        d->display = HPE_DISPLAY_BLOCK;
    d->set |= HPE_PROP_DISPLAY;
}

static void apply_color(HpeStyle *d, const char *value)
{
    HpeColor c;
    if (hpe_color_parse(value, &c)) {
        d->color = c;
        d->set |= HPE_PROP_COLOR;
    }
}

static void apply_background(HpeStyle *d, const char *value)
{
    HpeColor c;

    /* `background: linear-gradient(...)` also sets the image layer */
    if (parse_linear_gradient(value, &d->background_image)) {
        d->set2 |= HPE_PROP2_BACKGROUND_IMAGE;
        return;
    }
    if (!strcmp(value, "none") || !strcmp(value, "transparent")) {
        d->background = hpe_rgba(0, 0, 0, 0);
        d->set |= HPE_PROP_BACKGROUND;
        return;
    }
    /* try the whole value first: functional notations such as
       `rgba(217, 48, 37, 0.45)` contain spaces and must not be split */
    if (hpe_color_parse(value, &c) || last_color_token(value, &c)) {
        d->background = c;
        d->set |= HPE_PROP_BACKGROUND;
    }
}

static void apply_min_width(HpeStyle *d, const char *value)
{
    HpeLength len = hpe_len_parse(value);
    if (len.unit == HPE_LEN_PX || len.unit == HPE_LEN_PCT) {
        d->min_width = len;
        d->set |= HPE_PROP_MIN_WIDTH;
    }
}

static void apply_width(HpeStyle *d, const char *value)
{
    d->width = hpe_len_parse(value);
    d->set |= HPE_PROP_WIDTH;
}

static void apply_height(HpeStyle *d, const char *value)
{
    d->height = hpe_len_parse(value);
    d->set |= HPE_PROP_HEIGHT;
}

static void apply_margin(HpeStyle *d, const char *value)
{
    set_sides(d, value, 1);
}

static void apply_padding(HpeStyle *d, const char *value)
{
    set_sides(d, value, 0);
}

/* one side of the margin/padding shorthand as a longhand setting */
static void apply_box_side(HpeStyle *d, const char *value, int idx, int is_margin)
{
    HpeLength len = hpe_len_parse(value);
    if (is_margin) {
        d->margin[idx] = len;
        d->set |= HPE_PROP_MARGIN_TOP << idx;
    } else {
        d->padding[idx] = len;
        d->set |= HPE_PROP_PADDING_TOP << idx;
    }
}

static void apply_margin_top(HpeStyle *d, const char *v) { apply_box_side(d, v, 0, 1); }
static void apply_margin_right(HpeStyle *d, const char *v) { apply_box_side(d, v, 1, 1); }
static void apply_margin_bottom(HpeStyle *d, const char *v) { apply_box_side(d, v, 2, 1); }
static void apply_margin_left(HpeStyle *d, const char *v) { apply_box_side(d, v, 3, 1); }
static void apply_padding_top(HpeStyle *d, const char *v) { apply_box_side(d, v, 0, 0); }
static void apply_padding_right(HpeStyle *d, const char *v) { apply_box_side(d, v, 1, 0); }
static void apply_padding_bottom(HpeStyle *d, const char *v) { apply_box_side(d, v, 2, 0); }
static void apply_padding_left(HpeStyle *d, const char *v) { apply_box_side(d, v, 3, 0); }

/* `border-color: c1 [c2 [c3 [c4]]]` — top, right, bottom, left */
static void apply_border_color(HpeStyle *d, const char *value)
{
    char buf[256];
    char *parts[4];
    HpeColor vals[4];
    int n, i, got = 0;

    snprintf(buf, sizeof(buf), "%s", value);
    n = split_ws(buf, parts, 4);
    for (i = 0; i < n; i++) {
        if (hpe_color_parse(parts[i], &vals[got]))
            got++;
    }
    if (got > 0)
        expand_border_colors(d, vals, got);
}

/* `border` / `border-width` / `border-<side>`: width, style and colour in any
 * order; `side` < 0 means all four sides, `side_bit` is the side's width flag */
static void apply_border_side(HpeStyle *d, const char *value, int side, uint64_t side_bit)
{
    char buf[256];
    char *parts[8];
    int n, i;

    snprintf(buf, sizeof(buf), "%s", value);
    n = split_ws(buf, parts, 8);
    for (i = 0; i < n; i++) {
        HpeColor c;
        HpeLength len;

        if (!strcmp(parts[i], "none")) {
            /* `border: none` / `border-top: none` -> zero width on that side */
            set_border_width(d, 0.0f, side, side_bit);
            continue;
        }
        if (!strcmp(parts[i], "solid") || !strcmp(parts[i], "dashed") ||
            !strcmp(parts[i], "dotted") || !strcmp(parts[i], "double"))
            continue;
        if (hpe_color_parse(parts[i], &c)) {
            if (side >= 0) {
                /* a side shorthand sets only that side */
                d->border_colors[side] = c;
                d->set2 |= HPE_PROP2_BORDER_TOP_COLOR << side;
            } else {
                int k;
                d->border_color = c;
                for (k = 0; k < 4; k++)
                    d->border_colors[k] = c;
                d->set |= HPE_PROP_BORDER_COLOR;
            }
            continue;
        }
        len = hpe_len_parse(parts[i]);
        if (len.unit == HPE_LEN_PX)
            set_border_width(d, len.value, side, side_bit);
    }
}

static void apply_border(HpeStyle *d, const char *v) { apply_border_side(d, v, -1, 0); }
static void apply_border_width(HpeStyle *d, const char *v) { apply_border_side(d, v, -1, 0); }
static void apply_border_top(HpeStyle *d, const char *v) { apply_border_side(d, v, 0, HPE_PROP_BORDER_TOP); }
static void apply_border_right(HpeStyle *d, const char *v) { apply_border_side(d, v, 1, HPE_PROP_BORDER_RIGHT); }
static void apply_border_bottom(HpeStyle *d, const char *v) { apply_border_side(d, v, 2, HPE_PROP_BORDER_BOTTOM); }
static void apply_border_left(HpeStyle *d, const char *v) { apply_border_side(d, v, 3, HPE_PROP_BORDER_LEFT); }

static void apply_border_radius(HpeStyle *d, const char *value)
{
    HpeLength len = hpe_len_parse(value);

    if (len.unit == HPE_LEN_PX) {
        d->border_radius = len.value;
        d->set |= HPE_PROP_BORDER_RADIUS;
    } else if (len.unit == HPE_LEN_PCT) {
        /* percentages need the border box: stored negated, solved at paint time */
        d->border_radius = -len.value;
        d->set |= HPE_PROP_BORDER_RADIUS;
    }
}

static void apply_inset(HpeStyle *d, const char *value)
{
    char buf[256];
    char *parts[4];
    HpeLength vals[4];
    int n, i;

    snprintf(buf, sizeof(buf), "%s", value);
    n = split_ws(buf, parts, 4);
    if (n <= 0)
        return;
    for (i = 0; i < n && i < 4; i++)
        vals[i] = hpe_len_parse(parts[i]);
    if (n < 4) {
        HpeLength t = vals[0];
        HpeLength r = n > 1 ? vals[1] : t;
        HpeLength b = n > 2 ? vals[2] : t;
        HpeLength l = n > 1 ? vals[1] : t;
        vals[0] = t;
        vals[1] = r;
        vals[2] = b;
        vals[3] = l;
    }
    for (i = 0; i < 4; i++) {
        d->inset[i] = vals[i];
        d->set |= HPE_PROP_INSET_TOP << i;
    }
}

static void apply_flex_direction(HpeStyle *d, const char *value)
{
    d->flex_dir = !strcmp(value, "column") ? HPE_FLEX_COLUMN : HPE_FLEX_ROW;
    d->set |= HPE_PROP_FLEX_DIR;
}

static void apply_align_items(HpeStyle *d, const char *value)
{
    if (!strcmp(value, "center"))
        d->align_items = HPE_ALIGN_ITEMS_CENTER;
    else if (!strcmp(value, "flex-start") || !strcmp(value, "start"))
        d->align_items = HPE_ALIGN_ITEMS_FLEX_START;
    else if (!strcmp(value, "flex-end") || !strcmp(value, "end"))
        d->align_items = HPE_ALIGN_ITEMS_FLEX_END;
    else if (!strcmp(value, "baseline"))
        d->align_items = HPE_ALIGN_ITEMS_BASELINE;
    else
        d->align_items = HPE_ALIGN_ITEMS_STRETCH;
    d->set |= HPE_PROP_ALIGN_ITEMS;
}

static void apply_align_self(HpeStyle *d, const char *value)
{
    if (!strcmp(value, "center"))
        d->align_self = HPE_ALIGN_ITEMS_CENTER;
    else if (!strcmp(value, "flex-start") || !strcmp(value, "start"))
        d->align_self = HPE_ALIGN_ITEMS_FLEX_START;
    else if (!strcmp(value, "flex-end") || !strcmp(value, "end"))
        d->align_self = HPE_ALIGN_ITEMS_FLEX_END;
    else if (!strcmp(value, "stretch"))
        d->align_self = HPE_ALIGN_ITEMS_STRETCH;
    else
        d->align_self = HPE_ALIGN_ITEMS_AUTO;
    d->set |= HPE_PROP_ALIGN_SELF;
}

static void apply_justify(HpeStyle *d, const char *value)
{
    if (!strcmp(value, "center"))
        d->justify = HPE_JUSTIFY_CENTER;
    else if (!strcmp(value, "flex-end") || !strcmp(value, "end"))
        d->justify = HPE_JUSTIFY_END;
    else if (!strcmp(value, "space-between"))
        d->justify = HPE_JUSTIFY_SPACE_BETWEEN;
    else if (!strcmp(value, "space-around"))
        d->justify = HPE_JUSTIFY_SPACE_AROUND;
    else
        d->justify = HPE_JUSTIFY_START;
    d->set |= HPE_PROP_JUSTIFY;
}

static void apply_gap(HpeStyle *d, const char *value)
{
    HpeLength len = hpe_len_parse(value);
    d->gap = (len.unit == HPE_LEN_PX || len.unit == HPE_LEN_PCT) ? len.value : 0.0f;
    d->set |= HPE_PROP_GAP;
}

static void apply_flex_grow(HpeStyle *d, const char *value)
{
    HpeLength len = hpe_len_parse(value);
    d->flex_grow = (len.unit == HPE_LEN_PX || len.unit == HPE_LEN_PCT) ? len.value : 0.0f;
    d->set |= HPE_PROP_FLEX_GROW;
}

static void apply_font_size(HpeStyle *d, const char *value)
{
    float v = 0.0f;

    if (!strcmp(value, "small"))
        v = 13.0f;
    else if (!strcmp(value, "medium"))
        v = 16.0f;
    else if (!strcmp(value, "large"))
        v = 18.0f;
    else if (!strcmp(value, "x-large"))
        v = 24.0f;
    else if (!strcmp(value, "xx-large"))
        v = 32.0f;
    else if (!strcmp(value, "smaller"))
        v = 13.0f;
    else if (!strcmp(value, "larger"))
        v = 19.0f;
    else {
        HpeLength len = hpe_len_parse(value);
        if (len.unit == HPE_LEN_PCT || len.unit == HPE_LEN_EM)
            v = 16.0f * len.value / 100.0f; /* resolved against the parent (16px default) */
        else if (len.unit == HPE_LEN_PX)
            v = len.value;
        else if (len.unit == HPE_LEN_REM)
            v = 16.0f * len.value; /* the root font size is 16px by default */
    }
    if (v > 0.0f) {
        d->font_size = v;
        d->set |= HPE_PROP_FONT_SIZE;
    }
}

static void apply_font_weight(HpeStyle *d, const char *value)
{
    float v;
    int w = 400;

    if (!strcmp(value, "bold"))
        w = 700;
    else if (!strcmp(value, "normal"))
        w = 400;
    else if (parse_plain_number(value, &v))
        w = (int)v;
    d->font_weight = w >= 600 ? 700 : 400;
    d->set |= HPE_PROP_FONT_WEIGHT;
}

static void apply_text_align(HpeStyle *d, const char *value)
{
    if (!strcmp(value, "center"))
        d->text_align = HPE_ALIGN_CENTER;
    else if (!strcmp(value, "right"))
        d->text_align = HPE_ALIGN_RIGHT;
    else
        d->text_align = HPE_ALIGN_LEFT;
    d->set |= HPE_PROP_TEXT_ALIGN;
}

static void apply_line_height(HpeStyle *d, const char *value)
{
    float v;
    HpeLength len;

    if (!strcmp(value, "normal")) {
        d->line_height = 0.0f;
        d->set |= HPE_PROP_LINE_HEIGHT;
        return;
    }
    if (parse_plain_number(value, &v)) {
        d->line_height = -v; /* unitless: multiplier of the font size */
        d->set |= HPE_PROP_LINE_HEIGHT;
        return;
    }
    len = hpe_len_parse(value);
    if (len.unit == HPE_LEN_PX) {
        d->line_height = len.value;
        d->set |= HPE_PROP_LINE_HEIGHT;
    }
}

static void apply_white_space(HpeStyle *d, const char *value)
{
    if (!strcmp(value, "nowrap"))
        d->white_space = HPE_WS_NOWRAP;
    else if (!strcmp(value, "pre"))
        d->white_space = HPE_WS_PRE;
    else if (!strcmp(value, "pre-wrap"))
        d->white_space = HPE_WS_PRE_WRAP;
    else if (!strcmp(value, "pre-line"))
        d->white_space = HPE_WS_PRE_LINE;
    else
        d->white_space = HPE_WS_NORMAL;
    d->set |= HPE_PROP_WHITE_SPACE;
}

static void apply_font_family(HpeStyle *d, const char *value)
{
    char buf[256];
    HpeFontFamily fam = HPE_FONT_DEFAULT;
    char *p;

    /* take the first recognized family in the comma list */
    snprintf(buf, sizeof(buf), "%s", value);
    p = buf;
    while (*p) {
        char name[128];
        char *q = strchr(p, ',');
        char *t;
        size_t tl;
        HpeFontFamily got;

        if (q)
            *q = '\0';
        t = hpe_trim(p);
        tl = strlen(t);
        if (tl >= sizeof(name))
            tl = sizeof(name) - 1;
        memcpy(name, t, tl);
        name[tl] = '\0';
        normalize_family_name(name);
        got = classify_font_family(name);
        if (got != HPE_FONT_DEFAULT)
            fam = got;
        if (q) {
            p = q + 1;
            if (fam != HPE_FONT_DEFAULT)
                break;
        } else {
            if (fam == HPE_FONT_DEFAULT)
                fam = HPE_FONT_SANS;
            break;
        }
    }
    d->family = fam;
    d->set |= HPE_PROP_FONT_FAMILY;
}

static void apply_position(HpeStyle *d, const char *value)
{
    if (!strcmp(value, "relative"))
        d->position = HPE_POS_RELATIVE;
    else if (!strcmp(value, "absolute"))
        d->position = HPE_POS_ABSOLUTE;
    else if (!strcmp(value, "fixed"))
        d->position = HPE_POS_FIXED;
    else
        d->position = HPE_POS_STATIC;
    d->set |= HPE_PROP_POSITION;
}

/* top/right/bottom/left: a single inset side */
static void apply_inset_side(HpeStyle *d, const char *value, int idx, uint32_t bit)
{
    d->inset[idx] = hpe_len_parse(value);
    d->set |= bit;
}

static void apply_top(HpeStyle *d, const char *v) { apply_inset_side(d, v, 0, HPE_PROP_INSET_TOP); }
static void apply_right(HpeStyle *d, const char *v) { apply_inset_side(d, v, 1, HPE_PROP_INSET_RIGHT); }
static void apply_bottom(HpeStyle *d, const char *v) { apply_inset_side(d, v, 2, HPE_PROP_INSET_BOTTOM); }
static void apply_left(HpeStyle *d, const char *v) { apply_inset_side(d, v, 3, HPE_PROP_INSET_LEFT); }

static void apply_z_index(HpeStyle *d, const char *value)
{
    if (!strcmp(value, "auto")) {
        d->z_auto = 1;
        d->z_index = 0;
    } else {
        float v;
        if (parse_plain_number(value, &v)) {
            d->z_auto = 0;
            d->z_index = (int)v;
        }
    }
    d->set |= HPE_PROP_Z_INDEX;
}

static void apply_float(HpeStyle *d, const char *value)
{
    if (!strcmp(value, "left"))
        d->float_side = HPE_FLOAT_LEFT;
    else if (!strcmp(value, "right"))
        d->float_side = HPE_FLOAT_RIGHT;
    else
        d->float_side = HPE_FLOAT_NONE;
    d->set |= HPE_PROP_FLOAT;
}

static void apply_clear(HpeStyle *d, const char *value)
{
    if (!strcmp(value, "left"))
        d->clear_side = HPE_CLEAR_LEFT;
    else if (!strcmp(value, "right"))
        d->clear_side = HPE_CLEAR_RIGHT;
    else if (!strcmp(value, "both"))
        d->clear_side = HPE_CLEAR_BOTH;
    else
        d->clear_side = HPE_CLEAR_NONE;
    d->set |= HPE_PROP_CLEAR;
}

/* min-height / max-width / max-height (min-width is handled separately) */
static void apply_size2(HpeStyle *d, const char *value, HpeLength *slot, uint64_t bit)
{
    *slot = hpe_len_parse(value);
    d->set2 |= bit;
}

static void apply_min_height(HpeStyle *d, const char *v) { apply_size2(d, v, &d->min_height, HPE_PROP2_MIN_HEIGHT); }
static void apply_max_width(HpeStyle *d, const char *v) { apply_size2(d, v, &d->max_width, HPE_PROP2_MAX_WIDTH); }
static void apply_max_height(HpeStyle *d, const char *v) { apply_size2(d, v, &d->max_height, HPE_PROP2_MAX_HEIGHT); }

static void apply_box_sizing(HpeStyle *d, const char *value)
{
    d->box_sizing = !strcmp(value, "border-box") ? HPE_BOX_SIZING_BORDER : HPE_BOX_SIZING_CONTENT;
    d->set2 |= HPE_PROP2_BOX_SIZING;
}

static void apply_overflow(HpeStyle *d, const char *value)
{
    char buf[128];
    char *parts[2];
    int n;

    /* one value sets both axes; two values are x then y */
    snprintf(buf, sizeof(buf), "%s", value);
    n = split_ws(buf, parts, 2);
    if (n <= 0)
        return;
    d->overflow_x = overflow_keyword(parts[0]);
    d->overflow_y = n > 1 ? overflow_keyword(parts[1]) : d->overflow_x;
    d->set2 |= HPE_PROP2_OVERFLOW_X | HPE_PROP2_OVERFLOW_Y;
}

static void apply_overflow_x(HpeStyle *d, const char *value)
{
    d->overflow_x = overflow_keyword(value);
    d->set2 |= HPE_PROP2_OVERFLOW_X;
}

static void apply_overflow_y(HpeStyle *d, const char *value)
{
    d->overflow_y = overflow_keyword(value);
    d->set2 |= HPE_PROP2_OVERFLOW_Y;
}

static void apply_opacity(HpeStyle *d, const char *value)
{
    float v;
    if (parse_plain_number(value, &v)) {
        if (v < 0.0f)
            v = 0.0f;
        if (v > 1.0f)
            v = 1.0f;
        d->opacity = v;
        d->set2 |= HPE_PROP2_OPACITY;
    }
}

static void apply_visibility(HpeStyle *d, const char *value)
{
    if (!strcmp(value, "hidden"))
        d->visibility = HPE_VISIBILITY_HIDDEN;
    else if (!strcmp(value, "collapse"))
        d->visibility = HPE_VISIBILITY_COLLAPSE;
    else
        d->visibility = HPE_VISIBILITY_VISIBLE;
    d->set2 |= HPE_PROP2_VISIBILITY;
}

static void apply_cursor(HpeStyle *d, const char *value)
{
    if (!strcmp(value, "pointer"))
        d->cursor = HPE_CURSOR_POINTER;
    else if (!strcmp(value, "text"))
        d->cursor = HPE_CURSOR_TEXT;
    else if (!strcmp(value, "move"))
        d->cursor = HPE_CURSOR_MOVE;
    else if (!strcmp(value, "wait"))
        d->cursor = HPE_CURSOR_WAIT;
    else if (!strcmp(value, "help"))
        d->cursor = HPE_CURSOR_HELP;
    else if (!strcmp(value, "crosshair"))
        d->cursor = HPE_CURSOR_CROSSHAIR;
    else if (!strcmp(value, "not-allowed"))
        d->cursor = HPE_CURSOR_NOT_ALLOWED;
    else if (!strcmp(value, "grab"))
        d->cursor = HPE_CURSOR_GRAB;
    else if (!strcmp(value, "grabbing"))
        d->cursor = HPE_CURSOR_GRABBING;
    else if (!strcmp(value, "ew-resize") || !strcmp(value, "col-resize"))
        d->cursor = HPE_CURSOR_EW_RESIZE;
    else if (!strcmp(value, "ns-resize") || !strcmp(value, "row-resize"))
        d->cursor = HPE_CURSOR_NS_RESIZE;
    else if (!strcmp(value, "default"))
        d->cursor = HPE_CURSOR_DEFAULT;
    else
        d->cursor = HPE_CURSOR_AUTO;
    d->set2 |= HPE_PROP2_CURSOR;
}

static void apply_text_decoration(HpeStyle *d, const char *value)
{
    char buf[128];
    char *parts[4];
    int n, i;
    HpeTextDecoration dec = HPE_TD_NONE;

    snprintf(buf, sizeof(buf), "%s", value);
    n = split_ws(buf, parts, 4);
    for (i = 0; i < n; i++) {
        if (!strcmp(parts[i], "underline"))
            dec = (HpeTextDecoration)(dec | HPE_TD_UNDERLINE);
        else if (!strcmp(parts[i], "overline"))
            dec = (HpeTextDecoration)(dec | HPE_TD_OVERLINE);
        else if (!strcmp(parts[i], "line-through"))
            dec = (HpeTextDecoration)(dec | HPE_TD_LINE_THROUGH);
        else if (!strcmp(parts[i], "none"))
            dec = HPE_TD_NONE;
    }
    d->text_decoration = dec;
    d->set2 |= HPE_PROP2_TEXT_DECORATION;
}

static void apply_text_spacing(HpeStyle *d, const char *value, HpeLength *slot, uint64_t bit)
{
    HpeLength len = hpe_len_parse(value);
    if (len.unit == HPE_LEN_AUTO) /* `normal` */
        len = hpe_len_px(0.0f);
    *slot = len;
    d->set2 |= bit;
}

static void apply_letter_spacing(HpeStyle *d, const char *v) { apply_text_spacing(d, v, &d->letter_spacing, HPE_PROP2_LETTER_SPACING); }
static void apply_word_spacing(HpeStyle *d, const char *v) { apply_text_spacing(d, v, &d->word_spacing, HPE_PROP2_WORD_SPACING); }
static void apply_text_indent(HpeStyle *d, const char *v) { apply_text_spacing(d, v, &d->text_indent, HPE_PROP2_TEXT_INDENT); }

static void apply_vertical_align(HpeStyle *d, const char *value)
{
    if (!strcmp(value, "middle"))
        d->vertical_align = HPE_VALIGN_MIDDLE;
    else if (!strcmp(value, "top"))
        d->vertical_align = HPE_VALIGN_TOP;
    else if (!strcmp(value, "bottom"))
        d->vertical_align = HPE_VALIGN_BOTTOM;
    else if (!strcmp(value, "text-top"))
        d->vertical_align = HPE_VALIGN_TEXT_TOP;
    else if (!strcmp(value, "text-bottom"))
        d->vertical_align = HPE_VALIGN_TEXT_BOTTOM;
    else
        d->vertical_align = HPE_VALIGN_BASELINE;
    d->set2 |= HPE_PROP2_VERTICAL_ALIGN;
}

static void apply_list_style(HpeStyle *d, const char *value)
{
    char buf[128];
    char *parts[4];
    int n, i;

    snprintf(buf, sizeof(buf), "%s", value);
    n = split_ws(buf, parts, 4);
    for (i = 0; i < n; i++) {
        if (!strcmp(parts[i], "circle")) {
            d->list_style_type = HPE_LIST_CIRCLE;
            d->set2 |= HPE_PROP2_LIST_STYLE_TYPE;
        } else if (!strcmp(parts[i], "square")) {
            d->list_style_type = HPE_LIST_SQUARE;
            d->set2 |= HPE_PROP2_LIST_STYLE_TYPE;
        } else if (!strcmp(parts[i], "disc")) {
            d->list_style_type = HPE_LIST_DISC;
            d->set2 |= HPE_PROP2_LIST_STYLE_TYPE;
        } else if (!strcmp(parts[i], "decimal")) {
            d->list_style_type = HPE_LIST_DECIMAL;
            d->set2 |= HPE_PROP2_LIST_STYLE_TYPE;
        } else if (!strcmp(parts[i], "none")) {
            d->list_style_type = HPE_LIST_NONE;
            d->set2 |= HPE_PROP2_LIST_STYLE_TYPE;
        } else if (!strcmp(parts[i], "inside")) {
            d->list_style_position = HPE_LIST_INSIDE;
            d->set2 |= HPE_PROP2_LIST_STYLE_POSITION;
        } else if (!strcmp(parts[i], "outside")) {
            d->list_style_position = HPE_LIST_OUTSIDE;
            d->set2 |= HPE_PROP2_LIST_STYLE_POSITION;
        }
    }
}

static void apply_list_style_position(HpeStyle *d, const char *value)
{
    d->list_style_position = !strcmp(value, "inside") ? HPE_LIST_INSIDE : HPE_LIST_OUTSIDE;
    d->set2 |= HPE_PROP2_LIST_STYLE_POSITION;
}

static void apply_background_image(HpeStyle *d, const char *value)
{
    if (parse_linear_gradient(value, &d->background_image)) {
        d->set2 |= HPE_PROP2_BACKGROUND_IMAGE;
    } else if (!strcmp(value, "none")) {
        d->background_image.kind = HPE_BG_NONE;
        d->set2 |= HPE_PROP2_BACKGROUND_IMAGE;
    }
}

static void apply_box_shadow(HpeStyle *d, const char *value)
{
    if (parse_shadow(value, &d->box_shadow))
        d->set2 |= HPE_PROP2_BOX_SHADOW;
    else if (!strcmp(value, "none")) {
        d->box_shadow.valid = 0;
        d->set2 |= HPE_PROP2_BOX_SHADOW;
    }
}

static void apply_text_shadow(HpeStyle *d, const char *value)
{
    if (parse_shadow(value, &d->text_shadow))
        d->set2 |= HPE_PROP2_TEXT_SHADOW;
    else if (!strcmp(value, "none")) {
        d->text_shadow.valid = 0;
        d->set2 |= HPE_PROP2_TEXT_SHADOW;
    }
}

static void apply_transform(HpeStyle *d, const char *value)
{
    parse_transform(value, &d->transform);
    d->set2 |= HPE_PROP2_TRANSFORM;
}

static void apply_transition(HpeStyle *d, const char *value)
{
    d->has_transition = !strcmp(value, "none") ? 0 : 1;
    d->set2 |= HPE_PROP2_TRANSITION;
}

static void apply_animation(HpeStyle *d, const char *value)
{
    d->has_animation = !strcmp(value, "none") ? 0 : 1;
    d->set2 |= HPE_PROP2_ANIMATION;
}

/* per-side border colours: `border-top-color` etc. */
static void apply_border_side_color(HpeStyle *d, const char *value, int side, uint64_t bit)
{
    HpeColor c;
    if (!hpe_color_parse(value, &c))
        return;
    d->border_colors[side] = c;
    d->border_color = c; /* keep the shorthand in sync */
    d->set |= HPE_PROP_BORDER_COLOR;
    d->set2 |= bit;
}

static void apply_border_top_color(HpeStyle *d, const char *v) { apply_border_side_color(d, v, 0, HPE_PROP2_BORDER_TOP_COLOR); }
static void apply_border_right_color(HpeStyle *d, const char *v) { apply_border_side_color(d, v, 1, HPE_PROP2_BORDER_RIGHT_COLOR); }
static void apply_border_bottom_color(HpeStyle *d, const char *v) { apply_border_side_color(d, v, 2, HPE_PROP2_BORDER_BOTTOM_COLOR); }
static void apply_border_left_color(HpeStyle *d, const char *v) { apply_border_side_color(d, v, 3, HPE_PROP2_BORDER_LEFT_COLOR); }

/* name -> applier registry; the single source of truth for the property surface */
typedef void (*PropApplier)(HpeStyle *d, const char *value);

typedef struct {
    const char *name;
    PropApplier fn;
} PropEntry;

static const PropEntry k_prop_table[] = {
    {"display", apply_display},
    {"color", apply_color},
    {"background", apply_background},
    {"background-color", apply_background},
    {"min-width", apply_min_width},
    {"width", apply_width},
    {"height", apply_height},
    {"margin", apply_margin},
    {"padding", apply_padding},
    {"margin-top", apply_margin_top},
    {"margin-right", apply_margin_right},
    {"margin-bottom", apply_margin_bottom},
    {"margin-left", apply_margin_left},
    {"padding-top", apply_padding_top},
    {"padding-right", apply_padding_right},
    {"padding-bottom", apply_padding_bottom},
    {"padding-left", apply_padding_left},
    {"border-color", apply_border_color},
    {"border", apply_border},
    {"border-width", apply_border_width},
    {"border-top", apply_border_top},
    {"border-right", apply_border_right},
    {"border-bottom", apply_border_bottom},
    {"border-left", apply_border_left},
    {"border-radius", apply_border_radius},
    {"inset", apply_inset},
    {"flex-direction", apply_flex_direction},
    {"align-items", apply_align_items},
    {"align-self", apply_align_self},
    {"justify-content", apply_justify},
    {"gap", apply_gap},
    {"column-gap", apply_gap},
    {"row-gap", apply_gap},
    {"flex", apply_flex_grow},
    {"flex-grow", apply_flex_grow},
    {"font-size", apply_font_size},
    {"font-weight", apply_font_weight},
    {"text-align", apply_text_align},
    {"line-height", apply_line_height},
    {"white-space", apply_white_space},
    {"font-family", apply_font_family},
    {"position", apply_position},
    {"top", apply_top},
    {"right", apply_right},
    {"bottom", apply_bottom},
    {"left", apply_left},
    {"z-index", apply_z_index},
    {"float", apply_float},
    {"clear", apply_clear},
    {"min-height", apply_min_height},
    {"max-width", apply_max_width},
    {"max-height", apply_max_height},
    {"box-sizing", apply_box_sizing},
    {"overflow", apply_overflow},
    {"overflow-x", apply_overflow_x},
    {"overflow-y", apply_overflow_y},
    {"opacity", apply_opacity},
    {"visibility", apply_visibility},
    {"cursor", apply_cursor},
    {"text-decoration", apply_text_decoration},
    {"text-decoration-line", apply_text_decoration},
    {"letter-spacing", apply_letter_spacing},
    {"word-spacing", apply_word_spacing},
    {"text-indent", apply_text_indent},
    {"vertical-align", apply_vertical_align},
    {"list-style-type", apply_list_style},
    {"list-style", apply_list_style},
    {"list-style-position", apply_list_style_position},
    {"background-image", apply_background_image},
    {"box-shadow", apply_box_shadow},
    {"text-shadow", apply_text_shadow},
    {"transform", apply_transform},
    {"transition", apply_transition},
    {"animation", apply_animation},
    {"border-top-color", apply_border_top_color},
    {"border-right-color", apply_border_right_color},
    {"border-bottom-color", apply_border_bottom_color},
    {"border-left-color", apply_border_left_color},
};

static void apply_property(HpeStyle *d, const char *prop, const char *value)
{
    char lprop[64];
    size_t i;

    if (!prop || !value)
        return;
    snprintf(lprop, sizeof(lprop), "%s", prop);
    for (i = 0; lprop[i]; i++)
        lprop[i] = (char)hpe_ascii_lower((unsigned char)lprop[i]);

    /* the cascade already starts from the parent's computed style, so "inherit"
     * needs no work; being lenient here keeps `font-family: inherit` usable. */
    if (!strcmp(value, "inherit"))
        return;

    for (i = 0; i < sizeof(k_prop_table) / sizeof(k_prop_table[0]); i++) {
        if (!strcmp(lprop, k_prop_table[i].name)) {
            k_prop_table[i].fn(d, value);
            return;
        }
    }
}

/* ---------------- declaration parsing ---------------- */

static const char *strip_important(const char *value, char *buf, size_t buf_size, int *out_important)
{
    size_t len;
    if (out_important)
        *out_important = 0;
    snprintf(buf, buf_size, "%s", value);
    len = strlen(buf);
    while (len > 0 && hpe_is_space(buf[len - 1]))
        buf[--len] = '\0';
    if (len >= 10 && hpe_str_ieq(buf + len - 10, "!important")) {
        buf[len - 10] = '\0';
        if (out_important)
            *out_important = 1;
        return hpe_trim(buf);
    }
    return hpe_trim(buf);
}

void hpe_css_parse_declarations(const char *text, size_t length, HpeStyle *decl)
{
    char *copy;
    char *p;
    if (!decl)
        return;
    memset(decl, 0, sizeof(*decl));
    if (!text || length == 0)
        return;
    copy = hpe_strndup(text, length);
    p = copy;
    while (*p) {
        char *decl_end = p;
        char *colon;
        char *prop, *value;
        char vbuf[512];

        while (*decl_end && *decl_end != ';')
            decl_end++;
        {
            char saved = *decl_end;
            *decl_end = '\0';
            colon = strchr(p, ':');
            if (colon) {
                *colon = '\0';
                prop = hpe_trim(p);
                value = hpe_trim(colon + 1);
                if (*prop && *value) {
                    int important = 0;
                    uint64_t before, before2;
                    const char *clean =
                        strip_important(value, vbuf, sizeof(vbuf), &important);
                    before = decl->set;
                    before2 = decl->set2;
                    apply_property(decl, prop, clean);
                    if (important) {
                        /* mark exactly the properties this declaration set */
                        decl->imp |= decl->set & ~before;
                        decl->imp2 |= decl->set2 & ~before2;
                    }
                }
            }
            *decl_end = saved;
        }
        if (*decl_end == ';')
            p = decl_end + 1;
        else
            break;
    }
    hpe_free(copy);
}

/* ---------------- selectors ---------------- */

static int is_ident_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-';
}

static char *read_ident(const char **pp, const char *end, char *out, size_t out_size)
{
    const char *p = *pp;
    const char *start = p;
    size_t n;
    while (p < end && is_ident_char(*p))
        p++;
    if (p == start)
        return NULL;
    n = (size_t)(p - start);
    if (n >= out_size)
        n = out_size - 1;
    memcpy(out, start, n);
    out[n] = '\0';
    *pp = p;
    return out;
}

static void compound_add_class(HpeCompound *c, const char *name)
{
    c->classes = (char **)hpe_grow(c->classes, &c->class_cap, (size_t)c->class_count + 1,
                                   sizeof(char *));
    c->classes[c->class_count++] = hpe_strdup(name);
}

static void compound_free(HpeCompound *c)
{
    int i;
    if (c->tag)
        hpe_free(c->tag);
    if (c->id)
        hpe_free(c->id);
    for (i = 0; i < c->class_count; i++)
        hpe_free(c->classes[i]);
    if (c->classes)
        hpe_free(c->classes);
    for (i = 0; i < c->attr_count; i++) {
        if (c->attrs[i].name)
            hpe_free(c->attrs[i].name);
        if (c->attrs[i].op)
            hpe_free(c->attrs[i].op);
        if (c->attrs[i].value)
            hpe_free(c->attrs[i].value);
    }
    if (c->attrs)
        hpe_free(c->attrs);
    memset(c, 0, sizeof(*c));
}

/* parses one `.class` / `#id` token; returns 0 on a malformed selector */
static int parse_class_or_id(HpeCompound *out, const char **pp, const char *end, char *ident,
                             size_t ident_size)
{
    const char *p = *pp;
    char sigil = *p++;

    if (!read_ident(&p, end, ident, ident_size))
        return 0;
    if (sigil == '.')
        compound_add_class(out, ident);
    else {
        if (out->id)
            hpe_free(out->id);
        out->id = hpe_strdup(ident);
    }
    *pp = p;
    return 1;
}

/* parses a `:pseudo` token; only :root and :hover affect the static match, the
 * rest are tolerated so a selector containing them does not fail to parse */
static int parse_pseudo(HpeCompound *out, const char **pp, const char *end, char *ident,
                        size_t ident_size)
{
    const char *p = *pp + 1; /* skip ':' */

    if (!read_ident(&p, end, ident, ident_size))
        return 0;
    if (hpe_str_ieq(ident, "root")) {
        if (!out->tag)
            out->tag = hpe_strdup("html");
    } else if (hpe_str_ieq(ident, "hover")) {
        out->pseudo_hover = 1;
    }
    /* other dynamic / structural pseudo-classes are ignored on purpose */
    *pp = p;
    return 1;
}

/* splits the body of `[...]` into name / operator / value; mirrors the engine's
 * lenient (non-spec) tokenisation exactly so selector behaviour is unchanged */
static void dissect_attr_expr(const char *body, char *aname, char *aop, char *avalue,
                              size_t avalue_size, int *has_val)
{
    char *tmp = hpe_strdup(body);
    char *sp = tmp;
    char *namep = aname;

    aname[0] = '\0';
    aop[0] = '\0';
    avalue[0] = '\0';
    *has_val = 0;

    while (*sp && *sp != ' ' && *sp != '=')
        *namep++ = *sp++;
    *namep = '\0';
    while (*sp == ' ')
        sp++;
    if (*sp == '=') {
        const char *v;
        size_t vl;
        aop[0] = '=';
        aop[1] = '\0';
        sp++;
        while (*sp == ' ')
            sp++;
        if (*sp == '~' || *sp == '|' || *sp == '^' || *sp == '$' || *sp == '*') {
            aop[0] = *sp++;
            aop[1] = '=';
            aop[2] = '\0';
        }
        while (*sp == ' ')
            sp++;
        v = sp;
        vl = strlen(v);
        if (vl >= 2 && (v[0] == '"' || v[0] == '\'') && v[vl - 1] == v[0]) {
            v++;
            vl -= 2;
        }
        if (vl >= avalue_size)
            vl = avalue_size - 1;
        memcpy(avalue, v, vl);
        avalue[vl] = '\0';
        *has_val = 1;
    } else if (*sp == '~' || *sp == '|' || *sp == '^' || *sp == '$' || *sp == '*') {
        /* malformed but tolerate */
        aop[0] = *sp;
        aop[1] = '=';
        aop[2] = '\0';
        *has_val = 1;
    }
    hpe_free(tmp);
}

static void compound_add_attr(HpeCompound *out, const char *aname, const char *aop,
                              const char *avalue, int has_val)
{
    HpeAttrSel *as;
    int j;

    for (j = 0; aname[j]; j++)
        ((char *)aname)[j] = (char)hpe_ascii_lower((unsigned char)aname[j]);
    out->attrs = (HpeAttrSel *)hpe_grow(out->attrs, &out->attr_cap,
                                        (size_t)out->attr_count + 1, sizeof(HpeAttrSel));
    as = &out->attrs[out->attr_count++];
    as->name = hpe_strdup(aname);
    as->op = hpe_strdup(aop);
    as->value = hpe_strdup(has_val ? avalue : "");
}

/* parses a `[attr]` / `[attr=v]` / `[attr~=v]` selector */
static int parse_attr_selector(HpeCompound *out, const char **pp, const char *end)
{
    const char *a = *pp + 1;
    const char *close = a;
    char aname[128], aop[8], avalue[256];
    int has_val = 0;

    while (close < end && *close != ']')
        close++;
    if (close >= end)
        return 0; /* unbalanced */
    {
        char *body = hpe_strndup(a, (size_t)(close - a));
        dissect_attr_expr(body, aname, aop, avalue, sizeof(avalue), &has_val);
        hpe_free(body);
    }
    compound_add_attr(out, aname, aop, avalue, has_val);
    *pp = close + 1;
    return 1;
}

static int parse_compound(const char **pp, const char *end, HpeCompound *out)
{
    const char *p = *pp;
    char ident[128];

    memset(out, 0, sizeof(*out));

    if (p < end && *p == '*') {
        p++;
    } else if (p < end && is_ident_char(*p) && read_ident(&p, end, ident, sizeof(ident))) {
        size_t i;
        for (i = 0; ident[i]; i++)
            ident[i] = (char)hpe_ascii_lower((unsigned char)ident[i]);
        out->tag = hpe_strdup(ident);
    }

    while (p < end) {
        if (*p == '.' || *p == '#') {
            if (!parse_class_or_id(out, &p, end, ident, sizeof(ident)))
                return 0;
        } else if (*p == ':') {
            if (!parse_pseudo(out, &p, end, ident, sizeof(ident)))
                return 0;
        } else if (*p == '[') {
            if (!parse_attr_selector(out, &p, end))
                return 0;
        } else {
            break;
        }
    }
    *pp = p;
    return 1;
}

static int selector_add_part(HpeSelector *sel, const HpeCompound *part)
{
    sel->parts = (HpeCompound *)hpe_grow(sel->parts, &sel->cap, (size_t)sel->count + 1,
                                         sizeof(HpeCompound));
    sel->parts[sel->count++] = *part;
    if (part->id)
        sel->specificity += 100;
    sel->specificity += part->class_count * 10;
    if (part->pseudo_hover)
        sel->specificity += 10; /* pseudo-class counts as a class */
    if (part->tag)
        sel->specificity += 1;
    return 1;
}

static void selector_free(HpeSelector *sel)
{
    int i;
    for (i = 0; i < sel->count; i++)
        compound_free(&sel->parts[i]);
    if (sel->parts)
        hpe_free(sel->parts);
    memset(sel, 0, sizeof(*sel));
}

static void rule_free(HpeRule *rule)
{
    int i;
    for (i = 0; i < rule->selector_count; i++)
        selector_free(&rule->selectors[i]);
    if (rule->selectors)
        hpe_free(rule->selectors);
    if (rule->decls)
        hpe_free(rule->decls);
    memset(rule, 0, sizeof(*rule));
}

/* consumes an explicit combinator token (`>` `+` `~`), defaulting to descendant */
static int read_combinator(const char **pq, const char *end)
{
    const char *q = *pq;
    int comb = HPE_COMB_DESCENDANT;

    if (q < end && *q == '>') {
        comb = HPE_COMB_CHILD;
        q++;
    } else if (q < end && *q == '+') {
        comb = HPE_COMB_ADJ_SIBLING;
        q++;
    } else if (q < end && *q == '~') {
        comb = HPE_COMB_GEN_SIBLING;
        q++;
    }
    while (q < end && hpe_is_space(*q))
        q++;
    *pq = q;
    return comb;
}

/* parses one comma-separated selector (a chain of compounds) into `sel` */
static void parse_one_selector(const char *part_start, const char *part_end, HpeSelector *sel)
{
    const char *q = part_start;
    int first = 1;

    memset(sel, 0, sizeof(*sel));
    while (q < part_end) {
        int comb = HPE_COMB_DESCENDANT;
        HpeCompound comp;

        while (q < part_end && hpe_is_space(*q))
            q++;
        if (!first) {
            /* leading whitespace was already skipped above, so any token that is
             * not an explicit combinator means the parts were space-separated:
             * descendant */
            comb = read_combinator(&q, part_end);
        }
        if (q >= part_end)
            break;
        if (!parse_compound(&q, part_end, &comp)) {
            compound_free(&comp);
            sel->count = -1;
            break;
        }
        comp.comb = first ? HPE_COMB_DESCENDANT : comb;
        selector_add_part(sel, &comp);
        first = 0;
    }
}

/* parses "a b, c > d" into selector list */
static void parse_selector_list(HpeRule *rule, const char *text, size_t length)
{
    const char *p = text;
    const char *end = text + length;

    while (p < end) {
        const char *part_start = p;
        const char *part_end;
        HpeSelector sel;

        while (p < end && *p != ',')
            p++;
        part_end = p;
        if (p < end)
            p++;

        parse_one_selector(part_start, part_end, &sel);
        if (sel.count > 0) {
            rule->selectors = (HpeSelector *)hpe_grow(rule->selectors, &rule->selector_cap,
                                                      (size_t)rule->selector_count + 1,
                                                      sizeof(HpeSelector));
            rule->selectors[rule->selector_count++] = sel;
        } else {
            selector_free(&sel);
        }
    }
}

static int class_list_contains(const char *list, const char *name)
{
    const char *p = list;
    size_t n = strlen(name);
    if (!list)
        return 0;
    while (*p) {
        const char *start;
        while (*p && hpe_is_space(*p))
            p++;
        start = p;
        while (*p && !hpe_is_space(*p))
            p++;
        if ((size_t)(p - start) == n && !strncmp(start, name, n))
            return 1;
    }
    return 0;
}

/* true if `val` appears as a whitespace-separated word inside `list` */
static int attr_word_contains(const char *list, const char *val)
{
    size_t vlen = strlen(val);
    const char *p = list;
    if (!list || vlen == 0)
        return 0;
    while (*p) {
        const char *start;
        while (*p && hpe_is_space(*p))
            p++;
        start = p;
        while (*p && !hpe_is_space(*p))
            p++;
        if ((size_t)(p - start) == vlen && !strncmp(start, val, vlen))
            return 1;
    }
    return 0;
}

/*
 * Match one attribute selector, operator included, against the node's raw
 * attribute value. `attr_val` may be NULL (attribute absent). An empty op
 * means a bare presence test; any unrecognised operator is treated as a
 * match so that a future selector form degrades gracefully instead of
 * silently dropping the whole rule.
 */
static int attr_op_matches(const HpeAttrSel *as, const char *attr_val)
{
    if (as->op[0] == '\0')
        return attr_val != NULL;
    if (!attr_val)
        return 0;
    if (as->op[0] == '=' && as->op[1] == '\0')
        return strcmp(attr_val, as->value) == 0;
    if (as->op[0] == '~')
        return attr_word_contains(attr_val, as->value);
    if (as->op[0] == '|') {
        size_t vl = strlen(as->value);
        if (strncmp(attr_val, as->value, vl) != 0)
            return 0;
        return attr_val[vl] == '\0' || attr_val[vl] == '-';
    }
    if (as->op[0] == '^')
        return strncmp(attr_val, as->value, strlen(as->value)) == 0;
    if (as->op[0] == '$') {
        size_t al = strlen(attr_val), vl = strlen(as->value);
        return al >= vl && strcmp(attr_val + al - vl, as->value) == 0;
    }
    if (as->op[0] == '*')
        return strstr(attr_val, as->value) != NULL;
    return 1;
}

static int compound_matches(const HpeCompound *c, const HpeDomNode *node)
{
    int i;
    if (!node || node->type != HPE_NODE_ELEMENT)
        return 0;
    if (c->tag && strcmp(c->tag, node->tag) != 0)
        return 0;
    if (c->pseudo_hover && !node->hovered)
        return 0;
    if (c->id) {
        const char *id = hpe_dom_attr(node, "id");
        if (!id || strcmp(id, c->id) != 0)
            return 0;
    }
    if (c->class_count > 0) {
        const char *cls = hpe_dom_attr(node, "class");
        if (!cls)
            return 0;
        for (i = 0; i < c->class_count; i++) {
            if (!class_list_contains(cls, c->classes[i]))
                return 0;
        }
    }
    for (i = 0; i < c->attr_count; i++) {
        const HpeAttrSel *as = &c->attrs[i];
        if (!attr_op_matches(as, hpe_dom_attr(node, as->name)))
            return 0;
    }
    return 1;
}

int hpe_css_selector_matches(const HpeDomNode *node, const char *sel, size_t len)
{
    HpeRule rule;
    int i, hit = 0;

    if (!node || !sel || len == 0)
        return 0;
    memset(&rule, 0, sizeof(rule));
    parse_selector_list(&rule, sel, len);
    for (i = 0; i < rule.selector_count && !hit; i++)
        hit = selector_matches(&rule.selectors[i], node);
    rule_free(&rule);
    return hit;
}

static int selector_matches(const HpeSelector *sel, const HpeDomNode *node)
{
    int part;
    const HpeDomNode *cur = node;

    if (sel->count <= 0)
        return 0;
    part = sel->count - 1;
    if (!compound_matches(&sel->parts[part], cur))
        return 0;
    part--;
    while (part >= 0) {
        const HpeCompound *c = &sel->parts[part];
        if (c->comb == HPE_COMB_CHILD) {
            cur = cur->parent;
            if (!cur || !compound_matches(c, cur))
                return 0;
        } else if (c->comb == HPE_COMB_ADJ_SIBLING) {
            cur = hpe_dom_prev_sibling(cur);
            if (!cur || !compound_matches(c, cur))
                return 0;
        } else if (c->comb == HPE_COMB_GEN_SIBLING) {
            const HpeDomNode *s = hpe_dom_prev_sibling(cur);
            int ok = 0;
            while (s) {
                if (compound_matches(c, s)) {
                    ok = 1;
                    break;
                }
                s = hpe_dom_prev_sibling(s);
            }
            if (!ok)
                return 0;
            cur = s;
        } else { /* descendant */
            cur = cur->parent;
            while (cur && !compound_matches(c, cur))
                cur = cur->parent;
            if (!cur)
                return 0;
        }
        part--;
    }
    return 1;
}

/* ---------------- media queries ---------------- */

/* maps a media feature name to its HPE_MF_* id, 0 when unrecognised */
static int media_feature_id(const char *name)
{
    if (hpe_str_ieq(name, "min-width"))
        return HPE_MF_MIN_WIDTH;
    if (hpe_str_ieq(name, "max-width"))
        return HPE_MF_MAX_WIDTH;
    if (hpe_str_ieq(name, "min-height"))
        return HPE_MF_MIN_HEIGHT;
    if (hpe_str_ieq(name, "max-height"))
        return HPE_MF_MAX_HEIGHT;
    if (hpe_str_ieq(name, "width"))
        return HPE_MF_WIDTH;
    if (hpe_str_ieq(name, "height"))
        return HPE_MF_HEIGHT;
    return 0;
}

/* parses a media feature value in px (only px lengths are honoured) */
static int media_feature_value(const char *s, float *out)
{
    char *end = NULL;
    float v;
    if (!s || !*s)
        return 0;
    v = (float)strtod(s, &end);
    if (end == s)
        return 0;
    while (end && hpe_is_space(*end))
        end++;
    if (end && hpe_str_ieq_n(end, "px", 2))
        end += 2;
    while (end && hpe_is_space(*end))
        end++;
    if (end && *end)
        return 0; /* only a bare px length is understood */
    *out = v;
    return 1;
}

/* parses the text between `@media` and `{` into `mq`. */
/* consumes a leading `not` / `only` qualifier; returns the negate flag */
static int media_consume_qualifier(char **pq)
{
    char *q = *pq;
    if (hpe_starts_with_i(q, "not")) {
        *pq = hpe_trim((char *)q + 3);
        return 1;
    }
    if (hpe_starts_with_i(q, "only"))
        *pq = hpe_trim((char *)q + 4);
    return 0;
}

/* parses one `(feature: value)` condition into the alternative */
static void media_parse_condition(char **pq, HpeMediaAlt *alt)
{
    char *q = *pq;
    char *close = strchr(q, ')');
    char *inner;
    char *colon;
    int fid;
    float fv;

    if (!close)
        return;
    *close = '\0';
    inner = hpe_trim(q + 1);
    colon = strchr(inner, ':');
    if (colon)
        *colon = '\0';
    fid = media_feature_id(hpe_trim(inner));
    if (!fid || !colon || !media_feature_value(hpe_trim(colon + 1), &fv)) {
        alt->unknown = 1;
    } else if (alt->cond_count < HPE_MEDIA_MAX_COND) {
        alt->conds[alt->cond_count].feature = fid;
        alt->conds[alt->cond_count].value = fv;
        alt->cond_count++;
    }
    *pq = close + 1;
}

/* parses a bare media-type token (`all` / `screen` / `print`) into `alt` */
static void media_parse_type(char **pq, HpeMediaAlt *alt)
{
    char *q = *pq;
    char *sp = q;

    while (*sp && !hpe_is_space(*sp))
        sp++;
    {
        char saved = *sp;
        *sp = '\0';
        if (hpe_str_ieq(q, "all"))
            alt->type = HPE_MTYPE_ALL;
        else if (hpe_str_ieq(q, "screen"))
            alt->type = HPE_MTYPE_SCREEN;
        else if (hpe_str_ieq(q, "print"))
            alt->type = HPE_MTYPE_PRINT;
        else
            alt->unknown = 1;
        *sp = saved;
    }
    *pq = sp;
}

/* parses the conjunction inside one alternative (features joined by `and`) */
static void media_parse_alt_body(char *q, HpeMediaAlt *alt)
{
    while (*q) {
        if (*q == '(') {
            media_parse_condition(&q, alt);
        } else {
            media_parse_type(&q, alt);
        }
        while (*q && hpe_is_space(*q))
            q++;
        if (hpe_starts_with_i(q, "and")) {
            q = hpe_trim(q + 3);
        } else if (*q == '(') {
            continue;
        } else {
            break;
        }
    }
}

/* parses the text between `@media` and `{` into `mq`. */
static void parse_media_query(const char *text, size_t length, HpeMediaQuery *mq)
{
    const char *end = text + length;
    const char *p = text;
    int ai = 0;

    memset(mq, 0, sizeof(*mq));
    mq->present = 1;
    while (p < end) {
        const char *seg_end = p;
        char *buf;
        char *q;

        if (ai >= HPE_MEDIA_MAX_ALT)
            break;
        while (seg_end < end && *seg_end != ',')
            seg_end++;

        mq->alts[ai].type = HPE_MTYPE_UNSET;
        /* operate on a NUL-terminated copy so the tokeniser can be reused */
        buf = hpe_strndup(p, (size_t)(seg_end - p));
        q = hpe_trim(buf);
        mq->alts[ai].negate = media_consume_qualifier(&q);
        media_parse_alt_body(q, &mq->alts[ai]);
        hpe_free(buf);
        ai++;
        p = (seg_end < end) ? seg_end + 1 : seg_end;
    }
    mq->alt_count = ai;
}

/* evaluates one alternative against the viewport; 0 = no match */
static int media_alt_matches(const HpeMediaQuery *mq, float vw, float vh)
{
    int alt;
    for (alt = 0; alt < mq->alt_count; alt++) {
        int match = 1;
        int i;
        if (mq->alts[alt].unknown)
            match = 0;
        if (mq->alts[alt].type == HPE_MTYPE_PRINT)
            match = 0; /* the engine is a screen medium */
        /* an unset type is `all` */
        for (i = 0; i < mq->alts[alt].cond_count; i++) {
            int f = mq->alts[alt].conds[i].feature;
            float v = mq->alts[alt].conds[i].value;
            switch (f) {
            case HPE_MF_MIN_WIDTH: match = match && (vw >= v); break;
            case HPE_MF_MAX_WIDTH: match = match && (vw <= v); break;
            case HPE_MF_MIN_HEIGHT: match = match && (vh >= v); break;
            case HPE_MF_MAX_HEIGHT: match = match && (vh <= v); break;
            case HPE_MF_WIDTH: match = match && ((int)vw == (int)v); break;
            case HPE_MF_HEIGHT: match = match && ((int)vh == (int)v); break;
            default: match = 0; break;
            }
        }
        if (mq->alts[alt].negate)
            match = !match;
        if (match)
            return 1;
    }
    return 0;
}

void hpe_css_set_viewport(HpeStylesheet *sheet, float vw, float vh)
{
    if (!sheet)
        return;
    sheet->vw = vw;
    sheet->vh = vh;
}

/* ---------------- stylesheet parsing ---------------- */

static void skip_ws_comments(const char **pp, const char *end)
{
    const char *p = *pp;
    for (;;) {
        while (p < end && hpe_is_space(*p))
            p++;
        if (p + 1 < end && p[0] == '/' && p[1] == '*') {
            const char *q = p + 2;
            while (q + 1 < end && !(q[0] == '*' && q[1] == '/'))
                q++;
            p = (q + 1 < end) ? q + 2 : end;
            continue;
        }
        break;
    }
    *pp = p;
}

/* find the matching '}' of the block that starts at `block`; returns the index
 * of that closing brace, or `end` if the block is unterminated */
static const char *find_block_end(const char *block, const char *end)
{
    const char *scan = block;
    int depth = 1;

    while (scan < end && depth > 0) {
        if (*scan == '{')
            depth++;
        else if (*scan == '}')
            depth--;
        if (depth > 0)
            scan++;
    }
    return scan;
}

/* append one fully-parsed rule (with its enclosing @media chain) to `sheet` */
static void sheet_add_rule(HpeStylesheet *sheet, HpeRule *rule,
                           const HpeMediaQuery *mq_stack, int mq_depth)
{
    int k;

    if (mq_depth > 0) {
        for (k = 0; k < mq_depth; k++)
            rule->mq[k] = mq_stack[k];
        rule->mq_depth = mq_depth;
    }
    sheet->rules = (HpeRule *)hpe_grow(sheet->rules, &sheet->rule_cap,
                                       (size_t)sheet->rule_count + 1, sizeof(HpeRule));
    sheet->rules[sheet->rule_count++] = *rule;
}

/*
 * Parse the body of an at-rule that opened at `p` with its terminator found at
 * `q` (either ';' which ends a statement at-rule, or '{' which opens a block).
 * Only `@media` has an effect today; other blocks are skipped wholesale so an
 * unknown at-rule never corrupts the surrounding rule stream.
 */
static void parse_at_rule(HpeStylesheet *sheet, const char **pp, const char *q,
                          const char *end, HpeMediaQuery *mq_stack, int mq_depth)
{
    const char *p = *pp;
    int is_media;

    if (*q == ';') {
        *pp = q + 1;
        return;
    }
    is_media = (size_t)(q - p) >= 6 && hpe_str_ieq_n(p, "@media", 6);
    {
        const char *block = q + 1;
        const char *scan = find_block_end(block, end);
        int stacked = 0;

        if (is_media && mq_depth < HPE_MEDIA_MAX_DEPTH) {
            HpeMediaQuery mq;

            parse_media_query(p + 6, (size_t)(q - (p + 6)), &mq);
            if (mq.alt_count > 0) {
                mq_stack[mq_depth] = mq;
                stacked = 1;
            }
        }
        if (is_media)
            css_parse_into(sheet, block, (size_t)(scan - block), mq_stack,
                           mq_depth + (stacked ? 1 : 0));
        *pp = scan < end ? scan + 1 : end;
    }
}

/* advance past a declaration block, honouring quoted strings so a '}' inside
 * a string value does not terminate the block early */
static const char *find_decl_block_end(const char *p, const char *end)
{
    const char *scan = p;

    while (scan < end && *scan != '}') {
        if (*scan == '"' || *scan == '\'') {
            char quote = *scan++;

            while (scan < end && *scan != quote)
                scan++;
            if (scan < end)
                scan++;
            continue;
        }
        scan++;
    }
    return scan;
}

/* parse one `selector { declarations }` rule and, when it declares anything,
 * materialise it into the sheet */
static void parse_style_rule(HpeStylesheet *sheet, const char **pp, const char *end,
                             const HpeMediaQuery *mq_stack, int mq_depth)
{
    const char *p = *pp;
    const char *sel_start = p;
    const char *decl_start;
    const char *scan;
    size_t sel_len, decl_len;
    HpeRule rule;

    while (p < end && *p != '{')
        p++;
    if (p >= end) {
        *pp = end;
        return;
    }
    sel_len = (size_t)(p - sel_start);
    p++; /* '{' */
    decl_start = p;
    scan = find_decl_block_end(p, end);
    decl_len = (size_t)(scan - decl_start);
    *pp = (scan < end) ? scan + 1 : end;

    memset(&rule, 0, sizeof(rule));
    parse_selector_list(&rule, sel_start, sel_len);
    if (rule.selector_count > 0) {
        HpeStyle decl;

        hpe_css_parse_declarations(decl_start, decl_len, &decl);
        if (decl.set != 0 || decl.set2 != 0) {
            /* store one single-property declaration per entry for ordered application */
            rule.decls = (HpeStyle *)hpe_alloc(sizeof(HpeStyle));
            rule.decls[0] = decl;
            rule.decl_count = 1;
            rule.order = sheet->next_order++;
            sheet_add_rule(sheet, &rule, mq_stack, mq_depth);
            return;
        }
    }
    rule_free(&rule);
}

/* `mq_stack` holds the conditions of the enclosing @media blocks (outermost
 * first); rules are tagged with the whole chain and only match when every
 * enclosing query matches the viewport at cascade time. */
static void css_parse_into(HpeStylesheet *sheet, const char *css, size_t length,
                           HpeMediaQuery *mq_stack, int mq_depth)
{
    const char *p = css;
    const char *end = css + length;

    if (!sheet || !css)
        return;

    while (p < end) {
        skip_ws_comments(&p, end);
        if (p >= end)
            break;
        if (*p == '@') {
            const char *q = p;

            while (q < end && *q != '{' && *q != ';')
                q++;
            if (q >= end)
                break;
            parse_at_rule(sheet, &p, q, end, mq_stack, mq_depth);
        } else {
            parse_style_rule(sheet, &p, end, mq_stack, mq_depth);
        }
    }
}

void hpe_css_parse(HpeStylesheet *sheet, const char *css, size_t length)
{
    HpeMediaQuery stack[HPE_MEDIA_MAX_DEPTH];
    css_parse_into(sheet, css, length, stack, 0);
}

void hpe_css_init(HpeStylesheet *sheet)
{
    if (!sheet)
        return;
    memset(sheet, 0, sizeof(*sheet));
    sheet->vw = 800.0f; /* a sensible default until the engine sets the viewport */
    sheet->vh = 600.0f;
    hpe_css_parse(sheet, hpe_ua_stylesheet(), strlen(hpe_ua_stylesheet()));
}

void hpe_css_free(HpeStylesheet *sheet)
{
    int i;
    if (!sheet)
        return;
    for (i = 0; i < sheet->rule_count; i++)
        rule_free(&sheet->rules[i]);
    if (sheet->rules)
        hpe_free(sheet->rules);
    memset(sheet, 0, sizeof(*sheet));
}

/* ---------------- cascade ---------------- */

/* copies the four-sided (margin/padding/border-width/inset) longhands */
static void apply_box_edges(HpeStyle *dst, const HpeStyle *src, uint64_t s)
{
    int i;

    for (i = 0; i < 4; i++) {
        if (s & (HPE_PROP_MARGIN_TOP << i))
            dst->margin[i] = src->margin[i];
        if (s & (HPE_PROP_PADDING_TOP << i))
            dst->padding[i] = src->padding[i];
        if (s & (HPE_PROP_BORDER_TOP << i))
            dst->border_w[i] = src->border_w[i];
        if (s & (HPE_PROP_INSET_TOP << i))
            dst->inset[i] = src->inset[i];
    }
    if (s & HPE_PROP_BORDER_WIDTH) {
        for (i = 0; i < 4; i++)
            dst->border_w[i] = src->border_w[i];
    }
}

/* Table-driven longhand copy: every scalar longhand is "if its bit is set in
 * the declaration mask, copy the member bytes verbatim". Keeping the mapping
 * as data removes the previous long if-chains and guarantees that adding a
 * property only means adding one row here. */
typedef struct ScalarCopy {
    uint64_t bit;  /* property bit that gates the copy */
    size_t offset; /* offsetof the HpeStyle member */
    size_t size;   /* sizeof that member */
} ScalarCopy;

#define SCALAR_COPY(bit, member) \
    { (bit), offsetof(HpeStyle, member), sizeof(((HpeStyle *)0)->member) }

/* longhands carried in the first property word (HPE_PROP_*) */
static const ScalarCopy k_scalars_word1[] = {
    SCALAR_COPY(HPE_PROP_DISPLAY, display),
    SCALAR_COPY(HPE_PROP_COLOR, color),
    SCALAR_COPY(HPE_PROP_BACKGROUND, background),
    SCALAR_COPY(HPE_PROP_WIDTH, width),
    SCALAR_COPY(HPE_PROP_MIN_WIDTH, min_width),
    SCALAR_COPY(HPE_PROP_HEIGHT, height),
    SCALAR_COPY(HPE_PROP_BORDER_RADIUS, border_radius),
    SCALAR_COPY(HPE_PROP_BORDER_COLOR, border_color),
    SCALAR_COPY(HPE_PROP_FLEX_DIR, flex_dir),
    SCALAR_COPY(HPE_PROP_ALIGN_ITEMS, align_items),
    SCALAR_COPY(HPE_PROP_ALIGN_SELF, align_self),
    SCALAR_COPY(HPE_PROP_JUSTIFY, justify),
    SCALAR_COPY(HPE_PROP_GAP, gap),
    SCALAR_COPY(HPE_PROP_FLEX_GROW, flex_grow),
    SCALAR_COPY(HPE_PROP_FONT_SIZE, font_size),
    SCALAR_COPY(HPE_PROP_FONT_WEIGHT, font_weight),
    SCALAR_COPY(HPE_PROP_TEXT_ALIGN, text_align),
    SCALAR_COPY(HPE_PROP_LINE_HEIGHT, line_height),
    SCALAR_COPY(HPE_PROP_WHITE_SPACE, white_space),
    SCALAR_COPY(HPE_PROP_FONT_FAMILY, family),
    SCALAR_COPY(HPE_PROP_POSITION, position),
    /* `z-index` carries both the value and its `auto` flag */
    SCALAR_COPY(HPE_PROP_Z_INDEX, z_index),
    SCALAR_COPY(HPE_PROP_Z_INDEX, z_auto),
    SCALAR_COPY(HPE_PROP_FLOAT, float_side),
    SCALAR_COPY(HPE_PROP_CLEAR, clear_side),
};

/* longhands carried in the second property word (HPE_PROP2_*) */
static const ScalarCopy k_scalars_word2[] = {
    SCALAR_COPY(HPE_PROP2_MIN_HEIGHT, min_height),
    SCALAR_COPY(HPE_PROP2_MAX_WIDTH, max_width),
    SCALAR_COPY(HPE_PROP2_MAX_HEIGHT, max_height),
    SCALAR_COPY(HPE_PROP2_BOX_SIZING, box_sizing),
    SCALAR_COPY(HPE_PROP2_OVERFLOW_X, overflow_x),
    SCALAR_COPY(HPE_PROP2_OVERFLOW_Y, overflow_y),
    SCALAR_COPY(HPE_PROP2_OPACITY, opacity),
    SCALAR_COPY(HPE_PROP2_VISIBILITY, visibility),
    SCALAR_COPY(HPE_PROP2_CURSOR, cursor),
    SCALAR_COPY(HPE_PROP2_TEXT_DECORATION, text_decoration),
    SCALAR_COPY(HPE_PROP2_LETTER_SPACING, letter_spacing),
    SCALAR_COPY(HPE_PROP2_WORD_SPACING, word_spacing),
    SCALAR_COPY(HPE_PROP2_TEXT_INDENT, text_indent),
    SCALAR_COPY(HPE_PROP2_VERTICAL_ALIGN, vertical_align),
    SCALAR_COPY(HPE_PROP2_LIST_STYLE_TYPE, list_style_type),
    SCALAR_COPY(HPE_PROP2_LIST_STYLE_POSITION, list_style_position),
    SCALAR_COPY(HPE_PROP2_BACKGROUND_IMAGE, background_image),
    SCALAR_COPY(HPE_PROP2_BOX_SHADOW, box_shadow),
    SCALAR_COPY(HPE_PROP2_TEXT_SHADOW, text_shadow),
    SCALAR_COPY(HPE_PROP2_TRANSFORM, transform),
    SCALAR_COPY(HPE_PROP2_TRANSITION, has_transition),
    SCALAR_COPY(HPE_PROP2_ANIMATION, has_animation),
};

/* apply the rows whose bit is set in `mask`, copying member bytes dst<-src */
static void apply_scalar_table(HpeStyle *dst, const HpeStyle *src, uint64_t mask,
                               const ScalarCopy *tbl, size_t count)
{
    size_t i;

    for (i = 0; i < count; i++) {
        if (mask & tbl[i].bit)
            memcpy((char *)dst + tbl[i].offset, (const char *)src + tbl[i].offset,
                   tbl[i].size);
    }
}

/* copies the scalar longhands carried in the first property word */
static void apply_first_word_scalars(HpeStyle *dst, const HpeStyle *src, uint64_t s)
{
    apply_scalar_table(dst, src, s, k_scalars_word1,
                       sizeof(k_scalars_word1) / sizeof(k_scalars_word1[0]));
}

/* copies the scalar longhands carried in the second property word (v0.9) */
static void apply_second_word_scalars(HpeStyle *dst, const HpeStyle *src, uint64_t s2)
{
    apply_scalar_table(dst, src, s2, k_scalars_word2,
                       sizeof(k_scalars_word2) / sizeof(k_scalars_word2[0]));
}

/* per-side border colours default to the shared shorthand when only that one
 * was declared, so a `border-color` still paints all four sides. */
static void apply_border_color_resolved(HpeStyle *dst, const HpeStyle *src, uint64_t s,
                                        uint64_t s2)
{
    int i;

    for (i = 0; i < 4; i++) {
        uint64_t bit = HPE_PROP2_BORDER_TOP_COLOR << i;
        if (s2 & bit)
            dst->border_colors[i] = src->border_colors[i];
        else if (s & HPE_PROP_BORDER_COLOR)
            dst->border_colors[i] = src->border_color;
    }
}

static void apply_decl_masked(HpeStyle *dst, const HpeStyle *src, uint64_t mask,
                              uint64_t mask2)
{
    uint64_t s = src->set & mask;
    uint64_t s2 = src->set2 & mask2;
    int any_side;

    /* the computed style keeps the "declared" bits so consumers (e.g. paint's
     * list-style-type) can tell an explicit value from an inherited one. */
    dst->set |= s;
    dst->set2 |= s2;

    apply_box_edges(dst, src, s);
    apply_first_word_scalars(dst, src, s);
    apply_second_word_scalars(dst, src, s2);

    any_side = (s2 & (HPE_PROP2_BORDER_TOP_COLOR | HPE_PROP2_BORDER_RIGHT_COLOR |
                      HPE_PROP2_BORDER_BOTTOM_COLOR | HPE_PROP2_BORDER_LEFT_COLOR)) != 0;
    apply_border_color_resolved(dst, src, s, s2);
    if (!any_side && (s & HPE_PROP_BORDER_COLOR)) {
        int i;
        for (i = 0; i < 4; i++)
            dst->border_colors[i] = src->border_color;
    }
}

/* applies every declared property */
static void apply_decl(HpeStyle *dst, const HpeStyle *src)
{
    apply_decl_masked(dst, src, ~(uint64_t)0, ~(uint64_t)0);
}

/* applies only the properties flagged `!important` in `src` */
static void apply_decl_important(HpeStyle *dst, const HpeStyle *src)
{
    apply_decl_masked(dst, src, src->imp, src->imp2);
}

typedef struct {
    int specificity;
    int order;
    const HpeStyle *decl;
} HpeMatch;

/* one important declaration is applied in a second pass; remember its origin
 * (inline style attribute beats a stylesheet) and its cascade rank so
 * specificity + source order still decide within the important origin. */
typedef struct {
    const HpeStyle *decl;
    int specificity;
    int order;
    int inline_style;
} HpeImpMatch;

/* the cascade starts from the parent's computed style for inherited properties */
static void inherit_parent_style(HpeStyle *style, const HpeStyle *parent)
{
    style->color = parent->color;
    style->font_size = parent->font_size;
    style->font_weight = parent->font_weight;
    style->text_align = parent->text_align;
    style->line_height = parent->line_height;
    style->white_space = parent->white_space;
    style->family = parent->family;
    style->visibility = parent->visibility;
    style->cursor = parent->cursor;
    style->list_style_type = parent->list_style_type;
    style->list_style_position = parent->list_style_position;
    style->letter_spacing = parent->letter_spacing;
    style->word_spacing = parent->word_spacing;
    style->text_indent = parent->text_indent;
}

/* true when every enclosing @media query matches the viewport at cascade time */
static int rule_media_ok(const HpeRule *rule, const HpeStylesheet *sheet)
{
    int k;
    for (k = 0; k < rule->mq_depth; k++) {
        if (!media_alt_matches(&rule->mq[k], sheet->vw, sheet->vh))
            return 0;
    }
    return 1;
}

static int rule_best_specificity(const HpeRule *rule, const HpeDomNode *node)
{
    int best = -1;
    int j;
    for (j = 0; j < rule->selector_count; j++) {
        if (selector_matches(&rule->selectors[j], node) &&
            rule->selectors[j].specificity > best)
            best = rule->selectors[j].specificity;
    }
    return best;
}

/* collects every stylesheet rule matching `node`, recording its specificity */
static void collect_matches(const HpeStylesheet *sheet, const HpeDomNode *node,
                            HpeMatch **matches, size_t *count, size_t *cap)
{
    int i;

    for (i = 0; i < sheet->rule_count; i++) {
        const HpeRule *rule = &sheet->rules[i];
        int best;

        if (!rule_media_ok(rule, sheet))
            continue;
        best = rule_best_specificity(rule, node);
        if (best < 0)
            continue;
        *matches = (HpeMatch *)hpe_grow(*matches, cap, *count + 1, sizeof(HpeMatch));
        (*matches)[*count].specificity = best;
        (*matches)[*count].order = rule->order;
        (*matches)[*count].decl = rule->decls;
        (*count)++;
    }
}

/* stable insertion sort of the matches by (specificity, source order) */
static void sort_matches(HpeMatch *m, int n)
{
    int i, j;
    for (i = 1; i < n; i++) {
        HpeMatch key = m[i];
        j = i - 1;
        while (j >= 0 && (m[j].specificity > key.specificity ||
                          (m[j].specificity == key.specificity && m[j].order > key.order))) {
            m[j + 1] = m[j];
            j--;
        }
        m[j + 1] = key;
    }
}

/* stable insertion sort of the important matches by the same key */
static void sort_imps(HpeImpMatch *m, int n)
{
    int i, j;
    for (i = 1; i < n; i++) {
        HpeImpMatch key = m[i];
        j = i - 1;
        while (j >= 0 && (m[j].specificity > key.specificity ||
                          (m[j].specificity == key.specificity && m[j].order > key.order))) {
            m[j + 1] = m[j];
            j--;
        }
        m[j + 1] = key;
    }
}

/* parses the element's inline style attribute, returning 1 when non-empty */
static int parse_inline_style(const HpeDomNode *node, HpeStyle *out)
{
    const char *inline_style;

    if (!node || node->type != HPE_NODE_ELEMENT)
        return 0;
    inline_style = hpe_dom_attr(node, "style");
    if (!inline_style || !*inline_style)
        return 0;
    hpe_css_parse_declarations(inline_style, strlen(inline_style), out);
    return 1;
}

/* normal declarations: matching rules first, then the inline style attribute,
 * which beats any normal stylesheet declaration */
static void apply_normal_declarations(HpeStyle *style, const HpeMatch *matches, int n,
                                      const HpeDomNode *node)
{
    HpeStyle decl;
    int i;

    for (i = 0; i < n; i++)
        apply_decl(style, matches[i].decl);
    if (parse_inline_style(node, &decl))
        apply_decl(style, &decl);
}

/* gathers the important declarations (from rules and the inline attribute) that
 * the second pass will resolve */
static void collect_imps(const HpeMatch *matches, int n, const HpeDomNode *node,
                         HpeImpMatch **imps, size_t *count, size_t *cap)
{
    int i;

    for (i = 0; i < n; i++) {
        const HpeStyle *d = matches[i].decl;
        if (!d->imp && !d->imp2)
            continue;
        *imps = (HpeImpMatch *)hpe_grow(*imps, cap, *count + 1, sizeof(HpeImpMatch));
        (*imps)[*count].decl = d;
        (*imps)[*count].specificity = matches[i].specificity;
        (*imps)[*count].order = matches[i].order;
        (*imps)[*count].inline_style = 0;
        (*count)++;
    }
    {
        HpeStyle decl;
        if (parse_inline_style(node, &decl) && (decl.imp || decl.imp2)) {
            *imps = (HpeImpMatch *)hpe_grow(*imps, cap, *count + 1, sizeof(HpeImpMatch));
            (*imps)[*count].decl = (const HpeStyle *)hpe_dom_attr(node, "style"); /* marker */
            (*imps)[*count].specificity = (1 << 20); /* higher than any selector */
            (*imps)[*count].order = 1 << 30;         /* and later than any rule */
            (*imps)[*count].inline_style = 1;
            (*count)++;
        }
    }
}

/* applies the sorted important declarations in one final pass */
static void apply_important_pass(HpeStyle *style, const HpeImpMatch *imps, int n,
                                 const HpeDomNode *node)
{
    int i;
    for (i = 0; i < n; i++) {
        if (imps[i].inline_style) {
            HpeStyle decl;
            parse_inline_style(node, &decl);
            apply_decl_important(style, &decl);
        } else {
            apply_decl_important(style, imps[i].decl);
        }
    }
}

void hpe_css_compute(HpeStylesheet *sheet, const HpeDomNode *node, const HpeStyle *parent,
                     HpeStyle *out)
{
    HpeMatch *matches = NULL;
    size_t match_count = 0, match_cap = 0;
    HpeImpMatch *imps = NULL;
    size_t imp_count = 0, imp_cap = 0;
    HpeStyle style;

    style = hpe_style_default();
    if (parent)
        inherit_parent_style(&style, parent);

    if (sheet && node)
        collect_matches(sheet, node, &matches, &match_count, &match_cap);
    sort_matches(matches, (int)match_count);
    apply_normal_declarations(&style, matches, (int)match_count, node);

    /* second pass: important declarations win over every normal one. Within the
     * important origin an inline `style=""` beats a stylesheet important, then
     * source order decides. */
    collect_imps(matches, (int)match_count, node, &imps, &imp_count, &imp_cap);
    sort_imps(imps, (int)imp_count);
    apply_important_pass(&style, imps, (int)imp_count, node);

    if (matches)
        hpe_free(matches);
    if (imps)
        hpe_free(imps);
    *out = style;
}

/* ---------------- inline style attribute helpers ---------------- */

static void inline_append(char *out, size_t out_size, size_t *olen, const char *fmt, ...)
{
    va_list ap;
    if (*olen >= out_size)
        return;
    va_start(ap, fmt);
    *olen += (size_t)vsnprintf(out + *olen, out_size - *olen, fmt, ap);
    va_end(ap);
    if (*olen > out_size)
        *olen = out_size;
}

void hpe_css_inline_set(HpeDomNode *node, const char *prop, const char *value)
{
    char out[2048];
    size_t olen = 0;
    const char *cur = hpe_dom_attr(node, "style");
    int replaced = 0;

    if (cur) {
        const char *p = cur;
        while (*p) {
            const char *semi = strchr(p, ';');
            const char *end = semi ? semi : p + strlen(p);
            const char *colon = (const char *)memchr(p, ':', (size_t)(end - p));
            if (colon && colon > p) {
                char name[64];
                size_t nlen = (size_t)(colon - p);
                size_t i;
                if (nlen >= sizeof(name))
                    nlen = sizeof(name) - 1;
                for (i = 0; i < nlen; i++)
                    name[i] = (char)hpe_ascii_lower((unsigned char)p[i]);
                name[nlen] = '\0';
                if (strcmp(name, prop) == 0) {
                    if (!replaced && value && *value)
                        inline_append(out, sizeof(out), &olen, "%s: %s; ", prop, value);
                    replaced = 1;
                } else {
                    char pair[512];
                    size_t plen = (size_t)(end - p);
                    if (plen >= sizeof(pair))
                        plen = sizeof(pair) - 1;
                    memcpy(pair, p, plen);
                    pair[plen] = '\0';
                    inline_append(out, sizeof(out), &olen, "%s; ", hpe_trim(pair));
                }
            }
            p = semi ? semi + 1 : end;
        }
    }
    if (!replaced && value && *value)
        inline_append(out, sizeof(out), &olen, "%s: %s;", prop, value);
    out[olen < sizeof(out) ? olen : sizeof(out) - 1] = '\0';
    hpe_dom_set_attr(node, "style", out);
}

int hpe_css_inline_get(const HpeDomNode *node, const char *prop, char *out, size_t out_size)
{
    const char *cur = hpe_dom_attr(node, "style");
    if (!cur)
        return 0;
    {
        const char *p = cur;
        while (*p) {
            const char *semi = strchr(p, ';');
            const char *end = semi ? semi : p + strlen(p);
            const char *colon = (const char *)memchr(p, ':', (size_t)(end - p));
            if (colon && colon > p) {
                char name[64];
                size_t nlen = (size_t)(colon - p);
                size_t i;
                if (nlen >= sizeof(name))
                    nlen = sizeof(name) - 1;
                for (i = 0; i < nlen; i++)
                    name[i] = (char)hpe_ascii_lower((unsigned char)p[i]);
                name[nlen] = '\0';
                if (strcmp(name, prop) == 0) {
                    char val[512];
                    size_t vlen = (size_t)(end - colon - 1);
                    if (vlen >= sizeof(val))
                        vlen = sizeof(val) - 1;
                    memcpy(val, colon + 1, vlen);
                    val[vlen] = '\0';
                    snprintf(out, out_size, "%s", hpe_trim(val));
                    return 1;
                }
            }
            p = semi ? semi + 1 : end;
        }
    }
    return 0;
}

/* ---------------- user agent stylesheet ---------------- */

const char *hpe_ua_stylesheet(void)
{
    return
        "html { display: block; }\n"
        "body { display: block; margin: 8px; }\n"
        "head, script, style, title, meta, link, base, noscript, template { display: none; }\n"
        "div, p, section, article, header, footer, main, nav, aside, ul, ol, li, dl, dt, dd,\n"
        "table, thead, tbody, tfoot, tr, td, th, blockquote, pre, form, fieldset, figure,\n"
        "figcaption, hr, address, center { display: block; }\n"
        "span, a, b, i, em, strong, code, small, sub, sup, label, br, img, u, s, mark\n"
        "  { display: inline; }\n"
        "button, input, select, textarea { display: inline-block; font-size: 13px; text-align: center; }\n"
        "button { padding: 1px 6px; border: 1px solid #767676; background: #efefef; }\n"
        "h1 { display: block; font-size: 32px; font-weight: bold; margin: 21px 0; }\n"
        "h2 { display: block; font-size: 24px; font-weight: bold; margin: 19px 0; }\n"
        "h3 { display: block; font-size: 19px; font-weight: bold; margin: 18px 0; }\n"
        "h4 { display: block; font-size: 16px; font-weight: bold; margin: 21px 0; }\n"
        "h5 { display: block; font-size: 13px; font-weight: bold; margin: 22px 0; }\n"
        "h6 { display: block; font-size: 11px; font-weight: bold; margin: 24px 0; }\n"
        "p { margin: 16px 0; }\n"
        "ul, ol { margin: 16px 0; padding-left: 40px; }\n"
        "blockquote { margin: 16px 40px; }\n"
        "pre { font-size: 13px; margin: 13px 0; }\n"
        "code { font-size: 13px; }\n"
        "b, strong { font-weight: bold; }\n"
        "a { color: #1a0dab; }\n"
        "hr { border: 1px solid #c0c0c0; height: 0; margin: 8px 0; }\n"
        "table { border-color: #c0c0c0; }\n";
}
