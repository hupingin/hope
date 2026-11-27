#include "base/hpe_util.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- allocation statistics (HPE_ALLOC_STATS) ---------- */

/**
 * @brief 记账链表节点，记录一个存活分配的指针与大小 / Ledger node holding one live allocation's pointer and size
 *
 * @note 账本自身用普通 malloc 维护，绝不递归调用 hpe_alloc / The ledger itself uses plain malloc and never calls hpe_alloc recursively
 */
typedef struct HpeAllocNode {
    void *ptr;                   /**< 分配块指针 / Allocated block pointer */
    size_t size;                 /**< 分配块字节数 / Allocated size in bytes */
    struct HpeAllocNode *next;   /**< 链表下一节点 / Next node in the ledger */
} HpeAllocNode;

/** 账本头指针，新分配头插 / Ledger head, newest allocation first */
static HpeAllocNode *g_alloc_ledger = NULL;
/** 当前存活分配数 / Currently live allocation count */
static size_t g_alloc_live = 0;
/** 累计分配次数 / Total allocation count */
static size_t g_alloc_total = 0;
/** 当前存活字节数 / Currently live bytes */
static size_t g_alloc_live_bytes = 0;
/** 累计分配字节数 / Total bytes allocated */
static size_t g_alloc_total_bytes = 0;
/** 统计开关：-1 未初始化，0 关闭，1 打开 / Stats switch: -1 uninitialised, 0 off, 1 on */
static int g_alloc_stats = -1;

/**
 * @brief 进程退出时打印分配统计与泄漏汇总 / Print allocation statistics and a leak summary at process exit
 *
 * @details 通过 atexit 注册；存活分配数为 0 时打印无泄漏确认行，
 *          大于 0 时打印泄漏警告（含存活块数与字节数）。
 *          Registered via atexit; prints a no-leak confirmation when the live
 *          count is zero, otherwise a leak warning with live blocks and bytes.
 */
static void alloc_stats_report(void)
{
    int dump = g_alloc_stats >= 2; /* HPE_ALLOC_STATS=2: also list each block */
    fprintf(stderr, "[hpe-alloc] total allocations: %zu (%zu bytes)\n",
            g_alloc_total, g_alloc_total_bytes);
    if (g_alloc_live > 0) {
        const HpeAllocNode *n;
        fprintf(stderr,
                "[hpe-alloc] LEAK: %zu live allocation(s), %zu byte(s) not freed\n",
                g_alloc_live, g_alloc_live_bytes);
        if (dump)
            for (n = g_alloc_ledger; n; n = n->next)
                fprintf(stderr, "[hpe-alloc]   live block %p: %zu bytes\n", n->ptr, n->size);
    } else {
        fprintf(stderr, "[hpe-alloc] no leaks: 0 live allocation(s)\n");
    }
}

/**
 * @brief 惰性读取环境变量并初始化统计开关 / Lazily read the environment variable and initialise the stats switch
 *
 * @details 首次调用时检查 `HPE_ALLOC_STATS`，仅在值为 `1` 时启用，
 *          并注册退出报告；之后每次调用只做一次静态判断，零额外开销。
 *          Checks `HPE_ALLOC_STATS` on the first call, enables only when the
 *          value is `1` and registers the exit report; later calls are a single static branch.
 *
 * @return 统计开启返回 1，否则 0 / 1 when statistics are enabled, 0 otherwise
 */
static int alloc_stats_enabled(void)
{
    if (g_alloc_stats < 0) {
        const char *env = getenv("HPE_ALLOC_STATS");
        g_alloc_stats = (env && env[0] >= '1' && env[0] <= '9') ? env[0] - '0' : 0;
        if (g_alloc_stats)
            atexit(alloc_stats_report);
    }
    return g_alloc_stats;
}

/**
 * @brief 把一次新分配登记进账本并累加统计 / Record a new allocation in the ledger and update counters
 *
 * @param[in] ptr 分配块指针，非 NULL / Allocated block pointer, not NULL
 * @param[in] size 分配字节数 / Allocated size in bytes
 *
 * @note 账本节点分配失败时静默跳过，不影响主分配流程 / Silently skips when the ledger node cannot be allocated, never affecting the main path
 */
static void alloc_track(void *ptr, size_t size)
{
    HpeAllocNode *node = (HpeAllocNode *)malloc(sizeof(*node));
    if (!node)
        return;
    node->ptr = ptr;
    node->size = size;
    node->next = g_alloc_ledger;
    g_alloc_ledger = node;
    g_alloc_live++;
    g_alloc_total++;
    g_alloc_live_bytes += size;
    g_alloc_total_bytes += size;
}

/**
 * @brief 从账本摘除一个分配并返回其大小 / Unlink an allocation from the ledger and return its size
 *
 * @param[in]  ptr 待摘除的分配块指针 / Block pointer to unlink
 * @param[out] size_out 写入该块登记的字节数 / Receives the recorded size of the block
 *
 * @return 找到并摘除返回 1，未登记返回 0 / 1 when found and unlinked, 0 when never tracked
 */
