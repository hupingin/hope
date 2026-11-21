#include "render/hpe_font.h"

#include <stdlib.h>
#include <string.h>

#include "base/hpe_util.h"

/* HPE_NO_MASK_CACHE=1 bypasses the run-mask cache (debug/benchmark aid) */
static int mask_cache_disabled(void)
{
    static int flag = -1;
    if (flag < 0)
        flag = getenv("HPE_NO_MASK_CACHE") != NULL;
    return flag;
}

#define HPE_FONT_CACHE_MAX 48
#define HPE_ADV_CACHE_SIZE 1024
#define HPE_MASK_CACHE_SIZE 1024 /* power of two; open-addressed, full flush on pressure */

typedef struct {
    uint32_t codepoint;
    float advance;
} HpeAdvEntry;

/* Per-font glyph-run coverage mask cache. Key is a 64-bit FNV-1a hash of the
 * utf-8 bytes plus the length; the produced HpeTextMask is integer-position
 * independent (painting rounds x/y), so text content alone identifies it. */
typedef struct {
    uint64_t key; /* 0 = empty slot */
    int len;
    HpeTextMask mask; /* coverage buffer owned by the cache */
} HpeMaskEntry;

struct HpeFont {
    HpePlatFont *plat;
    float size;
    int weight;
    HpeFontFamily family;
    HpeAdvEntry *adv;
    size_t adv_size;
    HpeMaskEntry *masks;
    int masks_used;
};

static HpeFont *g_fonts[HPE_FONT_CACHE_MAX];
static int g_font_count = 0;

static HpeAdvEntry *adv_slot(HpeFont *font, uint32_t cp, int create)
{
    size_t mask, idx, i;
    if (!font->adv) {
        if (!create)
            return NULL;
        font->adv_size = HPE_ADV_CACHE_SIZE;
        font->adv = (HpeAdvEntry *)hpe_alloc(sizeof(HpeAdvEntry) * font->adv_size);
        for (i = 0; i < font->adv_size; i++) {
            font->adv[i].codepoint = 0xFFFFFFFFu;
            font->adv[i].advance = 0.0f;
        }
    }
    mask = font->adv_size - 1;
    idx = (cp * 2654435761u) & mask;
    for (i = 0; i < font->adv_size; i++) {
        HpeAdvEntry *e = &font->adv[(idx + i) & mask];
        if (e->codepoint == cp)
            return e;
        if (e->codepoint == 0xFFFFFFFFu)
            return create ? e : NULL;
    }
    return NULL;
}

HpeFont *hpe_font_get(float pixel_size, int weight, HpeFontFamily family)
{
    int i;
    int px = (int)(pixel_size + 0.5f);
    HpeFont *font;

    if (px < 6)
        px = 6;
    if (px > 200)
        px = 200;
    if (weight != 700)
        weight = 400;
    if (family != HPE_FONT_SERIF && family != HPE_FONT_MONO)
        family = HPE_FONT_SANS;

    for (i = 0; i < g_font_count; i++) {
        if ((int)(g_fonts[i]->size + 0.5f) == px && g_fonts[i]->weight == weight &&
            g_fonts[i]->family == family)
            return g_fonts[i];
    }

    if (g_font_count >= HPE_FONT_CACHE_MAX) {
        g_font_count = 0; /* simple flush: page styles rarely exceed this */
    }

    font = (HpeFont *)hpe_calloc(1, sizeof(HpeFont));
    font->plat = hpe_plat_font_create((float)px, weight, family);
    font->size = (float)px;
    font->weight = weight;
    font->family = family;
    if (!font->plat) {
        hpe_free(font);
        return NULL;
    }
    g_fonts[g_font_count++] = font;
    return font;
}

float hpe_font_size(HpeFont *font)
{
    return font ? font->size : 0.0f;
}

int hpe_font_weight(HpeFont *font)
{
    return font ? font->weight : 400;
}

float hpe_font_ascent(HpeFont *font)
{
    return font ? hpe_plat_font_ascent(font->plat) : 0.0f;
}

float hpe_font_descent(HpeFont *font)
{
    return font ? hpe_plat_font_descent(font->plat) : 0.0f;
}

float hpe_font_line_height(HpeFont *font)
{
    return font ? hpe_plat_font_line_height(font->plat) : 0.0f;
}

