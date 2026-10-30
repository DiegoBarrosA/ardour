#!/usr/bin/env bash
#
# run-seat-tests.sh -- runs the ytk tests that need a seat on a headless sway.
#
# The tests need a pointer and a keyboard, which headless weston does not have.  This
# script starts a SEPARATE headless sway (seat/start-sway.sh, own XDG_RUNTIME_DIR and
# sockets, 1920x1080), gives its seat a virtual pointer and keyboard (seat/seat-input.c),
# runs the tests, and stops the sway again.  It never touches the sway of the login
# session.  It is not part of run-tests.sh, because it needs sway, jq and grim.
#
# usage: run-seat-tests.sh [--wldebug] [--keep-sway] [--only "s01 s02 ..."] [test-stem ...]
#   test-stem  t0_10_popup_menu or t0_12_menus_seat (default: both)
#   --wldebug  run T0.12 with WAYLAND_DEBUG=1, the trace goes to LOGS/t0_12_menus_seat.trace
#   --ffm      use seat/sway-ffm.conf (focus_follows_mouse yes, the sway default), restarts the sway
#   --sway-conf FILE  start the sway with this config (seat/sway-scale2.conf gives a HiDPI output), restarts the sway
#   --keep-sway  leave the sway and the input helper running (stop with seat/stop-sway.sh)
#   --only     run only these T0.12 scenarios (names in seat/t12-scenarios.sh)
#
# env:   YTK_TEST_BUILD  waf build directory (default: <repo>/build)
#        YTK_TEST_OUT    where build.sh put the programs
#        YTK_TEST_LOGS   log and screenshot directory (default: <out>/logs-sway)
#        HARNESS_HOME    state of the harness (default ~/.cache/ytk-seat-harness)
#
# Run it from a shell that has sway, swaymsg, grim, jq and gcc, wayland-scanner and
# libxkbcommon for the first build of seat-input (the nix shell of the merge plan has
# them, and start-sway.sh enters it by itself when wayland-scanner is missing).
#
# T0.10 runs here with T_HAVE_SEAT=1: it only calls the menu code by program, so it
# says little about pointer routing.  T0.12 is the test that clicks.
#
# Origin: Wayland backend of the ydk fork, snapshot commit 23f48f0.

set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=${YTK_TEST_ROOT:-$(cd "$HERE/../../../../.." && pwd)}
BUILD=${YTK_TEST_BUILD:-$ROOT/build}
OUT=${YTK_TEST_OUT:-$BUILD/ytk-tests-wayland}
LOGS=${YTK_TEST_LOGS:-$OUT/logs-sway}
SEAT=$HERE/seat

WLDEBUG=0; KEEP=0; FFM=0; SWAYCONF=""; ONLY=""; TESTS=()
while [ $# -gt 0 ]; do
  case "$1" in
    --wldebug) WLDEBUG=1 ;;
    --keep-sway) KEEP=1 ;;
    --ffm) FFM=1 ;;
    --sway-conf) shift; SWAYCONF=$1 ;;
    --only) shift; ONLY=$1 ;;
    -*) echo "run-seat-tests.sh: unknown option $1" >&2; exit 2 ;;
    *) TESTS+=("$1") ;;
  esac
  shift
