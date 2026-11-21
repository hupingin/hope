/* Hope UI Engine - minimal regular expression engine
 *
 * A backtracking matcher working on UTF-8 bytes, with continuation-passing
 * recursion (ReK chain) so that quantifiers, alternation and look-around all
 * compose. Supported syntax:
 *   literals, '.', character classes with ranges/negation,
 *   \d \w \s \D \W \S \b \B, escapes incl. \uXXXX \xXX, anchors ^ $,
 *   quantifiers * + ? {n} {n,} {n,m} (each optionally lazy via a trailing '?'),
 *   grouping (…) (?:…) (?<name>…), capture groups with \1..\9 back-references,
 *   look-ahead (?=…) (?!…) and look-behind (?<=…) (?<!…), alternation |.
 * Flags: i (ignore case), m (multiline ^ $), s (dot matches newline). The 'g'
 * flag is interpreted by the caller, not here.
 *
 * hpe_regex_search reports byte offsets so callers can slice the original
 * subject: caps[0]/caps[1] are the whole match, caps[2+2g]/caps[3+2g] the g-th
 * capture group (1-based), with -1 for groups that did not participate.
 */
#include "js/hpe_js_internal.h"

#include <stdlib.h>
#include <string.h>

#define RE_MAX_STEPS 4000000
#define RE_MAX_DEPTH 12000
#define RE_MAX_RANGES 64
#define RE_MAX_GROUPS 32

typedef struct ReRange {
    uint32_t lo, hi;
} ReRange;

enum {
    RE_CHAR,
    RE_ANY,
    RE_CLASS,
    RE_SEQ,
    RE_ALT,
    RE_REP,
    RE_BOL,
    RE_EOL,
    RE_GROUP,
    RE_BACKREF,
    RE_LOOK,
    RE_LBEHIND,
    RE_WORDB,
    RE_NWORDB
};

#define RE_INF 0x3fffffff

typedef struct ReNode {
    int type;
    uint32_t ch;
    ReRange ranges[RE_MAX_RANGES];
    int nr;
    int negate;
    struct ReNode **kids;
    int n;
    struct ReNode *child;
    int min, max, greedy;
    int group; /* capture index (>=1), 0 when non-capturing */
    int ref;   /* back-reference index */
    int negative;
} ReNode;

struct HpeRegex {
    HpeJs *js;
    ReNode *root;
    int ngroups;
    int flags;
};

typedef struct {
    HpeJs *js;
    const char *p;
    size_t i, len;
    int ngroups;
} ReParser;

/* ---------------- parsing ---------------- */

static ReNode *re_new(ReParser *rp, int type)
{
    ReNode *n = (ReNode *)hpe_js_alloc(rp->js, sizeof(ReNode));
    memset(n, 0, sizeof(*n));
    n->type = type;
    n->max = 1;
    n->greedy = 1;
    return n;
}

static void re_add(ReParser *rp, ReNode *parent, ReNode *kid)
{
    ReNode **nk = (ReNode **)hpe_js_alloc(rp->js, ((size_t)parent->n + 1) * sizeof(ReNode *));
    if (parent->n)
        memcpy(nk, parent->kids, (size_t)parent->n * sizeof(ReNode *));
    nk[parent->n++] = kid;
    parent->kids = nk;
}

static void re_add_range(ReNode *cls, uint32_t lo, uint32_t hi)
{
    if (cls->nr >= RE_MAX_RANGES)
        return;
    cls->ranges[cls->nr].lo = lo;
    cls->ranges[cls->nr].hi = hi;
    cls->nr++;
}

