/* Hope UI Engine - built-in browser chrome (address bar)
 *
 * The address bar is drawn by the engine itself (not by an OS child control) so
 * it works on every platform backend and can be verified headlessly with
 * --dump. Layout: a strip of HPE_CHROME_H pixels pinned to the top of the
 * window; the page viewport is everything below it (see hpe_paint_*'s origin_y
 * and hpe_engine_view_height).
 *
 * The field is a real text editor, modelled on Chrome's omnibox:
 *   - click focuses it and selects the whole address (Chrome does that on the
 *     first click); the next click puts a caret where you clicked
 *   - drag selects a range, double click selects a word, triple click all
 *   - Shift+click / Shift+arrows extend, Ctrl+arrows move by word
 *   - typing, Backspace, Delete and Ctrl+V replace the selection
 *   - Ctrl+A/C/X work on it, Copy with no selection copies everything
 *   - Escape restores the address the field got focus with, then blurs
 * The selection state lives in the engine struct (addr_caret/addr_anchor), so
 * the painter and the event handlers always agree on it.
 */
#include "hope_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "base/hpe_util.h"
#include "platform/hpe_platform.h"
#include "render/hpe_font.h"
#include "render/hpe_raster.h"

#define HPE_CHROME_H 44
#define CHROME_PAD 10.0f
#define FIELD_PAD 8.0f
#define FIELD_H 28.0f
#define FIELD_RADIUS 8.0f
#define ADDR_FONT_PX 13.0f
#define SEL_RADIUS 2.0f
/* two presses within this window and this distance count as a multiple click */
#define CLICK_SECONDS 0.45
#define CLICK_SLOP 4.0f
/* how far the pointer has to travel before a press turns into a drag */
#define DRAG_SLOP 3.0f

static const char PLACEHOLDER[] = "\xE8\xBE\x93\xE5\x85\xA5\xE6\x9C\xAC\xE5\x9C\xB0 "
                                  "HTML \xE6\x96\x87\xE4\xBB\xB6\xE8\xB7\xAF\xE5\xBE\x84"
                                  "\xEF\xBC\x8C\xE5\x9B\x9E\xE8\xBD\xA6\xE6\x89\x93\xE5\xBC\x80";

/* ---------------- geometry ---------------- */

static HpeRect field_rect(const HpeEngine *engine)
{
    return hpe_rect(CHROME_PAD, 8.0f, (float)engine->width - CHROME_PAD * 2.0f, FIELD_H);
}

/* everything the painter and the hit testing need, resolved once */
typedef struct {
    HpeRect field;
    HpeFont *font;
    float inner_x;  /* left edge of the text viewport */
    float inner_w;  /* width of the text viewport */
    float baseline; /* text baseline in device y */
    float text_w;   /* full text advance */
    int valid;
} AddrLayout;

static int addr_layout(HpeEngine *engine, AddrLayout *L)
{
    float ascent, descent;

    memset(L, 0, sizeof(*L));
    if (!engine || engine->chrome_h <= 0)
        return 0;
    L->field = field_rect(engine);
    L->font = hpe_font_get(ADDR_FONT_PX, 400, HPE_FONT_SANS);
    if (!L->font)
        return 0;
    ascent = hpe_font_ascent(L->font);
    descent = hpe_font_descent(L->font);
    L->baseline = L->field.y + (L->field.h + ascent - descent) * 0.5f;
    L->inner_x = L->field.x + 1.0f + FIELD_PAD;
    L->inner_w = L->field.w - 2.0f - FIELD_PAD * 2.0f;
    if (L->inner_w < 4.0f)
        return 0;
    L->text_w = engine->addr_len ? hpe_font_measure(L->font, engine->addr, engine->addr_len) : 0.0f;
    L->valid = 1;
    return 1;
}

/* advance width of the first `index` bytes of the text (a prefix of a run has
 * exactly the summed advances of its code points, so this agrees with
 * hpe_font_measure() on every prefix) */
static float addr_width_to(HpeFont *font, const char *text, int len, int index)
{
    float w = 0.0f;
    int i = 0;

    if (!text || index <= 0)
        return 0.0f;
    if (index > len)
        index = len;
    while (i < index) {
        uint32_t cp;
        int n = hpe_utf8_next(text + i, &cp);
        if (n <= 0)
            break;
        w += hpe_font_advance(font, cp);
        i += n;
    }
    return w;
}

