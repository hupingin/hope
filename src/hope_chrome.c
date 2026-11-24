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

/**
 * @brief 计算地址栏输入框的矩形 / Compute the address-field rectangle
 *
 * @param[in]     engine 引擎实例，提供窗口宽度 / Engine instance, supplies the window width
 *
 * @return 输入框在设备坐标下的矩形 / The field rectangle in device coordinates
 *
 * @note 横向居中留边，纵向固定为窗口顶部条带内 / Padded horizontally and placed in the fixed strip at the top
 *
 * @see addr_layout
 */

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

/**
 * @brief 一次性解析绘制与命中测试所需的几何 / Resolve the geometry the painter and hit testing need in one pass
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[out]    L 接收排版结果的布局结构 / Layout struct that receives the result
 *
 * @return 可用返回 1，否则 0 / 1 when usable, otherwise 0
 * @retval     1 已填好字段、字体、基线、文本宽度 / Field, font, baseline and text width are filled in
 * @retval     0 无地址栏、无字体或文本视口过窄 / No chrome, no font, or the text viewport is too narrow
 *
 * @note 失败时 `L` 被清零，调用方须据此放弃绘制 / On failure `L` is zeroed and the caller must skip layout-dependent work
 *
 * @see field_rect addr_clamp_scroll
 */
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

/**
 * @brief 求文本前 `index` 字节的步进宽度 / Measure the advance width of the first `index` bytes of the text
 *
 * @param[in,out] font 用于测量的字体 / Font used for measuring
 * @param[in]     text 文本缓冲区 / Text buffer
 * @param[in]     len 文本总字节数 / Total text length in bytes
 * @param[in]     index 要累加到哪个字节偏移（夹到 `len`）/ Byte offset to accumulate up to, clamped to `len`
 *
 * @return 前缀的步进宽度（像素）/ Advance width of the prefix in pixels
 *
 * @note 按码点逐个累加，与 `hpe_font_measure` 在任意前缀上一致 / Sums code point by code point, matching `hpe_font_measure` on every prefix
 *
 * @see addr_index_at
 */
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

/**
 * @brief 求最接近设备 x 的插入符字节偏移 / Find the caret byte offset closest to a device x
 *
 * @param[in,out] engine 引擎实例，提供地址文本与滚动量 / Engine instance, provides the address text and scroll offset
 * @param[in]     L 地址栏布局 / Address-bar layout
 * @param[in]     x X 坐标 / X coordinate
 *
 * @return 插入符所在字节偏移 / Byte offset of the caret position
 *
 * @note 以字形步进的一半为界判断归属左半还是右半 / Decides by the midpoint of each glyph advance
 *
 * @see addr_width_to
 */
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

/**
 * @brief 让插入符保持在输入框可视范围内 / Keep the caret inside the field's viewport
 *
 * @param[in,out] engine 引擎实例，读写 `addr_scroll` / Engine instance, reads and writes `addr_scroll`
 * @param[in]     L 地址栏布局 / Address-bar layout
 *
 * @note 失焦时改为显示地址尾部（浏览器行为）；拖过边缘与在末尾输入都靠它滚动 / A blurred field shows the tail of the address (browser behaviour); this is what scrolls on drag-past-edge and typing at the end
 *
 * @see addr_layout
 */
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

/**
 * @brief 取选区起始字节偏移 / Get the selection's start byte offset
 *
 * @param[in]     engine 引擎实例 / Engine instance
 *
 * @return 锚点与插入符中较小者 / The smaller of anchor and caret
 *
 * @see addr_sel_end addr_has_selection
 */

static int addr_sel_start(const HpeEngine *engine)
{
    return engine->addr_anchor < engine->addr_caret ? engine->addr_anchor : engine->addr_caret;
}

/**
 * @brief 取选区结束字节偏移 / Get the selection's end byte offset
 *
 * @param[in]     engine 引擎实例 / Engine instance
 *
 * @return 锚点与插入符中较大者 / The larger of anchor and caret
 *
 * @see addr_sel_start
 */
