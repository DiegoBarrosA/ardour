/*
 * gdkwindowimpl-wayland.c
 *
 * ydk-wayland fork: the Wayland window implementation.
 *
 * Each GdkWindowObject ("window") is backed by one of these, attached as
 * window->impl.  Like the X11 backend, the impl is a GdkDrawable subclass
 * (the core get_colormap path in gdkwindow.c dispatches
 * gdk_drawable_get_colormap() off the impl drawable) that also implements
 * the GdkWindowImpl interface.
 *
 * M2 status: toplevel windows create an xdg toplevel and present an
 * ARGB shm buffer that is also the cairo backing store, so drawing
 * renders straight into the composited surface (zero-copy).  Present
 * happens in gdk_window_show() and again during display sync (the demo
 * calls gdk_flush(), which roundtrips and delivers the xdg configure).
 * Root windows have no wl_surface and never present.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "config.h"

#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include <wayland-client.h>
#include <cairo.h>

#include <ydk/gdk.h>
#include <ydk/gdkcairo.h>
#include <ydk/gdkinternals.h>

#include "xdg-shell-client.h"

#include "gdkprivate-wayland.h"
#include "gdkseat-wayland.h"
#include "gdkpixmap-wayland.h"

#ifdef GDK_COMPILATION
#include "gdkalias.h"
#endif

/* M6B PROTOTYPES */
void _gdk_wayland_window_hints_apply (GDKWWindowImplWayland *impl);
void _gdk_wayland_window_hints_configure (GDKWWindowImplWayland *impl,
					  const guint32 *states,
					  gsize n_states);
void _gdk_wayland_window_hints_free (GDKWWindowImplWayland *impl);
void _gdk_wayland_window_hints_mapped (GDKWWindowImplWayland *impl);

static void gdk_window_impl_iface_init (GdkWindowImplIface *iface);
static void gdk_paintable_iface_init (GdkPaintableIface *iface);

G_DEFINE_TYPE_WITH_CODE (GDKWWindowImplWayland,
                         gdkw_window_impl_wayland,
                         GDK_TYPE_DRAWABLE,
                         G_IMPLEMENT_INTERFACE (GDK_TYPE_WINDOW_IMPL,
                                                gdk_window_impl_iface_init)
                         G_IMPLEMENT_INTERFACE (GDK_TYPE_PAINTABLE,
                                                gdk_paintable_iface_init));

static gboolean ensure_shm_buffer (GDKWWindowImplWayland *impl);
static void paint_background (GDKWWindowImplWayland *impl);
static void resize_buffer (GDKWWindowImplWayland *impl,
			   gint width, gint height);

/*************** drawable vfuncs ***************/

static gint
gdkw_window_impl_wayland_get_depth (GdkDrawable *drawable)
{
  GDKWWindowImplWayland *impl = GDKW_WINDOW_IMPL_WAYLAND (drawable);

  if (impl->colormap)
    return gdk_colormap_get_visual (impl->colormap)->depth;

  return 0;
}

static void
gdkw_window_impl_wayland_get_size (GdkDrawable *drawable,
				   gint *width,
				   gint *height)
{
  GDKWWindowImplWayland *impl = GDKW_WINDOW_IMPL_WAYLAND (drawable);

  if (width)
    *width = impl->width;
  if (height)
    *height = impl->height;
}

static void
gdkw_window_impl_wayland_set_colormap (GdkDrawable *drawable,
				       GdkColormap *cmap)
{
  GDKWWindowImplWayland *impl = GDKW_WINDOW_IMPL_WAYLAND (drawable);

  if (impl->colormap)
    g_object_unref (impl->colormap);
  impl->colormap = cmap;
  if (cmap)
    g_object_ref (cmap);
}

static GdkColormap *
gdkw_window_impl_wayland_get_colormap (GdkDrawable *drawable)
{
  return GDKW_WINDOW_IMPL_WAYLAND (drawable)->colormap;
}

static GdkVisual *
gdkw_window_impl_wayland_get_visual (GdkDrawable *drawable)
{
  GDKWWindowImplWayland *impl = GDKW_WINDOW_IMPL_WAYLAND (drawable);

  if (impl->colormap)
    return gdk_colormap_get_visual (impl->colormap);

  return NULL;
}

