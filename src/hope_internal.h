/* Hope UI Engine - engine internals shared between hope.c and hope_chrome.c
 *
 * Not part of the public API (see hope.h); keeping the struct here lets the
 * browser-chrome implementation in hope_chrome.c reach engine state without
 * adding accessors to the public header.
 */
#ifndef HPE_HOPE_INTERNAL_H
#define HPE_HOPE_INTERNAL_H

#include "hope.h"

#include "css/hpe_css.h"
#include "dom/hpe_dom.h"
#include "js/hpe_js.h"
#include "layout/hpe_layout.h"
#include "paint/hpe_paint.h"
#include "render/hpe_pixmap.h"

#define HPE_SCROLL_LINE_PX 48.0f
#define HPE_ADDR_MAX 1024
/* <link> resource hints we record (stylesheet excluded -- it is consumed) */
#define HPE_MAX_LINKS 32
#define HPE_MAX_PATH 1024

/* dirty flags: what the next render() has to do */
#define HPE_DIRTY_NONE 0
#define HPE_DIRTY_SCROLL 1 /* only the scroll offset moved */
#define HPE_DIRTY_FULL 2   /* full repaint (load, resize, DOM/style mutation) */
#define HPE_DIRTY_CHROME 3 /* only the address bar needs repainting */

/* how much work the accumulated DOM mutations require on the next flush.
 * Ordered: a larger value subsumes the smaller one. */
#define HPE_REFLOW_NONE 0   /* nothing changed: skip layout and repaint */
#define HPE_REFLOW_PAINT 1  /* paint-only change (colour, opacity, shadow): restyle, no geometry */
#define HPE_REFLOW_LAYOUT 2 /* geometry may have changed: rebuild the box tree */

struct HpeEngine {
    HpePixmap pm;
    int width;
    int height;
    char *path;
    HpeDomNode *doc;
    HpeStylesheet sheet;
    HpeBox *root;
    HpeJs *js;
    float scroll_y;
    float painted_scroll; /* scroll offset actually present in the pixmap */
    int painted;          /* 1 once a full frame exists in the pixmap */
    int dirty;
    HpeDomNode *hover_node; /* element currently under the pointer (:hover) */
    float content_height;
    /* author stylesheet bookkeeping: the number of rules parsed after the
     * user-agent sheet and the last (dom node, text revision) that produced
     * them. lets re-collection skip when nothing about <style>/<link> changed. */
    int author_rule_base;
    HpeDomNode *style_sig;
    unsigned int style_rev;
    /* DOM-mutation tracker: the maximum reflow the mutations since the last
     * flush require, whether a flush has a pending change at all, and the DOM
     * element whose box subtree has to be rebuilt (NULL => full rebuild). */
    int reflow_need;
    int mutation_pending;
    HpeDomNode *reflow_node;
    double load_ms;
    double render_ms;
    char status[512];
    char error[256]; /* sticky load error, cleared on the next successful load */

    /* document metadata resolved at load time */
    char doc_encoding[16];      /* detected charset label, e.g. "utf-8" */
    int meta_charset;           /* 1 when <meta charset> declared the encoding */
    char base_href[HPE_MAX_PATH]; /* <base href> ("" when absent) */
    /* <link> resources other than the stylesheet, kept for resource hints */
    struct {
        char rel[24];  /* lowercased rel tokens joined by space */
        char href[HPE_MAX_PATH]; /* resolved absolute path */
    } links[HPE_MAX_LINKS];
    int link_count;

    /* browser chrome (address bar), disabled while chrome_h == 0 */
    int chrome_h;
    char addr[HPE_ADDR_MAX]; /* current text (utf-8) */
    int addr_len;
    int addr_focus;
    float addr_scroll; /* horizontal text scroll inside the field, px */
    /* text-editing state: `addr_caret` is the insertion point (byte index),
     * `addr_anchor` is where the current selection started. They differ only
     * while something is selected; the caret is hidden then (as in Chrome). */
    int addr_caret;
    int addr_anchor;
    int addr_drag;       /* left button held inside the field: dragging extends */
    int addr_drag_live;  /* the pointer moved far enough to start a selection */
    float addr_press_x;  /* press position, for the drag threshold */
    int addr_press_index; /* caret index under the press point (== anchor while dragging) */
    double addr_click_t; /* last press time/position, for double/triple click */
    float addr_click_x;
    int addr_click_count;
    char addr_committed[HPE_ADDR_MAX]; /* text as of focus time; Escape restores it */
};

/* re-builds the box tree and clamps the scroll offset (hope.c) */
void hpe_engine_relayout(HpeEngine *engine);
/* applies the DOM mutations recorded since the last call, doing only as much
 * layout/repaint as they require (hope.c). `force` requests a full rebuild. */
void hpe_engine_flush(HpeEngine *engine, int force);
/* refreshes the status line (hope.c) */
void hpe_engine_refresh_status(HpeEngine *engine);
/* height of the page viewport (window height minus chrome) */
int hpe_engine_view_height(HpeEngine *engine);

/* ---------------- hope_chrome.c ---------------- */

/* replaces the address text (truncating to the field capacity) and puts the
 * caret at the end, selection collapsed */
void hpe_chrome_set_text(HpeEngine *engine, const char *text);
/* paints the address bar into the top `chrome_h` pixels */
void hpe_chrome_paint(HpeEngine *engine);
/* pointer press/release inside the chrome strip; returns 1 when consumed.
 * `mods` carries HPE_MOD_SHIFT (Shift+click extends the selection). */
int hpe_chrome_mouse(HpeEngine *engine, int x, int y, int down, int mods);
/* pointer move while the field is being dragged; returns 1 when consumed */
int hpe_chrome_mouse_move(HpeEngine *engine, int x, int y);
/* loads engine->addr (empty text reloads the built-in blank page) */
int hpe_chrome_navigate(HpeEngine *engine);

#endif /* HPE_HOPE_INTERNAL_H */
