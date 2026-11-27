/* Hope UI Engine - unit tests for src/html (entity decoding via the parser, encoding detection) */
#include "hpe_test.h"

#include <string.h>

#include "base/hpe_util.h"
#include "dom/hpe_dom.h"
#include "html/hpe_encoding.h"
#include "html/hpe_html.h"

/**
 * @brief 解析一段 HTML 并取得 body 下首个 p 元素的首个文本节点 / Parse an HTML snippet and fetch the first text node under the first p in body
 *
 * @param[in]  html 待解析的源文本 / HTML source to parse
 * @param[out] out  接收文本内容（调用方用 hpe_free 释放）/ Receives the text content, freed by the caller with hpe_free
 *
 * @note `*out` 在未找到文本节点时被置为 NULL / `*out` is set to NULL when no text node is found
 */
static void parse_first_text(const char *html, char **out)
{
    HpeDomNode *doc = hpe_html_parse(html, strlen(html));
    HpeDomNode *p = hpe_dom_find_tag(doc, "p");

    *out = NULL;
    if (p && p->first_child && p->first_child->type == HPE_NODE_TEXT)
        *out = hpe_strdup(p->first_child->text);
    hpe_dom_free(doc);
}

/* ---------------- entity decoding ---------------- */

/**
 * @brief 测试具名实体的解码 / Test decoding of named entities
 */
HPE_TEST_CASE(html, entity_named)
{
    char *text;

    parse_first_text("<p>a&amp;b &lt;x&gt;</p>", &text);
    HPE_TEST_ASSERT_STR(text, "a&b <x>");
    hpe_free(text);
    parse_first_text("<p>&nbsp;-&mdash;</p>", &text);
    /* this engine maps nbsp to a plain space */
    HPE_TEST_ASSERT_STR(text, " -\xE2\x80\x94");
    hpe_free(text);
}

/**
 * @brief 测试十进制与十六进制数字实体 / Test decimal and hexadecimal numeric entities
 */
HPE_TEST_CASE(html, entity_numeric)
{
    char *text;

    parse_first_text("<p>&#65;&#x42;&#X43;</p>", &text);
    HPE_TEST_ASSERT_STR(text, "ABC");
    hpe_free(text);
    parse_first_text("<p>&#x4E2D;&#20013;</p>", &text);
    HPE_TEST_ASSERT_STR(text, "\xE4\xB8\xAD\xE4\xB8\xAD");
    hpe_free(text);
}

/**
 * @brief 测试未知与残缺实体原样保留 / Test unknown and truncated entities kept verbatim
 */
HPE_TEST_CASE(html, entity_unknown)
{
    char *text;

    parse_first_text("<p>&foo; &amp</p>", &text);
    HPE_TEST_ASSERT_STR(text, "&foo; &amp");
    hpe_free(text);
    parse_first_text("<p>&#;</p>", &text);
    HPE_TEST_ASSERT_STR(text, "&#;");
    hpe_free(text);
}

/**
 * @brief 测试实体解码发生在属性值中 / Test entity decoding inside attribute values
 */
HPE_TEST_CASE(html, entity_in_attr)
{
    HpeDomNode *doc = hpe_html_parse("<p title=\"a&amp;b\">x</p>", 24);
    HpeDomNode *p = hpe_dom_find_tag(doc, "p");

    HPE_TEST_ASSERT(p != NULL);
    HPE_TEST_ASSERT_STR(hpe_dom_attr(p, "title"), "a&b");
    hpe_dom_free(doc);
}

/**
 * @brief 测试解析器补全 html/body 结构 / Test that the parser synthesises the html/body structure
 */
HPE_TEST_CASE(html, tree_structure)
{
    HpeDomNode *doc = hpe_html_parse("<p>hi</p>", 9);
    HpeDomNode *html = doc ? doc->first_child : NULL;

    HPE_TEST_ASSERT(doc != NULL);
    HPE_TEST_ASSERT(doc->type == HPE_NODE_DOCUMENT);
    HPE_TEST_ASSERT(html != NULL && html->tag != NULL);
    HPE_TEST_ASSERT_STR(html->tag, "html");
    HPE_TEST_ASSERT(hpe_dom_find_tag(doc, "body") != NULL);
    HPE_TEST_ASSERT(hpe_dom_find_tag(doc, "p") != NULL);
    hpe_dom_free(doc);
}