static int alloc_untrack(const void *ptr, size_t *size_out)
{
    HpeAllocNode **pp = &g_alloc_ledger;
    while (*pp) {
        HpeAllocNode *dead = *pp;
        if (dead->ptr == ptr) {
            *pp = dead->next;
            *size_out = dead->size;
            free(dead);
            return 1;
        }
        pp = &dead->next;
    }
    return 0;
}

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
void *hpe_alloc(size_t size)
{
    void *p = malloc(size ? size : 1);
    if (!p) {
        fprintf(stderr, "hope: out of memory (%zu bytes)\n", size);
        abort();
    }
    if (alloc_stats_enabled())
        alloc_track(p, size ? size : 1);
    return p;
}

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
void *hpe_calloc(size_t count, size_t size)
{
    void *p = calloc(count ? count : 1, size ? size : 1);
    if (!p) {
        fprintf(stderr, "hope: out of memory (%zu x %zu bytes)\n", count, size);
        abort();
    }
    if (alloc_stats_enabled())
        alloc_track(p, (count ? count : 1) * (size ? size : 1));
    return p;
}

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
void *hpe_realloc(void *ptr, size_t size)
{
    void *p = realloc(ptr, size ? size : 1);
    if (!p) {
        fprintf(stderr, "hope: out of memory (%zu bytes)\n", size);
        abort();
    }
    if (alloc_stats_enabled()) {
        size_t old = 0;
        if (ptr && alloc_untrack(ptr, &old)) {
            g_alloc_live--;
            g_alloc_live_bytes -= old;
        }
        alloc_track(p, size ? size : 1);
    }
    return p;
}

/**
 * @brief 复制字符串到新分配的堆内存 / Duplicate a string onto the heap
 *
 * @param[in]     s 以 NUL 结尾的源字符串 / NUL-terminated source string
 *
 * @return 新分配的副本，调用方负责释放 / Newly allocated copy owned by the caller
 */