static uint32_t re_utf8(ReParser *rp)
{
    uint32_t cp = 0;
    const unsigned char *s = (const unsigned char *)rp->p;
    size_t i = rp->i;
    unsigned char c = s[i];
    int extra, k;

    if (c < 0x80) {
        rp->i++;
        return c;
    }
    if ((c & 0xE0) == 0xC0) {
        cp = c & 0x1FU;
        extra = 1;
    } else if ((c & 0xF0) == 0xE0) {
        cp = c & 0x0FU;
        extra = 2;
    } else if ((c & 0xF8) == 0xF0) {
        cp = c & 0x07U;
        extra = 3;
    } else {
        rp->i++;
        return 0xFFFD;
    }
    for (k = 1; k <= extra; k++) {
        if (i + (size_t)k >= rp->len)
            break;
        cp = (cp << 6) | (s[i + (size_t)k] & 0x3FU);
    }
    rp->i += (size_t)extra + 1;
    if (rp->i > rp->len)
        rp->i = rp->len;
    return cp;
}

static uint32_t re_hex(ReParser *rp, int digits)
{
    uint32_t v = 0;
    int k;
    for (k = 0; k < digits && rp->i < rp->len; k++) {
        char h = rp->p[rp->i];
        int d;
        if (h >= '0' && h <= '9')
            d = h - '0';
        else if (h >= 'a' && h <= 'f')
            d = h - 'a' + 10;
        else if (h >= 'A' && h <= 'F')
            d = h - 'A' + 10;
        else
            break;
        v = v * 16 + (uint32_t)d;
        rp->i++;
    }
    return v;
}

/* escape after '\': returns 1 when it is a class escape (fills cls) */
static int re_escape(ReParser *rp, ReNode *cls, uint32_t *lit)
{
    char e = rp->i < rp->len ? rp->p[rp->i++] : '\\';
    switch (e) {
    case 'd':
        re_add_range(cls, '0', '9');
        return 1;
    case 'D':
        re_add_range(cls, 0, '0' - 1);
        re_add_range(cls, '9' + 1, 0x10FFFF);
        return 1;
    case 'w':
        re_add_range(cls, 'a', 'z');
        re_add_range(cls, 'A', 'Z');
        re_add_range(cls, '0', '9');
        re_add_range(cls, '_', '_');
        return 1;
    case 'W':
        re_add_range(cls, 0, '/');
        re_add_range(cls, ':', '@');
        re_add_range(cls, '[', '^');
        re_add_range(cls, '`', '`');
        re_add_range(cls, '{', 0x10FFFF);
        return 1;
    case 's':
        re_add_range(cls, ' ', ' ');
        re_add_range(cls, '\t', '\t');
        re_add_range(cls, '\n', '\n');
        re_add_range(cls, '\r', '\r');
        re_add_range(cls, '\f', '\f');
        re_add_range(cls, '\v', '\v');
        return 1;
    case 'S':
        re_add_range(cls, 0x21, 0x10FFFF);
        return 1;
    case 'n':
        *lit = '\n';
        return 0;
    case 't':
        *lit = '\t';
        return 0;
    case 'r':
        *lit = '\r';
        return 0;
    case 'f':
        *lit = '\f';
        return 0;
    case 'v':
        *lit = '\v';
        return 0;
    case '0':
        *lit = 0;
        return 0;
    case 'u':
        *lit = re_hex(rp, 4);
        return 0;
    case 'x':
        *lit = re_hex(rp, 2);
        return 0;
    default:
        *lit = (uint32_t)(unsigned char)e;
        return 0;
    }
}

static ReNode *re_parse_alt(ReParser *rp);

static ReNode *re_parse_class(ReParser *rp)
{
    ReNode *cls = re_new(rp, RE_CLASS);
    if (rp->i < rp->len && rp->p[rp->i] == '^') {
        cls->negate = 1;
        rp->i++;
    }
    while (rp->i < rp->len && rp->p[rp->i] != ']') {
        uint32_t lo, hi = 0;
        int is_class_escape;
        if (rp->p[rp->i] == '\\') {
            rp->i++;
            is_class_escape = re_escape(rp, cls, &lo);
            if (is_class_escape)
                continue;
        } else {
            lo = re_utf8(rp);
        }
        if (rp->i + 1 < rp->len && rp->p[rp->i] == '-' && rp->p[rp->i + 1] != ']') {
            rp->i++;
            if (rp->p[rp->i] == '\\') {
                rp->i++;
                if (re_escape(rp, cls, &hi))
                    hi = lo;
            } else {
                hi = re_utf8(rp);
            }
        } else {
            hi = lo;
        }
        re_add_range(cls, lo, hi);
    }
    if (rp->i < rp->len)
        rp->i++; /* ] */
    return cls;
}

