#include "html/hpe_html.h"

#include <stdio.h>
#include <string.h>

#include "base/hpe_util.h"
#include "dom/hpe_dom.h"

/* ---------------- tag tables ---------------- */

static const char *k_void_tags[] = {
    "area", "base", "br", "col", "embed", "hr", "img", "input",
    "link", "meta", "param", "source", "track", "wbr", NULL};

/* A start tag from this list closes an open <p> (HTML "close a p element"
 * trigger set, block-level subset relevant to this engine). */
static const char *k_closes_p[] = {
    "address", "article", "aside", "blockquote", "details", "dir", "div", "dl",
    "fieldset", "figcaption", "figure", "footer", "form", "h1", "h2", "h3", "h4",
    "h5", "h6", "header", "hgroup", "hr", "li", "main", "menu", "nav", "ol", "p",
    "pre", "section", "summary", "table", "ul", NULL};

/* table-structure auto-closing sets */
static const char *k_table_sections[] = {"tbody", "thead", "tfoot", NULL};
static const char *k_closes_cell[] = {"td", "th", "tr", "tbody", "tfoot",
                                      "thead", "caption", "colgroup", "col", NULL};
static const char *k_closes_row[] = {"tr", "tbody", "tfoot", "thead", "caption",
                                     "colgroup", NULL};
static const char *k_closes_section[] = {"tbody", "tfoot", "thead", "caption",
                                         "colgroup", "col", NULL};
static const char *k_headings[] = {"h1", "h2", "h3", "h4", "h5", "h6", NULL};
static const char *k_dt_dd[] = {"dt", "dd", NULL};
static const char *k_option[] = {"option", "optgroup", NULL};
static const char *k_rt_rp[] = {"rt", "rp", NULL};

/* elements that may legally sit directly in each table context */
static const char *k_in_table[] = {"caption", "colgroup", "col", "tbody", "tfoot",
                                   "thead", "tr", "script", "style", "template", NULL};
static const char *k_in_section[] = {"tr", "script", "style", "template", NULL};
static const char *k_in_row[] = {"td", "th", "script", "style", "template", NULL};
/* the only content a <select> keeps */
static const char *k_in_select[] = {"option", "optgroup", "hr", "script",
                                    "style", "template", NULL};

/**
 * @brief 判断字符串是否命中以 NULL 结尾的字符串表 / Test whether a string appears in a NULL-terminated string table
 *
 * @param[in]     list 以 `NULL` 结尾的字符串数组 / A `NULL`-terminated array of strings
 * @param[in]     tag 待查找的字符串 / String to look up
 *
 * @return 命中返回 1，否则 0 / 1 when found, otherwise 0
 *
 * @note 用于标签集合的成员判定（如 `k_void_tags`、`k_closes_p`）/ Used for tag-set membership tests such as `k_void_tags` and `k_closes_p`
 */
static int in_list(const char **list, const char *tag)
{
    int i;
    for (i = 0; list[i]; i++) {
        if (strcmp(list[i], tag) == 0)
            return 1;
    }
    return 0;
}

/**
 * @brief 判断标签是否为 `h1`~`h6` / Test whether a tag is `h1` through `h6`
 *
 * @param[in]     tag 标签名 / Tag name
 *
 * @return 是标题标签返回 1 / 1 when the tag is a heading
 *
 * @note 标题之间互相隐式闭合，此判定用于该规则 / Headings implicitly close one another, and this predicate backs that rule
 */
static int is_heading(const char *tag)
{
    return tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6' && tag[2] == '\0';
}

/**
 * @brief 在具名实体表中查找字符实体的 UTF-8 值 / Look up the UTF-8 value of a named character entity
 *
 * @param[in]     name 实体名，不含前导 `&` 与结尾 `;` / Entity name without the leading `&` or trailing `;`
 *
 * @return 对应的 UTF-8 字节串（静态存储）或 `NULL` / The UTF-8 byte string in static storage, or `NULL`
 *
 * @note 表内覆盖 `amp`、`lt`、`nbsp`、`copy`、`mdash` 等常用实体 / The table covers common entities such as `amp`, `lt`, `nbsp`, `copy` and `mdash`
 */

static const char *entity_named(const char *name)
{
    static const struct {
        const char *name;
        const char *value; /* utf-8 bytes */
    } table[] = {
        {"amp", "&"},        {"lt", "<"},        {"gt", ">"},
        {"quot", "\""},      {"apos", "'"},      {"nbsp", " "},
        {"copy", "\xC2\xA9"}, {"middot", "\xC2\xB7"}, {"mdash", "\xE2\x80\x94"},
        {"ndash", "\xE2\x80\x93"}, {"hellip", "\xE2\x80\xA6"}, {"times", "\xC3\x97"},
        {"rarr", "\xE2\x86\x92"}, {"larr", "\xE2\x86\x90"}, {"uarr", "\xE2\x86\x91"},
        {"darr", "\xE2\x86\x93"}, {"bull", "\xE2\x80\xA2"}, {"deg", "\xC2\xB0"},
        {"trade", "\xE2\x84\xA2"}, {"reg", "\xC2\xAE"}, {"ldquo", "\xE2\x80\x9C"},
        {"rdquo", "\xE2\x80\x9D"}, {"lsquo", "\xE2\x80\x98"}, {"rsquo", "\xE2\x80\x99"},
    };
    size_t i;
    for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (!strcmp(name, table[i].name))
            return table[i].value;
    }
    return NULL;
}

