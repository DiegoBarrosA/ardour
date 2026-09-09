/*
 * gdkdisplay-wayland.c
 *
 * ydk-wayland fork: Wayland GdkDisplay subclass + the display-oriented
 * members of the internal `_gdk_wayland_*` contract.
 *
 * Skeleton status: connects to the compositor and gives back a valid
 * GdkDisplay so that `gdk_init_check`/`gdk_display_open` succeed under a
 * Wayland session.  Window/styling machinery lands in M1.
 */

#include "config.h"

#include <sys/types.h>
#include <unistd.h>

#include <wayland-client.h>

#include <ydk/gdk.h>
#include <ydk/gdkapplaunchcontext.h>
#include <ydk/gdkcairo.h>

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
  /* A real screen object arrives with window support in M1/M2. */
  return NULL;
}

static GdkScreen *
gdkw_display_wayland_get_default_screen (GdkDisplay *display)
{
  return NULL;
}

static void
gdkw_display_wayland_finalize (GObject *object)
{
  GDKWDisplayWayland *display = GDKW_DISPLAY_WAYLAND (object);

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
}

static void
gdkw_display_wayland_init (GDKWDisplayWayland *display)
{
  display->wl_display = NULL;
  display->wl_registry = NULL;
  display->serial = 0;
}

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

  /* A roundtrip proves we reached the compositor and populates the
   * global registry.  M1 walks the registry for wl_shm/xdg_wm_base. */
  if (wl_display_roundtrip (wl_display) < 0)
    {
      g_object_unref (display);
      return NULL;
    }

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

cairo_surface_t *
_gdk_wayland_windowing_create_cairo_surface (GdkDrawable *drawable,
					     int width,
					     int height)
{
  return NULL;
}