static int re_isdigit(char c)
{
    return c >= '0' && c <= '9';
}

/* parses {n} / {n,} / {n,m}; restores the cursor and returns 0 when malformed */
static int re_parse_braces(ReParser *rp, int *out_min, int *out_max)
{
    size_t save = rp->i;
    int lo, hi;
    if (rp->i >= rp->len || rp->p[rp->i] != '{')
        return 0;
    rp->i++;
    if (rp->i >= rp->len || !re_isdigit(rp->p[rp->i])) {
        rp->i = save;
        return 0;
    }
    lo = 0;
    while (rp->i < rp->len && re_isdigit(rp->p[rp->i]))
        lo = lo * 10 + (rp->p[rp->i++] - '0');
    hi = lo;
    if (rp->i < rp->len && rp->p[rp->i] == ',') {
        rp->i++;
        if (rp->i < rp->len && re_isdigit(rp->p[rp->i])) {
            hi = 0;
            while (rp->i < rp->len && re_isdigit(rp->p[rp->i]))
                hi = hi * 10 + (rp->p[rp->i++] - '0');
        } else {
            hi = RE_INF;
        }
    }
    if (rp->i >= rp->len || rp->p[rp->i] != '}') {
        rp->i = save;
        return 0;
    }
    rp->i++;
    if (hi < lo)
        hi = lo;
    *out_min = lo;
    *out_max = hi;
    return 1;
}

static ReNode *re_parse_atom(ReParser *rp)
{
    char c = rp->i < rp->len ? rp->p[rp->i] : '\0';
    if (c == '(') {
        ReNode *g;
        int capturing = 1;
        int look = 0, behind = 0, negative = 0;
        rp->i++;
        if (rp->i + 1 < rp->len && rp->p[rp->i] == '?') {
            char d = rp->p[rp->i + 1];
            if (d == ':') {
                capturing = 0;
                rp->i += 2;
            } else if (d == '=') {
                capturing = 0;
                look = 1;
                rp->i += 2;
            } else if (d == '!') {
                capturing = 0;
                look = 1;
                negative = 1;
                rp->i += 2;
            } else if (d == '<' && rp->i + 2 < rp->len &&
                       (rp->p[rp->i + 2] == '=' || rp->p[rp->i + 2] == '!')) {
                capturing = 0;
                behind = 1;
                negative = rp->p[rp->i + 2] == '!';
                rp->i += 3;
            } else if (d == '<') {
                rp->i += 2; /* (?<name>…) */
                while (rp->i < rp->len && rp->p[rp->i] != '>')
                    rp->i++;
                if (rp->i < rp->len)
                    rp->i++;
            }
        }
        if (look || behind) {
            g = re_new(rp, behind ? RE_LBEHIND : RE_LOOK);
            g->negative = negative;
        } else {
            g = re_new(rp, RE_GROUP);
            if (capturing) {
                if (rp->ngroups < RE_MAX_GROUPS)
                    rp->ngroups++;
                g->group = rp->ngroups;
            }
        }
        g->child = re_parse_alt(rp);
        if (rp->i < rp->len && rp->p[rp->i] == ')')
            rp->i++;
        return g;
    }
    if (c == '[') {
        rp->i++;
        return re_parse_class(rp);
    }
    if (c == '.') {
        rp->i++;
        return re_new(rp, RE_ANY);
    }
    if (c == '^') {
        rp->i++;
        return re_new(rp, RE_BOL);
    }
    if (c == '$') {
        rp->i++;
        return re_new(rp, RE_EOL);
    }
    if (c == '\\') {
        ReNode *n;
        uint32_t lit = 0;
        ReNode tmp;
        char e;
        rp->i++;
        e = rp->i < rp->len ? rp->p[rp->i] : '\0';
        if (e == 'b') {
            rp->i++;
            return re_new(rp, RE_WORDB);
        }
        if (e == 'B') {
            rp->i++;
            return re_new(rp, RE_NWORDB);
        }
        if (re_isdigit(e) && e != '0') {
            int ref = 0;
            while (rp->i < rp->len && re_isdigit(rp->p[rp->i]))
                ref = ref * 10 + (rp->p[rp->i++] - '0');
            n = re_new(rp, RE_BACKREF);
            n->ref = ref;
            return n;
        }
        memset(&tmp, 0, sizeof(tmp));
        if (re_escape(rp, &tmp, &lit)) {
            int k;
            n = re_new(rp, RE_CLASS);
            n->negate = tmp.negate;
            n->nr = tmp.nr;
            for (k = 0; k < tmp.nr; k++)
                n->ranges[k] = tmp.ranges[k];
            return n;
        }
        n = re_new(rp, RE_CHAR);
        n->ch = lit;
        return n;
    }
    {
        ReNode *n = re_new(rp, RE_CHAR);
        n->ch = re_utf8(rp);
        return n;
    }
}

