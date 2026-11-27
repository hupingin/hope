/* Hope UI Engine - image decoding (PNG / JPEG / GIF / BMP) + decoded-image cache.
 *
 * Every decoder is written from scratch against the published file formats, so
 * the engine keeps its zero-dependency promise and produces byte-identical
 * output on any platform. All four decoders normalise to 32bpp 0xAARRGGBB with
 * straight (non-premultiplied) alpha and a top-down pixel array, which is
 * exactly HpePixmap's layout, so compositing is a plain per-pixel alpha blend.
 *
 * Robustness policy: a decoder must never read past `len`. Every read goes
 * through the bounds-checked ByteReader below, and a truncated or malformed
 * stream fails the decode (return 0) instead of crashing. */

#include "render/hpe_image.h"
#include "base/hpe_util.h"
#include "platform/hpe_platform.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/* bounds-checked byte reader                                          */
/* ------------------------------------------------------------------ */

typedef struct ByteReader {
    const uint8_t *data;
    size_t len;
    size_t pos;
    int error; /* sticky: once set, every further read fails */
} ByteReader;

static void br_init(ByteReader *br, const uint8_t *data, size_t len)
{
    br->data = data;
    br->len = len;
    br->pos = 0;
    br->error = 0;
}

static int br_need(ByteReader *br, size_t n)
{
    if (br->error || br->pos + n > br->len || br->pos + n < br->pos) {
        br->error = 1;
        return 0;
    }
    return 1;
}

static int br_u8(ByteReader *br)
{
    if (!br_need(br, 1))
        return 0;
    return br->data[br->pos++];
}

static uint32_t br_u16le(ByteReader *br)
{
    uint32_t v;
    if (!br_need(br, 2))
        return 0;
    v = (uint32_t)br->data[br->pos] | ((uint32_t)br->data[br->pos + 1] << 8);
    br->pos += 2;
    return v;
}

static uint32_t br_u32le(ByteReader *br)
{
    uint32_t v;
    if (!br_need(br, 4))
        return 0;
    v = (uint32_t)br->data[br->pos] | ((uint32_t)br->data[br->pos + 1] << 8) |
        ((uint32_t)br->data[br->pos + 2] << 16) | ((uint32_t)br->data[br->pos + 3] << 24);
    br->pos += 4;
    return v;
}

static uint32_t br_u16be(ByteReader *br)
{
    uint32_t v;
    if (!br_need(br, 2))
        return 0;
    v = ((uint32_t)br->data[br->pos] << 8) | (uint32_t)br->data[br->pos + 1];
    br->pos += 2;
    return v;
}

static uint32_t br_u32be(ByteReader *br)
{
    uint32_t v;
    if (!br_need(br, 4))
        return 0;
    v = ((uint32_t)br->data[br->pos] << 24) | ((uint32_t)br->data[br->pos + 1] << 16) |
        ((uint32_t)br->data[br->pos + 2] << 8) | (uint32_t)br->data[br->pos + 3];
    br->pos += 4;
    return v;
}

static int br_skip(ByteReader *br, size_t n)
{
    if (!br_need(br, n))
        return 0;
    br->pos += n;
    return 1;
}

static int br_bytes(ByteReader *br, void *dst, size_t n)
{
    if (!br_need(br, n))
        return 0;
    memcpy(dst, br->data + br->pos, n);
    br->pos += n;
    return 1;
}

/**
 * @brief 按位掩码从打包像素中抽出一个通道 / Extract one channel from a packed pixel with a bit mask
 *
 * @details 掩码的最低置位给出通道起点，随后数出位数并线性缩放到 0~255，
 *          因此 5-6-5、10-10-10 之类的打包布局都能正确展开。
 *          The mask's lowest set bit locates the channel, then the bit count is measured
 *          and the value scaled linearly to 0..255, so 5-6-5 and 10-10-10 packed layouts
 *          all expand correctly.
 *
 * @param[in]     v 打包像素值 / Packed pixel value
 * @param[in]     mask 通道掩码；为 0 时返回 0 / Channel mask; 0 yields 0
 *
 * @return 展开到 0~255 的通道值 / The channel value, scaled to 0..255
 */
static uint32_t mask_extract(uint32_t v, uint32_t mask)
{
    uint32_t shift = 0, bits = 0, raw, maxv;
    if (!mask)
        return 0;
    while (shift < 32 && !((mask >> shift) & 1u))
        shift++;
    while (bits < 32 && ((mask >> (shift + bits)) & 1u))
        bits++;
    if (bits == 0 || shift + bits > 32)
        return 0;
    raw = (v & mask) >> shift;
    maxv = (bits >= 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
    return (raw * 255u + maxv / 2u) / maxv;
}

static uint32_t pack_rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    return ((a & 0xFFu) << 24) | ((r & 0xFFu) << 16) | ((g & 0xFFu) << 8) | (b & 0xFFu);
}

/* ------------------------------------------------------------------ */
/* format sniffing                                                     */
/* ------------------------------------------------------------------ */

HpeImageFormat hpe_image_format_from_path(const char *path)
{
    const char *dot;
    if (!path)
        return HPE_IMG_UNKNOWN;
    dot = strrchr(path, '.');
    if (!dot)
        return HPE_IMG_UNKNOWN;
    dot++;
    if (hpe_str_ieq(dot, "png"))
        return HPE_IMG_PNG;
    if (hpe_str_ieq(dot, "jpg") || hpe_str_ieq(dot, "jpeg") || hpe_str_ieq(dot, "jpe"))
        return HPE_IMG_JPEG;
    if (hpe_str_ieq(dot, "gif"))
        return HPE_IMG_GIF;
    if (hpe_str_ieq(dot, "bmp") || hpe_str_ieq(dot, "dib"))
        return HPE_IMG_BMP;
    return HPE_IMG_UNKNOWN;
}

HpeImageFormat hpe_image_sniff_format(const uint8_t *data, size_t len)
{
    if (!data)
        return HPE_IMG_UNKNOWN;
    if (len >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G' &&
        data[4] == 0x0D && data[5] == 0x0A && data[6] == 0x1A && data[7] == 0x0A)
        return HPE_IMG_PNG;
    if (len >= 2 && data[0] == 0xFF && data[1] == 0xD8)
        return HPE_IMG_JPEG;
    if (len >= 6 && (memcmp(data, "GIF87a", 6) == 0 || memcmp(data, "GIF89a", 6) == 0))
        return HPE_IMG_GIF;
    if (len >= 2 && data[0] == 'B' && data[1] == 'M')
        return HPE_IMG_BMP;
    if (len >= 4) {
        uint32_t hdr = (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) |
                       ((uint32_t)data[3] << 24);
        if (hdr == 12 || hdr == 40 || hdr == 52 || hdr == 56 || hdr == 108 || hdr == 124)
            return HPE_IMG_BMP;
    }
    return HPE_IMG_UNKNOWN;
}

/* ------------------------------------------------------------------ */
/* BMP                                                                 */
/* ------------------------------------------------------------------ */

/**
 * @brief BMP DIB 头解析结果 / Parsed BMP DIB header
 *
 * @see bmp_parse_header
 */
typedef struct {
    uint32_t width;      /**< 图像宽 / Image width */
    int height;          /**< 图像高（恒为正）/ Image height, always positive */
    int top_down;        /**< 自上而下存储（高度为负）/ Stored top-down (negative height) */
    int bpp;             /**< 每像素位数 / Bits per pixel */
    uint32_t colors_used;/**< 调色板条目数（0 表示未指定）/ Palette entry count, 0 when unspecified */
    uint32_t rmask, gmask, bmask, amask; /**< 16/32bpp 通道掩码 / Channel masks for 16/32 bpp */
    int have_masks;      /**< 掩码是否来自文件 / Whether the masks came from the file */
    uint32_t off_bits;   /**< 像素数据偏移 / Pixel-data offset */
    uint32_t dib_size;   /**< DIB 头大小 / DIB header size */
} BmpHeader;

/* The BITMAPCOREHEADER (12 bytes) packs 16-bit width/height/count and stores no
 * compression field. */
static void bmp_read_core_dib(ByteReader *br, BmpHeader *h)
{
    h->width = br_u16le(br);
    h->height = (int)br_u16le(br);
    (void)br_u16le(br);
    h->bpp = (int)br_u16le(br);
}

/* Read the 40+-byte header fields common to every later DIB revision. */
static void bmp_read_info_dib(ByteReader *br, BmpHeader *h, int *compression)
{
    uint32_t height_raw = 0;
    h->width = br_u32le(br);
    height_raw = br_u32le(br);
    (void)br_u16le(br);
    h->bpp = (int)br_u16le(br);
    *compression = (int)br_u32le(br);
    (void)br_u32le(br);
    (void)br_u32le(br);
    (void)br_u32le(br);
    h->colors_used = br_u32le(br);
    (void)br_u32le(br);
    if (height_raw & 0x80000000u) {
        h->top_down = 1;
        h->height = (int)(0u - height_raw);
    } else {
        h->height = (int)height_raw;
    }
}

/* BI_BITFIELDS masks live either inline in the v4+ header or (for the 40-byte
 * header) in the three words that follow it. */
static void bmp_read_masks(ByteReader *br, BmpHeader *h)
{
    if (h->dib_size >= 56) {
        h->rmask = br_u32le(br);
        h->gmask = br_u32le(br);
        h->bmask = br_u32le(br);
        h->amask = br_u32le(br);
        h->have_masks = 1;
    } else if (h->dib_size == 40) {
        br->pos = 14 + 40;
        h->rmask = br_u32le(br);
        h->gmask = br_u32le(br);
        h->bmask = br_u32le(br);
        h->have_masks = 1;
    }
}

/**
 * @brief 解析 BMP 文件头与 DIB 头 / Parse a BMP file header and DIB header
 *
 * @param[in,out] br 字节读取器（从文件头开始）/ Byte reader, positioned at the file start
 * @param[out]    h 接收解析结果 / Receives the parsed header
 *
 * @return 解析成功返回 1，否则 0 / 1 on success, otherwise 0
 *
 * @note 接受 12 字节到 124 字节的 DIB 头；仅处理未压缩与 `BI_BITFIELDS` / Accepts DIB headers from 12 to 124 bytes; handles only uncompressed and `BI_BITFIELDS`
 *
 * @see decode_bmp
 */
static int bmp_parse_header(ByteReader *br, BmpHeader *h)
{
    int compression = 0;

    memset(h, 0, sizeof(*h));
    br->pos = 0;
    if (br_u8(br) != 'B' || br_u8(br) != 'M')
        return 0;
    (void)br_u32le(br); /* file size, not trusted */
    (void)br_u16le(br);
    (void)br_u16le(br);
    h->off_bits = br_u32le(br);
    h->dib_size = br_u32le(br);
    if (br->error)
        return 0;

    if (h->dib_size == 12) {
        bmp_read_core_dib(br, h);
    } else if (h->dib_size >= 40) {
        bmp_read_info_dib(br, h, &compression);
        if (br->error)
            return 0;
        if (compression == 3)
            bmp_read_masks(br, h);
    } else {
        return 0;
    }
    return !br->error && h->width != 0 && h->height > 0 && h->width <= 65535u && h->height <= 65535 &&
           (h->bpp == 1 || h->bpp == 4 || h->bpp == 8 || h->bpp == 16 || h->bpp == 24 ||
            h->bpp == 32);
}

/**
 * @brief 读取 BMP 调色板 / Read a BMP colour palette
 *
 * @param[in,out] br 字节读取器 / Byte reader
 * @param[in]     bpp 每像素位数（<= 8）/ Bits per pixel (<= 8)
 * @param[in]     dib_size DIB 头大小，决定条目大小（12 → 3 字节，否则 4）/ DIB header size, choosing the entry size (3 bytes when 12, else 4)
 * @param[in]     colors_used 头里声明的条目数（0 表示按 bpp 推断）/ Declared entry count, 0 to infer from bpp
 * @param[out]    palette 接收 256 条 RGBA / Receives up to 256 RGBA entries
 *
 * @return 读取的条目数（同时写入 `*out_entries`），出错返回 -1 / The number of entries read (also stored in `*out_entries`), or -1 on error
 *
 * @see decode_bmp
 */
static int bmp_read_palette(ByteReader *br, int bpp, uint32_t dib_size, uint32_t colors_used,
                            uint8_t palette[256][4])
{
    int entries = colors_used ? (int)colors_used : (1 << bpp);
    int entry_size = (dib_size == 12) ? 3 : 4;
    int i;
    if (entries > 256)
        entries = 256;
    for (i = 0; i < entries; i++) {
        uint32_t b = br_u8(br), g = br_u8(br), r = br_u8(br);
        if (br->error)
            return -1;
        palette[i][0] = (uint8_t)r;
        palette[i][1] = (uint8_t)g;
        palette[i][2] = (uint8_t)b;
        palette[i][3] = 255;
        if (entry_size == 4 && !br_skip(br, 1))
            return -1;
    }
    return entries;
}

/**
 * @brief 解码 BMP 的一行像素 / Decode one BMP row into packed RGBA
 *
 * @param[in]     row 源行字节 / Source row bytes
 * @param[in]     width 行内像素数 / Number of pixels in the row
 * @param[in]     bpp 每像素位数 / Bits per pixel
 * @param[in]     palette 调色板（低 bpp 使用）/ Palette, used for low bit depths
 * @param[in]     palette_entries 调色板条目数 / Palette entry count
 * @param[in]     rmask 红通道掩码 / Red channel mask
 * @param[in]     gmask 绿通道掩码 / Green channel mask
 * @param[in]     bmask 蓝通道掩码 / Blue channel mask
 * @param[in]     amask alpha 通道掩码（0 表示无）/ Alpha channel mask, 0 when absent
 * @param[out]    dst 接收 `width` 个打包像素 / Receives `width` packed pixels
 *
 * @note 掩码缺失的 32bpp 中 alpha 为 0 视为填充而非真透明 / In a mask-less 32bpp image an alpha of 0 is treated as padding rather than real transparency
 *
 * @see decode_bmp
 */
static void bmp_decode_row(const uint8_t *row, int width, int bpp,
                           const uint8_t palette[256][4], int palette_entries, uint32_t rmask,
                           uint32_t gmask, uint32_t bmask, uint32_t amask, uint32_t *dst)
{
    int i;
    for (i = 0; i < width; i++) {
        int idx, pi;
        uint32_t v, a;
        switch (bpp) {
        case 1:
            idx = (row[i >> 3] >> (7 - (i & 7))) & 1;
            pi = idx < palette_entries ? idx : 0;
            dst[i] = pack_rgba(palette[pi][0], palette[pi][1], palette[pi][2], 255);
            break;
        case 4:
            idx = (i & 1) ? (row[i >> 1] & 0x0F) : (row[i >> 1] >> 4);
            pi = idx < palette_entries ? idx : 0;
            dst[i] = pack_rgba(palette[pi][0], palette[pi][1], palette[pi][2], 255);
            break;
        case 8:
            idx = row[i];
            pi = idx < palette_entries ? idx : 0;
            dst[i] = pack_rgba(palette[pi][0], palette[pi][1], palette[pi][2], 255);
            break;
        case 16:
            v = (uint32_t)row[i * 2] | ((uint32_t)row[i * 2 + 1] << 8);
            dst[i] = pack_rgba(mask_extract(v, rmask), mask_extract(v, gmask),
                               mask_extract(v, bmask), 255);
            break;
        case 24:
            dst[i] = pack_rgba(row[i * 3 + 2], row[i * 3 + 1], row[i * 3], 255);
            break;
        default: /* 32 */
            v = (uint32_t)row[i * 4] | ((uint32_t)row[i * 4 + 1] << 8) |
                ((uint32_t)row[i * 4 + 2] << 16) | ((uint32_t)row[i * 4 + 3] << 24);
            a = amask ? mask_extract(v, amask) : 255u;
            if (!amask && a == 0)
                a = 255u;
            dst[i] = pack_rgba(mask_extract(v, rmask), mask_extract(v, gmask),
                               mask_extract(v, bmask), a);
            break;
        }
    }
}

