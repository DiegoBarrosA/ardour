# seat-lib.sh -- helpers to drive a test program on the harness sway.  Source it.
#
# Needs the harness running (start-sway.sh) and client-env.sh sourced for the clients.
# Functions:
#   app_start LOG TRACE CMD...   run CMD with a stdin FIFO; stdout goes to LOG, stderr to TRACE
#   app_send TEXT                write one command line to the program
#   app_stop                     send "quit" and wait
#   step NAME                    start a step: the checks below only look at the log after this point
#   expect DESC REGEX [MS]       PASS when a log line matches REGEX within MS (default 1500)
#   expect_not DESC REGEX [MS]   wait MS (default 0), PASS when no log line matches
#   expect_count DESC REGEX N    PASS when exactly N log lines match
#   refresh_geom                 ask the program for its geometry (T0.12 "geom" command)
#   center NAME                  global x y of the centre of a named widget (needs refresh_geom)
#   item_center MENU LABEL       global x y of the centre of a menu item
#   menu_open LABEL              true when the last refresh_geom saw the menu mapped
#   shot NAME                    screenshot to $SHOTS/NAME.png (cropped to the window area)
# The log lines are "T0.12: [  1.234] HH:MM:SS.uuuuuu KIND text".

SEATDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
APP_LOG=; APP_FIFO=; APP_PID=; HOLD_PID=; L0=1; STEP=
NPASS=0; NFAIL=0; FAILS=()
SHOTS=${SHOTS:-/tmp}
declare -A GEO MITEM MENUWIN
GEO_N=0

say() { printf '%s\n' "$*"; }

app_start() {
  APP_LOG=$1; APP_TRACE=$2; shift 2
  APP_FIFO=$ST/app.fifo
  rm -f "$APP_FIFO"; mkfifo "$APP_FIFO"
  : > "$APP_LOG"
  ( exec sleep 100000 > "$APP_FIFO" ) & HOLD_PID=$!
  "$@" < "$APP_FIFO" > "$APP_LOG" 2> "$APP_TRACE" & APP_PID=$!
  for _ in $(seq 1 100); do grep -q "T0.12: ready" "$APP_LOG" 2>/dev/null && return 0; kill -0 "$APP_PID" 2>/dev/null || return 1; sleep 0.1; done
  return 1
}

app_send() { printf '%s\n' "$1" > "$APP_FIFO"; }

app_stop() {
  [ -n "$APP_PID" ] || return 0
  app_send quit
  for _ in $(seq 1 60); do kill -0 "$APP_PID" 2>/dev/null || break; sleep 0.1; done
  kill -KILL "$APP_PID" 2>/dev/null
  wait "$APP_PID" 2>/dev/null; APP_RC=$?
  kill "$HOLD_PID" 2>/dev/null; wait "$HOLD_PID" 2>/dev/null
  APP_PID=
}

lines() { wc -l < "$APP_LOG"; }
payload() { sed -E 's/^T0\.12: \[ *[0-9.]+\] [0-9:.]+ +//; s/^([A-Z_]+) +/\1 /'; }
since() { tail -n +"$L0" "$APP_LOG" | payload; }

arm() { L0=$(( $(lines) + 1 )); }
arm_step() { L0=$S0; }
step() { STEP=$1; app_send "mark step $1"; arm; S0=$L0; say "-- step $1"; }

ok()   { NPASS=$((NPASS+1)); say "   ok:     $1"; }
fail() { NFAIL=$((NFAIL+1)); FAILS+=("$STEP: $1"); say "   FAILED: $1"; }

expect() { # desc regex [ms]
  local end=$(( $(date +%s%N) / 1000000 + ${3:-1500} ))
  while :; do
    if since | grep -E "$2" >/dev/null; then ok "$1"; return 0; fi
    [ $(( $(date +%s%N) / 1000000 )) -ge $end ] && break
    sleep 0.05
  done
  fail "$1 (no match for: $2)"; return 1
}

expect_not() { # desc regex [ms]
  sleep "$(awk "BEGIN{print ${3:-0}/1000}")"
  if since | grep -E "$2" >/dev/null; then fail "$1 (unexpected: $(since | grep -E "$2" | head -n 2 | tr '\n' ';'))"; return 1; fi
  ok "$1"
}

