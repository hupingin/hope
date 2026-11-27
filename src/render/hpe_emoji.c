/* Hope UI Engine - color emoji sprites (COLRv0 layers from a system emoji font) */
#include "render/hpe_emoji.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/hpe_util.h"
#include "platform/hpe_platform.h"

/* ---------------------------------------------------------------- */
/* emoji classification                                             */
/* ---------------------------------------------------------------- */

/* flag (regional indicator pair) range / 旗帜（区域指示符对）区间 */
#define RI_LO 0x1F1E6u
#define RI_HI 0x1F1FFu
#define VS16 0xFE0Fu
#define ZWJ 0x200Du
#define TONE_LO 0x1F3FBu
#define TONE_HI 0x1F3FFu

/**
 * @brief 判断码点是否为 emoji 基字符 / Test whether a code point is an emoji base
 *
 * @param[in] cp Unicode 码点 / Unicode code point
 *
 * @return 是基字符返回 1 / 1 when it is a base
 *
 * @note 覆盖常用 emoji 区块：符号区、杂项符号与图形区、补充符号区 / Covers the common emoji blocks: symbols, misc symbols and pictographs, supplemental symbols
 */
static int is_emoji_base(uint32_t cp)
{
    if (cp >= RI_LO && cp <= RI_HI)
        return 1;
    if (cp >= 0x2600u && cp <= 0x27BFu)
        return 1; /* misc symbols + dingbats */
    if (cp >= 0x2B00u && cp <= 0x2BFFu)
        return 1;
    if (cp >= 0x1F000u && cp <= 0x1FAFFu)
        return 1;
    switch (cp) {
    case 0x231Au: /* watch */
    case 0x231Bu: /* hourglass */
    case 0x3030u: /* wavy dash */
    case 0x303Du: /* part alternation mark */
    case 0x3297u: /* congratulation */
    case 0x3299u: /* secret */
        return 1;
    default:
        return 0;
    }
}

/**
 * @brief 判断码点是否默认文本呈现（需要 VS16 才变彩色）/ Test whether a code point defaults to text presentation
 *
 * @param[in] cp Unicode 码点 / Unicode code point
 *
 * @return 默认文本呈现返回 1 / 1 when it defaults to text presentation
 *
 * @note 基本拉丁区符号（2600..27BF、2Bxx）默认画成单色，带 `U+FE0F` 才走彩色 / Symbols in 2600..27BF and 2Bxx paint monochrome by default and only turn color with `U+FE0F`
 */
static int is_text_default(uint32_t cp)
{
    return (cp >= 0x2600u && cp <= 0x27BFu) || (cp >= 0x2B00u && cp <= 0x2BFFu) ||
           cp == 0x231Au || cp == 0x231Bu || cp == 0x3030u || cp == 0x303Du || cp == 0x3297u ||
           cp == 0x3299u;
}

/**
 * @brief 判断码点是否为序列修饰符 / Test whether a code point is a sequence modifier
 *
 * @param[in] cp Unicode 码点 / Unicode code point
 *
 * @return 是修饰符返回 1 / 1 when it is a modifier
 *
 * @note 覆盖 VS16、肤色修饰符与 enclosing keycap / Covers VS16, skin-tone modifiers and the enclosing keycap
 */
static int is_modifier(uint32_t cp)
{
    return cp == VS16 || cp == 0x20E3u || (cp >= TONE_LO && cp <= TONE_HI);
}

/**
 * @brief 判断文本开头是否为彩色 emoji 序列 / Test whether the text starts a color emoji sequence
 *
 * @details 旗帜取区域指示符对；其余基字符后可跟任意多个修饰符。ZWJ（`U+200D`）
 *          不并入序列，各分量按独立 emoji 渲染。
 *          Flags take a regional-indicator pair; other bases may be followed by any
 *          number of modifiers. A ZWJ (`U+200D`) is not merged, so each part renders
 *          as its own emoji.
 *
 * @param[in]     utf8 UTF-8 文本 / UTF-8 text
 * @param[in]     max_len 参与扫描的最大字节数 / Maximum bytes to scan
 *
 * @return 序列的字节长度；不是彩色序列返回 0 / Byte length of the sequence, or 0 when the text does not start one
 */
