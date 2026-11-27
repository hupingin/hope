#include "render/hpe_font.h"

#include <stdlib.h>
#include <string.h>

#include "base/hpe_util.h"
#include "render/hpe_emoji.h"

/**
 * @brief 判断是否通过环境变量关闭字形掩码缓存 / Test whether the glyph-mask cache was disabled by an environment variable
 *
 * @details 读取一次 `HPE_NO_MASK_CACHE` 并缓存结果：变量存在即返回 1。
 *          `HPE_NO_MASK_CACHE` is read once and the result cached; the cache is considered disabled as soon as the variable exists.
 *
 * @return 禁用缓存返回 1，否则返回 0 / 1 when the cache is disabled, otherwise 0
 *
 * @note 供调试与基准测试绕过掩码缓存 / Debug and benchmark aid for bypassing the run-mask cache
 */
static int mask_cache_disabled(void)
{
    static int flag = -1;
    if (flag < 0)
        flag = getenv("HPE_NO_MASK_CACHE") != NULL;
    return flag;
}

#define HPE_FONT_CACHE_MAX 48
#define HPE_ADV_CACHE_SIZE 1024
#define HPE_MASK_CACHE_SIZE 1024 /* power of two; open-addressed, full flush on pressure */
#define HPE_COV_CACHE_SIZE 512   /* per-font coverage cache / 每字体覆盖缓存 */
#define HPE_FONT_FB_MAX 3        /* fallback chain length / 回退链长度 */

typedef struct {
    uint32_t codepoint;
    float advance;
} HpeAdvEntry;

typedef struct {
    uint32_t codepoint;
    uint8_t covered; /* 0 = missing glyph / 0 表示缺字 */
} HpeCovEntry;

/* Per-font glyph-run coverage mask cache. Key is a 64-bit FNV-1a hash of the
 * utf-8 bytes plus the length; the produced HpeTextMask is integer-position
 * independent (painting rounds x/y), so text content alone identifies it. */
typedef struct {
    uint64_t key; /* 0 = empty slot */
    int len;
    HpeTextMask mask; /* coverage buffer owned by the cache */
} HpeMaskEntry;

struct HpeFont {
    HpePlatFont *plat;
    float size;
    int weight;
    HpeFontFamily family;
    HpeAdvEntry *adv;
    size_t adv_size;
    HpeMaskEntry *masks;
    int masks_used;
    HpeCovEntry *cov; /* coverage cache, primary fonts only / 覆盖缓存，仅主字体 */
    int is_fb;        /* 1 = fallback font, owns no children / 1 表示后备字体，不再有子回退 */
    HpeFont *fb[HPE_FONT_FB_MAX];
    int fb_count;
};

/* hardcoded fallback chains per family (Chromium uses the same idea of a
 * small hardcoded candidate list before any per-character API fallback) */
static const char *const k_fb_names[HPE_FONT_FB_MAX] = {"Segoe UI Symbol", "Segoe UI Emoji",
                                                        "SimSun"};

static HpeFont *g_fonts[HPE_FONT_CACHE_MAX];
static int g_font_count = 0;

/**
 * @brief 定位或创建某码点的字宽缓存槽 / Locate or create the advance-cache slot of a code point
 *
 * @details 以开放寻址线性探测查找码点；`create` 为 0 且缓存未建立时直接返回 `NULL`。
 *          The code point is looked up with linear probing; when `create` is 0 and the table is not yet built it returns `NULL` immediately.
 *
 * @param[in,out] font 字体对象 / Font object
 * @param[in]     cp Unicode 码点 / Unicode code point
 * @param[in]     create 表未建立或码点缺失时是否分配空槽 / Whether to allocate a slot when the table is missing or the code point is absent
 *
 * @return 命中的槽；未命中且允许创建时返回新的空槽，否则返回 `NULL` / The matching entry, a fresh empty entry when creation is allowed, or `NULL`
 *
 * @see hpe_font_advance
 */
