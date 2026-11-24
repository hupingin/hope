#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/hpe_util.h"
#include "hope.h"
#include "js/hpe_js.h"
#include "platform/hpe_platform.h"

#define VK_F5_CODE 0x74
#define VK_ESCAPE_CODE 0x1B

/* ---------------- standalone script runner (development helper) ---------------- */

static int run_js_file(const char *path, int as_module)
{
    HpeJs *js = hpe_js_create();
    char dir[1024];
    char errbuf[256];
    char *src;
    int rc;

    if (!js) {
        fprintf(stderr, "hope: out of memory\n");
        return 1;
    }
    src = hpe_read_file(path, NULL);
    if (!src) {
        fprintf(stderr, "hope: cannot open %s\n", path);
        hpe_js_destroy(js);
        return 1;
    }
    hpe_path_dir(path, dir, sizeof(dir));
    hpe_js_set_current_dir(js, dir);
    errbuf[0] = '\0';
    rc = as_module ? hpe_js_eval_module(js, src, dir, errbuf, sizeof(errbuf))
                   : hpe_js_eval(js, src, errbuf, sizeof(errbuf));
    if (rc != 0)
        fprintf(stderr, "hope: %s\n", errbuf[0] ? errbuf : "script error");
    hpe_free(src);
    hpe_js_destroy(js);
    return rc != 0;
}

/* ---------------- headless screenshot (development helper) ---------------- */

static void put_u32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static int write_bmp(const char *path, const HpePixmap *pm)
{
    FILE *f;
    unsigned char header[54];
    uint32_t image_size;
    int y;

    if (!pm || !pm->pixels)
        return 0;
    f = fopen(path, "wb");
    if (!f)
        return 0;

    image_size = (uint32_t)pm->stride * (uint32_t)pm->height;
    memset(header, 0, sizeof(header));
    header[0] = 'B';
    header[1] = 'M';
    put_u32(header + 2, 54 + image_size);
    put_u32(header + 10, 54);
    put_u32(header + 14, 40);
    put_u32(header + 18, (uint32_t)pm->width);
    put_u32(header + 22, (uint32_t)pm->height); /* positive: bottom-up rows */
    header[26] = 1;
    header[28] = 32;
    put_u32(header + 34, image_size);
    fwrite(header, 1, sizeof(header), f);
    for (y = pm->height - 1; y >= 0; y--) {
        const unsigned char *row = (const unsigned char *)pm->pixels + (size_t)y * pm->stride;
        fwrite(row, 1, (size_t)pm->stride, f);
    }
    fclose(f);
    return 1;
}

typedef struct {
    HpeEngine *engine;
    HpeWindow *window;
} App;

static void app_refresh_title(App *app)
{
    char title[768];
    snprintf(title, sizeof(title), "Hope - %s", hpe_engine_status(app->engine));
    hpe_window_set_title(app->window, title);
}

static void app_paint(void *user)
{
    App *app = (App *)user;
    const HpePixmap *pm;

    if (!app->window)
        return;
    hpe_engine_render(app->engine);
    pm = hpe_engine_pixmap(app->engine);
    hpe_window_present(app->window, pm->pixels, pm->width, pm->height, pm->stride);
    app_refresh_title(app);
}

static void app_resize(void *user, int width, int height)
{
    App *app = (App *)user;
    hpe_engine_resize(app->engine, width, height);
    hpe_window_invalidate(app->window);
}

static void app_scroll(void *user, float lines)
{
    App *app = (App *)user;
    hpe_engine_scroll_by(app->engine, lines);
    hpe_window_invalidate(app->window);
}

/* virtual key -> address-bar editing key (0 = not an editing key) */
static int chrome_key_for_vk(int vk)
{
    switch (vk) {
    case 0x08: return HPE_KEY_BACKSPACE;
    case 0x0D: return HPE_KEY_RETURN;
    case 0x1B: return HPE_KEY_ESCAPE;
    case 0x2E: return HPE_KEY_DELETE;
    case 0x25: return HPE_KEY_LEFT;
    case 0x27: return HPE_KEY_RIGHT;
    case 0x24: return HPE_KEY_HOME;
    case 0x23: return HPE_KEY_END;
    default: return 0;
    }
}

