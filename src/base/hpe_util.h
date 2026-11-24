/* Hope UI Engine - memory, string, utf8 and css value helpers */
#ifndef HPE_UTIL_H
#define HPE_UTIL_H

#include "base/hpe_types.h"

/* ---------- memory ---------- */
void *hpe_alloc(size_t size);
void *hpe_calloc(size_t count, size_t size);
void *hpe_realloc(void *ptr, size_t size);
char *hpe_strdup(const char *s);
char *hpe_strndup(const char *s, size_t n);
void hpe_free(void *ptr);
/* grow an array (ptr may be NULL), returns new pointer, aborts on OOM */
void *hpe_grow(void *ptr, size_t *cap, size_t need, size_t elem_size);

/* ---------- string ---------- */
int hpe_ascii_lower(int c);
int hpe_str_ieq(const char *a, const char *b);
int hpe_str_ieq_n(const char *a, const char *b, size_t n);
char *hpe_trim(char *s);                    /* trims in place, returns s */
int hpe_is_space(char c);
int hpe_starts_with_i(const char *s, const char *prefix);

/* ---------- utf-8 ---------- */
/* decodes one codepoint, returns bytes consumed (>=1), cp set to 0xFFFD on error */
int hpe_utf8_next(const char *s, uint32_t *cp);
int hpe_utf8_prev(const char *begin, const char *cur);
int hpe_utf8_encode(uint32_t cp, char out[4]);

/* ---------- css values ---------- */
typedef enum HpeLenUnit {
    HPE_LEN_AUTO = 0,
    HPE_LEN_PX,
    HPE_LEN_PCT,
    HPE_LEN_EM,  /* relative to the element's own font-size */
    HPE_LEN_REM, /* relative to the root font-size */
    HPE_LEN_VW,
    HPE_LEN_VH,
    HPE_LEN_CH,  /* advance width of the "0" glyph */
    HPE_LEN_PT   /* points, resolved to px at parse time as px via `pt_to_px` */
} HpeLenUnit;

typedef struct HpeLength {
    float value;
    HpeLenUnit unit;
} HpeLength;

HpeLength hpe_len_auto(void);
HpeLength hpe_len_px(float v);
HpeLength hpe_len_pct(float v);
HpeLength hpe_len_unit(float v, HpeLenUnit unit);
HpeLength hpe_len_parse(const char *s);
/* resolves against a base length (containing block width / font size).
 * font_size / root_font_size / viewport / ch_width feed the relative units;
 * callers that do not care may pass 0 for all of them. */
float hpe_len_resolve(HpeLength len, float base);
float hpe_len_resolve_full(HpeLength len, float base, float font_size, float root_font_size,
                           float vw, float vh, float ch_width);

/* true when the length is an absolute px value (no context needed to resolve) */
int hpe_len_is_px(HpeLength len);
/* true when the length carries no value that can drive layout (auto) */
int hpe_len_is_auto(HpeLength len);

/* ---------- charset ---------- */
/* Widens a legacy single-byte buffer (iso-8859-1 / windows-1252) into utf-8.
 * Bytes 0x80..0x9F are mapped through the cp1252 table (curly quotes, dashes,
 * ellipsis, euro ...), everything else is treated as latin-1. The caller owns
 * the result (hpe_free it). */
char *hpe_latin1_to_utf8(const char *src, size_t len, size_t *out_len);

int hpe_color_parse(const char *s, HpeColor *out);

#endif /* HPE_UTIL_H */
