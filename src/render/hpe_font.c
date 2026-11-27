#include "render/hpe_font.h"

#include <stdlib.h>
#include <string.h>

#include "base/hpe_util.h"

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

typedef struct {
    uint32_t codepoint;
    float advance;
} HpeAdvEntry;

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
};

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
 * @brief 取单个码点的步进宽度（带缓存）/ Get the advance width of a code point, cached
 *
 * @details 首次查询某码点时向平台取宽度并写入缓存，之后直接命中缓存。
 *          The width is fetched from the platform on the first query for a code point and stored; later queries hit the cache.
 *
 * @param[in,out] font 字体对象 / Font object
 * @param[in]     codepoint Unicode 码点 / Unicode code point
 *
 * @return 步进宽度（像素）；`font` 为 `NULL` 返回 0 / Advance width in pixels, or 0 when `font` is `NULL`
 *
 * @note 缓存无法分配时退化为每次向平台查询 / Falls back to querying the platform on every call when the cache cannot be allocated
 *
 * @see hpe_font_measure
 */
float hpe_font_advance(HpeFont *font, uint32_t codepoint)
{
    HpeAdvEntry *e;
    if (!font)
        return 0.0f;
    e = adv_slot(font, codepoint, 1);
    if (!e)
        return hpe_plat_font_advance(font->plat, codepoint);
    if (e->codepoint == 0xFFFFFFFFu) {
        e->codepoint = codepoint;
        e->advance = hpe_plat_font_advance(font->plat, codepoint);
    }
    return e->advance;
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
 * @brief 取带水平亚像素相位的覆盖率掩码（带缓存）/ Get a coverage mask with a horizontal sub-pixel phase, cached
 *
 * @details 相位量化为 3 档（0、1/3、2/3 px）并入缓存键，因此同一文本最多缓存 3 份
 *          掩码；稳态帧仍不触碰平台光栅化器。`hpe_font_mask` 等价于相位 0。
 *          The phase is quantised to 3 steps (0, 1/3, 2/3 px) and folded into the
 *          cache key, so one text run caches up to three masks; steady-state frames
 *          still never touch the platform rasterizer. `hpe_font_mask` equals phase 0.
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
const HpeTextMask *hpe_font_mask_phase(HpeFont *font, const char *text, int len, float frac_x)
{
    HpeMaskEntry *empty, *hit;
    uint64_t key;
    int phase;
    static HpeTextMask uncached;

    if (!font || !font->plat || !text || len <= 0)
        return NULL;
    phase = (int)(frac_x * 3.0f + 0.5f);
    if (phase < 0)
        phase = 0;
    if (phase > 2)
        phase = 2;
    if (mask_cache_disabled())
        return hpe_plat_font_mask(font->plat, text, len, (float)phase / 3.0f, &uncached)
                   ? &uncached
                   : NULL;
    if (!font->masks)
        font->masks = (HpeMaskEntry *)hpe_calloc(HPE_MASK_CACHE_SIZE, sizeof(HpeMaskEntry));

    key = mask_hash(text, len) ^ ((uint64_t)(phase + 1) << 60);
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

    if (!hpe_plat_font_mask(font->plat, text, len, (float)phase / 3.0f, &empty->mask))
        return NULL;
    empty->key = key;
    empty->len = len;
    font->masks_used++;
    return &empty->mask;
}

/**
 * @brief 销毁全部缓存字体并释放其资源 / Destroy every cached font and release its resources
 *
 * @details 逐个释放掩码缓存、字宽缓存、平台字体与字体对象，并把缓存计数归零。
 *          Each font's mask cache, advance cache, platform font and the object itself are freed, and the cache count is reset to zero.
 *
 * @note 应在引擎退出时调用一次，调用后字体缓存为空 / Call once at engine shutdown; the font cache is empty afterwards
 *
 * @see hpe_font_get
 */
void hpe_font_shutdown(void)
{
    int i;
    for (i = 0; i < g_font_count; i++) {
        HpeFont *f = g_fonts[i];
        if (!f)
            continue;
        mask_flush(f);
        if (f->masks)
            hpe_free(f->masks);
        if (f->plat)
            hpe_plat_font_destroy(f->plat);
        if (f->adv)
            hpe_free(f->adv);
        hpe_free(f);
        g_fonts[i] = NULL;
    }
    g_font_count = 0;
}