static ReNode *re_parse_seq(ReParser *rp)
{
    ReNode *seq = re_new(rp, RE_SEQ);
    while (rp->i < rp->len && rp->p[rp->i] != '|' && rp->p[rp->i] != ')') {
        ReNode *atom = re_parse_atom(rp);
        char q = rp->i < rp->len ? rp->p[rp->i] : '\0';
        int min = -1, max = -1;
        if (q == '*') {
            min = 0;
            max = RE_INF;
            rp->i++;
        } else if (q == '+') {
            min = 1;
            max = RE_INF;
            rp->i++;
        } else if (q == '?') {
            min = 0;
            max = 1;
            rp->i++;
        } else if (q == '{') {
            if (!re_parse_braces(rp, &min, &max))
                min = max = -1;
        }
        if (min >= 0) {
            ReNode *rep = re_new(rp, RE_REP);
            rep->child = atom;
            rep->min = min;
            rep->max = max;
            if (rp->i < rp->len && rp->p[rp->i] == '?') {
                rep->greedy = 0;
                rp->i++;
            }
            atom = rep;
        }
        re_add(rp, seq, atom);
    }
    return seq;
}

static ReNode *re_parse_alt(ReParser *rp)
{
    ReNode *first = re_parse_seq(rp);
    if (rp->i >= rp->len || rp->p[rp->i] != '|')
        return first;
    {
        ReNode *alt = re_new(rp, RE_ALT);
        re_add(rp, alt, first);
        while (rp->i < rp->len && rp->p[rp->i] == '|') {
            rp->i++;
            re_add(rp, alt, re_parse_seq(rp));
        }
        return alt;
    }
}

/* ---------------- matching ---------------- */

typedef struct {
    const char *s;
    size_t len;
    int *caps;
    int ncaps;
    int flags;
    int depth;
    long steps;
    int match_end;
} ReCtx;

enum { K_END, K_SEQ, K_REP, K_GROUP };

typedef struct ReK {
    int kind;
    ReNode *seq; /* K_SEQ: resume at seq->kids[idx] */
    int idx;
    ReNode *rep; /* K_REP */
    int count;
    size_t at;
    int group; /* K_GROUP */
    size_t gstart;
    int target; /* K_END: required end offset, -1 for "any" */
    struct ReK *next;
} ReK;

static int re_node(ReNode *n, ReK *k, ReCtx *c, size_t pos);
static int re_cont(ReK *k, ReCtx *c, size_t pos);

static void re_set_cap(ReCtx *c, int idx, int start, int end)
{
    int b = 2 * idx, e = 2 * idx + 1;
    if (b + 1 < c->ncaps) {
        c->caps[b] = start;
        c->caps[e] = end;
    }
}

