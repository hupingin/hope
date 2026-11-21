#include "html/hpe_html.h"

#include <stdio.h>
#include <string.h>

#include "base/hpe_util.h"

/* ---------------- tag tables ---------------- */

static const char *k_void_tags[] = {
    "area", "base", "br", "col", "embed", "hr", "img", "input",
    "link", "meta", "param", "source", "track", "wbr", NULL};

static const char *k_block_tags[] = {
    "address", "article", "aside", "blockquote", "div", "dl", "fieldset",
    "figcaption", "figure", "footer", "form", "h1", "h2", "h3", "h4", "h5",
    "h6", "header", "hr", "li", "main", "nav", "ol", "p", "pre", "section",
    "table", "tbody", "td", "tfoot", "th", "thead", "tr", "ul", NULL};

static int in_list(const char **list, const char *tag)
{
    int i;
    for (i = 0; list[i]; i++) {
        if (strcmp(list[i], tag) == 0)
            return 1;
    }
    return 0;
}

/* ---------------- entity decoding ---------------- */

static const char *entity_named(const char *name)
{
    if (!strcmp(name, "amp"))
        return "&";
    if (!strcmp(name, "lt"))
        return "<";
    if (!strcmp(name, "gt"))
        return ">";
    if (!strcmp(name, "quot"))
        return "\"";
    if (!strcmp(name, "apos"))
        return "'";
    if (!strcmp(name, "nbsp"))
        return " "; /* rendered as a normal space by the layout engine */
    if (!strcmp(name, "copy"))
        return "\xC2\xA9";
    if (!strcmp(name, "middot"))
        return "\xC2\xB7";
    if (!strcmp(name, "mdash"))
        return "\xE2\x80\x94";
    if (!strcmp(name, "ndash"))
        return "\xE2\x80\x93";
    if (!strcmp(name, "hellip"))
        return "\xE2\x80\xA6";
    if (!strcmp(name, "times"))
        return "\xC3\x97";
    if (!strcmp(name, "rarr"))
        return "\xE2\x86\x92";
    if (!strcmp(name, "larr"))
        return "\xE2\x86\x90";
    if (!strcmp(name, "uarr"))
        return "\xE2\x86\x91";
    if (!strcmp(name, "darr"))
        return "\xE2\x86\x93";
    if (!strcmp(name, "bull"))
        return "\xE2\x80\xA2";
    if (!strcmp(name, "deg"))
        return "\xC2\xB0";
    if (!strcmp(name, "trade"))
        return "\xE2\x84\xA2";
    if (!strcmp(name, "reg"))
        return "\xC2\xAE";
    if (!strcmp(name, "ldquo"))
        return "\xE2\x80\x9C";
    if (!strcmp(name, "rdquo"))
        return "\xE2\x80\x9D";
    if (!strcmp(name, "lsquo"))
        return "\xE2\x80\x98";
    if (!strcmp(name, "rsquo"))
        return "\xE2\x80\x99";
    return NULL;
}

