#include "platform/hpe_platform.h"

#include <windows.h>
#include <imm.h>
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

/**
 * @brief 把前 `len` 字节 UTF-8 转成 UTF-16 / Convert the first `len` bytes of UTF-8 to UTF-16
 *
 * @param[in]     s UTF-8 源文本 / Source UTF-8 text
 * @param[in]     len 参与转换的字节数 / Number of bytes to convert
 * @param[out]    out 接收宽字符的缓冲 / Receives the wide characters
 * @param[in]     out_count 宽字符缓冲容量（含结尾） / Capacity in wide chars, including the terminator
 *
 * @return 写入的宽字符数 / Number of wide characters written
 * @retval     0 `len <= 0` 或转换失败 / `len <= 0` or conversion failed
 *
 * @note 直接封装的 `MultiByteToWideChar`，不做 NUL 终止 / Thin wrapper over `MultiByteToWideChar`, no NUL termination
 */

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

/**
 * @brief 把以 NUL 结尾的 UTF-8 文本转成 UTF-16 / Convert a NUL-terminated UTF-8 string to UTF-16
 *
 * @param[in]     s UTF-8 源文本，可为 NULL / Source UTF-8 text, may be NULL
 * @param[out]    out 接收宽字符的缓冲 / Receives the wide characters
 * @param[in]     out_count 宽字符缓冲容量（含结尾） / Capacity in wide chars, including the terminator
 *
 * @note `s` 为 NULL 或容量不足 2 时只写出结尾 NUL / With `s` NULL or capacity below 2 only the terminating NUL is written
 */
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
static int cp_to_wchars(uint32_t cp, wchar_t out[2]); /* defined in the font section / 定义见字体一节 */

/**
 * @brief 注册共享窗口类（每进程一次）/ Register the shared window class once per process
 *
 * @param[in]     inst 进程实例句柄 / Process instance handle
 *
 * @return 注册成功或此前已注册返回 1，失败返回 0 / 1 when registered or already registered, 0 on failure
 * @retval     1 类已可用 / The class is usable
 * @retval     0 `RegisterClassExW` 失败，已打印错误 / `RegisterClassExW` failed and an error was printed
 *
 * @note 幂等：用静态标志保证只注册一次 / Idempotent: a static flag makes registration happen only once
 *
 * @see hope_create_hwnd
 */
static int hope_register_class(HINSTANCE inst)
{
    static int registered = 0;
    WNDCLASSEXW wc;
    if (registered)
        return 1;
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
        return 0;
    }
    registered = 1;
    return 1;
}

/**
 * @brief 创建容纳 `width`×`height` 客户区像素的 OS 窗口 / Create an OS window sized for a `width`×`height` client area
 *
 * @param[in]     inst 进程实例句柄 / Process instance handle
 * @param[in]     title 窗口标题，NULL 时用 `Hope` / Window title, defaults to `Hope` when NULL
 * @param[in]     width 客户区宽度（像素）/ Client width in pixels
 * @param[in]     height 客户区高度（像素）/ Client height in pixels
 * @param[in,out] win 作为创建参数透传给窗口过程的窗口对象 / Window object passed through as the creation parameter
 *
 * @return 新窗口句柄，失败返回 NULL / The new window handle, NULL on failure
 *
 * @note 用 `AdjustWindowRect` 把客户区尺寸换算成含边框的窗口尺寸 / `AdjustWindowRect` expands the client size into the full frame size
 *
 * @see hope_register_class hpe_window_create
 */
static HWND hope_create_hwnd(HINSTANCE inst, const char *title, int width, int height,
                             HpeWindow *win)
{
    wchar_t wtitle[512];
    RECT rc;
    utf8_to_wide(title ? title : "Hope", wtitle, 512);
    rc.left = 0;
    rc.top = 0;
    rc.right = width;
    rc.bottom = height;
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    return CreateWindowExW(0, L"HopeWindowClass", wtitle, WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           rc.right - rc.left, rc.bottom - rc.top,
                           NULL, NULL, inst, win);
}

/**
 * @brief 创建平台窗口并绑定回调 / Create a platform window and bind its callbacks
 *
 * @param[in]     title 窗口标题 / Window title
 * @param[in]     width 客户区宽度，最小 64 像素 / Client width, clamped to at least 64 pixels
 * @param[in]     height 客户区高度，最小 64 像素 / Client height, clamped to at least 64 pixels
 * @param[in]     callbacks 窗口回调表，NULL 表示全部为空 / Callback table, NULL means all callbacks are empty
 * @param[in,out] user 透传给每个回调的用户数据 / User data passed to every callback
 *
 * @return 新窗口对象，失败返回 NULL / The new window object, NULL on failure
 *
 * @note 先调用 `SetProcessDPIAware`，随后显示窗口；若设置了 `on_tick` 则启动 15 毫秒定时器 / Calls `SetProcessDPIAware`, shows the window, and starts a 15 ms timer when `on_tick` is set
 *
 * @see hpe_window_destroy hpe_window_run
 */
