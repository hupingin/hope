# 性能基准回归（--bench 结果入库对比）

引擎命令行自带 `--bench N`：重复渲染 N-1 次并打印平均绘制耗时（stderr，一行）：

```
bench: 4 repaints, avg paint 0.0166 ms
```

本目录的两个脚本把该能力固化成"跑基准 → 入库 → 对比基线"的回归流程，
只用 Python3 标准库，无第三方依赖。

## 脚本

| 脚本 | 作用 |
| --- | --- |
| `tools/bench_run.py` | 对代表页各跑多轮 `--bench`，汇总为 JSON |
| `tools/bench_compare.py` | 对比 result.json 与基线，判定 REGRESSION / IMPROVED |

代表页（默认）：`examples/hello.html`、`examples/layout.html`、
`examples/js-modern.html`、`examples/svg-shapes.html`。

## 用法

### 1. 跑基准并生成结果 JSON

```sh
python tools/bench_run.py                    # 每页 5 轮，输出 build/bench/result.json
python tools/bench_run.py --rounds 10        # 更多轮数，均值更稳
python tools/bench_run.py --page examples/flex.html   # 追加自选页
```

对每个页面，脚本独立启动引擎 `rounds` 次，每次 `--bench 2`（恰好 1 次重绘），
从输出解析出每轮毫秒数；JSON 里包含 `pages.<name>.rounds_ms`（每轮）与
`avg_ms`（均值）：

```json
{
  "meta":  { "binary": "build\\bin\\hope.exe", "rounds": 5, "timestamp": "..." },
  "pages": { "hello.html": { "rounds_ms": [0.0180, ...], "avg_ms": 0.0168 } }
}
```

### 2. 生成基线（首次或确认性能良好时）

先跑 `bench_run.py`，确认当前性能正常后，把结果拷为基线：

```sh
python tools/bench_compare.py --rebase       # result.json -> baseline.json
```

基线文件为 `build/bench/baseline.json`（不入 git，与 result.json 同目录）。

### 3. 日常对比（改动代码后）

```sh
python tools/bench_run.py                    # 重新跑出 result.json
python tools/bench_compare.py                # 与基线对比，默认阈值 25%
python tools/bench_compare.py --threshold 10 # 收紧阈值
```

判定规则（对每页 avg_ms 相对基线的涨幅）：

- 涨幅 **> 阈值** → 打印 `REGRESSION`，**退出码为 1**（可直接接入 CI）；
- 降幅 ≥ 阈值 → 打印 `IMPROVED`（仅提示，不影响退出码）；
- 其余 → `OK`，退出码 0。

输出示例：

```
hello.html                  0.0168 ms ->   0.0171 ms  ( +1.8%)  OK
layout.html                 0.0131 ms ->   0.0133 ms  ( +1.5%)  OK

结果: OK
```

## 何时更新基线

- **性能有意改进/符合预期的变化合入后**：跑一次 `bench_run.py` + `--rebase`，
  让后续回归以新性能为基准。
- **出现意外 REGRESSION 时不要 rebase**：先定位是引擎改动、机器负载还是
  页面本身变化导致，修复后再更新基线。
- 机器、编译选项（Debug/Release）变更后建议重建基线，避免环境差异误报。

## 建议的 CI 用法

```sh
python tools/bench_run.py --rounds 10 && python tools/bench_compare.py --threshold 25
```

`bench_compare.py` 非 0 退出码即视为回归失败。
