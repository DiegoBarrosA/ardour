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
#include "gdkm7-wayland.h"
#include "gdkm8-wayland.h"
#include "gdkm9-wayland.h"
#include "gdkm10-wayland.h"

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
void _gdk_wayland_window_hints_set_initial_title (GDKWWindowImplWayland *impl,
						 const gchar *title);

static void gdk_window_impl_iface_init (GdkWindowImplIface *iface);
static void gdk_paintable_iface_init (GdkPaintableIface *iface);

G_DEFINE_TYPE_WITH_CODE (GDKWWindowImplWayland,
                         gdkw_window_impl_wayland,
                         GDK_TYPE_DRAWABLE,
                         G_IMPLEMENT_INTERFACE (GDK_TYPE_WINDOW_IMPL,
                                                gdk_window_impl_iface_init)
                         G_IMPLEMENT_INTERFACE (GDK_TYPE_PAINTABLE,
                                                gdk_paintable_iface_init));

static void grab_check_unmap (GdkWindow *window);

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

  /* M10: a fresh wrapper surface over the refcounted back store; the core
   * caches it per GdkWindow and detaches it when the store is replaced */
  return _gdk_wayland_buffers_create_wrapper (impl, width, height);
}

static cairo_surface_t *
gdkw_window_impl_wayland_ref_cairo_surface (GdkDrawable *drawable)
{
  GDKWWindowImplWayland *impl = GDKW_WINDOW_IMPL_WAYLAND (drawable);

  /* the shared backing; drawing code (the M5 draw vfuncs) paints on it.  A
   * WRITER entry (damage bookkeeping, M10 D5c). */
  return _gdk_wayland_buffers_ref_backing (impl);
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

  /* M10: frame callback, timers, idles, slot buffers and the back store.
   * Must run before the popup record and the wl_surface go. */
  _gdk_wayland_frame_teardown (impl);

  /* M9: destroys the xdg_popup and the record (idempotent); the code below
   * destroys the xdg_surface and the wl_surface */
  _gdk_wayland_popup_teardown (impl);

  if (impl->xdg_toplevel)
    {
      xdg_toplevel_destroy (impl->xdg_toplevel);
      _gdk_wayland_ledger_xdg (TRUE, FALSE);
      impl->xdg_toplevel = NULL;
    }
  if (impl->xdg_surface)
    {
      xdg_surface_destroy (impl->xdg_surface);
      _gdk_wayland_ledger_xdg (FALSE, FALSE);
      impl->xdg_surface = NULL;
    }
  if (impl->wl_surface)
    {
      /* M10: wp_viewport, wp_fractional_scale_v1, surface listener data */
      _gdk_wayland_scale_surface_destroy (impl);
      wl_surface_destroy (impl->wl_surface);
      impl->wl_surface = NULL;
    }
  impl->configured = FALSE;
  g_free (impl->wmclass);
  impl->wmclass = NULL;
}

/*************** xdg-shell listeners ***************/

