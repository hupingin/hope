/* Hope UI Engine - memory, string, utf8 and css value helpers */
#ifndef HPE_UTIL_H
#define HPE_UTIL_H

#include "base/hpe_types.h"

/**
 * @brief 分配一块堆内存 / Allocate a block of heap memory
 *
 * @param[in]     size 请求的字节数（0 视为 1）/ Requested byte count, 0 is treated as 1
 *
 * @return 指向新内存的指针 / Pointer to the new block
 *
 * @note 分配失败时打印错误并 abort，绝不返回 NULL / Prints an error and aborts on failure, never returns NULL
 *
 * @see hpe_calloc hpe_free
 */
void *hpe_alloc(size_t size);

/**
 * @brief 分配并清零一块堆内存 / Allocate a zeroed block of heap memory
 *
 * @param[in]     count 元素个数（0 视为 1）/ Element count, 0 is treated as 1
 * @param[in]     size 单个元素字节数（0 视为 1）/ Byte size of one element, 0 is treated as 1
 *
 * @return 指向已清零内存的指针 / Pointer to the zeroed block
 *
 * @note 分配失败时打印错误并 abort，绝不返回 NULL / Prints an error and aborts on failure, never returns NULL
 */
void *hpe_calloc(size_t count, size_t size);

/**
 * @brief 调整已分配内存块的大小 / Resize an allocated memory block
 *
 * @param[in,out] ptr 原内存块，可为 NULL / Previous block, may be NULL
 * @param[in]     size 新字节数（0 视为 1）/ New byte count, 0 is treated as 1
 *
 * @return 指向新内存块的指针 / Pointer to the resized block
 *
 * @warning 失败时 abort，因此调用方无需检查 NULL / Aborts on failure, so callers need not check for NULL
 */
void *hpe_realloc(void *ptr, size_t size);

/**
 * @brief 复制字符串到新分配的堆内存 / Duplicate a string onto the heap
 *
 * @param[in]     s 以 NUL 结尾的源字符串 / NUL-terminated source string
 *
 * @return 新分配的副本，调用方负责释放 / Newly allocated copy owned by the caller
 */
char *hpe_strdup(const char *s);

/**
 * @brief 复制字符串的前 n 个字节 / Duplicate the first n bytes of a string
 *
 * @param[in]     s 源字符串 / Source string
 * @param[in]     n 最多复制的字节数 / Maximum number of bytes to copy
 *
 * @return 新分配且以 NUL 结尾的副本 / Newly allocated NUL-terminated copy
 *
 * @note 遇到 `\0` 提前结束；结果长度取 n 与 strlen(s) 的较小者 / Stops early at `\0`; the copy is min(n, strlen(s)) bytes long
 */
char *hpe_strndup(const char *s, size_t n);

/**
 * @brief 释放引擎分配的内存 / Free memory allocated by the engine
 *
 * @param[in,out] ptr 待释放的指针，可为 NULL / Pointer to free, may be NULL
 */
void hpe_free(void *ptr);

/**
 * @brief 按需倍增数组容量 / Grow an array's capacity by repeated doubling
 *
 * @param[in,out] ptr 原数组，可为 NULL / Previous array, may be NULL
 * @param[out]    cap 当前容量，写入新容量 / Current capacity, receives the new one
 * @param[in]     need 需要的元素个数 / Required element count
 * @param[in]     elem_size 单个元素字节数 / Byte size of one element
 *
 * @return 必要时重新分配后的数组指针 / Reallocated array pointer when growth was needed
 *
 * @note 容量不足时从 8 起翻倍直到覆盖 need；已够用时原样返回 / Doubles from 8 until `need` fits; returns the input untouched when it already does
 */
void *hpe_grow(void *ptr, size_t *cap, size_t need, size_t elem_size);

/**
 * @brief 把 ASCII 大写字母转为小写 / Lowercase an ASCII letter
 *
 * @param[in]     c 输入字符 / Input character
 *
 * @return 小写形式，非大写字母原样返回 / Lowercased character, other inputs unchanged
 */
int hpe_ascii_lower(int c);

/**
 * @brief 忽略大小写比较两个字符串 / Compare two strings case-insensitively
 *
 * @param[in]     a 第一个字符串，可为 NULL / First string, may be NULL
 * @param[in]     b 第二个字符串，可为 NULL / Second string, may be NULL
 *
 * @return 相等返回 1 / 1 when equal
 *
 * @note 两个指针同为 NULL 时视为相等 / Two NULL pointers compare equal
 */
int hpe_str_ieq(const char *a, const char *b);

/**
 * @brief 忽略大小写比较两个字符串的前 n 个字节 / Compare the first n bytes of two strings case-insensitively
 *
 * @param[in]     a 第一个字符串 / First string
 * @param[in]     b 第二个字符串 / Second string
 * @param[in]     n 比较的字节数 / Number of bytes to compare
 *
 * @return 前 n 个字节都相同返回 1 / 1 when the first n bytes match
 */
int hpe_str_ieq_n(const char *a, const char *b, size_t n);

