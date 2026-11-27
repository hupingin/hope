#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/hpe_util.h"
#include "css/hpe_css.h"
#include "hope.h"
#include "js/hpe_js.h"
#include "platform/hpe_platform.h"

#define VK_F5_CODE 0x74
#define VK_ESCAPE_CODE 0x1B

/**
 * @brief 独立运行一个 JS 脚本文件（开发辅助）/ Run a standalone JS script file (development helper)
 *
 * @param[in]     path 脚本文件路径 / Script file path
 * @param[in]     as_module 按 ES 模块解析为 1，否则按脚本 / 1 to parse as an ES module, otherwise as a script
 *
 * @return 成功返回 0，失败返回 1 / 0 on success, 1 on failure
 *
 * @note 新建临时 JS 运行时，把脚本所在目录设为当前目录后求值，最后销毁运行时 / Creates a throwaway JS runtime, sets the script's directory as the current directory, evaluates, then destroys the runtime
 *
 * @see parse_args
 */

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
    hpe_js_set_entry_path(js, as_module ? path : NULL);
    errbuf[0] = '\0';
    rc = as_module ? hpe_js_eval_module(js, src, dir, errbuf, sizeof(errbuf))
                   : hpe_js_eval(js, src, errbuf, sizeof(errbuf));
    if (rc != 0)
        fprintf(stderr, "hope: %s\n", errbuf[0] ? errbuf : "script error");
    hpe_free(src);
    hpe_js_destroy(js);
    return rc != 0;
}

/**
 * @brief 按小端写入 32 位整数 / Write a 32-bit integer little-endian
 *
 * @param[out]    p 接收 4 字节的缓冲 / Buffer that receives the four bytes
 * @param[in]     v 待写入的数值 / Value to write
 *
 * @note 供 BMP 文件头组装使用 / Used when assembling the BMP file header
 *
 * @see write_bmp
 */

static void put_u32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

/**
 * @brief 把像素缓冲写成 32 位 BMP / Write a pixel buffer as a 32-bpp BMP
 *
 * @param[in]     path 输出文件路径 / Output file path
 * @param[in]     pm 源像素缓冲 / Source pixel buffer
 *
 * @return 写入成功返回 1，否则 0 / 1 on success, otherwise 0
 * @retval     1 文件已完整写出 / The file was fully written
 * @retval     0 `pm` 无像素或文件无法打开 / `pm` has no pixels or the file could not be opened
 *
 * @note BMP 行序自下而上，故逐行倒序写出 / BMP rows are stored bottom-up, so rows are written in reverse
 *
 * @see put_u32 run_headless
 */
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
    int mouse_x, mouse_y; /* last pointer position in window coords (wheel targeting) */
} App;

/**
 * @brief 把引擎状态同步到窗口标题 / Sync the engine status into the window title
 *
 * @param[in,out] app 应用状态 / Application state
 *
 * @note 标题形如 `Hope - <状态>`，其中状态行承载加载错误等信息 / The title reads `Hope - <status>`, the status line carrying load errors and the like
 *
 * @see hpe_engine_status
 */
static void app_refresh_title(App *app)
{
    char title[768];
    snprintf(title, sizeof(title), "Hope - %s", hpe_engine_status(app->engine));
    hpe_window_set_title(app->window, title);
}

/**
 * @brief 绘制一帧并提交给窗口 / Render a frame and present it to the window
 *
 * @param[in,out] user 宿主回调透传的用户数据，此处为 `App *` / User data passed through by the host, an `App *` here
 *
 * @note 渲染引擎、取像素缓冲提交，再刷新标题；无窗口时直接返回 / Renders the engine, presents its pixmap, then refreshes the title; returns early without a window
 *
 * @see app_refresh_title hpe_window_present
 */
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

/**
 * @brief 响应窗口尺寸变化 / Handle a window resize
 *
 * @param[in,out] user `App *` 用户数据 / The `App *` user data
 * @param[in]     width 新的客户区宽度 / New client width
 * @param[in]     height 新的客户区高度 / New client height
 *
 * @note 调整引擎尺寸后请求重绘 / Resizes the engine then requests a repaint
 *
 * @see hpe_engine_resize
 */
static void app_resize(void *user, int width, int height)
{
    App *app = (App *)user;
    hpe_engine_resize(app->engine, width, height);
    hpe_window_invalidate(app->window);
}

/**
 * @brief 响应滚轮滚动 / Handle wheel scrolling
 *
 * @param[in,out] user `App *` 用户数据 / The `App *` user data
 * @param[in]     lines 滚动行数（正负表示方向）/ Number of lines, sign gives the direction
 *
 * @note 先派发 `wheel` 给页面；滚动引擎后请求重绘 / Dispatches `wheel` into the page first, then scrolls the engine and requests a repaint
 *
 * @see hpe_engine_scroll_by hpe_engine_wheel_event
 */
