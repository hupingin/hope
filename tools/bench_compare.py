#!/usr/bin/env python3
# =============================================================================
# bench_compare.py -- 对比当前基准结果与基线, 判定性能回归。
#
# 用法:
#   python tools/bench_compare.py [--result PATH] [--baseline PATH]
#                                 [--threshold PCT] [--rebase]
#
# 规则:
#   对每个页面计算 avg_ms 相对基线的涨幅:
#     涨幅 >  threshold  -> REGRESSION (打印并使退出码非 0)
#     降幅 >= threshold  -> IMPROVED   (仅打印提示)
#     其余               -> OK
#   --rebase: 直接把 result.json 拷贝为 baseline.json, 不做对比。
# =============================================================================

import argparse
import json
import os
import shutil
import sys


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load_json(path, what):
    if not os.path.isfile(path):
        print("错误: %s 文件不存在: %s" % (what, path), file=sys.stderr)
        sys.exit(2)
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def main():
    root = repo_root()
    ap = argparse.ArgumentParser(description="对比 bench 结果与基线, 检测性能回归")
    ap.add_argument("--result", default=os.path.join("build", "bench", "result.json"))
    ap.add_argument("--baseline", default=os.path.join("build", "bench", "baseline.json"))
    ap.add_argument("--threshold", type=float, default=25.0,
                    help="回归阈值(%%), 默认 25")
    ap.add_argument("--rebase", action="store_true",
                    help="把当前 result 拷贝为新基线, 跳过对比")
    args = ap.parse_args()

    result_path = args.result if os.path.isabs(args.result) else os.path.join(root, args.result)
    baseline_path = args.baseline if os.path.isabs(args.baseline) else os.path.join(root, args.baseline)

    if args.rebase:
        result = load_json(result_path, "result")
        os.makedirs(os.path.dirname(baseline_path), exist_ok=True)
        shutil.copyfile(result_path, baseline_path)
        print("已把 %s 拷贝为新基线 %s" % (
            os.path.relpath(result_path, root), os.path.relpath(baseline_path, root)))
        return 0

    result = load_json(result_path, "result")
    baseline = load_json(baseline_path, "baseline")

    regressions = []
    improved = []
    missing = []

    for page, cur in sorted(result.get("pages", {}).items()):
        base = baseline.get("pages", {}).get(page)
        if base is None:
            missing.append(page)
            continue
        base_avg = base["avg_ms"]
        cur_avg = cur["avg_ms"]
        if base_avg <= 0:
            print("%-24s 基线均值为 0, 跳过" % page)
            continue
        delta_pct = (cur_avg - base_avg) / base_avg * 100.0
        if delta_pct > args.threshold:
            status = "REGRESSION"
            regressions.append(page)
        elif delta_pct <= -args.threshold:
            status = "IMPROVED"
            improved.append(page)
        else:
            status = "OK"
        print("%-24s %8.4f ms -> %8.4f ms  (%+6.1f%%)  %s"
              % (page, base_avg, cur_avg, delta_pct, status))

    for page in missing:
        print("%-24s 基线中不存在, 跳过 (可用 --rebase 更新基线)" % page)

    if regressions:
        print("\n结果: FAIL -- %d 个页面超出阈值 %.1f%%" % (len(regressions), args.threshold))
        return 1
    print("\n结果: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