HpeWindow *hpe_window_create(const char *title, int width, int height,
                             const HpeWindowCallbacks *callbacks, void *user)
{
    HpeWindow *win;
    HINSTANCE inst = GetModuleHandleW(NULL);

    if (width < 64)
        width = 64;
    if (height < 64)
        height = 64;

    SetProcessDPIAware();
    if (!hope_register_class(inst))
        return NULL;

    win = (HpeWindow *)hpe_calloc(1, sizeof(HpeWindow));
    win->cb = callbacks ? *callbacks : (HpeWindowCallbacks){0};
    win->user = user;
    win->width = width;
    win->height = height;

    win->hwnd = hope_create_hwnd(inst, title, width, height, win);
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

/**
 * @brief 提交一帧 BGRA 像素并请求重绘 / Present one BGRA frame and request a repaint
 *
 * @param[in,out] win 目标窗口，NULL 时直接返回 / Target window, returns early when NULL
 * @param[in]     bgra_pixels BGRA32 像素数据 / BGRA32 pixel data
 * @param[in]     width 像素宽度 / Pixel width
 * @param[in]     height 像素高度 / Pixel height
 * @param[in]     stride_bytes 每行字节数 / Bytes per row
 *
 * @note 只登记缓冲与尺寸并调用 `InvalidateRect`，真正的拷贝发生在 `WM_PAINT` / Stores the buffer and size and calls `InvalidateRect`; the actual copy happens in `WM_PAINT`
 *
 * @see hope_blit
 */
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

/**
 * @brief 设置窗口标题栏文本 / Set the window caption text
 *
 * @param[in,out] win 目标窗口，NULL 或无句柄时直接返回 / Target window, returns early when NULL or without a handle
 * @param[in]     title 新的标题文本（UTF-8）/ New caption text in UTF-8
 *
 * @see hpe_window_create
 */
void hpe_window_set_title(HpeWindow *win, const char *title)
{
    wchar_t wtitle[512];
    if (!win || !win->hwnd)
        return;
    utf8_to_wide(title, wtitle, 512);
    SetWindowTextW(win->hwnd, wtitle);
}

/**
 * @brief 请求重绘整个客户区 / Request a repaint of the whole client area
 *
 * @param[in,out] win 目标窗口，NULL 或无句柄时直接返回 / Target window, returns early when NULL or without a handle
 *
 * @see hpe_window_present
 */
void hpe_window_invalidate(HpeWindow *win)
{
    if (win && win->hwnd)
        InvalidateRect(win->hwnd, NULL, FALSE);
}

/**
 * @brief 异步请求关闭窗口 / Ask the window to close asynchronously
 *
 * @param[in,out] win 目标窗口，NULL 或无句柄时直接返回 / Target window, returns early when NULL or without a handle
 *
 * @note 投递 `WM_CLOSE`，消息循环随后退出 / Posts `WM_CLOSE`; the message loop then exits
 *
 * @see hpe_window_run
 */
void hpe_window_request_close(HpeWindow *win)
{
    if (win && win->hwnd)
        PostMessageW(win->hwnd, WM_CLOSE, 0, 0);
}

/**
 * @brief 读取当前修饰键状态 / Read the current modifier-key state
 *
 * @return `HPE_MOD_CTRL/SHIFT/ALT` 的位掩码 / Bit mask of `HPE_MOD_CTRL/SHIFT/ALT`
 *
 * @note 用 `GetKeyState` 的高位判断，供输入消息处理时抓取即时状态 / Tests the high bit of `GetKeyState` to capture the state at input-message time
 */
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

/**
 * @brief 把已登记的像素缓冲拷到窗口 / Copy the registered pixel buffer to the window
 *
 * @param[in,out] win 目标窗口，NULL 或无句柄时直接返回 / Target window, returns early when NULL or without a handle
 *
 * @note 自顶向下的 32 位 DIB，用 `StretchDIBits` 只画脏矩形；无像素时填黑 / A top-down 32-bit DIB painted with `StretchDIBits` over the damaged rect; fills black when there are no pixels
 *
 * @see hpe_window_present
 */
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

/**
 * @brief 把平台光标 ID 映射为系统光标句柄 / Map a platform cursor id onto a system cursor handle
 *
 * @param[in]     cursor `HPE_PLAT_CURSOR_*` ID / One of the `HPE_PLAT_CURSOR_*` ids
 *
 * @return 共享系统光标句柄 / A shared system cursor handle
 *
 * @note 未知 ID 回退为箭头 / Unknown ids fall back to the arrow
 *
 * @see hope_wnd_dispatch
 */
static HCURSOR hope_cursor_handle(int cursor)
{
    switch (cursor) {
    case HPE_PLAT_CURSOR_HAND:
        return LoadCursorW(NULL, (LPCWSTR)IDC_HAND);
    case HPE_PLAT_CURSOR_IBEAM:
        return LoadCursorW(NULL, (LPCWSTR)IDC_IBEAM);
    case HPE_PLAT_CURSOR_MOVE:
        return LoadCursorW(NULL, (LPCWSTR)IDC_SIZEALL);
    case HPE_PLAT_CURSOR_WAIT:
        return LoadCursorW(NULL, (LPCWSTR)IDC_WAIT);
    case HPE_PLAT_CURSOR_HELP:
        return LoadCursorW(NULL, (LPCWSTR)IDC_HELP);
    case HPE_PLAT_CURSOR_CROSS:
        return LoadCursorW(NULL, (LPCWSTR)IDC_CROSS);
    case HPE_PLAT_CURSOR_NO:
        return LoadCursorW(NULL, (LPCWSTR)IDC_NO);
    case HPE_PLAT_CURSOR_SIZEWE:
        return LoadCursorW(NULL, (LPCWSTR)IDC_SIZEWE);
    case HPE_PLAT_CURSOR_SIZENS:
        return LoadCursorW(NULL, (LPCWSTR)IDC_SIZENS);
    default:
        return LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    }
}

/**
 * @brief 处理鼠标消息并派发 `on_mouse` / Handle a mouse message and dispatch `on_mouse`
 *
 * @param[in,out] win 窗口对象 / Window object
 * @param[in]     hwnd 窗口句柄，用于捕获指针 / Window handle used for pointer capture
 * @param[in]     lparam 消息附加参数，低／高 16 位为客户端 x、y / Message LParam whose low/high words are client x and y
 * @param[in]     button 鼠标按键（0 左键，-1 表示纯移动）/ Mouse button (0 = left, -1 = bare move)
 * @param[in]     down 按下为 1、抬起为 0 / 1 when pressed, 0 when released
 *
 * @return 固定返回 0 / Always returns 0
 *
 * @note 左键按下时 `SetCapture`、抬起时 `ReleaseCapture`，使拖选能走出窗口 / Calls `SetCapture` on left-down and `ReleaseCapture` on left-up so a drag can leave the window
 */
static LRESULT hope_on_mouse(HpeWindow *win, HWND hwnd, LPARAM lparam, int button, int down)
{
    if (button == 0 && down)
        SetCapture(hwnd); /* keep tracking the drag outside the window */
    else if (button == 0 && !down && GetCapture() == hwnd)
        ReleaseCapture();
    if (win->cb.on_mouse)
        win->cb.on_mouse(win->user, (int)(short)LOWORD(lparam),
                         (int)(short)HIWORD(lparam), button, down, hope_mods());
    return 0;
}

/**
 * @brief 把已输入的字符派发给 `on_char` / Dispatch a typed character to `on_char`
 *
 * @param[in,out] win 窗口对象 / Window object
 * @param[in]     wparam 消息主参数，即 UTF-16 码元 / Message WParam, the UTF-16 code unit
 *
 * @return 固定返回 0 / Always returns 0
 *
 * @note 跳过控制字符（按 VK 处理）与代理项，只上报基本平面码点 / Skips control characters (handled as VK_*) and surrogates, reporting only BMP code points
 */
static LRESULT hope_on_char(HpeWindow *win, WPARAM wparam)
{
    unsigned int ch = (unsigned int)wparam;
    /* skip control chars (handled as VK_*) and UTF-16 surrogates */
    if (win->cb.on_char && ch >= 32 && (ch < 0xD800 || ch > 0xDFFF))
        win->cb.on_char(win->user, (int)ch);
    return 0;
}

/**
 * @brief 从输入法上下文取一个 UTF-16 组合串并转为 UTF-8 / Read one UTF-16 IME string and convert it to UTF-8
 *
 * @param[in]     imc 输入法上下文 / The input method context
 * @param[in]     index 串类型（GCS_COMPSTR 或 GCS_RESULTSTR）/ String kind (GCS_COMPSTR or GCS_RESULTSTR)
 * @param[out]    utf8 接收 UTF-8 结果 / Receives the UTF-8 string
 * @param[in]     n 缓冲字节数 / Buffer size in bytes
 *
 * @return 成功返回 1，空串或失败返回 0 / 1 on success, 0 when empty or on failure
 *
 * @note 两步查询（先大小后内容）；UTF-16 按 MultiByteToWideChar 转换 / Two-step query (size then contents); UTF-16 is converted with MultiByteToWideChar
 */
static int ime_get_string_w(HIMC imc, DWORD index, char *utf8, size_t n)
{
    LONG wlen = ImmGetCompositionStringW(imc, index, NULL, 0);
    wchar_t *w;
    int ok = 0;
    if (wlen <= 0)
        return 0;
    w = (wchar_t *)hpe_alloc((size_t)wlen + sizeof(wchar_t));
    if (!w)
        return 0;
    if (ImmGetCompositionStringW(imc, index, w, wlen) >= 0) {
        int cch = wlen / (LONG)sizeof(wchar_t);
        int bytes = WideCharToMultiByte(CP_UTF8, 0, w, cch, NULL, 0, NULL, NULL);
        if (bytes > 0 && (size_t)bytes < n) {
            char *tmp = (char *)hpe_alloc((size_t)bytes + 1);
            if (tmp) {
                if (WideCharToMultiByte(CP_UTF8, 0, w, cch, tmp, bytes, NULL, NULL) > 0) {
                    tmp[bytes] = '\0';
                    snprintf(utf8, n, "%s", tmp);
                    ok = 1;
                }
                hpe_free(tmp);
            }
        }
    }
    hpe_free(w);
    return ok;
}

/**
 * @brief 处理输入法组合消息 / Handle the IME composition message
 *
 * @param[in,out] win 目标窗口 / Target window
 * @param[in]     hwnd 窗口句柄 / Window handle
 * @param[in]     lparam 消息附加参数（GCS_* 位掩码）/ Message lparam (GCS_* bit mask)
 *
 * @return 消息处理结果，恒为 0 / The message result, always 0
 *
 * @note GCS_COMPSTR 更新内联组合显示，GCS_RESULTSTR 提交最终串；返回 0 吃掉默认路径，避免 IME 再发 WM_CHAR 造成重复插入 / GCS_COMPSTR updates the inline composition display and GCS_RESULTSTR commits the final string; returning 0 eats the default path so the IME does not also emit WM_CHAR (which would insert twice)
 */
static LRESULT hope_on_ime_composition(HpeWindow *win, HWND hwnd, LPARAM lparam)
{
    HIMC imc = ImmGetContext(hwnd);
    char utf8[1024];
    if (!imc)
        return 0;
    if ((lparam & GCS_COMPSTR) && win->cb.on_ime_compose) {
        if (ime_get_string_w(imc, GCS_COMPSTR, utf8, sizeof(utf8)))
            win->cb.on_ime_compose(win->user, utf8);
        else
            win->cb.on_ime_compose(win->user, "");
    }
    if ((lparam & GCS_RESULTSTR) && win->cb.on_ime_commit) {
        if (ime_get_string_w(imc, GCS_RESULTSTR, utf8, sizeof(utf8)))
            win->cb.on_ime_commit(win->user, utf8);
    }
    ImmReleaseContext(hwnd, imc);
    return 0;
}

/**
 * @brief 处理 `WM_SETCURSOR` 消息 / Handle a `WM_SETCURSOR` message
 *
 * @param[in,out] win 窗口对象 / Window object
 * @param[in]     hwnd 窗口句柄 / Window handle
 * @param[in]     msg 窗口消息编号 / Window message identifier
 * @param[in]     wparam 消息主参数 / Message WParam
 * @param[in]     lparam 消息附加参数，低字为命中测试结果 / Message LParam whose low word is the hit-test result
 *
 * @return 客户区内已设置光标返回 `TRUE`，否则交给 `DefWindowProcW` / `TRUE` once the cursor is set inside the client area, otherwise `DefWindowProcW`
 *
 * @note 只有客户区归我们管：经 `query_cursor` 回调解析引擎的 CSS 光标并设置；窗口边框区域交回系统处理 / Only the client area is ours: the `query_cursor` callback resolves the engine's CSS cursor and it is applied; the frame area falls back to the system
 *
 * @see hope_cursor_handle hope_wnd_dispatch
 */
static LRESULT hope_on_set_cursor(HpeWindow *win, HWND hwnd, UINT msg, WPARAM wparam,
                                  LPARAM lparam)
{
    if (LOWORD(lparam) == HTCLIENT && win->cb.query_cursor) {
        SetCursor(hope_cursor_handle(win->cb.query_cursor(win->user)));
        return TRUE;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

/**
 * @brief 处理输入法生命周期消息 / Handle the IME lifecycle messages
 *
 * @param[in,out] win 窗口对象 / Window object
 * @param[in]     hwnd 窗口句柄 / Window handle
 * @param[in]     msg 窗口消息编号（START/COMPOSITION/END）/ Message id (START/COMPOSITION/END)
 * @param[in]     lparam 消息附加参数，`WM_IME_COMPOSITION` 时为 GCS_* 位掩码 / Message lparam, the GCS_* mask for `WM_IME_COMPOSITION`
 *
 * @return 恒为 0（吃掉默认路径，避免 IME 再发 WM_CHAR 造成重复插入）/ Always 0 (the default path is eaten so the IME does not also emit WM_CHAR, which would insert twice)
 *
 * @note START/END 清空组合显示，COMPOSITION 分别更新组合串与提交最终串 / START/END clear the composition display; COMPOSITION updates the in-progress string and commits the final one
 *
 * @see hope_on_ime_composition hope_wnd_dispatch
 */
static LRESULT hope_wnd_ime(HpeWindow *win, HWND hwnd, UINT msg, LPARAM lparam)
{
    switch (msg) {
    case WM_IME_STARTCOMPOSITION:
        if (win->cb.on_ime_compose)
            win->cb.on_ime_compose(win->user, "");
        return 0;
    case WM_IME_COMPOSITION:
        return hope_on_ime_composition(win, hwnd, lparam);
    default: /* WM_IME_ENDCOMPOSITION: a cancel or an end the result already reported */
        if (win->cb.on_ime_compose)
            win->cb.on_ime_compose(win->user, "");
        return 0;
    }
}

/**
 * @brief 把输入类消息映射到回调 / Map input messages onto engine callbacks
 *
 * @param[in,out] win 窗口对象 / Window object
 * @param[in]     hwnd 窗口句柄 / Window handle
 * @param[in]     msg 窗口消息编号 / Window message identifier
 * @param[in]     wparam 消息主参数 / Message WParam
 * @param[in]     lparam 消息附加参数 / Message LParam
 *
 * @return 处理结果 / The message result
 *
 * @note 覆盖按键、字符、输入法、鼠标与光标消息 / Covers key, char, IME, mouse and cursor messages
 *
 * @see hope_wnd_dispatch
 */
static LRESULT hope_wnd_input(HpeWindow *win, HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_KEYDOWN:
        if (win->cb.on_key)
            win->cb.on_key(win->user, (int)wparam, hope_mods());
        return 0;
    case WM_KEYUP:
        if (win->cb.on_key_up)
            win->cb.on_key_up(win->user, (int)wparam, hope_mods());
        return 0;
    case WM_CHAR:
        return hope_on_char(win, wparam);
    case WM_IME_STARTCOMPOSITION:
    case WM_IME_COMPOSITION:
    case WM_IME_ENDCOMPOSITION:
        return hope_wnd_ime(win, hwnd, msg, lparam);
    case WM_LBUTTONDOWN:
        return hope_on_mouse(win, hwnd, lparam, 0, 1);
    case WM_LBUTTONUP:
        return hope_on_mouse(win, hwnd, lparam, 0, 0);
    case WM_MOUSEMOVE:
        /* button = -1 signals a pointer move (no button state change) */
        return hope_on_mouse(win, hwnd, lparam, -1, 0);
    case WM_SETCURSOR:
        return hope_on_set_cursor(win, hwnd, msg, wparam, lparam);
    default:
        return -1; /* not an input message */
    }
}

/**
 * @brief 把窗口消息映射到回调 / Map window messages onto engine callbacks
 *
 * @param[in,out] win 窗口对象 / Window object
 * @param[in]     hwnd 窗口句柄 / Window handle
 * @param[in]     msg 窗口消息编号 / Window message identifier
 * @param[in]     wparam 消息主参数 / Message WParam
 * @param[in]     lparam 消息附加参数 / Message LParam
 *
 * @return 处理结果为 0，`WM_ERASEBKGND` 返回 1，其余交给 `DefWindowProcW` / 0 for handled messages, 1 for `WM_ERASEBKGND`, otherwise `DefWindowProcW`
 *
 * @note 处理绘制、尺寸、滚轮、按键、字符、鼠标与定时器消息 / Handles paint, size, wheel, key, char, mouse and timer messages
 *
 * @see hope_wnd_proc
 */
static LRESULT hope_wnd_dispatch(HpeWindow *win, HWND hwnd, UINT msg, WPARAM wparam,
                                 LPARAM lparam)
{
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
    case WM_MOUSEWHEEL:
        if (win->cb.on_scroll)
            win->cb.on_scroll(win->user, -(float)GET_WHEEL_DELTA_WPARAM(wparam) /
                                           (float)WHEEL_DELTA * 3.0f);
        return 0;
    case WM_TIMER:
        if (wparam == 1 && win->cb.on_tick)
            win->cb.on_tick(win->user);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default: {
        LRESULT r = hope_wnd_input(win, hwnd, msg, wparam, lparam);
        if (r >= 0)
            return r;
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    }
}

/**
 * @brief 共享窗口类的主窗口过程 / Main window procedure of the shared window class
 *
 * @param[in]     hwnd 窗口句柄 / Window handle
 * @param[in]     msg 窗口消息编号 / Window message identifier
 * @param[in]     wparam 消息主参数 / Message WParam
 * @param[in]     lparam 消息附加参数 / Message LParam
 *
 * @return 消息处理结果 / The message handling result
 *
 * @note `WM_CREATE` 时把创建参数存入 `GWLP_USERDATA`；未找到窗口对象则交回 `DefWindowProcW` / Stores the creation parameter in `GWLP_USERDATA` on `WM_CREATE`; falls back to `DefWindowProcW` when no window object is found
 *
 * @see hope_wnd_dispatch
 */
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
    return hope_wnd_dispatch(win, hwnd, msg, wparam, lparam);
}

/**
 * @brief 进入消息循环直到窗口关闭 / Run the message loop until the window closes
 *
 * @param[in,out] win 目标窗口，NULL 时直接返回 1 / Target window, returns 1 immediately when NULL
 *
 * @return `WM_QUIT` 携带的退出码 / The exit code carried by `WM_QUIT`
 *
 * @note 阻塞式循环，`GetMessageW` 返回 0 时结束 / A blocking loop that ends when `GetMessageW` returns 0
 *
 * @see hpe_window_request_close
 */
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

/**
 * @brief 销毁窗口并释放窗口对象 / Destroy the window and free the window object
 *
 * @param[in,out] win 目标窗口，NULL 时直接返回 / Target window, returns early when NULL
 *
 * @note 先停掉定时器再 `DestroyWindow`，最后释放结构体 / Kills the timer, calls `DestroyWindow`, then frees the struct
 *
 * @see hpe_window_create
 */
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

/**
 * @brief 读取单调时钟秒数 / Read monotonic seconds
 *
 * @return 自某个未指定起点起的单调秒数 / Monotonic seconds since an unspecified origin
 *
 * @note 基于 `QueryPerformanceCounter`，频率只查询一次并缓存 / Uses `QueryPerformanceCounter`; the frequency is queried once and cached
 */

double hpe_time_seconds(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)freq.QuadPart;
}