/**
 * @brief 为未携带掩码的 16/32bpp 头补默认通道掩码 / Fill default channel masks for a
 *        mask-less 16/32bpp header
 *
 * @param[in,out] hdr 头信息，直接写入 rmask/gmask/bmask / Header, writes rmask/gmask/bmask in place
 *
 * @note 其它 bpp 或已带掩码的头原样不动 / Other bit depths or headers that already carry masks are left untouched
 *
 * @see decode_bmp
 */
static void bmp_default_masks(BmpHeader *hdr)
{
    if (hdr->have_masks)
        return;
    if (hdr->bpp == 16) {
        hdr->rmask = 0x7C00u;
        hdr->gmask = 0x03E0u;
        hdr->bmask = 0x001Fu;
    } else if (hdr->bpp == 32) {
        hdr->rmask = 0x00FF0000u;
        hdr->gmask = 0x0000FF00u;
        hdr->bmask = 0x000000FFu;
    }
}

/**
 * @brief 校验行跨度与数据区并解码全部行 / Validate the row stride and data region, then decode every row
 *
 * @param[in]     br 已定位到调色板之后的读取器 / Reader positioned at the pixel data
 * @param[in]     hdr 头信息 / Header
 * @param[in]     palette 调色板 / Palette
 * @param[in]     palette_entries 调色板条目数 / Palette entry count
 * @param[out]    pixels 接收 width*height 个打包像素 / Receives width*height packed pixels
 *
 * @return 1 成功，0 尺寸溢出或越界 / 1 on success, 0 when the size overflows or runs past the buffer
 *
 * @see decode_bmp
 */
static int bmp_decode_pixels(const ByteReader *br, const BmpHeader *hdr,
                             const uint8_t palette[256][4], int palette_entries, uint32_t *pixels)
{
    size_t row_pad = ((((size_t)hdr->width * (size_t)hdr->bpp + 7u) / 8u) + 3u) & ~(size_t)3u;
    int y;
    if (row_pad == 0)
        return 0;
    if ((size_t)hdr->height > ((size_t)-1) / row_pad)
        return 0;
    if (hdr->off_bits > br->len || row_pad * (size_t)hdr->height > br->len - hdr->off_bits)
        return 0;
    for (y = 0; y < hdr->height; y++) {
        size_t src_row =
            hdr->off_bits + (size_t)(hdr->top_down ? y : (hdr->height - 1 - y)) * row_pad;
        const uint8_t *row = br->data + src_row;
        uint32_t *dst = pixels + (size_t)y * (size_t)hdr->width;
        bmp_decode_row(row, (int)hdr->width, hdr->bpp, palette, palette_entries, hdr->rmask,
                       hdr->gmask, hdr->bmask, hdr->amask, dst);
    }
    return 1;
}

/* BMP stores rows bottom-up unless the height is negative, and pads each row to
 * a 4-byte boundary. DIB headers from 12 bytes (BITMAPCOREHEADER) to 124
 * (BITMAPV5HEADER) are accepted; 16/32bpp pixels are unpacked through channel
 * masks taken from the header or from the block following it. */
