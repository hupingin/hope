/* Hope UI Engine - color emoji sprites (COLRv0 layers from a system emoji font) */
#ifndef HPE_EMOJI_H
#define HPE_EMOJI_H

#include "base/hpe_types.h"
#include "render/hpe_pixmap.h"

/* one cached color emoji sprite. `pixels` is w*h 0xAARRGGBB values with
 * straight alpha, owned by the sprite cache (borrow, do not free).
 * `left`/`baseline` place the box relative to the pen point on the baseline. */
typedef struct HpeGlyphSprite {
    int w, h;
    int left;               /* box left offset from the pen x / 盒左缘相对笔位 x 的偏移 */
    int baseline;           /* rows above the baseline / 基线之上的行数 */
    const uint32_t *pixels; /* straight-alpha 0xAARRGGBB pixels / 直通 alpha 像素 */
} HpeGlyphSprite;

/**
 * @brief 判断文本开头是否为彩色 emoji 序列 / Test whether the text starts a color emoji sequence
 *
 * @details 序列 = emoji 基字符 + 可选的 VS16（`U+FE0F`）/ 肤色修饰符（`U+1F3FB`..`U+1F3FF`）/
 *          区域指示符对（旗帜）。ZWJ 链不并入序列（无 GSUB 连字支持，各分量独立渲染）。
 *          A sequence = one emoji base plus an optional VS16 (`U+FE0F`) / skin-tone
 *          modifiers (`U+1F3FB`..`U+1F3FF`) / a regional-indicator pair (flags). ZWJ
 *          chains are not merged (no GSUB ligature support; parts render separately).
 *
 * @param[in]     utf8 UTF-8 文本 / UTF-8 text
 * @param[in]     max_len 参与扫描的最大字节数 / Maximum bytes to scan
 *
 * @return 序列的字节长度；不是彩色序列返回 0 / Byte length of the sequence, or 0 when the text does not start one
 *
 * @note 仅默认彩色呈现或带 VS16 的字符才算；纯文本呈现符号交给单色掩码路径 / Only default-color presentation or VS16-forced characters count; text-presentation symbols stay on the monochrome mask path
 */
int hpe_emoji_seq_len(const char *utf8, int max_len);

/**
 * @brief 取（并缓存）一个彩色 emoji 序列的精灵 / Get (and cache) the sprite of a color emoji sequence
 *
 * @details 读取系统 emoji 字体（`seguiemj.ttf`）的 cmap/COLRv0/CPAL 表，逐层取字形
 *          覆盖率并用调色板色合成 RGBA 精灵；结果按「序列内容 + 字号 + 前景色」缓存。
 *          The cmap/COLRv0/CPAL tables of the system emoji font (`seguiemj.ttf`) are
 *          parsed, each layer's glyph coverage is fetched and composited with its
 *          palette colour into an RGBA sprite; results are cached per sequence,
 *          pixel size and foreground colour.
 *
 * @param[in]     pixel_size 字号（像素）/ Font size in pixels
 * @param[in]     seq UTF-8 序列（`hpe_emoji_seq_len` 的输入）/ UTF-8 sequence (input of `hpe_emoji_seq_len`)
 * @param[in]     len 序列字节长度 / Sequence length in bytes
 * @param[in]     fg 前景色（调色板索引 `0xFFFF` 的层使用）/ Foreground colour (used by layers with palette index `0xFFFF`)
 * @param[out]    out 接收精灵 / Receives the sprite
 *
 * @return 成功返回 1；无 COLR 数据、字体缺失或序列非法返回 0 / 1 on success, 0 when COLR data, the font or the sequence is missing
 *
 * @note 返回的像素由缓存持有，勿释放；调用 `hpe_emoji_shutdown` 后全部失效 / The pixels are cache-owned and must not be freed; everything is invalidated by `hpe_emoji_shutdown`
 *
 * @see hpe_emoji_seq_len hpe_emoji_shutdown
 */
int hpe_emoji_sprite(float pixel_size, const char *seq, int len, HpeColor fg,
                     HpeGlyphSprite *out);

/**
 * @brief 按直通 alpha 把精灵混合到像素缓冲 / Blit a sprite onto a pixel buffer with straight alpha
 *
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     sp 精灵 / The sprite
 * @param[in]     x 精灵盒左缘的设备 x / Device x of the sprite box's left edge
 * @param[in]     y 精灵盒顶缘的设备 y / Device y of the sprite box's top edge
 *
 * @note 裁剪范围外的像素跳过；结果像素始终不透明（与文本掩码绘制一致） / Pixels outside the clip are skipped; result pixels stay opaque (consistent with the text mask painter)
 *
 * @see hpe_emoji_sprite
 */
void hpe_emoji_blit(HpePixmap *pm, const HpeGlyphSprite *sp, int x, int y);

/**
 * @brief 释放 emoji 模块的全部缓存 / Release every emoji module cache
 *
 * @details 丢弃精灵缓存并关闭所有已打开的 emoji 字体对象。
 *          The sprite cache is dropped and every opened emoji font object is closed.
 *
 * @note 应在引擎退出时调用一次 / Call once at engine shutdown
 *
 * @see hpe_emoji_sprite
 */
void hpe_emoji_shutdown(void);

#endif /* HPE_EMOJI_H */