char *hpe_strdup(const char *s)
{
    if (!s)
        return NULL;
    return hpe_strndup(s, strlen(s));
}

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
char *hpe_strndup(const char *s, size_t n)
{
    char *p = (char *)hpe_alloc(n + 1);
    if (n)
        memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

/**
 * @brief 释放引擎分配的内存 / Free memory allocated by the engine
 *
 * @param[in,out] ptr 待释放的指针，可为 NULL / Pointer to free, may be NULL
 */
void hpe_free(void *ptr)
{
    if (ptr && g_alloc_stats > 0) {
        size_t old = 0;
        if (alloc_untrack(ptr, &old)) {
            g_alloc_live--;
            g_alloc_live_bytes -= old;
        }
    }
    free(ptr);
}

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
void *hpe_grow(void *ptr, size_t *cap, size_t need, size_t elem_size)
{
    size_t newcap;
    if (need <= *cap)
        return ptr;
    newcap = *cap ? *cap : 8;
    while (newcap < need)
        newcap *= 2;
    ptr = hpe_realloc(ptr, newcap * elem_size);
    *cap = newcap;
    return ptr;
}

/**
 * @brief 把 ASCII 大写字母转为小写 / Lowercase an ASCII letter
 *
 * @param[in]     c 输入字符 / Input character
 *
 * @return 小写形式，非大写字母原样返回 / Lowercased character, other inputs unchanged
 */
int hpe_ascii_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

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
int hpe_str_ieq(const char *a, const char *b)
{
    if (!a || !b)
        return a == b;
    while (*a && *b) {
        if (hpe_ascii_lower((unsigned char)*a) != hpe_ascii_lower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

/**
 * @brief 忽略大小写比较两个字符串的前 n 个字节 / Compare the first n bytes of two strings case-insensitively
 *
 * @param[in]     a 第一个字符串 / First string
 * @param[in]     b 第二个字符串 / Second string
 * @param[in]     n 比较的字节数 / Number of bytes to compare
 *
 * @return 前 n 个字节都相同返回 1 / 1 when the first n bytes match
 */
int hpe_str_ieq_n(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (hpe_ascii_lower((unsigned char)a[i]) != hpe_ascii_lower((unsigned char)b[i]))
            return 0;
        if (!a[i] || !b[i])
            return 0;
    }
    return 1;
}

/**
 * @brief 忽略大小写判断字符串是否以某前缀开头 / Test whether a string starts with a prefix, case-insensitively
 *
 * @param[in]     s 待检查字符串 / String to inspect
 * @param[in]     prefix 前缀 / Prefix to look for
 *
 * @return 以前缀开头返回 1 / 1 when the prefix matches
 */
int hpe_starts_with_i(const char *s, const char *prefix)
{
    return hpe_str_ieq_n(s, prefix, strlen(prefix));
}

/**
 * @brief 判断字符是否为 ASCII 空白 / Test whether a character is ASCII whitespace
 *
 * @param[in]     c 输入字符 / Input character
 *
 * @return 是空白返回 1 / 1 for whitespace
 *
 * @note 空白集合为空格、`\t`、`\n`、`\r`、`\f`、`\v` / The set is space, `\t`, `\n`, `\r`, `\f` and `\v`
 */
int hpe_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

/**
 * @brief 就地去除字符串首尾空白 / Trim leading and trailing whitespace in place
 *
 * @param[in,out] s 可修改的字符串，可为 NULL / Mutable string, may be NULL
 *
 * @return 去掉前导空白后的起始指针 / Pointer past the leading whitespace
 *
 * @warning 会写入 s 的内容，不可传字符串字面量 / Writes into `s`, so never pass a string literal
 */
char *hpe_trim(char *s)
{
    char *end;
    if (!s)
        return s;
    while (*s && hpe_is_space(*s))
        s++;
    end = s + strlen(s);
    while (end > s && hpe_is_space(end[-1]))
        end--;
    *end = '\0';
    return s;
}

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
int hpe_utf8_next(const char *s, uint32_t *cp)
{
    const unsigned char *p = (const unsigned char *)s;
    uint32_t c;
    int extra, i;

    if (p[0] < 0x80) {
        *cp = p[0];
        return 1;
    }
    if ((p[0] & 0xE0) == 0xC0) {
        c = p[0] & 0x1F;
        extra = 1;
    } else if ((p[0] & 0xF0) == 0xE0) {
        c = p[0] & 0x0F;
        extra = 2;
    } else if ((p[0] & 0xF8) == 0xF0) {
        c = p[0] & 0x07;
        extra = 3;
    } else {
        *cp = 0xFFFD;
        return 1;
    }
    for (i = 1; i <= extra; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            *cp = 0xFFFD;
            return 1;
        }
        c = (c << 6) | (uint32_t)(p[i] & 0x3F);
    }
    *cp = c;
    return extra + 1;
}

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
int hpe_utf8_prev(const char *begin, const char *cur)
{
    int n = 1;
    if (cur <= begin)
        return 0;
    cur--;
    while (cur > begin && ((unsigned char)*cur & 0xC0) == 0x80) {
        cur--;
        n++;
    }
    return n;
}

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
int hpe_utf8_encode(uint32_t cp, char out[4])
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/**
 * @brief 构造 `auto` 长度 / Build an `auto` length
 *
 * @return 单位为 `HPE_LEN_AUTO` 的长度值 / A length whose unit is `HPE_LEN_AUTO`
 */

HpeLength hpe_len_auto(void)
{
    HpeLength l;
    l.value = 0.0f;
    l.unit = HPE_LEN_AUTO;
    return l;
}

/**
 * @brief 构造像素长度 / Build a pixel length
 *
 * @param[in]     v 像素值 / Value in pixels
 *
 * @return 单位为 `HPE_LEN_PX` 的长度值 / A length whose unit is `HPE_LEN_PX`
 */
HpeLength hpe_len_px(float v)
{
    HpeLength l;
    l.value = v;
    l.unit = HPE_LEN_PX;
    return l;
}

/**
 * @brief 构造百分比长度 / Build a percentage length
 *
 * @param[in]     v 百分比数值（未除 100）/ Percentage value, not divided by 100
 *
 * @return 单位为 `HPE_LEN_PCT` 的长度值 / A length whose unit is `HPE_LEN_PCT`
 */
HpeLength hpe_len_pct(float v)
{
    HpeLength l;
    l.value = v;
    l.unit = HPE_LEN_PCT;
    return l;
}

/**
 * @brief 按指定单位构造长度 / Build a length with an explicit unit
 *
 * @param[in]     v 数值 / Numeric value
 * @param[in]     unit 长度单位 / Length unit
 *
 * @return 构造出的长度值 / The constructed length
 */
HpeLength hpe_len_unit(float v, HpeLenUnit unit)
{
    HpeLength l;
    l.value = v;
    l.unit = unit;
    return l;
}

/**
 * @brief 判断长度是否为绝对像素值 / Test whether a length is an absolute pixel value
 *
 * @param[in]     len 待检查长度 / Length to inspect
 *
 * @return 单位为 px 返回 1 / 1 when the unit is px
 *
 * @note 解析期已把物理单位换算为 px，因此此类长度无需上下文即可求值 / Physical units were already converted at parse time, so no layout context is needed to resolve it
 */
int hpe_len_is_px(HpeLength len)
{
    return len.unit == HPE_LEN_PX;
}

/**
 * @brief 判断长度是否为 `auto` / Test whether a length is `auto`
 *
 * @param[in]     len 待检查长度 / Length to inspect
 *
 * @return `auto` 返回 1 / 1 for `auto`
 */
int hpe_len_is_auto(HpeLength len)
{
    return len.unit == HPE_LEN_AUTO;
}

/**
 * @brief 把单位后缀映射为长度单位枚举 / Map a unit suffix to a length unit
 *
 * @details 后缀按最长优先匹配，避免 `vmin` 被 `vh` 抢先、`rem` 被 `em` 抢先。
 *          Suffixes are matched longest-first so that `vmin` does not shadow `vh` and `rem` does not shadow `em`.
 *
 * @param[in]     s 单位后缀，如 `rem`、`vh` / Unit suffix such as `rem` or `vh`
 * @param[out]    out 映射到的单位 / Mapped unit
 *
 * @return 识别成功返回 1 / 1 when the suffix was recognised
 *
 * @note `ex` 归入 em、`pc`/`pt` 归入 pt、`in`/`cm`/`mm`/`q` 归入 px / `ex` maps to em, `pc` and `pt` to pt, `in`/`cm`/`mm`/`q` to px
 */
static int len_unit_from_suffix(const char *s, HpeLenUnit *out)
{
    static const struct {
        const char *suffix;
        HpeLenUnit unit;
    } table[] = {
        {"rem", HPE_LEN_REM}, {"vmin", HPE_LEN_VW}, {"vmax", HPE_LEN_VW},
        {"vw", HPE_LEN_VW},   {"vh", HPE_LEN_VH},   {"ch", HPE_LEN_CH},
        {"em", HPE_LEN_EM},   {"ex", HPE_LEN_EM},   {"px", HPE_LEN_PX},
        {"pt", HPE_LEN_PT},   {"pc", HPE_LEN_PT},   {"in", HPE_LEN_PX},
        {"cm", HPE_LEN_PX},   {"mm", HPE_LEN_PX},   {"q", HPE_LEN_PX},
    };
    size_t i;
    for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (hpe_str_ieq(s, table[i].suffix)) {
            *out = table[i].unit;
            return 1;
        }
    }
    return 0;
}

/**
 * @brief 解析 CSS 长度字面量 / Parse a CSS length literal
 *
 * @param[in]     s 长度文本，可为 NULL / Length text, may be NULL
 *
 * @return 解析出的长度值 / The parsed length
 *
 * @note 支持 `auto`、`%`、`px`、`em`、`rem`、`vw`、`vh`、`ch`、`pt`、`pc`、`in`、`cm`、`mm`、`q`；物理单位按 96dpi 立即折算为 px，无法识别时按 px 处理 / Accepts `auto`, `%`, `px`, `em`, `rem`, `vw`, `vh`, `ch`, `pt`, `pc`, `in`, `cm`, `mm` and `q`; physical units become px at 96dpi and unrecognised suffixes fall back to px
 */
HpeLength hpe_len_parse(const char *s)
{
    char buf[64];
    size_t n;
    char *end = NULL;
    float v;
    HpeLenUnit unit = HPE_LEN_PX;

    if (!s)
        return hpe_len_auto();
    while (*s && hpe_is_space(*s))
        s++;
    n = strlen(s);
    if (n >= sizeof(buf))
        n = sizeof(buf) - 1;
    memcpy(buf, s, n);
    buf[n] = '\0';

    if (strncmp(buf, "auto", 4) == 0)
        return hpe_len_auto();

    v = (float)strtod(buf, &end);
    if (end == buf)
        return hpe_len_auto();
    while (*end && hpe_is_space(*end))
        end++;
    if (*end == '%')
        return hpe_len_pct(v);
    if (*end == '\0')
        return hpe_len_px(v);
    if (len_unit_from_suffix(end, &unit)) {
        if (unit == HPE_LEN_PX) {
            /* absolute physical units -> css px at 96dpi */
            if (hpe_str_ieq(end, "in"))
                return hpe_len_px(v * 96.0f);
            if (hpe_str_ieq(end, "cm"))
                return hpe_len_px(v * 96.0f / 2.54f);
            if (hpe_str_ieq(end, "mm"))
                return hpe_len_px(v * 96.0f / 25.4f);
            if (hpe_str_ieq(end, "q"))
                return hpe_len_px(v * 96.0f / 101.6f);
        }
        if (unit == HPE_LEN_PT)
            return hpe_len_px(hpe_str_ieq(end, "pc") ? v * 16.0f : v * 96.0f / 72.0f);
        return hpe_len_unit(v, unit);
    }
    /* unknown suffix: treat as px, matching the lenient parser elsewhere */
    return hpe_len_px(v);
}

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
float hpe_len_resolve(HpeLength len, float base)
{
    return hpe_len_resolve_full(len, base, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
}

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
                           float vw, float vh, float ch_width)
{
    switch (len.unit) {
    case HPE_LEN_PX:
        return len.value;
    case HPE_LEN_PCT:
        return base * len.value / 100.0f;
    case HPE_LEN_EM:
        return len.value * (font_size > 0.0f ? font_size : 16.0f);
    case HPE_LEN_REM:
        return len.value * (root_font_size > 0.0f ? root_font_size : 16.0f);
    case HPE_LEN_VW:
        return vw > 0.0f ? len.value * vw / 100.0f : len.value * 16.0f;
    case HPE_LEN_VH:
        return vh > 0.0f ? len.value * vh / 100.0f : len.value * 16.0f;
    case HPE_LEN_CH:
        /* fall back to half the font size when no glyph advance is supplied */
        return len.value * (ch_width > 0.0f ? ch_width : (font_size > 0.0f ? font_size * 0.5f
                                                                           : 8.0f));
    case HPE_LEN_AUTO:
    default:
        return -1.0f;
    }
}

/* ---------- colors ---------- */

typedef struct {
    const char *name;
    uint32_t rgb;
    uint8_t a;
} HpeNamedColor;

/* the full CSS Color 4 named-color table (148 entries incl. both grey spellings) */
static const HpeNamedColor g_named_colors[] = {
    {"transparent", 0x000000, 0},
    {"aliceblue", 0xF0F8FF, 255}, {"antiquewhite", 0xFAEBD7, 255},
    {"aqua", 0x00FFFF, 255}, {"aquamarine", 0x7FFFD4, 255}, {"azure", 0xF0FFFF, 255},
    {"beige", 0xF5F5DC, 255}, {"bisque", 0xFFE4C4, 255}, {"black", 0x000000, 255},
    {"blanchedalmond", 0xFFEBCD, 255}, {"blue", 0x0000FF, 255},
    {"blueviolet", 0x8A2BE2, 255}, {"brown", 0xA52A2A, 255},
    {"burlywood", 0xDEB887, 255}, {"cadetblue", 0x5F9EA0, 255},
    {"chartreuse", 0x7FFF00, 255}, {"chocolate", 0xD2691E, 255},
    {"coral", 0xFF7F50, 255}, {"cornflowerblue", 0x6495ED, 255},
    {"cornsilk", 0xFFF8DC, 255}, {"crimson", 0xDC143C, 255}, {"cyan", 0x00FFFF, 255},
    {"darkblue", 0x00008B, 255}, {"darkcyan", 0x008B8B, 255},
    {"darkgoldenrod", 0xB8860B, 255}, {"darkgray", 0xA9A9A9, 255},
    {"darkgreen", 0x006400, 255}, {"darkgrey", 0xA9A9A9, 255},
    {"darkkhaki", 0xBDB76B, 255}, {"darkmagenta", 0x8B008B, 255},
    {"darkolivegreen", 0x556B2F, 255}, {"darkorange", 0xFF8C00, 255},
    {"darkorchid", 0x9932CC, 255}, {"darkred", 0x8B0000, 255},
    {"darksalmon", 0xE9967A, 255}, {"darkseagreen", 0x8FBC8F, 255},
    {"darkslateblue", 0x483D8B, 255}, {"darkslategray", 0x2F4F4F, 255},
    {"darkslategrey", 0x2F4F4F, 255}, {"darkturquoise", 0x00CED1, 255},
    {"darkviolet", 0x9400D3, 255}, {"deeppink", 0xFF1493, 255},
    {"deepskyblue", 0x00BFFF, 255}, {"dimgray", 0x696969, 255},
    {"dimgrey", 0x696969, 255}, {"dodgerblue", 0x1E90FF, 255},
    {"firebrick", 0xB22222, 255}, {"floralwhite", 0xFFFAF0, 255},
    {"forestgreen", 0x228B22, 255}, {"fuchsia", 0xFF00FF, 255},
    {"gainsboro", 0xDCDCDC, 255}, {"ghostwhite", 0xF8F8FF, 255},
    {"gold", 0xFFD700, 255}, {"goldenrod", 0xDAA520, 255}, {"gray", 0x808080, 255},
    {"green", 0x008000, 255}, {"greenyellow", 0xADFF2F, 255},
    {"grey", 0x808080, 255}, {"honeydew", 0xF0FFF0, 255}, {"hotpink", 0xFF69B4, 255},
    {"indianred", 0xCD5C5C, 255}, {"indigo", 0x4B0082, 255},
    {"ivory", 0xFFFFF0, 255}, {"khaki", 0xF0E68C, 255}, {"lavender", 0xE6E6FA, 255},
    {"lavenderblush", 0xFFF0F5, 255}, {"lawngreen", 0x7CFC00, 255},
    {"lemonchiffon", 0xFFFACD, 255}, {"lightblue", 0xADD8E6, 255},
    {"lightcoral", 0xF08080, 255}, {"lightcyan", 0xE0FFFF, 255},
    {"lightgoldenrodyellow", 0xFAFAD2, 255}, {"lightgray", 0xD3D3D3, 255},
    {"lightgreen", 0x90EE90, 255}, {"lightgrey", 0xD3D3D3, 255},
    {"lightpink", 0xFFB6C1, 255}, {"lightsalmon", 0xFFA07A, 255},
    {"lightseagreen", 0x20B2AA, 255}, {"lightskyblue", 0x87CEFA, 255},
    {"lightslategray", 0x778899, 255}, {"lightslategrey", 0x778899, 255},
    {"lightsteelblue", 0xB0C4DE, 255}, {"lightyellow", 0xFFFFE0, 255},
    {"lime", 0x00FF00, 255}, {"limegreen", 0x32CD32, 255}, {"linen", 0xFAF0E6, 255},
    {"magenta", 0xFF00FF, 255}, {"maroon", 0x800000, 255},
    {"mediumaquamarine", 0x66CDAA, 255}, {"mediumblue", 0x0000CD, 255},
    {"mediumorchid", 0xBA55D3, 255}, {"mediumpurple", 0x9370DB, 255},
    {"mediumseagreen", 0x3CB371, 255}, {"mediumslateblue", 0x7B68EE, 255},
    {"mediumspringgreen", 0x00FA9A, 255}, {"mediumturquoise", 0x48D1CC, 255},
    {"mediumvioletred", 0xC71585, 255}, {"midnightblue", 0x191970, 255},
    {"mintcream", 0xF5FFFA, 255}, {"mistyrose", 0xFFE4E1, 255},
    {"moccasin", 0xFFE4B5, 255}, {"navajowhite", 0xFFDEAD, 255},
    {"navy", 0x000080, 255}, {"oldlace", 0xFDF5E6, 255}, {"olive", 0x808000, 255},
    {"olivedrab", 0x6B8E23, 255}, {"orange", 0xFFA500, 255},
    {"orangered", 0xFF4500, 255}, {"orchid", 0xDA70D6, 255},
    {"palegoldenrod", 0xEEE8AA, 255}, {"palegreen", 0x98FB98, 255},
    {"paleturquoise", 0xAFEEEE, 255}, {"palevioletred", 0xDB7093, 255},
    {"papayawhip", 0xFFEFD5, 255}, {"peachpuff", 0xFFDAB9, 255},
    {"peru", 0xCD853F, 255}, {"pink", 0xFFC0CB, 255}, {"plum", 0xDDA0DD, 255},
    {"powderblue", 0xB0E0E6, 255}, {"purple", 0x800080, 255},
    {"rebeccapurple", 0x663399, 255}, {"red", 0xFF0000, 255},
    {"rosybrown", 0xBC8F8F, 255}, {"royalblue", 0x4169E1, 255},
    {"saddlebrown", 0x8B4513, 255}, {"salmon", 0xFA8072, 255},
    {"sandybrown", 0xF4A460, 255}, {"seagreen", 0x2E8B57, 255},
    {"seashell", 0xFFF5EE, 255}, {"sienna", 0xA0522D, 255},
    {"silver", 0xC0C0C0, 255}, {"skyblue", 0x87CEEB, 255},
    {"slateblue", 0x6A5ACD, 255}, {"slategray", 0x708090, 255},
    {"slategrey", 0x708090, 255}, {"snow", 0xFFFAFA, 255},
    {"springgreen", 0x00FF7F, 255}, {"steelblue", 0x4682B4, 255},
    {"tan", 0xD2B48C, 255}, {"teal", 0x008080, 255}, {"thistle", 0xD8BFD8, 255},
    {"tomato", 0xFF6347, 255}, {"turquoise", 0x40E0D0, 255},
    {"violet", 0xEE82EE, 255}, {"wheat", 0xF5DEB3, 255}, {"white", 0xFFFFFF, 255},
    {"whitesmoke", 0xF5F5F5, 255}, {"yellow", 0xFFFF00, 255},
    {"yellowgreen", 0x9ACD32, 255},
};

/**
 * @brief 把十六进制字符转成数值 / Convert a hexadecimal digit to its value
 *
 * @param[in]     c 输入字符 / Input character
 *
 * @return 0~15，非十六进制字符返回 -1 / 0 to 15, or -1 when the character is not a hex digit
 */
static int hex_val(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    c = (char)hpe_ascii_lower((unsigned char)c);
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

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

char *hpe_latin1_to_utf8(const char *src, size_t len, size_t *out_len)
{
    char *out = (char *)hpe_alloc(len * 2 + 1);
    size_t i, o = 0;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)src[i];
        uint32_t cp;
        if (c < 0x80) {
            out[o++] = (char)c;
            continue;
        }
        cp = c;
        /* windows-1252 remaps 0x80..0x9F away from the C1 control range */
        if (c >= 0x80 && c <= 0x9F) {
            static const uint16_t cp1252[32] = {
                0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
                0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};
            cp = cp1252[c - 0x80];
        }
        {
            char tmp[4];
            int n = hpe_utf8_encode(cp, tmp);
            memcpy(out + o, tmp, (size_t)n);
            o += (size_t)n;
        }
    }
    out[o] = '\0';
    if (out_len)
        *out_len = o;
    return out;
}