static char *decode_entities(const char *s, size_t len, size_t *out_len)
{
    char *out = (char *)hpe_alloc(len + 1);
    size_t i = 0, o = 0;

    while (i < len) {
        if (s[i] == '&') {
            size_t semi = i + 1;
            while (semi < len && semi - i < 12 && s[semi] != ';')
                semi++;
            if (semi < len && s[semi] == ';' && semi > i + 1) {
                char ent[16];
                size_t nlen = semi - i - 1;
                if (nlen < sizeof(ent)) {
                    const char *rep = NULL;
                    char tmp[4];
                    int tn = 0;

                    memcpy(ent, s + i + 1, nlen);
                    ent[nlen] = '\0';
                    if (ent[0] == '#') {
                        uint32_t cp = 0;
                        int ok = 1;
                        const char *p = ent + 1;
                        int base = 10;
                        if (*p == 'x' || *p == 'X') {
                            base = 16;
                            p++;
                        }
                        if (!*p)
                            ok = 0;
                        while (ok && *p) {
                            int d;
                            if (*p >= '0' && *p <= '9')
                                d = *p - '0';
                            else if (base == 16 && hpe_ascii_lower((unsigned char)*p) >= 'a' &&
                                     hpe_ascii_lower((unsigned char)*p) <= 'f')
                                d = hpe_ascii_lower((unsigned char)*p) - 'a' + 10;
                            else {
                                ok = 0;
                                break;
                            }
                            cp = cp * (uint32_t)base + (uint32_t)d;
                            p++;
                        }
                        if (ok && cp > 0) {
                            tn = hpe_utf8_encode(cp, tmp);
                        }
                    } else {
                        rep = entity_named(ent);
                    }

                    if (rep) {
                        size_t rl = strlen(rep);
                        memcpy(out + o, rep, rl);
                        o += rl;
                        i = semi + 1;
                        continue;
                    }
                    if (tn > 0) {
                        memcpy(out + o, tmp, (size_t)tn);
                        o += (size_t)tn;
                        i = semi + 1;
                        continue;
                    }
                }
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

static HpeDomNode *insertion_parent(Parser *p)
{
    HpeDomNode *top = stack_top(&p->stack);
    if (!top)
        return p->body;
    if (top == p->html)
        return p->body;
    return top;
}

static int all_whitespace(const char *s)
{
    for (; *s; s++) {
        if (!hpe_is_space(*s))
            return 0;
    }
    return 1;
}

static void add_text(Parser *p, const char *begin, const char *end)
{
    size_t declen = 0;
    char *text;
    HpeDomNode *node;

    if (end <= begin)
        return;
    text = decode_entities(begin, (size_t)(end - begin), &declen);
    if (declen == 0) {
        hpe_free(text);
        return;
    }
    /* whitespace between head elements carries no meaning */
    if (stack_top(&p->stack) == p->head && all_whitespace(text)) {
        hpe_free(text);
        return;
    }
    node = hpe_dom_text(text);
    hpe_dom_append(insertion_parent(p), node);
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
           !strcmp(tag, "title") || !strcmp(tag, "base") || !strcmp(tag, "script");
}

/* ---------------- tree builder ---------------- */

static void handle_start_tag(Parser *p, const char *tag, HpeDomNode *node, int self_closing)
{
    if (!strcmp(tag, "html")) {
        /* the synthetic <html> already exists: keep only its attributes */
        int i;
        for (i = 0; i < node->attr_count; i++)
            hpe_dom_set_attr(p->html, node->attrs[i].name, node->attrs[i].value);
        return;
    }
    if (!strcmp(tag, "head")) {
        int i;
        for (i = 0; i < node->attr_count; i++)
            hpe_dom_set_attr(p->head, node->attrs[i].name, node->attrs[i].value);
        stack_push(&p->stack, p->head);
        return;
    }
    if (!strcmp(tag, "body")) {
        int i;
        for (i = 0; i < node->attr_count; i++)
            hpe_dom_set_attr(p->body, node->attrs[i].name, node->attrs[i].value);
        if (stack_top(&p->stack) == p->head)
            stack_pop_to(p, p->head);
        stack_push(&p->stack, p->body);
        return;
    }

    /* implicit </head> when body content starts */
    if (stack_top(&p->stack) == p->head && !is_head_content(tag))
        stack_pop_to(p, p->head);

    /* implicit closing of paragraphs and list items */
    {
        HpeDomNode *top = stack_top(&p->stack);
        while (top && top->type == HPE_NODE_ELEMENT) {
            int close_top = 0;
            if (!strcmp(top->tag, "p") && in_list(k_block_tags, tag))
                close_top = 1;
            if (!strcmp(top->tag, "li") && !strcmp(tag, "li"))
                close_top = 1;
            if (!close_top)
                break;
            p->stack.count--;
            top = stack_top(&p->stack);
        }
    }

    hpe_dom_append(insertion_parent(p), node);
    if (!self_closing && !in_list(k_void_tags, tag))
        stack_push(&p->stack, node);
}

static void handle_end_tag(Parser *p, const char *tag)
{
    int i;
    if (!strcmp(tag, "html") || !strcmp(tag, "body")) {
        while (p->stack.count > 0 && stack_top(&p->stack) != p->html)
            p->stack.count--;
        return;
    }
    for (i = p->stack.count - 1; i >= 0; i--) {
        HpeDomNode *node = p->stack.items[i];
        if (node->type == HPE_NODE_ELEMENT && hpe_str_ieq(node->tag, tag)) {
            p->stack.count = i;
            return;
        }
    }
}

HpeDomNode *hpe_html_parse(const char *source, size_t length)
{
    Parser p;
    memset(&p, 0, sizeof(p));
    p.src = source;
    p.len = length;

    p.doc = hpe_dom_document();
    p.html = hpe_dom_element("html");
    p.head = hpe_dom_element("head");
    p.body = hpe_dom_element("body");
    hpe_dom_append(p.doc, p.html);
    hpe_dom_append(p.html, p.head);
    hpe_dom_append(p.html, p.body);
    stack_push(&p.stack, p.body);

    while (p.pos < p.len) {
        char c = p.src[p.pos];

        if (c != '<') {
            size_t start = p.pos;
            while (p.pos < p.len && p.src[p.pos] != '<')
                p.pos++;
            add_text(&p, p.src + start, p.src + p.pos);
            continue;
        }

        /* comment */
        if (p.pos + 4 <= p.len && !strncmp(p.src + p.pos, "<!--", 4)) {
            size_t end = find_ci(p.src, p.len, p.pos + 4, "-->");
            p.pos = (end == (size_t)-1) ? p.len : end + 3;
            continue;
        }

        /* doctype or other <! ... > */
        if (p.pos + 2 <= p.len && (p.src[p.pos + 1] == '!' || p.src[p.pos + 1] == '?')) {
            while (p.pos < p.len && p.src[p.pos] != '>')
                p.pos++;
            if (p.pos < p.len)
                p.pos++;
            continue;
        }

        /* end tag */
        if (p.pos + 2 <= p.len && p.src[p.pos + 1] == '/') {
            char name[64];
            size_t start;
            int n = 0;
            p.pos += 2;
            skip_spaces(&p);
            start = p.pos;
            while (p.pos < p.len && is_name_char(p.src[p.pos])) {
                if (n < (int)sizeof(name) - 1)
                    name[n++] = (char)hpe_ascii_lower((unsigned char)p.src[p.pos]);
                p.pos++;
            }
            name[n] = '\0';
            while (p.pos < p.len && p.src[p.pos] != '>')
                p.pos++;
            if (p.pos < p.len)
                p.pos++;
            if (p.pos > start && name[0])
                handle_end_tag(&p, name);
            continue;
        }

        /* start tag */
        {
            char tag[64];
            int n = 0;
            int self_closing = 0;
            HpeDomNode *node;

            p.pos++; /* '<' */
            while (p.pos < p.len && is_name_char(p.src[p.pos])) {
                if (n < (int)sizeof(tag) - 1)
                    tag[n++] = (char)hpe_ascii_lower((unsigned char)p.src[p.pos]);
                p.pos++;
            }
            tag[n] = '\0';
            if (n == 0) {
                add_text(&p, p.src + p.pos - 1, p.src + p.pos);
                continue;
            }

            node = hpe_dom_element(tag);

            /* attributes */
            while (p.pos < p.len) {
                char aname[64];
                int an = 0;
                char *aval = NULL;

                skip_spaces(&p);
                if (p.pos >= p.len)
                    break;
                if (p.src[p.pos] == '>') {
                    p.pos++;
                    break;
                }
                if (p.src[p.pos] == '/') {
                    self_closing = 1;
                    p.pos++;
                    continue;
                }

                while (p.pos < p.len && is_name_char(p.src[p.pos])) {
                    if (an < (int)sizeof(aname) - 1)
                        aname[an++] = (char)hpe_ascii_lower((unsigned char)p.src[p.pos]);
                    p.pos++;
                }
                aname[an] = '\0';

                skip_spaces(&p);
                if (p.pos < p.len && p.src[p.pos] == '=') {
                    p.pos++;
                    skip_spaces(&p);
                    if (p.pos < p.len && (p.src[p.pos] == '"' || p.src[p.pos] == '\'')) {
                        char quote = p.src[p.pos++];
                        size_t vstart = p.pos;
                        while (p.pos < p.len && p.src[p.pos] != quote)
                            p.pos++;
                        aval = decode_entities(p.src + vstart, p.pos - vstart, NULL);
                        if (p.pos < p.len)
                            p.pos++;
                    } else {
                        size_t vstart = p.pos;
                        char *decoded;
                        size_t dlen = 0;
                        while (p.pos < p.len && !hpe_is_space(p.src[p.pos]) &&
                               p.src[p.pos] != '>')
                            p.pos++;
                        decoded = decode_entities(p.src + vstart, p.pos - vstart, &dlen);
                        aval = decoded;
                    }
                }
                if (an > 0) {
                    hpe_dom_set_attr(node, aname, aval ? aval : "");
                }
                if (aval)
                    hpe_free(aval);
            }

            if (!strcmp(tag, "style") || !strcmp(tag, "script")) {
                char close[80];
                size_t cend;
                hpe_dom_append(insertion_parent(&p), node);
                snprintf(close, sizeof(close), "</%s", tag);
                cend = find_ci(p.src, p.len, p.pos, close);
                if (cend == (size_t)-1)
                    cend = p.len;
                if (cend > p.pos) {
                    /* raw text becomes a child of the style/script element itself */
                    size_t tlen = 0;
                    char *raw = decode_entities(p.src + p.pos, cend - p.pos, &tlen);
                    hpe_dom_append(node, hpe_dom_text(raw));
                    hpe_free(raw);
                }
                p.pos = cend;
                if (cend < p.len) {
                    while (p.pos < p.len && p.src[p.pos] != '>')
                        p.pos++;
                    if (p.pos < p.len)
                        p.pos++;
                }
                continue;
            }

            handle_start_tag(&p, tag, node, self_closing);
            if (node->parent == NULL)
                hpe_dom_free(node); /* merged into an implicit element */
        }
    }

    if (p.stack.items)
        hpe_free(p.stack.items);
    return p.doc;
}