static void app_scroll(void *user, float lines)
{
    App *app = (App *)user;
    /* the page sees the wheel first; the scroll chain then runs either way */
    hpe_engine_wheel_event(app->engine, app->mouse_x, app->mouse_y, 0, lines, 0);
    /* a scrollable element under the pointer consumes the wheel first; whatever
     * it cannot scroll bubbles to the document through hpe_engine_wheel_scroll */
    if (!hpe_engine_wheel_scroll(app->engine, app->mouse_x, app->mouse_y, lines))
        hpe_engine_scroll_by(app->engine, lines);
    hpe_engine_scroll_event(app->engine, NULL);
    hpe_window_invalidate(app->window);
}

/**
 * @brief 把虚拟键码映射为地址栏编辑键 / Map a virtual key code to an address-bar editing key
 *
 * @param[in]     vk 虚拟键码 / Virtual key code
 *
 * @return 对应的 `HPE_KEY_*`，非编辑键返回 0 / The matching `HPE_KEY_*`, 0 when it is not an editing key
 *
 * @see chrome_key_for_ctrl
 */
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

/**
 * @brief 把 Ctrl+字母映射为编辑快捷键 / Map a Ctrl+letter combo to an editing shortcut
 *
 * @param[in]     vk 虚拟键码（字母）/ Virtual key code (the letter)
 *
 * @return 对应的 `HPE_KEY_*`，非快捷键返回 0 / The matching `HPE_KEY_*`, 0 when the combo is not a shortcut
 *
 * @note 识别 V/C/X/A 的粘贴、复制、剪切、全选 / Recognises paste, copy, cut and select-all on V/C/X/A
 *
 * @see chrome_key_for_vk app_key
 */
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

/**
 * @brief 把键盘事件翻译成引擎按键 / Translate a keyboard event into an engine key
 *
 * @param[in,out] user `App *` 用户数据 / The `App *` user data
 * @param[in]     vk 虚拟键码 / Virtual key code
 * @param[in]     mods 修饰键掩码 / Modifier mask
 *
 * @note 先尝试 Ctrl 编辑快捷键再尝试普通编辑键；未消费时 F5 重载页面、Escape 关闭窗口 / Tries Ctrl editing shortcuts then plain editing keys; when unconsumed F5 reloads the page and Escape closes the window
 *
 * @see chrome_key_for_ctrl chrome_key_for_vk
 */
