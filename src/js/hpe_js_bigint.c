/* Hope UI Engine - arbitrary-precision BigInt
 *
 * BigInt is modelled as a primitive type (`JS_BIGINT`), not an object, so
 * `typeof` and `===` behave per spec. The value carries an opaque pointer to
 * an `HpeBigInt` held in the interpreter arena: a sign plus a little-endian
 * array of base-2^32 limbs.
 *
 * Base 2^32 is chosen because the bitwise operators and the two's-complement
 * wrapping used by `BigInt.asIntN` / `asUintN` are then direct limb
 * operations, while radix output is a short repeated-division loop.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "js/hpe_js_internal.h"

struct HpeBigInt {
    int sign;       /* -1, 0 or 1 */
    int n;          /* limb count, always >= 1 */
    int cap;        /* allocated limbs */
    uint32_t *limb; /* little-endian, base 2^32 */
};

static const char k_digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";

/* ---------------- allocation & helpers ---------------- */

/**
 * @brief 数字字符转数值 / Convert a digit character to its value
 *
 * @param[in] c 字符 / Character
 *
 * @return 0..35，非数字返回 -1 / 0..35, or -1 when not a digit
 */
static int bi_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z')
        return c - 'A' + 10;
    return -1;
}

/**
 * @brief 在 arena 中分配一个大整数 / Allocate a BigInt in the arena
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     cap 预留 limb 数（至少 1）/ Limb capacity, at least 1
 *
 * @return 值为 0 的新大整数 / The new integer, holding zero
 */
static HpeBigInt *bi_new(HpeJs *js, int cap)
{
    HpeBigInt *a;
    if (cap < 1)
        cap = 1;
    a = (HpeBigInt *)hpe_js_alloc(js, sizeof(HpeBigInt));
    a->limb = (uint32_t *)hpe_js_alloc(js, (size_t)cap * sizeof(uint32_t));
    a->sign = 0;
    a->n = 1;
    a->cap = cap;
    return a;
}

/**
 * @brief 判断大整数是否为零 / Test whether a BigInt is zero
 *
 * @param[in] a 大整数 / The integer
 *
 * @return 为零返回 1 / 1 when zero
 */
static int bi_is_zero(const HpeBigInt *a)
{
    return a->sign == 0;
}

/**
 * @brief 去掉高位零 limb 并修正符号 / Strip high zero limbs and fix the sign
 *
 * @param[in,out] a 大整数 / The integer
 */
static void bi_norm(HpeBigInt *a)
{
    while (a->n > 1 && a->limb[a->n - 1] == 0)
        a->n--;
    if (a->n == 1 && a->limb[0] == 0)
        a->sign = 0;
    else if (a->sign == 0)
        a->sign = 1;
}

/**
 * @brief 复制一个大整数 / Copy a BigInt
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     a 源大整数 / Source integer
 *
 * @return 独立的新副本 / An independent copy
 */
static HpeBigInt *bi_copy(HpeJs *js, const HpeBigInt *a)
{
    HpeBigInt *b = bi_new(js, a->n);
    b->sign = a->sign;
    b->n = a->n;
    memcpy(b->limb, a->limb, (size_t)a->n * sizeof(uint32_t));
    return b;
}

/**
 * @brief 按符号改写结果 / Apply a sign to a result
 *
 * @param[in,out] a 大整数 / The integer
 * @param[in]     sign 目标符号 / The desired sign
 *
 * @note 零值始终规范化为符号 0 / Zero is always normalised to sign 0
 */
static void bi_apply_sign(HpeBigInt *a, int sign)
{
    if (a->n == 1 && a->limb[0] == 0)
        a->sign = 0;
    else
        a->sign = sign < 0 ? -1 : 1;
}

/* ---------------- magnitude arithmetic ---------------- */

/**
 * @brief 比较两个大整数的绝对值 / Compare the magnitudes of two BigInts
 *
 * @param[in] a 左操作数 / Left operand
 * @param[in] b 右操作数 / Right operand
 *
 * @return |a| < |b| 返回 -1，相等返回 0，否则返回 1 / -1 when |a| < |b|, 0 when equal, otherwise 1
 */
