#!/usr/bin/env bash
#
# run-tests.sh -- runs the stage 0 ytk test programs on a headless display.
#
# usage: run-tests.sh MODE [test-stem ...]
#   MODE   weston       headless weston, GDK_BACKEND=wayland
#          xvfb         Xvfb, GDK_BACKEND=x11
#          vg-weston    weston under valgrind
#          vg-xvfb      Xvfb under valgrind
#   test-stem  e.g. t0_2_label_ghost (default: every built t0_* program)
#
# env:   YTK_TEST_BUILD  waf build directory (default: <repo>/build)
#        YTK_TEST_OUT    where build.sh put the programs
#        YTK_TEST_LOGS   log and screenshot directory (default: <out>/logs-MODE)
#        YTK_TEST_TIMEOUT  seconds per program (default 60, times 15 under valgrind)
#        YTK_TEST_SHOTS=0  do not take screenshots
#
# Run it inside the nix shell of the merge plan, with valgrind, weston, Xvfb,
# dbus-run-session and ImageMagick on PATH.
#
# PASS rule: the program prints "T0.N: PASS" (or SKIP) and exits 0.  Under
# valgrind it must also have no error in the XML log (any error fails the run)
# and none of them may have a libydk frame; the suppression file names no
# libydk frame (the SNAP rule).  Frames of libytk and other libraries are
# counted and reported.  With --xml=yes, valgrind 3.27 lists "Leak_*" records
# and exits with the --error-exitcode although --leak-check=no is given (a
# text run has neither).  Those records are not errors: they are counted
# separately and exit code 9 is accepted when no other error is in the log.
#
# Origin: Wayland backend of the ydk fork, snapshot commit 23f48f0
# (scripts/smoke-test.sh, baseline-diff.sh and valgrind-smoke.sh).

set -uo pipefail

MODE=${1:?usage: run-tests.sh weston|xvfb|vg-weston|vg-xvfb [test ...]}
shift

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=${YTK_TEST_ROOT:-$(cd "$HERE/../../../../.." && pwd)}
BUILD=${YTK_TEST_BUILD:-$ROOT/build}
OUT=${YTK_TEST_OUT:-$BUILD/ytk-tests-wayland}
LOGS=${YTK_TEST_LOGS:-$OUT/logs-$MODE}
SHOTS=${YTK_TEST_SHOTS:-1}
TMO=${YTK_TEST_TIMEOUT:-60}

case "$MODE" in
  weston|vg-weston) BACKEND=wayland ;;
  xvfb|vg-xvfb)     BACKEND=x11 ;;
  *) echo "run-tests.sh: unknown mode $MODE" >&2; exit 2 ;;
esac
VG=0
case "$MODE" in vg-*) VG=1; SHOTS=0; TMO=$((TMO * 15)) ;; esac