/* byte index of the caret position closest to device x */
static int addr_index_at(HpeEngine *engine, const AddrLayout *L, float x)
{
    float tx = x - L->inner_x + engine->addr_scroll; /* text-space x */
    float w = 0.0f;
    int i = 0;

    while (i < engine->addr_len) {
        uint32_t cp;
        int n = hpe_utf8_next(engine->addr + i, &cp);
        float adv;
        if (n <= 0)
            break;
        adv = hpe_font_advance(L->font, cp);
        if (tx < w + adv * 0.5f)
            return i;
        w += adv;
        i += n;
    }
    return engine->addr_len;
}

/* keeps the caret inside the field: this is what makes dragging past the edge
 * scroll the text, and typing at the end scroll the tail into view. A blurred
 * field shows the tail of the address (browser behaviour). */
static void addr_clamp_scroll(HpeEngine *engine, const AddrLayout *L)
{
    float max_scroll = L->text_w > L->inner_w ? L->text_w - L->inner_w : 0.0f;
    float caret_x;

    if (!engine->addr_focus) {
        engine->addr_scroll = max_scroll;
        return;
    }
    caret_x = addr_width_to(L->font, engine->addr, engine->addr_len, engine->addr_caret);
    if (caret_x - engine->addr_scroll > L->inner_w - 1.0f)
        engine->addr_scroll = caret_x - L->inner_w + 1.0f;
    if (caret_x - engine->addr_scroll < 0.0f)
        engine->addr_scroll = caret_x;
    if (engine->addr_scroll > max_scroll)
        engine->addr_scroll = max_scroll;
    if (engine->addr_scroll < 0.0f)
        engine->addr_scroll = 0.0f;
}

/* ---------------- selection state ---------------- */

static int addr_sel_start(const HpeEngine *engine)
{
    return engine->addr_anchor < engine->addr_caret ? engine->addr_anchor : engine->addr_caret;
}

static int addr_sel_end(const HpeEngine *engine)
{
    return engine->addr_anchor < engine->addr_caret ? engine->addr_caret : engine->addr_anchor;
}

static int addr_has_selection(const HpeEngine *engine)
{
    return engine->addr_anchor != engine->addr_caret;
}

static void addr_set_caret(HpeEngine *engine, int index, int extend)
{
    if (index < 0)
        index = 0;
    if (index > engine->addr_len)
        index = engine->addr_len;
    engine->addr_caret = index;
    if (!extend)
        engine->addr_anchor = index;
}

/* ---------------- code point walking ---------------- */

static int addr_prev_index(const char *text, int i)
{
    if (i <= 0)
        return 0;
    return i - hpe_utf8_prev(text, text + i);
}

static int addr_next_index(const char *text, int i, int len)
{
    uint32_t cp;
    int n;
    if (i >= len)
        return len;
    n = hpe_utf8_next(text + i, &cp);
    return i + (n > 0 ? n : 1);
}

static uint32_t cp_at(const char *text, int i)
{
    uint32_t cp = 0;
    if (i < 0 || !text[i])
        return 0;
    hpe_utf8_next(text + i, &cp);
    return cp;
}

/* word = ASCII alphanumeric, '_' or any non-ASCII code point (so a Chinese path
 * segment is one word, like Chrome's word selection) */
static int is_word_cp(uint32_t cp)
{
    if (cp >= '0' && cp <= '9')
        return 1;
    if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z'))
        return 1;
    if (cp == '_')
        return 1;
    return cp >= 0x80;
}

static int addr_word_left(const char *text, int i)
{
    int j = i;

    while (j > 0) { /* skip the delimiters before the word */
        int k = addr_prev_index(text, j);
        if (is_word_cp(cp_at(text, k)))
            break;
        j = k;
    }
    while (j > 0) {
        int k = addr_prev_index(text, j);
        if (!is_word_cp(cp_at(text, k)))
            break;
        j = k;
    }
    return j;
}

