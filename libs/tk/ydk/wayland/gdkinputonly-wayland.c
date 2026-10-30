/*
 * gdkinputonly-wayland.c
 *
 * ydk-wayland fork, M9 (piece "inputonly"): input-only windows and the
 * pointer position model on Wayland.
 *
 * What lives here (see ydk/wayland/ydk/gdkm9-wayland.h, D1, D2, D13, D15):
 *
 *   - the "surfaceless" predicate: a window that must never own a wl_surface
 *     (input-only, native CHILD, DND hint) and the grab viewability rule
 *     built on it;
 *   - the pointer position in root space and in the coordinates of any
 *     window, native or client side;
 *   - root coordinates for events;
 *   - the two headless hooks that move the pointer to a point of any window.
 *
 * No crossing code is written here: the core already turns the toplevel
 * ENTER / MOTION into crossings on client side children (F3).
 *
 * No Xlib.  The functions are internal or test hooks; none is dispatched by
 * ydk/gdkbackend.c.
 */

#include "config.h"

#include <ydk/gdk.h>
#include <ydk/gdkinternals.h>

#include "gdkprivate-wayland.h"
#include "gdkseat-wayland.h"
#include "gdkm7-wayland.h"
#include "gdkm9-wayland.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

/* The Wayland display behind @display, or NULL. */
static GDKWDisplayWayland *
io_display (GdkDisplay *display)
{
  if (display == NULL || !GDKW_IS_DISPLAY_WAYLAND (display))
    return NULL;
  return GDKW_DISPLAY_WAYLAND (display);
}

/* The Wayland impl of a live window wrapper, else NULL. */
static GDKWWindowImplWayland *
io_live_impl (GdkWindow *window)
{
  GDKWWindowImplWayland *impl;

  if (window == NULL || !GDK_IS_WINDOW (window))
    return NULL;
  if (GDK_WINDOW_DESTROYED (window))
    return NULL;
  impl = _gdk_wayland_window_get_impl (window);
  return impl;
}

/* ------------------------------------------------------------------ */
/* Surfaceless rule (D1 rules 1 to 3) and grab viewability (D15)      */
/* ------------------------------------------------------------------ */

gboolean
_gdk_wayland_inputonly_surfaceless (GdkWindow *window)
{
  GdkWindowObject *private;

  if (io_live_impl (window) == NULL)
    return FALSE;

  private = (GdkWindowObject *) window;

  /* A client side child shares the impl of an ancestor and owns nothing:
   * the predicate is about native windows only. */
  if (private->impl_window != private)
    return FALSE;

  /* 1. input-only: the GTK grab transfer window, GtkInvisible */
  if (private->input_only)
    return TRUE;

  /* 2. native child: no wl_subsurface in M9 */
  if (private->window_type == GDK_WINDOW_CHILD)
    return TRUE;

  /* 3. DND icon window, whatever its window type (matches
   * _gdk_wayland_window_choose_role ()) */
  if (_gdk_wayland_window_get_type_hint (window) == GDK_WINDOW_TYPE_HINT_DND)
    return TRUE;

  return FALSE;
}

gboolean
_gdk_wayland_inputonly_grab_viewable (GdkWindow *native)
{
  return _gdk_wayland_inputonly_surfaceless (native);
}

/* ------------------------------------------------------------------ */
/* Pointer position (D2, D13)                                         */
/* ------------------------------------------------------------------ */

void
_gdk_wayland_pointer_root_position (GdkDisplay *display,
                                    gint *root_x, gint *root_y)
{
  gdouble rx = 0.0, ry = 0.0;
  GDKWSeat *seat;
  gint ox = 0, oy = 0;

  if (io_display (display) != NULL)
    {
      seat = _gdk_wayland_display_get_seat (display);

      if (seat->last_pointer_impl != NULL)
        _gdk_wayland_popup_root_origin (seat->last_pointer_impl, &ox, &oy);

      rx = seat->px + ox;
      ry = seat->py + oy;
    }

  if (root_x)
    *root_x = (gint) rx;
  if (root_y)
    *root_y = (gint) ry;
}

