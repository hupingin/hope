/* Hope UI Engine - unit test entry point and framework implementation */
#define HPE_TEST_IMPL
#include "hpe_test.h"

#include <stddef.h>

/**
 * @brief 单个注册条目 / One registered entry
 */
typedef struct {
    const char *section; /* 段名 / Section name */
    const char *name;    /* 用例名 / Case name */
    HpeTestCaseFn fn;    /* 用例函数 / Case function */
} HpeTestEntry;

/* 注册容量上限；超出后忽略新用例并打印错误 / Registration capacity; new
 * cases beyond it are ignored with an error print */
#define HPE_TEST_MAX_ENTRIES 256

static HpeTestEntry g_entries[HPE_TEST_MAX_ENTRIES];
static int g_entry_count = 0;

/**
 * @brief 注册一个测试用例 / Register one test case
 *
 * @param[in] section 用例所属段名 / Section name the case belongs to
 * @param[in] name    用例名 / Case name
 * @param[in] fn      用例函数 / Case function
 *
 * @note 表满时打印错误并忽略 / An overflow prints an error and is ignored
 *
 * @see HPE_TEST_CASE
 */
void hpe_test_register(const char *section, const char *name, HpeTestCaseFn fn)
{
    if (g_entry_count >= HPE_TEST_MAX_ENTRIES) {
        fprintf(stderr, "hpe_test: registration table full (%d), dropping %s::%s\n",
                HPE_TEST_MAX_ENTRIES, section, name);
        return;
    }
    g_entries[g_entry_count].section = section;
    g_entries[g_entry_count].name = name;
    g_entries[g_entry_count].fn = fn;
    g_entry_count++;
}

/**
 * @brief 打印用例结果行 / Print one case result line
 *
 * @param[in] e          用例条目 / Case entry
 * @param[in] ok         是否全部通过 / Whether every assertion passed
 * @param[in] assert_cnt 本用例执行的断言数 / Assertions executed by the case
 *
 * @note 输出形如 `[PASS] section::name (n asserts)` / The line looks like
 *       `[PASS] section::name (n asserts)`
 */
static void print_case_result(const HpeTestEntry *e, int ok, int assert_cnt)
{
    printf("[%s] %s::%s (%d asserts)\n", ok ? "PASS" : "FAIL", e->section,
           e->name, assert_cnt);
}

/**
 * @brief 运行全部已注册用例并输出汇总 / Run every registered case and print a summary
 *
 * @details 逐个调用用例函数；单个断言失败不会中断用例，单个用例失败也不会
 *          中断其余用例。汇总行给出用例与断言两级通过率。
 *          Case functions are invoked one by one; a failed assertion does not
 *          abort its case and a failed case does not abort the remaining ones.
 *          The summary line reports both the case-level and assertion-level pass rates.
 *
 * @return 失败用例数（可作进程退出码）/ Number of failed cases, usable as exit code
 *
 * @see hpe_test_register
 */
int hpe_test_run_all(void)
{
    int i;
    int ok_count = 0;

    printf("=== Hope UI Engine unit tests ===\n");
    hpe_test_g_case_total = g_entry_count;
    for (i = 0; i < g_entry_count; i++) {
        const HpeTestEntry *e = &g_entries[i];
        int asserts_before = hpe_test_g_assert_total;

        hpe_test_g_case_failed_flag = 0;
        e->fn();
        if (!hpe_test_g_case_failed_flag) {
            ok_count++;
        } else {
            hpe_test_g_case_failed++;
        }
        print_case_result(e, !hpe_test_g_case_failed_flag,
                          hpe_test_g_assert_total - asserts_before);
    }
    printf("--- summary: %d/%d cases passed, %d/%d asserts passed ---\n",
           ok_count, g_entry_count, hpe_test_g_assert_total,
           hpe_test_g_assert_total - hpe_test_g_assert_failed);
    return hpe_test_g_case_failed;
}

/**
 * @brief 测试程序入口 / Test program entry point
 *
 * @return 失败用例数 / Number of failed cases
 *
 * @note 注册工作由各翻译单元的构造函数在 main 之前完成 / Registration is done
 *       by per-translation-unit constructors before main runs
 */
int main(void)
{
    return hpe_test_run_all();
}
