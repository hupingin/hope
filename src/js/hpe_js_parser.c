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

/**
 * @brief 向词法分析器追加一个词法单元 / Append one token to the lexer
 *
 * @param[in,out] lx 词法分析器 / Lexer
 * @param[in]     type 词法单元类型（`T_*`）/ Token type (`T_*`)
 * @param[in]     num 数值载荷（数字字面量用）/ Numeric payload (for number literals)
 * @param[in]     text 词法单元文本；`NULL` 表示无文本 / Token text, or `NULL` for none
 * @param[in]     textlen 文本长度 / Text length
 * @param[in]     line 所在行号 / Source line
 *
 * @note 文本被复制到 js arena；容量不足时表翻倍并复制旧内容 / The text is copied into the JS arena; the table doubles and copies when full
 *
 * @see lex_all
 */
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

/**
 * @brief 判断字符能否作为标识符首字符 / Test whether a character can start an identifier
 *
 * @param[in]     c 待判断的字符 / Character to test
 *
 * @return 可作首字符返回 1，否则 0 / 1 when it may start an identifier, otherwise 0
 *
 * @note 放行非 ASCII，使标识符可为中文 / Non-ASCII is allowed so identifiers may be Chinese
 */
static int lex_ident_start(char c)
{
    /* allow non-ASCII so identifiers can be Chinese */
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' ||
           (unsigned char)c >= 0x80;
}

/**
 * @brief 判断字符能否作为标识符后续字符 / Test whether a character may continue an identifier
 *
 * @param[in]     c 待判断的字符 / Character to test
 *
 * @return 可作后续字符返回 1，否则 0 / 1 when it may continue an identifier, otherwise 0
 *
 * @see lex_ident_start
 */
static int lex_ident_char(char c)
{
    return lex_ident_start(c) || (c >= '0' && c <= '9');
}

/* ---------------- string literal ---------------- */

/**
 * @brief 求十六进制数位的值 / Convert a hex digit to its value
 *
 * @param[in]     c 待转换的字符 / Character to convert
 *
 * @return 0..15 的数位值；非十六进制字符返回 -1 / The digit value 0..15, or -1 when `c` is not a hex digit
 */
static int lex_hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/**
 * @brief 从游标处读取至多 max 个十六进制数位 / Read up to `max` hex digits from the cursor
 *
 * @param[in,out] lx 词法分析器 / Lexer
 * @param[in]     max 最多读取的数位个数 / Maximum number of digits to read
 *
 * @return 读到的数值（无有效数位为 0）/ The accumulated value (0 when no digit is read)
 *
 * @note 游标停在第一个非十六进制字符处 / The cursor stops at the first non-hex character
 */
static uint32_t lex_read_hex(Lexer *lx, int max)
{
    uint32_t v = 0;
    int i;

    for (i = 0; i < max && lx->pos < lx->len; i++) {
        int d = lex_hex_digit(lx->src[lx->pos]);
        if (d < 0)
            break;
        v = v * 16 + (uint32_t)d;
        lx->pos++;
    }
    return v;
}

/**
 * @brief 返回固定单字符转义对应的字符 / Map a fixed single-character escape to its character
 *
 * @param[in]     e 转义字母（反斜杠之后）/ The escape letter (after the backslash)
 *
 * @return 对应字符码；需要额外输入时返回 -1 / The character code, or -1 for escapes needing more input
 *
 * @note 覆盖 `\n \t \r \b \f \v \0` / Covers `\n`, `\t`, `\r`, `\b`, `\f`, `\v` and `\0`
 */
static int lex_escape_simple(char e)
{
    switch (e) {
    case 'n':
        return '\n';
    case 't':
        return '\t';
    case 'r':
        return '\r';
    case 'b':
        return '\b';
    case 'f':
        return '\f';
    case 'v':
        return '\v';
    case '0':
        return '\0';
    default:
        return -1;
    }
}

/**
 * @brief 解码 `\uXXXX` 或 `\u{XXXXX}` 转义 / Decode a `\uXXXX` or `\u{XXXXX}` escape
 *
 * @param[in,out] lx 词法分析器 / Lexer
 * @param[out]    buf 接收 UTF-8 编码的缓冲区 / Buffer receiving the UTF-8 encoding
 * @param[in,out] n 已写入的字节数 / Bytes already written
 * @param[in]     cap 缓冲区容量 / Buffer capacity
 *
 * @note 空间不足时静默丢弃；写入 `hpe_utf8_encode` 的结果 / Silently discards when the buffer is full; writes the output of `hpe_utf8_encode`
 */
static void lex_unicode_escape(Lexer *lx, char *buf, size_t *n, size_t cap)
{
    uint32_t cp;
    char utf[4];
    int m;

    if (lx->pos < lx->len && lx->src[lx->pos] == '{') {
        lx->pos++;
        cp = lex_read_hex(lx, 64);
        if (lx->pos < lx->len)
            lx->pos++; /* } */
    } else {
        cp = lex_read_hex(lx, 4);
    }
    m = hpe_utf8_encode(cp, utf);
    if (*n + 4 < cap) {
        memcpy(buf + *n, utf, (size_t)m);
        *n += (size_t)m;
    }
}

/**
 * @brief 解码一个转义序列 / Decode one escape sequence
 *
 * @param[in,out] lx 词法分析器 / Lexer
 * @param[out]    buf 接收解码结果的缓冲区 / Buffer receiving the decoded bytes
 * @param[in,out] n 已写入的字节数 / Bytes already written
 * @param[in]     cap 缓冲区容量 / Buffer capacity
 *
 * @note 前导反斜杠已由调用方消费；支持 `\xHH`、`\u` 与行续接 / The leading backslash is already consumed; supports `\xHH`, `\u` and line continuation
 *
 * @see lex_unicode_escape, lex_escape_simple
 */
static void lex_escape(Lexer *lx, char *buf, size_t *n, size_t cap)
{
    char e = lx->src[lx->pos];
    int simple;

    lx->pos++;
    if (e == 'x') {
        buf[(*n)++] = (char)lex_read_hex(lx, 2);
        return;
    }
    if (e == 'u') {
        lex_unicode_escape(lx, buf, n, cap);
        return;
    }
    if (e == '\n') {
        lx->line++;
        return;
    }
    if (e == '\r')
        return;
    simple = lex_escape_simple(e);
    buf[(*n)++] = (char)(simple < 0 ? e : simple);
}

/**
 * @brief 读取一个字符串字面量 / Scan a string literal
 *
 * @param[in,out] lx 词法分析器 / Lexer
 * @param[in]     quote 引号字符（`"` 或 `'`）/ The quote character (`"` or `'`)
 *
 * @note 结果作为 `T_STR` 词法单元加入；未闭合时报告语法错误 / The result is added as a `T_STR` token; an unterminated literal raises an error
 *
 * @see lex_escape, lex_add
 */
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
            lx->pos++;
            lex_escape(lx, buf, &n, cap);
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

/**
 * @brief 读取十六进制数字字面量 / Scan a hexadecimal numeric literal
 *
 * @param[in,out] lx 词法分析器 / Lexer
 * @param[out]    buf 接收数字字符的缓冲区 / Buffer receiving the digit characters
 * @param[in]     cap 缓冲区容量 / Buffer capacity
 * @param[in,out] n 已写入的字符数 / Characters already written
 *
 * @return 解析出的浮点数值 / The parsed floating-point value
 *
 * @note 跳过 `_` 分隔符；`0x` 前缀由调用方消费 / `_` separators are skipped; the caller consumes the `0x` prefix
 */
static double lex_hex_number(Lexer *lx, char *buf, size_t cap, size_t *n)
{
    lx->pos += 2; /* 0x */
    while (lx->pos < lx->len && *n < cap - 1) {
        char c = lx->src[lx->pos];
        if (c == '_') {
            lx->pos++;
            continue;
        }
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
            buf[(*n)++] = c;
            lx->pos++;
        } else {
            break;
        }
    }
    buf[*n] = '\0';
    return (double)strtoll(buf, NULL, 16);
}

/**
 * @brief 读取二进制或八进制数字字面量 / Scan a binary or octal numeric literal
 *
 * @param[in,out] lx 词法分析器 / Lexer
 * @param[in]     radix 进制（2 或 8）/ The radix (2 or 8)
 *
 * @return 解析出的浮点数值 / The parsed floating-point value
 *
 * @note 跳过 `_` 分隔符；`0b`/`0o` 前缀由本函数消费 / `_` separators are skipped; this function consumes the `0b`/`0o` prefix
 */
static double lex_radix_number(Lexer *lx, int radix)
{
    unsigned long long acc = 0;

    lx->pos += 2; /* 0b / 0o */
    while (lx->pos < lx->len) {
        char c = lx->src[lx->pos];
        if (c == '_') {
            lx->pos++;
            continue;
        }
        if (c >= '0' && c < (char)('0' + radix)) {
            acc = acc * (unsigned)radix + (unsigned)(c - '0');
            lx->pos++;
        } else {
            break;
        }
    }
    return (double)(long long)acc;
}

/**
 * @brief 读取十进制/小数/指数形式的数字 / Scan a decimal, fractional or exponent number
 *
 * @param[in,out] lx 词法分析器 / Lexer
 * @param[out]    buf 接收数字字符的缓冲区 / Buffer receiving the digit characters
 * @param[in]     cap 缓冲区容量 / Buffer capacity
 * @param[in,out] n 已写入的字符数 / Characters already written
 *
 * @return 解析出的浮点数值 / The parsed floating-point value
 *
 * @note 跳过 `_` 分隔符；用 `strtod` 转换 / `_` separators are skipped; the value is parsed with `strtod`
 */
static double lex_decimal_number(Lexer *lx, char *buf, size_t cap, size_t *n)
{
    while (lx->pos < lx->len && *n < cap - 1 &&
           ((lx->src[lx->pos] >= '0' && lx->src[lx->pos] <= '9') ||
            lx->src[lx->pos] == '.' || lx->src[lx->pos] == '_')) {
        if (lx->src[lx->pos] != '_')
            buf[(*n)++] = lx->src[lx->pos];
        lx->pos++;
    }
    if (lx->pos < lx->len && (lx->src[lx->pos] == 'e' || lx->src[lx->pos] == 'E') &&
        *n < cap - 4) {
        buf[(*n)++] = lx->src[lx->pos++];
        if (lx->pos < lx->len && (lx->src[lx->pos] == '+' || lx->src[lx->pos] == '-'))
            buf[(*n)++] = lx->src[lx->pos++];
        while (lx->pos < lx->len && lx->src[lx->pos] >= '0' && lx->src[lx->pos] <= '9' &&
               *n < cap - 1)
            buf[(*n)++] = lx->src[lx->pos++];
    }
    buf[*n] = '\0';
    return strtod(buf, NULL);
}

/**
 * @brief 读取任意形式的数字字面量 / Scan a numeric literal of any form
 *
 * @param[in,out] lx 词法分析器 / Lexer
 *
 * @note 按 `0x`/`0b`/`0o` 前缀分派；容忍 BigInt 后缀 `n`（仍按 double 存储）/ Dispatches on the `0x`/`0b`/`0o` prefix; tolerates a trailing BigInt `n` (still stored as a double)
 *
 * @see lex_hex_number, lex_radix_number, lex_decimal_number
 */
static void lex_number(Lexer *lx)
{
    char buf[80];
    size_t n = 0;
    int start_line = lx->line;
    double v;
    char c0 = lx->src[lx->pos];

    if (c0 == '0' && lx->pos + 1 < lx->len &&
        (lx->src[lx->pos + 1] == 'x' || lx->src[lx->pos + 1] == 'X'))
        v = lex_hex_number(lx, buf, sizeof(buf), &n);
    else if (c0 == '0' && lx->pos + 1 < lx->len &&
             (lx->src[lx->pos + 1] == 'b' || lx->src[lx->pos + 1] == 'B'))
        v = lex_radix_number(lx, 2);
    else if (c0 == '0' && lx->pos + 1 < lx->len &&
             (lx->src[lx->pos + 1] == 'o' || lx->src[lx->pos + 1] == 'O'))
        v = lex_radix_number(lx, 8);
    else
        v = lex_decimal_number(lx, buf, sizeof(buf), &n);
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

/**
 * @brief 判断当前位置的 `/` 是否开启正则字面量 / Decide whether a `/` here starts a regex literal
 *
 * @param[in,out] lx 词法分析器 / Lexer
 *
 * @return 为正则返回 1，为除号返回 0 / 1 for a regex, 0 when it is a division operator
 *
 * @note 依据上一词法单元判断：表达式结束处或 `)`/`]`/`}`/`++`/`--` 之后是除号，`return`/`typeof` 等关键字之后是正则 / Decides from the previous token: after an expression end or `)`/`]`/`}`/`++`/`--` it is division, after `return`/`typeof` and similar keywords it is a regex
 */
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

/**
 * @brief 扫描 `/…/flags` 的正则体 / Scan the body of a `/…/flags` regex
 *
 * @param[in,out] lx 词法分析器 / Lexer
 * @param[in]     start_line 正则起始行号（用于报错）/ Line where the regex starts (for errors)
 *
 * @return 闭合 `/` 的下标；未闭合时报告错误并返回 `lx->len` / The index of the closing `/`, or reports an error and returns `lx->len`
 *
 * @note 跟踪 `[...]` 字符类与 `\` 转义 / Tracks `[...]` character classes and `\` escapes
 */
static size_t lex_regex_body(Lexer *lx, int start_line)
{
    size_t i = lx->pos + 1; /* past the opening slash */
    int in_class = 0;

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
            return i;
        if (c == '\n') {
            js_error(lx->js, start_line, "unterminated regular expression");
            return lx->len;
        }
        i++;
    }
    js_error(lx->js, start_line, "unterminated regular expression");
    return lx->len;
}