static GdkScreen *
gdkw_window_impl_wayland_get_screen (GdkDrawable *drawable)
{
  return GDKW_WINDOW_IMPL_WAYLAND (drawable)->screen;
}

static cairo_surface_t *
gdkw_window_impl_wayland_create_cairo_surface (GdkDrawable *drawable,
					       int width,
					       int height)
{
  GDKWWindowImplWayland *impl = GDKW_WINDOW_IMPL_WAYLAND (drawable);
  cairo_surface_t *surface;

  if (!impl->backing && !ensure_shm_buffer (impl))
    return NULL;

  /* A FRESH surface over the same shm bytes.  gdkwindow.c's
   * gdk_window_ref_cairo_surface() sets a per-window device offset and user
   * data on whatever this returns; handing out the shared backing would shift
   * all later drawing through the impl.  (M10: resize_buffer still munmaps
   * the bytes while such wrapper-cached surfaces may point at them.) */
  surface = cairo_image_surface_create_for_data (impl->shm_data,
						 CAIRO_FORMAT_ARGB32,
						 impl->width, impl->height,
						 impl->stride);
  if (cairo_surface_status (surface) != CAIRO_STATUS_SUCCESS)
    {
      cairo_surface_destroy (surface);
      return NULL;
    }

  return surface;
}

static cairo_surface_t *
gdkw_window_impl_wayland_ref_cairo_surface (GdkDrawable *drawable)
{
  GDKWWindowImplWayland *impl = GDKW_WINDOW_IMPL_WAYLAND (drawable);

  /* the shared backing; drawing code (the M5 draw vfuncs) paints on it */
  if (!impl->backing && !ensure_shm_buffer (impl))
    return NULL;

  return cairo_surface_reference (impl->backing);
}

static void
gdkw_window_impl_wayland_set_cairo_clip (GdkDrawable *drawable, cairo_t *cr)
{
}

static void
gdkw_window_impl_wayland_finalize (GObject *object)
{
  GDKWWindowImplWayland *impl = GDKW_WINDOW_IMPL_WAYLAND (object);

  /* idempotent; the display may already be gone */
  _gdk_wayland_window_impl_teardown (impl);

  if (impl->colormap)
    {
      g_object_unref (impl->colormap);
      impl->colormap = NULL;
    }

  _gdk_wayland_window_hints_free (impl);

  G_OBJECT_CLASS (gdkw_window_impl_wayland_parent_class)->finalize (object);
}

static void
gdkw_window_impl_wayland_class_init (GDKWWindowImplWaylandClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GdkDrawableClass *drawable_class = GDK_DRAWABLE_CLASS (klass);

  object_class->finalize = gdkw_window_impl_wayland_finalize;

  /* create_gc and the draw_* vfuncs, shared with the pixmap impl */
  _gdk_wayland_drawable_install_vfuncs (drawable_class);

  drawable_class->get_depth = gdkw_window_impl_wayland_get_depth;
  drawable_class->get_size = gdkw_window_impl_wayland_get_size;
  drawable_class->set_colormap = gdkw_window_impl_wayland_set_colormap;
  drawable_class->get_colormap = gdkw_window_impl_wayland_get_colormap;
  drawable_class->get_visual = gdkw_window_impl_wayland_get_visual;
  drawable_class->get_screen = gdkw_window_impl_wayland_get_screen;
  drawable_class->set_cairo_clip = gdkw_window_impl_wayland_set_cairo_clip;
  drawable_class->get_image = _gdk_wayland_drawable_get_image;
  drawable_class->_copy_to_image = _gdk_wayland_drawable_copy_to_image;
  drawable_class->ref_cairo_surface = gdkw_window_impl_wayland_ref_cairo_surface;
  drawable_class->create_cairo_surface =
    gdkw_window_impl_wayland_create_cairo_surface;
}

static void
gdkw_window_impl_wayland_init (GDKWWindowImplWayland *impl)
{
  impl->screen = NULL;
  impl->colormap = NULL;
  impl->wrapper = NULL;
  impl->x = 0;
  impl->y = 0;
  impl->width = 0;
  impl->height = 0;
  impl->events = 0;
  impl->wl_surface = NULL;
  impl->xdg_surface = NULL;
  impl->xdg_toplevel = NULL;
  impl->wl_buffer = NULL;
  impl->shm_fd = -1;
  impl->shm_data = NULL;
  impl->shm_size = 0;
  impl->stride = 0;
  impl->backing = NULL;
  impl->configured = FALSE;
}

