/* Hope UI Engine - unit tests for src/base/hpe_util (string, utf8, length, color) */
#include "hpe_test.h"

#include <string.h>

#include "base/hpe_util.h"

/* ---------------- string helpers ---------------- */

/**
 * @brief 测试 ASCII 大小写转换与空白判断 / Test ASCII case folding and whitespace tests
 */
HPE_TEST_CASE(string, ascii_and_space)
{
    HPE_TEST_ASSERT_INT(hpe_ascii_lower('A'), 'a');
    HPE_TEST_ASSERT_INT(hpe_ascii_lower('z'), 'z');
    HPE_TEST_ASSERT_INT(hpe_ascii_lower('@'), '@');
    HPE_TEST_ASSERT(hpe_is_space(' '));
    HPE_TEST_ASSERT(hpe_is_space('\t'));
    HPE_TEST_ASSERT(!hpe_is_space('x'));
    HPE_TEST_ASSERT(!hpe_is_space('\0'));
}

/**
 * @brief 测试忽略大小写的字符串比较 / Test case-insensitive string comparison
 */
HPE_TEST_CASE(string, case_insensitive_compare)
{
    HPE_TEST_ASSERT(hpe_str_ieq("DIV", "div"));
    HPE_TEST_ASSERT(!hpe_str_ieq("diva", "div"));
    HPE_TEST_ASSERT(hpe_str_ieq(NULL, NULL));
    HPE_TEST_ASSERT(!hpe_str_ieq(NULL, "x"));
    HPE_TEST_ASSERT(hpe_str_ieq_n("Helvetica", "helvetica", 9));
    HPE_TEST_ASSERT(!hpe_str_ieq_n("hel", "he", 3));
    HPE_TEST_ASSERT(hpe_starts_with_i("REM-ark", "rem"));
    HPE_TEST_ASSERT(!hpe_starts_with_i("div", "span"));
}

/**
 * @brief 测试就地去首尾空白 / Test in-place leading/trailing whitespace trim
 */
HPE_TEST_CASE(string, trim)
{
    char buf[] = "  hello \t\n";
    char empty[] = "   ";
    char clean[] = "x";

    HPE_TEST_ASSERT_STR(hpe_trim(buf), "hello");
    HPE_TEST_ASSERT_STR(hpe_trim(empty), "");
    HPE_TEST_ASSERT_STR(hpe_trim(clean), "x");
    HPE_TEST_ASSERT(hpe_trim(NULL) == NULL);
}

/**
 * @brief 测试字符串复制与释放 / Test string duplication and release
 */
HPE_TEST_CASE(string, dup_and_free)
{
    char *s = hpe_strdup("abc");
    char *n = hpe_strndup("abcdef", 3);
    char *n2 = hpe_strndup("ab", 10);

    HPE_TEST_ASSERT_STR(s, "abc");
    HPE_TEST_ASSERT_STR(n, "abc");
    HPE_TEST_ASSERT_STR(n2, "ab");
    HPE_TEST_ASSERT(hpe_strdup(NULL) == NULL);
    hpe_free(s);
    hpe_free(n);
    hpe_free(n2);
    hpe_free(NULL);
}

/**
 * @brief 测试数组容量倍增逻辑 / Test the array capacity doubling logic
 */
HPE_TEST_CASE(string, grow)
{
    size_t cap = 0;
    int *arr = (int *)hpe_grow(NULL, &cap, 5, sizeof(int));

    HPE_TEST_ASSERT(arr != NULL);
    HPE_TEST_ASSERT_INT((int)cap, 8);
    arr[4] = 7;
    arr = (int *)hpe_grow(arr, &cap, 9, sizeof(int));
    HPE_TEST_ASSERT_INT((int)cap, 16);
    HPE_TEST_ASSERT_INT(arr[4], 7);
    hpe_free(arr);
}

/* ---------------- utf-8 ---------------- */

/**
 * @brief 测试单字节与 NUL 的解码 / Test decoding of single bytes and NUL
 */
HPE_TEST_CASE(utf8, decode_ascii)
{
    uint32_t cp = 0;

    HPE_TEST_ASSERT_INT(hpe_utf8_next("A", &cp), 1);
    HPE_TEST_ASSERT_INT((int)cp, 'A');
    HPE_TEST_ASSERT_INT(hpe_utf8_next("\0z", &cp), 1);
    HPE_TEST_ASSERT_INT((int)cp, 0);
}

