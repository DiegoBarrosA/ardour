# t12-scenarios.sh -- the click scenarios of T0.12.  Sourced by run-seat-tests.sh after
# seat-lib.sh.  Each scenario is a function s_NAME.  A scenario opens widgets with the
# virtual pointer and checks the signals that T0.12 logs.
#
# Pointer helpers keep the pointer position in PX PY, so "hover" moves in small steps like a
# mouse does and the compositor sends motion, enter and leave events on the way.
#
# Results on wayland/m11 at 2e81c8dc10 (headless sway 1.12, ffm = focus_follows_mouse):
#   s12-dropdown-drag   FAILS: press on a dropdown button, drag onto an item, release: the item is
#                       not activated and the menu stays open (Ardour: it closes without activating).
#                       Events during the drag go to the main surface and are not mapped to the popup.
#   s17-second-window   FAILS: with a menu open, a click in another toplevel of the same client closes
#                       the menu and is not swallowed.  With ffm the menu closes on hover (popup_done).
# Every other scenario passes with and without ffm and at scale 2.

PX=0; PY=0
hover()  { arm; "$CLICK" path "$PX" "$PY" "$1" "$2" "${3:-8}"; PX=$1; PY=$2; }
# into a menu from the item above it: straight down first, so the path does not cross the next menu bar item
hovv()   { arm; "$CLICK" path "$PX" "$PY" "$PX" "$2" 4; "$CLICK" path "$PX" "$2" "$1" "$2" 6; PX=$1; PY=$2; }
lclick() { hover "$1" "$2"; "$CLICK" click "$1" "$2"; }
rclick() { hover "$1" "$2"; "$CLICK" rclick "$1" "$2"; }
keys()   { arm; "$CLICK" key "$@"; }
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
  "$CLICK" press $(center dd-press); expect "press shows the menu" 'SIG show menu\[DropPress\]'; refresh_geom
  hover $(item_center DropPress "DropPress 3")
  "$CLICK" release
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
  expect_not "File stays open while the pointer is over the second window" 'SIG hide menu\[File\]'
  shot s17-over-second-window
  lclick $(center w2:W2)
  expect "File closes on a click in the second window" 'SIG hide menu\[File\]'
  expect_not "the click does not open the menu of the second window" 'SIG show menu\[W2\]' 500
  shot s17-after-click
  all_closed "everything is closed"
  lclick $(center w2:W2)
  expect "the second window opens its own menu" 'SIG show menu\[W2\]'
  refresh_geom
  shot s17-second-menu
  lclick $(item_center W2 "Other B")
  expect "Other B activates" 'SIG activate item\[Other B\]'
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

T12_ALL_SCENARIOS="$T12_ALL_SCENARIOS s16_pointer_leaves_client s17_second_window s18_keyboard_focus_back"

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