/**
 * @brief 就地去除字符串首尾空白 / Trim leading and trailing whitespace in place
 *
 * @param[in,out] s 可修改的字符串，可为 NULL / Mutable string, may be NULL
 *
 * @return 去掉前导空白后的起始指针 / Pointer past the leading whitespace
 *
 * @warning 会写入 s 的内容，不可传字符串字面量 / Writes into `s`, so never pass a string literal
 */
char *hpe_trim(char *s);                    /* trims in place, returns s */

/**
 * @brief 判断字符是否为 ASCII 空白 / Test whether a character is ASCII whitespace
 *
 * @param[in]     c 输入字符 / Input character
 *
 * @return 是空白返回 1 / 1 for whitespace
 *
 * @note 空白集合为空格、`\t`、`\n`、`\r`、`\f`、`\v` / The set is space, `\t`, `\n`, `\r`, `\f` and `\v`
 */
int hpe_is_space(char c);

/**
 * @brief 忽略大小写判断字符串是否以某前缀开头 / Test whether a string starts with a prefix, case-insensitively
 *
 * @param[in]     s 待检查字符串 / String to inspect
 * @param[in]     prefix 前缀 / Prefix to look for
 *
 * @return 以前缀开头返回 1 / 1 when the prefix matches
 */
int hpe_starts_with_i(const char *s, const char *prefix);

/* ---------- utf-8 ---------- */
/**
 * @brief 解码下一个 UTF-8 码点 / Decode the next UTF-8 code point
 *
 * @param[in]     s 指向码点起始字节 / Points at the first byte of the code point
 * @param[out]    cp 解码出的码点 / Decoded code point
 *
 * @return 消耗的字节数（至少 1）/ Number of bytes consumed, at least 1
 *
 * @note 非法序列写出 U+FFFD 并只前进 1 字节，因此不会死循环 / Illegally encoded sequences yield U+FFFD and advance one byte, so the caller never loops
 */
int hpe_utf8_next(const char *s, uint32_t *cp);

/**
 * @brief 向前回退一个 UTF-8 码点 / Step back one UTF-8 code point
 *
 * @param[in]     begin 缓冲区起始指针 / Start of the buffer
 * @param[in]     cur 当前指针 / Current pointer
 *
 * @return 回退消耗的字节数，已在起始处返回 0 / Bytes to step back, 0 when already at `begin`
 *
 * @note 只扫描 continuation 字节（`0b10xxxxxx`）/ Only continuation bytes are scanned
 */
int hpe_utf8_prev(const char *begin, const char *cur);

/**
 * @brief 把一个码点编码为 UTF-8 / Encode a code point as UTF-8
 *
 * @param[in]     cp Unicode 码点 / Unicode code point
 * @param[out]    out 至少 4 字节的输出缓冲 / Output buffer of at least 4 bytes
 *
 * @return 写出的字节数（1~4）/ Number of bytes written, 1 to 4
 *
 * @note 不做范围校验，码点大于 U+10FFFF 时按 4 字节写入 / Range is not validated; code points above U+10FFFF are still written as 4 bytes
 */
int hpe_utf8_encode(uint32_t cp, char out[4]);

/* ---------- css values ---------- */
typedef enum HpeLenUnit {
    HPE_LEN_AUTO = 0,
    HPE_LEN_PX,
    HPE_LEN_PCT,
    HPE_LEN_EM,  /* relative to the element's own font-size */
    HPE_LEN_REM, /* relative to the root font-size */
    HPE_LEN_VW,
    HPE_LEN_VH,
    HPE_LEN_CH,  /* advance width of the "0" glyph */
    HPE_LEN_PT   /* points, resolved to px at parse time as px via `pt_to_px` */
} HpeLenUnit;

typedef struct HpeLength {
    float value;
    HpeLenUnit unit;
} HpeLength;

/**
 * @brief 构造 `auto` 长度 / Build an `auto` length
 *
 * @return 单位为 `HPE_LEN_AUTO` 的长度值 / A length whose unit is `HPE_LEN_AUTO`
 */
HpeLength hpe_len_auto(void);

/**
 * @brief 构造像素长度 / Build a pixel length
 *
 * @param[in]     v 像素值 / Value in pixels
 *
 * @return 单位为 `HPE_LEN_PX` 的长度值 / A length whose unit is `HPE_LEN_PX`
 */
HpeLength hpe_len_px(float v);

/**
 * @brief 构造百分比长度 / Build a percentage length
 *
 * @param[in]     v 百分比数值（未除 100）/ Percentage value, not divided by 100
 *
 * @return 单位为 `HPE_LEN_PCT` 的长度值 / A length whose unit is `HPE_LEN_PCT`
 */
HpeLength hpe_len_pct(float v);

/**
 * @brief 按指定单位构造长度 / Build a length with an explicit unit
 *
 * @param[in]     v 数值 / Numeric value
 * @param[in]     unit 长度单位 / Length unit
 *
 * @return 构造出的长度值 / The constructed length
 */
HpeLength hpe_len_unit(float v, HpeLenUnit unit);

