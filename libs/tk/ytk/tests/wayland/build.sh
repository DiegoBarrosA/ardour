#!/usr/bin/env bash
#
# build.sh -- builds the stage 0 ytk test programs against the waf build tree.
#
# Not part of the default Ardour build.  Run it inside the nix shell of the
# merge plan (section 6), after "waf build --targets=libydk-pixbuf,libztk,libydk,libytk":
#
# T0.3 also needs the clearlooks engine.  Build it with
# "waf build --targets=clearlooks-newer,engines".  Without the "engines"
# target, GTK does not find the engine and T0.3 fails on its clearlooks phase.
#
#   nix-shell shell.nix --run 'unset AS; libs/tk/ytk/tests/wayland/build.sh'
#
# usage: build.sh [test-source-stem ...]     (default: every t0_*.c)
# env:   YTK_TEST_BUILD  waf build directory (default: <repo>/build)
#        YTK_TEST_OUT    output directory   (default: <build>/ytk-tests-wayland)
#
# Origin: Wayland backend of the ydk fork, snapshot commit 23f48f0.

set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=${YTK_TEST_ROOT:-$(cd "$HERE/../../../../.." && pwd)}
BUILD=${YTK_TEST_BUILD:-$ROOT/build}
OUT=${YTK_TEST_OUT:-$BUILD/ytk-tests-wayland}
TK=$ROOT/libs/tk
LIBS=$BUILD/libs/tk

for l in ydk/libydk.so ytk/libytk.so ydk-pixbuf/libydk-pixbuf.so ztk/libztk.so; do
  [ -e "$LIBS/$l" ] || { echo "build.sh: missing $LIBS/$l, build libydk, libytk, libztk and libydk-pixbuf first" >&2; exit 1; }
done

mkdir -p "$OUT"

if [ $# -gt 0 ]; then
  SRCS=(); for s in "$@"; do SRCS+=("$HERE/$s.c"); done
else
  SRCS=("$HERE"/t0_*.c)
fi

PKGS="glib-2.0 gobject-2.0 gio-2.0 pango pangocairo cairo gdk-pixbuf-2.0"
CFLAGS="-g -O0 -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations -Wno-cpp"
INC="-I$TK/ytk -I$TK/ydk -I$TK/ydk-pixbuf -I$TK/ztk -I$TK/ydk/x11"

RC=0
for src in "${SRCS[@]}"; do
  stem=$(basename "$src" .c)
  echo "build.sh: $stem"
  # shellcheck disable=SC2046
  if ! gcc $CFLAGS $INC $(pkg-config --cflags $PKGS) "$src" -o "$OUT/$stem" \
       -L"$LIBS/ytk" -L"$LIBS/ydk" -L"$LIBS/ydk-pixbuf" -L"$LIBS/ztk" \
       -lytk -lydk -lydk-pixbuf -lztk $(pkg-config --libs $PKGS) \
       -Wl,-rpath,"$LIBS/ytk" -Wl,-rpath,"$LIBS/ydk" -Wl,-rpath,"$LIBS/ydk-pixbuf" -Wl,-rpath,"$LIBS/ztk"; then
    echo "build.sh: FAILED $stem" >&2
    RC=1
  fi
done
exit $RC