/**
 * @brief 解析 `#rgb` / `#rgba` 短十六进制颜色 / Parse a `#rgb` or `#rgba` short hex colour
 *
 * @param[in]     s 以 `#` 开头的颜色文本 / Colour text beginning with `#`
 * @param[in]     len 文本长度 / Text length
 * @param[out]    out 解析出的颜色 / Parsed colour
 *
 * @return 解析成功返回 1 / 1 on success
 *
 * @note 每个十六进制位重复一次展开为 8 位分量；4 位形式第 4 位为 alpha / Each digit is doubled to form a full byte; a 4-digit form carries alpha
 */
static int color_parse_hex_short(const char *s, size_t len, HpeColor *out)
{
    int v[4] = {0, 0, 0, 15};
    int digits = (int)len - 1;
    size_t i;

    for (i = 0; i < (size_t)digits; i++) {
        int h = hex_val(s[1 + i]);
        if (h < 0)
            return 0;
        v[i] = h;
    }
    out->r = (uint8_t)(v[0] * 17);
    out->g = (uint8_t)(v[1] * 17);
    out->b = (uint8_t)(v[2] * 17);
    out->a = (uint8_t)((digits == 4 ? v[3] : 15) * 17);
    return 1;
}

/**
 * @brief 解析 `#rrggbb` / `#rrggbbaa` 长十六进制颜色 / Parse a `#rrggbb` or `#rrggbbaa` long hex colour
 *
 * @param[in]     s 以 `#` 开头的颜色文本 / Colour text beginning with `#`
 * @param[in]     len 文本长度 / Text length
 * @param[out]    out 解析出的颜色 / Parsed colour
 *
 * @return 解析成功返回 1 / 1 on success
 *
 * @note 6 位形式省略 alpha 时视为不透明 / A 6-digit form implies full opacity
 */
