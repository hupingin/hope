/* Hope UI Engine - platform abstraction layer (windowing, files, fonts, time) */
#ifndef HPE_PLATFORM_H
#define HPE_PLATFORM_H

#include "base/hpe_types.h"

/* ---------- window ---------- */
typedef struct HpeWindow HpeWindow;

/* modifier bits reported with on_key */
#define HPE_MOD_CTRL 1
#define HPE_MOD_SHIFT 2
#define HPE_MOD_ALT 4

typedef struct HpeWindowCallbacks {
    void (*on_paint)(void *user);
    void (*on_resize)(void *user, int width, int height);
    void (*on_scroll)(void *user, float delta_lines);
    /* virtual-key press; `mods` is a mask of HPE_MOD_* captured at the moment
     * of the key event (needed for shortcuts such as Ctrl+V) */
    void (*on_key)(void *user, int vk_code, int mods);
    /* typed text: unicode code point (ASCII today; surrogate pairs are skipped
     * by the Win32 backend). Used by the address bar for path entry. */
    void (*on_char)(void *user, int codepoint);
    /* mouse: x,y are viewport (client) coords; button 0 = left; down: 1 = pressed, 0 = released;
     * button -1 = pointer move (no button state change). `mods` is a mask of
     * HPE_MOD_* captured with the event (Shift+click extends a text selection).
     * A left-button drag keeps delivering moves while the pointer is outside the
     * window: the backend captures the pointer until the button is released. */
    void (*on_mouse)(void *user, int x, int y, int button, int down, int mods);
    /* periodic tick (~15 ms) used to drain script timers / rAF callbacks */
    void (*on_tick)(void *user);
} HpeWindowCallbacks;

HpeWindow *hpe_window_create(const char *title, int width, int height,
                             const HpeWindowCallbacks *callbacks, void *user);
void hpe_window_present(HpeWindow *win, const void *bgra_pixels, int width, int height,
                        int stride_bytes);
void hpe_window_set_title(HpeWindow *win, const char *title);
void hpe_window_invalidate(HpeWindow *win);
void hpe_window_request_close(HpeWindow *win);
int hpe_window_run(HpeWindow *win); /* blocks until the window closes */
void hpe_window_destroy(HpeWindow *win);

/* ---------- platform services ---------- */
double hpe_time_seconds(void); /* monotonic seconds since an unspecified origin */
/* wall-clock milliseconds since the Unix epoch, UTC (used by Date.now) */
double hpe_time_epoch_ms(void);

char *hpe_read_file(const char *path, size_t *out_size); /* NUL terminated, hpe_free() it */
void hpe_path_dir(const char *path, char *out, size_t out_size);
int hpe_path_is_absolute(const char *path);
void hpe_path_join(char *out, size_t out_size, const char *dir, const char *rel);
/* resolves `path` against the process working directory; on failure copies it
 * through unchanged (used by the address bar to display absolute file paths) */
void hpe_path_absolute(char *out, size_t out_size, const char *path);

/* ---------- clipboard ---------- */
/* Returns the clipboard text as utf-8 (hpe_free() it) or NULL when the
 * clipboard is empty / holds no text. Files copied in Explorer are exposed as
 * the first file's path, which is what an address bar user expects. */
char *hpe_clipboard_get_text(void);
/* Replaces the clipboard with `utf8` (no-op on failure). Returns 1 on success. */
int hpe_clipboard_set_text(const char *utf8);

/* ---------- font service ---------- */
typedef struct HpePlatFont HpePlatFont;

HpePlatFont *hpe_plat_font_create(float pixel_size, int weight, HpeFontFamily family);
void hpe_plat_font_destroy(HpePlatFont *font);
float hpe_plat_font_ascent(HpePlatFont *font);
float hpe_plat_font_descent(HpePlatFont *font);
float hpe_plat_font_line_height(HpePlatFont *font);
float hpe_plat_font_advance(HpePlatFont *font, uint32_t codepoint);

/* renders a utf-8 run into an 8-bit coverage mask.
 * coverage covers x in [0,width) and y in [-descent, ascent) relative to the pen
 * position at the baseline; mask->baseline is the row index of the baseline. */
typedef struct HpeTextMask {
    unsigned char *coverage;
    int width;
    int height;
    int baseline;
} HpeTextMask;

int hpe_plat_font_mask(HpePlatFont *font, const char *utf8, int len, HpeTextMask *out);
void hpe_text_mask_free(HpeTextMask *mask);

#endif /* HPE_PLATFORM_H */
