/*
 * gdkbackend.c
 *
 * ydk-wayland fork: backend dispatcher.
 *
 * GDK2's internal windowing contract (`_gdk_windowing_*`) and the public
 * `gdk_display_open` are defined per backend and originally shared one
 * symbol name.  This project compiles two backends (X11 and Wayland) into
 * one libydk, so the X11 backend compiles under a rename map
 * (x11/ydk/gdkwindowing-x11.h, force-included via -include) that
 * namespace-stamps its symbols `_gdk_x11_*`; the Wayland backend authors
 * the same contract natively as `_gdk_wayland_*`.
 *
 * This file owns the canonical names and forwards to the backend that
 * GDK_BACKEND selects.  Backend selection mirrors GDK3 semantics:
 *   - GDK_BACKEND "x11" or "wayland" forces a backend (unknown values
 *     warn and fall through to the default);
 *   - otherwise, WAYLAND_DISPLAY in the environment selects Wayland;
 *   - the X11 backend is the fallback default.
 *
 * `_gdk_windowing_args` stays with the X11 backend: gdk.c consumes it at
 * parse time before any backend exists, so a merged option table must be a
 * single definition; see gdkwindowing-x11.h.
 */

#include "config.h"

#include <string.h>

#include <ydk/gdk.h>

#include "gdkprivate-wayland.h"
#include "gdkpixmap-wayland.h"

#ifdef GDK_COMPILATION
#include "gdkalias.h"
#endif

/* X11 symbols as produced by the rename map.  gdkglobals-x11.c owns the
 * shared `_gdk_synchronize` backing the --sync entry in
 * `_gdk_windowing_args` (kept per-backend; see gdkwindowing-x11.h). */
extern void        _gdk_x11_windowing_init (void);
extern void        _gdk_x11_windowing_exit (void);
extern void        _gdk_x11_windowing_set_default_display (GdkDisplay *display);
extern gchar      *_gdk_x11_windowing_substitute_screen_number (const gchar *display_name,
								gint screen_number);
extern gulong      _gdk_x11_windowing_window_get_next_serial (GdkDisplay *display);
extern GdkRegion  *_gdk_x11_windowing_window_get_shape (GdkWindow *window);
extern GdkRegion  *_gdk_x11_windowing_window_get_input_shape (GdkWindow *window);
extern GdkRegion  *_gdk_x11_windowing_get_shape_for_mask (GdkBitmap *mask);
extern void        _gdk_x11_windowing_window_beep (GdkWindow *window);
extern void        _gdk_x11_windowing_get_pointer (GdkDisplay *display,
						   GdkScreen **screen,
						   gint *x, gint *y,
						   GdkModifierType *mask);
extern GdkWindow  *_gdk_x11_windowing_window_at_pointer (GdkDisplay *display,
							 gint *win_x, gint *win_y,
							 GdkModifierType *mask,
							 gboolean get_toplevel);
extern GdkGrabStatus _gdk_x11_windowing_pointer_grab (GdkWindow *window,
						      GdkWindow *native,
						      gboolean owner_events,
						      GdkEventMask event_mask,
						      GdkWindow *confine_to,
						      GdkCursor *cursor,
						      guint32 time_);
extern void        _gdk_x11_windowing_window_process_updates_recurse (GdkWindow *window,
								      GdkRegion *expose_region);
extern void        _gdk_x11_windowing_before_process_all_updates (void);
extern void        _gdk_x11_windowing_after_process_all_updates (void);
extern gint        _gdk_x11_windowing_get_bits_for_depth (GdkDisplay *display,
							  gint depth);
extern void        _gdk_x11_windowing_window_destroy_foreign (GdkWindow *window);
extern void        _gdk_x11_windowing_display_set_sm_client_id (GdkDisplay *display,
								const gchar *sm_client_id);
extern void        _gdk_x11_windowing_window_set_composited (GdkWindow *window,
							     gboolean composited);
extern void        _gdk_x11_windowing_gc_set_clip_region (GdkGC *gc,
							  const GdkRegion *region,
							  gboolean reset_origin);
extern void        _gdk_x11_windowing_gc_copy (GdkGC *dst_gc, GdkGC *src_gc);
extern char       *_gdk_x11_windowing_get_startup_notify_id (GAppLaunchContext *context,
							     GAppInfo *info,
							     GList *files);
extern void        _gdk_x11_windowing_launch_failed (GAppLaunchContext *context,
						     const char *startup_notify_id);
extern void        _gdk_x11_windowing_set_cairo_surface_size (cairo_surface_t *surface,
							      int width, int height);
extern cairo_surface_t *_gdk_x11_windowing_create_cairo_surface (GdkDrawable *drawable,
								 int width, int height);
extern GdkDisplay *_gdk_x11_display_open (const gchar *display_name);

/* `_gdk_window_impl_new` (see gdkinternals.h) is also per-backend; the X11
 * definition compiles as `_gdk_x11_window_impl_new` under the rename map
 * (gdkwindowing-x11.h), the Wayland one as `_gdk_wayland_window_impl_new`. */
extern void _gdk_x11_window_impl_new (GdkWindow     *window,
				      GdkWindow     *real_parent,
				      GdkScreen     *screen,
				      GdkVisual     *visual,
				      GdkEventMask   event_mask,
				      GdkWindowAttr *attributes,
				      gint           attributes_mask);

