/*
 * gdkdnd-source-wayland.c
 *
 * ydk-wayland fork, M8: Wayland drag and drop, the source half.  The context
 * type and the destination half are in gdkdnd-wayland.c.
 *
 * ytk drives a drag with gdk_drag_begin, then gdk_drag_find_window_for_screen
 * and gdk_drag_motion.  On Wayland the compositor owns the pointer once
 * start_drag is sent, so the only terminator ytk ever sees is a
 * GDK_DROP_FINISHED event.  gdk_drag_begin always returns a context; when the
 * drag cannot start the context is FAILED and a GDK_DROP_FINISHED is queued.
 * See gdkm8-wayland.h, DECISIONS: DRAG SOURCE.
 *
 * This file never calls a wl_data_* request.  Every request goes through the
 * _gdk_wayland_dd_* functions of the datadevice piece.
 */

#include "config.h"

#include <string.h>
#include <unistd.h>

#include <glib.h>

#include <wayland-client.h>

#include <ydk/gdk.h>
#include "gdkprivate-wayland.h"
#include "gdkseat-wayland.h"
#include "gdkm8-wayland.h"

#ifdef GDK_COMPILATION
#include "ydk/gdkalias.h"
#endif


/* Hook state: the last source context (weak) and a snapshot of its flags that
 * survives the context. */
static GDKWDragContext      *last_ctx = NULL;
static gboolean              snap_valid = FALSE;
static GDKWTestDndSourceInfo snap;
static guint                 src_events_queued = 0;

static GDKWDataDevice *
src_dd (GdkDisplay *display)
{
  if (display == NULL || !GDKW_IS_DISPLAY_WAYLAND (display))
    return NULL;

  return _gdk_wayland_display_get_data_device (display);
}

static gboolean
src_window_alive (GdkWindow *window)
{
  return window != NULL && GDK_IS_WINDOW (window) &&
         !gdk_window_is_destroyed (window);
}

/* A source context of ours, or NULL. */
static GDKWDragContext *
src_ctx (GdkDragContext *context)
{
  if (context == NULL || !GDKW_IS_DRAG_CONTEXT (context) || !context->is_source)
    return NULL;

  return GDKW_DRAG_CONTEXT (context);
}

static void
src_fill_info (const GDKWDragContext *ctx, GDKWTestDndSourceInfo *info)
{
  memset (info, 0, sizeof (*info));
  info->active = ctx->state != GDKW_DRAG_DONE;
  info->context = (GdkDragContext *) ctx;
  info->started = ctx->started;
  info->failed = ctx->failed;
  info->finished = ctx->succeeded;
  info->cancelled = ctx->state == GDKW_DRAG_DONE && !ctx->failed &&
                    !ctx->succeeded;
  info->succeeded = ctx->succeeded;
  info->local_dest = ctx->local_dest;
  info->delete_requested = ctx->delete_requested;
  info->events_queued = src_events_queued;
}

static void
src_track (GDKWDragContext *ctx)
{
  if (last_ctx != NULL)
    g_object_remove_weak_pointer (G_OBJECT (last_ctx), (gpointer *) &last_ctx);
  last_ctx = ctx;
  if (last_ctx != NULL)
    g_object_add_weak_pointer (G_OBJECT (last_ctx), (gpointer *) &last_ctx);
}

static void
src_snapshot (GDKWDragContext *ctx)
{
  src_fill_info (ctx, &snap);
  snap.context = NULL;
  snap_valid = TRUE;
}

static void
src_put_event (GDKWDragContext *ctx, GdkEventType type)
{
  GdkWindow *window = GDK_DRAG_CONTEXT (ctx)->source_window;
  GDKWSeat  *seat;
  guint32    time = GDK_CURRENT_TIME;

  if (!src_window_alive (window) || ctx->display == NULL ||
      !GDKW_IS_DISPLAY_WAYLAND (ctx->display))
    return;

  seat = _gdk_wayland_display_get_seat (ctx->display);
  if (seat->last_time != 0)
    time = seat->last_time;

  _gdk_wayland_dnd_put_event (type, window, ctx, time, 0, 0);
  src_events_queued++;
}

/* Ends the drag without an event and keeps the snapshot. */
static void
src_done (GDKWDragContext *ctx)
{
  g_object_ref (ctx);
  _gdk_wayland_dnd_source_end (ctx);
  src_snapshot (ctx);
  g_object_unref (ctx);
}

