/* Hope UI Engine - javascript lexer + recursive-descent parser
 *
 * Grammar coverage: declarations, arrow/async/generator/class, destructuring,
 * spread, default & rest parameters, try/catch/finally/throw, switch, labels,
 * optional chaining, nullish coalescing, bitwise/shift operators, template
 * literals (re-serialised to concatenation for the interpreter), modules.
 *
 * Template literals are lexed as a single token holding their full source and
 * expanded here into a JS_N_TEMPLATE node (cooked strings + expressions), so
 * the interpreter never sees a template-specific token.
 */
#include "js/hpe_js_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- tokens ---------------- */

enum { T_EOF, T_NUM, T_STR, T_IDENT, T_PUNCT, T_REGEX, T_TEMPLATE };

typedef struct {
    int type;
    double num;
    char *text;
    int line;
} Tok;

typedef struct {
    HpeJs *js;
    const char *src;
    size_t len;
    size_t pos;
    int line;
    Tok *toks;
    int ntok;
    size_t cap;
} Lexer;

static void lex_add(Lexer *lx, int type, double num, const char *text, size_t textlen, int line)
{
    if ((size_t)lx->ntok + 1 > lx->cap) {
        size_t ncap = lx->cap ? lx->cap * 2 : 512;
        Tok *nt = (Tok *)hpe_js_alloc(lx->js, ncap * sizeof(Tok));
        if (lx->toks && lx->ntok)
            memcpy(nt, lx->toks, (size_t)lx->ntok * sizeof(Tok));
        lx->toks = nt;
        lx->cap = ncap;
    }
    lx->toks[lx->ntok].type = type;
    lx->toks[lx->ntok].num = num;
    lx->toks[lx->ntok].text = text ? hpe_js_strndup(lx->js, text, textlen) : NULL;
    lx->toks[lx->ntok].line = line;
    lx->ntok++;
}

static int lex_ident_start(char c)
{
    /* allow non-ASCII so identifiers can be Chinese */
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' ||
           (unsigned char)c >= 0x80;
}

static int lex_ident_char(char c)
{
    return lex_ident_start(c) || (c >= '0' && c <= '9');
}

/* ---------------- string literal ---------------- */

static void lex_string(Lexer *lx, char quote)
{
    char *buf;
    size_t n = 0, cap = lx->len + 1;
    int start_line = lx->line;
    buf = (char *)malloc(cap);
    if (!buf)
        js_error(lx->js, lx->line, "out of memory");
    lx->pos++; /* opening quote */
    while (lx->pos < lx->len && lx->src[lx->pos] != quote) {
        char c = lx->src[lx->pos];
        if (c == '\\' && lx->pos + 1 < lx->len) {
            char e = lx->src[lx->pos + 1];
            lx->pos += 2;
            switch (e) {
            case 'n':
                buf[n++] = '\n';
                break;
            case 't':
                buf[n++] = '\t';
                break;
            case 'r':
                buf[n++] = '\r';
                break;
            case 'b':
                buf[n++] = '\b';
                break;
            case 'f':
                buf[n++] = '\f';
                break;
            case 'v':
                buf[n++] = '\v';
                break;
            case '0':
                buf[n++] = '\0';
                break;
            case 'x': {
                uint32_t cp = 0;
                int i;
                for (i = 0; i < 2 && lx->pos < lx->len; i++) {
                    char h = lx->src[lx->pos];
                    int d = -1;
                    if (h >= '0' && h <= '9')
                        d = h - '0';
                    else if (h >= 'a' && h <= 'f')
                        d = h - 'a' + 10;
                    else if (h >= 'A' && h <= 'F')
                        d = h - 'A' + 10;
                    if (d < 0)
                        break;
                    cp = cp * 16 + (uint32_t)d;
                    lx->pos++;
                }
                buf[n++] = (char)cp;
                break;
            }
            case 'u': {
                uint32_t cp = 0;
                int i;
                if (lx->pos < lx->len && lx->src[lx->pos] == '{') {
                    lx->pos++;
                    while (lx->pos < lx->len && lx->src[lx->pos] != '}') {
                        char h = lx->src[lx->pos];
                        int d = -1;
                        if (h >= '0' && h <= '9')
                            d = h - '0';
                        else if (h >= 'a' && h <= 'f')
                            d = h - 'a' + 10;
                        else if (h >= 'A' && h <= 'F')
                            d = h - 'A' + 10;
                        if (d < 0)
                            break;
                        cp = cp * 16 + (uint32_t)d;
                        lx->pos++;
                    }
                    if (lx->pos < lx->len)
                        lx->pos++; /* } */
                } else {
                    for (i = 0; i < 4 && lx->pos < lx->len; i++) {
                        char h = lx->src[lx->pos];
                        int d = -1;
                        if (h >= '0' && h <= '9')
                            d = h - '0';
                        else if (h >= 'a' && h <= 'f')
                            d = h - 'a' + 10;
                        else if (h >= 'A' && h <= 'F')
                            d = h - 'A' + 10;
                        if (d < 0)
                            break;
                        cp = cp * 16 + (uint32_t)d;
                        lx->pos++;
                    }
                }
                {
                    char utf[4];
                    int m = hpe_utf8_encode(cp, utf);
                    if (n + 4 < cap) {
                        memcpy(buf + n, utf, (size_t)m);
                        n += (size_t)m;
                    }
                }
                break;
            }
            case '\n':
                lx->line++;
                break;
            case '\r':
                break;
            default:
                buf[n++] = e;
                break;
            }
            continue;
        }
        if (c == '\n')
            lx->line++;
        if (n + 1 < cap)
            buf[n++] = c;
        lx->pos++;
    }
    if (lx->pos >= lx->len) {
        free(buf);
        js_error(lx->js, start_line, "unterminated string");
    }
    lx->pos++; /* closing quote */
    lex_add(lx, T_STR, 0, buf, n, start_line);
    free(buf);
}

/* ---------------- number literal ---------------- */

static void lex_number(Lexer *lx)
{
    char buf[80];
    size_t n = 0;
    int start_line = lx->line;
    double v;
    char c0 = lx->src[lx->pos];

    if (c0 == '0' && lx->pos + 1 < lx->len &&
        (lx->src[lx->pos + 1] == 'x' || lx->src[lx->pos + 1] == 'X')) {
        lx->pos += 2;
        while (lx->pos < lx->len && n < sizeof(buf) - 1) {
            char c = lx->src[lx->pos];
            if (c == '_') {
                lx->pos++;
                continue;
            }
            if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
                buf[n++] = c;
                lx->pos++;
            } else
                break;
        }
        buf[n] = '\0';
        v = (double)strtoll(buf, NULL, 16);
    } else if (c0 == '0' && lx->pos + 1 < lx->len &&
               (lx->src[lx->pos + 1] == 'b' || lx->src[lx->pos + 1] == 'B')) {
        unsigned long long acc = 0;
        lx->pos += 2;
        while (lx->pos < lx->len && (lx->src[lx->pos] == '0' || lx->src[lx->pos] == '1' ||
                                     lx->src[lx->pos] == '_')) {
            if (lx->src[lx->pos] != '_')
                acc = acc * 2 + (unsigned)(lx->src[lx->pos] - '0');
            lx->pos++;
        }
        v = (double)(long long)acc;
    } else if (c0 == '0' && lx->pos + 1 < lx->len &&
               (lx->src[lx->pos + 1] == 'o' || lx->src[lx->pos + 1] == 'O')) {
        unsigned long long acc = 0;
        lx->pos += 2;
        while (lx->pos < lx->len && ((lx->src[lx->pos] >= '0' && lx->src[lx->pos] <= '7') ||
                                     lx->src[lx->pos] == '_')) {
            if (lx->src[lx->pos] != '_')
                acc = acc * 8 + (unsigned)(lx->src[lx->pos] - '0');
            lx->pos++;
        }
        v = (double)(long long)acc;
    } else {
        while (lx->pos < lx->len && n < sizeof(buf) - 1 &&
               ((lx->src[lx->pos] >= '0' && lx->src[lx->pos] <= '9') ||
                lx->src[lx->pos] == '.' || lx->src[lx->pos] == '_')) {
            if (lx->src[lx->pos] != '_')
                buf[n++] = lx->src[lx->pos];
            lx->pos++;
        }
        if (lx->pos < lx->len && (lx->src[lx->pos] == 'e' || lx->src[lx->pos] == 'E') &&
            n < sizeof(buf) - 4) {
            buf[n++] = lx->src[lx->pos++];
            if (lx->pos < lx->len && (lx->src[lx->pos] == '+' || lx->src[lx->pos] == '-'))
                buf[n++] = lx->src[lx->pos++];
            while (lx->pos < lx->len && lx->src[lx->pos] >= '0' && lx->src[lx->pos] <= '9' &&
                   n < sizeof(buf) - 1)
                buf[n++] = lx->src[lx->pos++];
        }
        buf[n] = '\0';
        v = strtod(buf, NULL);
    }
    if (lx->pos < lx->len && lx->src[lx->pos] == 'n')
        lx->pos++; /* BigInt suffix: values are doubles here */
    lex_add(lx, T_NUM, v, NULL, 0, start_line);
}

/* ---------------- punctuation ---------------- */

static const char *k_puncts4[] = {">>>=", NULL};
static const char *k_puncts3[] = {"===", "!==", "...", "**=", ">>>", "<<=", ">>=",
                                  "&&=", "||=", "?" "?=", NULL};
static const char *k_puncts2[] = {"=>", "==", "!=", "<=", ">=", "&&", "||", "++", "--",
                                  "+=", "-=", "*=", "/=", "%=", "**", "<<", ">>", "&=",
                                  "|=", "^=", "??", "?.", NULL};
static const char *k_puncts1[] = {"+", "-", "*", "/", "%", "=", "<", ">", "!", "~",
                                  "&", "|", "^", "(", ")", "{", "}", "[", "]", ";", ",",
                                  ".", ":", "?", "#", "@", NULL};

/* a '/' starts a regex literal unless an expression just ended */
static int lex_regex_allowed(Lexer *lx)
{
    Tok *t;
    if (lx->ntok == 0)
        return 1;
    t = &lx->toks[lx->ntok - 1];
    if (t->type == T_NUM || t->type == T_STR || t->type == T_REGEX || t->type == T_TEMPLATE ||
        t->type == T_EOF)
        return 0;
    if (t->type == T_IDENT) {
        static const char *kw[] = {"return", "typeof",  "case",   "in",   "of",   "new",
                                   "delete", "void",    "do",     "else", "instanceof",
                                   "yield",  "await",   "throw",  "default", NULL};
        int i;
        for (i = 0; kw[i]; i++)
            if (!strcmp(t->text, kw[i]))
                return 1;
        return 0;
    }
    if (t->type == T_PUNCT) {
        if (!strcmp(t->text, ")") || !strcmp(t->text, "]") || !strcmp(t->text, "}"))
            return 0;
        if (!strcmp(t->text, "++") || !strcmp(t->text, "--"))
            return 0;
        return 1;
    }
    return 1;
}

