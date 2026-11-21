/* Hope UI Engine - software rasterizer primitives */
#ifndef HPE_RASTER_H
#define HPE_RASTER_H

#include "base/hpe_types.h"
#include "platform/hpe_platform.h"
#include "render/hpe_pixmap.h"

void hpe_fill_rect(HpePixmap *pm, HpeRect rect, HpeColor color);
void hpe_fill_rect_i(HpePixmap *pm, int x, int y, int w, int h, HpeColor color);
/* anti-aliased rounded rectangle (filled, or an inner border ring of `width`) */
void hpe_fill_round_rect(HpePixmap *pm, HpeRect rect, float radius, HpeColor color);
void hpe_stroke_round_rect(HpePixmap *pm, HpeRect rect, float radius, float width, HpeColor color);
/* composites an 8-bit coverage mask; (x, y) is the pen position on the baseline */
void hpe_draw_text_mask(HpePixmap *pm, int x, int y, const HpeTextMask *mask, HpeColor color);

/* one colour stop of a gradient */
typedef struct HpeRasterStop {
    float pos; /* 0..1, clamped */
    HpeColor color;
} HpeRasterStop;

/* samples the colour of a stop list at parameter t (0..1); shared by the CSS
 * background gradients and the SVG gradient painter */
void hpe_sample_stops(const HpeRasterStop *stops, int stop_count, float t, HpeColor *out);

/* linear gradient across `rect`; `angle_deg` follows CSS: 0 = upward (to top),
 * 90 = rightward (to right) — the engine only needs the axis, so the extremes
 * (0/90/180/270) are what reference pages use. `stops` must be sorted or the
 * caller may pass them unsorted (they are sampled by position scan here). */
void hpe_fill_linear_gradient(HpePixmap *pm, HpeRect rect, float angle_deg,
                              const HpeRasterStop *stops, int stop_count);
/* filled rounded rect with a linear gradient interior */
void hpe_fill_round_rect_gradient(HpePixmap *pm, HpeRect rect, float radius, float angle_deg,
                                  const HpeRasterStop *stops, int stop_count);
/* soft drop shadow cast by `rect` (blur approximated by concentric alpha rings) */
void hpe_fill_shadow(HpePixmap *pm, HpeRect rect, float radius, float blur, HpeColor color);

#endif /* HPE_RASTER_H */
