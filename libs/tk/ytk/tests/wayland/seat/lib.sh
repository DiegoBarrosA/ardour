# Shared paths of the seat harness.  Source it.
#   H   directory of these scripts
#   HH  harness home: bin/ (built helper) and state/ (one running sway)
#       default $XDG_CACHE_HOME/ytk-seat-harness, override with HARNESS_HOME
H=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
HH=${HARNESS_HOME:-${XDG_CACHE_HOME:-$HOME/.cache}/ytk-seat-harness}
ST=$HH/state
SHELL_NIX=${YTK_SHELL_NIX:-/home/diego/Notes/Projects/Personal/ydk-wayland/shell.nix}
