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
for f in boxes css css-cascade css-props dom-api dom-mutation event flex globals hello hover html-parse image inline-fx cursor js-arrow js-features js js-builtins grid layout scroll svg-shapes svg-text svg-fx stack table timers bfc js-gc vertical emoji anim form css-vars sel-api details-dialog canvas; do
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
# css-layout-matrix: layout property matrix (box model / flow / flex / grid / table)
# eventloop  : task/microtask order + 4 ms nesting clamp (needs wall time: page:wait)
# js-gc      : conservative GC (__gc/__gcStats, garbage cycles, survivors, gen/promise)
for spec in js-modern js-builtins dom-api dom-mutation globals html-parse css-layout-matrix eventloop:1.2 js-gc:0.5; do
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

# ---------------- 3a2. form controls (v0.23) ----------------
# Click hooks toggle controls / radio groups / select / label / submit / reset;
# the page records states from listeners and prints assertions after the wait.
formout="$(HOPE_TEST_CLICK="cb1;r2;sel;lbl2;subm;rset" HOPE_TEST_WAIT=0.4 \
            HOPE_TEST_TEXT=out "$BIN" examples/form.html --width 500 --height 700 \
            --dump "$OUT/form.bmp" 2>&1)"
if printf '%s\n' "$formout" | grep -q 'PASS 24/24'; then
  echo "  form         PASS 24/24"
else
  echo "  form         FAIL"
  printf '%s\n' "$formout" | grep 'FAIL' | head -5
  fail=1
fi
# keyboard editing: type two code points into the text input
formout2="$(HOPE_TEST_FORM_TYPE="t1:XY" HOPE_TEST_FORM_SNAP=t1 \
              "$BIN" examples/form.html --width 500 --height 700 \
              --dump "$OUT/form.bmp" 2>&1)"
if printf '%s\n' "$formout2" | grep -q 'FORM-SNAP t1 value=程序化XY'; then
  echo "  form键入      OK"
else
  echo "  form键入      FAIL"
  fail=1
fi

# ---------------- 3a3. CSS custom properties / var() (v0.24) ----------------
varout="$(HOPE_TEST_TEXT=out "$BIN" examples/css-vars.html --width 500 --height 500 \
          --dump "$OUT/css-vars.bmp" 2>&1)"
if printf '%s\n' "$varout" | grep -q 'PASS 11/11'; then
  echo "  css-vars     PASS 11/11"
else
  echo "  css-vars     FAIL"
  printf '%s\n' "$varout" | grep 'FAIL' | head -5
  fail=1
fi

# ---------------- 3a4. Selection / Range API (v0.25) ----------------
selapiout="$(HOPE_TEST_TEXT=out "$BIN" examples/sel-api.html --width 500 --height 500 \
             --dump "$OUT/sel-api.bmp" 2>&1)"
if printf '%s\n' "$selapiout" | grep -q 'PASS 23/23'; then
  echo "  sel-api      PASS 23/23"
else
  echo "  sel-api      FAIL"
  printf '%s\n' "$selapiout" | grep 'FAIL' | head -5
  fail=1
fi

# ---------------- 3a5. details / dialog (v0.26) ----------------
ddout="$(HOPE_TEST_CLICK="sum1;sum2" HOPE_TEST_WAIT=0.4 HOPE_TEST_TEXT=out \
         "$BIN" examples/details-dialog.html --width 500 --height 500 \
         --dump "$OUT/details-dialog.bmp" 2>&1)"
if printf '%s\n' "$ddout" | grep -q 'PASS 18/18'; then
  echo "  details-dlg  PASS 18/18"
else
  echo "  details-dlg  FAIL"
  printf '%s\n' "$ddout" | grep 'FAIL' | head -5
  fail=1
fi

# ---------------- 3a6. canvas 2D (v0.27) ----------------
canvout="$(HOPE_TEST_TEXT=out "$BIN" examples/canvas.html --width 500 --height 500 \
           --dump "$OUT/canvas.bmp" 2>&1)"
if printf '%s\n' "$canvout" | grep -q 'PASS 16/16'; then
  echo "  canvas       PASS 16/16"
else
  echo "  canvas       FAIL"
  printf '%s\n' "$canvout" | grep 'FAIL' | head -5
  fail=1