static int bi_cmp_abs(const HpeBigInt *a, const HpeBigInt *b)
{
    int i;
    if (a->n != b->n)
        return a->n < b->n ? -1 : 1;
    for (i = a->n - 1; i >= 0; i--)
        if (a->limb[i] != b->limb[i])
            return a->limb[i] < b->limb[i] ? -1 : 1;
    return 0;
}

/**
 * @brief 绝对值相加 / Add two magnitudes
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     a 左操作数 / Left operand
 * @param[in]     b 右操作数 / Right operand
 *
 * @return 和（符号为正）/ The sum, with a positive sign
 */
static HpeBigInt *bi_add_abs(HpeJs *js, const HpeBigInt *a, const HpeBigInt *b)
{
    int i, m = a->n > b->n ? a->n : b->n;
    HpeBigInt *r = bi_new(js, m + 1);
    uint64_t carry = 0;
    for (i = 0; i < m; i++) {
        uint64_t s = carry;
        if (i < a->n)
            s += a->limb[i];
        if (i < b->n)
            s += b->limb[i];
        r->limb[i] = (uint32_t)(s & 0xFFFFFFFFu);
        carry = s >> 32;
    }
    r->n = m;
    if (carry) {
        r->limb[m] = (uint32_t)carry;
        r->n = m + 1;
    }
    r->sign = 1;
    bi_norm(r);
    return r;
}

/**
 * @brief 就地做绝对值减法（要求 |a| >= |b|）/ Subtract magnitudes in place, requiring |a| >= |b|
 *
 * @param[in,out] a 被减数，就地改写为差 / Minuend, rewritten with the difference
 * @param[in]     b 减数 / Subtrahend
 *
 * @note 调用方负责随后设定符号；`|a| == |b|` 时得到零 / The caller sets the sign afterwards; `|a| == |b|` yields zero
 */
static void bi_sub_abs_inplace(HpeBigInt *a, const HpeBigInt *b)
{
    int i;
    int64_t borrow = 0;
    for (i = 0; i < a->n; i++) {
        int64_t d = (int64_t)a->limb[i] - borrow - (i < b->n ? (int64_t)b->limb[i] : 0);
        if (d < 0) {
            d += 0x100000000LL;
            borrow = 1;
        } else {
            borrow = 0;
        }
        a->limb[i] = (uint32_t)d;
    }
    bi_norm(a);
}

/**
 * @brief 绝对值相减 / Subtract two magnitudes
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     a 被减数（绝对值较大者）/ Minuend, the larger magnitude
 * @param[in]     b 减数 / Subtrahend
 *
 * @return 差（符号为正）/ The difference, with a positive sign
 */
static HpeBigInt *bi_sub_abs(HpeJs *js, const HpeBigInt *a, const HpeBigInt *b)
{
    HpeBigInt *r = bi_copy(js, a);
    bi_sub_abs_inplace(r, b);
    r->sign = 1;
    bi_norm(r);
    return r;
}

/**
 * @brief 绝对值相乘 / Multiply two magnitudes
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     a 左操作数 / Left operand
 * @param[in]     b 右操作数 / Right operand
 *
 * @return 积（符号为正）/ The product, with a positive sign
 *
 * @note 教科书式 `O(n*m)` 累加；内层进位落在 `i + b->n`，正是下一轮内层要读的首槽 / Textbook `O(n*m)` accumulation; the inner carry lands on `i + b->n`, the first slot the next round reads
 */
static HpeBigInt *bi_mul_abs(HpeJs *js, const HpeBigInt *a, const HpeBigInt *b)
{
    int i, j, n = a->n + b->n;
    HpeBigInt *r = bi_new(js, n + 1);
    for (i = 0; i < a->n; i++) {
        uint64_t carry = 0;
        for (j = 0; j < b->n; j++) {
            uint64_t s = (uint64_t)a->limb[i] * b->limb[j] + r->limb[i + j] + carry;
            r->limb[i + j] = (uint32_t)(s & 0xFFFFFFFFu);
            carry = s >> 32;
        }
        r->limb[i + b->n] = (uint32_t)carry;
    }
    r->n = n;
    r->sign = 1;
    bi_norm(r);
    return r;
}

