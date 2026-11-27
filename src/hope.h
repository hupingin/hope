/* Hope UI Engine - public api */
#ifndef HOPE_H
#define HOPE_H

#include "render/hpe_pixmap.h"

typedef struct HpeEngine HpeEngine;
typedef struct HpeDomNode HpeDomNode;

/**
 * @brief 创建引擎实例并初始化像素缓冲 / Create an engine instance and initialise its pixel buffer
 *
 * @param[in]     width 窗口宽度（像素），非正时回退为 800 / Window width in pixels, falls back to 800 when non-positive
 * @param[in]     height 窗口高度（像素），非正时回退为 600 / Window height in pixels, falls back to 600 when non-positive
 *
 * @return 新引擎实例；调用方最终须用 `hpe_engine_destroy` 释放 / The new engine instance; the caller must eventually release it with `hpe_engine_destroy`
 *
 * @see hpe_engine_destroy
 */
HpeEngine *hpe_engine_create(int width, int height);

/**
 * @brief 销毁引擎并释放全部资源 / Destroy the engine and release all resources
 *
 * @param[in,out] engine 引擎实例，NULL 时直接返回 / Engine instance, returns early when NULL
 *
 * @note 释放盒树、DOM、样式表、像素缓冲与 JS 上下文，并关闭进程级字体子系统 / Frees the box tree, DOM, stylesheet, pixel buffer and JS context, and shuts the process-wide font subsystem down
 *
 * @see hpe_engine_create
 */
void hpe_engine_destroy(HpeEngine *engine);

/**
 * @brief 加载并排版一个 HTML 文件 / Load and lay out an HTML file
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     path 要加载的文件路径 / Path of the file to load
 *
 * @return 加载成功返回 1，否则 0 / 1 when loaded, otherwise 0
 * @retval     1 已读取、解析并排版；启用地址栏时地址栏写入绝对路径 / Read, parsed and laid out; the absolute path is written into the address bar when chrome is enabled
 * @retval     0 `engine`/`path` 为 NULL，或文件无法打开（错误写入状态行）/ `engine` or `path` is NULL, or the file could not be opened (the error is stored in the status line)
 *
 * @note 相对资源（样式表、脚本、`<base href>`）以该文件所在目录为基准 / Relative resources (stylesheets, scripts, `<base href>`) resolve against the file's directory
 *
 * @see hpe_engine_load_blank hpe_engine_reload
 */
int hpe_engine_load_file(HpeEngine *engine, const char *path);

/**
 * @brief 加载内置空白页 / Load the built-in blank page
 *
 * @param[in,out] engine 引擎实例，NULL 时返回 0 / Engine instance, returns 0 when NULL
 *
 * @return 加载成功返回 1，否则 0 / 1 when loaded, otherwise 0
 *
 * @note 无文件参数启动时使用；相对资源以 `.` 为基准；启用地址栏时清空地址并失焦 / Used when the engine starts without a file argument; relative resources resolve against `.`; clears and blurs the address bar when chrome is enabled
 *
 * @see hpe_engine_load_file
 */
int hpe_engine_load_blank(HpeEngine *engine);

/**
 * @brief 重新加载当前文档 / Reload the current document
 *
 * @param[in,out] engine 引擎实例，NULL 时返回 0 / Engine instance, returns 0 when NULL
 *
 * @return 加载成功返回 1，否则 0 / 1 when reloaded, otherwise 0
 *
 * @note 尚无文档路径时改为加载内置空白页 / Loads the built-in blank page when no path is set
 *
 * @see hpe_engine_load_file hpe_engine_load_blank
 */
int hpe_engine_reload(HpeEngine *engine);

/**
 * @brief 调整窗口与像素缓冲尺寸并重排 / Resize the window and pixel buffer and relayout
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     width 新的窗口宽度（像素）/ New window width in pixels
 * @param[in]     height 新的窗口高度（像素）/ New window height in pixels
 *
 * @note 宽高非正或与当前完全一致时不做任何事；否则重设缓冲、重排并刷新状态行 / Does nothing for non-positive or unchanged dimensions; otherwise resizes the buffer, relayouts and refreshes the status line
 *
 * @see hpe_engine_load_file
 */
void hpe_engine_resize(HpeEngine *engine, int width, int height);

