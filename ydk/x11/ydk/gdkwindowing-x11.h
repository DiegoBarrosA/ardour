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

/* ydk-wayland: `_gdk_events_queue()` is the per-display "fetch events from
 * the server into the GDK queue" hook that gdk_display_get_event() calls
 * (gdkdisplay.c).  In GDK2 only X11 defined it; with two backends in one
 * library the X11 definition is renamed here and the Wayland backend owns
 * the canonical symbol. */

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
#define _gdk_events_queue                       _gdk_x11_events_queue

/* ydk-wayland M4: cursor, font, selection, drag and property families.
 * Public names are alias-remapped to IA__ by gdkalias.h, so chain through
 * IA__; internal (_gdk_*) names are renamed directly.  The dispatchers in
 * ydk/gdkbackend.c own the canonical names. */
/* ---- cursor family */
#define IA__gdk_cursor_new_for_display                   _gdk_x11_cursor_new_for_display
#define IA__gdk_cursor_new_from_pixmap                   _gdk_x11_cursor_new_from_pixmap
#define IA__gdk_cursor_new_from_pixbuf                   _gdk_x11_cursor_new_from_pixbuf
#define IA__gdk_cursor_new_from_name                     _gdk_x11_cursor_new_from_name
#define _gdk_cursor_destroy                              _gdk_x11_cursor_destroy
#define IA__gdk_cursor_get_display                       _gdk_x11_cursor_get_display
#define IA__gdk_cursor_get_image                         _gdk_x11_cursor_get_image
#define IA__gdk_display_supports_cursor_alpha            _gdk_x11_display_supports_cursor_alpha
#define IA__gdk_display_supports_cursor_color            _gdk_x11_display_supports_cursor_color
#define IA__gdk_display_get_default_cursor_size          _gdk_x11_display_get_default_cursor_size
#define IA__gdk_display_get_maximal_cursor_size          _gdk_x11_display_get_maximal_cursor_size
/* ---- font family */
#define IA__gdk_font_get_display                         _gdk_x11_font_get_display
#define IA__gdk_font_load_for_display                    _gdk_x11_font_load_for_display
#define IA__gdk_font_from_description_for_display        _gdk_x11_font_from_description_for_display
#define IA__gdk_fontset_load_for_display                 _gdk_x11_fontset_load_for_display
#define _gdk_font_destroy                                _gdk_x11_font_destroy
#define _gdk_font_strlen                                 _gdk_x11_font_strlen
#define IA__gdk_font_id                                  _gdk_x11_font_id
#define IA__gdk_font_equal                               _gdk_x11_font_equal
#define IA__gdk_text_width                               _gdk_x11_text_width
#define IA__gdk_text_width_wc                            _gdk_x11_text_width_wc
#define IA__gdk_text_extents                             _gdk_x11_text_extents
#define IA__gdk_text_extents_wc                          _gdk_x11_text_extents_wc
/* ---- selection family */
#define IA__gdk_selection_owner_set_for_display          _gdk_x11_selection_owner_set_for_display
#define IA__gdk_selection_owner_get_for_display          _gdk_x11_selection_owner_get_for_display
#define IA__gdk_selection_convert                        _gdk_x11_selection_convert
#define IA__gdk_selection_property_get                   _gdk_x11_selection_property_get
#define IA__gdk_selection_send_notify_for_display        _gdk_x11_selection_send_notify_for_display
#define IA__gdk_text_property_to_text_list_for_display   _gdk_x11_text_property_to_text_list_for_display
#define IA__gdk_text_property_to_utf8_list_for_display   _gdk_x11_text_property_to_utf8_list_for_display
#define IA__gdk_free_text_list                           _gdk_x11_free_text_list
#define IA__gdk_free_compound_text                       _gdk_x11_free_compound_text
#define IA__gdk_string_to_compound_text_for_display      _gdk_x11_string_to_compound_text_for_display
#define IA__gdk_utf8_to_compound_text_for_display        _gdk_x11_utf8_to_compound_text_for_display
/* ---- drag family */
#define IA__gdk_drag_begin                               _gdk_x11_drag_begin
#define IA__gdk_drag_get_protocol_for_display            _gdk_x11_drag_get_protocol_for_display
#define IA__gdk_drag_find_window_for_screen              _gdk_x11_drag_find_window_for_screen
#define IA__gdk_drag_motion                              _gdk_x11_drag_motion
#define IA__gdk_drag_drop                                _gdk_x11_drag_drop
#define IA__gdk_drag_abort                               _gdk_x11_drag_abort
#define IA__gdk_drag_status                              _gdk_x11_drag_status
#define IA__gdk_drag_get_selection                       _gdk_x11_drag_get_selection
#define IA__gdk_drag_drop_succeeded                      _gdk_x11_drag_drop_succeeded
#define IA__gdk_drop_reply                               _gdk_x11_drop_reply
#define IA__gdk_drop_finish                              _gdk_x11_drop_finish
#define IA__gdk_window_register_dnd                      _gdk_x11_window_register_dnd
/* ---- property family */
#define IA__gdk_property_get                             _gdk_x11_property_get
#define IA__gdk_property_change                          _gdk_x11_property_change
#define IA__gdk_property_delete                          _gdk_x11_property_delete
#define IA__gdk_atom_intern                              _gdk_x11_atom_intern
#define IA__gdk_atom_intern_static_string                _gdk_x11_atom_intern_static_string
#define IA__gdk_atom_name                                _gdk_x11_atom_name

