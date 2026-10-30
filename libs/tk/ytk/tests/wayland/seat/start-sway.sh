#!/usr/bin/env bash
#
# start-sway.sh -- starts a SEPARATE headless sway with a virtual pointer and keyboard.
#
# The sway has its own XDG_RUNTIME_DIR and sockets under $HARNESS_HOME/state and
# never touches the sway of the login session.  Its output is 1920x1080 (the config can say otherwise with a comment line "# harness-layout: W H", the size of the whole layout).  A small
# client (seat-input) gives the seat a pointer and a keyboard, because a headless
# sway has no input device and a client would see wl_seat.capabilities(0).
#
# usage: start-sway.sh        env: HARNESS_HOME, HARNESS_SWAY_CONF, YTK_SHELL_NIX
# Writes $HARNESS_HOME/state/env.sh, which the other scripts read.
# Stop it with stop-sway.sh.
set -u
. "$(dirname "$0")/lib.sh"

if [ -f "$ST/env.sh" ] && . "$ST/env.sh" && kill -0 "$SWAY_PID" 2>/dev/null; then
  echo "start-sway.sh: already running, pid $SWAY_PID"; exit 0
fi

# build the input helper once (needs wayland-scanner, gcc, libxkbcommon: shell.nix)
BIN=$HH/bin/seat-input
if [ ! -x "$BIN" ] || [ "$H/seat-input.c" -nt "$BIN" ]; then
  mkdir -p "$HH/bin" "$HH/gen"
  if ! command -v wayland-scanner >/dev/null 2>&1; then
    exec nix-shell "$SHELL_NIX" --run "$(printf '%q ' "$0" "$@")"
  fi
  for p in virtual-keyboard-unstable-v1 wlr-virtual-pointer-unstable-v1; do
    wayland-scanner client-header "$H/$p.xml" "$HH/gen/$p-client-protocol.h" &&
    wayland-scanner private-code "$H/$p.xml" "$HH/gen/$p-protocol.c" || exit 1
  done
  # shellcheck disable=SC2046
  gcc -O1 -Wall -I"$HH/gen" "$H/seat-input.c" "$HH"/gen/*-protocol.c -o "$BIN" \
    $(pkg-config --cflags --libs wayland-client xkbcommon) || { echo "start-sway.sh: cannot build seat-input" >&2; exit 1; }
fi

rm -rf "$ST"; mkdir -p "$ST"
RT=$ST/rt; mkdir -p "$RT"; chmod 700 "$RT"
CONF=${HARNESS_SWAY_CONF:-$H/sway.conf}
# the virtual pointer maps absolute positions onto the whole layout: its size is in the config
LAYOUT=$(sed -n 's/^# harness-layout: *//p' "$CONF" | head -n 1)
LAYOUT=${LAYOUT:-1920 1080}
# a config with more than one output says so: the headless backend makes that many outputs
NOUT=$(sed -n 's/^# harness-outputs: *//p' "$CONF" | head -n 1)
FONTDIR=$(ls -d /nix/store/*-dejavu-fonts-*/share/fonts 2>/dev/null | head -n 1)
cat > "$RT/fonts.conf" <<FC
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<fontconfig><dir>$FONTDIR</dir><cachedir>$RT/fccache</cachedir></fontconfig>
FC
(
  # a clean environment: nothing that points at the login session
  unset WAYLAND_DISPLAY DISPLAY SWAYSOCK I3SOCK WAYLAND_SOCKET DBUS_SESSION_BUS_ADDRESS
  export XDG_RUNTIME_DIR=$RT
  export WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman
  [ -n "$NOUT" ] && export WLR_HEADLESS_OUTPUTS=$NOUT
  export FONTCONFIG_FILE=$RT/fonts.conf
  cd "$ST"
  exec setsid sway -c "$CONF" > "$ST/sway.log" 2>&1
) &
for _ in $(seq 1 100); do
  ls "$RT"/sway-ipc.*.sock >/dev/null 2>&1 && ls "$RT"/wayland-*.lock >/dev/null 2>&1 && break
  sleep 0.1
done
SOCK=$(ls "$RT"/sway-ipc.*.sock 2>/dev/null | head -n 1)
PID=$(basename "${SOCK:-x.x.x.x}" .sock | awk -F. '{print $3}')
WD=$(ls "$RT"/wayland-* 2>/dev/null | grep -v '\.lock$' | head -n 1)
if [ -z "$SOCK" ] || [ -z "$WD" ]; then
  echo "start-sway.sh: sway did not start"; cat "$ST/sway.log"; exit 1
fi
# The socket is named wayland-1 like the login session's one, but it lives in our own
# runtime dir.  Clients get the absolute path, so they can never connect to the wrong one.
mkfifo "$ST/input.fifo"
(
  unset DISPLAY DBUS_SESSION_BUS_ADDRESS
  export XDG_RUNTIME_DIR=$RT WAYLAND_DISPLAY=$WD
  exec "$BIN" "$ST/input.fifo" $LAYOUT > "$ST/seat-input.log" 2>&1
) &
for _ in $(seq 1 50); do grep -q ready "$ST/seat-input.log" 2>/dev/null && break; sleep 0.1; done
grep -q ready "$ST/seat-input.log" || { echo "start-sway.sh: seat-input failed"; cat "$ST/seat-input.log"; exit 1; }
SPID=$(pgrep -u "$(id -u)" -f "^$BIN $ST/input.fifo" | head -n 1)
cat > "$ST/env.sh" <<ENV
SWAY_PID=$PID
SEAT_INPUT_PID=$SPID
HARNESS_RT=$RT
HARNESS_SWAYSOCK=$SOCK
HARNESS_WAYLAND_DISPLAY=$WD
ENV
echo "start-sway.sh: sway pid=$PID seat-input pid=${SPID:-?}"
echo "  XDG_RUNTIME_DIR=$RT"
echo "  WAYLAND_DISPLAY=$WD"
echo "  SWAYSOCK=$SOCK"
swaymsg -s "$SOCK" -t get_outputs -r | grep -E '"name"|"width"|"height"' | head -3
