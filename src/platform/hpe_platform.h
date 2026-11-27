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

/* cursor ids the query_cursor callback may return; the backend maps each one
 * onto its own system cursor imagery */
#define HPE_PLAT_CURSOR_DEFAULT 0
#define HPE_PLAT_CURSOR_HAND 1
#define HPE_PLAT_CURSOR_IBEAM 2
#define HPE_PLAT_CURSOR_MOVE 3
#define HPE_PLAT_CURSOR_WAIT 4
#define HPE_PLAT_CURSOR_HELP 5
#define HPE_PLAT_CURSOR_CROSS 6
#define HPE_PLAT_CURSOR_NO 7
#define HPE_PLAT_CURSOR_SIZEWE 8
#define HPE_PLAT_CURSOR_SIZENS 9

typedef struct HpeWindowCallbacks {
    void (*on_paint)(void *user);
    void (*on_resize)(void *user, int width, int height);
    void (*on_scroll)(void *user, float delta_lines);
    /* virtual-key press; `mods` is a mask of HPE_MOD_* captured at the moment
     * of the key event (needed for shortcuts such as Ctrl+V) */
    void (*on_key)(void *user, int vk_code, int mods);
    /* virtual-key release, mirroring on_key; pages receive a `keyup` for it.
     * May be NULL, in which case no `keyup` is delivered. */
    void (*on_key_up)(void *user, int vk_code, int mods);
    /* typed text: unicode code point (ASCII today; surrogate pairs are skipped
     * by the Win32 backend). Used by the address bar for path entry and by
     * pages as the source of `keypress`. */
    void (*on_char)(void *user, int codepoint);
    /* IME composition updated: `utf8` is the in-progress string ("" when the
     * composition was cleared or ended without a result). May be NULL. */
    void (*on_ime_compose)(void *user, const char *utf8);
    /* IME composition committed: `utf8` is the final string to insert. May be
     * NULL. */
    void (*on_ime_commit)(void *user, const char *utf8);
    /* mouse: x,y are viewport (client) coords; button 0 = left; down: 1 = pressed, 0 = released;
     * button -1 = pointer move (no button state change). `mods` is a mask of
     * HPE_MOD_* captured with the event (Shift+click extends a text selection).
     * A left-button drag keeps delivering moves while the pointer is outside the
     * window: the backend captures the pointer until the button is released. */
    void (*on_mouse)(void *user, int x, int y, int button, int down, int mods);
    /* periodic tick (~15 ms) used to drain script timers / rAF callbacks */
    void (*on_tick)(void *user);
    /* resolve the system cursor for the client area: return one of the
     * HPE_PLAT_CURSOR_* ids above. Called on WM_SETCURSOR while the pointer is
     * over the client area; may be NULL for the backend's default arrow. */
    int (*query_cursor)(void *user);
} HpeWindowCallbacks;

/**
 * @brief 创建平台窗口并绑定回调 / Create a platform window and bind its callbacks
 *
 * @param[in]     title 窗口标题 / Window title
 * @param[in]     width 客户区宽度（像素）/ Client width in pixels
 * @param[in]     height 客户区高度（像素）/ Client height in pixels
 * @param[in]     callbacks 窗口回调表，可为 NULL / Callback table, may be NULL
 * @param[in,out] user 透传给每个回调的用户数据 / User data passed through to every callback
 *
 * @return 新窗口对象，失败返回 NULL / The new window object, NULL on failure
 *
 * @note 窗口是运行期间唯一的顶层窗口；调用方随后需 `hpe_window_destroy` / The window is the single top-level window for the run; the caller later calls `hpe_window_destroy`
 *
 * @see hpe_window_run hpe_window_destroy
 */
HpeWindow *hpe_window_create(const char *title, int width, int height,
                             const HpeWindowCallbacks *callbacks, void *user);

/**
 * @brief 提交一帧 BGRA 像素并请求重绘 / Present one BGRA frame and request a repaint
 *
 * @param[in,out] win 目标窗口 / Target window
 * @param[in]     bgra_pixels BGRA32 像素数据 / BGRA32 pixel data
 * @param[in]     width 像素宽度 / Pixel width
 * @param[in]     height 像素高度 / Pixel height
 * @param[in]     stride_bytes 每行字节数 / Bytes per row
 *
 * @note 缓冲需在窗口下次重绘前保持有效 / The buffer must stay valid until the window repaints
 *
 * @see hpe_window_invalidate
 */
void hpe_window_present(HpeWindow *win, const void *bgra_pixels, int width, int height,
                        int stride_bytes);

