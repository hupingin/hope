#include "platform/hpe_platform.h"

#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>

#include "base/hpe_util.h"

struct HpeWindow {
    HWND hwnd;
    HpeWindowCallbacks cb;
    void *user;
    const void *pixels;
    int width;
    int height;
    int stride;
};

/* ---------------- utf-8 <-> utf-16 ---------------- */

static int utf8_to_wide_n(const char *s, int len, wchar_t *out, int out_count)
{
    int n;
    if (len <= 0)
        return 0;
    n = MultiByteToWideChar(CP_UTF8, 0, s, len, out, out_count);
    if (n < 0)
        n = 0;
    return n;
}

static void utf8_to_wide(const char *s, wchar_t *out, int out_count)
{
    int n = 0;
    if (s && out_count > 1) {
        n = utf8_to_wide_n(s, (int)strlen(s), out, out_count - 1);
    }
    out[n] = L'\0';
}

/* ---------------- window ---------------- */

static LRESULT CALLBACK hope_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

HpeWindow *hpe_window_create(const char *title, int width, int height,
                             const HpeWindowCallbacks *callbacks, void *user)
{
    static int registered = 0;
    HpeWindow *win;
    RECT rc;
    wchar_t wtitle[512];
    HINSTANCE inst = GetModuleHandleW(NULL);

    if (width < 64)
        width = 64;
    if (height < 64)
        height = 64;

    SetProcessDPIAware();

    if (!registered) {
        WNDCLASSEXW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc = hope_wnd_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = L"HopeWindowClass";
        if (!RegisterClassExW(&wc)) {
            fprintf(stderr, "hope: RegisterClassEx failed (%lu)\n", GetLastError());
            return NULL;
        }
        registered = 1;
    }

    win = (HpeWindow *)hpe_calloc(1, sizeof(HpeWindow));
    win->cb = callbacks ? *callbacks : (HpeWindowCallbacks){0};
    win->user = user;
    win->width = width;
    win->height = height;

    utf8_to_wide(title ? title : "Hope", wtitle, 512);

    rc.left = 0;
    rc.top = 0;
    rc.right = width;
    rc.bottom = height;
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    win->hwnd = CreateWindowExW(0, L"HopeWindowClass", wtitle, WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                rc.right - rc.left, rc.bottom - rc.top,
                                NULL, NULL, inst, win);
    if (!win->hwnd) {
        fprintf(stderr, "hope: CreateWindowEx failed (%lu)\n", GetLastError());
        hpe_free(win);
        return NULL;
    }

    ShowWindow(win->hwnd, SW_SHOW);
    UpdateWindow(win->hwnd);
    if (win->cb.on_tick)
        SetTimer(win->hwnd, 1, 15, NULL); /* script timers / rAF driver */
    return win;
}

void hpe_window_present(HpeWindow *win, const void *bgra_pixels, int width, int height,
                        int stride_bytes)
{
    if (!win)
        return;
    win->pixels = bgra_pixels;
    win->width = width;
    win->height = height;
    win->stride = stride_bytes;
    if (win->hwnd)
        InvalidateRect(win->hwnd, NULL, FALSE);
}

void hpe_window_set_title(HpeWindow *win, const char *title)
{
    wchar_t wtitle[512];
    if (!win || !win->hwnd)
        return;
    utf8_to_wide(title, wtitle, 512);
    SetWindowTextW(win->hwnd, wtitle);
}

void hpe_window_invalidate(HpeWindow *win)
{
    if (win && win->hwnd)
        InvalidateRect(win->hwnd, NULL, FALSE);
}

void hpe_window_request_close(HpeWindow *win)
{
    if (win && win->hwnd)
        PostMessageW(win->hwnd, WM_CLOSE, 0, 0);
}

/* modifier state at the moment an input message is handled */
static int hope_mods(void)
{
    int mods = 0;
    if (GetKeyState(VK_CONTROL) & 0x8000)
        mods |= HPE_MOD_CTRL;
    if (GetKeyState(VK_SHIFT) & 0x8000)
        mods |= HPE_MOD_SHIFT;
    if (GetKeyState(VK_MENU) & 0x8000)
        mods |= HPE_MOD_ALT;
    return mods;
}