static HpeAdvEntry *adv_slot(HpeFont *font, uint32_t cp, int create)
{
    size_t mask, idx, i;
    if (!font->adv) {
        if (!create)
            return NULL;
        font->adv_size = HPE_ADV_CACHE_SIZE;
        font->adv = (HpeAdvEntry *)hpe_alloc(sizeof(HpeAdvEntry) * font->adv_size);
        for (i = 0; i < font->adv_size; i++) {
            font->adv[i].codepoint = 0xFFFFFFFFu;
            font->adv[i].advance = 0.0f;
        }
    }
    mask = font->adv_size - 1;
    idx = (cp * 2654435761u) & mask;
    for (i = 0; i < font->adv_size; i++) {
        HpeAdvEntry *e = &font->adv[(idx + i) & mask];
        if (e->codepoint == cp)
            return e;
        if (e->codepoint == 0xFFFFFFFFu)
            return create ? e : NULL;
    }
    return NULL;
}

/**
 * @brief 取（并缓存）指定字号的字体对象 / Get a cached font object for the given size
 *
 * @details 字号四舍五入后夹到 [6, 200]；字重非 700 一律按 400；字体族非 serif/mono 一律按 sans。相同三元组命中同一缓存对象。
 *          The pixel size is rounded and clamped to [6, 200]; any weight other than 700 becomes 400; any family other than serif or mono becomes sans. Identical triples share one cached object.
 *
 * @param[in]     pixel_size 字号（像素）/ Font size in pixels
 * @param[in]     weight 字重，仅 700 与 400 两种取值 / Font weight, either 700 or 400 after normalisation
 * @param[in]     family 字体族 / Font family
 *
 * @return 缓存的字体对象；平台创建失败时返回 `NULL` / The cached font object, or `NULL` when the platform font could not be created
 *
 * @note 缓存上限 48，超限时整体清空后重建 / The cache holds up to 48 fonts and is flushed wholesale when it overflows; do not free the result
 *
 * @see hpe_font_shutdown
 */
HpeFont *hpe_font_get(float pixel_size, int weight, HpeFontFamily family)
{
    int i;
    int px = (int)(pixel_size + 0.5f);
    HpeFont *font;

    if (px < 6)
        px = 6;
    if (px > 200)
        px = 200;
    if (weight != 700)
        weight = 400;
    if (family != HPE_FONT_SERIF && family != HPE_FONT_MONO)
        family = HPE_FONT_SANS;

    for (i = 0; i < g_font_count; i++) {
        if ((int)(g_fonts[i]->size + 0.5f) == px && g_fonts[i]->weight == weight &&
            g_fonts[i]->family == family)
            return g_fonts[i];
    }

    if (g_font_count >= HPE_FONT_CACHE_MAX) {
        g_font_count = 0; /* simple flush: page styles rarely exceed this */
    }

    font = (HpeFont *)hpe_calloc(1, sizeof(HpeFont));
    font->plat = hpe_plat_font_create((float)px, weight, family);
    font->size = (float)px;
    font->weight = weight;
    font->family = family;
    if (!font->plat) {
        hpe_free(font);
        return NULL;
    }
    g_fonts[g_font_count++] = font;
    return font;
}

/**
 * @brief 取字体已归整的字号 / Get the font's normalised pixel size
 *
 * @param[in,out] font 字体对象，可为 NULL / Font object, may be NULL
 *
 * @return 字号（像素）；`font` 为 `NULL` 返回 0 / Pixel size, or 0 when `font` is `NULL`
 */
float hpe_font_size(HpeFont *font)
{
    return font ? font->size : 0.0f;
}

/**
 * @brief 取字体已归整的字重 / Get the font's normalised weight
 *
 * @param[in,out] font 字体对象，可为 NULL / Font object, may be NULL
 *
 * @return 字重；`font` 为 `NULL` 返回 400 / The weight, or 400 when `font` is `NULL`
 */
int hpe_font_weight(HpeFont *font)
{
    return font ? font->weight : 400;
}

/**
 * @brief 取字体的上伸部高度 / Get the font's ascent
 *
 * @param[in,out] font 字体对象，可为 NULL / Font object, may be NULL
 *
 * @return 上伸部（像素）；`font` 为 `NULL` 返回 0 / Ascent in pixels, or 0 when `font` is `NULL`
 */
float hpe_font_ascent(HpeFont *font)
{
    return font ? hpe_plat_font_ascent(font->plat) : 0.0f;
}

/**
 * @brief 取字体的下伸部高度 / Get the font's descent
 *
 * @param[in,out] font 字体对象，可为 NULL / Font object, may be NULL
 *
 * @return 下伸部（像素）；`font` 为 `NULL` 返回 0 / Descent in pixels, or 0 when `font` is `NULL`
 */