/**
 * @brief 把正则的模式与标志存成一个词法单元 / Store a regex pattern and flags as one token
 *
 * @param[in,out] lx 词法分析器 / Lexer
 * @param[in]     end 闭合 `/` 的下标 / Index of the closing `/`
 * @param[in]     start_line 正则起始行号 / Line where the regex starts
 *
 * @note 以 `pattern\0flags` 形式存为 `T_REGEX`，并推进游标 / Stored as `T_REGEX` in the form `pattern\0flags`; the cursor is advanced
 *
 * @see lex_regex_body
 */
static void lex_regex_emit(Lexer *lx, size_t end, int start_line)
{
    size_t plen = end - (lx->pos + 1);
    size_t j = end + 1;
    size_t flen;
    char *buf;

    while (j < lx->len && lex_ident_char(lx->src[j]))
        j++;
    flen = j - (end + 1);
    buf = (char *)malloc(plen + flen + 2);
    if (!buf)
        js_error(lx->js, start_line, "out of memory");
    memcpy(buf, lx->src + lx->pos + 1, plen);
    buf[plen] = '\0';
    memcpy(buf + plen + 1, lx->src + end + 1, flen);
    buf[plen + 1 + flen] = '\0';
    lex_add(lx, T_REGEX, 0, buf, plen + 1 + flen, start_line);
    free(buf);
    lx->pos = j;
}

/**
 * @brief 读取一个正则字面量 / Scan a regex literal
 *
 * @param[in,out] lx 词法分析器 / Lexer
 *
 * @note 未闭合时直接返回而不产生词法单元 / Returns without emitting a token when unterminated
 *
 * @see lex_regex_body, lex_regex_emit
 */
static void lex_regex(Lexer *lx)
{
    int start_line = lx->line;
    size_t end = lex_regex_body(lx, start_line);

    if (end >= lx->len)
        return;
    lex_regex_emit(lx, end, start_line);
}

static size_t scan_template(Lexer *lx, size_t i, int *lines);

/**
 * @brief 跳过一段引号字符串并统计换行 / Skip a quoted string while counting newlines
 *
 * @param[in,out] lx 词法分析器 / Lexer
 * @param[in]     j 起始下标（指向引号）/ Start index (pointing at the quote)
 * @param[in]     q 引号字符 / The quote character
 * @param[in,out] lines 累计的换行数 / Running newline count
 *
 * @return 越过闭合引号的下标 / The index just past the closing quote
 */
static size_t scan_skip_string(Lexer *lx, size_t j, char q, int *lines)
{
    j++;
    while (j < lx->len && lx->src[j] != q) {
        if (lx->src[j] == '\\')
            j++;
        if (j < lx->len && lx->src[j] == '\n')
            (*lines)++;
        j++;
    }
    return j + 1;
}

/**
 * @brief 越过一段 `${ … }` 替换体 / Advance past one `${ … }` substitution body
 *
 * @param[in,out] lx 词法分析器 / Lexer
 * @param[in]     j 起始下标 / Start index
 * @param[in,out] lines 累计的换行数 / Running newline count
 *
 * @return 越过闭合 `}` 的下标 / The index just past the closing `}`
 *
 * @note 处理嵌套花括号、字符串、模板与注释 / Handles nested braces, strings, templates and comments
 */
static size_t scan_skip_substitution(Lexer *lx, size_t j, int *lines)
{
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
            j = scan_skip_string(lx, j, d, lines);
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
    return j;
}

/**
 * @brief 扫描一个模板字面量 / Scan a template literal
 *
 * @param[in,out] lx 词法分析器 / Lexer
 * @param[in]     i 起始下标（指向反引号）/ Start index (pointing at the backtick)
 * @param[in,out] lines 累计的换行数 / Running newline count
 *
 * @return 越过闭合反引号的下标（出错时为 `len`）/ The index just past the closing backtick (`len` on error)
 *
 * @note 处理嵌套模板、`${}` 替换、字符串与注释 / Handles nested templates, `${}` substitutions, strings and comments
 *
 * @see scan_skip_substitution
 */
static size_t scan_template(Lexer *lx, size_t i, int *lines)
{
    i++; /* opening backtick */
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
            i = scan_skip_substitution(lx, i + 2, lines);
            continue;
        }
        i++;
    }
    return i;
}

/**
 * @brief 读取一个模板字面量 / Scan a template literal
 *
 * @param[in,out] lx 词法分析器 / Lexer
 *
 * @note 整段源码存为一个 `T_TEMPLATE` 词法单元；未闭合时报错 / The whole source is stored as one `T_TEMPLATE` token; raises an error when unterminated
 *
 * @see scan_template
 */
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

/**
 * @brief 跳过空白与注释 / Skip whitespace and comments
 *
 * @param[in,out] lx 词法分析器 / Lexer
 *
 * @return 消费了内容返回 1，否则 0 / 1 when something was consumed, otherwise 0
 *
 * @note 处理 `//` 行注释与块注释，并统计换行 / Handles `//` line comments and block comments and keeps track of newlines
 */
static int lex_skip_trivia(Lexer *lx)
{
    char c = lx->src[lx->pos];

    if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
        lx->pos++;
        return 1;
    }
    if (c == '\n') {
        lx->line++;
        lx->pos++;
        return 1;
    }
    if (c == '/' && lx->pos + 1 < lx->len && lx->src[lx->pos + 1] == '/') {
        while (lx->pos < lx->len && lx->src[lx->pos] != '\n')
            lx->pos++;
        return 1;
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
        return 1;
    }
    return 0;
}

/**
 * @brief 尝试匹配一个标点词法单元 / Try to match a punctuation token
 *
 * @param[in,out] lx 词法分析器 / Lexer
 *
 * @return 匹配到的长度（4/3/2/1）；不匹配返回 0 / The matched length (4/3/2/1), or 0 when nothing matches
 *
 * @note 按 4→1 从长到短尝试标点表；`?.` 后紧跟数字时按三元运算符处理 / Tries the tables longest-first; `?.` followed by a digit is treated as a ternary
 *
 * @see lex_all
 */
static int lex_match_punct(Lexer *lx)
{
    int i;

    for (i = 0; k_puncts4[i]; i++)
        if (lx->pos + 4 <= lx->len && strncmp(lx->src + lx->pos, k_puncts4[i], 4) == 0) {
            lex_add(lx, T_PUNCT, 0, k_puncts4[i], 4, lx->line);
            lx->pos += 4;
            return 4;
        }
    for (i = 0; k_puncts3[i]; i++)
        if (lx->pos + 3 <= lx->len && strncmp(lx->src + lx->pos, k_puncts3[i], 3) == 0) {
            lex_add(lx, T_PUNCT, 0, k_puncts3[i], 3, lx->line);
            lx->pos += 3;
            return 3;
        }
    for (i = 0; k_puncts2[i]; i++)
        if (lx->pos + 2 <= lx->len && strncmp(lx->src + lx->pos, k_puncts2[i], 2) == 0) {
            /* `?.3` is a ternary, not optional chaining */
            if (!strcmp(k_puncts2[i], "?.") && lx->pos + 2 < lx->len &&
                lx->src[lx->pos + 2] >= '0' && lx->src[lx->pos + 2] <= '9')
                continue;
            lex_add(lx, T_PUNCT, 0, k_puncts2[i], 2, lx->line);
            lx->pos += 2;
            return 2;
        }
    for (i = 0; k_puncts1[i]; i++)
        if (lx->src[lx->pos] == k_puncts1[i][0]) {
            lex_add(lx, T_PUNCT, 0, k_puncts1[i], 1, lx->line);
            lx->pos++;
            return 1;
        }
    return 0;
}

/**
 * @brief 判断游标处是否为数字字面量开头 / Test whether the cursor starts a numeric literal
 *
 * @param[in,out] lx 词法分析器 / Lexer
 *
 * @return 是数字返回 1，否则 0 / 1 for a number, otherwise 0
 *
 * @note 同时识别 `123` 与 `.5` 形式 / Recognises both `123` and `.5`
 */
static int lex_at_number(Lexer *lx)
{
    char c = lx->src[lx->pos];

    if (c >= '0' && c <= '9')
        return 1;
    return c == '.' && lx->pos + 1 < lx->len && lx->src[lx->pos + 1] >= '0' &&
           lx->src[lx->pos + 1] <= '9';
}

/**
 * @brief 把整段源码词法分析为词法单元序列 / Tokenise the whole source into the token list
 *
 * @param[in,out] lx 词法分析器 / Lexer
 *
 * @note 循环分派到字符串/模板/正则/数字/标识符/标点，末尾追加 `T_EOF` / Dispatches to string, template, regex, number, identifier and punctuation; appends a final `T_EOF`
 *
 * @see lex_skip_trivia, lex_match_punct
 */