/**
 * @brief 解码单个 `&...;` 实体 / Decode a single `&...;` entity token
 *
 * @details 支持十进制 `&#123;` 与十六进制 `&#x1F600;` 数字实体，以及 `entity_named` 表中的具名实体。
 *          Numeric forms `&#123;` and `&#x1F600;` are decoded, as are the named entities in the `entity_named` table.
 *
 * @param[in]     s 指向 `&` 之后、`;` 之前的实体正文 / Entity body after the `&` and before the `;`
 * @param[in]     len 实体正文加上结尾 `;` 的字节数 / Byte count of the body including the trailing `;`
 * @param[out]    out 写入最多 4 字节 UTF-8 的缓冲 / Buffer receiving up to 4 UTF-8 bytes
 *
 * @return 写出的 UTF-8 字节数（1~4），无法识别时 0 / UTF-8 bytes written, 1 to 4, or 0 when unrecognised
 *
 * @note 正文长度必须小于 16 且以 `;` 结尾，否则返回 0 / The body must be shorter than 16 bytes and end with `;`, otherwise 0 is returned
 */
static int decode_one_entity(const char *s, size_t len, char out[4])
{
    char ent[16];
    size_t nlen = len;

    if (len == 0 || len >= sizeof(ent) || s[len - 1] != ';')
        return 0;
    nlen = len - 1; /* drop the ';' */
    memcpy(ent, s, nlen);
    ent[nlen] = '\0';

    if (ent[0] == '#') {
        uint32_t cp = 0;
        const char *p = ent + 1;
        int base = 10;
        if (*p == 'x' || *p == 'X') {
            base = 16;
            p++;
        }
        if (!*p)
            return 0;
        for (; *p; p++) {
            int d;
            if (*p >= '0' && *p <= '9')
                d = *p - '0';
            else if (base == 16 && hpe_ascii_lower((unsigned char)*p) >= 'a' &&
                     hpe_ascii_lower((unsigned char)*p) <= 'f')
                d = hpe_ascii_lower((unsigned char)*p) - 'a' + 10;
            else
                return 0;
            cp = cp * (uint32_t)base + (uint32_t)d;
        }
        return cp > 0 ? hpe_utf8_encode(cp, out) : 0;
    }
    {
        const char *rep = entity_named(ent);
        size_t rl;
        if (!rep)
            return 0;
        rl = strlen(rep);
        if (rl > 4)
            return 0;
        memcpy(out, rep, rl);
        return (int)rl;
    }
}

/**
 * @brief 把一段文本里的字符实体全部解码 / Decode every character entity in a text run
 *
 * @details 逐字节扫描，遇到 `&` 时按最长 12 字节向前找 `;`，识别则替换、否则原样保留。
 *          Scans byte by byte; on `&` it looks ahead up to 12 bytes for `;` and replaces when recognised, otherwise copies verbatim.
 *
 * @param[in]     s 以 NUL 结尾的字符串 / NUL-terminated string
 * @param[in]     len 源文本字节数 / Source byte count
 * @param[out]    out_len 解码结果的字节数，可为 `NULL` / Result byte count, may be `NULL`
 *
 * @return 新分配且以 NUL 结尾的解码文本 / Newly allocated NUL-terminated decoded text
 *
 * @note 输出缓冲按输入长度 +1 分配，实体替换不会超出该大小 / The output is allocated at the input length plus one, and entity replacement never exceeds it
 */
static char *decode_entities(const char *s, size_t len, size_t *out_len)
{
    char *out = (char *)hpe_alloc(len + 1);
    size_t i = 0, o = 0;

    while (i < len) {
        if (s[i] == '&') {
            size_t semi = i + 1;
            char rep[4];
            int rn;
            while (semi < len && semi - i < 12 && s[semi] != ';')
                semi++;
            rn = (semi < len && semi > i + 1)
                     ? decode_one_entity(s + i + 1, semi - i, rep)
                     : 0;
            if (rn > 0) {
                memcpy(out + o, rep, (size_t)rn);
                o += (size_t)rn;
                i = semi + 1;
                continue;
            }
        }
        out[o++] = s[i++];
    }
    out[o] = '\0';
    if (out_len)
        *out_len = o;
    return out;
}

/* ---------------- parser state ---------------- */

typedef struct {
    HpeDomNode **items;
    int count;
    size_t cap;
} NodeStack;

typedef struct {
    const char *src;
    size_t len;
    size_t pos;
    HpeDomNode *doc;
    HpeDomNode *html;
    HpeDomNode *head;
    HpeDomNode *body;
    NodeStack stack;
} Parser;

/**
 * @brief 把节点压入开放元素栈 / Push a node onto the open-element stack
 *
 * @param[in,out] st 开放元素栈 / The open-element stack
 * @param[in,out] node 压入的节点 / Node being pushed
 *
 * @note 按需倍增内部数组容量 / The internal array grows by doubling as needed
 */
static void stack_push(NodeStack *st, HpeDomNode *node)
{
    st->items = (HpeDomNode **)hpe_grow(st->items, &st->cap, (size_t)st->count + 1,
                                        sizeof(HpeDomNode *));
    st->items[st->count++] = node;
}

/**
 * @brief 取得当前的插入位置节点 / Obtain the current insertion point node
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return 栈顶元素，栈空时返回 `<body>` / The stack top, or the `<body>` element when the stack is empty
 *
 * @note 树构建阶段的“当前节点”概念 / The “current node” concept of the tree-construction stage
 */
static HpeDomNode *current_node(Parser *p)
{
    if (p->stack.count > 0)
        return p->stack.items[p->stack.count - 1];
    return p->body;
}

