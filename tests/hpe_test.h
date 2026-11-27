/* Hope UI Engine - minimal single-header unit test framework */
#ifndef HPE_TEST_H
#define HPE_TEST_H

#include <stdio.h>
#include <string.h>

/**
 * @brief 测试用例函数指针类型 / Test case function pointer type
 */
typedef void (*HpeTestCaseFn)(void);

/* The implementation translation unit (test_main.c) defines the shared state;
 * every other translation unit sees plain extern declarations. */
#ifdef HPE_TEST_IMPL
#define HPE_TEST_API
#else
#define HPE_TEST_API extern
#endif

/**
 * @brief 已注册用例总数计数器 / Counter of registered cases in total
 */
HPE_TEST_API int hpe_test_g_case_total;

/**
 * @brief 失败用例计数器 / Counter of failed cases
 */
HPE_TEST_API int hpe_test_g_case_failed;

/**
 * @brief 断言总数计数器 / Counter of executed assertions in total
 */
HPE_TEST_API int hpe_test_g_assert_total;

/**
 * @brief 失败断言计数器 / Counter of failed assertions
 */
HPE_TEST_API int hpe_test_g_assert_failed;

/**
 * @brief 当前用例失败标志（0 通过，1 失败）/ Failure flag of the current case, 0 pass and 1 fail
 */
HPE_TEST_API int hpe_test_g_case_failed_flag;

/**
 * @brief 注册一个测试用例 / Register one test case
 *
 * @param[in] section 用例所属段名 / Section name the case belongs to
 * @param[in] name    用例名 / Case name
 * @param[in] fn      用例函数 / Case function
 *
 * @note 通常不直接调用，而是使用 HPE_TEST_CASE 宏自动注册；
 *       表满时打印错误并忽略 / Normally not called directly, use the
 *       HPE_TEST_CASE macro instead; an overflow prints an error and is ignored
 *
 * @see HPE_TEST_CASE hpe_test_run_all
 */
void hpe_test_register(const char *section, const char *name, HpeTestCaseFn fn);

/**
 * @brief 运行全部已注册用例并输出汇总 / Run every registered case and print a summary
 *
 * @return 失败用例数（可作进程退出码）/ Number of failed cases, usable as exit code
 *
 * @see hpe_test_register
 */
int hpe_test_run_all(void);

/**
 * @brief 定义并自动注册一个测试用例 / Define and auto-register one test case
 *
 * @details 展开为一个 static 用例函数加一个构造期注册函数，因此无需手工登记，
 *          `main` 之前即完成注册。同名段内的用例按定义顺序归组。
 *          Expands to a static case function plus a constructor-time registrar,
 *          so cases are registered before `main` without manual bookkeeping;
 *          cases sharing a section name are grouped in definition order.
 *
 * @param section_ 段名标识符（如 utf8）/ Section identifier, e.g. utf8
 * @param name_    用例名标识符（须与段内其他用例不同）/ Case identifier, unique within its section
 *
 * @see HPE_TEST_ASSERT HPE_TEST_ASSERT_INT HPE_TEST_ASSERT_STR HPE_TEST_ASSERT_NEAR
 */
