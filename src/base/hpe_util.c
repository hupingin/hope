#include "base/hpe_util.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *hpe_alloc(size_t size)
{
    void *p = malloc(size ? size : 1);
    if (!p) {
        fprintf(stderr, "hope: out of memory (%zu bytes)\n", size);
        abort();
    }
    return p;
}

void *hpe_calloc(size_t count, size_t size)
{
    void *p = calloc(count ? count : 1, size ? size : 1);
    if (!p) {
        fprintf(stderr, "hope: out of memory (%zu x %zu bytes)\n", count, size);
        abort();
    }
    return p;
}

void *hpe_realloc(void *ptr, size_t size)
{
    void *p = realloc(ptr, size ? size : 1);
    if (!p) {
        fprintf(stderr, "hope: out of memory (%zu bytes)\n", size);
        abort();
    }
    return p;
}

char *hpe_strdup(const char *s)
{
    if (!s)
        return NULL;
    return hpe_strndup(s, strlen(s));
}

char *hpe_strndup(const char *s, size_t n)
{
    char *p = (char *)hpe_alloc(n + 1);
    if (n)
        memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

void hpe_free(void *ptr)
{
    free(ptr);
}

void *hpe_grow(void *ptr, size_t *cap, size_t need, size_t elem_size)
{
    size_t newcap;
    if (need <= *cap)
        return ptr;
    newcap = *cap ? *cap : 8;
    while (newcap < need)
        newcap *= 2;
    ptr = hpe_realloc(ptr, newcap * elem_size);
    *cap = newcap;
    return ptr;
}

int hpe_ascii_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

int hpe_str_ieq(const char *a, const char *b)
{
    if (!a || !b)
        return a == b;
    while (*a && *b) {
        if (hpe_ascii_lower((unsigned char)*a) != hpe_ascii_lower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

int hpe_str_ieq_n(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (hpe_ascii_lower((unsigned char)a[i]) != hpe_ascii_lower((unsigned char)b[i]))
            return 0;
        if (!a[i] || !b[i])
            return 0;
    }
    return 1;
}

int hpe_starts_with_i(const char *s, const char *prefix)
{
    return hpe_str_ieq_n(s, prefix, strlen(prefix));
}

int hpe_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

char *hpe_trim(char *s)
{
    char *end;
    if (!s)
        return s;
    while (*s && hpe_is_space(*s))
        s++;
    end = s + strlen(s);
    while (end > s && hpe_is_space(end[-1]))
        end--;
    *end = '\0';
    return s;
}

int hpe_utf8_next(const char *s, uint32_t *cp)
{
    const unsigned char *p = (const unsigned char *)s;
    uint32_t c;
    int extra, i;

    if (p[0] < 0x80) {
        *cp = p[0];
        return 1;
    }
    if ((p[0] & 0xE0) == 0xC0) {
        c = p[0] & 0x1F;
        extra = 1;
    } else if ((p[0] & 0xF0) == 0xE0) {
        c = p[0] & 0x0F;
        extra = 2;
    } else if ((p[0] & 0xF8) == 0xF0) {
        c = p[0] & 0x07;
        extra = 3;
    } else {
        *cp = 0xFFFD;
        return 1;
    }
    for (i = 1; i <= extra; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            *cp = 0xFFFD;
            return 1;
        }
        c = (c << 6) | (uint32_t)(p[i] & 0x3F);
    }
    *cp = c;
    return extra + 1;
}

int hpe_utf8_prev(const char *begin, const char *cur)
{
    int n = 1;
    if (cur <= begin)
        return 0;
    cur--;
    while (cur > begin && ((unsigned char)*cur & 0xC0) == 0x80) {
        cur--;
        n++;
    }
    return n;
}

int hpe_utf8_encode(uint32_t cp, char out[4])
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* ---------- lengths ---------- */

HpeLength hpe_len_auto(void)
{
    HpeLength l;
    l.value = 0.0f;
    l.unit = HPE_LEN_AUTO;
    return l;
}

HpeLength hpe_len_px(float v)
{
    HpeLength l;
    l.value = v;
    l.unit = HPE_LEN_PX;
    return l;
}

HpeLength hpe_len_pct(float v)
{
    HpeLength l;
    l.value = v;
    l.unit = HPE_LEN_PCT;
    return l;
}

HpeLength hpe_len_unit(float v, HpeLenUnit unit)
{
    HpeLength l;
    l.value = v;
    l.unit = unit;
    return l;
}

int hpe_len_is_px(HpeLength len)
{
    return len.unit == HPE_LEN_PX;
}

int hpe_len_is_auto(HpeLength len)
{
    return len.unit == HPE_LEN_AUTO;
}

/* unit suffixes are matched longest-first so "vmin" does not shadow "vh" and
 * "rem" does not shadow "em" (both start with a shared prefix). */
static int len_unit_from_suffix(const char *s, HpeLenUnit *out)
{
    static const struct {
        const char *suffix;
        HpeLenUnit unit;
    } table[] = {
        {"rem", HPE_LEN_REM}, {"vmin", HPE_LEN_VW}, {"vmax", HPE_LEN_VW},
        {"vw", HPE_LEN_VW},   {"vh", HPE_LEN_VH},   {"ch", HPE_LEN_CH},
        {"em", HPE_LEN_EM},   {"ex", HPE_LEN_EM},   {"px", HPE_LEN_PX},
        {"pt", HPE_LEN_PT},   {"pc", HPE_LEN_PT},   {"in", HPE_LEN_PX},
        {"cm", HPE_LEN_PX},   {"mm", HPE_LEN_PX},   {"q", HPE_LEN_PX},
    };
    size_t i;
    for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (hpe_str_ieq(s, table[i].suffix)) {
            *out = table[i].unit;
            return 1;
        }
    }
    return 0;
}

HpeLength hpe_len_parse(const char *s)
{
    char buf[64];
    size_t n;
    char *end = NULL;
    float v;
    HpeLenUnit unit = HPE_LEN_PX;

    if (!s)
        return hpe_len_auto();
    while (*s && hpe_is_space(*s))
        s++;
    n = strlen(s);
    if (n >= sizeof(buf))
        n = sizeof(buf) - 1;
    memcpy(buf, s, n);
    buf[n] = '\0';

    if (strncmp(buf, "auto", 4) == 0)
        return hpe_len_auto();

    v = (float)strtod(buf, &end);
    if (end == buf)
        return hpe_len_auto();
    while (*end && hpe_is_space(*end))
        end++;
    if (*end == '%')
        return hpe_len_pct(v);
    if (*end == '\0')
        return hpe_len_px(v);
    if (len_unit_from_suffix(end, &unit)) {
        if (unit == HPE_LEN_PX) {
            /* absolute physical units -> css px at 96dpi */
            if (hpe_str_ieq(end, "in"))
                return hpe_len_px(v * 96.0f);
            if (hpe_str_ieq(end, "cm"))
                return hpe_len_px(v * 96.0f / 2.54f);
            if (hpe_str_ieq(end, "mm"))
                return hpe_len_px(v * 96.0f / 25.4f);
            if (hpe_str_ieq(end, "q"))
                return hpe_len_px(v * 96.0f / 101.6f);
        }
        if (unit == HPE_LEN_PT)
            return hpe_len_px(hpe_str_ieq(end, "pc") ? v * 16.0f : v * 96.0f / 72.0f);
        return hpe_len_unit(v, unit);
    }
    /* unknown suffix: treat as px, matching the lenient parser elsewhere */
    return hpe_len_px(v);
}

float hpe_len_resolve(HpeLength len, float base)
{
    return hpe_len_resolve_full(len, base, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
}

float hpe_len_resolve_full(HpeLength len, float base, float font_size, float root_font_size,
                           float vw, float vh, float ch_width)
{
    switch (len.unit) {
    case HPE_LEN_PX:
        return len.value;
    case HPE_LEN_PCT:
        return base * len.value / 100.0f;
    case HPE_LEN_EM:
        return len.value * (font_size > 0.0f ? font_size : 16.0f);
    case HPE_LEN_REM:
        return len.value * (root_font_size > 0.0f ? root_font_size : 16.0f);
    case HPE_LEN_VW:
        return vw > 0.0f ? len.value * vw / 100.0f : len.value * 16.0f;
    case HPE_LEN_VH:
        return vh > 0.0f ? len.value * vh / 100.0f : len.value * 16.0f;
    case HPE_LEN_CH:
        /* fall back to half the font size when no glyph advance is supplied */
        return len.value * (ch_width > 0.0f ? ch_width : (font_size > 0.0f ? font_size * 0.5f
                                                                           : 8.0f));
    case HPE_LEN_AUTO:
    default:
        return -1.0f;
    }
}

/* ---------- colors ---------- */

typedef struct {
    const char *name;
    uint32_t rgb;
    uint8_t a;
} HpeNamedColor;

/* the full CSS Color 4 named-color table (148 entries incl. both grey spellings) */
static const HpeNamedColor g_named_colors[] = {
    {"transparent", 0x000000, 0},
    {"aliceblue", 0xF0F8FF, 255}, {"antiquewhite", 0xFAEBD7, 255},
    {"aqua", 0x00FFFF, 255}, {"aquamarine", 0x7FFFD4, 255}, {"azure", 0xF0FFFF, 255},
    {"beige", 0xF5F5DC, 255}, {"bisque", 0xFFE4C4, 255}, {"black", 0x000000, 255},
    {"blanchedalmond", 0xFFEBCD, 255}, {"blue", 0x0000FF, 255},
    {"blueviolet", 0x8A2BE2, 255}, {"brown", 0xA52A2A, 255},
    {"burlywood", 0xDEB887, 255}, {"cadetblue", 0x5F9EA0, 255},
    {"chartreuse", 0x7FFF00, 255}, {"chocolate", 0xD2691E, 255},
    {"coral", 0xFF7F50, 255}, {"cornflowerblue", 0x6495ED, 255},
    {"cornsilk", 0xFFF8DC, 255}, {"crimson", 0xDC143C, 255}, {"cyan", 0x00FFFF, 255},
    {"darkblue", 0x00008B, 255}, {"darkcyan", 0x008B8B, 255},
    {"darkgoldenrod", 0xB8860B, 255}, {"darkgray", 0xA9A9A9, 255},
    {"darkgreen", 0x006400, 255}, {"darkgrey", 0xA9A9A9, 255},
    {"darkkhaki", 0xBDB76B, 255}, {"darkmagenta", 0x8B008B, 255},
    {"darkolivegreen", 0x556B2F, 255}, {"darkorange", 0xFF8C00, 255},
    {"darkorchid", 0x9932CC, 255}, {"darkred", 0x8B0000, 255},
    {"darksalmon", 0xE9967A, 255}, {"darkseagreen", 0x8FBC8F, 255},
    {"darkslateblue", 0x483D8B, 255}, {"darkslategray", 0x2F4F4F, 255},
    {"darkslategrey", 0x2F4F4F, 255}, {"darkturquoise", 0x00CED1, 255},
    {"darkviolet", 0x9400D3, 255}, {"deeppink", 0xFF1493, 255},
    {"deepskyblue", 0x00BFFF, 255}, {"dimgray", 0x696969, 255},
    {"dimgrey", 0x696969, 255}, {"dodgerblue", 0x1E90FF, 255},
    {"firebrick", 0xB22222, 255}, {"floralwhite", 0xFFFAF0, 255},
    {"forestgreen", 0x228B22, 255}, {"fuchsia", 0xFF00FF, 255},
    {"gainsboro", 0xDCDCDC, 255}, {"ghostwhite", 0xF8F8FF, 255},
    {"gold", 0xFFD700, 255}, {"goldenrod", 0xDAA520, 255}, {"gray", 0x808080, 255},
    {"green", 0x008000, 255}, {"greenyellow", 0xADFF2F, 255},
    {"grey", 0x808080, 255}, {"honeydew", 0xF0FFF0, 255}, {"hotpink", 0xFF69B4, 255},
    {"indianred", 0xCD5C5C, 255}, {"indigo", 0x4B0082, 255},
    {"ivory", 0xFFFFF0, 255}, {"khaki", 0xF0E68C, 255}, {"lavender", 0xE6E6FA, 255},
    {"lavenderblush", 0xFFF0F5, 255}, {"lawngreen", 0x7CFC00, 255},
    {"lemonchiffon", 0xFFFACD, 255}, {"lightblue", 0xADD8E6, 255},
    {"lightcoral", 0xF08080, 255}, {"lightcyan", 0xE0FFFF, 255},
    {"lightgoldenrodyellow", 0xFAFAD2, 255}, {"lightgray", 0xD3D3D3, 255},
    {"lightgreen", 0x90EE90, 255}, {"lightgrey", 0xD3D3D3, 255},
    {"lightpink", 0xFFB6C1, 255}, {"lightsalmon", 0xFFA07A, 255},
    {"lightseagreen", 0x20B2AA, 255}, {"lightskyblue", 0x87CEFA, 255},
    {"lightslategray", 0x778899, 255}, {"lightslategrey", 0x778899, 255},
    {"lightsteelblue", 0xB0C4DE, 255}, {"lightyellow", 0xFFFFE0, 255},
    {"lime", 0x00FF00, 255}, {"limegreen", 0x32CD32, 255}, {"linen", 0xFAF0E6, 255},
    {"magenta", 0xFF00FF, 255}, {"maroon", 0x800000, 255},
    {"mediumaquamarine", 0x66CDAA, 255}, {"mediumblue", 0x0000CD, 255},
    {"mediumorchid", 0xBA55D3, 255}, {"mediumpurple", 0x9370DB, 255},
    {"mediumseagreen", 0x3CB371, 255}, {"mediumslateblue", 0x7B68EE, 255},
    {"mediumspringgreen", 0x00FA9A, 255}, {"mediumturquoise", 0x48D1CC, 255},
    {"mediumvioletred", 0xC71585, 255}, {"midnightblue", 0x191970, 255},
    {"mintcream", 0xF5FFFA, 255}, {"mistyrose", 0xFFE4E1, 255},
    {"moccasin", 0xFFE4B5, 255}, {"navajowhite", 0xFFDEAD, 255},
    {"navy", 0x000080, 255}, {"oldlace", 0xFDF5E6, 255}, {"olive", 0x808000, 255},
    {"olivedrab", 0x6B8E23, 255}, {"orange", 0xFFA500, 255},
    {"orangered", 0xFF4500, 255}, {"orchid", 0xDA70D6, 255},
    {"palegoldenrod", 0xEEE8AA, 255}, {"palegreen", 0x98FB98, 255},
    {"paleturquoise", 0xAFEEEE, 255}, {"palevioletred", 0xDB7093, 255},
    {"papayawhip", 0xFFEFD5, 255}, {"peachpuff", 0xFFDAB9, 255},
    {"peru", 0xCD853F, 255}, {"pink", 0xFFC0CB, 255}, {"plum", 0xDDA0DD, 255},
    {"powderblue", 0xB0E0E6, 255}, {"purple", 0x800080, 255},
    {"rebeccapurple", 0x663399, 255}, {"red", 0xFF0000, 255},
    {"rosybrown", 0xBC8F8F, 255}, {"royalblue", 0x4169E1, 255},
    {"saddlebrown", 0x8B4513, 255}, {"salmon", 0xFA8072, 255},
    {"sandybrown", 0xF4A460, 255}, {"seagreen", 0x2E8B57, 255},
    {"seashell", 0xFFF5EE, 255}, {"sienna", 0xA0522D, 255},
    {"silver", 0xC0C0C0, 255}, {"skyblue", 0x87CEEB, 255},
    {"slateblue", 0x6A5ACD, 255}, {"slategray", 0x708090, 255},
    {"slategrey", 0x708090, 255}, {"snow", 0xFFFAFA, 255},
    {"springgreen", 0x00FF7F, 255}, {"steelblue", 0x4682B4, 255},
    {"tan", 0xD2B48C, 255}, {"teal", 0x008080, 255}, {"thistle", 0xD8BFD8, 255},
    {"tomato", 0xFF6347, 255}, {"turquoise", 0x40E0D0, 255},
    {"violet", 0xEE82EE, 255}, {"wheat", 0xF5DEB3, 255}, {"white", 0xFFFFFF, 255},
    {"whitesmoke", 0xF5F5F5, 255}, {"yellow", 0xFFFF00, 255},
    {"yellowgreen", 0x9ACD32, 255},
};

static int hex_val(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    c = (char)hpe_ascii_lower((unsigned char)c);
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

/* ---------- charset ---------- */

char *hpe_latin1_to_utf8(const char *src, size_t len, size_t *out_len)
{
    char *out = (char *)hpe_alloc(len * 2 + 1);
    size_t i, o = 0;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)src[i];
        uint32_t cp;
        if (c < 0x80) {
            out[o++] = (char)c;
            continue;
        }
        cp = c;
        /* windows-1252 remaps 0x80..0x9F away from the C1 control range */
        if (c >= 0x80 && c <= 0x9F) {
            static const uint16_t cp1252[32] = {
                0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
                0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};
            cp = cp1252[c - 0x80];
        }
        {
            char tmp[4];
            int n = hpe_utf8_encode(cp, tmp);
            memcpy(out + o, tmp, (size_t)n);
            o += (size_t)n;
        }
    }
    out[o] = '\0';
    if (out_len)
        *out_len = o;
    return out;
}

/* `#rgb` / `#rgba` (3/4 digits, each doubled) */
static int color_parse_hex_short(const char *s, size_t len, HpeColor *out)
{
    int v[4] = {0, 0, 0, 15};
    int digits = (int)len - 1;
    size_t i;

    for (i = 0; i < (size_t)digits; i++) {
        int h = hex_val(s[1 + i]);
        if (h < 0)
            return 0;
        v[i] = h;
    }
    out->r = (uint8_t)(v[0] * 17);
    out->g = (uint8_t)(v[1] * 17);
    out->b = (uint8_t)(v[2] * 17);
    out->a = (uint8_t)((digits == 4 ? v[3] : 15) * 17);
    return 1;
}

/* `#rrggbb` / `#rrggbbaa` (6/8 digits) */
static int color_parse_hex_long(const char *s, size_t len, HpeColor *out)
{
    uint8_t v[4] = {0, 0, 0, 255};
    int digits = (int)len - 1;
    size_t i;

    for (i = 0; i < (size_t)digits; i += 2) {
        int hi = hex_val(s[1 + i]);
        int lo = hex_val(s[2 + i]);
        if (hi < 0 || lo < 0)
            return 0;
        v[i / 2] = (uint8_t)(hi * 16 + lo);
    }
    out->r = v[0];
    out->g = v[1];
    out->b = v[2];
    out->a = (uint8_t)(digits == 8 ? v[3] : 255);
    return 1;
}

/* skips the separators (` `, `,`, `/`) that divide functional-notation parts */
static void color_skip_seps(const char **pp)
{
    while (**pp == ' ' || **pp == ',' || **pp == '/')
        (*pp)++;
}

/* reads up to four numeric components of a `rgb()` / `hsl()` argument list;
 * `pct` receives 1 for each component written with a trailing '%'. Returns the
 * number of components read. */
static int color_read_components(const char *p, float *comp, int *pct)
{
    int idx = 0;

    if (!p)
        return 0;
    p++; /* ( */
    while (*p && *p != ')' && idx < 4) {
        char *end = NULL;
        float v;

        color_skip_seps(&p);
        if (!*p || *p == ')')
            break;
        v = (float)strtod(p, &end);
        if (end == p)
            break;
        if (*end == '%') {
            pct[idx] = 1;
            if (idx == 3)
                v /= 100.0f;
            else
                v = v * 255.0f / 100.0f;
            end++;
        }
        comp[idx++] = v;
        p = end;
    }
    return idx;
}

static float clamp01(float v)
{
    if (v < 0.0f)
        return 0.0f;
    if (v > 1.0f)
        return 1.0f;
    return v;
}

static uint8_t clamp_byte(float v)
{
    if (v < 0.0f)
        return 0;
    if (v > 255.0f)
        return 255;
    return (uint8_t)v;
}

/* `rgb(r, g, b[, a])` and the `%`-based form */
static int color_parse_rgb(const char *s, HpeColor *out)
{
    const char *p = strchr(s, '(');
    float comp[4] = {0, 0, 0, 1.0f};
    int pct[4] = {0, 0, 0, 0};
    int idx = color_read_components(p, comp, pct);

    if (!p || idx < 3)
        return 0;
    out->r = clamp_byte(comp[0]);
    out->g = clamp_byte(comp[1]);
    out->b = clamp_byte(comp[2]);
    if (idx >= 4)
        out->a = (uint8_t)(comp[3] <= 1.0f ? comp[3] * 255.0f : comp[3]);
    else
        out->a = 255;
    HPE_UNUSED(pct);
    return 1;
}

/* maps the hue sextant to the rgb chroma triple used by hsl→rgb */
static void hsl_sextant_rgb(int sextant, float c, float x, float *r, float *g, float *b)
{
    switch (sextant) {
    case 0: *r = c; *g = x; *b = 0.0f; break;
    case 1: *r = x; *g = c; *b = 0.0f; break;
    case 2: *r = 0.0f; *g = c; *b = x; break;
    case 3: *r = 0.0f; *g = x; *b = c; break;
    case 4: *r = x; *g = 0.0f; *b = c; break;
    default: *r = c; *g = 0.0f; *b = x; break;
    }
}

/* reads up to four components of a `hsl()` argument list, applying /100 to any
 * component written with a trailing '%'; returns the count read */
static int color_read_hsl_components(const char *p, float *comp)
{
    int idx = 0;

    if (!p)
        return 0;
    p++; /* ( */
    while (*p && *p != ')' && idx < 4) {
        char *end = NULL;
        float v;

        color_skip_seps(&p);
        if (!*p || *p == ')')
            break;
        v = (float)strtod(p, &end);
        if (end == p)
            break;
        if (*end == '%') {
            v /= 100.0f;
            end++;
        }
        comp[idx++] = v;
        p = end;
    }
    return idx;
}

/* `hsl(h, s%, l%[, a])` */
static int color_parse_hsl(const char *s, HpeColor *out)
{
    const char *p = strchr(s, '(');
    float comp[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    int idx = color_read_hsl_components(p, comp);
    float hv, sv, lv, av;
    float r, g, b, c, x, m, hh;
    int sextant;

    if (!p || idx < 3)
        return 0;
    hv = comp[0];
    sv = clamp01(comp[1]);
    lv = clamp01(comp[2]);
    av = comp[3];
    hv = fmodf(hv, 360.0f);
    if (hv < 0.0f)
        hv += 360.0f;
    hh = hv / 60.0f;
    c = (1.0f - fabsf(2.0f * lv - 1.0f)) * sv;
    x = c * (1.0f - fabsf(fmodf(hh, 2.0f) - 1.0f));
    sextant = (int)hh % 6;
    hsl_sextant_rgb(sextant, c, x, &r, &g, &b);
    m = lv - c * 0.5f;
    out->r = (uint8_t)((r + m) * 255.0f + 0.5f);
    out->g = (uint8_t)((g + m) * 255.0f + 0.5f);
    out->b = (uint8_t)((b + m) * 255.0f + 0.5f);
    out->a = idx >= 4 ? (uint8_t)(av <= 1.0f ? av * 255.0f : av) : 255;
    return 1;
}

/* case-insensitive lookup of a CSS named colour */
static int color_parse_named(const char *s, size_t len, HpeColor *out)
{
    size_t i;

    for (i = 0; i < sizeof(g_named_colors) / sizeof(g_named_colors[0]); i++) {
        if (strlen(g_named_colors[i].name) == len &&
            hpe_str_ieq_n(s, g_named_colors[i].name, len)) {
            uint32_t rgb = g_named_colors[i].rgb;
            out->r = (uint8_t)((rgb >> 16) & 0xFF);
            out->g = (uint8_t)((rgb >> 8) & 0xFF);
            out->b = (uint8_t)(rgb & 0xFF);
            out->a = g_named_colors[i].a;
            return 1;
        }
    }
    return 0;
}

int hpe_color_parse(const char *s, HpeColor *out)
{
    size_t len;

    if (!s || !out)
        return 0;
    while (*s && hpe_is_space(*s))
        s++;
    len = strlen(s);
    while (len > 0 && hpe_is_space(s[len - 1]))
        len--;
    if (len == 0)
        return 0;

    if (s[0] == '#') {
        if (len == 4 || len == 5)
            return color_parse_hex_short(s, len, out);
        if (len == 7 || len == 9)
            return color_parse_hex_long(s, len, out);
        return 0;
    }
    if (hpe_str_ieq_n(s, "rgb", 3))
        return color_parse_rgb(s, out);
    if (hpe_str_ieq_n(s, "hsl", 3))
        return color_parse_hsl(s, out);
    return color_parse_named(s, len, out);
}