/**
 * @brief 只看不弹地取得栈顶节点 / Peek at the top node of the stack
 *
 * @param[in,out] st 开放元素栈 / The open-element stack
 *
 * @return 栈顶节点，栈空时 `NULL` / The top node, or `NULL` when the stack is empty
 */
static HpeDomNode *stack_top(NodeStack *st)
{
    return st->count > 0 ? st->items[st->count - 1] : NULL;
}

/**
 * @brief 反复弹栈直到指定节点被弹出 / Pop the stack until the given node has been popped
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in,out] node 目标节点；栈被清空仍未遇到时全部弹出 / Target node, with everything popped when it is never reached
 *
 * @note 该节点自身也会被弹出（含端点）/ The node itself is popped as well (inclusive)
 *
 * @see stack_top
 */
static void stack_pop_to(Parser *p, HpeDomNode *node)
{
    while (p->stack.count > 0) {
        HpeDomNode *top = p->stack.items[p->stack.count - 1];
        p->stack.count--;
        if (top == node)
            break;
    }
}

/**
 * @brief 判断字符串是否全为空白 / Test whether a string is entirely whitespace
 *
 * @param[in]     s 以 NUL 结尾的字符串 / NUL-terminated string
 *
 * @return 全为空白（含空串）返回 1 / 1 when every character is whitespace, including the empty string
 *
 * @note 空白集合由 `hpe_is_space` 定义 / The whitespace set is defined by `hpe_is_space`
 */
static int all_whitespace(const char *s)
{
    for (; *s; s++) {
        if (!hpe_is_space(*s))
            return 0;
    }
    return 1;
}

/**
 * @brief 判断节点是否处于表格插入上下文 / Test whether a node is a table insertion context
 *
 * @details 这些节点只接受合法的表格内容，其它文本与元素要被 foster parenting 搬到最近的 `<table>` 之前。
 *          These nodes only accept legal table content, so other text and elements are foster-parented to just before the nearest `<table>`.
 *
 * @param[in,out] n 待检查的节点，可为 `NULL` / Node to inspect, may be `NULL`
 *
 * @return 是 `table`/`tbody`/`thead`/`tfoot`/`tr` 元素返回 1 / 1 for a `table`, `tbody`, `thead`, `tfoot` or `tr` element
 */

static int in_table_context(HpeDomNode *n)
{
    if (!n || n->type != HPE_NODE_ELEMENT)
        return 0;
    return !strcmp(n->tag, "table") || !strcmp(n->tag, "tbody") ||
           !strcmp(n->tag, "thead") || !strcmp(n->tag, "tfoot") ||
           !strcmp(n->tag, "tr");
}

/**
 * @brief 判断标签在给定表格上下文中是否合法 / Test whether a tag is legal in a given table context
 *
 * @param[in,out] ctx 表格上下文节点（`table`/`tr`/`tbody` 等）/ Table context node such as `table`, `tr` or `tbody`
 * @param[in]     tag 待判定的标签名，可为 `NULL` / Tag name to test, may be `NULL`
 *
 * @return 合法返回 1 / 1 when the tag may appear there
 *
 * @note 按上下文选择 `k_in_table` / `k_in_row` / `k_in_section` 三张白名单 / The whitelist is chosen among `k_in_table`, `k_in_row` and `k_in_section` by context
 */
static int allowed_in_table(HpeDomNode *ctx, const char *tag)
{
    if (!strcmp(ctx->tag, "table"))
        return tag ? in_list(k_in_table, tag) : 0;
    if (!strcmp(ctx->tag, "tr"))
        return tag ? in_list(k_in_row, tag) : 0;
    return tag ? in_list(k_in_section, tag) : 0; /* tbody / thead / tfoot */
}

/**
 * @brief 在开放元素栈上寻找最近的 `<table>` / Find the nearest `<table>` on the open-element stack
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return 最近的 `table` 元素，无则 `NULL` / The nearest `table` element, or `NULL`
 *
 * @note foster parenting 的插入锚点由它给出 / It provides the insertion anchor for foster parenting
 */
static HpeDomNode *nearest_table(Parser *p)
{
    int i;
    for (i = p->stack.count - 1; i >= 0; i--) {
        HpeDomNode *n = p->stack.items[i];
        if (n->type == HPE_NODE_ELEMENT && !strcmp(n->tag, "table"))
            return n;
    }
    return NULL;
}

/**
 * @brief 把节点插入到当前插入点，必要时 foster parenting / Insert a node at the current point, foster-parenting when required
 *
 * @details 当当前节点是表格上下文且不接受 `tag` 时，节点被插到最近的 `<table>` 之前，与浏览器的容错行为一致。
 *          When the current node is a table context that does not accept `tag`, the node is placed just before the nearest `<table>`, matching browser fault tolerance.
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in,out] node 待插入的节点 / Node to insert
 * @param[in]     tag 标签名，可为 `NULL`（文本节点）/ Tag name, `NULL` for a text node
 *
 * @note 接受时按 `hpe_dom_children_root` 追加到当前节点 / When accepted it is appended through `hpe_dom_children_root` of the current node
 *
 * @see hpe_dom_append
 */
static void insert_node(Parser *p, HpeDomNode *node, const char *tag)
{
    HpeDomNode *ctx = current_node(p);

    if (in_table_context(ctx) && !allowed_in_table(ctx, tag)) {
        HpeDomNode *tbl = nearest_table(p);
        if (tbl && tbl->parent) {
            hpe_dom_insert_before(tbl->parent, node, tbl);
            return;
        }
    }
    hpe_dom_append(hpe_dom_children_root(ctx), node);
}