static void lex_regex(Lexer *lx)
{
    size_t i = lx->pos + 1;
    int in_class = 0;
    int start_line = lx->line;

    while (i < lx->len) {
        char c = lx->src[i];
        if (c == '\\') {
            i += 2;
            continue;
        }
        if (c == '[') {
            in_class = 1;
            i++;
            continue;
        }
        if (c == ']') {
            in_class = 0;
            i++;
            continue;
        }
        if (c == '/' && !in_class)
            break;
        if (c == '\n') {
            js_error(lx->js, start_line, "unterminated regular expression");
            return;
        }
        i++;
    }
    if (i >= lx->len) {
        js_error(lx->js, start_line, "unterminated regular expression");
        return;
    }
    {
        /* keep pattern and flags in one string: "pattern\0flags" */
        size_t plen = i - (lx->pos + 1);
        size_t j = i + 1;
        size_t flen;
        char *buf;
        while (j < lx->len && lex_ident_char(lx->src[j]))
            j++;
        flen = j - (i + 1);
        buf = (char *)malloc(plen + flen + 2);
        if (!buf)
            js_error(lx->js, start_line, "out of memory");
        memcpy(buf, lx->src + lx->pos + 1, plen);
        buf[plen] = '\0';
        memcpy(buf + plen + 1, lx->src + i + 1, flen);
        buf[plen + 1 + flen] = '\0';
        lex_add(lx, T_REGEX, 0, buf, plen + 1 + flen, start_line);
        free(buf);
        lx->pos = j;
    }
}

/* scans a template literal starting at the backtick; returns the index just
 * past the closing backtick (or len on an error). Handles nested templates,
 * ${} substitutions, strings and comments inside a substitution. */
static size_t scan_template(Lexer *lx, size_t i, int *lines)
{
    lx->len = lx->len; /* silence unused warnings in odd builds */
    i++;               /* opening backtick */
    while (i < lx->len) {
        char c = lx->src[i];
        if (c == '\\') {
            i += 2;
            continue;
        }
        if (c == '`')
            return i + 1;
        if (c == '\n') {
            (*lines)++;
            i++;
            continue;
        }
        if (c == '$' && i + 1 < lx->len && lx->src[i + 1] == '{') {
            size_t j = i + 2;
            int depth = 1;
            while (j < lx->len && depth) {
                char d = lx->src[j];
                if (d == '{') {
                    depth++;
                    j++;
                } else if (d == '}') {
                    depth--;
                    j++;
                } else if (d == '"' || d == '\'') {
                    char q = d;
                    j++;
                    while (j < lx->len && lx->src[j] != q) {
                        if (lx->src[j] == '\\')
                            j++;
                        if (lx->src[j] == '\n')
                            (*lines)++;
                        j++;
                    }
                    j++;
                } else if (d == '`') {
                    j = scan_template(lx, j, lines);
                } else if (d == '/' && j + 1 < lx->len && lx->src[j + 1] == '/') {
                    while (j < lx->len && lx->src[j] != '\n')
                        j++;
                } else if (d == '/' && j + 1 < lx->len && lx->src[j + 1] == '*') {
                    j += 2;
                    while (j + 1 < lx->len && !(lx->src[j] == '*' && lx->src[j + 1] == '/')) {
                        if (lx->src[j] == '\n')
                            (*lines)++;
                        j++;
                    }
                    j += 2;
                } else {
                    j++;
                }
            }
            i = j;
            continue;
        }
        i++;
    }
    return i;
}

static void lex_template(Lexer *lx)
{
    int start_line = lx->line;
    size_t end = scan_template(lx, lx->pos, &lx->line);
    if (end > lx->len || lx->src[end - 1] != '`') {
        js_error(lx->js, start_line, "unterminated template literal");
        return;
    }
    lex_add(lx, T_TEMPLATE, 0, lx->src + lx->pos, end - lx->pos, start_line);
    lx->pos = end;
}

static void lex_all(Lexer *lx)
{
    while (lx->pos < lx->len) {
        char c = lx->src[lx->pos];
        int i, matched;

        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
            lx->pos++;
            continue;
        }
        if (c == '\n') {
            lx->line++;
            lx->pos++;
            continue;
        }
        if (c == '/' && lx->pos + 1 < lx->len && lx->src[lx->pos + 1] == '/') {
            while (lx->pos < lx->len && lx->src[lx->pos] != '\n')
                lx->pos++;
            continue;
        }
        if (c == '/' && lx->pos + 1 < lx->len && lx->src[lx->pos + 1] == '*') {
            lx->pos += 2;
            while (lx->pos + 1 < lx->len &&
                   !(lx->src[lx->pos] == '*' && lx->src[lx->pos + 1] == '/')) {
                if (lx->src[lx->pos] == '\n')
                    lx->line++;
                lx->pos++;
            }
            lx->pos = (lx->pos + 1 < lx->len) ? lx->pos + 2 : lx->len;
            continue;
        }

        if (c == '"' || c == '\'') {
            lex_string(lx, c);
            continue;
        }
        if (c == '`') {
            lex_template(lx);
            continue;
        }
        if (c == '/' && lex_regex_allowed(lx)) {
            lex_regex(lx);
            continue;
        }
        if ((c >= '0' && c <= '9') ||
            (c == '.' && lx->pos + 1 < lx->len && lx->src[lx->pos + 1] >= '0' &&
             lx->src[lx->pos + 1] <= '9')) {
            lex_number(lx);
            continue;
        }
        if (lex_ident_start(c)) {
            size_t start = lx->pos;
            while (lx->pos < lx->len && lex_ident_char(lx->src[lx->pos]))
                lx->pos++;
            lex_add(lx, T_IDENT, 0, lx->src + start, lx->pos - start, lx->line);
            continue;
        }

        matched = 0;
        for (i = 0; k_puncts4[i]; i++) {
            if (lx->pos + 4 <= lx->len && strncmp(lx->src + lx->pos, k_puncts4[i], 4) == 0) {
                lex_add(lx, T_PUNCT, 0, k_puncts4[i], 4, lx->line);
                lx->pos += 4;
                matched = 1;
                break;
            }
        }
        if (matched)
            continue;
        for (i = 0; k_puncts3[i]; i++) {
            if (lx->pos + 3 <= lx->len && strncmp(lx->src + lx->pos, k_puncts3[i], 3) == 0) {
                lex_add(lx, T_PUNCT, 0, k_puncts3[i], 3, lx->line);
                lx->pos += 3;
                matched = 1;
                break;
            }
        }
        if (matched)
            continue;
        for (i = 0; k_puncts2[i]; i++) {
            if (lx->pos + 2 <= lx->len && strncmp(lx->src + lx->pos, k_puncts2[i], 2) == 0) {
                /* `?.3` is a ternary, not optional chaining */
                if (!strcmp(k_puncts2[i], "?.") && lx->pos + 2 < lx->len &&
                    lx->src[lx->pos + 2] >= '0' && lx->src[lx->pos + 2] <= '9')
                    continue;
                lex_add(lx, T_PUNCT, 0, k_puncts2[i], 2, lx->line);
                lx->pos += 2;
                matched = 1;
                break;
            }
        }
        if (matched)
            continue;
        for (i = 0; k_puncts1[i]; i++) {
            if (lx->src[lx->pos] == k_puncts1[i][0]) {
                lex_add(lx, T_PUNCT, 0, k_puncts1[i], 1, lx->line);
                lx->pos++;
                matched = 1;
                break;
            }
        }
        if (!matched)
            js_error(lx->js, lx->line, "unexpected character '%c'", c);
    }
    lex_add(lx, T_EOF, 0, NULL, 0, lx->line);
}
/* ---------------- parser core ---------------- */

typedef struct {
    HpeJs *js;
    Tok *t;
    int pos;
    int ntok;
} P;

static Tok *p_cur(P *p)
{
    return &p->t[p->pos];
}

static Tok *p_at(P *p, int i)
{
    int k = p->pos + i;
    if (k >= p->ntok)
        k = p->ntok - 1;
    return &p->t[k];
}

static int p_is(P *p, const char *punct)
{
    Tok *t = p_cur(p);
    return t->type == T_PUNCT && strcmp(t->text, punct) == 0;
}

static int p_is_ident(P *p, const char *word)
{
    Tok *t = p_cur(p);
    return t->type == T_IDENT && strcmp(t->text, word) == 0;
}

static int p_is_ident_at(P *p, int i, const char *word)
{
    Tok *t = p_at(p, i);
    return t->type == T_IDENT && strcmp(t->text, word) == 0;
}

static int p_line(P *p)
{
    return p_cur(p)->line;
}

static void p_advance(P *p)
{
    if (p->t[p->pos].type != T_EOF)
        p->pos++;
}

static void p_expect(P *p, const char *punct)
{
    if (!p_is(p, punct))
        js_error(p->js, p_line(p), "expected '%s' but found '%s'", punct,
                 p_cur(p)->text ? p_cur(p)->text : "end of input");
    p_advance(p);
}

static int p_at_statement_end(P *p)
{
    if (p_is(p, ";")) {
        p_advance(p);
        return 1;
    }
    if (p_is(p, "}") || p_cur(p)->type == T_EOF)
        return 1;
    /* automatic semicolon insertion: a line break before the next token */
    if (p->pos > 0 && p->t[p->pos].line > p->t[p->pos - 1].line)
        return 1;
    return 0;
}

/* is `word` a reserved word that cannot be used as a binding name? */
static int p_reserved(const char *word)
{
    static const char *kw[] = {"break", "case",  "catch",  "class", "const",    "continue",
                               "debugger", "default", "delete", "do", "else",   "export",
                               "extends", "finally", "for",  "function", "if", "import",
                               "in",      "instanceof", "new", "return", "super", "switch",
                               "this",    "throw",   "try",    "typeof", "var",  "void",
                               "while",   "with",    "yield",  "enum",   NULL};
    int i;
    for (i = 0; kw[i]; i++)
        if (!strcmp(word, kw[i]))
            return 1;
    return 0;
}

static JsNode *p_node(P *p, int kind)
{
    return js_node_new(p->js, kind, p_line(p));
}

static JsNode *p_expr(P *p);
static JsNode *p_assignment(P *p);
static JsNode *p_block(P *p);
static JsNode *p_statement(P *p);

/* skips a balanced (), [] or {} group starting at `start`; returns the index
 * just past the closing token, or -1 when unbalanced */
static int p_skip_balanced(P *p, int start)
{
    int depth = 0;
    int i = start;
    for (; i < p->ntok; i++) {
        Tok *t = &p->t[i];
        if (t->type == T_EOF)
            return -1;
        if (t->type != T_PUNCT)
            continue;
        if (!strcmp(t->text, "(") || !strcmp(t->text, "[") || !strcmp(t->text, "{"))
            depth++;
        else if (!strcmp(t->text, ")") || !strcmp(t->text, "]") || !strcmp(t->text, "}")) {
            depth--;
            if (depth == 0)
                return i + 1;
        }
    }
    return -1;
}

