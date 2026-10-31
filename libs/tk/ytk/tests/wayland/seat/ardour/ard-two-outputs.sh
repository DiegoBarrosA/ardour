#!/usr/bin/env bash
# Ardour on the second output of seat/sway-2out.conf.  usage: ard-two-outputs.sh OUTDIR
# Start with: HARNESS_SWAY_CONF=<this tree>/libs/tk/ytk/tests/wayland/seat/sway-2out.conf ard-repro.sh OUTDIR
# It moves the Ardour window to the bigger output, opens the Transport menu there and takes
# screenshots of the whole layout.  The Transport menu has to show in full and in its place.
export A=$1
. "$(dirname "${BASH_SOURCE[0]}")/ard-lib.sh"
. "$ST/env.sh"
OUTS=$(swaymsg -s "$HARNESS_SWAYSOCK" -t get_outputs -r)
BIG=$(jq -r 'sort_by(.rect.width) | last | .name' <<<"$OUTS")
read -r ARD_OX ARD_OY <<<"$(jq -r 'sort_by(.rect.width) | last | "\(.rect.x) \(.rect.y)"' <<<"$OUTS")"
step o0-first-output; fin 0.5
swaymsg -s "$HARNESS_SWAYSOCK" -q "[app_id=\"Ardour\"] move to output $BIG"
sleep 3
step o1-moved; fin 1
step o2-transport; $CL click $((ARD_OX + 114)) $((ARD_OY + 17)); fin 1
$CL key Escape; sleep 0.6
step o3-file; $CL click $((ARD_OX + 20)) $((ARD_OY + 17)); fin 1
$CL key Escape; sleep 0.6
