#!/usr/bin/env bash
# Starts a patched Ardour on the Wayland backend inside the harness sway.
# usage: run-ardour-harness.sh OUTDIR       env: WLDEBUG=1 (protocol trace to OUTDIR/ardour.trace),
#        PRELIB=DIR (a libydk.so in DIR goes first in LD_LIBRARY_PATH, for a before/after run)
# Needs start-sway.sh first (../start-sway.sh).  Stop it with stop-ardour.sh.  Paths and the
# variables that change them are in ard-lib.sh.
#
# Ardour runs with an isolated HOME ($ARDOUR_HARNESS_HOME) and the Dummy audio engine.  Seed that
# HOME once, outside this script: run gtk2_ardour/ardev on the X11 backend with HOME and the XDG
# directories pointing to it, finish the setup wizard, choose "None (Dummy)", device "Silence",
# start the engine, create the session at $ARDOUR_HARNESS_SESSION and quit.  Never point it at
# the real configuration of a user.  The tree needs a waf build of gtk2_ardour.
set -eu
OUT=${1:?outdir}; mkdir -p "$OUT"
. "$(dirname "${BASH_SOURCE[0]}")/ard-lib.sh"
. "$SEAT/client-env.sh"
export HOME=$ARD_HOME XDG_CONFIG_HOME=$ARD_HOME/.config XDG_CACHE_HOME=$ARD_HOME/.cache XDG_DATA_HOME=$ARD_HOME/.local/share
export ARDOUR_TRY_AUTOSTART_ENGINE=1 UBUNTU_MENUPROXY= G_MESSAGES_DEBUG=
[ "${WLDEBUG:-0}" = 1 ] && export WAYLAND_DEBUG=1
cd "$ROOT"
nohup nix-shell "$SHELL_NIX" --run \
  "unset AS; export TOP=$ROOT; . build/gtk2_ardour/ardev_common_waf.sh; ${PRELIB:+export LD_LIBRARY_PATH=$PRELIB:\$LD_LIBRARY_PATH;} exec ./\$EXECUTABLE -a -O -P $ARD_SESSION" \
  > "$OUT/ardour.log" 2> "$OUT/ardour.trace" &
echo $! > "$ST/ardour.pid"
echo "started, log $OUT/ardour.log"
