/*
 * gdkscreen-wayland.c
 *
 * ydk-wayland fork: Wayland GdkScreen subclass.
 *
 * Wayland treats the whole display as one logical screen, so this object
 * carries the compositor geometry (taken from the wl_output found during
 * display open) and answers the core dispatch queries that used to be
 * hardcoded in the X11 backend.
 *
 * M1 status: width/height are populated from the wl_output mode; the root
 * window is NULL until window support lands (M2), which makes
 * gdk_window_new(..., NULL parent) fail cleanly.
 */

#include "config.h"

#include <wayland-client.h>

#include <ydk/gdk.h>
#include <ydk/gdkscreen.h>

#include "gdkprivate-wayland.h"

#ifdef GDK_COMPILATION
#include "gdkalias.h"
#endif

static void gdkw_screen_wayland_finalize (GObject *object);

G_DEFINE_TYPE (GDKWScreenWayland, gdkw_screen_wayland, GDK_TYPE_SCREEN)

static GdkDisplay *
gdkw_screen_wayland_get_display (GdkScreen *screen)
{
  return GDKW_SCREEN_WAYLAND (screen)->display;
}

static gint
gdkw_screen_wayland_get_width (GdkScreen *screen)
{
  return GDKW_SCREEN_WAYLAND (screen)->width;
}

static gint
gdkw_screen_wayland_get_height (GdkScreen *screen)
{
  return GDKW_SCREEN_WAYLAND (screen)->height;
}

static GdkWindow *
gdkw_screen_wayland_get_root_window (GdkScreen *screen)
{
  /* Root window arrives with window support (M2).  Without it,
   * gdk_window_new() rejects a NULL parent and window creation reports
   * unsupported (demo exit 77). */
  return NULL;
}

static void
gdkw_screen_wayland_finalize (GObject *object)
{
  GDKWScreenWayland *screen = GDKW_SCREEN_WAYLAND (object);

  screen->display = NULL;

  G_OBJECT_CLASS (gdkw_screen_wayland_parent_class)->finalize (object);
}

static void
gdkw_screen_wayland_class_init (GDKWScreenWaylandClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GdkScreenClass *screen_class = GDK_SCREEN_CLASS (klass);

  object_class->finalize = gdkw_screen_wayland_finalize;

  screen_class->get_display = gdkw_screen_wayland_get_display;
  screen_class->get_width = gdkw_screen_wayland_get_width;
  screen_class->get_height = gdkw_screen_wayland_get_height;
  screen_class->get_root_window = gdkw_screen_wayland_get_root_window;
}

static void
gdkw_screen_wayland_init (GDKWScreenWayland *screen)
{
  screen->display = NULL;
  screen->width = 0;
  screen->height = 0;
}

GdkScreen *
_gdk_wayland_screen_new (GdkDisplay *display,
			 gint        width,
			 gint        height)
{
  GDKWScreenWayland *screen;

  screen = g_object_new (GDKW_TYPE_SCREEN_WAYLAND, NULL);
  screen->display = display;
  screen->width = width;
  screen->height = height;

  return GDK_SCREEN (screen);
}