/* Ctrl+<letter> editing shortcuts; returns 0 when the combo is not one of them */
static int chrome_key_for_ctrl(int vk)
{
    switch (vk) {
    case 'V':
    case 'v': return HPE_KEY_PASTE;
    case 'C':
    case 'c': return HPE_KEY_COPY;
    case 'X':
    case 'x': return HPE_KEY_CUT;
    case 'A':
    case 'a': return HPE_KEY_SELECT_ALL;
    default: return 0;
    }
}

static void app_key(void *user, int vk, int mods)
{
    App *app = (App *)user;
    int key = 0;

    /* editing shortcuts first: they all act on the address-bar selection */
    if (mods & HPE_MOD_CTRL)
        key = chrome_key_for_ctrl(vk);
    if (!key)
        key = chrome_key_for_vk(vk);

    if (key && hpe_engine_key_mods(app->engine, key, mods)) {
        hpe_window_invalidate(app->window);
        return;
    }
    if (vk == VK_F5_CODE) {
        hpe_engine_reload(app->engine);
        hpe_window_invalidate(app->window);
    } else if (vk == VK_ESCAPE_CODE) {
        hpe_window_request_close(app->window);
    }
}

static void app_char(void *user, int codepoint)
{
    App *app = (App *)user;
    if (hpe_engine_key(app->engine, codepoint))
        hpe_window_invalidate(app->window);
}

static void app_mouse(void *user, int x, int y, int button, int down, int mods)
{
    App *app = (App *)user;
    int changed;
    if (button < 0)
        changed = hpe_engine_mouse_move(app->engine, x, y);
    else
        changed = hpe_engine_mouse_event_mods(app->engine, x, y, button, down, mods);
    if (changed)
        hpe_window_invalidate(app->window);
}

static void app_tick(void *user)
{
    App *app = (App *)user;
    if (hpe_engine_tick(app->engine))
        hpe_window_invalidate(app->window);
}

/* ---------------- command line ---------------- */

typedef struct {
    const char *path;
    const char *dump;
    int width;
    int height;
    int bench;
    int chrome;
    float scroll_lines;
} Options;

static void print_usage(void)
{
    printf("usage: hope [file.html] [--width N] [--height N] [--dump out.bmp] "
           "[--bench N] [--scroll-lines N] [--chrome]\n"
           "       hope --js script.js | --module module.js   (run a script only)\n"
           "       with no file argument the built-in blank page is shown\n");
}

/* returns the process exit code when the run should stop here, or -1 to continue */
static int parse_args(int argc, char **argv, Options *o)
{
    int i;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--width") && i + 1 < argc) {
            o->width = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--height") && i + 1 < argc) {
            o->height = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--dump") && i + 1 < argc) {
            o->dump = argv[++i];
        } else if (!strcmp(argv[i], "--bench") && i + 1 < argc) {
            o->bench = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--chrome")) {
            o->chrome = 1;
        } else if (!strcmp(argv[i], "--js") && i + 1 < argc) {
            return run_js_file(argv[++i], 0);
        } else if (!strcmp(argv[i], "--module") && i + 1 < argc) {
            return run_js_file(argv[++i], 1);
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            print_usage();
            return 0;
        } else if (!strcmp(argv[i], "--scroll-lines") && i + 1 < argc) {
            o->scroll_lines = (float)atof(argv[++i]);
        } else {
            o->path = argv[i];
        }
    }
    return -1;
}

/* window mode always shows the address bar; headless --dump stays page-only
 * unless --chrome is passed (keeps pixel baselines stable) */
static void load_start_document(HpeEngine *engine, const Options *o)
{
    if (!o->dump || o->chrome)
        hpe_engine_enable_chrome(engine);
    if (o->path) {
        if (!hpe_engine_load_file(engine, o->path))
            fprintf(stderr, "hope: %s\n", hpe_engine_status(engine));
    } else if (!hpe_engine_load_blank(engine)) {
        fprintf(stderr, "hope: %s\n", hpe_engine_status(engine));
    }
}

/* ---------------- address-bar test helpers ---------------- */

#define ADDR_TEST_Y 22 /* vertical centre of the address field */

