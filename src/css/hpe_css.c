#include "css/hpe_css.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* forward declarations used by the public selector-matching entry point */
static int selector_matches(const HpeSelector *sel, const HpeDomNode *node);

/* ---------------- defaults ---------------- */

HpeStyle hpe_style_default(void)
{
    HpeStyle s;
    int i;
    memset(&s, 0, sizeof(s));
    s.set = 0;
    s.set2 = 0;
    s.display = HPE_DISPLAY_INLINE;
    s.color = hpe_rgb(0, 0, 0);
    s.background = hpe_rgba(0, 0, 0, 0);
    s.border_color = hpe_rgba(0, 0, 0, 0);
    s.width = hpe_len_auto();
    s.height = hpe_len_auto();
    s.min_width = hpe_len_auto();
    s.min_height = hpe_len_auto();
    s.max_width = hpe_len_auto();
    s.max_height = hpe_len_auto();
    for (i = 0; i < 4; i++) {
        s.margin[i] = hpe_len_px(0.0f);
        s.padding[i] = hpe_len_px(0.0f);
        s.border_w[i] = 0.0f;
        s.border_colors[i] = hpe_rgba(0, 0, 0, 0);
        s.inset[i] = hpe_len_auto();
    }
    s.border_radius = 0.0f;
    s.flex_dir = HPE_FLEX_ROW;
    s.align_items = HPE_ALIGN_ITEMS_STRETCH;
    s.justify = HPE_JUSTIFY_START;
    s.align_self = HPE_ALIGN_ITEMS_AUTO;
    s.gap = 0.0f;
    s.flex_grow = 0.0f;
    s.font_size = 16.0f;
    s.font_weight = 400;
    s.text_align = HPE_ALIGN_LEFT;
    s.line_height = 0.0f;
    s.white_space = HPE_WS_NORMAL;
    s.family = HPE_FONT_DEFAULT;
    s.position = HPE_POS_STATIC;
    s.z_index = 0;
    s.z_auto = 1;
    s.box_sizing = HPE_BOX_SIZING_CONTENT;
    s.overflow_x = HPE_OVERFLOW_VISIBLE;
    s.overflow_y = HPE_OVERFLOW_VISIBLE;
    s.opacity = 1.0f;
    s.visibility = HPE_VISIBILITY_VISIBLE;
    s.cursor = HPE_CURSOR_AUTO;
    s.text_decoration = HPE_TD_NONE;
    s.letter_spacing = hpe_len_px(0.0f);
    s.word_spacing = hpe_len_px(0.0f);
    s.text_indent = hpe_len_px(0.0f);
    s.vertical_align = HPE_VALIGN_BASELINE;
    s.list_style_type = HPE_LIST_DISC;
    s.list_style_position = HPE_LIST_OUTSIDE;
    s.background_image.kind = HPE_BG_NONE;
    s.transform.a = 1.0f;
    s.transform.b = 0.0f;
    s.transform.c = 0.0f;
    s.transform.d = 1.0f;
    s.transform.e = 0.0f;
    s.transform.f = 0.0f;
    s.transform.identity = 1;
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

/* parses `linear-gradient(...)`; only the 2-stop / n-stop colour-list form with
 * an optional leading direction (`to right`, `to bottom`, `Ndeg`) is supported. */
static int parse_linear_gradient(const char *value, HpeBackground *bg)
{
    const char *lp, *rp;
    int count = 0;
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
        char *seg[16];
        int nseg = 0;
        char *q = body;
        /* split on commas at depth 0 (colour functions have their own commas) */
        {
            int depth = 0;
            char *start = q;
            for (; *q; q++) {
                if (*q == '(')
                    depth++;
                else if (*q == ')')
                    depth--;
                else if (*q == ',' && depth == 0) {
                    *q = '\0';
                    if (nseg < 16)
                        seg[nseg++] = start;
                    start = q + 1;
                }
            }
            if (nseg < 16)
                seg[nseg++] = start;
        }
        for (int i = 0; i < nseg; i++) {
            char *tok = hpe_trim(seg[i]);
            HpeColor c;
            if (!*tok)
                continue;
            /* direction keyword or angle */
            if (hpe_starts_with_i(tok, "to ")) {
                char *dir = hpe_trim(tok + 3);
                if (hpe_str_ieq(dir, "top"))
                    angle = 0.0f;
                else if (hpe_str_ieq(dir, "bottom"))
                    angle = 180.0f;
                else if (hpe_str_ieq(dir, "left"))
                    angle = 270.0f;
                else if (hpe_str_ieq(dir, "right"))
                    angle = 90.0f;
                else if (hpe_str_ieq(dir, "top right") || hpe_str_ieq(dir, "right top"))
                    angle = 45.0f;
                else if (hpe_str_ieq(dir, "bottom right") || hpe_str_ieq(dir, "right bottom"))
                    angle = 135.0f;
                else if (hpe_str_ieq(dir, "top left") || hpe_str_ieq(dir, "left top"))
                    angle = 315.0f;
                else if (hpe_str_ieq(dir, "bottom left") || hpe_str_ieq(dir, "left bottom"))
                    angle = 225.0f;
                continue;
            }
            {
                float a;
                if (parse_float_token(tok, &a) && (strstr(tok, "deg") || strstr(tok, "rad") ||
                                                   strstr(tok, "turn"))) {
                    angle = a;
                    continue;
                }
            }
            /* a colour, optionally followed by a stop position */
            {
                char buf[128];
                char *sp;
                float pos = -1.0f;
                snprintf(buf, sizeof(buf), "%s", tok);
                sp = buf;
                while (*sp && !hpe_is_space(*sp))
                    sp++;
                if (*sp) {
                    float pv;
                    *sp = '\0';
                    if (parse_float_token(hpe_trim(sp + 1), &pv))
                        pos = (pv > 1.0f || strstr(hpe_trim(sp + 1), "%")) ? pv / 100.0f : pv;
                }
                if (!hpe_color_parse(hpe_trim(buf), &c))
                    continue;
                if (count < HPE_MAX_GRADIENT_STOPS) {
                    bg->stops[count].color = c;
                    bg->stops[count].pos = pos;
                    count++;
                }
            }
        }
        hpe_free(body);
    }
    if (count < 2)
        return 0;
    /* fill in missing positions by even distribution */
    if (bg->stops[0].pos < 0.0f)
        bg->stops[0].pos = 0.0f;
    if (bg->stops[count - 1].pos < 0.0f)
        bg->stops[count - 1].pos = 1.0f;
    for (int i = 1; i < count - 1; i++)
        if (bg->stops[i].pos < 0.0f)
            bg->stops[i].pos = (float)i / (float)(count - 1);
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
        {
            char fn[32];
            size_t flen = (size_t)(lp - p);
            if (flen >= sizeof(fn))
                flen = sizeof(fn) - 1;
            memcpy(fn, p, flen);
            fn[flen] = '\0';
            for (size_t i = 0; fn[i]; i++)
                fn[i] = (char)hpe_ascii_lower((unsigned char)fn[i]);
            {
                char *body = hpe_strndup(lp + 1, (size_t)(rp - lp - 1));
                float v1 = 0.0f, v2 = 0.0f;
                char *comma = strchr(body, ',');
                if (comma) {
                    *comma = '\0';
                    parse_float_token(hpe_trim(comma + 1), &v2);
                }
                parse_float_token(hpe_trim(body), &v1);
                {
                    HpeTransform m;
                    m.a = 1.0f; m.b = 0.0f; m.c = 0.0f; m.d = 1.0f; m.e = 0.0f; m.f = 0.0f;
                    m.identity = 1;
                    if (!strcmp(fn, "translate"))
                        m.e = v1, m.f = v2;
                    else if (!strcmp(fn, "translatex"))
                        m.e = v1;
                    else if (!strcmp(fn, "translatey"))
                        m.f = v1;
                    else if (!strcmp(fn, "scale"))
                        m.a = m.d = v1, m.b = m.c = 0.0f, m.e = m.f = 0.0f;
                    else if (!strcmp(fn, "scalex"))
                        m.a = v1;
                    else if (!strcmp(fn, "scaley"))
                        m.d = v1;
                    else if (!strcmp(fn, "rotate")) {
                        float r = v1 * 3.14159265f / 180.0f;
                        m.a = cosf(r);
                        m.b = sinf(r);
                        m.c = -sinf(r);
                        m.d = cosf(r);
                    } else if (!strcmp(fn, "skewx")) {
                        float r = v1 * 3.14159265f / 180.0f;
                        m.c = tanf(r);
                    } else if (!strcmp(fn, "skewy")) {
                        float r = v1 * 3.14159265f / 180.0f;
                        m.b = tanf(r);
                    } else if (!strcmp(fn, "matrix")) {
                        /* not supported: leave the identity */
                    }
                    m.identity = (m.a == 1.0f && m.b == 0.0f && m.c == 0.0f && m.d == 1.0f &&
                                  m.e == 0.0f && m.f == 0.0f);
                    if (!m.identity)
                        mat_mul(&acc, &acc, &m);
                }
                hpe_free(body);
            }
        }
        p = rp + 1;
    }
    *out = acc;
}

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

    if (!strcmp(lprop, "display")) {
        if (!strcmp(value, "block")) {
            d->display = HPE_DISPLAY_BLOCK;
            d->set |= HPE_PROP_DISPLAY;
        } else if (!strcmp(value, "none")) {
            d->display = HPE_DISPLAY_NONE;
            d->set |= HPE_PROP_DISPLAY;
        } else if (!strcmp(value, "inline-block")) {
            d->display = HPE_DISPLAY_INLINE_BLOCK;
            d->set |= HPE_PROP_DISPLAY;
        } else if (!strcmp(value, "flex") || !strcmp(value, "inline-flex")) {
            /* inline-flex is blockified: the engine has no inline-level flex container */
            d->display = HPE_DISPLAY_FLEX;
            d->set |= HPE_PROP_DISPLAY;
        } else if (!strcmp(value, "inline")) {
            d->display = HPE_DISPLAY_INLINE;
            d->set |= HPE_PROP_DISPLAY;
        } else {
            d->display = HPE_DISPLAY_BLOCK;
            d->set |= HPE_PROP_DISPLAY;
        }
        return;
    }
    if (!strcmp(lprop, "color")) {
        HpeColor c;
        if (hpe_color_parse(value, &c)) {
            d->color = c;
            d->set |= HPE_PROP_COLOR;
        }
        return;
    }
    if (!strcmp(lprop, "background") || !strcmp(lprop, "background-color")) {
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
        if (hpe_color_parse(value, &c)) {
            d->background = c;
            d->set |= HPE_PROP_BACKGROUND;
            return;
        }
        {
            /* pick the last token that parses as a color (handles "1px solid #ccc") */
            char buf[256];
            char *parts[8];
            int n, i;
            snprintf(buf, sizeof(buf), "%s", value);
            n = split_ws(buf, parts, 8);
            for (i = n - 1; i >= 0; i--) {
                if (hpe_color_parse(parts[i], &c)) {
                    d->background = c;
                    d->set |= HPE_PROP_BACKGROUND;
                    return;
                }
            }
        }
        return;
    }
    if (!strcmp(lprop, "min-width")) {
        HpeLength len = hpe_len_parse(value);
        if (len.unit == HPE_LEN_PX || len.unit == HPE_LEN_PCT) {
            d->min_width = len;
            d->set |= HPE_PROP_MIN_WIDTH;
        }
        return;
    }
    if (!strcmp(lprop, "width")) {
        d->width = hpe_len_parse(value);
        d->set |= HPE_PROP_WIDTH;
        return;
    }
    if (!strcmp(lprop, "height")) {
        d->height = hpe_len_parse(value);
        d->set |= HPE_PROP_HEIGHT;
        return;
    }
    if (!strcmp(lprop, "margin")) {
        set_sides(d, value, 1);
        return;
    }
    if (!strcmp(lprop, "padding")) {
        set_sides(d, value, 0);
        return;
    }
    if (!strncmp(lprop, "margin-", 7)) {
        HpeLength len = hpe_len_parse(value);
        int idx = -1;
        if (!strcmp(lprop + 7, "top"))
            idx = 0;
        else if (!strcmp(lprop + 7, "right"))
            idx = 1;
        else if (!strcmp(lprop + 7, "bottom"))
            idx = 2;
        else if (!strcmp(lprop + 7, "left"))
            idx = 3;
        if (idx >= 0) {
            d->margin[idx] = len;
            d->set |= HPE_PROP_MARGIN_TOP << idx;
        }
        return;
    }
    if (!strncmp(lprop, "padding-", 8)) {
        HpeLength len = hpe_len_parse(value);
        int idx = -1;
        if (!strcmp(lprop + 8, "top"))
            idx = 0;
        else if (!strcmp(lprop + 8, "right"))
            idx = 1;
        else if (!strcmp(lprop + 8, "bottom"))
            idx = 2;
        else if (!strcmp(lprop + 8, "left"))
            idx = 3;
        if (idx >= 0) {
            d->padding[idx] = len;
            d->set |= HPE_PROP_PADDING_TOP << idx;
        }
        return;
    }
    /* `border-color: c1 [c2 [c3 [c4]]]` — top, right, bottom, left */
    if (!strcmp(lprop, "border-color")) {
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
        if (got > 0) {
            /* CSS expands 1..4 values by repetition (T R B L) */
            static const int pick4[4] = {0, 1, 2, 3};
            static const int pick3[4] = {0, 1, 2, 1};
            static const int pick2[4] = {0, 1, 0, 1};
            static const int pick1[4] = {0, 0, 0, 0};
            const int *pick = got == 1 ? pick1 : (got == 2 ? pick2 : (got == 3 ? pick3 : pick4));
            for (i = 0; i < 4; i++) {
                d->border_colors[i] = vals[pick[i]];
                d->set2 |= HPE_PROP2_BORDER_TOP_COLOR << i;
            }
            d->border_color = vals[0]; /* keep the shorthand default in sync */
            d->set |= HPE_PROP_BORDER_COLOR;
        }
        return;
    }
    if (!strcmp(lprop, "border") || !strcmp(lprop, "border-width") ||
        !strcmp(lprop, "border-top") ||
        !strcmp(lprop, "border-bottom") || !strcmp(lprop, "border-left") ||
        !strcmp(lprop, "border-right")) {
        char buf[256];
        char *parts[8];
        int n, i, side = -1;
        uint64_t side_bit = 0;
        snprintf(buf, sizeof(buf), "%s", value);
        n = split_ws(buf, parts, 8);
        if (!strcmp(lprop, "border-top")) {
            side = 0;
            side_bit = HPE_PROP_BORDER_TOP;
        } else if (!strcmp(lprop, "border-right")) {
            side = 1;
            side_bit = HPE_PROP_BORDER_RIGHT;
        } else if (!strcmp(lprop, "border-bottom")) {
            side = 2;
            side_bit = HPE_PROP_BORDER_BOTTOM;
        } else if (!strcmp(lprop, "border-left")) {
            side = 3;
            side_bit = HPE_PROP_BORDER_LEFT;
        }
        for (i = 0; i < n; i++) {
            HpeColor c;
            HpeLength len;
            if (!strcmp(parts[i], "none")) {
                /* `border: none` / `border-top: none` -> zero width on that side */
                if (side >= 0) {
                    d->border_w[side] = 0.0f;
                    d->set |= side_bit;
                } else {
                    int k;
                    for (k = 0; k < 4; k++)
                        d->border_w[k] = 0.0f;
                    d->set |= HPE_PROP_BORDER_WIDTH;
                }
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
            if (len.unit == HPE_LEN_PX) {
                if (side >= 0) {
                    d->border_w[side] = len.value;
                    d->set |= side_bit;
                } else {
                    int k;
                    for (k = 0; k < 4; k++)
                        d->border_w[k] = len.value;
                    d->set |= HPE_PROP_BORDER_WIDTH;
                }
            }
        }
        return;
    }
    if (!strcmp(lprop, "border-radius")) {
        HpeLength len = hpe_len_parse(value);
        if (len.unit == HPE_LEN_PX) {
            d->border_radius = len.value;
            d->set |= HPE_PROP_BORDER_RADIUS;
        } else if (len.unit == HPE_LEN_PCT) {
            /* percentages need the border box: stored negated, solved at paint time */
            d->border_radius = -len.value;
            d->set |= HPE_PROP_BORDER_RADIUS;
        }
        return;
    }
    if (!strcmp(lprop, "inset")) {
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
        return;
    }
    if (!strcmp(lprop, "flex-direction")) {
        d->flex_dir = !strcmp(value, "column") ? HPE_FLEX_COLUMN : HPE_FLEX_ROW;
        d->set |= HPE_PROP_FLEX_DIR;
        return;
    }
    if (!strcmp(lprop, "align-items")) {
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
        return;
    }
    if (!strcmp(lprop, "align-self")) {
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
        return;
    }
    if (!strcmp(lprop, "justify-content")) {
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
        return;
    }
    if (!strcmp(lprop, "gap") || !strcmp(lprop, "column-gap") || !strcmp(lprop, "row-gap")) {
        HpeLength len = hpe_len_parse(value);
        if (len.unit == HPE_LEN_PX || len.unit == HPE_LEN_PCT)
            d->gap = len.value;
        else
            d->gap = 0.0f;
        d->set |= HPE_PROP_GAP;
        return;
    }
    if (!strcmp(lprop, "flex") || !strcmp(lprop, "flex-grow")) {
        HpeLength len = hpe_len_parse(value);
        if (len.unit == HPE_LEN_PX || len.unit == HPE_LEN_PCT)
            d->flex_grow = len.value;
        else
            d->flex_grow = 0.0f;
        d->set |= HPE_PROP_FLEX_GROW;
        return;
    }
    if (!strcmp(lprop, "font-size")) {
        float v = 0.0f;
        HpeLength len;
        if (!strcmp(value, "small")) {
            v = 13.0f;
        } else if (!strcmp(value, "medium")) {
            v = 16.0f;
        } else if (!strcmp(value, "large")) {
            v = 18.0f;
        } else if (!strcmp(value, "x-large")) {
            v = 24.0f;
        } else if (!strcmp(value, "xx-large")) {
            v = 32.0f;
        } else if (!strcmp(value, "smaller")) {
            v = 13.0f;
        } else if (!strcmp(value, "larger")) {
            v = 19.0f;
        } else {
            len = hpe_len_parse(value);
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
        return;
    }
    if (!strcmp(lprop, "font-weight")) {
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
        return;
    }
    if (!strcmp(lprop, "text-align")) {
        if (!strcmp(value, "center"))
            d->text_align = HPE_ALIGN_CENTER;
        else if (!strcmp(value, "right"))
            d->text_align = HPE_ALIGN_RIGHT;
        else
            d->text_align = HPE_ALIGN_LEFT;
        d->set |= HPE_PROP_TEXT_ALIGN;
        return;
    }
    if (!strcmp(lprop, "line-height")) {
        float v;
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
        if (hpe_len_parse(value).unit == HPE_LEN_PX) {
            d->line_height = hpe_len_parse(value).value;
            d->set |= HPE_PROP_LINE_HEIGHT;
        }
        return;
    }
    if (!strcmp(lprop, "white-space")) {
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
        return;
    }
    if (!strcmp(lprop, "font-family")) {
        /* take the first recognized family in the comma list */
        char buf[256];
        HpeFontFamily fam = HPE_FONT_DEFAULT;
        snprintf(buf, sizeof(buf), "%s", value);
        {
            char *p = buf;
            while (*p) {
                char *q = strchr(p, ',');
                char name[128];
                size_t len;
                if (q)
                    *q = '\0';
                {
                    char *t = hpe_trim(p);
                    size_t tl = strlen(t);
                    if (tl >= sizeof(name))
                        tl = sizeof(name) - 1;
                    memcpy(name, t, tl);
                    name[tl] = '\0';
                }
                len = strlen(name);
                if (len >= 2 && name[0] == '"' && name[len - 1] == '"') {
                    memmove(name, name + 1, len - 2);
                    name[len - 2] = '\0';
                    len -= 2;
                }
                {
                    int j;
                    for (j = 0; name[j]; j++)
                        name[j] = (char)hpe_ascii_lower((unsigned char)name[j]);
                }
                if (strstr(name, "mono") || !strcmp(name, "courier") ||
                    strstr(name, "consolas"))
                    fam = HPE_FONT_MONO;
                else if (strstr(name, "serif") || strstr(name, "times") ||
                         strstr(name, "georgia") || strstr(name, "simsun") ||
                         strstr(name, "song") || strstr(name, "宋"))
                    fam = HPE_FONT_SERIF;
                else if (strstr(name, "sans") || strstr(name, "yahei") ||
                         strstr(name, "arial") || strstr(name, "helvetica") ||
                         strstr(name, "segoe") || strstr(name, "黑") ||
                         strstr(name, "微软雅黑") || strstr(name, "hei"))
                    fam = HPE_FONT_SANS;
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
        }
        d->family = fam;
        d->set |= HPE_PROP_FONT_FAMILY;
        return;
    }
    if (!strcmp(lprop, "position")) {
        if (!strcmp(value, "relative"))
            d->position = HPE_POS_RELATIVE;
        else if (!strcmp(value, "absolute"))
            d->position = HPE_POS_ABSOLUTE;
        else if (!strcmp(value, "fixed"))
            d->position = HPE_POS_FIXED;
        else
            d->position = HPE_POS_STATIC;
        d->set |= HPE_PROP_POSITION;
        return;
    }
    if (!strcmp(lprop, "top") || !strcmp(lprop, "right") || !strcmp(lprop, "bottom") ||
        !strcmp(lprop, "left")) {
        HpeLength len = hpe_len_parse(value);
        int idx = -1;
        uint32_t bit = 0;
        if (!strcmp(lprop, "top")) {
            idx = 0;
            bit = HPE_PROP_INSET_TOP;
        } else if (!strcmp(lprop, "right")) {
            idx = 1;
            bit = HPE_PROP_INSET_RIGHT;
        } else if (!strcmp(lprop, "bottom")) {
            idx = 2;
            bit = HPE_PROP_INSET_BOTTOM;
        } else {
            idx = 3;
            bit = HPE_PROP_INSET_LEFT;
        }
        d->inset[idx] = len;
        d->set |= bit;
        return;
    }
    if (!strcmp(lprop, "z-index")) {
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
        return;
    }
    if (!strcmp(lprop, "float")) {
        if (!strcmp(value, "left"))
            d->float_side = HPE_FLOAT_LEFT;
        else if (!strcmp(value, "right"))
            d->float_side = HPE_FLOAT_RIGHT;
        else
            d->float_side = HPE_FLOAT_NONE;
        d->set |= HPE_PROP_FLOAT;
        return;
    }
    if (!strcmp(lprop, "clear")) {
        if (!strcmp(value, "left"))
            d->clear_side = HPE_CLEAR_LEFT;
        else if (!strcmp(value, "right"))
            d->clear_side = HPE_CLEAR_RIGHT;
        else if (!strcmp(value, "both"))
            d->clear_side = HPE_CLEAR_BOTH;
        else
            d->clear_side = HPE_CLEAR_NONE;
        d->set |= HPE_PROP_CLEAR;
        return;
    }

    /* ---------------- v0.9 additions ---------------- */

    if (!strcmp(lprop, "min-height") || !strcmp(lprop, "max-width") ||
        !strcmp(lprop, "max-height")) {
        HpeLength len = hpe_len_parse(value);
        uint64_t bit = !strcmp(lprop, "min-height")   ? HPE_PROP2_MIN_HEIGHT
                       : !strcmp(lprop, "max-width") ? HPE_PROP2_MAX_WIDTH
                                                     : HPE_PROP2_MAX_HEIGHT;
        if (!strcmp(lprop, "min-height")) {
            d->min_height = len;
        } else if (!strcmp(lprop, "max-width")) {
            d->max_width = len;
        } else {
            d->max_height = len;
        }
        d->set2 |= bit;
        return;
    }
    if (!strcmp(lprop, "box-sizing")) {
        d->box_sizing = !strcmp(value, "border-box") ? HPE_BOX_SIZING_BORDER
                                                     : HPE_BOX_SIZING_CONTENT;
        d->set2 |= HPE_PROP2_BOX_SIZING;
        return;
    }
    if (!strcmp(lprop, "overflow") || !strcmp(lprop, "overflow-x") ||
        !strcmp(lprop, "overflow-y")) {
        char buf[128];
        char *parts[2];
        int n;
#define OV_KEYWORD(tok, out)                                                                      \
    do {                                                                                          \
        if (!strcmp((tok), "hidden"))                                                             \
            (out) = HPE_OVERFLOW_HIDDEN;                                                          \
        else if (!strcmp((tok), "scroll"))                                                        \
            (out) = HPE_OVERFLOW_SCROLL;                                                          \
        else if (!strcmp((tok), "auto"))                                                          \
            (out) = HPE_OVERFLOW_AUTO;                                                            \
        else                                                                                      \
            (out) = HPE_OVERFLOW_VISIBLE;                                                         \
    } while (0)
        if (!strcmp(lprop, "overflow")) {
            /* one value sets both axes; two values are x then y */
            HpeOverflow ox, oy;
            snprintf(buf, sizeof(buf), "%s", value);
            n = split_ws(buf, parts, 2);
            OV_KEYWORD(parts[0], ox);
            oy = ox;
            if (n > 1)
                OV_KEYWORD(parts[1], oy);
            d->overflow_x = ox;
            d->overflow_y = oy;
            d->set2 |= HPE_PROP2_OVERFLOW_X | HPE_PROP2_OVERFLOW_Y;
        } else if (!strcmp(lprop, "overflow-x")) {
            OV_KEYWORD(value, d->overflow_x);
            d->set2 |= HPE_PROP2_OVERFLOW_X;
        } else {
            OV_KEYWORD(value, d->overflow_y);
            d->set2 |= HPE_PROP2_OVERFLOW_Y;
        }
#undef OV_KEYWORD
        return;
    }
    if (!strcmp(lprop, "opacity")) {
        float v;
        if (parse_plain_number(value, &v)) {
            if (v < 0.0f)
                v = 0.0f;
            if (v > 1.0f)
                v = 1.0f;
            d->opacity = v;
            d->set2 |= HPE_PROP2_OPACITY;
        } else if (!strcmp(value, "inherit")) {
            /* handled earlier */
        }
        return;
    }
    if (!strcmp(lprop, "visibility")) {
        if (!strcmp(value, "hidden"))
            d->visibility = HPE_VISIBILITY_HIDDEN;
        else if (!strcmp(value, "collapse"))
            d->visibility = HPE_VISIBILITY_COLLAPSE;
        else
            d->visibility = HPE_VISIBILITY_VISIBLE;
        d->set2 |= HPE_PROP2_VISIBILITY;
        return;
    }
    if (!strcmp(lprop, "cursor")) {
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
        return;
    }
    if (!strcmp(lprop, "text-decoration") || !strcmp(lprop, "text-decoration-line")) {
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
        return;
    }
    if (!strcmp(lprop, "letter-spacing") || !strcmp(lprop, "word-spacing") ||
        !strcmp(lprop, "text-indent")) {
        HpeLength len = hpe_len_parse(value);
        if (len.unit == HPE_LEN_AUTO) /* `normal` */
            len = hpe_len_px(0.0f);
        if (!strcmp(lprop, "letter-spacing")) {
            d->letter_spacing = len;
            d->set2 |= HPE_PROP2_LETTER_SPACING;
        } else if (!strcmp(lprop, "word-spacing")) {
            d->word_spacing = len;
            d->set2 |= HPE_PROP2_WORD_SPACING;
        } else {
            d->text_indent = len;
            d->set2 |= HPE_PROP2_TEXT_INDENT;
        }
        return;
    }
    if (!strcmp(lprop, "vertical-align")) {
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
        return;
    }
    if (!strcmp(lprop, "list-style-type") || !strcmp(lprop, "list-style")) {
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
        return;
    }
    if (!strcmp(lprop, "list-style-position")) {
        d->list_style_position = !strcmp(value, "inside") ? HPE_LIST_INSIDE : HPE_LIST_OUTSIDE;
        d->set2 |= HPE_PROP2_LIST_STYLE_POSITION;
        return;
    }
    if (!strcmp(lprop, "background-image")) {
        if (parse_linear_gradient(value, &d->background_image)) {
            d->set2 |= HPE_PROP2_BACKGROUND_IMAGE;
        } else if (!strcmp(value, "none")) {
            d->background_image.kind = HPE_BG_NONE;
            d->set2 |= HPE_PROP2_BACKGROUND_IMAGE;
        }
        return;
    }
    if (!strcmp(lprop, "box-shadow")) {
        if (parse_shadow(value, &d->box_shadow))
            d->set2 |= HPE_PROP2_BOX_SHADOW;
        else if (!strcmp(value, "none"))
            d->box_shadow.valid = 0, d->set2 |= HPE_PROP2_BOX_SHADOW;
        return;
    }
    if (!strcmp(lprop, "text-shadow")) {
        if (parse_shadow(value, &d->text_shadow))
            d->set2 |= HPE_PROP2_TEXT_SHADOW;
        else if (!strcmp(value, "none"))
            d->text_shadow.valid = 0, d->set2 |= HPE_PROP2_TEXT_SHADOW;
        return;
    }
    if (!strcmp(lprop, "transform")) {
        parse_transform(value, &d->transform);
        d->set2 |= HPE_PROP2_TRANSFORM;
        return;
    }
    if (!strcmp(lprop, "transition")) {
        d->has_transition = !strcmp(value, "none") ? 0 : 1;
        d->set2 |= HPE_PROP2_TRANSITION;
        return;
    }
    if (!strcmp(lprop, "animation")) {
        d->has_animation = !strcmp(value, "none") ? 0 : 1;
        d->set2 |= HPE_PROP2_ANIMATION;
        return;
    }
    /* per-side border colours: `border-top-color` etc. */
    if (!strncmp(lprop, "border-", 7) && strstr(lprop + 7, "color")) {
        int side = -1;
        uint64_t bit = 0;
        if (!strncmp(lprop + 7, "top", 3)) {
            side = 0;
            bit = HPE_PROP2_BORDER_TOP_COLOR;
        } else if (!strncmp(lprop + 7, "right", 5)) {
            side = 1;
            bit = HPE_PROP2_BORDER_RIGHT_COLOR;
        } else if (!strncmp(lprop + 7, "bottom", 6)) {
            side = 2;
            bit = HPE_PROP2_BORDER_BOTTOM_COLOR;
        } else if (!strncmp(lprop + 7, "left", 4)) {
            side = 3;
            bit = HPE_PROP2_BORDER_LEFT_COLOR;
        }
        if (side >= 0) {
            HpeColor c;
            if (hpe_color_parse(value, &c)) {
                d->border_colors[side] = c;
                d->border_color = c; /* keep the shorthand in sync */
                d->set |= HPE_PROP_BORDER_COLOR;
                d->set2 |= bit;
            }
        }
        return;
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

static int parse_compound(const char **pp, const char *end, HpeCompound *out)
{
    const char *p = *pp;
    char ident[128];

    memset(out, 0, sizeof(*out));

    if (p < end && *p == '*') {
        p++;
    } else if (p < end && is_ident_char(*p)) {
        size_t i;
        if (read_ident(&p, end, ident, sizeof(ident))) {
            for (i = 0; ident[i]; i++)
                ident[i] = (char)hpe_ascii_lower((unsigned char)ident[i]);
            out->tag = hpe_strdup(ident);
        }
    }

    while (p < end) {
        if (*p == '.') {
            p++;
            if (!read_ident(&p, end, ident, sizeof(ident)))
                return 0;
            compound_add_class(out, ident);
        } else if (*p == '#') {
            p++;
            if (!read_ident(&p, end, ident, sizeof(ident)))
                return 0;
            if (out->id)
                hpe_free(out->id);
            out->id = hpe_strdup(ident);
        } else if (*p == ':') {
            const char *pseudo_start;
            p++;
            pseudo_start = p;
            if (!read_ident(&p, end, ident, sizeof(ident)))
                return 0;
            if (hpe_str_ieq(ident, "root")) {
                if (!out->tag)
                    out->tag = hpe_strdup("html");
            } else if (hpe_str_ieq(ident, "hover")) {
                out->pseudo_hover = 1;
            } else {
                HPE_UNUSED(pseudo_start);
                /* other dynamic / structural pseudo-classes are ignored so
                 * they do not fail the selector */
            }
        } else if (*p == '[') {
            const char *a = p + 1;
            const char *close = a;
            char aname[128], aop[8], avalue[256];
            int has_val = 0;
            aname[0] = '\0';
            aop[0] = '\0';
            avalue[0] = '\0';
            while (close < end && *close != ']')
                close++;
            if (close >= end)
                return 0; /* unbalanced */
            {
                char *tmp = hpe_strndup(a, (size_t)(close - a));
                char *sp = tmp;
                char *namep = aname;
                /* name */
                while (*sp && *sp != ' ' && *sp != '=')
                    *namep++ = *sp++;
                *namep = '\0';
                /* operator + value */
                while (*sp == ' ')
                    sp++;
                if (*sp == '=') {
                    const char *v;
                    *aop = '=';
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
                    {
                        size_t vl = strlen(v);
                        if (vl >= 2 && (v[0] == '"' || v[0] == '\'') && v[vl - 1] == v[0]) {
                            v++;
                            vl -= 2;
                        }
                        if (vl >= sizeof(avalue))
                            vl = sizeof(avalue) - 1;
                        memcpy(avalue, v, vl);
                        avalue[vl] = '\0';
                        has_val = 1;
                    }
                } else if (*sp == '~' || *sp == '|' || *sp == '^' || *sp == '$' ||
                           *sp == '*') {
                    /* malformed but tolerate */
                    aop[0] = *sp;
                    aop[1] = '=';
                    aop[2] = '\0';
                    has_val = 1;
                }
                hpe_free(tmp);
            }
            {
                size_t nl = strlen(aname);
                int j;
                for (j = 0; j < (int)nl; j++)
                    aname[j] = (char)hpe_ascii_lower((unsigned char)aname[j]);
            }
            {
                HpeAttrSel *as;
                out->attrs = (HpeAttrSel *)hpe_grow(out->attrs, &out->attr_cap,
                                                   (size_t)out->attr_count + 1, sizeof(HpeAttrSel));
                as = &out->attrs[out->attr_count++];
                as->name = hpe_strdup(aname);
                as->op = hpe_strdup(aop);
                as->value = hpe_strdup(has_val ? avalue : "");
            }
            p = close + 1;
        } else if (*p == ':') {
            const char *pseudo_start;
            p++;
            pseudo_start = p;
            if (!read_ident(&p, end, ident, sizeof(ident)))
                return 0;
            if (hpe_str_ieq(ident, "root")) {
                if (!out->tag)
                    out->tag = hpe_strdup("html");
            } else {
                /* dynamic / structural pseudo-classes not evaluated statically:
                 * do not let them fail the selector */
                HPE_UNUSED(pseudo_start);
            }
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

        memset(&sel, 0, sizeof(sel));
        {
            const char *q = part_start;
            int first = 1;
            while (q < part_end) {
                int comb = HPE_COMB_DESCENDANT;
                HpeCompound comp;

                while (q < part_end && hpe_is_space(*q))
                    q++;
                if (!first) {
                    /* leading whitespace was already skipped above, so any
                     * token that is not an explicit combinator means the
                     * parts were space-separated: descendant */
                    if (q < part_end && *q == '>') {
                        comb = HPE_COMB_CHILD;
                        q++;
                    } else if (q < part_end && *q == '+') {
                        comb = HPE_COMB_ADJ_SIBLING;
                        q++;
                    } else if (q < part_end && *q == '~') {
                        comb = HPE_COMB_GEN_SIBLING;
                        q++;
                    } else {
                        comb = HPE_COMB_DESCENDANT;
                    }
                    while (q < part_end && hpe_is_space(*q))
                        q++;
                }
                if (q >= part_end)
                    break;
                if (!parse_compound(&q, part_end, &comp)) {
                    compound_free(&comp);
                    sel.count = -1;
                    break;
                }
                comp.comb = first ? HPE_COMB_DESCENDANT : comb;
                selector_add_part(&sel, &comp);
                first = 0;
            }
        }
        if (sel.count > 0) {
            rule->selectors = (HpeSelector *)hpe_grow(rule->selectors, &rule->selector_cap,
                                                      (size_t)rule->selector_count + 1,
                                                      sizeof(HpeSelector));
            rule->selectors[rule->selector_count++] = sel;
        } else {
            if (sel.count < 0)
                selector_free(&sel);
            else
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
    if (c->attr_count > 0) {
        int k;
        for (k = 0; k < c->attr_count; k++) {
            HpeAttrSel *as = &c->attrs[k];
            const char *attr_val = hpe_dom_attr(node, as->name);
            if (as->op[0] == '\0') {
                if (!attr_val)
                    return 0;
                continue; /* presence */
            }
            if (!attr_val)
                return 0;
            if (as->op[0] == '=' && as->op[1] == '\0') {
                if (strcmp(attr_val, as->value) != 0)
                    return 0;
            } else if (as->op[0] == '~') {
                if (!attr_word_contains(attr_val, as->value))
                    return 0;
            } else if (as->op[0] == '|') {
                size_t vl = strlen(as->value);
                if (strncmp(attr_val, as->value, vl) != 0)
                    return 0;
                if (attr_val[vl] != '\0' && attr_val[vl] != '-')
                    return 0;
            } else if (as->op[0] == '^') {
                if (strncmp(attr_val, as->value, strlen(as->value)) != 0)
                    return 0;
            } else if (as->op[0] == '$') {
                size_t al = strlen(attr_val), vl = strlen(as->value);
                if (al < vl || strcmp(attr_val + al - vl, as->value) != 0)
                    return 0;
            } else if (as->op[0] == '*') {
                if (!strstr(attr_val, as->value))
                    return 0;
            }
        }
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
static void parse_media_query(const char *text, size_t length, HpeMediaQuery *mq)
{
    const char *end = text + length;
    const char *p = text;
    int ai = 0;

    memset(mq, 0, sizeof(*mq));
    mq->present = 1;
    while (p < end) {
        const char *seg_end = p;
        char *q;
        int negate = 0;
        char *buf;
        char *tok;

        if (ai >= HPE_MEDIA_MAX_ALT)
            break;
        while (seg_end < end && *seg_end != ',')
            seg_end++;

        mq->alts[ai].type = HPE_MTYPE_UNSET;
        /* operate on a NUL-terminated copy so the tokeniser can be reused */
        buf = hpe_strndup(p, (size_t)(seg_end - p));
        tok = hpe_trim(buf);
        q = tok;
        if (hpe_starts_with_i(q, "not")) {
            negate = 1;
            q = hpe_trim((char *)q + 3);
        } else if (hpe_starts_with_i(q, "only")) {
            q = hpe_trim((char *)q + 4);
        }
        mq->alts[ai].negate = negate;
        while (*q) {
            if (*q == '(') {
                char *close = strchr(q, ')');
                char *inner;
                char *colon;
                int fid;
                float fv;
                if (!close)
                    break;
                *close = '\0';
                inner = hpe_trim(q + 1);
                colon = strchr(inner, ':');
                if (colon) {
                    *colon = '\0';
                }
                fid = media_feature_id(hpe_trim(inner));
                if (!fid || !colon || !media_feature_value(hpe_trim(colon + 1), &fv)) {
                    mq->alts[ai].unknown = 1;
                } else if (mq->alts[ai].cond_count < HPE_MEDIA_MAX_COND) {
                    mq->alts[ai].conds[mq->alts[ai].cond_count].feature = fid;
                    mq->alts[ai].conds[mq->alts[ai].cond_count].value = fv;
                    mq->alts[ai].cond_count++;
                }
                q = close + 1;
            } else {
                /* a bare media type token */
                char *sp = q;
                while (*sp && !hpe_is_space(*sp))
                    sp++;
                {
                    char saved = *sp;
                    *sp = '\0';
                    if (hpe_str_ieq(q, "all"))
                        mq->alts[ai].type = HPE_MTYPE_ALL;
                    else if (hpe_str_ieq(q, "screen"))
                        mq->alts[ai].type = HPE_MTYPE_SCREEN;
                    else if (hpe_str_ieq(q, "print"))
                        mq->alts[ai].type = HPE_MTYPE_PRINT;
                    else
                        mq->alts[ai].unknown = 1;
                    *sp = saved;
                }
                q = sp;
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
            int depth = 0;
            int is_media;
            int stacked = 0;
            while (q < end && *q != '{' && *q != ';')
                q++;
            if (q >= end)
                break;
            if (*q == ';') {
                p = q + 1;
                continue;
            }
            is_media = (size_t)(q - p) >= 6 && hpe_str_ieq_n(p, "@media", 6);
            {
                const char *block = q + 1;
                const char *scan = block;
                depth = 1;
                while (scan < end && depth > 0) {
                    if (*scan == '{')
                        depth++;
                    else if (*scan == '}')
                        depth--;
                    if (depth > 0)
                        scan++;
                }
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
                p = scan < end ? scan + 1 : end;
            }
            continue;
        }

        {
            const char *sel_start = p;
            const char *decl_start;
            const char *scan;
            size_t sel_len, decl_len;
            HpeRule rule;

            while (p < end && *p != '{')
                p++;
            if (p >= end)
                break;
            sel_len = (size_t)(p - sel_start);
            p++; /* '{' */
            decl_start = p;
            scan = p;
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
            decl_len = (size_t)(scan - decl_start);
            p = (scan < end) ? scan + 1 : end;

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
                    if (mq_depth > 0) {
                        int k;
                        for (k = 0; k < mq_depth; k++)
                            rule.mq[k] = mq_stack[k];
                        rule.mq_depth = mq_depth;
                    }
                    sheet->rules = (HpeRule *)hpe_grow(sheet->rules, &sheet->rule_cap,
                                                       (size_t)sheet->rule_count + 1,
                                                       sizeof(HpeRule));
                    sheet->rules[sheet->rule_count++] = rule;
                    continue;
                }
            }
            rule_free(&rule);
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

static void apply_decl_masked(HpeStyle *dst, const HpeStyle *src, uint64_t mask,
                              uint64_t mask2)
{
    uint64_t s = src->set & mask;
    uint64_t s2_all = src->set2 & mask2;
    int i;
    /* the computed style keeps the "declared" bits so consumers (e.g. paint's
     * list-style-type) can tell an explicit value from an inherited one. */
    dst->set |= s;
    dst->set2 |= s2_all;
    if (s & HPE_PROP_DISPLAY)
        dst->display = src->display;
    if (s & HPE_PROP_COLOR)
        dst->color = src->color;
    if (s & HPE_PROP_BACKGROUND)
        dst->background = src->background;
    if (s & HPE_PROP_WIDTH)
        dst->width = src->width;
    if (s & HPE_PROP_MIN_WIDTH)
        dst->min_width = src->min_width;
    if (s & HPE_PROP_HEIGHT)
        dst->height = src->height;
    for (i = 0; i < 4; i++) {
        if (s & (HPE_PROP_MARGIN_TOP << i))
            dst->margin[i] = src->margin[i];
        if (s & (HPE_PROP_PADDING_TOP << i))
            dst->padding[i] = src->padding[i];
        if (s & (HPE_PROP_BORDER_TOP << i))
            dst->border_w[i] = src->border_w[i];
    }
    if (s & HPE_PROP_BORDER_WIDTH) {
        for (i = 0; i < 4; i++)
            dst->border_w[i] = src->border_w[i];
    }
    if (s & HPE_PROP_BORDER_RADIUS)
        dst->border_radius = src->border_radius;
    if (s & HPE_PROP_BORDER_COLOR)
        dst->border_color = src->border_color;
    if (s & HPE_PROP_FLEX_DIR)
        dst->flex_dir = src->flex_dir;
    if (s & HPE_PROP_ALIGN_ITEMS)
        dst->align_items = src->align_items;
    if (s & HPE_PROP_ALIGN_SELF)
        dst->align_self = src->align_self;
    if (s & HPE_PROP_JUSTIFY)
        dst->justify = src->justify;
    if (s & HPE_PROP_GAP)
        dst->gap = src->gap;
    if (s & HPE_PROP_FLEX_GROW)
        dst->flex_grow = src->flex_grow;
    if (s & HPE_PROP_FONT_SIZE)
        dst->font_size = src->font_size;
    if (s & HPE_PROP_FONT_WEIGHT)
        dst->font_weight = src->font_weight;
    if (s & HPE_PROP_TEXT_ALIGN)
        dst->text_align = src->text_align;
    if (s & HPE_PROP_LINE_HEIGHT)
        dst->line_height = src->line_height;
    if (s & HPE_PROP_WHITE_SPACE)
        dst->white_space = src->white_space;
    if (s & HPE_PROP_FONT_FAMILY)
        dst->family = src->family;
    if (s & HPE_PROP_POSITION)
        dst->position = src->position;
    for (i = 0; i < 4; i++) {
        if (s & (HPE_PROP_INSET_TOP << i))
            dst->inset[i] = src->inset[i];
    }
    if (s & HPE_PROP_Z_INDEX) {
        dst->z_index = src->z_index;
        dst->z_auto = src->z_auto;
    }
    if (s & HPE_PROP_FLOAT)
        dst->float_side = src->float_side;
    if (s & HPE_PROP_CLEAR)
        dst->clear_side = src->clear_side;

    /* ---- second word (v0.9) ---- */
    {
        uint64_t s2 = s2_all;
        if (s2 & HPE_PROP2_MIN_HEIGHT)
            dst->min_height = src->min_height;
        if (s2 & HPE_PROP2_MAX_WIDTH)
            dst->max_width = src->max_width;
        if (s2 & HPE_PROP2_MAX_HEIGHT)
            dst->max_height = src->max_height;
        if (s2 & HPE_PROP2_BOX_SIZING)
            dst->box_sizing = src->box_sizing;
        if (s2 & HPE_PROP2_OVERFLOW_X)
            dst->overflow_x = src->overflow_x;
        if (s2 & HPE_PROP2_OVERFLOW_Y)
            dst->overflow_y = src->overflow_y;
        if (s2 & HPE_PROP2_OPACITY)
            dst->opacity = src->opacity;
        if (s2 & HPE_PROP2_VISIBILITY)
            dst->visibility = src->visibility;
        if (s2 & HPE_PROP2_CURSOR)
            dst->cursor = src->cursor;
        if (s2 & HPE_PROP2_TEXT_DECORATION)
            dst->text_decoration = src->text_decoration;
        if (s2 & HPE_PROP2_LETTER_SPACING)
            dst->letter_spacing = src->letter_spacing;
        if (s2 & HPE_PROP2_WORD_SPACING)
            dst->word_spacing = src->word_spacing;
        if (s2 & HPE_PROP2_TEXT_INDENT)
            dst->text_indent = src->text_indent;
        if (s2 & HPE_PROP2_VERTICAL_ALIGN)
            dst->vertical_align = src->vertical_align;
        if (s2 & HPE_PROP2_LIST_STYLE_TYPE)
            dst->list_style_type = src->list_style_type;
        if (s2 & HPE_PROP2_LIST_STYLE_POSITION)
            dst->list_style_position = src->list_style_position;
        if (s2 & HPE_PROP2_BACKGROUND_IMAGE)
            dst->background_image = src->background_image;
        if (s2 & HPE_PROP2_BOX_SHADOW)
            dst->box_shadow = src->box_shadow;
        if (s2 & HPE_PROP2_TEXT_SHADOW)
            dst->text_shadow = src->text_shadow;
        if (s2 & HPE_PROP2_TRANSFORM)
            dst->transform = src->transform;
        if (s2 & HPE_PROP2_TRANSITION)
            dst->has_transition = src->has_transition;
        if (s2 & HPE_PROP2_ANIMATION)
            dst->has_animation = src->has_animation;
        /* per-side border colours default to the shared shorthand when only that
         * one was declared, so a `border-color` still paints all four sides. */
        {
            int any_side = (s2 & (HPE_PROP2_BORDER_TOP_COLOR | HPE_PROP2_BORDER_RIGHT_COLOR |
                                  HPE_PROP2_BORDER_BOTTOM_COLOR | HPE_PROP2_BORDER_LEFT_COLOR)) != 0;
            for (i = 0; i < 4; i++) {
                uint64_t bit = HPE_PROP2_BORDER_TOP_COLOR << i;
                if (s2 & bit)
                    dst->border_colors[i] = src->border_colors[i];
                else if (s & HPE_PROP_BORDER_COLOR)
                    dst->border_colors[i] = src->border_color;
            }
            if (!any_side && (s & HPE_PROP_BORDER_COLOR)) {
                for (i = 0; i < 4; i++)
                    dst->border_colors[i] = src->border_color;
            }
        }
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

void hpe_css_compute(HpeStylesheet *sheet, const HpeDomNode *node, const HpeStyle *parent,
                     HpeStyle *out)
{
    HpeMatch *matches = NULL;
    size_t match_count = 0, match_cap = 0;
    HpeImpMatch *imps = NULL;
    size_t imp_count = 0, imp_cap = 0;
    HpeStyle style;
    int i, j;

    style = hpe_style_default();
    if (parent) {
        style.color = parent->color;
        style.font_size = parent->font_size;
        style.font_weight = parent->font_weight;
        style.text_align = parent->text_align;
        style.line_height = parent->line_height;
        style.white_space = parent->white_space;
        style.family = parent->family;
        style.visibility = parent->visibility;
        style.cursor = parent->cursor;
        style.list_style_type = parent->list_style_type;
        style.list_style_position = parent->list_style_position;
        style.letter_spacing = parent->letter_spacing;
        style.word_spacing = parent->word_spacing;
        style.text_indent = parent->text_indent;
    }

    if (sheet && node) {
        for (i = 0; i < sheet->rule_count; i++) {
            HpeRule *rule = &sheet->rules[i];
            int best = -1;
            int k, mq_ok = 1;
            /* an @media rule contributes nothing unless every enclosing query
             * matches the viewport at cascade time */
            for (k = 0; k < rule->mq_depth && mq_ok; k++)
                mq_ok = media_alt_matches(&rule->mq[k], sheet->vw, sheet->vh);
            if (!mq_ok)
                continue;
            for (j = 0; j < rule->selector_count; j++) {
                if (selector_matches(&rule->selectors[j], node)) {
                    if (rule->selectors[j].specificity > best)
                        best = rule->selectors[j].specificity;
                }
            }
            if (best >= 0) {
                matches = (HpeMatch *)hpe_grow(matches, &match_cap, match_count + 1,
                                               sizeof(HpeMatch));
                matches[match_count].specificity = best;
                matches[match_count].order = rule->order;
                matches[match_count].decl = rule->decls;
                match_count++;
            }
        }
    }

    /* stable insertion sort by (specificity, source order) */
    for (i = 1; i < (int)match_count; i++) {
        HpeMatch key = matches[i];
        j = i - 1;
        while (j >= 0 && (matches[j].specificity > key.specificity ||
                          (matches[j].specificity == key.specificity &&
                           matches[j].order > key.order))) {
            matches[j + 1] = matches[j];
            j--;
        }
        matches[j + 1] = key;
    }
    for (i = 0; i < (int)match_count; i++)
        apply_decl(&style, matches[i].decl);

    /* inline `style=""` declarations: normal ones beat any normal stylesheet
     * declaration; important ones are collected for the second pass. */
    if (node && node->type == HPE_NODE_ELEMENT) {
        const char *inline_style = hpe_dom_attr(node, "style");
        if (inline_style && *inline_style) {
            HpeStyle decl;
            hpe_css_parse_declarations(inline_style, strlen(inline_style), &decl);
            apply_decl(&style, &decl);
        }
    }

    /* second pass: important declarations win over every normal one. Within the
     * important origin an inline `style=""` beats a stylesheet important, then
     * source order decides. */
    for (i = 0; i < (int)match_count; i++) {
        const HpeStyle *d = matches[i].decl;
        if (d->imp || d->imp2) {
            imps = (HpeImpMatch *)hpe_grow(imps, &imp_cap, imp_count + 1, sizeof(HpeImpMatch));
            imps[imp_count].decl = d;
            imps[imp_count].specificity = matches[i].specificity;
            imps[imp_count].order = matches[i].order;
            imps[imp_count].inline_style = 0;
            imp_count++;
        }
    }
    if (node && node->type == HPE_NODE_ELEMENT) {
        const char *inline_style = hpe_dom_attr(node, "style");
        if (inline_style && *inline_style) {
            HpeStyle decl;
            hpe_css_parse_declarations(inline_style, strlen(inline_style), &decl);
            if (decl.imp || decl.imp2) {
                imps = (HpeImpMatch *)hpe_grow(imps, &imp_cap, imp_count + 1,
                                               sizeof(HpeImpMatch));
                imps[imp_count].decl = (const HpeStyle *)inline_style; /* marker */
                imps[imp_count].specificity = (1 << 20); /* higher than any selector */
                imps[imp_count].order = 1 << 30;         /* and later than any rule */
                imps[imp_count].inline_style = 1;
                imp_count++;
            }
        }
    }
    /* stable sort by (specificity, source order) within the important origin */
    for (i = 1; i < (int)imp_count; i++) {
        HpeImpMatch key = imps[i];
        j = i - 1;
        while (j >= 0 && (imps[j].specificity > key.specificity ||
                          (imps[j].specificity == key.specificity &&
                           imps[j].order > key.order))) {
            imps[j + 1] = imps[j];
            j--;
        }
        imps[j + 1] = key;
    }
    for (i = 0; i < (int)imp_count; i++) {
        if (imps[i].inline_style) {
            const char *inline_style = hpe_dom_attr(node, "style");
            HpeStyle decl;
            hpe_css_parse_declarations(inline_style, strlen(inline_style), &decl);
            apply_decl_important(&style, &decl);
        } else {
            apply_decl_important(&style, imps[i].decl);
        }
    }

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
        "head, script, style, title, meta, link, base, noscript { display: none; }\n"
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