/**
 * @brief 读取自 Unix 纪元起的 UTC 墙钟毫秒 / Read UTC wall-clock milliseconds since the Unix epoch
 *
 * @return 自 1970-01-01 UTC 起的毫秒数 / Milliseconds since 1970-01-01 UTC
 *
 * @note 由 `FILETIME`（自 1601 起的 100 纳秒刻度）换算到 Unix 纪元 / Derived from `FILETIME` (100 ns ticks since 1601) shifted to the Unix epoch
 *
 * @see hpe_time_seconds
 */
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

/**
 * @brief 把 UTF-16 时区名转成 UTF-8 / Convert a UTF-16 timezone name to UTF-8
 *
 * @param[in]     w UTF-16 源文本 / Source UTF-16 text
 * @param[out]    out 接收 UTF-8 的缓冲 / Receives the UTF-8 text
 * @param[in]     cap 缓冲容量（字节）/ Buffer capacity in bytes
 *
 * @note 转换失败时写入空串 / Writes an empty string when the conversion fails
 */
static void tz_name_utf8(const WCHAR *w, char *out, int cap)
{
    if (!out || cap <= 0)
        return;
    out[0] = '\0';
    if (w && w[0])
        WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL);
}

/**
 * @brief 求某组时区规则下 UTC 时刻的偏移分钟 / Offset in minutes for a UTC instant under one rule set
 *
 * @param[in]     tzi 时区规则 / The timezone rules
 * @param[in]     utc 待换算的 UTC 时刻 / The UTC instant to convert
 * @param[out]    out_dst 接收夏令时标志，可为 `NULL` / Receives the daylight flag, may be `NULL`
 *
 * @return 本地时间相对 UTC 的偏移分钟；换算失败返回 0 / Offset in minutes, or 0 when the conversion fails
 *
 * @note 由 `SystemTimeToTzSpecificLocalTime` 换算后回推差值，因此自动包含该年的夏令时规则 / The difference is derived after `SystemTimeToTzSpecificLocalTime`, so that year's daylight rule is accounted for automatically
 */
