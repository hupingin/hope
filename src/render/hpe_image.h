/* Hope UI Engine - image decoding (PNG / JPEG / GIF / BMP) and the decoded
 * image cache. The decoders are self-contained: no third-party code, no
 * platform dependency, so the same bytes decode identically everywhere. */
#ifndef HPE_IMAGE_H
#define HPE_IMAGE_H

#include "base/hpe_types.h"
#include "render/hpe_pixmap.h"

typedef enum HpeImageFormat {
    HPE_IMG_UNKNOWN = 0,
    HPE_IMG_PNG,
    HPE_IMG_JPEG,
    HPE_IMG_GIF,
    HPE_IMG_BMP
} HpeImageFormat;

/* A decoded image: always 32bpp 0xAARRGGBB with straight (non-premultiplied)
 * alpha and a top-down pixel array, so it can be blitted straight into an
 * HpePixmap. `owned` says whether `pixels` came from hpe_alloc and therefore
 * has to be released; an image that views another buffer keeps it borrowed. */
typedef struct HpeImage {
    int width;
    int height;
    uint32_t *pixels; /* row-major, width * height entries */
    int owned;
    HpeImageFormat format;
    int frame_count; /* GIF: number of frames found; 1 for the still formats */
} HpeImage;

/**
 * @brief 从文件扩展名推断图像格式 / Infer the image format from a file name
 *
 * @param[in]     path 文件路径或 URL / File path or URL
 *
 * @return 推断出的格式；无法判断时返回 `HPE_IMG_UNKNOWN` / The inferred format, or `HPE_IMG_UNKNOWN`
 *
 * @note 只看扩展名，不读文件内容 / Only the extension is consulted, never the file contents
 *
 * @see hpe_image_sniff_format
 */
HpeImageFormat hpe_image_format_from_path(const char *path);

/**
 * @brief 从文件头字节嗅探图像格式 / Sniff the image format from the leading bytes
 *
 * @param[in]     data 文件起始字节 / The first bytes of the file
 * @param[in]     len 可用字节数 / Number of bytes available
 *
 * @return 嗅探出的格式；无法判断时返回 `HPE_IMG_UNKNOWN` / The sniffed format, or `HPE_IMG_UNKNOWN`
 *
 * @note 识别 PNG / JPEG / GIF / BMP 四类魔数 / Recognises the PNG, JPEG, GIF and BMP magic numbers
 *
 * @see hpe_image_format_from_path
 */
HpeImageFormat hpe_image_sniff_format(const uint8_t *data, size_t len);

/**
 * @brief 解码一段内存中的图像数据 / Decode image data held in memory
 *
 * @details 先按魔数判格式再分派到对应解码器；`HPE_IMG_UNKNOWN` 时按扩展名再试一次。
 *          The format is sniffed from the magic bytes and dispatched to the matching
 *          decoder; an unknown magic falls back to the hint supplied by the caller.
 *
 * @param[in]     data 图像数据 / The encoded image bytes
 * @param[in]     len 数据长度 / Length of the encoded data
 * @param[in]     hint 扩展名提示（可为 `HPE_IMG_UNKNOWN`）/ Format hint, may be `HPE_IMG_UNKNOWN`
 * @param[out]    out 接收解码结果的图像 / Receives the decoded image
 *
 * @return 成功返回 1，失败返回 0 / 1 on success, 0 on failure
 *
 * @note 失败时不修改 `*out`，调用方无需清理 / `*out` is untouched on failure, so there is nothing for the caller to release
 *
 * @see hpe_image_free hpe_image_load_file
 */
int hpe_image_decode(const uint8_t *data, size_t len, HpeImageFormat hint, HpeImage *out);

/**
 * @brief 解码磁盘上的图像文件 / Decode an image file on disk
 *
 * @param[in]     path 文件路径 / File path
 * @param[in]     hint 扩展名提示（可为 `HPE_IMG_UNKNOWN`）/ Format hint, may be `HPE_IMG_UNKNOWN`
 * @param[out]    out 接收解码结果的图像 / Receives the decoded image
 *
 * @return 成功返回 1，失败（含文件不存在）返回 0 / 1 on success, 0 on failure including a missing file
 *
 * @see hpe_image_decode hpe_image_free
 */