static void hope_blit(HpeWindow *win)
{
    HDC hdc;
    BITMAPINFO bmi;
    PAINTSTRUCT ps;

    if (!win || !win->hwnd)
        return;
    hdc = BeginPaint(win->hwnd, &ps);
    if (win->pixels && win->width > 0 && win->height > 0) {
        ZeroMemory(&bmi, sizeof(bmi));
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = win->width;
        bmi.bmiHeader.biHeight = -win->height; /* top-down */
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(hdc, COLORONCOLOR);
        StretchDIBits(hdc, ps.rcPaint.left, ps.rcPaint.top,
                      ps.rcPaint.right - ps.rcPaint.left,
                      ps.rcPaint.bottom - ps.rcPaint.top,
                      ps.rcPaint.left, ps.rcPaint.top,
                      ps.rcPaint.right - ps.rcPaint.left,
                      ps.rcPaint.bottom - ps.rcPaint.top,
                      win->pixels, &bmi, DIB_RGB_COLORS, SRCCOPY);
    } else {
        FillRect(hdc, &ps.rcPaint, (HBRUSH)GetStockObject(BLACK_BRUSH));
    }
    EndPaint(win->hwnd, &ps);
}

static LRESULT CALLBACK hope_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    HpeWindow *win;

    if (msg == WM_CREATE) {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lparam;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return 0;
    }

    win = (HpeWindow *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!win)
        return DefWindowProcW(hwnd, msg, wparam, lparam);

    switch (msg) {
    case WM_PAINT:
        if (win->cb.on_paint)
            win->cb.on_paint(win->user);
        hope_blit(win);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        win->width = LOWORD(lparam);
        win->height = HIWORD(lparam);
        if (win->cb.on_resize)
            win->cb.on_resize(win->user, win->width, win->height);
        return 0;
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wparam);
        if (win->cb.on_scroll)
            win->cb.on_scroll(win->user, -(float)delta / (float)WHEEL_DELTA * 3.0f);
        return 0;
    }
    case WM_KEYDOWN: {
        int mods = hope_mods();
        if (win->cb.on_key)
            win->cb.on_key(win->user, (int)wparam, mods);
        return 0;
    }
    case WM_CHAR:
        if (win->cb.on_char) {
            unsigned int ch = (unsigned int)wparam;
            /* skip control chars (handled as VK_*) and UTF-16 surrogates */
            if (ch >= 32 && (ch < 0xD800 || ch > 0xDFFF))
                win->cb.on_char(win->user, (int)ch);
        }
        return 0;
    case WM_LBUTTONDOWN:
        SetCapture(hwnd); /* keep tracking the drag outside the window */
        if (win->cb.on_mouse)
            win->cb.on_mouse(win->user, (int)(short)LOWORD(lparam),
                            (int)(short)HIWORD(lparam), 0, 1, hope_mods());
        return 0;
    case WM_LBUTTONUP:
        if (GetCapture() == hwnd)
            ReleaseCapture();
        if (win->cb.on_mouse)
            win->cb.on_mouse(win->user, (int)(short)LOWORD(lparam),
                            (int)(short)HIWORD(lparam), 0, 0, hope_mods());
        return 0;
    case WM_MOUSEMOVE:
        /* button = -1 signals a pointer move (no button state change) */
        if (win->cb.on_mouse)
            win->cb.on_mouse(win->user, (int)(short)LOWORD(lparam),
                             (int)(short)HIWORD(lparam), -1, 0, hope_mods());
        return 0;
    case WM_TIMER:
        if (wparam == 1 && win->cb.on_tick)
            win->cb.on_tick(win->user);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

int hpe_window_run(HpeWindow *win)
{
    MSG msg;
    if (!win)
        return 1;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}

void hpe_window_destroy(HpeWindow *win)
{
    if (!win)
        return;
    if (win->hwnd)
        KillTimer(win->hwnd, 1);
    if (win->hwnd)
        DestroyWindow(win->hwnd);
    hpe_free(win);
}

/* ---------------- services ---------------- */

double hpe_time_seconds(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)freq.QuadPart;
}