/*************** shm buffer + present ***************/

static gboolean
ensure_shm_buffer (GDKWWindowImplWayland *impl)
{
  GDKWDisplayWayland *display;
  struct wl_shm_pool *pool;
  unsigned int stride, size;
  guint32 *pixels;
  unsigned int i;
  int fd;

  if (impl->wl_buffer)
    return TRUE;

  display = GDKW_DISPLAY_WAYLAND (gdk_screen_get_display (impl->screen));
  if (!display->wl_shm)
    return FALSE;

  if (impl->width <= 0 || impl->height <= 0)
    return FALSE;

  stride = cairo_format_stride_for_width (CAIRO_FORMAT_ARGB32, impl->width);
  size = stride * impl->height;

  fd = memfd_create ("ydk-wayland", MFD_CLOEXEC);
  if (fd < 0)
    return FALSE;

  if (ftruncate (fd, size) < 0)
    {
      close (fd);
      return FALSE;
    }

  impl->shm_data = mmap (NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
			 fd, 0);
  if (impl->shm_data == MAP_FAILED)
    {
      impl->shm_data = NULL;
      close (fd);
      return FALSE;
    }
  impl->shm_fd = fd;
  impl->shm_size = size;
  impl->stride = stride;

  /* opaque black, matching an X11 background pixel of 0 */
  pixels = (guint32 *) impl->shm_data;
  for (i = 0; i < size / 4; i++)
    pixels[i] = 0xff000000;

  pool = wl_shm_create_pool (display->wl_shm, fd, size);
  impl->wl_buffer =
    wl_shm_pool_create_buffer (pool, 0, impl->width, impl->height, stride,
			       WL_SHM_FORMAT_ARGB8888);
  wl_shm_pool_destroy (pool);

  impl->backing =
    cairo_image_surface_create_for_data (impl->shm_data,
					 CAIRO_FORMAT_ARGB32,
					 impl->width, impl->height,
					 stride);

  paint_background (impl);

  return TRUE;
}

/* M3: drop the current buffer/backing so a later present (or
 * create-cairo-surface) rebuilds them at the new geometry.  Used when the
 * compositor forces a size change via the xdg configure event. */
static void
resize_buffer (GDKWWindowImplWayland *impl, gint width, gint height)
{
  if (impl->wl_buffer)
    {
      wl_buffer_destroy (impl->wl_buffer);
      impl->wl_buffer = NULL;
    }
  if (impl->backing)
    {
      cairo_surface_destroy (impl->backing);
      impl->backing = NULL;
    }
  if (impl->shm_data)
    {
      munmap (impl->shm_data, impl->shm_size);
      impl->shm_data = NULL;
    }
  if (impl->shm_fd >= 0)
    {
      close (impl->shm_fd);
      impl->shm_fd = -1;
    }
  impl->shm_size = 0;
  impl->stride = 0;

  impl->width = width;
  impl->height = height;
}

void
_gdk_wayland_window_impl_present (GDKWWindowImplWayland *impl)
{
  if (!impl)
    return;

  /* xdg-shell forbids committing a buffer before the xdg_surface has been
   * acked-configured at least once.  Until then, no-ops. */
  if (!impl->wl_surface || !impl->configured)
    return;

  if (!impl->wl_buffer && !ensure_shm_buffer (impl))
    return;

  wl_surface_attach (impl->wl_surface, impl->wl_buffer, 0, 0);
  wl_surface_damage (impl->wl_surface, 0, 0, impl->width, impl->height);
  wl_surface_commit (impl->wl_surface);
}

GDKWWindowImplWayland *
_gdk_wayland_impl_from_surface (GDKWDisplayWayland *display,
				struct wl_surface *surface)
{
  GSList *l;

  /* a NULL surface would match any impl that has no wl_surface yet */
  if (surface == NULL)
    return NULL;

  for (l = display->windows; l; l = l->next)
    {
      GDKWWindowImplWayland *impl = l->data;

      if (impl->wl_surface == surface)
	return impl;
    }

  return NULL;
}

