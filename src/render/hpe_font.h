/* Hope UI Engine - font objects with advance caching (on top of the platform font service) */
#ifndef HPE_FONT_H
#define HPE_FONT_H

#include "base/hpe_types.h"
#include "platform/hpe_platform.h"

typedef struct HpeFont HpeFont;

/* returns a cached font object; do not free it directly */
HpeFont *hpe_font_get(float pixel_size, int weight, HpeFontFamily family);
float hpe_font_size(HpeFont *font);
int hpe_font_weight(HpeFont *font);
float hpe_font_ascent(HpeFont *font);
float hpe_font_descent(HpeFont *font);
float hpe_font_line_height(HpeFont *font);
float hpe_font_advance(HpeFont *font, uint32_t codepoint);
float hpe_font_measure(HpeFont *font, const char *text, int len);
/* Returns a cache-owned coverage mask for the utf-8 run (borrowed; do not free).
 * NULL on failure. Masks are cached per font+text, so steady-state frames
 * never touch the platform rasterizer for repeated runs. */
const HpeTextMask *hpe_font_mask(HpeFont *font, const char *text, int len);

void hpe_font_shutdown(void);

#endif /* HPE_FONT_H */