static int addr_sel_end(const HpeEngine *engine)
{
    return engine->addr_anchor < engine->addr_caret ? engine->addr_caret : engine->addr_anchor;
}

/**
 * @brief 判断是否存在选区 / Test whether there is a selection
 *
 * @param[in]     engine 引擎实例 / Engine instance
 *
 * @return 锚点与插入符不同返回 1，否则 0 / 1 when anchor differs from caret, otherwise 0
 */
static int addr_has_selection(const HpeEngine *engine)
{
    return engine->addr_anchor != engine->addr_caret;
}

/**
 * @brief 把插入符移到指定字节偏移 / Move the caret to a byte offset
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     index 目标字节偏移，夹到 `[0, addr_len]` / Target byte offset, clamped to `[0, addr_len]`
 * @param[in]     extend 非零时保留锚点以扩展选区 / Non-zero keeps the anchor to extend the selection
 *
 * @see addr_has_selection
 */
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

/**
 * @brief 求前一个码点的起始字节偏移 / Find the byte offset of the previous code point
 *
 * @param[in]     text 文本缓冲区 / Text buffer
 * @param[in]     i 当前字节偏移 / Current byte offset
 *
 * @return 前一个码点起点，已在开头时返回 0 / The previous code point's start, 0 when already at the beginning
 */

static int addr_prev_index(const char *text, int i)
{
    if (i <= 0)
        return 0;
    return i - hpe_utf8_prev(text, text + i);
}

/**
 * @brief 求后一个码点的起始字节偏移 / Find the byte offset of the next code point
 *
 * @param[in]     text 文本缓冲区 / Text buffer
 * @param[in]     i 当前字节偏移 / Current byte offset
 * @param[in]     len 文本总字节数 / Total text length in bytes
 *
 * @return 后一个码点起点，已到末尾时返回 `len` / The next code point's start, `len` when already at the end
 *
 * @note 解码失败时至少前进 1 字节，保证循环收敛 / Advances at least one byte on a decode failure so loops terminate
 */
static int addr_next_index(const char *text, int i, int len)
{
    uint32_t cp;
    int n;
    if (i >= len)
        return len;
    n = hpe_utf8_next(text + i, &cp);
    return i + (n > 0 ? n : 1);
}

/**
 * @brief 取某字节偏移处的码点 / Read the code point at a byte offset
 *
 * @param[in]     text 文本缓冲区 / Text buffer
 * @param[in]     i 字节偏移 / Byte offset
 *
 * @return 该处码点；越界或已到 NUL 时返回 0 / The code point there, 0 when out of range or at the NUL
 */
static uint32_t cp_at(const char *text, int i)
{
    uint32_t cp = 0;
    if (i < 0 || !text[i])
        return 0;
    hpe_utf8_next(text + i, &cp);
    return cp;
}

/**
 * @brief 判断码点是否属于单词 / Test whether a code point counts as part of a word
 *
 * @param[in]     cp Unicode 码点 / Unicode code point
 *
 * @return 属于单词返回 1，否则 0 / 1 when it is part of a word, otherwise 0
 *
 * @note ASCII 字母数字、`_` 以及任意非 ASCII 码点都算单词，使中文路径段成为一整词，与 Chrome 的选词一致 / ASCII alphanumerics, `_` and any non-ASCII code point count as a word, so a Chinese path segment is one word like Chrome's word selection
 *
 * @see addr_word_at
 */
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

/**
 * @brief 求左侧单词边界 / Find the word boundary to the left
 *
 * @param[in]     text 文本缓冲区 / Text buffer
 * @param[in]     i 起始字节偏移 / Starting byte offset
 *
 * @return 向左跨过一个单词后的字节偏移 / Byte offset after moving left across one word
 *
 * @note 先跳过单词前的分隔符，再跨过单词本身 / Skips the delimiters before the word, then the word itself
 *
 * @see is_word_cp addr_word_right
 */
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