/**
 * @brief 按行滚动页面 / Scroll the page by a number of lines
 *
 * @param[in,out] engine 引擎实例，NULL 时直接返回 / Engine instance, returns early when NULL
 * @param[in]     lines 滚动的行数，正为下滚、负为上滚 / Number of lines to scroll, positive scrolls down and negative up
 *
 * @note 一行等于 `HPE_SCROLL_LINE_PX` 像素；滚动偏移夹到 `[0, content_height - view_height]` 后取整到整像素，使损伤矩形路径与全量重绘逐字节一致；仅在未脏时把脏标记置为 `HPE_DIRTY_SCROLL` / One line is `HPE_SCROLL_LINE_PX` pixels; the offset is clamped to `[0, content_height - view_height]` then snapped to whole pixels so the damage-rect path stays byte-identical to a full repaint; only sets the dirty flag to `HPE_DIRTY_SCROLL` when not already dirty
 *
 * @see hpe_engine_scroll_offset hpe_engine_render
 */
void hpe_engine_scroll_by(HpeEngine *engine, float lines);

/**
 * @brief 把滚轮事件投递给指针处的滚动容器 / Deliver a wheel event to the scroll container under the pointer
 *
 * @details 指针落在可滚动元素（`overflow:scroll`/`auto` 且内容溢出）上时滚动该元素，
 *          未被消费的行数继续冒泡给整页，模拟浏览器的滚动链。
 *          When the pointer is over a scrollable element (`overflow:scroll`/`auto` with
 *          overflowing content) that element scrolls and the remaining lines bubble to the
 *          page, mirroring browser scroll chaining.
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     x 视口 x（窗口坐标，含浏览器外框）/ Viewport x in window coordinates
 * @param[in]     y 视口 y（窗口坐标，含浏览器外框）/ Viewport y in window coordinates
 * @param[in]     lines 滚动的行数，正为下滚 / Lines to scroll, positive scrolls down
 *
 * @return 有元素消费该事件返回 1，否则返回 0 / 1 when an element consumed the wheel, 0 otherwise
 *
 * @see hpe_engine_scroll_by
 */
int hpe_engine_wheel_scroll(HpeEngine *engine, int x, int y, float lines);

/**
 * @brief 向页面派发滚轮事件 / Dispatch a wheel event into the page
 *
 * @param[in,out] engine 引擎实例，无 JS 上下文时返回 0 / Engine instance; 0 without a JS context
 * @param[in]     x X 坐标（窗口坐标）/ X coordinate in window space
 * @param[in]     y Y 坐标（窗口坐标）/ Y coordinate in window space
 * @param[in]     delta_x 水平滚动量（像素）/ Horizontal scroll amount in pixels
 * @param[in]     lines 垂直滚动行数，正为下滚（按 `HPE_SCROLL_LINE_PX` 换算成 `deltaY`）/ Vertical scroll in lines, positive scrolls down (converted to `deltaY` via `HPE_SCROLL_LINE_PX`)
 * @param[in]     mods `HPE_MOD_*` 修饰键掩码 / The `HPE_MOD_*` modifier mask
 *
 * @return 有监听器触发返回 1，否则 0 / 1 when any listener ran, otherwise 0
 *
 * @note 打到指针下的元素上并向上冒泡；事件对象带 `deltaX`/`deltaY`/`deltaMode`
 *       Targets the element under the pointer and bubbles; the event object carries `deltaX`/`deltaY`/`deltaMode`
 *
 * @see hpe_engine_wheel_scroll
 */
int hpe_engine_wheel_event(HpeEngine *engine, int x, int y, int delta_x, float lines, int mods);

/**
 * @brief 向页面派发键盘事件 / Dispatch a keyboard event into the page
 *
 * @param[in,out] engine 引擎实例，无 JS 上下文时返回 0 / Engine instance; 0 without a JS context
 * @param[in]     vk 虚拟键码（非可打印键）/ Virtual key code for keys that are not printable
 * @param[in]     printable 可打印字符（0 表示不是可打印键）/ The printable character, or 0 when the key is not printable
 * @param[in]     mods `HPE_MOD_*` 修饰键掩码 / The `HPE_MOD_*` modifier mask
 * @param[in]     phase 0 = `keydown`，1 = `keyup`，2 = `keypress` / 0 = `keydown`, 1 = `keyup`, 2 = `keypress`
 * @param[in]     repeat 按住重复为 1 / 1 while the key is held down
 *
 * @return 有监听器触发返回 1，否则 0 / 1 when any listener ran, otherwise 0
 *
 * @note 目标是当前获得 `:focus` 的元素，无焦点时退回 `document.body`；事件对象带
 *       `key`/`code`/`keyCode`/`repeat` 与四个修饰键
 *       The target is the element owning `:focus`, falling back to `document.body`; the event object carries `key`/`code`/`keyCode`/`repeat` and the four modifiers
 */
