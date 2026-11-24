/* Hope UI Engine - box tree and block/inline layout */
#ifndef HPE_LAYOUT_H
#define HPE_LAYOUT_H

#include "css/hpe_css.h"
#include "dom/hpe_dom.h"
#include "render/hpe_font.h"

typedef enum HpeBoxKind {
    HPE_BOX_BLOCK = 0,
    HPE_BOX_INLINE,
    HPE_BOX_TEXT,
    HPE_BOX_INLINE_BLOCK
} HpeBoxKind;

typedef struct HpeTextRun {
    const char *text;
    int len;
    HpeFont *font;
    HpeColor color;
    float x;
    float width;
    float ascent;
    float descent;
    float line_height;
    struct HpeBox *atomic;   /* non-NULL => this run is an inline-block box */
} HpeTextRun;

typedef struct HpeLine {
    HpeTextRun *runs;
    int run_count;
    size_t run_cap;
    float width;
    float height;
    float baseline;
    float y;
} HpeLine;

/* one floated box inside a block container's float band */
typedef struct HpeFloatBox {
    struct HpeBox *box;
    float top, bottom; /* band extent in document coords (margin box) */
    float left, right; /* horizontal extent in document coords (margin box) */
    int side;          /* HpeFloatSide */
} HpeFloatBox;

typedef struct HpeFloatBand {
    HpeFloatBox *items;
    int count;
    size_t cap;
} HpeFloatBand;

typedef struct HpeBox {
    HpeBoxKind kind;
    HpeDomNode *node;
    HpeStyle style;
    HpeRect border_box;
    HpeRect content_box;
    HpeRect pad_box;     /* padding box in document coords (containing block for abspos) */
    const char *text;
    int text_len;
    HpeLine *lines;
    int line_count;
    size_t line_cap;
    struct HpeBox *parent;
    struct HpeBox *first_child;
    struct HpeBox *last_child;
    struct HpeBox *next;
    HpeFloatBand floats; /* floated children of this block container */
} HpeBox;

/* builds the box tree for a document and lays it out into `viewport_width` */
HpeBox *hpe_layout_build(HpeDomNode *document, HpeStylesheet *sheet, float viewport_width,
                         float viewport_height);
/* incremental counterpart used after a DOM mutation. `old_root` is the box tree
 * of the previous pass and `reflow_root` the highest DOM node whose subtree
 * changed. Boxes outside that subtree are reused verbatim (their styles cannot
 * have changed because the stylesheet did not); only the path from the root down
 * to `reflow_root` is rebuilt. Falls back to a full build when the inputs do not
 * allow it. The ordering/positioning pass always runs, so the geometry is
 * identical to hpe_layout_build(). */
HpeBox *hpe_layout_rebuild(HpeDomNode *document, HpeStylesheet *sheet, float viewport_width,
                           float viewport_height, HpeBox *old_root,
                           HpeDomNode *reflow_root);
void hpe_layout_free(HpeBox *box);
float hpe_layout_content_height(HpeBox *root);

#endif /* HPE_LAYOUT_H */
