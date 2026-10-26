# ydk-wayland planning

Roadmap for making the Wayland backend in this fork capable of running a
real GTK-ish widget stack (ytk/ytkmm/ztkmm/gtkmm2ext) and, ultimately,
Ardour on a Wayland compositor.

## Background

This tree is a standalone snapshot of the ydk tree (the GDK fork embedded
in Ardour at `libs/tk/ydk`) with a Wayland backend under development in
`ydk/wayland/`.  The plan is to prove the backend here, then merge it back
into the Ardour tree for real Ardour bring-up.

The backend is not GTK3's clean-slate Wayland backend: ydk is a GTK+ 2.x
era codebase whose widget layer (ytk, ~228 files / ~369k lines) draws mostly
through the legacy GdkGC / GdkDrawable / GdkPixmap surface API, not pure
cairo.  Everything below reflects that reality.

## Survey results (what the audit found)

- The GdkWindowImplIface table is complete but roughly half the vfuncs are
  empty or geometry-only (cursors, shapes, grabs, reparent, close, pointer,
  input-window crossings).
- Two dozen `_gdk_windowing_*` internal hooks have no-op Wayland
  implementations (GC clip, gc_copy, pointer at, get_pointer, shapes, beep,
  set_cairo_surface_size).
- Cursors, fonts, selection, properties, and DnD currently run the X11
  public implementations against a Wayland display and crash inside Xlib
  (`gdk_cursor_new_for_display`, `gdk_font_load_for_display`,
  `gdk_selection_*`, `gdk_drag_*`, `gdk_x11_atom_*`).
- Missing Wayland protocols entirely: frame callbacks, wl_data_device,
  pointer cursor protocol, xdg_popup, buffer scale / fractional scale /
  viewporter, touch, key repeat, focus-change synthesis.
- gtkmm2ext is almost pure cairo and small (~10.7k lines); ytk needs the
  full legacy surface layer: GdkPixmap offscreen stores,
  begin/end_paint backing stores, 1-bit depth-1 mask pixmaps, GdkGC wiring,
  and heavy `gdk_draw_line/gdk_draw_rectangle/gdk_draw_pixbuf` use
  (gtkstyle.c alone has ~230 draw_line calls).

## Milestones

Everything before M11 is verifiable in this snapshot via the demo and
`scripts/smoke-test.sh` (X11 baseline + Weston headless phases).  The M11
merge is where it becomes "Ardour on Wayland" for real.

### M4 - X11 public-symbol isolation (crash fix, must be first)

Rename or guard every X11-only public symbol so that `GDK_BACKEND=wayland`
no-ops or dispatches to a safe implementation instead of dereferencing X11
struct fields:

- `gdk_cursor_new_for_display` / `_gdk_cursor_destroy`
- `gdk_font_load_for_display` (and the gdk_text_* / font width functions)
- `gdk_selection_*` (owner_get/set, send_notify, text_property_*)
- `gdk_drag_*` (begin / find_window / motion)
- `gdk_property_*` and `gdk_x11_atom_to_xatom_for_display`

Verification: call each path from the demo under Wayland and observe no
crash.

### M5 - Offscreen drawing core

The rendering layer every ytk redraw depends on.  A real Wayland pixmap
implementation backed by a cairo image surface, with the full
GdkDrawableClass draw surface implemented over cairo:

- `ydk/wayland/gdkpixmap-wayland.c`: `_gdk_pixmap_new`,
  `_gdk_pixmap_impl_get_type`, offscreen pixmap with cairo backing, and
  draw vfuncs (`draw_rectangle`, `draw_line`, `draw_polygon`, `draw_arc`,
  `draw_image`, `draw_drawable`, `draw_pixbuf`).
- `get_image` (read back from shm/backing).
- GC wiring: `_gdk_windowing_gc_set_clip_region`, `_gdk_windowing_gc_copy`,
  clip origin so `gdk_set_clip_region` output is actually clipped.
- Depth-1 mask pixmaps (`gdk_bitmap_create_from_data`) for shape masks and
  drag icons.
- Rename X11's pixmap symbols via gdkwindowing-x11.h; add canonical
  dispatchers in gdkbackend.c (mirrors the `_gdk_events_queue` precedent).

