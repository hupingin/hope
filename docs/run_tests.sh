#!/usr/bin/env bash
# Hope engine regression harness.
#
#   ./docs/run_tests.sh            pixel baselines + JS language checks
#   ./docs/run_tests.sh --rebase   refresh the pixel baselines from current output
#
# Exit code 0 = everything matches.
#
# This script lives in docs/ but operates on the project root (one level up), so
# every path it touches (build/, examples/) is resolved relative to that root.
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT" || exit 1

BIN="build/bin/hope.exe"
BASE="build/baseline"
OUT="build/reg"
REBASE=0
[ "${1:-}" = "--rebase" ] && REBASE=1

if [ ! -x "$BIN" ]; then
  echo "hope: $BIN not built yet (cmake --build build)" >&2
  exit 1
fi
mkdir -p "$OUT" "$BASE"
[ "$REBASE" = 1 ] && echo "== refreshing baselines =="

fail=0

# ---------------- 1. pixel baselines ----------------
# `--dump` is headless and chrome-less, so identical geometry gives identical bytes.
# A non-zero exit is a hard failure: the engine must tear down cleanly even on
# pages that register timers (a bad free() there once corrupted the heap at exit).
for f in boxes css css-cascade css-props dom-api dom-mutation event flex globals hello hover html-parse image inline-fx cursor js-arrow js-features js js-builtins grid layout scroll svg-shapes svg-text svg-fx stack table timers bfc js-gc; do
  html="examples/$f.html"
  [ -f "$html" ] || continue
  bmp="$OUT/$f.bmp"
  "$BIN" "$html" --width 400 --height 600 --dump "$bmp" >/dev/null 2>&1
  rc=$?
  if [ "$rc" != 0 ]; then
    printf '  %-12s CRASH (exit %s)\n' "$f" "$rc"
    fail=1
    continue
  fi
  if [ "$REBASE" = 1 ]; then
    cp "$bmp" "$BASE/$f.bmp"
    printf '  %-12s rebased\n' "$f"
    continue
  fi
  if [ ! -f "$BASE/$f.bmp" ]; then
    printf '  %-12s SKIP (no baseline)\n' "$f"
    continue
  fi
  if cmp -s "$bmp" "$BASE/$f.bmp"; then
    printf '  %-12s OK\n' "$f"
  else
    printf '  %-12s DIFF\n' "$f"
    fail=1
  fi
done

# ---------------- 2. JS language checks ----------------
# Each assertion page runs its own checks and writes a "PASS n/n" summary into
# #out; HOPE_TEST_TEXT dumps that element's text so we can assert on it.
# js-modern  : syntax + language semantics
# js-builtins: standard built-in library (Date/Map/Set/Array/String/Number/Math/Object)
# dom-api    : DOM query/traversal/geometry/events/dispatchEvent
# dom-mutation: DOM mutation APIs + MutationObserver (needs live JS, so it is a script check)
# globals    : performance/history/localStorage/sessionStorage/console extensions
# html-parse : parser fault-tolerance (auto-closing, table/select/template)
# eventloop  : task/microtask order + 4 ms nesting clamp (needs wall time: page:wait)
# js-gc      : conservative GC (__gc/__gcStats, garbage cycles, survivors, gen/promise)
for spec in js-modern js-builtins dom-api dom-mutation globals html-parse eventloop:1.2 js-gc:0.5; do
  f="${spec%%:*}"
  wait_s=""
  [ "$f" != "$spec" ] && wait_s="${spec#*:}"
  html="examples/$f.html"
  [ -f "$html" ] || continue
  if [ -n "$wait_s" ]; then
    out="$(HOPE_TEST_TEXT=out HOPE_TEST_WAIT="$wait_s" "$BIN" "$html" \
            --width 400 --height 600 --dump "$OUT/$f.bmp" 2>&1)"
  else
    out="$(HOPE_TEST_TEXT=out "$BIN" "$html" \
            --width 400 --height 600 --dump "$OUT/$f.bmp" 2>&1)"
  fi
  rc=$?
  if [ "$rc" != 0 ]; then
    printf '  %-12s CRASH (exit %s)\n' "$f" "$rc"
    fail=1
    continue
  fi
  title="$(printf '%s\n' "$out" | grep -o 'PASS [0-9]*/[0-9]*\|FAIL [0-9]*/[0-9]*' | head -1)"
  if [ -z "$title" ]; then
    printf '  %-12s NO RESULT (script did not run)\n' "$f"
    fail=1
  elif [ "${title#PASS}" = "$title" ]; then
    printf '  %-12s %s\n' "$f" "$title"
    printf '%s\n' "$out" | grep -o 'FAIL [^|]*' | head -20
    fail=1
  else
    printf '  %-12s %s\n' "$f" "$title"
  fi
done

# ---------------- 3. ES modules (headless) ----------------
# Covers the §7.9 surface: default/named imports, `import * as ns`, renamed
# imports, `export * from`, renamed re-exports, nested relative resolution and
# `import.meta.url` for both an imported module and the entry module.
mkdir -p "$OUT/mod/sub"
cat > "$OUT/mod/util.js" <<'EOF'
export const PI2 = Math.PI * 2;
export function area(r) { return PI2 * r * r / 2; }
export default function greet(n) { return "hi " + n; }
export const NAME = "util";
export const META = import.meta.url;
EOF
cat > "$OUT/mod/barrel.js" <<'EOF'
export * from "./util.js";
export { area as circleArea } from "./util.js";
EOF
cat > "$OUT/mod/sub/deep.js" <<'EOF'
export const DEEP = "deep";
EOF
cat > "$OUT/mod/main.js" <<'EOF'
import greet, { PI2, area } from "./util.js";
import * as util from "./util.js";
import { area as al } from "./util.js";
import * as barrel from "./barrel.js";
import { DEEP } from "./sub/deep.js";
console.log(greet("hope"), PI2.toFixed(2), area(1).toFixed(2));
console.log("ns", util.NAME, util.area(2).toFixed(4));
console.log("renamed", al(1).toFixed(2));
console.log("star", barrel.NAME, barrel.area(1).toFixed(2));
console.log("reexport-renamed", barrel.circleArea(1).toFixed(2));
console.log("nested", DEEP);
console.log("meta", util.META.indexOf("util.js") >= 0);
console.log("entry-meta", import.meta.url.indexOf("main.js") >= 0);
import("./sub/deep.js").then(function (ns) { console.log("dyn", ns.DEEP); });
import("./missing.js").then(function () { console.log("dyn-bad", "resolved"); },
                            function (e) { console.log("dyn-bad", "rejected"); });
import(42).then(function () { console.log("dyn-num", "resolved"); },
                function (e) { console.log("dyn-num", "rejected"); });
EOF
modout="$("$BIN" --module "$OUT/mod/main.js" 2>&1 | tr -d '\r')"
modwant='hi hope 6.28 3.14
ns util 12.5664
renamed 3.14
star util 3.14
reexport-renamed 3.14
nested deep
meta true
entry-meta true
dyn deep
dyn-bad rejected
dyn-num rejected'
if [ "$modout" = "$modwant" ]; then
  echo "  es-modules   OK"
else
  echo "  es-modules   FAIL"
  printf '%s\n' "$modout"
  fail=1
fi

echo
if [ "$fail" = 0 ]; then
  echo "ALL TESTS PASSED"
else
  echo "SOME TESTS FAILED"
fi
exit $fail
