/*
 * gdkdisplay-wayland.c
 *
 * ydk-wayland fork: Wayland GdkDisplay subclass + the display-oriented
 * members of the internal `_gdk_wayland_*` contract.
 *
 * Skeleton status: connects to the compositor and gives back a valid
 * GdkDisplay so that `gdk_init_check`/`gdk_display_open` succeed under a
 * Wayland session.  Window/styling machinery lands in M2.
 */

#include "config.h"

#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <wayland-client.h>

#include <ydk/gdk.h>
#include <ydk/gdkapplaunchcontext.h>
#include <ydk/gdkcairo.h>

#include "xdg-shell-client.h"

#include "gdkprivate-wayland.h"

#ifdef GDK_COMPILATION
#include "gdkalias.h"
#endif

static void gdkw_display_wayland_finalize (GObject *object);

G_DEFINE_TYPE (GDKWDisplayWayland, gdkw_display_wayland, GDK_TYPE_DISPLAY)

static const gchar *
gdkw_display_wayland_get_display_name (GdkDisplay *display)
{
  const gchar *name = g_getenv ("WAYLAND_DISPLAY");

  return name ? name : "wayland";
}

static gint
gdkw_display_wayland_get_n_screens (GdkDisplay *display)
{
  /* Wayland has no separate screens. */
  return 1;
}

static GdkScreen *
gdkw_display_wayland_get_screen (GdkDisplay *display, gint screen_num)
{
  GDKWDisplayWayland *wayland_display = GDKW_DISPLAY_WAYLAND (display);

  return screen_num == 0 ? wayland_display->default_screen : NULL;
}

static GdkScreen *
gdkw_display_wayland_get_default_screen (GdkDisplay *display)
{
  return GDKW_DISPLAY_WAYLAND (display)->default_screen;
}

void
_gdk_wayland_display_present_all (GDKWDisplayWayland *display)
{
  GSList *l;

  for (l = display->windows; l; l = l->next)
    {
      GDKWWindowImplWayland *impl = l->data;

      _gdk_wayland_window_impl_present (impl);
    }
}

static void
gdkw_display_wayland_sync (GdkDisplay *display)
{
  GDKWDisplayWayland *wayland_display = GDKW_DISPLAY_WAYLAND (display);

  /* Present any outstanding window content, then block until the
   * compositor has consumed it.  The roundtrip is what delivers the
   * xdg_surface configure events that let toplevels map. */
  _gdk_wayland_display_present_all (wayland_display);
  wl_display_flush (wayland_display->wl_display);
  wl_display_roundtrip (wayland_display->wl_display);
}

static void
gdkw_display_wayland_flush (GdkDisplay *display)
{
  wl_display_flush (GDKW_DISPLAY_WAYLAND (display)->wl_display);
}