float hpe_font_descent(HpeFont *font)
{
    return font ? hpe_plat_font_descent(font->plat) : 0.0f;
}

/**
 * @brief 取字体的默认行高 / Get the font's default line height
 *
 * @param[in,out] font 字体对象，可为 NULL / Font object, may be NULL
 *
 * @return 行高（像素）；`font` 为 `NULL` 返回 0 / Line height in pixels, or 0 when `font` is `NULL`
 */
float hpe_font_line_height(HpeFont *font)
{
    return font ? hpe_plat_font_line_height(font->plat) : 0.0f;
}

/**
 * @brief 在覆盖缓存中探测槽位 / Probe a slot in the coverage cache
 *
 * @param[in,out] font 字体对象 / Font object
 * @param[in]     cp Unicode 码点 / Unicode code point
 * @param[in]     create 未建立或缺失时是否分配空槽 / Whether to allocate a slot when missing
 *
 * @return 命中或新建的槽；`create` 为 0 且未建立时返回 `NULL` / The matching or new slot, or NULL when `create` is 0 and the table is absent
 */
static HpeCovEntry *cov_slot(HpeFont *font, uint32_t cp, int create)
{
    size_t mask, idx, i;
    if (!font->cov) {
        if (!create)
            return NULL;
        font->cov = (HpeCovEntry *)hpe_calloc(HPE_COV_CACHE_SIZE, sizeof(HpeCovEntry));
        for (i = 0; i < HPE_COV_CACHE_SIZE; i++)
            font->cov[i].codepoint = 0xFFFFFFFFu;
    }
    mask = HPE_COV_CACHE_SIZE - 1;
    idx = (cp * 2654435761u) & mask;
    for (i = 0; i < HPE_COV_CACHE_SIZE; i++) {
        HpeCovEntry *e = &font->cov[(idx + i) & mask];
        if (e->codepoint == cp)
            return e;
        if (e->codepoint == 0xFFFFFFFFu)
            return create ? e : NULL;
    }
    return NULL;
}

/**
 * @brief 查询字体是否覆盖码点（带缓存）/ Query font coverage of a code point, cached
 *
 * @param[in,out] font 字体对象 / Font object
 * @param[in]     cp Unicode 码点 / Unicode code point
 *
 * @return 覆盖返回 1 / 1 when covered
 */
static int font_covers(HpeFont *font, uint32_t cp)
{
    HpeCovEntry *e = cov_slot(font, cp, 1);
    if (!e)
        return hpe_plat_font_covers(font->plat, cp);
    if (e->codepoint == 0xFFFFFFFFu) {
        e->codepoint = cp;
        e->covered = (uint8_t)hpe_plat_font_covers(font->plat, cp);
    }
    return e->covered;
}

/**
 * @brief 解析某码点真正使用的字体（含回退链）/ Resolve the font that actually serves a code point
 *
 * @details 主字体覆盖该码点时返回主字体本身；否则按硬编码回退链逐个验证覆盖并
 *          返回第一个覆盖的后备字体。结果按码点缓存在主字体上。
 *          When the primary font covers the code point the primary itself is
 *          returned; otherwise the hardcoded fallback chain is probed and the
 *          first covering fallback is returned. Results are cached per code point.
 *
 * @param[in,out] font 主字体对象 / Primary font object
 * @param[in]     codepoint Unicode 码点 / Unicode code point
 *
 * @return 服务该码点的字体；`font` 为 `NULL` 返回 `NULL` / The font serving the code point, or `NULL` when `font` is `NULL`
 */
HpeFont *hpe_font_resolve(HpeFont *font, uint32_t codepoint)
{
    int i;
    if (!font || !font->plat || font->is_fb)
        return font;
    if (font_covers(font, codepoint))
        return font;
    for (i = 0; i < HPE_FONT_FB_MAX; i++) {
        if (!font->fb[i]) {
            font->fb[i] = (HpeFont *)hpe_calloc(1, sizeof(HpeFont));
            font->fb[i]->plat = hpe_plat_font_create_face(font->size, font->weight,
                                                          k_fb_names[i]);
            font->fb[i]->size = font->size;
            font->fb[i]->weight = font->weight;
            font->fb[i]->family = font->family;
            font->fb[i]->is_fb = 1;
            if (!font->fb[i]->plat) {
                hpe_free(font->fb[i]);
                font->fb[i] = NULL;
                continue;
            }
        }
        if (font_covers(font->fb[i], codepoint))
            return font->fb[i];
    }
    return font;
}

