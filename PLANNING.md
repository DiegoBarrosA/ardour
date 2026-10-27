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

### M6b - Remaining X11-bound public functions, display hardening (DONE)

Follow-up audit of the public symbols that were still X11-owned and cast a
Wayland object to an X11 one.  Method: `nm` on every `build/ydk/x11/*.o`
(public names are the `IA__gdk_*` symbols, minus `IA__gdk_x11_*` and
`IA__gdkx_*`, which are X11 API by design), `nm -u` on the core and Wayland
objects, then the body of every candidate.  The result is 116 symbols:
**86 dispatched** and **30 that stay in the X11 objects on purpose**.

Dispatched (each is `_gdk_wayland_<name>` plus `_gdk_x11_<name>` behind a
dispatcher in `ydk/gdkbackend.c`):

- Keymap (11, `gdkkeys-wayland.c`): `gdk_keymap_get_direction`,
  `_have_bidi_layouts`, `_get_caps_lock_state`, `_get_entries_for_keyval`,
  `_get_entries_for_keycode`, `_lookup_key`, `_add_virtual_modifiers`,
  `_map_virtual_modifiers`, `gdk_keyval_name`, `gdk_keyval_from_name`,
  `gdk_keyval_convert_case`.
- Display (30, `gdkdisplay-misc-wayland.c`): `gdk_display_get_name`, the
  `supports_*` queries, `request_selection_notification`, `store_clipboard`,
  `get_default_group`, `gdk_get_display`, `gdk_notify_startup_complete*`, the
  client message filters, `gdk_event_send_client_message_for_display`,
  `gdk_event_get_graphics_expose`, `gdk_devices_list`,
  `gdk_display_list_devices`, `gdk_device_get_history`,
  `gdk_input_set_extension_events`, `gdk_spawn_*_on_screen*`,
  `gdk_set_locale`, `gdk_wcstombs`, `gdk_mbstowcs`, `gdk_error_trap_*`,
  `gdk_get_use_xshm`, `gdk_fontset_load`.
- Toplevel hints (45, `gdkwindow-hints-wayland.c`): title, role, startup id,
  transient parent, type hint, modal / skip / urgency hints, size hints,
  focus, icons, iconify, stick, maximize, fullscreen, keep above / below,
  group, decorations, functions, opacity, move and resize drags, root
  origin, frame extents, synchronized configure, `gdk_window_destroy_notify`,
  foreign window and lookup stubs.

X11 handle accessors and API guards (8 more, the public `gdk_x11_*` names,
so they are not part of the 116 above).  ytk reaches them through the
`GDK_DISPLAY_XDISPLAY`, `GDK_WINDOW_XID` and `GDK_WINDOW_XDISPLAY` macros and
from `#ifdef GDK_WINDOWING_X11` blocks that also run on Wayland, because
`gdkconfig.h` defines both windowing macros:

- Handle accessors (`gdkxhandles-wayland.c`): `gdk_x11_display_get_xdisplay`,
  `gdk_x11_drawable_get_xdisplay`, `gdk_x11_drawable_get_xid`,
  `gdk_x11_window_get_drawable_impl`.  On Wayland they give NULL, NULL, 0 and
  the window impl, for every argument including NULL, a pixmap and an unmapped
  window, with no CRITICAL.  A NULL `Display *` still crashes inside Xlib, so
  there is a new public predicate, `gdk_x11_display_is_x11 (display)` (declared
  in `gdkx.h`, defined in `gdkxhandles-wayland.c`; FALSE for NULL and for a
  Wayland display).  Call it before any Xlib call that takes
  `GDK_DISPLAY_XDISPLAY`.
- API guards (`gdkx11-guards-wayland.c`): `gdk_x11_display_set_cursor_theme`
  and `gdk_x11_window_set_user_time` do nothing.  `gdk_x11_display_get_user_time`
  and `gdk_x11_get_server_time` return the time of the newest seat event
  (`GDKWSeat.last_time`), or 0 before any input, for NULL, a pixmap and a
  window of another backend.  There is no server round trip.
- Naming: the X11 half is `_gdk_x11_x11_<name>` (the public name already has
  `x11` in it), the Wayland half `_gdk_wayland_x11_<name>`.  The rename macros
  are the `IA__gdk_x11_*` block at the end of `gdkwindowing-x11.h`.
- Build: these names are declared in `gdkx.h`, so `gdkbackend.c` needs Xlib
  types.  The wscript gives that one object the `X11` uselib and
  `GDK_YDK_DISPATCH_OBJ`, and `gdkaliasdef-decls.h` then force-includes
  `gdkx.h` before the first `gdkalias.h` pass.  Without that pass the alias
  snippet fails with "alias between function and variable is not supported".