static int re_cap_get(ReCtx *c, int idx, int *start, int *end)
{
    int b = 2 * idx, e = 2 * idx + 1;
    if (b + 1 >= c->ncaps || c->caps[b] < 0 || c->caps[e] < 0)
        return 0;
    *start = c->caps[b];
    *end = c->caps[e];
    return 1;
}

static int re_is_word(uint32_t cp)
{
    return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9') ||
           cp == '_' || cp > 0x7F;
}

/* decodes one codepoint at `pos`; returns its byte width (>=1), 0 at the end */
static int re_decode_at(const char *s, size_t len, size_t pos, uint32_t *out)
{
    const unsigned char *u = (const unsigned char *)s + pos;
    uint32_t cp;
    int extra, k;
    if (pos >= len) {
        *out = 0;
        return 0;
    }
    if (u[0] < 0x80) {
        *out = u[0];
        return 1;
    }
    if ((u[0] & 0xE0) == 0xC0) {
        cp = u[0] & 0x1FU;
        extra = 1;
    } else if ((u[0] & 0xF0) == 0xE0) {
        cp = u[0] & 0x0FU;
        extra = 2;
    } else if ((u[0] & 0xF8) == 0xF0) {
        cp = u[0] & 0x07U;
        extra = 3;
    } else {
        *out = 0xFFFD;
        return 1;
    }
    for (k = 1; k <= extra; k++) {
        if (pos + (size_t)k >= len) {
            *out = cp;
            return k;
        }
        cp = (cp << 6) | (u[k] & 0x3FU);
    }
    *out = cp;
    return extra + 1;
}

/* codepoint immediately before `pos` (0 at the start of the subject) */
static uint32_t re_prev_cp(const char *s, size_t pos)
{
    size_t i;
    uint32_t cp;
    if (pos == 0)
        return 0;
    i = pos - 1;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80)
        i--;
    re_decode_at(s, pos, i, &cp);
    return cp;
}

static char re_lower(char a)
{
    return (a >= 'A' && a <= 'Z') ? (char)(a - 'A' + 'a') : a;
}

/* does `n` match the single codepoint at `pos`? sets `adv` to its byte width */
static int re_single(ReNode *n, ReCtx *c, size_t pos, int *adv)
{
    uint32_t cp;
    int w;
    if (pos >= c->len)
        return 0;
    w = re_decode_at(c->s, c->len, pos, &cp);
    *adv = w;
    switch (n->type) {
    case RE_CHAR:
        if (n->ch == cp)
            return 1;
        if ((c->flags & 1) && n->ch < 0x80 && cp < 0x80)
            return re_lower((char)n->ch) == re_lower((char)cp);
        return 0;
    case RE_ANY:
        return (c->flags & 4) ? 1 : cp != '\n';
    case RE_CLASS: {
        int k, hit = 0;
        for (k = 0; k < n->nr; k++) {
            if (cp >= n->ranges[k].lo && cp <= n->ranges[k].hi) {
                hit = 1;
                break;
            }
            if ((c->flags & 1) && cp < 0x80 && n->ranges[k].lo < 0x80 && n->ranges[k].hi < 0x80) {
                char a = re_lower((char)cp);
                if (a >= (char)n->ranges[k].lo && a <= (char)n->ranges[k].hi) {
                    hit = 1;
                    break;
                }
            }
        }
        return hit != n->negate;
    }
    default:
        return 0;
    }
}

static int re_at_eol(ReCtx *c, size_t pos)
{
    if (pos >= c->len)
        return 1;
    return (c->flags & 2) && c->s[pos] == '\n';
}

