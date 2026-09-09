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