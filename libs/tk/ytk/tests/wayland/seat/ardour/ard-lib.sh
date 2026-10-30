# ard-lib.sh -- helpers to drive a patched Ardour in the harness sway.  Source it.
#
# Paths are relative to this directory or come from the environment:
#   HARNESS_HOME           harness state and helper binary (see ../lib.sh, default
#                          $XDG_CACHE_HOME/ytk-seat-harness)
#   YTK_TEST_ROOT          the Ardour tree with the waf build (default: the tree of this file)
#   ARDOUR_HARNESS_HOME    HOME of Ardour: an isolated config with the Dummy engine
#                          (default $HARNESS_HOME/ardour-home).  Seed it once with a session
#                          on the X11 backend, see the README comment of run-ardour-harness.sh.
#   ARDOUR_HARNESS_SESSION the session file (default $HARNESS_HOME/ardsess/t1/t1.ardour)
#   A                      output directory of a run (default $HARNESS_HOME/ard-out/r1)
#   ARD_OX, ARD_OY         global position of the Ardour window, for steps on another output
# Functions: tl, step NAME, fin [SECONDS], pop STEP, ardour_pids.
ARD_HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SEAT=$(cd "$ARD_HERE/.." && pwd)
. "$SEAT/lib.sh"
ROOT=${YTK_TEST_ROOT:-$(cd "$ARD_HERE/../../../../../../.." && pwd)}
ARD_HOME=${ARDOUR_HARNESS_HOME:-$HH/ardour-home}
ARD_SESSION=${ARDOUR_HARNESS_SESSION:-$HH/ardsess/t1/t1.ardour}
CL=$SEAT/click.sh
A=${A:-$HH/ard-out/r1}
ARD_OX=${ARD_OX:-0}; ARD_OY=${ARD_OY:-0}

tl() { wc -l < "$A/ardour.trace"; }
step() { NAME=$1; T0=$(( $(tl) + 1 )); echo "== $NAME"; }
fin() { sleep "${1:-0.5}"; "$SEAT/shot.sh" "$A/$NAME.png" >/dev/null; sed -n "$T0,\$p" "$A/ardour.trace" > "$A/$NAME.trace"; echo "   trace lines: $(wc -l < "$A/$NAME.trace")"; }
pop() { grep -E "xdg_positioner|xdg_popup|get_popup|popup_done|wl_pointer#[0-9]+\.(enter|leave|button)|wl_keyboard#[0-9]+\.(enter|leave)|wl_surface#[0-9]+\.(attach|destroy)" "$A/$1.trace" | grep -v "wl_surface.*attach" | cut -c1-200; }

# pids of the Ardour that runs in the harness sway: its environment names the harness runtime dir
ardour_pids() {
  [ -f "$ST/env.sh" ] || return 0
  . "$ST/env.sh"
  local pid
  for pid in $(pgrep -u "$(id -u)" -f 'gtk2_ardour/ardour-|/ardour-[0-9]'); do
    tr '\0' '\n' < "/proc/$pid/environ" 2>/dev/null | grep -qx "XDG_RUNTIME_DIR=$HARNESS_RT" && echo "$pid"
  done
}