static void app_key(void *user, int vk, int mods)
{
    App *app = (App *)user;
    int key = 0;

    /* editing shortcuts first: they all act on the address-bar selection */
    if (mods & HPE_MOD_CTRL)
        key = chrome_key_for_ctrl(vk);
    if (!key)
        key = chrome_key_for_vk(vk);

    /* the page sees `keydown` before the chrome acts on it, like a browser */
    if (hpe_engine_key_event(app->engine, vk, 0, mods, 0, 0))
        hpe_window_invalidate(app->window);

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

/**
 * @brief 把按键抬起通知页面 / Notify the page that a key was released
 *
 * @param[in,out] user `App *` 用户数据 / The `App *` user data
 * @param[in]     vk 虚拟键码 / Virtual key code
 * @param[in]     mods 修饰键掩码 / Modifier mask
 *
 * @note 页面收到 `keyup`；有监听器触发时请求重绘 / The page receives `keyup`; requests a repaint when a listener ran
 *
 * @see app_key hpe_engine_key_event
 */
static void app_key_up(void *user, int vk, int mods)
{
    App *app = (App *)user;
    if (hpe_engine_key_event(app->engine, vk, 0, mods, 1, 0))
        hpe_window_invalidate(app->window);
}

/**
 * @brief 把已输入字符送给地址栏 / Pass a typed character to the address bar
 *
 * @param[in,out] user `App *` 用户数据 / The `App *` user data
 * @param[in]     codepoint Unicode 码点 / Unicode code point
 *
 * @note 引擎报告已消费时请求重绘 / Requests a repaint when the engine reports the key was consumed
 *
 * @see hpe_engine_key
 */
static void app_char(void *user, int codepoint)
{
    App *app = (App *)user;
    /* pages read typed text through `keypress`, so send it there as well */
    hpe_engine_key_event(app->engine, codepoint, codepoint, 0, 2, 0);
    if (hpe_engine_key(app->engine, codepoint))
        hpe_window_invalidate(app->window);
}

/**
 * @brief 把鼠标事件交给引擎 / Forward a mouse event to the engine
 *
 * @param[in,out] user `App *` 用户数据 / The `App *` user data
 * @param[in]     x 客户端 X 坐标 / Client X coordinate
 * @param[in]     y 客户端 Y 坐标 / Client Y coordinate
 * @param[in]     button 鼠标按键（0 左键，负数表示纯移动）/ Mouse button (0 = left, negative means a bare move)
 * @param[in]     down 按下为 1、抬起为 0 / 1 when pressed, 0 when released
 * @param[in]     mods 修饰键掩码 / Modifier mask
 *
 * @note 按键为负时走移动路径，否则走按键路径；有变化才请求重绘 / Uses the move path for a negative button, the button path otherwise; repaints only on a change
 *
 * @see hpe_engine_mouse_event_mods
 */
static void app_mouse(void *user, int x, int y, int button, int down, int mods)
{
    App *app = (App *)user;
    int changed;
    app->mouse_x = x;
    app->mouse_y = y;
    if (button < 0)
        changed = hpe_engine_mouse_move(app->engine, x, y);
    else
        changed = hpe_engine_mouse_event_mods(app->engine, x, y, button, down, mods);
    if (changed)
        hpe_window_invalidate(app->window);
}

/**
 * @brief 响应周期定时器 / Handle the periodic timer
 *
 * @param[in,out] user `App *` 用户数据 / The `App *` user data
 *
 * @note 泵脚本定时器与 rAF 回调；有变化才请求重绘 / Drains script timers and rAF callbacks; repaints only on a change
 *
 * @see hpe_engine_tick
 */
static void app_tick(void *user)
{
    App *app = (App *)user;
    if (hpe_engine_tick(app->engine))
        hpe_window_invalidate(app->window);
}

/**
 * @brief 把引擎光标语义转成平台光标 ID / Translate the engine's cursor semantics into a platform cursor id
 *
 * @param[in]     user `App *` 用户数据 / The `App *` user data
 *
 * @return `HPE_PLAT_CURSOR_*` ID / One of the `HPE_PLAT_CURSOR_*` ids
 *
 * @note 读引擎缓存的 `HpeCursor` 并映射到平台词汇表；grab/grabbing 暂用手形近似 / Reads the engine's cached `HpeCursor` and maps it onto the platform vocabulary; grab/grabbing are approximated with the hand cursor for now
 *
 * @see hpe_engine_page_cursor
 */
static int app_query_cursor(void *user)
{
    App *app = (App *)user;
    switch (hpe_engine_page_cursor(app->engine)) {
    case HPE_CURSOR_POINTER:
    case HPE_CURSOR_GRAB:
    case HPE_CURSOR_GRABBING:
        return HPE_PLAT_CURSOR_HAND;
    case HPE_CURSOR_TEXT:
        return HPE_PLAT_CURSOR_IBEAM;
    case HPE_CURSOR_MOVE:
        return HPE_PLAT_CURSOR_MOVE;
    case HPE_CURSOR_WAIT:
        return HPE_PLAT_CURSOR_WAIT;
    case HPE_CURSOR_HELP:
        return HPE_PLAT_CURSOR_HELP;
    case HPE_CURSOR_CROSSHAIR:
        return HPE_PLAT_CURSOR_CROSS;
    case HPE_CURSOR_NOT_ALLOWED:
        return HPE_PLAT_CURSOR_NO;
    case HPE_CURSOR_EW_RESIZE:
        return HPE_PLAT_CURSOR_SIZEWE;
    case HPE_CURSOR_NS_RESIZE:
        return HPE_PLAT_CURSOR_SIZENS;
    default:
        return HPE_PLAT_CURSOR_DEFAULT;
    }
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
    float elem_scroll_lines; /* wheel lines to send to the element at --elem-scroll-at */
    int elem_scroll_x, elem_scroll_y;
} Options;

/**
 * @brief 打印命令行用法 / Print the command-line usage
 *
 * @note 列出窗口尺寸、`--dump` 截图、`--bench`、`--scroll-lines`、`--chrome` 以及 `--js`/`--module` 脚本模式 / Lists window size, `--dump` screenshots, `--bench`, `--scroll-lines`, `--chrome` and the `--js`/`--module` script modes
 *
 * @see parse_args
 */
static void print_usage(void)
{
    printf("usage: hope [file.html] [--width N] [--height N] [--dump out.bmp] "
           "[--bench N] [--scroll-lines N] [--elem-scroll X Y N] [--chrome]\n"
           "       hope --js script.js | --module module.js   (run a script only)\n"
           "       with no file argument the built-in blank page is shown\n");
}

/**
 * @brief 解析命令行参数 / Parse the command-line arguments
 *
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] argv 参数值数组 / Argument value array
 * @param[out]    o 接收解析结果的选项结构 / Options struct that receives the parsed settings
 *
 * @return 需要就此结束时返回进程退出码，继续运行返回 -1 / The process exit code when the run should stop here, -1 to continue
 *
 * @note `--js`/`--module` 会在解析期间直接运行脚本并以其退出码返回；无法识别的参数被当作待打开的文件路径 / `--js`/`--module` run a script immediately and return its exit code; an unrecognised argument is taken as the file path to open
 *
 * @see print_usage run_js_file
 */
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
        } else if (!strcmp(argv[i], "--elem-scroll") && i + 2 < argc) {
            /* `--elem-scroll X Y LINES`: wheel over (X,Y) by LINES */
            o->elem_scroll_x = atoi(argv[++i]);
            o->elem_scroll_y = atoi(argv[++i]);
            o->elem_scroll_lines = (float)atof(argv[++i]);
        } else {
            o->path = argv[i];
        }
    }
    return -1;
}

