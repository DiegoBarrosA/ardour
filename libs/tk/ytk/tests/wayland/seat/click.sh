#!/usr/bin/env bash
# Drives the virtual pointer and keyboard of the harness seat (see seat-input.c).
# Positions are output pixels (1920x1080).  Returns when sway has the events.
#
# usage: click.sh move X Y
#        click.sh click X Y [N]          move, click button 1 N times
#        click.sh rclick X Y             move, click button 3
#        click.sh press X Y [N] | release [N]     button N (default 1)
#        click.sh path X1 Y1 X2 Y2 [STEPS]       move in small steps, 15 ms apart (hover test)
#        click.sh drag X1 Y1 X2 Y2
#        click.sh wheel up|down [N]
#        click.sh key NAME...            xkb keysym names: Down, Escape, Return, a, A
#        click.sh raw "CMD" ...          lines of the seat-input protocol
. "$(dirname "$0")/lib.sh"
[ -f "$ST/env.sh" ] || { echo "click.sh: harness sway not running" >&2; exit 1; }
. "$ST/env.sh"
SYNC=$ST/sync.$$.$RANDOM
B=""
add() { B+="$*"$'\n'; }
cmd=${1:?usage: see the header of click.sh}; shift
case "$cmd" in
  move)    add "move $1 $2" ;;
  click)   add "move $1 $2"; add "sleep ${CLICK_SETTLE:-150}"
           for _ in $(seq 1 "${3:-1}"); do add "down 1"; add "sleep 60"; add "up 1"; add "sleep 100"; done ;;
  rclick)  add "move $1 $2"; add "sleep ${CLICK_SETTLE:-150}"; add "down 3"; add "sleep 60"; add "up 3"; add "sleep 100" ;;
  press)   add "move $1 $2"; add "sleep ${CLICK_SETTLE:-150}"; add "down ${3:-1}" ;;
  release) add "up ${1:-1}"; add "sleep 100" ;;
  path)    n=${5:-10}
           for i in $(seq 0 "$n"); do add "move $(( $1 + ($3 - $1) * i / n )) $(( $2 + ($4 - $2) * i / n ))"; add "sleep 15"; done ;;
  drag)    add "move $1 $2"; add "sleep 150"; add "down 1"; add "sleep 100"
           for i in 1 2 3 4 5 6 7 8; do add "move $(( $1 + ($3 - $1) * i / 8 )) $(( $2 + ($4 - $2) * i / 8 ))"; add "sleep 30"; done
           add "sleep 100"; add "up 1"; add "sleep 100" ;;
  wheel)   for _ in $(seq 1 "${2:-1}"); do [ "$1" = up ] && add "axis -10" || add "axis 10"; add "sleep 50"; done ;;
  key)     for k in "$@"; do add "key $k"; add "sleep 60"; done ;;
  raw)     for l in "$@"; do add "$l"; done ;;
  *) echo "click.sh: unknown command $cmd" >&2; exit 2 ;;
esac
add "sync $SYNC"
printf '%s' "$B" > "$ST/input.fifo"
for _ in $(seq 1 300); do [ -e "$SYNC" ] && { rm -f "$SYNC"; exit 0; }; sleep 0.05; done
echo "click.sh: timeout waiting for seat-input" >&2; exit 1
