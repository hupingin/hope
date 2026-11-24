#include "html/hpe_encoding.h"

#include <string.h>

#include "base/hpe_util.h"

#define HPE_SNIFF_BYTES 1024

/**
 * @brief 把字符集标签映射为解码器枚举 / Map a charset label to the decoder enumeration
 *
 * @details 识别 `utf-8`/`utf8`、`iso-8859-1`/`latin1`/`windows-1252`/`cp1252`/`iso8859-1`、`utf-16`/`utf-16le` 与 `utf-16be`；标签大小写不敏感。
 *          Recognises `utf-8`/`utf8`, `iso-8859-1`/`latin1`/`windows-1252`/`cp1252`/`iso8859-1`, `utf-16`/`utf-16le` and `utf-16be`; the label is matched case-insensitively.
 *
 * @param[in]     label 字符集标签，如 `utf-8`、`iso-8859-1` / Charset label such as `utf-8` or `iso-8859-1`
 * @param[in]     len 标签字节数 / Label byte count
 *
 * @return 对应的 `HpeDocEncoding` / The matching `HpeDocEncoding`
 *
 * @note 无法识别的标签回退为 utf-8（规范默认值）/ An unrecognised label falls back to utf-8, the spec default
 */
static HpeDocEncoding encoding_from_label(const char *label, size_t len)
{
    char buf[32];
    size_t i;

    if (len >= sizeof(buf))
        return HPE_ENC_UTF8;
    for (i = 0; i < len; i++)
        buf[i] = (char)hpe_ascii_lower((unsigned char)label[i]);
    buf[len] = '\0';

    if (!strcmp(buf, "utf-8") || !strcmp(buf, "utf8"))
        return HPE_ENC_UTF8;
    if (!strcmp(buf, "iso-8859-1") || !strcmp(buf, "latin1") ||
        !strcmp(buf, "windows-1252") || !strcmp(buf, "cp1252") ||
        !strcmp(buf, "iso8859-1"))
        return HPE_ENC_LATIN1;
    if (!strcmp(buf, "utf-16") || !strcmp(buf, "utf-16le"))
        return HPE_ENC_UTF16LE;
    if (!strcmp(buf, "utf-16be"))
        return HPE_ENC_UTF16BE;
    return HPE_ENC_UTF8;
}

/**
 * @brief 从前 1024 字节中嗅探 `<meta charset>` 声明 / Sniff a `<meta charset>` declaration from the first 1024 bytes
 *
 * @details 宽松匹配：既接受 `<meta charset=...>`，也接受 `content="text/html; charset=..."`，做法是直接搜索 `charset` 关键字。
 *          Matching is deliberately lenient: both `<meta charset=...>` and `content="text/html; charset=..."` are accepted by searching for the bare `charset` keyword.
 *
 * @param[in]     raw 原始文档字节 / Raw document bytes
 * @param[in]     len 原始字节数 / Raw byte count
 * @param[out]    found 置 1 表示确实找到声明，可为 `NULL` / Set to 1 when a declaration was actually found, may be `NULL`
 *
 * @return 声明指定的编码，未找到时为 utf-8 / The declared encoding, or utf-8 when none was found
 *
 * @note 只扫描前 `HPE_SNIFF_BYTES`（1024）字节 / Only the first `HPE_SNIFF_BYTES` (1024) bytes are scanned
 */
static HpeDocEncoding sniff_meta_charset(const char *raw, size_t len, int *found)
{
    size_t limit = len < HPE_SNIFF_BYTES ? len : HPE_SNIFF_BYTES;
    size_t i;

    if (found)
        *found = 0;
    for (i = 0; i + 7 <= limit; i++) {
        if (hpe_str_ieq_n(raw + i, "charset", 7)) {
            size_t p = i + 7;
            char quote = 0;
            size_t start;
            while (p < limit && (raw[p] == ' ' || raw[p] == '\t' || raw[p] == '\n' ||
                                 raw[p] == '\r' || raw[p] == '\f'))
                p++;
            if (p >= limit || raw[p] != '=')
                continue;
            p++;
            while (p < limit && (raw[p] == ' ' || raw[p] == '\t' || raw[p] == '\n' ||
                                 raw[p] == '\r' || raw[p] == '\f'))
                p++;
            if (p < limit && (raw[p] == '"' || raw[p] == '\''))
                quote = raw[p++];
            start = p;
            while (p < limit && raw[p] != quote &&
                   (quote || (raw[p] != ';' && raw[p] != ' ' && raw[p] != '\t' &&
                              raw[p] != '\n' && raw[p] != '\r' && raw[p] != '>')))
                p++;
            if (p > start) {
                if (found)
                    *found = 1;
                return encoding_from_label(raw + start, p - start);
            }
        }
    }
    return HPE_ENC_UTF8;
}

