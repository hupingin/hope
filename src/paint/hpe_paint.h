/* Hope UI Engine - painting */
#ifndef HPE_PAINT_H
#define HPE_PAINT_H

#include "layout/hpe_layout.h"
#include "render/hpe_pixmap.h"

/* `origin_y` is the device-space y of the page viewport's top edge: 0 normally,
 * hpe_engine_chrome_height() when a browser chrome (address bar) occupies the
 * top of the window. Content is clipped to [origin_y, pixmap height). */
void hpe_paint_document(HpePixmap *pm, HpeBox *root, float scroll_y, HpeColor page_background,
                        float origin_y);
/* partial repaint constrained to `clip` (used by scroll damage tracking);
 * `clip` is in device coordinates and is intersected with the page viewport. */
void hpe_paint_region(HpePixmap *pm, HpeBox *root, float scroll_y, HpeColor page_background,
                      HpeRect clip, float origin_y);

#endif /* HPE_PAINT_H */