/**
 * @brief 解码并插入一段文本节点 / Decode and insert a text node
 *
 * @details 先做实体解码；`<head>` 内的空白与表格上下文中的纯空白文本被丢弃。
 *          Entities are decoded first; whitespace inside `<head>` and whitespace-only text in a table context are dropped.
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in]     begin 文本起止指针（含）/ Start of the text run, inclusive
 * @param[in]     end 文本结束指针（不含）/ End of the text run, exclusive
 *
 * @note 解码结果为空或纯空白（在上述位置）时不插入任何节点 / No node is inserted when the decoded text is empty or whitespace-only in those positions
 */
static void add_text(Parser *p, const char *begin, const char *end)
{
    size_t declen = 0;
    char *text;
    HpeDomNode *node;
    HpeDomNode *ctx;

    if (end <= begin)
        return;
    text = decode_entities(begin, (size_t)(end - begin), &declen);
    if (declen == 0) {
        hpe_free(text);
        return;
    }
    ctx = current_node(p);
    /* whitespace between head elements carries no meaning; whitespace-only text
     * inside a table context is dropped entirely */
    if ((stack_top(&p->stack) == p->head || in_table_context(ctx)) &&
        all_whitespace(text)) {
        hpe_free(text);
        return;
    }
    node = hpe_dom_text(text);
    insert_node(p, node, NULL);
    hpe_free(text);
}

/**
 * @brief 判断字符是否可出现在标签/属性名中 / Test whether a character may appear in a tag or attribute name
 *
 * @param[in]     c 输入字符 / Input character
 *
 * @return 是名称字符返回 1 / 1 when it is a name character
 *
 * @note 空白、`>`、`/`、`=` 与 `\0` 都不是名称字符 / Whitespace, `>`, `/`, `=` and `\0` are not name characters
 */

static int is_name_char(char c)
{
    return !hpe_is_space(c) && c != '>' && c != '/' && c != '=' && c != '\0';
}

/**
 * @brief 跳过解析游标处的连续空白 / Skip consecutive whitespace at the parse cursor
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @note 只前进 `pos`，不修改源文本 / Only advances `pos` and never modifies the source
 */
static void skip_spaces(Parser *p)
{
    while (p->pos < p->len && hpe_is_space(p->src[p->pos]))
        p->pos++;
}

/**
 * @brief 读取并小写化一个标签/属性名 / Read and lowercase a tag or attribute name
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[out]    out 接收小写名称的缓冲 / Buffer receiving the lowercased name
 * @param[in]     cap 输出缓冲容量（字节）/ Capacity of the output buffer in bytes
 *
 * @return 读到的名称长度 / Length of the name read
 *
 * @note 超出 `cap` 的部分只前进游标不再写入，结果始终以 NUL 结尾 / Characters beyond `cap` advance the cursor without being stored, and the result is always NUL-terminated
 */
static int read_name(Parser *p, char *out, size_t cap)
{
    int n = 0;
    while (p->pos < p->len && is_name_char(p->src[p->pos])) {
        if ((size_t)n + 1 < cap)
            out[n++] = (char)hpe_ascii_lower((unsigned char)p->src[p->pos]);
        p->pos++;
    }
    out[n] = '\0';
    return n;
}

/**
 * @brief 前进到下一个 `>` 并跳过它 / Advance past the next `>` consuming it
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @note 到达输入末尾时停在末尾，不越界 / Stops at the end of input and never reads past it
 */
static void skip_to_tag_end(Parser *p)
{
    while (p->pos < p->len && p->src[p->pos] != '>')
        p->pos++;
    if (p->pos < p->len)
        p->pos++;
}

/**
 * @brief 在文本中忽略大小写查找子串 / Search a substring case-insensitively
 *
 * @param[in]     hay 被搜索的文本 / Text being searched
 * @param[in]     hay_len 被搜索文本长度 / Length of the haystack
 * @param[in]     from 起始搜索下标 / Index to start searching from
 * @param[in]     needle 要查找的子串 / Substring to look for
 *
 * @return 首次出现的下标，未找到返回 `(size_t)-1` / Index of the first occurrence, or `(size_t)-1` when absent
 *
 * @note 用于查找 `</script` 等结束标签与注释结束符 / Used to locate end tags such as `</script` and comment terminators
 */
static size_t find_ci(const char *hay, size_t hay_len, size_t from, const char *needle)
{
    size_t nlen = strlen(needle), i, j;
    if (nlen == 0 || hay_len < nlen)
        return (size_t)-1;
    for (i = from; i + nlen <= hay_len; i++) {
        for (j = 0; j < nlen; j++) {
            if (hpe_ascii_lower((unsigned char)hay[i + j]) !=
                hpe_ascii_lower((unsigned char)needle[j]))
                break;
        }
        if (j == nlen)
            return i;
    }
    return (size_t)-1;
}

/**
 * @brief 判断标签是否属于 `<head>` 的合法内容 / Test whether a tag is legal `<head>` content
 *
 * @param[in]     tag 标签名 / Tag name
 *
 * @return 属于 head 内容返回 1 / 1 when the tag belongs in the head
 *
 * @note `meta`/`link`/`style`/`title`/`base`/`script`/`template`/`noscript` 视为 head 内容 / `meta`, `link`, `style`, `title`, `base`, `script`, `template` and `noscript` count as head content
 */