/* ---------------- bit-level access ---------------- */

/**
 * @brief 读取一个二进制位 / Read one bit
 *
 * @param[in] a 大整数（按绝对值解读）/ The integer, read as a magnitude
 * @param[in] i 位下标（0 为最低位）/ Bit index, 0 being the least significant
 *
 * @return 该位为 1 返回 1 / 1 when the bit is set
 */
static int bi_get_bit(const HpeBigInt *a, int i)
{
    int w = i >> 5;
    if (w >= a->n)
        return 0;
    return (int)((a->limb[w] >> (i & 31)) & 1u);
}

/**
 * @brief 置一个二进制位 / Set one bit
 *
 * @param[in,out] a 大整数 / The integer
 * @param[in]     i 位下标 / Bit index
 */
static void bi_set_bit(HpeBigInt *a, int i)
{
    int w = i >> 5;
    if (w < a->cap)
        a->limb[w] |= (uint32_t)1u << (i & 31);
}

/**
 * @brief 就地左移一位 / Shift left by one bit in place
 *
 * @param[in,out] a 大整数 / The integer
 */
static void bi_shl1(HpeBigInt *a)
{
    int i;
    uint32_t carry = 0;
    for (i = 0; i < a->n; i++) {
        uint32_t v = (uint32_t)(a->limb[i] >> 31);
        a->limb[i] = (uint32_t)((a->limb[i] << 1) | carry);
        carry = v;
    }
    if (carry && a->n < a->cap) {
        a->limb[a->n] = carry;
        a->n++;
    }
}

/* ---------------- division ---------------- */

/**
 * @brief 用一个小整数除并就地取余 / Divide by a small integer in place
 *
 * @param[in,out] a 被除数，就地改写为商 / Dividend, rewritten with the quotient
 * @param[in]     d 除数（非零）/ The divisor, non-zero
 *
 * @return 余数（非负）/ The non-negative remainder
 */
static uint32_t bi_divmod_small_inplace(HpeBigInt *a, uint32_t d)
{
    uint64_t rem = 0;
    int i;
    for (i = a->n - 1; i >= 0; i--) {
        uint64_t cur = (rem << 32) | a->limb[i];
        a->limb[i] = (uint32_t)(cur / d);
        rem = cur % d;
    }
    bi_norm(a);
    return (uint32_t)rem;
}

/**
 * @brief 绝对值相除并取余 / Divide two magnitudes and take the remainder
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     a 被除数 / Dividend
 * @param[in]     b 除数（非零）/ Divisor, non-zero
 * @param[out]    qo 接收商的绝对值 / Receives the quotient magnitude
 * @param[out]    ro 接收余数的绝对值 / Receives the remainder magnitude
 *
 * @note 二进制长除法（逐位试减），复杂度 `O(bits * limbs)`；够用且完全不引入浮点 / Binary long division, `O(bits * limbs)`; adequate and entirely free of floating point
 */
static void bi_divmod_abs(HpeJs *js, const HpeBigInt *a, const HpeBigInt *b, HpeBigInt **qo,
                          HpeBigInt **ro)
{
    HpeBigInt *q = bi_new(js, a->n + 1);
    HpeBigInt *r = bi_new(js, a->n + 2);
    int bits = a->n * 32;
    int i;
    q->n = a->n + 1;
    for (i = bits - 1; i >= 0; i--) {
        bi_shl1(r);
        if (bi_get_bit(a, i))
            r->limb[0] |= 1u;
        bi_apply_sign(r, 1);
        if (bi_cmp_abs(r, b) >= 0) {
            bi_sub_abs_inplace(r, b);
            bi_set_bit(q, i);
        }
    }
    bi_norm(q);
    bi_norm(r);
    bi_apply_sign(q, 1);
    bi_apply_sign(r, 1);
    *qo = q;
    *ro = r;
}

/* ---------------- two's complement ---------------- */