done
[ ${#TESTS[@]} -gt 0 ] || TESTS=(t0_10_popup_menu t0_12_menus_seat)
for t in sway swaymsg grim jq; do command -v $t >/dev/null || { echo "run-seat-tests.sh: $t is not in PATH" >&2; exit 1; }; done

mkdir -p "$LOGS"
WAS_RUNNING=0
. "$SEAT/lib.sh"
if [ "$FFM" = 1 ]; then SWAYCONF=$SEAT/sway-ffm.conf; fi
if [ -n "$SWAYCONF" ]; then
  "$SEAT/stop-sway.sh" >/dev/null 2>&1
  export HARNESS_SWAY_CONF=$SWAYCONF
  case "$SWAYCONF" in *scale*) export SHOT_SCALE=1 ;; esac
fi
[ -f "$ST/env.sh" ] && WAS_RUNNING=1
"$SEAT/start-sway.sh" > "$LOGS/start-sway.log" 2>&1 || { cat "$LOGS/start-sway.log"; exit 1; }
cleanup() { [ "$KEEP" = 1 ] || [ "$WAS_RUNNING" = 1 ] || "$SEAT/stop-sway.sh" >/dev/null 2>&1; }
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

. "$SEAT/client-env.sh" || exit 1
# T12_FFM tells the scenarios whether sway moves focus with the pointer (focus_follows_mouse yes)
CONF=${SWAYCONF:-$SEAT/sway.conf}
T12_EDGE=$(sed -n 's/^# harness-edge-output: *//p' "$CONF" | head -n 1); export T12_EDGE
T12_FFM=0; grep -qE "^[[:space:]]*focus_follows_mouse[[:space:]]+(yes|always)" "$CONF" && T12_FFM=1; export T12_FFM
export GTK_PATH="$BUILD/libs/clearlooks-newer" G_MESSAGES_DEBUG= T_HAVE_SEAT=1
SUMMARY="$LOGS/summary.txt"; : > "$SUMMARY"
ALLRC=0

for t in "${TESTS[@]}"; do
  bin="$OUT/$t"
  [ -x "$bin" ] || { echo "run-seat-tests.sh: $bin is not built" >&2; ALLRC=1; continue; }
  tn=$(echo "$t" | sed -E 's/^t0_([0-9]+)_.*/T0.\1/')
  log="$LOGS/$t.log"
  start=$(date +%s)
  note=""
  if [ "$t" = t0_12_menus_seat ]; then
    . "$SEAT/seat-lib.sh"
    . "$SEAT/t12-scenarios.sh"
    SHOTS=$LOGS/shots-$t; rm -rf "$SHOTS"; mkdir -p "$SHOTS"
    export T12_DRIVEN=1
    trace=/dev/null
    if [ "$WLDEBUG" = 1 ]; then trace=$LOGS/$t.trace; export WAYLAND_DEBUG=1; fi
    if app_start "$log" "$trace" timeout 300 "$bin"; then
      t12_run "$ONLY"
      app_stop
    else
      NFAIL=$((NFAIL+1)); FAILS+=("start: the test program did not become ready")
      app_stop
    fi
    unset WAYLAND_DEBUG
    cp "$log" "$log.full" 2>/dev/null
    verdict=$(grep -E "^T0\.[0-9]+: (PASS|FAIL|SKIP)" "$log" | tail -n 1)
    status=PASS
    [ "$NFAIL" != 0 ] && { status=FAIL; note="$NFAIL failed expectations of $((NPASS+NFAIL)): $(printf '%s | ' "${FAILS[@]}")"; }
    case "$verdict" in *": PASS"*) ;; *) status=FAIL; note="$note program verdict: ${verdict:-none}" ;; esac
    rc=0; [ "$status" = FAIL ] && rc=1
  else
    timeout 60 "$bin" > "$log" 2>&1; rc=$?
    verdict=$(grep -E "^T0\.[0-9]+: (PASS|FAIL|SKIP)" "$log" | tail -n 1)
    status=FAIL
    case "$verdict" in *": PASS"*) status=PASS ;; *": SKIP"*) status=SKIP ;; esac
    [ "$rc" != 0 ] && status=FAIL
  fi
  [ "$status" = FAIL ] && ALLRC=1
  printf '%-4s %-9s %-34s %-5s rc=%-3s %3ss %s\n' "$tn" sway "$t" "$status" "$rc" "$(( $(date +%s) - start ))" "$note" | tee -a "$SUMMARY"
done

echo "run-seat-tests.sh: logs in $LOGS"
exit $ALLRC