static void synth_click(HpeEngine *engine, int x, int y, int mods)
{
    hpe_engine_mouse_move(engine, x, y);
    hpe_engine_mouse_event_mods(engine, x, y, 0, 1, mods);
    hpe_engine_mouse_event_mods(engine, x, y, 0, 0, mods);
}

/* press, walk the pointer over (like a human drag), release */
static void synth_drag(HpeEngine *engine, int x1, int x2, int y)
{
    int i;
    hpe_engine_mouse_event_mods(engine, x1, y, 0, 1, 0);
    for (i = 1; i <= 6; i++)
        hpe_engine_mouse_move(engine, x1 + (x2 - x1) * i / 6, y);
    hpe_engine_mouse_event_mods(engine, x2, y, 0, 0, 0);
}

typedef struct {
    const char *name;
    int key;
    int mods;
} KeyName;

static int key_named(const char *name, int *key, int *mods)
{
    static const KeyName table[] = {
        {"BACKSPACE", HPE_KEY_BACKSPACE, 0},
        {"DELETE", HPE_KEY_DELETE, 0},
        {"CTRL_BACKSPACE", HPE_KEY_BACKSPACE, HPE_MOD_CTRL},
        {"CTRL_DELETE", HPE_KEY_DELETE, HPE_MOD_CTRL},
        {"LEFT", HPE_KEY_LEFT, 0},
        {"RIGHT", HPE_KEY_RIGHT, 0},
        {"HOME", HPE_KEY_HOME, 0},
        {"END", HPE_KEY_END, 0},
        {"ENTER", HPE_KEY_RETURN, 0},
        {"ESCAPE", HPE_KEY_ESCAPE, 0},
        {"SHIFT_LEFT", HPE_KEY_LEFT, HPE_MOD_SHIFT},
        {"SHIFT_RIGHT", HPE_KEY_RIGHT, HPE_MOD_SHIFT},
        {"SHIFT_HOME", HPE_KEY_HOME, HPE_MOD_SHIFT},
        {"SHIFT_END", HPE_KEY_END, HPE_MOD_SHIFT},
        {"CTRL_LEFT", HPE_KEY_LEFT, HPE_MOD_CTRL},
        {"CTRL_RIGHT", HPE_KEY_RIGHT, HPE_MOD_CTRL},
        {"CTRL_A", HPE_KEY_SELECT_ALL, HPE_MOD_CTRL},
        {"CTRL_C", HPE_KEY_COPY, HPE_MOD_CTRL},
        {"CTRL_X", HPE_KEY_CUT, HPE_MOD_CTRL},
        {"CTRL_V", HPE_KEY_PASTE, HPE_MOD_CTRL},
        {"PASTE", HPE_KEY_PASTE, 0},
    };
    size_t i;
    for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (!strcmp(name, table[i].name)) {
            *key = table[i].key;
            *mods = table[i].mods;
            return 1;
        }
    }
    return 0;
}

/* feed a UTF-8 string into the engine as individual key strokes */
static void synth_type(HpeEngine *engine, const char *s)
{
    const char *p = s;
    uint32_t cp;
    while (*p) {
        int n = hpe_utf8_next(p, &cp);
        if (n <= 0)
            break;
        hpe_engine_key(engine, (int)cp);
        p += n;
    }
}

/* dev/test hook: drive the address bar exactly like a user would (focus by
 * clicking the strip, then type + Enter) so headless dumps can verify
 * navigation. HOPE_TEST_ADDR=/path/to/page.html */
static void hook_test_addr(HpeEngine *engine)
{
    const char *typed = getenv("HOPE_TEST_ADDR");
    if (!typed || !*typed)
        return;
    synth_click(engine, 20, ADDR_TEST_Y, 0); /* focus the field */
    synth_type(engine, typed);
    if (getenv("HOPE_TEST_ADDR_ENTER"))
        hpe_engine_key(engine, HPE_KEY_RETURN);
}

/* dev/test hook: seed the clipboard, then paste with Ctrl+V semantics.
 * HOPE_TEST_CLIPBOARD=text [HOPE_TEST_PASTE=1] [HOPE_TEST_PASTE_ENTER=1] */