int hpe_emoji_seq_len(const char *utf8, int max_len)
{
    const char *p = utf8, *end = utf8 + max_len;
    uint32_t cp;
    int n;
    if (!utf8 || max_len <= 0)
        return 0;
    n = hpe_utf8_next(p, &cp);
    if (n <= 0)
        return 0;
    p += n;
    if (cp >= RI_LO && cp <= RI_HI) {
        uint32_t cp2;
        int n2 = p < end ? hpe_utf8_next(p, &cp2) : 0;
        if (n2 > 0 && cp2 >= RI_LO && cp2 <= RI_HI)
            return n + n2;
        return 0;
    }
    if (!is_emoji_base(cp))
        return 0;
    if (is_text_default(cp)) {
        uint32_t cp2;
        int n2 = p < end ? hpe_utf8_next(p, &cp2) : 0;
        if (n2 <= 0 || cp2 != VS16)
            return 0;
    }
    while (p < end) {
        n = hpe_utf8_next(p, &cp);
        if (n <= 0 || !is_modifier(cp))
            break;
        p += n;
    }
    return (int)(p - utf8);
}

/* ---------------------------------------------------------------- */
/* system emoji font file (TrueType tables)                         */
/* ---------------------------------------------------------------- */

#define HPE_EMOJI_SPRITE_MAX 128

typedef struct {
    uint64_t key; /* 0 = empty slot / 0 表示空槽 */
    HpeGlyphSprite sp; /* 像素缓冲由 sp.pixels 持有 / the pixel buffer lives in sp.pixels */
} HpeSpriteEntry;

typedef struct {
    uint8_t *data;
    size_t len;
    uint32_t cmap_sub; /* subtable offset / 子表偏移 */
    int cmap_fmt;      /* 4 or 12 / 4 或 12 */
    uint32_t num_base;
    uint32_t base_off;  /* base glyph records offset / 基字形记录偏移 */
    uint32_t layer_off; /* layer records offset / 层记录偏移 */
    uint32_t num_layers;
    uint32_t rec_off;      /* color records offset / 颜色记录偏移 */
    uint32_t pal_idx_off;  /* palette index array offset / 调色板索引数组偏移 */
    uint16_t num_entries;  /* palette entries per palette / 每个调色板的条目数 */
    uint16_t num_palettes;
} HpeEmojiFile;

static HpeEmojiFile g_file;
static int g_file_loaded = 0; /* 1 = attempted / 1 表示已尝试加载 */
static int g_file_ok = 0;

static HpeSpriteEntry g_sprites[HPE_EMOJI_SPRITE_MAX];

#define HPE_EMOJI_FONT_MAX 8
typedef struct {
    float px;
    HpePlatEmojiFont *font;
} HpeEmojiFontSlot;
static HpeEmojiFontSlot g_fonts[HPE_EMOJI_FONT_MAX];
static int g_font_count = 0;

/**
 * @brief 大端读 16 位（带边界检查）/ Read a big-endian 16-bit value (bounds checked)
 *
 * @param[in] data 文件数据 / File data
 * @param[in] len 数据总长 / Total data length
 * @param[in] off 读取偏移 / Read offset
 *
 * @return 16 位值；越界返回 0 / The value, or 0 when out of bounds
 */
static uint32_t rd16(const uint8_t *data, size_t len, size_t off)
{
    if (off + 2 > len)
        return 0;
    return ((uint32_t)data[off] << 8) | data[off + 1];
}

/**
 * @brief 大端读 32 位（带边界检查）/ Read a big-endian 32-bit value (bounds checked)
 *
 * @param[in] data 文件数据 / File data
 * @param[in] len 数据总长 / Total data length
 * @param[in] off 读取偏移 / Read offset
 *
 * @return 32 位值；越界返回 0 / The value, or 0 when out of bounds
 */
static uint32_t rd32(const uint8_t *data, size_t len, size_t off)
{
    if (off + 4 > len)
        return 0;
    return ((uint32_t)data[off] << 24) | ((uint32_t)data[off + 1] << 16) |
           ((uint32_t)data[off + 2] << 8) | data[off + 3];
}

/**
 * @brief 在表目录中定位一张表 / Locate one table in the table directory
 *
 * @param[in]     f 已加载的字体文件 / Loaded font file
 * @param[in]     tag 4 字节表标签 / 4-byte table tag
 *
 * @return 表偏移；不存在返回 0 / The table offset, or 0 when absent
 *
 * @note 遍历 `sfnt` 表目录逐个比对标签 / Walks the sfnt table directory comparing tags
 */
static uint32_t ttf_table(const HpeEmojiFile *f, const char *tag)
{
    uint32_t num = rd16(f->data, f->len, 4);
    uint32_t i;
    for (i = 0; i < num && i < 64; i++) {
        size_t rec = 12 + (size_t)i * 16;
        if (rec + 16 > f->len)
            break;
        if (memcmp(f->data + rec, tag, 4) == 0)
            return rd32(f->data, f->len, rec + 8);
    }
    return 0;
}