/**
 * @brief 加载启动文档并决定是否显示地址栏 / Load the start document and decide whether to show the address bar
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     o 命令行选项 / Command-line options
 *
 * @note 窗口模式总是显示地址栏；无头 `--dump` 模式默认只画页面，除非传了 `--chrome`，以保持像素基线稳定 / Window mode always shows the address bar; headless `--dump` stays page-only unless `--chrome` is passed, keeping pixel baselines stable
 *
 * @see main
 */
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
/* horizontal point inside the field's text viewport (the nav buttons occupy
 * the left edge, so a click must land past CHROME_PAD + 3*(28+4) = 106) */
#define ADDR_TEST_X 200

/**
 * @brief 合成一次鼠标点击 / Synthesise one mouse click
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     x X 坐标 / X coordinate
 * @param[in]     y Y 坐标 / Y coordinate
 * @param[in]     mods 修饰键掩码 / Modifier mask
 *
 * @note 先移动指针再按下与抬起，模拟真实点击 / Moves the pointer first, then presses and releases, mimicking a real click
 *
 * @see synth_drag
 */
static void synth_click(HpeEngine *engine, int x, int y, int mods)
{
    hpe_engine_mouse_move(engine, x, y);
    hpe_engine_mouse_event_mods(engine, x, y, 0, 1, mods);
    hpe_engine_mouse_event_mods(engine, x, y, 0, 0, mods);
}

/**
 * @brief 合成一次拖拽（按下、滑过、抬起）/ Synthesise a drag (press, walk over, release)
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     x1 拖拽起点 X / Drag start X
 * @param[in]     x2 拖拽终点 X / Drag end X
 * @param[in]     y 拖拽 Y 坐标 / Drag Y coordinate
 *
 * @note 中间分 6 步移动指针，像真人拖拽一样经过中间位置 / Walks the pointer over six intermediate steps so it looks like a human drag
 *
 * @see synth_click
 */
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

/**
 * @brief 把按键名解析为按键码与修饰键 / Resolve a key name into a key code and modifiers
 *
 * @param[in]     name 按键名，如 `LEFT`、`CTRL_C` / Key name such as `LEFT` or `CTRL_C`
 * @param[out]    key 接收 `HPE_KEY_*` 按键码 / Receives the `HPE_KEY_*` key code
 * @param[out]    mods 接收 `HPE_MOD_*` 修饰键掩码 / Receives the `HPE_MOD_*` modifier mask
 *
 * @return 名称匹配返回 1，否则 0 / 1 when the name matched, otherwise 0
 *
 * @note 内置一张固定名表，未匹配时不写出输出 / Looks up a fixed name table and writes nothing when there is no match
 *
 * @see hook_addr_keys
 */
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

/**
 * @brief 把 UTF-8 字符串逐字符当作按键送入引擎 / Feed a UTF-8 string into the engine as individual key strokes
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     s 要输入文本 / Text to type
 *
 * @note 按码点切分后逐个调用 `hpe_engine_key`，用于无头测试打字 / Splits by code point and calls `hpe_engine_key` per character, used for headless typing tests
 *
 * @see hook_test_addr_editor
 */
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

/**
 * @brief 测试钩子：像用户一样驱动地址栏导航 / Test hook: drive address-bar navigation like a user would
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @note 读 `HOPE_TEST_ADDR` 作为输入；先点击条带聚焦，再打字，设置了 `HOPE_TEST_ADDR_ENTER` 时按回车 / Reads `HOPE_TEST_ADDR` for the input; clicks the strip to focus, types it, and presses Enter when `HOPE_TEST_ADDR_ENTER` is set
 *
 * @see hpe_engine_key
 */
static void hook_test_addr(HpeEngine *engine)
{
    const char *typed = getenv("HOPE_TEST_ADDR");
    if (!typed || !*typed)
        return;
    synth_click(engine, ADDR_TEST_X, ADDR_TEST_Y, 0); /* focus the field */
    synth_type(engine, typed);
    if (getenv("HOPE_TEST_ADDR_ENTER"))
        hpe_engine_key(engine, HPE_KEY_RETURN);
}