/**
 * @brief 设置窗口标题栏文本 / Set the window caption text
 *
 * @param[in,out] win 窗口对象 / Window object
 * @param[in]     title 新的标题文本（UTF-8）/ New caption text in UTF-8
 *
 * @see hpe_window_create
 */
void hpe_window_set_title(HpeWindow *win, const char *title);

/**
 * @brief 请求重绘整个客户区 / Request a repaint of the whole client area
 *
 * @param[in,out] win 目标窗口 / Target window
 *
 * @see hpe_window_present
 */
void hpe_window_invalidate(HpeWindow *win);

/**
 * @brief 异步请求关闭窗口 / Ask the window to close asynchronously
 *
 * @param[in,out] win 目标窗口 / Target window
 *
 * @see hpe_window_run
 */
void hpe_window_request_close(HpeWindow *win);

/**
 * @brief 进入消息循环直到窗口关闭 / Run the message loop until the window closes
 *
 * @param[in,out] win 目标窗口 / Target window
 *
 * @return 进程退出码 / The process exit code
 *
 * @note 阻塞调用，窗口关闭后返回 / Blocks and returns once the window closes
 *
 * @see hpe_window_request_close
 */
int hpe_window_run(HpeWindow *win); /* blocks until the window closes */

/**
 * @brief 销毁窗口并释放窗口对象 / Destroy the window and free the window object
 *
 * @param[in,out] win 目标窗口 / Target window
 *
 * @see hpe_window_create
 */
void hpe_window_destroy(HpeWindow *win);

/**
 * @brief 读取单调时钟秒数 / Read monotonic seconds
 *
 * @return 自某个未指定起点起的单调秒数 / Monotonic seconds since an unspecified origin
 */
double hpe_time_seconds(void); /* monotonic seconds since an unspecified origin */

/**
 * @brief 读取自 Unix 纪元起的 UTC 墙钟毫秒 / Read UTC wall-clock milliseconds since the Unix epoch
 *
 * @return 自 1970-01-01 UTC 起的毫秒数 / Milliseconds since 1970-01-01 UTC
 *
 * @note `Date.now` 的数据来源 / Backs `Date.now`
 *
 * @see hpe_time_seconds
 */
double hpe_time_epoch_ms(void);

/**
 * @brief 求某个 UTC 时刻的系统时区偏移 / Resolve the system timezone offset at a UTC instant
 *
 * @param[in]     epoch_ms 自 Unix 纪元起的 UTC 毫秒 / UTC milliseconds since the Unix epoch
 * @param[out]    out_name 接收时区名的缓冲，可为 `NULL` / Receives the timezone name, may be `NULL`
 * @param[in]     cap `out_name` 的容量（字节，UTF-8）/ Capacity of `out_name` in bytes, UTF-8
 * @param[out]    out_dst 接收是否为夏令时的标志，可为 `NULL` / Receives the daylight-saving flag, may be `NULL`
 *
 * @return 本地时间相对 UTC 的偏移分钟数（东为正，与 `getTimezoneOffset()` 反号）/ Offset in minutes of local time from UTC, positive east (the opposite sign of `getTimezoneOffset()`)
 *
 * @note 按**该时刻**的历史时区规则求解，因此跨夏令时切换点前后会得到不同偏移；无时区数据库的平台上退化为固定偏移 / Resolved with the **historical** rules in force at that instant, so the offset differs across a DST transition; a platform without a timezone database degrades to a fixed offset
 *
 * @see hpe_time_epoch_ms
 */
int hpe_time_utc_offset_minutes(double epoch_ms, char *out_name, size_t cap, int *out_dst);

/**
 * @brief 一段可挂起/恢复的执行栈 / A suspendable, resumable execution stack
 *
 * @note 生成器靠它把 `yield` 处的 C 调用栈原地冻结，`next()` 时从断点继续；
 *       异常仍走 `longjmp`，因此**不能**跨上下文抛错，调用方须保证两侧都在同一上下文内。
 *       Generators use this to freeze the C call stack at a `yield` and resume from that point on
 *       `next()`; exceptions still travel via `longjmp`, so an error must **not** cross the boundary.
 */
typedef struct HpeCoro HpeCoro;