void
_gdk_wayland_window_impl_teardown (GDKWWindowImplWayland *impl)
{
  GDKWDisplayWayland *display;

  if (!impl)
    return;

  if (impl->screen)
    {
      display = GDKW_DISPLAY_WAYLAND (gdk_screen_get_display (impl->screen));
      /* drop dangling seat pointers (focus, key repeat, explicit grab) */
      if (display)
	_gdk_wayland_seat_forget_impl
	  (_gdk_wayland_display_get_seat (GDK_DISPLAY_OBJECT (display)), impl);

      if (display && display->windows)
	display->windows = g_slist_remove (display->windows, impl);
    }

  if (impl->backing)
    {
      cairo_surface_destroy (impl->backing);
      impl->backing = NULL;
    }
  if (impl->wl_buffer)
    {
      wl_buffer_destroy (impl->wl_buffer);
      impl->wl_buffer = NULL;
    }
  if (impl->shm_data)
    {
      munmap (impl->shm_data, impl->shm_size);
      impl->shm_data = NULL;
      impl->shm_size = 0;
    }
  if (impl->shm_fd >= 0)
    {
      close (impl->shm_fd);
      impl->shm_fd = -1;
    }
  if (impl->xdg_toplevel)
    {
      xdg_toplevel_destroy (impl->xdg_toplevel);
      impl->xdg_toplevel = NULL;
    }
  if (impl->xdg_surface)
    {
      xdg_surface_destroy (impl->xdg_surface);
      impl->xdg_surface = NULL;
    }
  if (impl->wl_surface)
    {
      wl_surface_destroy (impl->wl_surface);
      impl->wl_surface = NULL;
    }
  impl->configured = FALSE;
  g_free (impl->wmclass);
  impl->wmclass = NULL;
}

static void
paint_background (GDKWWindowImplWayland *impl)
{
  cairo_t *cr;

  if (!impl->backing)
    return;

  cr = cairo_create (impl->backing);
  cairo_set_source_rgb (cr,
			impl->background.red / 65535.0,
			impl->background.green / 65535.0,
			impl->background.blue / 65535.0);
  cairo_paint (cr);
  cairo_destroy (cr);
}

/*************** xdg-shell listeners ***************/

static void
xdg_surface_handle_configure (void *data,
			      struct xdg_surface *xdg_surface,
			      uint32_t serial)
{
  GDKWWindowImplWayland *impl = data;

  xdg_surface_set_window_geometry (xdg_surface, 0, 0,
				   impl->width, impl->height);
  xdg_surface_ack_configure (xdg_surface, serial);
  impl->configured = TRUE;

  /* children waiting for this window as their transient parent */
  _gdk_wayland_window_hints_mapped (impl);

  /* first allowed buffer commit: attach the freshly-acked surface.  Later
   * commits happen in display sync via present_all. */
  _gdk_wayland_window_impl_present (impl);
}

static const struct xdg_surface_listener xdg_surface_listener = {
  xdg_surface_handle_configure
};

static void
xdg_toplevel_handle_configure (void *data,
			       struct xdg_toplevel *toplevel,
			       int32_t width,
			       int32_t height,
			       struct wl_array *states)
{
  GDKWWindowImplWayland *impl = data;

  if (width > 0 && height > 0 &&
      ((guint) width != impl->width || (guint) height != impl->height))
    {
      resize_buffer (impl, width, height);

      if (impl->configured && impl->wrapper)
	{
	  GdkRectangle rect = { 0, 0, width, height };

	  gdk_window_invalidate_rect (impl->wrapper, &rect, FALSE);
	}
    }

  /* compositor window state: maximized, fullscreen, activated */
  _gdk_wayland_window_hints_configure
    (impl, states ? (const guint32 *) states->data : NULL,
     states ? states->size / sizeof (guint32) : 0);
}

static void
xdg_toplevel_handle_close (void *data, struct xdg_toplevel *toplevel)
{
  GDKWWindowImplWayland *impl = data;
  GdkEvent *ev;

  if (impl == NULL || impl->wrapper == NULL ||
      GDK_WINDOW_DESTROYED (impl->wrapper))
    return;

  /* The core does not destroy the window; the application answers
   * GDK_DELETE as it would a WM_DELETE_WINDOW message.  Heap event, as
   * everywhere else (gdk_event_put () copies it). */
  ev = gdk_event_new (GDK_DELETE);
  ev->any.window = g_object_ref (impl->wrapper);
  ev->any.send_event = FALSE;

  gdk_event_put (ev);
  gdk_event_free (ev);
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
  xdg_toplevel_handle_configure,
  xdg_toplevel_handle_close
};