static void
xdg_wm_base_ping (void *data,
		  struct xdg_wm_base *xdg_wm_base,
		  uint32_t serial)
{
  xdg_wm_base_pong (xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
  xdg_wm_base_ping
};

static void
gdkw_display_wayland_finalize (GObject *object)
{
  GDKWDisplayWayland *display = GDKW_DISPLAY_WAYLAND (object);

  /* give the running window impls (their wl objects are already gone if
   * gdk_window_destroy ran) a last chance to release their surfaces */
  if (display->windows)
    {
      GSList *l;

      for (l = display->windows; l; l = l->next)
	_gdk_wayland_window_impl_teardown (l->data);
      g_slist_free (display->windows);
      display->windows = NULL;
    }

  if (display->wl_registry)
    {
      wl_registry_destroy (display->wl_registry);
      display->wl_registry = NULL;
    }
  if (display->wl_shm)
    {
      wl_shm_destroy (display->wl_shm);
      display->wl_shm = NULL;
    }
  if (display->xdg_wm_base)
    {
      xdg_wm_base_destroy (display->xdg_wm_base);
      display->xdg_wm_base = NULL;
    }
  if (display->wl_compositor)
    {
      wl_compositor_destroy (display->wl_compositor);
      display->wl_compositor = NULL;
    }
  if (display->wl_output)
    {
      wl_output_destroy (display->wl_output);
      display->wl_output = NULL;
    }

  if (display->wl_display)
    {
      wl_display_flush (display->wl_display);
      wl_display_disconnect (display->wl_display);
      display->wl_display = NULL;
    }

  G_OBJECT_CLASS (gdkw_display_wayland_parent_class)->finalize (object);
}

static void
gdkw_display_wayland_class_init (GDKWDisplayWaylandClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GdkDisplayClass *display_class = GDK_DISPLAY_CLASS (klass);

  object_class->finalize = gdkw_display_wayland_finalize;

  display_class->get_display_name = gdkw_display_wayland_get_display_name;
  display_class->get_n_screens = gdkw_display_wayland_get_n_screens;
  display_class->get_screen = gdkw_display_wayland_get_screen;
  display_class->get_default_screen = gdkw_display_wayland_get_default_screen;
  display_class->flush = gdkw_display_wayland_flush;
  display_class->sync = gdkw_display_wayland_sync;
}

static void
gdkw_display_wayland_init (GDKWDisplayWayland *display)
{
  display->wl_display = NULL;
  display->wl_registry = NULL;
  display->wl_compositor = NULL;
  display->wl_shm = NULL;
  display->xdg_wm_base = NULL;
  display->wl_output = NULL;
  display->serial = 0;
  display->default_screen = NULL;
  display->output_width = 0;
  display->output_height = 0;
  display->windows = NULL;
  display->xdg_shell_supported = FALSE;
}

/*************** wl_registry + wl_output (screen geometry) ***************/

static void
wl_output_geometry (void *data,
		    struct wl_output *output,
		    int32_t x,
		    int32_t y,
		    int32_t physical_width,
		    int32_t physical_height,
		    int32_t subpixel,
		    const char *make,
		    const char *model,
		    int32_t transform)
{
}

static void
wl_output_mode (void *data,
		struct wl_output *output,
		uint32_t flags,
		int32_t width,
		int32_t height,
		int32_t refresh)
{
  GDKWDisplayWayland *display = data;

  /* M1 tracks a single output (the first with a current mode); multihead
   * handling can extend this when window support lands. */
  if (flags & WL_OUTPUT_MODE_CURRENT)
    {
      display->output_width = width;
      display->output_height = height;
    }
}

static void
wl_output_done (void *data, struct wl_output *output)
{
}

static const struct wl_output_listener wl_output_listener = {
  wl_output_geometry,
  wl_output_mode,
  wl_output_done
};

static void
wl_registry_global (void *data,
		    struct wl_registry *registry,
		    uint32_t name,
		    const char *interface,
		    uint32_t version)
{
  GDKWDisplayWayland *display = data;

  if (strcmp (interface, "wl_output") == 0 && !display->wl_output)
    {
      display->wl_output =
	wl_registry_bind (registry, name, &wl_output_interface, 1);
      wl_output_add_listener (display->wl_output, &wl_output_listener,
			      display);
    }
  else if (strcmp (interface, "wl_compositor") == 0 && !display->wl_compositor)
    {
      display->wl_compositor =
	wl_registry_bind (registry, name, &wl_compositor_interface, 1);
    }
  else if (strcmp (interface, "wl_shm") == 0 && !display->wl_shm)
    {
      display->wl_shm =
	wl_registry_bind (registry, name, &wl_shm_interface, 1);
    }
  else if (strcmp (interface, "xdg_wm_base") == 0 && !display->xdg_wm_base)
    {
      display->xdg_wm_base =
	wl_registry_bind (registry, name, &xdg_wm_base_interface, 1);
      xdg_wm_base_add_listener (display->xdg_wm_base,
				&xdg_wm_base_listener, display);
      display->xdg_shell_supported = TRUE;
    }
}

static void
wl_registry_global_remove (void *data,
			   struct wl_registry *registry,
			   uint32_t name)
{
}

static const struct wl_registry_listener wl_registry_listener = {
  wl_registry_global,
  wl_registry_global_remove
};

GdkDisplay *
_gdk_wayland_display_open (const gchar *display_name)
{
  GDKWDisplayWayland *display;
  struct wl_display *wl_display;

  if (display_name)
    wl_display = wl_display_connect (display_name);
  else
    wl_display = wl_display_connect (NULL);

  if (!wl_display)
    return NULL;

  display = g_object_new (GDKW_TYPE_DISPLAY_WAYLAND, NULL);
  display->wl_display = wl_display;
  display->serial = 0;

  /* Walk the registry for wl_output so the screen below has compositor
   * geometry; wl_shm/xdg_wm_base are picked up with window support.  A
   * second roundtrip settles the output mode events. */
  display->wl_registry = wl_display_get_registry (wl_display);
  wl_registry_add_listener (display->wl_registry, &wl_registry_listener,
			    display);

  if (wl_display_roundtrip (wl_display) < 0 ||
      wl_display_roundtrip (wl_display) < 0)
    {
      g_object_unref (display);
      return NULL;
    }

  wl_registry_destroy (display->wl_registry);
  display->wl_registry = NULL;

  /* M1: one logical screen per display.  The depth/scale of the output is
   * read later if the fallback is not used. */
  if (display->output_width <= 0 || display->output_height <= 0)
    {
      display->output_width = 1024;
      display->output_height = 768;
    }

  display->default_screen =
    _gdk_wayland_screen_new (GDK_DISPLAY_OBJECT (display),
			     display->output_width, display->output_height);

  return GDK_DISPLAY_OBJECT (display);
}

/*************** windowning contract (stubs until M1) ***************/

void
_gdk_wayland_windowing_set_default_display (GdkDisplay *display)
{
}

GdkRegion *
_gdk_wayland_windowing_window_get_shape (GdkWindow *window)
{
  return NULL;
}

GdkRegion *
_gdk_wayland_windowing_window_get_input_shape (GdkWindow *window)
{
  return NULL;
}

GdkRegion *
_gdk_wayland_windowing_get_shape_for_mask (GdkBitmap *mask)
{
  return NULL;
}

void
_gdk_wayland_windowing_window_beep (GdkWindow *window)
{
}

void
_gdk_wayland_windowing_get_pointer (GdkDisplay *display,
				    GdkScreen **screen,
				    gint *x,
				    gint *y,
				    GdkModifierType *mask)
{
  if (screen)
    *screen = NULL;
  if (x)
    *x = 0;
  if (y)
    *y = 0;
  if (mask)
    *mask = 0;
}

GdkWindow *
_gdk_wayland_windowing_window_at_pointer (GdkDisplay *display,
					  gint *win_x,
					  gint *win_y,
					  GdkModifierType *mask,
					  gboolean get_toplevel)
{
  return NULL;
}

GdkGrabStatus
_gdk_wayland_windowing_pointer_grab (GdkWindow *window,
				     GdkWindow *native,
				     gboolean owner_events,
				     GdkEventMask event_mask,
				     GdkWindow *confine_to,
				     GdkCursor *cursor,
				     guint32 time_)
{
  return GDK_GRAB_FROZEN;
}

void
_gdk_wayland_windowing_window_process_updates_recurse (GdkWindow *window,
						       GdkRegion *expose_region)
{
}

void
_gdk_wayland_windowing_before_process_all_updates (void)
{
}

void
_gdk_wayland_windowing_after_process_all_updates (void)
{
}

gint
_gdk_wayland_windowing_get_bits_for_depth (GdkDisplay *display, gint depth)
{
  return 32;
}

void
_gdk_wayland_windowing_window_destroy_foreign (GdkWindow *window)
{
}

void
_gdk_wayland_windowing_display_set_sm_client_id (GdkDisplay *display,
						 const gchar *sm_client_id)
{
}

void
_gdk_wayland_windowing_window_set_composited (GdkWindow *window,
					      gboolean composited)
{
}

void
_gdk_wayland_windowing_gc_set_clip_region (GdkGC *gc,
					   const GdkRegion *region,
					   gboolean reset_origin)
{
}

void
_gdk_wayland_windowing_gc_copy (GdkGC *dst_gc, GdkGC *src_gc)
{
}

char *
_gdk_wayland_windowing_get_startup_notify_id (GAppLaunchContext *context,
					      GAppInfo *info,
					      GList *files)
{
  return NULL;
}

void
_gdk_wayland_windowing_launch_failed (GAppLaunchContext *context,
				      const char *startup_notify_id)
{
}

void
_gdk_wayland_windowing_set_cairo_surface_size (cairo_surface_t *surface,
					       int width,
					       int height)
{
}

/* `_gdk_wayland_windowing_create_cairo_surface` lives in the window impl
 * TU (gdkwindowimpl-wayland.c): the cairo target for a window is the
 * shm-backed image surface, so it must be created where the impl lives. */