/**
 * @brief 把大整数写成定宽二进制补码 / Render a BigInt as fixed-width two's complement
 *
 * @param[in]  a 大整数 / The integer
 * @param[out] out 接收 `nl` 个 limb / Receives `nl` limbs
 * @param[in]  nl 目标宽度（limb 数）/ Target width in limbs
 *
 * @note 负数按 2^(32*nl) - |a| 展开；超出宽度的位被截断。`a` 之外的高位 limb 必须补 `0xFFFFFFFF` 再进位，否则 `asIntN(64, -1n)` 会错算成 `2^32 - 1` / A negative value expands as 2^(32*nl) - |a|; bits beyond the width are truncated. Limbs above `a` must be filled with `0xFFFFFFFF` before the carry, or `asIntN(64, -1n)` yields `2^32 - 1`
 */
static void bi_to_twos(const HpeBigInt *a, uint32_t *out, int nl)
{
    int i;
    memset(out, 0, (size_t)nl * sizeof(uint32_t));
    if (a->sign >= 0) {
        for (i = 0; i < a->n && i < nl; i++)
            out[i] = a->limb[i];
        return;
    }
    for (i = 0; i < nl; i++)
        out[i] = i < a->n ? ~a->limb[i] : 0xFFFFFFFFu;
    for (i = 0; i < nl; i++) {
        out[i] = out[i] + 1u;
        if (out[i] != 0)
            break;
    }
}

/**
 * @brief 截断到指定位宽 / Truncate to a bit width
 *
 * @param[in,out] a 大整数 / The integer
 * @param[in]     nbits 保留的低位数量 / Number of low bits to keep
 *
 * @note 仅供补码解包使用，因此只改幅值、不动符号 / Used only when unpacking two's complement, so it touches the magnitude only
 */
static void bi_mask_bits(HpeBigInt *a, int nbits)
{
    int top = (nbits - 1) >> 5, b = nbits & 31, i;
    if (nbits <= 0) {
        a->limb[0] = 0;
        a->n = 1;
        return;
    }
    for (i = top + 1; i < a->n; i++)
        a->limb[i] = 0;
    if (b && top < a->n)
        a->limb[top] &= (uint32_t)((1u << b) - 1u);
    bi_norm(a);
}

/**
 * @brief 从定宽二进制补码还原大整数 / Rebuild a BigInt from fixed-width two's complement
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     t 补码 limb 数组 / The two's-complement limbs
 * @param[in]     nl limb 数 / Limb count
 * @param[in]     nbits 有效位宽 / The significant bit width
 * @param[in]     signedp 非零表示按有符号解读 / Non-zero to read the value as signed
 *
 * @return 还原后的大整数 / The reconstructed integer
 *
 * @note 符号位是第 `nbits - 1` 位，且补码展宽只覆盖 `nbits` 位；`asIntN(8, 255n)` 的位宽为 8，故 `0xFF` 解读为 -1 / The sign bit is bit `nbits - 1` and the complement spans just `nbits`; `asIntN(8, 255n)` is 8 bits wide, so `0xFF` reads as -1
 */
static HpeBigInt *bi_from_twos(HpeJs *js, const uint32_t *t, int nl, int nbits, int signedp)
{
    int neg = signedp && nbits > 0 && (((t[(nbits - 1) >> 5] >> ((nbits - 1) & 31)) & 1u) != 0);
    HpeBigInt *r = bi_new(js, nl + 2);
    if (neg) {
        HpeBigInt *k = bi_new(js, nl + 2);
        r->n = nl;
        memcpy(r->limb, t, (size_t)nl * sizeof(uint32_t));
        bi_mask_bits(r, nbits);
        r->sign = 1;
        k->limb[nbits >> 5] = (uint32_t)1u << (nbits & 31);
        k->n = (nbits >> 5) + 1;
        k->sign = 1;
        bi_sub_abs_inplace(k, r); /* 2^nbits - |r| */
        bi_apply_sign(k, -1);     /* ... and the result is the negative of that */
        return k;
    }
    r->n = nl;
    memcpy(r->limb, t, (size_t)nl * sizeof(uint32_t));
    bi_mask_bits(r, nbits);
    r->sign = 1;
    return r;
}

/* ---------------- string conversion ---------------- */

/**
 * @brief 按进制把大整数转成字符串 / Render a BigInt in the given radix
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     a 大整数 / The integer
 * @param[in]     radix 进制（2..36）/ The radix (2..36)
 *
 * @return arena 中的字符串 / A string in the arena
 */