static int color_parse_hex_long(const char *s, size_t len, HpeColor *out)
{
    uint8_t v[4] = {0, 0, 0, 255};
    int digits = (int)len - 1;
    size_t i;

    for (i = 0; i < (size_t)digits; i += 2) {
        int hi = hex_val(s[1 + i]);
        int lo = hex_val(s[2 + i]);
        if (hi < 0 || lo < 0)
            return 0;
        v[i / 2] = (uint8_t)(hi * 16 + lo);
    }
    out->r = v[0];
    out->g = v[1];
    out->b = v[2];
    out->a = (uint8_t)(digits == 8 ? v[3] : 255);
    return 1;
}

/**
 * @brief 跳过函数式颜色参数间的分隔符 / Skip the separators between functional colour arguments
 *
 * @param[in,out] pp 读写游标指针 / Read-write cursor pointer
 *
 * @note 分隔符为空格、逗号和斜杠 / The separators are space, comma and slash
 */
static void color_skip_seps(const char **pp)
{
    while (**pp == ' ' || **pp == ',' || **pp == '/')
        (*pp)++;
}

/**
 * @brief 读取 `rgb()` / `hsl()` 参数表里的数值分量 / Read the numeric components of an `rgb()` or `hsl()` argument list
 *
 * @param[in]     p 指向左括号的文本 / Text pointing at the opening parenthesis
 * @param[out]    comp 分量数组，容量至少 4 / Component array of at least 4 slots
 * @param[out]    pct 每个分量是否带百分号 / Percent-sign flag per component
 *
 * @return 实际读到的分量个数（0~4）/ Number of components read, 0 to 4
 *
 * @note 带 `%` 的 rgb 分量按 255 折算，第 4 个分量（alpha）按 1.0 折算 / Percent-suffixed rgb components scale to 255, while the fourth (alpha) scales to 1.0
 */
