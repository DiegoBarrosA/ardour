#!/usr/bin/env bash
# The press, drag, release reproduction in a running Ardour (one output).  usage: ard-steps.sh OUTDIR
# Coordinates are those of the editor of the test session at 1920x1080 on one output, relative to
# the Ardour window at ARD_OX, ARD_OY (0, 0 by default).  The trace needs WLDEBUG=1 in ard-repro.sh.
export A=$1
. "$(dirname "${BASH_SOURCE[0]}")/ard-lib.sh"
X() { echo $(( ARD_OX + $1 )); }
Y() { echo $(( ARD_OY + $1 )); }
# 1. dropdown: press on Slide, drag to Lock, release
step d1-press-slide;  $CL press $(X 48) $(Y 156); fin 1
step d2-drag-to-lock; $CL path $(X 48) $(Y 156) $(X 32) $(Y 212) 8; fin 0.6
step d3-release;      $CL release; fin 1.2
# 2. menu bar: press Transport, drag to Auto Return, release
step m1-press-transport; $CL press $(X 114) $(Y 17); fin 1
step m2-drag-auto-return; $CL path $(X 114) $(Y 17) $(X 114) $(Y 60) 3; $CL path $(X 114) $(Y 60) $(X 142) $(Y 746) 24; fin 0.6
step m3-release; $CL release; fin 1.2
# 3. reopen Transport to see whether Auto Return is checked, then Escape
step m4-reopen; $CL click $(X 114) $(Y 17); fin 1
$CL key Escape; sleep 0.6
# 4. reopen the dropdown to see the edit mode, then Escape
step d4-reopen; $CL click $(X 48) $(Y 156); fin 1
$CL key Escape; sleep 0.6
# 5. a menu opened by the keyboard: F10 opens the first menu of the menu bar
step k1-f10; $CL key F10; fin 1
$CL key Escape; sleep 0.6