static char *bi_to_radix(HpeJs *js, const HpeBigInt *a, int radix)
{
    char *buf = (char *)hpe_js_alloc(js, (size_t)a->n * 33 + 8);
    HpeBigInt *t;
    int len = 0, i;
    if (bi_is_zero(a)) {
        buf[0] = '0';
        buf[1] = '\0';
        return buf;
    }
    t = bi_copy(js, a);
    t->sign = 1; /* digits are produced from the magnitude */
    while (!bi_is_zero(t))
        buf[len++] = k_digits[bi_divmod_small_inplace(t, (uint32_t)radix)];
    if (a->sign < 0)
        buf[len++] = '-';
    for (i = 0; i < len / 2; i++) {
        char c = buf[i];
        buf[i] = buf[len - 1 - i];
        buf[len - 1 - i] = c;
    }
    buf[len] = '\0';
    return buf;
}

/**
 * @brief 按进制把数字串解析成大整数 / Parse a digit string in the given radix into a BigInt
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     s 数字串（不含符号与进制前缀）/ Digit string, without sign or radix prefix
 * @param[in]     radix 进制（2..36）/ The radix (2..36)
 *
 * @return 解析出的大整数；无有效数字时为 0 / The parsed integer, or zero when no digit is present
 *
 * @note 预分配足够容量，累乘期间无需重新分配 / Capacity is pre-sized so the accumulation never needs to grow
 */
static HpeBigInt *bi_from_radix(HpeJs *js, const char *s, int radix)
{
    int cap = (int)strlen(s) * 6 / 32 + 2;
    HpeBigInt *a = bi_new(js, cap);
    a->n = 1;
    a->sign = 1;
    for (; *s; s++) {
        uint64_t carry;
        int d = bi_digit(*s), i;
        if (d < 0 || d >= radix)
            continue;
        carry = (uint64_t)d;
        for (i = 0; i < a->n; i++) {
            uint64_t cur = (uint64_t)a->limb[i] * (uint32_t)radix + carry;
            a->limb[i] = (uint32_t)(cur & 0xFFFFFFFFu);
            carry = cur >> 32;
        }
        if (carry && a->n < a->cap)
            a->limb[a->n++] = (uint32_t)carry;
    }
    bi_norm(a);
    return a;
}

/* ---------------- primitive bridge ---------------- */

int js_is_bigint(JsValue v)
{
    return v.t == JS_BIGINT && v.big != NULL;
}

/**
 * @brief 把大整数指针包装成 JS 值 / Wrap a BigInt pointer into a JS value
 *
 * @param[in] a 大整数 / The integer
 *
 * @return 类型为 `JS_BIGINT` 的值 / A value of type `JS_BIGINT`
 */
static JsValue bi_value(HpeBigInt *a)
{
    JsValue v;
    v.t = JS_BIGINT;
    v.num = 0;
    v.str = NULL;
    v.obj = NULL;
    v.big = a;
    return v;
}

JsValue js_bigint_from_i64(HpeJs *js, long long v)
{
    HpeBigInt *a = bi_new(js, 3);
    unsigned long long u;
    a->n = 0;
    u = v < 0 ? (unsigned long long)(-(v + 1)) + 1ull : (unsigned long long)v;
    while (u) {
        a->limb[a->n++] = (uint32_t)(u & 0xFFFFFFFFu);
        u >>= 32;
    }
    if (a->n == 0)
        a->n = 1;
    a->sign = v < 0 ? -1 : (a->limb[0] == 0 && a->n == 1 ? 0 : 1);
    return bi_value(a);
}

JsValue js_bigint_from_str(HpeJs *js, const char *digits, int radix)
{
    return bi_value(bi_from_radix(js, digits ? digits : "0", radix));
}

char *js_bigint_to_radix(HpeJs *js, JsValue v, int radix)
{
    if (radix < 2 || radix > 36)
        js_throw_value(js, js_make_error(js, "RangeError", "toString() radix must be between 2 and 36"));
    if (!js_is_bigint(v))
        return hpe_js_strdup(js, "0");
    return bi_to_radix(js, (const HpeBigInt *)v.big, radix);
}