/**
 * @brief 测试 2/3/4 字节多字节序列解码 / Test decoding of 2/3/4-byte sequences
 */
HPE_TEST_CASE(utf8, decode_multibyte)
{
    uint32_t cp = 0;

    HPE_TEST_ASSERT_INT(hpe_utf8_next("\xC3\xA9", &cp), 2); /* U+00E9 é */
    HPE_TEST_ASSERT_INT((int)cp, 0xE9);
    HPE_TEST_ASSERT_INT(hpe_utf8_next("\xE4\xB8\xAD", &cp), 3); /* U+4E2D 中 */
    HPE_TEST_ASSERT_INT((int)cp, 0x4E2D);
    HPE_TEST_ASSERT_INT(hpe_utf8_next("\xF0\x9F\x98\x80", &cp), 4); /* U+1F600 */
    HPE_TEST_ASSERT_INT((int)cp, 0x1F600);
}

/**
 * @brief 测试非法序列回退为 U+FFFD 且只前进 1 字节 / Test illegal sequences falling back to U+FFFD with a 1-byte advance
 */
HPE_TEST_CASE(utf8, decode_illegal)
{
    uint32_t cp = 0;

    HPE_TEST_ASSERT_INT(hpe_utf8_next("\x80", &cp), 1); /* stray continuation */
    HPE_TEST_ASSERT_INT((int)cp, 0xFFFD);
    HPE_TEST_ASSERT_INT(hpe_utf8_next("\xF8\x88\x80\x80\x80", &cp), 1); /* 5-byte lead */
    HPE_TEST_ASSERT_INT((int)cp, 0xFFFD);
    HPE_TEST_ASSERT_INT(hpe_utf8_next("\xC3 A", &cp), 1); /* truncated 2-byte */
    HPE_TEST_ASSERT_INT((int)cp, 0xFFFD);
    HPE_TEST_ASSERT_INT(hpe_utf8_next("\xE4\xB8\x41", &cp), 1); /* broken 3-byte */
    HPE_TEST_ASSERT_INT((int)cp, 0xFFFD);
}

/**
 * @brief 测试向前回退一个码点 / Test stepping back one code point
 */
HPE_TEST_CASE(utf8, prev)
{
    const char *s = "a\xC3\xA9\xE4\xB8\xAD"; /* a é 中 */

    HPE_TEST_ASSERT_INT(hpe_utf8_prev(s, s), 0);
    HPE_TEST_ASSERT_INT(hpe_utf8_prev(s, s + 1), 1);      /* back to 'a' */
    HPE_TEST_ASSERT_INT(hpe_utf8_prev(s, s + 3), 2);      /* over é */
    HPE_TEST_ASSERT_INT(hpe_utf8_prev(s, s + 6), 3);      /* over 中 */
}

/**
 * @brief 测试码点编码及与解码的往返 / Test code point encoding and the encode/decode roundtrip
 */
HPE_TEST_CASE(utf8, encode_roundtrip)
{
    char buf[4];
    uint32_t cp = 0;

    HPE_TEST_ASSERT_INT(hpe_utf8_encode('A', buf), 1);
    HPE_TEST_ASSERT_INT((int)(unsigned char)buf[0], 'A');
    HPE_TEST_ASSERT_INT(hpe_utf8_encode(0x4E2D, buf), 3);
    HPE_TEST_ASSERT_INT(hpe_utf8_next(buf, &cp), 3);
    HPE_TEST_ASSERT_INT((int)cp, 0x4E2D);
    HPE_TEST_ASSERT_INT(hpe_utf8_encode(0x1F600, buf), 4);
    HPE_TEST_ASSERT_INT(hpe_utf8_next(buf, &cp), 4);
    HPE_TEST_ASSERT_INT((int)cp, 0x1F600);
}

/* ---------------- lengths ---------------- */

/**
 * @brief 测试长度构造与谓词 / Test length constructors and predicates
 */
HPE_TEST_CASE(length, constructors)
{
    HpeLength l = hpe_len_auto();

    HPE_TEST_ASSERT(hpe_len_is_auto(l));
    HPE_TEST_ASSERT(!hpe_len_is_px(l));
    HPE_TEST_ASSERT(hpe_len_is_px(hpe_len_px(4.0f)));
    HPE_TEST_ASSERT(hpe_len_is_auto(hpe_len_parse(NULL)));
}

/**
 * @brief 测试常见 CSS 长度字面量解析 / Test parsing of common CSS length literals
 */
