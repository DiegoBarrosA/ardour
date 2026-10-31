#!/usr/bin/env bash
# Starts the harness sway and Ardour, waits for the main window and takes a first screenshot.
# usage: ard-repro.sh OUTDIR [PRELIB]
#   PRELIB   directory with a libydk.so that goes first in LD_LIBRARY_PATH (a "before" build)
# env: WLDEBUG=1 for a protocol trace, HARNESS_SWAY_CONF for another sway config (two outputs:
#      ../sway-2out.conf).  Then run ard-steps.sh OUTDIR or ard-two-outputs.sh OUTDIR.
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
A=$1; export PRELIB=${2:-}; mkdir -p "$A"; : > "$A/ardour.trace"
. "$HERE/ard-lib.sh"
"$SEAT/start-sway.sh" > "$A/sway.log" 2>&1
WLDEBUG=${WLDEBUG:-0} "$HERE/run-ardour-harness.sh" "$A" > "$A/start.log" 2>&1
# wait for the main window (the editor shows the Slide dropdown)
for i in $(seq 1 60); do sleep 2; "$SEAT/shot.sh" "$A/probe.png" >/dev/null 2>&1; [ -s "$A/probe.png" ] && grep -q "Ardour" "$A/ardour.log" 2>/dev/null && [ $i -gt 8 ] && break; done
sleep 4
"$SEAT/shot.sh" "$A/start.png" >/dev/null
pid=$(ardour_pids | head -n 1); echo "ardour pid=$pid"
grep -m1 libydk "/proc/$pid/maps" | awk '{print "libydk: "$6}' | tee "$A/libydk.txt"