/**
 * @brief 取单个码点的步进宽度（带缓存）/ Get the advance width of a code point, cached
 *
 * @details 首次查询某码点时先解析服务字体（含回退链），再向平台取宽度并写入缓存。
 *          The serving font (fallback chain included) is resolved on the first query
 *          for a code point, the width is fetched from the platform and cached.
 *
 * @param[in,out] font 字体对象 / Font object
 * @param[in]     codepoint Unicode 码点 / Unicode code point
 *
 * @return 步进宽度（像素）；`font` 为 `NULL` 返回 0 / Advance width in pixels, or 0 when `font` is `NULL`
 *
 * @see hpe_font_measure hpe_font_resolve
 */
float hpe_font_advance(HpeFont *font, uint32_t codepoint)
{
    HpeAdvEntry *e;
    HpeFont *src;
    if (!font)
        return 0.0f;
    src = hpe_font_resolve(font, codepoint);
    if (src == font) {
        e = adv_slot(font, codepoint, 1);
        if (!e)
            return hpe_plat_font_advance(font->plat, codepoint);
        if (e->codepoint == 0xFFFFFFFFu) {
            e->codepoint = codepoint;
            e->advance = hpe_plat_font_advance(font->plat, codepoint);
        }
        return e->advance;
    }
    return hpe_font_advance(src, codepoint);
}

/**
 * @brief 测量一段 UTF-8 文本的宽度 / Measure the advance width of a UTF-8 text run
 *
 * @details 逐码点累加步进宽度，换行 `\n` 与回车 `\r` 不占宽度；非法码点处停止累加。
 *          Advance widths are summed code point by code point; `\n` and `\r` contribute nothing and an ill-formed code point stops the scan.
 *
 * @param[in,out] font 字体对象 / Font object
 * @param[in]     text UTF-8 文本 / UTF-8 text
 * @param[in]     len 文本字节数 / Text length in bytes
 *
 * @return 文本总宽度（像素）；参数无效或长度为非正数返回 0 / Total width in pixels, or 0 for invalid arguments or a non-positive length
 *
 * @see hpe_font_advance
 */
float hpe_font_measure(HpeFont *font, const char *text, int len)
{
    const char *p, *end;
    float w = 0.0f;
    if (!font || !text || len <= 0)
        return 0.0f;
    p = text;
    end = text + len;
    while (p < end) {
        uint32_t cp;
        int n = hpe_utf8_next(p, &cp);
        if (n <= 0)
            break;
        p += n;
        if (cp == '\n' || cp == '\r')
            continue;
        w += hpe_font_advance(font, cp);
    }
    return w;
}

/**
 * @brief 计算文本字节流的 FNV-1a 哈希 / Compute the FNV-1a hash of a text run
 *
 * @param[in]     text UTF-8 文本 / UTF-8 text
 * @param[in]     len 文本字节数 / Text length in bytes
 *
 * @return 64 位哈希值，绝不为 0 / 64-bit hash, never 0
 */
static uint64_t mask_hash(const char *text, int len)
{
    uint64_t h = 1469598103934665603ULL;
    int i;
    for (i = 0; i < len; i++) {
        h ^= (unsigned char)text[i];
        h *= 1099511628211ULL;
    }
    if (h == 0)
        h = 1; /* 0 marks empty slots */
    return h;
}

/**
 * @brief 在掩码缓存中探测空槽或命中槽 / Probe the mask cache for an empty slot or a hit
 *
 * @param[in,out] font 字体对象 / Font object
 * @param[in]     key 文本哈希键 / Text hash key
 * @param[out]    out_empty 首个空槽，未找到时置 `NULL` / First empty slot, or `NULL` when none was found
 * @param[out]    out_hit 键命中的槽，未命中时置 `NULL` / Slot matching the key, or `NULL` on a miss
 *
 * @note 开放寻址线性探测，两者只会有一个非 `NULL` / Open-addressed linear probing, so at most one of the two out parameters is non-`NULL`
 *
 * @see hpe_font_mask
 */