double hpe_time_epoch_ms(void)
{
    /* FILETIME counts 100 ns ticks since 1601-01-01 UTC; shift to the Unix epoch */
    FILETIME ft;
    ULARGE_INTEGER u;
    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (double)u.QuadPart / 10000.0 - 11644473600000.0;
}

char *hpe_read_file(const char *path, size_t *out_size)
{
    FILE *f;
    long size;
    char *buf;
    size_t got;

    if (out_size)
        *out_size = 0;
    f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    size = ftell(f);
    if (size < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = (char *)hpe_alloc((size_t)size + 1);
    got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[got] = '\0';
    if (got >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB &&
        (unsigned char)buf[2] == 0xBF) {
        memmove(buf, buf + 3, got - 3);
        got -= 3;
        buf[got] = '\0';
    }
    if (out_size)
        *out_size = got;
    return buf;
}

void hpe_path_dir(const char *path, char *out, size_t out_size)
{
    size_t i, last = (size_t)-1;
    if (!path || out_size == 0)
        return;
    for (i = 0; path[i]; i++) {
        if (path[i] == '/' || path[i] == '\\')
            last = i;
    }
    if (last == (size_t)-1) {
        snprintf(out, out_size, ".");
        return;
    }
    if (last >= out_size)
        last = out_size - 1;
    memcpy(out, path, last);
    out[last] = '\0';
    if (out[0] == '\0')
        snprintf(out, out_size, "/");
}

int hpe_path_is_absolute(const char *path)
{
    if (!path || !*path)
        return 0;
    if (path[0] == '/' || path[0] == '\\')
        return 1;
    if (((path[0] >= 'a' && path[0] <= 'z') || (path[0] >= 'A' && path[0] <= 'Z')) &&
        path[1] == ':')
        return 1;
    return 0;
}

void hpe_path_join(char *out, size_t out_size, const char *dir, const char *rel)
{
    while (rel && rel[0] == '.' && (rel[1] == '/' || rel[1] == '\\'))
        rel += 2;
    if (!rel)
        rel = "";
    if (hpe_path_is_absolute(rel) || !dir || !*dir)
        snprintf(out, out_size, "%s", rel);
    else
        snprintf(out, out_size, "%s/%s", dir, rel);
}

void hpe_path_absolute(char *out, size_t out_size, const char *path)
{
    char full[MAX_PATH * 2];
    DWORD n;

    if (!out || out_size == 0)
        return;
    if (!path || !*path) {
        snprintf(out, out_size, "%s", path ? path : "");
        return;
    }
    n = GetFullPathNameA(path, (DWORD)(sizeof(full) / sizeof(full[0])), full, NULL);
    if (n == 0 || n >= sizeof(full) / sizeof(full[0]))
        snprintf(out, out_size, "%s", path);
    else {
        DWORD i;
        for (i = 0; full[i]; i++)
            if (full[i] == '\\')
                full[i] = '/'; /* consistent separators in the address bar */
        snprintf(out, out_size, "%s", full);
    }
}

/* ---------------- clipboard ---------------- */

static char *wide_to_utf8_dup(const wchar_t *w, int len)
{
    int n;
    char *out;

    if (!w)
        return NULL;
    if (len <= 0)
        len = (int)lstrlenW(w);
    if (len <= 0)
        return NULL;
    n = WideCharToMultiByte(CP_UTF8, 0, w, len, NULL, 0, NULL, NULL);
    if (n <= 0)
        return NULL;
    out = (char *)hpe_alloc((size_t)n + 1);
    if (WideCharToMultiByte(CP_UTF8, 0, w, len, out, n, NULL, NULL) != n) {
        hpe_free(out);
        return NULL;
    }
    out[n] = '\0';
    return out;
}

char *hpe_clipboard_get_text(void)
{
    char *result = NULL;

    if (!OpenClipboard(NULL))
        return NULL;
    if (getenv("HPE_TRACE_CLIPBOARD"))
        fprintf(stderr, "clip: open ok, uni=%d hdrop=%d\n",
                IsClipboardFormatAvailable(CF_UNICODETEXT) ? 1 : 0,
                IsClipboardFormatAvailable(CF_HDROP) ? 1 : 0);
    if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        const wchar_t *w = h ? (const wchar_t *)GlobalLock(h) : NULL;
        if (w) {
            result = wide_to_utf8_dup(w, 0);
            GlobalUnlock(h);
        }
    }
    /* Explorer's "copy" (and any shell drag source) exposes files through
     * CF_HDROP; a pasted file should become its path, as in a real browser */
    if (!result && IsClipboardFormatAvailable(CF_HDROP)) {
        HANDLE h = GetClipboardData(CF_HDROP);
        if (getenv("HPE_TRACE_CLIPBOARD"))
            fprintf(stderr, "clip: hdrop handle=%p\n", (void *)h);
        if (h) {
            wchar_t wpath[MAX_PATH * 2];
            UINT n = DragQueryFileW((HDROP)h, 0, wpath,
                                    (UINT)(sizeof(wpath) / sizeof(wpath[0])));
            if (getenv("HPE_TRACE_CLIPBOARD"))
                fprintf(stderr, "clip: dragquery n=%u path=%ls\n", n, n ? wpath : L"(none)");
            if (n > 0)
                result = wide_to_utf8_dup(wpath, (int)n);
        }
    }
    CloseClipboard();
    return result;
}