/* ---- M4 families: X11 symbols as produced by the rename map ---- */
extern GdkCursor *_gdk_x11_cursor_new_for_display (GdkDisplay *display, GdkCursorType cursor_type);
extern GdkCursor *_gdk_x11_cursor_new_from_pixmap (GdkPixmap *source, GdkPixmap *mask, const GdkColor *fg, const GdkColor *bg, gint x, gint y);
extern GdkCursor *_gdk_x11_cursor_new_from_pixbuf (GdkDisplay *display, GdkPixbuf *pixbuf, gint x, gint y);
extern GdkCursor *_gdk_x11_cursor_new_from_name (GdkDisplay *display, const gchar *name);
extern void _gdk_x11_cursor_destroy (GdkCursor *cursor);
extern GdkDisplay *_gdk_x11_cursor_get_display (GdkCursor *cursor);
extern GdkPixbuf *_gdk_x11_cursor_get_image (GdkCursor *cursor);
extern gboolean _gdk_x11_display_supports_cursor_alpha (GdkDisplay *display);
extern gboolean _gdk_x11_display_supports_cursor_color (GdkDisplay *display);
extern guint _gdk_x11_display_get_default_cursor_size (GdkDisplay *display);
extern void _gdk_x11_display_get_maximal_cursor_size (GdkDisplay *display, guint *width, guint *height);
extern GdkDisplay *_gdk_x11_font_get_display (GdkFont *font);
extern GdkFont *_gdk_x11_font_load_for_display (GdkDisplay *display, const gchar *font_name);
extern GdkFont *_gdk_x11_font_from_description_for_display (GdkDisplay *display, PangoFontDescription *font_desc);
extern GdkFont *_gdk_x11_fontset_load_for_display (GdkDisplay *display, const gchar *fontset_name);
extern void _gdk_x11_font_destroy (GdkFont *font);
extern gint _gdk_x11_font_strlen (GdkFont *font, const gchar *str);
extern gint _gdk_x11_font_id (const GdkFont *font);
extern gboolean _gdk_x11_font_equal (const GdkFont *fonta, const GdkFont *fontb);
extern gint _gdk_x11_text_width (GdkFont *font, const gchar *text, gint text_length);
extern gint _gdk_x11_text_width_wc (GdkFont *font, const GdkWChar *text, gint text_length);
extern void _gdk_x11_text_extents (GdkFont *font, const gchar *text, gint text_length, gint *lbearing, gint *rbearing, gint *width, gint *ascent, gint *descent);
extern void _gdk_x11_text_extents_wc (GdkFont *font, const GdkWChar *text, gint text_length, gint *lbearing, gint *rbearing, gint *width, gint *ascent, gint *descent);
extern gboolean _gdk_x11_selection_owner_set_for_display (GdkDisplay *display, GdkWindow *owner, GdkAtom selection, guint32 time, gboolean send_event);
extern GdkWindow *_gdk_x11_selection_owner_get_for_display (GdkDisplay *display, GdkAtom selection);
extern void _gdk_x11_selection_convert (GdkWindow *requestor, GdkAtom selection, GdkAtom target, guint32 time);
extern gint _gdk_x11_selection_property_get (GdkWindow *requestor, guchar **data, GdkAtom *ret_type, gint *ret_format);
extern void _gdk_x11_selection_send_notify_for_display (GdkDisplay *display, GdkNativeWindow requestor, GdkAtom selection, GdkAtom target, GdkAtom property, guint32 time);
extern gint _gdk_x11_text_property_to_text_list_for_display (GdkDisplay *display, GdkAtom encoding, gint format, const guchar *text, gint length, gchar ***list);
extern gint _gdk_x11_text_property_to_utf8_list_for_display (GdkDisplay *display, GdkAtom encoding, gint format, const guchar *text, gint length, gchar ***list);
extern void _gdk_x11_free_text_list (gchar **list);
extern void _gdk_x11_free_compound_text (guchar *ctext);
extern gint _gdk_x11_string_to_compound_text_for_display (GdkDisplay *display, const gchar *str, GdkAtom *encoding, gint *format, guchar **ctext, gint *length);
extern gboolean _gdk_x11_utf8_to_compound_text_for_display (GdkDisplay *display, const gchar *str, GdkAtom *encoding, gint *format, guchar **ctext, gint *length);
extern GdkDragContext *_gdk_x11_drag_begin (GdkWindow *window, GList *targets);
extern GdkNativeWindow _gdk_x11_drag_get_protocol_for_display (GdkDisplay *display, GdkNativeWindow xid, GdkDragProtocol *protocol);
extern void _gdk_x11_drag_find_window_for_screen (GdkDragContext *context, GdkWindow *drag_window, GdkScreen *screen, gint x_root, gint y_root, GdkWindow **dest_window, GdkDragProtocol *protocol);
extern gboolean _gdk_x11_drag_motion (GdkDragContext *context, GdkWindow *dest_window, GdkDragProtocol protocol, gint x_root, gint y_root, GdkDragAction suggested_action, GdkDragAction possible_actions, guint32 time_);
extern void _gdk_x11_drag_drop (GdkDragContext *context, guint32 time_);
extern void _gdk_x11_drag_abort (GdkDragContext *context, guint32 time_);
extern void _gdk_x11_drag_status (GdkDragContext *context, GdkDragAction action, guint32 time_);
extern GdkAtom _gdk_x11_drag_get_selection (GdkDragContext *context);
extern gboolean _gdk_x11_drag_drop_succeeded (GdkDragContext *context);
extern void _gdk_x11_drop_reply (GdkDragContext *context, gboolean ok, guint32 time_);
extern void _gdk_x11_drop_finish (GdkDragContext *context, gboolean success, guint32 time_);
extern void _gdk_x11_window_register_dnd (GdkWindow *window);
extern gboolean _gdk_x11_property_get (GdkWindow *window, GdkAtom property, GdkAtom type, gulong offset, gulong length, gint pdelete, GdkAtom *actual_property_type, gint *actual_format_type, gint *actual_length, guchar **data);
extern void _gdk_x11_property_change (GdkWindow *window, GdkAtom property, GdkAtom type, gint format, GdkPropMode mode, const guchar *data, gint nelements);
extern void _gdk_x11_property_delete (GdkWindow *window, GdkAtom property);
extern GdkAtom _gdk_x11_atom_intern (const gchar *atom_name, gboolean only_if_exists);
extern GdkAtom _gdk_x11_atom_intern_static_string (const gchar *atom_name);
extern gchar *_gdk_x11_atom_name (GdkAtom atom);

typedef enum
{
  GDK_SELECTED_BACKEND_UNSET = 0,
  GDK_SELECTED_BACKEND_X11,
  GDK_SELECTED_BACKEND_WAYLAND
} GdkSelectedBackend;

