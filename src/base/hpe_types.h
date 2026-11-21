/* Hope UI Engine - basic types */
#ifndef HPE_TYPES_H
#define HPE_TYPES_H

#include <stddef.h>
#include <stdint.h>

typedef struct HpeRect {
    float x, y, w, h;
} HpeRect;

typedef struct HpeColor {
    uint8_t r, g, b, a;
} HpeColor;

static inline HpeColor hpe_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    HpeColor c;
    c.r = r;
    c.g = g;
    c.b = b;
    c.a = a;
    return c;
}

static inline HpeColor hpe_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return hpe_rgba(r, g, b, 255);
}

/* generic font categories resolved from CSS font-family; the platform layer
 * maps each to a concrete typeface on the current OS. */
typedef enum HpeFontFamily {
    HPE_FONT_DEFAULT = 0, /* engine default (same as sans) */
    HPE_FONT_SERIF,
    HPE_FONT_SANS,
    HPE_FONT_MONO
} HpeFontFamily;

static inline HpeRect hpe_rect(float x, float y, float w, float h)
{
    HpeRect r;
    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    return r;
}

static inline int hpe_rect_empty(HpeRect r)
{
    return r.w <= 0.0f || r.h <= 0.0f;
}

#define HPE_UNUSED(x) ((void)(x))

#ifndef HPE_MAX
#define HPE_MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#ifndef HPE_MIN
#define HPE_MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#endif /* HPE_TYPES_H */