static int decode_bmp(ByteReader *br, HpeImage *out)
{
    BmpHeader hdr;
    uint8_t palette[256][4];
    int palette_entries = 0;
    uint32_t *pixels;

    if (!bmp_parse_header(br, &hdr))
        return 0;
    if (hdr.bpp <= 8) {
        palette_entries = bmp_read_palette(br, hdr.bpp, hdr.dib_size, hdr.colors_used, palette);
        if (palette_entries < 0)
            return 0;
    }
    bmp_default_masks(&hdr);
    pixels = (uint32_t *)hpe_alloc((size_t)hdr.width * (size_t)hdr.height * sizeof(uint32_t));
    if (!pixels)
        return 0;
    if (!bmp_decode_pixels(br, &hdr, palette, palette_entries, pixels)) {
        hpe_free(pixels);
        return 0;
    }
    out->width = (int)hdr.width;
    out->height = hdr.height;
    out->pixels = pixels;
    out->owned = 1;
    out->format = HPE_IMG_BMP;
    out->frame_count = 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/* GIF (LZW)                                                           */
/* ------------------------------------------------------------------ */

#define GIF_MAX_CODES 4096

/* The standard GIF LZW variant. Codes begin one bit wider than the minimum code
 * size; the two reserved codes reset the dictionary and end the stream. The
 * decoder emits the raw index stream — de-interlacing and palette lookup happen
 * afterwards. `suffix[i] + prefix[i]` reads entry `i` as "the string of
 * `prefix[i]` followed by byte `suffix[i]`"; `length[i]` caches the string
 * length so emission needs only one chain walk. */
typedef struct GifLzw {
    uint16_t prefix[GIF_MAX_CODES];
    uint8_t suffix[GIF_MAX_CODES];
    uint8_t first[GIF_MAX_CODES];
    uint16_t length[GIF_MAX_CODES];
    int next;
    int code_size;
    int min_code_size;
    int clear_code;
    int end_code;
} GifLzw;

static void gif_lzw_reset(GifLzw *lzw, int min_code_size)
{
    int i, n = 1 << min_code_size;
    lzw->min_code_size = min_code_size;
    lzw->clear_code = n;
    lzw->end_code = n + 1;
    lzw->code_size = min_code_size + 1;
    lzw->next = n + 2;
    for (i = 0; i < n; i++) {
        lzw->prefix[i] = 0xFFFF;
        lzw->suffix[i] = (uint8_t)i;
        lzw->first[i] = (uint8_t)i;
        lzw->length[i] = 1;
    }
}

/* LSB-first code reader over the sub-block payload. */
typedef struct GifBits {
    const uint8_t *data;
    size_t len, pos;
    uint32_t bitbuf;
    int bitcnt;
} GifBits;

/* Pull the next code of `code_size` bits, returning 0 at end of stream. */
static int gif_bits(GifBits *gb, int code_size, uint32_t *out)
{
    while (gb->bitcnt < code_size && gb->pos < gb->len) {
        gb->bitbuf |= (uint32_t)gb->data[gb->pos++] << gb->bitcnt;
        gb->bitcnt += 8;
    }
    if (gb->bitcnt < code_size)
        return 0;
    *out = gb->bitbuf & ((1u << code_size) - 1u);
    gb->bitbuf >>= code_size;
    gb->bitcnt -= code_size;
    return 1;
}

/* Expand entry `code` into `stack` (one byte per string position) and return the
 * string length. */
static int gif_lzw_walk(const GifLzw *lzw, int code, uint8_t *stack, int cap)
{
    int i, walk = code, n = lzw->length[code];
    for (i = n - 1; i >= 0 && i < cap; i--) {
        stack[i] = lzw->suffix[walk];
        walk = lzw->prefix[walk];
    }
    return n;
}

/* Append `sp` bytes off the top of `stack`, without overrunning `out_cap`. */
static size_t gif_lzw_emit(uint8_t *out, size_t out_cap, size_t written, const uint8_t *stack,
                           int sp)
{
    int i;
    for (i = 0; i < sp; i++) {
        if (written < out_cap)
            out[written++] = stack[i];
    }
    return written;
}

/* Define the next dictionary entry as "previous string + first byte", widening
 * the code size once the table outgrows it. */
static void gif_lzw_add(GifLzw *lzw, int prev, int first_byte, int sp)
{
    if (prev < 0 || lzw->next >= GIF_MAX_CODES || sp <= 0)
        return;
    lzw->prefix[lzw->next] = (uint16_t)prev;
    lzw->suffix[lzw->next] = (uint8_t)first_byte;
    lzw->first[lzw->next] = lzw->first[prev];
    lzw->length[lzw->next] = (uint16_t)(lzw->length[prev] + 1);
    lzw->next++;
    if (lzw->next == (1 << lzw->code_size) && lzw->code_size < 12)
        lzw->code_size++;
}

/* Decode the whole sub-block payload into `out`, returning how many bytes were
 * written. A truncated stream keeps whatever complete codes were decoded, which
 * mirrors how browsers salvage damaged GIFs. */
static size_t gif_lzw_decode(const uint8_t *data, size_t len, int min_code_size, uint8_t *out,
                             size_t out_cap)
{
    GifLzw lzw;
    GifBits gb;
    size_t written = 0;
    uint8_t stack[GIF_MAX_CODES + 2];
    int prev = -1, cap = (int)sizeof(stack);

    if (min_code_size < 2 || min_code_size > 8)
        return 0;
    gb.data = data;
    gb.len = len;
    gb.pos = 0;
    gb.bitbuf = 0;
    gb.bitcnt = 0;
    gif_lzw_reset(&lzw, min_code_size);

    for (;;) {
        uint32_t code;
        int sp, first_byte;
        if (!gif_bits(&gb, lzw.code_size, &code))
            break;
        if ((int)code == lzw.clear_code) {
            gif_lzw_reset(&lzw, min_code_size);
            prev = -1;
            continue;
        }
        if ((int)code == lzw.end_code)
            break;
        if ((int)code < lzw.next) {
            /* the code names an existing entry: expand it into the stack */
            sp = gif_lzw_walk(&lzw, (int)code, stack, cap);
        } else {
            /* the KwKwK case: the code names the entry being defined right now,
             * i.e. the previous string followed by its own first byte */
            if (prev < 0)
                break;
            sp = gif_lzw_walk(&lzw, prev, stack, cap);
            if (sp >= cap)
                break;
            stack[sp++] = lzw.first[prev];
        }
        first_byte = stack[0];
        written = gif_lzw_emit(out, out_cap, written, stack, sp);
        gif_lzw_add(&lzw, prev, first_byte, sp);
        prev = (int)code;
    }
    return written;
}

/* GIF interlacing stores row r of the image in four passes: rows 0,8,16.. then
 * 4,12,20.. then 2,6,10.. then 1,3,5... The stored stream is contiguous, so
 * display row `row` lives at storage offset "rows in earlier passes + position
 * within its own pass". This is the inverse of walking the passes in order; the
 * obvious-looking forward walk gets the first row of each pass right and every
 * row after it wrong. */
static const int k_gif_il_start[4] = {0, 4, 2, 1};
static const int k_gif_il_step[4] = {8, 8, 4, 2};

static int gif_il_count(int height, int pass)
{
    int start = k_gif_il_start[pass], step = k_gif_il_step[pass];
    int count = (height - start + step - 1) / step;
    return count > 0 ? count : 0;
}

static int gif_interlaced_row(int row, int height)
{
    int p, offset = 0;
    if (row < 0 || row >= height)
        return row;
    for (p = 0; p < 4; p++) {
        int start = k_gif_il_start[p], step = k_gif_il_step[p];
        int count = gif_il_count(height, p);
        if (row >= start && ((row - start) % step) == 0) {
            int within = (row - start) / step;
            if (within < count)
                return offset + within;
        }
        offset += count;
    }
    return row;
}

/* Logical-screen header plus the global colour table, shared by every frame. */
typedef struct GifState {
    uint32_t width, height;
    int has_gct, gct_entries, bg_index;
    uint8_t gct[256][4];
} GifState;

static int gif_read_header(ByteReader *br, GifState *st)
{
    char sig[6];
    int packed, gct_size, i;
    br->pos = 0;
    if (!br_bytes(br, sig, 6))
        return 0;
    if (memcmp(sig, "GIF87a", 6) != 0 && memcmp(sig, "GIF89a", 6) != 0)
        return 0;
    st->width = br_u16le(br);
    st->height = br_u16le(br);
    packed = br_u8(br);
    st->bg_index = br_u8(br);
    (void)br_u8(br); /* pixel aspect ratio */
    if (br->error || st->width == 0 || st->height == 0 || st->width > 16384 || st->height > 16384)
        return 0;
    st->has_gct = (packed & 0x80) != 0;
    gct_size = packed & 0x07;
    st->gct_entries = 0;
    if (st->has_gct) {
        st->gct_entries = 1 << (gct_size + 1);
        for (i = 0; i < st->gct_entries; i++) {
            st->gct[i][0] = (uint8_t)br_u8(br);
            st->gct[i][1] = (uint8_t)br_u8(br);
            st->gct[i][2] = (uint8_t)br_u8(br);
            st->gct[i][3] = 255;
        }
        if (br->error)
            return 0;
    }
    return 1;
}

/* Frame 0 sits on the logical-screen background colour, which browsers treat as
 * fully transparent. */
static void gif_fill_background(uint8_t *canvas, uint32_t width, uint32_t height,
                                const GifState *st)
{
    uint8_t cr = 0, cg = 0, cb = 0;
    uint32_t i, n = width * height;
    if (st->has_gct && st->bg_index >= 0 && st->bg_index < st->gct_entries) {
        cr = st->gct[st->bg_index][0];
        cg = st->gct[st->bg_index][1];
        cb = st->gct[st->bg_index][2];
    }
    for (i = 0; i < n; i++) {
        canvas[i * 4 + 0] = cr;
        canvas[i * 4 + 1] = cg;
        canvas[i * 4 + 2] = cb;
        canvas[i * 4 + 3] = 0;
    }
}

/* Consume one extension block. The Graphic Control Extension is fixed-length and
 * carries its own terminator: it must be parsed explicitly and not fall through
 * to the generic sub-block walk, which would read the *image descriptor* byte as
 * a block length and skip the whole frame. */
static void gif_handle_extension(ByteReader *br)
{
    int label = br_u8(br);
    if (label == 0xF9) {
        int bs = br_u8(br);
        if (bs >= 4) {
            (void)br_u8(br);    /* flags */
            (void)br_u16le(br); /* delay */
            (void)br_u8(br);    /* transparent index */
            if (bs > 4)
                br_skip(br, (size_t)(bs - 4));
        } else if (bs > 0) {
            br_skip(br, (size_t)bs);
        }
        (void)br_u8(br); /* block terminator */
        return;
    }
    for (;;) {
        int sz = br_u8(br);
        if (br->error || sz == 0)
            break;
        if (!br_skip(br, (size_t)sz))
            break;
    }
}

/* Read the image-data sub-blocks into one contiguous growable buffer. Returns
 * NULL when the stream holds no data blocks. */
static uint8_t *gif_read_subblocks(ByteReader *br, size_t *out_len)
{
    uint8_t *data = NULL;
    size_t data_cap = 0, data_len = 0;
    for (;;) {
        int sz = br_u8(br);
        if (br->error || sz == 0)
            break;
        if (!br_need(br, (size_t)sz))
            break;
        if (data_len + (size_t)sz > data_cap) {
            size_t ncap = data_cap ? data_cap : 4096;
            uint8_t *nb;
            while (ncap < data_len + (size_t)sz)
                ncap *= 2;
            nb = (uint8_t *)hpe_realloc(data, ncap);
            if (!nb)
                break;
            data = nb;
            data_cap = ncap;
        }
        memcpy(data + data_len, br->data + br->pos, (size_t)sz);
        br->pos += (size_t)sz;
        data_len += (size_t)sz;
    }
    *out_len = data_len;
    return data;
}

/* Look an index stream up in the palette and stamp it onto the RGBA canvas,
 * undoing the interlacing on the way. */
static void gif_paint_indices(uint8_t *canvas, uint32_t width, uint32_t ix, uint32_t iy,
                              uint32_t iw, uint32_t ih, const uint8_t *indices, int interlaced,
                              const uint8_t palette[256][4], int palette_entries)
{
    uint32_t r, col;
    for (r = 0; r < ih; r++) {
        int src_row = interlaced ? gif_interlaced_row((int)r, (int)ih) : (int)r;
        for (col = 0; col < iw; col++) {
            uint32_t dx = ix + col, dy = iy + r;
            int idx = indices[(size_t)src_row * iw + col];
            if (idx < 0 || idx >= palette_entries)
                idx = 0;
            canvas[((size_t)dy * width + dx) * 4 + 0] = palette[idx][0];
            canvas[((size_t)dy * width + dx) * 4 + 1] = palette[idx][1];
            canvas[((size_t)dy * width + dx) * 4 + 2] = palette[idx][2];
            canvas[((size_t)dy * width + dx) * 4 + 3] = 255;
        }
    }
}

/* Decode one image descriptor (already positioned at the "0x2C" byte) onto the
 * canvas. Returns 1 when a frame was painted, 0 when the block is malformed. */
static int gif_render_frame(ByteReader *br, const GifState *st, uint8_t *canvas)
{
    uint32_t ix = br_u16le(br), iy = br_u16le(br), iw = br_u16le(br), ih = br_u16le(br);
    int ipacked = br_u8(br);
    int has_lct = (ipacked & 0x80) != 0;
    int interlaced = (ipacked & 0x40) != 0;
    int palette_entries = 0, min_code_size, i;
    uint8_t palette[256][4];
    uint8_t *data, *indices;
    size_t data_len = 0;

    if (br->error)
        return 0;
    if (iw == 0 || ih == 0 || ix + iw > st->width || iy + ih > st->height)
        return 0;
    if (has_lct) {
        palette_entries = 1 << ((ipacked & 0x07) + 1);
        for (i = 0; i < palette_entries; i++) {
            palette[i][0] = (uint8_t)br_u8(br);
            palette[i][1] = (uint8_t)br_u8(br);
            palette[i][2] = (uint8_t)br_u8(br);
            palette[i][3] = 255;
        }
    } else {
        memcpy(palette, st->gct, sizeof(palette));
        palette_entries = st->gct_entries;
    }
    if (br->error)
        return 0;
    min_code_size = br_u8(br);
    data = gif_read_subblocks(br, &data_len);
    if (!data)
        return 0;
    indices = (uint8_t *)hpe_alloc((size_t)iw * ih);
    if (!indices) {
        hpe_free(data);
        return 0;
    }
    memset(indices, 0, (size_t)iw * ih);
    gif_lzw_decode(data, data_len, min_code_size, indices, (size_t)iw * ih);
    hpe_free(data);
    gif_paint_indices(canvas, st->width, ix, iy, iw, ih, indices, interlaced, palette,
                      palette_entries);
    hpe_free(indices);
    return 1;
}

/* Scan blocks until frame 0 is painted or the stream ends. Extensions are
 * skipped; only the first image descriptor is rendered. Returns 1 when a frame
 * was produced. */
static int gif_scan_blocks(ByteReader *br, const GifState *st, uint8_t *canvas)
{
    for (;;) {
        int block = br_u8(br);
        if (br->error)
            break;
        if (block == 0x3B) /* trailer */
            break;
        if (block == 0x21) { /* extension */
            gif_handle_extension(br);
            if (br->error)
                break;
            continue;
        }
        if (block != 0x2C)
            break; /* not an image descriptor */
        return gif_render_frame(br, st, canvas);
    }
    return 0;
}

/* Decode the first image block onto an RGBA canvas. Later frames are ignored —
 * a still page shows frame 0 — but the frame count is reported so callers can
 * tell an animation from a still. */
static int decode_gif(ByteReader *br, HpeImage *out)
{
    GifState st;
    uint8_t *canvas;
    uint32_t *pixels;
    uint32_t i, n;

    if (!gif_read_header(br, &st))
        return 0;
    canvas = (uint8_t *)hpe_alloc((size_t)st.width * (size_t)st.height * 4);
    if (!canvas)
        return 0;
    gif_fill_background(canvas, st.width, st.height, &st);

    if (!gif_scan_blocks(br, &st, canvas)) {
        hpe_free(canvas);
        return 0;
    }
    n = st.width * st.height;
    pixels = (uint32_t *)hpe_alloc((size_t)n * sizeof(uint32_t));
    if (!pixels) {
        hpe_free(canvas);
        return 0;
    }
    for (i = 0; i < n; i++)
        pixels[i] =
            pack_rgba(canvas[i * 4], canvas[i * 4 + 1], canvas[i * 4 + 2], canvas[i * 4 + 3]);
    hpe_free(canvas);

    out->width = (int)st.width;
    out->height = (int)st.height;
    out->pixels = pixels;
    out->owned = 1;
    out->format = HPE_IMG_GIF;
    out->frame_count = 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/* PNG (inflate + unfilter + optional Adam7)                           */
/* ------------------------------------------------------------------ */

/* ---- a compact DEFLATE decoder ---- */

typedef struct HuffTable {
    uint16_t count[16];   /* how many codes of each length exist */
    uint16_t symbol[288]; /* symbols ordered by (length, symbol value) */
} HuffTable;

static int huff_build(HuffTable *t, const uint8_t *lengths, int n)
{
    int i, left;
    uint16_t offs[16];
    memset(t->count, 0, sizeof(t->count));
    for (i = 0; i < n; i++)
        t->count[lengths[i]]++;
    if (t->count[0] == n)
        return 0; /* no codes at all */
    /* reject over-subscribed sets */
    left = 1;
    for (i = 1; i < 16; i++) {
        left <<= 1;
        left -= t->count[i];
        if (left < 0)
            return 0;
    }
    offs[1] = 0;
    for (i = 1; i < 15; i++)
        offs[i + 1] = (uint16_t)(offs[i] + t->count[i]);
    for (i = 0; i < n; i++) {
        if (lengths[i])
            t->symbol[offs[lengths[i]]++] = (uint16_t)i;
    }
    return 1;
}

typedef struct InflateState {
    ByteReader *br;
    uint32_t bitbuf;
    int bitcnt;
    int error;
} InflateState;

static int inf_bits(InflateState *s, int need)
{
    int v;
    while (s->bitcnt < need) {
        int b = br_u8(s->br);
        if (s->br->error) {
            s->error = 1;
            return 0;
        }
        s->bitbuf |= (uint32_t)b << s->bitcnt;
        s->bitcnt += 8;
    }
    v = (int)(s->bitbuf & ((1u << need) - 1u));
    s->bitbuf >>= need;
    s->bitcnt -= need;
    return v;
}

/* Canonical Huffman decode by walking the length table: at each width the code
 * is compared with the first code of that width and, failing that, both the
 * code and the first-code baseline are shifted one bit further. */
static int inf_decode(InflateState *s, const HuffTable *t)
{
    int code = 0, first = 0, index = 0, len;
    for (len = 1; len < 16; len++) {
        int b = inf_bits(s, 1);
        int count;
        if (s->error)
            return -1;
        code |= b;
        count = t->count[len];
        if (code - first < count)
            return t->symbol[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

/* Output buffer that grows on demand; the length is always tracked exactly so
 * truncated input yields a short (not corrupt) result. */
typedef struct OutBuf {
    uint8_t *data;
    size_t len;
    size_t cap;
    int oom;
} OutBuf;

static int outbuf_put(OutBuf *o, uint8_t b)
{
    if (o->len >= o->cap) {
        size_t ncap = o->cap ? o->cap * 2 : 8192;
        uint8_t *nb = (uint8_t *)hpe_realloc(o->data, ncap);
        if (!nb) {
            o->oom = 1;
            return 0;
        }
        o->data = nb;
        o->cap = ncap;
    }
    o->data[o->len++] = b;
    return 1;
}

static int outbuf_putn(OutBuf *o, const uint8_t *src, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (!outbuf_put(o, src[i]))
            return 0;
    }
    return 1;
}

static const uint16_t k_len_base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,
                                        23, 27, 31, 35, 43, 51, 59, 67,  83,  99,  115, 131, 163,
                                        195, 227, 258};
static const uint8_t k_len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                        2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t k_dist_base[30] = {1,    2,    3,    4,    5,     7,     9,     13,
                                         17,   25,   33,   49,   65,    97,    129,   193,
                                         257,  385,  513,  769,  1025,  1537,  2049,  3073,
                                         4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t k_dist_extra[30] = {0, 0, 0,  0,  1,  1,  2,  2,  3,  3,  4,  4,  5,  5,  6,
                                         6, 7, 7,  8,  8,  9,  9,  10, 10, 11, 11, 12, 12, 13, 13};

/* Resolve one length/distance pair (a match) and copy it from the already
 * emitted history. `o->data` may move as the buffer grows, so the source byte is
 * re-read through `o->data` on every iteration. */
static int inf_copy_match(InflateState *s, OutBuf *o, int lsym, const HuffTable *dist)
{
    int length, dsym, distval, i;
    size_t from;
    if (lsym >= 29) {
        s->error = 1;
        return 0;
    }
    length = k_len_base[lsym];
    if (k_len_extra[lsym]) {
        int e = inf_bits(s, k_len_extra[lsym]);
        if (s->error)
            return 0;
        length += e;
    }
    dsym = inf_decode(s, dist);
    if (dsym < 0 || dsym >= 30) {
        s->error = 1;
        return 0;
    }
    distval = k_dist_base[dsym];
    if (k_dist_extra[dsym]) {
        int e = inf_bits(s, k_dist_extra[dsym]);
        if (s->error)
            return 0;
        distval += e;
    }
    if ((size_t)distval > o->len) {
        s->error = 1;
        return 0;
    }
    from = o->len - (size_t)distval;
    for (i = 0; i < length; i++) {
        uint8_t b = o->data[from + (size_t)i];
        if (!outbuf_put(o, b))
            return 0;
    }
    return 1;
}

static int inf_codes(InflateState *s, OutBuf *o, const HuffTable *lit, const HuffTable *dist)
{
    for (;;) {
        int sym = inf_decode(s, lit);
        if (sym < 0) {
            s->error = 1;
            return 0;
        }
        if (sym < 256) {
            if (!outbuf_put(o, (uint8_t)sym))
                return 0;
        } else if (sym == 256) {
            return 1;
        } else if (!inf_copy_match(s, o, sym - 257, dist)) {
            return 0;
        }
    }
}

static int inf_fixed(InflateState *s, OutBuf *o)
{
    HuffTable lit, dist;
    uint8_t ll[288], dl[30];
    int i;
    for (i = 0; i < 144; i++)
        ll[i] = 8;
    for (; i < 256; i++)
        ll[i] = 9;
    for (; i < 280; i++)
        ll[i] = 7;
    for (; i < 288; i++)
        ll[i] = 8;
    for (i = 0; i < 30; i++)
        dl[i] = 5;
    if (!huff_build(&lit, ll, 288) || !huff_build(&dist, dl, 30)) {
        s->error = 1;
        return 0;
    }
    return inf_codes(s, o, &lit, &dist);
}

/* Build the code-length Huffman table, whose code lengths arrive in a fixed
 * permuted order and only the first `hclen` are coded. */
static int inf_read_clen(InflateState *s, HuffTable *cl, int hclen)
{
    static const uint16_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14,
                                       1, 15};
    uint8_t clen[19];
    int i;
    memset(clen, 0, sizeof(clen));
    for (i = 0; i < hclen; i++) {
        int v = inf_bits(s, 3);
        if (s->error)
            return 0;
        clen[order[i]] = (uint8_t)v;
    }
    if (!huff_build(cl, clen, 19)) {
        s->error = 1;
        return 0;
    }
    return 1;
}

/* Expand one repeat symbol (16/17/18) into `lens`, honouring the case where a
 * symbol-16 repeat copies the just-written length. Returns 0 past the bound. */
static int inf_apply_repeat(InflateState *s, int sym, uint8_t *lens, int *np, int limit)
{
    int repeat, value = 0;
    if (sym == 16) {
        if (*np == 0) {
            s->error = 1;
            return 0;
        }
        value = lens[*np - 1];
        repeat = 3 + inf_bits(s, 2);
    } else if (sym == 17) {
        repeat = 3 + inf_bits(s, 3);
    } else {
        repeat = 11 + inf_bits(s, 7);
    }
    if (s->error)
        return 0;
    while (repeat-- > 0 && *np < limit)
        lens[(*np)++] = (uint8_t)value;
    return 1;
}

/* Read the literal/length + distance code-length lists and build their tables. */
static int inf_read_lens(InflateState *s, const HuffTable *cl, uint8_t *lens, int hlit, int hdist,
                         HuffTable *lit, HuffTable *dist)
{
    int n = 0;
    memset(lens, 0, 320);
    while (n < hlit + hdist) {
        int sym = inf_decode(s, cl);
        if (sym < 0) {
            s->error = 1;
            return 0;
        }
        if (sym < 16)
            lens[n++] = (uint8_t)sym;
        else if (!inf_apply_repeat(s, sym, lens, &n, hlit + hdist))
            return 0;
    }
    if (!huff_build(lit, lens, hlit) || !huff_build(dist, lens + hlit, hdist)) {
        s->error = 1;
        return 0;
    }
    return 1;
}

static int inf_dynamic(InflateState *s, OutBuf *o)
{
    uint8_t lens[320];
    HuffTable cl, lit, dist;
    int hlit, hdist, hclen;

    hlit = inf_bits(s, 5);
    hdist = inf_bits(s, 5);
    hclen = inf_bits(s, 4);
    if (s->error)
        return 0;
    hlit += 257;
    hdist += 1;
    hclen += 4;
    if (hlit > 286 || hdist > 30) {
        s->error = 1;
        return 0;
    }
    if (!inf_read_clen(s, &cl, hclen))
        return 0;
    if (!inf_read_lens(s, &cl, lens, hlit, hdist, &lit, &dist))
        return 0;
    return inf_codes(s, o, &lit, &dist);
}

static int inf_stored(InflateState *s, OutBuf *o)
{
    uint32_t len, nlen;
    /* discard the partial byte so the stored block starts byte-aligned */
    s->bitbuf = 0;
    s->bitcnt = 0;
    if (!br_need(s->br, 4))
        return 0;
    len = br_u16le(s->br);
    nlen = br_u16le(s->br);
    if (s->br->error || (len ^ 0xFFFFu) != nlen)
        return 0;
    if (!br_need(s->br, len))
        return 0;
    if (!outbuf_putn(o, s->br->data + s->br->pos, len))
        return 0;
    s->br->pos += len;
    return 1;
}

/* Inflate one deflate block of the given type into `o`. Returns 0 for a
 * reserved block type or a decode failure. */
static int inf_one_block(InflateState *s, OutBuf *o, int type)
{
    if (type == 0)
        return inf_stored(s, o);
    if (type == 1)
        return inf_fixed(s, o);
    if (type == 2)
        return inf_dynamic(s, o);
    return 0;
}

/* Inflate a complete zlib stream (PNG's IDAT payload). `expected` is a size
 * hint used only to pre-size the output buffer. */
static int zlib_inflate(const uint8_t *data, size_t len, size_t expected, OutBuf *o)
{
    ByteReader br;
    InflateState s;
    int cmf, flg, final;

    br_init(&br, data, len);
    cmf = br_u8(&br);
    flg = br_u8(&br);
    if (br.error)
        return 0;
    if ((cmf & 0x0F) != 8) /* only the deflate method is defined */
        return 0;
    if (((cmf << 8) | flg) % 31 != 0)
        return 0;
    if (flg & 0x20)
        return 0; /* preset dictionary: never used by PNG */
    /* the 4-byte Adler-32 checksum sits at the *end* of the stream, after the
     * deflate data, so the compressed payload starts right here */

    if (expected > 0) {
        o->data = (uint8_t *)hpe_alloc(expected);
        if (!o->data)
            return 0;
        o->cap = expected;
        o->len = 0;
    }

    memset(&s, 0, sizeof(s));
    s.br = &br;
    do {
        int type;
        final = inf_bits(&s, 1);
        if (s.error)
            return 0;
        type = inf_bits(&s, 2);
        if (s.error)
            return 0;
        if (!inf_one_block(&s, o, type))
            return 0;
    } while (!final);
    return s.error ? 0 : 1;
}

/* ---- PNG ---- */

static uint32_t paeth(int a, int b, int c)
{
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc)
        return (uint32_t)a;
    if (pb <= pc)
        return (uint32_t)b;
    return (uint32_t)c;
}

/* Reverse one scanline's filter. `bpp` is the channel count times the bit depth
 * rounded up to whole bytes, exactly as the spec defines it. `cur` is modified
 * in place and `prev` must already be unfiltered (NULL on the first row). */
static void png_unfilter_row(uint8_t *cur, const uint8_t *prev, int rowbytes, int bpp, int filter)
{
    int i;
    switch (filter) {
    case 0:
        break;
    case 1:
        for (i = bpp; i < rowbytes; i++)
            cur[i] = (uint8_t)(cur[i] + cur[i - bpp]);
        break;
    case 2:
        if (prev) {
            for (i = 0; i < rowbytes; i++)
                cur[i] = (uint8_t)(cur[i] + prev[i]);
        }
        break;
    case 3:
        for (i = 0; i < rowbytes; i++) {
            int a = (i >= bpp) ? cur[i - bpp] : 0;
            int b = prev ? prev[i] : 0;
            cur[i] = (uint8_t)(cur[i] + ((a + b) >> 1));
        }
        break;
    case 4:
        for (i = 0; i < rowbytes; i++) {
            int a = (i >= bpp) ? cur[i - bpp] : 0;
            int b = prev ? prev[i] : 0;
            int c = (prev && i >= bpp) ? prev[i - bpp] : 0;
            cur[i] = (uint8_t)(cur[i] + paeth(a, b, c));
        }
        break;
    default:
        break;
    }
}

/* Grayscale sample `x` scaled up to 8 bits (16/8-bit samples are used as-is). */
static uint32_t png_gray_value(const uint8_t *src, int x, int bit_depth)
{
    if (bit_depth == 16)
        return src[x * 2];
    if (bit_depth == 8)
        return src[x];
    if (bit_depth == 4)
        return (uint32_t)(((src[x >> 1] >> ((x & 1) ? 0 : 4)) & 0x0F) * 17u);
    if (bit_depth == 2)
        return (uint32_t)(((src[x >> 2] >> (6 - 2 * (x & 3))) & 0x03) * 85u);
    return ((src[x >> 3] >> (7 - (x & 7))) & 0x01) ? 255u : 0u;
}

/* Palette index `x`, unpacked from 1/2/4/8-bit depths without scaling. */
static int png_pal_index(const uint8_t *src, int x, int bit_depth)
{
    if (bit_depth == 8)
        return src[x];
    if (bit_depth == 4)
        return (src[x >> 1] >> ((x & 1) ? 0 : 4)) & 0x0F;
    if (bit_depth == 2)
        return (src[x >> 2] >> (6 - 2 * (x & 3))) & 0x03;
    return (src[x >> 3] >> (7 - (x & 7))) & 0x01;
}

static void png_expand_gray(const uint8_t *src, int width, int bit_depth, uint32_t trns_gray,
                            int have_trns_gray, uint32_t *dst)
{
    int x;
    for (x = 0; x < width; x++) {
        uint32_t v = png_gray_value(src, x, bit_depth);
        uint32_t a = (have_trns_gray && v == trns_gray) ? 0u : 255u;
        dst[x] = pack_rgba(v, v, v, a);
    }
}

static void png_expand_truecolor(const uint8_t *src, int width, int bit_depth,
                                 const uint8_t *trns_rgb, int have_trns_rgb, uint32_t *dst)
{
    int x;
    for (x = 0; x < width; x++) {
        uint32_t r, g, b, a = 255;
        if (bit_depth == 16) {
            r = src[x * 6];
            g = src[x * 6 + 2];
            b = src[x * 6 + 4];
        } else {
            r = src[x * 3];
            g = src[x * 3 + 1];
            b = src[x * 3 + 2];
        }
        if (have_trns_rgb && r == trns_rgb[0] && g == trns_rgb[1] && b == trns_rgb[2])
            a = 0;
        dst[x] = pack_rgba(r, g, b, a);
    }
}

static void png_expand_palette(const uint8_t *src, int width, int bit_depth, const uint8_t *palette,
                               int palette_entries, const uint8_t *alpha, uint32_t *dst)
{
    int x;
    for (x = 0; x < width; x++) {
        int idx = png_pal_index(src, x, bit_depth);
        if (idx >= palette_entries)
            idx = 0;
        dst[x] = pack_rgba(palette[idx * 3], palette[idx * 3 + 1], palette[idx * 3 + 2],
                           alpha ? alpha[idx] : 255);
    }
}

static void png_expand_gray_alpha(const uint8_t *src, int width, int bit_depth, uint32_t *dst)
{
    int x;
    for (x = 0; x < width; x++) {
        uint32_t v, a;
        if (bit_depth == 16) {
            v = src[x * 4];
            a = src[x * 4 + 2];
        } else {
            v = src[x * 2];
            a = src[x * 2 + 1];
        }
        dst[x] = pack_rgba(v, v, v, a);
    }
}

static void png_expand_rgba(const uint8_t *src, int width, int bit_depth, uint32_t *dst)
{
    int x;
    for (x = 0; x < width; x++) {
        uint32_t r, g, b, a;
        if (bit_depth == 16) {
            r = src[x * 8];
            g = src[x * 8 + 2];
            b = src[x * 8 + 4];
            a = src[x * 8 + 6];
        } else {
            r = src[x * 4];
            g = src[x * 4 + 1];
            b = src[x * 4 + 2];
            a = src[x * 4 + 3];
        }
        dst[x] = pack_rgba(r, g, b, a);
    }
}

/* Expand one unfiltered scanline of `width` pixels into RGBA words; covers
 * every legal PNG colour type / bit depth combination. */
static void png_expand_row(const uint8_t *src, int width, int color_type, int bit_depth,
                           const uint8_t *palette, int palette_entries, uint32_t trns_gray,
                           int have_trns_gray, const uint8_t *trns_rgb, int have_trns_rgb,
                           const uint8_t *alpha, uint32_t *dst)
{
    switch (color_type) {
    case 0: /* grayscale */
        png_expand_gray(src, width, bit_depth, trns_gray, have_trns_gray, dst);
        break;
    case 2: /* truecolour */
        png_expand_truecolor(src, width, bit_depth, trns_rgb, have_trns_rgb, dst);
        break;
    case 3: /* palette */
        png_expand_palette(src, width, bit_depth, palette, palette_entries, alpha, dst);
        break;
    case 4: /* grayscale + alpha */
        png_expand_gray_alpha(src, width, bit_depth, dst);
        break;
    default: /* 6: truecolour + alpha */
        png_expand_rgba(src, width, bit_depth, dst);
        break;
    }
}

static int png_channels(int color_type)
{
    switch (color_type) {
    case 0: return 1;
    case 2: return 3;
    case 3: return 1;
    case 4: return 2;
    default: return 4;
    }
}

/* The row filter's `bpp` is defined as ceil(channels * bit_depth / 8), i.e. the
 * channel count rounded up to whole bytes. */
static int png_filter_bpp(int color_type, int bit_depth)
{
    int bits = png_channels(color_type) * bit_depth;
    return bits < 8 ? 1 : bits / 8;
}

/* Adam7 pass geometry: start offsets and step per pass, in x and y. */
static const int k_png_adam7_xs[7] = {0, 4, 0, 2, 0, 1, 0};
static const int k_png_adam7_ys[7] = {0, 0, 4, 0, 2, 0, 1};
static const int k_png_adam7_xd[7] = {8, 8, 4, 4, 2, 2, 1};
static const int k_png_adam7_yd[7] = {8, 8, 8, 4, 4, 2, 2};

/* Everything the chunk stream contributes: the IHDR fields, the palette, the
 * tRNS-derived transparency, and the concatenated IDAT payload. */
typedef struct PngInfo {
    uint32_t width, height;
    int bit_depth, color_type, interlace;
    uint8_t palette[256 * 3];
    int palette_entries;
    uint32_t trns_gray;
    int have_trns_gray;
    uint8_t trns_rgb[3];
    int have_trns_rgb;
    uint8_t *alpha;
    OutBuf idat;
    int seen_ihdr;
} PngInfo;

static int png_handle_ihdr(ByteReader *br, PngInfo *pi)
{
    pi->width = br_u32be(br);
    pi->height = br_u32be(br);
    pi->bit_depth = br_u8(br);
    pi->color_type = br_u8(br);
    (void)br_u8(br); /* compression method, always 0 */
    (void)br_u8(br); /* filter method, always 0 */
    pi->interlace = br_u8(br);
    pi->seen_ihdr = 1;
    if (br->error || pi->width == 0 || pi->height == 0 || pi->width > 65535u ||
        pi->height > 65535u)
        return 0;
    if (pi->interlace != 0 && pi->interlace != 1)
        return 0;
    if (!(pi->color_type == 0 || pi->color_type == 2 || pi->color_type == 3 ||
          pi->color_type == 4 || pi->color_type == 6))
        return 0;
    if (!(pi->bit_depth == 1 || pi->bit_depth == 2 || pi->bit_depth == 4 || pi->bit_depth == 8 ||
          pi->bit_depth == 16))
        return 0;
    if ((pi->color_type == 2 || pi->color_type == 4 || pi->color_type == 6) && pi->bit_depth < 8)
        return 0;
    return 1;
}

static int png_handle_plte(ByteReader *br, PngInfo *pi, uint32_t clen, size_t chunk_start)
{
    uint32_t n = clen / 3;
    if (n > 256)
        n = 256;
    pi->palette_entries = (int)n;
    memcpy(pi->palette, br->data + chunk_start, (size_t)n * 3);
    return 1;
}

/* tRNS carries transparency for palette, grayscale or truecolour images, each
 * in a different width; only the 8-bit sample is read back out. */
static int png_handle_trns(ByteReader *br, PngInfo *pi, uint32_t clen, size_t chunk_start)
{
    if (pi->color_type == 3) {
        uint32_t n = clen, i;
        if (n > 256)
            n = 256;
        pi->alpha = (uint8_t *)hpe_alloc(256);
        if (!pi->alpha)
            return 0;
        memset(pi->alpha, 255, 256);
        for (i = 0; i < n; i++)
            pi->alpha[i] = br->data[chunk_start + i];
    } else if (pi->color_type == 0 && clen >= 2) {
        pi->trns_gray = br->data[chunk_start + 1]; /* the 8-bit sample */
        pi->have_trns_gray = 1;
    } else if (pi->color_type == 2 && clen >= 6) {
        pi->trns_rgb[0] = br->data[chunk_start + 1];
        pi->trns_rgb[1] = br->data[chunk_start + 3];
        pi->trns_rgb[2] = br->data[chunk_start + 5];
        pi->have_trns_rgb = 1;
    }
    return 1;
}

/* Walk the chunk stream, gathering the header, palette and transparency and
 * concatenating every IDAT. Returns 1 once IEND is reached, 0 on a bad stream. */
static int png_read_chunks(ByteReader *br, PngInfo *pi)
{
    br->pos = 8; /* skip the signature, already validated by the sniffer */
    for (;;) {
        uint32_t clen = br_u32be(br);
        char type[5];
        size_t chunk_start;
        int ok = 1;
        if (br->error)
            return 0;
        if (!br_bytes(br, type, 4) || !br_need(br, (size_t)clen + 4))
            return 0;
        type[4] = '\0';
        chunk_start = br->pos;
        if (memcmp(type, "IHDR", 4) == 0)
            ok = png_handle_ihdr(br, pi);
        else if (memcmp(type, "PLTE", 4) == 0)
            ok = png_handle_plte(br, pi, clen, chunk_start);
        else if (memcmp(type, "tRNS", 4) == 0)
            ok = png_handle_trns(br, pi, clen, chunk_start);
        else if (memcmp(type, "IDAT", 4) == 0)
            ok = outbuf_putn(&pi->idat, br->data + chunk_start, clen);
        else if (memcmp(type, "IEND", 4) == 0)
            return 1;
        if (!ok)
            return 0;
        /* every chunk ends with a 4-byte CRC, which we do not verify */
        br->pos = chunk_start + clen;
        if (br->error || !br_skip(br, 4))
            return 0;
    }
}

/* The pre-filter byte count of the whole inflated stream, summed over the seven
 * Adam7 passes or the single interlace-free pass. */
static size_t png_expected_size(const PngInfo *pi, int channels, size_t raw_row1)
{
    size_t expected = 0;
    int p;
    if (pi->interlace) {
        for (p = 0; p < 7; p++) {
            int pw = ((int)pi->width - k_png_adam7_xs[p] + k_png_adam7_xd[p] - 1) /
                     k_png_adam7_xd[p];
            int ph = ((int)pi->height - k_png_adam7_ys[p] + k_png_adam7_yd[p] - 1) /
                     k_png_adam7_yd[p];
            if (pw > 0 && ph > 0)
                expected +=
                    (size_t)ph * (((size_t)pw * channels * pi->bit_depth + 7u) / 8u + 1u);
        }
    } else {
        expected = (size_t)pi->height * (raw_row1 + 1);
    }
    return expected;
}

/* Unfilter + expand every scanline of a non-interlaced image, in place. */
static int png_place_none(const PngInfo *pi, const OutBuf *inflated, size_t raw_row1,
                          uint32_t *pixels)
{
    int bpp = png_filter_bpp(pi->color_type, pi->bit_depth);
    uint8_t *raw = inflated->data;
    uint8_t *prev = NULL;
    size_t off = 0;
    int y;
    for (y = 0; y < (int)pi->height; y++) {
        uint8_t *cur;
        if (off + raw_row1 + 1 > inflated->len)
            return 0;
        cur = raw + off + 1;
        png_unfilter_row(cur, prev, (int)raw_row1, bpp, raw[off]);
        png_expand_row(cur, (int)pi->width, pi->color_type, pi->bit_depth, pi->palette,
                       pi->palette_entries, pi->trns_gray, pi->have_trns_gray, pi->trns_rgb,
                       pi->have_trns_rgb, pi->alpha, pixels + (size_t)y * pi->width);
        prev = cur;
        off += raw_row1 + 1;
    }
    return 1;
}

/* Unfilter + expand the seven Adam7 passes, scattering each pass row into the
 * final image through the pass geometry tables. */
static int png_place_adam7(const PngInfo *pi, const OutBuf *inflated, int channels,
                           uint32_t *pixels)
{
    uint32_t *tmp = (uint32_t *)hpe_alloc((size_t)pi->width * sizeof(uint32_t));
    uint8_t *raw = inflated->data;
    size_t off = 0;
    int p;
    if (!tmp)
        return 0;
    for (p = 0; p < 7; p++) {
        int pw = ((int)pi->width - k_png_adam7_xs[p] + k_png_adam7_xd[p] - 1) / k_png_adam7_xd[p];
        int ph =
            ((int)pi->height - k_png_adam7_ys[p] + k_png_adam7_yd[p] - 1) / k_png_adam7_yd[p];
        int bpp = png_filter_bpp(pi->color_type, pi->bit_depth);
        size_t pass_row = ((size_t)pw * channels * pi->bit_depth + 7u) / 8u;
        uint8_t *prev = NULL;
        int py;
        if (pw <= 0 || ph <= 0)
            continue;
        for (py = 0; py < ph; py++) {
            uint8_t *cur;
            int px;
            if (off + pass_row + 1 > inflated->len)
                goto fail;
            cur = raw + off + 1;
            png_unfilter_row(cur, prev, (int)pass_row, bpp, raw[off]);
            png_expand_row(cur, pw, pi->color_type, pi->bit_depth, pi->palette,
                           pi->palette_entries, pi->trns_gray, pi->have_trns_gray, pi->trns_rgb,
                           pi->have_trns_rgb, pi->alpha, tmp);
            for (px = 0; px < pw; px++) {
                int dx = k_png_adam7_xs[p] + px * k_png_adam7_xd[p];
                int dy = k_png_adam7_ys[p] + py * k_png_adam7_yd[p];
                if (dx < (int)pi->width && dy < (int)pi->height)
                    pixels[(size_t)dy * pi->width + dx] = tmp[px];
            }
            prev = cur;
            off += pass_row + 1;
        }
    }
    hpe_free(tmp);
    return 1;
fail:
    hpe_free(tmp);
    return 0;
}

static int decode_png(ByteReader *br, HpeImage *out)
{
    PngInfo pi;
    uint8_t *raw = NULL;
    uint32_t *pixels = NULL;
    int channels;
    size_t raw_row1, expected;
    OutBuf inflated;

    memset(&pi, 0, sizeof(pi));
    if (!png_read_chunks(br, &pi) || !pi.seen_ihdr || pi.idat.len == 0)
        goto fail;

    channels = png_channels(pi.color_type);
    raw_row1 = ((size_t)pi.width * (size_t)channels * (size_t)pi.bit_depth + 7u) / 8u;
    expected = png_expected_size(&pi, channels, raw_row1);

    memset(&inflated, 0, sizeof(inflated));
    if (!zlib_inflate(pi.idat.data, pi.idat.len, expected, &inflated))
        goto fail;
    raw = inflated.data;

    pixels = (uint32_t *)hpe_alloc((size_t)pi.width * (size_t)pi.height * sizeof(uint32_t));
    if (!pixels)
        goto fail;
    if (pi.interlace) {
        if (!png_place_adam7(&pi, &inflated, channels, pixels))
            goto fail;
    } else if (!png_place_none(&pi, &inflated, raw_row1, pixels)) {
        goto fail;
    }

    hpe_free(inflated.data); /* raw scanlines are no longer needed / 解压行数据已摆放完毕，释放 */
    hpe_free(pi.idat.data);
    hpe_free(pi.alpha);
    out->width = (int)pi.width;
    out->height = (int)pi.height;
    out->pixels = pixels;
    out->owned = 1;
    out->format = HPE_IMG_PNG;
    out->frame_count = 1;
    return 1;

fail:
    hpe_free(pi.idat.data);
    hpe_free(raw);
    hpe_free(pixels);
    hpe_free(pi.alpha);
    return 0;
}

/* ------------------------------------------------------------------ */
/* JPEG (baseline sequential + progressive)                            */
/* ------------------------------------------------------------------ */

/* Zig-zag order in which DQT and the DCT coefficients are stored. */
static const uint8_t k_jpeg_zigzag[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

#define JPEG_MAX_COMPONENTS 4

/* Huffman table expressed as per-length code counts plus a value list in code
 * order, which is all the canonical decoder below needs. */
typedef struct JpegHuff {
    int count[17];      /* number of codes of each length, index 1..16 */
    uint8_t value[256]; /* symbols in code order */
} JpegHuff;

static int jpeg_huff_build(JpegHuff *h, const uint8_t counts[17], const uint8_t *values,
                           int total)
{
    int i, sum = 0;
    if (total > 256)
        return 0;
    memset(h->count, 0, sizeof(h->count));
    for (i = 1; i <= 16; i++) {
        h->count[i] = counts[i];
        sum += counts[i];
    }
    if (sum != total)
        return 0;
    memcpy(h->value, values, (size_t)total);
    return 1;
}

/* Entropy-coded bit reader. JPEG stuffs a zero byte after every 0xFF inside the
 * entropy stream, and any other 0xFFxx sequence is a marker that terminates the
 * scan — both are handled here, so callers never see a marker as data. */
typedef struct JpegBits {
    ByteReader br;
    uint32_t bitbuf;
    int bitcnt;
    int eof;
    int marker; /* the marker byte that ended the scan, 0 while still reading */
} JpegBits;

static void jpeg_bits_init(JpegBits *b, const uint8_t *data, size_t len, size_t start)
{
    br_init(&b->br, data, len);
    b->br.pos = start;
    b->bitbuf = 0;
    b->bitcnt = 0;
    b->eof = 0;
    b->marker = 0;
}

/* Read `need` bits MSB-first, or -1 when the scan ran out.
 *
 * The bit buffer keeps its `bitcnt` unconsumed bits in the *high* end of the
 * 32-bit word and shifts them up as each new byte arrives, so extraction is a
 * plain "take the top `need` bits" and the remainder is shifted back into
 * place. Getting this wrong silently misaligns every symbol after the first
 * byte boundary, which shows up as a flat-block decode. */
static int jpeg_bits(JpegBits *b, int need)
{
    int v;
    if (need == 0)
        return 0;
    while (b->bitcnt < need) {
        int c;
        if (b->eof)
            return -1;
        c = br_u8(&b->br);
        if (b->br.error) {
            b->eof = 1;
            return -1;
        }
        if (c == 0xFF) {
            int nxt = br_u8(&b->br);
            if (b->br.error) {
                b->eof = 1;
                return -1;
            }
            if (nxt == 0x00) {
                c = 0xFF; /* stuffed: a literal 0xFF in the entropy data */
            } else if (nxt == 0xFF) {
                b->br.pos -= 1; /* fill byte before the real marker */
                continue;
            } else {
                b->br.pos -= 2; /* leave the marker for the caller */
                b->marker = nxt;
                b->eof = 1;
                return -1;
            }
        }
        b->bitbuf |= (uint32_t)c << (24 - b->bitcnt);
        b->bitcnt += 8;
    }
    v = (int)((b->bitbuf >> (32 - need)) & ((1u << need) - 1u));
    b->bitbuf <<= need;
    b->bitcnt -= need;
    return v;
}

/* Canonical Huffman decode: shift one bit at a time and compare the running
 * code against each length's code range. */
static int jpeg_huff_decode(JpegBits *b, const JpegHuff *h)
{
    int code = 0, first = 0, index = 0, len;
    for (len = 1; len <= 16; len++) {
        int bit = jpeg_bits(b, 1);
        if (bit < 0)
            return -1;
        code = (code << 1) | bit;
        if (code - first < h->count[len])
            return h->value[index + (code - first)];
        index += h->count[len];
        first = (first + h->count[len]) << 1;
    }
    return -1;
}

/* JPEG's "extend": turn an n-bit magnitude into a signed difference. */
static int jpeg_extend(JpegBits *b, int nbits)
{
    int v;
    if (nbits == 0)
        return 0;
    v = jpeg_bits(b, nbits);
    if (v < 0)
        return 0;
    if (v < (1 << (nbits - 1)))
        v -= (1 << nbits) - 1;
    return v;
}

/* Inverse-DCT basis matrix. Entry [x][u] is 0.5 * C(u) * cos((2x+1) u pi / 16)
 * with C(0) = 1/sqrt(2) and C(u) = 1 otherwise, so a row transform followed by
 * a column transform reproduces the 2-D IDCT
 *     f(x,y) = sum_uv 0.25 C(u) C(v) F(u,v) cos(..) cos(..)
 * exactly (the 0.25 is 0.5 from each pass), with no extra scale factor left to
 * apply. Values are generated from the closed form and written out so the
 * transform is a pair of plain 8x8 matrix multiplies — slower than a
 * factorised IDCT but trivially auditable and accurate to well under one LSB.
 * (A hand-factorised 1-D kernel is easy to get subtly wrong: each frequency
 * ends up with a different scale error, which shows up as a washed-out or
 * over-contrasty image rather than as an obvious corruption.) */
static const float k_jpeg_idct_m[8][8] = {
    {3.5355339059e-01f, 4.9039264020e-01f, 4.6193976626e-01f, 4.1573480615e-01f, 3.5355339059e-01f, 2.7778511651e-01f, 1.9134171618e-01f, 9.7545161008e-02f},
    {3.5355339059e-01f, 4.1573480615e-01f, 1.9134171618e-01f, -9.7545161008e-02f, -3.5355339059e-01f, -4.9039264020e-01f, -4.6193976626e-01f, -2.7778511651e-01f},
    {3.5355339059e-01f, 2.7778511651e-01f, -1.9134171618e-01f, -4.9039264020e-01f, -3.5355339059e-01f, 9.7545161008e-02f, 4.6193976626e-01f, 4.1573480615e-01f},
    {3.5355339059e-01f, 9.7545161008e-02f, -4.6193976626e-01f, -2.7778511651e-01f, 3.5355339059e-01f, 4.1573480615e-01f, -1.9134171618e-01f, -4.9039264020e-01f},
    {3.5355339059e-01f, -9.7545161008e-02f, -4.6193976626e-01f, 2.7778511651e-01f, 3.5355339059e-01f, -4.1573480615e-01f, -1.9134171618e-01f, 4.9039264020e-01f},
    {3.5355339059e-01f, -2.7778511651e-01f, -1.9134171618e-01f, 4.9039264020e-01f, -3.5355339059e-01f, -9.7545161008e-02f, 4.6193976626e-01f, -4.1573480615e-01f},
    {3.5355339059e-01f, -4.1573480615e-01f, 1.9134171618e-01f, 9.7545161008e-02f, -3.5355339059e-01f, 4.9039264020e-01f, -4.6193976626e-01f, 2.7778511651e-01f},
    {3.5355339059e-01f, -4.9039264020e-01f, 4.6193976626e-01f, -4.1573480615e-01f, 3.5355339059e-01f, -2.7778511651e-01f, 1.9134171618e-01f, -9.7545161008e-02f}
};

/* Dequantise one 8x8 block, inverse-transform it and store the result as 8-bit
 * samples offset by +128 (the level shift the encoder applied). Coefficients
 * arrive still quantised because progressive scans refine them bit-plane by bit
 * -plane; the multiplication by the quantisation table belongs here. */
static void jpeg_idct(const int *in, uint8_t *out, int stride, const int *quant)
{
    float blk[64], tmp[64];
    int i, k, x, y;

    for (i = 0; i < 64; i++)
        blk[i] = (float)(in[i] * (quant ? quant[i] : 1));

    /* rows: transform across u for every row y */
    for (y = 0; y < 8; y++) {
        for (x = 0; x < 8; x++) {
            float s = 0.0f;
            for (k = 0; k < 8; k++)
                s += k_jpeg_idct_m[x][k] * blk[y * 8 + k];
            tmp[y * 8 + x] = s;
        }
    }
    /* columns: transform across v for every column x */
    for (x = 0; x < 8; x++) {
        for (y = 0; y < 8; y++) {
            float s = 0.0f;
            for (k = 0; k < 8; k++)
                s += k_jpeg_idct_m[y][k] * tmp[k * 8 + x];
            s += 128.0f;
            {
                int v = (int)floorf(s + 0.5f);
                if (v < 0)
                    v = 0;
                if (v > 255)
                    v = 255;
                out[(size_t)y * stride + x] = (uint8_t)v;
            }
        }
    }
}

typedef struct JpegComponent {
    int id, h, v, tq, dc_tbl, ac_tbl;
    int dc_pred;
    int bx, by;  /* MCU-aligned coefficient grid: the array stride */
    int tw, th;  /* true block counts (ceil(component samples / 8)) */
    int *coeff;  /* bx*by*64 *quantised* coefficients (dequantised by the IDCT) */
    int pred_used; /* progressive: whether the DC predictor took part */
} JpegComponent;

/* Read the next marker, skipping any fill bytes, and return -1 on error. */
static int jpeg_next_marker(ByteReader *br)
{
    int c;
    do {
        c = br_u8(br);
    } while (!br->error && c != 0xFF);
    if (br->error)
        return -1;
    do {
        c = br_u8(br);
    } while (!br->error && c == 0xFF);
    if (br->error)
        return -1;
    return c;
}

/* Rewind to the next real marker at or after `from`.
 *
 * A scan loop stops as soon as it has emitted every MCU it expected, which can
 * leave the bit reader parked in the middle of the entropy payload — or past a
 * marker it never pulled the bits for. The scan's own byte position is therefore
 * not a reliable "next marker" pointer, and trusting it made the parser read
 * entropy bytes as if they were a segment header. Scanning forward for an
 * unstuffed 0xFF is what libjpeg does and needs no bookkeeping in the scan. */
static int jpeg_resync(ByteReader *br, size_t from)
{
    size_t i = from;
    while (i + 1 < br->len) {
        if (br->data[i] == 0xFF) {
            size_t j = i + 1;
            while (j < br->len && br->data[j] == 0xFF)
                j++;
            if (j >= br->len)
                break;
            if (br->data[j] != 0x00) { /* 0xFF00 is stuffed data, not a marker */
                br->pos = i;
                return 1;
            }
            i = j + 1;
            continue;
        }
        i++;
    }
    return 0;
}

/* Per-decode state shared by the marker parser, the scan decoders and the
 * post-processing passes. */
typedef struct JpegState {
    int width, height, ncomp, max_h, max_v;
    JpegComponent comps[JPEG_MAX_COMPONENTS];
    /* Quantisation tables scale the *unit* coefficient, so they are stored as
     * plain integers and the IDCT multiplies after the scan. Progressive scans
     * refine stored coefficients bit by bit and must not round through float. */
    int quant[4][64];
    int quant_valid[4];
    JpegHuff huff_dc[4], huff_ac[4];
    int huff_dc_valid[4], huff_ac_valid[4];
    int restart_interval;
    int progressive;
    int have_scan;
} JpegState;

/* Scan (SOS) parameters plus the MCU walk bounds and the end-of-band run that
 * spans blocks within a scan. */
typedef struct JpegScan {
    int nscan;
    int comp[JPEG_MAX_COMPONENTS];
    int dc[JPEG_MAX_COMPONENTS];
    int ac[JPEG_MAX_COMPONENTS];
    int ss, se, ah, al;
    int non_interleaved;
    int mcus_x, mcus_y;
    int eobrun;
} JpegScan;

/* SOF0/1/2: frame header with the component sampling factors. Returns 0 on a
 * malformed frame. */
static int jpeg_parse_sof(JpegState *st, ByteReader *br, int marker)
{
    int i, ncomp;
    st->progressive = (marker == 0xC2);
    (void)br_u8(br); /* sample precision, 8 or 12 */
    st->height = (int)br_u16be(br);
    st->width = (int)br_u16be(br);
    ncomp = br_u8(br);
    if (br->error || ncomp < 1 || ncomp > JPEG_MAX_COMPONENTS)
        return 0;
    if (st->width <= 0 || st->height <= 0 || st->width > 65535 || st->height > 65535)
        return 0;
    st->ncomp = ncomp;
    st->max_h = st->max_v = 1;
    for (i = 0; i < ncomp; i++) {
        int id = br_u8(br);
        int hv = br_u8(br);
        int tq = br_u8(br);
        st->comps[i].id = id;
        st->comps[i].h = (hv >> 4) & 0x0F;
        st->comps[i].v = hv & 0x0F;
        st->comps[i].tq = tq;
        if (st->comps[i].h == 0 || st->comps[i].v == 0)
            return 0;
        if (st->comps[i].h > st->max_h)
            st->max_h = st->comps[i].h;
        if (st->comps[i].v > st->max_v)
            st->max_v = st->comps[i].v;
    }
    return !br->error;
}

/* DQT: one or more quantisation tables, in zig-zag order. */
static void jpeg_parse_dqt(JpegState *st, ByteReader *br, size_t seg_end)
{
    while (br->pos < seg_end && !br->error) {
        int pq = br_u8(br);
        int tq = pq & 0x0F;
        int prec = (pq >> 4) & 0x0F;
        int k;
        if (tq > 3)
            break;
        for (k = 0; k < 64; k++) {
            int v = prec ? (int)br_u16be(br) : br_u8(br);
            st->quant[tq][k_jpeg_zigzag[k]] = v;
        }
        st->quant_valid[tq] = 1;
    }
}

/* DHT: one or more Huffman tables (DC or AC). */
static void jpeg_parse_dht(JpegState *st, ByteReader *br, size_t seg_end)
{
    while (br->pos < seg_end && !br->error) {
        int tc_th = br_u8(br);
        int tc = (tc_th >> 4) & 0x0F;
        int th = tc_th & 0x0F;
        uint8_t counts[17];
        uint8_t values[256];
        int total = 0, k;
        if (th > 3 || tc > 1)
            break;
        memset(counts, 0, sizeof(counts));
        for (k = 1; k <= 16; k++)
            counts[k] = (uint8_t)br_u8(br);
        for (k = 1; k <= 16; k++)
            total += counts[k];
        if (total > 256 || br->pos + (size_t)total > seg_end)
            break;
        for (k = 0; k < total; k++)
            values[k] = (uint8_t)br_u8(br);
        if (tc == 0) {
            jpeg_huff_build(&st->huff_dc[th], counts, values, total);
            st->huff_dc_valid[th] = 1;
        } else {
            jpeg_huff_build(&st->huff_ac[th], counts, values, total);
            st->huff_ac_valid[th] = 1;
        }
    }
}

/* The coefficient grids are MCU-aligned, not sample-aligned: every MCU
 * contributes `h` by `v` blocks per component, so the grid is `mcus * h/v`
 * blocks even when the image ends mid-MCU. Sizing it from the sample extent
 * instead would under-allocate and let the scan loop write past the buffer. The
 * grid is allocated once and reused by every scan of a progressive file. */
static int jpeg_init_grids(JpegState *st)
{
    int mcx = (st->width + st->max_h * 8 - 1) / (st->max_h * 8);
    int mcy = (st->height + st->max_v * 8 - 1) / (st->max_v * 8);
    int i;
    for (i = 0; i < st->ncomp; i++) {
        /* Samples this component actually covers, then blocks. The true count
         * is *not* MCU-aligned: for a 4:2:0 24-pixel-wide image the luma has 3
         * block columns even though an MCU is two blocks wide. Non-interleaved
         * scans walk this true grid, so using the MCU-aligned width there makes
         * the decoder read blocks the encoder never wrote and every later block
         * in the scan lands on the wrong coefficients. */
        int sw = (st->width * st->comps[i].h + st->max_h - 1) / st->max_h;
        int sh = (st->height * st->comps[i].v + st->max_v - 1) / st->max_v;
        st->comps[i].tw = (sw + 7) / 8;
        st->comps[i].th = (sh + 7) / 8;
        if (st->comps[i].tw < 1)
            st->comps[i].tw = 1;
        if (st->comps[i].th < 1)
            st->comps[i].th = 1;
        if (!st->comps[i].coeff) {
            st->comps[i].bx = mcx * st->comps[i].h;
            st->comps[i].by = mcy * st->comps[i].v;
            if (st->comps[i].bx < 1)
                st->comps[i].bx = 1;
            if (st->comps[i].by < 1)
                st->comps[i].by = 1;
            if ((size_t)st->comps[i].bx > 65535u || (size_t)st->comps[i].by > 65535u)
                return 0;
            st->comps[i].coeff = (int *)hpe_calloc(
                (size_t)st->comps[i].bx * st->comps[i].by * 64, sizeof(int));
            if (!st->comps[i].coeff)
                return 0;
        }
    }
    return 1;
}

/* SOS header: component-to-table mapping plus the band and successive-
 * approximation parameters. */
static int jpeg_parse_scan_header(JpegState *st, ByteReader *br, JpegScan *sc)
{
    int a;
    sc->nscan = br_u8(br);
    if (br->error || sc->nscan < 1 || sc->nscan > JPEG_MAX_COMPONENTS)
        return 0;
    for (a = 0; a < sc->nscan; a++) {
        int cs = br_u8(br);
        int tdta = br_u8(br);
        int idx = -1, c;
        for (c = 0; c < st->ncomp; c++) {
            if (st->comps[c].id == cs)
                idx = c;
        }
        if (idx < 0)
            return 0;
        sc->comp[a] = idx;
        sc->dc[a] = (tdta >> 4) & 0x0F;
        sc->ac[a] = tdta & 0x0F;
        st->comps[idx].dc_tbl = sc->dc[a];
        st->comps[idx].ac_tbl = sc->ac[a];
    }
    sc->ss = br_u8(br);
    sc->se = br_u8(br);
    a = br_u8(br);
    sc->ah = (a >> 4) & 0x0F; /* successive approximation: high bit position */
    sc->al = a & 0x0F;        /* ... and the low one for this scan */
    return !br->error;
}

/* A one-component scan is not interleaved: its MCU *is* a single block, so the
 * entropy stream walks that component's block grid in raster order. An
 * interleaved scan walks MCUs of max_h x max_v samples and emits h*v blocks per
 * component inside each one. The two orders differ (the interleaved one groups
 * blocks per MCU), so getting this wrong shuffles the coefficients without ever
 * reading past the buffer. */
static void jpeg_scan_dims(JpegState *st, JpegScan *sc)
{
    int i;
    sc->non_interleaved = (sc->nscan == 1);
    if (sc->non_interleaved) {
        sc->mcus_x = st->comps[sc->comp[0]].tw;
        sc->mcus_y = st->comps[sc->comp[0]].th;
    } else {
        sc->mcus_x = (st->width + st->max_h * 8 - 1) / (st->max_h * 8);
        sc->mcus_y = (st->height + st->max_v * 8 - 1) / (st->max_v * 8);
    }
    for (i = 0; i < st->ncomp; i++)
        st->comps[i].dc_pred = 0; /* predictors restart with every scan */
}

/* Baseline (sequential) block: DC and AC in one pass. Ss/Se are 0/63 by
 * definition, so the progressive branch below must not be reached. */
static int jpeg_decode_block_seq(JpegState *st, const JpegScan *sc, JpegBits *bits, int ci,
                                 int cc, int *blk)
{
    int s, k;
    memset(blk, 0, 64 * sizeof(int));
    if (!st->huff_dc_valid[sc->dc[ci]] || !st->huff_ac_valid[sc->ac[ci]])
        return 0;
    s = jpeg_huff_decode(bits, &st->huff_dc[sc->dc[ci]]);
    if (s < 0)
        return 0;
    st->comps[cc].dc_pred += jpeg_extend(bits, s);
    blk[0] = st->comps[cc].dc_pred;
    for (k = 1; k < 64;) {
        int rs = jpeg_huff_decode(bits, &st->huff_ac[sc->ac[ci]]);
        int r, sz;
        if (rs < 0)
            return 0;
        r = (rs >> 4) & 0x0F;
        sz = rs & 0x0F;
        if (sz == 0) {
            if (r == 15) {
                k += 16;
                continue;
            }
            break; /* EOB */
        }
        k += r;
        if (k > 63)
            break;
        blk[k_jpeg_zigzag[k]] = jpeg_extend(bits, sz);
        k++;
    }
    return 1;
}

/* Progressive DC scan. There is no EOB run here: every block carries its own
 * DC. The first pass sends a difference, the refinement pass a single bit
 * saying whether the magnitude gains this plane. */
static int jpeg_decode_block_dc_prog(JpegState *st, const JpegScan *sc, JpegBits *bits, int ci,
                                     int cc, int *blk)
{
    if (sc->ah == 0) {
        int s, diff;
        if (!st->huff_dc_valid[sc->dc[ci]])
            return 0;
        s = jpeg_huff_decode(bits, &st->huff_dc[sc->dc[ci]]);
        if (s < 0)
            return 0;
        diff = jpeg_extend(bits, s);
        st->comps[cc].dc_pred += diff;
        blk[0] = st->comps[cc].dc_pred << sc->al;
    } else {
        int bit = jpeg_bits(bits, 1);
        if (bit < 0)
            return 0;
        if (bit)
            blk[0] |= (1 << sc->al);
    }
    return 1;
}

/* Progressive AC first pass: the band ss..se of each block, with an end-of-band
 * run covering the remaining blocks. */
static int jpeg_decode_block_ac_first(JpegState *st, JpegScan *sc, JpegBits *bits, int ci, int *blk)
{
    int kk;
    if (!st->huff_ac_valid[sc->ac[ci]])
        return 0;
    if (sc->eobrun > 0) {
        sc->eobrun--;
        return 1;
    }
    for (kk = sc->ss; kk <= sc->se && !bits->eof;) {
        int rs = jpeg_huff_decode(bits, &st->huff_ac[sc->ac[ci]]);
        int r, sz;
        if (rs < 0)
            return 0;
        r = (rs >> 4) & 0x0F;
        sz = rs & 0x0F;
        if (sz == 0) {
            if (r != 15) {
                /* EOBn: the band of this block is finished and so are the next
                 * 2^r + extra *blocks*. That count includes the one being
                 * decoded, so blocks still to skip is one less — forgetting the
                 * -1 silently swallows a whole block's coefficients. */
                sc->eobrun = (1 << r) - 1;
                if (r > 0) {
                    int extra = jpeg_bits(bits, r);
                    if (extra < 0)
                        return 0;
                    sc->eobrun += extra;
                }
                break;
            }
            kk += 16; /* ZRL: 16 zeros */
            continue;
        }
        kk += r;
        if (kk > sc->se)
            break;
        blk[k_jpeg_zigzag[kk]] = jpeg_extend(bits, sz) << sc->al;
        kk++;
    }
    return 1;
}

/* Apply one correction bit to an already-nonzero coefficient. Returns 0 when
 * the stream ran out mid-bit. */
static int jpeg_refine_coef(JpegBits *bits, int *coef, int p1, int m1)
{
    int bit = jpeg_bits(bits, 1);
    if (bit < 0)
        return 0;
    if (bit && (*coef & p1) == 0)
        *coef += (*coef >= 0) ? p1 : m1;
    return 1;
}

/* Walk from *kkp toward se, feeding a correction bit to every nonzero
 * coefficient and stepping over `r` zero slots; stop after the r-th zero (or at
 * se), leaving *kkp on that slot. Returns 0 when the stream ran out. */
static int jpeg_refine_walk(JpegBits *bits, int *blk, int *kkp, int se, int *rp, int p1, int m1)
{
    int kk = *kkp, r = *rp;
    for (;;) {
        int *coef;
        if (kk > se)
            break;
        coef = &blk[k_jpeg_zigzag[kk]];
        if (*coef != 0) {
            if (!jpeg_refine_coef(bits, coef, p1, m1)) {
                *kkp = kk;
                *rp = r;
                return 0;
            }
        } else if (--r < 0) {
            break;
        }
        kk++;
    }
    *kkp = kk;
    *rp = r;
    return 1;
}

/* Progressive AC refinement pass: the band ss..se again, but coefficients
 * already set only receive a correction bit (raise the magnitude if the bit is
 * 1) and newly appearing coefficients are +/- the new bit plane. Getting the
 * walk wrong desynchronises the whole scan rather than corrupting one block, so
 * it follows the reference decoder step for step. */
static int jpeg_decode_block_ac_refine(JpegState *st, JpegScan *sc, JpegBits *bits, int ci, int *blk)
{
    int p1 = 1 << sc->al;
    int m1 = -(1 << sc->al);
    int kk = sc->ss;
    if (!st->huff_ac_valid[sc->ac[ci]])
        return 0;
    if (sc->eobrun == 0) {
        for (; kk <= sc->se && !bits->eof; kk++) {
            int rs = jpeg_huff_decode(bits, &st->huff_ac[sc->ac[ci]]);
            int r, sz, s = 0;
            if (rs < 0)
                return 0;
            r = (rs >> 4) & 0x0F;
            sz = rs & 0x0F;
            if (sz) {
                int bit = jpeg_bits(bits, 1);
                if (bit < 0)
                    return 0;
                s = bit ? p1 : m1;
            } else if (r != 15) {
                sc->eobrun = 1 << r;
                if (r > 0) {
                    int extra = jpeg_bits(bits, r);
                    if (extra < 0)
                        return 0;
                    sc->eobrun += extra;
                }
                break;
            }
            if (!jpeg_refine_walk(bits, blk, &kk, sc->se, &r, p1, m1))
                return 0;
            if (s && kk <= sc->se)
                blk[k_jpeg_zigzag[kk]] = s;
        }
    }
    /* rest of the band for an EOB block: only correction bits for nonzero entries */
    for (; kk <= sc->se && !bits->eof; kk++) {
        int *coef = &blk[k_jpeg_zigzag[kk]];
        if (*coef != 0 && !jpeg_refine_coef(bits, coef, p1, m1))
            return 0;
    }
    if (sc->eobrun > 0)
        sc->eobrun--;
    return 1;
}

/* Dispatch one block to the decoder its scan type calls for. `cc` is the
 * component index, `ci` its position within the scan. */
static int jpeg_decode_block(JpegState *st, JpegScan *sc, JpegBits *bits, int ci, int cc, int *blk)
{
    if (!st->progressive)
        return jpeg_decode_block_seq(st, sc, bits, ci, cc, blk);
    if (sc->ss == 0)
        return jpeg_decode_block_dc_prog(st, sc, bits, ci, cc, blk);
    if (sc->ah == 0)
        return jpeg_decode_block_ac_first(st, sc, bits, ci, blk);
    return jpeg_decode_block_ac_refine(st, sc, bits, ci, blk);
}

/* Decode one component's blocks inside a single MCU. Returns 0 to abort the
 * whole scan (the stream desynchronised). */
static int jpeg_scan_component(JpegState *st, JpegScan *sc, JpegBits *bits, int mx, int my, int ci,
                               int *scratch)
{
    int cc = sc->comp[ci];
    int bh = sc->non_interleaved ? 1 : st->comps[cc].h;
    int bv = sc->non_interleaved ? 1 : st->comps[cc].v;
    int byi;
    for (byi = 0; byi < bv && !bits->eof; byi++) {
        int bxi;
        for (bxi = 0; bxi < bh && !bits->eof; bxi++) {
            int bx = mx * bh + bxi;
            int by = my * bv + byi;
            int *blk;
            /* A block beyond the component's true extent is part of an
             * interleaved MCU but was never written to the file... except that
             * it *was*: the encoder still emits h*v blocks per MCU. Decode it so
             * the bit stream stays in phase, then throw the result away by
             * pointing at a scratch block instead of the array. Skipping the
             * decode instead desynchronises the scan. */
            if (bx >= st->comps[cc].tw || by >= st->comps[cc].th)
                blk = scratch;
            else
                blk = st->comps[cc].coeff + ((size_t)by * st->comps[cc].bx + bx) * 64;
            if (!jpeg_decode_block(st, sc, bits, ci, cc, blk))
                return 0;
        }
    }
    return 1;
}

/* Resynchronise at a restart marker: consume entropy until the RSTn marker,
 * step over its two bytes, and reset the DC predictors and the end-of-band
 * run. */
static void jpeg_restart(JpegState *st, JpegScan *sc, JpegBits *bits)
{
    int ci;
    while (!bits->eof && bits->marker == 0)
        jpeg_bits(bits, 1);
    if (bits->marker >= 0xD0 && bits->marker <= 0xD7)
        bits->br.pos += 2; /* skip 0xFF and the marker byte */
    bits->eof = 0;
    bits->marker = 0;
    bits->bitbuf = 0;
    bits->bitcnt = 0;
    for (ci = 0; ci < sc->nscan; ci++)
        st->comps[sc->comp[ci]].dc_pred = 0;
    sc->eobrun = 0;
}

/* Walk every MCU of a scan, decoding each component's blocks in order. */
static void jpeg_scan_mcus(JpegState *st, JpegScan *sc, JpegBits *bits)
{
    int scratch[64];
    int mx, my, mcu = 0;
    for (my = 0; my < sc->mcus_y; my++) {
        for (mx = 0; mx < sc->mcus_x; mx++, mcu++) {
            int ci;
            if (st->restart_interval && mcu > 0 && (mcu % st->restart_interval) == 0)
                jpeg_restart(st, sc, bits);
            for (ci = 0; ci < sc->nscan && !bits->eof; ci++) {
                if (!jpeg_scan_component(st, sc, bits, mx, my, ci, scratch))
                    return;
            }
        }
    }
}

/* Parse an SOS header, size the coefficient grids and decode the scan. Stores
 * the entropy start offset so the caller can resync afterwards. */
static int jpeg_do_scan(JpegState *st, ByteReader *br, size_t *entropy_start)
{
    JpegScan sc;
    JpegBits bits;
    memset(&sc, 0, sizeof(sc));
    if (!jpeg_parse_scan_header(st, br, &sc))
        return 0;
    *entropy_start = br->pos;
    jpeg_bits_init(&bits, br->data, br->len, br->pos);
    if (!jpeg_init_grids(st))
        return 0;
    jpeg_scan_dims(st, &sc);
    jpeg_scan_mcus(st, &sc, &bits);
    return 1;
}

/* IDCT each component's coefficient grid into its own 8-bit plane. */
static int jpeg_idct_planes(JpegState *st, uint8_t **planes, int *plane_w)
{
    int c;
    for (c = 0; c < st->ncomp; c++) {
        int cw, ch, bxi, byi;
        if (!st->comps[c].coeff)
            return 0;
        cw = st->comps[c].bx * 8;
        ch = st->comps[c].by * 8;
        planes[c] = (uint8_t *)hpe_alloc((size_t)cw * ch);
        if (!planes[c])
            return 0;
        plane_w[c] = cw;
        for (byi = 0; byi < st->comps[c].by; byi++) {
            for (bxi = 0; bxi < st->comps[c].bx; bxi++) {
                const int *blk = st->comps[c].coeff + ((size_t)byi * st->comps[c].bx + bxi) * 64;
                jpeg_idct(blk, planes[c] + (size_t)byi * 8 * cw + (size_t)bxi * 8, cw,
                          st->quant_valid[st->comps[c].tq] ? st->quant[st->comps[c].tq] : NULL);
            }
        }
    }
    return 1;
}

/* Map output index `o` of an `s`-fold upsample to a source index and a weight
 * over 256; the sample sits at (o + 0.5) / s - 0.5 with clamped edges. */
static void jpeg_lerp_pos(int o, int s, int src_n, int *idx, int *f)
{
    int n = 2 * o + 1 - s, den = 2 * s;
    if (n <= 0) {
        *idx = 0;
        *f = 0;
        return;
    }
    *idx = n / den;
    *f = (n - *idx * den) * 256 / den;
    if (*idx >= src_n - 1) {
        *idx = src_n - 1;
        *f = 0;
    }
}

/* Triangular (integer) resample of two neighbours by weight f/256. */
static uint8_t jpeg_lerp(uint8_t a, uint8_t b, int f)
{
    if (!f)
        return a;
    return (uint8_t)(((256 - f) * a + f * b + 128) >> 8);
}

/* Horizontal pass of the upsample: src_w -> out_w, row count unchanged. */
static void jpeg_upsample_h(const uint8_t *src, int src_w, int rows, int out_w, int sh,
                            uint8_t *hbuf)
{
    int x, y;
    for (y = 0; y < rows; y++) {
        const uint8_t *row = src + (size_t)y * src_w;
        for (x = 0; x < out_w; x++) {
            int i, f;
            jpeg_lerp_pos(x, sh, src_w, &i, &f);
            hbuf[(size_t)y * out_w + x] = jpeg_lerp(row[i], f ? row[i + 1] : row[i], f);
        }
    }
}

/* Vertical pass of the upsample: src_h -> out_h. */
static void jpeg_upsample_v(const uint8_t *hbuf, int out_w, int src_h, int out_h, int sv,
                            uint8_t *dst)
{
    int x, y;
    for (y = 0; y < out_h; y++) {
        int i, f;
        const uint8_t *r0, *r1;
        jpeg_lerp_pos(y, sv, src_h, &i, &f);
        r0 = hbuf + (size_t)i * out_w;
        r1 = f ? hbuf + (size_t)(i + 1) * out_w : r0;
        for (x = 0; x < out_w; x++)
            dst[(size_t)y * out_w + x] = jpeg_lerp(r0[x], r1[x], f);
    }
}

/* Upsample one subsampled component to full resolution.
 *
 * Chroma arrives subsampled (4:2:0 halves both axes, 4:2:2 the horizontal one).
 * Replicating the nearest sample — the obvious approach — leaves visible 2x2
 * blocks of colour and, measured against a reference decoder that
 * interpolates, is wrong on roughly a third of all pixels. Every real decoder
 * instead reconstructs each output sample by bilinear sampling at
 * (x + 0.5) * scale - 0.5 with the edges clamped: for a 2x scale that is
 * exactly the "triangular" filter (3/4 nearest + 1/4 neighbour) browsers use,
 * and the rounded integer weights reproduce it bit for bit. */
static int jpeg_upsample_component(JpegState *st, int c, uint8_t **planes, int *plane_w)
{
    int sh, sv, ch;
    uint8_t *hbuf, *dst;
    if (st->max_h % st->comps[c].h || st->max_v % st->comps[c].v)
        return 1; /* non-integer ratio: keep the plain nearest mapping */
    sh = st->max_h / st->comps[c].h;
    sv = st->max_v / st->comps[c].v;
    if (sh <= 1 && sv <= 1)
        return 1; /* already full resolution */
    if (sh < 1)
        sh = 1;
    if (sv < 1)
        sv = 1;
    ch = st->comps[c].by * 8;
    hbuf = (uint8_t *)hpe_alloc((size_t)st->width * (size_t)ch);
    if (!hbuf)
        return 0;
    jpeg_upsample_h(planes[c], plane_w[c], ch, st->width, sh, hbuf);
    dst = (uint8_t *)hpe_alloc((size_t)st->width * (size_t)st->height);
    if (!dst) {
        hpe_free(hbuf);
        return 0;
    }
    jpeg_upsample_v(hbuf, st->width, ch, st->height, sv, dst);
    hpe_free(hbuf);
    hpe_free(planes[c]);
    planes[c] = dst;
    plane_w[c] = st->width;
    return 1;
}

/* Convert the (now full-resolution) planes to RGB. Chroma is left at neutral
 * 128 for a grayscale file. The YCbCr constants are the usual
 * 1.402 / 0.344136 / 0.714136 / 1.772 scaled by 1024. */
static void jpeg_to_rgb(JpegState *st, uint8_t **planes, const int *plane_w, uint32_t *pixels)
{
    int x, y;
    int yc = 0, cbc = -1, crc = -1;
    if (st->ncomp >= 3) {
        yc = 0;
        cbc = 1;
        crc = 2;
    }
    for (y = 0; y < st->height; y++) {
        for (x = 0; x < st->width; x++) {
            int yy = planes[yc][(size_t)y * plane_w[yc] + x];
            int cb = 128, cr = 128;
            int r, g, b;
            if (cbc >= 0)
                cb = planes[cbc][(size_t)y * plane_w[cbc] + x];
            if (crc >= 0)
                cr = planes[crc][(size_t)y * plane_w[crc] + x];
            r = yy + ((cr - 128) * 1436) / 1024;
            g = yy - ((cb - 128) * 352 + (cr - 128) * 731) / 1024;
            b = yy + ((cb - 128) * 1815) / 1024;
            if (r < 0)
                r = 0;
            if (r > 255)
                r = 255;
            if (g < 0)
                g = 0;
            if (g > 255)
                g = 255;
            if (b < 0)
                b = 0;
            if (b > 255)
                b = 255;
            pixels[(size_t)y * st->width + x] =
                pack_rgba((uint32_t)r, (uint32_t)g, (uint32_t)b, 255);
        }
    }
}

/* Walk the JPEG segment stream, filling `st`. Returns 1 on a complete parse
 * (EOI reached or the stream ran out of markers after a scan), 0 on error. */
static int jpeg_parse_segments(JpegState *st, ByteReader *br)
{
    br->pos = 2; /* skip SOI */
    for (;;) {
        int marker = jpeg_next_marker(br);
        uint32_t seglen;
        size_t seg_end;
        if (marker < 0)
            return 0;
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7))
            continue;
        if (marker == 0xD9) {
            /* end of image: normal once at least one scan has been decoded,
             * premature before that */
            return st->have_scan;
        }
        seglen = br_u16be(br);
        if (br->error || seglen < 2)
            return 0;
        seg_end = br->pos + seglen - 2;
        if (seg_end > br->len)
            return 0;

        if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
            if (!jpeg_parse_sof(st, br, marker))
                return 0;
        } else if (marker == 0xDB) {
            jpeg_parse_dqt(st, br, seg_end);
        } else if (marker == 0xC4) {
            jpeg_parse_dht(st, br, seg_end);
        } else if (marker == 0xDD) {
            st->restart_interval = (int)br_u16be(br);
            if (br->error)
                return 0;
        } else if (marker == 0xDA) {
            size_t entropy_start;
            if (!jpeg_do_scan(st, br, &entropy_start))
                return 0;
            st->have_scan = 1;
            /* Resync from the start of this scan's entropy data. The scan loop
             * stops as soon as it has emitted the number of MCUs it computed,
             * which leaves the bit reader parked at an arbitrary point inside
             * the payload — or even past a marker it never pulled bits for. */
            if (!jpeg_resync(br, entropy_start))
                return 1; /* no further marker: the file ends here */
            continue;
        }
        /* every other segment is skipped wholesale */
        br->pos = seg_end;
        if (br->error)
            return 0;
    }
}