static int color_read_components(const char *p, float *comp, int *pct)
{
    int idx = 0;

    if (!p)
        return 0;
    p++; /* ( */
    while (*p && *p != ')' && idx < 4) {
        char *end = NULL;
        float v;

        color_skip_seps(&p);
        if (!*p || *p == ')')
            break;
        v = (float)strtod(p, &end);
        if (end == p)
            break;
        if (*end == '%') {
            pct[idx] = 1;
            if (idx == 3)
                v /= 100.0f;
            else
                v = v * 255.0f / 100.0f;
            end++;
        }
        comp[idx++] = v;
        p = end;
    }
    return idx;
}

/**
 * @brief 把浮点数夹到 [0, 1] / Clamp a float into [0, 1]
 *
 * @param[in]     v 输入值 / Input value
 *
 * @return 夹取后的值 / The clamped value
 */
static float clamp01(float v)
{
    if (v < 0.0f)
        return 0.0f;
    if (v > 1.0f)
        return 1.0f;
    return v;
}

/**
 * @brief 把浮点数夹到 [0, 255] 并转为字节 / Clamp a float into [0, 255] and narrow it to a byte
 *
 * @param[in]     v 输入值 / Input value
 *
 * @return 夹取并取整后的字节 / The clamped byte
 */
static uint8_t clamp_byte(float v)
{
    if (v < 0.0f)
        return 0;
    if (v > 255.0f)
        return 255;
    return (uint8_t)v;
}