/**
 * @brief 在当前线程上创建一个协程 / Create a coroutine on the current thread
 *
 * @param[in]     entry 协程入口函数 / Coroutine entry point
 * @param[in,out] user 透传给入口函数的指针 / Pointer forwarded to the entry function
 * @param[in]     stack_size 栈字节数，0 表示默认 / Stack size in bytes, 0 for the default
 *
 * @return 新协程；失败返回 `NULL` / The new coroutine, or `NULL` on failure
 *
 * @note 入口函数返回即视为协程结束 / Returning from the entry function marks the coroutine as finished
 *
 * @see hpe_coro_resume hpe_coro_destroy
 */
HpeCoro *hpe_coro_create(void (*entry)(void *), void *user, size_t stack_size);

/**
 * @brief 把控制权交给协程，直到它 `yield` 或结束 / Hand control to a coroutine until it yields or finishes
 *
 * @param[in,out] coro 目标协程 / Target coroutine
 *
 * @return 协程仍在运行（只是 `yield` 了）返回 1；已结束返回 0 / 1 when it yielded and is still live, 0 when it finished
 *
 * @note 只能从创建它的线程调用 / Must be called from the creating thread
 */
int hpe_coro_resume(HpeCoro *coro);

/**
 * @brief 从协程内部把控制权交还调用者 / Yield control back to the resumer from inside a coroutine
 *
 * @param[in,out] coro 当前协程 / The current coroutine
 *
 * @note 只允许在协程入口函数内部调用 / Only valid inside the coroutine entry
 */
void hpe_coro_yield(HpeCoro *coro);

/**
 * @brief 销毁协程并释放其栈 / Destroy a coroutine and release its stack
 *
 * @param[in,out] coro 目标协程；`NULL` 时无操作 / Target coroutine; a `NULL` argument is a no-op
 *
 * @note 仍在运行的协程会被直接丢弃，不会执行清理 / A still-running coroutine is discarded without unwinding
 */
void hpe_coro_destroy(HpeCoro *coro);

/**
 * @brief 查询协程栈的边界（供 GC 保守扫描挂起协程）/ Query a coroutine's stack bounds (for GC scanning of suspended coroutines)
 *
 * @param[in]  coro 协程句柄 / Coroutine handle
 * @param[out] lo 栈低端地址（含）/ Low address of the stack (inclusive)
 * @param[out] hi 栈高端地址（不含）/ High address of the stack (exclusive)
 *
 * @return 边界可用返回 1；协程为空或从未挂起过返回 0 / 1 when bounds are known; 0 when the coroutine is NULL or has never yielded
 */
int hpe_coro_stack_range(HpeCoro *coro, void **lo, void **hi);

/**
 * @brief 读取整个文件到新分配的缓冲 / Read a whole file into a freshly allocated buffer
 *
 * @param[in]     path 文件路径 / File path
 * @param[out]    out_size 接收读入的字节数，可为 NULL / Receives the byte count read, may be NULL
 *
 * @return 以 NUL 结尾的内容缓冲，失败返回 NULL / NUL-terminated contents buffer, NULL on failure
 *
 * @note 调用方需用 `hpe_free` 释放 / The caller must `hpe_free` the result
 */
char *hpe_read_file(const char *path, size_t *out_size); /* NUL terminated, hpe_free() it */

/**
 * @brief 取出路径的目录部分 / Take the directory part of a path
 *
 * @param[in]     path 输入路径 / Input path
 * @param[out]    out 接收目录的缓冲 / Receives the directory
 * @param[in]     out_size 接收缓冲区容量 / Capacity of the output buffer
 */
void hpe_path_dir(const char *path, char *out, size_t out_size);

/**
 * @brief 判断路径是否为绝对路径 / Test whether a path is absolute
 *
 * @param[in]     path 待检查路径 / Path to check
 *
 * @return 绝对路径返回 1，否则 0 / 1 for an absolute path, 0 otherwise
 */
int hpe_path_is_absolute(const char *path);

/**
 * @brief 把相对路径接到目录上 / Join a relative path onto a directory
 *
 * @param[out]    out 接收拼接结果的缓冲 / Receives the joined path
 * @param[in]     out_size 接收缓冲区容量 / Capacity of the output buffer
 * @param[in]     dir 基准目录 / Base directory
 * @param[in]     rel 相对路径 / Relative path
 */
void hpe_path_join(char *out, size_t out_size, const char *dir, const char *rel);

/**
 * @brief 把路径解析为绝对路径 / Resolve a path to an absolute one
 *
 * @param[out]    out 接收绝对路径的缓冲 / Receives the absolute path
 * @param[in]     out_size 接收缓冲区容量 / Capacity of the output buffer
 * @param[in]     path 输入路径 / Input path
 *
 * @note 相对进程工作目录解析；失败时原样拷回，供地址栏显示绝对文件路径 / Resolves against the process working directory; copies the input through unchanged on failure, as the address bar shows absolute file paths
 */