/**
 * @brief 在 cmap 子表目录里挑最优子表并记录其格式 / Pick the best cmap subtable and record its format
 *
 * @param[in,out] f 已加载的字体文件 / Loaded font file
 * @param[in]     cmap cmap 表偏移 / cmap table offset
 *
 * @return 成功返回 1，没有可用子表返回 0 / 1 on success, 0 when no usable subtable exists
 *
 * @note 优先 (3,10) format 12（完整 Unicode），其次 (3,1) format 4（BMP）/ Prefers (3,10) format 12 (full Unicode), then (3,1) format 4 (BMP)
 */
static int cmap_pick(HpeEmojiFile *f, uint32_t cmap)
{
    uint32_t n = rd16(f->data, f->len, cmap + 2);
    uint32_t i, best_score = 0;
    for (i = 0; i < n && i < 32; i++) {
        size_t rec = cmap + 4 + (size_t)i * 8;
        uint32_t plat = rd16(f->data, f->len, rec);
        uint32_t enc = rd16(f->data, f->len, rec + 2);
        uint32_t off = rd32(f->data, f->len, rec + 4);
        uint32_t fmt = rd16(f->data, f->len, cmap + off);
        uint32_t score = 0;
        if (fmt == 12 && plat == 3 && enc == 10)
            score = 4;
        else if (fmt == 12)
            score = 3;
        else if (fmt == 4 && ((plat == 3 && enc == 1) || plat == 0))
            score = 2;
        else if (fmt == 4)
            score = 1;
        if (score > best_score) {
            best_score = score;
            f->cmap_sub = cmap + off;
            f->cmap_fmt = (int)fmt;
        }
    }
    return best_score > 0;
}

/**
 * @brief 解析 COLRv0 与 CPAL 表头 / Parse the COLRv0 and CPAL table headers
 *
 * @param[in,out] f 已加载的字体文件 / Loaded font file
 * @param[in]     colr COLR 表偏移 / COLR table offset
 * @param[in]     cpal CPAL 表偏移 / CPAL table offset
 *
 * @return COLRv0 + CPAL 都有效返回 1 / 1 when both COLRv0 and CPAL are valid
 *
 * @note 仅支持 COLR version 0 与 CPAL version 0/1 / Only COLR version 0 and CPAL version 0/1 are supported
 */
static int tables_parse_colr(HpeEmojiFile *f, uint32_t colr, uint32_t cpal)
{
    if (rd16(f->data, f->len, colr) != 0)
        return 0;
    /* header: version(0) numBase(2) baseOff(4) layerOff(8) numLayers(12) */
    f->num_base = rd16(f->data, f->len, colr + 2);
    f->base_off = colr + rd32(f->data, f->len, colr + 4);
    f->layer_off = colr + rd32(f->data, f->len, colr + 8);
    f->num_layers = rd16(f->data, f->len, colr + 12);
    if (f->num_base == 0 || f->num_layers == 0)
        return 0;
    f->num_entries = rd16(f->data, f->len, cpal + 2);
    f->num_palettes = rd16(f->data, f->len, cpal + 4);
    f->rec_off = cpal + rd32(f->data, f->len, cpal + 8);
    f->pal_idx_off = cpal + 12;
    if (f->num_entries == 0 || f->num_palettes == 0)
        return 0;
    return 1;
}

/**
 * @brief 定位并解析系统 emoji 字体文件 / Locate and parse the system emoji font file
 *
 * @return 成功返回 1；文件缺失或表无效返回 0 / 1 on success, 0 when the file or its tables are missing
 *
 * @note 只尝试一次并缓存结果；路径取 `%SystemRoot%\\Fonts\\seguiemj.ttf` / Attempted once with the result cached; the path is `%SystemRoot%\Fonts\seguiemj.ttf`
 */
static int emoji_file_load(void)
{
    char path[300];
    const char *root = getenv("SystemRoot");
    size_t size = 0;
    uint32_t cmap, colr, cpal;
    if (g_file_loaded)
        return g_file_ok;
    g_file_loaded = 1;
    if (!root || !root[0])
        root = "C:\\Windows";
    snprintf(path, sizeof(path), "%s\\Fonts\\seguiemj.ttf", root);
    g_file.data = (uint8_t *)hpe_read_file(path, &size);
    if (getenv("HPE_TRACE_EMOJI"))
        fprintf(stderr, "emoji: read %s -> %s (%zu bytes)\n", path,
                g_file.data ? "ok" : "FAIL", size);
    if (!g_file.data)
        return 0;
    g_file.len = size;
    cmap = ttf_table(&g_file, "cmap");
    colr = ttf_table(&g_file, "COLR");
    cpal = ttf_table(&g_file, "CPAL");
    if (getenv("HPE_TRACE_EMOJI"))
        fprintf(stderr, "emoji: cmap=%u colr=%u cpal=%u\n", cmap, colr, cpal);
    if (!cmap || !colr || !cpal || !cmap_pick(&g_file, cmap) || !tables_parse_colr(&g_file, colr, cpal)) {
        hpe_free(g_file.data);
        g_file.data = NULL;
        return 0;
    }
    if (getenv("HPE_TRACE_EMOJI"))
        fprintf(stderr, "emoji: fmt=%d sub=%u base=%u layers=%u\n", g_file.cmap_fmt,
                g_file.cmap_sub, g_file.num_base, g_file.num_layers);
    g_file_ok = 1;
    return 1;
}