/* The drag could not start: queue DROP_FINISHED once and end it. */
static void
src_fail (GDKWDragContext *ctx)
{
  if (ctx->state == GDKW_DRAG_DONE && ctx->failed)
    return;

  g_object_ref (ctx);
  ctx->failed = TRUE;
  GDK_DRAG_CONTEXT (ctx)->action = 0;
  src_put_event (ctx, GDK_DROP_FINISHED);
  src_done (ctx);
  g_object_unref (ctx);
}

static gboolean
src_start (GDKWDragContext *ctx)
{
  GDKWDataDevice        *dd;
  GDKWSeat              *seat;
  GDKWWindowImplWayland *impl;
  GdkDragAction          actions;
  guint32                mask;
  gboolean               ok = FALSE;

  if (ctx->started)
    return TRUE;
  if (ctx->state != GDKW_DRAG_NEW)
    return FALSE;

  g_object_ref (ctx);

  if (ctx->start_timeout != 0)
    {
      g_source_remove (ctx->start_timeout);
      ctx->start_timeout = 0;
    }

  dd = src_dd (ctx->display);
  if (dd != NULL && ctx->source != NULL)
    {
      seat = _gdk_wayland_display_get_seat (ctx->display);
      impl = seat->pointer_focus != NULL ? seat->pointer_focus
                                         : seat->last_pointer_impl;

      if (seat->button_mask != 0 && seat->button_serial != 0 &&
          impl != NULL && impl->wl_surface != NULL &&
          src_window_alive (impl->wrapper))
        {
          actions = GDK_DRAG_CONTEXT (ctx)->actions;
          if (actions == 0)
            actions = GDK_ACTION_COPY | GDK_ACTION_MOVE;
          mask = _gdk_wayland_dnd_actions_to_wl (actions);
          if (mask == 0)
            mask = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY |
                   WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;

          _gdk_wayland_dd_source_set_actions (ctx->source, mask);
          ok = _gdk_wayland_dd_start_drag (dd, ctx->source, impl->wrapper,
                                           seat->button_serial);
        }
    }

  if (ok)
    {
      ctx->started = TRUE;
      ctx->state = GDKW_DRAG_STARTED;
    }
  else
    src_fail (ctx);

  g_object_unref (ctx);

  return ok;
}

static gboolean
src_start_cb (gpointer data)
{
  GDKWDragContext *ctx = data;

  ctx->start_timeout = 0;
  g_object_ref (ctx);
  src_start (ctx);
  g_object_unref (ctx);

  return G_SOURCE_REMOVE;
}

/* ---- source handlers (user_data is the GDKWDragContext) ------------------- */

static void
h_target (GDKWSource *src, const gchar *mime)
{
  GDKWDragContext *ctx = src->user_data;
  GDKWDataDevice  *dd;
  gboolean         has = mime != NULL;
  guint            version;

  if (ctx == NULL || ctx->state == GDKW_DRAG_DONE)
    return;

  dd = src_dd (ctx->display);
  version = dd == NULL ? 0 : (dd->virtual_device ? 3 : dd->manager_version);

  /* sent_valid / sent_action are unused on a source context: they remember
   * whether a target was seen and whether it was non NULL */
  if (ctx->sent_valid && (ctx->sent_action != 0) == has)
    return;
  ctx->sent_valid = TRUE;
  ctx->sent_action = has ? GDK_ACTION_COPY : 0;

  if (version < 3)
    {
      g_object_ref (ctx);
      GDK_DRAG_CONTEXT (ctx)->action = has ? GDK_ACTION_COPY : 0;
      src_put_event (ctx, GDK_DRAG_STATUS);
      g_object_unref (ctx);
    }
}

static void
h_send (GDKWSource *src, const gchar *mime, int fd)
{
  GDKWDragContext *ctx = src->user_data;

  if (ctx == NULL || ctx->state == GDKW_DRAG_DONE || ctx->source_map == NULL ||
      mime == NULL)
    {
      if (fd >= 0)
        close (fd);
      return;
    }

  _gdk_wayland_selection_serve (src, _gdk_wayland_selection_dnd_atom (),
                                ctx->source_map, mime, fd);
}

static void
h_cancelled (GDKWSource *src)
{
  GDKWDragContext *ctx = src->user_data;

  if (ctx == NULL || ctx->state == GDKW_DRAG_DONE)
    return;

  g_object_ref (ctx);
  ctx->succeeded = FALSE;
  GDK_DRAG_CONTEXT (ctx)->action = 0;
  src_put_event (ctx, GDK_DROP_FINISHED);
  src_done (ctx);
  g_object_unref (ctx);
}

static void
h_drop_performed (GDKWSource *src)
{
  GDKWDragContext *ctx = src->user_data;

  if (ctx == NULL || ctx->state == GDKW_DRAG_DONE)
    return;

  ctx->drop_performed = TRUE;
  ctx->state = GDKW_DRAG_DROPPED;
}