/**
 * @brief 求右侧单词边界 / Find the word boundary to the right
 *
 * @param[in]     text 文本缓冲区 / Text buffer
 * @param[in]     len 文本总字节数 / Total text length in bytes
 * @param[in]     i 起始字节偏移 / Starting byte offset
 *
 * @return 向右跨过一个单词后的字节偏移 / Byte offset after moving right across one word
 *
 * @note 先跳过单词后的分隔符，再跨过单词本身 / Skips the delimiters after the word, then the word itself
 *
 * @see is_word_cp addr_word_left
 */
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

/**
 * @brief 求某偏移两侧同类码点的连续区间 / Find the run of same-class code points around an offset
 *
 * @param[in]     text 文本缓冲区 / Text buffer
 * @param[in]     len 文本总字节数 / Total text length in bytes
 * @param[in]     i 目标字节偏移 / Target byte offset
 * @param[out]    out_a 区间起始字节偏移 / Start byte offset of the run
 * @param[out]    out_b 区间结束字节偏移 / End byte offset of the run
 *
 * @note 供双击选词使用；空文本时两个输出都为 0 / Used for double-click word selection; both outputs are 0 for empty text
 *
 * @see is_word_cp
 */
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

/**
 * @brief 替换地址栏文本并把插入符移到末尾 / Replace the address-bar text and put the caret at the end
 *
 * @param[in,out] engine 引擎实例，NULL 时直接返回 / Engine instance, returns early when NULL
 * @param[in]     text 新文本，NULL 视为空串 / New text, NULL is treated as an empty string
 *
 * @note 同时记录为 `addr_committed`，即按 Escape 时回退到的地址；超长则按缓冲容量截断 / Also stored as `addr_committed`, the address Escape falls back to; longer input is truncated to the buffer capacity
 *
 * @see hpe_chrome_navigate
 */

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

/**
 * @brief 删除字节区间并把插入符移到区间起点 / Delete a byte range and put the caret at its start
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     a 区间起始字节偏移 / Start byte offset
 * @param[in]     b 区间结束字节偏移 / End byte offset
 *
 * @return 实际删除返回 1，区间为空或越界返回 0 / 1 when something was removed, 0 for an empty or out-of-range span
 *
 * @see addr_delete_selection
 */
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

/**
 * @brief 删除当前选区 / Delete the current selection
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @return 删除了内容返回 1，否则 0 / 1 when something was removed, otherwise 0
 *
 * @see addr_delete_range addr_has_selection
 */
static int addr_delete_selection(HpeEngine *engine)
{
    return addr_delete_range(engine, addr_sel_start(engine), addr_sel_end(engine));
}

/**
 * @brief 在插入符处插入文本并替换选区 / Insert text at the caret, replacing the selection
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     text 待插入的 UTF-8 文本 / UTF-8 text to insert
 * @param[in]     len 待插入的字节数 / Number of bytes to insert
 *
 * @return 插入成功返回 1，否则 0 / 1 when inserted, otherwise 0
 * @retval     1 文本已写入，插入符停在插入内容之后 / The text was written and the caret sits after it
 * @retval     0 无输入或缓冲已满 / No input or the buffer is full
 *
 * @note 缓冲写满时按码点边界截断，绝不切断 UTF-8 序列 / Truncates on a code point boundary when the buffer is full, never splitting a UTF-8 sequence
 */
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

/**
 * @brief 清洗粘贴文本中的控制字符与首尾空白 / Sanitise pasted text of control characters and surrounding blanks
 *
 * @param[in]     src 原始粘贴文本 / Raw pasted text
 *
 * @return 清洗后的新缓冲区，调用方负责释放 / The cleaned buffer, owned by the caller
 *
 * @note 剪贴板常带结尾换行，多行拷贝也不能把换行偷偷带进路径，因此丢弃换行、制表符等控制字符并去除首尾空格 / Clipboards often carry a trailing newline and multi-line copies must not smuggle a newline into a path, so line breaks, tabs and other control characters are dropped and surrounding spaces trimmed
 *
 * @see addr_paste
 */
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