int hpe_clipboard_set_text(const char *utf8)
{
    wchar_t *w;
    HGLOBAL mem;
    SIZE_T bytes;
    int wlen, ok = 0;

    if (!utf8)
        utf8 = "";
    wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (wlen <= 0)
        return 0;
    w = (wchar_t *)hpe_alloc((size_t)wlen * sizeof(wchar_t));
    if (!MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, wlen)) {
        hpe_free(w);
        return 0;
    }

    bytes = (SIZE_T)wlen * sizeof(wchar_t);
    mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem) {
        void *dst = GlobalLock(mem);
        if (dst) {
            memcpy(dst, w, (size_t)bytes);
            GlobalUnlock(mem);
            if (OpenClipboard(NULL)) {
                EmptyClipboard();
                if (SetClipboardData(CF_UNICODETEXT, mem))
                    ok = 1; /* the clipboard owns `mem` from here on */
                else
                    GlobalFree(mem);
                CloseClipboard();
            } else {
                GlobalFree(mem);
            }
        } else {
            GlobalFree(mem);
        }
    }
    hpe_free(w);
    return ok;
}

/* ---------------- font ---------------- */

struct HpePlatFont {
    HFONT hfont;
    HDC hdc;
    HGDIOBJ old_font;
    int ascent;
    int descent;
    int line_height;
};

HpePlatFont *hpe_plat_font_create(float pixel_size, int weight, HpeFontFamily family)
{
    HpePlatFont *font;
    TEXTMETRICW tm;
    int px = (int)(pixel_size + 0.5f);
    const wchar_t *face;

    if (px < 6)
        px = 6;
    if (px > 240)
        px = 240;

    if (family == HPE_FONT_MONO)
        face = L"Consolas";
    else if (family == HPE_FONT_SERIF)
        face = L"Times New Roman";
    else
        face = L"Microsoft YaHei";

    font = (HpePlatFont *)hpe_calloc(1, sizeof(HpePlatFont));
    font->hdc = CreateCompatibleDC(NULL);
    if (!font->hdc) {
        hpe_free(font);
        return NULL;
    }
    font->hfont = CreateFontW(-px, 0, 0, 0, weight >= 700 ? FW_BOLD : FW_NORMAL,
                              FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                              CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, face);
    if (!font->hfont) {
        DeleteDC(font->hdc);
        hpe_free(font);
        return NULL;
    }
    font->old_font = SelectObject(font->hdc, font->hfont);
    SetMapMode(font->hdc, MM_TEXT);
    if (!GetTextMetricsW(font->hdc, &tm)) {
        DeleteObject(font->hfont);
        DeleteDC(font->hdc);
        hpe_free(font);
        return NULL;
    }
    font->ascent = tm.tmAscent;
    font->descent = tm.tmDescent;
    font->line_height = tm.tmHeight + tm.tmExternalLeading;
    return font;
}