#define HPE_TEST_CASE(section_, name_)                                        \
    static void hpe_test_body_##section_##_##name_(void);                     \
    __attribute__((constructor)) static void                                  \
        hpe_test_reg_##section_##_##name_(void)                               \
    {                                                                         \
        hpe_test_register(#section_, #name_, hpe_test_body_##section_##_##name_); \
    }                                                                         \
    static void hpe_test_body_##section_##_##name_(void)

/**
 * @brief 记录一次失败断言 / Record one failed assertion
 *
 * @param fmt  printf 格式串 / printf format string
 * @param ...  格式参数 / Format arguments
 *
 * @note 失败不中断当前用例，只累计失败数 / A failure does not abort the case,
 *       it only accumulates the failure count
 */
#define HPE_TEST_FAIL(fmt, ...)                                               \
    do {                                                                      \
        hpe_test_g_assert_failed++;                                           \
        hpe_test_g_case_failed_flag = 1;                                      \
        printf("    [FAIL] %s:%d: ", __FILE__, __LINE__);                     \
        printf(fmt, __VA_ARGS__);                                             \
    } while (0)

/**
 * @brief 断言条件为真 / Assert that a condition holds
 *
 * @param cond 条件表达式 / Condition expression
 *
 * @note 失败时打印条件原文，继续执行后续断言 / On failure the condition text
 *       is printed and execution continues with the next assertion
 */
#define HPE_TEST_ASSERT(cond)                                                 \
    do {                                                                      \
        hpe_test_g_assert_total++;                                            \
        if (cond)                                                             \
            break;                                                            \
        HPE_TEST_FAIL("assertion `%s`\n", #cond);                             \
    } while (0)

/**
 * @brief 断言两个整数表达式相等 / Assert that two integer expressions are equal
 *
 * @param actual   实际值表达式 / Actual value expression
 * @param expected 期望值表达式 / Expected value expression
 *
 * @note 双方均按 long long 比较，失败时打印两个值 / Both sides are compared as
 *       long long; on failure both values are printed
 */
#define HPE_TEST_ASSERT_INT(actual, expected)                                 \
    do {                                                                      \
        long long hpe_test_a_ = (long long)(actual);                          \
        long long hpe_test_e_ = (long long)(expected);                        \
        hpe_test_g_assert_total++;                                            \
        if (hpe_test_a_ == hpe_test_e_)                                       \
            break;                                                            \
        HPE_TEST_FAIL("`%s` = %lld, expected %lld\n", #actual,                \
                      hpe_test_a_, hpe_test_e_);                              \
    } while (0)

/**
 * @brief 断言两个字符串内容相等 / Assert that two strings are equal in content
 *
 * @param actual   实际字符串表达式，可为 NULL / Actual string, may be NULL
 * @param expected 期望字符串表达式，可为 NULL / Expected string, may be NULL
 *
 * @note 双方均为 NULL 视为相等；失败时打印两个字符串 / Two NULLs compare equal;
 *       on failure both strings are printed
 */
#define HPE_TEST_ASSERT_STR(actual, expected)                                 \
    do {                                                                      \
        const char *hpe_test_a_ = (actual);                                   \
        const char *hpe_test_e_ = (expected);                                 \
        int hpe_test_ok_ = (hpe_test_a_ && hpe_test_e_)                       \
                               ? strcmp(hpe_test_a_, hpe_test_e_) == 0        \
                               : hpe_test_a_ == hpe_test_e_;                  \
        hpe_test_g_assert_total++;                                            \
        if (hpe_test_ok_)                                                     \
            break;                                                            \
        HPE_TEST_FAIL("`%s` = \"%s\", expected \"%s\"\n", #actual,            \
                      hpe_test_a_ ? hpe_test_a_ : "(null)",                   \
                      hpe_test_e_ ? hpe_test_e_ : "(null)");                  \
    } while (0)

/**
 * @brief 断言两个浮点表达式在容差内相等 / Assert that two floats match within a tolerance
 *
 * @param actual   实际值表达式 / Actual value expression
 * @param expected 期望值表达式 / Expected value expression
 * @param eps      允许的绝对误差 / Allowed absolute tolerance
 *
 * @note 双方均按 double 比较，失败时打印两个值与容差 / Both sides are compared
 *       as double; on failure both values and the tolerance are printed
 */
#define HPE_TEST_ASSERT_NEAR(actual, expected, eps)                           \
    do {                                                                      \
        double hpe_test_a_ = (double)(actual);                                \
        double hpe_test_e_ = (double)(expected);                              \
        double hpe_test_d_ = hpe_test_a_ - hpe_test_e_;                       \
        if (hpe_test_d_ < 0.0)                                                \
            hpe_test_d_ = -hpe_test_d_;                                       \
        hpe_test_g_assert_total++;                                            \
        if (hpe_test_d_ <= (double)(eps))                                     \
            break;                                                            \
        HPE_TEST_FAIL("`%s` = %g, expected %g (eps %g)\n", #actual,           \
                      hpe_test_a_, hpe_test_e_, (double)(eps));               \
    } while (0)

#endif /* HPE_TEST_H */