static int addr_word_right(const char *text, int len, int i)
{
    int j = i;

    while (j < len) { /* skip the delimiters after the word */
        if (is_word_cp(cp_at(text, j)))
            break;
        j = addr_next_index(text, j, len);
    }
    while (j < len) {
        if (!is_word_cp(cp_at(text, j)))
            break;
        j = addr_next_index(text, j, len);
    }
    return j;
}

/* the run of same-class code points around `i`, for double click */
static void addr_word_at(const char *text, int len, int i, int *out_a, int *out_b)
{
    uint32_t cp;
    int a, b;
    int word;

    if (len <= 0) {
        *out_a = *out_b = 0;
        return;
    }
    if (i >= len)
        i = addr_prev_index(text, len);
    cp = cp_at(text, i);
    word = is_word_cp(cp);
    a = i;
    b = addr_next_index(text, i, len);
    while (a > 0) {
        int k = addr_prev_index(text, a);
        if (is_word_cp(cp_at(text, k)) != word)
            break;
        a = k;
    }
    while (b < len) {
        if (is_word_cp(cp_at(text, b)) != word)
            break;
        b = addr_next_index(text, b, len);
    }
    *out_a = a;
    *out_b = b;
}

/* ---------------- text buffer ---------------- */

void hpe_chrome_set_text(HpeEngine *engine, const char *text)
{
    size_t n;
    if (!engine)
        return;
    if (!text)
        text = "";
    n = strlen(text);
    if (n >= sizeof(engine->addr))
        n = sizeof(engine->addr) - 1;
    memcpy(engine->addr, text, n);
    engine->addr[n] = '\0';
    engine->addr_len = (int)n;
    engine->addr_caret = engine->addr_len;
    engine->addr_anchor = engine->addr_len;
    engine->addr_scroll = 0.0f;
    engine->addr_drag = 0;
    engine->addr_drag_live = 0;
    /* this is the address the field falls back to on Escape */
    memcpy(engine->addr_committed, engine->addr, n + 1);
}

static int addr_delete_range(HpeEngine *engine, int a, int b)
{
    if (a >= b || b > engine->addr_len)
        return 0;
    memmove(engine->addr + a, engine->addr + b, (size_t)(engine->addr_len - b) + 1);
    engine->addr_len -= b - a;
    engine->addr_caret = a;
    engine->addr_anchor = a;
    return 1;
}

static int addr_delete_selection(HpeEngine *engine)
{
    return addr_delete_range(engine, addr_sel_start(engine), addr_sel_end(engine));
}

/* inserts `len` bytes at the caret, replacing the selection. Truncates on a code
 * point boundary when the buffer is full. */
static int addr_insert_at_caret(HpeEngine *engine, const char *text, int len)
{
    int a = addr_sel_start(engine);
    int b = addr_sel_end(engine);
    int avail, copy;

    if (!text || len <= 0)
        return 0;
    avail = (int)sizeof(engine->addr) - 1 - (engine->addr_len - (b - a));
    copy = len;
    if (copy > avail) {
        copy = avail;
        /* never split a UTF-8 sequence */
        while (copy > 0 && ((unsigned char)text[copy] & 0xC0) == 0x80)
            copy--;
    }
    if (copy <= 0)
        return 0;
    memmove(engine->addr + a + copy, engine->addr + b, (size_t)(engine->addr_len - b) + 1);
    memcpy(engine->addr + a, text, (size_t)copy);
    engine->addr_len = engine->addr_len - (b - a) + copy;
    engine->addr[engine->addr_len] = '\0'; /* the tail move already copied a NUL */
    engine->addr_caret = a + copy;
    engine->addr_anchor = a + copy;
    return 1;
}

/* pasted text is user-controlled: strip line breaks and control characters
 * (clipboards often carry a trailing newline, and multi-line copies must not
 * smuggle a newline into a path) and trim the surrounding blanks */
static char *sanitize_paste(const char *src)
{
    const unsigned char *p = (const unsigned char *)src;
    size_t n = 0, start = 0;
    char *out = (char *)hpe_alloc(strlen(src) + 1);

    while (*p) {
        unsigned char c = *p++;
        if (c < 32 || c == 127)
            continue; /* CR/LF/TAB and other control chars are dropped */
        out[n++] = (char)c;
    }
    out[n] = '\0';
    while (n > 0 && out[n - 1] == ' ')
        out[--n] = '\0';
    while (start < n && out[start] == ' ')
        start++;
    if (start > 0)
        memmove(out, out + start, n - start + 1);
    return out;
}