Guarded in place in the X11 source, not dispatched (they take a display or an
atom, and they check `GDK_IS_DISPLAY_X11` first):
`gdk_x11_window_foreign_new_for_display` (NULL),
`gdk_x11_atom_to_xatom`, `_for_display` and `gdk_x11_xatom_to_atom`,
`_for_display` (None / GDK_NONE), `gdk_x11_get_xatom_by_name`, `_for_display`
(None), `gdk_x11_get_xatom_name`, `_for_display` (the empty string, because
the documented result is never NULL), and `gdk_x11_free_compound_text`
(`g_free` on the Wayland backend, `XFree` otherwise; it asks
`_gdk_backend_is_wayland ()` because the display may be gone already).

> [!warning] Corrected 2026-10-26
> The note below this list says the 3 in-place guards are in the X11 source
> because `gdkbackend.c` cannot define an exported alias for names declared in
> `gdkx.h`.  That is wrong.  It works when the dispatcher object also gets the
> `gdkx.h` force-include (see Build above).  The 3 stay in place because the
> guard is enough for them, not because dispatch is impossible.

Stays X11-bound, and why (the list is the allowlist in
`scripts/audit-x11-symbols.sh`, which fails on any other name):

- 7 type getters (`gdk_colormap_get_type`, `gdk_drag_context_get_type`,
  `gdk_image_get_type`, `gdk_visual_get_type`, `gdk_device_get_type`,
  `gdk_pixmap_impl_x11_get_type`, `gdk_window_impl_x11_get_type`): GObject
  types whose class is X11 specific by design.
- 20 backend neutral (bodies read, no X11 cast, no Xlib):
  `gdk_drag_context_new/_ref/_unref`, the `gdk_device_*` accessors
  (`free_history`, `get_axis`, `get_axis_use`, `get_has_cursor`, `get_key`,
  `get_mode`, `get_name`, `get_n_axes`, `get_n_keys`, `get_source`,
  `set_axis_use`, `set_key`, `set_source`, `get_state`, `set_mode`),
  `gdk_set_use_xshm` (empty) and `gdk_utf8_to_string_target` (pure).
- 3 guarded inside the X11 source: `gdk_net_wm_supports`,
  `gdk_xid_table_lookup`, `gdk_xid_table_lookup_for_display`.  They are
  declared in `gdkx.h`, and the build only force-includes the `gdkx.h` alias
  declarations into X11 objects, so `gdkbackend.c` cannot define an exported
  alias for them.  Each now returns NULL / FALSE when its display or screen is
  not an X11 one.
- The old global `gdk_display` (`Display *`) and `_xwindow_get_shape`.

Discrepancies with the first audit, found by method:

- `gdk_test_simulate_key` / `_button` are defined nowhere:
  `gdktestutils-x11.c` is not built and `libydk.so` has no `gdk_test_*`
  export.  Nothing to dispatch.  If ytk needs them later, add both backends in
  a new milestone.
- The toplevel hints are 45 functions, not about 55.
- `gdk_keyval_name`, `_from_name` and `_convert_case` are display-less Xlib
  table lookups.  They did not crash, but they are Xlib, so they now use
  xkbcommon.  `convert_case` keeps an exception table to match Xlib 1.8.13 where
  xkbcommon differs (mostly the Latin-2/3/4 range).
- `gdk_spawn_*` children got `DISPLAY=<wayland socket>`, which breaks Xlib
  children.  They now get `WAYLAND_DISPLAY`, from the socket name the display
  really connected to (`GDKWDisplayWayland.socket_name`), and `DISPLAY` is
  left alone.
- After `gdk_set_locale ()` under a non-C locale `gdk_wcstombs` and
  `gdk_mbstowcs` called `GDK_DISPLAY_XDISPLAY` on the default display: a
  crash path on Wayland, and ytk calls `gdk_set_locale` at start.
- `gdk_fontset_load` and `gdk_devices_list` are thin wrappers that called the
  renamed `_gdk_x11_*` function directly, so on Wayland they reached X11 code.

Display hardening (`gdkdisplay-wayland.c`): `dispose` destroys every
toplevel while the display is still valid, finalize walks a copy of
`display->windows`, releases the keymap, and sets
`GDKWScreenWayland.display` of the (leaked) default screen to NULL, so a
`GdkWindow` that outlives its display sees `gdk_screen_get_display () ==
NULL` instead of a dangling pointer.  `gdk_window_wayland_destroy`,
`_gdk_wayland_window_impl_teardown`, hide and the impl finalize work with a
NULL display.  `flush` and `sync` return early without a `wl_display`.