if [ $# -gt 0 ]; then TESTS=("$@"); else
  TESTS=(); for f in "$OUT"/t0_*; do [ -x "$f" ] && [ -f "$f" ] && TESTS+=("$(basename "$f")"); done
  IFS=$'\n' TESTS=($(printf '%s\n' "${TESTS[@]}" | sort -t_ -k2,2n -k3,3n)); unset IFS
fi

mkdir -p "$LOGS"
RUNTIME=$(mktemp -d)
chmod 0700 "$RUNTIME"
export XDG_RUNTIME_DIR="$RUNTIME"
mkdir -p "$RUNTIME/cache"
export XDG_CACHE_HOME="$RUNTIME/cache"

# fontconfig: DejaVu only, like the flake of the snapshot (makeFontsConf).
FONTDIR=${YTK_TEST_FONTDIR:-$(ls -d /nix/store/*-dejavu-fonts-*/share/fonts 2>/dev/null | head -n 1)}
FONTDIR=${FONTDIR:-/usr/share/fonts}
cat > "$RUNTIME/fonts.conf" <<FC
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<fontconfig>
<dir>$FONTDIR</dir>
<cachedir>$RUNTIME/fccache</cachedir>
</fontconfig>
FC
export FONTCONFIG_FILE="$RUNTIME/fonts.conf"

# The snapshot scripts do not set XCURSOR_PATH.  Point it at an Adwaita cursor
# theme if one exists, so the cursor code finds a theme; otherwise leave it.
CURDIR=$(ls -d /nix/store/*-adwaita-icon-theme-*/share/icons 2>/dev/null | head -n 1)
if [ -n "$CURDIR" ]; then export XCURSOR_PATH="$CURDIR" XCURSOR_THEME=Adwaita; fi

export GTK_PATH="$BUILD/libs/clearlooks-newer"
export G_DEBUG=fatal-criticals
export G_MESSAGES_DEBUG=
unset DBUS_SESSION_BUS_ADDRESS

SERVER_PID=""
cleanup() {
  if [ -n "$SERVER_PID" ]; then
    kill -TERM -- "-$SERVER_PID" 2>/dev/null || kill -TERM "$SERVER_PID" 2>/dev/null || true
    for _ in 1 2 3 4 5 6 7 8 9 10; do kill -0 "$SERVER_PID" 2>/dev/null || break; sleep 0.1; done
    kill -KILL -- "-$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
  rm -rf "$RUNTIME"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# ---- display server -----------------------------------------------------
case "$BACKEND" in
  wayland)
    cat > "$RUNTIME/weston.ini" <<'INI'
[shell]
animation=none
panel-position=none
startup-animation=none
focus-animation=none
INI
    # --renderer=pixman and --debug are both needed for weston-screenshooter.
    setsid dbus-run-session -- weston --backend=headless-backend.so \
      --renderer=pixman --debug --socket=ytktest --idle-time=0 \
      --config="$RUNTIME/weston.ini" > "$LOGS/weston.log" 2>&1 &
    SERVER_PID=$!
    for _ in $(seq 1 100); do [ -S "$RUNTIME/ytktest" ] && break; sleep 0.1; done
    [ -S "$RUNTIME/ytktest" ] || { echo "run-tests.sh: weston did not start" >&2; exit 1; }
    DISP_ENV=(env -u DISPLAY WAYLAND_DISPLAY=ytktest GDK_BACKEND=wayland T_HAVE_SEAT=0)
    ;;
  x11)
    setsid Xvfb -displayfd 7 -screen 0 1024x768x24 -nolisten tcp 7> "$RUNTIME/displayfd" \
      > "$LOGS/xvfb.log" 2>&1 &
    SERVER_PID=$!
    for _ in $(seq 1 100); do [ -s "$RUNTIME/displayfd" ] && break; sleep 0.1; done
    [ -s "$RUNTIME/displayfd" ] || { echo "run-tests.sh: Xvfb did not start" >&2; exit 1; }
    XDISP=":$(head -n 1 "$RUNTIME/displayfd")"
    DISP_ENV=(env -u WAYLAND_DISPLAY DISPLAY="$XDISP" GDK_BACKEND=x11 T_HAVE_SEAT=1)
    ;;
esac

take_shot() { # tag testname
  local tag=$1 dest=$2
  if [ "$BACKEND" = wayland ]; then
    local d="$RUNTIME/shot"; rm -rf "$d"; mkdir -p "$d"
    ( cd "$d" && WAYLAND_DISPLAY=ytktest timeout 20 weston-screenshooter >/dev/null 2>&1 )
    local f; f=$(ls "$d"/wayland-screenshot-*.png 2>/dev/null | head -n 1)
    [ -n "$f" ] && cp "$f" "$dest" || echo "run-tests.sh: no screenshot for $tag" >&2
  else
    DISPLAY="$XDISP" timeout 20 import -window root "$dest" 2>/dev/null \
      || echo "run-tests.sh: no screenshot for $tag" >&2
  fi
}

# Parses a valgrind xml log; prints "errors libydk libytk" and the offenders.
vg_summary() {
  python3 - "$1" <<'PY'
import sys, xml.etree.ElementTree as ET
try:
    root = ET.parse(sys.argv[1]).getroot()
except Exception as e:
    print("-1 -1 -1 0"); print("  cannot parse valgrind xml: %s" % e); sys.exit(0)
allerrs = root.findall("error")
errs = [e for e in allerrs if not (e.findtext("kind") or "").startswith("Leak_")]
leaks = len(allerrs) - len(errs)
ydk = ytk = 0
lines = []
for e in errs:
    objs = []
    for fr in e.iter("frame"):
        o = fr.findtext("obj") or ""
        fn = fr.findtext("fn") or "?"
        objs.append((o, fn, fr.findtext("file") or "", fr.findtext("line") or ""))
    in_ydk = any("libydk" in o for o, *_ in objs)
    in_ytk = any("libytk" in o for o, *_ in objs)
    ydk += in_ydk; ytk += in_ytk
    kind = (e.findtext("kind") or "?")
    top = objs[0] if objs else ("", "?", "", "")
    lines.append("  %s%s%s: %s at %s (%s:%s)" % (kind, " [libydk]" if in_ydk else "",
                 " [libytk]" if in_ytk else "", (e.findtext("xwhat/text") or e.findtext("what") or "").strip(),
                 top[1], top[2], top[3]))
    for o, fn, f, l in objs[:12]:
        lines.append("      %s %s:%s %s" % (fn, f, l, o.rsplit("/", 1)[-1]))
print(len(errs), ydk, ytk, leaks)
print("\n".join(lines))
PY
}