static int addr_paste(HpeEngine *engine)
{
    char *text = hpe_clipboard_get_text();
    int added;

    if (!text)
        return 0;
    {
        char *clean = sanitize_paste(text);
        added = addr_insert_at_caret(engine, clean, (int)strlen(clean));
        hpe_free(clean);
    }
    hpe_free(text);
    return added;
}

/* Ctrl+C copies the selection; with nothing selected Chrome copies the whole
 * address. Ctrl+X only cuts a real selection. */
static int addr_copy(HpeEngine *engine, int cut)
{
    int a, b, written = 0;

    if (!addr_has_selection(engine))
        return cut ? 0 : (engine->addr_len ? hpe_clipboard_set_text(engine->addr) : 0);
    a = addr_sel_start(engine);
    b = addr_sel_end(engine);
    {
        int n = b - a;
        char *tmp = (char *)hpe_alloc((size_t)n + 1);
        memcpy(tmp, engine->addr + a, (size_t)n);
        tmp[n] = '\0';
        written = hpe_clipboard_set_text(tmp);
        hpe_free(tmp);
    }
    if (cut)
        addr_delete_selection(engine);
    return written || cut;
}

/* ---------------- painting ---------------- */

static void draw_run(HpePixmap *pm, HpeFont *font, const char *text, int len, float x, float baseline,
                     HpeColor color)
{
    const HpeTextMask *mask;
    if (len <= 0)
        return;
    mask = hpe_font_mask(font, text, len);
    if (!mask)
        return;
    hpe_draw_text_mask(pm, (int)x, (int)baseline, mask, color);
}

void hpe_chrome_paint(HpeEngine *engine)
{
    HpePixmap *pm;
    HpeColor bar_bg = hpe_rgb(232, 235, 239);
    HpeColor bar_line = hpe_rgb(201, 208, 216);
    HpeColor field_bg = hpe_rgb(255, 255, 255);
    HpeColor field_border = engine->addr_focus ? hpe_rgb(26, 115, 232) : hpe_rgb(198, 205, 214);
    HpeColor text_color = hpe_rgb(32, 33, 36);
    HpeColor hint_color = hpe_rgb(163, 172, 184);
    HpeColor sel_bg = hpe_rgb(26, 115, 232);
    HpeColor sel_fg = hpe_rgb(255, 255, 255);
    AddrLayout L;
    int sel_a, sel_b;
    float scroll;

    if (!engine || engine->chrome_h <= 0 || !engine->pm.pixels)
        return;
    pm = &engine->pm;
    hpe_pixmap_reset_clip(pm);

    /* strip background + 1px separator */
    hpe_fill_rect(pm, hpe_rect(0.0f, 0.0f, (float)engine->width, (float)engine->chrome_h), bar_bg);
    hpe_fill_rect(pm, hpe_rect(0.0f, (float)engine->chrome_h - 1.0f, (float)engine->width, 1.0f),
                  bar_line);

    if (!addr_layout(engine, &L))
        return;

    /* rounded field with a focus ring (Chrome: 2px accent outline) */
    hpe_fill_round_rect(pm, L.field, FIELD_RADIUS, field_bg);
    hpe_stroke_round_rect(pm, L.field, FIELD_RADIUS, engine->addr_focus ? 2.0f : 1.0f,
                          field_border);

    addr_clamp_scroll(engine, &L);
    scroll = engine->addr_scroll;
    sel_a = addr_sel_start(engine);
    sel_b = addr_sel_end(engine);

    hpe_pixmap_set_clip(pm, hpe_rect(L.inner_x, L.field.y + 2.0f, L.inner_w, L.field.h - 4.0f));
    if (engine->addr_len == 0) {
        draw_run(pm, L.font, PLACEHOLDER, (int)strlen(PLACEHOLDER), L.inner_x, L.baseline,
                 hint_color);
    } else if (sel_b > sel_a && engine->addr_focus) {
        /* an unfocused field shows no highlight (Chrome clears it on blur) */
        /* highlight first, then the three text runs: the selected one in white,
         * so the glyphs stay crisp on top of the selection colour */
        float x0 = L.inner_x - scroll + addr_width_to(L.font, engine->addr, engine->addr_len, sel_a);
        float x1 = L.inner_x - scroll + addr_width_to(L.font, engine->addr, engine->addr_len, sel_b);
        int ix0 = (int)floorf(x0);
        int ix1 = (int)ceilf(x1);

        if (ix1 <= ix0)
            ix1 = ix0 + 1;
        hpe_fill_round_rect(pm,
                            hpe_rect((float)ix0, L.field.y + 5.0f, (float)(ix1 - ix0),
                                     L.field.h - 10.0f),
                            SEL_RADIUS, sel_bg);
        draw_run(pm, L.font, engine->addr, sel_a, L.inner_x - scroll, L.baseline, text_color);
        draw_run(pm, L.font, engine->addr + sel_a, sel_b - sel_a, x0, L.baseline, sel_fg);
        draw_run(pm, L.font, engine->addr + sel_b, engine->addr_len - sel_b, x1, L.baseline,
                 text_color);
    } else {
        draw_run(pm, L.font, engine->addr, engine->addr_len, L.inner_x - scroll, L.baseline,
                 text_color);
    }

    /* caret at the insertion point; a selection hides it (Chrome) */
    if (engine->addr_focus && sel_b == sel_a) {
        float cx = L.inner_x - scroll +
                   addr_width_to(L.font, engine->addr, engine->addr_len, engine->addr_caret);
        if (cx < L.inner_x)
            cx = L.inner_x;
        if (cx <= L.inner_x + L.inner_w)
            hpe_fill_rect(pm, hpe_rect(cx, L.field.y + 5.0f, 1.0f, L.field.h - 10.0f), text_color);
    }
    hpe_pixmap_reset_clip(pm);
}