static void mask_slot(HpeFont *font, uint64_t key, HpeMaskEntry **out_empty, HpeMaskEntry **out_hit)
{
    size_t mask = HPE_MASK_CACHE_SIZE - 1;
    size_t idx = (size_t)((key ^ (key >> 32)) * 2654435761u) & mask;
    size_t i;
    *out_empty = NULL;
    *out_hit = NULL;
    for (i = 0; i < HPE_MASK_CACHE_SIZE; i++) {
        HpeMaskEntry *e = &font->masks[(idx + i) & mask];
        if (e->key == 0) {
            *out_empty = e;
            return;
        }
        if (e->key == key) {
            *out_hit = e;
            return;
        }
    }
}

/**
 * @brief 清空字体掩码缓存 / Flush the font's mask cache
 *
 * @param[in,out] font 字体对象 / Font object
 *
 * @note 释放每个已占用槽的覆盖率掩码并把已用计数归零 / Every occupied slot's coverage mask is freed and the used counter is reset
 */
static void mask_flush(HpeFont *font)
{
    int i;
    if (!font->masks)
        return;
    for (i = 0; i < HPE_MASK_CACHE_SIZE; i++) {
        if (font->masks[i].key) {
            hpe_text_mask_free(&font->masks[i].mask);
            font->masks[i].key = 0;
        }
    }
    font->masks_used = 0;
}

/**
 * @brief 取某段 UTF-8 文本的覆盖率掩码（带缓存）/ Get the coverage mask of a UTF-8 run, cached
 *
 * @details 掩码按「字体 + 文本内容」缓存，稳定帧不再触碰平台光栅化器；缓存负载超过 3/4 时整体清空重建。
 *          Masks are cached per font and text content, so steady-state frames never touch the platform rasterizer; the cache is flushed wholesale once its load factor exceeds 3/4.
 *
 * @param[in,out] font 字体对象 / Font object
 * @param[in]     text UTF-8 文本 / UTF-8 text
 * @param[in]     len 文本字节数 / Text length in bytes
 *
 * @return 缓存持有的掩码指针（借出，勿释放）；失败返回 `NULL` / A cache-owned mask pointer that must not be freed, or `NULL` on failure
 *
 * @note `HPE_NO_MASK_CACHE` 置位时绕过缓存直接走平台光栅化器 / With `HPE_NO_MASK_CACHE` set the platform rasterizer is used directly
 *
 * @see hpe_font_shutdown
 */
const HpeTextMask *hpe_font_mask(HpeFont *font, const char *text, int len)
{
    return hpe_font_mask_phase(font, text, len, 0.0f);
}

/**
 * @brief 快速判断整段文本是否都由主字体服务 / Quickly test whether the whole run is served by the primary font
 *
 * @details 遇到彩色 emoji 序列或任一码点解析到回退字体即返回 0。
 *          Returns 0 as soon as a color emoji sequence or a code point that resolves
 *          to a fallback font is found.
 *
 * @param[in,out] font 主字体对象 / Primary font object
 * @param[in]     text UTF-8 文本 / UTF-8 text
 * @param[in]     len 文本字节数 / Text length in bytes
 *
 * @return 全部由主字体服务返回 1 / 1 when everything is served by the primary font
 */
static int run_is_simple(HpeFont *font, const char *text, int len)
{
    const char *p = text, *end = text + len;
    if (!font || !font->plat || !text || len <= 0)
        return 0;
    while (p < end) {
        uint32_t cp;
        int n;
        if (hpe_emoji_seq_len(p, (int)(end - p)) > 0)
            return 0;
        n = hpe_utf8_next(p, &cp);
        if (n <= 0)
            return 0;
        if (hpe_font_resolve(font, cp) != font)
            return 0;
        p += n;
    }
    return 1;
}

typedef struct {
    const char *text;
    int len;
    float adv_start; /* accumulated advance at segment start / 段起点处累计步进 */
    HpeFont *font;   /* NULL = emoji hole, sprites only / NULL 表示 emoji 空洞，仅画精灵 */
} HpeFontSeg;