static int tz_offset_for(const TIME_ZONE_INFORMATION *tzi, const SYSTEMTIME *utc, int *out_dst)
{
    SYSTEMTIME loc;
    FILETIME fa, fb;
    ULARGE_INTEGER ua, ub;
    double diff;
    if (!SystemTimeToTzSpecificLocalTime(tzi, utc, &loc))
        return 0;
    SystemTimeToFileTime(utc, &fa);
    SystemTimeToFileTime(&loc, &fb);
    ua.LowPart = fa.dwLowDateTime;
    ua.HighPart = fa.dwHighDateTime;
    ub.LowPart = fb.dwLowDateTime;
    ub.HighPart = fb.dwHighDateTime;
    diff = ((double)ub.QuadPart - (double)ua.QuadPart) / 600000000.0;
    if (out_dst)
        *out_dst = tzi->DaylightDate.wMonth != 0 && loc.wHour != utc->wHour;
    return (int)(diff >= 0 ? diff + 0.5 : diff - 0.5);
}

/**
 * @brief 求某个 UTC 时刻的系统时区偏移 / Resolve the system timezone offset at a UTC instant
 *
 * @param[in]     epoch_ms 自 Unix 纪元起的 UTC 毫秒 / UTC milliseconds since the Unix epoch
 * @param[out]    out_name 接收时区名的缓冲，可为 `NULL` / Receives the timezone name, may be `NULL`
 * @param[in]     cap `out_name` 的容量（字节，UTF-8）/ Capacity of `out_name` in bytes, UTF-8
 * @param[out]    out_dst 接收是否为夏令时的标志，可为 `NULL` / Receives the daylight-saving flag, may be `NULL`
 *
 * @return 本地时间相对 UTC 的偏移分钟数（东为正）/ Offset in minutes, positive east
 *
 * @note 用 `GetTimeZoneInformationForYear` 取**该年份生效**的规则，因此夏令时起止日期随年份变化；该 API 不可用时退回当前规则 / `GetTimeZoneInformationForYear` supplies the rules **in force for that year**, so DST boundaries track the year; the current rules are used when that call is unavailable
 */
int hpe_time_utc_offset_minutes(double epoch_ms, char *out_name, size_t cap, int *out_dst)
{
    DYNAMIC_TIME_ZONE_INFORMATION dyn;
    TIME_ZONE_INFORMATION tzi;
    ULARGE_INTEGER u;
    SYSTEMTIME utc;
    int ok;

    ZeroMemory(&dyn, sizeof(dyn));
    GetDynamicTimeZoneInformation(&dyn);
    u.QuadPart = (unsigned long long)(epoch_ms + 11644473600000.0) * 10000ULL;
    ZeroMemory(&utc, sizeof(utc));
    ok = FileTimeToSystemTime((FILETIME *)&u, &utc) != 0;
    ZeroMemory(&tzi, sizeof(tzi));
    if (!ok)
        GetTimeZoneInformation(&tzi);
    else if (!GetTimeZoneInformationForYear(utc.wYear, &dyn, &tzi))
        GetTimeZoneInformation(&tzi);
    if (out_name)
        tz_name_utf8(out_dst ? tzi.DaylightName : tzi.StandardName, out_name, (int)cap);
    if (out_dst)
        *out_dst = 0;
    return tz_offset_for(&tzi, &utc, out_dst);
}

/* ---------------- coroutines ---------------- */

/**
 * @brief 协程句柄 / Coroutine handle
 *
 * @note Win32 用 Fiber 实现：入口跑在自己的栈上，`SwitchToFiber` 即挂起/恢复，
 *       因此 `yield` 处冻结的 C 调用栈可以原样续跑。
 *       Backed by a Win32 Fiber: the entry runs on its own stack and `SwitchToFiber` does the
 *       suspend/resume, so the C call stack frozen at a `yield` resumes exactly where it stopped.
 */
struct HpeCoro {
    void *fiber;      /* the coroutine's own fiber / 协程自身的 fiber */
    void *caller;     /* fiber to switch back to on yield / yield 时切回的 fiber */
    void (*entry)(void *);
    void *user;
    int finished;
    int started;
    void *stack_lo;   /* fiber stack bounds, captured on first yield / 首次 yield 时捕获的协程栈边界 */
    void *stack_hi;
};

