/* Hope UI Engine - font objects with advance caching (on top of the platform font service) */
#ifndef HPE_FONT_H
#define HPE_FONT_H

#include "base/hpe_types.h"
#include "platform/hpe_platform.h"

typedef struct HpeFont HpeFont;

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
 * @note 不要直接释放返回的对象，它由引擎的字体缓存持有 / Do not free the returned object, it is owned by the engine's font cache
 *
 * @see hpe_font_shutdown
 */
HpeFont *hpe_font_get(float pixel_size, int weight, HpeFontFamily family);

/**
 * @brief 取字体已归整的字号 / Get the font's normalised pixel size
 *
 * @param[in,out] font 字体对象，可为 NULL / Font object, may be NULL
 *
 * @return 字号（像素）；`font` 为 `NULL` 返回 0 / Pixel size, or 0 when `font` is `NULL`
 */
float hpe_font_size(HpeFont *font);

/**
 * @brief 取字体已归整的字重 / Get the font's normalised weight
 *
 * @param[in,out] font 字体对象，可为 NULL / Font object, may be NULL
 *
 * @return 字重；`font` 为 `NULL` 返回 400 / The weight, or 400 when `font` is `NULL`
 */
int hpe_font_weight(HpeFont *font);

/**
 * @brief 取字体的上伸部高度 / Get the font's ascent
 *
 * @param[in,out] font 字体对象，可为 NULL / Font object, may be NULL
 *
 * @return 上伸部（像素）；`font` 为 `NULL` 返回 0 / Ascent in pixels, or 0 when `font` is `NULL`
 */
float hpe_font_ascent(HpeFont *font);

/**
 * @brief 取字体的下伸部高度 / Get the font's descent
 *
 * @param[in,out] font 字体对象，可为 NULL / Font object, may be NULL
 *
 * @return 下伸部（像素）；`font` 为 `NULL` 返回 0 / Descent in pixels, or 0 when `font` is `NULL`
 */
float hpe_font_descent(HpeFont *font);

/**
 * @brief 取字体的默认行高 / Get the font's default line height
 *
 * @param[in,out] font 字体对象，可为 NULL / Font object, may be NULL
 *
 * @return 行高（像素）；`font` 为 `NULL` 返回 0 / Line height in pixels, or 0 when `font` is `NULL`
 */
float hpe_font_line_height(HpeFont *font);

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
float hpe_font_advance(HpeFont *font, uint32_t codepoint);

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
float hpe_font_measure(HpeFont *font, const char *text, int len);

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
 * @note 掩码与整数位置无关，绘制时才四舍五入 x/y，因此仅文本内容即可标识 / Masks are independent of integer position since painting rounds x/y, so the text content alone identifies them
 *
 * @see hpe_font_shutdown
 */
const HpeTextMask *hpe_font_mask(HpeFont *font, const char *text, int len);

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
void hpe_font_shutdown(void);

#endif /* HPE_FONT_H */
