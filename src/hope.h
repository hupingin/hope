/* Hope UI Engine - public api */
#ifndef HOPE_H
#define HOPE_H

#include "render/hpe_pixmap.h"

typedef struct HpeEngine HpeEngine;

HpeEngine *hpe_engine_create(int width, int height);
void hpe_engine_destroy(HpeEngine *engine);

int hpe_engine_load_file(HpeEngine *engine, const char *path);
/* loads the built-in blank page (used when no file argument is given) */
int hpe_engine_load_blank(HpeEngine *engine);
int hpe_engine_reload(HpeEngine *engine);
void hpe_engine_resize(HpeEngine *engine, int width, int height);
void hpe_engine_scroll_by(HpeEngine *engine, float lines);
/* delivers a mouse press/release; button 0 = left; down 1 = pressed, 0 = released.
 * returns 1 if the page mutated (engine already re-laid-out and the window should repaint). */
int hpe_engine_mouse_event(HpeEngine *engine, int x, int y, int button, int down);
/* delivers a pointer move: updates :hover state and fires "mousemove" listeners.
 * returns 1 if a repaint is needed (hover changed or a listener mutated the page). */
int hpe_engine_mouse_move(HpeEngine *engine, int x, int y);
/* drains due timers and the requestAnimationFrame queue.
 * returns 1 if a callback ran (page should be re-laid-out and repainted). */
int hpe_engine_tick(HpeEngine *engine);
void hpe_engine_render(HpeEngine *engine);

/* ---------------- browser chrome: built-in address bar ---------------- */

/* key codes accepted by hpe_engine_key() besides printable code points */
#define HPE_KEY_BACKSPACE 8
#define HPE_KEY_RETURN 13
#define HPE_KEY_ESCAPE 27
/* sentinels above the unicode range, for keys that collide with printable code
 * points ('.' is 0x2E, which is also VK_DELETE) */
#define HPE_KEY_PASTE 0x110000 /* Ctrl+V: insert the clipboard text at the caret */
#define HPE_KEY_COPY 0x110001  /* Ctrl+C: copy the selection (all text if none) */
#define HPE_KEY_CUT 0x110002   /* Ctrl+X: cut the selection */
#define HPE_KEY_SELECT_ALL 0x110003 /* Ctrl+A */
#define HPE_KEY_DELETE 0x110004 /* forward delete */
#define HPE_KEY_LEFT 0x110005
#define HPE_KEY_RIGHT 0x110006
#define HPE_KEY_HOME 0x110007
#define HPE_KEY_END 0x110008

/* enables the engine-drawn address bar at the top of the viewport: the page is
 * laid out/painted below it and receives mouse coordinates relative to itself. */
void hpe_engine_enable_chrome(HpeEngine *engine);
/* height of the chrome strip in pixels (0 when disabled) */
int hpe_engine_chrome_height(HpeEngine *engine);
const char *hpe_engine_address_text(HpeEngine *engine);
int hpe_engine_address_focused(HpeEngine *engine);
/* byte range of the selected address text; returns 0 when the selection is
 * empty (a caret is not a selection, exactly like the omnibox) */
int hpe_engine_address_selection(HpeEngine *engine, int *start, int *end);
/* byte offset of the insertion point (the caret) in the address text */
int hpe_engine_address_caret(HpeEngine *engine);
/* same as hpe_engine_mouse_event() but with the HPE_MOD_* mask of the click
 * (Shift+click extends the address-bar selection) */
int hpe_engine_mouse_event_mods(HpeEngine *engine, int x, int y, int button, int down, int mods);
/* feeds a key/character to the chrome: printable code point (unicode),
 * HPE_KEY_BACKSPACE, HPE_KEY_DELETE, HPE_KEY_LEFT/RIGHT/HOME/END,
 * HPE_KEY_RETURN (navigates to the typed path), HPE_KEY_ESCAPE (restores the
 * address the field got focus with, then blurs) or the HPE_KEY_PASTE/COPY/CUT/
 * SELECT_ALL shortcuts. All of them act on the selection first, like any text
 * field: typing or backspace replaces it, arrows collapse it, and Ctrl+C/X
 * operate on it.
 * returns 1 when the chrome consumed the input. */
int hpe_engine_key(HpeEngine *engine, int key);
/* as hpe_engine_key() but with the HPE_MOD_* mask (Shift extends the selection,
 * Ctrl moves by word) */
int hpe_engine_key_mods(HpeEngine *engine, int key, int mods);

const HpePixmap *hpe_engine_pixmap(HpeEngine *engine);
const char *hpe_engine_status(HpeEngine *engine);
float hpe_engine_scroll_offset(HpeEngine *engine);

/* ---------------- document metadata ---------------- */

/* charset the last document was decoded from ("utf-8" / "latin1" / "utf-16le" /
 * "utf-16be"); valid until the next load */
const char *hpe_engine_document_encoding(HpeEngine *engine);
/* 1 when the charset came from an in-document <meta charset> (as opposed to a
 * BOM or utf-8 validation) */
int hpe_engine_meta_charset(HpeEngine *engine);
/* the resolved <base href> ("" when the document declares none) */
const char *hpe_engine_base_href(HpeEngine *engine);
/* number of <link> elements with a rel other than "stylesheet" (icon/preload/
 * alternate ...), plus accessors for their normalised rel token string and
 * resolved absolute href. Returns 0 when `index` is out of range. */
int hpe_engine_link_count(HpeEngine *engine);
int hpe_engine_link_at(HpeEngine *engine, int index, const char **rel, const char **href);

#endif /* HOPE_H */