/**
 * @brief 测试钩子：播种剪贴板后按 Ctrl+V 语义粘贴 / Test hook: seed the clipboard and paste with Ctrl+V semantics
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @note `HOPE_TEST_CLIPBOARD` 提供剪贴板文本；设置了 `HOPE_TEST_PASTE` 时聚焦并粘贴，再按 `HOPE_TEST_PASTE_ENTER` 决定是否回车 / `HOPE_TEST_CLIPBOARD` supplies the clipboard text; with `HOPE_TEST_PASTE` set it focuses and pastes, pressing Enter when `HOPE_TEST_PASTE_ENTER` is set
 *
 * @see hpe_clipboard_set_text
 */
static void hook_test_clipboard(HpeEngine *engine)
{
    const char *seed = getenv("HOPE_TEST_CLIPBOARD");
    if (seed)
        hpe_clipboard_set_text(seed);
    if (getenv("HOPE_TEST_PASTE")) {
        synth_click(engine, ADDR_TEST_X, ADDR_TEST_Y, 0); /* focus the field */
        hpe_engine_key(engine, HPE_KEY_PASTE);
        if (getenv("HOPE_TEST_PASTE_ENTER"))
            hpe_engine_key(engine, HPE_KEY_RETURN);
    }
}

/**
 * @brief 测试钩子：按 `x[,mods]` 串点击地址栏 / Test hook: click the address bar from an `x[,mods]` string
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     click 分号分隔的 `x[,mods]` 点击项 / Semicolon-separated `x[,mods]` click entries
 *
 * @note 每项在地址栏纵向中心合成一次点击 / Synthesises one click at each entry on the address-bar vertical centre
 *
 * @see synth_click hook_test_addr_editor
 */
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

/**
 * @brief 测试钩子：按 `x1[,x2]` 串连点两次选词 / Test hook: click twice from an `x1[,x2]` string to select a word
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     dbl `x1[,x2]` 双击坐标 / `x1[,x2]` double-click coordinates
 *
 * @note 省略 `x2` 或为 0 时两次都点在 `x1` / Uses `x1` twice when `x2` is omitted or zero
 *
 * @see synth_click
 */
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

/**
 * @brief 测试钩子：按 `x1,x2[,y]` 串合成拖选 / Test hook: drag-select from an `x1,x2[,y]` string
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     drag `x1,x2[,y]` 拖选坐标 / `x1,x2[,y]` drag coordinates
 *
 * @note 省略 `y` 时使用地址栏纵向中心 / Falls back to the address-bar vertical centre when `y` is omitted
 *
 * @see synth_drag
 */
static void hook_addr_drag(HpeEngine *engine, const char *drag)
{
    int x1 = 0, x2 = 0, dy = ADDR_TEST_Y;
    if (sscanf(drag, "%d,%d,%d", &x1, &x2, &dy) >= 2)
        synth_drag(engine, x1, x2, dy);
}

/**
 * @brief 测试钩子：按分号串发送命名按键 / Test hook: send named key strokes from a semicolon-separated string
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     keys 分号分隔的按键名，如 `LEFT;CTRL_C` / Semicolon-separated key names such as `LEFT;CTRL_C`
 *
 * @note 每个名字经 `key_named` 解析后带修饰键送入引擎 / Each name is resolved by `key_named` and sent to the engine with its modifiers
 *
 * @see key_named
 */
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

/**
 * @brief 测试钩子：驱动地址栏文本编辑器 / Test hook: drive the address-bar text editor
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @note 依次处理 `HOPE_TEST_ADDR_CLICK`、`HOPE_TEST_ADDR_DBLCLICK`、`HOPE_TEST_ADDR_DRAG`、`HOPE_TEST_TYPE`、`HOPE_TEST_KEYS` / Processes `HOPE_TEST_ADDR_CLICK`, `HOPE_TEST_ADDR_DBLCLICK`, `HOPE_TEST_ADDR_DRAG`, `HOPE_TEST_TYPE` and `HOPE_TEST_KEYS` in turn
 *
 * @see hook_addr_click hook_addr_keys
 */
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

/**
 * @brief 测试钩子：合成点击以驱动交互页面 / Test hook: synthesise clicks to drive interactive pages
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @note 读 `HOPE_TEST_CLICK_AT`，形如 `x,y[,button]`；用分号分隔多项，可让思维导图节点折叠或按钮触发 / Reads `HOPE_TEST_CLICK_AT` as `x,y[,button]`, semicolon-separated, so a mind-map node collapses or a button fires
 *
 * @see hpe_engine_mouse_event
 */
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

