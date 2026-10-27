#!/usr/bin/env bash
#
# smoke-test.sh -- headless two-backend smoke test.
#
# Phase 1 runs ydk_demo against an Xvfb X server (the baseline X11 path)
# and captures a screenshot with ImageMagick to prove real rendering.
# Phase 2 connects the demo to a headless Weston compositor over the
# Wayland backend; since M2 the demo creates a real toplevel and renders
# into an shm buffer (rc 0), and since M4 it also runs the cursor, font,
# selection, drag and property checks ("ydk_demo: M4: PASS"), and since M5
# the offscreen drawing checks ("ydk_demo: M5: PASS"), and since F7 the
# colormap family checks ("ydk_demo: M5C: PASS"), and since F8 the visual /
# default-colormap / rgba / gdkrgb checks ("ydk_demo: M5V: PASS"), and since M6
# the input and screen checks driven through the headless test hooks
# ("ydk_demo: M6: PASS"), and since M6b the keymap, display and toplevel hint
# checks plus the audit gaps ("ydk_demo: M6B: PASS"), and since M7 the cursor,
# shape and font checks ("ydk_demo: M7: PASS"), with CRITICAL log
# messages fatal in the demo.  Backends without window support exit 77,
# which still proves the display open/roundtrip works.
#
# Phase 0 runs the dispatch audit (scripts/audit-x11-symbols.sh) when the
# built objects are available (YDK_AUDIT_BUILD, set by the flake app).
# Phase 1 also requires "ydk_demo: M6B-X11: PASS": the dispatched M6b
# functions still reach the X11 backend.
#
# Exits 0 on success.

set -euo pipefail

RUNTIME=$(mktemp -d)
chmod 0700 "$RUNTIME"
export XDG_RUNTIME_DIR="$RUNTIME"
# M7: fontconfig needs a writable cache directory
mkdir -p "$RUNTIME/cache"
export XDG_CACHE_HOME="$RUNTIME/cache"
trap 'rm -rf "$RUNTIME"' EXIT

echo "== Phase 0: dispatch audit =="
AUDIT_SCRIPT=${YDK_AUDIT_SCRIPT:-$(dirname "$0")/audit-x11-symbols.sh}
AUDIT_BUILD=${YDK_AUDIT_BUILD:-build}
if [ -f "$AUDIT_SCRIPT" ] && [ -d "$AUDIT_BUILD/ydk" ]; then
  bash "$AUDIT_SCRIPT" "$AUDIT_BUILD"
else
  echo "smoke-test: audit skipped (no objects under $AUDIT_BUILD)"
fi

echo "== Phase 1: X11 baseline (Xvfb) =="
Xvfb :97 -screen 0 1024x768x24 -nolisten tcp &
XVFB_PID=$!
sleep 1

DISPLAY=:97 GDK_BACKEND=x11 ydk-demo 2>&1 | tee "$RUNTIME/x11-demo.log"
if ! grep -q "ydk_demo: M6B-X11: PASS" "$RUNTIME/x11-demo.log"; then
  echo "smoke-test: x11 demo did not report M6B-X11: PASS" >&2
  kill "$XVFB_PID" 2>/dev/null || true
  exit 1
fi
DISPLAY=:97 import -window root "$RUNTIME/x11-baseline.png"

kill "$XVFB_PID" 2>/dev/null || true
wait "$XVFB_PID" 2>/dev/null || true

if [ ! -s "$RUNTIME/x11-baseline.png" ]; then
  echo "smoke-test: X11 screenshot is empty" >&2
  exit 1
fi
echo "smoke-test: X11 phase OK ($RUNTIME/x11-baseline.png)"

echo "== Phase 2: Wayland connect (weston headless) =="
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
  --socket=smoketest --idle-time=0 --no-config &
WESTON_PID=$!
sleep 2