/**
 * @brief 把文本按服务字体与 emoji 序列切段 / Split the text into per-font segments and emoji holes
 *
 * @param[in,out] font 主字体对象 / Primary font object
 * @param[in]     text UTF-8 文本 / UTF-8 text
 * @param[in]     len 文本字节数 / Text length in bytes
 * @param[out]    segs 接收切段的数组 / Receives the segments
 * @param[in]     max 数组容量 / Array capacity
 * @param[out]    out_adv 接收整段累计步进 / Receives the total accumulated advance
 *
 * @return 切段个数 / Number of segments
 *
 * @note 相邻同字体码点合并成一段；emoji 序列记为空洞但计入步进 / Adjacent code points of one font merge into a segment; emoji sequences become holes that still consume advance
 */
static int segments_build(HpeFont *font, const char *text, int len, HpeFontSeg *segs, int max,
                          float *out_adv)
{
    const char *p = text, *end = text + len;
    int n_segs = 0;
    float acc = 0.0f;
    while (p < end && n_segs < max) {
        int el = hpe_emoji_seq_len(p, (int)(end - p));
        if (el > 0) {
            const char *q = p + el;
            segs[n_segs].text = p;
            segs[n_segs].len = el;
            segs[n_segs].font = NULL;
            segs[n_segs].adv_start = acc;
            n_segs++;
            while (p < q) {
                uint32_t cp;
                int n = hpe_utf8_next(p, &cp);
                if (n <= 0)
                    break;
                acc += hpe_font_advance(font, cp);
                p += n;
            }
            continue;
        }
        {
            uint32_t cp;
            int n = hpe_utf8_next(p, &cp);
            HpeFont *src;
            if (n <= 0)
                break;
            src = hpe_font_resolve(font, cp);
            if (n_segs > 0 && segs[n_segs - 1].font == src &&
                segs[n_segs - 1].text + segs[n_segs - 1].len == p) {
                segs[n_segs - 1].len += n;
            } else {
                segs[n_segs].text = p;
                segs[n_segs].len = n;
                segs[n_segs].font = src;
                segs[n_segs].adv_start = acc;
                n_segs++;
            }
            acc += hpe_font_advance(font, cp);
            p += n;
        }
    }
    *out_adv = acc;
    return n_segs;
}

/**
 * @brief 把一段掩码按最大覆盖并进合成缓冲 / Merge one mask into the composed buffer by maximal coverage
 *
 * @param[in,out] dst 合成缓冲 / Composed buffer
 * @param[in]     dw 缓冲宽度 / Buffer width
 * @param[in]     dh 缓冲高度 / Buffer height
 * @param[in]     dx0 掩码 (0,0) 在缓冲中的列 / Column of the mask origin in the buffer
 * @param[in]     dy0 掩码 (0,0) 在缓冲中的行 / Row of the mask origin in the buffer
 * @param[in]     m 待并进的掩码 / Mask to merge
 */
static void mask_blit(unsigned char *dst, int dw, int dh, int dx0, int dy0, const HpeTextMask *m)
{
    int x, y;
    for (y = 0; y < m->height; y++) {
        int ty = dy0 + y;
        if (ty < 0 || ty >= dh)
            continue;
        for (x = 0; x < m->width; x++) {
            int tx = dx0 + x;
            unsigned char v;
            if (tx < 0 || tx >= dw)
                continue;
            v = m->coverage[(size_t)y * m->width + x];
            if (dst[(size_t)ty * dw + tx] < v)
                dst[(size_t)ty * dw + tx] = v;
        }
    }
}

/**
 * @brief 合成多字体/含 emoji 的整段掩码 / Compose a whole-run mask across fonts and emoji holes
 *
 * @details 按段向平台取掩码并对齐基线后并进一张整图；emoji 序列位置留空（绘制期
 *          由彩色精灵覆盖）。整段步进决定宽度。
 *          Per-segment masks are fetched from the platform, baseline-aligned and
 *          merged into one image; emoji sequence positions stay blank (color sprites
 *          cover them at paint time). The total advance decides the width.
 *
 * @param[in,out] font 主字体对象 / Primary font object
 * @param[in]     text UTF-8 文本 / UTF-8 text
 * @param[in]     len 文本字节数 / Text length in bytes
 * @param[in]     dx 水平亚像素相位（0..1）/ Horizontal sub-pixel phase (0..1)
 * @param[out]    out 接收掩码，coverage 由缓存或调用方释放 / Receives the mask, the coverage is freed by the cache or the caller
 *
 * @return 成功返回 1 / 1 on success
 */