static GdkSelectedBackend selected_backend = GDK_SELECTED_BACKEND_UNSET;

static GdkSelectedBackend
gdk_backend_select (void)
{
  const gchar *backend;

  if (selected_backend != GDK_SELECTED_BACKEND_UNSET)
    return selected_backend;

  backend = g_getenv ("GDK_BACKEND");
  if (backend)
    {
      if (g_ascii_strcasecmp (backend, "x11") == 0)
	selected_backend = GDK_SELECTED_BACKEND_X11;
      else if (g_ascii_strcasecmp (backend, "wayland") == 0)
	selected_backend = GDK_SELECTED_BACKEND_WAYLAND;
      else
	{
	  g_warning ("GDK_BACKEND '%s' not recognized; expected 'x11' or "
		     "'wayland'", backend);
	  selected_backend = GDK_SELECTED_BACKEND_X11;
	}
      return selected_backend;
    }

  if (g_getenv ("WAYLAND_DISPLAY"))
    selected_backend = GDK_SELECTED_BACKEND_WAYLAND;
  else
    selected_backend = GDK_SELECTED_BACKEND_X11;

  return selected_backend;
}

void
_gdk_windowing_init (void)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    _gdk_wayland_windowing_init ();
  else
    _gdk_x11_windowing_init ();
}

void
_gdk_windowing_exit (void)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    _gdk_wayland_windowing_exit ();
  else
    _gdk_x11_windowing_exit ();
}

void
_gdk_windowing_set_default_display (GdkDisplay *display)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    _gdk_wayland_windowing_set_default_display (display);
  else
    _gdk_x11_windowing_set_default_display (display);
}

gchar *
_gdk_windowing_substitute_screen_number (const gchar *display_name,
					 gint screen_number)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_windowing_substitute_screen_number (display_name,
							    screen_number);
  else
    return _gdk_x11_windowing_substitute_screen_number (display_name,
							screen_number);
}

gulong
_gdk_windowing_window_get_next_serial (GdkDisplay *display)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_windowing_window_get_next_serial (display);
  else
    return _gdk_x11_windowing_window_get_next_serial (display);
}

GdkRegion *
_gdk_windowing_window_get_shape (GdkWindow *window)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_windowing_window_get_shape (window);
  else
    return _gdk_x11_windowing_window_get_shape (window);
}

GdkRegion *
_gdk_windowing_window_get_input_shape (GdkWindow *window)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_windowing_window_get_input_shape (window);
  else
    return _gdk_x11_windowing_window_get_input_shape (window);
}

GdkRegion *
_gdk_windowing_get_shape_for_mask (GdkBitmap *mask)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_windowing_get_shape_for_mask (mask);
  else
    return _gdk_x11_windowing_get_shape_for_mask (mask);
}

void
_gdk_windowing_window_beep (GdkWindow *window)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    _gdk_wayland_windowing_window_beep (window);
  else
    _gdk_x11_windowing_window_beep (window);
}

void
_gdk_windowing_get_pointer (GdkDisplay *display,
			    GdkScreen **screen,
			    gint *x,
			    gint *y,
			    GdkModifierType *mask)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    _gdk_wayland_windowing_get_pointer (display, screen, x, y, mask);
  else
    _gdk_x11_windowing_get_pointer (display, screen, x, y, mask);
}

GdkWindow *
_gdk_windowing_window_at_pointer (GdkDisplay *display,
				  gint *win_x,
				  gint *win_y,
				  GdkModifierType *mask,
				  gboolean get_toplevel)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_windowing_window_at_pointer (display, win_x, win_y,
						    mask, get_toplevel);
  else
    return _gdk_x11_windowing_window_at_pointer (display, win_x, win_y,
						 mask, get_toplevel);
}

GdkGrabStatus
_gdk_windowing_pointer_grab (GdkWindow *window,
			     GdkWindow *native,
			     gboolean owner_events,
			     GdkEventMask event_mask,
			     GdkWindow *confine_to,
			     GdkCursor *cursor,
			     guint32 time_)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_windowing_pointer_grab (window, native, owner_events,
					       event_mask, confine_to, cursor,
					       time_);
  else
    return _gdk_x11_windowing_pointer_grab (window, native, owner_events,
					    event_mask, confine_to, cursor,
					    time_);
}

void
_gdk_windowing_window_process_updates_recurse (GdkWindow *window,
					       GdkRegion *expose_region)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    _gdk_wayland_windowing_window_process_updates_recurse (window,
							   expose_region);
  else
    _gdk_x11_windowing_window_process_updates_recurse (window, expose_region);
}

void
_gdk_windowing_before_process_all_updates (void)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    _gdk_wayland_windowing_before_process_all_updates ();
  else
    _gdk_x11_windowing_before_process_all_updates ();
}

void
_gdk_windowing_after_process_all_updates (void)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    _gdk_wayland_windowing_after_process_all_updates ();
  else
    _gdk_x11_windowing_after_process_all_updates ();
}

gint
_gdk_windowing_get_bits_for_depth (GdkDisplay *display, gint depth)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_windowing_get_bits_for_depth (display, depth);
  else
    return _gdk_x11_windowing_get_bits_for_depth (display, depth);
}

void
_gdk_windowing_window_destroy_foreign (GdkWindow *window)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    _gdk_wayland_windowing_window_destroy_foreign (window);
  else
    _gdk_x11_windowing_window_destroy_foreign (window);
}

void
_gdk_windowing_display_set_sm_client_id (GdkDisplay *display,
					 const gchar *sm_client_id)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    _gdk_wayland_windowing_display_set_sm_client_id (display, sm_client_id);
  else
    _gdk_x11_windowing_display_set_sm_client_id (display, sm_client_id);
}

void
_gdk_windowing_window_set_composited (GdkWindow *window, gboolean composited)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    _gdk_wayland_windowing_window_set_composited (window, composited);
  else
    _gdk_x11_windowing_window_set_composited (window, composited);
}