/**
 * @brief Fiber 入口：先调用用户入口，再把 finished 置位并交还控制权 / Fiber entry: run the user entry, mark it finished, hand control back
 *
 * @param[in,out] ud 指向 `HpeCoro` / Points at the `HpeCoro`
 *
 * @note 用户入口返回后不再切回协程体，所以**末尾那次** `SwitchToFiber` 是必要的，否则 `resume` 会永远等下去 / After the user entry returns the fiber must still switch back once, or `resume` would wait forever
 */
static void CALLBACK coro_trampoline(void *ud)
{
    HpeCoro *c = (HpeCoro *)ud;
    c->entry(c->user);
    c->finished = 1;
    SwitchToFiber(c->caller);
}

/**
 * @brief 读取当前运行中的 fiber 句柄 / Read the handle of the currently running fiber
 *
 * @return 当前 fiber 句柄 / The current fiber handle
 *
 * @note 等价于 `GetCurrentFiber`（TEB + 0x20 的 FiberData），但用内联汇编直读
 *       TEB，避免 mingw SDK 的 `__readgsqword` 内联实现触发 -Warray-bounds 噪声
 *       / Same as `GetCurrentFiber` (FiberData at TEB + 0x20), but reads the TEB
 *       with inline assembly so the mingw SDK's `__readgsqword` inlining cannot
 *       emit -Warray-bounds noise
 */
static void *coro_self(void)
{
    void **teb;
#if defined(_WIN64)
    __asm__ volatile("movq %%gs:0x30, %0" : "=r"(teb));
#else
    __asm__ volatile("movl %%fs:0x18, %0" : "=r"(teb));
#endif
    return teb[4]; /* TEB.FiberData (8-byte pointers: +0x20 = index 4) */
}

/**
 * @brief 在当前线程上创建一个协程 / Create a coroutine on the current thread
 *
 * @param[in]     entry 协程入口函数 / Coroutine entry point
 * @param[in,out] user 透传给入口函数的指针 / Pointer forwarded to the entry function
 * @param[in]     stack_size 栈字节数，0 表示默认 / Stack size in bytes, 0 for the default
 *
 * @return 新协程；失败返回 `NULL` / The new coroutine, or `NULL` on failure
 *
 * @note 线程首次调用时会先 `ConvertThreadToFiber`，使其自身也能作为 `caller` / The thread is converted to a fiber on first use so it can act as the `caller` side
 */
HpeCoro *hpe_coro_create(void (*entry)(void *), void *user, size_t stack_size)
{
    HpeCoro *c = (HpeCoro *)calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    if (!IsThreadAFiber() && !ConvertThreadToFiber(NULL)) {
        free(c);
        return NULL;
    }
    c->entry = entry;
    c->user = user;
    c->fiber = CreateFiber(stack_size ? stack_size : 0, coro_trampoline, c);
    if (!c->fiber) {
        free(c);
        return NULL;
    }
    return c;
}

/**
 * @brief 把控制权交给协程，直到它 `yield` 或结束 / Hand control to a coroutine until it yields or finishes
 *
 * @param[in,out] coro 目标协程 / Target coroutine
 *
 * @return 协程仍在运行返回 1；已结束返回 0 / 1 when still live, 0 when finished
 *
 * @note `caller` 每次进入前重设，因此嵌套协程（生成器里再开生成器）也能回到正确的调用者 / `caller` is re-read on entry so nested coroutines return to the right frame
 */
int hpe_coro_resume(HpeCoro *coro)
{
    if (!coro || coro->finished)
        return 0;
    coro->caller = coro_self();
    coro->started = 1;
    SwitchToFiber(coro->fiber);
    return !coro->finished;
}

/**
 * @brief 从协程内部把控制权交还调用者 / Yield control back to the resumer from inside a coroutine
 *
 * @param[in,out] coro 当前协程 / The current coroutine
 */
void hpe_coro_yield(HpeCoro *coro)
{
    if (coro && coro->caller) {
        /* capture the fiber's stack bounds once, while running on it, so the
         * GC can conservatively scan suspended coroutine stacks later. The
         * TEB pointer is read through the gs/fs segment register (offset
         * 0x30 on x64, 0x18 on x86); the TIB's StackBase and StackLimit then
         * sit at +8 and +16 bytes in both pointer widths. */
        if (!coro->stack_hi) {
            void **teb;
#if defined(_WIN64)
            __asm__ volatile("movq %%gs:0x30, %0" : "=r"(teb));
#else
            __asm__ volatile("movl %%fs:0x18, %0" : "=r"(teb));
#endif
            coro->stack_hi = teb[1]; /* NtTib.StackBase */
            coro->stack_lo = teb[2]; /* NtTib.StackLimit */
        }
        SwitchToFiber(coro->caller);
    }
}

/**
 * @brief 查询协程栈的边界（供 GC 保守扫描挂起协程）/ Query a coroutine's stack bounds (for GC scanning of suspended coroutines)
 *
 * @param[in]  coro 协程句柄 / Coroutine handle
 * @param[out] lo 栈低端地址（含）/ Low address of the stack (inclusive)
 * @param[out] hi 栈高端地址（不含）/ High address of the stack (exclusive)
 *
 * @return 边界可用返回 1；协程为空或从未挂起过返回 0（此时栈上没有活动帧）/ 1 when bounds are known; 0 when the coroutine is NULL or has never yielded (its stack holds no live frames yet)
 */
int hpe_coro_stack_range(HpeCoro *coro, void **lo, void **hi)
{
    if (!coro || !coro->stack_hi)
        return 0;
    *lo = coro->stack_lo;
    *hi = coro->stack_hi;
    return 1;
}

/**
 * @brief 销毁协程并释放其栈 / Destroy a coroutine and release its stack
 *
 * @param[in,out] coro 目标协程；`NULL` 时无操作 / Target coroutine; a `NULL` argument is a no-op
 *
 * @note `DeleteFiber` 会直接丢弃协程栈而不展开任何 C 帧，这与 JS 里「未跑完的生成器被回收」
 *       的语义一致；它只能从**别的** fiber 上对挂起的协程调用，调用方须保证这一点。
 *       `DeleteFiber` discards the coroutine stack without unwinding any C frame, which matches
 *       how JS reclaims an unfinished generator; it must only be called from a different fiber,
 *       which the caller is responsible for.
 */
void hpe_coro_destroy(HpeCoro *coro)
{
    if (!coro)
        return;
    if (coro->fiber && coro->fiber != coro_self())
        DeleteFiber(coro->fiber);
    free(coro);
}

/**
 * @brief 读取整个文件到新分配的缓冲 / Read a whole file into a freshly allocated buffer
 *
 * @param[in]     path 文件路径 / File path
 * @param[out]    out_size 接收读入的字节数，可为 NULL / Receives the byte count read, may be NULL
 *
 * @return 以 NUL 结尾的内容缓冲，失败返回 NULL / NUL-terminated contents buffer, NULL on failure
 * @retval  NULL 打开、定位或长度获取失败 / Opening, seeking or sizing failed
 *
 * @note 调用方需用 `hpe_free` 释放；开头的 UTF-8 BOM 会被剥掉 / The caller must `hpe_free` it; a leading UTF-8 BOM is stripped
 */
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

/**
 * @brief 取出路径的目录部分 / Take the directory part of a path
 *
 * @param[in]     path 输入路径 / Input path
 * @param[out]    out 接收目录的缓冲 / Receives the directory
 * @param[in]     out_size 接收缓冲区容量 / Capacity of the output buffer
 *
 * @note 无分隔符时写出 `.`；结果为空时写出 `/` / Writes `.` when there is no separator, and `/` when the result would be empty
 */
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