static void
h_finished (GDKWSource *src)
{
  GDKWDragContext *ctx = src->user_data;

  if (ctx == NULL || ctx->state == GDKW_DRAG_DONE)
    return;

  g_object_ref (ctx);

  ctx->succeeded = TRUE;

  /* A MOVE with a destination outside this process: the destination cannot ask
   * for DELETE, so do what an X11 destination does and ask ourselves.  With a
   * local destination ytk deletes through its in process shortcut. */
  if (GDK_DRAG_CONTEXT (ctx)->action == GDK_ACTION_MOVE && !ctx->local_dest &&
      !ctx->delete_requested && ctx->display != NULL)
    {
      ctx->delete_requested = TRUE;
      _gdk_wayland_selection_discard_request (ctx->display,
                                              _gdk_wayland_selection_dnd_atom (),
                                              gdk_atom_intern ("DELETE", FALSE));
    }

  src_put_event (ctx, GDK_DROP_FINISHED);
  src_done (ctx);

  g_object_unref (ctx);
}

static void
h_action (GDKWSource *src, guint32 wl_action)
{
  GDKWDragContext *ctx = src->user_data;
  GdkDragAction    action;

  if (ctx == NULL || ctx->state == GDKW_DRAG_DONE)
    return;

  action = _gdk_wayland_dnd_actions_from_wl (wl_action);

  if (ctx->got_action && GDK_DRAG_CONTEXT (ctx)->action == action)
    return;

  g_object_ref (ctx);
  ctx->got_action = TRUE;
  GDK_DRAG_CONTEXT (ctx)->action = action;
  src_put_event (ctx, GDK_DRAG_STATUS);
  g_object_unref (ctx);
}

static const GDKWSourceHandlers dnd_source_handlers =
{
  h_target,
  h_send,
  h_cancelled,
  h_drop_performed,
  h_finished,
  h_action
};

const GDKWSourceHandlers *
_gdk_wayland_dnd_source_handlers (void)
{
  return &dnd_source_handlers;
}

GDKWDragContext *
_gdk_wayland_dnd_active_source (GdkDisplay *display)
{
  GDKWDataDevice *dd = src_dd (display);

  if (dd == NULL || dd->dnd.source == NULL ||
      dd->dnd.source->state == GDKW_DRAG_DONE)
    return NULL;

  return dd->dnd.source;
}

/* ---- GDK entry points ----------------------------------------------------- */