int hpe_engine_key_event(HpeEngine *engine, int vk, int printable, int mods, int phase,
                         int repeat);

/**
 * @brief 向页面派发一次 `scroll` 事件 / Dispatch a `scroll` event into the page
 *
 * @param[in,out] engine 引擎实例，无 JS 上下文时返回 0 / Engine instance; 0 without a JS context
 * @param[in,out] target 滚动发生的元素；传 `NULL` 表示文档 / The element that scrolled, or `NULL` for the document
 *
 * @return 有监听器触发返回 1，否则 0 / 1 when any listener ran, otherwise 0
 *
 * @note `scroll` 不冒泡，因此只在目标元素上触发 / `scroll` does not bubble, so it fires on the target only
 */
int hpe_engine_scroll_event(HpeEngine *engine, HpeDomNode *target);

/**
 * @brief 派发一次鼠标按下 / 抬起 / Deliver a mouse press/release
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     x X 坐标（窗口坐标）/ X coordinate in window space
 * @param[in]     y Y 坐标（窗口坐标）/ Y coordinate in window space
 * @param[in]     button 鼠标按键（0 = 左键）/ Mouse button (0 = left)
 * @param[in]     down 按下为 1、抬起为 0 / 1 when pressed, 0 when released
 *
 * @return 页面发生变更返回 1（引擎已重排，窗口应重绘），否则 0 / 1 when the page mutated (the engine already relaid-out and the window should repaint), otherwise 0
 *
 * @note 点击落在地址栏条带内或页面点击使地址栏失焦时同样返回 1；等价于修饰键掩码为 0 的 `hpe_engine_mouse_event_mods` / Also returns 1 for a click inside the chrome strip or when a page click blurs the address bar; equivalent to `hpe_engine_mouse_event_mods` with a zero modifier mask
 *
 * @see hpe_engine_mouse_event_mods hpe_engine_mouse_move
 */
int hpe_engine_mouse_event(HpeEngine *engine, int x, int y, int button, int down);

/**
 * @brief 处理指针移动：更新 `:hover` 状态并触发 `mousemove` 监听器 / Handle a pointer move: update `:hover` state and fire `mousemove` listeners
 *
 * @param[in,out] engine 引擎实例，NULL 或无盒树时返回 0 / Engine instance, returns 0 when NULL or without a box tree
 * @param[in]     x X 坐标（窗口坐标）/ X coordinate in window space
 * @param[in]     y Y 坐标（窗口坐标）/ Y coordinate in window space
 *
 * @return 需要重绘返回 1（悬停变化或有监听器改动了页面），否则 0 / 1 when a repaint is needed (hover changed or a listener mutated the page), otherwise 0
 *
 * @note 悬停变化总是强制重排（`:hover` 是选择器，须重新级联）；否则仅在监听器产生变更时惰性重排；地址栏拖选进行中时优先交给地址栏 / A hover change always forces a relayout (`:hover` is a selector and needs a fresh cascade); otherwise a listener-driven change is relaid out lazily; a live address-bar drag is handed off to the bar first
 *
 * @see hpe_engine_mouse_event hpe_engine_tick
 */
int hpe_engine_mouse_move(HpeEngine *engine, int x, int y);

/**
 * @brief 泵到期定时器与 requestAnimationFrame 队列 / Drain due timers and the requestAnimationFrame queue
 *
 * @param[in,out] engine 引擎实例，NULL 时返回 0 / Engine instance, returns 0 when NULL
 *
 * @return 有回调运行或地址栏插入符闪烁相位翻转返回 1（页面应重绘），否则 0 / 1 when a callback ran or the address caret's blink phase flipped (the page should be repainted), otherwise 0
 *
 * @note 每轮只处理已到期的回调，期间新增的留待下一轮；插入符时钟独立于 JS 上下文，无脚本时同样闪烁（仅整帧/地址栏脏路径会重绘） / Only callbacks already due are run; ones added during the pass wait for the next round; the caret clock runs independently of the JS context, so blinking works without scripts (repaints ride the full-frame / chrome-only dirty paths)
 *
 * @see hpe_engine_mouse_move
 */