/**
 * @brief 判断路径是否为绝对路径 / Test whether a path is absolute
 *
 * @param[in]     path 待检查路径 / Path to check
 *
 * @return 绝对路径返回 1，否则 0 / 1 for an absolute path, 0 otherwise
 *
 * @note 识别前导 `/`、`\` 以及盘符形式如 `C:` / Recognises a leading `/`, `\`, or a drive prefix such as `C:`
 */
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

/**
 * @brief 把相对路径接到目录上 / Join a relative path onto a directory
 *
 * @param[out]    out 接收拼接结果的缓冲 / Receives the joined path
 * @param[in]     out_size 接收缓冲区容量 / Capacity of the output buffer
 * @param[in]     dir 基准目录 / Base directory
 * @param[in]     rel 相对路径 / Relative path
 *
 * @note 先剥掉开头的 `./`；若 `rel` 已是绝对路径或 `dir` 为空则只用 `rel` / Strips leading `./`; uses `rel` alone when it is absolute or `dir` is empty
 */
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

/**
 * @brief 把路径解析为绝对路径 / Resolve a path to an absolute one
 *
 * @param[out]    out 接收绝对路径的缓冲 / Receives the absolute path
 * @param[in]     out_size 接收缓冲区容量 / Capacity of the output buffer
 * @param[in]     path 输入路径 / Input path
 *
 * @note 用 `GetFullPathNameA` 相对当前工作目录解析，并把反斜杠统一成正斜杠；失败时原样拷回 / Resolves against the process working directory with `GetFullPathNameA` and normalises backslashes to slashes; copies the input through unchanged on failure
 */
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

/**
 * @brief 把 UTF-16 复制成新分配的 UTF-8 串 / Duplicate UTF-16 into a freshly allocated UTF-8 string
 *
 * @param[in]     w UTF-16 源文本，NULL 时返回 NULL / Source UTF-16 text, returns NULL when NULL
 * @param[in]     len 宽字符个数，`<= 0` 时用 `lstrlenW` 求长 / Wide-character count, measured with `lstrlenW` when `<= 0`
 *
 * @return 调用方负责释放的 UTF-8 副本，失败返回 NULL / UTF-8 copy owned by the caller, NULL on failure
 */

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

/**
 * @brief 取剪贴板文本或首个文件路径 / Read clipboard text or the first file path
 *
 * @return 以 UTF-8 编码的剪贴板内容（需 `hpe_free`），无文本时返回 NULL / UTF-8 clipboard contents (needs `hpe_free`), NULL when there is no text
 *
 * @note 优先读 `CF_UNICODETEXT`；若只有 `CF_HDROP`（资源管理器复制的文件）则返回首个文件路径，便于地址栏使用 / Prefers `CF_UNICODETEXT`; falls back to the first file path in `CF_HDROP` (files copied in Explorer) as an address bar expects
 *
 * @see hpe_clipboard_set_text
 */
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

/**
 * @brief 用 UTF-8 文本替换剪贴板内容 / Replace the clipboard contents with UTF-8 text
 *
 * @param[in]     utf8 待写入的 UTF-8 文本，NULL 视为空串 / UTF-8 text to write, NULL is treated as an empty string
 *
 * @return 成功返回 1，失败返回 0 / 1 on success, 0 on failure
 * @retval     1 已写入 `CF_UNICODETEXT` / `CF_UNICODETEXT` was written
 * @retval     0 转换、分配或打开剪贴板失败 / Conversion, allocation or opening the clipboard failed
 *
 * @note 以 UTF-16 形式写入；失败时释放临时内存（成功则剪贴板接管）/ Writes as UTF-16; frees the temporary memory on failure (on success the clipboard takes ownership)
 *
 * @see hpe_clipboard_get_text
 */
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

/**
 * @brief 把字体族映射为具体字体名 / Map a font family onto a concrete typeface name
 *
 * @param[in]     family 字体族 / Font family
 *
 * @return 字体名的宽字符形式 / The typeface name as a wide string
 *
 * @note mono 取 Consolas、serif 取 Times New Roman，其余取 Microsoft YaHei / Mono uses Consolas, serif uses Times New Roman, everything else Microsoft YaHei
 *
 * @see hpe_plat_font_create
 */
static const wchar_t *face_for_family(HpeFontFamily family)
{
    if (family == HPE_FONT_MONO)
        return L"Consolas";
    if (family == HPE_FONT_SERIF)
        return L"Times New Roman";
    return L"Microsoft YaHei";
}

/**
 * @brief 创建 GDI 字体句柄 / Create a GDI font handle
 *
 * @param[in]     pixel_size 字号（像素），取整并夹在 6..240 / Font size in pixels, rounded and clamped to 6..240
 * @param[in]     weight 字重，`>= 700` 用粗体 / Font weight, uses bold when `>= 700`
 * @param[in]     face 字体名（宽字符） / Typeface name (wide)
 *
 * @return 新字体对象，失败返回 NULL / The new font object, NULL on failure
 *
 * @note 为内存 DC 选入字体并读出上伸部、下伸部与行高 / Selects the font into a memory DC and reads ascent, descent and line height
 *
 * @see hpe_plat_font_destroy
 */
