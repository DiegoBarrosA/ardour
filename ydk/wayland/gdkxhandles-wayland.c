/*
 * gdkxhandles-wayland.c
 *
 * ydk-wayland fork: Wayland half of the X11 handle accessor family
 * (M6b "x11-handle-accessors").
 *
 * The macros in x11/ydk/gdkx.h
 *
 *   GDK_DISPLAY_XDISPLAY (display)  -> gdk_x11_display_get_xdisplay
 *   GDK_WINDOW_XDISPLAY (win)       -> gdk_x11_drawable_get_xdisplay
 *                                      (gdk_x11_window_get_drawable_impl)
 *   GDK_WINDOW_XID / GDK_DRAWABLE_XID / GDK_WINDOW_XWINDOW
 *                                   -> gdk_x11_drawable_get_xid
 *
 * cast the display or the drawable impl to an X11 struct.  On a Wayland
 * display that cast lands on an unrelated object, so the X11 originals
 * return garbage.  The dispatchers in ydk/gdkbackend.c route these four
 * calls to the functions below when the Wayland backend is selected.
 * There is no X server behind a Wayland display, so:
 *
 *   _gdk_wayland_x11_display_get_xdisplay    NULL
 *   _gdk_wayland_x11_drawable_get_xdisplay   NULL
 *   _gdk_wayland_x11_drawable_get_xid        0 (None)
 *   _gdk_wayland_x11_window_get_drawable_impl  the impl pointer, as on X11
 *
 * None of them emits a CRITICAL or a warning, for any argument including
 * NULL, a pixmap or an unmapped window.
 *
 * Returning NULL does not make Xlib calls safe: a NULL Display handed to
 * XGrabKey and friends still crashes.  Callers must test first, so this
 * file also exports a runtime predicate that ytk and applications can use:
 *
 *   gboolean gdk_x11_display_is_x11 (GdkDisplay *display);
 *
 * It is TRUE only for a live X11 display (NULL gives FALSE).  Declare it in
 * x11/ydk/gdkx.h next to gdk_x11_display_get_xdisplay.
 *
 * The prototypes return gpointer / gulong instead of Display * / XID on
 * purpose: this file has no Xlib.  The dispatchers assign the gpointer to
 * Display * with no cast, and gulong is what XID is.
 *
 * Naming: `_gdk_wayland_x11_*` (public name minus gdk_, see the audit script),
 * prototypes in gdkprivate-wayland.h, no alias blocks.  gdk_x11_display_is_x11 is a
 * new public name with no IA__ alias.
 */

#include "config.h"

#include <glib.h>
#include <glib-object.h>

#include <ydk/gdk.h>
#include <ydk/gdkinternals.h>

#include "gdkprivate-wayland.h"

gpointer
_gdk_wayland_x11_display_get_xdisplay (GdkDisplay *display)
{
  (void) display;
  return NULL;
}

gpointer
_gdk_wayland_x11_drawable_get_xdisplay (GdkDrawable *drawable)
{
  (void) drawable;
  return NULL;
}

gulong
_gdk_wayland_x11_drawable_get_xid (GdkDrawable *drawable)
{
  (void) drawable;
  return 0;
}

GdkDrawable *
_gdk_wayland_x11_window_get_drawable_impl (GdkWindow *window)
{
  if (window == NULL || !GDK_IS_WINDOW (window))
    return NULL;

  return ((GdkWindowObject *) window)->impl;
}

gboolean
gdk_x11_display_is_x11 (GdkDisplay *display)
{
  if (display == NULL || !GDK_IS_DISPLAY (display))
    return FALSE;

  return !GDKW_IS_DISPLAY_WAYLAND (display);
}
