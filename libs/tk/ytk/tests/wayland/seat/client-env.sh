# Source this to run a Wayland client against the harness sway.  It refuses to leave
# the harness runtime dir, so a client can never reach the login session.
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
[ -f "$ST/env.sh" ] || { echo "client-env.sh: harness sway not running, run start-sway.sh" >&2; return 1; }
. "$ST/env.sh"
export XDG_RUNTIME_DIR=$HARNESS_RT
export WAYLAND_DISPLAY=$HARNESS_WAYLAND_DISPLAY
export SWAYSOCK=$HARNESS_SWAYSOCK
unset DISPLAY I3SOCK DBUS_SESSION_BUS_ADDRESS
export GDK_BACKEND=wayland
case "$WAYLAND_DISPLAY" in "$ST"/rt/*) ;; *) echo "client-env.sh: unexpected WAYLAND_DISPLAY $WAYLAND_DISPLAY" >&2; return 1 ;; esac
export FONTCONFIG_FILE=$HARNESS_RT/fonts.conf
CURDIR=$(ls -d /nix/store/*-adwaita-icon-theme-*/share/icons 2>/dev/null | head -n 1)
[ -n "$CURDIR" ] && export XCURSOR_PATH="$CURDIR" XCURSOR_THEME=Adwaita