HPE_TEST_CASE(length, parse_basic)
{
    HpeLength l = hpe_len_parse("10px");

    HPE_TEST_ASSERT_INT((int)l.unit, HPE_LEN_PX);
    HPE_TEST_ASSERT_NEAR(l.value, 10.0, 1e-6);
    l = hpe_len_parse("50%");
    HPE_TEST_ASSERT_INT((int)l.unit, HPE_LEN_PCT);
    HPE_TEST_ASSERT_NEAR(l.value, 50.0, 1e-6);
    l = hpe_len_parse("1.5em");
    HPE_TEST_ASSERT_INT((int)l.unit, HPE_LEN_EM);
    HPE_TEST_ASSERT_NEAR(l.value, 1.5, 1e-6);
    l = hpe_len_parse("2rem");
    HPE_TEST_ASSERT_INT((int)l.unit, HPE_LEN_REM);
    HPE_TEST_ASSERT_NEAR(l.value, 2.0, 1e-6);
}

/**
 * @brief 测试 auto、物理单位与未知后缀回退 / Test auto, physical units and the unknown-suffix fallback
 */
HPE_TEST_CASE(length, parse_edge)
{
    HpeLength l = hpe_len_parse("auto");

    HPE_TEST_ASSERT(hpe_len_is_auto(l));
    l = hpe_len_parse("1pt"); /* 96/72 px */
    HPE_TEST_ASSERT_INT((int)l.unit, HPE_LEN_PX);
    HPE_TEST_ASSERT_NEAR(l.value, 96.0 / 72.0, 1e-4);
    l = hpe_len_parse("2pc"); /* 2 * 16 px */
    HPE_TEST_ASSERT_NEAR(l.value, 32.0, 1e-4);
    l = hpe_len_parse("1in");
    HPE_TEST_ASSERT_NEAR(l.value, 96.0, 1e-4);
    l = hpe_len_parse("10xx"); /* unknown suffix falls back to px */
    HPE_TEST_ASSERT_INT((int)l.unit, HPE_LEN_PX);
    HPE_TEST_ASSERT_NEAR(l.value, 10.0, 1e-6);
}

/**
 * @brief 测试单基准与完整上下文求值 / Test resolving with a single base and with a full context
 */
HPE_TEST_CASE(length, resolve)
{
    HPE_TEST_ASSERT_NEAR(hpe_len_resolve(hpe_len_px(12.0f), 100.0f), 12.0, 1e-6);
    HPE_TEST_ASSERT_NEAR(hpe_len_resolve(hpe_len_pct(50.0f), 200.0f), 100.0, 1e-6);
    HPE_TEST_ASSERT_NEAR(hpe_len_resolve(hpe_len_auto(), 100.0f), -1.0, 1e-6);
    /* em with an explicit font size */
    HPE_TEST_ASSERT_NEAR(hpe_len_resolve_full(hpe_len_unit(2.0f, HPE_LEN_EM),
                                              0.0f, 14.0f, 0.0f, 0.0f, 0.0f, 0.0f),
                         28.0, 1e-5);
    /* rem falls back to 16 when the root font size is 0 */
    HPE_TEST_ASSERT_NEAR(hpe_len_resolve_full(hpe_len_unit(2.0f, HPE_LEN_REM),
                                              0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f),
                         32.0, 1e-5);
    /* vw: 50vw of a 800px viewport */
    HPE_TEST_ASSERT_NEAR(hpe_len_resolve_full(hpe_len_unit(50.0f, HPE_LEN_VW),
                                              0.0f, 0.0f, 0.0f, 800.0f, 0.0f, 0.0f),
                         400.0, 1e-5);
}

/* ---------------- colors ---------------- */

/**
 * @brief 测试十六进制颜色解析 / Test hexadecimal colour parsing
 */
HPE_TEST_CASE(color, hex)
{
    HpeColor c;

    HPE_TEST_ASSERT(hpe_color_parse("#ff0000", &c));
    HPE_TEST_ASSERT_INT(c.r, 255);
    HPE_TEST_ASSERT_INT(c.g, 0);
    HPE_TEST_ASSERT_INT(c.b, 0);
    HPE_TEST_ASSERT_INT(c.a, 255);
    HPE_TEST_ASSERT(hpe_color_parse("#f00", &c));
    HPE_TEST_ASSERT_INT(c.r, 255);
    HPE_TEST_ASSERT_INT(c.b, 0);
    HPE_TEST_ASSERT(hpe_color_parse("#f00f", &c));
    HPE_TEST_ASSERT_INT(c.a, 255);
    HPE_TEST_ASSERT(hpe_color_parse("#11223344", &c));
    HPE_TEST_ASSERT_INT(c.r, 0x11);
    HPE_TEST_ASSERT_INT(c.a, 0x44);
    HPE_TEST_ASSERT(!hpe_color_parse("#ff", &c));
    HPE_TEST_ASSERT(!hpe_color_parse("#zzzzzz", &c));
}

