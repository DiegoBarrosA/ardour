#!/usr/bin/env bash
#
# baseline-diff.sh -- captures a rendering baseline under both backends and
# diffs the demo window pixel-for-pixel with ImageMagick.
#
# Both captures are taken while the demo window is still on screen (the demo
# lingers for a couple of seconds after it has drawn).  The Wayland frame is
# cropped to the 640x480 white demo window, wherever the compositor put it;
# the X11 frame is cropped at the fixed 10,10 position the demo asks for.
#
# Every external program runs under a timeout and every long-lived child
# (Xvfb, weston plus its dbus session and helpers) is started in its own
# process group that the EXIT trap tears down, so the script always terminates
# with a verdict and never leaves an orphan holding the caller's stdout open.
#
# Verdict lines:
#   baseline-diff: MATCH  (absolute-error=0)
#   baseline-diff: DIFF   (absolute-error=N)
#   baseline-diff: SKIP   (the Wayland demo could not create a window)
# MATCH, DIFF and SKIP exit 0 (DIFF is informational until the backends are
# expected to be pixel-identical).  Any harness failure exits 1.

set -euo pipefail

DEMO_TIMEOUT=${BASELINE_DEMO_TIMEOUT:-40}      # seconds, whole demo run
SHOOT_TIMEOUT=${BASELINE_SHOOT_TIMEOUT:-20}    # seconds, weston-screenshooter
START_TIMEOUT=${BASELINE_START_TIMEOUT:-10}    # seconds, compositor/X startup

RUNTIME=$(mktemp -d)
chmod 0700 "$RUNTIME"
export XDG_RUNTIME_DIR="$RUNTIME"
# M7: the demo needs fontconfig and a writable cache directory
mkdir -p "$RUNTIME/cache"
export XDG_CACHE_HOME="$RUNTIME/cache"

XVFB_PID=""
WESTON_PID=""

# kill_group PID: TERM then KILL the whole process group led by PID.
kill_group() {
  local pid=$1
  [ -n "$pid" ] || return 0
  kill -TERM -- "-$pid" 2>/dev/null || kill -TERM "$pid" 2>/dev/null || true
  for _ in 1 2 3 4 5 6 7 8 9 10; do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.1
  done
  kill -KILL -- "-$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
}