/**
 * @brief 解析 `rgb()` 颜色函数 / Parse an `rgb()` colour function
 *
 * @param[in]     s 以 `rgb(` 开头的颜色文本 / Colour text beginning with `rgb(`
 * @param[out]    out 解析出的颜色 / Parsed colour
 *
 * @return 解析成功返回 1 / 1 on success
 *
 * @note 支持 `rgb(r, g, b)`、`rgb(r, g, b, a)` 以及各分量的百分比写法 / Accepts `rgb(r, g, b)`, `rgb(r, g, b, a)` and percentage forms
 */
static int color_parse_rgb(const char *s, HpeColor *out)
{
    const char *p = strchr(s, '(');
    float comp[4] = {0, 0, 0, 1.0f};
    int pct[4] = {0, 0, 0, 0};
    int idx = color_read_components(p, comp, pct);

    if (!p || idx < 3)
        return 0;
    out->r = clamp_byte(comp[0]);
    out->g = clamp_byte(comp[1]);
    out->b = clamp_byte(comp[2]);
    if (idx >= 4)
        out->a = (uint8_t)(comp[3] <= 1.0f ? comp[3] * 255.0f : comp[3]);
    else
        out->a = 255;
    HPE_UNUSED(pct);
    return 1;
}

/**
 * @brief 把色相扇区映射为 HSL 转 RGB 所需的色度三元组 / Map a hue sextant to the chroma triple used by the HSL-to-RGB conversion
 *
 * @param[in]     sextant 色相扇区 0~5 / Hue sextant, 0 to 5
 * @param[in]     c 色度 / Chroma
 * @param[in]     x 中间分量 / Intermediate component
 * @param[out]    r 红色分量 / Red component
 * @param[out]    g 绿色分量 / Green component
 * @param[out]    b 蓝色分量 / Blue component
 */
static void hsl_sextant_rgb(int sextant, float c, float x, float *r, float *g, float *b)
{
    switch (sextant) {
    case 0: *r = c; *g = x; *b = 0.0f; break;
    case 1: *r = x; *g = c; *b = 0.0f; break;
    case 2: *r = 0.0f; *g = c; *b = x; break;
    case 3: *r = 0.0f; *g = x; *b = c; break;
    case 4: *r = x; *g = 0.0f; *b = c; break;
    default: *r = c; *g = 0.0f; *b = x; break;
    }
}