void
_gdk_windowing_gc_set_clip_region (GdkGC *gc,
				   const GdkRegion *region,
				   gboolean reset_origin)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    _gdk_wayland_windowing_gc_set_clip_region (gc, region, reset_origin);
  else
    _gdk_x11_windowing_gc_set_clip_region (gc, region, reset_origin);
}

void
_gdk_windowing_gc_copy (GdkGC *dst_gc, GdkGC *src_gc)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    _gdk_wayland_windowing_gc_copy (dst_gc, src_gc);
  else
    _gdk_x11_windowing_gc_copy (dst_gc, src_gc);
}

char *
_gdk_windowing_get_startup_notify_id (GAppLaunchContext *context,
				      GAppInfo *info,
				      GList *files)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_windowing_get_startup_notify_id (context, info, files);
  else
    return _gdk_x11_windowing_get_startup_notify_id (context, info, files);
}

void
_gdk_windowing_launch_failed (GAppLaunchContext *context,
			      const char *startup_notify_id)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    _gdk_wayland_windowing_launch_failed (context, startup_notify_id);
  else
    _gdk_x11_windowing_launch_failed (context, startup_notify_id);
}

void
_gdk_windowing_set_cairo_surface_size (cairo_surface_t *surface,
				       int width,
				       int height)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    _gdk_wayland_windowing_set_cairo_surface_size (surface, width, height);
  else
    _gdk_x11_windowing_set_cairo_surface_size (surface, width, height);
}

cairo_surface_t *
_gdk_windowing_create_cairo_surface (GdkDrawable *drawable,
				     int width,
				     int height)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_windowing_create_cairo_surface (drawable, width,
							height);
  else
    return _gdk_x11_windowing_create_cairo_surface (drawable, width, height);
}

void
_gdk_window_impl_new (GdkWindow     *window,
		      GdkWindow     *real_parent,
		      GdkScreen     *screen,
		      GdkVisual     *visual,
		      GdkEventMask   event_mask,
		      GdkWindowAttr *attributes,
		      gint           attributes_mask)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    _gdk_wayland_window_impl_new (window, real_parent, screen, visual,
				  event_mask, attributes, attributes_mask);
  else
    _gdk_x11_window_impl_new (window, real_parent, screen, visual,
			      event_mask, attributes, attributes_mask);
}

/**
 * gdk_display_open:
 * @display_name: (allow-none): the name of the display to open.
 *
 * Opens the backend display selected by GDK_BACKEND (default: Wayland when
 * WAYLAND_DISPLAY is set, X11 otherwise).  See gdkbackend.c.
 *
 * Returns: a new %GdkDisplay, or %NULL if the display could not be opened.
 */
GdkDisplay *
gdk_display_open (const gchar *display_name)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_display_open (display_name);
  else
    return _gdk_x11_display_open (display_name);
}

/* gdkaliasdef.c no longer emits this alias (see its note): the exported
 * `gdk_display_open` must resolve to whichever backend is active, and the
 * dispatcher owns the canonical internal definition.  Emit it here. */
#undef gdk_display_open
extern __typeof (gdk_display_open) gdk_display_open
  __attribute ((alias ("IA__gdk_display_open"), visibility ("default")));


/* ---- M5 families: pixmap, GC screen, GdkImage (X11 symbols as produced by
 * the rename map) ---- */
extern GType _gdk_x11_pixmap_impl_get_type (void);
extern GdkPixmap *_gdk_x11_pixmap_new (GdkDrawable *drawable, gint width, gint height, gint depth);
extern GdkPixmap *_gdk_x11_bitmap_create_from_data (GdkDrawable *drawable, const gchar *data, gint width, gint height);
extern GdkPixmap *_gdk_x11_pixmap_create_from_data (GdkDrawable *drawable, const gchar *data, gint width, gint height, gint depth, const GdkColor *fg, const GdkColor *bg);
extern GdkPixmap *_gdk_x11_pixmap_foreign_new_for_display (GdkDisplay *display, GdkNativeWindow anid);
extern GdkPixmap *_gdk_x11_pixmap_foreign_new_for_screen (GdkScreen *screen, GdkNativeWindow anid, gint width, gint height, gint depth);
extern GdkPixmap *_gdk_x11_pixmap_foreign_new (GdkNativeWindow anid);
extern GdkPixmap *_gdk_x11_pixmap_lookup (GdkNativeWindow anid);
extern GdkPixmap *_gdk_x11_pixmap_lookup_for_display (GdkDisplay *display, GdkNativeWindow anid);
extern GdkScreen *_gdk_x11_gc_get_screen (GdkGC *gc);
extern GdkImage *_gdk_x11_image_new_for_depth (GdkScreen *screen, GdkImageType type, GdkVisual *visual, gint width, gint height, gint depth);
extern GdkImage *_gdk_x11_image_new_bitmap (GdkVisual *visual, gpointer data, gint width, gint height);
extern guint32 _gdk_x11_image_get_pixel (GdkImage *image, gint x, gint y);
extern void _gdk_x11_image_put_pixel (GdkImage *image, gint x, gint y, guint32 pixel);

/* ---- F7 externs: colormap family, visual screen */
extern GdkScreen *_gdk_x11_visual_get_screen (GdkVisual *visual);
extern GdkColormap *_gdk_x11_colormap_new (GdkVisual *visual, gboolean allocate);
extern GdkScreen *_gdk_x11_colormap_get_screen (GdkColormap *cmap);
extern gint _gdk_x11_colormap_get_system_size (void);
extern void _gdk_x11_colormap_change (GdkColormap *colormap, gint ncolors);
extern gint _gdk_x11_colors_alloc (GdkColormap *colormap, gboolean contiguous, gulong *planes, gint nplanes, gulong *pixels, gint npixels);
extern void _gdk_x11_colors_free (GdkColormap *colormap, gulong *pixels, gint npixels, gulong planes);
extern void _gdk_x11_colormap_free_colors (GdkColormap *colormap, const GdkColor *colors, gint n_colors);
extern gint _gdk_x11_colormap_alloc_colors (GdkColormap *colormap, GdkColor *colors, gint n_colors, gboolean writeable, gboolean best_match, gboolean *success);
extern void _gdk_x11_colormap_query_color (GdkColormap *colormap, gulong pixel, GdkColor *result);
extern gint _gdk_x11_color_change (GdkColormap *colormap, GdkColor *color);