static int mask_compose(HpeFont *font, const char *text, int len, float dx, HpeTextMask *out)
{
    HpeFontSeg *segs = (HpeFontSeg *)hpe_alloc(sizeof(HpeFontSeg) * (size_t)(len + 1));
    float acc, max_asc = 0.0f, max_desc = 0.0f;
    unsigned char *cov;
    int n, i, w, h, dest_baseline;
    if (!segs)
        return 0;
    memset(out, 0, sizeof(*out));
    n = segments_build(font, text, len, segs, len + 1, &acc);
    for (i = 0; i < n; i++) {
        HpeFont *f = segs[i].font ? segs[i].font : font;
        max_asc = HPE_MAX(max_asc, hpe_plat_font_ascent(f->plat));
        max_desc = HPE_MAX(max_desc, hpe_plat_font_descent(f->plat));
    }
    w = (int)(acc + 0.999f) + 2;
    h = (int)(max_asc + max_desc + 0.999f) + 2;
    dest_baseline = 1 + (int)(max_asc + 0.5f);
    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;
    cov = (unsigned char *)hpe_calloc((size_t)w * h, 1);
    for (i = 0; i < n; i++) {
        HpeTextMask m;
        if (!segs[i].font)
            continue;
        if (hpe_plat_font_mask(segs[i].font->plat, segs[i].text, segs[i].len, dx, &m)) {
            mask_blit(cov, w, h, (int)(segs[i].adv_start + 0.5f), dest_baseline - m.baseline, &m);
            hpe_text_mask_free(&m);
        }
    }
    hpe_free(segs);
    out->coverage = cov;
    out->width = w;
    out->height = h;
    out->baseline = dest_baseline;
    return 1;
}

/**
 * @brief 取带水平亚像素相位的覆盖率掩码（带缓存）/ Get a coverage mask with a horizontal sub-pixel phase, cached
 *
 * @details 相位量化为 3 档（0、1/3、2/3 px）并入缓存键，因此同一文本最多缓存 3 份
 *          掩码；稳态帧仍不触碰平台光栅化器。`hpe_font_mask` 等价于相位 0。
 *          整段全部由主字体服务时走平台整段渲染的快路径；含回退字体或彩色 emoji
 *          序列时改为按段合成（emoji 位置留空，由绘制期的彩色精灵覆盖）。
 *          The phase is quantised to 3 steps (0, 1/3, 2/3 px) and folded into the
 *          cache key, so one text run caches up to three masks; steady-state frames
 *          still never touch the platform rasterizer. `hpe_font_mask` equals phase 0.
 *          Runs fully served by the primary font take the platform's whole-run fast
 *          path; runs with fallback fonts or color emoji sequences are composed per
 *          segment instead (emoji positions stay blank for the paint-time sprites).
 *
 * @param[in,out] font 字体对象 / Font object
 * @param[in]     text UTF-8 文本 / UTF-8 text
 * @param[in]     len 文本字节数 / Text length in bytes
 * @param[in]     frac_x 笔位的小数部分（0..1）/ Fractional part of the pen x (0..1)
 *
 * @return 缓存持有的掩码指针（借出，勿释放）；失败返回 `NULL` / A cache-owned mask pointer that must not be freed, or `NULL` on failure
 *
 * @see hpe_font_mask
 */
/**
 * @brief 求量化的相位档位 / Quantise the sub-pixel phase into its 3 steps
 *
 * @param[in] frac_x 笔位的小数部分（0..1）/ Fractional part of the pen x (0..1)
 *
 * @return 档位 0..2 / The step in 0..2
 */
static int mask_phase_step(float frac_x)
{
    int phase = (int)(frac_x * 3.0f + 0.5f);
    if (phase < 0)
        phase = 0;
    if (phase > 2)
        phase = 2;
    return phase;
}

/**
 * @brief 绕过缓存的掩码构建（调试旁路路径）/ Build a mask bypassing the cache (the debug bypass path)
 *
 * @param[in,out] font 字体对象 / Font object
 * @param[in]     text UTF-8 文本 / UTF-8 text
 * @param[in]     len 文本字节数 / Text length in bytes
 * @param[in]     phase 量化相位档位 / Quantised phase step
 * @param[in]     composed 含回退/emoji 序列为 1 / 1 when the run needs composition
 * @param[in,out] out 接收掩码的静态槽 / Static slot receiving the mask
 *
 * @return 成功返回 `out`，失败返回 `NULL` / `out` on success, `NULL` on failure
 *
 * @note 每次调用先释放上一次的覆盖率，避免旁路路径无限累积 / The previous coverage is freed on every call so the bypass path cannot accumulate
 */