Other fixes on the way:

- `gdk_window_wayland_destroy` now ends pointer, keyboard and seat grabs at
  once (it used to leave an explicit pointer grab alive until the next event).
- `xdg_toplevel_handle_close` builds a heap event.
- Titles and app ids are capped below the 4096 byte Wayland message limit.
- `begin_move_drag` / `begin_resize_drag` need a button that is held now (the
  last `wl_pointer.button` serial may belong to a release).
- Size hints keep the requested values; only the value sent is clamped.
- A transient parent that was not mapped at the child's first configure is
  sent when the parent maps (`_gdk_wayland_window_hints_mapped`).
- Meta / Super / Hyper are found on any level and layout, and Mod1 is
  included in the virtual modifier maps, as in GTK2 on X11 (on a stock pc105
  map the Alt key is `{ Alt_L, Meta_L }`, so Mod1 gains Meta).

Verification:

- `scripts/audit-x11-symbols.sh` (run first by `scripts/smoke-test.sh`): the X11
  objects define exactly the 30 allowlisted public symbols, and each of the
  94 dispatched names (the 86 above and the 8 public `gdk_x11_*` names) is
  exported once, defined as `IA__<name>` by the dispatcher, and implemented by
  both backends.
- `m6b_exercise` in the demo calls all 94 functions through the public API
  (`ydk_demo: M6B: PASS`), plus the gaps of the M6 audit: pointer state with a
  button held, warp, beep, `gdk_events_pending`, destroying windows that hold
  key repeat or grabs, and closing a second display with live windows.  The 8 `gdk_x11_*` names are called on the main window, a child, an unmapped
  toplevel, the root window, a pixmap and NULL (`m6b_x11_api`).  New
  hooks: `_gdk_wayland_test_toplevel_info`, `_gdk_wayland_test_toplevel_configure`,
  `_gdk_wayland_test_keyboard_state`, `_gdk_wayland_test_keyboard_keymap_text`.
- Under X11 the demo calls the dispatched functions too
  (`ydk_demo: M6B-X11: PASS`), which proves the rename map.
- `nix run .#valgrind-demo` (`scripts/valgrind-smoke.sh`) runs the Wayland
  demo under valgrind, separate from the smoke test because it is 20 to 50
  times slower.  `scripts/valgrind.supp` is empty on purpose and documents
  the rules for adding entries.  The run is clean now (0 errors, nothing
  suppressed).  The valgrind step was not run against the old code, so the
  expected findings (read of freed memory in display finalize with a live
  window, the uninitialised `GdkColor` in the demo) are unconfirmed.  What was
  confirmed: with the display `dispose` disabled, the demo's window checks
  in step C5 fail.

Real versus stubbed:

- Real: keymap and keyval functions (xkbcommon), display name and socket name,
  default group, device list, spawn environment, multibyte conversion, title /
  app id / role storage and transmission, transient parent, size hints,
  maximize / fullscreen / minimize requests and their window state events,
  interactive move and resize requests, group, decorations, opacity storage.

> [!warning] Corrected 2026-10-26 (M7)
> Shapes and input shapes were stubbed (FALSE) until M7. They are real now,
> see the M7 section.

- Stubbed and reporting a safe value: selection notification and clipboard persistence (FALSE until M8), composite
  (FALSE), device history, extension events, startup notification, client
  message send and filters, foreign windows and lookups (NULL), icons
  (stored, never sent: xdg-shell has no icon request), type hints, keep
  above / below, sticky, skip taskbar / pager, urgency, modal (stored only),
  override redirect (until M9).
- Not observable on Wayland: `GDK_WINDOW_STATE_ICONIFIED`, `_ABOVE`, `_BELOW`,
  `_STICKY` are never set (the compositor does not report them), and GDK2 has
  no FOCUSED state, so xdg `activated` is only recorded in the test info.

Known gaps (deliberately left, in addition to the M6 list):

- A compositor initiated resize (a maximize answer, a tiling window manager)
  changes the shm buffer size but does not update the `GdkWindow` size and does
  not emit `GDK_CONFIGURE`.  Found while testing maximize; belongs with M10.
- The default screen is leaked on display close (root window, system visual
  and colormap with it).  The fix needs a dispose in `gdkscreen-wayland.c`
  that calls `_gdk_window_destroy (root, TRUE)` like `gdk_screen_x11_dispose`,
  then an unref in display finalize.
