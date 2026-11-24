/* Hope UI Engine - html parser (tokenizer + simplified tree builder) */
#ifndef HPE_HTML_H
#define HPE_HTML_H

#include "base/hpe_types.h"
#include "dom/hpe_dom.h"

/**
 * @brief 解析本地 HTML 文档为 DOM 树 / Parse a local HTML document into a DOM tree
 *
 * @details 容错解析：隐式闭合、表格节点补全、foster parenting、原始文本与实体解码都在此过程中完成。
 *          Fault-tolerant parsing: implicit closing, table-node synthesis, foster parenting, raw-text handling and entity decoding all happen here.
 *
 * @param[in]     source HTML 源文本 / HTML source text
 * @param[in]     length 源文本字节数 / Source byte count
 *
 * @return 包含 `<html><head></head><body>...</body></html>` 的文档节点 / A document node containing `<html><head></head><body>...</body></html>`
 *
 * @note 返回的文档树由调用方用 `hpe_dom_free` 释放 / The caller frees the returned document tree with `hpe_dom_free`
 *
 * @see hpe_dom_free
 */
HpeDomNode *hpe_html_parse(const char *source, size_t length);

#endif /* HPE_HTML_H */
