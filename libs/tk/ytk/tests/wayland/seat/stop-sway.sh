#!/usr/bin/env bash
# Stops the harness sway and its input helper.  Only the pids recorded in state/env.sh
# are stopped, and only if their XDG_RUNTIME_DIR is the one of the harness.
. "$(dirname "$0")/lib.sh"
[ -f "$ST/env.sh" ] || { echo "stop-sway.sh: not running"; exit 0; }
. "$ST/env.sh"
ours() { [ -r "/proc/$1/environ" ] && tr '\0' '\n' < "/proc/$1/environ" | grep -qx "XDG_RUNTIME_DIR=$HARNESS_RT"; }
if [ -n "${SEAT_INPUT_PID:-}" ] && ours "$SEAT_INPUT_PID"; then
  echo quit > "$ST/input.fifo"; sleep 0.3; kill -TERM "$SEAT_INPUT_PID" 2>/dev/null
fi
if ours "$SWAY_PID"; then
  PP=$(ps -o ppid= -p "$SWAY_PID" | tr -d ' ')
  kill -TERM "$SWAY_PID" 2>/dev/null
  for _ in $(seq 1 50); do kill -0 "$SWAY_PID" 2>/dev/null || break; sleep 0.1; done
  kill -KILL "$SWAY_PID" 2>/dev/null
  ps -o args= -p "$PP" 2>/dev/null | grep -q dbus-run-session && kill -TERM "$PP" 2>/dev/null
  echo "stop-sway.sh: stopped sway $SWAY_PID"
else
  echo "stop-sway.sh: pid $SWAY_PID is not the harness sway, not touching it"
fi
rm -rf "$ST"