/* continue: resume the quants/branches scheduled after the current node */
static int re_cont(ReK *k, ReCtx *c, size_t pos)
{
    if (!k) {
        c->match_end = (int)pos;
        return 1;
    }
    switch (k->kind) {
    case K_END:
        if (k->target < 0 || (size_t)k->target == pos) {
            c->match_end = (int)pos;
            return 1;
        }
        return 0;
    case K_SEQ: {
        ReK nk;
        if (k->idx >= k->seq->n)
            return re_cont(k->next, c, pos);
        nk.kind = K_SEQ;
        nk.seq = k->seq;
        nk.idx = k->idx + 1;
        nk.rep = NULL;
        nk.count = 0;
        nk.at = 0;
        nk.group = 0;
        nk.gstart = 0;
        nk.target = -1;
        nk.next = k->next;
        return re_node(k->seq->kids[k->idx], &nk, c, pos);
    }
    case K_GROUP: {
        int g = k->group;
        int os, oe;
        int had = re_cap_get(c, g, &os, &oe);
        if (g > 0)
            re_set_cap(c, g, (int)k->gstart, (int)pos);
        if (re_cont(k->next, c, pos))
            return 1;
        if (g > 0)
            re_set_cap(c, g, had ? os : -1, had ? oe : -1);
        return 0;
    }
    case K_REP: {
        ReNode *rep = k->rep;
        int count = k->count;
        int progressed = (count == 0) || (pos != k->at);
        if (rep->greedy) {
            if (progressed && count < rep->max) {
                ReK rk;
                rk.kind = K_REP;
                rk.seq = NULL;
                rk.idx = 0;
                rk.rep = rep;
                rk.count = count + 1;
                rk.at = pos;
                rk.group = 0;
                rk.gstart = 0;
                rk.target = -1;
                rk.next = k->next;
                if (re_node(rep->child, &rk, c, pos))
                    return 1;
            }
            if (count >= rep->min)
                return re_cont(k->next, c, pos);
            return 0;
        }
        if (count >= rep->min && re_cont(k->next, c, pos))
            return 1;
        if (progressed && count < rep->max) {
            ReK rk;
            rk.kind = K_REP;
            rk.seq = NULL;
            rk.idx = 0;
            rk.rep = rep;
            rk.count = count + 1;
            rk.at = pos;
            rk.group = 0;
            rk.gstart = 0;
            rk.target = -1;
            rk.next = k->next;
            return re_node(rep->child, &rk, c, pos);
        }
        return 0;
    }
    default:
        return 0;
    }
}

/* single-codepoint bodies repeat iteratively to keep recursion shallow */
static int re_rep_simple(ReNode *rep, ReK *k, ReCtx *c, size_t pos)
{
    ReNode *sub = rep->child;
    size_t cap = c->len - pos + 2;
    size_t *stops = (size_t *)malloc(cap * sizeof(size_t));
    size_t i = pos;
    int count = 0;
    int ok = 0;

    if (!stops)
        return 0;
    stops[0] = pos;
    while (count < rep->max && i < c->len) {
        int adv = 0;
        if (!re_single(sub, c, i, &adv))
            break;
        i += (size_t)adv;
        count++;
        stops[count] = i;
    }
    if (count >= rep->min) {
        int n;
        if (rep->greedy) {
            for (n = count; n >= rep->min; n--)
                if (re_cont(k, c, stops[n])) {
                    ok = 1;
                    break;
                }
        } else {
            for (n = rep->min; n <= count; n++)
                if (re_cont(k, c, stops[n])) {
                    ok = 1;
                    break;
                }
        }
    }
    free(stops);
    return ok;
}

static int re_backref(ReNode *n, ReK *k, ReCtx *c, size_t pos)
{
    int s, e;
    size_t sl, i;
    if (!re_cap_get(c, n->ref, &s, &e))
        return re_cont(k, c, pos); /* unset group matches the empty string */
    sl = (size_t)(e - s);
    if (pos + sl > c->len)
        return 0;
    for (i = 0; i < sl; i++) {
        char a = c->s[s + (int)i], b = c->s[pos + i];
        if ((c->flags & 1) && (unsigned char)a < 0x80 && (unsigned char)b < 0x80) {
            a = re_lower(a);
            b = re_lower(b);
        }
        if (a != b)
            return 0;
    }
    return re_cont(k, c, pos + sl);
}