/**
 * @brief 在 cmap 中查码点的字形索引 / Look up the glyph index of a code point in cmap
 *
 * @param[in] f 已加载的字体文件 / Loaded font file
 * @param[in] cp Unicode 码点 / Unicode code point
 *
 * @return 字形索引；未映射返回 0 / The glyph index, or 0 when unmapped
 *
 * @note format 12 走组二分，format 4 走段线性查找 / format 12 binary-searches the groups, format 4 scans the segments
 */
static uint32_t cmap_lookup(const HpeEmojiFile *f, uint32_t cp)
{
    size_t sub = f->cmap_sub;
    uint32_t i;
    if (f->cmap_fmt == 12) {
        uint32_t lo = 0, hi = rd32(f->data, f->len, sub + 12);
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2, rec = sub + 16 + (size_t)mid * 12;
            uint32_t s = rd32(f->data, f->len, rec), e = rd32(f->data, f->len, rec + 4);
            if (cp < s)
                hi = mid;
            else if (cp > e)
                lo = mid + 1;
            else
                return rd32(f->data, f->len, rec + 8) + (cp - s);
        }
        return 0;
    }
    {
        uint32_t segs = rd16(f->data, f->len, sub + 6) / 2;
        const size_t ends = sub + 14;
        for (i = 0; i < segs && i < 8192; i++) {
            uint32_t end = rd16(f->data, f->len, ends + (size_t)i * 2);
            uint32_t start = rd16(f->data, f->len, ends + ((size_t)segs + 1) * 2 + (size_t)i * 2);
            uint32_t delta = rd16(f->data, f->len, ends + (size_t)segs * 2 + 2 + (size_t)i * 2);
            uint32_t ro = rd16(f->data, f->len, ends + (size_t)segs * 4 + 2 + (size_t)i * 2);
            if (cp >= start && cp <= end) {
                if (ro == 0)
                    return (int)((cp + delta) & 0xFFFFu);
                {
                    size_t gi = ends + (size_t)segs * 4 + 2 + (size_t)i * 2 + ro +
                                (size_t)(cp - start) * 2;
                    uint32_t g = rd16(f->data, f->len, gi);
                    return g == 0 ? 0 : (uint32_t)((int)(g + delta) & 0xFFFFu);
                }
            }
        }
    }
    return 0;
}

/**
 * @brief 在 COLR 基字形记录里二分查找 / Binary-search the COLR base glyph records
 *
 * @param[in] f 已加载的字体文件 / Loaded font file
 * @param[in] gid 基字形索引 / Base glyph index
 * @param[out] out_first 首层索引 / Receives the first layer index
 * @param[out] out_count 层数 / Receives the layer count
 *
 * @return 找到返回 1 / 1 when found
 *
 * @note 记录按字形索引升序存放（规范要求）/ Records are sorted by glyph index as the spec requires
 */
static int colr_lookup(const HpeEmojiFile *f, uint32_t gid, uint32_t *out_first,
                       uint32_t *out_count)
{
    uint32_t lo = 0, hi = f->num_base;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2, rec = f->base_off + (size_t)mid * 6;
        uint32_t g = rd16(f->data, f->len, rec);
        if (g < gid)
            lo = mid + 1;
        else if (g > gid)
            hi = mid;
        else {
            *out_first = rd16(f->data, f->len, rec + 2);
            *out_count = rd16(f->data, f->len, rec + 4);
            return *out_count > 0;
        }
    }
    return 0;
}

/**
 * @brief 取调色板条目的颜色 / Fetch the colour of one palette entry
 *
 * @param[in] f 已加载的字体文件 / Loaded font file
 * @param[in] index 调色板条目索引 / Palette entry index
 * @param[in] fg 前景色（索引 `0xFFFF` 时使用）/ Foreground colour (used for index `0xFFFF`)
 *
 * @return 颜色；索引越界返回不透明白 / The colour, or opaque white for an out-of-range index
 *
 * @note CPAL 颜色记录按 B/G/R/A 字节序存放 / CPAL colour records store B/G/R/A bytes
 */
