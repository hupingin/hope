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

/**
 * @brief 分配并初始化一个正则 AST 节点 / Allocate and initialise a regex AST node
 *
 * @param[in,out] rp 正则解析器 / Regex parser
 * @param[in]     type 节点类型（`RE_CHAR` 等）/ Node type such as `RE_CHAR`
 *
 * @return 新的模式节点 / The new pattern node
 *
 * @note 节点从 arena 分配并清零，`max` 初始化为 1、`greedy` 为 1 / The node is arena-allocated and zeroed with `max` 1 and `greedy` 1
 */

static ReNode *re_new(ReParser *rp, int type)
{
    ReNode *n = (ReNode *)hpe_js_alloc(rp->js, sizeof(ReNode));
    memset(n, 0, sizeof(*n));
    n->type = type;
    n->max = 1;
    n->greedy = 1;
    return n;
}

/**
 * @brief 把子节点追加到父节点的孩子表 / Append a child node to a parent's child list
 *
 * @param[in,out] rp 正则解析器 / Regex parser
 * @param[in,out] parent 父节点 / Parent node
 * @param[in,out] kid 要追加的子节点 / Child node to append
 *
 * @note 每次追加都另分配一份更大的孩子数组再拷贝 / Each append allocates a larger child array and copies the old pointers
 */
static void re_add(ReParser *rp, ReNode *parent, ReNode *kid)
{
    ReNode **nk = (ReNode **)hpe_js_alloc(rp->js, ((size_t)parent->n + 1) * sizeof(ReNode *));
    if (parent->n)
        memcpy(nk, parent->kids, (size_t)parent->n * sizeof(ReNode *));
    nk[parent->n++] = kid;
    parent->kids = nk;
}

/**
 * @brief 向字符类追加一个码点区间 / Append a code-point range to a character class
 *
 * @param[in,out] cls 字符类节点 / Character-class node
 * @param[in]     lo 区间下界（含）/ Inclusive lower bound
 * @param[in]     hi 区间上界（含）/ Inclusive upper bound
 *
 * @note 区间数达到 `RE_MAX_RANGES` 后静默丢弃 / Ranges are silently dropped once `RE_MAX_RANGES` is reached
 */
static void re_add_range(ReNode *cls, uint32_t lo, uint32_t hi)
{
    if (cls->nr >= RE_MAX_RANGES)
        return;
    cls->ranges[cls->nr].lo = lo;
    cls->ranges[cls->nr].hi = hi;
    cls->nr++;
}

/**
 * @brief 从模式串当前位置解码一个 UTF-8 码点 / Decode one UTF-8 code point at the pattern cursor
 *
 * @param[in,out] rp 正则解析器（游标前进）/ Regex parser, whose cursor advances
 *
 * @return 解码出的码点；非法首字节返回 `0xFFFD` / The decoded code point, or `0xFFFD` for an illegal lead byte
 *
 * @note 游标越过串尾时被夹到 `len` / The cursor is clamped to `len` when it would overshoot
 */
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

/**
 * @brief 读取定长十六进制数字 / Read a fixed number of hexadecimal digits
 *
 * @param[in,out] rp 正则解析器（游标前进）/ Regex parser, whose cursor advances
 * @param[in]     digits 期望读取的数字个数 / Number of digits expected
 *
 * @return 读到的十六进制值 / The hexadecimal value read
 *
 * @note 遇到非十六进制字符或串尾提前停止 / Stops early on a non-hex character or the end of the pattern
 */
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

/**
 * @brief 展开 `\d \D \w \W \s \S` 简写类为码点区间 / Expand a `\d \D \w \W \s \S` shorthand class into code-point ranges
 *
 * @param[in,out] cls 接收展开区间的字符类节点 / Character-class node receiving the ranges
 * @param[in]     e 转义字符 / The escape character
 *
 * @return 确为简写类并已追加区间返回 1，否则返回 0 / 1 when `e` was a shorthand class and ranges were added, otherwise 0
 */
static int re_add_class_escapes(ReNode *cls, char e)
{
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
    default:
        return 0;
    }
}

/**
 * @brief 把单字符转义映射为字面码点 / Map a single-character escape to its literal code point
 *
 * @param[in]     e 转义字符 / The escape character
 *
 * @return 对应的码点（`\n`、`\t`、`\r`、`\f`、`\v`、`\0` 等）/ The matching code point for `\n`, `\t`, `\r`, `\f`, `\v`, `\0` and friends
 *
 * @note 未知转义按原字符返回 / An unknown escape falls back to the raw character
 */
static uint32_t re_escape_literal(char e)
{
    switch (e) {
    case 'n':
        return '\n';
    case 't':
        return '\t';
    case 'r':
        return '\r';
    case 'f':
        return '\f';
    case 'v':
        return '\v';
    case '0':
        return 0;
    default:
        return (uint32_t)(unsigned char)e;
    }
}