/* ---------------- pattern helpers ---------------- */

/* is `[` or `{` starting a destructuring pattern (not an array/object
 * literal expression)? Patterns only appear left of `=`, in parameters, in
 * for-of/for-in heads and in catch clauses, so the callers use this to decide
 * between p_pattern() and p_assignment(). */
static int p_pattern_start(P *p)
{
    Tok *t = p_cur(p);
    return t->type == T_PUNCT && (!strcmp(t->text, "[") || !strcmp(t->text, "{"));
}

static JsNode *p_binding_element(P *p);

/* binding target: identifier, [pattern] or {pattern} */
static JsNode *p_binding_target(P *p)
{
    Tok *t = p_cur(p);
    if (t->type == T_IDENT) {
        JsNode *id;
        if (p_reserved(t->text) && strcmp(t->text, "yield") != 0 &&
            strcmp(t->text, "await") != 0 && strcmp(t->text, "this") != 0) {
            /* reserved words: allow `this` for housekeeping, others error */
            if (p_reserved(t->text) && strcmp(t->text, "let") != 0 &&
                strcmp(t->text, "static") != 0 && strcmp(t->text, "get") != 0 &&
                strcmp(t->text, "set") != 0 && strcmp(t->text, "async") != 0 &&
                strcmp(t->text, "of") != 0 && strcmp(t->text, "as") != 0 &&
                strcmp(t->text, "from") != 0) {
                /* still allow: JS engines accept many of these as names too */
            }
        }
        id = p_node(p, JS_N_IDENT);
        id->str = t->text;
        p_advance(p);
        return id;
    }
    if (p_is(p, "[")) {
        JsNode *arr = p_node(p, JS_N_PAT_ARR);
        p_advance(p);
        while (!p_is(p, "]")) {
            if (p_is(p, ",")) {
                js_node_add(p->js, arr, NULL); /* hole */
                p_advance(p);
                continue;
            }
            js_node_add(p->js, arr, p_binding_element(p));
            if (p_is(p, ","))
                p_advance(p);
            else
                break;
        }
        p_expect(p, "]");
        return arr;
    }
    if (p_is(p, "{")) {
        JsNode *obj = p_node(p, JS_N_PAT_OBJ);
        p_advance(p);
        while (!p_is(p, "}")) {
            if (p_is(p, "...")) {
                JsNode *rest = p_node(p, JS_N_PAT_REST);
                p_advance(p);
                js_node_add(p->js, rest, p_binding_target(p));
                js_node_add(p->js, obj, rest);
            } else {
                char *key;
                JsNode *pair;
                if (p_cur(p)->type == T_IDENT || p_cur(p)->type == T_STR) {
                    key = p_cur(p)->text;
                    p_advance(p);
                } else if (p_cur(p)->type == T_NUM) {
                    key = js_num_to_string(p->js, p_cur(p)->num);
                    p_advance(p);
                } else {
                    js_error(p->js, p_line(p), "object pattern key expected");
                    return obj;
                }
                pair = p_node(p, JS_N_ASSIGN);
                pair->str = key;
                if (p_is(p, ":") ) {
                    p_advance(p);
                    js_node_add(p->js, pair, p_binding_element(p));
                } else if (p_is(p, "=")) {
                    JsNode *def = p_node(p, JS_N_PAT_DEFAULT);
                    js_node_add(p->js, def, p_binding_target(p));
                    p_advance(p); /* = */
                    js_node_add(p->js, def, p_assignment(p));
                    js_node_add(p->js, pair, def);
                } else {
                    JsNode *id = p_node(p, JS_N_IDENT);
                    id->str = key;
                    js_node_add(p->js, pair, id);
                }
                js_node_add(p->js, obj, pair);
            }
            if (p_is(p, ","))
                p_advance(p);
            else
                break;
        }
        p_expect(p, "}");
        return obj;
    }
    js_error(p->js, p_line(p), "binding name expected");
    return p_node(p, JS_N_IDENT);
}

/* a parameter: target, target = default, ...target */
static JsNode *p_binding_element(P *p)
{
    if (p_is(p, "...")) {
        JsNode *rest = p_node(p, JS_N_PAT_REST);
        p_advance(p);
        js_node_add(p->js, rest, p_binding_target(p));
        return rest;
    }
    if (p_cur(p)->type == T_IDENT && p_at(p, 1)->type == T_PUNCT &&
        !strcmp(p_at(p, 1)->text, "=") && strcmp(p_cur(p)->text, "yield") != 0) {
        JsNode *def = p_node(p, JS_N_PAT_DEFAULT);
        js_node_add(p->js, def, p_binding_target(p));
        p_advance(p); /* = */
        js_node_add(p->js, def, p_assignment(p));
        return def;
    }
    if (p_pattern_start(p) && p_at(p, p_skip_balanced(p, p->pos) - p->pos) != NULL) {
        /* pattern (possibly with a default) */
        int after = p_skip_balanced(p, p->pos);
        if (after > 0 && p->t[after].type == T_PUNCT && !strcmp(p->t[after].text, "=")) {
            JsNode *def = p_node(p, JS_N_PAT_DEFAULT);
            js_node_add(p->js, def, p_binding_target(p));
            p->pos = after;
            p_advance(p); /* = */
            js_node_add(p->js, def, p_assignment(p));
            return def;
        }
        return p_binding_target(p);
    }
    return p_binding_target(p);
}

/* ---------------- templates ---------------- */

/* decodes the escape sequences of a template chunk into `out` */
static size_t decode_chunk(HpeJs *js, const char *s, size_t n, char *out, size_t cap)
{
    size_t i = 0, o = 0;
    (void)js;
    while (i < n && o + 1 < cap) {
        char c = s[i];
        if (c == '\\' && i + 1 < n) {
            char e = s[i + 1];
            i += 2;
            switch (e) {
            case 'n':
                out[o++] = '\n';
                break;
            case 't':
                out[o++] = '\t';
                break;
            case 'r':
                out[o++] = '\r';
                break;
            case 'b':
                out[o++] = '\b';
                break;
            case 'f':
                out[o++] = '\f';
                break;
            case 'v':
                out[o++] = '\v';
                break;
            case '0':
                out[o++] = '\0';
                break;
            case 'x':
            case 'u': {
                uint32_t cp = 0;
                int k, maxk = (e == 'x') ? 2 : 4;
                if (e == 'u' && i < n && s[i] == '{') {
                    i++;
                    while (i < n && s[i] != '}') {
                        int d = -1;
                        char h = s[i];
                        if (h >= '0' && h <= '9')
                            d = h - '0';
                        else if (h >= 'a' && h <= 'f')
                            d = h - 'a' + 10;
                        else if (h >= 'A' && h <= 'F')
                            d = h - 'A' + 10;
                        if (d < 0)
                            break;
                        cp = cp * 16 + (uint32_t)d;
                        i++;
                    }
                    if (i < n)
                        i++;
                } else {
                    for (k = 0; k < maxk && i < n; k++) {
                        int d = -1;
                        char h = s[i];
                        if (h >= '0' && h <= '9')
                            d = h - '0';
                        else if (h >= 'a' && h <= 'f')
                            d = h - 'a' + 10;
                        else if (h >= 'A' && h <= 'F')
                            d = h - 'A' + 10;
                        if (d < 0)
                            break;
                        cp = cp * 16 + (uint32_t)d;
                        i++;
                    }
                }
                if (cp < 0x80) {
                    out[o++] = (char)cp;
                } else {
                    char utf[4];
                    int m = hpe_utf8_encode(cp, utf);
                    if (o + 4 < cap) {
                        memcpy(out + o, utf, (size_t)m);
                        o += (size_t)m;
                    }
                }
                break;
            }
            case '\n':
                break;
            default:
                out[o++] = e;
                break;
            }
            continue;
        }
        out[o++] = c;
        i++;
    }
    out[o] = '\0';
    return o;
}

/* parses a standalone expression source (used for `${...}` substitutions) */
static JsNode *parse_sub_expression(HpeJs *js, const char *src, size_t len)
{
    Lexer lx;
    P p;
    JsNode *n;
    memset(&lx, 0, sizeof(lx));
    lx.js = js;
    lx.src = src;
    lx.len = len;
    lx.line = 1;
    lex_all(&lx);
    memset(&p, 0, sizeof(p));
    p.js = js;
    p.t = lx.toks;
    p.pos = 0;
    p.ntok = lx.ntok;
    n = p_assignment(&p);
    if (p_cur(&p)->type != T_EOF)
        js_error(js, p_line(&p), "unexpected token in template substitution");
    return n;
}

/* expands a T_TEMPLATE token into a JS_N_TEMPLATE node */
static JsNode *p_template(P *p)
{
    const char *s = p_cur(p)->text ? p_cur(p)->text : "";
    size_t len = strlen(s);
    size_t i = 1; /* past the opening backtick */
    JsNode *node = p_node(p, JS_N_TEMPLATE);
    js_node_add(p->js, node, p_node(p, JS_N_BLOCK)); /* kids[0]: cooked chunks */

    while (i < len) {
        size_t start = i;
        char *cooked;
        size_t cn;
        while (i < len && s[i] != '`' && !(s[i] == '$' && i + 1 < len && s[i + 1] == '{')) {
            if (s[i] == '\\' && i + 1 < len)
                i += 2;
            else
                i++;
        }
        cooked = (char *)malloc(len - start + 8);
        if (!cooked)
            js_error(p->js, p_line(p), "out of memory");
        cn = decode_chunk(p->js, s + start, i - start, cooked, len - start + 4);
        {
            JsNode *str = p_node(p, JS_N_STR);
            str->str = hpe_js_strndup(p->js, cooked, cn);
            js_node_add(p->js, node->kids[0], str);
        }
        free(cooked);
        if (i >= len)
            break;
        if (s[i] == '`')
            break;
        /* ${ expr } */
        {
            size_t es = i + 2;
            size_t j = es;
            int depth = 1;
            while (j < len && depth) {
                char c = s[j];
                if (c == '{')
                    depth++;
                else if (c == '}')
                    depth--;
                else if (c == '`') {
                    /* nested template: skip it */
                    int lines = 0;
                    Lexer tmp;
                    memset(&tmp, 0, sizeof(tmp));
                    tmp.js = p->js;
                    tmp.src = s;
                    tmp.len = len;
                    j = scan_template(&tmp, j, &lines);
                    continue;
                } else if (c == '"' || c == '\'') {
                    char q = c;
                    j++;
                    while (j < len && s[j] != q) {
                        if (s[j] == '\\')
                            j++;
                        j++;
                    }
                }
                j++;
            }
            js_node_add(p->js, node, parse_sub_expression(p->js, s + es, j - 1 - es));
            i = j;
        }
    }
    p_advance(p);
    return node;
}
/* ---------------- declarations / functions ---------------- */