void hpe_plat_font_destroy(HpePlatFont *font)
{
    if (!font)
        return;
    if (font->hdc) {
        SelectObject(font->hdc, font->old_font);
        DeleteDC(font->hdc);
    }
    if (font->hfont)
        DeleteObject(font->hfont);
    hpe_free(font);
}

float hpe_plat_font_ascent(HpePlatFont *font)
{
    return font ? (float)font->ascent : 0.0f;
}

float hpe_plat_font_descent(HpePlatFont *font)
{
    return font ? (float)font->descent : 0.0f;
}

float hpe_plat_font_line_height(HpePlatFont *font)
{
    return font ? (float)font->line_height : 0.0f;
}

static int cp_to_wchars(uint32_t cp, wchar_t out[2])
{
    if (cp < 0x10000) {
        out[0] = (wchar_t)cp;
        return 1;
    }
    cp -= 0x10000;
    out[0] = (wchar_t)(0xD800 + (cp >> 10));
    out[1] = (wchar_t)(0xDC00 + (cp & 0x3FF));
    return 2;
}

float hpe_plat_font_advance(HpePlatFont *font, uint32_t codepoint)
{
    wchar_t buf[2];
    int n;
    SIZE sz;
    if (!font)
        return 0.0f;
    n = cp_to_wchars(codepoint, buf);
    if (!GetTextExtentPoint32W(font->hdc, buf, n, &sz))
        return 0.0f;
    return (float)sz.cx;
}

int hpe_plat_font_mask(HpePlatFont *font, const char *utf8, int len, HpeTextMask *out)
{
    wchar_t wbuf[8192];
    int wlen, w, h, i;
    SIZE sz;
    BITMAPINFO bmi;
    void *bits = NULL;
    HBITMAP hbm;
    HDC memdc;
    HGDIOBJ old_bm, old_font;
    unsigned char *cov;

    if (!out)
        return 0;
    memset(out, 0, sizeof(*out));
    if (!font || !utf8 || len <= 0) {
        out->width = 1;
        out->height = 1;
        out->coverage = (unsigned char *)hpe_calloc(1, 1);
        return 1;
    }

    wlen = utf8_to_wide_n(utf8, len, wbuf, 8191);
    if (wlen <= 0)
        wlen = 0;

    if (wlen == 0) {
        out->width = 1;
        out->height = 1;
        out->coverage = (unsigned char *)hpe_calloc(1, 1);
        return 1;
    }

    if (!GetTextExtentPoint32W(font->hdc, wbuf, wlen, &sz))
        return 0;

    w = sz.cx + 2;
    h = font->ascent + font->descent + 2;
    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;

    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    hbm = CreateDIBSection(font->hdc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!hbm || !bits) {
        if (hbm)
            DeleteObject(hbm);
        return 0;
    }

    memdc = CreateCompatibleDC(font->hdc);
    if (!memdc) {
        DeleteObject(hbm);
        return 0;
    }
    old_bm = SelectObject(memdc, hbm);
    old_font = SelectObject(memdc, font->hfont);
    SetBkMode(memdc, TRANSPARENT);
    SetTextColor(memdc, RGB(255, 255, 255));
    SetTextAlign(memdc, TA_LEFT | TA_TOP);
    TextOutW(memdc, 1, 1, wbuf, wlen);
    SelectObject(memdc, old_font);
    SelectObject(memdc, old_bm);
    DeleteDC(memdc);

    cov = (unsigned char *)hpe_alloc((size_t)w * (size_t)h);
    for (i = 0; i < w * h; i++) {
        const unsigned char *px = (const unsigned char *)bits + (size_t)i * 4;
        unsigned char v = px[0] > px[1] ? px[0] : px[1];
        if (px[2] > v)
            v = px[2];
        cov[i] = v;
    }
    DeleteObject(hbm);

    out->coverage = cov;
    out->width = w;
    out->height = h;
    out->baseline = 1 + font->ascent;
    return 1;
}

void hpe_text_mask_free(HpeTextMask *mask)
{
    if (!mask)
        return;
    if (mask->coverage)
        hpe_free(mask->coverage);
    memset(mask, 0, sizeof(*mask));
}