SUMMARY="$LOGS/summary.txt"
: > "$SUMMARY"
ALLRC=0

for t in "${TESTS[@]}"; do
  bin="$OUT/$t"
  [ -x "$bin" ] || { echo "run-tests.sh: $bin is not built" >&2; ALLRC=1; continue; }
  tn=$(echo "$t" | sed -E 's/^t0_([0-9]+)_.*/T0.\1/')
  log="$LOGS/$t.log"
  sync="$RUNTIME/sync-$t"; rm -rf "$sync"; mkdir -p "$sync"
  savedir="$LOGS/fail-png"; mkdir -p "$savedir"

  cmd=("${DISP_ENV[@]}" T_SAVE_DIR="$savedir")
  if [ "$VG" = 1 ]; then
    cmd+=(G_SLICE=always-malloc G_DEBUG=gc-friendly,fatal-criticals T_TIMEOUT_SCALE=15)
    vgxml="$LOGS/$t.vg.xml"
    cmd+=(valgrind --xml=yes --xml-file="$vgxml" --error-exitcode=9 --track-origins=yes
          --num-callers=30 --leak-check=no --suppressions="$HERE/valgrind.supp" "$bin")
  else
    [ "$SHOTS" = 1 ] && cmd+=(T_SYNC_DIR="$sync")
    cmd+=("$bin")
  fi

  start=$(date +%s)
  timeout "$TMO" "${cmd[@]}" > "$log" 2>&1 &
  pid=$!
  while kill -0 "$pid" 2>/dev/null; do
    for f in "$sync"/ready-*; do
      [ -e "$f" ] || continue
      tag=${f##*/ready-}
      [ -e "$sync/done-$tag" ] && continue
      take_shot "$tag" "$LOGS/shot-$MODE-$tag.png"
      touch "$sync/done-$tag"
    done
    sleep 0.1
  done
  wait "$pid"; rc=$?
  secs=$(( $(date +%s) - start ))

  verdict=$(grep -E "^T0\.[0-9]+: (PASS|FAIL|SKIP)" "$log" | tail -n 1)
  status=FAIL
  case "$verdict" in *": PASS"*) status=PASS ;; *": SKIP"*) status=SKIP ;; esac
  note=""
  [ "$rc" = 124 ] && { status=FAIL; note="timeout after ${TMO}s"; }
  if [ "$VG" = 1 ] && [ "$rc" = 9 ] && [ -f "$LOGS/$t.vg.xml" ]; then
    # exit code 9 with only Leak_* records in the XML log is not an error
    read -r vnerr _ <<<"$(vg_summary "$LOGS/$t.vg.xml" | head -n 1)"
    [ "$vnerr" = 0 ] && rc=0
  fi
  [ "$rc" != 0 ] && [ "$rc" != 124 ] && [ "$status" != FAIL ] && { status=FAIL; note="rc=$rc with verdict line"; }
  if [ "$VG" = 1 ]; then
    read -r nerr nydk nytk nleak <<<"$(vg_summary "$vgxml" | head -n 1)"
    vg_summary "$vgxml" | tail -n +2 > "$LOGS/$t.vg-errors.txt"
    if [ "$nerr" != 0 ]; then status=FAIL; note="$note $nerr valgrind errors"; fi
    if [ "$nydk" != 0 ]; then status=FAIL; note="$note libydk frames in $nydk valgrind errors"; fi
    note="$note valgrind errors=$nerr libydk=$nydk libytk=$nytk (leak records ignored: $nleak)"
  fi
  [ "$status" = FAIL ] && ALLRC=1
  printf '%-4s %-9s %-34s %-5s rc=%-3s %3ss %s\n' "$tn" "$MODE" "$t" "$status" "$rc" "$secs" "$note" | tee -a "$SUMMARY"
done

echo "run-tests.sh: logs in $LOGS"
exit $ALLRC