/* zero-width assertion: does `child` match at (look-ahead) / end at (look-behind) pos? */
static int re_assert(ReNode *n, ReCtx *c, size_t pos)
{
    int saved[RE_MAX_GROUPS * 2];
    int nc = c->ncaps < RE_MAX_GROUPS * 2 ? c->ncaps : RE_MAX_GROUPS * 2;
    int saved_end = c->match_end;
    int hit = 0;

    memcpy(saved, c->caps, (size_t)nc * sizeof(int));
    if (n->type == RE_LOOK) {
        ReK e;
        e.kind = K_END;
        e.seq = NULL;
        e.idx = 0;
        e.rep = NULL;
        e.count = 0;
        e.at = 0;
        e.group = 0;
        e.gstart = 0;
        e.target = -1;
        e.next = NULL;
        hit = re_node(n->child, &e, c, pos);
    } else {
        size_t start;
        for (start = 0; start <= pos; start++) {
            ReK e;
            e.kind = K_END;
            e.seq = NULL;
            e.idx = 0;
            e.rep = NULL;
            e.count = 0;
            e.at = 0;
            e.group = 0;
            e.gstart = 0;
            e.target = (int)pos;
            e.next = NULL;
            if (re_node(n->child, &e, c, start)) {
                hit = 1;
                break;
            }
        }
    }
    if (!hit)
        memcpy(c->caps, saved, (size_t)nc * sizeof(int));
    c->match_end = saved_end;
    return n->negative ? !hit : hit;
}

static int re_node(ReNode *n, ReK *k, ReCtx *c, size_t pos)
{
    int r = 0;

    if (++c->steps > RE_MAX_STEPS)
        return 0;
    if (++c->depth > RE_MAX_DEPTH) {
        c->depth--;
        return 0;
    }
    switch (n->type) {
    case RE_BOL:
        if (pos == 0 || ((c->flags & 2) && pos > 0 && c->s[pos - 1] == '\n'))
            r = re_cont(k, c, pos);
        break;
    case RE_EOL:
        if (re_at_eol(c, pos))
            r = re_cont(k, c, pos);
        break;
    case RE_CHAR:
    case RE_ANY:
    case RE_CLASS: {
        int adv = 0;
        if (re_single(n, c, pos, &adv))
            r = re_cont(k, c, pos + (size_t)adv);
        break;
    }
    case RE_SEQ: {
        ReK nk;
        if (n->n == 0) {
            r = re_cont(k, c, pos);
            break;
        }
        nk.kind = K_SEQ;
        nk.seq = n;
        nk.idx = 1;
        nk.rep = NULL;
        nk.count = 0;
        nk.at = 0;
        nk.group = 0;
        nk.gstart = 0;
        nk.target = -1;
        nk.next = k;
        r = re_node(n->kids[0], &nk, c, pos);
        break;
    }
    case RE_ALT: {
        int i;
        for (i = 0; i < n->n; i++) {
            if (re_node(n->kids[i], k, c, pos)) {
                r = 1;
                break;
            }
        }
        break;
    }
    case RE_REP: {
        ReNode *sub = n->child;
        if (n->max == 0) {
            r = (n->min == 0) ? re_cont(k, c, pos) : 0;
            break;
        }
        if (sub->type == RE_CHAR || sub->type == RE_ANY || sub->type == RE_CLASS) {
            r = re_rep_simple(n, k, c, pos);
        } else {
            ReK rk;
            rk.kind = K_REP;
            rk.seq = NULL;
            rk.idx = 0;
            rk.rep = n;
            rk.count = 0;
            rk.at = pos;
            rk.group = 0;
            rk.gstart = 0;
            rk.target = -1;
            rk.next = k;
            r = re_cont(&rk, c, pos);
        }
        break;
    }
    case RE_GROUP: {
        int g = n->group;
        if (g > 0)
            re_set_cap(c, g, -1, -1);
        if (g > 0) {
            ReK gk;
            gk.kind = K_GROUP;
            gk.seq = NULL;
            gk.idx = 0;
            gk.rep = NULL;
            gk.count = 0;
            gk.at = 0;
            gk.group = g;
            gk.gstart = pos;
            gk.target = -1;
            gk.next = k;
            r = re_node(n->child, &gk, c, pos);
        } else {
            r = re_node(n->child, k, c, pos);
        }
        break;
    }
    case RE_LOOK:
    case RE_LBEHIND:
        if (re_assert(n, c, pos))
            r = re_cont(k, c, pos);
        break;
    case RE_WORDB:
    case RE_NWORDB: {
        uint32_t cp = 0;
        int before = re_is_word(re_prev_cp(c->s, pos));
        int after;
        if (pos < c->len)
            re_decode_at(c->s, c->len, pos, &cp);
        after = pos < c->len && re_is_word(cp);
        if ((n->type == RE_WORDB) == (before != after))
            r = re_cont(k, c, pos);
        break;
    }
    case RE_BACKREF:
        r = re_backref(n, k, c, pos);
        break;
    default:
        break;
    }
    c->depth--;
    return r;
}