int hpe_engine_tick(HpeEngine *engine);

/**
 * @brief 按脏标记把引擎绘制到像素缓冲 / Paint the engine into its pixel buffer according to the dirty flag
 *
 * @param[in,out] engine 引擎实例，NULL 时直接返回 / Engine instance, returns early when NULL
 *
 * @note 无变更且已绘制过时整帧跳过，使无实际效果的 DOM 变更免费；仅脏地址栏时只重画地址栏，仅滚动时走局部位移重绘，否则整帧重绘；用后取 `hpe_engine_pixmap` 获取结果 / Skips the whole frame when nothing changed and a frame exists, making a no-op DOM mutation free; repaints only the bar when just the chrome is dirty, does a shift-based partial repaint for scroll-only damage, otherwise repaints the whole frame; fetch the result with `hpe_engine_pixmap`
 *
 * @see hpe_engine_pixmap hpe_engine_scroll_by
 */
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

/**
 * @brief 为引擎启用自绘地址栏 / Enable the engine's self-drawn address bar
 *
 * @param[in,out] engine 引擎实例，NULL 或已启用时直接返回 / Engine instance, returns early when NULL or already enabled
 *
 * @note 启用后视口变窄，页面在地址栏下方排版并接收相对自身的鼠标坐标；已排版则重排以重新夹紧滚动，否则标记整帧重绘 / Enabling narrows the viewport: the page lays out below the bar and receives mouse coordinates relative to itself; a laid-out document is relaid to re-clamp the scroll, otherwise the whole frame is marked dirty
 *
 * @see hpe_engine_chrome_height
 */
void hpe_engine_enable_chrome(HpeEngine *engine);

/**
 * @brief 取地址栏高度 / Get the address-bar height
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @return 地址栏高度（像素），未启用时为 0 / Address-bar height in pixels, 0 when disabled
 *
 * @note 窗口坐标减去该值即得页面视口坐标 / Subtracting this from a window coordinate gives the page viewport coordinate
 *
 * @see hpe_engine_enable_chrome
 */
int hpe_engine_chrome_height(HpeEngine *engine);

/**
 * @brief 取地址栏当前文本 / Get the address bar's current text
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @return 地址文本（UTF-8），`engine` 为 NULL 时返回空串 / The address text in UTF-8, an empty string when `engine` is NULL
 */
const char *hpe_engine_address_text(HpeEngine *engine);

/**
 * @brief 查询地址栏是否获得焦点 / Query whether the address bar is focused
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @return 聚焦返回 1，否则 0 / 1 when focused, otherwise 0
 *
 * @see hpe_engine_key
 */
int hpe_engine_address_focused(HpeEngine *engine);

/**
 * @brief 取地址栏选区的字节范围 / Get the byte range of the selected address text
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[out]    start 接收选区起始字节偏移，可为 NULL / Receives the selection start byte offset, may be NULL
 * @param[out]    end 接收选区结束字节偏移，可为 NULL / Receives the selection end byte offset, may be NULL
 *
 * @return 选取非空返回 1，否则 0 / 1 when the selection is non-empty, otherwise 0
 *
 * @note 空选区（只有插入符）返回 0 且不写出任何输出，与 omnibox 一致 / An empty selection (only a caret) returns 0 and writes nothing, exactly like the omnibox
 */
int hpe_engine_address_selection(HpeEngine *engine, int *start, int *end);

/**
 * @brief 取插入符在地址文本中的字节偏移 / Get the byte offset of the insertion point (the caret) in the address text
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @return 插入符字节偏移，`engine` 为 NULL 时返回 0 / Caret byte offset, 0 when `engine` is NULL
 *
 * @see hpe_engine_address_selection
 */
int hpe_engine_address_caret(HpeEngine *engine);