static HpePlatFont *plat_font_create_named(float pixel_size, int weight, const wchar_t *face)
{
    HpePlatFont *font;
    TEXTMETRICW tm;
    int px = (int)(pixel_size + 0.5f);

    if (px < 6)
        px = 6;
    if (px > 240)
        px = 240;

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

/**
 * @brief 创建平台字体对象 / Create a platform font object
 *
 * @param[in]     pixel_size 字号（像素）/ Font size in pixels
 * @param[in]     weight 字重 / Font weight
 * @param[in]     family 字体族 / Font family
 *
 * @return 新字体对象，失败返回 NULL / The new font object, NULL on failure
 *
 * @note 由 `hpe_plat_font_create_face` 按字体族映射出的字体名创建 / Created through `hpe_plat_font_create_face` with the family-mapped name
 *
 * @see hpe_plat_font_destroy
 */
HpePlatFont *hpe_plat_font_create(float pixel_size, int weight, HpeFontFamily family)
{
    return plat_font_create_named(pixel_size, weight, face_for_family(family));
}

/**
 * @brief 用显式字体名创建平台字体对象 / Create a platform font object with an explicit face name
 *
 * @param[in]     pixel_size 字号（像素）/ Font size in pixels
 * @param[in]     weight 字重 / Font weight
 * @param[in]     face_utf8 字体名（UTF-8），NULL 时按字体族映射 / Typeface name in UTF-8, NULL maps by family
 *
 * @return 新字体对象，失败返回 NULL / The new font object, NULL on failure
 *
 * @note 字体不存在时 GDI 会静默替换，调用方需用 `hpe_plat_font_covers` 验证覆盖 / GDI silently substitutes a missing face, so callers should verify coverage with `hpe_plat_font_covers`
 *
 * @see hpe_plat_font_destroy
 */
HpePlatFont *hpe_plat_font_create_face(float pixel_size, int weight, const char *face_utf8)
{
    wchar_t wface[64];
    if (!face_utf8 || !face_utf8[0])
        return plat_font_create_named(pixel_size, weight, face_for_family(HPE_FONT_SANS));
    utf8_to_wide(face_utf8, wface, 64);
    return plat_font_create_named(pixel_size, weight, wface);
}

/**
 * @brief 判断字体是否覆盖某个码点 / Test whether the font covers a code point
 *
 * @param[in,out] font 字体对象 / Font object
 * @param[in]     codepoint Unicode 码点 / Unicode code point
 *
 * @return 覆盖返回 1，未覆盖或字体为空返回 0 / 1 when covered, 0 when missing or the font is NULL
 *
 * @note 用 `GetGlyphIndicesW` 并带 `GGI_MARK_NONEXISTING_GLYPHS` 标记缺字 / Uses `GetGlyphIndicesW` with `GGI_MARK_NONEXISTING_GLYPHS`
 *
 * @see hpe_plat_font_create
 */
int hpe_plat_font_covers(HpePlatFont *font, uint32_t codepoint)
{
    wchar_t buf[2];
    WORD glyph = 0xFFFF;
    int n;
    if (!font)
        return 0;
    n = cp_to_wchars(codepoint, buf);
    if (GetGlyphIndicesW(font->hdc, buf, n, &glyph, GGI_MARK_NONEXISTING_GLYPHS) == GDI_ERROR)
        return 0;
    return glyph != 0xFFFF;
}

/**
 * @brief 销毁字体对象并释放 GDI 资源 / Destroy a font object and release its GDI resources
 *
 * @param[in,out] font 待销毁的字体，NULL 时直接返回 / Font to destroy, returns early when NULL
 *
 * @note 先恢复原字体再删除 DC 与字体对象 / Restores the previous font before deleting the DC and the font object
 *
 * @see hpe_plat_font_create
 */
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

/**
 * @brief 取字体的上伸部 / Get the font ascent
 *
 * @param[in,out] font 字体对象，NULL 时返回 0 / Font object, returns 0 when NULL
 *
 * @return 上伸部像素数 / Ascent in pixels
 */
float hpe_plat_font_ascent(HpePlatFont *font)
{
    return font ? (float)font->ascent : 0.0f;
}

/**
 * @brief 取字体的下伸部 / Get the font descent
 *
 * @param[in,out] font 字体对象，NULL 时返回 0 / Font object, returns 0 when NULL
 *
 * @return 下伸部像素数 / Descent in pixels
 */
float hpe_plat_font_descent(HpePlatFont *font)
{
    return font ? (float)font->descent : 0.0f;
}

/**
 * @brief 取字体的行高 / Get the font line height
 *
 * @param[in,out] font 字体对象，NULL 时返回 0 / Font object, returns 0 when NULL
 *
 * @return 行高像素数（含外部行距）/ Line height in pixels, including external leading
 */
float hpe_plat_font_line_height(HpePlatFont *font)
{
    return font ? (float)font->line_height : 0.0f;
}

/**
 * @brief 把码点编码为 UTF-16 码元 / Encode a code point into UTF-16 code units
 *
 * @param[in]     cp Unicode 码点 / Unicode code point
 * @param[out]    out 接收 1 或 2 个宽字符的数组 / Receives one or two wide characters
 *
 * @return 写出的宽字符个数 / Number of wide characters written
 * @retval     1 基本平面码点 / A BMP code point
 * @retval     2 补充平面码点，写成代理对 / A supplementary code point written as a surrogate pair
 *
 * @note 调用方需保证 `out` 至少两个元素 / The caller must provide room for two elements
 */
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

/**
 * @brief 取单个码点的步进宽度 / Measure the advance width of a single code point
 *
 * @param[in,out] font 字体对象 / Font object
 * @param[in]     codepoint Unicode 码点 / Unicode code point
 *
 * @return 步进宽度（像素），字体为空或测量失败时为 0 / Advance width in pixels, 0 when the font is NULL or measuring fails
 */
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

/**
 * @brief 构造 1×1 全透明掩码占位 / Build a 1×1 fully transparent placeholder mask
 *
 * @param[out]    out 接收掩码的结构，被就地改写 / Mask struct rewritten in place
 *
 * @return 固定返回 1 / Always returns 1
 *
 * @note 用于空输入或非法输入，避免调用方处理 NULL 掩码 / Used for empty or invalid input so callers never see a NULL mask
 */
static int mask_empty(HpeTextMask *out)
{
    out->width = 1;
    out->height = 1;
    out->coverage = (unsigned char *)hpe_calloc(1, 1);
    return 1;
}

/**
 * @brief 把宽字符串渲染到 32 位 DIB / Render a wide-character run onto a 32-bpp DIB
 *
 * @param[in,out] font 字体对象 / Font object
 * @param[in]     wbuf 待渲染的宽字符 / Wide characters to render
 * @param[in]     wlen 宽字符个数 / Number of wide characters
 * @param[in]     dx 水平亚像素相位（0..1），经世界变换实现 / Horizontal sub-pixel phase (0..1), applied via the world transform
 * @param[in]     w 位图宽度（像素）/ Bitmap width in pixels
 * @param[in]     h 位图高度（像素）/ Bitmap height in pixels
 * @param[out]    out_bm 接收位图句柄，供调用方删除 / Receives the bitmap handle for the caller to delete
 *
 * @return DIB 像素缓冲指针，失败返回 NULL / Pointer to the DIB pixel buffer, NULL on failure
 *
 * @note 白字透明背景绘制，自顶向下；调用方负责 `DeleteObject` 位图句柄 / Draws white text on a transparent background, top-down; the caller owns deleting the bitmap handle
 */
static void *mask_render(HpePlatFont *font, const wchar_t *wbuf, int wlen, float dx, int w, int h,
                         HBITMAP *out_bm)
{
    BITMAPINFO bmi;
    void *bits = NULL;
    HBITMAP hbm;
    HDC memdc;
    HGDIOBJ old_bm, old_font;

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
        return NULL;
    }
    memdc = CreateCompatibleDC(font->hdc);
    if (!memdc) {
        DeleteObject(hbm);
        return NULL;
    }
    old_bm = SelectObject(memdc, hbm);
    old_font = SelectObject(memdc, font->hfont);
    SetBkMode(memdc, TRANSPARENT);
    SetTextColor(memdc, RGB(255, 255, 255));
    SetTextAlign(memdc, TA_LEFT | TA_TOP);
    if (dx > 0.0f && dx < 1.0f) {
        /* sub-pixel phase: GDI honours the world transform when rendering
         * text, so a fractional translate shifts the antialiased glyphs */
        XFORM xf;
        xf.eM11 = 1.0f;
        xf.eM12 = 0.0f;
        xf.eM21 = 0.0f;
        xf.eM22 = 1.0f;
        xf.eDx = dx;
        xf.eDy = 0.0f;
        SetGraphicsMode(memdc, GM_ADVANCED);
        SetWorldTransform(memdc, &xf);
    }
    TextOutW(memdc, 1, 1, wbuf, wlen);
    SelectObject(memdc, old_font);
    SelectObject(memdc, old_bm);
    DeleteDC(memdc);
    *out_bm = hbm;
    return bits;
}

/**
 * @brief 把 32 位像素转成 8 位覆盖率 / Convert 32-bpp pixels into 8-bit coverage
 *
 * @param[in]     bits BGRA32 像素数据 / BGRA32 pixel data
 * @param[in]     count 像素个数 / Number of pixels
 *
 * @return 新分配的覆盖率数组，调用方负责释放 / Newly allocated coverage array owned by the caller
 *
 * @note 每个像素取三通道最大值作为抗锯齿覆盖率 / Takes the maximum of the three colour channels per pixel as the antialiased coverage
 */
static unsigned char *mask_coverage(const void *bits, int count)
{
    unsigned char *cov = (unsigned char *)hpe_alloc((size_t)count);
    int i;
    for (i = 0; i < count; i++) {
        const unsigned char *px = (const unsigned char *)bits + (size_t)i * 4;
        unsigned char v = px[0] > px[1] ? px[0] : px[1];
        if (px[2] > v)
            v = px[2];
        cov[i] = v;
    }
    return cov;
}

/**
 * @brief 把 UTF-8 文本渲染成 8 位覆盖率掩码 / Render a UTF-8 run into an 8-bit coverage mask
 *
 * @param[in,out] font 字体对象 / Font object
 * @param[in]     utf8 待渲染的 UTF-8 文本 / UTF-8 text to render
 * @param[in]     len 文本字节数 / Number of bytes
 * @param[in]     dx 水平亚像素相位（0..1）/ Horizontal sub-pixel phase (0..1)
 * @param[out]    out 接收掩码的结构 / Struct that receives the mask
 *
 * @return 成功返回 1，失败返回 0 / 1 on success, 0 on failure
 * @retval     1 掩码已写出（空输入也会写出 1×1 占位掩码）/ The mask was produced, empty input yields the 1×1 placeholder
 * @retval     0 `GetTextExtentPoint32W` 或 DIB 创建失败 / `GetTextExtentPoint32W` or DIB creation failed
 *
 * @note 覆盖区相对基线：x 从 0 起，y 在 `[-descent, ascent)`；`baseline` 为基线所在行 / Coverage is relative to the baseline: x from 0 and y over `[-descent, ascent)`; `baseline` is the row index of the baseline
 *
 * @see hpe_text_mask_free
 */