/* IDCT, upsample and colour-convert the decoded coefficients into RGBA. */
static int jpeg_reconstruct(JpegState *st, uint32_t **out_pixels, uint8_t **planes, int *plane_w)
{
    uint32_t *pixels;
    int c;
    if (st->width == 0 || st->height == 0 || st->ncomp == 0)
        return 0;
    if (!st->quant_valid[0])
        return 0;
    if (!jpeg_idct_planes(st, planes, plane_w))
        return 0;
    for (c = 0; c < st->ncomp; c++) {
        if (!jpeg_upsample_component(st, c, planes, plane_w))
            return 0;
    }
    pixels = (uint32_t *)hpe_alloc((size_t)st->width * st->height * sizeof(uint32_t));
    if (!pixels)
        return 0;
    jpeg_to_rgb(st, planes, plane_w, pixels);
    *out_pixels = pixels;
    return 1;
}

static int decode_jpeg(ByteReader *br, HpeImage *out)
{
    JpegState st;
    uint32_t *pixels = NULL;
    uint8_t *planes[JPEG_MAX_COMPONENTS];
    int plane_w[JPEG_MAX_COMPONENTS];
    int i, c;

    memset(&st, 0, sizeof(st));
    memset(planes, 0, sizeof(planes));

    if (jpeg_parse_segments(&st, br) &&
        jpeg_reconstruct(&st, &pixels, planes, plane_w)) {
        for (c = 0; c < st.ncomp; c++) {
            hpe_free(st.comps[c].coeff);
            hpe_free(planes[c]);
        }
        out->width = st.width;
        out->height = st.height;
        out->pixels = pixels;
        out->owned = 1;
        out->format = HPE_IMG_JPEG;
        out->frame_count = 1;
        return 1;
    }

    for (i = 0; i < st.ncomp; i++) {
        hpe_free(st.comps[i].coeff);
        hpe_free(planes[i]);
    }
    hpe_free(pixels);
    return 0;
}