cleanup() {
  kill_group "$WESTON_PID"
  kill_group "$XVFB_PID"
  # BASELINE_KEEP=<dir> keeps the captured images for inspection.
  if [ -n "${BASELINE_KEEP:-}" ]; then
    mkdir -p "$BASELINE_KEEP"
    cp "$RUNTIME"/*.png "$BASELINE_KEEP"/ 2>/dev/null || true
  fi
  rm -rf "$RUNTIME"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

fail() {
  echo "baseline-diff: FAIL: $*" >&2
  exit 1
}

# wait_for TIMEOUT_SECONDS CMD...: poll CMD every 0.1s until it succeeds.
wait_for() {
  local limit=$(( $1 * 10 )) i
  shift
  for (( i = 0; i < limit; i++ )); do
    "$@" && return 0
    sleep 0.1
  done
  return 1
}

echo "== X11 reference capture =="
setsid Xvfb :97 -screen 0 1024x768x24 -nolisten tcp &
XVFB_PID=$!
wait_for "$START_TIMEOUT" test -S /tmp/.X11-unix/X97 \
  || fail "Xvfb did not come up within ${START_TIMEOUT}s"

set +e
(
  export DISPLAY=:97 GDK_BACKEND=x11
  timeout "$DEMO_TIMEOUT" ydk-demo &
  DEMO=$!
  sleep 1
  timeout "$SHOOT_TIMEOUT" import -window root "$RUNTIME/x11-root.png"
  IMPORT_RC=$?
  wait "$DEMO"
  DEMO_RC=$?
  [ "$DEMO_RC" -eq 0 ] || exit "$DEMO_RC"
  exit "$IMPORT_RC"
)
X11_RC=$?
set -e
[ "$X11_RC" -eq 0 ] || fail "X11 capture failed (rc=$X11_RC; 124 means timeout)"
kill_group "$XVFB_PID"
XVFB_PID=""

# The demo window is at 10,10 and 640x480.
magick "$RUNTIME/x11-root.png" -crop 640x480+10+10 +repage "$RUNTIME/x11.png"

echo "== Wayland capture =="
# Two weston options matter for weston-screenshooter:
#  --renderer=pixman  the headless backend defaults to the no-op renderer, which
#                     never answers a capture request, so the screenshooter
#                     blocks forever (this was the original hang);
#  --debug            without it weston denies the capture ("unauthorized") and
#                     the screenshot comes back all black.
# The shell's default fade-in would otherwise leave the new window partly
# transparent in the capture, so switch the animations off.
cat > "$RUNTIME/weston.ini" <<'INI'
[shell]
animation=none
panel-position=none
startup-animation=none
focus-animation=none
INI
setsid dbus-run-session -- weston --backend=headless-backend.so \
  --renderer=pixman --debug --socket=diff --idle-time=0 \
  --config="$RUNTIME/weston.ini" &
WESTON_PID=$!
wait_for "$START_TIMEOUT" test -S "$RUNTIME/diff" \
  || fail "weston did not create its socket within ${START_TIMEOUT}s"

WL_LOG="$RUNTIME/wayland-demo.log"
(
  export WAYLAND_DISPLAY=diff GDK_BACKEND=wayland YDK_DEMO_NO_MARKS=1
  exec timeout "$DEMO_TIMEOUT" ydk-demo
) >"$WL_LOG" 2>&1 &
DEMO_PID=$!

# The demo runs its M4/M5 checks and then lingers ~2.5s; shoot in that gap.
# Stop early if it exits instead (no window, or failure).
demo_ready() {
  grep -q "ydk_demo: exercises done" "$WL_LOG" || ! kill -0 "$DEMO_PID" 2>/dev/null
}
wait_for "$DEMO_TIMEOUT" demo_ready || true

SHOT_RC=0
if kill -0 "$DEMO_PID" 2>/dev/null; then
  ( cd "$RUNTIME" && WAYLAND_DISPLAY=diff \
      timeout "$SHOOT_TIMEOUT" weston-screenshooter >/dev/null 2>&1 ) \
    || SHOT_RC=$?
fi

set +e
wait "$DEMO_PID"
WL_RC=$?
set -e
cat "$WL_LOG"

kill_group "$WESTON_PID"
WESTON_PID=""

case "$WL_RC" in
  0) ;;
  77|78)
    echo "baseline-diff: SKIP (wayland demo rc=$WL_RC, no window)"
    exit 0 ;;
  124) fail "wayland demo timed out after ${DEMO_TIMEOUT}s" ;;
  *)   fail "wayland demo failed with rc=$WL_RC" ;;
esac

[ "$SHOT_RC" -eq 0 ] \
  || fail "weston-screenshooter failed (rc=$SHOT_RC; 124 means timeout)"

SHOT=$(ls "$RUNTIME"/wayland-screenshot-*.png 2>/dev/null | head -n 1 || true)
[ -n "$SHOT" ] && [ -s "$SHOT" ] || fail "weston-screenshooter wrote no image"

# Locate the white demo window: bounding box of pure-white pixels.
BOX=$(magick "$SHOT" -fill black +opaque white -trim -format '%wx%h%O' info:)
case "$BOX" in
  640x480+*)
    magick "$SHOT" -crop "$BOX" +repage "$RUNTIME/wayland.png" ;;
  *)
    fail "could not find the 640x480 demo window in the Wayland frame (white box: $BOX)" ;;
esac

# compare exits 1 when the images differ; the metric goes to stderr.
AE=$(compare -metric AE "$RUNTIME/x11.png" "$RUNTIME/wayland.png" \
       "$RUNTIME/diff.png" 2>&1 || true)
AE=${AE%% *}
case "$AE" in
  ''|*[!0-9.e+]*) fail "compare gave no usable metric: '$AE'" ;;
esac

if [ "$AE" = "0" ]; then
  echo "baseline-diff: MATCH (absolute-error=0)"
else
  echo "baseline-diff: DIFF (absolute-error=$AE)"
fi
exit 0