int hpe_regex_group_count(HpeRegex *re)
{
    return re ? re->ngroups : 0;
}

void hpe_regex_set_flags(HpeRegex *re, const char *flags)
{
    const char *p;
    if (!re)
        return;
    re->flags = 0;
    if (!flags)
        return;
    for (p = flags; *p; p++) {
        if (*p == 'i')
            re->flags |= 1;
        else if (*p == 'm')
            re->flags |= 2;
        else if (*p == 's')
            re->flags |= 4;
    }
}

/* ---------------- public entry points ---------------- */

HpeRegex *hpe_regex_compile(HpeJs *js, const char *pattern)
{
    ReParser rp;
    HpeRegex *re = (HpeRegex *)hpe_js_alloc(js, sizeof(HpeRegex));

    re->js = js;
    re->root = NULL;
    re->ngroups = 0;
    re->flags = 0;
    memset(&rp, 0, sizeof(rp));
    rp.js = js;
    rp.p = pattern ? pattern : "";
    rp.len = strlen(rp.p);
    re->root = re_parse_alt(&rp);
    re->ngroups = rp.ngroups;
    return re;
}

/* searches from `start`; returns the offset just past the match, or -1.
 * captures hold byte offsets: [0]/[1] whole match, [2+2g]/[3+2g] group g. */
int hpe_regex_search(HpeRegex *re, const char *subject, int start, int *captures, int ncaps)
{
    ReCtx c;
    size_t pos;
    const char *s = subject ? subject : "";
    int i;

    if (!re || !re->root)
        return -1;
    memset(&c, 0, sizeof(c));
    c.s = s;
    c.len = strlen(s);
    c.caps = captures;
    c.ncaps = ncaps;
    c.flags = re->flags;
    if (start < 0)
        start = 0;
    if ((size_t)start > c.len)
        return -1;
    for (pos = (size_t)start; pos <= c.len; pos++) {
        ReK end;
        c.depth = 0;
        c.match_end = -1;
        for (i = 0; i < ncaps; i++)
            captures[i] = -1;
        end.kind = K_END;
        end.seq = NULL;
        end.idx = 0;
        end.rep = NULL;
        end.count = 0;
        end.at = 0;
        end.group = 0;
        end.gstart = 0;
        end.target = -1;
        end.next = NULL;
        if (re_node(re->root, &end, &c, pos) && c.match_end >= 0) {
            /* caps[0]/caps[1] describe the whole match (byte offsets) */
            if (ncaps > 1) {
                captures[0] = (int)pos;
                captures[1] = c.match_end;
            }
            return c.match_end;
        }
        /* a top-level ^ can only ever match at offset 0 (or after a newline) */
        if (!(re->flags & 2) && re->root->type == RE_SEQ && re->root->n > 0 &&
            re->root->kids[0]->type == RE_BOL && pos > 0)
            break;
    }
    return -1;
}

int hpe_regex_match(HpeRegex *re, const char *subject)
{
    int caps[2];
    return hpe_regex_search(re, subject, 0, caps, 2) >= 0;
}