This completes the pixmap path that M3 bypassed by implementing
GdkPaintable on the window impl: the window stays paintable (direct
shm/cairo rendering), while offscreen pixmaps used by widgets get real
implementations.

Verification: demo creates a pixmap, draws via GC ops into it, blits it to
the window, and reads it back with get_image.

### M6 - Input completeness (DONE)

Seat state lives in one place, `GDKWSeat` (`ydk/wayland/ydk/gdkseat-wayland.h`),
and every input event is a heap `GdkEvent` that goes through
`_gdk_windowing_got_event` (the core event engine: child window proxying,
implicit grabs, crossing events, double and triple clicks).

- Keyboard enter/leave produce `GDK_FOCUS_CHANGE`; the keymap really loads
  (`XKB_V1` format, evdev + 8 keycodes); `gdk_keymap_translate_keyboard_state`
  honours its state and group arguments; modifier state comes only from
  `wl_keyboard.modifiers`.
- Key repeat from `repeat_info` (GLib timeout, default 25/400, rate 0
  disables); stops on release, keyboard leave, keymap change, capability loss
  and window destruction; modifier keys never repeat.
- `wl_seat` bound at up to v5; pointer `frame` / `axis_source` /
  `axis_discrete` / `axis_stop`: wheel steps and finger accumulation, both
  axes, kinetic stop resets the remainder.
- Buttons: BTN_LEFT 1, BTN_MIDDLE 2, BTN_RIGHT 3, BTN_SIDE 8, BTN_EXTRA 9,
  BTN_FORWARD 10, BTN_BACK 11, BTN_TASK 12.
- `gdk_window_get_pointer`, `gdk_display_get_pointer` and
  `gdk_display_get_window_at_pointer` report real position and button mask.