static int is_head_content(const char *tag)
{
    return !strcmp(tag, "meta") || !strcmp(tag, "link") || !strcmp(tag, "style") ||
           !strcmp(tag, "title") || !strcmp(tag, "base") || !strcmp(tag, "script") ||
           !strcmp(tag, "template") || !strcmp(tag, "noscript");
}

/**
 * @brief 判断标签是否为原始文本元素 / Test whether a tag is a raw-text element
 *
 * @param[in]     tag 标签名 / Tag name
 *
 * @return `style` 或 `script` 返回 1 / 1 for `style` or `script`
 *
 * @note 原始文本元素内部不做标签解析，直到遇到自己的结束标签 / Their body is not tag-parsed until their own end tag appears
 */
static int is_rawtext(const char *tag)
{
    return !strcmp(tag, "style") || !strcmp(tag, "script");
}

/**
 * @brief 寻找最内层仍开放的 `<select>` / Find the innermost still-open `<select>`
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return 开放中的 `select` 元素，无则 `NULL` / The open `select` element, or `NULL`
 *
 * @note 遇到 `table` 或 `template` 即停止搜索，作为 select 作用域边界 / The search stops at `table` or `template`, the select scope boundaries
 *
 * @see handle_start_tag handle_end_tag
 */
static HpeDomNode *open_select(Parser *p)
{
    int i;
    for (i = p->stack.count - 1; i >= 0; i--) {
        HpeDomNode *n = p->stack.items[i];
        if (n->type != HPE_NODE_ELEMENT)
            continue;
        if (!strcmp(n->tag, "select"))
            return n;
        if (!strcmp(n->tag, "table") || !strcmp(n->tag, "template"))
            break; /* select scope boundary */
    }
    return NULL;
}

/* ---------------- implicit closing ----------------
 * Pops the stack while the current node is implicitly closed by `tag`. This is
 * the fault-tolerance core: `<li>a<li>b` closes the first item, `<p>x<div>`
 * closes the paragraph, `<td>a<td>b` closes the first cell, and so on. The rule
 * set is table-driven so adding a case is a data change, not a code change. */

typedef struct {
    const char *open_tag;   /* the element currently open at the stack top */
    const char **triggers;  /* start tags that close it */
} CloseRule;

static const char *k_trig_self_li[] = {"li", NULL};
static const char *k_trig_optgroup[] = {"optgroup", NULL};
static const char *k_trig_self_a[] = {"a", NULL};
static const char *k_trig_self_button[] = {"button", NULL};

static const CloseRule k_close_rules[] = {
    {"p", k_closes_p},
    {"li", k_trig_self_li},
    {"dt", k_dt_dd},
    {"dd", k_dt_dd},
    {"option", k_option},
    {"optgroup", k_trig_optgroup},
    {"td", k_closes_cell},
    {"th", k_closes_cell},
    {"tr", k_closes_row},
    {"tbody", k_closes_section},
    {"thead", k_closes_section},
    {"tfoot", k_closes_section},
    {"h1", k_headings},
    {"h2", k_headings},
    {"h3", k_headings},
    {"h4", k_headings},
    {"h5", k_headings},
    {"h6", k_headings},
    {"a", k_trig_self_a},
    {"button", k_trig_self_button},
    {"rt", k_rt_rp},
    {"rp", k_rt_rp},
};

/**
 * @brief 判断起始标签是否隐式闭合当前开放元素 / Test whether a start tag implicitly closes the current open element
 *
 * @details 规则由数据表 `k_close_rules` 驱动（`<li>a<li>b`、`<p>x<div>`、`<td>a<td>b` 等）；标题之间互相闭合。
 *          Rules are driven by the `k_close_rules` table (`<li>a<li>b`, `<p>x<div>`, `<td>a<td>b`, ...); headings close one another.
 *
 * @param[in]     open_tag 栈顶当前开放的标签 / Tag currently open at the stack top
 * @param[in]     tag 即将开始的标签 / Tag that is about to start
 *
 * @return 会触发闭合返回 1 / 1 when it triggers a close
 *
 * @note 新增容错规则只需改数据表，无需改代码 / Adding a fault-tolerance rule is a data change, not a code change
 */
static int closes_open_element(const char *open_tag, const char *tag)
{
    size_t i;
    for (i = 0; i < sizeof(k_close_rules) / sizeof(k_close_rules[0]); i++) {
        if (!strcmp(k_close_rules[i].open_tag, open_tag))
            return in_list(k_close_rules[i].triggers, tag);
    }
    /* a heading opens and is closed by any heading */
    if (is_heading(open_tag))
        return is_heading(tag);
    return 0;
}

/**
 * @brief 为即将开始的标签弹出需要隐式闭合的开放元素 / Pop the elements implicitly closed by an upcoming start tag
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in]     tag 即将开始的标签 / Tag that is about to start
 *
 * @note 至多循环 64 次以防规则异常导致死循环 / Loops at most 64 times to guard against runaway rules
 *
 * @see closes_open_element
 */
static void auto_close_for_start(Parser *p, const char *tag)
{
    int guard = 0;
    while (guard++ < 64) {
        HpeDomNode *top = stack_top(&p->stack);
        if (!top || top->type != HPE_NODE_ELEMENT)
            break;
        if (!closes_open_element(top->tag, tag))
            break;
        p->stack.count--;
    }
}