int hpe_plat_font_mask(HpePlatFont *font, const char *utf8, int len, float dx, HpeTextMask *out)
{
    wchar_t wbuf[8192];
    int wlen, w, h;
    SIZE sz;
    void *bits;
    HBITMAP hbm = NULL;

    if (!out)
        return 0;
    memset(out, 0, sizeof(*out));
    if (!font || !utf8 || len <= 0)
        return mask_empty(out);

    wlen = utf8_to_wide_n(utf8, len, wbuf, 8191);
    if (wlen <= 0)
        return mask_empty(out);

    if (!GetTextExtentPoint32W(font->hdc, wbuf, wlen, &sz))
        return 0;

    w = sz.cx + 2 + (dx > 0.0f ? 3 : 0);
    h = font->ascent + font->descent + 2;
    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;

    bits = mask_render(font, wbuf, wlen, dx, w, h, &hbm);
    if (!bits)
        return 0;

    out->coverage = mask_coverage(bits, w * h);
    DeleteObject(hbm);
    out->width = w;
    out->height = h;
    out->baseline = 1 + font->ascent;
    return 1;
}

/**
 * @brief 释放文本掩码并清零结构 / Free a text mask and zero the struct
 *
 * @param[in,out] mask 待释放的掩码，NULL 时直接返回 / Mask to free, returns early when NULL
 *
 * @see hpe_plat_font_mask
 */
void hpe_text_mask_free(HpeTextMask *mask)
{
    if (!mask)
        return;
    if (mask->coverage)
        hpe_free(mask->coverage);
    memset(mask, 0, sizeof(*mask));
}

/* ---------------- color emoji (COLRv0 layer outlines) ---------------- */

struct HpePlatEmojiFont {
    HFONT hfont;
    HDC hdc;
    HGDIOBJ old_font;
    int px;
};

/**
 * @brief 打开彩色 emoji 字体（按字号）/ Open the color emoji font at a pixel size
 *
 * @param[in]     pixel_size 字号（像素）/ Font size in pixels
 *
 * @return 新对象，系统无彩色 emoji 字体时返回 NULL / The new object, NULL when the system has no color emoji font
 *
 * @note 选入 `Segoe UI Emoji` 后用 `GetTextFaceW` 验证 GDI 没有替换掉它 / Selects `Segoe UI Emoji` and verifies with `GetTextFaceW` that GDI did not substitute it
 *
 * @see hpe_plat_emoji_font_destroy
 */
HpePlatEmojiFont *hpe_plat_emoji_font_create(float pixel_size)
{
    HpePlatEmojiFont *ef;
    WCHAR face[64];
    int px = (int)(pixel_size + 0.5f);
    if (px < 6)
        px = 6;
    if (px > 240)
        px = 240;
    ef = (HpePlatEmojiFont *)hpe_calloc(1, sizeof(HpePlatEmojiFont));
    ef->px = px;
    ef->hdc = CreateCompatibleDC(NULL);
    if (!ef->hdc) {
        hpe_free(ef);
        return NULL;
    }
    ef->hfont = CreateFontW(-px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Emoji");
    if (!ef->hfont) {
        DeleteDC(ef->hdc);
        hpe_free(ef);
        return NULL;
    }
    ef->old_font = SelectObject(ef->hdc, ef->hfont);
    SetMapMode(ef->hdc, MM_TEXT);
    /* a missing face is silently substituted; verify it really is the emoji font */
    if (GetTextFaceW(ef->hdc, 64, face) == 0 || lstrcmpiW(face, L"Segoe UI Emoji") != 0) {
        hpe_plat_emoji_font_destroy(ef);
        return NULL;
    }
    return ef;
}

/**
 * @brief 按字形索引取抗锯齿覆盖率轮廓 / Fetch an antialiased coverage outline by glyph index
 *
 * @param[in,out] font emoji 字体对象 / Emoji font object
 * @param[in]     glyph_id 字形索引 / Glyph index
 * @param[out]    out 接收轮廓的结构，coverage 由调用方 `hpe_free` / Receives the outline, the coverage is freed by the caller via `hpe_free`
 *
 * @return 成功返回 1，空字形或失败返回 0 / 1 on success, 0 for a blank glyph or failure
 *
 * @note `GGO_GRAY8_BITMAP` 输出 0..64 共 65 级灰度，这里归一到 0..255 / `GGO_GRAY8_BITMAP` yields 65 grey levels in 0..64, normalised here to 0..255
 *
 * @see hpe_plat_emoji_font_create
 */
int hpe_plat_emoji_glyph(HpePlatEmojiFont *font, uint32_t glyph_id, HpeGlyphOutline *out)
{
    GLYPHMETRICS gm;
    /* identity MAT2: each FIXED is {fract, value} / 单位 MAT2：FIXED 为 {小数, 整数} */
    MAT2 mat = {{0, 1}, {0, 0}, {0, 0}, {0, 1}};
    DWORD size;
    unsigned char *buf, *cov;

    if (!out)
        return 0;
    memset(out, 0, sizeof(*out));
    if (!font)
        return 0;
    size = GetGlyphOutlineW(font->hdc, (UINT)glyph_id, GGO_GLYPH_INDEX | GGO_GRAY8_BITMAP, &gm,
                            0, NULL, &mat);
    if (getenv("HPE_TRACE_EMOJI"))
        fprintf(stderr, "glyph: id=%u size=%lu bb=%u err=%lu\n", glyph_id,
                (unsigned long)size, gm.gmBlackBoxX, GetLastError());
    if (size == GDI_ERROR || gm.gmBlackBoxX == 0 || gm.gmBlackBoxY == 0)
        return 0;
    buf = (unsigned char *)hpe_alloc(size);
    if (GetGlyphOutlineW(font->hdc, (UINT)glyph_id, GGO_GLYPH_INDEX | GGO_GRAY8_BITMAP, &gm,
                         size, buf, &mat) == GDI_ERROR) {
        hpe_free(buf);
        return 0;
    }
    cov = (unsigned char *)hpe_alloc((size_t)gm.gmBlackBoxX * gm.gmBlackBoxY);
    {
        /* rows are DWORD-aligned in the GGO buffer / GGO 缓冲每行按 DWORD 对齐 */
        DWORD stride = (gm.gmBlackBoxX + 3u) & ~3u;
        DWORD y, x;
        for (y = 0; y < gm.gmBlackBoxY; y++)
            for (x = 0; x < gm.gmBlackBoxX; x++)
                cov[y * gm.gmBlackBoxX + x] = (unsigned char)(buf[y * stride + x] * 255 / 64);
    }
    hpe_free(buf);
    out->coverage = cov;
    out->w = gm.gmBlackBoxX;
    out->h = gm.gmBlackBoxY;
    out->left = gm.gmptGlyphOrigin.x;
    out->baseline = gm.gmptGlyphOrigin.y; /* rows above the baseline / 基线之上的行数 */
    out->advance = (float)gm.gmCellIncX;
    return 1;
}

/**
 * @brief 关闭彩色 emoji 字体并释放资源 / Close the color emoji font and release its resources
 *
 * @param[in,out] font emoji 字体对象，NULL 时无操作 / Emoji font object, NULL is a no-op
 *
 * @see hpe_plat_emoji_font_create
 */
void hpe_plat_emoji_font_destroy(HpePlatEmojiFont *font)
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
