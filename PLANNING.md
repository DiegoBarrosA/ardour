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

### M6 - Input completeness

- keyboard enter/leave -> synthetic GDK_FOCUS_CHANGE events.
- key repeat synthesis from the `repeat_info` event (retro-fitting into the
  GSource dispatch / main context).
- pointer `frame` / `axis_source` events (bind wl_pointer at v5), horizontal
  scroll axis, `stop` for kinetic scrolling.
- GDK button 2/3 mapping fix (`translate_button` currently swaps them).
- real `gdk_window_get_pointer` including modifier mask.
- `_gdk_windowing_pointer_grab` (menus/popups depend on grabs).
- `xdg_toplevel_handle_close` -> GDK_DELETE on the wrapper window.

Verification: demo exercises key, focus, scroll and grab paths.

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