/**
 * @brief 解析反斜杠后的转义，区分类转义与字面字符 / Parse an escape after a backslash, telling class escapes from literal characters
 *
 * @param[in,out] rp 正则解析器（游标前进）/ Regex parser, whose cursor advances
 * @param[in,out] cls 接收类转义区间的字符类节点 / Character-class node receiving class-escape ranges
 * @param[out]    lit 接收字面码点（`\u`/`\x` 与单字符转义）/ Receives the literal code point for `\u`, `\x` and single-character escapes
 *
 * @return 是类转义返回 1（填充 `cls`），是字面转义返回 0（填充 `*lit`）/ 1 for a class escape filling `cls`, 0 for a literal escape filling `*lit`
 */
static int re_escape(ReParser *rp, ReNode *cls, uint32_t *lit)
{
    char e = rp->i < rp->len ? rp->p[rp->i++] : '\\';

    if (re_add_class_escapes(cls, e))
        return 1;
    if (e == 'u') {
        *lit = re_hex(rp, 4);
        return 0;
    }
    if (e == 'x') {
        *lit = re_hex(rp, 2);
        return 0;
    }
    *lit = re_escape_literal(e);
    return 0;
}

static ReNode *re_parse_alt(ReParser *rp);

/**
 * @brief 解析字符类 `[...]` / Parse a character class `[...]`
 *
 * @details 支持开头 `^` 取反、`a-z` 区间、`\` 转义与简写类；区间两端可各为码点或转义。
 *          Supports a leading `^` negation, `a-z` ranges, `\` escapes and shorthand classes, with either range endpoint usable as a code point or escape.
 *
 * @param[in,out] rp 正则解析器（游标位于 `[` 之后）/ Regex parser with the cursor just past `[`
 *
 * @return 新建的 `RE_CLASS` 节点 / The new `RE_CLASS` node
 */
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

/**
 * @brief 判断字符是否为十进制数字 / Test whether a character is a decimal digit
 *
 * @param[in]     c 待测字符 / Character to test
 *
 * @return 是 `0`..`9` 返回 1 / 1 when the character is `0`..`9`
 */
static int re_isdigit(char c)
{
    return c >= '0' && c <= '9';
}

/**
 * @brief 解析 `{n}` / `{n,}` / `{n,m}` 量词范围 / Parse a `{n}` / `{n,}` / `{n,m}` quantifier range
 *
 * @param[in,out] rp 正则解析器（游标前进）/ Regex parser, whose cursor advances
 * @param[out]    out_min 接收最小重复次数 / Receives the minimum repeat count
 * @param[out]    out_max 接收最大重复次数（`RE_INF` 表示无上限）/ Receives the maximum repeat count, `RE_INF` when unbounded
 *
 * @return 格式合法返回 1，否则游标回退并返回 0 / 1 when well-formed, otherwise the cursor is restored and 0 is returned
 *
 * @note `hi` 小于 `lo` 时被抬到 `lo` / A `hi` below `lo` is raised to `lo`
 */
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

/* parses the `(?:`, `(?=`, `(?!`, `(?<=`, `(?<!` and `(?<name>` prefixes and
 * reports which kind of group follows */
typedef struct {
    int capturing;
    int look;
    int behind;
    int negative;
} ReGroupHdr;

/**
 * @brief 解析分组前缀，判定分组种类 / Parse a group prefix and classify the group
 *
 * @param[in,out] rp 正则解析器（游标前进）/ Regex parser, whose cursor advances
 * @param[out]    h 接收分组头信息（捕获/环视/后顾/否定）/ Receives the header info: capturing, look-around, look-behind and negation
 *
 * @note 识别 `(?:`、`(?=`、`(?!`、`(?<=`、`(?<!` 与 `(?<name>`；具名组的名字不保留 / Recognises `(?:`, `(?=`, `(?!`, `(?<=`, `(?<!` and `(?<name>`; the name itself is discarded
 */
static void re_parse_group_header(ReParser *rp, ReGroupHdr *h)
{
    char d;

    if (!(rp->i + 1 < rp->len && rp->p[rp->i] == '?'))
        return;
    d = rp->p[rp->i + 1];
    if (d == ':') {
        h->capturing = 0;
        rp->i += 2;
    } else if (d == '=') {
        h->capturing = 0;
        h->look = 1;
        rp->i += 2;
    } else if (d == '!') {
        h->capturing = 0;
        h->look = 1;
        h->negative = 1;
        rp->i += 2;
    } else if (d == '<' && rp->i + 2 < rp->len &&
               (rp->p[rp->i + 2] == '=' || rp->p[rp->i + 2] == '!')) {
        h->capturing = 0;
        h->behind = 1;
        h->negative = rp->p[rp->i + 2] == '!';
        rp->i += 3;
    } else if (d == '<') {
        rp->i += 2; /* (?<name>…) — the name is not retained */
        while (rp->i < rp->len && rp->p[rp->i] != '>')
            rp->i++;
        if (rp->i < rp->len)
            rp->i++;
    }
}