/* ydk-wayland M5: pixmap, GC-screen and GdkImage families (same convention
 * as M4: public names chain through IA__, internal names are renamed
 * directly; the dispatchers in ydk/gdkbackend.c own the canonical names).
 * gdk_image_get_type () and the image/GC types themselves stay X11-owned. */
#define _gdk_pixmap_impl_get_type                        _gdk_x11_pixmap_impl_get_type
#define _gdk_pixmap_new                                  _gdk_x11_pixmap_new
#define _gdk_bitmap_create_from_data                     _gdk_x11_bitmap_create_from_data
#define _gdk_pixmap_create_from_data                     _gdk_x11_pixmap_create_from_data
#define IA__gdk_pixmap_foreign_new_for_display           _gdk_x11_pixmap_foreign_new_for_display
#define IA__gdk_pixmap_foreign_new_for_screen            _gdk_x11_pixmap_foreign_new_for_screen
#define IA__gdk_pixmap_foreign_new                       _gdk_x11_pixmap_foreign_new
#define IA__gdk_pixmap_lookup                            _gdk_x11_pixmap_lookup
#define IA__gdk_pixmap_lookup_for_display                _gdk_x11_pixmap_lookup_for_display
#define IA__gdk_gc_get_screen                            _gdk_x11_gc_get_screen
#define _gdk_image_new_for_depth                         _gdk_x11_image_new_for_depth
#define IA__gdk_image_new_bitmap                         _gdk_x11_image_new_bitmap
#define IA__gdk_image_get_pixel                          _gdk_x11_image_get_pixel
#define IA__gdk_image_put_pixel                          _gdk_x11_image_put_pixel

/* ---- ydk-wayland F7: colormap family (gdk_colormap_get_type and the
 * gdk_x11_colormap_* accessors stay X11-owned, not dispatched) */
#define IA__gdk_colormap_new                   _gdk_x11_colormap_new
#define IA__gdk_colormap_get_screen            _gdk_x11_colormap_get_screen
#define IA__gdk_colormap_get_system_size       _gdk_x11_colormap_get_system_size
#define IA__gdk_colormap_change                _gdk_x11_colormap_change
#define IA__gdk_colors_alloc                   _gdk_x11_colors_alloc
#define IA__gdk_colors_free                    _gdk_x11_colors_free
#define IA__gdk_colormap_free_colors           _gdk_x11_colormap_free_colors
#define IA__gdk_colormap_alloc_colors          _gdk_x11_colormap_alloc_colors
#define IA__gdk_colormap_query_color           _gdk_x11_colormap_query_color
#define IA__gdk_visual_get_screen              _gdk_x11_visual_get_screen
#define IA__gdk_color_change                   _gdk_x11_color_change


