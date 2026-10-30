#!/usr/bin/env bash
# usage: shot.sh OUT.png [X,Y WxH]     Screenshot of the harness sway with grim.
. "$(dirname "$0")/client-env.sh" || exit 1
OUT=${1:?usage: shot.sh OUT.png [X,Y WxH]}
S=${SHOT_SCALE:+-s $SHOT_SCALE}
if [ -n "${2:-}" ]; then grim $S -g "$2${3:+ $3}" "$OUT"; else grim $S "$OUT"; fi
echo "shot.sh: $OUT"