/**
 * @brief 粘贴剪贴板文本到插入符处 / Paste clipboard text at the caret
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @return 插入了内容返回 1，否则 0 / 1 when something was inserted, otherwise 0
 *
 * @note 先经 `sanitize_paste` 清洗再插入 / Cleans the text through `sanitize_paste` before inserting
 *
 * @see sanitize_paste addr_insert_at_caret
 */
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

/**
 * @brief 复制或剪切选区到剪贴板 / Copy or cut the selection to the clipboard
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     cut 非零表示剪切 / Non-zero means cut instead of copy
 *
 * @return 写入了剪贴板或执行了剪切返回 1，否则 0 / 1 when the clipboard was written or a cut happened, otherwise 0
 *
 * @note 无选区时 Ctrl+C 复制整条地址（剪切则不做任何事），与 Chrome 一致 / With no selection Ctrl+C copies the whole address (cut does nothing), matching Chrome
 *
 * @see addr_delete_selection
 */
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

/**
 * @brief 在基线上绘制一段文本 / Draw one text run on the baseline
 *
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in,out] font 字体 / Font
 * @param[in]     text 待绘制的 UTF-8 文本 / UTF-8 text to draw
 * @param[in]     len 文本字节数 / Number of bytes
 * @param[in]     x 绘制起点 X（设备坐标）/ Starting X in device coordinates
 * @param[in]     baseline 文本基线的设备 Y 坐标 / Device Y of the text baseline
 * @param[in]     color 文本颜色 / Text colour
 *
 * @note 取覆盖率掩码后调用 `hpe_draw_text_mask` 混合；空段或取掩码失败时直接返回 / Fetches the coverage mask and blends via `hpe_draw_text_mask`; returns early for an empty run or a failed mask
 */

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

/**
 * @brief 绘制输入框文本（占位提示或选区加文本）/ Paint the field's text, either the hint or the selection plus runs
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     L 地址栏布局 / Address-bar layout
 * @param[in]     text_color 正文颜色 / Body text colour
 * @param[in]     hint_color 占位提示颜色 / Placeholder hint colour
 * @param[in]     sel_bg 选区背景色 / Selection background colour
 * @param[in]     sel_fg 选区前景色 / Selection foreground colour
 *
 * @note 失焦时不画高亮；有选区时先画高亮圆角矩形，再把选区画成 `sel_fg` 以保持字形清晰，两侧各画一段 `text_color` / An unfocused field shows no highlight; with a selection it paints the highlight rounded rect first, then the selected run in `sel_fg` so the glyphs stay crisp, flanked by two `text_color` runs
 *
 * @see chrome_paint_caret draw_run
 */
static void chrome_paint_field_text(HpeEngine *engine, HpePixmap *pm, const AddrLayout *L,
                                    HpeColor text_color, HpeColor hint_color,
                                    HpeColor sel_bg, HpeColor sel_fg)
{
    float scroll = engine->addr_scroll;
    int sel_a = addr_sel_start(engine);
    int sel_b = addr_sel_end(engine);

    if (engine->addr_len == 0) {
        draw_run(pm, L->font, PLACEHOLDER, (int)strlen(PLACEHOLDER), L->inner_x, L->baseline,
                 hint_color);
        return;
    }
    if (sel_b > sel_a && engine->addr_focus) {
        /* an unfocused field shows no highlight (Chrome clears it on blur).
         * highlight first, then three text runs: the selected one in white so
         * the glyphs stay crisp on top of the selection colour */
        float x0 = L->inner_x - scroll +
                   addr_width_to(L->font, engine->addr, engine->addr_len, sel_a);
        float x1 = L->inner_x - scroll +
                   addr_width_to(L->font, engine->addr, engine->addr_len, sel_b);
        int ix0 = (int)floorf(x0);
        int ix1 = (int)ceilf(x1);

        if (ix1 <= ix0)
            ix1 = ix0 + 1;
        hpe_fill_round_rect(pm,
                            hpe_rect((float)ix0, L->field.y + 5.0f, (float)(ix1 - ix0),
                                     L->field.h - 10.0f),
                            SEL_RADIUS, sel_bg);
        draw_run(pm, L->font, engine->addr, sel_a, L->inner_x - scroll, L->baseline, text_color);
        draw_run(pm, L->font, engine->addr + sel_a, sel_b - sel_a, x0, L->baseline, sel_fg);
        draw_run(pm, L->font, engine->addr + sel_b, engine->addr_len - sel_b, x1, L->baseline,
                 text_color);
        return;
    }
    draw_run(pm, L->font, engine->addr, engine->addr_len, L->inner_x - scroll, L->baseline,
             text_color);
}