int js_bigint_is_zero(JsValue v)
{
    return js_is_bigint(v) && bi_is_zero((const HpeBigInt *)v.big);
}

int js_bigint_sign(JsValue v)
{
    return js_is_bigint(v) ? ((const HpeBigInt *)v.big)->sign : 0;
}

double js_bigint_to_number(JsValue v)
{
    const HpeBigInt *a;
    double d = 0.0;
    int i;
    if (!js_is_bigint(v))
        return 0.0;
    a = (const HpeBigInt *)v.big;
    for (i = a->n - 1; i >= 0; i--)
        d = d * 4294967296.0 + (double)a->limb[i];
    return a->sign < 0 ? -d : d;
}

/* ---------------- operators ---------------- */

/**
 * @brief 两个大整数的有符号加减 / Signed addition and subtraction of two BigInts
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     a 左操作数 / Left operand
 * @param[in]     b 右操作数 / Right operand
 * @param[in]     sub 非零表示做减法 / Non-zero to subtract
 *
 * @return 结果 / The result
 */
static HpeBigInt *bi_add_signed(HpeJs *js, const HpeBigInt *a, const HpeBigInt *b, int sub)
{
    int bs = sub ? -b->sign : b->sign;
    HpeBigInt *r;
    if (a->sign == 0) {
        r = bi_copy(js, b);
        bi_apply_sign(r, bs);
        return r;
    }
    if (bs == 0)
        return bi_copy(js, a);
    if (a->sign == bs) {
        r = bi_add_abs(js, a, b);
        bi_apply_sign(r, a->sign);
        return r;
    }
    if (bi_cmp_abs(a, b) >= 0) {
        r = bi_sub_abs(js, a, b);
        bi_apply_sign(r, a->sign);
    } else {
        r = bi_sub_abs(js, b, a);
        bi_apply_sign(r, bs);
    }
    return r;
}

JsValue js_bigint_add(HpeJs *js, JsValue l, JsValue r, int sub)
{
    return bi_value(bi_add_signed(js, (const HpeBigInt *)l.big, (const HpeBigInt *)r.big, sub));
}

JsValue js_bigint_mul(HpeJs *js, JsValue l, JsValue r)
{
    HpeBigInt *a = bi_mul_abs(js, (const HpeBigInt *)l.big, (const HpeBigInt *)r.big);
    bi_apply_sign(a, js_bigint_sign(l) * js_bigint_sign(r));
    return bi_value(a);
}

JsValue js_bigint_divmod(HpeJs *js, JsValue l, JsValue r, int want_rem)
{
    HpeBigInt *q, *rem;
    if (js_bigint_is_zero(r))
        js_throw_value(js, js_make_error(js, "RangeError", "Division by zero"));
    bi_divmod_abs(js, (const HpeBigInt *)l.big, (const HpeBigInt *)r.big, &q, &rem);
    if (want_rem) {
        bi_apply_sign(rem, js_bigint_sign(l));
        return bi_value(rem);
    }
    bi_apply_sign(q, js_bigint_sign(l) * js_bigint_sign(r));
    return bi_value(q);
}

JsValue js_bigint_neg(HpeJs *js, JsValue v)
{
    HpeBigInt *a = bi_copy(js, (const HpeBigInt *)v.big);
    bi_apply_sign(a, -js_bigint_sign(v));
    return bi_value(a);
}

int js_bigint_cmp(JsValue l, JsValue r)
{
    const HpeBigInt *a = (const HpeBigInt *)l.big;
    const HpeBigInt *b = (const HpeBigInt *)r.big;
    if (a->sign != b->sign)
        return a->sign < b->sign ? -1 : 1;
    if (a->sign == 0)
        return 0;
    return a->sign > 0 ? bi_cmp_abs(a, b) : -bi_cmp_abs(a, b);
}

/**
 * @brief 求最高有效位的下标加一 / The bit length of a magnitude
 *
 * @param[in] a 大整数 / The integer
 *
 * @return 位长；零返回 0 / The bit length, or 0 for zero
 *
 * @note 平方求幂只需遍历到最高有效位，否则 `2n ** 64n` 会一路平方到 `2^(2^31)` / Exponentiation by squaring must stop at the top set bit, or `2n ** 64n` squares all the way to `2^(2^31)`
 */
