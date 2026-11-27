# Hope UI Engine 单元测试 / Hope UI Engine Unit Tests

纯 C11、零第三方依赖的极简单测框架与单元测试，覆盖 `src/base`、`src/css`、`src/html`
中可脱离引擎上下文独立调用的纯函数。

A minimal zero-dependency C11 test framework plus unit tests covering the pure
functions of `src/base`, `src/css` and `src/html` that are callable without an
engine context.

## 文件 / Files

| 文件 / File | 说明 / Description |
| --- | --- |
| `hpe_test.h` | 单头测试框架：用例自动注册宏、断言宏 / Single-header framework: auto-registration macro and assertion macros |
| `test_main.c` | `main` 入口与注册表实现，返回失败用例数作退出码 / `main` entry and registry implementation, exits with the failed-case count |
| `test_base.c` | `hpe_util` 单测：字符串、UTF-8、长度、颜色 / `hpe_util` tests: strings, UTF-8, lengths, colours |
| `test_css.c` | CSS 单测：声明解析、样式表、选择器匹配、内联样式 / CSS tests: declarations, stylesheet, selector matching, inline style |
| `test_html.c` | HTML 单测：实体解码、编码探测 / HTML tests: entity decoding, encoding detection |

## 编译 / Build

用 MinGW GCC 手动编译（建议的 CMake target 片段见下，本目录不改动根
`CMakeLists.txt`）：

Compile manually with MinGW GCC (a suggested CMake target snippet is given
below; this directory does not modify the root `CMakeLists.txt`):

```sh
gcc -std=c11 -Wall -Wextra -Isrc \
    tests/test_main.c tests/test_base.c tests/test_css.c tests/test_html.c \
    src/base/hpe_util.c src/dom/hpe_dom.c src/css/hpe_css.c \
    src/html/hpe_html.c src/html/hpe_encoding.c \
    -o build/test_runner.exe
```

要点 / Notes:

- 头文件按 `"base/hpe_util.h"` 等形式包含，必须加 `-Isrc`。
  Headers are included as `"base/hpe_util.h"`, so `-Isrc` is required.
- 依赖链：`hpe_css.c` 需要 `hpe_dom.c`，`hpe_html.c`/`hpe_encoding.c` 需要
  `hpe_util.c`；链接错误时把对应 `.c` 一并编入即可。
  Dependency chain: `hpe_css.c` needs `hpe_dom.c`, and `hpe_html.c` /
  `hpe_encoding.c` need `hpe_util.c`; add any missing `.c` files on link errors.

### 建议 CMake target 片段 / Suggested CMake target snippet

```cmake
enable_testing()
add_executable(hpe_test_runner
    tests/test_main.c
    tests/test_base.c
    tests/test_css.c
    tests/test_html.c
    src/base/hpe_util.c
    src/dom/hpe_dom.c
    src/css/hpe_css.c
    src/html/hpe_html.c
    src/html/hpe_encoding.c)
target_include_directories(hpe_test_runner PRIVATE src)
target_compile_features(hpe_test_runner PRIVATE c_std_11)
add_test(NAME hope_unit_tests COMMAND hpe_test_runner)
```

## 运行 / Run

```sh
./build/test_runner.exe
echo $?
```

输出每个用例的 `[PASS]/[FAIL] section::name (n asserts)` 与汇总行；进程退出码
等于失败用例数（0 表示全部通过）。

Each case prints `[PASS]/[FAIL] section::name (n asserts)` followed by a summary
line; the process exit code equals the number of failed cases (0 means all
passed).

## 框架用法 / Framework usage

```c
#include "hpe_test.h"

HPE_TEST_CASE(utf8, decode_multibyte)   /* 段名, 用例名 / section, case name */
{
    uint32_t cp = 0;
    HPE_TEST_ASSERT_INT(hpe_utf8_next("\xC3\xA9", &cp), 2);
    HPE_TEST_ASSERT_NEAR(hpe_len_parse("1pt").value, 1.3333, 1e-3);
    HPE_TEST_ASSERT_STR(hpe_encoding_name(HPE_ENC_UTF8), "utf-8");
}
```

- 用例经构造函数在 `main` 之前自动注册，无需手工登记 / Cases are registered by
  constructors before `main`, no manual bookkeeping needed.
- 断言失败只累计失败数并继续执行同段其余断言，不中断用例 / A failed assertion
  only bumps the failure count and continues with the following assertions.
- 可用断言宏 / Available assertion macros: `HPE_TEST_ASSERT`、
  `HPE_TEST_ASSERT_INT`、`HPE_TEST_ASSERT_STR`、`HPE_TEST_ASSERT_NEAR`。
