#!/usr/bin/env bash
# Stops the Ardour that run-ardour-harness.sh started: only processes whose XDG_RUNTIME_DIR is the
# one of the harness sway.
. "$(dirname "${BASH_SOURCE[0]}")/ard-lib.sh"
for pid in $(ardour_pids); do kill -TERM "$pid"; sleep 3; kill -KILL "$pid" 2>/dev/null; done
exit 0
