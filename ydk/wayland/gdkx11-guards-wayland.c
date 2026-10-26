/*
 * gdkx11-guards-wayland.c
 *
 * ydk-wayland fork, M6b: the Wayland half of four X11-only public
 * functions that ytk calls from #ifdef GDK_WINDOWING_X11 blocks.
 *
 * gdkconfig.h defines GDK_WINDOWING_X11 and GDK_WINDOWING_WAYLAND together,
 * so those blocks compile in and also run on a Wayland display.  The X11
 * originals cast the display or window to an X11 type with no runtime check
 * and then call Xlib.  On a Wayland display that is a crash or a hang.
 *
 *   gdk_x11_display_set_cursor_theme  settings_update_cursor_theme, hit from
 *                                     gtk_settings_get_for_screen before any
 *                                     window is realized
 *   gdk_x11_window_set_user_time      gtk_window_realize (startup id) and
 *                                     gtk_window_set_startup_id
 *   gdk_x11_display_get_user_time     gtk_window_present_with_time
 *   gdk_x11_get_server_time           clipboard set path, when the event
 *                                     time is GDK_CURRENT_TIME
 *
 * Behaviour on Wayland
 *
 *   set_cursor_theme  No effect.  The Wayland cursor theme belongs to M7
 *                     (gdkcursor-wayland.c).  This is the single place that
 *                     can forward theme and size there later.
 *   set_user_time     No effect.  xdg-shell has no user time.  The
 *                     activation token path is the Wayland equivalent.
 *   get_user_time     The ms timestamp of the newest seat event (GDKWSeat
 *                     last_time), or GDK_CURRENT_TIME (0) before any input.
 *                     gtk_window_present_with_time passes it on to
 *                     gdk_window_focus, which ignores it.
 *   get_server_time   The same seat timestamp, or 0.  There is no server
 *                     round trip: the X11 version waits in XIfEvent.
 *
 * Rules: a bad argument (NULL, destroyed window, display or window of
 * another backend) returns the safe value silently.  There is never a
 * g_critical.  The X11 versions use g_return_if_fail on some of these; the
 * demo makes CRITICAL fatal, so this file validates instead.  No Xlib.
 *
 * Why the runtime check: the dispatchers in gdkbackend.c select by backend,
 * but a caller may still hold an object of the other backend (for example a
 * stale X11 display).  The explicit GDKW_IS_DISPLAY_WAYLAND and impl type
 * tests keep those calls harmless.
 *
 * Naming: every definition is `_gdk_wayland_x11_*` (the public name minus
 * the gdk_ prefix, which is what scripts/audit-x11-symbols.sh derives), the
 * target of the dispatchers in gdkbackend.c.  The prototypes are in
 * gdkprivate-wayland.h.  There are no alias blocks in this file.
 */

#include "config.h"

#include <glib.h>
#include <glib-object.h>

#include <ydk/gdk.h>
#include <ydk/gdkinternals.h>

#include "gdkprivate-wayland.h"
#include "gdkseat-wayland.h"

/* ---- helpers ---- */

/* Newest seat event time of @display in ms, 0 (GDK_CURRENT_TIME) if the seat
 * has seen no input yet or @display is not a live Wayland display. */
static guint32
x11_guard_last_time (GdkDisplay *display)
{
  GDKWSeat *seat;

  if (display == NULL || !GDKW_IS_DISPLAY_WAYLAND (display))
    return GDK_CURRENT_TIME;
  seat = _gdk_wayland_display_get_seat (display);
  return seat != NULL ? seat->last_time : GDK_CURRENT_TIME;
}

/* ---- gdk_x11_display_set_cursor_theme ---- */

void
_gdk_wayland_x11_display_set_cursor_theme (GdkDisplay  *display,
                                           const gchar *theme,
                                           const gint   size)
{
  /* Nothing to do yet.  M7 can forward @theme and @size to the Wayland
   * cursor code from here.  XcursorSetTheme must not run: there is no X
   * Display behind this object. */
  (void) display;
  (void) theme;
  (void) size;
}

/* ---- gdk_x11_window_set_user_time ---- */

void
_gdk_wayland_x11_window_set_user_time (GdkWindow *window,
                                       guint32    timestamp)
{
  /* xdg-shell has no user time property.  A stale, destroyed or foreign
   * window is ignored the same way as a valid one. */
  (void) window;
  (void) timestamp;
}

/* ---- gdk_x11_display_get_user_time ---- */

guint32
_gdk_wayland_x11_display_get_user_time (GdkDisplay *display)
{
  return x11_guard_last_time (display);
}

/* ---- gdk_x11_get_server_time ---- */

guint32
_gdk_wayland_x11_get_server_time (GdkWindow *window)
{
  GdkDisplay *display;

  /* Only a live Wayland window has a time (the root window of a Wayland
   * screen has a Wayland impl too); NULL, a destroyed window, a pixmap and an
   * X11 window give GDK_CURRENT_TIME. */
  if (_gdk_wayland_window_get_impl (window) == NULL)
    return GDK_CURRENT_TIME;

  display = gdk_drawable_get_display (GDK_DRAWABLE (window));
  return x11_guard_last_time (display);
}