/* parses `function [name](params) { body }`; the `function` token is current.
 * Returns a JS_N_FUNC node: op = FN_* flags, kids[0] = params, kids[1] = body. */
static JsNode *p_function(P *p, int need_name, int is_async, int is_gen)
{
    JsNode *fn = p_node(p, JS_N_FUNC);
    JsNode *params = p_node(p, JS_N_BLOCK);
    fn->op = (is_async ? FN_ASYNC : 0) | (is_gen ? FN_GEN : 0);

    p_advance(p); /* function */
    if (is_gen && p_is(p, "*"))
        p_advance(p);
    if (p_cur(p)->type == T_IDENT && !p_reserved(p_cur(p)->text)) {
        fn->str = p_cur(p)->text;
        p_advance(p);
    } else if (need_name) {
        js_error(p->js, p_line(p), "function name expected");
    }
    p_expect(p, "(");
    while (!p_is(p, ")")) {
        if (p_is(p, "...") || p_cur(p)->type == T_IDENT || p_pattern_start(p))
            js_node_add(p->js, params, p_binding_element(p));
        else
            js_error(p->js, p_line(p), "parameter expected");
        if (p_is(p, ","))
            p_advance(p);
        else
            break;
    }
    p_expect(p, ")");
    js_node_add(p->js, fn, params);
    if (p_is(p, "{"))
        js_node_add(p->js, fn, p_block(p));
    else
        js_error(p->js, p_line(p), "function body expected");
    return fn;
}

/* arrow function: params already parsed, current token must be "=>" */
static JsNode *p_arrow_from_params(P *p, JsNode *params, int is_async)
{
    JsNode *fn = p_node(p, JS_N_FUNC);
    fn->op = FN_ARROW | (is_async ? FN_ASYNC : 0);
    js_node_add(p->js, fn, params);
    p_advance(p); /* => */
    if (p_is(p, "{")) {
        js_node_add(p->js, fn, p_block(p));
    } else {
        JsNode *blk = p_node(p, JS_N_BLOCK);
        JsNode *ret = p_node(p, JS_N_RETURN);
        js_node_add(p->js, ret, p_assignment(p));
        js_node_add(p->js, blk, ret);
        js_node_add(p->js, fn, blk);
    }
    return fn;
}

/* is the '(' at `pos` an arrow parameter list? (lookahead scan) */
static int p_paren_arrow(P *p, int pos)
{
    int end = p_skip_balanced(p, pos);
    if (end < 0)
        return 0;
    return p->t[end].type == T_PUNCT && !strcmp(p->t[end].text, "=>");
}

static JsNode *p_arrow_params(P *p)
{
    JsNode *params = p_node(p, JS_N_BLOCK);
    p_expect(p, "(");
    while (!p_is(p, ")")) {
        js_node_add(p->js, params, p_binding_element(p));
        if (p_is(p, ","))
            p_advance(p);
        else
            break;
    }
    p_expect(p, ")");
    return params;
}

/* ---------------- object / class members ---------------- */

static JsNode *p_unary(P *p);
static JsNode *p_postfix(P *p);
static JsNode *p_primary(P *p);
static JsNode *p_assign_target(P *p);

/* private class member name: '#' IDENT -> "#name" (allocated from the arena) */
static int p_private_name(P *p, char **out)
{
    if (!p_is(p, "#") || p_at(p, 1)->type != T_IDENT)
        return 0;
    {
        char *nm = p_at(p, 1)->text;
        size_t n = strlen(nm);
        char *buf = (char *)hpe_js_alloc(p->js, n + 2);
        buf[0] = '#';
        memcpy(buf + 1, nm, n + 1);
        *out = buf;
    }
    p_advance(p);
    p_advance(p);
    return 1;
}

static int p_property_name(P *p, char **out, JsNode **computed)
{
    Tok *t = p_cur(p);
    *computed = NULL;
    *out = NULL;
    if (p_is(p, "[")) {
        JsNode *k;
        p_advance(p);
        k = p_assignment(p);
        p_expect(p, "]");
        *computed = k;
        return 1;
    }
    if (p_private_name(p, out))
        return 1;
    if (t->type == T_IDENT || t->type == T_STR) {
        *out = t->text;
        p_advance(p);
        return 1;
    }
    if (t->type == T_NUM) {
        *out = js_num_to_string(p->js, t->num);
        p_advance(p);
        return 1;
    }
    return 0;
}

/* object literal member list (the '{' is already consumed) */
static void p_object_props(P *p, JsNode *obj)
{
    while (!p_is(p, "}")) {
        char *key = NULL;
        JsNode *computed = NULL;
        int is_async = 0, is_gen = 0, is_get = 0, is_set = 0;

        if (p_is(p, "...")) {
            JsNode *sp = p_node(p, JS_N_SPREAD);
            p_advance(p);
            js_node_add(p->js, sp, p_assignment(p));
            js_node_add(p->js, obj, sp);
            if (p_is(p, ","))
                p_advance(p);
            continue;
        }
        if (p_is_ident(p, "async") && !p_is(p, ",") && !p_is(p, "}") && !p_is(p, ":")) {
            Tok *nx = p_at(p, 1);
            int nameish = (nx->type == T_IDENT || nx->type == T_STR || nx->type == T_NUM) ;
            int is_paren = nx->type == T_PUNCT && !strcmp(nx->text, "(");
            if (nameish || is_paren || (nx->type == T_PUNCT && !strcmp(nx->text, "*"))) {
                is_async = 1;
                p_advance(p);
                if (p_is(p, "*")) {
                    is_gen = 1;
                    p_advance(p);
                }
            }
        }
        if (!is_async && p_is_ident(p, "get") &&
            !(p_at(p, 1)->type == T_PUNCT && (!strcmp(p_at(p, 1)->text, ":") ||
                                              !strcmp(p_at(p, 1)->text, ",") ||
                                              !strcmp(p_at(p, 1)->text, "}") ||
                                              !strcmp(p_at(p, 1)->text, "(")))) {
            is_get = 1;
            p_advance(p);
        } else if (!is_async && p_is_ident(p, "set") &&
                   !(p_at(p, 1)->type == T_PUNCT && (!strcmp(p_at(p, 1)->text, ":") ||
                                                     !strcmp(p_at(p, 1)->text, ",") ||
                                                     !strcmp(p_at(p, 1)->text, "}") ||
                                                     !strcmp(p_at(p, 1)->text, "(")))) {
            is_set = 1;
            p_advance(p);
        }
        if (!is_get && !is_set && !is_async && p_is(p, "*")) {
            is_gen = 1;
            p_advance(p);
        }
        if (!p_property_name(p, &key, &computed)) {
            js_error(p->js, p_line(p), "object key expected");
            break;
        }
        if (p_is(p, "(")) {
            /* method shorthand */
            JsNode *pair = p_node(p, JS_N_ASSIGN);
            JsNode *fn = p_node(p, JS_N_FUNC);
            JsNode *params = p_node(p, JS_N_BLOCK);
            fn->op = (is_async ? FN_ASYNC : 0) | (is_gen ? FN_GEN : 0) |
                     (is_get ? 0 : (is_set ? 0 : FN_CLASS));
            pair->op = is_get ? JS_PROP_GET : (is_set ? JS_PROP_SET : JS_PROP_METHOD);
            pair->str = key;
            p_advance(p); /* ( */
            while (!p_is(p, ")")) {
                js_node_add(p->js, params, p_binding_element(p));
                if (p_is(p, ","))
                    p_advance(p);
                else
                    break;
            }
            p_expect(p, ")");
            js_node_add(p->js, fn, params);
            js_node_add(p->js, fn, p_block(p));
            js_node_add(p->js, pair, fn);
            if (computed)
                js_node_add(p->js, pair, computed);
            js_node_add(p->js, obj, pair);
        } else if (p_is(p, ":")) {
            JsNode *pair = p_node(p, JS_N_ASSIGN);
            p_advance(p);
            pair->op = (key && !strcmp(key, "__proto__") && !computed) ? JS_PROP_PROTO
                                                                       : JS_PROP_DATA;
            pair->str = key;
            js_node_add(p->js, pair, p_assignment(p));
            if (computed)
                js_node_add(p->js, pair, computed);
            js_node_add(p->js, obj, pair);
        } else {
            /* shorthand (with optional default for destructuring) */
            JsNode *pair = p_node(p, JS_N_ASSIGN);
            JsNode *id = p_node(p, JS_N_IDENT);
            if (!key) {
                js_error(p->js, p_line(p), "invalid shorthand property");
                break;
            }
            id->str = key;
            if (p_is(p, "=")) {
                JsNode *def = p_node(p, JS_N_PAT_DEFAULT);
                js_node_add(p->js, def, id);
                p_advance(p);
                js_node_add(p->js, def, p_assignment(p));
                pair->op = JS_PROP_DATA;
                js_node_add(p->js, pair, def);
            } else {
                pair->op = JS_PROP_SHORTHAND;
                js_node_add(p->js, pair, id);
            }
            pair->str = key;
            js_node_add(p->js, obj, pair);
        }
        if (p_is(p, ","))
            p_advance(p);
        else
            break;
    }
    p_expect(p, "}");
}