void hpe_path_absolute(char *out, size_t out_size, const char *path);

/* ---------- clipboard ---------- */
/**
 * @brief 取剪贴板文本或首个文件路径 / Read clipboard text or the first file path
 *
 * @return 以 UTF-8 编码的剪贴板内容（需 `hpe_free`），无文本时返回 NULL / UTF-8 clipboard contents (needs `hpe_free`), NULL when there is no text
 *
 * @note 资源管理器复制的文件以首个文件路径的形式暴露，符合地址栏用户的预期 / Files copied in Explorer are exposed as the first file's path, which is what an address bar user expects
 *
 * @see hpe_clipboard_set_text
 */
char *hpe_clipboard_get_text(void);

/**
 * @brief 用 UTF-8 文本替换剪贴板内容 / Replace the clipboard contents with UTF-8 text
 *
 * @param[in]     utf8 待写入的 UTF-8 文本 / UTF-8 text to write
 *
 * @return 成功返回 1，失败返回 0 / 1 on success, 0 on failure
 *
 * @note 失败时为无操作 / A no-op on failure
 *
 * @see hpe_clipboard_get_text
 */
int hpe_clipboard_set_text(const char *utf8);

/* ---------- font service ---------- */
typedef struct HpePlatFont HpePlatFont;

/**
 * @brief 创建平台字体对象 / Create a platform font object
 *
 * @param[in]     pixel_size 字号（像素）/ Font size in pixels
 * @param[in]     weight 字重 / Font weight
 * @param[in]     family 字体族 / Font family
 *
 * @return 新字体对象，失败返回 NULL / The new font object, NULL on failure
 *
 * @see hpe_plat_font_destroy
 */
HpePlatFont *hpe_plat_font_create(float pixel_size, int weight, HpeFontFamily family);

/**
 * @brief 销毁字体对象并释放其资源 / Destroy a font object and release its resources
 *
 * @param[in,out] font 待销毁的字体 / Font to destroy
 *
 * @see hpe_plat_font_create
 */
void hpe_plat_font_destroy(HpePlatFont *font);

/**
 * @brief 取字体的上伸部 / Get the font ascent
 *
 * @param[in,out] font 字体对象 / Font object
 *
 * @return 上伸部像素数 / Ascent in pixels
 */
float hpe_plat_font_ascent(HpePlatFont *font);

/**
 * @brief 取字体的下伸部 / Get the font descent
 *
 * @param[in,out] font 字体对象 / Font object
 *
 * @return 下伸部像素数 / Descent in pixels
 */
float hpe_plat_font_descent(HpePlatFont *font);

/**
 * @brief 取字体的行高 / Get the font line height
 *
 * @param[in,out] font 字体对象 / Font object
 *
 * @return 行高像素数 / Line height in pixels
 */
float hpe_plat_font_line_height(HpePlatFont *font);

/**
 * @brief 取单个码点的步进宽度 / Measure the advance width of a single code point
 *
 * @param[in,out] font 字体对象 / Font object
 * @param[in]     codepoint Unicode 码点 / Unicode code point
 *
 * @return 步进宽度（像素）/ Advance width in pixels
 */
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

/**
 * @brief 把 UTF-8 文本渲染成 8 位覆盖率掩码 / Render a UTF-8 run into an 8-bit coverage mask
 *
 * @param[in,out] font 字体对象 / Font object
 * @param[in]     utf8 待渲染的 UTF-8 文本 / UTF-8 text to render
 * @param[in]     len 文本字节数 / Number of bytes
 * @param[in]     dx 水平亚像素相位（0..1），非整数平移参与抗锯齿 / Horizontal sub-pixel phase (0..1); the fractional shift participates in antialiasing
 * @param[out]    out 接收掩码的结构 / Struct that receives the mask
 *
 * @return 成功返回 1，失败返回 0 / 1 on success, 0 on failure
 *
 * @note 覆盖区相对基线：x 从 0 起，y 在 `[-descent, ascent)`；`baseline` 为基线所在行索引 / Coverage is relative to the baseline: x from 0 and y over `[-descent, ascent)`; `baseline` is the row index of the baseline
 *
 * @see hpe_text_mask_free
 */
int hpe_plat_font_mask(HpePlatFont *font, const char *utf8, int len, float dx, HpeTextMask *out);

