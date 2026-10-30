# t12-scenarios.sh -- the click scenarios of T0.12.  Sourced by run-seat-tests.sh after
# seat-lib.sh.  Each scenario is a function s_NAME.  A scenario opens widgets with the
# virtual pointer and checks the signals that T0.12 logs.
#
# Pointer helpers keep the pointer position in PX PY, so "hover" moves in small steps like a
# mouse does and the compositor sends motion, enter and leave events on the way.
#
# Results on wayland/m11 at 2e81c8dc10 (headless sway 1.12, ffm = focus_follows_mouse):
#   s12-dropdown-drag   FAILED: press on a dropdown button, drag onto an item, release: the item was
#                       not activated and the menu stayed open (Ardour: it closed without activating).
#                       Events during the drag went to the main surface and were not mapped to the popup.
#   s17-second-window   FAILED, but the cause is the compositor, not the backend.  Sway closes the
#                       popups of a toplevel when that toplevel loses keyboard focus (send_unfocus in
#                       sway/input/seat.c calls view_close_popups, which destroys every xdg_popup of
#                       the view, with or without xdg_popup.grab).  So a menu closes when focus moves
#                       to another toplevel of the same client: on a click there (the click is then
#                       delivered, X11 would swallow it) and with focus_follows_mouse when the pointer
#                       enters it.  The scenario now checks that the menu closes cleanly and that both
#                       windows work afterwards.
# Every other scenario passes with and without ffm, at scale 2 and at scale 1.25.
# T12_FFM is 1 when the sway of the run has focus_follows_mouse yes (run-seat-tests.sh sets it).

PX=0; PY=0
hover()  { arm; "$CLICK" path "$PX" "$PY" "$1" "$2" "${3:-8}"; PX=$1; PY=$2; }
# into a menu from the item above it: straight down first, so the path does not cross the next menu bar item
hovv()   { arm; "$CLICK" path "$PX" "$PY" "$PX" "$2" 4; "$CLICK" path "$PX" "$2" "$1" "$2" 6; PX=$1; PY=$2; }
lclick() { hover "$1" "$2"; "$CLICK" click "$1" "$2"; }
rclick() { hover "$1" "$2"; "$CLICK" rclick "$1" "$2"; }
ltap()   { hover "$1" "$2"; "$CLICK" tap "$1" "$2"; }   # press and release at once, like a touchpad tap
vtap()   { hovv "$1" "$2"; "$CLICK" tap "$1" "$2"; }     # same, entering a menu from the item above it
vclick() { hovv "$1" "$2"; "$CLICK" click "$1" "$2"; }
keys()   { arm; "$CLICK" key "$@"; }

# GTK ignores a button release less than 500 ms after the press that opened a menu (MENU_SHELL_TIMEOUT
# in gtkmenushell.c: the event times must differ by more than 500).  A drag scenario has to hold the
# button for longer, or it passes and fails with the speed of the run.  press_hold presses and
# remembers when, release_held waits until T12_HOLD_MS (default 650) ms have passed and then releases.
press_hold()   { "$CLICK" press "$@"; PRESS_NS=$(date +%s%N); }
release_held() {
  local left=$(( ${T12_HOLD_MS:-650} - ($(date +%s%N) - ${PRESS_NS:-0}) / 1000000 ))
  [ "$left" -gt 0 ] && pause "$left"
  "$CLICK" release
}
pause()  { sleep "$(awk "BEGIN{print $1/1000}")"; }