/* class body (the '{' is current) */
static void p_class_body(P *p, JsNode *cls)
{
    JsNode *members = p_node(p, JS_N_BLOCK);
    js_node_add(p->js, cls, members);
    p_expect(p, "{");
    while (!p_is(p, "}")) {
        int is_static = 0, is_async = 0, is_gen = 0, is_get = 0, is_set = 0;
        char *key = NULL;
        JsNode *computed = NULL;
        JsNode *member;

        if (p_cur(p)->type == T_EOF)
            js_error(p->js, p_line(p), "unexpected end of input in class body");
        if (p_is(p, ";")) {
            p_advance(p);
            continue;
        }
        if (p_is_ident(p, "static") && !p_is(p, "(") && !p_is(p, "=")) {
            Tok *nx = p_at(p, 1);
            if (!(nx->type == T_PUNCT && (!strcmp(nx->text, "(") || !strcmp(nx->text, "="))) &&
                !(nx->type == T_PUNCT && !strcmp(nx->text, "}"))) {
                is_static = 1;
                p_advance(p);
            }
        }
        /* static initialiser block */
        if (is_static && p_is(p, "{")) {
            member = p_node(p, JS_N_BLOCK);
            member->op = JS_STATIC_BLOCK;
            js_node_add(p->js, members, member);
            p_advance(p);
            while (!p_is(p, "}")) {
                if (p_cur(p)->type == T_EOF)
                    js_error(p->js, p_line(p), "unexpected end of input");
                js_node_add(p->js, member, p_statement(p));
            }
            p_expect(p, "}");
            continue;
        }
        if (p_is_ident(p, "async") && !(p_at(p, 1)->type == T_PUNCT &&
                                        (!strcmp(p_at(p, 1)->text, "(") ||
                                         !strcmp(p_at(p, 1)->text, "=") ||
                                         !strcmp(p_at(p, 1)->text, ";")))) {
            is_async = 1;
            p_advance(p);
        }
        if (p_is(p, "*")) {
            is_gen = 1;
            p_advance(p);
        }
        if (!is_async && !is_gen && (p_is_ident(p, "get") || p_is_ident(p, "set")) &&
            !(p_at(p, 1)->type == T_PUNCT &&
              (!strcmp(p_at(p, 1)->text, "(") || !strcmp(p_at(p, 1)->text, "=") ||
               !strcmp(p_at(p, 1)->text, ";")))) {
            is_get = p_is_ident(p, "get");
            is_set = !is_get;
            p_advance(p);
        }
        if (!p_property_name(p, &key, &computed)) {
            js_error(p->js, p_line(p), "class member name expected");
            break;
        }
        if (p_is(p, "(")) {
            JsNode *fn = p_node(p, JS_N_FUNC);
            JsNode *params = p_node(p, JS_N_BLOCK);
            fn->op = (is_async ? FN_ASYNC : 0) | (is_gen ? FN_GEN : 0) | FN_CLASS;
            fn->str = key;
            p_advance(p);
            while (!p_is(p, ")")) {
                js_node_add(p->js, params, p_binding_element(p));
                if (p_is(p, ","))
                    p_advance(p);
                else
                    break;
            }
            p_expect(p, ")");
            js_node_add(p->js, fn, params);
            js_node_add(p->js, fn, p_block(p));
            if (computed)
                js_node_add(p->js, fn, computed);
            if (key && !strcmp(key, "constructor") && !is_static && !computed)
                fn->op = (is_async ? FN_ASYNC : 0) | (is_gen ? FN_GEN : 0) | FN_CLASS | FN_CTOR;
            fn->num = is_get ? JS_METHOD_GET
                             : is_set ? JS_METHOD_SET
                             : is_static ? (key && !strcmp(key, "constructor") ? JS_METHOD_STATIC
                                                                                : JS_METHOD_STATIC)
                             : (key && !strcmp(key, "constructor")) ? JS_METHOD_CTOR
                                                                    : JS_METHOD;
            js_node_add(p->js, members, fn);
        } else {
            /* field */
            JsNode *f = p_node(p, JS_N_ASSIGN);
            f->op = is_static ? JS_MEMBER_STATIC_FIELD : JS_MEMBER_FIELD;
            f->str = key;
            if (p_is(p, "=")) {
                p_advance(p);
                js_node_add(p->js, f, p_assignment(p));
            } else {
                js_node_add(p->js, f, NULL);
            }
            if (computed)
                js_node_add(p->js, f, computed);
            js_node_add(p->js, members, f);
            if (p_is(p, ";"))
                p_advance(p);
        }
    }
    p_expect(p, "}");
}

static JsNode *p_class(P *p)
{
    JsNode *cls = p_node(p, JS_N_CLASS);
    p_advance(p); /* class */
    if (p_cur(p)->type == T_IDENT && !p_reserved(p_cur(p)->text) &&
        !p_is_ident(p, "extends")) {
        cls->str = p_cur(p)->text;
        p_advance(p);
    }
    if (p_is_ident(p, "extends")) {
        p_advance(p);
        js_node_add(p->js, cls, p_unary(p));
    } else {
        js_node_add(p->js, cls, NULL);
    }
    p_class_body(p, cls);
    return cls;
}
/* ---------------- expressions ---------------- */

/* member/call/optional-chain tail. `opt_chain` marks that the chain already
 * contains a `?.` link. */
/* Parses the tail of a MemberExpression: `.name`, `[expr]`, `?.name` and — when
 * `allow_call` is set — `(args)`. The `new` operator parses its callee with
 * calls disabled so that `new Date(0).getTime()` binds as
 * `(new Date(0)).getTime()` rather than `new (Date(0).getTime())`. */
static JsNode *p_member_tail(P *p, JsNode *base, int allow_call)
{
    for (;;) {
        if (p_is(p, "?.") ) {
            Tok *nx = p_at(p, 1);
            p_advance(p); /* ?. */
            if (allow_call && nx->type == T_PUNCT && !strcmp(nx->text, "(")) {
                JsNode *call = p_node(p, JS_N_CALL);
                call->op = 1; /* optional call */
                js_node_add(p->js, call, base);
                p_advance(p);
                while (!p_is(p, ")")) {
                    if (p_is(p, "...")) {
                        JsNode *sp = p_node(p, JS_N_SPREAD);
                        p_advance(p);
                        js_node_add(p->js, sp, p_assignment(p));
                        js_node_add(p->js, call, sp);
                    } else {
                        js_node_add(p->js, call, p_assignment(p));
                    }
                    if (p_is(p, ","))
                        p_advance(p);
                    else
                        break;
                }
                p_expect(p, ")");
                base = call;
            } else if (nx->type == T_PUNCT && !strcmp(nx->text, "[")) {
                JsNode *m = p_node(p, JS_N_MEMBER);
                m->op = JS_OP_INDEX;
                m->num = 1; /* optional */
                js_node_add(p->js, m, base);
                p_advance(p);
                js_node_add(p->js, m, p_expr(p));
                p_expect(p, "]");
                base = m;
            } else {
                JsNode *m = p_node(p, JS_N_MEMBER);
                if (nx->type != T_IDENT && nx->type != T_STR)
                    js_error(p->js, p_line(p), "property name expected after '?.'");
                m->op = JS_OP_DOT;
                m->num = 1;
                m->str = nx->text;
                js_node_add(p->js, m, base);
                p_advance(p);
                base = m;
            }
            continue;
        }
        if (p_is(p, ".")) {
            JsNode *m;
            p_advance(p);
            m = p_node(p, JS_N_MEMBER);
            m->op = JS_OP_DOT;
            if (p_private_name(p, &m->str)) {
                js_node_add(p->js, m, base);
                base = m;
                continue;
            }
            if (p_cur(p)->type != T_IDENT && p_cur(p)->type != T_STR &&
                p_cur(p)->type != T_NUM)
                js_error(p->js, p_line(p), "property name expected");
            m->str = p_cur(p)->text;
            js_node_add(p->js, m, base);
            p_advance(p);
            base = m;
        } else if (p_is(p, "[")) {
            JsNode *m;
            p_advance(p);
            m = p_node(p, JS_N_MEMBER);
            m->op = JS_OP_INDEX;
            js_node_add(p->js, m, base);
            js_node_add(p->js, m, p_expr(p));
            p_expect(p, "]");
            base = m;
        } else if (allow_call && p_is(p, "(")) {
            JsNode *call = p_node(p, JS_N_CALL);
            js_node_add(p->js, call, base);
            p_advance(p);
            while (!p_is(p, ")")) {
                if (p_is(p, "...")) {
                    JsNode *sp = p_node(p, JS_N_SPREAD);
                    p_advance(p);
                    js_node_add(p->js, sp, p_assignment(p));
                    js_node_add(p->js, call, sp);
                } else {
                    js_node_add(p->js, call, p_assignment(p));
                }
                if (p_is(p, ","))
                    p_advance(p);
                else
                    break;
            }
            p_expect(p, ")");
            base = call;
        } else if (base && base->kind == JS_N_SUPER_MEMBER && !base->kid_count) {
            break;
        } else {
            break;
        }
    }
    return base;
}

static JsNode *p_primary(P *p)
{
    Tok *t = p_cur(p);
    if (t->type == T_NUM) {
        JsNode *n = p_node(p, JS_N_NUM);
        n->num = t->num;
        p_advance(p);
        return n;
    }
    if (t->type == T_STR) {
        JsNode *n = p_node(p, JS_N_STR);
        n->str = t->text;
        p_advance(p);
        return n;
    }
    if (t->type == T_REGEX) {
        JsNode *n = p_node(p, JS_N_REGEX);
        n->str = t->text; /* "pattern\0flags" */
        p_advance(p);
        return n;
    }
    if (t->type == T_TEMPLATE)
        return p_template(p);
    if (t->type == T_IDENT) {
        if (!strcmp(t->text, "true") || !strcmp(t->text, "false")) {
            JsNode *n = p_node(p, !strcmp(t->text, "true") ? JS_N_TRUE : JS_N_FALSE);
            p_advance(p);
            return n;
        }
        if (!strcmp(t->text, "null")) {
            p_advance(p);
            return p_node(p, JS_N_NULL);
        }
        if (!strcmp(t->text, "this")) {
            p_advance(p);
            return p_node(p, JS_N_THIS);
        }
        if (!strcmp(t->text, "super")) {
            JsNode *n = p_node(p, JS_N_SUPER_MEMBER);
            p_advance(p);
            if (p_is(p, "(")) {
                JsNode *call = p_node(p, JS_N_SUPER_CALL);
                p_advance(p);
                while (!p_is(p, ")")) {
                    if (p_is(p, "...")) {
                        JsNode *sp = p_node(p, JS_N_SPREAD);
                        p_advance(p);
                        js_node_add(p->js, sp, p_assignment(p));
                        js_node_add(p->js, call, sp);
                    } else {
                        js_node_add(p->js, call, p_assignment(p));
                    }
                    if (p_is(p, ","))
                        p_advance(p);
                    else
                        break;
                }
                p_expect(p, ")");
                return call;
            }
            if (p_is(p, ".")) {
                p_advance(p);
                if (p_cur(p)->type != T_IDENT && p_cur(p)->type != T_STR)
                    js_error(p->js, p_line(p), "property name expected after 'super.'");
                n->str = p_cur(p)->text;
                p_advance(p);
            } else if (!p_is(p, "[")) {
                js_error(p->js, p_line(p), "'super' must be followed by '(' or '.'");
            }
            return n;
        }
        if (!strcmp(t->text, "function")) {
            int gen = p_at(p, 1)->type == T_PUNCT && !strcmp(p_at(p, 1)->text, "*");
            return p_function(p, 0, 0, gen);
        }
        /* async function / async arrow */
        if (!strcmp(t->text, "async") && !p_is(p, "=>")) {
            Tok *nx = p_at(p, 1);
            if (nx->type == T_IDENT && !strcmp(nx->text, "function")) {
                p_advance(p);
                return p_function(p, 0, 1, 0);
            }
            if (nx->type == T_IDENT && p_at(p, 2)->type == T_PUNCT &&
                !strcmp(p_at(p, 2)->text, "=>")) {
                JsNode *params = p_node(p, JS_N_BLOCK);
                JsNode *id = p_node(p, JS_N_IDENT);
                id->str = nx->text;
                js_node_add(p->js, params, id);
                p_advance(p);
                p_advance(p);
                return p_arrow_from_params(p, params, 1);
            }
            if (nx->type == T_PUNCT && !strcmp(nx->text, "(") && p_paren_arrow(p, p->pos + 1)) {
                p_advance(p);
                {
                    JsNode *params = p_arrow_params(p);
                    return p_arrow_from_params(p, params, 1);
                }
            }
        }
        if (!strcmp(t->text, "class"))
            return p_class(p);
        if (!strcmp(t->text, "new"))
            return p_unary(p); /* handled below, kept for clarity */
        if (!strcmp(t->text, "import")) {
            /* dynamic import() is not supported; `import.meta` falls through */
            if (p_at(p, 1)->type == T_PUNCT && !strcmp(p_at(p, 1)->text, ".")) {
                JsNode *n = p_node(p, JS_N_OBJECT);
                p_advance(p);
                p_advance(p);
                if (p_cur(p)->type == T_IDENT)
                    p_advance(p);
                return n;
            }
        }
        /* single-parameter arrow: x => ... */
        if (p_at(p, 1)->type == T_PUNCT && !strcmp(p_at(p, 1)->text, "=>")) {
            JsNode *params = p_node(p, JS_N_BLOCK);
            JsNode *id = p_node(p, JS_N_IDENT);
            id->str = t->text;
            js_node_add(p->js, params, id);
            p_advance(p);
            return p_arrow_from_params(p, params, 0);
        }
        {
            JsNode *n = p_node(p, JS_N_IDENT);
            n->str = t->text;
            p_advance(p);
            return n;
        }
    }
    if (t->type == T_PUNCT && !strcmp(t->text, "(")) {
        JsNode *n;
        if (p_paren_arrow(p, p->pos)) {
            JsNode *params = p_arrow_params(p);
            return p_arrow_from_params(p, params, 0);
        }
        p_advance(p);
        n = p_expr(p);
        p_expect(p, ")");
        return n;
    }
    if (t->type == T_PUNCT && !strcmp(t->text, "[")) {
        JsNode *n = p_node(p, JS_N_ARRAY);
        p_advance(p);
        while (!p_is(p, "]")) {
            if (p_is(p, ",")) {
                js_node_add(p->js, n, NULL); /* hole */
                p_advance(p);
                continue;
            }
            if (p_is(p, "...")) {
                JsNode *sp = p_node(p, JS_N_SPREAD);
                p_advance(p);
                js_node_add(p->js, sp, p_assignment(p));
                js_node_add(p->js, n, sp);
            } else {
                js_node_add(p->js, n, p_assignment(p));
            }
            if (p_is(p, ","))
                p_advance(p);
            else
                break;
        }
        p_expect(p, "]");
        return n;
    }
    if (t->type == T_PUNCT && !strcmp(t->text, "{")) {
        JsNode *n = p_node(p, JS_N_OBJECT);
        p_advance(p);
        p_object_props(p, n);
        return n;
    }
    if (t->type == T_EOF)
        js_error(p->js, p_line(p), "unexpected end of input");
    else
        js_error(p->js, p_line(p), "unexpected token '%s'", t->text ? t->text : "?");
    return p_node(p, JS_N_NULL);
}