/**
 * @brief 为表格内容补出缺失的 `tbody` / `tr` 元素 / Synthesise missing `tbody` and `tr` elements for table content
 *
 * @details `<table><tr>` 补出 `<tbody>`；`<table><td>` 或 `<tbody><td>` 补出 `<tr>`（必要时先补 `<tbody>`）。补出的元素是合法表格内容，不触发 foster parenting。
 *          `<table><tr>` gains a `<tbody>`; `<table><td>` or `<tbody><td>` gains a `<tr>` (and a `<tbody>` first when needed). The synthesised elements are legal table content and are not foster-parented.
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in]     tag 即将开始的标签 / Tag that is about to start
 *
 * @note 仅当栈上存在 `<table>` 时才生效 / It only takes effect when a `<table>` is open on the stack
 */
static void synthesize_table_nodes(Parser *p, const char *tag)
{
    HpeDomNode *top;
    HpeDomNode *node;

    if (!nearest_table(p))
        return;
    top = current_node(p);
    if (!strcmp(tag, "tr")) {
        if (top->type == HPE_NODE_ELEMENT && !strcmp(top->tag, "table")) {
            node = hpe_dom_element("tbody");
            hpe_dom_append(hpe_dom_children_root(top), node);
            stack_push(&p->stack, node);
        }
    } else if (!strcmp(tag, "td") || !strcmp(tag, "th")) {
        if (top->type == HPE_NODE_ELEMENT && !strcmp(top->tag, "table")) {
            node = hpe_dom_element("tbody");
            hpe_dom_append(hpe_dom_children_root(top), node);
            stack_push(&p->stack, node);
            top = node;
        }
        if (top->type == HPE_NODE_ELEMENT && in_list(k_table_sections, top->tag)) {
            node = hpe_dom_element("tr");
            hpe_dom_append(hpe_dom_children_root(top), node);
            stack_push(&p->stack, node);
        }
    }
}

/* ---------------- tree builder ---------------- */

/**
 * @brief 把 `<html>`/`<head>`/`<body>` 折算进合成骨架 / Fold `<html>`, `<head>` and `<body>` into the synthetic scaffolding
 *
 * @details 属性被合并到预建的骨架元素上，`<head>`/`<body>` 还会相应调整开放元素栈。
 *          Attributes are merged onto the pre-built scaffolding elements, and `<head>` or `<body>` also adjust the open-element stack.
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in]     tag 标签名 / Tag name
 * @param[in,out] node 已创建的对应元素节点 / The element node that was created for it
 *
 * @return 按这种方式处理掉返回 1，否则 0 / 1 when the tag was consumed this way, otherwise 0
 *
 * @note 非这三个标签一律返回 0，交由常规路径处理 / Any other tag returns 0 and goes down the normal path
 */
static int handle_implied_container(Parser *p, const char *tag, HpeDomNode *node)
{
    HpeDomNode *target = NULL;
    int i;

    if (!strcmp(tag, "html"))
        target = p->html;
    else if (!strcmp(tag, "head"))
        target = p->head;
    else if (!strcmp(tag, "body"))
        target = p->body;
    else
        return 0;

    for (i = 0; i < node->attr_count; i++)
        hpe_dom_set_attr(target, node->attrs[i].name, node->attrs[i].value);
    if (target == p->head) {
        stack_push(&p->stack, p->head);
    } else if (target == p->body) {
        if (stack_top(&p->stack) == p->head)
            stack_pop_to(p, p->head);
        stack_push(&p->stack, p->body);
    }
    return 1;
}

/**
 * @brief 处理一个起始标签 / Handle a start tag
 *
 * @details 依次执行：折入隐含容器、`<select>` 作用域过滤、body 内容开始时隐式闭合 `</head>`、隐式闭合规则、表格节点补全，最后插入并按需压栈。
 *          In order: fold into implied containers, filter inside `<select>` scope, implicitly close `</head>` when body content starts, apply implicit-closing rules, synthesise table nodes, then insert and push when appropriate.
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in]     tag 标签名（已小写）/ Tag name, already lowercased
 * @param[in,out] node 已创建并解析好属性的元素节点 / Element node already created with its attributes parsed
 * @param[in]     self_closing 自闭合标志（`/>` 为 1）/ Self-closing flag, 1 for `/>`
 *
 * @note 空元素（`k_void_tags` 中的标签）与自闭合标签不压栈 / Void tags (those in `k_void_tags`) and self-closing tags are not pushed
 */
static void handle_start_tag(Parser *p, const char *tag, HpeDomNode *node, int self_closing)
{
    HpeDomNode *sel;

    if (handle_implied_container(p, tag, node))
        return;

    /* inside <select> only option/optgroup/hr/script/style/template survive:
     * a nested <select> is treated as its end tag, anything else is ignored */
    sel = open_select(p);
    if (sel) {
        if (!strcmp(tag, "select")) {
            stack_pop_to(p, sel);
            return;
        }
        if (!in_list(k_in_select, tag))
            return; /* dropped: no node inserted, nothing pushed */
    }

    /* implicit </head> when body content starts */
    if (stack_top(&p->stack) == p->head && !is_head_content(tag))
        stack_pop_to(p, p->head);

    auto_close_for_start(p, tag);
    synthesize_table_nodes(p, tag);

    insert_node(p, node, tag);
    if (!self_closing && !in_list(k_void_tags, tag))
        stack_push(&p->stack, node);
}

/**
 * @brief 插入一个空 `<p>` 元素 / Insert an empty `p` element
 *
 * @details `</p>` 在没有开放段落时按 HTML 容错规则插入一个空段落。
 *          A `</p>` with no open paragraph inserts an empty one, per HTML fault tolerance.
 *
 * @param[in,out] p 解析器状态 / Parser state
 */
