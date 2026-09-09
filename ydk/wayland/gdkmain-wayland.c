/*
 * gdkmain-wayland.c
 *
 * ydk-wayland fork: Wayland backend hooks that mirror the X11
 * `_gdk_windowing_*` contract, namespaced `_gdk_wayland_*` so both
 * backends can live in one libydk.  See gdkprivate-wayland.h and
 * ydk/gdkbackend.c.
 */

#include "config.h"

#include <glib.h>

#include <ydk/gdk.h>
#include "gdkprivate-wayland.h"

#ifdef GDK_COMPILATION
#include "ydk/gdkalias.h"
#endif

const GOptionEntry _gdk_wayland_windowing_args[] = {
  /* Wayland-specific command line options land here (M1+). */
  { NULL }
};

void
_gdk_wayland_windowing_init (void)
{
  /* No global setup required yet.  M1 attaches a GSource for the
   * wl_display fd and registers xkbcommon state here. */
}

void
_gdk_wayland_windowing_exit (void)
{
}

gchar *
_gdk_wayland_windowing_substitute_screen_number (const gchar *display_name,
						 gint         screen_number)
{
  /* Wayland has no screen numbers. */
  return g_strdup (display_name);
}

gulong
_gdk_wayland_windowing_window_get_next_serial (GdkDisplay *display)
{
  GDKWDisplayWayland *wayland_display = GDKW_DISPLAY_WAYLAND (display);

  return ++wayland_display->serial;
}