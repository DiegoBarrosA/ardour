#!/usr/bin/env bash
#
# audit-x11-symbols.sh -- regression guard for the backend dispatch.
#
# Every public gdk_* function must reach the right backend.  A function that
# is defined only by the X11 objects and exported as is would run Xlib code on
# a Wayland display.  This script checks the built objects with nm:
#
#   1. The public IA__gdk_* symbols still defined by the X11 objects are
#      exactly the allowlist below (30 names).  Each one is X11 only on
#      purpose, or backend neutral, or guarded inside the X11 source.
#      Any other name is a FAIL.
#   2. Each of the 94 names dispatched since M6b is exported once from
#      libydk.so, defined as IA__<name> by the dispatcher object
#      (gdkbackend.c), and implemented by both backends as
#      _gdk_x11_<suffix> and _gdk_wayland_<suffix>.
#
# Usage: scripts/audit-x11-symbols.sh [build-dir]    (default: build)
# Set NM to a specific nm binary if nm is not on PATH.
# Exits 0 when the audit passes.

set -uo pipefail

BUILD=${1:-build}

NM=${NM:-$(command -v nm || true)}
if [ -z "$NM" ]; then
  for cand in /nix/store/*-binutils-*/bin/nm; do
    [ -x "$cand" ] && NM=$cand
  done
fi
if [ -z "$NM" ] || [ ! -x "$NM" ]; then
  echo "audit-x11-symbols: nm not found (set NM)" >&2
  exit 2
fi

LIB="$BUILD/libydk.so"
if [ ! -f "$LIB" ]; then
  echo "audit-x11-symbols: $LIB not found, build first" >&2
  exit 2
fi

# --- allowlist: public symbols that stay in the X11 objects ---------------
# 7 type getters (GObject types whose class is X11 specific by design)
# 20 backend neutral (plain struct accessors, GObject refcounting, one
#    empty function, one pure string function)
# 3 guarded inside the X11 source (they return NULL or FALSE for a non X11
#    display; the guard is enough, so they are not dispatched)
ALLOW="
  gdk_colormap_get_type gdk_drag_context_get_type gdk_image_get_type
  gdk_visual_get_type gdk_device_get_type gdk_pixmap_impl_x11_get_type
  gdk_window_impl_x11_get_type
  gdk_drag_context_new gdk_drag_context_ref gdk_drag_context_unref
  gdk_device_free_history gdk_device_get_axis gdk_device_get_axis_use
  gdk_device_get_has_cursor gdk_device_get_key gdk_device_get_mode
  gdk_device_get_name gdk_device_get_n_axes gdk_device_get_n_keys
  gdk_device_get_source gdk_device_set_axis_use gdk_device_set_key
  gdk_device_set_source gdk_device_get_state gdk_device_set_mode
  gdk_set_use_xshm gdk_utf8_to_string_target
  gdk_net_wm_supports gdk_xid_table_lookup gdk_xid_table_lookup_for_display
"

# --- the 94 names dispatched by M6b (suffix after gdk_; the last 8 are the
# --- public gdk_x11_* names, so their X11 half is _gdk_x11_x11_*) ---------
M6B="
  keymap_get_direction keymap_have_bidi_layouts keymap_get_caps_lock_state
  keymap_get_entries_for_keyval keymap_get_entries_for_keycode
  keymap_lookup_key keymap_add_virtual_modifiers
  keymap_map_virtual_modifiers keyval_name keyval_from_name
  keyval_convert_case display_get_name display_supports_shapes
  display_supports_input_shapes display_supports_composite
  display_supports_selection_notification
  display_supports_clipboard_persistence
  display_request_selection_notification display_store_clipboard
  display_get_default_group get_display notify_startup_complete
  notify_startup_complete_with_id add_client_message_filter
  display_add_client_message_filter event_send_client_message_for_display
  event_get_graphics_expose devices_list display_list_devices
  device_get_history input_set_extension_events spawn_on_screen
  spawn_on_screen_with_pipes spawn_command_line_on_screen set_locale
  wcstombs mbstowcs error_trap_push error_trap_pop get_use_xshm
  fontset_load window_set_title window_set_role window_set_startup_id
  window_set_transient_for window_set_type_hint window_get_type_hint
  window_set_modal_hint window_set_skip_taskbar_hint
  window_set_skip_pager_hint window_set_urgency_hint window_set_hints
  window_set_geometry_hints window_focus window_set_accept_focus
  window_set_focus_on_map window_set_override_redirect window_set_icon
  window_set_icon_list window_set_icon_name window_iconify window_deiconify
  window_stick window_unstick window_maximize window_unmaximize
  window_fullscreen window_unfullscreen window_set_keep_above
  window_set_keep_below window_get_group window_set_group
  window_set_decorations window_get_decorations window_set_functions
  window_set_opacity window_begin_resize_drag window_begin_move_drag
  window_get_root_origin window_get_frame_extents
  window_enable_synchronized_configure window_configure_finished
  window_destroy_notify window_foreign_new_for_display
  window_lookup_for_display window_lookup
  x11_display_get_xdisplay x11_drawable_get_xdisplay x11_drawable_get_xid
  x11_window_get_drawable_impl x11_display_set_cursor_theme
  x11_window_set_user_time x11_display_get_user_time x11_get_server_time