/**
 * @brief 严格校验一段字节是否为合法 UTF-8 / Strictly validate whether a byte run is well-formed UTF-8
 *
 * @details 拒绝过长编码、代理对（U+D800~U+DFFF）与超出 U+10FFFF 的码点，从而能把 latin-1 字节流正确区分开。
 *          Over-long forms, surrogates (U+D800 to U+DFFF) and code points above U+10FFFF are rejected, which is what lets a latin-1 stream be told apart from utf-8.
 *
 * @param[in]     s 待校验字节 / Bytes to validate
 * @param[in]     len 字节数 / Byte count
 *
 * @return 合法 UTF-8 返回 1 / 1 when the bytes are valid UTF-8
 *
 * @note 用于无声明时在 utf-8 与 latin-1 之间做抉择 / Used to choose between utf-8 and latin-1 when the document declares nothing
 */
static int is_valid_utf8(const char *s, size_t len)
{
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        int extra, k;
        uint32_t cp;
        if (c < 0x80) {
            i++;
            continue;
        }
        if ((c & 0xE0) == 0xC0) {
            extra = 1;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            cp = c & 0x07;
        } else {
            return 0; /* stray continuation or 5+ byte lead */
        }
        if (i + (size_t)extra >= len)
            return 0;
        for (k = 1; k <= extra; k++) {
            unsigned char cc = (unsigned char)s[i + (size_t)k];
            if ((cc & 0xC0) != 0x80)
                return 0;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (extra == 1 && cp < 0x80)
            return 0;
        if (extra == 2 && cp < 0x800)
            return 0;
        if (extra == 3 && cp < 0x10000)
            return 0;
        if (cp > 0x10FFFF)
            return 0;
        if (cp >= 0xD800 && cp <= 0xDFFF)
            return 0;
        i += (size_t)extra + 1;
    }
    return 1;
}

/**
 * @brief 把 UTF-16 字节流转码为 UTF-8 / Transcode a UTF-16 byte stream to UTF-8
 *
 * @details 正确合并 UTF-16 代理对（高代理 `D800`~`DBFF` + 低代理 `DC00`~`DFFF`）为单个码点。
 *          Surrogate pairs (high `D800` to `DBFF` plus low `DC00` to `DFFF`) are combined into a single code point.
 *
 * @param[in]     raw UTF-16 原始字节 / Raw UTF-16 bytes
 * @param[in]     len 原始字节数 / Raw byte count
 * @param[in]     big_endian 大端序为 1 / 1 for big-endian
 * @param[out]    out_len 结果的 UTF-8 字节数，可为 `NULL` / Result byte count, may be `NULL`
 *
 * @return 新分配且以 NUL 结尾的 UTF-8 字符串 / Newly allocated NUL-terminated UTF-8 string
 *
 * @note 输出缓冲按输入长度 × 3 + 1 分配；调用方负责释放 / The output is allocated at three times the input length plus one; the caller owns it
 */