int hpe_image_load_file(const char *path, HpeImageFormat hint, HpeImage *out);

/**
 * @brief 释放解码图像占用的内存 / Release the memory held by a decoded image
 *
 * @param[in,out] img 目标图像；为 `NULL` 时安全无操作 / Target image; a NULL input is a safe no-op
 *
 * @note 只释放本模块自己分配的像素数组（`owned` 为 1 时）/ Only releases a pixel array this module allocated, i.e. when `owned` is 1
 *
 * @see hpe_image_decode
 */
void hpe_image_free(HpeImage *img);

/**
 * @brief 把解码图像缩放为新的像素尺寸 / Resample a decoded image to a new pixel size
 *
 * @details 双线性重采样，按 `alpha` 加权以避免透明边缘发黑。
 *          Bilinear resampling weighted by alpha, so transparent edges do not darken.
 *
 * @param[in]     src 源图像 / Source image
 * @param[in]     w 目标宽度（至少 1）/ Target width, at least 1
 * @param[in]     h 目标高度（至少 1）/ Target height, at least 1
 * @param[out]    out 接收缩放结果 / Receives the resampled image
 *
 * @return 成功返回 1，失败返回 0 / 1 on success, 0 on failure
 *
 * @note 尺寸相同也走一次拷贝，保证 `out` 始终独立可释放 / Even an identical size copies, so `out` is always independently releasable
 *
 * @see hpe_image_free
 */
int hpe_image_scale(const HpeImage *src, int w, int h, HpeImage *out);

/* ------------- decoded-image cache ------------- */

typedef struct HpeImageCache HpeImageCache;

/**
 * @brief 新建一个解码图像缓存 / Create a decoded-image cache
 *
 * @return 新缓存；分配失败返回 `NULL` / The new cache, or `NULL` when allocation fails
 *
 * @see hpe_image_cache_free hpe_image_cache_get
 */
HpeImageCache *hpe_image_cache_new(void);

/**
 * @brief 释放解码图像缓存 / Release a decoded-image cache
 *
 * @param[in,out] cache 目标缓存；为 `NULL` 时安全无操作 / Target cache; a NULL input is a safe no-op
 *
 * @note 逐条释放其持有的解码图像 / Releases every decoded image the cache holds
 *
 * @see hpe_image_cache_new
 */
void hpe_image_cache_free(HpeImageCache *cache);

/**
 * @brief 按路径取解码图像，缺失时解码并缓存 / Fetch a decoded image by path, decoding and caching on a miss
 *
 * @details 命中缓存直接返回同一指针；未命中时读文件、解码、按需缩放并插入链表头部。
 *          A cache hit returns the very same pointer; a miss reads the file, decodes it,
 *          resamples when a target size was given, and inserts the entry at the head.
 *
 * @param[in,out] cache 目标缓存 / Target cache
 * @param[in]     path 资源路径 / Resource path
 * @param[in]     hint 格式提示（可为 `HPE_IMG_UNKNOWN`）/ Format hint, may be `HPE_IMG_UNKNOWN`
 * @param[in]     want_w 目标像素宽；小于 0 表示保持固有尺寸 / Target pixel width, negative to keep the intrinsic size
 * @param[in]     want_h 目标像素高；小于 0 表示保持固有尺寸 / Target pixel height, negative to keep the intrinsic size
 *
 * @return 解码图像（缓存持有，调用方不得释放）；失败返回 `NULL` / The decoded image owned by the cache and not to be freed by the caller, or `NULL` on failure
 *
 * @note 目标尺寸参与缓存键，同一路径的不同尺寸各存一份 / The target size takes part in the cache key, so different sizes of one path are cached separately
 *
 * @see hpe_image_cache_new hpe_image_decode
 */