/* ---- F8 externs: visual query, default colormap, rgba */
extern gint _gdk_x11_visual_get_best_depth (void);
extern GdkVisualType _gdk_x11_visual_get_best_type (void);
extern GdkVisual *_gdk_x11_visual_get_best (void);
extern GdkVisual *_gdk_x11_visual_get_best_with_depth (gint depth);
extern GdkVisual *_gdk_x11_visual_get_best_with_type (GdkVisualType visual_type);
extern GdkVisual *_gdk_x11_visual_get_best_with_both (gint depth, GdkVisualType visual_type);
extern void _gdk_x11_query_depths (gint **depths, gint *count);
extern void _gdk_x11_query_visual_types (GdkVisualType **visual_types, gint *count);
extern GList *_gdk_x11_screen_list_visuals (GdkScreen *screen);
extern GdkColormap *_gdk_x11_screen_get_default_colormap (GdkScreen *screen);
extern void _gdk_x11_screen_set_default_colormap (GdkScreen *screen, GdkColormap *colormap);
extern GdkColormap *_gdk_x11_screen_get_rgba_colormap (GdkScreen *screen);
extern GdkVisual *_gdk_x11_screen_get_rgba_visual (GdkScreen *screen);

/* ---- M4 dispatchers: cursor, font, selection, drag, property ----
 *
 * The names defined below expand to IA__gdk_* via gdkalias.h; the exported
 * public aliases are emitted for this TU by the build (wscript stamp, see
 * tools/gen_aliases.py) because this file is the canonical definer. */

/* ---- cursor */

GdkCursor *
gdk_cursor_new_for_display (GdkDisplay *display, GdkCursorType cursor_type)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_cursor_new_for_display (display, cursor_type);
  else
    return _gdk_x11_cursor_new_for_display (display, cursor_type);
}

GdkCursor *
gdk_cursor_new_from_pixmap (GdkPixmap *source, GdkPixmap *mask, const GdkColor *fg, const GdkColor *bg, gint x, gint y)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_cursor_new_from_pixmap (source, mask, fg, bg, x, y);
  else
    return _gdk_x11_cursor_new_from_pixmap (source, mask, fg, bg, x, y);
}

GdkCursor *
gdk_cursor_new_from_pixbuf (GdkDisplay *display, GdkPixbuf *pixbuf, gint x, gint y)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_cursor_new_from_pixbuf (display, pixbuf, x, y);
  else
    return _gdk_x11_cursor_new_from_pixbuf (display, pixbuf, x, y);
}

GdkCursor *
gdk_cursor_new_from_name (GdkDisplay *display, const gchar *name)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_cursor_new_from_name (display, name);
  else
    return _gdk_x11_cursor_new_from_name (display, name);
}

void
_gdk_cursor_destroy (GdkCursor *cursor)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    _gdk_wayland_cursor_destroy (cursor);
  else
    _gdk_x11_cursor_destroy (cursor);
}

GdkDisplay *
gdk_cursor_get_display (GdkCursor *cursor)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_cursor_get_display (cursor);
  else
    return _gdk_x11_cursor_get_display (cursor);
}

GdkPixbuf *
gdk_cursor_get_image (GdkCursor *cursor)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_cursor_get_image (cursor);
  else
    return _gdk_x11_cursor_get_image (cursor);
}

gboolean
gdk_display_supports_cursor_alpha (GdkDisplay *display)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_display_supports_cursor_alpha (display);
  else
    return _gdk_x11_display_supports_cursor_alpha (display);
}

gboolean
gdk_display_supports_cursor_color (GdkDisplay *display)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_display_supports_cursor_color (display);
  else
    return _gdk_x11_display_supports_cursor_color (display);
}

guint
gdk_display_get_default_cursor_size (GdkDisplay *display)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_display_get_default_cursor_size (display);
  else
    return _gdk_x11_display_get_default_cursor_size (display);
}

void
gdk_display_get_maximal_cursor_size (GdkDisplay *display, guint *width, guint *height)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    _gdk_wayland_display_get_maximal_cursor_size (display, width, height);
  else
    _gdk_x11_display_get_maximal_cursor_size (display, width, height);
}

/* ---- font */

GdkDisplay *
gdk_font_get_display (GdkFont *font)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_font_get_display (font);
  else
    return _gdk_x11_font_get_display (font);
}

GdkFont *
gdk_font_load_for_display (GdkDisplay *display, const gchar *font_name)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_font_load_for_display (display, font_name);
  else
    return _gdk_x11_font_load_for_display (display, font_name);
}

GdkFont *
gdk_font_from_description_for_display (GdkDisplay *display, PangoFontDescription *font_desc)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_font_from_description_for_display (display, font_desc);
  else
    return _gdk_x11_font_from_description_for_display (display, font_desc);
}

GdkFont *
gdk_fontset_load_for_display (GdkDisplay *display, const gchar *fontset_name)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_fontset_load_for_display (display, fontset_name);
  else
    return _gdk_x11_fontset_load_for_display (display, fontset_name);
}

void
_gdk_font_destroy (GdkFont *font)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    _gdk_wayland_font_destroy (font);
  else
    _gdk_x11_font_destroy (font);
}

gint
_gdk_font_strlen (GdkFont *font, const gchar *str)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_font_strlen (font, str);
  else
    return _gdk_x11_font_strlen (font, str);
}

gint
gdk_font_id (const GdkFont *font)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_font_id (font);
  else
    return _gdk_x11_font_id (font);
}

gboolean
gdk_font_equal (const GdkFont *fonta, const GdkFont *fontb)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_font_equal (fonta, fontb);
  else
    return _gdk_x11_font_equal (fonta, fontb);
}

gint
gdk_text_width (GdkFont *font, const gchar *text, gint text_length)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_text_width (font, text, text_length);
  else
    return _gdk_x11_text_width (font, text, text_length);
}