/**
 * @brief 测试具名颜色解析 / Test named colour parsing
 */
HPE_TEST_CASE(color, named)
{
    HpeColor c;

    HPE_TEST_ASSERT(hpe_color_parse("red", &c));
    HPE_TEST_ASSERT_INT(c.r, 255);
    HPE_TEST_ASSERT_INT(c.a, 255);
    HPE_TEST_ASSERT(hpe_color_parse("  WHITE ", &c));
    HPE_TEST_ASSERT_INT(c.b, 255);
    HPE_TEST_ASSERT(hpe_color_parse("transparent", &c));
    HPE_TEST_ASSERT_INT(c.a, 0);
    HPE_TEST_ASSERT(!hpe_color_parse("notacolor", &c));
    HPE_TEST_ASSERT(!hpe_color_parse(NULL, &c));
}

/**
 * @brief 测试 rgb()/rgba() 解析 / Test rgb() and rgba() parsing
 */
HPE_TEST_CASE(color, rgb)
{
    HpeColor c;

    HPE_TEST_ASSERT(hpe_color_parse("rgb(1, 2, 3)", &c));
    HPE_TEST_ASSERT_INT(c.r, 1);
    HPE_TEST_ASSERT_INT(c.g, 2);
    HPE_TEST_ASSERT_INT(c.b, 3);
    HPE_TEST_ASSERT_INT(c.a, 255);
    HPE_TEST_ASSERT(hpe_color_parse("rgba(10, 20, 30, 0.5)", &c));
    HPE_TEST_ASSERT_INT(c.a, 127);
    HPE_TEST_ASSERT(hpe_color_parse("rgb(50%, 0%, 100%)", &c));
    HPE_TEST_ASSERT_INT(c.r, 127);
    HPE_TEST_ASSERT_INT(c.b, 255);
    HPE_TEST_ASSERT(!hpe_color_parse("rgb(1, 2)", &c));
}

/**
 * @brief 测试 hsl() 解析 / Test hsl() parsing
 */
HPE_TEST_CASE(color, hsl)
{
    HpeColor c;

    HPE_TEST_ASSERT(hpe_color_parse("hsl(120, 100%, 50%)", &c));
    HPE_TEST_ASSERT_INT(c.r, 0);
    HPE_TEST_ASSERT_INT(c.g, 255);
    HPE_TEST_ASSERT_INT(c.b, 0);
    HPE_TEST_ASSERT_INT(c.a, 255);
    HPE_TEST_ASSERT(hpe_color_parse("hsl(240, 100%, 50%)", &c));
    HPE_TEST_ASSERT_INT(c.r, 0);
    HPE_TEST_ASSERT_INT(c.g, 0);
    HPE_TEST_ASSERT_INT(c.b, 255);
    HPE_TEST_ASSERT(!hpe_color_parse("hsl(120, 100%)", &c));
}

/* ---------------- latin-1 transcoding ---------------- */

/**
 * @brief 测试 Latin-1/cp1252 到 UTF-8 的转码 / Test Latin-1/cp1252 to UTF-8 transcoding
 */
HPE_TEST_CASE(base, latin1_to_utf8)
{
    size_t out_len = 0;
    /* "caf<E9>" plus a cp1252 euro sign (0x80) */
    char src[] = { 'c', 'a', 'f', (char)0xE9, (char)0x80 };
    char *out = hpe_latin1_to_utf8(src, 5, &out_len);

    HPE_TEST_ASSERT_INT((int)out_len, 8); /* 3 + 2 bytes for é + 3 bytes for € */
    HPE_TEST_ASSERT_INT((int)(unsigned char)out[3], 0xC3);
    HPE_TEST_ASSERT_INT((int)(unsigned char)out[4], 0xA9);
    HPE_TEST_ASSERT_INT((int)(unsigned char)out[5], 0xE2);
    HPE_TEST_ASSERT_INT((int)(unsigned char)out[6], 0x82);
    HPE_TEST_ASSERT_INT((int)(unsigned char)out[7], 0xAC);
    hpe_free(out);
}