/**
 * @brief 在插入点绘制插入符 / Paint the caret at the insertion point
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     L 地址栏布局 / Address-bar layout
 * @param[in]     color 插入符颜色 / Caret colour
 *
 * @note 有选区或未聚焦时不绘制（与 Chrome 一致）；插入符位置夹在文本视口内 / Not drawn when there is a selection or the field is unfocused (matching Chrome); the position is clamped to the text viewport
 *
 * @see chrome_paint_field_text
 */
static void chrome_paint_caret(HpeEngine *engine, HpePixmap *pm, const AddrLayout *L,
                               HpeColor color)
{
    float cx;
    if (!engine->addr_focus || addr_sel_end(engine) != addr_sel_start(engine))
        return;
    cx = L->inner_x - engine->addr_scroll +
         addr_width_to(L->font, engine->addr, engine->addr_len, engine->addr_caret);
    if (cx < L->inner_x)
        cx = L->inner_x;
    if (cx <= L->inner_x + L->inner_w)
        hpe_fill_rect(pm, hpe_rect(cx, L->field.y + 5.0f, 1.0f, L->field.h - 10.0f), color);
}

/**
 * @brief 绘制整条地址栏 / Paint the whole address bar
 *
 * @param[in,out] engine 引擎实例，NULL 或无地址栏时直接返回 / Engine instance, returns early when NULL or without chrome
 *
 * @note 先铺条带背景与 1 像素分隔线，再画带聚焦描边的圆角输入框，最后裁剪到文本视口绘制文本与插入符 / Paints the strip background and a 1 px separator, then the rounded field with its focus outline, then clips to the text viewport to paint the text and caret
 *
 * @see chrome_paint_field_text chrome_paint_caret
 */
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
    hpe_pixmap_set_clip(pm, hpe_rect(L.inner_x, L.field.y + 2.0f, L.inner_w, L.field.h - 4.0f));
    chrome_paint_field_text(engine, pm, &L, text_color, hint_color, sel_bg, sel_fg);
    chrome_paint_caret(engine, pm, &L, text_color);
    hpe_pixmap_reset_clip(pm);
}

/* ---------------- mouse ---------------- */

/**
 * @brief 处理输入框内的一次按下 / Handle one press inside the field
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     L 地址栏布局 / Address-bar layout
 * @param[in]     x 按下的 X 坐标（设备坐标）/ X position of the press in device coordinates
 * @param[in]     mods 修饰键掩码 / Modifier mask
 *
 * @note 统计单击/双击/三击并决定选区：首次点击全选，Shift+点击移动选区末端，双击选词，三击全选；同时记录拖拽起点 / Counts single/double/triple clicks and picks the selection: the first click selects all, Shift+click moves the selection end, double click selects a word, triple click selects all; also records the drag origin
 *
 * @see hpe_chrome_mouse addr_word_at
 */
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