/**
 * @brief 解析一个分组（含具名与非捕获组）/ Parse a group, named and non-capturing included
 *
 * @details 先读分组头决定节点种类，再以嵌套的并集表达式作为孩子；捕获组按出现顺序编号。
 *          The header decides the node kind and a nested alternation becomes the child; capturing groups are numbered in order of appearance.
 *
 * @param[in,out] rp 正则解析器（游标位于 `(`）/ Regex parser with the cursor at `(`
 *
 * @return 分组/环视/后顾节点 / The group, look-around or look-behind node
 *
 * @note 捕获组数量超过 `RE_MAX_GROUPS` 后不再编号但仍会解析 / Beyond `RE_MAX_GROUPS` groups are still parsed but no longer numbered
 */
static ReNode *re_parse_group(ReParser *rp)
{
    ReGroupHdr h = {1, 0, 0, 0};
    ReNode *g;

    rp->i++; /* ( */
    re_parse_group_header(rp, &h);
    if (h.look || h.behind) {
        g = re_new(rp, h.behind ? RE_LBEHIND : RE_LOOK);
        g->negative = h.negative;
    } else {
        g = re_new(rp, RE_GROUP);
        if (h.capturing) {
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

/**
 * @brief 解析反斜杠起头的原子 / Parse a backslash-introduced atom
 *
 * @details `\b`/`\B` 生成词边界节点，数字生成反向引用，简写类生成 `RE_CLASS`，其余按字面字符处理。
 *          `\b` and `\B` become word-boundary nodes, digits become back-references, shorthand classes become `RE_CLASS` and everything else is a literal character.
 *
 * @param[in,out] rp 正则解析器（游标位于 `\`）/ Regex parser with the cursor at `\`
 *
 * @return 对应的 `RE_CLASS`、`RE_WORDB`、`RE_NWORDB`、`RE_BACKREF` 或 `RE_CHAR` 节点 / The matching `RE_CLASS`, `RE_WORDB`, `RE_NWORDB`, `RE_BACKREF` or `RE_CHAR` node
 */
static ReNode *re_parse_backslash_atom(ReParser *rp)
{
    uint32_t lit = 0;
    ReNode tmp;
    ReNode *n;
    char e;

    rp->i++; /* \ */
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

/**
 * @brief 把裸标点映射为零宽锚点节点 / Map a bare punctuation character to its zero-width anchor node
 *
 * @param[in,out] rp 正则解析器（命中时游标前进）/ Regex parser, whose cursor advances on a hit
 * @param[in]     c 待判定的字符 / Character to classify
 *
 * @return `.`/`^`/`$` 对应 `RE_ANY`/`RE_BOL`/`RE_EOL` 节点，其它字符返回 `NULL` / An `RE_ANY`/`RE_BOL`/`RE_EOL` node for `.`, `^` and `$`, otherwise `NULL` for an ordinary literal
 */
static ReNode *re_parse_anchor(ReParser *rp, char c)
{
    int kind;

    switch (c) {
    case '.':
        kind = RE_ANY;
        break;
    case '^':
        kind = RE_BOL;
        break;
    case '$':
        kind = RE_EOL;
        break;
    default:
        return NULL;
    }
    rp->i++;
    return re_new(rp, kind);
}

/**
 * @brief 解析一个原子（分组、字符类、转义、锚点或字面）/ Parse one atom: group, class, escape, anchor or literal
 *
 * @param[in,out] rp 正则解析器 / Regex parser
 *
 * @return 解析出的原子节点 / The parsed atom node
 *
 * @note 字面字符按 UTF-8 解码为单码点 `RE_CHAR` / A literal character is UTF-8-decoded into a single-code-point `RE_CHAR`
 */
static ReNode *re_parse_atom(ReParser *rp)
{
    char c = rp->i < rp->len ? rp->p[rp->i] : '\0';
    ReNode *anchor;

    if (c == '(')
        return re_parse_group(rp);
    if (c == '[') {
        rp->i++;
        return re_parse_class(rp);
    }
    if (c == '\\')
        return re_parse_backslash_atom(rp);
    anchor = re_parse_anchor(rp, c);
    if (anchor)
        return anchor;
    {
        ReNode *n = re_new(rp, RE_CHAR);
        n->ch = re_utf8(rp);
        return n;
    }
}

/**
 * @brief 解析一个并置序列（可带量词）/ Parse a concatenation sequence with optional quantifiers
 *
 * @details 逐个解析原子并识别 `*`/`+`/`?`/`{n,m}` 量词，量词后紧跟 `?` 表示懒惰；遇 `
 *          ` 或 `)` 结束。
 *          Atoms are parsed one by one, then `*`/`+`/`?`/`{n,m}` quantifiers are recognised with a trailing `?` meaning lazy; parsing stops at `
 *          ` or `)`.
 *
 * @param[in,out] rp 正则解析器 / Regex parser
 *
 * @return 由若干原子构成的 `RE_SEQ` 节点 / The `RE_SEQ` node built from the atoms
 */
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

/**
 * @brief 解析 `|` 并集表达式 / Parse a `|` alternation expression
 *
 * @param[in,out] rp 正则解析器 / Regex parser
 *
 * @return 只有一个分支时返回该序列，否则返回 `RE_ALT` 节点 / The single sequence when there is one branch, otherwise an `RE_ALT` node
 */
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

/**
 * @brief 写入第 idx 个捕获组的字节区间 / Store the byte span of capture group idx
 *
 * @param[in,out] c 匹配上下文 / Match context
 * @param[in]     idx 捕获组下标 / Capture group index
 * @param[in]     start 起始字节偏移 / Start byte offset
 * @param[in]     end 结束字节偏移 / End byte offset
 *
 * @note 下标越界（超出 `ncaps`）时静默忽略 / Out-of-range indices beyond `ncaps` are silently ignored
 */
static void re_set_cap(ReCtx *c, int idx, int start, int end)
{
    int b = 2 * idx, e = 2 * idx + 1;
    if (b + 1 < c->ncaps) {
        c->caps[b] = start;
        c->caps[e] = end;
    }
}

/**
 * @brief 读取第 idx 个捕获组的字节区间 / Read the byte span of capture group idx
 *
 * @param[in,out] c 匹配上下文 / Match context
 * @param[in]     idx 捕获组下标 / Capture group index
 * @param[out]    start 接收起始字节偏移 / Receives the start byte offset
 * @param[out]    end 接收结束字节偏移 / Receives the end byte offset
 *
 * @return 该组已参与匹配返回 1，未设置或越界返回 0 / 1 when the group participated, 0 when unset or out of range
 */
static int re_cap_get(ReCtx *c, int idx, int *start, int *end)
{
    int b = 2 * idx, e = 2 * idx + 1;
    if (b + 1 >= c->ncaps || c->caps[b] < 0 || c->caps[e] < 0)
        return 0;
    *start = c->caps[b];
    *end = c->caps[e];
    return 1;
}

/**
 * @brief 判断码点是否属于单词字符 / Test whether a code point counts as a word character
 *
 * @param[in]     cp 码点 / Code point
 *
 * @return 为字母、数字、`_` 或非 ASCII 码点返回 1 / 1 for letters, digits, `_` or non-ASCII code points
 */
static int re_is_word(uint32_t cp)
{
    return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9') ||
           cp == '_' || cp > 0x7F;
}

/**
 * @brief 在指定字节位置解码一个 UTF-8 码点 / Decode one UTF-8 code point at a byte position
 *
 * @param[in]     s 主题字节串 / Subject byte string
 * @param[in]     len 主题字节长度 / Subject byte length
 * @param[in]     pos 解码起点字节偏移 / Start byte offset
 * @param[out]    out 接收解码出的码点 / Receives the decoded code point
 *
 * @return 该码点的字节宽度（至少 1），到串尾返回 0 / The byte width of the code point, at least 1, or 0 at the end of the subject
 *
 * @note 非法首字节输出 `0xFFFD` 且消耗 1 字节 / An illegal lead byte yields `0xFFFD` and consumes one byte
 */
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

/**
 * @brief 取某位置之前的码点 / Get the code point immediately before a position
 *
 * @param[in]     s 主题字节串 / Subject byte string
 * @param[in]     pos 当前位置字节偏移 / Current byte offset
 *
 * @return 前一个码点；位于串首时返回 0 / The previous code point, or 0 at the start of the subject
 *
 * @note 回溯跳过 UTF-8 续字节以定位字符起点 / Continuation bytes are skipped backwards to find the character start
 */
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

/**
 * @brief 把 ASCII 大写字母转小写 / Lower-case an ASCII uppercase letter
 *
 * @param[in]     a 输入字符 / Input character
 *
 * @return 小写字符；非大写字母原样返回 / The lower-case character, or the input unchanged when it is not uppercase
 */
static char re_lower(char a)
{
    return (a >= 'A' && a <= 'Z') ? (char)(a - 'A' + 'a') : a;
}

/**
 * @brief 判断单码点节点是否匹配该位置 / Test whether a single-code-point node matches at a position
 *
 * @details `RE_CHAR` 比较码点（`i` 标志下做 ASCII 折叠），`RE_ANY` 默认不匹配换行（`s` 标志下匹配），`RE_CLASS` 查区间并按 `negate` 取反。
 *          `RE_CHAR` compares code points with ASCII folding under `i`, `RE_ANY` rejects newlines unless `s` is set and `RE_CLASS` scans the ranges, inverted when `negate` is set.
 *
 * @param[in,out] n 模式节点（`RE_CHAR`/`RE_ANY`/`RE_CLASS`）/ Pattern node, one of `RE_CHAR`, `RE_ANY`, `RE_CLASS`
 * @param[in,out] c 匹配上下文 / Match context
 * @param[in]     pos 待匹配字节偏移 / Byte offset to match at
 * @param[out]    adv 接收匹配字符的字节宽度 / Receives the matched character's byte width
 *
 * @return 匹配返回 1，否则返回 0 / 1 on a match, otherwise 0
 */
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

/**
 * @brief 判断是否处于行尾 / Test whether the position is at an end of line
 *
 * @param[in,out] c 匹配上下文 / Match context
 * @param[in]     pos 字节偏移 / Byte offset
 *
 * @return 串尾或（`m` 标志下）换行处返回 1 / 1 at the end of the subject or, under `m`, at a newline character
 */
static int re_at_eol(ReCtx *c, size_t pos)
{
    if (pos >= c->len)
        return 1;
    return (c->flags & 2) && c->s[pos] == '\n';
}

/**
 * @brief 零初始化一个续延内核并链接后继 / Zero-initialise a continuation kernel and link it to the next one
 *
 * @param[out]    k 待初始化的续延内核 / The continuation kernel to initialise
 * @param[in]     kind 内核种类（`K_END`/`K_SEQ`/`K_REP`/`K_GROUP`）/ Kernel kind, one of `K_END`, `K_SEQ`, `K_REP`, `K_GROUP`
 * @param[in,out] next 后继内核，可为 `NULL` / The following kernel, may be `NULL`
 *
 * @note 共享的字段默认值把 `target` 置为 -1，调用方只需覆盖各自用到的字段 / Shared defaults set `target` to -1 so callers only fill their own fields
 */
static void re_k_init(ReK *k, int kind, ReK *next)
{
    memset(k, 0, sizeof(*k));
    k->kind = kind;
    k->target = -1;
    k->next = next;
}

/**
 * @brief 推进序列内核到下一个元素 / Advance a sequence kernel to its next element
 *
 * @param[in,out] k 续延内核（`K_SEQ`）/ The continuation kernel, `K_SEQ`
 * @param[in,out] c 匹配上下文 / Match context
 * @param[in]     pos 当前字节偏移 / Current byte offset
 *
 * @return 匹配成功返回 1 / 1 when the match succeeds
 *
 * @note 序列耗尽后转交给后继内核 / Hands off to the following kernel once the sequence is exhausted
 */
static int re_cont_seq(ReK *k, ReCtx *c, size_t pos)
{
    ReK nk;

    if (k->idx >= k->seq->n)
        return re_cont(k->next, c, pos);
    re_k_init(&nk, K_SEQ, k->next);
    nk.seq = k->seq;
    nk.idx = k->idx + 1;
    return re_node(k->seq->kids[k->idx], &nk, c, pos);
}

/**
 * @brief 捕获组的续延：记录区间，失败时回滚 / Continuation for a captured group: record the span and roll it back on failure
 *
 * @param[in,out] k 续延内核（`K_GROUP`）/ The continuation kernel, `K_GROUP`
 * @param[in,out] c 匹配上下文 / Match context
 * @param[in]     pos 当前字节偏移 / Current byte offset
 *
 * @return 匹配成功返回 1 / 1 when the match succeeds
 *
 * @note 后续匹配失败时恢复该组原有的区间 / The group's previous span is restored when the continuation fails
 */
static int re_cont_group(ReK *k, ReCtx *c, size_t pos)
{
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

/**
 * @brief 量词内核的续延：贪婪先试下一轮、懒惰先试余下模式 / Continuation for a repetition kernel: greedy retries, lazy tries the rest first
 *
 * @param[in,out] k 续延内核（`K_REP`）/ The continuation kernel, `K_REP`
 * @param[in,out] c 匹配上下文 / Match context
 * @param[in]     pos 当前字节偏移 / Current byte offset
 *
 * @return 匹配成功返回 1 / 1 when the match succeeds
 *
 * @note `progressed` 防止无进展的循环体（如空匹配的 `*`）无限重复 / A progress guard stops an unbounded `*` over a body that consumes nothing from spinning forever
 */
static int re_cont_rep(ReK *k, ReCtx *c, size_t pos)
{
    ReNode *rep = k->rep;
    int count = k->count;
    int progressed = (count == 0) || (pos != k->at);

    if (rep->greedy) {
        if (progressed && count < rep->max) {
            ReK rk;
            re_k_init(&rk, K_REP, k->next);
            rk.rep = rep;
            rk.count = count + 1;
            rk.at = pos;
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
        re_k_init(&rk, K_REP, k->next);
        rk.rep = rep;
        rk.count = count + 1;
        rk.at = pos;
        return re_node(rep->child, &rk, c, pos);
    }
    return 0;
}

/**
 * @brief 续延分发：按内核种类恢复量词/分支 / Continuation dispatch: resume the scheduled quantifiers or branches by kernel kind
 *
 * @param[in,out] k 续延内核，`NULL` 表示整段匹配成功 / The continuation kernel, `NULL` meaning the whole match succeeded
 * @param[in,out] c 匹配上下文 / Match context
 * @param[in]     pos 当前字节偏移 / Current byte offset
 *
 * @return 整体匹配成功返回 1 / 1 when the overall match succeeds
 *
 * @note 内核为 `NULL` 时把 `match_end` 置为该位置并返回成功 / A `NULL` kernel records `match_end` and reports success
 */
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
    case K_SEQ:
        return re_cont_seq(k, c, pos);
    case K_GROUP:
        return re_cont_group(k, c, pos);
    case K_REP:
        return re_cont_rep(k, c, pos);
    default:
        return 0;
    }
}

/**
 * @brief 单码点循环体的量词快速路径 / Fast path for quantifiers whose body consumes a single code point
 *
 * @details 先迭代收集每个可能的停止位置，再按贪婪（从多到少）或懒惰（从少到多）依次尝试余下模式，从而保持递归深度较浅。
 *          Iteratively collects every reachable stop position, then tries the rest of the pattern greedily from most to least or lazily from least to most, keeping recursion shallow.
 *
 * @param[in,out] rep 重复节点 / Quantifier node
 * @param[in,out] k 后继续延内核 / The following continuation kernel
 * @param[in,out] c 匹配上下文 / Match context
 * @param[in]     pos 起始字节偏移 / Starting byte offset
 *
 * @return 匹配成功返回 1 / 1 when the match succeeds
 */
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

/**
 * @brief 匹配反向引用 `\1`..`\9` / Match a back-reference `\1`..`\9`
 *
 * @param[in,out] n 反向引用节点 / The back-reference node
 * @param[in,out] k 后继续延内核 / The following continuation kernel
 * @param[in,out] c 匹配上下文 / Match context
 * @param[in]     pos 当前字节偏移 / Current byte offset
 *
 * @return 匹配成功返回 1 / 1 when the match succeeds
 *
 * @note 被引用组未参与匹配时按空串处理；`i` 标志下逐字节做 ASCII 折叠 / An unset group matches the empty string; under `i` the bytes are ASCII-folded
 */
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

/**
 * @brief 零宽断言：前瞻或后顾 / Zero-width assertion: look-ahead or look-behind
 *
 * @details 前瞻用 `K_END` 直接在该位置匹配；后顾尝试所有起始位置直到结束点等于 `pos`；捕获组保存与 `match_end` 在失败时恢复。
 *          A look-ahead matches at the position with a `K_END` kernel; a look-behind tries every start offset until the end lands on `pos`; captures and `match_end` are restored on failure.
 *
 * @param[in,out] n 环视节点（`RE_LOOK` 或 `RE_LBEHIND`）/ The look-around node, `RE_LOOK` or `RE_LBEHIND`
 * @param[in,out] c 匹配上下文 / Match context
 * @param[in]     pos 断言位置字节偏移 / Byte offset of the assertion
 *
 * @return 断言通过返回 1（否定断言取反）/ 1 when the assertion holds, inverted for a negative one
 */
static int re_assert(ReNode *n, ReCtx *c, size_t pos)
{
    int saved[RE_MAX_GROUPS * 2];
    int nc = c->ncaps < RE_MAX_GROUPS * 2 ? c->ncaps : RE_MAX_GROUPS * 2;
    int saved_end = c->match_end;
    int hit = 0;

    memcpy(saved, c->caps, (size_t)nc * sizeof(int));
    if (n->type == RE_LOOK) {
        ReK e;
        re_k_init(&e, K_END, NULL);
        hit = re_node(n->child, &e, c, pos);
    } else {
        size_t start;
        for (start = 0; start <= pos; start++) {
            ReK e;
            re_k_init(&e, K_END, NULL);
            e.target = (int)pos;
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

/**
 * @brief 匹配序列节点，并把余下元素排进 `K_SEQ` 内核 / Match a sequence node, scheduling the remaining elements through a `K_SEQ` kernel
 *
 * @param[in,out] n 序列节点 / Sequence node
 * @param[in,out] k 后继续延内核 / The following continuation kernel
 * @param[in,out] c 匹配上下文 / Match context
 * @param[in]     pos 当前字节偏移 / Current byte offset
 *
 * @return 匹配成功返回 1 / 1 when the match succeeds
 *
 * @note 空序列直接转交后继内核 / An empty sequence falls straight through to the following kernel
 */
static int re_run_seq(ReNode *n, ReK *k, ReCtx *c, size_t pos)
{
    ReK nk;

    if (n->n == 0)
        return re_cont(k, c, pos);
    re_k_init(&nk, K_SEQ, k);
    nk.seq = n;
    nk.idx = 1;
    return re_node(n->kids[0], &nk, c, pos);
}

/**
 * @brief 依次尝试并集的各个分支 / Try each alternation branch in order
 *
 * @param[in,out] n 并集节点（`RE_ALT`）/ The alternation node, `RE_ALT`
 * @param[in,out] k 后继续延内核 / The following continuation kernel
 * @param[in,out] c 匹配上下文 / Match context
 * @param[in]     pos 当前字节偏移 / Current byte offset
 *
 * @return 某分支成功返回 1 / 1 when one branch succeeds
 */
static int re_run_alt(ReNode *n, ReK *k, ReCtx *c, size_t pos)
{
    int i;

    for (i = 0; i < n->n; i++)
        if (re_node(n->kids[i], k, c, pos))
            return 1;
    return 0;
}

/**
 * @brief 匹配量词节点 / Match a quantifier node
 *
 * @param[in,out] n 量词节点（`RE_REP`）/ The quantifier node, `RE_REP`
 * @param[in,out] k 后继续延内核 / The following continuation kernel
 * @param[in,out] c 匹配上下文 / Match context
 * @param[in]     pos 当前字节偏移 / Current byte offset
 *
 * @return 匹配成功返回 1 / 1 when the match succeeds
 *
 * @note 最大次数为 0 时按最小次数直接成功或失败；单码点循环体走 `re_rep_simple` 快速路径，否则用 `K_REP` 内核 / A zero maximum succeeds or fails per the minimum count; single-code-point bodies take the `re_rep_simple` fast path and otherwise a `K_REP` kernel is used
 */
static int re_run_rep(ReNode *n, ReK *k, ReCtx *c, size_t pos)
{
    ReNode *sub = n->child;
    ReK rk;

    if (n->max == 0)
        return (n->min == 0) ? re_cont(k, c, pos) : 0;
    if (sub->type == RE_CHAR || sub->type == RE_ANY || sub->type == RE_CLASS)
        return re_rep_simple(n, k, c, pos);
    re_k_init(&rk, K_REP, k);
    rk.rep = n;
    rk.at = pos;
    return re_cont(&rk, c, pos);
}

/**
 * @brief 匹配分组节点，捕获组记录起止区间 / Match a group node, recording the span for capturing groups
 *
 * @param[in,out] n 分组节点（`RE_GROUP`）/ The group node, `RE_GROUP`
 * @param[in,out] k 后继续延内核 / The following continuation kernel
 * @param[in,out] c 匹配上下文 / Match context
 * @param[in]     pos 当前字节偏移 / Current byte offset
 *
 * @return 匹配成功返回 1 / 1 when the match succeeds
 *
 * @note 非捕获组仅递归子节点 / A non-capturing group just recurses into its child
 */
static int re_run_group(ReNode *n, ReK *k, ReCtx *c, size_t pos)
{
    int g = n->group;
    ReK gk;

    if (g <= 0)
        return re_node(n->child, k, c, pos);
    re_set_cap(c, g, -1, -1);
    re_k_init(&gk, K_GROUP, k);
    gk.group = g;
    gk.gstart = pos;
    return re_node(n->child, &gk, c, pos);
}

/**
 * @brief 匹配词边界断言 `\b` / `\B` / Match the word-boundary assertion `\b` or `\B`
 *
 * @param[in,out] n 词边界节点（`RE_WORDB` 或 `RE_NWORDB`）/ The word-boundary node, `RE_WORDB` or `RE_NWORDB`
 * @param[in,out] k 后继续延内核 / The following continuation kernel
 * @param[in,out] c 匹配上下文 / Match context
 * @param[in]     pos 当前字节偏移 / Current byte offset
 *
 * @return 断言通过返回 1 / 1 when the assertion holds
 *
 * @note 比较该位置前后是否为单词字符，`\B` 取相反条件 / Compares word-character status before and after the position, with `\B` negating the condition
 */
static int re_run_wordb(ReNode *n, ReK *k, ReCtx *c, size_t pos)
{
    uint32_t cp = 0;
    int before = re_is_word(re_prev_cp(c->s, pos));
    int after;

    if (pos < c->len)
        re_decode_at(c->s, c->len, pos, &cp);
    after = pos < c->len && re_is_word(cp);
    if ((n->type == RE_WORDB) == (before != after))
        return re_cont(k, c, pos);
    return 0;
}

/**
 * @brief 匹配 `^` / `$` 锚点 / Match the `^` and `$` anchors
 *
 * @param[in,out] n 锚点节点（`RE_BOL` 或 `RE_EOL`）/ The anchor node, `RE_BOL` or `RE_EOL`
 * @param[in,out] k 后继续延内核 / The following continuation kernel
 * @param[in,out] c 匹配上下文 / Match context
 * @param[in]     pos 当前字节偏移 / Current byte offset
 *
 * @return 锚点成立返回 1 / 1 when the anchor holds
 *
 * @note 多行模式下 `^` 也匹配换行之后、`$` 匹配换行之前 / Under multi-line mode `^` also matches after a newline and `$` before one
 */
static int re_run_anchor(ReNode *n, ReK *k, ReCtx *c, size_t pos)
{
    if (n->type == RE_BOL) {
        if (pos == 0 || ((c->flags & 2) && pos > 0 && c->s[pos - 1] == '\n'))
            return re_cont(k, c, pos);
        return 0;
    }
    return re_at_eol(c, pos) ? re_cont(k, c, pos) : 0;
}

/**
 * @brief 匹配并消费一个单码点节点 / Match and consume a single-code-point node
 *
 * @param[in,out] n 单码点节点（`RE_CHAR`/`RE_ANY`/`RE_CLASS`）/ A single-code-point node, one of `RE_CHAR`, `RE_ANY`, `RE_CLASS`
 * @param[in,out] k 后继续延内核 / The following continuation kernel
 * @param[in,out] c 匹配上下文 / Match context
 * @param[in]     pos 当前字节偏移 / Current byte offset
 *
 * @return 匹配成功返回 1 / 1 when the match succeeds
 *
 * @note 成功后按匹配字符的字节宽度推进 / The cursor advances by the matched character's byte width
 */
static int re_run_single_node(ReNode *n, ReK *k, ReCtx *c, size_t pos)
{
    int adv = 0;

    if (re_single(n, c, pos, &adv))
        return re_cont(k, c, pos + (size_t)adv);
    return 0;
}

/**
 * @brief 匹配单个模式节点的统一入口 / Unified entry point for matching one pattern node
 *
 * @details 先递增步数与深度计数，超过 `RE_MAX_STEPS`/`RE_MAX_DEPTH` 即放弃以防回溯爆炸，再按节点类型分派到各 `re_run_*`。
 *          Step and depth counters are bumped first — exceeding `RE_MAX_STEPS` or `RE_MAX_DEPTH` aborts to bound backtracking — then the node type dispatches to the matching `re_run_*` helper.
 *
 * @param[in,out] n 模式节点 / Pattern node
 * @param[in,out] k 后继续延内核 / The following continuation kernel
 * @param[in,out] c 匹配上下文 / Match context
 * @param[in]     pos 当前字节偏移 / Current byte offset
 *
 * @return 匹配成功返回 1 / 1 when the match succeeds
 */
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
    case RE_EOL:
        r = re_run_anchor(n, k, c, pos);
        break;
    case RE_CHAR:
    case RE_ANY:
    case RE_CLASS:
        r = re_run_single_node(n, k, c, pos);
        break;
    case RE_SEQ:
        r = re_run_seq(n, k, c, pos);
        break;
    case RE_ALT:
        r = re_run_alt(n, k, c, pos);
        break;
    case RE_REP:
        r = re_run_rep(n, k, c, pos);
        break;
    case RE_GROUP:
        r = re_run_group(n, k, c, pos);
        break;
    case RE_LOOK:
    case RE_LBEHIND:
        if (re_assert(n, c, pos))
            r = re_cont(k, c, pos);
        break;
    case RE_WORDB:
    case RE_NWORDB:
        r = re_run_wordb(n, k, c, pos);
        break;
    case RE_BACKREF:
        r = re_backref(n, k, c, pos);
        break;
    default:
        break;
    }
    c->depth--;
    return r;
}

/**
 * @brief 返回正则的捕获组个数 / Return the number of capture groups
 *
 * @param[in,out] re 正则对象，可为 `NULL` / Regex object, may be `NULL`
 *
 * @return 捕获组个数，`re` 为 `NULL` 时返回 0 / The capture group count, or 0 when `re` is `NULL`
 */
int hpe_regex_group_count(HpeRegex *re)
{
    return re ? re->ngroups : 0;
}

/**
 * @brief 从标志串解析 `/i /m /s` 开关 / Parse the `/i /m /s` switches from a flag string
 *
 * @param[in,out] re 目标正则对象，可为 `NULL` / Target regex object, may be `NULL`
 * @param[in]     flags 标志串，可为 `NULL` / Flag string, may be `NULL`
 *
 * @note 先清零再逐字符置位（`i`=1、`m`=2、`s`=4）；`g` 由调用方处理 / The bit mask is cleared first, then `i`=1, `m`=2 and `s`=4 are set per character; `g` is handled by the caller
 */
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

/**
 * @brief 编译正则模式串为匹配程序 / Compile a regex pattern into a matcher program
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     pattern 模式串，`NULL` 视为空模式 / Pattern string, `NULL` treated as empty
 *
 * @return arena 中新建的正则对象 / The new regex object in the arena
 *
 * @note 标志位初始为 0，需另行调用 `hpe_regex_set_flags` / Flags start at 0 and must be set later with `hpe_regex_set_flags`
 */

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

/**
 * @brief 从指定位置起搜索主题串 / Search the subject starting from a given offset
 *
 * @details `captures[0]/[1]` 为整段匹配，`[2+2g]/[3+2g]` 为第 g 个捕获组；未参与匹配的组置 -1。
 *          `captures[0]`/`[1]` describe the whole match and `[2+2g]`/`[3+2g]` the g-th group; groups that did not participate are set to -1.
 *
 * @param[in,out] re 已编译的正则对象 / A compiled regex object
 * @param[in]     subject 主题串，`NULL` 视为空串 / Subject string, `NULL` treated as empty
 * @param[in]     start 起始字节偏移（负值夹为 0）/ Start byte offset, clamped to 0 when negative
 * @param[out]    captures 接收捕获组的字节偏移数组 / Receives the capture byte-offset array
 * @param[in]     ncaps `captures` 的可用长度 / Capacity of `captures`
 *
 * @return 匹配则返回匹配结束后的字节偏移，否则返回 -1 / The byte offset just past the match, or -1 when nothing matches
 *
 * @note 非多行模式下顶层 `^` 只可能在偏移 0 匹配，越过后提前退出 / A top-level `^` can only match at offset 0 outside multi-line mode, so the loop stops once past it
 */
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

/**
 * @brief 判断主题串中是否存在匹配 / Test whether the subject contains a match
 *
 * @param[in,out] re 已编译的正则对象 / A compiled regex object
 * @param[in]     subject 主题串 / Subject string
 *
 * @return 找到匹配返回 1 / 1 when a match is found
 *
 * @note 内部用两个槽位接收整段匹配区间 / Uses a two-slot array to receive the whole-match span
 */
int hpe_regex_match(HpeRegex *re, const char *subject)
{
    int caps[2];
    return hpe_regex_search(re, subject, 0, caps, 2) >= 0;
}
