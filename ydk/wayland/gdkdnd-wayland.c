/*
 * gdkdnd-wayland.c
 *
 * ydk-wayland fork: Wayland backend drag-and-drop stubs (M4).
 *
 * The GdkDragContext GObject type (class, new/ref/unref, finalize) is a
 * single GType shared by both backends.  It is registered once, by the
 * X11 backend's G_DEFINE_TYPE in gdkdnd-x11.c, and is not duplicated
 * here: its init/finalize touch no Xlib for a context whose protocol is
 * not XDND, so Wayland contexts are created with gdk_drag_context_new()
 * and are refcountable like any other.
 *
 * Everything that actually drives a drag is per-backend.  Until real
 * wl_data_device support lands (M8), begin/motion/drop report "no drag
 * possible" without touching Xlib.  Symbols are namespaced
 * `_gdk_wayland_drag_*`; ydk/gdkbackend.c owns the canonical gdk_drag_*
 * names and forwards (including the destination-side gdk_drop_reply,
 * gdk_drop_finish and gdk_window_register_dnd, which would otherwise run
 * Xlib on a Wayland window).  See gdkprivate-wayland.h.
 */

#include "config.h"

#include <glib.h>

#include <ydk/gdk.h>
#include "gdkprivate-wayland.h"

#ifdef GDK_COMPILATION
#include "ydk/gdkalias.h"
#endif

GdkDisplay *
_gdk_wayland_drag_context_get_display (GdkDragContext *context)
{
  g_return_val_if_fail (context != NULL, NULL);

  if (context->source_window)
    return gdk_drawable_get_display (GDK_DRAWABLE (context->source_window));
  else if (context->dest_window)
    return gdk_drawable_get_display (GDK_DRAWABLE (context->dest_window));

  return NULL;
}

GdkDragContext *
_gdk_wayland_drag_begin (GdkWindow *window,
			 GList     *targets)
{
  g_return_val_if_fail (window != NULL, NULL);

  /* M8: create a wl_data_source and start wl_data_device.start_drag. */
  return NULL;
}

GdkNativeWindow
_gdk_wayland_drag_get_protocol_for_display (GdkDisplay      *display,
					    GdkNativeWindow  xid,
					    GdkDragProtocol *protocol)
{
  /* There are no foreign native windows to probe on Wayland. */
  if (protocol)
    *protocol = GDK_DRAG_PROTO_NONE;

  return 0;
}

void
_gdk_wayland_drag_find_window_for_screen (GdkDragContext  *context,
					  GdkWindow       *drag_window,
					  GdkScreen       *screen,
					  gint             x_root,
					  gint             y_root,
					  GdkWindow      **dest_window,
					  GdkDragProtocol *protocol)
{
  /* The compositor routes the drop target; the client cannot hit-test. */
  if (dest_window)
    *dest_window = NULL;
  if (protocol)
    *protocol = GDK_DRAG_PROTO_NONE;
}

gboolean
_gdk_wayland_drag_motion (GdkDragContext *context,
			  GdkWindow      *dest_window,
			  GdkDragProtocol protocol,
			  gint            x_root,
			  gint            y_root,
			  GdkDragAction   suggested_action,
			  GdkDragAction   possible_actions,
			  guint32         time)
{
  return FALSE;
}

void
_gdk_wayland_drag_drop (GdkDragContext *context,
			guint32         time)
{
  g_return_if_fail (context != NULL);
}

void
_gdk_wayland_drag_abort (GdkDragContext *context,
			 guint32         time)
{
  g_return_if_fail (context != NULL);
}

void
_gdk_wayland_drag_status (GdkDragContext *context,
			  GdkDragAction   action,
			  guint32         time)
{
  g_return_if_fail (context != NULL);
}

GdkAtom
_gdk_wayland_drag_get_selection (GdkDragContext *context)
{
  g_return_val_if_fail (context != NULL, GDK_NONE);

  return GDK_NONE;
}

gboolean
_gdk_wayland_drag_drop_succeeded (GdkDragContext *context)
{
  g_return_val_if_fail (context != NULL, FALSE);

  return FALSE;
}

/* Destination-side calls.  The X11 versions send XDND/Motif client messages
 * and set X properties on the native window, which would run Xlib on a
 * Wayland window, so they are dispatched here too.  Real wl_data_device
 * drop handling arrives with M8. */
void
_gdk_wayland_drop_reply (GdkDragContext *context,
			 gboolean        ok,
			 guint32         time)
{
  g_return_if_fail (context != NULL);
}

void
_gdk_wayland_drop_finish (GdkDragContext *context,
			  gboolean        success,
			  guint32         time)
{
  g_return_if_fail (context != NULL);
}

void
_gdk_wayland_window_register_dnd (GdkWindow *window)
{
  g_return_if_fail (window != NULL);

  /* Wayland needs no per-window drop-target registration; the data device
   * delivers enter/motion/drop to the surface under the pointer. */
}