static void hook_test_clipboard(HpeEngine *engine)
{
    const char *seed = getenv("HOPE_TEST_CLIPBOARD");
    if (seed)
        hpe_clipboard_set_text(seed);
    if (getenv("HOPE_TEST_PASTE")) {
        synth_click(engine, 20, ADDR_TEST_Y, 0); /* focus the field */
        hpe_engine_key(engine, HPE_KEY_PASTE);
        if (getenv("HOPE_TEST_PASTE_ENTER"))
            hpe_engine_key(engine, HPE_KEY_RETURN);
    }
}

static void hook_addr_click(HpeEngine *engine, const char *click)
{
    char buf[256];
    char *tok;
    snprintf(buf, sizeof(buf), "%s", click);
    for (tok = strtok(buf, ";"); tok; tok = strtok(NULL, ";")) {
        int cx = 0, mods = 0;
        if (sscanf(tok, "%d,%d", &cx, &mods) >= 1)
            synth_click(engine, cx, ADDR_TEST_Y, mods);
    }
}

static void hook_addr_dblclick(HpeEngine *engine, const char *dbl)
{
    int x1 = 0, x2 = 0;
    if (sscanf(dbl, "%d,%d", &x1, &x2) >= 1) {
        if (x2 == 0)
            x2 = x1;
        synth_click(engine, x1, ADDR_TEST_Y, 0);
        synth_click(engine, x2, ADDR_TEST_Y, 0);
    }
}

static void hook_addr_drag(HpeEngine *engine, const char *drag)
{
    int x1 = 0, x2 = 0, dy = ADDR_TEST_Y;
    if (sscanf(drag, "%d,%d,%d", &x1, &x2, &dy) >= 2)
        synth_drag(engine, x1, x2, dy);
}

static void hook_addr_keys(HpeEngine *engine, const char *keys)
{
    char buf[512];
    char *tok;
    snprintf(buf, sizeof(buf), "%s", keys);
    for (tok = strtok(buf, ";"); tok; tok = strtok(NULL, ";")) {
        int key = 0, mods = 0;
        if (key_named(tok, &key, &mods))
            hpe_engine_key_mods(engine, key, mods);
    }
}

/* dev/test hooks for the address-bar text editor:
 *   HOPE_TEST_ADDR_CLICK=x[,mods]     one click into the field
 *   HOPE_TEST_ADDR_DBLCLICK=x[,x2]    two clicks (word selection)
 *   HOPE_TEST_ADDR_DRAG=x1,x2[,y]     press, drag, release
 *   HOPE_TEST_TYPE=text               typed characters
 *   HOPE_TEST_KEYS="LEFT;SHIFT_RIGHT;CTRL_C;BACKSPACE"   named keys */
static void hook_test_addr_editor(HpeEngine *engine)
{
    const char *click = getenv("HOPE_TEST_ADDR_CLICK");
    const char *dbl = getenv("HOPE_TEST_ADDR_DBLCLICK");
    const char *drag = getenv("HOPE_TEST_ADDR_DRAG");
    const char *type = getenv("HOPE_TEST_TYPE");
    const char *keys = getenv("HOPE_TEST_KEYS");

    if (click && *click)
        hook_addr_click(engine, click);
    if (dbl && *dbl)
        hook_addr_dblclick(engine, dbl);
    if (drag && *drag)
        hook_addr_drag(engine, drag);
    if (type && *type)
        synth_type(engine, type);
    if (keys && *keys)
        hook_addr_keys(engine, keys);
}

/* dev/test hook: synthesise a click so headless dumps can drive interactive
 * pages (a mind-map node collapses, a button fires). HOPE_TEST_CLICK_AT=x,y[,button] */
static void hook_test_click_at(HpeEngine *engine)
{
    const char *click = getenv("HOPE_TEST_CLICK_AT");
    char buf[256];
    char *tok;
    if (!click || !*click)
        return;
    snprintf(buf, sizeof(buf), "%s", click);
    for (tok = strtok(buf, ";"); tok; tok = strtok(NULL, ";")) {
        int cx = 0, cy = 0, btn = 1;
        if (sscanf(tok, "%d,%d,%d", &cx, &cy, &btn) < 2)
            continue;
        hpe_engine_mouse_move(engine, cx, cy);
        hpe_engine_mouse_event(engine, cx, cy, btn, 1);
        hpe_engine_mouse_event(engine, cx, cy, btn, 0);
    }
}