expect_count() { # desc regex n
  local n; n=$(since | grep -cE "$2")
  if [ "$n" = "$3" ]; then ok "$1"; else fail "$1 (got $n, want $3, regex: $2)"; fi
}

win_origin() { # global origin of the main window (width != 260) and of the second one (width 260), from sway
  swaymsg -s "$HARNESS_SWAYSOCK" -t get_tree -r |
    jq -r '[.. | objects | select((.app_id? // "") | test("^[Tt]0_12")) | {x: (.rect.x + .window_rect.x), y: (.rect.y + .window_rect.y), w: .window_rect.width}] |
           (map(select(.w != 260)) | first // {x: 0, y: 0}) as $a | (map(select(.w == 260)) | first // {x: 0, y: 0}) as $b |
           "\($a.x) \($a.y) \($b.x) \($b.y)"' 2>/dev/null
}

refresh_geom() {
  local m="g$RANDOM$RANDOM" start; start=$(( $(lines) + 1 ))
  app_send geom; app_send "mark $m"
  for _ in $(seq 1 60); do grep -q "MARK .*$m" "$APP_LOG" && break; sleep 0.05; done
  GEO=(); MITEM=(); MENUWIN=(); TOPS=()
  read -r OX OY O2X O2Y <<<"$(win_origin)"
  local line k a b c d e
  while IFS= read -r line; do
    case "$line" in
      GEOM\ main*) ;;
      GEOM\ *) read -r _ k a b c d <<<"$line"; GEO[$k]="$a $b $c $d" ;;
      MENU\ *) if [[ $line =~ ^MENU\ (.*)\ window\ at\ (-?[0-9]+),(-?[0-9]+)\ size\ ([0-9]+)x([0-9]+)\ root=(main|win2)$ ]]; then
                 MENUWIN[${BASH_REMATCH[1]}]="${BASH_REMATCH[2]} ${BASH_REMATCH[3]} ${BASH_REMATCH[4]} ${BASH_REMATCH[5]} ${BASH_REMATCH[6]}"; fi ;;
      MITEM\ *) if [[ $line =~ ^MITEM\ (.*)\ \|\ (.*)\ (-?[0-9]+)\ (-?[0-9]+)\ ([0-9]+)\ ([0-9]+)\ root=(main|win2)$ ]]; then
                 MITEM["${BASH_REMATCH[1]}|${BASH_REMATCH[2]}"]="${BASH_REMATCH[3]} ${BASH_REMATCH[4]} ${BASH_REMATCH[5]} ${BASH_REMATCH[6]} ${BASH_REMATCH[7]}"; fi ;;
      TOPLEVEL\ *) TOPS+=("$line") ;;
    esac
  done < <(tail -n +"$start" "$APP_LOG" | payload)
}

_c() { # "x y w h root" or "x y w h": global centre
  local x y w h r ox=$OX oy=$OY; read -r x y w h r <<<"$1"
  [ "${r:-main}" = win2 ] && { ox=$O2X; oy=$O2Y; }
  echo "$(( ox + x + w / 2 )) $(( oy + y + h / 2 ))"
}
center() { [ -n "${GEO[$1]:-}" ] || { echo "center: no widget $1" >&2; echo "0 0"; return 1; }; case "$1" in w2:*) _c "${GEO[$1]} win2" ;; *) _c "${GEO[$1]}" ;; esac; }
item_center() { [ -n "${MITEM[$1|$2]:-}" ] || { echo "item_center: no item $1|$2" >&2; echo "0 0"; return 1; }; _c "${MITEM[$1|$2]}"; }
menu_open() { [ -n "${MENUWIN[$1]:-}" ]; }
popup_toplevels() { printf "%s\n" "${TOPS[@]:-}" | grep -v " main type=0" | grep -v " win2 type=0" | grep -c TOPLEVEL ; }

shot() { "$SEATDIR/shot.sh" "$SHOTS/$1.png" "${SHOT_GEOM:-0,0 1000x700}" >/dev/null 2>&1; }

CLICK="$SEATDIR/click.sh"
