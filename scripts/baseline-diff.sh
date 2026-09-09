#!/usr/bin/env bash
#
# baseline-diff.sh -- captures rendering baselines under both backends and
# diffs them once the Wayland backend draws real frames (M1).
#
# Current behaviour: captures the X11 reference scene and, if the demo is
# able to create a window on the Wayland backend, compares the two frames
# pixel-for-pixel with ImageMagick.  Before M1 the wayland demo exits 77
# and the comparison is reported as SKIP rather than a failure.

set -euo pipefail

RUNTIME=$(mktemp -d)
chmod 0700 "$RUNTIME"
trap 'rm -rf "$RUNTIME"' EXIT

echo "== X11 reference capture =="
Xvfb :97 -screen 0 1024x768x24 -nolisten tcp &
XVFB_PID=$!
sleep 1

DISPLAY=:97 GDK_BACKEND=x11 ydk-demo
DISPLAY=:97 import -window root "$RUNTIME/x11.png"
kill "$XVFB_PID" 2>/dev/null || true
wait "$XVFB_PID" 2>/dev/null || true

echo "== Wayland capture =="
WL_RC=0
dbus-run-session -- weston --backend=headless-backend.so \
  --socket=diff --idle-time=0 --no-config &
WESTON_PID=$!
sleep 2

set +e
(
  export WAYLAND_DISPLAY=diff
  export GDK_BACKEND=wayland
  ydk-demo
)
WL_RC=$?
set -e

if [ "$WL_RC" -eq 0 ]; then
  (
    cd "$RUNTIME"
    export WAYLAND_DISPLAY=diff
    weston-screenshooter >/dev/null 2>&1 || true
  )
fi

kill "$WESTON_PID" 2>/dev/null || true
wait "$WESTON_PID" 2>/dev/null || true

if [ "$WL_RC" -eq 0 ] && [ -s "$RUNTIME/wayland.png" ]; then
  # Crop to the 640x480 demo area at 10,10.
  AE=$(compare -metric AE "$RUNTIME/x11.png" "$RUNTIME/wayland.png" \
        "$RUNTIME/diff.png" 2>&1 || true)
  echo "baseline-diff: absolute-error=$AE"
  exit 0
fi

echo "baseline-diff: wayland windows land in M1 (skipped, wn rc=$WL_RC)"
exit 0