- `gdk_pointer_grab` (validates, records; routing is the core's),
  `gdk_display_pointer_ungrab`, `gdk_display_keyboard_ungrab`,
  `gdk_keyboard_grab` (key redirection), `gdk_display_warp_pointer` and
  `gdk_display_beep` (silent no-ops), `gdk_events_pending`; grabs end when
  their window is hidden or destroyed.
- `xdg_toplevel_handle_close` queues `GDK_DELETE`.
- The 15 remaining X11-bound public screen functions are dispatched
  (`gdk_screen_get_width_mm` ... `gdk_screen_get_setting`).
- Fixed on the way: core `gdk_keymap_get_for_display` and
  `gdk_keymap_translate_keyboard_state` used `GDK_*_CLASS (instance)`
  (reads the instance as a class struct, crashed on first call);
  `_gdk_events_queue` blocked in `wl_display_dispatch` and is now
  non-blocking and forwards X11 displays to `_gdk_x11_events_queue`.

Verification: `m6_exercise` in the demo drives the real listeners through
the `_gdk_wayland_test_*` hooks (a headless Weston has no input devices) and
prints `ydk_demo: M6: PASS`; `scripts/smoke-test.sh` requires it.

Known gaps (deliberately left):

- No active pointer grab on Wayland: `confine_to` is recorded but ignored
  (needs `zwp_pointer_constraints`), the grab cursor is only remembered
  (M7), warp is impossible.
- Buttons 8 to 12 are tracked but have no `GDK_BUTTONn_MASK` bit, so the
  core's implicit grab ends when button 1 is released while 8 is still held.
- Only the first `wl_seat`, `wl_output` and monitor are used; touch, tablet,
  `value120` and `axis_relative_direction` (seat v8/v9) are not handled.
- Input-only window crossings (M9) and real `xdg_popup` grabs (M9).
- Everything is verified through hooks; a real compositor may order events
  differently (the pointer `leave` handler ends implicit grabs defensively).

Follow-up audit (M6b) of public symbols that are still X11-owned and cast a
Wayland object to an X11 one:

- Keymap: `gdk_keymap_get_direction`, `_have_bidi_layouts`,
  `_get_caps_lock_state`, `_get_entries_for_keyval`, `_get_entries_for_keycode`,
  `_lookup_key`, `_add_virtual_modifiers`, `_map_virtual_modifiers`,
  `gdk_keyval_name`, `gdk_keyval_from_name` (GtkEntry realize calls
  `get_direction`).
- Display: `gdk_display_get_name` (the class vfunc exists), `_supports_shapes`,
  `_supports_input_shapes`, `_supports_composite`,
  `_supports_selection_notification`, `_supports_clipboard_persistence`,
  `_request_selection_notification`, `_store_clipboard`,
  `_get_default_group`, `gdk_get_display`, `gdk_notify_startup_complete*`,
  the client message filters, `gdk_event_send_client_message_for_display`,
  `gdk_event_get_graphics_expose`.
- Devices: `gdk_devices_list`, `gdk_display_list_devices`, `gdk_device_*`
  accessors, `gdk_input_set_extension_events`.
- Toplevel hints in `gdkwindow-x11.c` (about 55 functions: title, transient,
  type hint, focus, iconify, maximize, fullscreen, icons, geometry hints,
  move/resize drags, frame extents, keep above/below, opacity, role, group,
  `gdk_window_lookup`, foreign windows), `gdk_spawn_on_screen*`,
  `gdk_test_simulate_*`.  Roughly 100 public functions in all.

### M7 - Cursors, shapes, fonts

- `wl_cursor_theme` + `wl_pointer.set_cursor`; real `set_cursor` vfunc.
- `gdk_window_shape_combine_mask` via `wl_surface.set_opaque_region` /
  input region (`wl_region`) with the depth-1 masks from M5.
- a safe font-load path on Wayland (pango-based, no Xlib).

Verification: demo sets named cursors and shapes with no crash.

### M8 - Selection, clipboard, DnD

- bind `wl_data_device_manager`; implement `gdk_selection_*` over
  `wl_data_offer` (needed by ytkmm `selectiondata.cc`, gtk_editable etc).
- `gdk_drag_*` over `wl_data_device`/`wl_source` for text/URI drag
  (gtkdnd.c, tree/icon view drag-reorder).

Verification: demo copies/pastes between two windows and drops a text drag.

### M9 - Popups and non-toplevel windows

- `xdg_popup` + `xdg_positioner` for menus/tooltips/combo (gtkmenu.c,
  menu_shell).
- input-only windows with enter/leave crossing synthesis.
- reparent (subsurface / xdg_popup parenting).

Verification: demo opens a popup that tracks the parent window.

### M10 - Frame sync and scale

- `wl_surface_frame` pacing for redraw throttling; buffer-release handling
  so the compositor does not read a recycled buffer mid-frame.
- `wl_output` v2 scale + `wl_surface_set_buffer_scale` (and fractional
  scale / viewporter beyond that).
- real `_gdk_windowing_set_cairo_surface_size`.

Verification: demo repaint is frame-paced under Weston.

### M11 - Merge and Ardour bring-up

- Move the whole `ydk/wayland/` backend into the Ardour tree under
  `libs/tk/ydk` (and `ydk/wayland/ydk/gdkprivate-wayland.h`).
- Wire `GDK_BACKEND=wayland` into the Ardour-tree gdkbackend.c dispatch
  (currently absent: the tree only compiles x11/quartz/win32).
- Build Ardour with `--with-wayland`; run one `gtk2_ardour` window on
  Weston headless; iterate on real-widget crash fallout (fonts, cursors,
  selection, DnD, gadgets).
- Extend the smoke test to cover actual ytk widget redraw, not just the
  raw-GDK demo.

## Working notes

- Backend dispatch precedent: X11 TUs compile with
  `-include ydk/x11/ydk/gdkwindowing-x11.h`, which macros-renames X11
  globals to `_gdk_x11_*`; gdkbackend.c owns canonical names and forwards
  per `GDK_BACKEND`; Wayland authors `_gdk_wayland_*` natively.  Follow this
  pattern for every newly isolated public symbol.
- The Wayland window impl implements `GdkPaintable` with no-op
  begin/end_paint: its shm-backed cairo surface is already the double
  buffer, so the core skips the implicit-paint pixmap machinery
  (gdkwindow.c `gdk_window_begin_implicit_paint` early-return).  Keep this
  strategy; offscreen pixmaps are implemented separately in M5.
- Both backends are always compiled today (`--without-x11` would fail to
  link via the dispatcher).  Do not weaken this; it is what keeps the X11
  baseline in the smoke test green.