/**
 * @brief 把一次鼠标事件喂给地址栏 / Feed one mouse event to the address bar
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     x X 坐标（设备坐标）/ X position in device coordinates
 * @param[in]     y Y 坐标（设备坐标）/ Y position in device coordinates
 * @param[in]     down 按下为 1、抬起为 0 / 1 when pressed, 0 when released
 * @param[in]     mods 修饰键掩码 / Modifier mask
 *
 * @return 事件被地址栏消费返回 1，否则 0 / 1 when the bar consumed the event, otherwise 0
 *
 * @note y 落在条带之外一律返回 0，交给页面处理；按下时根据是否落在输入框内决定聚焦或失焦 / Returns 0 for any y outside the strip so the page handles it; on press, focus or blur is decided by whether the point is inside the field
 *
 * @see addr_press hpe_chrome_mouse_move
 */
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

/**
 * @brief 把指针移动喂给地址栏以拖选 / Feed a pointer move to the address bar for drag selection
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     x X 坐标（设备坐标）/ X position in device coordinates
 * @param[in]     y Y 坐标（设备坐标，仅作接口占位）/ Y position in device coordinates, used only for the interface signature
 *
 * @return 选区发生变化返回 1，否则 0 / 1 when the selection changed, otherwise 0
 *
 * @note 移动超过拖拽阈值后按下点才成为锚点，因此从全选状态拖拽会转为普通区间选择 / The press point becomes the anchor only after the pointer travels past the drag threshold, so dragging from a select-all turns into an ordinary range selection
 *
 * @see hpe_chrome_mouse
 */
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

/**
 * @brief 规范化地址栏输入 / Normalise address-bar input
 *
 * @param[in,out] s 就地修改的输入文本 / Input text modified in place
 *
 * @note 去掉首尾空白、成对的引号以及可选的 `file://` 前缀（`file:///C:/...` 归一为 `C:/...`）/ Strips surrounding whitespace, a matching pair of quotes and an optional `file://` prefix (`file:///C:/...` becomes `C:/...`)
 *
 * @see hpe_chrome_navigate
 */
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

/**
 * @brief 按地址栏内容导航 / Navigate using the address-bar contents
 *
 * @param[in,out] engine 引擎实例，NULL 时返回 0 / Engine instance, returns 0 when NULL
 *
 * @return 加载成功返回 1，否则 0 / 1 when the load succeeded, otherwise 0
 * @retval     1 已加载目标文件，或输入为空时加载空白页 / The target file was loaded, or a blank page when the input was empty
 * @retval     0 加载失败，地址文本保留以便用户修正 / The load failed; the typed text is kept so the user can fix it
 *
 * @note 失败时错误已写入状态行（显示在窗口标题中），本函数只把状态标为需重绘 / On failure the error already lives in the status line (shown in the window title), so this only marks the chrome dirty
 *
 * @see normalize_input hpe_chrome_set_text
 */
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

/**
 * @brief 发送一个无修饰键的按键 / Send one key stroke with no modifier
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     key 按键码或码点 / Key code or code point
 *
 * @return 与 `hpe_engine_key_mods` 相同 / Same as `hpe_engine_key_mods`
 *
 * @note 等价于修饰键掩码为 0 的调用 / Equivalent to calling with a zero modifier mask
 *
 * @see hpe_engine_key_mods
 */
int hpe_engine_key(HpeEngine *engine, int key)
{
    return hpe_engine_key_mods(engine, key, 0);
}

/**
 * @brief 处理退格与删除键 / Handle the Backspace and Delete keys
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     key 按键码（`HPE_KEY_*`）/ Key code (`HPE_KEY_*`)
 * @param[in]     ctrl 非零时按单词删除 / Non-zero deletes by word
 *
 * @return 地址内容改变返回 1，否则 0 / 1 when the address changed, otherwise 0
 *
 * @note 有选区时先删选区，否则删插入符两侧的字符或单词 / Deletes the selection first when there is one, otherwise the character or word on either side of the caret
 *
 * @see addr_key_move
 */