/* Root position as doubles, same rule as above. */
static void
io_root_position_d (GDKWDisplayWayland *wd, gdouble *rx, gdouble *ry)
{
  GDKWSeat *seat = _gdk_wayland_display_get_seat (GDK_DISPLAY_OBJECT (wd));
  gint ox = 0, oy = 0;

  if (seat->last_pointer_impl != NULL)
    _gdk_wayland_popup_root_origin (seat->last_pointer_impl, &ox, &oy);

  *rx = seat->px + ox;
  *ry = seat->py + oy;
}

gboolean
_gdk_wayland_pointer_in_window (GdkDisplay *display,
                                GdkWindow  *window,
                                gdouble *x, gdouble *y)
{
  GDKWDisplayWayland *wd = io_display (display);
  GdkWindowObject *private;
  gdouble rx = 0.0, ry = 0.0, fx, fy;
  gint wx = 0, wy = 0;

  if (x)
    *x = 0.0;
  if (y)
    *y = 0.0;

  if (wd == NULL)
    return FALSE;

  io_root_position_d (wd, &rx, &ry);

  if (window == NULL || !GDK_IS_WINDOW (window) ||
      GDK_WINDOW_DESTROYED (window))
    {
      if (x)
        *x = rx;
      if (y)
        *y = ry;
      return FALSE;
    }

  /* Origin of the window in root space; the impl vfunc adds the popup
   * origin (I8g). */
  gdk_window_get_origin (window, &wx, &wy);

  fx = rx - wx;
  fy = ry - wy;

  if (x)
    *x = fx;
  if (y)
    *y = fy;

  private = (GdkWindowObject *) window;

  return (fx >= 0.0 && fy >= 0.0 &&
          fx < (gdouble) private->width && fy < (gdouble) private->height);
}

void
_gdk_wayland_event_root_coords (GDKWWindowImplWayland *impl,
                                gdouble x, gdouble y,
                                gdouble *x_root, gdouble *y_root)
{
  gint ox = 0, oy = 0;

  if (impl != NULL)
    _gdk_wayland_popup_root_origin (impl, &ox, &oy);

  if (x_root)
    *x_root = x + ox;
  if (y_root)
    *y_root = y + oy;
}

/* ------------------------------------------------------------------ */
/* Test hooks                                                         */
/* ------------------------------------------------------------------ */

gboolean
_gdk_wayland_test_pointer_move_to_window (GdkDisplay *display,
                                          guint32     time,
                                          GdkWindow  *window,
                                          gdouble     x,
                                          gdouble     y)
{
  GDKWDisplayWayland *wd = io_display (display);
  GdkWindowObject *private;
  GDKWWindowImplWayland *impl;
  GDKWSeat *seat;
  GdkWindow *native;
  gdouble nx = x, ny = y;
  guint depth = 0;

  if (wd == NULL || io_live_impl (window) == NULL)
    return FALSE;

  /* Convert to the coordinates of the native window by summing the
   * offsets up to the impl window. */
  private = (GdkWindowObject *) window;
  while (private != NULL && private->impl_window != private)
    {
      if (++depth > 1024 || private->parent == NULL)
        return FALSE;
      nx += private->x;
      ny += private->y;
      private = (GdkWindowObject *) private->parent;
    }
  native = (GdkWindow *) private;

  impl = io_live_impl (native);
  if (impl == NULL || impl->wl_surface == NULL)
    return FALSE;

  seat = _gdk_wayland_display_get_seat (display);

  if (seat->pointer_focus == impl)
    return _gdk_wayland_test_pointer_motion (display, time, nx, ny);

  if (seat->pointer_focus != NULL && seat->pointer_focus->wrapper != NULL)
    _gdk_wayland_test_pointer_leave (display, seat->pointer_focus->wrapper);

  return _gdk_wayland_test_pointer_enter (display, native, nx, ny);
}

gboolean
_gdk_wayland_test_pointer_root (GdkDisplay  *display,
                                gint        *root_x,
                                gint        *root_y,
                                GdkWindow  **focus_native)
{
  GDKWSeat *seat;

  if (io_display (display) == NULL)
    return FALSE;

  seat = _gdk_wayland_display_get_seat (display);

  _gdk_wayland_pointer_root_position (display, root_x, root_y);

  if (focus_native)
    *focus_native = seat->pointer_focus != NULL ? seat->pointer_focus->wrapper
                                                : NULL;
  return TRUE;
}