/**
 * @brief 测试钩子：合成页面滚动条拖拽 / Test hook: synthesise a page-scrollbar drag
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @note 读 `HOPE_TEST_SCROLLBAR_DRAG`，形如 `x,y0,y1`：先在 `(x,y0)` 按下，再逐步移动到
 *       `(x,y1)`，最后在同一 x 抬起；用打印状态行观察滚动位置变化
 *       / Reads `HOPE_TEST_SCROLLBAR_DRAG` as `x,y0,y1`: presses at `(x,y0)`, walks the
 *       pointer to `(x,y1)` and releases; the status line shows how the scroll moved
 *
 * @see hpe_engine_mouse_move hpe_engine_mouse_event
 */
static void hook_test_scrollbar_drag(HpeEngine *engine)
{
    const char *spec = getenv("HOPE_TEST_SCROLLBAR_DRAG");
    int x = 0, y0 = 0, y1 = 0, i, steps = 8;
    if (!spec || !*spec)
        return;
    if (sscanf(spec, "%d,%d,%d", &x, &y0, &y1) < 3)
        return;
    hpe_engine_mouse_move(engine, x, y0);
    hpe_engine_mouse_event(engine, x, y0, 0, 1);
    for (i = 1; i <= steps; i++)
        hpe_engine_mouse_move(engine, x, y0 + (y1 - y0) * i / steps);
    hpe_engine_mouse_event(engine, x, y1, 0, 0);
}

/**
 * @brief 测试钩子：在指定点合成一次滚轮并复刻窗口路径 / Test hook: synthesise a wheel at a point, replaying the window path
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @note 读 `HOPE_TEST_WHEEL`，形如 `x,y,lines`；按 `app_scroll` 的顺序派发 `wheel` 事件 → 元素滚动链 → 页面整页滚动 → `scroll` 事件，最后打印滚动偏移（`scroll=<offset>`）供断言 / Reads `HOPE_TEST_WHEEL` as `x,y,lines`; replays the `app_scroll` order — `wheel` event → element scroll chain → page scroll → `scroll` event — then prints the scroll offset (`scroll=<offset>`) for assertions
 *
 * @see app_scroll hpe_engine_wheel_scroll
 */
static void hook_test_wheel(HpeEngine *engine)
{
    const char *spec = getenv("HOPE_TEST_WHEEL");
    int x = 0, y = 0;
    float lines = 0.0f;
    if (!spec || !*spec)
        return;
    if (sscanf(spec, "%d,%d,%f", &x, &y, &lines) < 3)
        return;
    hpe_engine_wheel_event(engine, x, y, 0, lines, 0);
    if (!hpe_engine_wheel_scroll(engine, x, y, lines))
        hpe_engine_scroll_by(engine, lines);
    hpe_engine_scroll_event(engine, NULL);
    printf("scroll=%g\n", (double)hpe_engine_scroll_offset(engine));
}

/**
 * @brief 测试钩子：合成键盘 / 滚轮 / 滚动事件 / Test hook: synthesise keyboard, wheel and scroll events
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @note 读 `HOPE_TEST_EVENTS`，形如 `keydown:65;keyup:65;wheel:0,96;scroll`（分号分隔）。
 *       `keydown`/`keyup` 的参数是虚拟键码；`wheel` 的参数是 `dx,dy`；`scroll` 无参数。
 *       坐标取自 `HOPE_TEST_EVENT_AT`（`x,y`，默认 `100,200`），用于定位滚轮与指针事件的目标。
 *       Reads `HOPE_TEST_EVENTS` as `keydown:65;keyup:65;wheel:0,96;scroll`, semicolon-separated.
 *       The key names take a virtual key code, `wheel` takes `dx,dy` and `scroll` takes nothing.
 *       Coordinates come from `HOPE_TEST_EVENT_AT` (`x,y`, defaulting to `100,200`) so wheel and
 *       pointer events can be aimed at a specific element.
 *
 * @see hpe_engine_key_event hpe_engine_wheel_event hpe_engine_scroll_event
 */