static char *utf16_to_utf8(const char *raw, size_t len, int big_endian, size_t *out_len)
{
    char *out = (char *)hpe_alloc(len * 3 + 1);
    size_t i = 0, o = 0;

    while (i + 2 <= len) {
        uint32_t u;
        if (big_endian)
            u = ((uint32_t)(unsigned char)raw[i] << 8) | (unsigned char)raw[i + 1];
        else
            u = ((uint32_t)(unsigned char)raw[i + 1] << 8) | (unsigned char)raw[i];
        i += 2;
        if (u >= 0xD800 && u <= 0xDBFF && i + 2 <= len) {
            uint32_t lo;
            if (big_endian)
                lo = ((uint32_t)(unsigned char)raw[i] << 8) | (unsigned char)raw[i + 1];
            else
                lo = ((uint32_t)(unsigned char)raw[i + 1] << 8) | (unsigned char)raw[i];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        {
            char tmp[4];
            int n = hpe_utf8_encode(u, tmp);
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
 * @brief 探测字节序标记并报告其隐含编码 / Detect a byte order mark and report the encoding it implies
 *
 * @details 识别 UTF-8 BOM（`EF BB BF`）、UTF-16LE（`FF FE`）与 UTF-16BE（`FE FF`）。
 *          Recognises the UTF-8 BOM (`EF BB BF`), UTF-16LE (`FF FE`) and UTF-16BE (`FE FF`).
 *
 * @param[in]     raw 原始字节 / Raw bytes
 * @param[in]     len 字节数 / Byte count
 * @param[out]    enc 写入 BOM 隐含的编码 / Receives the encoding implied by the BOM
 *
 * @return BOM 占用的字节数，无 BOM 时 0 / Number of bytes the BOM occupies, 0 when there is none
 *
 * @note utf-8 没有独立的编码差别，但其 BOM 仍需被跳过 / utf-8 has no encoding of its own, yet its BOM still has to be skipped
 */
static size_t detect_bom(const char *raw, size_t len, HpeDocEncoding *enc)
{
    if (len >= 3 && (unsigned char)raw[0] == 0xEF && (unsigned char)raw[1] == 0xBB &&
        (unsigned char)raw[2] == 0xBF) {
        *enc = HPE_ENC_UTF8;
        return 3;
    }
    if (len >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE) {
        *enc = HPE_ENC_UTF16LE;
        return 2;
    }
    if (len >= 2 && (unsigned char)raw[0] == 0xFE && (unsigned char)raw[1] == 0xFF) {
        *enc = HPE_ENC_UTF16BE;
        return 2;
    }
    return 0;
}

/**
 * @brief 返回一个空字符串 / Return an empty string
 *
 * @return 新分配的单字节空串 / A newly allocated single-byte empty string
 *
 * @note 用于 `NULL` 或空输入的场合，使返回值永不为 `NULL` / Used for `NULL` or empty input so that the caller never receives `NULL`
 */
static char *empty_string(void)
{
    char *out = (char *)hpe_alloc(1);
    out[0] = '\0';
    return out;
}

/**
 * @brief 按 BOM 解码文档字节 / Decode document bytes according to the BOM
 *
 * @param[in]     raw 原始字节 / Raw bytes
 * @param[in]     len 字节数 / Byte count
 * @param[in,out] enc 读入 BOM 隐含的编码 / Carries the encoding implied by the BOM
 * @param[out]    out_len 结果的 UTF-8 字节数，可为 `NULL` / Result byte count, may be `NULL`
 * @param[out]    out_enc 最终使用的编码，可为 `NULL` / Encoding finally used, may be `NULL`
 *
 * @return UTF-16 输入返回转码结果；无 BOM 或 UTF-8 BOM 返回 `NULL` / The transcoded result for UTF-16 input, or `NULL` for no BOM or a UTF-8 BOM
 *
 * @note UTF-8 BOM 返回 `NULL` 表示由调用方剥掉 BOM 后继续后续嗅探 / A `NULL` return for a UTF-8 BOM tells the caller to strip it and continue sniffing
 *
 * @see detect_bom hpe_html_decode
 */
static char *decode_by_bom(const char *raw, size_t len, HpeDocEncoding *enc, size_t *out_len,
                           HpeDocEncoding *out_enc)
{
    size_t bom = detect_bom(raw, len, enc);
    if (bom == 0)
        return NULL;
    if (*enc == HPE_ENC_UTF16LE || *enc == HPE_ENC_UTF16BE) {
        if (out_enc)
            *out_enc = *enc;
        return utf16_to_utf8(raw + bom, len - bom, *enc == HPE_ENC_UTF16BE, out_len);
    }
    return NULL; /* UTF-8 BOM: caller strips it and continues */
}

/**
 * @brief 按文档内 `<meta charset>` 声明解码 / Decode using the in-document `<meta charset>` declaration
 *
 * @param[in]     raw 原始字节 / Raw bytes
 * @param[in]     len 字节数 / Byte count
 * @param[out]    out_len 结果的 UTF-8 字节数，可为 `NULL` / Result byte count, may be `NULL`
 * @param[out]    out_enc 最终使用的编码，可为 `NULL` / Encoding finally used, may be `NULL`
 * @param[out]    out_from_meta 置 1 表示编码来自文档内声明 / Set to 1 when the encoding came from the in-document declaration
 * @param[out]    handled 置 1 表示本函数已给出结果 / Set to 1 when this function produced a result
 *
 * @return 声明为 UTF-16 时返回转码结果，否则 `NULL` / The transcoded result for a UTF-16 declaration, otherwise `NULL`
 *
 * @note 声明为 UTF-8 或 latin-1 时 `handled` 为 0，交由后续嗅探处理 / For a UTF-8 or latin-1 declaration `handled` stays 0 and later sniffing takes over
 *
 * @see sniff_meta_charset
 */
static char *decode_by_meta(const char *raw, size_t len, size_t *out_len, HpeDocEncoding *out_enc,
                            int *out_from_meta, int *handled)
{
    HpeDocEncoding enc;
    int from_meta = 0;
    enc = sniff_meta_charset(raw, len, &from_meta);
    if (from_meta && out_from_meta)
        *out_from_meta = 1;
    if (enc == HPE_ENC_UTF16LE) {
        *handled = 1;
        return utf16_to_utf8(raw, len, 0, out_len);
    }
    if (enc == HPE_ENC_UTF16BE) {
        *handled = 1;
        return utf16_to_utf8(raw, len, 1, out_len);
    }
    *handled = 0;
    return NULL;
}

/**
 * @brief 无声明时按内容嗅探解码 / Decode by content sniffing when the document declares nothing
 *
 * @details 通过 `is_valid_utf8` 判断：合法按 utf-8 原样复制，否则按 latin-1/cp1252 转码。
 *          Uses `is_valid_utf8`: valid bytes are copied as-is, otherwise the stream is transcoded from latin-1/cp1252.
 *
 * @param[in]     raw 原始字节 / Raw bytes
 * @param[in]     len 字节数 / Byte count
 * @param[out]    out_len 结果的 UTF-8 字节数，可为 `NULL` / Result byte count, may be `NULL`
 * @param[out]    out_enc 最终使用的编码，可为 `NULL` / Encoding finally used, may be `NULL`
 *
 * @return 新分配且以 NUL 结尾的 UTF-8 字符串 / Newly allocated NUL-terminated UTF-8 string
 *
 * @see is_valid_utf8 hpe_latin1_to_utf8
 */
static char *decode_by_sniff(const char *raw, size_t len, size_t *out_len, HpeDocEncoding *out_enc)
{
    HpeDocEncoding enc = HPE_ENC_UTF8;
    char *out;
    if (!is_valid_utf8(raw, len))
        enc = HPE_ENC_LATIN1;
    if (out_enc)
        *out_enc = enc;
    if (enc == HPE_ENC_LATIN1)
        return hpe_latin1_to_utf8(raw, len, out_len);
    out = (char *)hpe_alloc(len + 1);
    memcpy(out, raw, len);
    out[len] = '\0';
    if (out_len)
        *out_len = len;
    return out;
}

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
 *
 * @see decode_by_bom decode_by_meta decode_by_sniff
 */
char *hpe_html_decode(const char *raw, size_t len, size_t *out_len, HpeDocEncoding *out_enc,
                      int *out_from_meta)
{
    HpeDocEncoding enc = HPE_ENC_UTF8;
    size_t bom;
    int handled = 0;

    if (out_len)
        *out_len = 0;
    if (out_from_meta)
        *out_from_meta = 0;
    if (!raw) {
        if (out_enc)
            *out_enc = HPE_ENC_UTF8;
        return empty_string();
    }

    /* 1. a byte order mark wins over everything else */
    bom = detect_bom(raw, len, &enc);
    if (bom > 0) {
        char *decoded = decode_by_bom(raw, len, &enc, out_len, out_enc);
        if (decoded)
            return decoded;
        raw += bom; /* UTF-8 BOM: strip it and fall through to sniffing */
        len -= bom;
    }

    {
        char *decoded = decode_by_meta(raw, len, out_len, out_enc, out_from_meta, &handled);
        if (handled)
            return decoded;
    }
    return decode_by_sniff(raw, len, out_len, out_enc);
}

/**
 * @brief 取得编码的小写名称 / Obtain the lowercase name of an encoding
 *
 * @param[in]     enc 待命名的编码 / Encoding to name
 *
 * @return `utf-8` / `latin1` / `utf-16le` / `utf-16be` 之一 / One of `utf-8`, `latin1`, `utf-16le` or `utf-16be`
 *
 * @note 供开发/测试钩子使用的标签 / A label used by the dev and test hooks
 */
const char *hpe_encoding_name(HpeDocEncoding enc)
{
    switch (enc) {
    case HPE_ENC_LATIN1: return "latin1";
    case HPE_ENC_UTF16LE: return "utf-16le";
    case HPE_ENC_UTF16BE: return "utf-16be";
    default: return "utf-8";
    }
}
