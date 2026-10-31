/*
 * gdkreparent-wayland.c
 *
 * ydk-wayland fork, M9 (piece "reparent"): gdk_window_reparent () on Wayland.
 *
 * Three transitions (see ydk/gdkm9-wayland.h, D14):
 *
 *   child -> child     core only, the backend is not called.
 *   child -> toplevel  the core makes a native impl (role NONE while the type
 *                      is still CHILD) and calls iface->reparent, which is
 *                      _gdk_wayland_reparent () here.  It breaks the grabs of
 *                      the subtree and stores the root position.  Returning
 *                      was_mapped makes the core show the window AFTER the
 *                      type became TOPLEVEL, so the xdg objects appear then.
 *   toplevel / popup -> child
 *                      the core calls iface->demote_to_child, which is
 *                      _gdk_wayland_reparent_demote () here.  It breaks the
 *                      grabs, releases the popups of the family and destroys
 *                      the role objects of the impl.  The core then hides the
 *                      window and moves it to the impl of the new parent.
 *
 * Also here: the reparent counters read by the demo through
 * _gdk_wayland_test_reparent_counts ().
 *
 * No Xlib.  No protocol request of xdg_popup / xdg_positioner (popup piece).
 */

#include "config.h"

#include <string.h>

#include <wayland-client.h>

#include <ydk/gdk.h>
#include <ydk/gdkinternals.h>

#include "gdkprivate-wayland.h"
#include "gdkseat-wayland.h"
#include "gdkm7-wayland.h"
#include "gdkm9-wayland.h"

/* ------------------------------------------------------------------ */
/* Counters                                                           */
/* ------------------------------------------------------------------ */

#define GDKW_REPARENT_COUNTS_KEY "gdkw-reparent-counts"

/* The counters of @display, created zeroed on first use.  Owned by the
 * display as object data. */
static GDKWTestReparentCounts *
reparent_counts_get (GdkDisplay *display)
{
  GDKWTestReparentCounts *c;

  if (display == NULL)
    return NULL;

  c = g_object_get_data (G_OBJECT (display), GDKW_REPARENT_COUNTS_KEY);
  if (c == NULL)
    {
      c = g_new0 (GDKWTestReparentCounts, 1);
      g_object_set_data_full (G_OBJECT (display), GDKW_REPARENT_COUNTS_KEY,
			      c, g_free);
    }
  return c;
}

