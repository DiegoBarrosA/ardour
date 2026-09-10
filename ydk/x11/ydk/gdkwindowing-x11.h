/*
 * gdkwindowing-x11.h
 *
 * ydk-wayland fork: namespacing of the X11 backend's windowing hooks.
 *
 * GDK2 was architected for a single compiled-in backend: both the internal
 * `_gdk_windowing_*` contract and a handful of public functions
 * (`gdk_display_open`, ...) are defined per backend and share one symbol
 * name.  This project compiles both the X11 and the Wayland backend into a
 * single libydk (see README.md / DESIGN.md), so the per-backend definitions
 * must not collide.
 *
 * The X11 backend compiles with this header force-included (`-include`),
 * which renames every symbol it defines to a backend-specific
 * `_gdk_x11_*` name.  The surrounding dispatcher in ydk/gdkbackend.c then
 * owns the original names and forwards to the backend selected at runtime.
 *
 * Only symbols actually defined in the X11 backend are listed.  Anything
 * this header leaves alone keeps its name: shared/core routines
 * (`_gdk_windowing_got_event` in gdkwindow.c, the internal-only
 * `_gdk_windowing_image_init`/`_gdk_windowing_window_init` pair is renamed
 * below with the rest) and the per-backend PUBLIC surface (gdk_window_*,
 * gdk_screen_*, ...) which currently remains owned by the X11 backend.
 */

#ifndef __YDK_GDKWINDOWING_X11_H__
#define __YDK_GDKWINDOWING_X11_H__

/* internal windowing contract (gdkinternals.h)
 *
 * `_gdk_windowing_args` is deliberately NOT renamed: gdk.c passes it to
 * g_option_group_add_entries() at parse time, before any backend is
 * selected, so the merged GOptionEntry table must be a single symbol.
 * The X11 backend owns it for now (it is the only backend with options);
 * the Wayland backend exports its own (empty) `_gdk_wayland_windowing_args`
 * for the post-selection merge planned with M1. */
#define _gdk_windowing_init                      _gdk_x11_windowing_init
#define _gdk_windowing_exit                      _gdk_x11_windowing_exit
#define _gdk_windowing_set_default_display       _gdk_x11_windowing_set_default_display
#define _gdk_windowing_substitute_screen_number  _gdk_x11_windowing_substitute_screen_number
#define _gdk_windowing_window_get_next_serial    _gdk_x11_windowing_window_get_next_serial
#define _gdk_windowing_window_get_shape          _gdk_x11_windowing_window_get_shape
#define _gdk_windowing_window_get_input_shape    _gdk_x11_windowing_window_get_input_shape
#define _gdk_windowing_get_shape_for_mask        _gdk_x11_windowing_get_shape_for_mask
#define _gdk_windowing_window_beep               _gdk_x11_windowing_window_beep
#define _gdk_windowing_get_pointer               _gdk_x11_windowing_get_pointer
#define _gdk_windowing_window_at_pointer         _gdk_x11_windowing_window_at_pointer
#define _gdk_windowing_pointer_grab              _gdk_x11_windowing_pointer_grab
#define _gdk_windowing_window_process_updates_recurse _gdk_x11_windowing_window_process_updates_recurse
#define _gdk_windowing_before_process_all_updates _gdk_x11_windowing_before_process_all_updates
#define _gdk_windowing_after_process_all_updates  _gdk_x11_windowing_after_process_all_updates
#define _gdk_windowing_get_bits_for_depth        _gdk_x11_windowing_get_bits_for_depth
#define _gdk_windowing_window_destroy_foreign    _gdk_x11_windowing_window_destroy_foreign
#define _gdk_windowing_display_set_sm_client_id  _gdk_x11_windowing_display_set_sm_client_id
#define _gdk_windowing_window_set_composited     _gdk_x11_windowing_window_set_composited
#define _gdk_windowing_gc_set_clip_region        _gdk_x11_windowing_gc_set_clip_region
#define _gdk_windowing_gc_copy                   _gdk_x11_windowing_gc_copy
#define _gdk_windowing_get_startup_notify_id     _gdk_x11_windowing_get_startup_notify_id
#define _gdk_windowing_launch_failed             _gdk_x11_windowing_launch_failed
#define _gdk_windowing_set_cairo_surface_size    _gdk_x11_windowing_set_cairo_surface_size
#define _gdk_windowing_create_cairo_surface      _gdk_x11_windowing_create_cairo_surface
#define _gdk_windowing_image_init                _gdk_x11_windowing_image_init
#define _gdk_windowing_window_init               _gdk_x11_windowing_window_init
#define _gdk_window_impl_new                     _gdk_x11_window_impl_new

/* ydk-wayland: `_gdk_window_impl_new()` is per-backend in GTK2 (the core
 * declares it in gdkinternals.h and each backend defines the one symbol).
 * With both backends in one lib the X11 definition is renamed here and a
 * canonical dispatcher is provided by ydk/gdkbackend.c. */

/* public functions that are per-backend in GDK2
 *
 * gdk_display_open is special: gdkalias.h remaps gdk_display_open to
 * IA__gdk_display_open inside every TU and gdkaliasdef.c emits the exported
 * `gdk_display_open` as an alias of IA__gdk_display_open.  So the rename
 * must chain through the intermediate token: in x11 TUs the definition
 * `gdk_display_open` expands gdkalias.h-first to IA__gdk_display_open and
 * then, via this macro, to _gdk_x11_display_open.  The dispatcher
 * (ydk/gdkbackend.c) defines IA__gdk_display_open itself, so the public
 * alias resolves to whichever backend is active at runtime.
 */
#define IA__gdk_display_open                    _gdk_x11_display_open

#endif /* __YDK_GDKWINDOWING_X11_H__ */