set +e
(
  export WAYLAND_DISPLAY=smoketest
  export GDK_BACKEND=wayland
  ydk-demo
) 2>&1 | tee "$RUNTIME/wayland-demo.log"
DEMO_RC=${PIPESTATUS[0]}
set -e

# 0 = window created and rendered, 77 = connect OK but window unsupported,
# 78 = connect OK but no primary screen yet, 1 = fail
if [ "$DEMO_RC" -ne 0 ] && [ "$DEMO_RC" -ne 77 ] && [ "$DEMO_RC" -ne 78 ]; then
  echo "smoke-test: wayland demo failed with rc=$DEMO_RC" >&2
  pkill -TERM -P "$WESTON_PID" 2>/dev/null || true
  kill "$WESTON_PID" 2>/dev/null || true
  exit 1
fi

# WESTON_PID is dbus-run-session; take weston down with it so no orphan keeps
# the caller's stdout pipe open.
pkill -TERM -P "$WESTON_PID" 2>/dev/null || true
kill "$WESTON_PID" 2>/dev/null || true
wait "$WESTON_PID" 2>/dev/null || true

# Since M4 the demo exercises the cursor/font/selection/drag/property
# families under the Wayland backend and prints a verdict once it got a window.
if [ "$DEMO_RC" -eq 0 ] && ! grep -q "ydk_demo: M4: PASS" "$RUNTIME/wayland-demo.log"; then
  echo "smoke-test: wayland demo exited 0 but did not report M4: PASS" >&2
  exit 1
fi

if [ "$DEMO_RC" -eq 0 ] && ! grep -q "ydk_demo: M5: PASS" "$RUNTIME/wayland-demo.log"; then
  echo "smoke-test: wayland demo exited 0 but did not report M5: PASS" >&2
  exit 1
fi

if [ "$DEMO_RC" -eq 0 ] && ! grep -q "ydk_demo: M5C: PASS" "$RUNTIME/wayland-demo.log"; then
  echo "smoke-test: wayland demo exited 0 but did not report M5C: PASS" >&2
  exit 1
fi

if [ "$DEMO_RC" -eq 0 ] && ! grep -q "ydk_demo: M5V: PASS" "$RUNTIME/wayland-demo.log"; then
  echo "smoke-test: wayland demo exited 0 but did not report M5V: PASS" >&2
  exit 1
fi

if [ "$DEMO_RC" -eq 0 ] && ! grep -q "ydk_demo: M6: PASS" "$RUNTIME/wayland-demo.log"; then
  echo "smoke-test: wayland demo exited 0 but did not report M6: PASS" >&2
  exit 1
fi

if [ "$DEMO_RC" -eq 0 ] && ! grep -q "ydk_demo: M6B: PASS" "$RUNTIME/wayland-demo.log"; then
  echo "smoke-test: wayland demo exited 0 but did not report M6B: PASS" >&2
  exit 1
fi

if [ "$DEMO_RC" -eq 0 ] && ! grep -q "ydk_demo: M7: PASS" "$RUNTIME/wayland-demo.log"; then
  echo "smoke-test: wayland demo exited 0 but did not report M7: PASS" >&2
  exit 1
fi

if grep -q "GdkFont text drawing unsupported" "$RUNTIME/wayland-demo.log"; then
  echo "smoke-test: wayland phase logged the old font drawing warning" >&2
  exit 1
fi

# Since F8 the demo makes CRITICAL and ERROR log messages fatal under both
# backends (rc 1 with a "FATAL" line); this catches one that was
# logged but did not stop the run, whatever the exit code.
if grep -q "CRITICAL" "$RUNTIME/wayland-demo.log"; then
  echo "smoke-test: wayland phase logged a CRITICAL:" >&2
  grep "CRITICAL" "$RUNTIME/wayland-demo.log" >&2
  exit 1
fi

echo "smoke-test: wayland phase OK (demo rc=$DEMO_RC)"
echo "smoke-test: PASS"