- `gdk_window_wayland_hide` commits without attaching a NULL buffer, so a
  hidden window stays visible on a real compositor.
- `wl_registry.global_remove` is empty: hot-unplugged outputs and seats are
  not handled.
- `gdk_window_destroy_notify` unrefs the window like the X11 version.  Nothing
  on Wayland calls it, so it is only safe for a caller that owns that
  reference.
- A child keeps `parent_sent` set if its parent's toplevel is torn down and
  the parent window is shown again (cannot happen today: a destroyed window
  is not shown again).
- Raw `seat->grab.confine_to` needs a weak pointer. This was due before M7 and
  is still open after it.
- `drag_sent` of the move / resize hook depends on a `wl_seat` being present,
  which the headless Weston does not offer, so the demo does not assert it.
- Everything is verified against Weston headless and the test hooks; no real
  seat, tablet or multi-output compositor has been tried.

### M7 - Cursors, shapes, fonts (DONE)

Contract header: `ydk/wayland/ydk/gdkm7-wayland.h` (installed; its public
section holds the test hooks). Three pieces, no file shared between them.

- Cursors (`gdkcursor-wayland.c`): `wl_cursor_theme_load` on first use, theme
  from `XCURSOR_THEME`, size from `XCURSOR_SIZE` (1 to 512, else 24), changed by
  `gdk_x11_display_set_cursor_theme`. Resolution order: blank cursor hides the
  pointer, pixmap and pixbuf cursors own a shm buffer, typed and named cursors
  come from the theme (primary name, then aliases such as `ew-resize`), then
  `left_ptr` or `default`, and last a built in 1x1 transparent buffer.
  `wl_pointer.set_cursor` is sent on every `wl_pointer.enter` (new serial), from
  the `set_cursor` vfunc while the window has pointer focus, when an explicit
  grab starts or ends, and after a theme change. `gdk_cursor_get_image` returns
  a pixbuf for custom cursors only.
- Shapes (`gdkshape-wayland.c`): the bounding shape B clears backing pixels
  outside B on every present and, for depth 24 windows, becomes the opaque
  region. The effective input region is B intersect I, or whichever exists.
  Requests are sent only when the region changed; a shape set before `show` is
  sent before the first commit. `supports_shapes` and `supports_input_shapes`
  are TRUE.
- Fonts (`gdkfont-wayland.c`, `gdkdrawable-wayland.c`): `gdk_draw_text` and
  `gdk_draw_text_wc` draw through Pango with fixed options (96 dpi, gray
  antialias, slight hinting, hint metrics on, round glyph positions), so
  measuring equals drawing. Depth 1 targets draw the glyph path, so edges are
  exact.
- Build: `wayland-cursor` is a new dependency (`WAYLAND-CURSOR` uselib for the
  wayland objects and `libydk`); nix needs no new package. The smoke test and
  valgrind scripts export `FONTCONFIG_FILE` (DejaVu through `makeFontsConf`) and
  `XDG_CACHE_HOME` under the temp dir.

Verification: `m7_exercise` in the demo (headless, prints `ydk_demo: M7: PASS`;
`scripts/smoke-test.sh` and `scripts/valgrind-smoke.sh` require it). Cursors
use a generated three file Xcursor theme and the cursor info hook (a headless
Weston has no seat, so nothing reaches a real `wl_pointer`). Shapes are read
back as the regions sent and as backing pixels. Fonts are measured by drawing
into pixmaps and comparing the ink box with the extents.

Known gaps (deliberately left, in addition to the M6 and M6b lists):

- No cursor animation: only image 0 of a theme cursor is used.
- No built in arrow: if no theme is usable the pointer is invisible (1x1
  transparent buffer, logged once with `g_debug`).
- No scale: cursor size and buffer scale are 1 (M10 multiplies them).
- The opaque region is sent only for shaped depth 24 windows. Unshaped windows
  send none (follow up).
- Theme images are not readable by `gdk_cursor_get_image` (libwayland-cursor
  hides the pixels), so it returns NULL for theme, typed and blank cursors.
- A cursor set while the pointer is outside the window is only remembered by
  the vfunc when the core calls it (it does not call it from outside). On
  `wl_pointer.enter` the last resolved cursor is sent first and the core's
  crossing handling then re-sends the right one (same serial, so one or two
  `set_cursor` requests).
- `_gdk_wayland_cursor_theme_set` with a NULL theme and size <= 0 still reloads
  the theme (no leak, no crash).
- Verified only against headless Weston and the hooks; the real
  `wl_pointer.set_cursor` request has not been seen by a compositor.

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