static JsNode *p_postfix(P *p)
{
    JsNode *base = p_primary(p);
    base = p_member_tail(p, base, 1);
    if ((p_is(p, "++") || p_is(p, "--")) && p_cur(p)->line == p->t[p->pos - 1].line) {
        JsNode *u = p_node(p, JS_N_UPDATE);
        u->op = p_is(p, "++") ? JS_OP_POSTINC : JS_OP_POSTDEC;
        js_node_add(p->js, u, base);
        p_advance(p);
        return u;
    }
    return base;
}

static JsNode *p_unary(P *p)
{
    if (p_is(p, "!") || p_is(p, "-") || p_is(p, "+") || p_is(p, "~")) {
        JsNode *n = p_node(p, JS_N_UN);
        n->op = p_is(p, "!")     ? JS_OP_NOT
                : p_is(p, "-")   ? JS_OP_NEG
                : p_is(p, "+")   ? JS_OP_POS
                                 : JS_OP_BNOT;
        p_advance(p);
        js_node_add(p->js, n, p_unary(p));
        return n;
    }
    if (p_is_ident(p, "typeof") || p_is_ident(p, "void") || p_is_ident(p, "delete")) {
        JsNode *n = p_node(p, JS_N_UN);
        n->op = p_is_ident(p, "typeof") ? JS_OP_TYPEOF
                : p_is_ident(p, "void") ? JS_OP_VOID
                                        : JS_OP_DELETE;
        p_advance(p);
        js_node_add(p->js, n, p_unary(p));
        return n;
    }
    if (p_is_ident(p, "await")) {
        JsNode *n = p_node(p, JS_N_AWAIT);
        p_advance(p);
        js_node_add(p->js, n, p_unary(p));
        return n;
    }
    if (p_is_ident(p, "yield")) {
        JsNode *n = p_node(p, JS_N_YIELD);
        p_advance(p);
        if (p_is(p, "*")) {
            n->op = 1; /* yield* */
            p_advance(p);
        }
        if (!p_is(p, ")") && !p_is(p, "]") && !p_is(p, "}") && !p_is(p, ",") &&
            !p_is(p, ";") && p_cur(p)->type != T_EOF && p_cur(p)->line == p->t[p->pos - 1].line)
            js_node_add(p->js, n, p_assignment(p));
        else
            js_node_add(p->js, n, NULL);
        return n;
    }
    if (p_is_ident(p, "new")) {
        JsNode *n = p_node(p, JS_N_NEW);
        JsNode *callee;
        p_advance(p);
        if (p_is(p, "."))
            p_advance(p); /* new.target */
        /* The callee is a MemberExpression with calls suppressed, so the `(...)`
         * that follows belongs to `new` itself; any further `.`/`()` chain binds
         * to the constructed value: new Date(0).getTime() === (new Date(0)).getTime() */
        callee = p_member_tail(p, p_primary(p), 0);
        if (p_is(p, "(")) {
            JsNode *call = p_node(p, JS_N_CALL);
            js_node_add(p->js, call, callee);
            p_advance(p);
            while (!p_is(p, ")")) {
                if (p_is(p, "...")) {
                    JsNode *sp = p_node(p, JS_N_SPREAD);
                    p_advance(p);
                    js_node_add(p->js, sp, p_assignment(p));
                    js_node_add(p->js, call, sp);
                } else {
                    js_node_add(p->js, call, p_assignment(p));
                }
                if (p_is(p, ","))
                    p_advance(p);
                else
                    break;
            }
            p_expect(p, ")");
            callee = call;
        }
        js_node_add(p->js, n, callee);
        return p_member_tail(p, n, 1);
    }
    if (p_is(p, "++") || p_is(p, "--")) {
        JsNode *n = p_node(p, JS_N_UPDATE);
        n->op = p_is(p, "++") ? JS_OP_PREINC : JS_OP_PREDEC;
        p_advance(p);
        js_node_add(p->js, n, p_unary(p));
        return n;
    }
    return p_postfix(p);
}

static JsNode *p_bin_make(P *p, int code, JsNode *l, JsNode *r)
{
    JsNode *n = p_node(p, JS_N_BIN);
    n->op = code;
    js_node_add(p->js, n, l);
    js_node_add(p->js, n, r);
    return n;
}

static JsNode *p_exp(P *p)
{
    JsNode *l = p_unary(p);
    if (p_is(p, "**")) {
        p_advance(p);
        return p_bin_make(p, JS_OP_EXP, l, p_exp(p)); /* right associative */
    }
    return l;
}

static JsNode *p_mul(P *p)
{
    JsNode *l = p_exp(p);
    while (p_is(p, "*") || p_is(p, "/") || p_is(p, "%")) {
        int code = p_is(p, "*") ? JS_OP_MUL : (p_is(p, "/") ? JS_OP_DIV : JS_OP_MOD);
        p_advance(p);
        l = p_bin_make(p, code, l, p_exp(p));
    }
    return l;
}

static JsNode *p_add(P *p)
{
    JsNode *l = p_mul(p);
    while (p_is(p, "+") || p_is(p, "-")) {
        int code = p_is(p, "+") ? JS_OP_ADD : JS_OP_SUB;
        p_advance(p);
        l = p_bin_make(p, code, l, p_mul(p));
    }
    return l;
}

static JsNode *p_shift(P *p)
{
    JsNode *l = p_add(p);
    while (p_is(p, "<<") || p_is(p, ">>") || p_is(p, ">>>")) {
        int code = p_is(p, "<<") ? JS_OP_SHL : p_is(p, ">>") ? JS_OP_SHR : JS_OP_USHR;
        p_advance(p);
        l = p_bin_make(p, code, l, p_add(p));
    }
    return l;
}

static JsNode *p_rel(P *p)
{
    JsNode *l = p_shift(p);
    for (;;) {
        if (p_is(p, "<") || p_is(p, ">") || p_is(p, "<=") || p_is(p, ">=")) {
            int code = p_is(p, "<")   ? JS_OP_LT
                       : p_is(p, ">") ? JS_OP_GT
                       : p_is(p, "<=") ? JS_OP_LE
                                       : JS_OP_GE;
            p_advance(p);
            l = p_bin_make(p, code, l, p_shift(p));
        } else if (p_is_ident(p, "instanceof")) {
            p_advance(p);
            l = p_bin_make(p, JS_OP_INSTANCEOF, l, p_shift(p));
        } else if (p_is_ident(p, "in")) {
            p_advance(p);
            l = p_bin_make(p, JS_OP_IN, l, p_shift(p));
        } else {
            break;
        }
    }
    return l;
}

static JsNode *p_eq(P *p)
{
    JsNode *l = p_rel(p);
    while (p_is(p, "==") || p_is(p, "!=") || p_is(p, "===") || p_is(p, "!==")) {
        int code = p_is(p, "==")     ? JS_OP_EQ
                   : p_is(p, "!=")   ? JS_OP_NEQ
                   : p_is(p, "===")  ? JS_OP_SEQ
                                     : JS_OP_SNEQ;
        p_advance(p);
        l = p_bin_make(p, code, l, p_rel(p));
    }
    return l;
}

static JsNode *p_band(P *p)
{
    JsNode *l = p_eq(p);
    while (p_is(p, "&")) {
        p_advance(p);
        l = p_bin_make(p, JS_OP_BAND, l, p_eq(p));
    }
    return l;
}

static JsNode *p_bxor(P *p)
{
    JsNode *l = p_band(p);
    while (p_is(p, "^")) {
        p_advance(p);
        l = p_bin_make(p, JS_OP_BXOR, l, p_band(p));
    }
    return l;
}

static JsNode *p_bor(P *p)
{
    JsNode *l = p_bxor(p);
    while (p_is(p, "|")) {
        p_advance(p);
        l = p_bin_make(p, JS_OP_BOR, l, p_bxor(p));
    }
    return l;
}