gint
gdk_text_width_wc (GdkFont *font, const GdkWChar *text, gint text_length)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_text_width_wc (font, text, text_length);
  else
    return _gdk_x11_text_width_wc (font, text, text_length);
}

void
gdk_text_extents (GdkFont *font, const gchar *text, gint text_length, gint *lbearing, gint *rbearing, gint *width, gint *ascent, gint *descent)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    _gdk_wayland_text_extents (font, text, text_length, lbearing, rbearing, width, ascent, descent);
  else
    _gdk_x11_text_extents (font, text, text_length, lbearing, rbearing, width, ascent, descent);
}

void
gdk_text_extents_wc (GdkFont *font, const GdkWChar *text, gint text_length, gint *lbearing, gint *rbearing, gint *width, gint *ascent, gint *descent)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    _gdk_wayland_text_extents_wc (font, text, text_length, lbearing, rbearing, width, ascent, descent);
  else
    _gdk_x11_text_extents_wc (font, text, text_length, lbearing, rbearing, width, ascent, descent);
}

/* ---- selection */

gboolean
gdk_selection_owner_set_for_display (GdkDisplay *display, GdkWindow *owner, GdkAtom selection, guint32 time, gboolean send_event)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_selection_owner_set_for_display (display, owner, selection, time, send_event);
  else
    return _gdk_x11_selection_owner_set_for_display (display, owner, selection, time, send_event);
}

GdkWindow *
gdk_selection_owner_get_for_display (GdkDisplay *display, GdkAtom selection)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_selection_owner_get_for_display (display, selection);
  else
    return _gdk_x11_selection_owner_get_for_display (display, selection);
}

void
gdk_selection_convert (GdkWindow *requestor, GdkAtom selection, GdkAtom target, guint32 time)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    _gdk_wayland_selection_convert (requestor, selection, target, time);
  else
    _gdk_x11_selection_convert (requestor, selection, target, time);
}

gint
gdk_selection_property_get (GdkWindow *requestor, guchar **data, GdkAtom *ret_type, gint *ret_format)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_selection_property_get (requestor, data, ret_type, ret_format);
  else
    return _gdk_x11_selection_property_get (requestor, data, ret_type, ret_format);
}

void
gdk_selection_send_notify_for_display (GdkDisplay *display, GdkNativeWindow requestor, GdkAtom selection, GdkAtom target, GdkAtom property, guint32 time)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    _gdk_wayland_selection_send_notify_for_display (display, requestor, selection, target, property, time);
  else
    _gdk_x11_selection_send_notify_for_display (display, requestor, selection, target, property, time);
}

gint
gdk_text_property_to_text_list_for_display (GdkDisplay *display, GdkAtom encoding, gint format, const guchar *text, gint length, gchar ***list)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_text_property_to_text_list_for_display (display, encoding, format, text, length, list);
  else
    return _gdk_x11_text_property_to_text_list_for_display (display, encoding, format, text, length, list);
}

gint
gdk_text_property_to_utf8_list_for_display (GdkDisplay *display, GdkAtom encoding, gint format, const guchar *text, gint length, gchar ***list)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_text_property_to_utf8_list_for_display (display, encoding, format, text, length, list);
  else
    return _gdk_x11_text_property_to_utf8_list_for_display (display, encoding, format, text, length, list);
}

void
gdk_free_text_list (gchar **list)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    _gdk_wayland_free_text_list (list);
  else
    _gdk_x11_free_text_list (list);
}

void
gdk_free_compound_text (guchar *ctext)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    _gdk_wayland_free_compound_text (ctext);
  else
    _gdk_x11_free_compound_text (ctext);
}

gint
gdk_string_to_compound_text_for_display (GdkDisplay *display, const gchar *str, GdkAtom *encoding, gint *format, guchar **ctext, gint *length)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_string_to_compound_text_for_display (display, str, encoding, format, ctext, length);
  else
    return _gdk_x11_string_to_compound_text_for_display (display, str, encoding, format, ctext, length);
}

gboolean
gdk_utf8_to_compound_text_for_display (GdkDisplay *display, const gchar *str, GdkAtom *encoding, gint *format, guchar **ctext, gint *length)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_utf8_to_compound_text_for_display (display, str, encoding, format, ctext, length);
  else
    return _gdk_x11_utf8_to_compound_text_for_display (display, str, encoding, format, ctext, length);
}

/* ---- drag */

GdkDragContext *
gdk_drag_begin (GdkWindow *window, GList *targets)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_drag_begin (window, targets);
  else
    return _gdk_x11_drag_begin (window, targets);
}

GdkNativeWindow
gdk_drag_get_protocol_for_display (GdkDisplay *display, GdkNativeWindow xid, GdkDragProtocol *protocol)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_drag_get_protocol_for_display (display, xid, protocol);
  else
    return _gdk_x11_drag_get_protocol_for_display (display, xid, protocol);
}

void
gdk_drag_find_window_for_screen (GdkDragContext *context, GdkWindow *drag_window, GdkScreen *screen, gint x_root, gint y_root, GdkWindow **dest_window, GdkDragProtocol *protocol)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    _gdk_wayland_drag_find_window_for_screen (context, drag_window, screen, x_root, y_root, dest_window, protocol);
  else
    _gdk_x11_drag_find_window_for_screen (context, drag_window, screen, x_root, y_root, dest_window, protocol);
}

gboolean
gdk_drag_motion (GdkDragContext *context, GdkWindow *dest_window, GdkDragProtocol protocol, gint x_root, gint y_root, GdkDragAction suggested_action, GdkDragAction possible_actions, guint32 time_)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_drag_motion (context, dest_window, protocol, x_root, y_root, suggested_action, possible_actions, time_);
  else
    return _gdk_x11_drag_motion (context, dest_window, protocol, x_root, y_root, suggested_action, possible_actions, time_);
}

void
gdk_drag_drop (GdkDragContext *context, guint32 time_)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    _gdk_wayland_drag_drop (context, time_);
  else
    _gdk_x11_drag_drop (context, time_);
}