const HpeImage *hpe_image_cache_get(HpeImageCache *cache, const char *path, HpeImageFormat hint,
                                    int want_w, int want_h);

/**
 * @brief 报告缓存中已解码图像的条目数 / Report how many decoded images the cache holds
 *
 * @param[in]     cache 目标缓存 / Target cache
 *
 * @return 条目数；`cache` 为 `NULL` 时返回 0 / The entry count, or 0 for a NULL cache
 *
 * @see hpe_image_cache_get
 */
int hpe_image_cache_count(const HpeImageCache *cache);

/* ------------- document resource resolution ------------- */

/* Layout and paint need to turn a relative `src` / `url()` into a loadable path
 * without reaching into the engine, exactly like the font subsystem keeps its
 * own process-global cache. The engine installs the document directory once per
 * layout pass; every later lookup resolves against it. */

/**
 * @brief 设置解析相对图像的基准目录 / Set the base directory used to resolve relative image references
 *
 * @param[in]     dir 文档（或 `<base href>`）所在目录；`NULL` 视为当前目录 / The document (or `<base href>`) directory; `NULL` means the current directory
 *
 * @note 只记录路径字符串，不做任何解析 / Only the path string is recorded; nothing is resolved here
 *
 * @see hpe_image_resolve_url
 */
void hpe_image_set_base_dir(const char *dir);

/**
 * @brief 取当前的相对引用基准目录 / Fetch the current base directory
 *
 * @return 基准目录，从未设置时返回 `""` / The base directory, or `""` when never set
 */
const char *hpe_image_base_dir(void);

/**
 * @brief 把 `src` / `url()` 引用解析为可打开的绝对路径 / Resolve a `src` / `url()` reference to an openable path
 *
 * @details 绝对路径原样返回；`data:` 与非文件方案（`http:`、`https:`、`about:`）返回空串，
 *          因为本引擎只加载本地资源。其余相对引用按 `hpe_image_set_base_dir` 装的基准目录拼接。
 *          An absolute path is passed through, `data:` and non-file schemes (`http:`, `https:`,
 *          `about:`) yield an empty string because the engine only loads local resources, and
 *          anything else is joined onto the base directory installed by `hpe_image_set_base_dir`.
 *
 * @param[in]     src 原始引用文本，可含首尾空白与引号 / Raw reference text, surrounding whitespace and quotes tolerated
 * @param[out]    out 接收绝对路径的缓冲区 / Buffer that receives the absolute path
 * @param[in]     out_size 输出缓冲区容量（字节）/ Capacity of the output buffer in bytes
 *
 * @return 得到可加载路径返回 1，否则 0 / 1 when a loadable path was produced, otherwise 0
 *
 * @see hpe_image_set_base_dir
 */
int hpe_image_resolve_url(const char *src, char *out, size_t out_size);

/**
 * @brief 取进程级共享的解码图像缓存 / Fetch the process-global decoded-image cache
 *
 * @details 布局与绘制共用同一份缓存，因此一张图在一帧内只解码一次；缓存随引擎存续，
 *          由 `hpe_image_shutdown` 统一释放。
 *          Layout and paint share the cache, so an image is decoded at most once per frame;
 *          it lives as long as the engine and is released by `hpe_image_shutdown`.
 *
 * @return 共享缓存（惰性创建）；分配失败返回 `NULL` / The shared cache, created lazily, or `NULL` when allocation fails
 *
 * @see hpe_image_shutdown hpe_image_cache_get
 */
HpeImageCache *hpe_image_shared_cache(void);

/**
 * @brief 释放共享缓存与基准目录 / Release the shared cache and the base directory
 *
 * @note 引擎销毁时调用；之后再取共享缓存会重新惰性创建 / Called when the engine is destroyed; a later fetch recreates the cache lazily
 *
 * @see hpe_image_shared_cache
 */
void hpe_image_shutdown(void);

#endif /* HPE_IMAGE_H */