static void hook_test_events(HpeEngine *engine)
{
    const char *spec = getenv("HOPE_TEST_EVENTS");
    const char *at = getenv("HOPE_TEST_EVENT_AT");
    char buf[512];
    char *tok;
    int px = 100, py = 200;
    if (!spec || !*spec)
        return;
    if (at && *at)
        sscanf(at, "%d,%d", &px, &py);
    snprintf(buf, sizeof(buf), "%s", spec);
    for (tok = strtok(buf, ";"); tok; tok = strtok(NULL, ";")) {
        int arg = 0, dx = 0, dy = 0;
        const char *colon = strchr(tok, ':');
        if (colon)
            arg = atoi(colon + 1);
        if (!strncmp(tok, "keydown", 7))
            hpe_engine_key_event(engine, arg, 0, 0, 0, 0);
        else if (!strncmp(tok, "keyup", 5))
            hpe_engine_key_event(engine, arg, 0, 0, 1, 0);
        else if (!strncmp(tok, "keypress", 8))
            hpe_engine_key_event(engine, arg, arg, 0, 2, 0);
        else if (!strncmp(tok, "wheel", 5)) {
            if (colon && sscanf(colon + 1, "%d,%d", &dx, &dy) >= 1)
                hpe_engine_wheel_event(engine, px, py, dx, (float)dy / 48.0f, 0);
        } else if (!strncmp(tok, "move", 4)) {
            hpe_engine_mouse_move(engine, px, py);
        } else if (!strncmp(tok, "scroll", 6)) {
            hpe_engine_scroll_event(engine, NULL);
        }
    }
}

/**
 * @brief 报告地址栏最终状态 / Report the final address-bar state
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @note 仅在设置了 `HOPE_TEST_ADDR_TEXT` 时输出文本、长度、选区、插入符与焦点 / Prints the text, length, selection, caret and focus only when `HOPE_TEST_ADDR_TEXT` is set
 *
 * @see hpe_engine_address_selection
 */
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

/**
 * @brief 报告加载时探测到的文档元数据 / Report document metadata detected at load time
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @note 仅在设置了 `HOPE_TEST_META` 时输出编码、`<meta charset>`、`<base href>` 以及逐条的 `<link>` 资源提示 / Prints the encoding, `<meta charset>`, `<base href>` and each `<link>` resource hint only when `HOPE_TEST_META` is set
 *
 * @see hpe_engine_link_at
 */
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

/**
 * @brief 报告某点的光标语义与插入符相位 / Report the cursor semantics and caret phase at given points
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @note 仅在设置了 `HOPE_TEST_CURSOR` 时输出：把该值当作 `x,y;x,y;...` 逐点派发一次指针移动并打印解析出的 `HpeCursor` 名称；额外设置 `HOPE_TEST_CARET`（值为等待秒数）时先跑该时长的 tick 再打印插入符相位 / Prints only when `HOPE_TEST_CURSOR` is set: the value is treated as `x,y;x,y;...`, each point gets one synthetic pointer move and its resolved `HpeCursor` name is printed; with `HOPE_TEST_CARET` additionally set (its value is the wait in seconds) the caret phase is printed after ticking for that long
 *
 * @see hpe_engine_page_cursor hpe_engine_mouse_move hpe_engine_tick
 */
static void report_cursor(HpeEngine *engine)
{
    static const char *names[] = {"auto",     "default",   "pointer",  "text",
                                  "move",     "wait",      "help",     "crosshair",
                                  "not-allowed", "grab",  "grabbing", "ew-resize",
                                  "ns-resize"};
    const char *spec = getenv("HOPE_TEST_CURSOR");
    const char *caret = getenv("HOPE_TEST_CARET");
    char buf[512];
    char *tok;
    if (!spec || !*spec)
        return;
    snprintf(buf, sizeof(buf), "%s", spec);
    for (tok = strtok(buf, ";"); tok; tok = strtok(NULL, ";")) {
        int cx = 0, cy = 0, cur;
        if (sscanf(tok, "%d,%d", &cx, &cy) < 2)
            continue;
        hpe_engine_mouse_move(engine, cx, cy);
        cur = hpe_engine_page_cursor(engine);
        printf("cursor %d,%d %s\n", cx, cy,
               (cur >= 0 && cur <= HPE_CURSOR_NS_RESIZE) ? names[cur] : "?");
    }
    if (caret) {
        double wait = atof(caret);
        double t0 = hpe_time_seconds();
        while (hpe_time_seconds() - t0 < wait)
            hpe_engine_tick(engine);
        printf("caret visible=%d\n", hpe_engine_caret_visible(engine));
    }
}

/**
 * @brief 测试钩子：按 `HOPE_TEST_NAV_CLICK` 点击导航按钮 / Test hook: click nav buttons from `HOPE_TEST_NAV_CLICK`
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @note 值为分号分隔的按钮下标（0 后退、1 前进、2 刷新），逐个在按钮中心合成一次点击；配合 `HOPE_TEST_NAV` 观察历史栈变化 / The value is semicolon-separated button indices (0 back, 1 forward, 2 reload); each gets one synthetic click at its centre, so `HOPE_TEST_NAV` can observe the history change
 *
 * @see nav_activate synth_click report_nav
 */