static int addr_key_delete(HpeEngine *engine, int key, int ctrl)
{
    int has_sel = addr_has_selection(engine);
    if (has_sel)
        return addr_delete_selection(engine);
    if (key == HPE_KEY_BACKSPACE && engine->addr_caret > 0)
        return addr_delete_range(
            engine,
            ctrl ? addr_word_left(engine->addr, engine->addr_caret)
                 : addr_prev_index(engine->addr, engine->addr_caret),
            engine->addr_caret);
    if (key == HPE_KEY_DELETE && engine->addr_caret < engine->addr_len)
        return addr_delete_range(
            engine, engine->addr_caret,
            ctrl ? addr_word_right(engine->addr, engine->addr_len, engine->addr_caret)
                 : addr_next_index(engine->addr, engine->addr_caret, engine->addr_len));
    return 0;
}

/**
 * @brief 处理左右与行首行尾键 / Handle the Left, Right, Home and End keys
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     key 按键码（`HPE_KEY_*`）/ Key code (`HPE_KEY_*`)
 * @param[in]     ctrl 非零时按单词移动 / Non-zero moves by word
 * @param[in]     shift 非零时扩展选区而非折叠 / Non-zero extends the selection instead of collapsing it
 *
 * @return 固定返回 1 / Always returns 1
 *
 * @note 不带 Shift 且已有选区时，左右键折叠到选区相应端点 / Without Shift and with a selection, Left/Right collapse to the respective selection end
 *
 * @see addr_set_caret addr_key_delete
 */
static int addr_key_move(HpeEngine *engine, int key, int ctrl, int shift)
{
    int collapse = !shift && addr_has_selection(engine);
    switch (key) {
    case HPE_KEY_LEFT:
        addr_set_caret(engine,
                       collapse ? addr_sel_start(engine)
                                : (ctrl ? addr_word_left(engine->addr, engine->addr_caret)
                                        : addr_prev_index(engine->addr, engine->addr_caret)),
                       shift);
        return 1;
    case HPE_KEY_RIGHT:
        addr_set_caret(engine,
                       collapse ? addr_sel_end(engine)
                                : (ctrl ? addr_word_right(engine->addr, engine->addr_len,
                                                          engine->addr_caret)
                                        : addr_next_index(engine->addr, engine->addr_caret,
                                                          engine->addr_len)),
                       shift);
        return 1;
    case HPE_KEY_HOME:
        addr_set_caret(engine, 0, shift);
        return 1;
    default: /* HPE_KEY_END */
        addr_set_caret(engine, engine->addr_len, shift);
        return 1;
    }
}

/**
 * @brief 按键处理中的编辑部分 / The editing half of key handling
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     key 按键码或码点 / Key code or code point
 * @param[in]     ctrl Control 按下为 1 / 1 when Control is held
 * @param[in]     shift Shift 按下为 1 / 1 when Shift is held
 *
 * @return 地址内容改变返回 1，否则 0 / 1 when the address changed, otherwise 0
 *
 * @note 分发退格/删除、移动、全选、复制/剪切/粘贴；其余可打印码点直接插入 / Dispatches Backspace/Delete, movement, select-all, copy/cut/paste; any other printable code point is inserted directly
 *
 * @see addr_key_delete addr_key_move
 */
static int addr_key_edit(HpeEngine *engine, int key, int ctrl, int shift)
{
    switch (key) {
    case HPE_KEY_BACKSPACE:
    case HPE_KEY_DELETE:
        return addr_key_delete(engine, key, ctrl);
    case HPE_KEY_LEFT:
    case HPE_KEY_RIGHT:
    case HPE_KEY_HOME:
    case HPE_KEY_END:
        return addr_key_move(engine, key, ctrl, shift);
    case HPE_KEY_SELECT_ALL:
        engine->addr_anchor = 0;
        engine->addr_caret = engine->addr_len;
        engine->addr_drag_live = 0;
        return 1;
    case HPE_KEY_COPY:
        return addr_copy(engine, 0);
    case HPE_KEY_CUT:
        return addr_copy(engine, 1);
    case HPE_KEY_PASTE:
        return addr_paste(engine);
    default:
        if (key >= 32 && key <= 0x10FFFF) {
            char enc[4];
            int n = hpe_utf8_encode((uint32_t)key, enc);
            return addr_insert_at_caret(engine, enc, n);
        }
        return 0;
    }
}