static HpeColor cpal_color(const HpeEmojiFile *f, uint32_t index, HpeColor fg)
{
    size_t rec;
    if (index == 0xFFFFu)
        return fg;
    if (index >= f->num_entries)
        return hpe_rgb(255, 255, 255);
    rec = f->rec_off + (size_t)index * 4;
    /* CPAL colour records are blue, green, red, alpha bytes / CPAL 按 B/G/R/A 字节存放 */
    return hpe_rgba(f->data[rec + 2], f->data[rec + 1], f->data[rec], f->data[rec + 3]);
}

/**
 * @brief 取（并缓存）指定字号的 emoji 字体对象 / Get (and cache) the emoji font object for a pixel size
 *
 * @param[in] pixel_size 字号（像素）/ Font size in pixels
 *
 * @return emoji 字体对象；系统无彩色 emoji 字体返回 NULL / The emoji font object, NULL when the system has no color emoji font
 *
 * @note 最多缓存 8 个字号，超限整体清空 / Up to 8 sizes are cached with a wholesale flush beyond that
 */
static HpePlatEmojiFont *emoji_font_for(float pixel_size)
{
    int px = (int)(pixel_size + 0.5f), i;
    if (px < 6)
        px = 6;
    if (px > 240)
        px = 240;
    for (i = 0; i < g_font_count; i++) {
        if ((int)(g_fonts[i].px + 0.5f) == px)
            return g_fonts[i].font;
    }
    if (g_font_count >= HPE_EMOJI_FONT_MAX)
        g_font_count = 0;
    {
        HpePlatEmojiFont *font = hpe_plat_emoji_font_create((float)px);
        if (!font)
            return NULL;
        g_fonts[g_font_count].px = (float)px;
        g_fonts[g_font_count].font = font;
        g_font_count++;
        return font;
    }
}

/**
 * @brief 计算精灵缓存键 / Compute the sprite cache key
 *
 * @param[in] seq UTF-8 序列 / UTF-8 sequence
 * @param[in] len 序列字节数 / Sequence length in bytes
 * @param[in] px 量化后的字号 / Quantised pixel size
 * @param[in] fg 前景色 / Foreground colour
 *
 * @return 64 位非零键 / A non-zero 64-bit key
 *
 * @note FNV-1a 混入字号与前景色 / FNV-1a mixed with the pixel size and foreground colour
 */
static uint64_t sprite_key(const char *seq, int len, int px, HpeColor fg)
{
    uint64_t h = 1469598103934665603ULL;
    int i;
    for (i = 0; i < len; i++) {
        h ^= (unsigned char)seq[i];
        h *= 1099511628211ULL;
    }
    h ^= (uint64_t)(uint32_t)px << 24;
    h ^= ((uint32_t)fg.r << 16) | ((uint32_t)fg.g << 8) | (uint32_t)fg.b;
    return h ? h : 1;
}

/**
 * @brief 在精灵缓存中探测空槽或命中槽 / Probe the sprite cache for an empty slot or a hit
 *
 * @param[in]  key 缓存键 / Cache key
 * @param[out] out_empty 首个空槽 / Receives the first empty slot
 * @param[out] out_hit 命中槽 / Receives the matching slot
 *
 * @note 开放寻址线性探测 / Open-addressed linear probing
 */
