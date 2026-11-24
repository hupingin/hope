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

/* Decodes a raw document byte buffer into utf-8. The order of the sniff is the
 * one the HTML spec prescribes: a BOM wins, otherwise `<meta charset>` (or
 * `http-equiv="content-type"`) is read from the first 1024 bytes, otherwise a
 * strict utf-8 validation decides between utf-8 and latin-1. Always returns a
 * freshly allocated NUL-terminated buffer (hpe_free it); never NULL. The
 * detected encoding is written to *out_enc when non-NULL, and *out_from_meta is
 * set to 1 when the choice came from an in-document declaration. */
char *hpe_html_decode(const char *raw, size_t len, size_t *out_len, HpeDocEncoding *out_enc,
                      int *out_from_meta);

/* lowercase label used by the dev/test hooks ("utf-8" / "latin1" / ...) */
const char *hpe_encoding_name(HpeDocEncoding enc);

#endif /* HPE_ENCODING_H */