static void lex_all(Lexer *lx)
{
    while (lx->pos < lx->len) {
        char c = lx->src[lx->pos];

        if (lex_skip_trivia(lx))
            continue;
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
        if (lex_at_number(lx)) {
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
        if (!lex_match_punct(lx))
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

/**
 * @brief 取当前词法单元 / Return the current token
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return 指向当前词法单元的指针 / Pointer to the current token
 */
static Tok *p_cur(P *p)
{
    return &p->t[p->pos];
}

/**
 * @brief 取相对当前偏移 i 的词法单元 / Return the token at offset `i` from the cursor
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in]     i 相对偏移 / Relative offset
 *
 * @return 指向该词法单元的指针；越界时钳到最后一个（`T_EOF`）/ Pointer to that token; clamped to the last one (`T_EOF`) when out of range
 */
static Tok *p_at(P *p, int i)
{
    int k = p->pos + i;
    if (k >= p->ntok)
        k = p->ntok - 1;
    return &p->t[k];
}

/**
 * @brief 判断当前词法单元是否为给定标点 / Test whether the current token is the given punctuation
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in]     punct 标点串 / The punctuation string
 *
 * @return 匹配返回 1，否则 0 / 1 on a match, otherwise 0
 */
static int p_is(P *p, const char *punct)
{
    Tok *t = p_cur(p);
    return t->type == T_PUNCT && strcmp(t->text, punct) == 0;
}

/**
 * @brief 判断当前词法单元是否为给定标识符 / Test whether the current token is the given identifier
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in]     word 单词文本 / The word text
 *
 * @return 匹配返回 1，否则 0 / 1 on a match, otherwise 0
 */
static int p_is_ident(P *p, const char *word)
{
    Tok *t = p_cur(p);
    return t->type == T_IDENT && strcmp(t->text, word) == 0;
}

/**
 * @brief 判断偏移 i 处的词法单元是否为给定标识符 / Test whether the token at offset `i` is the given identifier
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in]     i 相对偏移 / Relative offset
 * @param[in]     word 单词文本 / The word text
 *
 * @return 匹配返回 1，否则 0 / 1 on a match, otherwise 0
 */
static int p_is_ident_at(P *p, int i, const char *word)
{
    Tok *t = p_at(p, i);
    return t->type == T_IDENT && strcmp(t->text, word) == 0;
}

/**
 * @brief 取当前词法单元的行号 / Return the current token's line number
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return 当前行号 / The current line number
 */
static int p_line(P *p)
{
    return p_cur(p)->line;
}

/**
 * @brief 推进解析游标一个词法单元 / Advance the parse cursor by one token
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @note 已到 `T_EOF` 时不再前进 / Stops advancing once at `T_EOF`
 */
static void p_advance(P *p)
{
    if (p->t[p->pos].type != T_EOF)
        p->pos++;
}

/**
 * @brief 断言当前词法单元为指定标点并前进 / Assert the current token is `punct` and advance
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in]     punct 期望的标点串 / The expected punctuation string
 *
 * @warning 不匹配时报告语法错误 / Reports a syntax error on mismatch
 */
static void p_expect(P *p, const char *punct)
{
    if (!p_is(p, punct))
        js_error(p->js, p_line(p), "expected '%s' but found '%s'", punct,
                 p_cur(p)->text ? p_cur(p)->text : "end of input");
    p_advance(p);
}

/**
 * @brief 消费语句结束（`;` 或自动分号插入）/ Consume a statement end (`;` or automatic semicolon insertion)
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return 认为语句已结束返回 1，否则 0 / 1 when the statement is treated as ended, otherwise 0
 *
 * @note `;`、`}`、`T_EOF` 或下一词法单元换行时均视为结束 / `;`, `}`, `T_EOF` or a line break before the next token all count as an end
 */
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

/**
 * @brief 判断单词是否为不可作绑定名的保留字 / Test whether a word is a reserved word that may not be a binding name
 *
 * @param[in]     word 待判断的单词 / Word to test
 *
 * @return 是保留字返回 1，否则 0 / 1 when reserved, otherwise 0
 */
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

/**
 * @brief 在当前行新建一个 AST 节点 / Allocate a new AST node at the current line
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in]     kind 节点种类（`JS_N_*`）/ Node kind (`JS_N_*`)
 *
 * @return 新建的 AST 节点（存于 arena）/ The new AST node (allocated from the arena)
 *
 * @see p_expr, p_assignment, p_block, p_statement
 */
static JsNode *p_node(P *p, int kind)
{
    return js_node_new(p->js, kind, p_line(p));
}

static JsNode *p_expr(P *p);
static JsNode *p_assignment(P *p);
static JsNode *p_block(P *p);
static JsNode *p_statement(P *p);

/**
 * @brief 跳过从 start 开始的成对括号组 / Skip a balanced (), [] or {} group starting at `start`
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in]     start 组起始词法单元下标 / Token index where the group starts
 *
 * @return 越过闭合符号的下标；不配对返回 -1 / The index just past the closing token, or -1 when unbalanced
 */
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

/**
 * @brief 判断当前位置是否开始一个解构模式 / Test whether `[` or `{` here starts a destructuring pattern
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return 是模式开头返回 1，否则 0 / 1 when a pattern starts here, otherwise 0
 *
 * @note 用于区分解构模式与数组/对象字面量 / Used to tell a destructuring pattern from an array/object literal
 */
static int p_pattern_start(P *p)
{
    Tok *t = p_cur(p);
    return t->type == T_PUNCT && (!strcmp(t->text, "[") || !strcmp(t->text, "{"));
}

static JsNode *p_binding_element(P *p);
static JsNode *p_binding_target(P *p);

/**
 * @brief 解析 `[a, b, ...rest]` 数组解构绑定 / Parse an `[a, b, ...rest]` array binding pattern
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return `JS_N_PAT_ARR` 节点 / A `JS_N_PAT_ARR` node
 *
 * @note 支持空位（hole）/ Supports holes
 */
static JsNode *p_binding_array(P *p)
{
    JsNode *arr = p_node(p, JS_N_PAT_ARR);
    p_advance(p); /* [ */
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

/**
 * @brief 解析对象解构模式中的一项 / Parse one entry of an object binding pattern
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in,out] obj 模式节点 / The pattern node
 *
 * @note 覆盖 `{key: target}`、`{key = default}` 与 `{key}` 三种形式 / Covers `{key: target}`, `{key = default}` and `{key}`
 *
 * @see p_binding_object
 */
static void p_binding_obj_pair(P *p, JsNode *obj)
{
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
        return;
    }
    pair = p_node(p, JS_N_ASSIGN);
    pair->str = key;
    if (p_is(p, ":")) {
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

/**
 * @brief 解析 `{a, b: c, ...rest}` 对象解构绑定 / Parse a `{a, b: c, ...rest}` object binding pattern
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return `JS_N_PAT_OBJ` 节点 / A `JS_N_PAT_OBJ` node
 *
 * @see p_binding_obj_pair
 */
static JsNode *p_binding_object(P *p)
{
    JsNode *obj = p_node(p, JS_N_PAT_OBJ);
    p_advance(p); /* { */
    while (!p_is(p, "}")) {
        if (p_is(p, "...")) {
            JsNode *rest = p_node(p, JS_N_PAT_REST);
            p_advance(p);
            js_node_add(p->js, rest, p_binding_target(p));
            js_node_add(p->js, obj, rest);
        } else {
            p_binding_obj_pair(p, obj);
        }
        if (p_is(p, ","))
            p_advance(p);
        else
            break;
    }
    p_expect(p, "}");
    return obj;
}

/**
 * @brief 解析一个绑定目标 / Parse a binding target
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return 标识符、数组或对象模式节点 / An identifier, array or object pattern node
 *
 * @note 在非严格位置宽容接受保留字作名字 / Tolerates reserved words as names in non-strict positions
 */
static JsNode *p_binding_target(P *p)
{
    Tok *t = p_cur(p);

    if (t->type == T_IDENT) {
        /* reserved words are accepted here as names too: JS engines allow many
         * of them (`let`, `static`, `get`, `set`, `async`, `of`, `as`, `from`)
         * in non-strict positions, so we do not reject them */
        JsNode *id = p_node(p, JS_N_IDENT);
        id->str = t->text;
        p_advance(p);
        return id;
    }
    if (p_is(p, "["))
        return p_binding_array(p);
    if (p_is(p, "{"))
        return p_binding_object(p);
    js_error(p->js, p_line(p), "binding name expected");
    return p_node(p, JS_N_IDENT);
}

/**
 * @brief 解析一个参数或数组元素 / Parse a parameter or an array element
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return 目标、带默认值的 `JS_N_PAT_DEFAULT` 或 `JS_N_PAT_REST` 节点 / A target, a `JS_N_PAT_DEFAULT`, or a `JS_N_PAT_REST` node
 *
 * @note 覆盖 `target`、`target = default` 与 `...target` / Covers `target`, `target = default` and `...target`
 *
 * @see p_binding_target
 */
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

/**
 * @brief 从模板文本块读取 `\xHH`（maxk=2）或 `\uXXXX`（maxk=4）/ Read `\xHH` (maxk=2) or `\uXXXX` (maxk=4) from a template chunk
 *
 * @param[in]     s 以 NUL 结尾的字符串 / NUL-terminated string
 * @param[in]     n 节点 / Node
 * @param[in,out] i 读取游标 / Read cursor
 * @param[in]     maxk 最多读取的数位个数 / Maximum number of digits to read
 *
 * @return 读到的码点 / The accumulated code point
 *
 * @see decode_chunk
 */
static uint32_t chunk_read_hex(const char *s, size_t n, size_t *i, int maxk)
{
    uint32_t cp = 0;
    int k;

    for (k = 0; k < maxk && *i < n; k++) {
        int d = lex_hex_digit(s[*i]);
        if (d < 0)
            break;
        cp = cp * 16 + (uint32_t)d;
        (*i)++;
    }
    return cp;
}

/**
 * @brief 读取 `\u{XXXXX}` 花括号形式 / Read the braced `\u{XXXXX}` form
 *
 * @param[in]     s 以 NUL 结尾的字符串 / NUL-terminated string
 * @param[in]     n 节点 / Node
 * @param[in,out] i 读取游标 / Read cursor
 *
 * @return 读到的码点 / The accumulated code point
 *
 * @note 闭合花括号在此一并消费 / The closing brace is consumed here
 */
static uint32_t chunk_read_hex_braced(const char *s, size_t n, size_t *i)
{
    uint32_t cp = 0;

    (*i)++; /* { */
    while (*i < n && s[*i] != '}') {
        int d = lex_hex_digit(s[*i]);
        if (d < 0)
            break;
        cp = cp * 16 + (uint32_t)d;
        (*i)++;
    }
    if (*i < n)
        (*i)++;
    return cp;
}

/**
 * @brief 把一个码点以 UTF-8 追加到输出 / Append one code point to the output in UTF-8
 *
 * @param[out]    out 接收结果的缓冲区或结构 / Receives the result
 * @param[in]     cap 输出缓冲区容量 / Output buffer capacity
 * @param[in,out] o 已写出的字节数 / Bytes already written
 * @param[in]     cp 待写入的码点 / Code point to write
 *
 * @note ASCII 直接写一字节；其余走 `hpe_utf8_encode`，空间不足时丢弃 / ASCII is written directly; others go through `hpe_utf8_encode` and are dropped when the buffer is full
 */
static void chunk_out_cp(char *out, size_t cap, size_t *o, uint32_t cp)
{
    if (cp < 0x80) {
        out[(*o)++] = (char)cp;
        return;
    }
    {
        char utf[4];
        int m = hpe_utf8_encode(cp, utf);
        if (*o + 4 < cap) {
            memcpy(out + *o, utf, (size_t)m);
            *o += (size_t)m;
        }
    }
}

/**
 * @brief 解码模板文本块中的一个转义序列 / Decode one escape sequence in a template chunk
 *
 * @param[in]     s 以 NUL 结尾的字符串 / NUL-terminated string
 * @param[in]     n 节点 / Node
 * @param[in,out] i 读取游标 / Read cursor
 * @param[out]    out 接收结果的缓冲区或结构 / Receives the result
 * @param[in]     cap 输出缓冲区容量 / Output buffer capacity
 * @param[in,out] o 已写出的字节数 / Bytes already written
 *
 * @note 前导反斜杠已消费；`\n` 续行不输出任何字符 / The leading backslash is consumed; a `\n` line continuation emits nothing
 *
 * @see chunk_read_hex, chunk_out_cp
 */
static void chunk_escape(const char *s, size_t n, size_t *i, char *out, size_t cap, size_t *o)
{
    char e = s[(*i)++];
    int simple;

    if (e == 'x' || e == 'u') {
        uint32_t cp;
        if (e == 'u' && *i < n && s[*i] == '{')
            cp = chunk_read_hex_braced(s, n, i);
        else
            cp = chunk_read_hex(s, n, i, e == 'x' ? 2 : 4);
        chunk_out_cp(out, cap, o, cp);
        return;
    }
    if (e == '\n')
        return; /* line continuation: emit nothing */
    simple = -1;
    switch (e) {
    case 'n': simple = '\n'; break;
    case 't': simple = '\t'; break;
    case 'r': simple = '\r'; break;
    case 'b': simple = '\b'; break;
    case 'f': simple = '\f'; break;
    case 'v': simple = '\v'; break;
    case '0': simple = '\0'; break;
    default: break;
    }
    out[(*o)++] = (char)(simple < 0 ? e : simple);
}

/**
 * @brief 解码一个模板文本块的全部转义 / Decode all escapes of one template chunk
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     s 以 NUL 结尾的字符串 / NUL-terminated string
 * @param[in]     n 节点 / Node
 * @param[out]    out 接收结果的缓冲区或结构 / Receives the result
 * @param[out]    cap 当前容量 / Current capacity
 *
 * @return 写出的字节数 / The number of bytes written
 *
 * @note 输出始终以 NUL 结尾 / The output is always NUL-terminated
 *
 * @see chunk_escape
 */
static size_t decode_chunk(HpeJs *js, const char *s, size_t n, char *out, size_t cap)
{
    size_t i = 0, o = 0;
    (void)js;
    while (i < n && o + 1 < cap) {
        char c = s[i];
        if (c == '\\' && i + 1 < n) {
            i++;
            chunk_escape(s, n, &i, out, cap, &o);
            continue;
        }
        out[o++] = c;
        i++;
    }
    out[o] = '\0';
    return o;
}

/**
 * @brief 把独立表达式源码解析成 AST / Parse a standalone expression source into an AST
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     src 源数据 / Source data
 * @param[in]     len 源码长度 / Source length
 *
 * @return 表达式根节点 / The expression root node
 *
 * @note 用于 `${...}` 替换体；末尾若还有词法单元则报错 / Used for `${...}` substitutions; leftover tokens raise an error
 */
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

/**
 * @brief 向模板节点的 cooked 块列表追加一块 / Append one cooked chunk to a template node
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in,out] node 模板节点 / The template node
 * @param[in]     s 以 NUL 结尾的字符串 / NUL-terminated string
 * @param[in]     start 块起始下标 / Chunk start offset
 * @param[in]     end 块结束下标 / Chunk end offset
 *
 * @note 偏移相对 `s`；解码转义后作为 `JS_N_STR` 加入 `node->kids[0]` / Offsets are relative to `s`; the decoded text is added as a `JS_N_STR` into `node->kids[0]`
 *
 * @see decode_chunk
 */
static void p_template_add_chunk(P *p, JsNode *node, const char *s, size_t start, size_t end)
{
    char *cooked = (char *)malloc(end - start + 8);
    size_t cn;
    JsNode *str;

    if (!cooked)
        js_error(p->js, p_line(p), "out of memory");
    cn = decode_chunk(p->js, s + start, end - start, cooked, end - start + 4);
    str = p_node(p, JS_N_STR);
    str->str = hpe_js_strndup(p->js, cooked, cn);
    js_node_add(p->js, node->kids[0], str);
    free(cooked);
}

/**
 * @brief 越过 `${ … }` 替换体（跳过嵌套花括号/模板/字符串）/ Advance past a `${ … }` substitution, skipping nested braces, templates and strings
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     s 以 NUL 结尾的字符串 / NUL-terminated string
 * @param[in]     len 长度 / Length
 * @param[in]     es 替换体起始下标（`${` 之后）/ Substitution start offset (past `${`)
 *
 * @return 越过闭合 `}` 的下标 / The index just past the closing `}`
 *
 * @see p_template
 */
static size_t p_template_scan_sub(HpeJs *js, const char *s, size_t len, size_t es)
{
    size_t j = es;
    int depth = 1;

    while (j < len && depth) {
        char c = s[j];
        if (c == '{') {
            depth++;
        } else if (c == '}') {
            depth--;
        } else if (c == '`') {
            int lines = 0;
            Lexer tmp;
            memset(&tmp, 0, sizeof(tmp));
            tmp.js = js;
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
    return j;
}

/**
 * @brief 把 `T_TEMPLATE` 词法单元展开为模板节点 / Expand a `T_TEMPLATE` token into a template node
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return `JS_N_TEMPLATE` 节点（cooked 块 + 各替换表达式）/ A `JS_N_TEMPLATE` node (cooked chunks plus substitution expressions)
 *
 * @note kids[0] 为 cooked 块列表，其后为每个 `${}` 的表达式 / `kids[0]` is the cooked-chunk list, followed by one expression per `${}`
 *
 * @see p_template_add_chunk, p_template_scan_sub, parse_sub_expression
 */
static JsNode *p_template(P *p)
{
    const char *s = p_cur(p)->text ? p_cur(p)->text : "";
    size_t len = strlen(s);
    size_t i = 1; /* past the opening backtick */
    JsNode *node = p_node(p, JS_N_TEMPLATE);
    js_node_add(p->js, node, p_node(p, JS_N_BLOCK)); /* kids[0]: cooked chunks */

    while (i < len) {
        size_t start = i;

        while (i < len && s[i] != '`' && !(s[i] == '$' && i + 1 < len && s[i + 1] == '{')) {
            if (s[i] == '\\' && i + 1 < len)
                i += 2;
            else
                i++;
        }
        p_template_add_chunk(p, node, s, start, i);
        if (i >= len || s[i] == '`')
            break;
        {
            size_t es = i + 2; /* past ${ */
            size_t j = p_template_scan_sub(p->js, s, len, es);
            js_node_add(p->js, node, parse_sub_expression(p->js, s + es, j - 1 - es));
            i = j;
        }
    }
    p_advance(p);
    return node;
}
/* ---------------- declarations / functions ---------------- */

/**
 * @brief 解析 `function [name](params) { body }` / Parse `function [name](params) { body }`
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in]     need_name 必须有名返回 1 / 1 when a name is required
 * @param[in]     is_async async 函数为 1 / 1 for an async function
 * @param[in]     is_gen 生成器为 1 / 1 for a generator
 *
 * @return `JS_N_FUNC` 节点（op 为 `FN_*` 标志，kids[0] 参数、kids[1] 函数体）/ A `JS_N_FUNC` node (op holds `FN_*` flags; kids[0] params, kids[1] body)
 *
 * @note 调用时当前词法单元须为 `function` / The current token must be `function`
 *
 * @see p_arrow_from_params
 */
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

/**
 * @brief 由已解析的参数构造箭头函数 / Build an arrow function from already-parsed parameters
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in,out] params 形参节点 / Parameter node
 * @param[in]     is_async async 箭头为 1 / 1 for an async arrow
 *
 * @return `JS_N_FUNC` 节点（带 `FN_ARROW`）/ A `JS_N_FUNC` node carrying `FN_ARROW`
 *
 * @note 当前词法单元须为 `=>`；非块体转为 `return expr` / The current token must be `=>`; a non-block body becomes `return expr`
 *
 * @see p_function
 */
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

/**
 * @brief 判断 pos 处的 `(` 是否为箭头函数参数表 / Test whether the `(` at `pos` introduces arrow parameters
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in]     pos `(` 所在词法单元下标 / Token index of the `(`
 *
 * @return 是箭头参数表返回 1，否则 0 / 1 when it is an arrow parameter list, otherwise 0
 *
 * @note 用配对扫描做前瞻 / Uses a balanced-scan lookahead
 */
static int p_paren_arrow(P *p, int pos)
{
    int end = p_skip_balanced(p, pos);
    if (end < 0)
        return 0;
    return p->t[end].type == T_PUNCT && !strcmp(p->t[end].text, "=>");
}

/**
 * @brief 解析 `(params)` 箭头参数表 / Parse a `(params)` arrow parameter list
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return 参数块节点（`JS_N_BLOCK`）/ A parameter block node (`JS_N_BLOCK`)
 *
 * @see p_arrow_from_params
 */
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

/**
 * @brief 解析类私有名 `#name` / Parse a private class name `#name`
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[out]    out 接收 `#name` 字符串 / Receives the `#name` string
 *
 * @return 匹配并消费返回 1，否则 0 / 1 when matched and consumed, otherwise 0
 *
 * @note 结果从 arena 分配 / The result is allocated from the arena
 */
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

/**
 * @brief 解析一个属性名（含计算名与私有名）/ Parse a property name (computed and private included)
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[out]    out 接收静态属性名字符串 / Receives the static property name string
 * @param[out]    computed 接收 `[expr]` 计算名节点，无则置 `NULL` / Receives the `[expr]` computed node, set to `NULL` when absent
 *
 * @return 解析成功返回 1，否则 0 / 1 on success, otherwise 0
 *
 * @note 覆盖标识符/字符串/数字键、`[expr]` 与 `#private` / Covers identifier/string/number keys, `[expr]` and `#private`
 */
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

/* `async`, `get`, `set` and `*` prefixes that may precede an object method */
typedef struct {
    int is_async;
    int is_gen;
    int is_get;
    int is_set;
} ObjMemberFlags;

/**
 * @brief 识别对象方法的 `get`/`set` 前缀 / Recognise the `get`/`set` prefixes of an object method
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in,out] f 成员标志 / Member flags
 *
 * @note 仅当后面跟着名字（非 `:`/`,`/`}`/`(`）才生效，使 `{get: 1}`、`{get}` 保持普通属性 / Only applies when a name follows (not `:`/`,`/`}`/`(`), keeping `{get: 1}` and `{get}` plain properties
 *
 * @see p_obj_member_prefix
 */
static void p_obj_accessor_flags(P *p, ObjMemberFlags *f)
{
    Tok *nx = p_at(p, 1);
    int ends = nx->type == T_PUNCT && (!strcmp(nx->text, ":") || !strcmp(nx->text, ",") ||
                                       !strcmp(nx->text, "}") || !strcmp(nx->text, "("));

    if (f->is_async || ends)
        return;
    if (p_is_ident(p, "get")) {
        f->is_get = 1;
        p_advance(p);
    } else if (p_is_ident(p, "set")) {
        f->is_set = 1;
        p_advance(p);
    }
}

/**
 * @brief 识别对象方法的 `async` / `async *` 前缀 / Recognise the `async` / `async *` prefixes of an object method
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in,out] f 成员标志 / Member flags
 *
 * @see p_obj_member_prefix
 */
static void p_obj_async_prefix(P *p, ObjMemberFlags *f)
{
    Tok *nx = p_at(p, 1);
    int nameish = (nx->type == T_IDENT || nx->type == T_STR || nx->type == T_NUM);
    int is_paren = nx->type == T_PUNCT && !strcmp(nx->text, "(");
    int is_star = nx->type == T_PUNCT && !strcmp(nx->text, "*");

    if (!p_is_ident(p, "async") || p_is(p, ",") || p_is(p, "}") || p_is(p, ":"))
        return;
    if (!nameish && !is_paren && !is_star)
        return;
    f->is_async = 1;
    p_advance(p);
    if (p_is(p, "*")) {
        f->is_gen = 1;
        p_advance(p);
    }
}

/**
 * @brief 识别一个对象属性前的全部成员修饰前缀 / Detect all member-modifier prefixes of one object property
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[out]    f 成员标志 / Member flags
 *
 * @note 依次处理 `async`、`get`/`set` 与 `*` / Handles `async`, `get`/`set` and `*` in order
 *
 * @see p_obj_async_prefix, p_obj_accessor_flags
 */
static void p_obj_member_prefix(P *p, ObjMemberFlags *f)
{
    f->is_async = f->is_gen = f->is_get = f->is_set = 0;
    p_obj_async_prefix(p, f);
    p_obj_accessor_flags(p, f);
    if (!f->is_get && !f->is_set && !f->is_async && p_is(p, "*")) {
        f->is_gen = 1;
        p_advance(p);
    }
}

/**
 * @brief 解析 `key(...) { … }` 方法简写 / Parse the `key(...) { … }` method shorthand
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in,out] obj 对象节点 / The object node
 * @param[in,out] key 属性名 / Property name
 * @param[in,out] computed 计算名节点，可为 `NULL` / Computed-name node, may be `NULL`
 * @param[in]     f 成员标志 / Member flags
 *
 * @note 访问器/方法标志写入 `pair->op` 与函数 op / Accessor/method flags go into `pair->op` and the function's op
 *
 * @see p_object_props
 */
static void p_obj_method(P *p, JsNode *obj, char *key, JsNode *computed,
                         const ObjMemberFlags *f)
{
    JsNode *pair = p_node(p, JS_N_ASSIGN);
    JsNode *fn = p_node(p, JS_N_FUNC);
    JsNode *params = p_node(p, JS_N_BLOCK);

    fn->op = (f->is_async ? FN_ASYNC : 0) | (f->is_gen ? FN_GEN : 0) |
             (f->is_get ? 0 : (f->is_set ? 0 : FN_CLASS));
    pair->op = f->is_get ? JS_PROP_GET : (f->is_set ? JS_PROP_SET : JS_PROP_METHOD);
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
}

/**
 * @brief 解析 `key: value` 数据属性 / Parse a `key: value` data property
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in,out] obj 对象节点 / The object node
 * @param[in,out] key 属性名 / Property name
 * @param[in,out] computed 计算名节点，可为 `NULL` / Computed-name node, may be `NULL`
 *
 * @note 字面量 `__proto__` 被识别为原型属性 / A literal `__proto__` is recognised as a prototype property
 */
static void p_obj_colon_prop(P *p, JsNode *obj, char *key, JsNode *computed)
{
    JsNode *pair = p_node(p, JS_N_ASSIGN);
    p_advance(p);
    pair->op = (key && !strcmp(key, "__proto__") && !computed) ? JS_PROP_PROTO : JS_PROP_DATA;
    pair->str = key;
    js_node_add(p->js, pair, p_assignment(p));
    if (computed)
        js_node_add(p->js, pair, computed);
    js_node_add(p->js, obj, pair);
}

/**
 * @brief 解析 `key` 或 `key = default` 简写属性 / Parse a `key` or `key = default` shorthand property
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in,out] obj 对象节点 / The object node
 * @param[in,out] key 属性名 / Property name
 *
 * @note 带默认值的形式仅用于解构 / The defaulted form is only valid in destructuring
 */
static void p_obj_shorthand_prop(P *p, JsNode *obj, char *key)
{
    JsNode *pair = p_node(p, JS_N_ASSIGN);
    JsNode *id = p_node(p, JS_N_IDENT);

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

/**
 * @brief 解析对象字面量的成员列表 / Parse the member list of an object literal
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in,out] obj 对象节点 / The object node
 *
 * @note 支持 `...spread`、方法、访问器、简写与计算名；`{` 已被消费 / Supports `...spread`, methods, accessors, shorthands and computed names; the `{` is already consumed
 *
 * @see p_obj_method, p_obj_colon_prop, p_obj_shorthand_prop
 */
static void p_object_props(P *p, JsNode *obj)
{
    while (!p_is(p, "}")) {
        ObjMemberFlags f;
        char *key = NULL;
        JsNode *computed = NULL;

        if (p_is(p, "...")) {
            JsNode *sp = p_node(p, JS_N_SPREAD);
            p_advance(p);
            js_node_add(p->js, sp, p_assignment(p));
            js_node_add(p->js, obj, sp);
            if (p_is(p, ","))
                p_advance(p);
            continue;
        }
        p_obj_member_prefix(p, &f);
        if (!p_property_name(p, &key, &computed)) {
            js_error(p->js, p_line(p), "object key expected");
            break;
        }
        if (p_is(p, "(")) {
            p_obj_method(p, obj, key, computed, &f);
        } else if (p_is(p, ":")) {
            p_obj_colon_prop(p, obj, key, computed);
        } else {
            if (!key) {
                js_error(p->js, p_line(p), "invalid shorthand property");
                break;
            }
            p_obj_shorthand_prop(p, obj, key);
        }
        if (p_is(p, ","))
            p_advance(p);
        else
            break;
    }
    p_expect(p, "}");
}

/* class member prefixes: `static`, `async`, `*`, `get`/`set` */
typedef struct {
    int is_static;
    int is_async;
    int is_gen;
    int is_get;
    int is_set;
} ClassMemberFlags;

/**
 * @brief 判断下一个词法单元是否结束成员名 / Test whether the next token ends a member name
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return 下一词法单元为 `(`、`=` 或 `;` 返回 1，否则 0 / 1 when the next token is `(`, `=` or `;`, otherwise 0
 *
 * @note 若是，则前面的单词是普通字段名而非修饰关键字 / If so, the preceding word is a plain field name rather than a modifier keyword
 */
static int p_next_ends_member(P *p)
{
    Tok *nx = p_at(p, 1);
    return nx->type == T_PUNCT &&
           (!strcmp(nx->text, "(") || !strcmp(nx->text, "=") || !strcmp(nx->text, ";"));
}

/**
 * @brief 识别类成员的 `static` 前缀 / Recognise the `static` prefix of a class member
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in,out] f 成员标志 / Member flags
 *
 * @note `static` 后面为 `(`/`=` 时不当作前缀 / `static` followed by `(`/`=` is not treated as a prefix
 *
 * @see p_class_member_prefix
 */
static void p_class_static_prefix(P *p, ClassMemberFlags *f)
{
    Tok *nx = p_at(p, 1);
    if (!p_is_ident(p, "static") || p_is(p, "(") || p_is(p, "="))
        return;
    if (nx->type == T_PUNCT && (!strcmp(nx->text, "(") || !strcmp(nx->text, "=") ||
                                !strcmp(nx->text, "}")))
        return;
    f->is_static = 1;
    p_advance(p);
}

/**
 * @brief 收集一个类成员前的全部修饰前缀 / Collect all modifier prefixes preceding one class member
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[out]    f 成员标志 / Member flags
 *
 * @note 处理 `static`、`async`、`*` 与 `get`/`set` / Handles `static`, `async`, `*` and `get`/`set`
 *
 * @see p_class_static_prefix
 */
static void p_class_member_prefix(P *p, ClassMemberFlags *f)
{
    f->is_static = f->is_async = f->is_gen = f->is_get = f->is_set = 0;
    p_class_static_prefix(p, f);
    if (p_is_ident(p, "async") && !p_next_ends_member(p)) {
        f->is_async = 1;
        p_advance(p);
    }
    if (p_is(p, "*")) {
        f->is_gen = 1;
        p_advance(p);
    }
    if (!f->is_async && !f->is_gen && (p_is_ident(p, "get") || p_is_ident(p, "set")) &&
        !p_next_ends_member(p)) {
        f->is_get = p_is_ident(p, "get");
        f->is_set = !f->is_get;
        p_advance(p);
    }
}

/**
 * @brief 解析 `static { … }` 类静态初始化块 / Parse a `static { … }` class initialiser block
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in,out] members 类成员列表 / The class member list
 *
 * @note 语句被加入以 `JS_STATIC_BLOCK` 标记的块节点 / Statements go into a block node flagged `JS_STATIC_BLOCK`
 */
static void p_class_static_block(P *p, JsNode *members)
{
    JsNode *member = p_node(p, JS_N_BLOCK);
    member->op = JS_STATIC_BLOCK;
    js_node_add(p->js, members, member);
    p_advance(p); /* { */
    while (!p_is(p, "}")) {
        if (p_cur(p)->type == T_EOF)
            js_error(p->js, p_line(p), "unexpected end of input");
        js_node_add(p->js, member, p_statement(p));
    }
    p_expect(p, "}");
}

/**
 * @brief 判定类成员的方法种类标签 / Compute the method-kind tag of a class member
 *
 * @param[in]     key 成员名 / Member name
 * @param[in]     f 成员标志 / Member flags
 * @param[in,out] computed 计算名节点，可为 `NULL` / Computed-name node, may be `NULL`
 *
 * @return `JS_METHOD_GET`/`SET`/`STATIC`/`CTOR` 或 `JS_METHOD` / One of `JS_METHOD_GET`/`SET`/`STATIC`/`CTOR` or `JS_METHOD`
 *
 * @note 非计算名且名字为 `constructor` 视为构造器 / A `constructor` name that is not computed counts as the constructor
 */
static int p_class_method_kind(const char *key, const ClassMemberFlags *f, JsNode *computed)
{
    int is_ctor = key && !strcmp(key, "constructor");

    if (f->is_get)
        return JS_METHOD_GET;
    if (f->is_set)
        return JS_METHOD_SET;
    if (f->is_static)
        return JS_METHOD_STATIC;
    if (is_ctor && !computed)
        return JS_METHOD_CTOR;
    return JS_METHOD;
}

/**
 * @brief 解析 `name(params) { … }` 类方法（或构造器）/ Parse a `name(params) { … }` class method (or constructor)
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in,out] members 类成员列表 / The class member list
 * @param[in,out] key 方法名 / Method name
 * @param[in,out] computed 计算名节点，可为 `NULL` / Computed-name node, may be `NULL`
 * @param[in]     f 成员标志 / Member flags
 *
 * @note 构造器叠加 `FN_CTOR`；方法种类写入 `fn->num` / The constructor adds `FN_CTOR`; the method kind lands in `fn->num`
 *
 * @see p_class_body, p_class_method_kind
 */
static void p_class_method(P *p, JsNode *members, char *key, JsNode *computed,
                           const ClassMemberFlags *f)
{
    JsNode *fn = p_node(p, JS_N_FUNC);
    JsNode *params = p_node(p, JS_N_BLOCK);
    int ctor = key && !strcmp(key, "constructor") && !f->is_static && !computed;

    fn->op = (f->is_async ? FN_ASYNC : 0) | (f->is_gen ? FN_GEN : 0) | FN_CLASS |
             (ctor ? FN_CTOR : 0);
    fn->str = key;
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
    if (computed)
        js_node_add(p->js, fn, computed);
    fn->num = p_class_method_kind(key, f, computed);
    js_node_add(p->js, members, fn);
}

/**
 * @brief 解析 `name = value;` 类字段（值可省）/ Parse a `name = value;` class field (value optional)
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in,out] members 类成员列表 / The class member list
 * @param[in,out] key 字段名 / Field name
 * @param[in,out] computed 计算名节点，可为 `NULL` / Computed-name node, may be `NULL`
 * @param[in]     f 成员标志 / Member flags
 *
 * @note 静态字段用 `JS_MEMBER_STATIC_FIELD`，实例字段用 `JS_MEMBER_FIELD` / Static fields use `JS_MEMBER_STATIC_FIELD`, instance fields `JS_MEMBER_FIELD`
 *
 * @see p_class_body
 */
static void p_class_field(P *p, JsNode *members, char *key, JsNode *computed,
                          const ClassMemberFlags *f)
{
    JsNode *field = p_node(p, JS_N_ASSIGN);
    field->op = f->is_static ? JS_MEMBER_STATIC_FIELD : JS_MEMBER_FIELD;
    field->str = key;
    if (p_is(p, "=")) {
        p_advance(p);
        js_node_add(p->js, field, p_assignment(p));
    } else {
        js_node_add(p->js, field, NULL);
    }
    if (computed)
        js_node_add(p->js, field, computed);
    js_node_add(p->js, members, field);
    if (p_is(p, ";"))
        p_advance(p);
}

/**
 * @brief 解析类体（`{` 为当前词法单元）/ Parse a class body (the `{` is current)
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in,out] cls 类节点 / The class node
 *
 * @note 成员列表加入 `cls`；`;` 空成员被跳过 / The member list is added to `cls`; empty `;` members are skipped
 *
 * @see p_class_method, p_class_field, p_class_static_block
 */
static void p_class_body(P *p, JsNode *cls)
{
    JsNode *members = p_node(p, JS_N_BLOCK);
    js_node_add(p->js, cls, members);
    p_expect(p, "{");
    while (!p_is(p, "}")) {
        ClassMemberFlags f;
        char *key = NULL;
        JsNode *computed = NULL;

        if (p_cur(p)->type == T_EOF)
            js_error(p->js, p_line(p), "unexpected end of input in class body");
        if (p_is(p, ";")) {
            p_advance(p);
            continue;
        }
        p_class_member_prefix(p, &f);
        if (f.is_static && p_is(p, "{")) {
            p_class_static_block(p, members);
            continue;
        }
        if (!p_property_name(p, &key, &computed)) {
            js_error(p->js, p_line(p), "class member name expected");
            break;
        }
        if (p_is(p, "("))
            p_class_method(p, members, key, computed, &f);
        else
            p_class_field(p, members, key, computed, &f);
    }
    p_expect(p, "}");
}

/**
 * @brief 解析 `class [name] [extends expr] { body }` / Parse `class [name] [extends expr] { body }`
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return `JS_N_CLASS` 节点 / A `JS_N_CLASS` node
 *
 * @note 调用时当前词法单元须为 `class`；kids[0] 为父类或 `NULL` / The current token must be `class`; kids[0] holds the superclass or `NULL`
 *
 * @see p_class_body
 */
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

/**
 * @brief 解析 CallExpression 的 `(arg, ...spread)` 实参表 / Parse the `(arg, ...spread)` argument list of a call
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in,out] call 调用节点 / The call node
 *
 * @note 实参逐个加入 `call`；`...spread` 包成 `JS_N_SPREAD` / Arguments are added to `call`; `...spread` becomes a `JS_N_SPREAD`
 *
 * @see p_member_tail, p_super_expr
 */
static void p_call_args(P *p, JsNode *call)
{
    p_advance(p); /* ( */
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
}

/**
 * @brief 解析 `.name` / `.#private` 成员访问 / Parse a `.name` / `.#private` member access
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in,out] base 被访问的基础表达式 / The base expression
 *
 * @return `JS_N_MEMBER` 节点 / A `JS_N_MEMBER` node
 *
 * @note 调用时当前词法单元须为 `.` 且属性名紧随 / The current token must be `.` with the property name right after
 */
static JsNode *p_member_dot(P *p, JsNode *base)
{
    JsNode *m = p_node(p, JS_N_MEMBER);
    p_advance(p); /* . */
    m->op = JS_OP_DOT;
    if (p_private_name(p, &m->str)) {
        js_node_add(p->js, m, base);
        return m;
    }
    if (p_cur(p)->type != T_IDENT && p_cur(p)->type != T_STR &&
        p_cur(p)->type != T_NUM)
        js_error(p->js, p_line(p), "property name expected");
    m->str = p_cur(p)->text;
    js_node_add(p->js, m, base);
    p_advance(p);
    return m;
}

/**
 * @brief 解析 `[expr]` 下标访问 / Parse a `[expr]` index access
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in,out] base 被访问的基础表达式 / The base expression
 * @param[in]     optional 可选链访问为 1 / 1 for an optional-chain access
 *
 * @return `JS_N_MEMBER` 节点 / A `JS_N_MEMBER` node
 */
static JsNode *p_member_index(P *p, JsNode *base, int optional)
{
    JsNode *m = p_node(p, JS_N_MEMBER);
    m->op = JS_OP_INDEX;
    m->num = optional;
    js_node_add(p->js, m, base);
    p_advance(p);
    js_node_add(p->js, m, p_expr(p));
    p_expect(p, "]");
    return m;
}

/**
 * @brief 解析 `?.` 可选访问（调用/下标/点名）/ Parse a `?.` optional access (call, index or dotted name)
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in,out] base 被访问的基础表达式 / The base expression
 * @param[in]     allow_call 允许调用为 1 / 1 to allow a call
 *
 * @return 可选调用、可选下标或可选成员节点 / An optional call, optional index or optional member node
 *
 * @note 调用时当前词法单元须为 `?.` / The current token must be `?.`
 */
static JsNode *p_member_optional(P *p, JsNode *base, int allow_call)
{
    Tok *nx = p_at(p, 1);
    p_advance(p); /* ?. */
    if (allow_call && nx->type == T_PUNCT && !strcmp(nx->text, "(")) {
        JsNode *call = p_node(p, JS_N_CALL);
        call->op = 1; /* optional call */
        js_node_add(p->js, call, base);
        p_call_args(p, call);
        return call;
    }
    if (nx->type == T_PUNCT && !strcmp(nx->text, "["))
        return p_member_index(p, base, 1);
    {
        JsNode *m = p_node(p, JS_N_MEMBER);
        if (nx->type != T_IDENT && nx->type != T_STR)
            js_error(p->js, p_line(p), "property name expected after '?.'");
        m->op = JS_OP_DOT;
        m->num = 1;
        m->str = nx->text;
        js_node_add(p->js, m, base);
        p_advance(p);
        return m;
    }
}

/**
 * @brief 解析 MemberExpression 的尾部链 / Parse the tail chain of a MemberExpression
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in,out] base 基础表达式 / The base expression
 * @param[in]     allow_call 允许 `(args)` 调用为 1 / 1 to allow `(args)` calls
 *
 * @return 链接完成后的表达式节点 / The fully chained expression node
 *
 * @note `new` 以 allow_call=0 解析被调者，使 `new Date(0).getTime()` 绑定为 `(new Date(0)).getTime()` / `new` parses its callee with calls disabled so `new Date(0).getTime()` binds as `(new Date(0)).getTime()`
 *
 * @see p_member_dot, p_member_index, p_member_optional
 */
static JsNode *p_member_tail(P *p, JsNode *base, int allow_call)
{
    for (;;) {
        if (p_is(p, "?.")) {
            base = p_member_optional(p, base, allow_call);
            continue;
        }
        if (p_is(p, ".")) {
            base = p_member_dot(p, base);
        } else if (p_is(p, "[")) {
            base = p_member_index(p, base, 0);
        } else if (allow_call && p_is(p, "(")) {
            JsNode *call = p_node(p, JS_N_CALL);
            js_node_add(p->js, call, base);
            p_call_args(p, call);
            base = call;
        } else {
            /* a bare `super` may only be followed by `.`, `[` or `(` */
            break;
        }
    }
    return base;
}

/**
 * @brief 解析 `super` 后的 `(args)`、`.name` 或 `[expr]` / Parse `super` followed by `(args)`, `.name` or `[expr]`
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return `JS_N_SUPER_CALL` 或 `JS_N_SUPER_MEMBER` 节点 / A `JS_N_SUPER_CALL` or `JS_N_SUPER_MEMBER` node
 *
 * @warning `super` 后不是 `(`/`.`/`[` 时报错 / Errors when `super` is not followed by `(`, `.` or `[`
 */
static JsNode *p_super_expr(P *p)
{
    JsNode *n = p_node(p, JS_N_SUPER_MEMBER);
    p_advance(p);
    if (p_is(p, "(")) {
        JsNode *call = p_node(p, JS_N_SUPER_CALL);
        p_call_args(p, call);
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

/**
 * @brief 解析 async 函数/箭头形式 / Parse the async function or async arrow forms
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return `JS_N_FUNC` 节点；不是 async 形式时返回 `NULL` / A `JS_N_FUNC` node, or `NULL` when it is not an async form
 *
 * @note 覆盖 `async function`、`async x =>` 与 `async (…) =>` / Covers `async function`, `async x =>` and `async (…) =>`
 *
 * @see p_ident_primary
 */
static JsNode *p_async_expr(P *p)
{
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
    return NULL;
}

/**
 * @brief 解析 `import.meta` / Parse `import.meta`
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return 一个 `JS_N_OBJECT` 节点（占位对象）/ A `JS_N_OBJECT` node (a placeholder object)
 *
 * @note 动态 `import()` 不受支持 / Dynamic `import()` is not supported
 */
static JsNode *p_import_meta(P *p)
{
    JsNode *n = p_node(p, JS_N_OBJECT);
    p_advance(p);
    p_advance(p);
    if (p_cur(p)->type == T_IDENT)
        p_advance(p);
    return n;
}

/**
 * @brief 构造单参数箭头 `name => ...` / Build a single-parameter arrow `name => ...`
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in,out] name 参数名 / Parameter name
 * @param[in]     is_async async 箭头为 1 / 1 for an async arrow
 *
 * @return `JS_N_FUNC` 节点 / A `JS_N_FUNC` node
 *
 * @see p_arrow_from_params
 */
static JsNode *p_single_arrow(P *p, char *name, int is_async)
{
    JsNode *params = p_node(p, JS_N_BLOCK);
    JsNode *id = p_node(p, JS_N_IDENT);
    id->str = name;
    js_node_add(p->js, params, id);
    p_advance(p);
    return p_arrow_from_params(p, params, is_async);
}

/**
 * @brief 解析由关键字引导的初级表达式 / Resolve keyword-led primary expressions
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return 字面量、super、function/class、箭头等节点；普通标识符返回 `NULL` / A literal, `super`, function/class or arrow node; `NULL` means a plain identifier
 *
 * @note 覆盖 `true`/`false`/`null`/`this`/`super`/`function`/`async`/`class`/`new`/`import.meta` 与单参箭头 / Covers `true`/`false`/`null`/`this`/`super`/`function`/`async`/`class`/`new`/`import.meta` and single-parameter arrows
 *
 * @see p_primary
 */
static JsNode *p_ident_primary(P *p)
{
    Tok *t = p_cur(p);

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
    if (!strcmp(t->text, "super"))
        return p_super_expr(p);
    if (!strcmp(t->text, "function")) {
        int gen = p_at(p, 1)->type == T_PUNCT && !strcmp(p_at(p, 1)->text, "*");
        return p_function(p, 0, 0, gen);
    }
    if (!strcmp(t->text, "async") && !p_is(p, "=>")) {
        JsNode *n = p_async_expr(p);
        if (n)
            return n;
    }
    if (!strcmp(t->text, "class"))
        return p_class(p);
    if (!strcmp(t->text, "new"))
        return p_unary(p); /* `new` is handled by the unary level */
    if (!strcmp(t->text, "import") && p_at(p, 1)->type == T_PUNCT &&
        !strcmp(p_at(p, 1)->text, "."))
        return p_import_meta(p);
    if (p_at(p, 1)->type == T_PUNCT && !strcmp(p_at(p, 1)->text, "=>"))
        return p_single_arrow(p, t->text, 0);
    return NULL;
}

/**
 * @brief 解析带空位与展开的 `[…]` 数组字面量 / Parse a `[…]` array literal with holes and spread
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return `JS_N_ARRAY` 节点 / A `JS_N_ARRAY` node
 */
static JsNode *p_array_literal(P *p)
{
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

/**
 * @brief 解析 `(expr)` 或括号化箭头参数表 / Parse `(expr)` or a parenthesised arrow parameter list
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return 括起的表达式节点，或箭头函数节点 / The parenthesised expression, or an arrow function node
 *
 * @see p_paren_arrow, p_arrow_params
 */
static JsNode *p_paren_expr(P *p)
{
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

/**
 * @brief 解析初级表达式 / Parse a primary expression
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return 数字、字符串、正则、模板、标识符、括号、数组或对象字面量节点 / A number, string, regex, template, identifier, parenthesised, array or object literal node
 *
 * @warning 遇到 `T_EOF` 或意外词法单元时报错 / Errors on `T_EOF` or an unexpected token
 *
 * @see p_ident_primary, p_paren_expr, p_array_literal
 */
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
        JsNode *kw = p_ident_primary(p);
        if (kw)
            return kw;
        {
            JsNode *n = p_node(p, JS_N_IDENT);
            n->str = t->text;
            p_advance(p);
            return n;
        }
    }
    if (t->type == T_PUNCT && !strcmp(t->text, "("))
        return p_paren_expr(p);
    if (t->type == T_PUNCT && !strcmp(t->text, "["))
        return p_array_literal(p);
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

/**
 * @brief 解析后缀 `++`/`--` 表达式 / Parse a postfix `++`/`--` expression
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return 更新表达式节点或初级表达式结果 / An update-expression node, or the primary result
 *
 * @note 仅当运算符与操作数同行时才算后缀更新 / Only counts as a postfix update when the operator is on the same line as its operand
 */
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

/**
 * @brief 构造包裹操作数的前缀一元节点 / Build a prefix unary node wrapping its operand
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in]     kind 节点种类（`JS_N_UN`、`JS_N_AWAIT` 等）/ Node kind (`JS_N_UN`, `JS_N_AWAIT`, …)
 * @param[in]     op 运算符码 / Operator code
 *
 * @return 新建的一元节点 / The new unary node
 */
static JsNode *p_unwrap_prefix(P *p, int kind, int op)
{
    JsNode *n = p_node(p, kind);
    n->op = op;
    p_advance(p);
    js_node_add(p->js, n, p_unary(p));
    return n;
}

/**
 * @brief 解析 `yield [*] [expr]` / Parse `yield [*] [expr]`
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return `JS_N_YIELD` 节点 / A `JS_N_YIELD` node
 *
 * @note `yield*` 置 `op=1`；其后为终止符或换行时省略操作数 / `yield*` sets `op=1`; the operand is omitted when a terminator or a newline follows
 */
static JsNode *p_yield(P *p)
{
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

/**
 * @brief 解析 `new` 表达式（被调者、实参与方法尾）/ Parse a `new` expression (callee, arguments and method tail)
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return `JS_N_NEW` 或更外层的成员/调用节点 / A `JS_N_NEW` node, possibly wrapped by further member/call nodes
 *
 * @note 以 allow_call=0 解析被调者，使后续 `(...)` 归属于 `new` / The callee is parsed with calls disabled so a following `(...)` belongs to `new`
 *
 * @see p_member_tail
 */
static JsNode *p_new_expr(P *p)
{
    JsNode *n = p_node(p, JS_N_NEW);
    JsNode *callee;

    p_advance(p);
    if (p_is(p, "."))
        p_advance(p); /* new.target */
    /* The callee is a MemberExpression with calls suppressed, so the `(...)` that
     * follows belongs to `new` itself; any further `.`/`()` chain binds to the
     * constructed value: new Date(0).getTime() === (new Date(0)).getTime() */
    callee = p_member_tail(p, p_primary(p), 0);
    if (p_is(p, "(")) {
        JsNode *call = p_node(p, JS_N_CALL);
        js_node_add(p->js, call, callee);
        p_call_args(p, call);
        callee = call;
    }
    js_node_add(p->js, n, callee);
    return p_member_tail(p, n, 1);
}

/**
 * @brief 解析一元表达式 / Parse a unary expression
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return 一元、更新、`yield`、`new` 或后缀表达式的结果节点 / The result of a unary, update, `yield`, `new` or postfix expression
 *
 * @note 覆盖 `! - + ~ typeof void delete await yield new` 与前置 `++`/`--` / Covers `! - + ~ typeof void delete await yield new` and prefix `++`/`--`
 *
 * @see p_unwrap_prefix, p_yield, p_new_expr, p_postfix
 */
static JsNode *p_unary(P *p)
{
    if (p_is(p, "!"))
        return p_unwrap_prefix(p, JS_N_UN, JS_OP_NOT);
    if (p_is(p, "-"))
        return p_unwrap_prefix(p, JS_N_UN, JS_OP_NEG);
    if (p_is(p, "+"))
        return p_unwrap_prefix(p, JS_N_UN, JS_OP_POS);
    if (p_is(p, "~"))
        return p_unwrap_prefix(p, JS_N_UN, JS_OP_BNOT);
    if (p_is_ident(p, "typeof"))
        return p_unwrap_prefix(p, JS_N_UN, JS_OP_TYPEOF);
    if (p_is_ident(p, "void"))
        return p_unwrap_prefix(p, JS_N_UN, JS_OP_VOID);
    if (p_is_ident(p, "delete"))
        return p_unwrap_prefix(p, JS_N_UN, JS_OP_DELETE);
    if (p_is_ident(p, "await"))
        return p_unwrap_prefix(p, JS_N_AWAIT, 0);
    if (p_is_ident(p, "yield"))
        return p_yield(p);
    if (p_is_ident(p, "new"))
        return p_new_expr(p);
    if (p_is(p, "++") || p_is(p, "--")) {
        JsNode *n = p_node(p, JS_N_UPDATE);
        n->op = p_is(p, "++") ? JS_OP_PREINC : JS_OP_PREDEC;
        p_advance(p);
        js_node_add(p->js, n, p_unary(p));
        return n;
    }
    return p_postfix(p);
}

/**
 * @brief 构造一个二元运算节点 / Build a binary-operation node
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in]     code 运算符码 / Operator code
 * @param[in,out] l 左操作数 / Left operand
 * @param[in,out] r 右操作数 / Right operand
 *
 * @return `JS_N_BIN` 节点 / A `JS_N_BIN` node
 */
static JsNode *p_bin_make(P *p, int code, JsNode *l, JsNode *r)
{
    JsNode *n = p_node(p, JS_N_BIN);
    n->op = code;
    js_node_add(p->js, n, l);
    js_node_add(p->js, n, r);
    return n;
}

/**
 * @brief 解析指数运算 `**`（右结合）/ Parse the exponentiation operator `**` (right-associative)
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return 一元表达式或指数节点 / A unary expression or an exponentiation node
 *
 * @see p_unary
 */
static JsNode *p_exp(P *p)
{
    JsNode *l = p_unary(p);
    if (p_is(p, "**")) {
        p_advance(p);
        return p_bin_make(p, JS_OP_EXP, l, p_exp(p)); /* right associative */
    }
    return l;
}

/**
 * @brief 解析乘性运算 `* / %` / Parse the multiplicative operators `* / %`
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return 乘法层的表达式节点 / The multiplicative-level expression node
 *
 * @see p_exp
 */
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

/**
 * @brief 解析加减运算 `+ -` / Parse the additive operators `+ -`
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return 加法层的表达式节点 / The additive-level expression node
 *
 * @see p_mul
 */
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

/**
 * @brief 解析移位运算 `<< >> >>>` / Parse the shift operators `<< >> >>>`
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return 移位层的表达式节点 / The shift-level expression node
 *
 * @see p_add
 */
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

/**
 * @brief 解析关系与成员运算 `< > <= >= instanceof in` / Parse relational and membership operators `< > <= >= instanceof in`
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return 关系层的表达式节点 / The relational-level expression node
 *
 * @see p_shift
 */
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

/**
 * @brief 解析相等运算 `== != === !==` / Parse the equality operators `== != === !==`
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return 相等层的表达式节点 / The equality-level expression node
 *
 * @see p_rel
 */
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

/**
 * @brief 解析按位与 `&` / Parse bitwise AND, `&`
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return 按位与层的表达式节点 / The bitwise-AND-level expression node
 *
 * @see p_eq
 */
static JsNode *p_band(P *p)
{
    JsNode *l = p_eq(p);
    while (p_is(p, "&")) {
        p_advance(p);
        l = p_bin_make(p, JS_OP_BAND, l, p_eq(p));
    }
    return l;
}

/**
 * @brief 解析按位异或 `^` / Parse bitwise XOR, `^`
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return 按位异或层的表达式节点 / The bitwise-XOR-level expression node
 *
 * @see p_band
 */
static JsNode *p_bxor(P *p)
{
    JsNode *l = p_band(p);
    while (p_is(p, "^")) {
        p_advance(p);
        l = p_bin_make(p, JS_OP_BXOR, l, p_band(p));
    }
    return l;
}

/**
 * @brief 解析按位或 `|` / Parse bitwise OR, `|`
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return 按位或层的表达式节点 / The bitwise-OR-level expression node
 *
 * @see p_bxor
 */
static JsNode *p_bor(P *p)
{
    JsNode *l = p_bxor(p);
    while (p_is(p, "|")) {
        p_advance(p);
        l = p_bin_make(p, JS_OP_BOR, l, p_bxor(p));
    }
    return l;
}

/**
 * @brief 解析逻辑与 `&&` / Parse logical AND, `&&`
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return 逻辑与层的表达式节点 / The logical-AND-level expression node
 *
 * @see p_bor
 */
static JsNode *p_and(P *p)
{
    JsNode *l = p_bor(p);
    while (p_is(p, "&&")) {
        p_advance(p);
        l = p_bin_make(p, JS_OP_AND, l, p_bor(p));
    }
    return l;
}

/**
 * @brief 解析逻辑或 `||` / Parse logical OR, `||`
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return 逻辑或层的表达式节点 / The logical-OR-level expression node
 *
 * @see p_and
 */
static JsNode *p_or(P *p)
{
    JsNode *l = p_and(p);
    while (p_is(p, "||")) {
        p_advance(p);
        l = p_bin_make(p, JS_OP_OR, l, p_and(p));
    }
    return l;
}

/**
 * @brief 解析空值合并 `??`（右结合）/ Parse the nullish-coalescing operator `??` (right-associative)
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return 空值合并层的表达式节点 / The nullish-coalescing-level expression node
 *
 * @see p_or
 */
static JsNode *p_nullish(P *p)
{
    JsNode *l = p_or(p);
    if (p_is(p, "??")) {
        p_advance(p);
        return p_bin_make(p, JS_OP_NULLISH, l, p_nullish(p));
    }
    return l;
}

/**
 * @brief 解析条件（三元）表达式 `?:` / Parse the conditional (ternary) expression `?:`
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return `JS_N_TERNARY` 节点或条件表达式 / A `JS_N_TERNARY` node or the condition expression
 *
 * @see p_nullish, p_assignment
 */
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

/**
 * @brief 解析赋值表达式 / Parse an assignment expression
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return `JS_N_ASSIGN` 节点或三元表达式 / A `JS_N_ASSIGN` node or the ternary expression
 *
 * @note 覆盖 `=` 与全部复合赋值（含 `&&=`/`||=`/`??=`）；右结合 / Covers `=` and every compound assignment (including `&&=`, `||=`, `??=`); right-associative
 *
 * @see p_ternary
 */
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

/**
 * @brief 解析（逗号）表达式 / Parse a (comma) expression
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return `JS_N_SEQ` 序列节点，或单个赋值表达式 / A `JS_N_SEQ` sequence node, or a single assignment expression
 *
 * @see p_assignment
 */
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

/**
 * @brief 解析 for 头部的赋值目标 / Parse the target of a `for` head
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return 表达式节点 / The expression node
 *
 * @note 当前等同于 `p_expr`，形参与解构在 `p_binding_target` 处理 / Currently equivalent to `p_expr`; destructuring is handled by `p_binding_target`
 *
 * @see p_expr
 */
static JsNode *p_assign_target(P *p)
{
    HPE_UNUSED(p);
    return p_expr(p);
}
/* ---------------- statements ---------------- */

/**
 * @brief 解析变量声明列表 / Parse a variable declaration list
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in,out] decl `JS_N_VAR` 节点 / The `JS_N_VAR` node
 * @param[in]     kind 声明关键字（`var`/`let`/`const`）/ The declaration keyword (`var`/`let`/`const`)
 *
 * @note 每项是可省初值的 `JS_N_ASSIGN`（`op` 为 `JS_OP_SET`）/ Each item is a `JS_N_ASSIGN` (`op` = `JS_OP_SET`) with an optional initialiser
 *
 * @see p_var_stmt, p_for_classic
 */
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

/**
 * @brief 解析 `{ … }` 语句块 / Parse a `{ … }` statement block
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return `JS_N_BLOCK` 节点 / A `JS_N_BLOCK` node
 *
 * @warning 块内出现 `T_EOF` 时报错 / Errors when `T_EOF` appears inside the block
 */
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

/**
 * @brief 解析函数体 / Parse a function body
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return 函数体块节点 / The body block node
 *
 * @note 等同于 `p_block` / Equivalent to `p_block`
 *
 * @see p_block
 */
static JsNode *p_function_body(P *p)
{
    JsNode *blk = p_block(p);
    return blk;
}

/**
 * @brief 解析 `for (… of …)` / `for (… in …)` 头 / Parse a `for (… of …)` / `for (… in …)` head
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in]     decl_off 目标前有 var/let/const 关键字时为 1 / 1 when a var/let/const keyword precedes the target
 * @param[in]     is_async_iter for await 为 1 / 1 for `for await`
 *
 * @return `JS_N_FORIN` 或 `JS_N_FOROF` 节点 / A `JS_N_FORIN` or `JS_N_FOROF` node
 *
 * @note `decl_off` 非 0 时把绑定目标包进 `JS_N_VAR` / A non-zero `decl_off` wraps the binding target in a `JS_N_VAR`
 *
 * @see p_for
 */
static JsNode *p_for_each(P *p, int decl_off, int is_async_iter)
{
    int is_in = p_is_ident_at(p, decl_off + 1, "in");
    JsNode *loop = p_node(p, is_in ? JS_N_FORIN : JS_N_FOROF);
    JsNode *target;

    loop->op = is_async_iter;
    if (decl_off) {
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
    js_node_add(p->js, loop, target);
    js_node_add(p->js, loop, is_in ? p_expr(p) : p_assignment(p));
    p_expect(p, ")");
    js_node_add(p->js, loop, p_statement(p));
    return loop;
}

/**
 * @brief 解析经典三段式 `for (;;)` 头 / Parse the classic three-part `for (;;)` head
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return `JS_N_FOR` 节点 / A `JS_N_FOR` node
 *
 * @note init/cond/update 各段缺失时放入 `NULL` / Missing init/cond/update parts are stored as `NULL`
 *
 * @see p_for
 */
static JsNode *p_for_classic(P *p)
{
    JsNode *loop = p_node(p, JS_N_FOR);

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
    js_node_add(p->js, loop, p_is(p, ")") ? NULL : p_expr(p));
    p_expect(p, ")");
    js_node_add(p->js, loop, p_statement(p));
    return loop;
}

/**
 * @brief 解析 `for` 语句（含 `for await`）/ Parse a `for` statement (including `for await`)
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return 经典 `for`、`for-of` 或 `for-in` 节点 / A classic `for`, `for-of` or `for-in` node
 *
 * @note 按头部形态分派给 `p_for_each` 或 `p_for_classic` / Dispatches to `p_for_each` or `p_for_classic` based on the head
 *
 * @see p_for_each, p_for_classic
 */
static JsNode *p_for(P *p)
{
    int is_async_iter = 0;
    int decl_off = 0;

    p_advance(p); /* for */
    if (p_is_ident(p, "await")) {
        is_async_iter = 1;
        p_advance(p);
    }
    p_expect(p, "(");

    /* for ( [decl] x of/in y ) */
    if (p_is_ident(p, "var") || p_is_ident(p, "let") || p_is_ident(p, "const"))
        decl_off = 1;
    if (p_is_ident_at(p, decl_off + 1, "of") || p_is_ident_at(p, decl_off + 1, "in"))
        return p_for_each(p, decl_off, is_async_iter);
    return p_for_classic(p);
}

/**
 * @brief 解析 `switch (expr) { … }` / Parse `switch (expr) { … }`
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return `JS_N_SWITCH` 节点 / A `JS_N_SWITCH` node
 *
 * @note 每个 case/default 子句是一个 `JS_N_BLOCK`，首子节点为测试表达式或 `NULL` / Each case/default clause is a `JS_N_BLOCK` whose first child is the test expression or `NULL`
 *
 * @warning 非 case/default 开始子句时报错 / Errors when a clause does not start with case/default
 */
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

/**
 * @brief 解析 `try/catch/finally` / Parse `try/catch/finally`
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return `JS_N_TRY` 节点（kids: try 块、catch 子句/`NULL`、finally 块/`NULL`）/ A `JS_N_TRY` node (kids: try block, catch clause/`NULL`, finally block/`NULL`)
 *
 * @note catch 子句的 kids[0] 为绑定目标（可为 `NULL`）/ The catch clause's kids[0] is the binding target (may be `NULL`)
 */
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

/**
 * @brief 解析 `export { a, b as c } [from "src"]` / Parse `export { a, b as c } [from "src"]`
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in,out] ex 导出节点 / The export node
 *
 * @note 置 `ex->op=2`；kids[0] 为重导出列表，kids[1] 为来源模块或 `NULL` / Sets `ex->op=2`; kids[0] is the re-export list and kids[1] the source module or `NULL`
 *
 * @see p_export
 */
static void p_export_list(P *p, JsNode *ex)
{
    JsNode *list = p_node(p, JS_N_BLOCK);
    p_advance(p); /* { */
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
}

/**
 * @brief 解析 `export default <function|class|expr>` / Parse `export default <function|class|expr>`
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in,out] ex 导出节点 / The export node
 *
 * @note 置 `ex->op=1`；kids[0] 为被导出的值/声明 / Sets `ex->op=1`; kids[0] holds the exported value or declaration
 *
 * @see p_export
 */
static void p_export_default(P *p, JsNode *ex)
{
    ex->op = 1; /* export default */
    p_advance(p);
    if (p_is_ident(p, "function")) {
        int gen = p_at(p, 1)->type == T_PUNCT && !strcmp(p_at(p, 1)->text, "*");
        js_node_add(p->js, ex, p_function(p, 0, 0, gen));
    } else if (p_is_ident(p, "async") && p_is_ident_at(p, 1, "function")) {
        int gen;
        p_advance(p);
        gen = p_at(p, 1)->type == T_PUNCT && !strcmp(p_at(p, 1)->text, "*");
        js_node_add(p->js, ex, p_function(p, 0, 1, gen));
    } else if (p_is_ident(p, "class")) {
        js_node_add(p->js, ex, p_class(p));
    } else {
        js_node_add(p->js, ex, p_assignment(p));
    }
    p_at_statement_end(p);
}

/**
 * @brief 解析 `export * [as ns] from "src"` / Parse `export * [as ns] from "src"`
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in,out] ex 导出节点 / The export node
 *
 * @note 置 `ex->op=3`；带 `as ns` 时 `ex->num=1` 且 `ex->str` 为命名空间 / Sets `ex->op=3`; with `as ns`, `ex->num=1` and `ex->str` is the namespace
 *
 * @warning 缺少 `from` 时报错 / Errors when `from` is missing
 *
 * @see p_export
 */
static void p_export_star(P *p, JsNode *ex)
{
    ex->op = 3; /* export * from "..." */
    p_advance(p); /* * */
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
}

/**
 * @brief 解析一条 `export` 声明 / Parse one `export` declaration
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return `JS_N_EXPORT` 节点 / A `JS_N_EXPORT` node
 *
 * @note 按后续形态分派给 `{` 列表/`default`/`*` 或普通语句 / Dispatches on what follows to the `{` list, `default`, `*` or a plain statement
 *
 * @see p_export_list, p_export_default, p_export_star
 */
static JsNode *p_export(P *p)
{
    JsNode *ex = p_node(p, JS_N_EXPORT);
    p_advance(p); /* export */
    if (p_is(p, "{")) {
        p_export_list(p, ex);
        return ex;
    }
    if (p_is_ident(p, "default")) {
        p_export_default(p, ex);
        return ex;
    }
    if (p_is(p, "*")) {
        p_export_star(p, ex);
        return ex;
    }
    js_node_add(p->js, ex, p_statement(p));
    return ex;
}

/**
 * @brief 解析 `* as ns` 命名空间导入绑定 / Parse a `* as ns` namespace import binding
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in,out] list 导入绑定列表 / The import binding list
 *
 * @note 绑定项 `op` 为 2 / The binding's `op` is 2
 *
 * @warning 缺少 `as` 时报错 / Errors when `as` is missing
 *
 * @see p_import_decl
 */
static void p_import_namespace(P *p, JsNode *list)
{
    JsNode *pair = p_node(p, JS_N_ASSIGN);
    p_advance(p); /* * */
    if (!p_is_ident(p, "as"))
        js_error(p->js, p_line(p), "'as' expected in import");
    p_advance(p);
    pair->op = 2; /* namespace binding */
    pair->str = p_cur(p)->text;
    p_advance(p);
    js_node_add(p->js, list, pair);
}

/**
 * @brief 解析 `{ a, b as c }` 具名导入绑定 / Parse `{ a, b as c }` named import bindings
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in,out] list 导入绑定列表 / The import binding list
 *
 * @note 绑定项 `op` 为 0，`str` 为本地名，kids[0] 为来源名 / The binding's `op` is 0, `str` is the local name and kids[0] the imported name
 *
 * @see p_import_decl
 */
static void p_import_named(P *p, JsNode *list)
{
    p_advance(p); /* { */
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

/**
 * @brief 解析 `import name` 默认导入绑定 / Parse an `import name` default binding
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in,out] list 导入绑定列表 / The import binding list
 *
 * @note 无默认导入时静默返回；绑定项 `op` 为 1 / Silently returns when there is no default; the binding's `op` is 1
 *
 * @see p_import_decl
 */
static void p_import_default(P *p, JsNode *list)
{
    JsNode *pair;

    if (p_cur(p)->type != T_IDENT || p_is_ident(p, "from"))
        return;
    pair = p_node(p, JS_N_ASSIGN);
    pair->op = 1; /* default binding */
    pair->str = p_cur(p)->text;
    p_advance(p);
    js_node_add(p->js, list, pair);
    if (p_is(p, ","))
        p_advance(p);
}

/**
 * @brief 解析一条 `import` 声明 / Parse one `import` declaration
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return `JS_N_IMPORT` 节点 / A `JS_N_IMPORT` node
 *
 * @note 支持 `import "x"` 副作用导入（`op=1`）与默认/命名空间/具名组合 / Supports `import "x"` side-effect imports (`op=1`) and default/namespace/named combinations
 *
 * @warning 缺少 `from` 时报错 / Errors when `from` is missing
 *
 * @see p_import_default, p_import_namespace, p_import_named
 */
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
    p_import_default(p, list);
    if (p_is(p, "*"))
        p_import_namespace(p, list);
    else if (p_is(p, "{"))
        p_import_named(p, list);
    if (!p_is_ident(p, "from"))
        js_error(p->js, p_line(p), "'from' expected");
    p_advance(p);
    js_node_add(p->js, im, p_primary(p));
    p_at_statement_end(p);
    return im;
}

/**
 * @brief 解析 `var`/`let`/`const` 声明语句 / Parse a `var`/`let`/`const` declaration statement
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return `JS_N_VAR` 节点 / A `JS_N_VAR` node
 *
 * @warning 缺少 `;` 时报错 / Errors when `;` is missing
 *
 * @see p_var_decl
 */
static JsNode *p_var_stmt(P *p)
{
    JsNode *decl = p_node(p, JS_N_VAR);
    const char *kind = p_cur(p)->text;
    p_advance(p);
    p_var_decl(p, decl, kind);
    if (!p_at_statement_end(p))
        js_error(p->js, p_line(p), "expected ';' after variable declaration");
    return decl;
}

/**
 * @brief 解析 `if (cond) stmt [else stmt]` / Parse `if (cond) stmt [else stmt]`
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return `JS_N_IF` 节点（kids: 条件、then、可选 else）/ A `JS_N_IF` node (kids: condition, then-branch, optional else-branch)
 */
static JsNode *p_if_stmt(P *p)
{
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

/**
 * @brief 解析 `while (cond) stmt` / Parse `while (cond) stmt`
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return `JS_N_WHILE` 节点 / A `JS_N_WHILE` node
 */
static JsNode *p_while_stmt(P *p)
{
    JsNode *n = p_node(p, JS_N_WHILE);
    p_advance(p);
    p_expect(p, "(");
    js_node_add(p->js, n, p_expr(p));
    p_expect(p, ")");
    js_node_add(p->js, n, p_statement(p));
    return n;
}

/**
 * @brief 解析 `do stmt while (cond);` / Parse `do stmt while (cond);`
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return `JS_N_DO` 节点 / A `JS_N_DO` node
 *
 * @note 结尾分号可省 / The trailing semicolon is optional
 *
 * @warning 缺少 `while` 时报错 / Errors when `while` is missing
 */
static JsNode *p_do_stmt(P *p)
{
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

/**
 * @brief 解析 `throw expr;` / Parse `throw expr;`
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return `JS_N_THROW` 节点 / A `JS_N_THROW` node
 *
 * @warning `throw` 后出现换行是语法错误 / A line terminator after `throw` is a syntax error
 */
static JsNode *p_throw_stmt(P *p)
{
    Tok *t = p_cur(p);
    JsNode *n = p_node(p, JS_N_THROW);
    p_advance(p);
    if (p_cur(p)->line != t->line)
        js_error(p->js, t->line, "illegal newline after throw");
    js_node_add(p->js, n, p_expr(p));
    if (!p_at_statement_end(p))
        js_error(p->js, p_line(p), "expected ';' after throw");
    return n;
}

/**
 * @brief 解析 `return [expr];` / Parse `return [expr];`
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return `JS_N_RETURN` 节点 / A `JS_N_RETURN` node
 *
 * @note 下一词法单元换行时按自动分号插入处理，省略返回值 / Automatic semicolon insertion applies when the next token is on a new line, omitting the value
 */
static JsNode *p_return_stmt(P *p)
{
    Tok *t = p_cur(p);
    JsNode *n = p_node(p, JS_N_RETURN);
    p_advance(p);
    if (!p_is(p, ";") && !p_is(p, "}") && p_cur(p)->type != T_EOF &&
        p_cur(p)->line == t->line)
        js_node_add(p->js, n, p_expr(p));
    if (!p_at_statement_end(p))
        js_error(p->js, p_line(p), "expected ';' after return");
    return n;
}

/**
 * @brief 解析 `break [label];` / `continue [label];` / Parse `break [label];` / `continue [label];`
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return `JS_N_BREAK` 或 `JS_N_CONTINUE` 节点 / A `JS_N_BREAK` or `JS_N_CONTINUE` node
 *
 * @note 标签须与关键字同行 / The label must be on the same line as the keyword
 */
static JsNode *p_break_stmt(P *p)
{
    Tok *t = p_cur(p);
    int is_break = !strcmp(t->text, "break");
    JsNode *n = p_node(p, is_break ? JS_N_BREAK : JS_N_CONTINUE);
    p_advance(p);
    if (p_cur(p)->type == T_IDENT && p_cur(p)->line == t->line) {
        n->str = p_cur(p)->text;
        p_advance(p);
    }
    p_at_statement_end(p); /* tolerate `} else` etc. after a bare break */
    return n;
}

/**
 * @brief 解析 `debugger;` / Parse `debugger;`
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return 一个 `JS_N_EMPTY` 占位节点 / A `JS_N_EMPTY` placeholder node
 */
static JsNode *p_debugger_stmt(P *p)
{
    p_advance(p);
    if (!p_at_statement_end(p))
        js_error(p->js, p_line(p), "expected ';' after debugger");
    return p_node(p, JS_N_EMPTY);
}

/**
 * @brief 解析带标签语句 `ident: stmt` / Parse a labelled statement `ident: stmt`
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return `JS_N_LABEL` 节点 / A `JS_N_LABEL` node
 *
 * @note 调用方已排除 `default:` 与 `case:` / The caller has already excluded `default:` and `case:`
 *
 * @see p_statement
 */
static JsNode *p_label_stmt(P *p)
{
    JsNode *lab = p_node(p, JS_N_LABEL);
    lab->str = p_cur(p)->text;
    p_advance(p);
    p_advance(p);
    js_node_add(p->js, lab, p_statement(p));
    return lab;
}

/**
 * @brief 解析 `function` / `async function` 声明 / Parse a `function` / `async function` declaration
 *
 * @param[in,out] p 解析器状态 / Parser state
 * @param[in]     is_async async 声明为 1 / 1 for an async declaration
 *
 * @return `JS_N_FUNC` 节点 / A `JS_N_FUNC` node
 *
 * @see p_function
 */
static JsNode *p_function_stmt(P *p, int is_async)
{
    int gen;

    if (is_async)
        p_advance(p); /* async */
    gen = p_at(p, 1)->type == T_PUNCT && !strcmp(p_at(p, 1)->text, "*");
    return p_function(p, 1, is_async, gen);
}

/**
 * @brief 解析由关键字开头的语句 / Resolve statements that begin with a keyword
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return 对应的语句节点；普通表达式开头时返回 `NULL` / The matching statement node, or `NULL` when the token is an ordinary expression start
 *
 * @note 分派到 var/function/class/if/while/do/for/switch/try/throw/return/break/debugger/export/import / Dispatches to var/function/class/if/while/do/for/switch/try/throw/return/break/debugger/export/import
 *
 * @see p_statement
 */
static JsNode *p_keyword_stmt(P *p)
{
    Tok *t = p_cur(p);

    if (!strcmp(t->text, "var") || !strcmp(t->text, "let") || !strcmp(t->text, "const"))
        return p_var_stmt(p);
    if (!strcmp(t->text, "function"))
        return p_function_stmt(p, 0);
    if (!strcmp(t->text, "async") && p_is_ident_at(p, 1, "function"))
        return p_function_stmt(p, 1);
    if (!strcmp(t->text, "class"))
        return p_class(p);
    if (!strcmp(t->text, "if"))
        return p_if_stmt(p);
    if (!strcmp(t->text, "while"))
        return p_while_stmt(p);
    if (!strcmp(t->text, "do"))
        return p_do_stmt(p);
    if (!strcmp(t->text, "for"))
        return p_for(p);
    if (!strcmp(t->text, "switch"))
        return p_switch(p);
    if (!strcmp(t->text, "try"))
        return p_try(p);
    if (!strcmp(t->text, "throw"))
        return p_throw_stmt(p);
    if (!strcmp(t->text, "return"))
        return p_return_stmt(p);
    if (!strcmp(t->text, "break") || !strcmp(t->text, "continue"))
        return p_break_stmt(p);
    if (!strcmp(t->text, "debugger"))
        return p_debugger_stmt(p);
    if (!strcmp(t->text, "export"))
        return p_export(p);
    if (!strcmp(t->text, "import") && !(p_at(p, 1)->type == T_PUNCT &&
                                        !strcmp(p_at(p, 1)->text, "(")))
        return p_import_decl(p);
    return NULL;
}

/**
 * @brief 解析一条语句 / Parse one statement
 *
 * @param[in,out] p 解析器状态 / Parser state
 *
 * @return 块、空语句、标签、关键字语句或表达式语句节点 / A block, empty, labelled, keyword or expression statement node
 *
 * @note `{` 后跟标识符加 `:`（非 default/case）视为标签语句 / `{` is a block; an identifier followed by `:` (other than default/case) is a label
 *
 * @see p_keyword_stmt, p_label_stmt, p_expr
 */
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
        JsNode *kw;
        if (p_at(p, 1)->type == T_PUNCT && !strcmp(p_at(p, 1)->text, ":") &&
            strcmp(t->text, "default") != 0 && strcmp(t->text, "case") != 0)
            return p_label_stmt(p);
        kw = p_keyword_stmt(p);
        if (kw)
            return kw;
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

/**
 * @brief 词法分析与解析的共用入口 / Shared entry point for lexing and parsing
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     source 源文本 / Source text
 * @param[in]     module 按 ES 模块标记为 1 / 1 to mark the program as an ES module
 *
 * @return 语句块根节点（`op` 标记是否模块）/ The statement-block root node (`op` marks module mode)
 *
 * @note 先 `lex_all` 再循环 `p_statement` 直到 `T_EOF` / Runs `lex_all` then loops `p_statement` until `T_EOF`
 *
 * @see js_parse_program, js_parse_module
 */

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

/**
 * @brief 把脚本文本解析成程序 AST / Parse script text into a program AST
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     source 源文本 / Source text
 *
 * @return 程序语句块根节点（脚本模式）/ The program statement-block root (script mode)
 *
 * @note 等价于 `parse_common(js, source, 0)` / Equivalent to `parse_common(js, source, 0)`
 *
 * @see parse_common
 */
JsNode *js_parse_program(HpeJs *js, const char *source)
{
    return parse_common(js, source, 0);
}

/**
 * @brief 把模块文本解析成模块 AST / Parse module text into a module AST
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     source 源文本 / Source text
 *
 * @return 程序语句块根节点（模块模式）/ The program statement-block root (module mode)
 *
 * @note 等价于 `parse_common(js, source, 1)` / Equivalent to `parse_common(js, source, 1)`
 *
 * @see parse_common
 */
JsNode *js_parse_module(HpeJs *js, const char *source)
{
    return parse_common(js, source, 1);
}

/**
 * @brief 把独立表达式源码解析成 AST / Parse a standalone expression source into an AST
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     source 源文本 / Source text
 *
 * @return 表达式根节点 / The expression root node
 *
 * @note 供模块加载器与 DOM 桥复用 / Exported for the module loader and the DOM bridge
 *
 * @see p_expr
 */
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