static JsNode *p_and(P *p)
{
    JsNode *l = p_bor(p);
    while (p_is(p, "&&")) {
        p_advance(p);
        l = p_bin_make(p, JS_OP_AND, l, p_bor(p));
    }
    return l;
}

static JsNode *p_or(P *p)
{
    JsNode *l = p_and(p);
    while (p_is(p, "||")) {
        p_advance(p);
        l = p_bin_make(p, JS_OP_OR, l, p_and(p));
    }
    return l;
}

static JsNode *p_nullish(P *p)
{
    JsNode *l = p_or(p);
    if (p_is(p, "??")) {
        p_advance(p);
        return p_bin_make(p, JS_OP_NULLISH, l, p_nullish(p));
    }
    return l;
}

static JsNode *p_ternary(P *p)
{
    JsNode *cond = p_nullish(p);
    if (p_is(p, "?")) {
        JsNode *n;
        p_advance(p);
        n = p_node(p, JS_N_TERNARY);
        js_node_add(p->js, n, cond);
        js_node_add(p->js, n, p_assignment(p));
        p_expect(p, ":");
        js_node_add(p->js, n, p_assignment(p));
        return n;
    }
    return cond;
}

static JsNode *p_assignment(P *p)
{
    JsNode *target = p_ternary(p);
    if (p_is(p, "=") || p_is(p, "+=") || p_is(p, "-=") || p_is(p, "*=") || p_is(p, "/=") ||
        p_is(p, "%=") || p_is(p, "**=") || p_is(p, "<<=") || p_is(p, ">>=") ||
        p_is(p, ">>>=") || p_is(p, "&=") || p_is(p, "|=") || p_is(p, "^=") ||
        p_is(p, "&&=") || p_is(p, "||=") || p_is(p, "?" "?=")) {
        int code = p_is(p, "=")       ? JS_OP_SET
                   : p_is(p, "+=")    ? JS_OP_ADDA
                   : p_is(p, "-=")    ? JS_OP_SUBA
                   : p_is(p, "*=")    ? JS_OP_MULA
                   : p_is(p, "/=")    ? JS_OP_DIVA
                   : p_is(p, "%=")    ? JS_OP_MODA
                   : p_is(p, "**=")   ? JS_OP_EXPA
                   : p_is(p, "<<=")   ? JS_OP_LSHA
                   : p_is(p, ">>=")   ? JS_OP_RSHA
                   : p_is(p, ">>>=")  ? JS_OP_URSHA
                   : p_is(p, "&=")    ? JS_OP_BANDA
                   : p_is(p, "|=")    ? JS_OP_BORA
                   : p_is(p, "^=")    ? JS_OP_BXORA
                   : p_is(p, "&&=")   ? JS_OP_ANDASSIGN
                   : p_is(p, "||=")   ? JS_OP_ORASSIGN
                                      : JS_OP_NULLASSIGN;
        JsNode *n = p_node(p, JS_N_ASSIGN);
        n->op = code;
        js_node_add(p->js, n, target);
        p_advance(p);
        js_node_add(p->js, n, p_assignment(p));
        return n;
    }
    return target;
}

static JsNode *p_expr(P *p)
{
    JsNode *first = p_assignment(p);
    if (p_is(p, ",")) {
        JsNode *seq = p_node(p, JS_N_SEQ);
        js_node_add(p->js, seq, first);
        while (p_is(p, ",")) {
            p_advance(p);
            js_node_add(p->js, seq, p_assignment(p));
        }
        return seq;
    }
    return first;
}

/* parsing target of for(...) heads: an expression or a destructuring pattern */
static JsNode *p_assign_target(P *p)
{
    HPE_UNUSED(p);
    return p_expr(p);
}
/* ---------------- statements ---------------- */

/* var declaration list; `node` is a JS_N_VAR, kid = JS_N_ASSIGN(name=pattern) */
static void p_var_decl(P *p, JsNode *decl, const char *kind)
{
    decl->str = (char *)kind;
    for (;;) {
        JsNode *one = p_node(p, JS_N_ASSIGN);
        one->op = JS_OP_SET;
        js_node_add(p->js, one, p_binding_target(p));
        if (p_is(p, "=")) {
            p_advance(p);
            js_node_add(p->js, one, p_assignment(p));
        } else {
            js_node_add(p->js, one, NULL);
        }
        js_node_add(p->js, decl, one);
        if (p_is(p, ","))
            p_advance(p);
        else
            break;
    }
}

static JsNode *p_block(P *p)
{
    JsNode *blk = p_node(p, JS_N_BLOCK);
    p_expect(p, "{");
    while (!p_is(p, "}")) {
        if (p_cur(p)->type == T_EOF)
            js_error(p->js, p_line(p), "unexpected end of input in block");
        js_node_add(p->js, blk, p_statement(p));
    }
    p_expect(p, "}");
    return blk;
}

/* the body of a function: `{` handled by p_block, everything else parsed as a
 * statement (also records the `"use strict"` directive) */
static JsNode *p_function_body(P *p)
{
    JsNode *blk = p_block(p);
    return blk;
}

static JsNode *p_for(P *p)
{
    JsNode *loop;
    int is_async_iter = 0;

    p_advance(p); /* for */
    if (p_is_ident(p, "await")) {
        is_async_iter = 1;
        p_advance(p);
    }
    p_expect(p, "(");

    /* for ( [decl] x of/in y ) */
    {
        int decl_kind_off = 0;
        if (p_is_ident(p, "var") || p_is_ident(p, "let") || p_is_ident(p, "const"))
            decl_kind_off = 1;
        if (p_is_ident_at(p, decl_kind_off + 1, "of") || p_is_ident_at(p, decl_kind_off + 1, "in")) {
            int is_in = p_is_ident_at(p, decl_kind_off + 1, "in");
            JsNode *target;
            JsNode *iter;
            loop = p_node(p, is_in ? JS_N_FORIN : JS_N_FOROF);
            loop->op = is_async_iter;
            if (decl_kind_off) {
                JsNode *decl = p_node(p, JS_N_VAR);
                decl->str = (char *)p_cur(p)->text;
                p_advance(p);
                target = p_binding_target(p);
                js_node_add(p->js, decl, target);
                target = decl;
            } else {
                target = p_binding_target(p);
            }
            p_advance(p); /* of / in */
            iter = is_in ? p_expr(p) : p_assignment(p);
            p_expect(p, ")");
            js_node_add(p->js, loop, target);
            js_node_add(p->js, loop, iter);
            js_node_add(p->js, loop, p_statement(p));
            return loop;
        }
    }

    loop = p_node(p, JS_N_FOR);
    /* init */
    if (p_is(p, ";")) {
        p_advance(p);
        js_node_add(p->js, loop, NULL);
    } else if (p_is_ident(p, "var") || p_is_ident(p, "let") || p_is_ident(p, "const")) {
        JsNode *decl = p_node(p, JS_N_VAR);
        const char *kind = p_cur(p)->text;
        p_advance(p);
        p_var_decl(p, decl, kind);
        p_expect(p, ";");
        js_node_add(p->js, loop, decl);
    } else {
        js_node_add(p->js, loop, p_expr(p));
        p_expect(p, ";");
    }
    /* cond */
    if (p_is(p, ";")) {
        p_advance(p);
        js_node_add(p->js, loop, NULL);
    } else {
        js_node_add(p->js, loop, p_expr(p));
        p_expect(p, ";");
    }
    /* update */
    if (p_is(p, ")")) {
        js_node_add(p->js, loop, NULL);
    } else {
        js_node_add(p->js, loop, p_expr(p));
    }
    p_expect(p, ")");
    js_node_add(p->js, loop, p_statement(p));
    return loop;
}

static JsNode *p_switch(P *p)
{
    JsNode *sw = p_node(p, JS_N_SWITCH);
    p_advance(p); /* switch */
    p_expect(p, "(");
    js_node_add(p->js, sw, p_expr(p));
    p_expect(p, ")");
    p_expect(p, "{");
    while (!p_is(p, "}")) {
        JsNode *clause = p_node(p, JS_N_BLOCK);
        if (p_is_ident(p, "case")) {
            p_advance(p);
            js_node_add(p->js, clause, p_expr(p));
            p_expect(p, ":");
        } else if (p_is_ident(p, "default")) {
            p_advance(p);
            js_node_add(p->js, clause, NULL);
            p_expect(p, ":");
        } else {
            js_error(p->js, p_line(p), "'case' or 'default' expected");
            break;
        }
        while (!p_is(p, "}") && !p_is_ident(p, "case") && !p_is_ident(p, "default")) {
            if (p_cur(p)->type == T_EOF)
                js_error(p->js, p_line(p), "unexpected end of input in switch");
            js_node_add(p->js, clause, p_statement(p));
        }
        js_node_add(p->js, sw, clause);
    }
    p_expect(p, "}");
    return sw;
}

static JsNode *p_try(P *p)
{
    JsNode *tn = p_node(p, JS_N_TRY);
    p_advance(p); /* try */
    js_node_add(p->js, tn, p_block(p)); /* kids[0] = try block */
    if (p_is_ident(p, "catch")) {
        JsNode *clause = p_node(p, JS_N_BLOCK);
        JsNode *binding = NULL;
        p_advance(p);
        if (p_is(p, "(")) {
            p_advance(p);
            binding = p_binding_target(p);
            p_expect(p, ")");
        }
        js_node_add(p->js, clause, binding);
        js_node_add(p->js, clause, p_block(p));
        js_node_add(p->js, tn, clause);
    } else {
        js_node_add(p->js, tn, NULL);
    }
    if (p_is_ident(p, "finally")) {
        p_advance(p);
        js_node_add(p->js, tn, p_block(p));
    } else {
        js_node_add(p->js, tn, NULL);
    }
    return tn;
}