/*************** paintable interface ***************/

/* Each Wayland window owns a shm-backed cairo surface and re-composites it
 * (via wl_surface_attach) whenever it needs a new frame, so its backing
 * store is already the double buffer.  By implementing GdkPaintable we tell
 * the core (gdk_window_begin_implicit_paint gdkwindow.c) to skip the
 * implicit paint and backing-pixmap machinery entirely; drawing during
 * expose goes straight to the window's own surface.  begin/end_paint are
 * therefore no-ops. */
static void
gdk_paintable_wayland_begin_paint_region (GdkPaintable *paintable,
					  GdkWindow *window,
					  const GdkRegion *region)
{
}

static void
gdk_paintable_wayland_end_paint (GdkPaintable *paintable)
{
}

static void
gdk_paintable_iface_init (GdkPaintableIface *iface)
{
  iface->begin_paint_region = gdk_paintable_wayland_begin_paint_region;
  iface->end_paint = gdk_paintable_wayland_end_paint;
}

/*************** window impl interface ***************/

static void
gdk_window_wayland_show (GdkWindow *window, gboolean already_mapped)
{
  GdkWindowObject *private = (GdkWindowObject *) window;
  GDKWWindowImplWayland *impl;
  GDKWDisplayWayland *display;

  if (private->window_type == GDK_WINDOW_ROOT ||
      private->window_type == GDK_WINDOW_FOREIGN)
    return;

  impl = GDKW_WINDOW_IMPL_WAYLAND (private->impl);

  if (impl->wl_surface)
    return; /* already shown */

  if (impl->screen == NULL ||
      gdk_screen_get_display (impl->screen) == NULL)
    return; /* display already closed */

  display = GDKW_DISPLAY_WAYLAND (gdk_screen_get_display (impl->screen));

  if (!display->wl_compositor || !display->xdg_wm_base || !display->wl_shm)
    {
      g_warning ("Wayland: compositor is missing wl_compositor/xdg_wm_base/"
		 "wl_shm; cannot show window");
      return;
    }

  impl->wl_surface = wl_compositor_create_surface (display->wl_compositor);
  impl->xdg_surface =
    xdg_wm_base_get_xdg_surface (display->xdg_wm_base, impl->wl_surface);
  impl->xdg_toplevel = xdg_surface_get_toplevel (impl->xdg_surface);

  xdg_surface_add_listener (impl->xdg_surface, &xdg_surface_listener, impl);
  xdg_toplevel_add_listener (impl->xdg_toplevel, &xdg_toplevel_listener,
			     impl);

  /* Stored hints (app id, title, size limits, parent, state requests) go
   * out before the first commit. */
  _gdk_wayland_window_hints_apply (impl);

  display->windows = g_slist_prepend (display->windows, impl);

  /* First commit must be buffer-less: xdg-shell's initial configure is only
   * produced in response to a commit, and committing a buffer before that
   * configure is acked is a protocol error.  The configure listener acks and
   * then performs the first real present; subsequent frames are committed by
   * display sync via present_all. */
  wl_surface_commit (impl->wl_surface);
}

/* Mirrors _gdk_xgrab_check_unmap () in gdkmain-x11.c: a window that stops
 * being viewable loses any pointer or keyboard grab that depended on it. */
static void
grab_check_unmap (GdkWindow *window)
{
  GdkDisplay *display = gdk_drawable_get_display (window);
  GDKWSeat *seat = _gdk_wayland_display_get_seat (display);
  gulong serial = _gdk_windowing_window_get_next_serial (display);

  _gdk_display_end_pointer_grab (display, serial, window, TRUE);
  _gdk_display_pointer_grab_update (display, serial);

  if (display->keyboard_grab.window != NULL &&
      serial >= display->keyboard_grab.serial)
    {
      GdkWindowObject *tmp = GDK_WINDOW_OBJECT (display->keyboard_grab.window);

      while (tmp && tmp != GDK_WINDOW_OBJECT (window))
	tmp = tmp->parent;

      if (tmp)
	_gdk_display_unset_has_keyboard_grab (display, TRUE);
    }

  if (seat->grab.active && seat->grab.native == window)
    _gdk_wayland_seat_grab_clear (seat);

  /* The focus pointers are NOT cleared here: the compositor sends the leave
   * events, which the events piece turns into crossing events. */
}

