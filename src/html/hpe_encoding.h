/* Hope UI Engine - document character-encoding detection */
#ifndef HPE_ENCODING_H
#define HPE_ENCODING_H

#include "base/hpe_types.h"

typedef enum HpeDocEncoding {
    HPE_ENC_UTF8 = 0,
    HPE_ENC_LATIN1,  /* iso-8859-1 / windows-1252 byte stream */
    HPE_ENC_UTF16LE,
    HPE_ENC_UTF16BE
} HpeDocEncoding;

/**
 * @brief 把原始文档字节解码为 UTF-8 / Decode a raw document byte buffer into UTF-8
 *
 * @details 嗅探顺序即 HTML 规范规定的顺序：BOM 优先，其次是前 1024 字节内的 `<meta charset>`（或 `http-equiv="content-type"`），最后用严格的 utf-8 校验在 utf-8 与 latin-1 之间抉择。
 *          The sniff order follows the HTML spec: a BOM wins, then `<meta charset>` (or `http-equiv="content-type"`) within the first 1024 bytes, and finally a strict utf-8 validation decides between utf-8 and latin-1.
 *
 * @param[in]     raw 原始文档字节，为 `NULL` 时返回空串 / Raw document bytes; a `NULL` value yields an empty string
 * @param[in]     len 原始字节数 / Raw byte count
 * @param[out]    out_len 结果的 UTF-8 字节数，可为 `NULL` / Result byte count, may be `NULL`
 * @param[out]    out_enc 探测到的编码，可为 `NULL` / Detected encoding, may be `NULL`
 * @param[out]    out_from_meta 置 1 表示编码来自文档内声明，可为 `NULL` / Set to 1 when the encoding came from an in-document declaration, may be `NULL`
 *
 * @return 新分配且以 NUL 结尾的 UTF-8 字符串，永不返回 `NULL` / Newly allocated NUL-terminated UTF-8 string, never `NULL`
 *
 * @note 调用方负责用 `hpe_free` 释放返回值；UTF-8 BOM 会被剥掉 / The caller frees the result with `hpe_free`; a UTF-8 BOM is stripped
 */
char *hpe_html_decode(const char *raw, size_t len, size_t *out_len, HpeDocEncoding *out_enc,
                      int *out_from_meta);

/**
 * @brief 取得编码的小写名称 / Obtain the lowercase name of an encoding
 *
 * @param[in]     enc 待命名的编码 / Encoding to name
 *
 * @return `utf-8` / `latin1` / `utf-16le` / `utf-16be` 之一 / One of `utf-8`, `latin1`, `utf-16le` or `utf-16be`
 *
 * @note 供开发/测试钩子使用的标签 / A label used by the dev and test hooks
 */
const char *hpe_encoding_name(HpeDocEncoding enc);

#endif /* HPE_ENCODING_H */