/* report the address-bar state once every hook above has run */
static void report_addr_text(HpeEngine *engine)
{
    int a = 0, b = 0, sel;
    if (!getenv("HOPE_TEST_ADDR_TEXT"))
        return;
    sel = hpe_engine_address_selection(engine, &a, &b);
    printf("addr: [%s] len=%d sel=%d..%d caret=%d focus=%d\n", hpe_engine_address_text(engine),
           (int)strlen(hpe_engine_address_text(engine)), sel ? a : -1, sel ? b : -1,
           hpe_engine_address_caret(engine), hpe_engine_address_focused(engine));
}

/* dev/test hook: print document metadata detected at load time (encoding,
 * <meta charset>, <base href>, <link> resource hints). HOPE_TEST_META=1 */
static void report_meta(HpeEngine *engine)
{
    int lc, li;
    if (!getenv("HOPE_TEST_META"))
        return;
    lc = hpe_engine_link_count(engine);
    printf("encoding=%s meta=%d base=%s\n", hpe_engine_document_encoding(engine),
           hpe_engine_meta_charset(engine), hpe_engine_base_href(engine));
    printf("links=%d\n", lc);
    for (li = 0; li < lc; li++) {
        const char *rel = "";
        const char *href = "";
        if (hpe_engine_link_at(engine, li, &rel, &href))
            printf("link[%d] rel=%s href=%s\n", li, rel, href);
    }
}

/* ---------------- run modes ---------------- */

static int run_headless(HpeEngine *engine, const Options *o)
{
    if (o->scroll_lines != 0.0f) {
        /* paint a full frame first so the scroll below goes through the
         * damage-rect path — this is what makes --scroll-lines a real test
         * of incremental scrolling (compare with HPE_NO_DIRTY=1) */
        hpe_engine_render(engine);
        hpe_engine_scroll_by(engine, o->scroll_lines);
    }
    hpe_engine_render(engine);
    if (o->bench > 1) {
        double t0 = hpe_time_seconds();
        double t_last, avg;
        int n;
        for (n = 1; n < o->bench; n++)
            hpe_engine_render(engine);
        t_last = hpe_time_seconds();
        avg = (t_last - t0) * 1000.0 / (o->bench - 1);
        fprintf(stderr, "bench: %d repaints, avg paint %.4f ms\n", o->bench - 1, avg);
    }
    if (o->dump) {
        if (write_bmp(o->dump, hpe_engine_pixmap(engine)))
            printf("%s\n", hpe_engine_status(engine));
        else
            fprintf(stderr, "hope: cannot write %s\n", o->dump);
    }
    hpe_engine_destroy(engine);
    return 0;
}

static int run_window(HpeEngine *engine, int width, int height)
{
    HpeWindowCallbacks callbacks;
    App app;

    memset(&app, 0, sizeof(app));
    app.engine = engine;

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.on_paint = app_paint;
    callbacks.on_resize = app_resize;
    callbacks.on_scroll = app_scroll;
    callbacks.on_key = app_key;
    callbacks.on_char = app_char;
    callbacks.on_mouse = app_mouse;
    callbacks.on_tick = app_tick;

    app.window = hpe_window_create("Hope", width, height, &callbacks, &app);
    if (!app.window) {
        hpe_engine_destroy(engine);
        return 1;
    }
    app_refresh_title(&app);
    hpe_window_invalidate(app.window);
    hpe_window_run(app.window);

    hpe_window_destroy(app.window);
    hpe_engine_destroy(engine);
    return 0;
}

int main(int argc, char **argv)
{
    Options o;
    HpeEngine *engine;
    int rc;

    memset(&o, 0, sizeof(o));
    o.width = 1000;
    o.height = 700;
    rc = parse_args(argc, argv, &o);
    if (rc >= 0)
        return rc;

    engine = hpe_engine_create(o.width, o.height);
    load_start_document(engine, &o);

    hook_test_addr(engine);
    hook_test_clipboard(engine);
    hook_test_addr_editor(engine);
    hook_test_click_at(engine);
    report_addr_text(engine);
    report_meta(engine);

    if (o.dump || o.bench)
        return run_headless(engine, &o);
    return run_window(engine, o.width, o.height);
}
