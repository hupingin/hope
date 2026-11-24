#include "html/hpe_encoding.h"

#include <string.h>

#include "base/hpe_util.h"

#define HPE_SNIFF_BYTES 1024

/* maps a charset label (as it appears in <meta charset> / Content-Type) to the
 * decoder we support; unknown labels fall back to utf-8 (the spec default). */
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

/* Scans the first HPE_SNIFF_BYTES for `charset` and returns its value. This is
 * deliberately lenient: it accepts both `<meta charset=...>` and the longer
 * `content="text/html; charset=..."` form by searching for the keyword alone. */
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

/* strict utf-8 validation: rejects over-long forms, surrogates and out-of-range
 * code points, so a latin-1 byte stream is correctly told apart from utf-8. */
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

/* Detects a byte order mark at the start of `raw`; writes the implied encoding
 * to *enc and returns how many bytes the BOM occupies (0 when there is none).
 * utf-8 has no encoding of its own but its BOM still has to be skipped. */
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

/* an empty return string, used for missing / empty input */
static char *empty_string(void)
{
    char *out = (char *)hpe_alloc(1);
    out[0] = '\0';
    return out;
}

/* byte order mark: a UTF-16 BOM selects the byte order, a UTF-8 BOM is stripped */
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

/* 2. an explicit <meta charset> in the prologue selects the codec */
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

/* 3. no declaration: utf-8 when it validates, otherwise assume latin-1 */
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

const char *hpe_encoding_name(HpeDocEncoding enc)
{
    switch (enc) {
    case HPE_ENC_LATIN1: return "latin1";
    case HPE_ENC_UTF16LE: return "utf-16le";
    case HPE_ENC_UTF16BE: return "utf-16be";
    default: return "utf-8";
    }
}