/**
 * @brief 带修饰键地把按键送给地址栏 / Send a key stroke with modifiers to the address bar
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     key 按键码或码点 / Key code or code point
 * @param[in]     mods 修饰键掩码 / Modifier mask
 *
 * @return 按键被地址栏消费返回 1，否则 0 / 1 when the bar consumed the key, otherwise 0
 *
 * @note 回车触发导航并保持聚焦；Escape 回退到获得焦点时的地址（`addr_committed`）后失焦；其余交给编辑逻辑处理 / Return triggers navigation and keeps focus; Escape restores the address the field got focus with (`addr_committed`) and blurs; the rest goes through the editing logic
 *
 * @see addr_key_edit hpe_chrome_navigate
 */
int hpe_engine_key_mods(HpeEngine *engine, int key, int mods)
{
    int shift = (mods & HPE_MOD_SHIFT) != 0;
    int ctrl = (mods & HPE_MOD_CTRL) != 0;
    int changed;

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

    changed = addr_key_edit(engine, key, ctrl, shift);

    engine->addr_drag = 0;
    engine->addr_drag_live = 0;
    if (!changed)
        return 0;
    engine->dirty = HPE_DIRTY_CHROME;
    return 1;
}

/**
 * @brief 为引擎启用自绘地址栏 / Enable the engine's self-drawn address bar
 *
 * @param[in,out] engine 引擎实例，NULL 或已启用时直接返回 / Engine instance, returns early when NULL or already enabled
 *
 * @note 启用后视口变窄：已排版则重排以重新夹紧滚动，否则标记整帧重绘 / Enabling narrows the viewport: a laid-out document is relaid to re-clamp the scroll, otherwise the whole frame is marked dirty
 *
 * @see hpe_engine_chrome_height
 */

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

/**
 * @brief 取地址栏高度 / Get the address-bar height
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @return 地址栏高度（像素），未启用时为 0 / Address-bar height in pixels, 0 when disabled
 *
 * @see hpe_engine_enable_chrome
 */
int hpe_engine_chrome_height(HpeEngine *engine)
{
    return engine ? engine->chrome_h : 0;
}

/**
 * @brief 取地址栏当前文本 / Get the address bar's current text
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @return 地址文本，`engine` 为 NULL 时返回空串 / The address text, an empty string when `engine` is NULL
 */
const char *hpe_engine_address_text(HpeEngine *engine)
{
    return engine ? engine->addr : "";
}

/**
 * @brief 查询地址栏是否获得焦点 / Query whether the address bar is focused
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @return 聚焦返回 1，否则 0 / 1 when focused, otherwise 0
 */
int hpe_engine_address_focused(HpeEngine *engine)
{
    return engine ? engine->addr_focus : 0;
}

/**
 * @brief 取插入符字节偏移 / Get the caret's byte offset
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @return 插入符字节偏移，`engine` 为 NULL 时返回 0 / Caret byte offset, 0 when `engine` is NULL
 */
int hpe_engine_address_caret(HpeEngine *engine)
{
    return engine ? engine->addr_caret : 0;
}

/**
 * @brief 取地址栏选区范围 / Get the address bar's selection range
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[out]    start 接收选区起始字节偏移，可为 NULL / Receives the selection start byte offset, may be NULL
 * @param[out]    end 接收选区结束字节偏移，可为 NULL / Receives the selection end byte offset, may be NULL
 *
 * @return 有选区返回 1，否则 0 / 1 when there is a selection, otherwise 0
 *
 * @note 无选区时不写出任何输出 / Writes no output when there is no selection
 *
 * @see addr_has_selection
 */
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
