/* Hope UI Engine - html parser (tokenizer + simplified tree builder) */
#ifndef HPE_HTML_H
#define HPE_HTML_H

#include "base/hpe_types.h"
#include "dom/hpe_dom.h"

/* Parses a local html document. Always returns a document node containing
 * <html><head></head><body>...</body></html>. */
HpeDomNode *hpe_html_parse(const char *source, size_t length);

#endif /* HPE_HTML_H */