/* ---------------- mouse ---------------- */

/* a press inside the field: focus, place the caret or start a selection */
static void addr_press(HpeEngine *engine, const AddrLayout *L, float x, int mods)
{
    int index = addr_index_at(engine, L, x);
    double now = hpe_time_seconds();

    engine->addr_drag = 1;
    engine->addr_drag_live = 0;
    engine->addr_press_x = x;
    engine->addr_press_index = index;

    if (fabsf(x - engine->addr_click_x) <= CLICK_SLOP &&
        now - engine->addr_click_t <= CLICK_SECONDS)
        engine->addr_click_count = engine->addr_click_count >= 3 ? 1 : engine->addr_click_count + 1;
    else
        engine->addr_click_count = 1;
    engine->addr_click_t = now;
    engine->addr_click_x = x;

    if (!engine->addr_focus) {
        /* Chrome selects the whole address on the first click of a blurred bar;
         * `addr_press_index` is remembered so that dragging from here turns into
         * an ordinary range selection instead of keeping everything selected */
        engine->addr_anchor = 0;
        engine->addr_caret = engine->addr_len;
    } else if (mods & HPE_MOD_SHIFT) {
        /* Shift+click moves the selection end, keeping the anchor */
        engine->addr_caret = index;
        engine->addr_press_index = engine->addr_anchor;
        engine->addr_drag_live = 1;
    } else if (engine->addr_click_count == 2) {
        int a, b;
        addr_word_at(engine->addr, engine->addr_len, index, &a, &b);
        engine->addr_anchor = a;
        engine->addr_caret = b;
        engine->addr_drag_live = 1; /* dragging now extends from the word */
    } else if (engine->addr_click_count >= 3) {
        engine->addr_anchor = 0;
        engine->addr_caret = engine->addr_len;
        engine->addr_drag_live = 1;
    } else {
        engine->addr_caret = index;
        engine->addr_anchor = index;
    }
    engine->addr_focus = 1;
    engine->dirty = HPE_DIRTY_CHROME;
}