static void sprite_slot(uint64_t key, HpeSpriteEntry **out_empty, HpeSpriteEntry **out_hit)
{
    size_t idx = (size_t)(key ^ (key >> 32)) & (HPE_EMOJI_SPRITE_MAX - 1);
    size_t i;
    *out_empty = NULL;
    *out_hit = NULL;
    for (i = 0; i < HPE_EMOJI_SPRITE_MAX; i++) {
        HpeSpriteEntry *e = &g_sprites[(idx + i) & (HPE_EMOJI_SPRITE_MAX - 1)];
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
 * @brief 清空精灵缓存 / Flush the sprite cache
 *
 * @note 释放每个已占用槽的像素并把键归零 / Every occupied slot's pixels are freed and the keys zeroed
 */
static void sprite_flush(void)
{
    int i;
    for (i = 0; i < HPE_EMOJI_SPRITE_MAX; i++) {
        if (g_sprites[i].key) {
            if (getenv("HPE_TRACE_EMOJI"))
                fprintf(stderr, "sprite flush: free %p (%dx%d)\n", (void *)g_sprites[i].sp.pixels,
                        g_sprites[i].sp.w, g_sprites[i].sp.h);
            hpe_free(g_sprites[i].sp.pixels);
            g_sprites[i].key = 0;
        }
    }
}

/**
 * @brief 统计精灵缓存已占用槽数 / Count the occupied slots of the sprite cache
 *
 * @return 已占用槽数 / Number of occupied slots
 */
static int sprite_occupied(void)
{
    int i, n = 0;
    for (i = 0; i < HPE_EMOJI_SPRITE_MAX; i++) {
        if (g_sprites[i].key)
            n++;
    }
    return n;
}

#define HPE_EMOJI_MAX_LAYERS 16

/**
 * @brief 计算所有 COLR 层的包围盒 / Compute the bounding box over all COLR layers
 *
 * @param[in,out] font emoji 字体对象 / Emoji font object
 * @param[in]     first 首层索引 / First layer index
 * @param[in]     count 层数 / Layer count
 * @param[out]    out_left 盒左缘相对笔位的最小偏移 / Receives the minimal left offset
 * @param[out]    out_top 基线之上的最大行数 / Receives the maximal rows above the baseline
 * @param[out]    out_right 盒右缘相对笔位的最大偏移 / Receives the maximal right offset
 * @param[out]    out_bottom 基线之下的最大行数 / Receives the maximal rows below the baseline
 *
 * @return 成功取到至少一层返回 1 / 1 when at least one layer was fetched
 */
static int sprite_bbox(HpePlatEmojiFont *font, uint32_t first, uint32_t count, int *out_left,
                       int *out_top, int *out_right, int *out_bottom)
{
    uint32_t i;
    int left = 0, top = 0, right = 0, bottom = 0, any = 0;
    if (count > HPE_EMOJI_MAX_LAYERS)
        count = HPE_EMOJI_MAX_LAYERS;
    for (i = 0; i < count; i++) {
        uint32_t gid = rd16(g_file.data, g_file.len, g_file.layer_off + (size_t)(first + i) * 4);
        HpeGlyphOutline ol;
        if (gid == 0 || !hpe_plat_emoji_glyph(font, gid, &ol))
            continue;
        if (!any) {
            left = ol.left;
            right = ol.left + ol.w;
            top = ol.baseline;
            bottom = ol.h - ol.baseline;
            any = 1;
        } else {
            left = HPE_MIN(left, ol.left);
            right = HPE_MAX(right, ol.left + ol.w);
            top = HPE_MAX(top, ol.baseline);
            bottom = HPE_MAX(bottom, ol.h - ol.baseline);
        }
        hpe_free(ol.coverage);
    }
    *out_left = left;
    *out_top = top;
    *out_right = right;
    *out_bottom = bottom;
    return any;
}

/**
 * @brief 把一层字形覆盖率按调色板色合成进精灵 / Composite one layer's coverage into the sprite with its palette colour
 *
 * @param[in,out] pixels 精灵像素（直通 alpha）/ Sprite pixels (straight alpha)
 * @param[in]     w 精灵宽度 / Sprite width
 * @param[in]     h 精灵高度 / Sprite height
 * @param[in]     ol 层轮廓 / The layer outline
 * @param[in]     dx 层盒左缘在精灵中的列偏移 / Column offset of the layer box in the sprite
 * @param[in]     dy 层盒顶缘在精灵中的行偏移 / Row offset of the layer box in the sprite
 * @param[in]     color 层颜色 / The layer colour
 *
 * @note 标准 source-over 直通 alpha 合成 / Standard source-over straight-alpha compositing
 */
static void sprite_blend_layer(uint32_t *pixels, int w, int h, const HpeGlyphOutline *ol, int dx,
                               int dy, HpeColor color)
{
    int x, y;
    for (y = 0; y < ol->h; y++) {
        int ty = dy + y;
        if (ty < 0 || ty >= h)
            continue;
        for (x = 0; x < ol->w; x++) {
            int tx = dx + x;
            uint32_t *dst;
            float sa, da, oa, r, g, b;
            if (tx < 0 || tx >= w)
                continue;
            dst = &pixels[(size_t)ty * w + tx];
            sa = (ol->coverage[(size_t)y * ol->w + x] / 255.0f) * (color.a / 255.0f);
            da = ((*dst >> 24) & 0xFF) / 255.0f;
            oa = sa + da * (1.0f - sa);
            if (oa <= 0.0f)
                continue;
            r = (color.r / 255.0f * sa + ((*dst >> 16) & 0xFF) / 255.0f * da * (1.0f - sa)) / oa;
            g = (color.g / 255.0f * sa + ((*dst >> 8) & 0xFF) / 255.0f * da * (1.0f - sa)) / oa;
            b = (color.b / 255.0f * sa + (*dst & 0xFF) / 255.0f * da * (1.0f - sa)) / oa;
            *dst = ((uint32_t)(oa * 255.0f + 0.5f) << 24) | ((uint32_t)(r * 255.0f + 0.5f) << 16) |
                   ((uint32_t)(g * 255.0f + 0.5f) << 8) | (uint32_t)(b * 255.0f + 0.5f);
        }
    }
}

/**
 * @brief 把 COLR 各层按调色板色合成进精灵缓冲 / Composite the COLR layers into the sprite buffer
 *
 * @param[in,out] font emoji 字体对象 / Emoji font object
 * @param[in,out] pixels 精灵像素缓冲 / Sprite pixel buffer
 * @param[in]     w 精灵宽度 / Sprite width
 * @param[in]     h 精灵高度 / Sprite height
 * @param[in]     first 首层索引 / First layer index
 * @param[in]     count 层数 / Layer count
 * @param[in]     left 盒左缘 / Box left edge
 * @param[in]     top 盒顶缘（基线之上的行数）/ Box top edge (rows above the baseline)
 * @param[in]     fg 前景色 / Foreground colour
 *
 * @return 至少合成了一层返回 1 / 1 when at least one layer was composited
 *
 * @note 层顺序即绘制顺序（规范要求） / Layers paint in record order as the spec requires
 */
static int sprite_layers_composite(HpePlatEmojiFont *font, uint32_t *pixels, int w, int h,
                                   uint32_t first, uint32_t count, int left, int top, HpeColor fg)
{
    uint32_t i;
    int any = 0;
    if (count > HPE_EMOJI_MAX_LAYERS)
        count = HPE_EMOJI_MAX_LAYERS;
    for (i = 0; i < count; i++) {
        uint32_t lg = rd16(g_file.data, g_file.len, g_file.layer_off + (size_t)(first + i) * 4);
        uint32_t pi = rd16(g_file.data, g_file.len, g_file.layer_off + (size_t)(first + i) * 4 + 2);
        HpeGlyphOutline ol;
        if (lg == 0 || !hpe_plat_emoji_glyph(font, lg, &ol))
            continue;
        sprite_blend_layer(pixels, w, h, &ol, ol.left - left, top - ol.baseline,
                           cpal_color(&g_file, pi, fg));
        hpe_free(ol.coverage);
        any = 1;
    }
    return any;
}

/**
 * @brief 逐层合成一个彩色 emoji 精灵 / Build one color emoji sprite layer by layer
 *
 * @param[in]     px 量化后的字号 / Quantised pixel size
 * @param[in]     seq UTF-8 序列 / UTF-8 sequence
 * @param[in]     len 序列字节长度 / Sequence length in bytes
 * @param[in]     fg 前景色 / Foreground colour
 * @param[out]    out 接收精灵 / Receives the sprite
 *
 * @return 成功返回 1；无 COLR 层或字形缺失返回 0 / 1 on success, 0 when COLR layers or glyphs are missing
 *
 * @note 基字符取序列首码点；层轮廓取自平台 emoji 字体 / The base is the first code point of the sequence; layer outlines come from the platform emoji font
 */
static int sprite_build(float px, const char *seq, int len, HpeColor fg, HpeGlyphSprite *out)
{
    HpePlatEmojiFont *font;
    uint32_t cp, gid, first, count;
    int n, left, top, right, bottom, w, h;
    uint32_t *pixels;
    if ((n = hpe_utf8_next(seq, &cp)) <= 0 || !is_emoji_base(cp) || (cp >= RI_LO && cp <= RI_HI))
        return 0;
    font = emoji_font_for(px);
    if (!font)
        return 0;
    gid = cmap_lookup(&g_file, cp);
    if (getenv("HPE_TRACE_EMOJI"))
        fprintf(stderr, "emoji: cp=%04X gid=%u\n", cp, gid);
    if (gid == 0 || !colr_lookup(&g_file, gid, &first, &count))
        return 0;
    if (getenv("HPE_TRACE_EMOJI"))
        fprintf(stderr, "emoji: colr first=%u count=%u\n", first, count);
    if (!sprite_bbox(font, first, count, &left, &top, &right, &bottom))
        return 0;
    w = right - left;
    h = top + bottom;
    if (w <= 0 || h <= 0)
        return 0;
    pixels = (uint32_t *)hpe_calloc((size_t)w * h, sizeof(uint32_t));
    if (!sprite_layers_composite(font, pixels, w, h, first, count, left, top, fg)) {
        hpe_free(pixels);
        return 0;
    }
    out->w = w;
    out->h = h;
    out->left = left;
    out->baseline = top;
    out->pixels = pixels;
    return 1;
}

/**
 * @brief 取（并缓存）一个彩色 emoji 序列的精灵 / Get (and cache) the sprite of a color emoji sequence
 *
 * @details 先查精灵缓存；未命中时按 COLR 层逐层取字形覆盖率并用调色板色做
 *          source-over 合成，最后把结果放回缓存。
 *          The sprite cache is consulted first; on a miss each COLR layer's glyph
 *          coverage is fetched and composited source-over with its palette colour,
 *          and the result is stored back into the cache.
 *
 * @param[in]     pixel_size 字号（像素）/ Font size in pixels
 * @param[in]     seq UTF-8 序列 / UTF-8 sequence
 * @param[in]     len 序列字节长度 / Sequence length in bytes
 * @param[in]     fg 前景色 / Foreground colour
 * @param[out]    out 接收精灵 / Receives the sprite
 *
 * @return 成功返回 1 / 1 on success
 */
int hpe_emoji_sprite(float pixel_size, const char *seq, int len, HpeColor fg, HpeGlyphSprite *out)
{
    HpeSpriteEntry *empty, *hit;
    uint64_t key;
    int px = (int)(pixel_size + 0.5f);
    if (!out || !seq || len <= 0)
        return 0;
    if (px < 6)
        px = 6;
    if (px > 240)
        px = 240;
    if (!emoji_file_load())
        return 0;
    key = sprite_key(seq, len, px, fg);
    sprite_slot(key, &empty, &hit);
    if (hit) {
        *out = hit->sp;
        return 1;
    }
    if (!empty)
        return 0; /* cache full without a hole / 缓存满且无空槽 */
    if (sprite_occupied() * 4 >= HPE_EMOJI_SPRITE_MAX * 3) {
        sprite_flush();
        sprite_slot(key, &empty, &hit);
        if (hit) {
            *out = hit->sp;
            return 1;
        }
        if (!empty)
            return 0;
    }
    if (!sprite_build((float)px, seq, len, fg, &empty->sp))
        return 0;
    empty->key = key;
    *out = empty->sp;
    return 1;
}

/**
 * @brief 按直通 alpha 把精灵混合到像素缓冲 / Blit a sprite onto a pixel buffer with straight alpha
 *
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     sp 精灵 / The sprite
 * @param[in]     x 精灵盒左缘的设备 x / Device x of the sprite box's left edge
 * @param[in]     y 精灵盒顶缘的设备 y / Device y of the sprite box's top edge
 *
 * @note 逐像素 source-over 合成，写入结果保持不透明；越界或被裁剪的像素跳过 / Per-pixel source-over compositing keeps the result opaque; out-of-bounds or clipped pixels are skipped
 *
 * @see hpe_emoji_sprite
 */
void hpe_emoji_blit(HpePixmap *pm, const HpeGlyphSprite *sp, int x, int y)
{
    int sx, sy;
    if (!pm || !pm->pixels || !sp || !sp->pixels)
        return;
    for (sy = 0; sy < sp->h; sy++) {
        int ty = y + sy;
        uint32_t *dstrow;
        if (ty < 0 || ty >= pm->height || ty < pm->clip_y0 || ty >= pm->clip_y1)
            continue;
        dstrow = pm->pixels + (size_t)ty * (size_t)(pm->stride / 4);
        for (sx = 0; sx < sp->w; sx++) {
            int tx = x + sx;
            uint32_t s = sp->pixels[(size_t)sy * sp->w + sx], d;
            uint32_t sa, inv, sr, sg, sb, dr, dg, db;
            if (tx < 0 || tx >= pm->width || tx < pm->clip_x0 || tx >= pm->clip_x1)
                continue;
            sa = (s >> 24) & 0xFFu;
            if (sa == 0)
                continue;
            d = dstrow[tx];
            sr = (s >> 16) & 0xFFu;
            sg = (s >> 8) & 0xFFu;
            sb = s & 0xFFu;
            dr = (d >> 16) & 0xFFu;
            dg = (d >> 8) & 0xFFu;
            db = d & 0xFFu;
            inv = 255u - sa;
            dstrow[tx] = 0xFF000000u | (((sr * sa + dr * inv) / 255u) << 16) |
                         (((sg * sa + dg * inv) / 255u) << 8) | ((sb * sa + db * inv) / 255u);
        }
    }
}

/**
 * @brief 释放 emoji 模块的全部缓存 / Release every emoji module cache
 *
 * @details 丢弃精灵缓存、关闭已打开的 emoji 字体并释放字体文件数据。
 *          The sprite cache is dropped, opened emoji fonts are closed and the font
 *          file data is freed.
 */
void hpe_emoji_shutdown(void)
{
    int i;
    if (getenv("HPE_TRACE_EMOJI"))
        fprintf(stderr, "emoji shutdown: occupied=%d fonts=%d\n", sprite_occupied(), g_font_count);
    sprite_flush();
    for (i = 0; i < g_font_count; i++) {
        hpe_plat_emoji_font_destroy(g_fonts[i].font);
        g_fonts[i].font = NULL;
    }
    g_font_count = 0;
    if (g_file.data) {
        hpe_free(g_file.data);
        g_file.data = NULL;
    }
    g_file_ok = 0;
    g_file_loaded = 0;
}