/**
 * @brief 解析 CSS 长度字面量 / Parse a CSS length literal
 *
 * @param[in]     s 长度文本，可为 NULL / Length text, may be NULL
 *
 * @return 解析出的长度值 / The parsed length
 *
 * @note 支持 `auto`、`%`、`px`、`em`、`rem`、`vw`、`vh`、`ch`、`pt`、`pc`、`in`、`cm`、`mm`、`q`；物理单位按 96dpi 立即折算为 px，无法识别时按 px 处理 / Accepts `auto`, `%`, `px`, `em`, `rem`, `vw`, `vh`, `ch`, `pt`, `pc`, `in`, `cm`, `mm` and `q`; physical units become px at 96dpi and unrecognised suffixes fall back to px
 */
HpeLength hpe_len_parse(const char *s);

/**
 * @brief 把长度按基准值求值为像素 / Resolve a length against a single base value
 *
 * @param[in]     len 待求值长度 / Length to resolve
 * @param[in]     base 百分比基准（通常为包含块宽度）/ Percentage base, normally the containing block width
 *
 * @return 像素值；`auto` 返回 -1 / Pixel value, or -1 for `auto`
 *
 * @note 相对单位（em/rem/vw/vh/ch）一律按 0 上下文求值，等价于传全 0 给 hpe_len_resolve_full / Relative units are resolved with an all-zero context
 *
 * @see hpe_len_resolve_full
 */
float hpe_len_resolve(HpeLength len, float base);

/**
 * @brief 带完整上下文地把长度求值为像素 / Resolve a length to pixels with a full context
 *
 * @param[in]     len 待求值长度 / Length to resolve
 * @param[in]     base 百分比基准（包含块宽度或高度）/ Percentage base
 * @param[in]     font_size 元素自身字号，供 em 使用（0 视为 16）/ Element font size for em, 0 falls back to 16
 * @param[in]     root_font_size 根元素字号，供 rem 使用（0 视为 16）/ Root font size for rem, 0 falls back to 16
 * @param[in]     vw 视口宽度，供 vw 使用 / Viewport width for vw
 * @param[in]     vh 视口高度，供 vh 使用 / Viewport height for vh
 * @param[in]     ch_width `0` 字形步进宽度，供 ch 使用 / Advance width of the `0` glyph for ch
 *
 * @return 像素值；`auto` 返回 -1 / Pixel value, or -1 for `auto`
 *
 * @note 缺省上下文时 em/rem 退回 16px、vw/vh 退回 16px、ch 退回半字号 / With a missing context em and rem fall back to 16px, vw and vh to 16px, and ch to half the font size
 */
float hpe_len_resolve_full(HpeLength len, float base, float font_size, float root_font_size,
                           float vw, float vh, float ch_width);

/**
 * @brief 判断长度是否为绝对像素值 / Test whether a length is an absolute pixel value
 *
 * @param[in]     len 待检查长度 / Length to inspect
 *
 * @return 单位为 px 返回 1 / 1 when the unit is px
 *
 * @note 解析期已把物理单位换算为 px，因此此类长度无需上下文即可求值 / Physical units were already converted at parse time, so no layout context is needed to resolve it
 */
int hpe_len_is_px(HpeLength len);

/**
 * @brief 判断长度是否为 `auto` / Test whether a length is `auto`
 *
 * @param[in]     len 待检查长度 / Length to inspect
 *
 * @return `auto` 返回 1 / 1 for `auto`
 */
int hpe_len_is_auto(HpeLength len);

/* ---------- charset ---------- */
/**
 * @brief 把 Latin-1/cp1252 字节流转换为 UTF-8 / Transcode a Latin-1/cp1252 byte stream to UTF-8
 *
 * @details `0x80`~`0x9F` 经 cp1252 表映射（弯引号、破折号、省略号、欧元符号等），其余按 Latin-1 处理。
 *          Bytes `0x80` to `0x9F` go through the cp1252 table (curly quotes, dashes, ellipsis, euro sign, ...), everything else is treated as Latin-1.
 *
 * @param[in]     src 源字节流 / Source bytes
 * @param[in]     len 源字节数 / Source byte count
 * @param[out]    out_len 结果的 UTF-8 字节数，可为 NULL / Result length in bytes, may be NULL
 *
 * @return 新分配且以 NUL 结尾的 UTF-8 字符串 / Newly allocated NUL-terminated UTF-8 string
 *
 * @note 调用方负责释放返回值 / The caller owns the returned buffer
 */
char *hpe_latin1_to_utf8(const char *src, size_t len, size_t *out_len);

/**
 * @brief 解析任意 CSS 颜色写法 / Parse any supported CSS colour notation
 *
 * @param[in]     s 颜色文本，可为 NULL / Colour text, may be NULL
 * @param[out]    out 解析出的颜色 / Parsed colour
 *
 * @return 解析成功返回 1 / 1 on success
 *
 * @note 依次尝试具名色、`#` 十六进制短/长写法、`rgb()`/`rgba()`、`hsl()`/`hsla()` / Tries named colours, short and long `#` hex, `rgb()`/`rgba()` and `hsl()`/`hsla()`
 */
int hpe_color_parse(const char *s, HpeColor *out);

#endif /* HPE_UTIL_H */