gboolean
_gdk_wayland_test_reparent_counts (GdkDisplay             *display,
				   GDKWTestReparentCounts *counts)
{
  GDKWTestReparentCounts *c;

  if (display == NULL || !GDKW_IS_DISPLAY_WAYLAND (display) || counts == NULL)
    return FALSE;

  c = reparent_counts_get (display);
  *counts = *c;
  return TRUE;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

/* The Wayland display of a live window, else NULL. */
static GdkDisplay *
window_display (GdkWindow *window)
{
  GdkDisplay *display;

  if (window == NULL || !GDK_IS_WINDOW (window))
    return NULL;

  display = gdk_drawable_get_display (GDK_DRAWABLE (window));
  if (display == NULL || !GDKW_IS_DISPLAY_WAYLAND (display))
    return NULL;
  return display;
}

/* TRUE if @window is @ancestor or lies below it. */
static gboolean
is_same_or_descendant (GdkWindow *ancestor,
		       GdkWindow *window)
{
  GdkWindowObject *tmp = (GdkWindowObject *) window;
  guint depth = 0;

  while (tmp != NULL && depth++ < 4096)
    {
      if ((GdkWindow *) tmp == ancestor)
	return TRUE;
      tmp = tmp->parent;
    }
  return FALSE;
}

/* ------------------------------------------------------------------ */
/* Grabs                                                              */
/* ------------------------------------------------------------------ */


void
_gdk_wayland_reparent_break_grabs (GdkWindow *window)
{
  GdkDisplay *display = window_display (window);
  GDKWSeat *seat;
  GdkPointerGrabInfo *pgrab;
  GDKWTestReparentCounts *counts;
  gulong serial;

  if (display == NULL || GDK_WINDOW_DESTROYED (window))
    return;

  seat = _gdk_wayland_display_get_seat (display);
  counts = reparent_counts_get (display);
  serial = _gdk_windowing_window_get_next_serial (display);

  /* Pointer grab: same calls as grab_check_unmap () in the impl file, so the
   * core queues GDK_GRAB_BROKEN (grab_window NULL) for ytk. */
  /* Count a grab only when it is live: one that already has a serial_end
   * is waiting for pointer_grab_update and is not broken by this call. */
  pgrab = _gdk_display_get_last_pointer_grab (display);
  if (pgrab != NULL && pgrab->window != NULL &&
      pgrab->serial_end == G_MAXULONG &&
      is_same_or_descendant (window, pgrab->window))
    counts->grabs_broken++;

  _gdk_display_end_pointer_grab (display, serial, window, TRUE);
  _gdk_display_pointer_grab_update (display, serial);

  /* Keyboard grab. */
  if (display->keyboard_grab.window != NULL &&
      serial >= display->keyboard_grab.serial &&
      is_same_or_descendant (window, display->keyboard_grab.window))
    {
      _gdk_display_unset_has_keyboard_grab (display, TRUE);
      counts->grabs_broken++;
    }

  /* The backend's own record of the explicit grab.  seat->grab.native is
   * the TOPLEVEL of the grab window, an ancestor of a client side child that
   * is being promoted, so test the grab window itself as well: otherwise the
   * record (window reference, grab cursor) outlives the core grab. */
  if (seat != NULL && seat->grab.active &&
      ((seat->grab.native != NULL &&
        is_same_or_descendant (window, seat->grab.native)) ||
       (seat->grab.window != NULL &&
        is_same_or_descendant (window, seat->grab.window))))
    _gdk_wayland_seat_grab_clear (seat);
}

/* ------------------------------------------------------------------ */
/* iface->reparent: promotion and toplevel / popup move               */
/* ------------------------------------------------------------------ */

gboolean
_gdk_wayland_reparent (GdkWindow *window,
		       GdkWindow *new_parent,
		       gint       x,
		       gint       y)
{
  GdkDisplay *display;
  GDKWWindowImplWayland *impl;
  GDKWTestReparentCounts *counts;
  gboolean was_mapped;
  gboolean promoting;

  if (window == NULL || !GDK_IS_WINDOW (window) || GDK_WINDOW_DESTROYED (window))
    return FALSE;

  was_mapped = GDK_WINDOW_IS_MAPPED (window);

  impl = _gdk_wayland_window_get_impl (window);
  display = window_display (window);
  if (impl == NULL || display == NULL)
    return was_mapped;

  counts = reparent_counts_get (display);

  /* The type is still CHILD while a client side child is being promoted
   * (the core switches it after this call). */
  promoting = (GDK_WINDOW_TYPE (window) == GDK_WINDOW_CHILD);

  if (promoting)
    {
      /* Conservative: a grab that belongs to the subtree does not survive. */
      _gdk_wayland_reparent_break_grabs (window);
    }
  else
    {
      /* A toplevel or popup that is moved: only a popup can follow the new
       * position, a plain xdg_toplevel ignores it (root space is the surface
       * space of the base toplevel). */
      if (_gdk_wayland_popup_root_origin (impl, NULL, NULL))
	_gdk_wayland_popup_move_resize (window, TRUE, x, y,
					(gint) impl->width, (gint) impl->height);
      counts->moves++;
    }

  /* The root position the window will have; the core stores it in the
   * GdkWindow right after this call. */
  impl->x = x;
  impl->y = y;

  if (promoting)
    counts->promotions++;

  /* Cursor state lives on the impl: apply the effective cursor again now
   * that the grabs are gone. */
  _gdk_wayland_cursor_update (display);

  /* TRUE makes the core show the window again once its type is final. */
  return was_mapped;
}

/* ------------------------------------------------------------------ */
/* iface->demote_to_child                                             */
/* ------------------------------------------------------------------ */

gboolean
_gdk_wayland_reparent_demote (GdkWindow *window,
			      GdkWindow *new_parent)
{
  GdkDisplay *display;
  GDKWDisplayWayland *wd;
  GDKWWindowImplWayland *impl;
  GdkWindowObject *np;

  if (window == NULL || new_parent == NULL ||
      !GDK_IS_WINDOW (window) || !GDK_IS_WINDOW (new_parent) ||
      GDK_WINDOW_DESTROYED (window) || GDK_WINDOW_DESTROYED (new_parent))
    return FALSE;

  np = (GdkWindowObject *) new_parent;
  if (np->window_type == GDK_WINDOW_ROOT ||
      np->window_type == GDK_WINDOW_FOREIGN)
    return FALSE;

  impl = _gdk_wayland_window_get_impl (window);
  display = window_display (window);
  if (impl == NULL || display == NULL)
    return FALSE;
  wd = GDKW_DISPLAY_WAYLAND (display);

  /* 1. grabs of the subtree: GRAB_BROKEN reaches ytk before anything hides */
  _gdk_wayland_reparent_break_grabs (window);

  /* 2. popups above this window go first (xdg-shell: a parent outlives its
   *    popups) */
  _gdk_wayland_popup_release_children (impl, TRUE);

  /* 3. the role objects of the window.  A popup or surfaceless window is
   *    released by the popup piece; a plain xdg_toplevel by the shared
   *    teardown.  Both are idempotent, so the hide the core does next, and
   *    the finalize of the old impl, find nothing left to do. */
  if (!_gdk_wayland_popup_hide (window))
    {
      if (impl->wl_surface != NULL)
	_gdk_wayland_seat_surface_gone (wd, impl);
      _gdk_wayland_window_impl_teardown (impl);
    }

  reparent_counts_get (display)->demotions++;

  _gdk_wayland_cursor_update (display);

  return TRUE;
}