/* ---------------- encoding detection ---------------- */

/**
 * @brief 测试编码名称映射 / Test the encoding name mapping
 */
HPE_TEST_CASE(encoding, names)
{
    HPE_TEST_ASSERT_STR(hpe_encoding_name(HPE_ENC_UTF8), "utf-8");
    HPE_TEST_ASSERT_STR(hpe_encoding_name(HPE_ENC_LATIN1), "latin1");
    HPE_TEST_ASSERT_STR(hpe_encoding_name(HPE_ENC_UTF16LE), "utf-16le");
    HPE_TEST_ASSERT_STR(hpe_encoding_name(HPE_ENC_UTF16BE), "utf-16be");
}

/**
 * @brief 测试 UTF-8 BOM 被剥除 / Test that a UTF-8 BOM is stripped
 */
HPE_TEST_CASE(encoding, utf8_bom)
{
    char raw[] = { (char)0xEF, (char)0xBB, (char)0xBF, 'h', 'i' };
    HpeDocEncoding enc = HPE_ENC_UTF8;
    size_t out_len = 0;
    char *out = hpe_html_decode(raw, 5, &out_len, &enc, NULL);

    HPE_TEST_ASSERT_STR(out, "hi");
    HPE_TEST_ASSERT_INT((int)out_len, 2);
    HPE_TEST_ASSERT_INT((int)enc, HPE_ENC_UTF8);
    hpe_free(out);
}

/**
 * @brief 测试 meta charset 声明驱动 latin-1 解码 / Test latin-1 decoding driven by a meta charset declaration
 */
HPE_TEST_CASE(encoding, meta_latin1)
{
    const char *raw = "<html><meta charset=\"iso-8859-1\"><p>caf\xE9</p>";
    HpeDocEncoding enc = HPE_ENC_UTF8;
    int from_meta = 0;
    char *out = hpe_html_decode(raw, strlen(raw), NULL, &enc, &from_meta);

    /* the latin-1 é becomes a two-byte UTF-8 sequence */
    HPE_TEST_ASSERT_INT((int)enc, HPE_ENC_LATIN1);
    HPE_TEST_ASSERT_INT(from_meta, 1);
    HPE_TEST_ASSERT(strstr(out, "caf\xC3\xA9") != NULL);
    hpe_free(out);
}

/**
 * @brief 测试无声明时按内容嗅探 latin-1 / Test content sniffing of latin-1 without a declaration
 */
HPE_TEST_CASE(encoding, sniff_latin1)
{
    char raw[] = { 'c', 'a', 'f', (char)0xE9 };
    HpeDocEncoding enc = HPE_ENC_UTF8;
    char *out = hpe_html_decode(raw, 4, NULL, &enc, NULL);

    HPE_TEST_ASSERT_INT((int)enc, HPE_ENC_LATIN1);
    HPE_TEST_ASSERT_STR(out, "caf\xC3\xA9");
    hpe_free(out);
}

/**
 * @brief 测试 UTF-16LE BOM 转码 / Test the UTF-16LE BOM transcoding
 */
HPE_TEST_CASE(encoding, utf16le_bom)
{
    char raw[] = { (char)0xFF, (char)0xFE, 'a', 0, 'b', 0 };
    HpeDocEncoding enc = HPE_ENC_UTF8;
    size_t out_len = 0;
    char *out = hpe_html_decode(raw, 6, &out_len, &enc, NULL);

    HPE_TEST_ASSERT_INT((int)enc, HPE_ENC_UTF16LE);
    HPE_TEST_ASSERT_INT((int)out_len, 2);
    HPE_TEST_ASSERT_STR(out, "ab");
    hpe_free(out);
}

/**
 * @brief 测试空输入与 NULL 输入的兜底 / Test the fallback for empty and NULL inputs
 */
HPE_TEST_CASE(encoding, null_and_empty)
{
    HpeDocEncoding enc = HPE_ENC_UTF8;
    char *out = hpe_html_decode(NULL, 0, NULL, &enc, NULL);

    HPE_TEST_ASSERT_STR(out, "");
    HPE_TEST_ASSERT_INT((int)enc, HPE_ENC_UTF8);
    hpe_free(out);
    out = hpe_html_decode("", 0, NULL, NULL, NULL);
    HPE_TEST_ASSERT_STR(out, "");
    hpe_free(out);
}
