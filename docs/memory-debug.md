# 内存泄漏检测 / Memory Debugging

Hope UI Engine 提供内置的轻量分配统计钩子（位于 `src/base/hpe_util.c`），并支持接入 ASan、Dr. Memory、valgrind 等外部工具。本文说明三者的用法与适用场景。

The Hope UI Engine ships a lightweight built-in allocation-statistics hook (in `src/base/hpe_util.c`) and can also be paired with external tools such as ASan, Dr. Memory and valgrind. This document covers usage of all three.

## 1. HPE_ALLOC_STATS 内置分配统计 / Built-in allocation statistics

### 原理 / How it works

- 设置环境变量 `HPE_ALLOC_STATS=1` 后，`hpe_alloc` / `hpe_calloc` / `hpe_realloc` 会把每次分配登记进一个用**普通 malloc 维护的账本**（指针→大小，单链表），`hpe_free` 时摘除并递减存活计数。账本绝不递归调用 `hpe_alloc`。
  When `HPE_ALLOC_STATS=1` is set, `hpe_alloc` / `hpe_calloc` / `hpe_realloc` register every allocation in a ledger maintained with **plain malloc** (pointer→size, singly linked list); `hpe_free` unlinks it and decrements the live counters. The ledger never calls `hpe_alloc` recursively.
- 开关**默认关闭**：首次调用时惰性读取环境变量，此后只是一个静态分支判断，关闭时零开销。
  The switch is **off by default**: the environment variable is read lazily once, afterwards every call is a single static branch with zero overhead when disabled.
- 进程退出时通过 `atexit` 注册的报告函数向 stderr 打印汇总。
  At process exit an `atexit`-registered reporter prints the summary to stderr.

### 用法 / Usage

```bash
cmake --build build -j4
HPE_ALLOC_STATS=1 ./build/bin/hope.exe examples/hello.html --dump build/tmp/leak.bmp
```

### 输出解读 / Reading the output

```text
[hpe-alloc] total allocations: 1129 (4022937 bytes)
[hpe-alloc] no leaks: 0 live allocation(s)          <- 存活为 0，无泄漏 / zero live blocks, no leak
```

```text
[hpe-alloc] total allocations: 920 (5665975 bytes)
[hpe-alloc] LEAK: 14 live allocation(s), 52164 byte(s) not freed
```

- `total allocations` / `total ... bytes`：进程生命周期内累计分配次数与字节（含已释放）。
  Cumulative allocation count and bytes over the process lifetime (freed blocks included).
- `LEAK: N live ...`：进程退出时仍未释放的块数与字节数。注意字体/emoji 等缓存随引擎销毁释放，理想情况存活应为 0 或接近 0。
  Blocks still allocated at exit. Font/emoji caches are freed with engine destruction, so ideally the live count is 0 or near 0.

### 实测结果（2026-11-27，hello 渲染 + 立即退出）/ Measured results

| 示例页面 | 累计分配 | 存活块数 | 存活字节 |
|---|---|---|---|
| `hello.html` | 1129 次 / 4,022,937 B | **0** | 0 |
| `js.html` | — | **0** | 0 |
| `table.html` | — | **0** | 0 |
| `flex.html` | — | **0** | 0 |
| `emoji.html` | 920 次 / 5,665,975 B | 14 | 52,164 |
| `dom-api.html` | — | 15 | 940 |
| `image.html` | — | 21 | 187,312 |

结论：常规渲染路径（hello/js/table/flex）退出时存活为 0，无泄漏；`emoji.html`、`dom-api.html`、`image.html` 存在少量未释放块，属已知待查项（可疑方向：JS DOM 绑定对象与图片解码缓存未随引擎销毁释放），可用 ASan 精确定位。

Conclusion: the normal render paths (hello/js/table/flex) exit with zero live blocks. `emoji.html`, `dom-api.html` and `image.html` leave a small number of unfreed blocks — known open items (suspects: JS DOM wrapper objects and decoded-image caches not released with engine teardown). Use ASan to pinpoint the exact call sites.

## 2. AddressSanitizer（MinGW GCC）

ASan 能精确报告泄漏块的**分配调用栈**，适合定位上表中的泄漏来源；代价是运行速度大幅降低。

ASan reports the exact allocation stack of every leaked block, which is the right tool for the leaks above; the trade-off is a large slowdown.

> MSYS2 MinGW GCC 需要 ASan 运行库：`pacman -S mingw-w64-x86_64-addr2line` 无需；直接安装 `mingw-w64-x86_64-gcc` 已含 `libasan`（若链接报缺 `libasan.dll`，确认它在 PATH 中即可）。

建议复制 build 目录另建 build-asan，不要污染常规构建目录（本仓库 CMakeLists 暂未集成 ASan 选项，需临时通过环境变量注入编译选项）：

```bash
# 用环境变量注入编译选项，无需改 CMakeLists.txt
export CFLAGS="-fsanitize=address -fno-omit-frame-pointer -g -O1"
export LDFLAGS="-fsanitize=address"
cmake -S . -B build-asan -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan -j4

ASAN_OPTIONS=detect_leaks=1:log_path=asan.log ./build-asan/bin/hope.exe examples/image.html --dump build/tmp/asan.bmp
```

输出示例（节选）：每个泄漏给出分配栈 `#0 malloc → #1 hpe_alloc → ...`，按 `Direct leak` / `Indirect leak` 分类并给出字节总量。

> 注：老版本 MinGW GCC（含 MSYS2 12.2.0）在 Windows 上可能未链接 ASan 运行库或 `detect_leaks` 不可用（Windows ASan 泄漏检测历史上受限）。若 ASan 泄漏报告不可用，可退回方案：保留 `HPE_ALLOC_STATS=1` 的汇总定位“有没有漏”，再用 Dr. Memory 定位“漏在哪”。

## 3. Dr. Memory（Windows）与 valgrind（Linux）

| 工具 | 平台 | 特点 / 适用场景 |
|---|---|---|
| **Dr. Memory** | Windows / Linux | 免重编译（对 MSVC/MinGW 二进制直接运行），报告未初始化读、泄漏、越界；速度约为原生的 1/20~1/50。适合在**已有 release 构建**上做整体体检。 |
| **valgrind memcheck** | Linux（macOS 已停止支持） | 免重编译、泄漏报告带完整分配栈（`--leak-check=full --show-leak-kinds=definite,indirect`）；Linux CI 上做例行检查的首选。 |

```bash
# Dr. Memory（Windows，直接跑现有构建产物）
drmemory.exe -- build/bin/hope.exe examples/image.html --dump build/tmp/dm.bmp

# valgrind（Linux）
valgrind --leak-check=full --show-leak-kinds=definite,indirect \
  ./build/bin/hope.exe examples/image.html --dump /tmp/vg.bmp
```

建议工作流：日常构建用 `HPE_ALLOC_STATS=1` 快速确认存活数（秒级）；发现泄漏后，在 Linux CI 上用 valgrind、或在 Windows 上用 Dr. Memory / ASan 构建定位具体分配栈。

Recommended workflow: run `HPE_ALLOC_STATS=1` on normal builds for a quick live-count check (seconds), then use valgrind on Linux CI, or Dr. Memory / an ASan build on Windows, to get the exact allocation stacks once a leak is suspected.
