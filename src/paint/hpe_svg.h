/* Hope UI Engine - SVG painter (subset), see hpe_svg.c */
#ifndef HPE_SVG_H
#define HPE_SVG_H

#include "render/hpe_pixmap.h"

struct HpeBox;

/* paints the <svg> subtree of `box` with its top edge at device y `y_dev` */
void hpe_svg_paint(HpePixmap *pm, struct HpeBox *box, float y_dev);

/* Hit testing. (x, y) is a document-space point; returns the deepest SVG element
 * whose geometry contains it (so a click on a <rect> bubbles up to its <g>),
 * or NULL. Paths are not hit tested yet. */
struct HpeDomNode *hpe_svg_hit_test(struct HpeBox *box, float x, float y);

#endif /* HPE_SVG_H */