/* ------------------------------------------------------------------ */
/* scaling                                                             */
/* ------------------------------------------------------------------ */

/**
 * @brief 把索引夹到 `[0, n-1]` / Clamp an index to `[0, n-1]`
 *
 * @param[in]     i 待夹取的索引 / Index to clamp
 * @param[in]     n 有效元素个数 / Number of valid elements
 *
 * @return 夹取后的索引 / The clamped index
 *
 * @note `n <= 0` 时返回 0 / Returns 0 when `n <= 0`
 *
 * @see hpe_image_scale
 */
static int scale_clamp(int i, int n)
{
    if (n <= 0)
        return 0;
    if (i < 0)
        return 0;
    if (i > n - 1)
        return n - 1;
    return i;
}

/**
 * @brief 在源图上按像素中心双线性采样一个目标像素 / Bilinearly sample one destination pixel at pixel centres
 *
 * @param[in]     src 源图像 / Source image
 * @param[in]     x0 左列索引（已夹取）/ Left column index, already clamped
 * @param[in]     y0 上行索引（已夹取）/ Upper row index, already clamped
 * @param[in]     x1 右列索引（已夹取）/ Right column index, already clamped
 * @param[in]     y1 下行索引（已夹取）/ Lower row index, already clamped
 * @param[in]     fx 水平小数 / Horizontal fraction
 * @param[in]     fy 垂直小数 / Vertical fraction
 *
 * @return 打包后的 `0xAARRGGBB` 目标像素（权重和为 0 时为 0）/ The packed `0xAARRGGBB` destination pixel, or 0 when the weight sum is 0
 *
 * @note 每个样本权重乘以其自身 alpha，透明像素不会把颜色拖向黑色 / Each sample's weight is scaled by its own alpha so transparent pixels do not drag the colour towards black
 *
 * @see hpe_image_scale pack_rgba
 */
