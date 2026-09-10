#!/usr/bin/env bash
#
# smoke-test.sh -- headless two-backend smoke test.
#
# Phase 1 runs ydk_demo against an Xvfb X server (the baseline X11 path)
# and captures a screenshot with ImageMagick to prove real rendering.
# Phase 2 connects the demo to a headless Weston compositor over the
# Wayland backend; since M2 the demo creates a real toplevel and renders
# into an shm buffer (rc 0).  Backends without window support exit 77,
# which still proves the display open/roundtrip works.
#
# Exits 0 on success.

set -euo pipefail

RUNTIME=$(mktemp -d)
chmod 0700 "$RUNTIME"
export XDG_RUNTIME_DIR="$RUNTIME"
trap 'rm -rf "$RUNTIME"' EXIT

echo "== Phase 1: X11 baseline (Xvfb) =="
Xvfb :97 -screen 0 1024x768x24 -nolisten tcp &
XVFB_PID=$!
sleep 1

DISPLAY=:97 GDK_BACKEND=x11 ydk-demo
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
)
DEMO_RC=$?
set -e

# 0 = window created and rendered, 77 = connect OK but window unsupported,
# 78 = connect OK but no primary screen yet, 1 = fail
if [ "$DEMO_RC" -ne 0 ] && [ "$DEMO_RC" -ne 77 ] && [ "$DEMO_RC" -ne 78 ]; then
  echo "smoke-test: wayland demo failed with rc=$DEMO_RC" >&2
  kill "$WESTON_PID" 2>/dev/null || true
  exit 1
fi

kill "$WESTON_PID" 2>/dev/null || true
wait "$WESTON_PID" 2>/dev/null || true

echo "smoke-test: wayland phase OK (demo rc=$DEMO_RC)"
echo "smoke-test: PASS"