GdkDragContext *
_gdk_wayland_drag_begin (GdkWindow *window,
                         GList     *targets)
{
  GdkDisplay      *display = NULL;
  GDKWDataDevice  *dd;
  GDKWSeat        *seat;
  GDKWDragContext *ctx;
  GdkDragContext  *base;
  GdkAtom         *atoms;
  gchar          **mimes;
  GList           *l;
  gint             n, i;
  gboolean         have_window;

  have_window = src_window_alive (window);
  if (have_window)
    display = gdk_window_get_display (window);
  if (display == NULL || !GDKW_IS_DISPLAY_WAYLAND (display))
    display = gdk_display_get_default ();

  dd = src_dd (display);

  /* a previous unfinished drag is dropped silently */
  if (dd != NULL && dd->dnd.source != NULL)
    src_done (dd->dnd.source);

  ctx = g_object_new (GDKW_TYPE_DRAG_CONTEXT, NULL);
  base = GDK_DRAG_CONTEXT (ctx);

  ctx->display = display;
  ctx->state = GDKW_DRAG_NEW;

  base->is_source = TRUE;
  base->protocol = GDK_DRAG_PROTO_LOCAL;
  base->source_window = have_window ? g_object_ref (window) : NULL;
  base->targets = g_list_copy (targets);
  base->actions = 0;
  base->start_time = dd != NULL
    ? (_gdk_wayland_display_get_seat (display)->last_time != 0
       ? _gdk_wayland_display_get_seat (display)->last_time : GDK_CURRENT_TIME)
    : GDK_CURRENT_TIME;

  src_track (ctx);

  n = g_list_length (targets);
  atoms = g_new0 (GdkAtom, n + 1);
  for (i = 0, l = targets; l != NULL; l = l->next)
    atoms[i++] = (GdkAtom) l->data;
  ctx->source_map = _gdk_wayland_selection_source_map_new (atoms, n);
  g_free (atoms);

  if (!have_window || dd == NULL || !_gdk_wayland_dd_usable (dd, GDKW_SEL_DND))
    {
      /* no window means nobody to send DRAG_STATUS or DROP_FINISHED to */
      src_fail (ctx);
      return base;
    }

  seat = _gdk_wayland_display_get_seat (display);
  if (seat->button_mask == 0 || seat->button_serial == 0 ||
      (seat->pointer_focus == NULL && seat->last_pointer_impl == NULL))
    {
      src_fail (ctx);
      return base;
    }

  /* dnd_finished (the only end of a successful drop) needs manager version 3.
   * An older manager would leave the drag, and ytk's grab, open for good. */
  if (!dd->virtual_device && dd->manager_version < 3)
    {
      g_debug ("ydk-wayland: drag refused, wl_data_device_manager version %u < 3",
               dd->manager_version);
      src_fail (ctx);
      return base;
    }

  mimes = _gdk_wayland_selection_source_map_mimes (ctx->source_map);
  if (mimes == NULL || mimes[0] == NULL)
    {
      /* nothing offerable among the targets: no destination could accept it */
      g_strfreev (mimes);
      src_fail (ctx);
      return base;
    }
  ctx->source = _gdk_wayland_dd_source_new (dd, GDKW_SEL_DND,
                                            (const gchar * const *) mimes,
                                            &dnd_source_handlers, ctx);
  g_strfreev (mimes);

  if (ctx->source == NULL)
    {
      src_fail (ctx);
      return base;
    }

  dd->dnd.source = g_object_ref (ctx);   /* the device's reference */
  ctx->start_timeout = g_timeout_add (GDKW_DRAG_START_FALLBACK_MS,
                                      src_start_cb, ctx);
  src_snapshot (ctx);

  return base;
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
  GDKWDragContext *ctx = src_ctx (context);

  /* The compositor routes the drop target; the client cannot hit-test. */
  if (dest_window)
    *dest_window = NULL;
  if (protocol)
    *protocol = GDK_DRAG_PROTO_NONE;

  if (ctx == NULL)
    return;

  if (ctx->icon_window != drag_window)
    {
      if (ctx->icon_window != NULL)
        g_object_remove_weak_pointer (G_OBJECT (ctx->icon_window),
                                      (gpointer *) &ctx->icon_window);
      ctx->icon_window = NULL;
      if (drag_window != NULL && GDK_IS_WINDOW (drag_window))
        {
          ctx->icon_window = drag_window;
          g_object_add_weak_pointer (G_OBJECT (ctx->icon_window),
                                     (gpointer *) &ctx->icon_window);
        }
    }

  if (ctx->state != GDKW_DRAG_NEW)
    return;

  if (context->actions != 0)
    src_start (ctx);
  else
    {
      /* ytk calls gdk_drag_motion right after this, and only that call knows
       * the possible actions (set_actions must precede start_drag).  Start
       * from an idle so that the motion call wins. */
      if (ctx->start_timeout != 0)
        g_source_remove (ctx->start_timeout);
      ctx->start_timeout = g_idle_add (src_start_cb, ctx);
    }
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
  GDKWDragContext *ctx = src_ctx (context);

  if (ctx == NULL || ctx->state == GDKW_DRAG_DONE)
    return FALSE;

  if (!ctx->started)
    context->actions = possible_actions;
  context->suggested_action = suggested_action;

  if (ctx->state == GDKW_DRAG_NEW)
    src_start (ctx);

  return FALSE;
}

void
_gdk_wayland_drag_drop (GdkDragContext *context,
                        guint32         time)
{
  GDKWDragContext *ctx = src_ctx (context);

  if (ctx != NULL)
    ctx->drop_requested = TRUE;
}

void
_gdk_wayland_drag_abort (GdkDragContext *context,
                         guint32         time)
{
  GDKWDragContext *ctx = src_ctx (context);

  /* Not started or failed: finish locally.  Started: freeing the source makes
   * the compositor cancel the drag.  No event: ytk ends the drag itself. */
  if (ctx == NULL || ctx->state == GDKW_DRAG_DONE)
    return;

  src_done (ctx);
}

gboolean
_gdk_wayland_drag_drop_succeeded (GdkDragContext *context)
{
  GDKWDragContext *ctx = src_ctx (context);

  return ctx != NULL && ctx->succeeded;
}

/* ---- test hook ------------------------------------------------------------- */

gboolean
_gdk_wayland_test_dnd_source_info (GdkDisplay            *display,
                                   GDKWTestDndSourceInfo *info)
{
  if (info == NULL || display == NULL || !GDKW_IS_DISPLAY_WAYLAND (display))
    return FALSE;

  if (last_ctx != NULL)
    src_fill_info (last_ctx, info);
  else if (snap_valid)
    *info = snap;
  else
    memset (info, 0, sizeof (*info));

  info->events_queued = src_events_queued;

  return TRUE;
}