int hpe_chrome_mouse(HpeEngine *engine, int x, int y, int down, int mods)
{
    AddrLayout L;
    int inside;

    if (!engine || engine->chrome_h <= 0 || y >= engine->chrome_h)
        return 0;
    if (!addr_layout(engine, &L)) {
        if (down) {
            engine->addr_focus = 1;
            engine->dirty = HPE_DIRTY_CHROME;
        }
        return 1;
    }
    inside = (float)x >= L.field.x && (float)x <= L.field.x + L.field.w &&
             (float)y >= L.field.y && (float)y <= L.field.y + L.field.h;

    if (down) {
        if (!inside) {
            /* clicking the strip outside the field drops focus, like Chrome */
            engine->addr_focus = 0;
            engine->addr_drag = 0;
            engine->addr_drag_live = 0;
            engine->dirty = HPE_DIRTY_CHROME;
            return 1;
        }
        addr_press(engine, &L, (float)x, mods);
        return 1;
    }
    if (engine->addr_drag) {
        engine->addr_drag = 0;
        engine->addr_drag_live = 0;
        engine->dirty = HPE_DIRTY_CHROME;
        return 1;
    }
    return 0;
}

int hpe_chrome_mouse_move(HpeEngine *engine, int x, int y)
{
    AddrLayout L;

    HPE_UNUSED(y);
    if (!engine || engine->chrome_h <= 0 || !engine->addr_drag)
        return 0;
    if (!addr_layout(engine, &L))
        return 1;
    if (!engine->addr_drag_live && fabsf((float)x - engine->addr_press_x) >= DRAG_SLOP) {
        /* the press point is the anchor once the pointer actually moves */
        engine->addr_drag_live = 1;
        engine->addr_anchor = engine->addr_press_index;
        engine->addr_caret = engine->addr_press_index;
    }
    if (engine->addr_drag_live) {
        engine->addr_caret = addr_index_at(engine, &L, (float)x);
        engine->dirty = HPE_DIRTY_CHROME;
        return 1;
    }
    return 0;
}

/* ---------------- input ---------------- */

/* strips surrounding whitespace and quotes, and an optional file:// prefix */
static void normalize_input(char *s)
{
    size_t n;
    char *start = s;

    while (*start == ' ' || *start == '\t')
        start++;
    if (start != s)
        memmove(s, start, strlen(start) + 1);
    n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = '\0';
    if (n >= 2 && ((s[0] == '"' && s[n - 1] == '"') || (s[0] == '\'' && s[n - 1] == '\''))) {
        memmove(s, s + 1, n - 2);
        s[n - 2] = '\0';
    }
    if (strncmp(s, "file://", 7) == 0) {
        size_t off = 7;
        if (off + 2 < strlen(s) && s[off] == '/' && s[off + 2] == ':')
            off++; /* file:///C:/... -> C:/... */
        memmove(s, s + off, strlen(s + off) + 1);
    }
}

int hpe_chrome_navigate(HpeEngine *engine)
{
    char typed[HPE_ADDR_MAX];
    char abs[HPE_ADDR_MAX * 2];

    if (!engine)
        return 0;
    snprintf(typed, sizeof(typed), "%s", engine->addr);
    normalize_input(typed);
    if (!*typed) {
        hpe_engine_load_blank(engine);
        return 1;
    }
    hpe_path_absolute(abs, sizeof(abs), typed);
    if (hpe_engine_load_file(engine, abs)) {
        hpe_chrome_set_text(engine, abs);
        engine->dirty = HPE_DIRTY_FULL;
        return 1;
    }
    /* keep the typed text so the user can fix the path; load_file already put
     * the error into the status line (shown in the window title) */
    engine->dirty = HPE_DIRTY_CHROME;
    return 0;
}

int hpe_engine_key(HpeEngine *engine, int key)
{
    return hpe_engine_key_mods(engine, key, 0);
}

