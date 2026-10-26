#!/usr/bin/env bash
#
# valgrind-smoke.sh -- run the Wayland demo under valgrind.
#
# Separate from smoke-test.sh because valgrind is 20 to 50 times slower.
# It starts the same headless Weston as smoke-test.sh Phase 2 and runs
# ydk-demo under valgrind with GDK_BACKEND=wayland.  The demo is told that it
# runs under valgrind (YDK_DEMO_VALGRIND=1 stretches its quit and tick
# timeouts) and not to leave test marks in the window (YDK_DEMO_NO_MARKS=1).
#
# Pass: valgrind reports "ERROR SUMMARY: 0 errors" and the demo exits 0 and
# prints every PASS line.  A single invalid read or write, use after free or
# use of an uninitialised value that is not suppressed fails the run.
#
# Environment:
#   YDK_VALGRIND_SUPP   suppression file (default: scripts/valgrind.supp)
#   YDK_DEMO            demo binary (default: ydk-demo from PATH)
#
# Exits 0 on success, 9 when valgrind found errors, 1 on any other failure.

set -euo pipefail

SUPP=${YDK_VALGRIND_SUPP:-$(dirname "$0")/valgrind.supp}
DEMO=${YDK_DEMO:-ydk-demo}

if ! command -v valgrind >/dev/null 2>&1; then
  echo "valgrind-smoke: valgrind not found in PATH" >&2
  exit 1
fi

RUNTIME=$(mktemp -d)
chmod 0700 "$RUNTIME"
export XDG_RUNTIME_DIR="$RUNTIME"
WESTON_PID=
cleanup () {
  if [ -n "$WESTON_PID" ]; then
    pkill -TERM -P "$WESTON_PID" 2>/dev/null || true
    kill "$WESTON_PID" 2>/dev/null || true
    wait "$WESTON_PID" 2>/dev/null || true
  fi
  rm -rf "$RUNTIME"
}
trap cleanup EXIT

cat > "$RUNTIME/bus.conf" <<BUSCONF
<busconfig>
  <type>session</type>
  <listen>unix:path=$RUNTIME/bus</listen>
  <auth>EXTERNAL</auth>
  <policy context="default"><allow user="*"/></policy>
</busconfig>
BUSCONF
dbus-run-session --config-file="$RUNTIME/bus.conf" -- weston \
  --backend=headless-backend.so \
  --socket=valgrindtest --idle-time=0 --no-config \
  > "$RUNTIME/weston.log" 2>&1 &
WESTON_PID=$!
sleep 2

LOG="$RUNTIME/valgrind.log"

set +e
(
  export WAYLAND_DISPLAY=valgrindtest
  export GDK_BACKEND=wayland
  export G_SLICE=always-malloc
  export G_DEBUG=gc-friendly
  export YDK_DEMO_VALGRIND=1
  export YDK_DEMO_NO_MARKS=1
  valgrind --error-exitcode=9 --track-origins=yes --num-callers=30 \
    --leak-check=no --suppressions="$SUPP" "$DEMO"
) 2>&1 | tee "$LOG"
RC=${PIPESTATUS[0]}
set -e

if [ "$RC" -eq 9 ]; then
  echo "valgrind-smoke: FAIL: valgrind reported errors" >&2
  exit 9
fi
if [ "$RC" -ne 0 ]; then
  echo "valgrind-smoke: FAIL: demo exited with rc=$RC" >&2
  exit 1
fi
if ! grep -q "ERROR SUMMARY: 0 errors" "$LOG"; then
  echo "valgrind-smoke: FAIL: no 'ERROR SUMMARY: 0 errors' line" >&2
  exit 1
fi
for m in M4 M5 M5C M5V M6 M6B; do
  if ! grep -q "ydk_demo: $m: PASS" "$LOG"; then
    echo "valgrind-smoke: FAIL: demo did not report $m: PASS" >&2
    exit 1
  fi
done

echo "valgrind-smoke: PASS"