static const HpeTextMask *mask_build_uncached(HpeFont *font, const char *text, int len, int phase,
                                              int composed, HpeTextMask *out)
{
    hpe_text_mask_free(out);
    memset(out, 0, sizeof(*out));
    if (composed)
        return mask_compose(font, text, len, (float)phase / 3.0f, out) ? out : NULL;
    return hpe_plat_font_mask(font->plat, text, len, (float)phase / 3.0f, out) ? out : NULL;
}

const HpeTextMask *hpe_font_mask_phase(HpeFont *font, const char *text, int len, float frac_x)
{
    HpeMaskEntry *empty, *hit;
    uint64_t key;
    int phase, composed;
    static HpeTextMask uncached;

    if (!font || !font->plat || !text || len <= 0)
        return NULL;
    phase = mask_phase_step(frac_x);
    composed = !run_is_simple(font, text, len);
    if (mask_cache_disabled())
        return mask_build_uncached(font, text, len, phase, composed, &uncached);
    if (!font->masks)
        font->masks = (HpeMaskEntry *)hpe_calloc(HPE_MASK_CACHE_SIZE, sizeof(HpeMaskEntry));

    key = mask_hash(text, len) ^ ((uint64_t)(phase + 1) << 60);
    if (composed)
        key ^= (uint64_t)1 << 59; /* composed content differs from the plain run */
    mask_slot(font, key, &empty, &hit);
    if (hit)
        return &hit->mask;
    if (!empty)
        return NULL; /* table full without a hole: treat as miss, no store */

    /* Keep load factor <= 3/4: a full flush beats unbounded clustering.
     * Steady-state pages far exceed this budget with few distinct runs. */
    if (font->masks_used * 4 >= HPE_MASK_CACHE_SIZE * 3) {
        mask_flush(font);
        mask_slot(font, key, &empty, &hit);
        if (hit)
            return &hit->mask;
        if (!empty)
            return NULL;
    }

    if (composed) {
        if (!mask_compose(font, text, len, (float)phase / 3.0f, &empty->mask))
            return NULL;
    } else if (!hpe_plat_font_mask(font->plat, text, len, (float)phase / 3.0f, &empty->mask)) {
        return NULL;
    }
    empty->key = key;
    empty->len = len;
    font->masks_used++;
    return &empty->mask;
}

/**
 * @brief 销毁单个字体对象及其回退子字体 / Destroy one font object and its fallback children
 *
 * @param[in,out] f 待销毁字体，NULL 时无操作 / Font to destroy, NULL is a no-op
 *
 * @note 后备字体由主字体持有、不在全局缓存中，故随主字体一并释放 / Fallback fonts are owned by the primary and not in the global cache, so they are freed along with it
 */
static void font_destroy(HpeFont *f)
{
    int i;
    if (!f)
        return;
    for (i = 0; i < HPE_FONT_FB_MAX; i++) {
        if (f->fb[i]) {
            font_destroy(f->fb[i]);
            f->fb[i] = NULL;
        }
    }
    mask_flush(f);
    if (f->masks)
        hpe_free(f->masks);
    if (f->cov)
        hpe_free(f->cov);
    if (f->plat)
        hpe_plat_font_destroy(f->plat);
    if (f->adv)
        hpe_free(f->adv);
    hpe_free(f);
}

/**
 * @brief 销毁全部缓存字体并释放其资源 / Destroy every cached font and release its resources
 *
 * @details 逐个释放掩码缓存、字宽缓存、平台字体与字体对象（含回退子字体），并把
 *          缓存计数归零。
 *          Each font's mask cache, advance cache, platform font and the object itself
 *          are freed (fallback children included), and the cache count is reset.
 *
 * @note 应在引擎退出时调用一次，调用后字体缓存为空 / Call once at engine shutdown; the font cache is empty afterwards
 *
 * @see hpe_font_get
 */
void hpe_font_shutdown(void)
{
    int i;
    for (i = 0; i < g_font_count; i++) {
        font_destroy(g_fonts[i]);
        g_fonts[i] = NULL;
    }
    g_font_count = 0;
}
