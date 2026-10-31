#!/usr/bin/env bash
#
# run-snapshot-demo.sh -- stage 0b: the snapshot harness of the ydk fork
# against the libydk.so that the Ardour tree builds.
#
# usage: run-snapshot-demo.sh smoke | valgrind | baseline
#   smoke      scripts/smoke-test.sh: demo on Xvfb (M6B-X11), on headless
#              weston (M4 to M10) and on weston --scale=2 (M10-REAL2)
#   valgrind   scripts/valgrind-smoke.sh: the demo on weston under valgrind
#   baseline   scripts/baseline-diff.sh: pixel diff of the demo window on
#              Xvfb and on weston
#
# The demo (demo/ydk_demo.c and ydk_demo_m10.inc) and the scripts are used as
# they are in the fork, which is only read.  The demo is compiled against the
# headers and libraries of this tree.
#
# env:   YDK_SNAPSHOT    the fork checkout (default ~/Projects/Personal/ydk-wayland)
#        YTK_TEST_BUILD  waf build directory (default: <repo>/build)
#
# Run it inside the nix shell of the merge plan (section 6) with valgrind,
# weston, Xvfb, dbus-run-session and ImageMagick on PATH.
#
# Origin: Wayland backend of the ydk fork, snapshot commit 23f48f0.

set -euo pipefail

MODE=${1:?usage: run-snapshot-demo.sh smoke|valgrind|baseline}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=${YTK_TEST_ROOT:-$(cd "$HERE/../../../../.." && pwd)}
BUILD=${YTK_TEST_BUILD:-$ROOT/build}
SNAP=${YDK_SNAPSHOT:-$HOME/Projects/Personal/ydk-wayland}
TK=$ROOT/libs/tk
L=$BUILD/libs/tk
OUT=${YTK_TEST_OUT:-$BUILD/ytk-tests-wayland}/snapshot-demo

mkdir -p "$OUT/bin" "$OUT/run"
cp -f "$SNAP/demo/ydk_demo.c" "$SNAP/demo/ydk_demo_m10.inc" "$OUT/"

gcc -g -O0 -w -o "$OUT/bin/ydk-demo" "$OUT/ydk_demo.c" \
  -I"$TK/ydk" -I"$TK/ydk/wayland" -I"$TK/ydk/x11" -I"$TK/ydk-pixbuf" \
  $(pkg-config --cflags glib-2.0 gobject-2.0 gio-2.0 pango pangocairo cairo gdk-pixbuf-2.0) \
  -L"$L/ydk" -L"$L/ydk-pixbuf" -lydk -lydk-pixbuf \
  $(pkg-config --libs glib-2.0 gobject-2.0 gio-2.0 pango pangocairo cairo gdk-pixbuf-2.0 wayland-client wayland-cursor xkbcommon)

export PATH="$OUT/bin:$PATH"
export LD_LIBRARY_PATH="$L/ydk:$L/ytk:$L/ydk-pixbuf:$L/ztk"

FONTDIR=${YTK_TEST_FONTDIR:-$(ls -d /nix/store/*-dejavu-fonts-*/share/fonts 2>/dev/null | head -n 1)}
cat > "$OUT/run/fonts.conf" <<FC
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<fontconfig>
<dir>${FONTDIR:-/usr/share/fonts}</dir>
<cachedir>$OUT/run/fccache</cachedir>
</fontconfig>
FC
export FONTCONFIG_FILE="$OUT/run/fonts.conf"

# the scripts skip the dispatch audit when there is no object directory here
cd "$OUT/run"
case "$MODE" in
  smoke)    exec bash "$SNAP/scripts/smoke-test.sh" ;;
  valgrind) exec bash "$SNAP/scripts/valgrind-smoke.sh" ;;
  baseline) exec bash "$SNAP/scripts/baseline-diff.sh" ;;
  *) echo "run-snapshot-demo.sh: unknown mode $MODE" >&2; exit 2 ;;
esac