/**
 * @brief 取客户端区当前应显示的光标 / Get the cursor currently indicated for the client area
 *
 * @param[in]     engine 引擎实例 / Engine instance
 *
 * @return `HpeCursor` 枚举值，`engine` 为 NULL 时返回 `HPE_CURSOR_DEFAULT` / An `HpeCursor` value, `HPE_CURSOR_DEFAULT` when `engine` is NULL
 *
 * @note 由指针移动事件更新：地址栏条带内输入框为 I-beam、条带其余处为箭头；页面上取命中盒解析出的 CSS `cursor`（含 UA 默认：链接为 pointer、按钮为 default），文本盒回退为 I-beam；平台层可在窗口回调 `query_cursor` 中读取它映射为系统光标 / Updated by pointer-move events: I-beam inside the address field, the arrow elsewhere in the strip; over the page the hit box's resolved CSS `cursor` is used (with UA defaults: pointer for links, default for buttons), falling back to the I-beam for text boxes; the platform layer may map it to a system cursor from the `query_cursor` window callback
 *
 * @see hpe_engine_mouse_move
 */
int hpe_engine_page_cursor(HpeEngine *engine);

/**
 * @brief 查询地址栏插入符此刻是否可见 / Query whether the address-bar caret is visible right now
 *
 * @param[in]     engine 引擎实例 / Engine instance
 *
 * @return 插入符可见返回 1，否则 0 / 1 when the caret is visible, otherwise 0
 *
 * @note 地址栏聚焦、选区折叠且处在 0.5s 可见相位时返回 1；`engine` 为 NULL 时返回 0 / 1 when the bar is focused, the selection is collapsed and the 0.5s visible phase holds; 0 when `engine` is NULL
 *
 * @see hpe_engine_tick hpe_engine_address_focused
 */
int hpe_engine_caret_visible(HpeEngine *engine);

/* ---------------- navigation history (the chrome's back/forward buttons) ---------------- */

/**
 * @brief 取导航历史项数 / Get the number of navigation history entries
 *
 * @param[in]     engine 引擎实例 / Engine instance
 *
 * @return 历史项数，`engine` 为 NULL 时返回 0 / Entry count, 0 when `engine` is NULL
 *
 * @note 仅在启用地址栏时记录 / Recorded only while chrome is enabled
 *
 * @see hpe_engine_nav_pos
 */
int hpe_engine_nav_count(HpeEngine *engine);

/**
 * @brief 取当前历史项下标 / Get the index of the current history entry
 *
 * @param[in]     engine 引擎实例 / Engine instance
 *
 * @return 当前下标（0 起），无历史或 `engine` 为 NULL 时返回 -1 / Current index (zero based), -1 when there is no history or `engine` is NULL
 *
 * @see hpe_engine_nav_count hpe_engine_back
 */
int hpe_engine_nav_pos(HpeEngine *engine);

/**
 * @brief 带修饰键地派发一次鼠标按下 / 抬起 / Deliver a mouse press/release with modifiers
 *
 * @param[in,out] engine 引擎实例，NULL 或无盒树时返回 0 / Engine instance, returns 0 when NULL or without a box tree
 * @param[in]     x X 坐标（窗口坐标）/ X coordinate in window space
 * @param[in]     y Y 坐标（窗口坐标）/ Y coordinate in window space
 * @param[in]     button 鼠标按键（0 = 左键；当前实现未使用）/ Mouse button (0 = left; currently unused by the implementation)
 * @param[in]     down 按下为 1、抬起为 0 / 1 when pressed, 0 when released
 * @param[in]     mods `HPE_MOD_*` 修饰键掩码 / The `HPE_MOD_*` modifier mask
 *
 * @return 触发监听器或使地址栏失焦返回 1，否则 0 / 1 when a listener fired or the address bar was blurred, otherwise 0
 *
 * @note 与 `hpe_engine_mouse_event` 相同，但额外携带修饰键掩码：Shift+点击扩展地址栏选区 / Same as `hpe_engine_mouse_event` but carries a modifier mask: Shift+click extends the address-bar selection
 *
 * @see hpe_engine_mouse_event
 */
int hpe_engine_mouse_event_mods(HpeEngine *engine, int x, int y, int button, int down, int mods);

