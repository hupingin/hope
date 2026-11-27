#!/usr/bin/env python3
# =============================================================================
# bench_run.py -- 对代表页运行 hope.exe --bench 并把耗时汇总成 JSON 入库。
#
# 用法:
#   python tools/bench_run.py [--rounds N] [--output PATH] [--page NAME ...]
#
# 说明:
#   引擎的 `--bench N` 会做 N-1 次重绘并打印一行:
#       bench: <repaints> repaints, avg paint <X.XXXX> ms   (stderr)
#   为了得到"每轮"耗时, 本脚本对每个页面独立启动进程 rounds 次,
#   每次使用 `--bench 2`(即恰好 1 次重绘), 解析出该轮的毫秒数。
#
# 输出 JSON 结构:
#   {
#     "meta":   { "binary": "...", "rounds": N, "timestamp": "...", "threshold_hint": 25 },
#     "pages":  {
#       "hello.html": { "rounds_ms": [..N 个..], "avg_ms": X.XXXX },
#       ...
#     }
#   }
# =============================================================================

import argparse
import datetime
import json
import os
import re
import subprocess
import sys

# 默认参与基准的代表页
DEFAULT_PAGES = [
    "examples/hello.html",
    "examples/layout.html",
    "examples/js-modern.html",
    "examples/svg-shapes.html",
]

# 与引擎 main.c 输出格式保持一致
BENCH_LINE = re.compile(r"bench:\s*(\d+)\s*repaints,\s*avg paint\s*([0-9.]+)\s*ms")

# 默认输出路径(相对仓库根)
DEFAULT_OUTPUT = os.path.join("build", "bench", "result.json")


def repo_root():
    """以本脚本所在目录的上一级作为仓库根, 保证可在任意 cwd 下运行。"""
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def find_binary(root):
    """定位 hope 可执行文件(Windows/Linux 常见位置)。"""
    cands = [
        os.path.join(root, "build", "bin", "hope.exe"),
        os.path.join(root, "build", "bin", "hope"),
    ]
    for c in cands:
        if os.path.isfile(c):
            return c
    return None


def run_one_round(binary, page, cwd):
    """跑一次 --bench 2, 返回该轮的重绘毫秒数; 失败抛 RuntimeError。"""
    proc = subprocess.run(
        [binary, page, "--bench", "2"],
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    text = proc.stdout + proc.stderr
    m = BENCH_LINE.search(text)
    if m is None:
        raise RuntimeError(
            "无法解析 bench 输出 (exit=%d): %r" % (proc.returncode, text.strip())
        )
    return float(m.group(2))


def main():
    ap = argparse.ArgumentParser(description="运行 Hope UI 引擎性能基准并汇总为 JSON")
    ap.add_argument("--rounds", type=int, default=5, help="每页运行轮数 (默认 5)")
    ap.add_argument("--output", default=DEFAULT_OUTPUT, help="输出 JSON 路径")
    ap.add_argument(
        "--page", action="append", default=[],
        help="指定页面(可多次), 默认使用内置代表页列表",
    )
    args = ap.parse_args()

    if args.rounds < 1:
        ap.error("--rounds 至少为 1")

    root = repo_root()
    binary = find_binary(root)
    if binary is None:
        print("错误: 未找到 build/bin/hope 可执行文件, 请先构建", file=sys.stderr)
        return 2

    pages = args.page if args.page else DEFAULT_PAGES
    for p in pages:
        if not os.path.isfile(os.path.join(root, p)):
            print("错误: 页面不存在: %s" % p, file=sys.stderr)
            return 2

    pages_data = {}
    for p in pages:
        rounds_ms = []
        for i in range(args.rounds):
            ms = run_one_round(binary, p, root)
            rounds_ms.append(ms)
            print("%s round %d/%d: %.4f ms" % (p, i + 1, args.rounds, ms))
        avg = sum(rounds_ms) / len(rounds_ms)
        pages_data[os.path.basename(p)] = {
            "rounds_ms": rounds_ms,
            "avg_ms": round(avg, 4),
        }

    result = {
        "meta": {
            "binary": os.path.relpath(binary, root),
            "rounds": args.rounds,
            "timestamp": datetime.datetime.now().isoformat(timespec="seconds"),
        },
        "pages": pages_data,
    }

    out_path = args.output if os.path.isabs(args.output) else os.path.join(root, args.output)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(result, f, indent=2, ensure_ascii=False)
        f.write("\n")
    print("已写入 %s" % os.path.relpath(out_path, root))
    return 0


if __name__ == "__main__":
    sys.exit(main())