float hpe_font_advance(HpeFont *font, uint32_t codepoint)
{
    HpeAdvEntry *e;
    if (!font)
        return 0.0f;
    e = adv_slot(font, codepoint, 1);
    if (!e)
        return hpe_plat_font_advance(font->plat, codepoint);
    if (e->codepoint == 0xFFFFFFFFu) {
        e->codepoint = codepoint;
        e->advance = hpe_plat_font_advance(font->plat, codepoint);
    }
    return e->advance;
}

float hpe_font_measure(HpeFont *font, const char *text, int len)
{
    const char *p, *end;
    float w = 0.0f;
    if (!font || !text || len <= 0)
        return 0.0f;
    p = text;
    end = text + len;
    while (p < end) {
        uint32_t cp;
        int n = hpe_utf8_next(p, &cp);
        if (n <= 0)
            break;
        p += n;
        if (cp == '\n' || cp == '\r')
            continue;
        w += hpe_font_advance(font, cp);
    }
    return w;
}

static uint64_t mask_hash(const char *text, int len)
{
    uint64_t h = 1469598103934665603ULL;
    int i;
    for (i = 0; i < len; i++) {
        h ^= (unsigned char)text[i];
        h *= 1099511628211ULL;
    }
    if (h == 0)
        h = 1; /* 0 marks empty slots */
    return h;
}

static void mask_slot(HpeFont *font, uint64_t key, HpeMaskEntry **out_empty, HpeMaskEntry **out_hit)
{
    size_t mask = HPE_MASK_CACHE_SIZE - 1;
    size_t idx = (size_t)((key ^ (key >> 32)) * 2654435761u) & mask;
    size_t i;
    *out_empty = NULL;
    *out_hit = NULL;
    for (i = 0; i < HPE_MASK_CACHE_SIZE; i++) {
        HpeMaskEntry *e = &font->masks[(idx + i) & mask];
        if (e->key == 0) {
            *out_empty = e;
            return;
        }
        if (e->key == key) {
            *out_hit = e;
            return;
        }
    }
}

static void mask_flush(HpeFont *font)
{
    int i;
    if (!font->masks)
        return;
    for (i = 0; i < HPE_MASK_CACHE_SIZE; i++) {
        if (font->masks[i].key) {
            hpe_text_mask_free(&font->masks[i].mask);
            font->masks[i].key = 0;
        }
    }
    font->masks_used = 0;
}

const HpeTextMask *hpe_font_mask(HpeFont *font, const char *text, int len)
{
    HpeMaskEntry *empty, *hit;
    uint64_t key;
    static HpeTextMask uncached;

    if (!font || !font->plat || !text || len <= 0)
        return NULL;
    if (mask_cache_disabled())
        return hpe_plat_font_mask(font->plat, text, len, &uncached) ? &uncached : NULL;
    if (!font->masks)
        font->masks = (HpeMaskEntry *)hpe_calloc(HPE_MASK_CACHE_SIZE, sizeof(HpeMaskEntry));

    key = mask_hash(text, len);
    mask_slot(font, key, &empty, &hit);
    if (hit)
        return &hit->mask;
    if (!empty)
        return NULL; /* table full without a hole: treat as miss, no store */

    /* Keep load factor <= 3/4: a full flush beats unbounded clustering.
     * Steady-state pages far exceed this budget with few distinct runs. */
    if (font->masks_used * 4 >= HPE_MASK_CACHE_SIZE * 3) {
        mask_flush(font);
        mask_slot(font, key, &empty, &hit);
        if (hit)
            return &hit->mask;
        if (!empty)
            return NULL;
    }

    if (!hpe_plat_font_mask(font->plat, text, len, &empty->mask))
        return NULL;
    empty->key = key;
    empty->len = len;
    font->masks_used++;
    return &empty->mask;
}

void hpe_font_shutdown(void)
{
    int i;
    for (i = 0; i < g_font_count; i++) {
        HpeFont *f = g_fonts[i];
        if (!f)
            continue;
        mask_flush(f);
        if (f->masks)
            hpe_free(f->masks);
        if (f->plat)
            hpe_plat_font_destroy(f->plat);
        if (f->adv)
            hpe_free(f->adv);
        hpe_free(f);
        g_fonts[i] = NULL;
    }
    g_font_count = 0;
}