/**
 * @brief 无修饰键地向地址栏送入一个按键 / Feed one key stroke to the chrome with no modifier
 *
 * @details 接受的键包括可打印码点，以及 `HPE_KEY_BACKSPACE`、`HPE_KEY_DELETE`、`HPE_KEY_LEFT`/`RIGHT`/`HOME`/`END`、`HPE_KEY_RETURN`（导航到所输入的路径）、`HPE_KEY_ESCAPE`（回退到该字段获得焦点时的地址，然后失焦），以及 `HPE_KEY_PASTE`/`COPY`/`CUT`/`SELECT_ALL` 快捷键。
 *          Accepted keys are printable code points plus `HPE_KEY_BACKSPACE`, `HPE_KEY_DELETE`, `HPE_KEY_LEFT`/`RIGHT`/`HOME`/`END`, `HPE_KEY_RETURN` (navigates to the typed path), `HPE_KEY_ESCAPE` (restores the address the field got focus with, then blurs) and the `HPE_KEY_PASTE`/`COPY`/`CUT`/`SELECT_ALL` shortcuts.
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     key 可打印码点（Unicode）或 `HPE_KEY_*` 键码 / A printable code point (Unicode) or an `HPE_KEY_*` key code
 *
 * @return 地址栏消费了该输入返回 1，否则 0 / 1 when the chrome consumed the input, otherwise 0
 *
 * @note 所有键都先作用于选区，与普通文本框一致：输入或退格替换选区，方向键折叠选区，Ctrl+C/X 对选区操作；等价于修饰键掩码为 0 的 `hpe_engine_key_mods` / All of them act on the selection first, like any text field: typing or backspace replaces it, arrows collapse it, and Ctrl+C/X operate on it; equivalent to `hpe_engine_key_mods` with a zero modifier mask
 *
 * @see hpe_engine_key_mods hpe_engine_address_caret
 */
int hpe_engine_key(HpeEngine *engine, int key);

/**
 * @brief 带修饰键地把按键送给地址栏 / Send a key stroke with modifiers to the address bar
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     key 可打印码点（Unicode）或 `HPE_KEY_*` 键码 / A printable code point (Unicode) or an `HPE_KEY_*` key code
 * @param[in]     mods `HPE_MOD_*` 修饰键掩码 / The `HPE_MOD_*` modifier mask
 *
 * @return 地址栏消费了该输入返回 1，否则 0 / 1 when the chrome consumed the input, otherwise 0
 *
 * @note 与 `hpe_engine_key` 相同，但额外携带修饰键掩码：Shift 扩展选区、Ctrl 按单词移动 / Same as `hpe_engine_key` but carries a modifier mask: Shift extends the selection and Ctrl moves by word
 *
 * @see hpe_engine_key
 */
int hpe_engine_key_mods(HpeEngine *engine, int key, int mods);

/**
 * @brief 取引擎的像素缓冲（上一帧结果）/ Get the engine's pixel buffer (the last frame)
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @return 像素缓冲指针，`engine` 为 NULL 时返回 `NULL` / Pointer to the pixel buffer, `NULL` when `engine` is NULL
 *
 * @note 须在 `hpe_engine_render` 之后读取，直到下次 `hpe_engine_resize` 前有效 / Read it after `hpe_engine_render`; valid until the next `hpe_engine_resize`
 *
 * @see hpe_engine_render
 */
const HpePixmap *hpe_engine_pixmap(HpeEngine *engine);

/**
 * @brief 取状态行文本 / Get the status-line text
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @return 状态文本（含文件名、尺寸、加载/绘制耗时、滚动位置，及粘性加载错误），`engine` 为 NULL 时返回空串 / The status text (file name, size, load/paint timings, scroll position and any sticky load error), an empty string when `engine` is NULL
 *
 * @see hpe_engine_load_file
 */
const char *hpe_engine_status(HpeEngine *engine);

/**
 * @brief 取当前纵向滚动偏移 / Get the current vertical scroll offset
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @return 滚动偏移（像素，整像素取整后），`engine` 为 NULL 时返回 0 / Scroll offset in pixels (snapped to whole pixels), 0 when `engine` is NULL
 *
 * @see hpe_engine_scroll_by
 */
float hpe_engine_scroll_offset(HpeEngine *engine);

/* ---------------- dynamic element state (state pseudo-classes) ---------------- */

/**
 * @brief 设置拥有焦点的 DOM 节点 / Set the DOM node that owns the focus
 *
 * @param[in,out] engine 引擎实例，NULL 时直接返回 / Engine instance, returns immediately when NULL
 * @param[in,out] node 新的焦点元素，传 NULL 清除焦点 / New focus element, NULL clears the focus
 *
 * @note `:focus` 是选择器，因此焦点变化会触发一次重排并刷新状态行；`node` 与当前焦点相同时不做任何事 / `:focus` is a selector, so a change triggers a relayout and refreshes the status line; nothing happens when `node` already owns the focus
 *
 * @see hpe_engine_focused_node hpe_engine_set_target
 */