fi

# ---------------- 3b. page text selection (drag + copy) ----------------
# A synthetic drag over hello.html's h1 must copy exactly "pe — 希"; the
# sentinel seed keeps a stale system clipboard from faking a pass.
selout="$(HOPE_TEST_CLIPBOARD="@@SENTINEL@@" HOPE_TEST_SEL_DRAG="40,100;200,100" \
          HOPE_TEST_SEL=COPY "$BIN" examples/hello.html \
          --width 500 --height 400 --dump "$OUT/sel.bmp" 2>&1)"
if [ $? = 0 ] && printf '%s' "$selout" | grep -q 'SEL-COPY: pe — 希'; then
  echo "  sel-drag     OK"
else
  echo "  sel-drag     FAIL"
  printf '%s\n' "$selout" | grep -E 'SEL-COPY|not consumed' | head -5
  fail=1
fi

# ---------------- 3c. browser tabs (new / switch / close) ----------------
# Ctrl+T opens a blank second tab; switch:0 must return to the document tab and
# show its path in the address bar again (proves per-tab identity).
tabout="$(HOPE_TEST_TAB="CTRL_T;switch:0" "$BIN" examples/hello.html \
          --width 760 --height 500 --chrome --dump "$OUT/tabs.bmp" 2>&1)"
tabline="$(printf '%s\n' "$tabout" | grep '^tabs ' | head -1)"
if [ $? = 0 ] && printf '%s' "$tabline" | grep -q 'count=2 active=0' \
   && printf '%s' "$tabline" | grep -q 'hello.html'; then
  echo "  tabs         OK"
else
  echo "  tabs         FAIL ($tabline)"
  fail=1
fi

# ---------------- 3d. css transitions + animation clock ----------------
# HOPE_TEST_ANIM_STEP makes every tick advance the animation clock by a fixed
# amount and HOPE_TEST_TICKS pumps exactly N ticks, so N*STEP seconds is the
# deterministic animation time that HOPE_TEST_ANIM_SNAPSHOT prints.
anim_fail=0
anim_check() {
  local step="$1" ticks="$2" ident="$3" want="$4" out
  out="$(HOPE_TEST_ANIM_STEP="$step" HOPE_TEST_TICKS="$ticks" \
          HOPE_TEST_ANIM_SNAPSHOT="$ident" "$BIN" examples/anim.html \
          --width 400 --height 300 --dump "$OUT/anim.bmp" 2>&1)"
  if [ $? = 0 ] && printf '%s' "$out" | grep -q "ANIM-SNAP $ident" \
     && printf '%s' "$out" | grep -q "$want"; then
    printf '  anim %-6s %ss x%-2s OK\n' "$ident" "$step" "$ticks"
  else
    printf '  anim %-6s %ss x%-2s FAIL (want %s)\n' "$ident" "$step" "$ticks" "$want"
    printf '%s\n' "$out" | grep 'ANIM-SNAP' | head -2
    fail=1
  fi
}
anim_check 0.05  5  kf    'opacity=0.350'   # linear keyframes 25% in
anim_check 0.1   5  tr    'width=150.00'    # inline-style transition, linear
anim_check 0.1   15 slide  'ml=75.00'        # layout-property keyframes mid-flight
anim_check 0.5   5  kf    'opacity=0.200'   # fill both after 2 alternate iterations
anim_check 0.125 5  steps 'opacity=0.500'   # steps(4, end) quantisation
anim_check 0.5   8  slide 'ml=0.00'         # fill none: back to the declared value

# ---------------- 4. C unit tests ----------------
# hope_tests covers base (utf8/string) + css parsing + html parsing + encoding.
TESTS_BIN="build/bin/hope_tests.exe"
if [ -x "$TESTS_BIN" ]; then
  "$TESTS_BIN" | tail -1 | grep -q "cases passed" || { echo "  hope_tests   FAIL"; fail=1; }
  printf '  %-12s %s\n' "hope_tests" "$("$TESTS_BIN" 2>/dev/null | grep -o '[0-9]*/[0-9]* cases passed' | tail -1)"
fi

echo
if [ "$fail" = 0 ]; then
  echo "ALL TESTS PASSED"
else
  echo "SOME TESTS FAILED"
fi
exit $fail