/**
 * @brief 读取 `hsl()` 参数表里的数值分量 / Read the numeric components of an `hsl()` argument list
 *
 * @param[in]     p 指向左括号的文本 / Text pointing at the opening parenthesis
 * @param[out]    comp 分量数组，容量至少 4 / Component array of at least 4 slots
 *
 * @return 实际读到的分量个数（0~4）/ Number of components read, 0 to 4
 *
 * @note 带 `%` 的分量就地除以 100，得到 0~1 的归一值 / Percent-suffixed components are divided by 100 into the 0..1 range
 */
static int color_read_hsl_components(const char *p, float *comp)
{
    int idx = 0;

    if (!p)
        return 0;
    p++; /* ( */
    while (*p && *p != ')' && idx < 4) {
        char *end = NULL;
        float v;

        color_skip_seps(&p);
        if (!*p || *p == ')')
            break;
        v = (float)strtod(p, &end);
        if (end == p)
            break;
        if (*end == '%') {
            v /= 100.0f;
            end++;
        }
        comp[idx++] = v;
        p = end;
    }
    return idx;
}

/**
 * @brief 解析 `hsl()` 颜色函数 / Parse an `hsl()` colour function
 *
 * @param[in]     s 以 `hsl(` 开头的颜色文本 / Colour text beginning with `hsl(`
 * @param[out]    out 解析出的颜色 / Parsed colour
 *
 * @return 解析成功返回 1 / 1 on success
 *
 * @note 支持 `hsl(h, s%, l%)` 与带 alpha 的四分量形式；色相按度取模 360 / Accepts `hsl(h, s%, l%)` and the four-component alpha form; hue wraps modulo 360 degrees
 */
static int color_parse_hsl(const char *s, HpeColor *out)
{
    const char *p = strchr(s, '(');
    float comp[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    int idx = color_read_hsl_components(p, comp);
    float hv, sv, lv, av;
    float r, g, b, c, x, m, hh;
    int sextant;

    if (!p || idx < 3)
        return 0;
    hv = comp[0];
    sv = clamp01(comp[1]);
    lv = clamp01(comp[2]);
    av = comp[3];
    hv = fmodf(hv, 360.0f);
    if (hv < 0.0f)
        hv += 360.0f;
    hh = hv / 60.0f;
    c = (1.0f - fabsf(2.0f * lv - 1.0f)) * sv;
    x = c * (1.0f - fabsf(fmodf(hh, 2.0f) - 1.0f));
    sextant = (int)hh % 6;
    hsl_sextant_rgb(sextant, c, x, &r, &g, &b);
    m = lv - c * 0.5f;
    out->r = (uint8_t)((r + m) * 255.0f + 0.5f);
    out->g = (uint8_t)((g + m) * 255.0f + 0.5f);
    out->b = (uint8_t)((b + m) * 255.0f + 0.5f);
    out->a = idx >= 4 ? (uint8_t)(av <= 1.0f ? av * 255.0f : av) : 255;
    return 1;
}

/**
 * @brief 按具名色表解析颜色 / Resolve a colour from the named-colour table
 *
 * @param[in]     s 颜色名 / Colour name
 * @param[in]     len 名称长度 / Name length
 * @param[out]    out 解析出的颜色 / Parsed colour
 *
 * @return 命中返回 1 / 1 when the name was found
 *
 * @note 表内为完整的 CSS Color 4 具名色（含两种 gray/grey 拼写）与 `transparent` / The table holds the full CSS Color 4 set including both grey spellings and `transparent`
 */
static int color_parse_named(const char *s, size_t len, HpeColor *out)
{
    size_t i;

    for (i = 0; i < sizeof(g_named_colors) / sizeof(g_named_colors[0]); i++) {
        if (strlen(g_named_colors[i].name) == len &&
            hpe_str_ieq_n(s, g_named_colors[i].name, len)) {
            uint32_t rgb = g_named_colors[i].rgb;
            out->r = (uint8_t)((rgb >> 16) & 0xFF);
            out->g = (uint8_t)((rgb >> 8) & 0xFF);
            out->b = (uint8_t)(rgb & 0xFF);
            out->a = g_named_colors[i].a;
            return 1;
        }
    }
    return 0;
}

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
int hpe_color_parse(const char *s, HpeColor *out)
{
    size_t len;

    if (!s || !out)
        return 0;
    while (*s && hpe_is_space(*s))
        s++;
    len = strlen(s);
    while (len > 0 && hpe_is_space(s[len - 1]))
        len--;
    if (len == 0)
        return 0;

    if (s[0] == '#') {
        if (len == 4 || len == 5)
            return color_parse_hex_short(s, len, out);
        if (len == 7 || len == 9)
            return color_parse_hex_long(s, len, out);
        return 0;
    }
    if (hpe_str_ieq_n(s, "rgb", 3))
        return color_parse_rgb(s, out);
    if (hpe_str_ieq_n(s, "hsl", 3))
        return color_parse_hsl(s, out);
    return color_parse_named(s, len, out);
}