static int bi_bitlen(const HpeBigInt *a)
{
    int i;
    if (a->sign == 0)
        return 0;
    for (i = a->n * 32; i > 0; i--)
        if (bi_get_bit(a, i - 1))
            return i;
    return 0;
}

JsValue js_bigint_pow(HpeJs *js, JsValue l, JsValue r)
{
    const HpeBigInt *base = (const HpeBigInt *)l.big;
    const HpeBigInt *exp = (const HpeBigInt *)r.big;
    HpeBigInt *result, *b;
    int i, maxbit;
    int neg = 0;
    if (exp->sign < 0)
        js_throw_value(js, js_make_error(js, "RangeError", "Exponent must be non-negative"));
    maxbit = bi_bitlen(exp);
    if (base->sign < 0 && bi_get_bit(exp, 0))
        neg = 1;
    result = bi_new(js, 1);
    result->n = 1;
    result->sign = 1;
    result->limb[0] = 1;
    b = bi_copy(js, base);
    b->sign = 1; /* magnitudes only; the sign is applied at the end */
    for (i = 0; i < maxbit; i++) {
        if (bi_get_bit(exp, i))
            result = bi_mul_abs(js, result, b);
        if (i + 1 < maxbit)
            b = bi_mul_abs(js, b, b);
    }
    bi_apply_sign(result, neg ? -1 : 1);
    return bi_value(result);
}

/**
 * @brief 按位运算的实施 / Carry out a bitwise operation
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     op `JS_OP_BAND` / `JS_OP_BOR` / `JS_OP_BXOR` / Operator
 * @param[in]     l 左操作数 / Left operand
 * @param[in]     r 右操作数 / Right operand
 *
 * @return 结果 / The result
 */
static JsValue bi_bitwise(HpeJs *js, int op, const HpeBigInt *l, const HpeBigInt *r)
{
    int nl = (l->n > r->n ? l->n : r->n) + 1;
    uint32_t *lt = (uint32_t *)hpe_js_alloc(js, (size_t)nl * sizeof(uint32_t));
    uint32_t *rt = (uint32_t *)hpe_js_alloc(js, (size_t)nl * sizeof(uint32_t));
    int i;
    bi_to_twos(l, lt, nl);
    bi_to_twos(r, rt, nl);
    for (i = 0; i < nl; i++)
        lt[i] = op == JS_OP_BAND ? (lt[i] & rt[i]) : op == JS_OP_BOR ? (lt[i] | rt[i]) : (lt[i] ^ rt[i]);
    return bi_value(bi_from_twos(js, lt, nl, nl * 32, 1));
}

JsValue js_bigint_bitop(HpeJs *js, int op, JsValue l, JsValue r)
{
    return bi_bitwise(js, op, (const HpeBigInt *)l.big, (const HpeBigInt *)r.big);
}

/**
 * @brief 大整数的按位取反 / Bitwise complement of a BigInt
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     v 操作数 / The operand
 *
 * @return `~v`，等价于 `-(v + 1)` / `~v`, which equals `-(v + 1)`
 *
 * @note 直接借用「加一后取负」这一恒等式，省去补码展宽 / Uses the identity `-(v+1)` directly, avoiding a complement widening
 */
JsValue js_bigint_not(HpeJs *js, JsValue v)
{
    HpeBigInt *one = bi_new(js, 1);
    HpeBigInt *sum;
    one->n = 1;
    one->sign = 1;
    one->limb[0] = 1;
    sum = bi_add_signed(js, (const HpeBigInt *)v.big, one, 0);
    bi_apply_sign(sum, -sum->sign);
    return bi_value(sum);
}

/**
 * @brief 绝对值左移若干位 / Shift a magnitude left by a bit count
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     a 源大整数（按绝对值使用）/ Source integer, used as a magnitude
 * @param[in]     n 左移位数 / Left-shift amount
 *
 * @return 移位后的新大整数（符号为正）/ The shifted integer, with a positive sign
 */