# Closes whatever is open and parks the pointer on the grey area.  A menu that stays open
# after Escape is a failure of the scenario before.
t12_reset() { # [quiet]
  refresh_geom
  if [ ${#MENUWIN[@]} -gt 0 ] || [ "$(popup_toplevels)" != 0 ]; then
    [ "${1:-}" = quiet ] && { keys Escape; pause 400; hover $(center ctxarea) 4; return; }
    STEP="reset-after-$STEP"
    fail "a menu or popup was still open at the start of the next scenario (${!MENUWIN[*]} / $(popup_toplevels) toplevels)"
    keys Escape; pause 400; refresh_geom
    if [ ${#MENUWIN[@]} -gt 0 ] || [ "$(popup_toplevels)" != 0 ]; then
      lclick $(center ctxarea); pause 400; refresh_geom
    fi
  fi
  hover $(center ctxarea) 4
}

open_file() { lclick $(center bar:File); expect "File menu shows" 'SIG show menu\[File\]'; refresh_geom; }
all_closed() { # desc
  refresh_geom
  if [ ${#MENUWIN[@]} -eq 0 ] && [ "$(popup_toplevels)" = 0 ]; then ok "$1"; else fail "$1 (open: ${!MENUWIN[*]}; popup toplevels: $(popup_toplevels))"; fi
}

s01_menubar_open() {
  t12_reset; step s01-menubar-open
  lclick $(center bar:File)
  expect "File menu shows" 'SIG show menu\[File\]'
  expect_not "File menu stays open for 1 s" 'hide menu\[File\]' 1000
  refresh_geom
  menu_open File && ok "File menu is mapped" || fail "File menu is not mapped"
  shot s01-file-open
  keys Escape; pause 300
  all_closed "closed after Escape"
}

s02_cascade() {
  t12_reset; step s02-cascade
  open_file
  hovv $(item_center File "Open Recent")
  expect "Recent shows when hovering Open Recent" 'SIG show menu\[Recent\]' 2000
  refresh_geom
  hover $(item_center Recent "More")
  expect "More shows (level 2)" 'SIG show menu\[More\]' 2000
  refresh_geom
  hover $(item_center More "Deeper")
  expect "Deeper shows (level 3)" 'SIG show menu\[Deeper\]' 2000
  arm_step; expect_not "no menu hides while the cascade opens" 'SIG hide menu' 400
  refresh_geom
  for m in File Recent More Deeper; do menu_open $m && ok "$m is mapped" || fail "$m is not mapped"; done
  shot s02-cascade
}

s03_deep_click() { # continues from s02: the cascade is open
  step s03-deep-click
  refresh_geom
  hover $(item_center Deeper "Deepest 2")
  expect "Deepest 2 is selected" 'SIG select item\[Deepest 2\]'
  lclick $(item_center Deeper "Deepest 2")
  expect "Deepest 2 activates" 'SIG activate item\[Deepest 2\]'
  expect "selection-done" 'SIG selection-done'
  expect "File hides" 'SIG hide menu\[File\]'
  expect "Deeper hides" 'SIG hide menu\[Deeper\]'
  all_closed "everything is closed after the click"
}

s04_diagonal() {
  t12_reset; step s04-diagonal
  open_file
  hovv $(item_center File "Open Recent")
  expect "Recent shows" 'SIG show menu\[Recent\]' 2000
  refresh_geom
  # a straight line from the item to the far item of the submenu: GTK keeps the submenu open on that path
  "$CLICK" path "$PX" "$PY" $(item_center Recent "Project B") 12; read -r PX PY <<<"$(item_center Recent "Project B")"
  expect "Project B is selected" 'SIG select item\[Project B\]'
  expect_not "Recent stays open while moving toward it" 'SIG hide menu\[Recent\]' 600
  keys Escape; pause 300
  all_closed "closed after Escape"
}

s05_outside_click() {
  t12_reset; step s05-outside-click
  open_file
  lclick 330 $(( OY + 100 ))
  expect "File hides on an outside click" 'SIG hide menu\[File\]'
  expect "selection-done" 'SIG selection-done'
  expect_not "no menu item activates" 'SIG activate item\[(New|Close|Project)' 200
  all_closed "everything is closed"
}

s06_reopen() {
  t12_reset; step s06-reopen
  local i
  for i in 1 2 3 4; do
    lclick $(center bar:File)
    expect "open $i: File shows" "SIG show menu\\[File\\]" 1500
    pause 300
    if [ $((i % 2)) = 1 ]; then keys Escape; else lclick 330 $(( OY + 100 )); fi
    pause 400
    all_closed "open $i: closed again"
  done
  arm_step; expect_count "File shown 4 times" 'SIG show menu\[File\]' 4
  expect_count "File hidden 4 times" 'SIG hide menu\[File\]' 4
}

s07_bar_switch() {
  t12_reset; step s07-bar-switch
  open_file
  hover $(center bar:Edit)
  expect "Edit shows when hovering the menu bar" 'SIG show menu\[Edit\]' 1500
  expect "File hides" 'SIG hide menu\[File\]' 1500
  refresh_geom
  hover $(center bar:View)
  expect "View shows" 'SIG show menu\[View\]' 1500
  expect "Edit hides" 'SIG hide menu\[Edit\]' 1500
  shot s07-bar-switch
  keys Escape; pause 300
  all_closed "closed after Escape"
}

s08_check_item() {
  t12_reset; step s08-check-item
  lclick $(center bar:Edit); expect "Edit shows" 'SIG show menu\[Edit\]'; refresh_geom
  lclick $(item_center Edit "Toggle me")
  expect "Toggle me toggles on" 'SIG toggled item\[Toggle me\] active=1'
  all_closed "closed after the click"
  lclick $(center bar:Edit); expect "Edit shows the second time" 'SIG show menu\[Edit\]'; refresh_geom
  shot s08-check-item
  lclick $(item_center Edit "Toggle me")
  expect "Toggle me toggles off" 'SIG toggled item\[Toggle me\] active=0'
  lclick $(center bar:Edit); expect "Edit shows the third time" 'SIG show menu\[Edit\]'; refresh_geom
  lclick $(item_center Edit "Mode B")
  expect "Mode B toggles on" 'SIG toggled item\[Mode B\] active=1'
  all_closed "closed after the radio click"
}

s09_context() {
  t12_reset; step s09-context
  rclick $(center ctxarea)
  expect "context menu shows" 'SIG show menu\[Context\]'
  refresh_geom
  hover $(item_center Context "Properties")
  expect "Properties submenu shows" 'SIG show menu\[Properties\]' 2000
  refresh_geom
  shot s09-context
  lclick $(item_center Properties "Colour")
  expect "Colour activates" 'SIG activate item\[Colour\]'
  all_closed "closed after the click"
  rclick $(center ctxarea)
  expect "context menu shows again" 'SIG show menu\[Context\]'
  pause 300
  keys Escape
  expect "context menu hides on Escape" 'SIG hide menu\[Context\]'
  all_closed "closed after Escape"
}

s10_combo() {
  t12_reset; step s10-combo
  lclick $(center combo)
  expect "combo popup opens" 'notify::popup-shown combo 1'
  expect_not "combo popup stays open for 700 ms" 'notify::popup-shown combo 0' 700
  refresh_geom
  [ "$(popup_toplevels)" -ge 1 ] && ok "a popup toplevel is mapped" || fail "no popup toplevel is mapped"
  shot s10-combo-open
  # the combo menu items have no label of ours: #0 ... #5
  local key; for key in "${!MITEM[@]}"; do :; done
  lclick $(item_center GtkMenu "#3")
  expect "combo changes to 3" 'SIG changed combo active=3'
  expect "combo popup closes" 'notify::popup-shown combo 0'
  all_closed "closed after the choice"
  lclick $(center combo)
  expect "combo popup opens the second time" 'notify::popup-shown combo 1'
  pause 300
  lclick 330 $(( OY + 100 ))
  expect "combo popup closes on an outside click" 'notify::popup-shown combo 0'
  all_closed "closed after the outside click"
}

s11_optmenu() {
  t12_reset; step s11-optmenu
  lclick $(center optmenu)
  expect "option menu shows" 'SIG show menu\[Option\]'
  refresh_geom
  shot s11-optmenu
  lclick $(item_center Option "Option 2")
  expect "option menu changes to 2" 'SIG changed optmenu active=2'
  all_closed "closed after the choice"
}

s12_dropdowns() {
  t12_reset; step s12-dropdown-press
  lclick $(center dd-press)
  expect "press dropdown shows" 'SIG show menu\[DropPress\]'
  expect_not "press dropdown stays open for 700 ms" 'SIG hide menu\[DropPress\]' 700
  refresh_geom
  shot s12-dropdown
  lclick $(item_center DropPress "DropPress 2")
  expect "DropPress 2 activates" 'SIG activate item\[DropPress 2\]'
  all_closed "closed after the click"
  t12_reset quiet; step s12-dropdown-drag
  hover $(center dd-press)
  press_hold $(center dd-press); expect "press shows the menu" 'SIG show menu\[DropPress\]'; refresh_geom
  hover $(item_center DropPress "DropPress 3")
  expect "the item under the pointer is selected during the drag" 'SIG select item\[DropPress 3\]'
  release_held
  expect "release over an item activates it" 'SIG activate item\[DropPress 3\]'
  all_closed "closed after the release"
  t12_reset quiet; step s12-dropdown-clicked
  lclick $(center dd-click)
  expect "clicked dropdown shows" 'SIG show menu\[DropClick\]'
  pause 400
  keys Escape
  expect "clicked dropdown hides on Escape" 'SIG hide menu\[DropClick\]'
  all_closed "closed after Escape"
}

s13_tooltip() {
  t12_reset; step s13-tooltip
  hover $(center tipbtn)
  pause 1800
  refresh_geom
  [ "$(popup_toplevels)" -ge 1 ] && ok "the tooltip toplevel is mapped" || fail "the tooltip did not map"
  shot s13-tooltip
  hover $(center ctxarea)
  pause 1000
  all_closed "the tooltip is gone after leaving the button"
}

s14_keyboard() {
  t12_reset; step s14-keyboard
  open_file
  keys Down; expect "Down selects New" 'SIG select item\[New\]'
  keys Down; expect "Down selects Open Recent" 'SIG select item\[Open Recent\]'
  keys Right; expect "Right opens Recent" 'SIG show menu\[Recent\]'
  keys Return
  expect "Return activates an item of Recent" 'SIG activate item\[Project A\]'
  all_closed "closed after Return"
}

s15_leave_and_return() {
  t12_reset; step s15-leave-and-return
  open_file
  hovv $(item_center File "New")
  expect "New is selected" 'SIG select item\[New\]'
  hover 330 $(( OY + 100 ))
  expect_not "File stays open while the pointer is outside" 'SIG hide menu\[File\]' 800
  hovv $(item_center File "Close")
  expect "Close is selected after coming back" 'SIG select item\[Close\]'
  keys Escape; pause 300
  step s15-stale-selection
  open_file
  pause 300
  expect_not "no item of the menu is selected on a fresh open" 'SIG select item\[(New|Open Recent|Close)\]' 300
  keys Escape; pause 300
  all_closed "closed after Escape"
}

T12_ALL_SCENARIOS="s01_menubar_open s02_cascade s03_deep_click s04_diagonal s05_outside_click s06_reopen s07_bar_switch s08_check_item s09_context s10_combo s11_optmenu s12_dropdowns s13_tooltip s14_keyboard s15_leave_and_return"

place_win2() { # move the second window next to the main one
  local id
  id=$(swaymsg -s "$HARNESS_SWAYSOCK" -t get_tree -r | jq -r '.. | objects | select((.app_id? // "") | test("^[Tt]0_12")) | select(.window_rect.width == 260) | .id' | head -n 1)
  [ -n "$id" ] && swaymsg -s "$HARNESS_SWAYSOCK" -q "[con_id=$id] move position 700 100"
  pause 300
}

s16_pointer_leaves_client() {
  t12_reset; step s16-pointer-leaves-client
  open_file
  hover 1700 900 14
  pause 800
  expect_not "File stays open while the pointer is over the empty desktop" 'SIG hide menu\[File\]'
  refresh_geom
  menu_open File && ok "File is still mapped" || fail "File is not mapped any more"
  shot s16-pointer-away
  hovv $(item_center File "New") 
  expect "the menu takes hover again when the pointer comes back" 'SIG select item\[New\]'
  keys Escape; pause 300
  all_closed "closed after Escape"
}

s17_second_window() {
  t12_reset; step s17-second-window
  app_send win2; pause 900; place_win2; refresh_geom
  open_file
  hover 330 $(( OY + 150 )) 8; hover $(center w2:W2) 10
  pause 700
  shot s17-over-second-window
  if [ "${T12_FFM:-0}" = 1 ]; then
    expect "File closes when the pointer enters the second window (sway moves focus)" 'SIG hide menu\[File\]'
    all_closed "everything is closed after the hover"
    lclick $(center w2:W2)
  else
    expect_not "File stays open while the pointer is over the second window" 'SIG hide menu\[File\]'
    lclick $(center w2:W2)
    expect "File closes on a click in the second window" 'SIG hide menu\[File\]'
    # Sway moves focus on the press and closes the popup, then delivers the press (X11 swallows it).
    expect "the click reaches the second window and opens its menu" 'SIG show menu\[W2\]'
  fi
  expect "the second window opens its own menu" 'SIG show menu\[W2\]'
  refresh_geom
  shot s17-second-menu
  lclick $(item_center W2 "Other B")
  expect "Other B activates" 'SIG activate item\[Other B\]'
  all_closed "closed after the click"
  # The main window still works after sway closed its popup.
  open_file; vclick $(item_center File "New")
  expect "New activates in the main window" 'SIG activate item\[New\]'
  all_closed "closed after the click"
  app_send win2-hide; pause 600
}

s18_keyboard_focus_back() {
  t12_reset; step s18-keyboard-focus
  lclick $(center ctxarea)
  keys a; expect "a key reaches the main window before any menu" 'KEY_PRESS .* main keyval=0x61'
  open_file; keys Escape; pause 300
  keys b; expect "a key reaches the main window after a menu closed with Escape" 'KEY_PRESS .* main keyval=0x62'
  open_file; lclick $(item_center File "New"); pause 300
  keys c; expect "a key reaches the main window after a menu item was clicked" 'KEY_PRESS .* main keyval=0x63'
  lclick $(center combo); expect "combo opens" 'notify::popup-shown combo 1'; refresh_geom
  lclick $(item_center GtkMenu "#2"); pause 300
  keys d; expect "a key reaches the main window after a combo choice" 'KEY_PRESS .* main keyval=0x64'
}

# Press on the menu bar, drag, release: the item under the pointer activates.  While the button is
# down the compositor keeps pointer focus on the main surface, so the backend has to aim the events
# at the popup under the pointer (update_pointer_target in gdkevents-wayland.c).
s19_press_drag_release() {
  t12_reset; step s19-bar-drag-item
  hover $(center bar:File)
  press_hold $(center bar:File); expect "press on File shows the menu" 'SIG show menu\[File\]'; refresh_geom
  hovv $(item_center File "New")
  expect "New is selected during the drag" 'SIG select item\[New\]'
  release_held
  expect "release over New activates it" 'SIG activate item\[New\]'
  all_closed "closed after the release"

  t12_reset quiet; step s19-bar-drag-cascade
  hover $(center bar:File)
  press_hold $(center bar:File); expect "press on File shows the menu" 'SIG show menu\[File\]'; refresh_geom
  hovv $(item_center File "Open Recent")
  expect "Recent shows during the drag" 'SIG show menu\[Recent\]' 2000
  refresh_geom
  hover $(item_center Recent "More")
  expect "More shows during the drag (level 2)" 'SIG show menu\[More\]' 2000
  refresh_geom
  hover $(item_center More "Deeper")
  expect "Deeper shows during the drag (level 3)" 'SIG show menu\[Deeper\]' 2000
  refresh_geom
  hover $(item_center Deeper "Deepest 2")
  expect "Deepest 2 is selected during the drag" 'SIG select item\[Deepest 2\]'
  release_held
  expect "release over Deepest 2 activates it" 'SIG activate item\[Deepest 2\]'
  all_closed "everything is closed after the release"

  t12_reset quiet; step s19-release-outside
  hover $(center bar:File)
  press_hold $(center bar:File); expect "press on File shows the menu" 'SIG show menu\[File\]'; refresh_geom
  hovv $(item_center File "New")
  expect "New is selected during the drag" 'SIG select item\[New\]'
  hover 330 $(( OY + 100 ))
  release_held
  expect "release outside the menu hides it" 'SIG hide menu\[File\]'
  expect_not "no leaf item activates on a release outside" 'SIG activate item\[(New|Close|Project|Deepest)' 200
  all_closed "everything is closed after the release"
}

T12_ALL_SCENARIOS="$T12_ALL_SCENARIOS s16_pointer_leaves_client s17_second_window s18_keyboard_focus_back s19_press_drag_release"

t12_run() { # [only list: s01 s02 ...]
  local only=$1 s
  refresh_geom
  read -r PX PY <<<"$(center ctxarea)"
  for s in $T12_ALL_SCENARIOS; do
    if [ -n "$only" ] && ! grep -qw "${s%%_*}" <<<"$only"; then continue; fi
    "$s"
  done
  t12_reset
}

# The title of a toplevel reaches the compositor: the one given at creation (GTK passes it as
# GDK_WA_TITLE and does not call gdk_window_set_title at realize) and one set later.
sway_names() { # "main NAME" and "win2 NAME" of the T0.12 windows, from the compositor
  swaymsg -s "$HARNESS_SWAYSOCK" -t get_tree -r 2>/dev/null |
    jq -r '.. | objects | select((.app_id? // "") | test("^[Tt]0_12")) | "\(if .window_rect.width == 260 then "win2" else "main" end) \(.name // "null")"'
}
title_is() { # which expected
  if sway_names | grep -qxF "$1 $2"; then ok "$1 window title is \"$2\""; else fail "$1 window title is not \"$2\" (compositor has: $(sway_names | tr '\n' ';'))"; fi
}
s20_titles() {
  t12_reset; step s20-titles
  title_is main "T0.12 menus"
  app_send win2; pause 900
  title_is win2 "T0.12 second"
  app_send "title Renamed main"; pause 400
  title_is main "Renamed main"
  app_send "title T0.12 menus"; app_send win2-hide; pause 400
}

T12_ALL_SCENARIOS="$T12_ALL_SCENARIOS s20_titles"

# Menus of a window near the right and bottom edge of the output.  GTK believes a toplevel sits
# at 0,0 (Wayland hides the real position), so it never flips a menu itself.  The compositor
# slides a popup that does not fit (SLIDE_X | SLIDE_Y).  Every popup has to stay inside the output.
OUT_W=1920; OUT_H=1080
move_main() { # global x y of the main window
  local id
  id=$(swaymsg -s "$HARNESS_SWAYSOCK" -t get_tree -r | jq -r '.. | objects | select((.app_id? // "") | test("^[Tt]0_12")) | select(.window_rect.width != 260) | .id' | head -n 1)
  [ -n "$id" ] && swaymsg -s "$HARNESS_SWAYSOCK" -q "[con_id=$id] move position $1 $2"
  pause 500
}
popups_inside_output() { # desc: every popup toplevel lies inside the output
  local line bad="" n=0 rx ry w h
  refresh_geom
  for line in "${TOPS[@]}"; do
    [[ $line =~ \ main\ type=0 ]] && continue
    [[ $line =~ \ win2\ type=0 ]] && continue
    [[ $line =~ \(relative\ to\ main\ (-?[0-9]+),(-?[0-9]+)\)\ size\ ([0-9]+)x([0-9]+) ]] || continue
    rx=$(( OX + BASH_REMATCH[1] )); ry=$(( OY + BASH_REMATCH[2] )); w=${BASH_REMATCH[3]}; h=${BASH_REMATCH[4]}
    n=$((n+1))
    if [ $rx -lt 0 ] || [ $ry -lt 0 ] || [ $((rx+w)) -gt $OUT_W ] || [ $((ry+h)) -gt $OUT_H ]; then bad="$bad [$rx,$ry ${w}x$h]"; fi
  done
  if [ -z "$bad" ]; then ok "$1 ($n popups inside the output)"; else fail "$1 (outside:$bad)"; fi
}
s21_near_edges() {
  t12_reset; step s21-near-bottom-right; SHOT_GEOM="900,380 1020x700"
  move_main $(( OUT_W - 400 )) $(( OUT_H - 260 )); refresh_geom
  read -r PX PY <<<"$(center ctxarea)"
  open_file
  hovv $(item_center File "Open Recent")
  expect "Recent shows" 'SIG show menu\[Recent\]' 2000
  refresh_geom; hover $(item_center Recent "More")
  expect "More shows" 'SIG show menu\[More\]' 2000
  refresh_geom; hover $(item_center More "Deeper")
  expect "Deeper shows" 'SIG show menu\[Deeper\]' 2000
  pause 400
  popups_inside_output "the cascade of File stays inside the output"
  shot s21-cascade-right-edge
  refresh_geom; hover $(item_center Deeper "Deepest 2")
  expect "Deepest 2 is selected" 'SIG select item\[Deepest 2\]'
  lclick $(item_center Deeper "Deepest 2")
  expect "Deepest 2 activates" 'SIG activate item\[Deepest 2\]'
  all_closed "closed after the click"

  t12_reset quiet; step s21-dropdown-bottom
  lclick $(center dd-press)
  expect "dropdown shows" 'SIG show menu\[DropPress\]'
  pause 400; popups_inside_output "the dropdown near the bottom edge stays inside the output"
  shot s21-dropdown-bottom
  refresh_geom; lclick $(item_center DropPress "DropPress 4")
  expect "DropPress 4 activates" 'SIG activate item\[DropPress 4\]'
  all_closed "closed after the click"

  t12_reset quiet; step s21-combo-bottom
  lclick $(center combo); expect "combo opens" 'notify::popup-shown combo 1'
  pause 400; popups_inside_output "the combo menu stays inside the output"
  shot s21-combo-bottom
  keys Escape; pause 300; all_closed "closed after Escape"
  move_main 100 100; unset SHOT_GEOM
}

T12_ALL_SCENARIOS="$T12_ALL_SCENARIOS s21_near_edges"

# A touchpad tap is a press and a release with no delay.  The release can reach the client before
# the popup the press opened is mapped.  The menu has to stay open, like a short click on X11.
s22_fast_tap() {
  t12_reset; step s22-tap-dropdown
  ltap $(center dd-press)
  expect "tap on a press-dropdown shows the menu" 'SIG show menu\[DropPress\]'
  expect_not "the menu stays open after the tap" 'SIG hide menu\[DropPress\]' 700
  refresh_geom; menu_open DropPress && ok "DropPress is mapped" || fail "DropPress is not mapped"
  ltap $(item_center DropPress "DropPress 3")
  expect "a tap on an item activates it" 'SIG activate item\[DropPress 3\]'
  all_closed "closed after the tap"

  t12_reset quiet; step s22-tap-menubar
  ltap $(center bar:File)
  expect "tap on File shows the menu" 'SIG show menu\[File\]'
  expect_not "File stays open after the tap" 'SIG hide menu\[File\]' 700
  refresh_geom
  vtap $(item_center File "New")
  expect "a tap on New activates it" 'SIG activate item\[New\]'
  all_closed "closed after the tap"

  t12_reset quiet; step s22-tap-combo
  ltap $(center combo)
  expect "tap on the combo opens it" 'notify::popup-shown combo 1'
  expect_not "the combo stays open after the tap" 'notify::popup-shown combo 0' 700
  refresh_geom
  ltap $(item_center GtkMenu "#5")
  expect "a tap on a combo item changes it" 'SIG changed combo active=5'
  all_closed "closed after the tap"
}

T12_ALL_SCENARIOS="$T12_ALL_SCENARIOS s22_fast_tap"

# A click outside every surface of the client dismisses the menu: the compositor ends the popup
# grab (popup_done) because the pointer has no focus, and the click does not reach the client.
s23_click_on_desktop() {
  t12_reset; step s23-desktop-click-menu
  open_file
  hover 1700 900 14
  lclick 1700 900
  expect "File hides on a click on the empty desktop" 'SIG hide menu\[File\]'
  expect_not "no menu item activates" 'SIG activate item' 200
  all_closed "everything is closed"
  open_file
  expect_not "File opens again and stays open" 'SIG hide menu\[File\]' 500
  keys Escape; pause 300; all_closed "closed after Escape"

  t12_reset quiet; step s23-desktop-click-dropdown
  lclick $(center dd-press)
  expect "dropdown shows" 'SIG show menu\[DropPress\]'
  hover 1700 900 14
  rclick 1700 900
  expect "the dropdown hides on a right click on the empty desktop" 'SIG hide menu\[DropPress\]'
  all_closed "everything is closed"

  t12_reset quiet; step s23-desktop-click-combo
  lclick $(center combo)
  expect "combo opens" 'notify::popup-shown combo 1'
  hover 1700 900 14
  lclick 1700 900
  expect "the combo popup closes on a click on the empty desktop" 'notify::popup-shown combo 0'
  all_closed "everything is closed"
  lclick $(center combo)
  expect "the combo opens again" 'notify::popup-shown combo 1'
  keys Escape; pause 300; all_closed "closed after Escape"
}

T12_ALL_SCENARIOS="$T12_ALL_SCENARIOS s23_click_on_desktop"

# The screen and monitor sizes that GTK sees have to be the logical size of the output.  GTK sizes
# and flips menus with them: a size that is too small gives scroll arrows on tall menus.  A
# fractional scale (sway-scale125.conf) is the case that matters: wl_output.scale is 2 there, so
# mode / scale is wrong and the xdg_output logical size has to be used.
s24_monitor_size() {
  t12_reset quiet; step s24-monitor-size
  local m="g$RANDOM$RANDOM" start; start=$(( $(lines) + 1 ))
  app_send geom; app_send "mark $m"
  for _ in $(seq 1 60); do grep -q "MARK .*$m" "$APP_LOG" && break; sleep 0.05; done
  local mon scr ow oh
  mon=$(tail -n +"$start" "$APP_LOG" | payload | grep '^MONITOR 0 ' | head -n 1)
  scr=$(tail -n +"$start" "$APP_LOG" | payload | grep '^SCREEN ' | head -n 1)
  read -r ow oh <<<"$(swaymsg -s "$HARNESS_SWAYSOCK" -t get_outputs -r | jq -r '.[0] | "\(.rect.width) \(.rect.height)"')"
  [ "$mon" = "MONITOR 0 0 0 $ow $oh" ] && ok "monitor 0 is ${ow}x${oh}, the logical size of the output" \
    || fail "monitor 0 is '$mon', the output is ${ow}x${oh} (logical)"
  [ "$scr" = "SCREEN $ow $oh" ] && ok "the screen is ${ow}x${oh}" || fail "the screen is '$scr', the output is ${ow}x${oh}"
}

T12_ALL_SCENARIOS="$T12_ALL_SCENARIOS s24_monitor_size"

# xdg_popup.grab takes the serial of a user event: a button press or a key press.  The serial of
# a release is not valid, and a virtual keyboard (and a quick tap) delivers the release in the same
# batch as the press, before the backend commits the popup.  The test program logs the serial of
# every grab and of every key and button event that reached the backend (the wire log hooks, see
# "wire" in t0_12_menus_seat.c).  The serial of a grab has to be the serial of the latest press
# that came before it, and the popup info hook has to say the same.
wire_read() { # sets W to the WIRE and POPUP lines of the program
  local m="w$RANDOM$RANDOM" start; start=$(( $(lines) + 1 ))
  app_send wire; app_send popups; app_send "mark $m"
  for _ in $(seq 1 60); do grep -q "MARK .*$m" "$APP_LOG" && break; sleep 0.05; done
  W=$(tail -n +"$start" "$APP_LOG" | payload | grep -E '^(WIRE|POPUP) ')
}
grab_serial_check() { # desc: every grab in W carries the serial of the press before it
  local r
  wire_read
  r=$(awk '
    /^WIRE dropped=/ { split($2, a, "="); dropped = a[2] }
    /^WIRE (key|button) serial=/ { split($3, s, "="); split($4, p, "="); if (p[2] == 1) last = s[2]; else rel[s[2]] = 1 }
    /^WIRE grab serial=/ { split($3, s, "="); n++; if (s[2] == last) good++; else bad = bad " " s[2] "(press " last ")"; if (s[2] in rel) bad = bad " " s[2] "(is a release)" }
    END { printf "%d %d %s|%s", n, good, dropped, bad }' <<<"$W")
  local n good rest bad
  read -r n good rest <<<"${r%%|*}"; bad=${r#*|}
  if [ "${n:-0}" -ge 1 ] && [ "$good" = "$n" ] && [ -z "$bad" ]; then ok "$1 ($n grabs, all with a press serial)"
  else fail "$1 (grabs: $n, with the right serial: ${good:-0}; wrong:$bad)"; fi
  # the popup info hook shows the serial that was sent
  local gs ps
  gs=$(grep -E '^WIRE grab' <<<"$W" | tail -n 1 | sed -E 's/.*serial=([0-9]+).*/\1/')
  ps=$(grep -E '^POPUP .*grab_sent=1' <<<"$W" | sed -E 's/.*grab_serial=([0-9]+).*/\1/' | sort -u | tr '\n' ' ')
  if [ -n "$gs" ] && [ "$ps" = "$gs " ]; then ok "the popup info hook shows the serial that was sent ($gs)"
  else fail "the popup info hook shows '$ps', the wire has $gs"; fi
}

s25_grab_serial() {
  t12_reset; step s25-grab-click
  app_send wire-on
  lclick $(center bar:File); expect "File menu shows" 'SIG show menu\[File\]'; pause 400
  grab_serial_check "a menu opened by a click"
  keys Escape; pause 300; all_closed "closed after Escape"

  t12_reset quiet; step s25-grab-tap
  app_send wire-on
  ltap $(center bar:File); expect "File menu shows after a tap" 'SIG show menu\[File\]'; pause 400
  grab_serial_check "a menu opened by a tap (press and release in one batch)"
  keys Escape; pause 300; all_closed "closed after Escape"

  t12_reset quiet; step s25-grab-dropdown
  app_send wire-on
  lclick $(center dd-press); expect "dropdown shows" 'SIG show menu\[DropPress\]'; pause 400
  grab_serial_check "a dropdown opened on the press"
  keys Escape; pause 300; all_closed "closed after Escape"

  # F10 opens the first menu of the menu bar.  The virtual keyboard sends the release of F10 right
  # after the press, so the release has arrived when the popup is committed.
  t12_reset quiet; step s25-grab-key
  app_send wire-on
  keys F10; expect "File menu shows when opened by F10" 'SIG show menu\[File\]' 2000; pause 400
  grab_serial_check "a menu opened by the keyboard (F10)"
  keys Escape; pause 300
  app_send wire-off
  all_closed "closed after Escape"
}

T12_ALL_SCENARIOS="$T12_ALL_SCENARIOS s25_grab_serial"

# A menu goes away while the button that opened it is still down.  The compositor keeps the pointer
# focus on the surface of the press, and the backend aims the events at the popup under the pointer
# (pointer_target).  When that popup is hidden, the target has to move back at once.  Otherwise the
# next events go to a window that is gone and the LEAVE of the crossing goes to it as well.
# Events for a window of the dismissed menu after its UNMAP are stale.
stale_menu_events() { # menu-label: EV lines for windows of that menu after its UNMAP in this step
  since | awk -v m="$1" '
    $0 ~ "^EV +UNMAP .*popup\\[" m "\\]" { f = 1; next }
    f && /^EV +/ && $0 !~ /^EV +(UNMAP|MAP|CONFIGURE|VISIBILITY|WINDOW_STATE)/ && ($0 ~ "popup\\[" m "\\]" || $0 ~ "child:[0-9a-fx]+ " m " " || $0 ~ "item\\[(New|Open Recent|Close|GtkSeparatorMenuItem)\\]") { print }'
}
check_no_stale_events() { # desc menu-label
  local s; s=$(stale_menu_events "$2")
  if [ -z "$s" ]; then ok "$1"; else fail "$1 (stale: $(printf '%s' "$s" | head -n 3 | cut -c1-110 | tr '\n' ';'))"; fi
}

drag_to_new_held() { # opens File by a held press and drags onto New
  hover $(center bar:File)
  press_hold $(center bar:File); expect "press on File shows the menu" 'SIG show menu\[File\]'; refresh_geom
  hovv $(item_center File "New")
  expect "New is selected during the drag" 'SIG select item\[New\]'
}

s26_dismiss_mid_drag() {
  t12_reset; step s26-escape-mid-drag
  drag_to_new_held
  keys Escape
  expect "File hides on Escape with the button down" 'SIG hide menu\[File\]'
  # the first event after the dismissal is a scroll: nothing has moved the target yet
  pause 100; arm_step; "$CLICK" wheel down 1
  expect "a scroll right after Escape reaches the main window" 'EV SCROLL [^ ]+ (main|ctxarea|menubar) '
  arm_step
  hover 330 $(( OY + 100 )) 6
  hover 360 $(( OY + 110 )) 4
  release_held
  pause 300
  check_no_stale_events "no event reaches the hidden menu after Escape, the motion and the release" File
  all_closed "closed after Escape"
  open_file
  hovv $(item_center File "New")
  expect "the menu works again afterwards" 'SIG select item\[New\]'
  keys Escape; pause 300; all_closed "closed again"

  # the compositor ends the popup (popup_done): sway closes the popups of a toplevel that loses the
  # keyboard focus
  t12_reset quiet; step s26-popup-done-mid-drag
  app_send win2; pause 900; place_win2; refresh_geom
  hover $(center ctxarea)
  drag_to_new_held
  local id
  id=$(swaymsg -s "$HARNESS_SWAYSOCK" -t get_tree -r | jq -r '.. | objects | select((.app_id? // "") | test("^[Tt]0_12")) | select(.window_rect.width == 260) | .id' | head -n 1)
  swaymsg -s "$HARNESS_SWAYSOCK" -q "[con_id=$id] focus"
  expect "File hides when the compositor ends the popup" 'SIG hide menu\[File\]' 2000
  pause 100; arm_step; "$CLICK" wheel down 1
  expect "a scroll right after popup_done reaches the main window" 'EV SCROLL [^ ]+ (main|ctxarea|menubar) '
  arm_step
  hover 330 $(( OY + 100 )) 6
  hover 360 $(( OY + 110 )) 4
  release_held
  pause 300
  check_no_stale_events "no event reaches the hidden menu after popup_done, the motion and the release" File
  all_closed "closed after popup_done"
  app_send win2-hide; pause 600
  lclick $(center ctxarea); pause 300
  open_file
  expect_not "the menu stays open after the focus came back" 'SIG hide menu\[File\]' 400
  keys Escape; pause 300; all_closed "closed again"
}

T12_ALL_SCENARIOS="$T12_ALL_SCENARIOS s26_dismiss_mid_drag"

# The parent toplevel loses the keyboard focus right after a menu opened.  Sway then ends the popups
# of that toplevel (view_close_popups).  If that popup_done comes before the first configure, the
# backend must not take it for a refused grab and create the popup again without a grab: that
# leaves a menu on the screen that GTK believes it grabs.  The menu has to close, and the window has
# to work afterwards.  Whether popup_done arrives before the configure depends on timing, so the
# deterministic check of that path is in T0.13 (section D).  Run with the ffm config
# (run-seat-tests.sh --ffm), where the pointer alone moves the focus.
steal_focus() { # focus the second window through the compositor, at once
  local id
  id=$(swaymsg -s "$HARNESS_SWAYSOCK" -t get_tree -r | jq -r '.. | objects | select((.app_id? // "") | test("^[Tt]0_12")) | select(.window_rect.width == 260) | .id' | head -n 1)
  swaymsg -s "$HARNESS_SWAYSOCK" -q "[con_id=$id] focus"
}
s27_focus_change_after_open() {
  t12_reset; step s27-focus-change-after-open
  app_send win2; pause 900; place_win2; refresh_geom
  local i
  for i in 1 2 3; do
    hover $(center bar:File)
    "$CLICK" click $(center bar:File) & local cp=$!
    steal_focus
    wait $cp
    pause 700
    refresh_geom
    if [ ${#MENUWIN[@]} -eq 0 ] && [ "$(popup_toplevels)" = 0 ]; then ok "round $i: no menu is left on the screen"
    else
      # a menu that is open must still be under a working grab: Escape closes it
      keys Escape; pause 400; refresh_geom
      if [ ${#MENUWIN[@]} -eq 0 ] && [ "$(popup_toplevels)" = 0 ]; then ok "round $i: the menu that stayed closes on Escape"
      else fail "round $i: a menu is left on the screen and Escape does not close it (${!MENUWIN[*]})"; lclick 1700 900; pause 400; fi
    fi
    lclick $(center ctxarea); pause 300
  done
  # the toplevels work after the focus changes
  open_file
  expect_not "File stays open on a fresh click" 'SIG hide menu\[File\]' 500
  keys Escape; pause 300; all_closed "closed after Escape"
  app_send win2-hide; pause 600
}

T12_ALL_SCENARIOS="$T12_ALL_SCENARIOS s27_focus_change_after_open"

# A gesture that is faster than the popup: press, drag and release reach the backend before the popup
# the press opened has its first configure.  The popup is only COMMITTED then, and the backend has to
# aim the events at it already (the compositor keeps the focus on the surface of the press, so the
# events come with main surface coordinates).  A busy main loop makes this certain: the program
# blocks for 1200 ms while the menu is shown, the driver moves onto an item and releases 800 ms after
# the press, so the release is well past GTK's 500 ms rule, and the program reads all of it at once.
s28_fast_gesture() {
  t12_reset; step s28-fast-gesture-geometry
  # learn where DropPress 3 will be
  lclick $(center dd-press); expect "dropdown shows" 'SIG show menu\[DropPress\]'; refresh_geom
  local ix iy bx by
  read -r ix iy <<<"$(item_center DropPress "DropPress 3")"
  read -r bx by <<<"$(center dd-press)"
  keys Escape; pause 400; all_closed "closed after Escape"
  hover $bx $by

  step s28-fast-gesture
  app_send "stall-next-show 1200"
  "$CLICK" press $bx $by
  "$CLICK" raw "move $(( (bx + ix) / 2 )) $(( (by + iy) / 2 ))" "sleep 20" "move $ix $iy" "sleep 700" "up 1" "sync $ST/s28.$$" >/dev/null
  expect "DropPress 3 activates when the gesture is read after the popup is committed" 'SIG activate item\[DropPress 3\]' 3000
  expect_not "the release did not go to a main window widget" 'BUTTON_RELEASE [^ ]+ (dd-click|combo|optmenu|tipbtn|ctxarea|main)' 200
  pause 300
  all_closed "closed after the gesture"
  t12_reset quiet
}

T12_ALL_SCENARIOS="$T12_ALL_SCENARIOS s28_fast_gesture"