static uint32_t scale_sample(const HpeImage *src, int x0, int y0, int x1, int y1, float fx, float fy)
{
    uint32_t p00 = src->pixels[(size_t)y0 * src->width + x0];
    uint32_t p10 = src->pixels[(size_t)y0 * src->width + x1];
    uint32_t p01 = src->pixels[(size_t)y1 * src->width + x0];
    uint32_t p11 = src->pixels[(size_t)y1 * src->width + x1];
    float w00 = (1.0f - fx) * (1.0f - fy) * (float)((p00 >> 24) & 0xFF);
    float w10 = fx * (1.0f - fy) * (float)((p10 >> 24) & 0xFF);
    float w01 = (1.0f - fx) * fy * (float)((p01 >> 24) & 0xFF);
    float w11 = fx * fy * (float)((p11 >> 24) & 0xFF);
    float wsum = w00 + w10 + w01 + w11;
    float r, g, b, wa;

    if (wsum <= 0.0f)
        return 0;
    r = (float)((p00 >> 16) & 0xFF) * w00 + (float)((p10 >> 16) & 0xFF) * w10 +
        (float)((p01 >> 16) & 0xFF) * w01 + (float)((p11 >> 16) & 0xFF) * w11;
    g = (float)((p00 >> 8) & 0xFF) * w00 + (float)((p10 >> 8) & 0xFF) * w10 +
        (float)((p01 >> 8) & 0xFF) * w01 + (float)((p11 >> 8) & 0xFF) * w11;
    b = (float)(p00 & 0xFF) * w00 + (float)(p10 & 0xFF) * w10 + (float)(p01 & 0xFF) * w01 +
        (float)(p11 & 0xFF) * w11;
    wa = wsum / 255.0f;
    return pack_rgba((uint32_t)((r / wsum) + 0.5f), (uint32_t)((g / wsum) + 0.5f),
                     (uint32_t)((b / wsum) + 0.5f), (uint32_t)(wa + 0.5f));
}