void
gdk_drag_abort (GdkDragContext *context, guint32 time_)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    _gdk_wayland_drag_abort (context, time_);
  else
    _gdk_x11_drag_abort (context, time_);
}

void
gdk_drag_status (GdkDragContext *context, GdkDragAction action, guint32 time_)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    _gdk_wayland_drag_status (context, action, time_);
  else
    _gdk_x11_drag_status (context, action, time_);
}

GdkAtom
gdk_drag_get_selection (GdkDragContext *context)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_drag_get_selection (context);
  else
    return _gdk_x11_drag_get_selection (context);
}

gboolean
gdk_drag_drop_succeeded (GdkDragContext *context)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_drag_drop_succeeded (context);
  else
    return _gdk_x11_drag_drop_succeeded (context);
}

void
gdk_drop_reply (GdkDragContext *context, gboolean ok, guint32 time_)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    _gdk_wayland_drop_reply (context, ok, time_);
  else
    _gdk_x11_drop_reply (context, ok, time_);
}

void
gdk_drop_finish (GdkDragContext *context, gboolean success, guint32 time_)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    _gdk_wayland_drop_finish (context, success, time_);
  else
    _gdk_x11_drop_finish (context, success, time_);
}

void
gdk_window_register_dnd (GdkWindow *window)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    _gdk_wayland_window_register_dnd (window);
  else
    _gdk_x11_window_register_dnd (window);
}

/* ---- property */

gboolean
gdk_property_get (GdkWindow *window, GdkAtom property, GdkAtom type, gulong offset, gulong length, gint pdelete, GdkAtom *actual_property_type, gint *actual_format_type, gint *actual_length, guchar **data)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_property_get (window, property, type, offset, length, pdelete, actual_property_type, actual_format_type, actual_length, data);
  else
    return _gdk_x11_property_get (window, property, type, offset, length, pdelete, actual_property_type, actual_format_type, actual_length, data);
}

void
gdk_property_change (GdkWindow *window, GdkAtom property, GdkAtom type, gint format, GdkPropMode mode, const guchar *data, gint nelements)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    _gdk_wayland_property_change (window, property, type, format, mode, data, nelements);
  else
    _gdk_x11_property_change (window, property, type, format, mode, data, nelements);
}

void
gdk_property_delete (GdkWindow *window, GdkAtom property)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    _gdk_wayland_property_delete (window, property);
  else
    _gdk_x11_property_delete (window, property);
}

GdkAtom
gdk_atom_intern (const gchar *atom_name, gboolean only_if_exists)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_atom_intern (atom_name, only_if_exists);
  else
    return _gdk_x11_atom_intern (atom_name, only_if_exists);
}

GdkAtom
gdk_atom_intern_static_string (const gchar *atom_name)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_atom_intern_static_string (atom_name);
  else
    return _gdk_x11_atom_intern_static_string (atom_name);
}

gchar *
gdk_atom_name (GdkAtom atom)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_atom_name (atom);
  else
    return _gdk_x11_atom_name (atom);
}

/* ---- M5 dispatchers: pixmap, GC screen, GdkImage ---- */

GType
_gdk_pixmap_impl_get_type (void)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_pixmap_impl_get_type ();
  else
    return _gdk_x11_pixmap_impl_get_type ();
}

GdkPixmap *
_gdk_pixmap_new (GdkDrawable *drawable, gint width, gint height, gint depth)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_pixmap_new (drawable, width, height, depth);
  else
    return _gdk_x11_pixmap_new (drawable, width, height, depth);
}

GdkPixmap *
_gdk_bitmap_create_from_data (GdkDrawable *drawable, const gchar *data, gint width, gint height)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_bitmap_create_from_data (drawable, data, width, height);
  else
    return _gdk_x11_bitmap_create_from_data (drawable, data, width, height);
}

GdkPixmap *
_gdk_pixmap_create_from_data (GdkDrawable *drawable, const gchar *data, gint width, gint height, gint depth, const GdkColor *fg, const GdkColor *bg)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_pixmap_create_from_data (drawable, data, width, height, depth, fg, bg);
  else
    return _gdk_x11_pixmap_create_from_data (drawable, data, width, height, depth, fg, bg);
}

GdkPixmap *
gdk_pixmap_foreign_new_for_display (GdkDisplay *display, GdkNativeWindow anid)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_pixmap_foreign_new_for_display (display, anid);
  else
    return _gdk_x11_pixmap_foreign_new_for_display (display, anid);
}

GdkPixmap *
gdk_pixmap_foreign_new_for_screen (GdkScreen *screen, GdkNativeWindow anid, gint width, gint height, gint depth)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_pixmap_foreign_new_for_screen (screen, anid, width, height, depth);
  else
    return _gdk_x11_pixmap_foreign_new_for_screen (screen, anid, width, height, depth);
}

GdkPixmap *
gdk_pixmap_foreign_new (GdkNativeWindow anid)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_pixmap_foreign_new (anid);
  else
    return _gdk_x11_pixmap_foreign_new (anid);
}

GdkPixmap *
gdk_pixmap_lookup (GdkNativeWindow anid)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_pixmap_lookup (anid);
  else
    return _gdk_x11_pixmap_lookup (anid);
}

GdkPixmap *
gdk_pixmap_lookup_for_display (GdkDisplay *display, GdkNativeWindow anid)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_pixmap_lookup_for_display (display, anid);
  else
    return _gdk_x11_pixmap_lookup_for_display (display, anid);
}

GdkScreen *
gdk_gc_get_screen (GdkGC *gc)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_gc_get_screen (gc);
  else
    return _gdk_x11_gc_get_screen (gc);
}

GdkImage *
_gdk_image_new_for_depth (GdkScreen *screen, GdkImageType type, GdkVisual *visual, gint width, gint height, gint depth)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_image_new_for_depth (screen, type, visual, width, height, depth);
  else
    return _gdk_x11_image_new_for_depth (screen, type, visual, width, height, depth);
}