"

fail=0
err () { echo "audit-x11-symbols: FAIL: $*" >&2; fail=1; }

shopt -s nullglob
x11_objs=("$BUILD"/ydk/x11/*.o)
wl_objs=("$BUILD"/ydk/wayland/*.o)
bk_objs=("$BUILD"/ydk/gdkbackend.c.*.o)

if [ ${#x11_objs[@]} -eq 0 ] || [ ${#wl_objs[@]} -eq 0 ] || [ ${#bk_objs[@]} -eq 0 ]; then
  echo "audit-x11-symbols: object files missing under $BUILD/ydk" >&2
  exit 2
fi

x11_defs=$("$NM" --defined-only "${x11_objs[@]}" 2>/dev/null | awk '$2 ~ /^[TDBRVW]$/ {print $3}' | sort -u)
wl_defs=$("$NM" --defined-only "${wl_objs[@]}" 2>/dev/null | awk '$2 ~ /^[TDBRVW]$/ {print $3}' | sort -u)
bk_defs=$("$NM" --defined-only "${bk_objs[@]}" 2>/dev/null | awk '$2 ~ /^[TDBRVW]$/ {print $3}' | sort -u)
lib_defs=$("$NM" -D --defined-only "$LIB" 2>/dev/null | awk '$2 ~ /^[TDBRVW]$/ {print $3}')

# Step 1: X11 objects must define exactly the allowlist.
found=$(printf '%s\n' "$x11_defs" | grep '^IA__gdk_' | grep -v '^IA__gdk_x11_' | grep -v '^IA__gdkx_' | sed 's/^IA__//' | sort -u)
want=$(printf '%s\n' $ALLOW | sort -u)

n_found=$(printf '%s\n' "$found" | grep -c .)
n_want=$(printf '%s\n' "$want" | grep -c .)
echo "audit: X11 objects define $n_found public symbols, allowlist has $n_want"

for s in $(comm -23 <(printf '%s\n' "$found") <(printf '%s\n' "$want")); do
  err "X11 object defines public symbol not on the allowlist: $s"
done
for s in $(comm -13 <(printf '%s\n' "$found") <(printf '%s\n' "$want")); do
  err "allowlisted symbol no longer defined by an X11 object (update the allowlist): $s"
done

# Step 2: every dispatched name.
n=0
for suf in $M6B; do
  n=$((n + 1))
  s="gdk_$suf"
  c=$(printf '%s\n' "$lib_defs" | grep -cx "$s")
  [ "$c" -eq 1 ] || err "$s exported $c times from libydk.so (want 1)"
  printf '%s\n' "$bk_defs" | grep -x "IA__$s" >/dev/null || err "dispatcher object does not define IA__$s"
  printf '%s\n' "$x11_defs" | grep -x "_gdk_x11_$suf" >/dev/null || err "no X11 object defines _gdk_x11_$suf"
  printf '%s\n' "$wl_defs" | grep -x "_gdk_wayland_$suf" >/dev/null || err "no Wayland object defines _gdk_wayland_$suf"
  # The X11 object must not export the public name any more.
  printf '%s\n' "$x11_defs" | grep -x "IA__$s" >/dev/null && err "X11 object still defines IA__$s"
done
echo "audit: checked $n dispatched names"
[ "$n" -eq 94 ] || err "expected 94 dispatched names, script lists $n"

if [ "$fail" -ne 0 ]; then
  echo "audit-x11-symbols: FAILED" >&2
  exit 1
fi
echo "audit-x11-symbols: PASS"