static void insert_empty_p(Parser *p)
{
    HpeDomNode *node = hpe_dom_element("p");
    insert_node(p, node, "p");
}

/**
 * @brief 处理一个结束标签 / Handle an end tag
 *
 * @details `</html>`/`</body>` 弹空到 `<html>`；`</br>` 按 `<br>` 处理；`<select>` 内只认可少数结束标签；否则沿栈找到同名元素并把其上的节点全部弹出。
 *          `</html>` and `</body>` pop back to `<html>`; `</br>` behaves as `<br>`; only a few end tags are honoured inside `<select>`; otherwise the stack is searched for a matching element and everything above it is popped.
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in]     tag 标签名（已小写）/ Tag name, already lowercased
 *
 * @note 找不到匹配元素且标签为 `p` 时补插空段落 / When no element matches and the tag is `p`, an empty paragraph is inserted
 *
 * @see insert_empty_p
 */
static void handle_end_tag(Parser *p, const char *tag)
{
    HpeDomNode *sel;
    int i;

    if (!strcmp(tag, "html") || !strcmp(tag, "body")) {
        while (p->stack.count > 0 && stack_top(&p->stack) != p->html)
            p->stack.count--;
        return;
    }
    /* `</br>` behaves like `<br>` */
    if (!strcmp(tag, "br")) {
        handle_start_tag(p, "br", hpe_dom_element("br"), 1);
        return;
    }
    /* inside <select> only a few end tags are honoured */
    sel = open_select(p);
    if (sel && strcmp(tag, "select") && strcmp(tag, "option") &&
        strcmp(tag, "optgroup") && strcmp(tag, "template"))
        return;
    for (i = p->stack.count - 1; i >= 0; i--) {
        HpeDomNode *node = p->stack.items[i];
        if (node->type == HPE_NODE_ELEMENT && hpe_str_ieq(node->tag, tag)) {
            p->stack.count = i;
            return;
        }
    }
    if (!strcmp(tag, "p"))
        insert_empty_p(p);
}

/* ---------------- tokenizer ---------------- */

/**
 * @brief 解析起始标签内的 `name="value"` 属性 / Parse the `name="value"` attributes of a start tag
 *
 * @details 支持双引号、单引号与无引号三种取值形式，属性值经实体解码后写入；读到 `>` 结束。
 *          Quoted (double or single) and unquoted values are supported, each entity-decoded before being stored; parsing ends at `>`.
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in,out] node 接收属性的元素节点 / Element node receiving the attributes
 * @param[out]    self_closing 置 1 表示见到结尾 `/` / Set to 1 when a trailing `/` was seen
 *
 * @note 同名属性重复出现时后者覆盖前者 / Later duplicates of the same attribute overwrite earlier ones
 */
static void parse_attributes(Parser *p, HpeDomNode *node, int *self_closing)
{
    while (p->pos < p->len) {
        char aname[64];
        int an;
        char *aval = NULL;

        skip_spaces(p);
        if (p->pos >= p->len)
            break;
        if (p->src[p->pos] == '>') {
            p->pos++;
            break;
        }
        if (p->src[p->pos] == '/') {
            *self_closing = 1;
            p->pos++;
            continue;
        }

        an = read_name(p, aname, sizeof(aname));
        skip_spaces(p);
        if (p->pos < p->len && p->src[p->pos] == '=') {
            p->pos++;
            skip_spaces(p);
            if (p->pos < p->len && (p->src[p->pos] == '"' || p->src[p->pos] == '\'')) {
                char quote = p->src[p->pos++];
                size_t vstart = p->pos;
                while (p->pos < p->len && p->src[p->pos] != quote)
                    p->pos++;
                aval = decode_entities(p->src + vstart, p->pos - vstart, NULL);
                if (p->pos < p->len)
                    p->pos++;
            } else {
                size_t vstart = p->pos;
                size_t dlen = 0;
                while (p->pos < p->len && !hpe_is_space(p->src[p->pos]) &&
                       p->src[p->pos] != '>')
                    p->pos++;
                aval = decode_entities(p->src + vstart, p->pos - vstart, &dlen);
            }
        }
        if (an > 0)
            hpe_dom_set_attr(node, aname, aval ? aval : "");
        if (aval)
            hpe_free(aval);
    }
}

/**
 * @brief 消费 `<style>`/`<script>` 的原始文本直到其结束标签 / Consume the raw body of a `<style>` or `<script>` up to its end tag
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in,out] node 原始文本所属元素 / Element owning the raw text
 * @param[in]     tag 元素标签名，用于构造结束标签 / Element tag name, used to build the closing tag
 *
 * @note 找不到结束标签时一直消费到输入末尾 / When no end tag is found the rest of the input is consumed
 */
static void parse_rawtext_body(Parser *p, HpeDomNode *node, const char *tag)
{
    char close[80];
    size_t cend;

    snprintf(close, sizeof(close), "</%s", tag);
    cend = find_ci(p->src, p->len, p->pos, close);
    if (cend == (size_t)-1)
        cend = p->len;
    if (cend > p->pos) {
        size_t tlen = 0;
        char *raw = decode_entities(p->src + p->pos, cend - p->pos, &tlen);
        hpe_dom_append(node, hpe_dom_text(raw));
        hpe_free(raw);
    }
    p->pos = cend;
    if (cend < p->len)
        skip_to_tag_end(p);
}

/**
 * @brief 消费一个 `</name ...>` 结束标签并应用 / Consume a `</name ...>` end tag and apply it
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return 固定返回 1 表示已读到一个结束标签 / Always 1, reporting that an end tag was read
 *
 * @note 读到名称为空时只前进游标，不触发任何树操作 / An empty name only advances the cursor without any tree action
 *
 * @see handle_end_tag
 */