int hpe_image_scale(const HpeImage *src, int w, int h, HpeImage *out)
{
    uint32_t *dst;
    int x, y;

    if (!src || !src->pixels || !out || w <= 0 || h <= 0 || src->width <= 0 || src->height <= 0)
        return 0;
    dst = (uint32_t *)hpe_alloc((size_t)w * (size_t)h * sizeof(uint32_t));
    if (!dst)
        return 0;

    for (y = 0; y < h; y++) {
        /* sample at pixel centres so the result is symmetric */
        float sy = ((float)y + 0.5f) * (float)src->height / (float)h - 0.5f;
        int y0 = (int)floorf(sy);
        float fy = sy - (float)y0;
        int y1 = y0 + 1;
        y0 = scale_clamp(y0, src->height);
        y1 = scale_clamp(y1, src->height);
        for (x = 0; x < w; x++) {
            float sx = ((float)x + 0.5f) * (float)src->width / (float)w - 0.5f;
            int x0 = (int)floorf(sx);
            float fx = sx - (float)x0;
            int x1 = x0 + 1;
            dst[(size_t)y * w + x] =
                scale_sample(src, scale_clamp(x0, src->width), y0, scale_clamp(x1, src->width), y1,
                             fx, fy);
        }
    }

    out->width = w;
    out->height = h;
    out->pixels = dst;
    out->owned = 1;
    out->format = src->format;
    out->frame_count = src->frame_count;
    return 1;
}