/**
 * @brief 判断字体是否覆盖某个码点 / Test whether the font covers a code point
 *
 * @details 对每个码点做一次字形索引查询并标记不存在的字形；结果由调用方缓存。
 *          Each query maps the code point to a glyph index marking non-existing
 *          glyphs; the caller is expected to cache the result.
 *
 * @param[in,out] font 字体对象 / Font object
 * @param[in]     codepoint Unicode 码点 / Unicode code point
 *
 * @return 覆盖返回 1，未覆盖或字体为空返回 0 / 1 when covered, 0 when missing or the font is NULL
 *
 * @see hpe_plat_font_create
 */
int hpe_plat_font_covers(HpePlatFont *font, uint32_t codepoint);

/**
 * @brief 用显式字体名创建平台字体对象 / Create a platform font object with an explicit face name
 *
 * @param[in]     pixel_size 字号（像素）/ Font size in pixels
 * @param[in]     weight 字重 / Font weight
 * @param[in]     face_utf8 字体名（UTF-8），NULL 时按字体族映射 / Typeface name in UTF-8, NULL maps by family
 *
 * @return 新字体对象，失败返回 NULL / The new font object, NULL on failure
 *
 * @note 供字形回退链按具体字体名创建后备字体 / Lets the glyph fallback chain create a fallback font by name
 *
 * @see hpe_plat_font_create hpe_plat_font_destroy
 */
HpePlatFont *hpe_plat_font_create_face(float pixel_size, int weight, const char *face_utf8);

/* ---------- color emoji glyph service (COLRv0 layers via GDI outlines) ---------- */
typedef struct HpePlatEmojiFont HpePlatEmojiFont;

/* one glyph's antialiased coverage extracted by glyph index. `coverage` is
 * w*h bytes (0..255) owned by the caller (hpe_free); `left`/`baseline` place
 * the box relative to the pen point on the baseline. */
typedef struct HpeGlyphOutline {
    unsigned char *coverage;
    int w, h;
    int left;      /* box left offset from the pen x / 盒左缘相对笔位 x 的偏移 */
    int baseline;  /* row index of the baseline inside the box / 盒内基线所在行 */
    float advance; /* cell increment x / 水平步进宽度 */
} HpeGlyphOutline;

/**
 * @brief 打开彩色 emoji 字体（按字号）/ Open the color emoji font at a pixel size
 *
 * @param[in]     pixel_size 字号（像素）/ Font size in pixels
 *
 * @return 新对象，系统无彩色 emoji 字体时返回 NULL / The new object, NULL when the system has no color emoji font
 *
 * @note Win32 后端使用 `Segoe UI Emoji`；对象只承载 DC 与字体句柄 / The Win32 backend uses `Segoe UI Emoji`; the object only owns a DC and a font handle
 *
 * @see hpe_plat_emoji_font_destroy
 */
HpePlatEmojiFont *hpe_plat_emoji_font_create(float pixel_size);

/**
 * @brief 按字形索引取抗锯齿覆盖率轮廓 / Fetch an antialiased coverage outline by glyph index
 *
 * @param[in,out] font emoji 字体对象 / Emoji font object
 * @param[in]     glyph_id 字形索引 / Glyph index
 * @param[out]    out 接收轮廓的结构，coverage 由调用方 `hpe_free` / Receives the outline, the coverage is freed by the caller via `hpe_free`
 *
 * @return 成功返回 1，空字形或失败返回 0 / 1 on success, 0 for a blank glyph or failure
 *
 * @note 内部使用 `GetGlyphOutlineW` 的 `GGO_GLYPH_INDEX|GGO_GRAY8_BITMAP` 形式 / Implemented via `GetGlyphOutlineW` with `GGO_GLYPH_INDEX|GGO_GRAY8_BITMAP`
 *
 * @see hpe_plat_emoji_font_create
 */
int hpe_plat_emoji_glyph(HpePlatEmojiFont *font, uint32_t glyph_id, HpeGlyphOutline *out);

/**
 * @brief 关闭彩色 emoji 字体并释放资源 / Close the color emoji font and release its resources
 *
 * @param[in,out] font emoji 字体对象，NULL 时无操作 / Emoji font object, NULL is a no-op
 *
 * @see hpe_plat_emoji_font_create
 */
void hpe_plat_emoji_font_destroy(HpePlatEmojiFont *font);

/**
 * @brief 释放文本掩码 / Free a text mask
 *
 * @param[in,out] mask 待释放的掩码 / Mask to free
 *
 * @see hpe_plat_font_mask
 */
void hpe_text_mask_free(HpeTextMask *mask);

#endif /* HPE_PLATFORM_H */