void hpe_engine_set_focus(HpeEngine *engine, HpeDomNode *node);

/**
 * @brief 取当前拥有焦点的 DOM 节点 / Get the DOM node that currently owns the focus
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @return 焦点元素，无焦点或 `engine` 为 NULL 时返回 `NULL` / The focused element, or `NULL` when nothing is focused or `engine` is `NULL`
 *
 * @see hpe_engine_set_focus
 */
HpeDomNode *hpe_engine_focused_node(HpeEngine *engine);

/**
 * @brief 设置 URL 片段目标元素 / Set the URL-fragment target element
 *
 * @param[in,out] engine 引擎实例，NULL 时直接返回 / Engine instance, returns immediately when NULL
 * @param[in,out] node 新的片段目标元素，传 NULL 表示无目标 / New fragment target, NULL clears it
 *
 * @note `:target` 是选择器，因此变化会触发一次重排 / `:target` is a selector, so a change triggers a relayout
 *
 * @see hpe_engine_set_focus
 */
void hpe_engine_set_target(HpeEngine *engine, HpeDomNode *node);

/* ---------------- document metadata ---------------- */

/**
 * @brief 取上次加载文档所用的字符编码 / Get the charset the last loaded document was decoded from
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @return 编码名（`utf-8`、`latin1`、`utf-16le`、`utf-16be`），`engine` 为 NULL 时返回空串 / The charset label (`utf-8`, `latin1`, `utf-16le`, `utf-16be`), an empty string when `engine` is NULL
 *
 * @note 直到下次加载前有效 / Valid until the next load
 *
 * @see hpe_engine_meta_charset
 */
const char *hpe_engine_document_encoding(HpeEngine *engine);

/**
 * @brief 查询编码是否来自文档内 `<meta charset>` / Query whether the charset came from an in-document `<meta charset>`
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @return 来自文档内 `<meta charset>` 返回 1，否则 0 / 1 when it came from an in-document `<meta charset>`, otherwise 0
 *
 * @note 与来自 BOM 或 UTF-8 校验的情况相区分 / Distinguishes it from a BOM or UTF-8 validation
 *
 * @see hpe_engine_document_encoding
 */
int hpe_engine_meta_charset(HpeEngine *engine);

/**
 * @brief 取解析后的 `<base href>` / Get the resolved `<base href>`
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @return 绝对基址，文档未声明时返回空串 / The absolute base URL, an empty string when the document declares none
 */
const char *hpe_engine_base_href(HpeEngine *engine);

/**
 * @brief 取非 stylesheet 的 `<link>` 资源数 / Get the number of non-stylesheet `<link>` resources
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @return 记录的 `<link>` 资源提示个数（icon/preload/alternate 等），`engine` 为 NULL 时返回 0 / Number of recorded `<link>` resource hints (icon/preload/alternate ...), 0 when `engine` is NULL
 *
 * @note stylesheet 链接已被消费、不计入 / Stylesheet links are consumed and are not counted
 *
 * @see hpe_engine_link_at
 */
int hpe_engine_link_count(HpeEngine *engine);

/**
 * @brief 取某条 `<link>` 资源提示的归一化 rel 与解析后的绝对 href / Get a `<link>` resource hint's normalised rel token and resolved absolute href
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     index 资源提示下标（0 起）/ Resource hint index (zero based)
 * @param[out]    rel 接收归一化（小写、空格分隔）的 rel token 串，可为 NULL / Receives the normalised (lowercase, space-separated) rel token string, may be NULL
 * @param[out]    href 接收解析后的绝对地址，可为 NULL / Receives the resolved absolute URL, may be NULL
 *
 * @return `index` 有效返回 1，越界返回 0 / 1 for a valid `index`, 0 when out of range
 *
 * @note `index` 越界时不写出任何输出 / Nothing is written when `index` is out of range
 *
 * @see hpe_engine_link_count
 */
int hpe_engine_link_at(HpeEngine *engine, int index, const char **rel, const char **href);

#endif /* HOPE_H */