int hpe_engine_key_mods(HpeEngine *engine, int key, int mods)
{
    int shift = (mods & HPE_MOD_SHIFT) != 0;
    int ctrl = (mods & HPE_MOD_CTRL) != 0;
    int changed = 0;

    if (!engine || engine->chrome_h <= 0 || !engine->addr_focus)
        return 0;

    if (key == HPE_KEY_RETURN) {
        int ok = hpe_chrome_navigate(engine);
        engine->addr_focus = 1;
        engine->dirty = ok ? HPE_DIRTY_FULL : HPE_DIRTY_CHROME;
        return 1;
    }
    if (key == HPE_KEY_ESCAPE) {
        /* Chrome: Escape puts back the address the field was showing when it got
         * focus (the last committed page), then leaves the field */
        if (strcmp(engine->addr, engine->addr_committed) != 0)
            hpe_chrome_set_text(engine, engine->addr_committed);
        engine->addr_focus = 0;
        engine->dirty = HPE_DIRTY_CHROME;
        return 1;
    }

    switch (key) {
    case HPE_KEY_BACKSPACE:
        if (addr_has_selection(engine))
            changed = addr_delete_selection(engine);
        else if (engine->addr_caret > 0)
            changed = addr_delete_range(
                engine,
                ctrl ? addr_word_left(engine->addr, engine->addr_caret)
                     : addr_prev_index(engine->addr, engine->addr_caret),
                engine->addr_caret);
        break;
    case HPE_KEY_DELETE:
        if (addr_has_selection(engine))
            changed = addr_delete_selection(engine);
        else if (engine->addr_caret < engine->addr_len)
            changed = addr_delete_range(
                engine, engine->addr_caret,
                ctrl ? addr_word_right(engine->addr, engine->addr_len, engine->addr_caret)
                     : addr_next_index(engine->addr, engine->addr_caret, engine->addr_len));
        break;
    case HPE_KEY_LEFT:
        if (!shift && addr_has_selection(engine))
            addr_set_caret(engine, addr_sel_start(engine), 0);
        else
            addr_set_caret(engine,
                           ctrl ? addr_word_left(engine->addr, engine->addr_caret)
                                : addr_prev_index(engine->addr, engine->addr_caret),
                           shift);
        changed = 1;
        break;
    case HPE_KEY_RIGHT:
        if (!shift && addr_has_selection(engine))
            addr_set_caret(engine, addr_sel_end(engine), 0);
        else
            addr_set_caret(engine,
                           ctrl ? addr_word_right(engine->addr, engine->addr_len, engine->addr_caret)
                                : addr_next_index(engine->addr, engine->addr_caret, engine->addr_len),
                           shift);
        changed = 1;
        break;
    case HPE_KEY_HOME:
        addr_set_caret(engine, 0, shift);
        changed = 1;
        break;
    case HPE_KEY_END:
        addr_set_caret(engine, engine->addr_len, shift);
        changed = 1;
        break;
    case HPE_KEY_SELECT_ALL:
        engine->addr_anchor = 0;
        engine->addr_caret = engine->addr_len;
        engine->addr_drag_live = 0;
        changed = 1;
        break;
    case HPE_KEY_COPY:
        changed = addr_copy(engine, 0);
        break;
    case HPE_KEY_CUT:
        changed = addr_copy(engine, 1);
        break;
    case HPE_KEY_PASTE:
        changed = addr_paste(engine);
        break;
    default:
        if (key >= 32 && key <= 0x10FFFF) {
            char enc[4];
            int n = hpe_utf8_encode((uint32_t)key, enc);
            changed = addr_insert_at_caret(engine, enc, n);
        }
        break;
    }

    engine->addr_drag = 0;
    engine->addr_drag_live = 0;
    if (!changed)
        return 0;
    engine->dirty = HPE_DIRTY_CHROME;
    return 1;
}

/* ---------------- public api ---------------- */

void hpe_engine_enable_chrome(HpeEngine *engine)
{
    if (!engine || engine->chrome_h > 0)
        return;
    engine->chrome_h = HPE_CHROME_H;
    if (engine->root)
        hpe_engine_relayout(engine); /* narrower viewport: re-clamp the scroll */
    else
        engine->dirty = HPE_DIRTY_FULL;
}

int hpe_engine_chrome_height(HpeEngine *engine)
{
    return engine ? engine->chrome_h : 0;
}

const char *hpe_engine_address_text(HpeEngine *engine)
{
    return engine ? engine->addr : "";
}

int hpe_engine_address_focused(HpeEngine *engine)
{
    return engine ? engine->addr_focus : 0;
}

int hpe_engine_address_caret(HpeEngine *engine)
{
    return engine ? engine->addr_caret : 0;
}

int hpe_engine_address_selection(HpeEngine *engine, int *start, int *end)
{
    int a, b;

    if (!engine || !addr_has_selection(engine))
        return 0;
    a = addr_sel_start(engine);
    b = addr_sel_end(engine);
    if (start)
        *start = a;
    if (end)
        *end = b;
    return 1;
}