void hpe_image_free(HpeImage *img)
{
    if (!img)
        return;
    if (img->owned && img->pixels)
        hpe_free(img->pixels);
    img->pixels = NULL;
    img->width = 0;
    img->height = 0;
    img->owned = 0;
}

/* ------------------------------------------------------------------ */
/* decoding entry points                                               */
/* ------------------------------------------------------------------ */

/* Dispatch on the sniffed magic, falling back to the caller's extension hint
 * when the bytes are inconclusive. */
static int decode_any(const uint8_t *data, size_t len, HpeImageFormat hint, HpeImage *out)
{
    ByteReader br;
    HpeImageFormat f = hpe_image_sniff_format(data, len);
    if (f == HPE_IMG_UNKNOWN)
        f = hint;
    if (f == HPE_IMG_UNKNOWN)
        return 0;
    br_init(&br, data, len);
    switch (f) {
    case HPE_IMG_PNG:
        return decode_png(&br, out);
    case HPE_IMG_JPEG:
        return decode_jpeg(&br, out);
    case HPE_IMG_GIF:
        return decode_gif(&br, out);
    case HPE_IMG_BMP:
        return decode_bmp(&br, out);
    default:
        return 0;
    }
}

int hpe_image_decode(const uint8_t *data, size_t len, HpeImageFormat hint, HpeImage *out)
{
    if (!data || len < 8 || !out)
        return 0;
    return decode_any(data, len, hint, out);
}

int hpe_image_load_file(const char *path, HpeImageFormat hint, HpeImage *out)
{
    size_t size = 0;
    char *buf;
    int ok;
    if (!path || !out)
        return 0;
    buf = hpe_read_file(path, &size);
    if (!buf)
        return 0;
    if (getenv("HPE_TRACE_IMAGE"))
        fprintf(stderr, "image decode: %s (%zu bytes)\n", path, size);
    if (hint == HPE_IMG_UNKNOWN)
        hint = hpe_image_format_from_path(path);
    ok = decode_any((const uint8_t *)buf, size, hint, out);
    hpe_free(buf);
    return ok;
}

/* ------------------------------------------------------------------ */
/* decoded-image cache                                                 */
/* ------------------------------------------------------------------ */

typedef struct ImageCacheEntry {
    char *path;
    HpeImageFormat hint;
    int want_w, want_h;
    HpeImage image;
    struct ImageCacheEntry *next;
} ImageCacheEntry;

struct HpeImageCache {
    ImageCacheEntry *head;
    int count;
};

HpeImageCache *hpe_image_cache_new(void)
{
    return (HpeImageCache *)hpe_calloc(1, sizeof(HpeImageCache));
}

void hpe_image_cache_free(HpeImageCache *cache)
{
    ImageCacheEntry *e;
    if (!cache)
        return;
    e = cache->head;
    while (e) {
        ImageCacheEntry *next = e->next;
        hpe_image_free(&e->image);
        hpe_free(e->path);
        hpe_free(e);
        e = next;
    }
    hpe_free(cache);
}

int hpe_image_cache_count(const HpeImageCache *cache)
{
    return cache ? cache->count : 0;
}

const HpeImage *hpe_image_cache_get(HpeImageCache *cache, const char *path, HpeImageFormat hint,
                                    int want_w, int want_h)
{
    ImageCacheEntry *e;
    HpeImage decoded;
    HpeImageFormat stored;
    if (!cache || !path)
        return NULL;

    for (e = cache->head; e; e = e->next) {
        if (e->want_w == want_w && e->want_h == want_h && e->hint == hint &&
            strcmp(e->path, path) == 0) {
            if (getenv("HPE_TRACE_IMAGE"))
                fprintf(stderr, "cache hit: %s (count=%d)\n", path, cache->count);
            return &e->image;
        }
    }
    if (getenv("HPE_TRACE_IMAGE"))
        fprintf(stderr, "cache miss: %s (count=%d)\n", path, cache->count);

    /* the key keeps the caller's hint verbatim: lookups always come in with the
     * same argument, so storing the sniffed format would make every later
     * lookup with HPE_IMG_UNKNOWN miss forever / 缓存键保存调用方的原始 hint：
     * 查找参数恒定，若存嗅探出的格式则后续 UNKNOWN 查找永不命中 */
    stored = hint;
    if (hint == HPE_IMG_UNKNOWN)
        hint = hpe_image_format_from_path(path);
    memset(&decoded, 0, sizeof(decoded));
    if (!hpe_image_load_file(path, hint, &decoded))
        return NULL;

    if (want_w > 0 && want_h > 0 && (decoded.width != want_w || decoded.height != want_h)) {
        HpeImage scaled;
        memset(&scaled, 0, sizeof(scaled));
        if (hpe_image_scale(&decoded, want_w, want_h, &scaled)) {
            hpe_image_free(&decoded);
            decoded = scaled;
        }
    }

    e = (ImageCacheEntry *)hpe_calloc(1, sizeof(ImageCacheEntry));
    if (!e) {
        hpe_image_free(&decoded);
        return NULL;
    }
    e->path = hpe_strdup(path);
    e->hint = stored;
    e->want_w = want_w;
    e->want_h = want_h;
    e->image = decoded;
    e->next = cache->head;
    cache->head = e;
    cache->count++;
    return &e->image;
}

/* ------------------------------------------------------------------ */
/* document resource resolution                                        */
/* ------------------------------------------------------------------ */

/* The base directory the current document resolves relative image URLs
 * against. Layout and paint both need it, and neither reaches into the engine,
 * so it lives here next to the cache -- the same pattern hpe_font.c uses for
 * its process-global font cache. */
static char g_image_base_dir[1024];
static HpeImageCache *g_image_cache = NULL;

void hpe_image_set_base_dir(const char *dir)
{
    snprintf(g_image_base_dir, sizeof(g_image_base_dir), "%s", dir ? dir : "");
}

const char *hpe_image_base_dir(void)
{
    return g_image_base_dir;
}

/**
 * @brief 去掉 URL 两端空白与成对引号 / Strip surrounding whitespace and an optional quote pair
 *
 * @param[in,out] src 指向 URL 起始的指针，就地前移到去空白后的首字符 / Pointer to the URL start, advanced past leading whitespace
 * @param[in]     out 接收去引号后的字节数 / Receives the length after quote removal
 *
 * @return 去空白与引号后的文本长度（不含结尾 NUL）/ Length after stripping, excluding any terminating NUL
 *
 * @note 只处理 ASCII 空白（空格 / Tab / CR / LF）；首尾引号须成对且相同才剥 / Handles ASCII whitespace only; a quote pair is stripped only when both ends match
 *
 * @see hpe_image_resolve_url
 */
static size_t strip_url_noise(const char **src, size_t *out)
{
    const char *s = *src;
    size_t n;
    while (*s && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r'))
        s++;
    n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' || s[n - 1] == '\r'))
        n--;
    if (n >= 2 && (s[0] == '"' || s[0] == '\'') && s[n - 1] == s[0]) {
        s++;
        n -= 2;
    }
    *src = s;
    *out = n;
    return n;
}

/**
 * @brief 判断 URL 是否带 scheme（排除 Windows 盘符）/ Test whether a URL carries a scheme, excluding a Windows drive letter
 *
 * @param[in]     buf URL 文本（已去空白引号）/ URL text, already stripped
 * @param[in]     n 文本长度 / Text length
 *
 * @return scheme 冒号位置（0 基），无 scheme 或为盘符时返回 -1 / The scheme colon index, or -1 when absent or a drive letter
 *
 * @note `C:\...` / `c:\...` 视为绝对路径而非 scheme / `C:\...` and `c:\...` are treated as absolute paths, not schemes
 *
 * @see hpe_image_resolve_url
 */
static int url_scheme_colon(const char *buf, size_t n)
{
    size_t i, j;
    int is_drive;
    for (i = 0; i + 1 < n; i++)
        if (buf[i] == ':')
            break;
    if (i + 1 >= n)
        return -1;
    /* a single leading letter followed by ':' is a drive letter, not a scheme */
    is_drive = (i == 1 && ((buf[0] >= 'A' && buf[0] <= 'Z') ||
                           (buf[0] >= 'a' && buf[0] <= 'z')));
    if (is_drive)
        return -1;
    for (j = 0; j < i; j++) {
        char c = buf[j];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '+' || c == '-' || c == '.'))
            return -1;
    }
    return (int)i;
}

/**
 * @brief 解析 `file:` URL，其余 scheme 一律不支持 / Resolve a `file:` URL; every other scheme is unsupported
 *
 * @param[in]     buf URL 文本 / URL text
 * @param[in]     colon scheme 冒号位置 / Scheme colon index
 * @param[out]    out 接收解析结果的缓冲 / Buffer that receives the resolved path
 * @param[in]     out_size 缓冲容量 / Buffer capacity
 *
 * @return 解析出非空路径返回 1，否则 0 / 1 when a non-empty path was produced, otherwise 0
 *
 * @note `file:///x` 的多余前导 `/` 会被剥离 / Redundant leading slashes of `file:///x` are stripped
 *
 * @see hpe_image_resolve_url url_scheme_colon
 */
static int url_resolve_scheme(const char *buf, int colon, char *out, size_t out_size)
{
    const char *p;
    if (!(colon == 5 && hpe_str_ieq_n(buf, "file:", 5)))
        return 0;
    p = buf + 5;
    while (*p == '/')
        p++;
    snprintf(out, out_size, "%s", p);
    return out[0] != '\0';
}

int hpe_image_resolve_url(const char *src, char *out, size_t out_size)
{
    char buf[1024];
    size_t n;
    int colon;

    if (!src || !out || out_size == 0)
        return 0;
    out[0] = '\0';
    strip_url_noise(&src, &n);
    if (n == 0 || n >= sizeof(buf))
        return 0;
    memcpy(buf, src, n);
    buf[n] = '\0';

    if (buf[0] == '#') /* a fragment-only reference names nothing to load */
        return 0;
    colon = url_scheme_colon(buf, n);
    if (colon >= 0)
        return url_resolve_scheme(buf, colon, out, out_size);

    if (hpe_path_is_absolute(buf)) {
        snprintf(out, out_size, "%s", buf);
        return 1;
    }
    hpe_path_join(out, out_size, g_image_base_dir[0] ? g_image_base_dir : ".", buf);
    return 1;
}

HpeImageCache *hpe_image_shared_cache(void)
{
    if (!g_image_cache)
        g_image_cache = hpe_image_cache_new();
    return g_image_cache;
}

void hpe_image_shutdown(void)
{
    if (g_image_cache) {
        hpe_image_cache_free(g_image_cache);
        g_image_cache = NULL;
    }
    g_image_base_dir[0] = '\0';
}