GdkImage *
gdk_image_new_bitmap (GdkVisual *visual, gpointer data, gint width, gint height)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_image_new_bitmap (visual, data, width, height);
  else
    return _gdk_x11_image_new_bitmap (visual, data, width, height);
}

guint32
gdk_image_get_pixel (GdkImage *image, gint x, gint y)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_image_get_pixel (image, x, y);
  else
    return _gdk_x11_image_get_pixel (image, x, y);
}

void
gdk_image_put_pixel (GdkImage *image, gint x, gint y, guint32 pixel)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    _gdk_wayland_image_put_pixel (image, x, y, pixel);
  else
    _gdk_x11_image_put_pixel (image, x, y, pixel);
}

/* ---- F7 dispatchers: colormap ---- */

GdkColormap *
gdk_colormap_new (GdkVisual *visual, gboolean allocate)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_colormap_new (visual, allocate);
  else
    return _gdk_x11_colormap_new (visual, allocate);
}

GdkScreen *
gdk_colormap_get_screen (GdkColormap *cmap)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_colormap_get_screen (cmap);
  else
    return _gdk_x11_colormap_get_screen (cmap);
}

gint
gdk_colormap_get_system_size (void)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_colormap_get_system_size ();
  else
    return _gdk_x11_colormap_get_system_size ();
}

void
gdk_colormap_change (GdkColormap *colormap, gint ncolors)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    _gdk_wayland_colormap_change (colormap, ncolors);
  else
    _gdk_x11_colormap_change (colormap, ncolors);
}

gint
gdk_colors_alloc (GdkColormap *colormap, gboolean contiguous, gulong *planes, gint nplanes, gulong *pixels, gint npixels)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_colors_alloc (colormap, contiguous, planes, nplanes, pixels, npixels);
  else
    return _gdk_x11_colors_alloc (colormap, contiguous, planes, nplanes, pixels, npixels);
}

void
gdk_colors_free (GdkColormap *colormap, gulong *pixels, gint npixels, gulong planes)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    _gdk_wayland_colors_free (colormap, pixels, npixels, planes);
  else
    _gdk_x11_colors_free (colormap, pixels, npixels, planes);
}

void
gdk_colormap_free_colors (GdkColormap *colormap, const GdkColor *colors, gint n_colors)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    _gdk_wayland_colormap_free_colors (colormap, colors, n_colors);
  else
    _gdk_x11_colormap_free_colors (colormap, colors, n_colors);
}

gint
gdk_colormap_alloc_colors (GdkColormap *colormap, GdkColor *colors, gint n_colors, gboolean writeable, gboolean best_match, gboolean *success)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_colormap_alloc_colors (colormap, colors, n_colors, writeable, best_match, success);
  else
    return _gdk_x11_colormap_alloc_colors (colormap, colors, n_colors, writeable, best_match, success);
}

void
gdk_colormap_query_color (GdkColormap *colormap, gulong pixel, GdkColor *result)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    _gdk_wayland_colormap_query_color (colormap, pixel, result);
  else
    _gdk_x11_colormap_query_color (colormap, pixel, result);
}

gint
gdk_color_change (GdkColormap *colormap, GdkColor *color)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_color_change (colormap, color);
  else
    return _gdk_x11_color_change (colormap, color);
}

GdkScreen *
gdk_visual_get_screen (GdkVisual *visual)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_visual_get_screen (visual);
  else
    return _gdk_x11_visual_get_screen (visual);
}


/* ---- F8 dispatchers: visual query, default colormap, rgba */

gint
gdk_visual_get_best_depth (void)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_visual_get_best_depth ();
  else
    return _gdk_x11_visual_get_best_depth ();
}

GdkVisualType
gdk_visual_get_best_type (void)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_visual_get_best_type ();
  else
    return _gdk_x11_visual_get_best_type ();
}

GdkVisual *
gdk_visual_get_best (void)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_visual_get_best ();
  else
    return _gdk_x11_visual_get_best ();
}

GdkVisual *
gdk_visual_get_best_with_depth (gint depth)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_visual_get_best_with_depth (depth);
  else
    return _gdk_x11_visual_get_best_with_depth (depth);
}

GdkVisual *
gdk_visual_get_best_with_type (GdkVisualType visual_type)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_visual_get_best_with_type (visual_type);
  else
    return _gdk_x11_visual_get_best_with_type (visual_type);
}

GdkVisual *
gdk_visual_get_best_with_both (gint depth, GdkVisualType visual_type)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_visual_get_best_with_both (depth, visual_type);
  else
    return _gdk_x11_visual_get_best_with_both (depth, visual_type);
}

void
gdk_query_depths (gint **depths, gint *count)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    _gdk_wayland_query_depths (depths, count);
  else
    _gdk_x11_query_depths (depths, count);
}

void
gdk_query_visual_types (GdkVisualType **visual_types, gint *count)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    _gdk_wayland_query_visual_types (visual_types, count);
  else
    _gdk_x11_query_visual_types (visual_types, count);
}

GList *
gdk_screen_list_visuals (GdkScreen *screen)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_screen_list_visuals (screen);
  else
    return _gdk_x11_screen_list_visuals (screen);
}

GdkColormap *
gdk_screen_get_default_colormap (GdkScreen *screen)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_screen_get_default_colormap (screen);
  else
    return _gdk_x11_screen_get_default_colormap (screen);
}

void
gdk_screen_set_default_colormap (GdkScreen *screen, GdkColormap *colormap)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    _gdk_wayland_screen_set_default_colormap (screen, colormap);
  else
    _gdk_x11_screen_set_default_colormap (screen, colormap);
}

GdkColormap *
gdk_screen_get_rgba_colormap (GdkScreen *screen)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_screen_get_rgba_colormap (screen);
  else
    return _gdk_x11_screen_get_rgba_colormap (screen);
}

GdkVisual *
gdk_screen_get_rgba_visual (GdkScreen *screen)
{
  if (gdk_backend_select () == GDK_SELECTED_BACKEND_WAYLAND)
    return _gdk_wayland_screen_get_rgba_visual (screen);
  else
    return _gdk_x11_screen_get_rgba_visual (screen);
}
