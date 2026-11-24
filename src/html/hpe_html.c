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

static int in_list(const char **list, const char *tag)
{
    int i;
    for (i = 0; list[i]; i++) {
        if (strcmp(list[i], tag) == 0)
            return 1;
    }
    return 0;
}

static int is_heading(const char *tag)
{
    return tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6' && tag[2] == '\0';
}

/* ---------------- entity decoding ---------------- */

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

/* decodes a single `&...;` token at `s` (length `len`); returns the number of
 * utf-8 bytes written to `out` (max 4) or 0 when the token is not recognised */
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

static void stack_push(NodeStack *st, HpeDomNode *node)
{
    st->items = (HpeDomNode **)hpe_grow(st->items, &st->cap, (size_t)st->count + 1,
                                        sizeof(HpeDomNode *));
    st->items[st->count++] = node;
}

static HpeDomNode *current_node(Parser *p)
{
    if (p->stack.count > 0)
        return p->stack.items[p->stack.count - 1];
    return p->body;
}

static HpeDomNode *stack_top(NodeStack *st)
{
    return st->count > 0 ? st->items[st->count - 1] : NULL;
}

static void stack_pop_to(Parser *p, HpeDomNode *node)
{
    while (p->stack.count > 0) {
        HpeDomNode *top = p->stack.items[p->stack.count - 1];
        p->stack.count--;
        if (top == node)
            break;
    }
}

static int all_whitespace(const char *s)
{
    for (; *s; s++) {
        if (!hpe_is_space(*s))
            return 0;
    }
    return 1;
}

/* ---------------- table context / foster parenting ----------------
 * When text or a non-table element appears directly inside table/tbody/thead/
 * tfoot/tr, the HTML parser "fosters" it to just before the nearest <table>
 * (that is why a stray <div> in a table renders above it in browsers). */

static int in_table_context(HpeDomNode *n)
{
    if (!n || n->type != HPE_NODE_ELEMENT)
        return 0;
    return !strcmp(n->tag, "table") || !strcmp(n->tag, "tbody") ||
           !strcmp(n->tag, "thead") || !strcmp(n->tag, "tfoot") ||
           !strcmp(n->tag, "tr");
}

static int allowed_in_table(HpeDomNode *ctx, const char *tag)
{
    if (!strcmp(ctx->tag, "table"))
        return tag ? in_list(k_in_table, tag) : 0;
    if (!strcmp(ctx->tag, "tr"))
        return tag ? in_list(k_in_row, tag) : 0;
    return tag ? in_list(k_in_section, tag) : 0; /* tbody / thead / tfoot */
}

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

/* appends `node` at the insertion point, fostering it before the table when the
 * current node is a table context that does not accept it */
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

/* ---------------- token helpers ---------------- */

static int is_name_char(char c)
{
    return !hpe_is_space(c) && c != '>' && c != '/' && c != '=' && c != '\0';
}

static void skip_spaces(Parser *p)
{
    while (p->pos < p->len && hpe_is_space(p->src[p->pos]))
        p->pos++;
}

/* reads a lowercased tag/attribute name into `out`; returns its length */
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

/* advances past the next '>' (consuming it when present) */
static void skip_to_tag_end(Parser *p)
{
    while (p->pos < p->len && p->src[p->pos] != '>')
        p->pos++;
    if (p->pos < p->len)
        p->pos++;
}

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

static int is_head_content(const char *tag)
{
    return !strcmp(tag, "meta") || !strcmp(tag, "link") || !strcmp(tag, "style") ||
           !strcmp(tag, "title") || !strcmp(tag, "base") || !strcmp(tag, "script") ||
           !strcmp(tag, "template") || !strcmp(tag, "noscript");
}

static int is_rawtext(const char *tag)
{
    return !strcmp(tag, "style") || !strcmp(tag, "script");
}

/* the innermost open <select>, or NULL */
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

/* `<table><tr>` implies `<tbody>`; `<table><td>` / `<tbody><td>` imply `<tr>`
 * (and a section). Synthesised elements are legal table content, no fostering. */
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

/* <html>/<head>/<body> fold into the synthetic scaffolding instead of being
 * inserted; returns 1 when the tag was consumed that way */
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

/* `</p>` with no open paragraph inserts an empty one (HTML fault tolerance) */
static void insert_empty_p(Parser *p)
{
    HpeDomNode *node = hpe_dom_element("p");
    insert_node(p, node, "p");
}

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

/* parses `name="value"` pairs up to the closing '>'; sets *self_closing when a
 * trailing '/' was seen */
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

/* consumes the raw body of a <style>/<script> up to its own end tag */
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

/* consumes `</name ...>` and applies it; returns 1 when an end tag was read */
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

/* consumes `<name attrs...>` (or rawtext); returns 1 when a start tag was read */
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

/* consumes a comment, doctype or processing instruction */
static void consume_markup_decl(Parser *p)
{
    if (p->pos + 4 <= p->len && !strncmp(p->src + p->pos, "<!--", 4)) {
        size_t end = find_ci(p->src, p->len, p->pos + 4, "-->");
        p->pos = (end == (size_t)-1) ? p->len : end + 3;
        return;
    }
    skip_to_tag_end(p); /* <!doctype ...> / <?pi ...> */
}

/* ---------------- entry point ---------------- */

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

/* one step of the tokenizer: text run, markup declaration, end tag or start tag */
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