static void
xdg_surface_handle_configure (void *data,
			      struct xdg_surface *xdg_surface,
			      uint32_t serial)
{
  GDKWWindowImplWayland *impl = data;

  /* M9: a popup does its own ack, geometry and position feedback */
  if (impl->popup != NULL)
    {
      _gdk_wayland_popup_surface_configure (impl, xdg_surface, serial);
      _gdk_wayland_window_impl_present (impl);
      return;
    }

  xdg_surface_set_window_geometry (xdg_surface, 0, 0,
				   impl->width, impl->height);
  xdg_surface_ack_configure (xdg_surface, serial);
  impl->configured = TRUE;

  /* children waiting for this window as their transient parent */
  _gdk_wayland_window_hints_mapped (impl);

  /* first allowed buffer commit (M10): the frame piece commits at once on
   * the first configure since the map and only arms the settle idle on later
   * ones. */
  _gdk_wayland_frame_configured (impl);
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

  /* M10 D9: a compositor size replaces the back store, updates the
   * GdkWindowObject, invalidates and queues one GDK_CONFIGURE.  0x0 and an
   * unchanged size do nothing. */
  if (width > 0 && height > 0)
    _gdk_wayland_frame_apply_configure_size (impl, width, height);

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
 * expose goes straight to the window's own surface.  end_paint is therefore
 * a no-op.
 *
 * begin_paint_region is not a no-op (M11, G12, the "H1" bug).  The core
 * returns right after calling this hook, so it never pushes a GdkWindowPaint
 * and never runs gdk_window_clear_backing_region (), the step that gives a
 * double buffered widget a clean background to draw on.  The X11 backend gets
 * the same result with a pixmap that is cleared to the window background.
 * gtkmain.c calls begin_paint_region (event->expose.region) for every
 * double buffered widget, and widgets such as GtkLabel then draw only their
 * ink.  Without the clear, the back store keeps the old ink and a changed
 * text shows both the old and the new glyphs.
 *
 * So clear the part of the region that is visible and not covered by a child
 * window to the window background.  gdk_window_clear_area () does the work
 * in the core: it handles a solid colour, a tiled background pixmap and
 * GDK_PARENT_RELATIVE_BG, with the same rules as the X11 path.  It reaches
 * gdk_window_clear_backing_region_direct () because this impl has no
 * clear_region vfunc (see gdk_window_impl_iface_init below).
 *
 * Damage: the expose region is the region that gdkframe-wayland.c damages
 * after the update scope, and the clear stays inside it, so no extra damage
 * is needed.  Outside an update scope the draw goes through the writer entry
 * of the buffers piece (_gdk_wayland_buffers_ref_backing), which marks the
 * window "unknown dirty", so the next flush sends full damage.
 *
 * Shape: gdkshape-wayland.c clears the pixels outside the bounding shape to
 * transparent in the back store before every present.  That runs after the
 * expose, so the opaque background written here does not show outside the
 * shape.
 *
 * Alpha: the clear is opaque (the GdkColor background has no alpha), as it
 * is for a double buffered widget under X11.  A translucent window draws its
 * own pixels with CAIRO_OPERATOR_SOURCE, as under X11. */
static void
gdk_paintable_wayland_begin_paint_region (GdkPaintable *paintable,
					  GdkWindow *window,
					  const GdkRegion *region)
{
  GdkWindowObject *private = (GdkWindowObject *) window;
  GdkRegion *clear;
  GdkRectangle *rects = NULL;
  gint n_rects = 0, i;

  if (region == NULL || private->input_only ||
      private->clip_region_with_children == NULL)
    return;

  /* Same clip as the core's own begin_paint_region (): what is visible,
   * without the children, which are painted by their own windows. */
  clear = gdk_region_copy (region);
  gdk_region_intersect (clear, private->clip_region_with_children);
  gdk_region_get_rectangles (clear, &rects, &n_rects);
  gdk_region_destroy (clear);

  for (i = 0; i < n_rects; i++)
    gdk_window_clear_area (window, rects[i].x, rects[i].y,
			   rects[i].width, rects[i].height);

  g_free (rects);
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

/* M9: the part of show () that runs once the impl has (or is to get) a
 * wl_surface and is to be a plain xdg_toplevel.  Also used by the popup piece
 * for the fallback toplevel (a popup candidate with no usable parent). */
void
_gdk_wayland_window_impl_create_toplevel (GDKWWindowImplWayland *impl)
{
  GDKWDisplayWayland *display;

  if (impl == NULL || impl->screen == NULL ||
      gdk_screen_get_display (impl->screen) == NULL)
    return;

  display = GDKW_DISPLAY_WAYLAND (gdk_screen_get_display (impl->screen));

  if (impl->xdg_toplevel != NULL)
    return;

  if (impl->wl_surface == NULL)
    {
      impl->wl_surface = wl_compositor_create_surface (display->wl_compositor);
      /* M10: wl_surface listener, wp_viewport, wp_fractional_scale_v1.
       * Idempotent. */
      _gdk_wayland_scale_surface_created (impl);
    }

  if (impl->xdg_surface == NULL)
    {
      impl->xdg_surface =
        xdg_wm_base_get_xdg_surface (display->xdg_wm_base, impl->wl_surface);
      _gdk_wayland_ledger_xdg (FALSE, TRUE);
      xdg_surface_add_listener (impl->xdg_surface, &xdg_surface_listener,
                                impl);
    }

  impl->xdg_toplevel = xdg_surface_get_toplevel (impl->xdg_surface);
  _gdk_wayland_ledger_xdg (TRUE, TRUE);
  xdg_toplevel_add_listener (impl->xdg_toplevel, &xdg_toplevel_listener,
			     impl);

  /* Stored hints (app id, title, size limits, parent, state requests) go
   * out before the first commit. */
  _gdk_wayland_window_hints_apply (impl);

  /* Stored shape regions go out before the first commit. */
  _gdk_wayland_shape_surface_created (impl);

  if (g_slist_find (display->windows, impl) == NULL)
    display->windows = g_slist_prepend (display->windows, impl);

  /* First commit must be buffer-less: xdg-shell's initial configure is only
   * produced in response to a commit, and committing a buffer before that
   * configure is acked is a protocol error.  The configure listener acks and
   * then performs the first real present; subsequent frames are committed by
   * display sync via present_all. */
  wl_surface_commit (impl->wl_surface);
}

void
_gdk_wayland_window_impl_grab_check_unmap (GdkWindow *window)
{
  grab_check_unmap (window);
}

const struct xdg_surface_listener *
_gdk_wayland_window_impl_xdg_surface_listener (void)
{
  return &xdg_surface_listener;
}

/* X11 reports each map and unmap of a native window with MapNotify and
 * UnmapNotify, and the core queues GDK_MAP and GDK_UNMAP only for windows
 * without an impl.  Wayland has no such event, so queue them here.  Without
 * GDK_MAP, gtk_widget_show_now () never returns for a toplevel and the
 * map-event handlers of GTK and Ardour never run.  Only the toplevel role is
 * covered: the popup piece queues its own GDK_UNMAP when the compositor
 * dismisses a popup (the demo checks for exactly one), and popups and
 * surfaceless windows get no GDK_MAP yet.  Same heap event pattern as
 * queue_configure () in gdkframe-wayland.c. */
static void
queue_map_event (GdkWindow *window, GdkEventType type)
{
  GdkWindowObject *private = (GdkWindowObject *) window;
  GdkEvent *ev;

  if (!(private->event_mask & GDK_STRUCTURE_MASK))
    return;

  ev = gdk_event_new (type);
  ev->any.window = g_object_ref (window);
  ev->any.send_event = FALSE;
  gdk_event_set_screen (ev, gdk_drawable_get_screen (GDK_DRAWABLE (window)));
  gdk_event_put (ev);
  gdk_event_free (ev);
}

static void
send_map_event (GdkWindow *window, GDKWWindowImplWayland *impl)
{
  if (impl->map_event_sent)
    return;
  impl->map_event_sent = TRUE;
  queue_map_event (window, GDK_MAP);
}

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

  /* M9: surfaceless windows (input-only, native child, DND) and popup
   * candidates are handled by the popup piece. */
  if (_gdk_wayland_popup_show (window))
    return;

  /* M10 D8: hide destroys the xdg role but keeps the wl_surface, so the
   * "already shown" test is the toplevel role, not the surface */
  if (impl->xdg_toplevel != NULL)
    return;

  /* creates the wl_surface when there is none */
  _gdk_wayland_window_impl_create_toplevel (impl);
  send_map_event (window, impl);
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
    {
      if (impl->map_event_sent)
	{
	  impl->map_event_sent = FALSE;
	  queue_map_event (window, GDK_UNMAP);
	}

      grab_check_unmap (window);

      /* M9: popups above this window go first (xdg-shell wants top-down),
       * then this window's own popup or surfaceless role */
      _gdk_wayland_popup_release_children (impl, TRUE);
      if (_gdk_wayland_popup_hide (window))
	return;
    }

  /* M10 D8: attach NULL + commit, destroy callback, timers and slots, destroy
   * xdg_toplevel and xdg_surface */
  _gdk_wayland_frame_unmap (impl);
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
  GdkWindowObject *private = (GdkWindowObject *) window;
  gboolean size_changed;
  gint new_width, new_height;

  impl = GDKW_WINDOW_IMPL_WAYLAND (private->impl);
  if (with_move)
    {
      impl->x = x;
      impl->y = y;
    }
  /* gdk_window_move () passes -1 for width and height: keep the old size. */
  new_width = width == -1 ? (gint) impl->width : width;
  new_height = height == -1 ? (gint) impl->height : height;
  size_changed = (guint) new_width != impl->width ||
                 (guint) new_height != impl->height;
  impl->width = new_width;
  impl->height = new_height;

  /* The core does not set the size of a toplevel after this call: the X11
   * backend gets it from the ConfigureNotify, and a compositor configure sets
   * it in _gdk_wayland_frame_apply_configure_size (), but Wayland never
   * answers a client resize.  Without this the GdkWindow keeps its old size,
   * so its clip region, gdk_drawable_get_size () and the pixel reader stay
   * stale.  Children are sized by the core and popups by the popup piece. */
  if (impl->popup == NULL && private->window_type != GDK_WINDOW_CHILD &&
      width > 0 && height > 0)
    {
      private->width = width;
      private->height = height;
    }

  /* M9: no-op unless the impl is a popup */
  _gdk_wayland_popup_move_resize (window, with_move, x, y, width, height);

  /* M10 D9: a client resize of a toplevel replaces the back store (a popup
   * does it in its own positioner path) */
  if (impl->popup == NULL && size_changed)
    _gdk_wayland_frame_client_resize (impl, new_width, new_height);
}

static void
gdk_window_wayland_set_background (GdkWindow *window, const GdkColor *color)
{
  GDKWWindowImplWayland *impl;

  impl = GDKW_WINDOW_IMPL_WAYLAND (((GdkWindowObject *) window)->impl);
  impl->background = *color;
  _gdk_wayland_buffers_paint_background (impl);
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
  return _gdk_wayland_reparent (window, new_parent, x, y);
}

static void
gdk_window_wayland_set_cursor (GdkWindow *window, GdkCursor *cursor)
{
  _gdk_wayland_cursor_window_set_cursor (window, cursor);
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
  GDKWWindowImplWayland *impl;
  gint ox = 0, oy = 0;

  /* M9 D2: a popup sits at its root position, everything else at (0, 0) */
  impl = GDKW_WINDOW_IMPL_WAYLAND (((GdkWindowObject *) window)->impl);
  _gdk_wayland_popup_root_origin (impl, &ox, &oy);

  if (root_x)
    *root_x = x + ox;
  if (root_y)
    *root_y = y + oy;

  return 0;
}

static gint
gdk_window_wayland_get_deskrelative_origin (GdkWindow *window,
					    gint *x,
					    gint *y)
{
  GDKWWindowImplWayland *impl;
  gint ox = 0, oy = 0;

  impl = GDKW_WINDOW_IMPL_WAYLAND (((GdkWindowObject *) window)->impl);
  _gdk_wayland_popup_root_origin (impl, &ox, &oy);

  if (x)
    *x = ox;
  if (y)
    *y = oy;

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

  /* M9: x and y are always filled, through root space, even when the pointer
   * is in another native window (a menu grab window while the pointer is
   * over the base).  TRUE only means the pointer is inside this native
   * window (the core's contract); the core then finds the child itself.
   * The core subtracts abs_x and abs_y itself, so the answer is relative to
   * the impl window, not to @window. */
  {
    gdouble fx = 0, fy = 0;
    GdkWindow *native = (GdkWindow *) ((GdkWindowObject *) window)->impl_window;

    _gdk_wayland_pointer_in_window (gdk_screen_get_display (impl->screen),
				    native != NULL ? native : window, &fx, &fy);
    if (x)
      *x = (gint) fx;
    if (y)
      *y = (gint) fy;
  }

  return impl == seat->pointer_focus;
}

static void
gdk_window_wayland_shape_combine_region (GdkWindow *window,
					 const GdkRegion *shape_region,
					 gint offset_x,
					 gint offset_y)
{
  _gdk_wayland_shape_combine_region (window, shape_region,
				     offset_x, offset_y);
}

static void
gdk_window_wayland_input_shape_combine_region (GdkWindow *window,
					       const GdkRegion *shape_region,
					       gint offset_x,
					       gint offset_y)
{
  _gdk_wayland_shape_input_combine_region (window, shape_region,
					   offset_x, offset_y);
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
    {
      grab_check_unmap (window);
      /* M9: popups of this window first, GDK-hiding them */
      _gdk_wayland_popup_release_children (impl, TRUE);
      _gdk_wayland_selection_window_destroyed (window);
    }

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
  /* No clear_region vfunc on purpose (M11, G12).  Wayland has no server
   * side window background, so the core's direct clear
   * (gdk_window_clear_backing_region_direct) is the right implementation.
   * It draws the background colour or pixmap on the back store and, with
   * send_expose, invalidates the region.  An empty stub here made
   * gdk_window_clear_area () and gdk_window_clear () do nothing. */
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
  iface->demote_to_child = _gdk_wayland_reparent_demote;
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
  else
    {
      /* X11 gives an input-only window the system colormap (gdkwindow-x11.c).
       * GTK relies on it: gtk_style_attach () on the input-only window of a
       * GtkInvisible, which every GtkClipboard owns, reads the colormap and
       * would fail the colormap assertion with NULL. */
      impl->colormap = gdk_screen_get_system_colormap (screen);
      g_object_ref (impl->colormap);
    }

  /* Capture the WM_CLASS hint so it can be forwarded as the xdg_toplevel
   * app_id at show time.  wmclass_class is the more specific half and
   * matches the desktop entry name on most compositors. */
  if ((attributes_mask & GDK_WA_WMCLASS) && attributes->wmclass_class)
    impl->wmclass = g_strdup (attributes->wmclass_class);

  /* Window title, as gdk_window_new () does on X11: GTK passes it as
   * GDK_WA_TITLE and does not call gdk_window_set_title () at realize. */
  switch (private->window_type)
    {
    case GDK_WINDOW_TOPLEVEL:
    case GDK_WINDOW_DIALOG:
    case GDK_WINDOW_TEMP:
      {
        const gchar *title = NULL;

        if (attributes_mask & GDK_WA_TITLE)
          title = attributes->title;
        if (title == NULL)
          title = g_get_application_name ();
        if (title == NULL)
          title = g_get_prgname ();

        _gdk_wayland_window_hints_set_initial_title (impl, title);
      }
      break;
    default:
      break;
    }

  /* M9: override redirect decides the popup role (D1 rule 4) */
  if ((attributes_mask & GDK_WA_NOREDIR) && attributes->override_redirect)
    _gdk_wayland_window_set_override_redirect (window, TRUE);

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