static HpeBigInt *bi_shl_n(HpeJs *js, const HpeBigInt *a, long long n)
{
    int limbs = (int)(n / 32), bits = (int)(n % 32), i;
    HpeBigInt *b = bi_new(js, a->n + limbs + 2);
    if (limbs) {
        memset(b->limb, 0, (size_t)limbs * sizeof(uint32_t));
        memcpy(b->limb + limbs, a->limb, (size_t)a->n * sizeof(uint32_t));
        b->n = a->n + limbs;
    } else {
        memcpy(b->limb, a->limb, (size_t)a->n * sizeof(uint32_t));
        b->n = a->n;
    }
    if (bits) {
        for (i = b->n; i > 0; i--)
            b->limb[i] = (uint32_t)((b->limb[i] << bits) | (b->limb[i - 1] >> (32 - bits)));
        b->limb[0] <<= bits;
        b->n++;
    }
    b->sign = 1;
    bi_norm(b);
    return b;
}

/**
 * @brief 绝对值右移若干位 / Shift a magnitude right by a bit count
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     a 源大整数（按绝对值使用）/ Source integer, used as a magnitude
 * @param[in]     n 右移位数 / Right-shift amount
 *
 * @return 移位后的新大整数（符号为正）/ The shifted integer, with a positive sign
 */
static HpeBigInt *bi_shr_n(HpeJs *js, const HpeBigInt *a, long long n)
{
    int limbs = (int)(n / 32), bits = (int)(n % 32), i;
    HpeBigInt *b = bi_new(js, a->n + 1);
    if (limbs >= a->n) {
        b->sign = 0;
        return b;
    }
    b->n = a->n - limbs;
    for (i = 0; i < b->n; i++)
        b->limb[i] = a->limb[i + limbs];
    if (bits) {
        for (i = 0; i + 1 < b->n; i++)
            b->limb[i] = (uint32_t)((b->limb[i] >> bits) | (b->limb[i + 1] << (32 - bits)));
        b->limb[b->n - 1] = (uint32_t)(b->limb[b->n - 1] >> bits);
    }
    b->sign = 1;
    bi_norm(b);
    return b;
}

JsValue js_bigint_shift(HpeJs *js, JsValue l, JsValue r, int left)
{
    const HpeBigInt *src = (const HpeBigInt *)l.big;
    double dv = js_bigint_to_number(r);
    long long n;
    int sign = src->sign, i, lost = 0;
    HpeBigInt *a;
    if (dv < 0)
        js_throw_value(js, js_make_error(js, "RangeError", "BigInt shift count must be non-negative"));
    if (dv > 1e9)
        dv = 1e9;
    n = (long long)dv;
    if (left) {
        a = bi_shl_n(js, src, n);
    } else {
        /* `>>` on BigInt is an arithmetic shift: floor division by 2^n, so a
         * negative value with discarded low bits rounds away from zero. */
        for (i = 0; i < (int)n && i < src->n * 32; i++)
            lost |= bi_get_bit(src, i);
        a = bi_shr_n(js, src, n);
        if (sign < 0 && lost) {
            HpeBigInt *one = bi_new(js, 1);
            one->n = 1;
            one->sign = 1;
            one->limb[0] = 1;
            a = bi_add_signed(js, a, one, 0);
        }
    }
    bi_apply_sign(a, sign);
    return bi_value(a);
}

JsValue js_bigint_asn(HpeJs *js, double bits, JsValue v, int signedp)
{
    int nb, nl;
    uint32_t *t;
    if (bits != bits || bits < 0 || bits > 1e15)
        js_throw_value(js, js_make_error(js, "RangeError", "Invalid bit length"));
    nb = (int)bits;
    if (nb == 0 || !js_is_bigint(v))
        return js_bigint_from_i64(js, 0);
    nl = (nb + 31) / 32;
    t = (uint32_t *)hpe_js_alloc(js, (size_t)nl * sizeof(uint32_t));
    bi_to_twos((const HpeBigInt *)v.big, t, nl);
    if (nb % 32)
        t[nl - 1] &= (uint32_t)((1u << (nb % 32)) - 1u);
    return bi_value(bi_from_twos(js, t, nl, nb, signedp));
}