static void
gdk_window_wayland_hide (GdkWindow *window)
{
  GDKWWindowImplWayland *impl;

  impl = GDKW_WINDOW_IMPL_WAYLAND (((GdkWindowObject *) window)->impl);

  /* the display may already be gone */
  if (impl->screen != NULL && gdk_screen_get_display (impl->screen) != NULL)
    grab_check_unmap (window);

  resize_buffer (impl, impl->width, impl->height);

  if (impl->wl_surface)
    wl_surface_commit (impl->wl_surface);
}

static void
gdk_window_wayland_withdraw (GdkWindow *window)
{
  gdk_window_wayland_hide (window);
}

static void
gdk_window_wayland_raise (GdkWindow *window)
{
}

static void
gdk_window_wayland_lower (GdkWindow *window)
{
}

static void
gdk_window_wayland_restack_under (GdkWindow *window,
				  GList *native_siblings)
{
}

static void
gdk_window_wayland_restack_toplevel (GdkWindow *window,
				     GdkWindow *sibling,
				     gboolean above)
{
}

static void
gdk_window_wayland_move_resize (GdkWindow *window,
				gboolean with_move,
				gint x,
				gint y,
				gint width,
				gint height)
{
  GDKWWindowImplWayland *impl;

  impl = GDKW_WINDOW_IMPL_WAYLAND (((GdkWindowObject *) window)->impl);
  impl->x = x;
  impl->y = y;
  impl->width = width;
  impl->height = height;
}

static void
gdk_window_wayland_set_background (GdkWindow *window, const GdkColor *color)
{
  GDKWWindowImplWayland *impl;

  impl = GDKW_WINDOW_IMPL_WAYLAND (((GdkWindowObject *) window)->impl);
  impl->background = *color;
  paint_background (impl);
}

static void
gdk_window_wayland_set_back_pixmap (GdkWindow *window, GdkPixmap *pixmap)
{
}

static GdkEventMask
gdk_window_wayland_get_events (GdkWindow *window)
{
  GDKWWindowImplWayland *impl;

  impl = GDKW_WINDOW_IMPL_WAYLAND (((GdkWindowObject *) window)->impl);
  return impl->events;
}

static void
gdk_window_wayland_set_events (GdkWindow *window, GdkEventMask event_mask)
{
  GDKWWindowImplWayland *impl;

  impl = GDKW_WINDOW_IMPL_WAYLAND (((GdkWindowObject *) window)->impl);
  impl->events = event_mask;
}

static gboolean
gdk_window_wayland_reparent (GdkWindow *window,
			     GdkWindow *new_parent,
			     gint x,
			     gint y)
{
  return TRUE;
}

static void
gdk_window_wayland_clear_region (GdkWindow *window,
				 GdkRegion *region,
				 gboolean send_expose)
{
}

static void
gdk_window_wayland_set_cursor (GdkWindow *window, GdkCursor *cursor)
{
}

static void
gdk_window_wayland_get_geometry (GdkWindow *window,
				 gint *x,
				 gint *y,
				 gint *width,
				 gint *height,
				 gint *depth)
{
  GDKWWindowImplWayland *impl;

  impl = GDKW_WINDOW_IMPL_WAYLAND (((GdkWindowObject *) window)->impl);

  if (x)
    *x = impl->x;
  if (y)
    *y = impl->y;
  if (width)
    *width = impl->width;
  if (height)
    *height = impl->height;
  if (depth)
    *depth = impl->colormap ?
      gdk_colormap_get_visual (impl->colormap)->depth : 0;
}

static gint
gdk_window_wayland_get_root_coords (GdkWindow *window,
				    gint x,
				    gint y,
				    gint *root_x,
				    gint *root_y)
{
  if (root_x)
    *root_x = x;
  if (root_y)
    *root_y = y;

  return 0;
}

static gint
gdk_window_wayland_get_deskrelative_origin (GdkWindow *window,
					    gint *x,
					    gint *y)
{
  if (x)
    *x = 0;
  if (y)
    *y = 0;

  return 0;
}