static JsNode *p_export(P *p)
{
    JsNode *ex = p_node(p, JS_N_EXPORT);
    p_advance(p); /* export */
    if (p_is(p, "{")) {
        JsNode *list = p_node(p, JS_N_BLOCK);
        p_advance(p);
        while (!p_is(p, "}")) {
            JsNode *pair = p_node(p, JS_N_ASSIGN);
            JsNode *exported = p_node(p, JS_N_IDENT);
            pair->str = p_cur(p)->text; /* local name */
            exported->str = p_cur(p)->text;
            p_advance(p);
            if (p_is_ident(p, "as")) {
                p_advance(p);
                exported->str = p_cur(p)->text; /* exported as this name */
                p_advance(p);
            }
            js_node_add(p->js, pair, exported);
            js_node_add(p->js, list, pair);
            if (p_is(p, ","))
                p_advance(p);
            else
                break;
        }
        p_expect(p, "}");
        ex->op = 2; /* export list */
        js_node_add(p->js, ex, list);
        if (p_is_ident(p, "from")) {
            p_advance(p);
            js_node_add(p->js, ex, p_primary(p));
        } else {
            js_node_add(p->js, ex, NULL);
        }
        p_at_statement_end(p);
        return ex;
    }
    if (p_is_ident(p, "default")) {
        ex->op = 1; /* export default */
        p_advance(p);
        if (p_is_ident(p, "function"))
            js_node_add(p->js, ex, p_function(p, 0, 0, p_at(p, 1)->type == T_PUNCT &&
                                                             !strcmp(p_at(p, 1)->text, "*")));
        else if (p_is_ident(p, "async") && p_is_ident_at(p, 1, "function")) {
            p_advance(p);
            js_node_add(p->js, ex, p_function(p, 0, 1, p_at(p, 1)->type == T_PUNCT &&
                                                             !strcmp(p_at(p, 1)->text, "*")));
        } else if (p_is_ident(p, "class"))
            js_node_add(p->js, ex, p_class(p));
        else
            js_node_add(p->js, ex, p_assignment(p));
        p_at_statement_end(p);
        return ex;
    }
    if (p_is(p, "*")) {
        ex->op = 3; /* export * from "..." */
        p_advance(p);
        if (p_is_ident(p, "as")) {
            p_advance(p);
            ex->num = 1;
            ex->str = p_cur(p)->text;
            p_advance(p);
        }
        if (!p_is_ident(p, "from"))
            js_error(p->js, p_line(p), "'from' expected");
        p_advance(p);
        js_node_add(p->js, ex, p_primary(p));
        p_at_statement_end(p);
        return ex;
    }
    js_node_add(p->js, ex, p_statement(p));
    return ex;
}

static JsNode *p_import_decl(P *p)
{
    JsNode *im = p_node(p, JS_N_IMPORT);
    JsNode *list = p_node(p, JS_N_BLOCK);
    p_advance(p); /* import */
    if (p_cur(p)->type == T_STR) {
        /* import "side-effect.js" */
        im->op = 1;
        js_node_add(p->js, im, p_primary(p));
        p_at_statement_end(p);
        return im;
    }
    js_node_add(p->js, im, list);
    if (p_cur(p)->type == T_IDENT && !p_is_ident(p, "from")) {
        JsNode *pair = p_node(p, JS_N_ASSIGN);
        pair->op = 1; /* default binding */
        pair->str = p_cur(p)->text;
        p_advance(p);
        js_node_add(p->js, list, pair);
        if (p_is(p, ","))
            p_advance(p);
    }
    if (p_is(p, "*")) {
        JsNode *pair = p_node(p, JS_N_ASSIGN);
        p_advance(p);
        if (!p_is_ident(p, "as"))
            js_error(p->js, p_line(p), "'as' expected in import");
        p_advance(p);
        pair->op = 2; /* namespace binding */
        pair->str = p_cur(p)->text;
        p_advance(p);
        js_node_add(p->js, list, pair);
    } else if (p_is(p, "{")) {
        p_advance(p);
        while (!p_is(p, "}")) {
            JsNode *pair = p_node(p, JS_N_ASSIGN);
            JsNode *imported = p_node(p, JS_N_IDENT);
            pair->op = 0;
            imported->str = p_cur(p)->text; /* imported (source) name */
            pair->str = p_cur(p)->text;     /* local binding name */
            p_advance(p);
            if (p_is_ident(p, "as")) {
                p_advance(p);
                pair->str = p_cur(p)->text;
                p_advance(p);
            }
            js_node_add(p->js, pair, imported);
            js_node_add(p->js, list, pair);
            if (p_is(p, ","))
                p_advance(p);
            else
                break;
        }
        p_expect(p, "}");
    }
    if (!p_is_ident(p, "from"))
        js_error(p->js, p_line(p), "'from' expected");
    p_advance(p);
    js_node_add(p->js, im, p_primary(p));
    p_at_statement_end(p);
    return im;
}

static JsNode *p_statement(P *p)
{
    Tok *t = p_cur(p);

    if (t->type == T_PUNCT && !strcmp(t->text, "{"))
        return p_block(p);

    if (t->type == T_PUNCT && !strcmp(t->text, ";")) {
        p_advance(p);
        return p_node(p, JS_N_EMPTY);
    }

    if (t->type == T_IDENT) {
        /* labelled statement: ident ':' stmt (excluding `default:`) */
        if (p_at(p, 1)->type == T_PUNCT && !strcmp(p_at(p, 1)->text, ":") &&
            strcmp(t->text, "default") != 0 && strcmp(t->text, "case") != 0) {
            JsNode *lab = p_node(p, JS_N_LABEL);
            lab->str = t->text;
            p_advance(p);
            p_advance(p);
            js_node_add(p->js, lab, p_statement(p));
            return lab;
        }
        if (!strcmp(t->text, "var") || !strcmp(t->text, "let") || !strcmp(t->text, "const")) {
            JsNode *decl = p_node(p, JS_N_VAR);
            const char *kind = t->text;
            p_advance(p);
            p_var_decl(p, decl, kind);
            if (!p_at_statement_end(p))
                js_error(p->js, p_line(p), "expected ';' after variable declaration");
            return decl;
        }
        if (!strcmp(t->text, "function")) {
            int gen = p_at(p, 1)->type == T_PUNCT && !strcmp(p_at(p, 1)->text, "*");
            return p_function(p, 1, 0, gen);
        }
        if (!strcmp(t->text, "async") && p_is_ident_at(p, 1, "function")) {
            p_advance(p);
            {
                int gen = p_at(p, 1)->type == T_PUNCT && !strcmp(p_at(p, 1)->text, "*");
                return p_function(p, 1, 1, gen);
            }
        }
        if (!strcmp(t->text, "class"))
            return p_class(p);
        if (!strcmp(t->text, "if")) {
            JsNode *n = p_node(p, JS_N_IF);
            p_advance(p);
            p_expect(p, "(");
            js_node_add(p->js, n, p_expr(p));
            p_expect(p, ")");
            js_node_add(p->js, n, p_statement(p));
            if (p_is_ident(p, "else")) {
                p_advance(p);
                js_node_add(p->js, n, p_statement(p));
            }
            return n;
        }
        if (!strcmp(t->text, "while")) {
            JsNode *n = p_node(p, JS_N_WHILE);
            p_advance(p);
            p_expect(p, "(");
            js_node_add(p->js, n, p_expr(p));
            p_expect(p, ")");
            js_node_add(p->js, n, p_statement(p));
            return n;
        }
        if (!strcmp(t->text, "do")) {
            JsNode *n = p_node(p, JS_N_DO);
            p_advance(p);
            js_node_add(p->js, n, p_statement(p));
            if (!p_is_ident(p, "while"))
                js_error(p->js, p_line(p), "expected 'while'");
            p_advance(p);
            p_expect(p, "(");
            js_node_add(p->js, n, p_expr(p));
            p_expect(p, ")");
            if (p_is(p, ";"))
                p_advance(p);
            return n;
        }
        if (!strcmp(t->text, "for"))
            return p_for(p);
        if (!strcmp(t->text, "switch"))
            return p_switch(p);
        if (!strcmp(t->text, "try"))
            return p_try(p);
        if (!strcmp(t->text, "throw")) {
            JsNode *n = p_node(p, JS_N_THROW);
            p_advance(p);
            if (p_cur(p)->line != t->line)
                js_error(p->js, t->line, "illegal newline after throw");
            js_node_add(p->js, n, p_expr(p));
            if (!p_at_statement_end(p))
                js_error(p->js, p_line(p), "expected ';' after throw");
            return n;
        }
        if (!strcmp(t->text, "return")) {
            JsNode *n = p_node(p, JS_N_RETURN);
            p_advance(p);
            if (!p_is(p, ";") && !p_is(p, "}") && p_cur(p)->type != T_EOF &&
                p_cur(p)->line == t->line)
                js_node_add(p->js, n, p_expr(p));
            if (!p_at_statement_end(p))
                js_error(p->js, p_line(p), "expected ';' after return");
            return n;
        }
        if (!strcmp(t->text, "break") || !strcmp(t->text, "continue")) {
            int is_break = !strcmp(t->text, "break");
            JsNode *n = p_node(p, is_break ? JS_N_BREAK : JS_N_CONTINUE);
            p_advance(p);
            if (p_cur(p)->type == T_IDENT && p_cur(p)->line == t->line) {
                n->str = p_cur(p)->text;
                p_advance(p);
            }
            if (!p_at_statement_end(p)) {
                /* `} else` etc. after a bare break */
                p_at_statement_end(p);
            }
            return n;
        }
        if (!strcmp(t->text, "debugger")) {
            p_advance(p);
            if (!p_at_statement_end(p))
                js_error(p->js, p_line(p), "expected ';' after debugger");
            return p_node(p, JS_N_EMPTY);
        }
        if (!strcmp(t->text, "export"))
            return p_export(p);
        if (!strcmp(t->text, "import") && !(p_at(p, 1)->type == T_PUNCT &&
                                            !strcmp(p_at(p, 1)->text, "(")))
            return p_import_decl(p);
    }

    /* expression statement */
    {
        JsNode *n = p_node(p, JS_N_EXPR);
        js_node_add(p->js, n, p_expr(p));
        if (!p_at_statement_end(p))
            js_error(p->js, p_line(p), "expected ';' after expression");
        return n;
    }
}

/* ---------------- entry points ---------------- */

static JsNode *parse_common(HpeJs *js, const char *source, int module)
{
    Lexer lx;
    P p;
    JsNode *prog = js_node_new(js, JS_N_BLOCK, 0);

    memset(&lx, 0, sizeof(lx));
    lx.js = js;
    lx.src = source ? source : "";
    lx.len = strlen(lx.src);
    lx.line = 1;
    lex_all(&lx);

    memset(&p, 0, sizeof(p));
    p.js = js;
    p.t = lx.toks;
    p.pos = 0;
    p.ntok = lx.ntok;

    prog->op = module ? 1 : 0;
    while (p_cur(&p)->type != T_EOF)
        js_node_add(js, prog, p_statement(&p));
    return prog;
}

JsNode *js_parse_program(HpeJs *js, const char *source)
{
    return parse_common(js, source, 0);
}

JsNode *js_parse_module(HpeJs *js, const char *source)
{
    return parse_common(js, source, 1);
}

/* exported for the module loader: lexes a standalone expression (used by
 * `import()`-free helpers and by the DOM bridge) */
JsNode *js_parse_expression(HpeJs *js, const char *source)
{
    Lexer lx;
    P p;
    JsNode *n;
    memset(&lx, 0, sizeof(lx));
    lx.js = js;
    lx.src = source ? source : "";
    lx.len = strlen(lx.src);
    lx.line = 1;
    lex_all(&lx);
    memset(&p, 0, sizeof(p));
    p.js = js;
    p.t = lx.toks;
    p.pos = 0;
    p.ntok = lx.ntok;
    n = p_expr(&p);
    return n;
}




