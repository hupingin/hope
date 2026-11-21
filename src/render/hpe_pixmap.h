/* Hope UI Engine - pixel buffer (32bpp, 0xAARRGGBB, top-down, GDI compatible) */
#ifndef HPE_PIXMAP_H
#define HPE_PIXMAP_H

#include "base/hpe_types.h"

typedef struct HpePixmap {
    int width;
    int height;
    int stride; /* bytes per row */
    uint32_t *pixels;
    /* damage-rect support: all raster ops clip to [clip_x0,clip_x1)x[clip_y0,clip_y1) */
    int clip_x0, clip_y0, clip_x1, clip_y1;
} HpePixmap;

void hpe_pixmap_init(HpePixmap *pm, int width, int height);
void hpe_pixmap_resize(HpePixmap *pm, int width, int height);
void hpe_pixmap_free(HpePixmap *pm);
void hpe_pixmap_clear(HpePixmap *pm, HpeColor color);
/* restrict all subsequent drawing to the given rect (intersected with the pixmap) */
void hpe_pixmap_set_clip(HpePixmap *pm, HpeRect rect);
/* narrow the *current* clip to `rect` as well; this is what a nested clip (an
 * <svg> viewport inside a damaged region) needs, since set_clip() replaces it
 * and would let the caller paint outside the damage rect */
void hpe_pixmap_intersect_clip(HpePixmap *pm, HpeRect rect);
void hpe_pixmap_reset_clip(HpePixmap *pm);
/* shifts every row vertically by dy pixels (dy > 0 scrolls content up);
 * vacated rows keep stale pixels and must be repainted by the caller.
 * returns the shift actually applied (clamped to +-height). */
int hpe_pixmap_shift_v(HpePixmap *pm, int dy);
/* same, but only for rows [y0, y1) — lets browser chrome (address bar) stay
 * pinned while the page content below it scrolls. */
int hpe_pixmap_shift_v_rect(HpePixmap *pm, int dy, int y0, int y1);

static inline uint32_t hpe_argb(HpeColor c)
{
    return ((uint32_t)c.a << 24) | ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b;
}

#endif /* HPE_PIXMAP_H */