static gboolean
gdk_window_wayland_get_pointer (GdkWindow *window,
				gint *x,
				gint *y,
				GdkModifierType *mask)
{
  GDKWWindowImplWayland *impl;
  GDKWSeat *seat;

  impl = GDKW_WINDOW_IMPL_WAYLAND (((GdkWindowObject *) window)->impl);
  seat = _gdk_wayland_display_get_seat (gdk_screen_get_display (impl->screen));

  if (mask)
    *mask = _gdk_wayland_seat_get_state (seat);

  /* TRUE means the pointer is inside this toplevel; the core then finds the
   * child window under it itself. */
  if (impl == seat->pointer_focus)
    {
      if (x)
	*x = (gint) seat->px;
      if (y)
	*y = (gint) seat->py;
      return TRUE;
    }

  if (x)
    *x = 0;
  if (y)
    *y = 0;

  return FALSE;
}

static void
gdk_window_wayland_shape_combine_region (GdkWindow *window,
					 const GdkRegion *shape_region,
					 gint offset_x,
					 gint offset_y)
{
}

static void
gdk_window_wayland_input_shape_combine_region (GdkWindow *window,
					       const GdkRegion *shape_region,
					       gint offset_x,
					       gint offset_y)
{
}

static gboolean
gdk_window_wayland_set_static_gravities (GdkWindow *window,
					 gboolean use_static)
{
  return FALSE;
}

static gboolean
gdk_window_wayland_queue_antiexpose (GdkWindow *window,
				     GdkRegion *update_area)
{
  return FALSE;
}

static void
gdk_window_wayland_queue_translation (GdkWindow *window,
				      GdkGC *gc,
				      GdkRegion *area,
				      gint dx,
				      gint dy)
{
}

static void
gdk_window_wayland_destroy (GdkWindow *window,
			    gboolean recursing,
			    gboolean foreign_destroy)
{
  GDKWWindowImplWayland *impl;

  impl = GDKW_WINDOW_IMPL_WAYLAND (((GdkWindowObject *) window)->impl);

  /* Mirrors _gdk_xgrab_check_destroy (): make sure no grab outlives the
   * native window.  X11 lets the next event end the grab; there may be no
   * next event here, so end it now (same as when the window is hidden). */
  if (impl->screen != NULL && gdk_screen_get_display (impl->screen) != NULL)
    grab_check_unmap (window);

  _gdk_wayland_window_impl_teardown (impl);
}

static void
gdk_window_wayland_input_window_destroy (GdkWindow *window)
{
}

static void
gdk_window_wayland_input_window_crossing (GdkWindow *window,
					  gboolean enter)
{
}

static void
gdk_window_impl_iface_init (GdkWindowImplIface *iface)
{
  iface->show = gdk_window_wayland_show;
  iface->hide = gdk_window_wayland_hide;
  iface->withdraw = gdk_window_wayland_withdraw;
  iface->set_events = gdk_window_wayland_set_events;
  iface->get_events = gdk_window_wayland_get_events;
  iface->raise = gdk_window_wayland_raise;
  iface->lower = gdk_window_wayland_lower;
  iface->restack_under = gdk_window_wayland_restack_under;
  iface->restack_toplevel = gdk_window_wayland_restack_toplevel;
  iface->move_resize = gdk_window_wayland_move_resize;
  iface->set_background = gdk_window_wayland_set_background;
  iface->set_back_pixmap = gdk_window_wayland_set_back_pixmap;
  iface->reparent = gdk_window_wayland_reparent;
  iface->clear_region = gdk_window_wayland_clear_region;
  iface->set_cursor = gdk_window_wayland_set_cursor;
  iface->get_geometry = gdk_window_wayland_get_geometry;
  iface->get_root_coords = gdk_window_wayland_get_root_coords;
  iface->get_deskrelative_origin = gdk_window_wayland_get_deskrelative_origin;
  iface->get_pointer = gdk_window_wayland_get_pointer;
  iface->shape_combine_region = gdk_window_wayland_shape_combine_region;
  iface->input_shape_combine_region =
    gdk_window_wayland_input_shape_combine_region;
  iface->set_static_gravities = gdk_window_wayland_set_static_gravities;
  iface->queue_antiexpose = gdk_window_wayland_queue_antiexpose;
  iface->queue_translation = gdk_window_wayland_queue_translation;
  iface->destroy = gdk_window_wayland_destroy;
  iface->input_window_destroy = gdk_window_wayland_input_window_destroy;
  iface->input_window_crossing = gdk_window_wayland_input_window_crossing;
  iface->supports_native_bg = TRUE;
}

