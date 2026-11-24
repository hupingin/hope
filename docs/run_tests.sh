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
for f in boxes css css-cascade css-props dom-mutation event hello hover html-parse js-arrow js-features js js-builtins layout svg-shapes timers; do
  html="examples/$f.html"
  [ -f "$html" ] || continue
  bmp="$OUT/$f.bmp"
  "$BIN" "$html" --width 400 --height 600 --dump "$bmp" >/dev/null 2>&1
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
# dom-mutation: DOM mutation APIs + MutationObserver (needs live JS, so it is a script check)
# html-parse : parser fault-tolerance (auto-closing, table/select/template)
for f in js-modern js-builtins dom-mutation html-parse; do
  html="examples/$f.html"
  [ -f "$html" ] || continue
  out="$(HOPE_TEST_TEXT=out "$BIN" "$html" --width 400 --height 600 \
          --dump "$OUT/$f.bmp" 2>&1)"
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
mkdir -p "$OUT/mod"
cat > "$OUT/mod/util.js" <<'EOF'
export const PI2 = Math.PI * 2;
export function area(r) { return PI2 * r * r / 2; }
export default function greet(n) { return "hi " + n; }
EOF
cat > "$OUT/mod/main.js" <<'EOF'
import greet, { PI2, area } from "./util.js";
console.log(greet("hope"), PI2.toFixed(2), area(1).toFixed(2));
EOF
modout="$("$BIN" --module "$OUT/mod/main.js" 2>&1)"
if [ "$modout" = "hi hope 6.28 3.14" ]; then
  echo "  es-modules   OK"
else
  echo "  es-modules   FAIL ($modout)"
  fail=1
fi

echo
if [ "$fail" = 0 ]; then
  echo "ALL TESTS PASSED"
else
  echo "SOME TESTS FAILED"
fi
exit $fail