/* ---- ydk-wayland F8: visual query, default colormap and rgba family
 * (gdk_visual_get_type, gdk_x11_screen_lookup_visual, gdkx_visual_get and
 * gdk_x11_visual_get_xvisual stay X11-owned, not dispatched) */
#define IA__gdk_visual_get_best_depth          _gdk_x11_visual_get_best_depth
#define IA__gdk_visual_get_best_type           _gdk_x11_visual_get_best_type
#define IA__gdk_visual_get_best                _gdk_x11_visual_get_best
#define IA__gdk_visual_get_best_with_depth     _gdk_x11_visual_get_best_with_depth
#define IA__gdk_visual_get_best_with_type      _gdk_x11_visual_get_best_with_type
#define IA__gdk_visual_get_best_with_both      _gdk_x11_visual_get_best_with_both
#define IA__gdk_query_depths                   _gdk_x11_query_depths
#define IA__gdk_query_visual_types             _gdk_x11_query_visual_types
#define IA__gdk_screen_list_visuals            _gdk_x11_screen_list_visuals
#define IA__gdk_screen_get_default_colormap    _gdk_x11_screen_get_default_colormap
#define IA__gdk_screen_set_default_colormap    _gdk_x11_screen_set_default_colormap
#define IA__gdk_screen_get_rgba_colormap       _gdk_x11_screen_get_rgba_colormap
#define IA__gdk_screen_get_rgba_visual         _gdk_x11_screen_get_rgba_visual

/* ---- ydk-wayland M6: screen functions that were X11-owned, plus the
 * grab / warp / beep / pending entry points (same convention as M4 and M5:
 * public names chain through IA__, the dispatchers in ydk/gdkbackend.c own
 * the canonical names) */
#define IA__gdk_screen_get_width_mm                     _gdk_x11_screen_get_width_mm
#define IA__gdk_screen_get_height_mm                    _gdk_x11_screen_get_height_mm
#define IA__gdk_screen_get_number                       _gdk_x11_screen_get_number
#define IA__gdk_screen_get_n_monitors                   _gdk_x11_screen_get_n_monitors
#define IA__gdk_screen_get_primary_monitor              _gdk_x11_screen_get_primary_monitor
#define IA__gdk_screen_get_monitor_geometry             _gdk_x11_screen_get_monitor_geometry
#define IA__gdk_screen_get_monitor_width_mm             _gdk_x11_screen_get_monitor_width_mm
#define IA__gdk_screen_get_monitor_height_mm            _gdk_x11_screen_get_monitor_height_mm
#define IA__gdk_screen_get_monitor_plug_name            _gdk_x11_screen_get_monitor_plug_name
#define IA__gdk_screen_is_composited                    _gdk_x11_screen_is_composited
#define IA__gdk_screen_make_display_name                _gdk_x11_screen_make_display_name
#define IA__gdk_screen_get_active_window                _gdk_x11_screen_get_active_window
#define IA__gdk_screen_get_window_stack                 _gdk_x11_screen_get_window_stack
#define IA__gdk_screen_broadcast_client_message         _gdk_x11_screen_broadcast_client_message
#define IA__gdk_screen_get_setting                      _gdk_x11_screen_get_setting
#define IA__gdk_display_pointer_ungrab         _gdk_x11_display_pointer_ungrab
#define IA__gdk_display_keyboard_ungrab        _gdk_x11_display_keyboard_ungrab
#define IA__gdk_keyboard_grab                  _gdk_x11_keyboard_grab
#define IA__gdk_display_warp_pointer           _gdk_x11_display_warp_pointer
#define IA__gdk_display_beep                   _gdk_x11_display_beep
#define IA__gdk_events_pending                 _gdk_x11_events_pending

#endif /* __YDK_GDKWINDOWING_X11_H__ */