/*************** impl constructor ***************/

GdkDrawable *
_gdk_wayland_window_impl_new (GdkWindow     *window,
			      GdkWindow     *real_parent,
			      GdkScreen     *screen,
			      GdkVisual     *visual,
			      GdkEventMask   event_mask,
			      GdkWindowAttr *attributes,
			      gint           attributes_mask)
{
  GdkWindowObject *private = (GdkWindowObject *) window;
  GDKWWindowImplWayland *impl;

  impl = g_object_new (gdkw_window_impl_wayland_get_type (), NULL);

  private->impl = (GdkDrawable *) impl;

  impl->wrapper = window;
  impl->screen = screen;
  impl->events = event_mask;
  /* core sets bg_color before the impl is created (black by default) */
  impl->background = private->bg_color;

  impl->x = private->x;
  impl->y = private->y;
  impl->width = private->width;
  impl->height = private->height;

  if (!private->input_only)
    {
      if (attributes_mask & GDK_WA_COLORMAP)
	{
	  impl->colormap = attributes->colormap;
	  g_object_ref (impl->colormap);
	}
      else if (visual == gdk_screen_get_system_visual (screen))
	{
	  impl->colormap = gdk_screen_get_system_colormap (screen);
	  g_object_ref (impl->colormap);
	}
      else
	{
	  impl->colormap = _gdk_wayland_screen_create_colormap (screen,
								visual);
	}
    }

  /* Capture the WM_CLASS hint so it can be forwarded as the xdg_toplevel
   * app_id at show time.  wmclass_class is the more specific half and
   * matches the desktop entry name on most compositors. */
  if ((attributes_mask & GDK_WA_WMCLASS) && attributes->wmclass_class)
    impl->wmclass = g_strdup (attributes->wmclass_class);

  return (GdkDrawable *) impl;
}

cairo_surface_t *
_gdk_wayland_windowing_create_cairo_surface (GdkDrawable *drawable,
					     int width,
					     int height)
{
  /* pixmap wrappers reach this through gdk_pixmap_create_cairo_surface */
  if (GDKW_IS_PIXMAP_IMPL_WAYLAND (drawable))
    return _gdk_wayland_pixmap_impl_create_cairo_surface (drawable,
							  width, height);

  g_return_val_if_fail (GDKW_IS_WINDOW_IMPL_WAYLAND (drawable), NULL);

  return gdkw_window_impl_wayland_create_cairo_surface (drawable,
							width, height);
}
/*************** test hook (M6) ***************/

/* Calls the real xdg_toplevel close listener for @window, so the demo can
 * check GDK_DELETE delivery without a compositor sending a close. */
gboolean
_gdk_wayland_test_toplevel_close (GdkWindow *window)
{
  GDKWWindowImplWayland *impl = _gdk_wayland_window_get_impl (window);

  if (impl == NULL || impl->xdg_toplevel == NULL)
    return FALSE;

  xdg_toplevel_handle_close (impl, impl->xdg_toplevel);

  return TRUE;
}

/*************** test hook (M6b) ***************/

/* Builds an xdg state array from GDKW_TEST_STATE_* bits and calls the real
 * xdg_toplevel configure listener. width / height <= 0 keep the size. */
gboolean
_gdk_wayland_test_toplevel_configure (GdkWindow *window,
				      gint width, gint height,
				      guint states)
{
  GDKWWindowImplWayland *impl = _gdk_wayland_window_get_impl (window);
  struct wl_array array;
  guint32 *p;

  if (impl == NULL || impl->xdg_toplevel == NULL)
    return FALSE;

  wl_array_init (&array);

#define ADD_STATE(bit, value) \
  if (states & (bit)) \
    { \
      p = wl_array_add (&array, sizeof (guint32)); \
      if (p) *p = (value); \
    }
  ADD_STATE (GDKW_TEST_STATE_MAXIMIZED, 1)
  ADD_STATE (GDKW_TEST_STATE_FULLSCREEN, 2)
  ADD_STATE (GDKW_TEST_STATE_RESIZING, 3)
  ADD_STATE (GDKW_TEST_STATE_ACTIVATED, 4)
#undef ADD_STATE

  xdg_toplevel_handle_configure (impl, NULL, width, height, &array);
  wl_array_release (&array);

  return TRUE;
}