static int consume_end_tag(Parser *p)
{
    char name[64];
    size_t start;
    int n;

    p->pos += 2; /* "</" */
    skip_spaces(p);
    start = p->pos;
    n = read_name(p, name, sizeof(name));
    skip_to_tag_end(p);
    if (p->pos > start && n > 0)
        handle_end_tag(p, name);
    return 1;
}

/**
 * @brief 消费一个 `<name attrs...>` 起始标签或原始文本 / Consume a `<name attrs...>` start tag or raw text
 *
 * @details 读取名称与属性；原始文本元素转入 `parse_rawtext_body`，其余交给 `handle_start_tag`。
 *          The name and attributes are read; raw-text elements divert to `parse_rawtext_body`, everything else goes to `handle_start_tag`.
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return 固定返回 1 表示已读到一个起始标签 / Always 1, reporting that a start tag was read
 *
 * @note 孤立 `<` 当作普通文本写入；未被树接纳的节点会被立即释放 / A stray `<` is emitted as text, and a node the tree did not accept is freed immediately
 */
static int consume_start_tag(Parser *p)
{
    char tag[64];
    int self_closing = 0;
    HpeDomNode *node;
    int n;

    p->pos++; /* '<' */
    n = read_name(p, tag, sizeof(tag));
    if (n == 0) {
        add_text(p, p->src + p->pos - 1, p->src + p->pos); /* stray '<' */
        return 1;
    }

    node = hpe_dom_element(tag);
    parse_attributes(p, node, &self_closing);

    if (is_rawtext(tag)) {
        insert_node(p, node, NULL);
        parse_rawtext_body(p, node, tag);
        return 1;
    }

    handle_start_tag(p, tag, node, self_closing);
    /* merged into an implicit element, dropped, or ignored inside <select>:
     * the node never entered the tree, so release it */
    if (node->parent == NULL && node->content == NULL)
        hpe_dom_free(node);
    return 1;
}

/**
 * @brief 消费注释、DOCTYPE 或处理指令 / Consume a comment, doctype or processing instruction
 *
 * @details `<!-- ... -->` 跳到注释结束符；`<!doctype ...>` 与 `<?pi ...>` 跳到下一个 `>`。
 *          `<!-- ... -->` skips to the comment terminator, while `<!doctype ...>` and `<?pi ...>` skip to the next `>`.
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @note 这些结构不产生任何 DOM 节点 / None of these constructs produce a DOM node
 */
static void consume_markup_decl(Parser *p)
{
    if (p->pos + 4 <= p->len && !strncmp(p->src + p->pos, "<!--", 4)) {
        size_t end = find_ci(p->src, p->len, p->pos + 4, "-->");
        p->pos = (end == (size_t)-1) ? p->len : end + 3;
        return;
    }
    skip_to_tag_end(p); /* <!doctype ...> / <?pi ...> */
}

/**
 * @brief 初始化解析器及其合成骨架 / Initialise the parser and its synthetic scaffolding
 *
 * @details 预建 `document → html → head, body` 骨架并把 `<body>` 压入开放元素栈。
 *          Pre-builds the `document` → `html` → `head`, `body` scaffolding and pushes `<body>` onto the open-element stack.
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in]     source 待解析的 HTML 源文本 / HTML source text to parse
 * @param[in]     length 源文本字节数 / Source byte count
 */

static void parser_init(Parser *p, const char *source, size_t length)
{
    memset(p, 0, sizeof(*p));
    p->src = source;
    p->len = length;
    p->doc = hpe_dom_document();
    p->html = hpe_dom_element("html");
    p->head = hpe_dom_element("head");
    p->body = hpe_dom_element("body");
    hpe_dom_append(p->doc, p->html);
    hpe_dom_append(p->html, p->head);
    hpe_dom_append(p->html, p->body);
    stack_push(&p->stack, p->body);
}

/**
 * @brief 推进解析器一步 / Advance the parser by one step
 *
 * @details 按当前字符分派：普通文本走 `add_text`，`<!`/`<?` 走 `consume_markup_decl`，`</` 走 `consume_end_tag`，其余走 `consume_start_tag`。
 *          Dispatch on the current character: plain text goes to `add_text`, `<!`/`<?` to `consume_markup_decl`, `</` to `consume_end_tag` and anything else to `consume_start_tag`.
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @note 一次调用至少消费一个字符，因此主循环必然前进 / A call always consumes at least one character, so the main loop always makes progress
 */
static void parse_step(Parser *p)
{
    if (p->src[p->pos] != '<') {
        size_t start = p->pos;
        while (p->pos < p->len && p->src[p->pos] != '<')
            p->pos++;
        add_text(p, p->src + start, p->src + p->pos);
        return;
    }
    if (p->pos + 2 <= p->len && (p->src[p->pos + 1] == '!' || p->src[p->pos + 1] == '?')) {
        consume_markup_decl(p);
        return;
    }
    if (p->pos + 2 <= p->len && p->src[p->pos + 1] == '/') {
        consume_end_tag(p);
        return;
    }
    consume_start_tag(p);
}

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
HpeDomNode *hpe_html_parse(const char *source, size_t length)
{
    Parser p;

    parser_init(&p, source, length);
    while (p.pos < p.len)
        parse_step(&p);
    if (p.stack.items)
        hpe_free(p.stack.items);
    return p.doc;
}