static void hook_test_nav_click(HpeEngine *engine)
{
    const char *spec = getenv("HOPE_TEST_NAV_CLICK");
    char buf[128];
    char *tok;
    if (!spec || !*spec)
        return;
    snprintf(buf, sizeof(buf), "%s", spec);
    for (tok = strtok(buf, ";"); tok; tok = strtok(NULL, ";")) {
        int i = atoi(tok);
        /* CHROME_PAD 10 + i * (NAV_BTN_W 28 + NAV_BTN_GAP 4), centre of a 28px box */
        int x = 10 + i * 32 + 14;
        if (i >= 0 && i < 3)
            synth_click(engine, x, ADDR_TEST_Y, 0);
    }
}

/**
 * @brief 报告导航历史栈状态 / Report the navigation history state
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @note 仅在设置了 `HOPE_TEST_NAV` 时输出 `nav=<count>@<pos>` 与地址文本 / Prints `nav=<count>@<pos>` plus the address text only when `HOPE_TEST_NAV` is set
 *
 * @see hpe_engine_back hpe_engine_forward hpe_engine_reload
 */
static void report_nav(HpeEngine *engine)
{
    const char *spec = getenv("HOPE_TEST_NAV");
    if (!spec || !*spec)
        return;
    printf("nav count=%d pos=%d addr=[%s]\n", hpe_engine_nav_count(engine),
           hpe_engine_nav_pos(engine), hpe_engine_address_text(engine));
}

/**
 * @brief 无头模式运行并可选导出截图 / Run headlessly and optionally export a screenshot
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     o 命令行选项 / Command-line options
 *
 * @return 固定返回 0 / Always returns 0
 *
 * @note 指定 `--scroll-lines` 时先整帧渲染再滚动，使滚动走脏矩形路径以成为真正的增量滚动测试；`--bench` 重复渲染并打印平均耗时；最后销毁引擎 / With `--scroll-lines` it renders a full frame first so the scroll goes through the damage-rect path and becomes a real incremental-scrolling test; `--bench` repeats rendering and prints the average; then the engine is destroyed
 *
 * @see write_bmp hpe_engine_scroll_by
 */

static int run_headless(HpeEngine *engine, const Options *o)
{
    if (o->scroll_lines != 0.0f) {
        /* paint a full frame first so the scroll below goes through the
         * damage-rect path — this is what makes --scroll-lines a real test
         * of incremental scrolling (compare with HPE_NO_DIRTY=1) */
        hpe_engine_render(engine);
        hpe_engine_scroll_by(engine, o->scroll_lines);
    }
    if (o->elem_scroll_lines != 0.0f) {
        /* element-level wheel: paint first, then deliver the wheel to whatever
         * scroll container sits under (X,Y) */
        hpe_engine_render(engine);
        hpe_engine_wheel_scroll(engine, o->elem_scroll_x, o->elem_scroll_y,
                                o->elem_scroll_lines);
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

/**
 * @brief 创建窗口并进入事件循环 / Create the window and enter the event loop
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     width 窗口宽度 / Window width
 * @param[in]     height 窗口高度 / Window height
 *
 * @return 成功返回 0，创建窗口失败返回 1 / 0 on success, 1 when window creation failed
 *
 * @note 装配全部窗口回调后阻塞运行；循环返回后销毁窗口与引擎 / Wires up all window callbacks and blocks in the loop; destroys the window and engine afterwards
 *
 * @see app_paint hpe_window_run
 */
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
    callbacks.on_key_up = app_key_up;
    callbacks.on_char = app_char;
    callbacks.on_mouse = app_mouse;
    callbacks.on_tick = app_tick;
    callbacks.query_cursor = app_query_cursor;

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

/**
 * @brief 程序入口：解析参数并选择运行模式 / Program entry: parse arguments and pick the run mode
 *
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] argv 参数值数组 / Argument value array
 *
 * @return 进程退出码 / The process exit code
 *
 * @note 默认窗口 1000×700；创建引擎、加载启动文档、依次跑各测试钩子并汇报，然后按 `--dump`/`--bench` 走无头或窗口模式 / Defaults to a 1000×700 window; creates the engine, loads the start document, runs the test hooks and reports, then chooses headless or window mode based on `--dump`/`--bench`
 *
 * @see parse_args run_headless run_window
 */
int main(int argc, char **argv)
{
    Options o;
    HpeEngine *engine;
    int rc;

    hpe_js_gc_capture_main_base(); /* outermost frame = GC stack scan bound */
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
    hook_test_scrollbar_drag(engine);
    hook_test_wheel(engine);
    hook_test_nav_click(engine);
    hook_test_events(engine);
    report_addr_text(engine);
    report_meta(engine);
    report_cursor(engine);
    report_nav(engine);

    if (o.dump || o.bench)
        return run_headless(engine, &o);
    return run_window(engine, o.width, o.height);
}
