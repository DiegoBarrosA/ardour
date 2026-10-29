/*
 * gdkdnd-wayland.c
 *
 * ydk-wayland fork, M8: Wayland drag and drop, the context type and the
 * destination half.  The source half is in gdkdnd-source-wayland.c.
 *
 * GdkDragContext is one GType, registered by the X11 backend.  Wayland
 * contexts are a SUBCLASS, GDKWDragContext, so that the X11 init and finalize
 * (which keep an X11 private block in windowing_data) stay valid, and so that
 * every entry point can tell its own contexts from a foreign one.  The
 * protocol is always GDK_DRAG_PROTO_LOCAL.  See gdkm8-wayland.h, DECISIONS:
 * DRAG CONTEXT TYPE and DRAG DESTINATION.
 *
 * This file never calls a wl_data_* request.  Every request goes through the
 * _gdk_wayland_dd_* functions of the datadevice piece.
 */

#include "config.h"

#include <string.h>

#include <glib.h>

#include <wayland-client.h>

#include <ydk/gdk.h>
#include "gdkprivate-wayland.h"
#include "gdkseat-wayland.h"
#include "gdkm8-wayland.h"

#ifdef GDK_COMPILATION
#include "ydk/gdkalias.h"
#endif

/* Defined below, used by gdkdnd-source-wayland.c (declared there too, the
 * header has no slot for it).  Ends a source context without an event. */
void _gdk_wayland_dnd_source_end (GDKWDragContext *context);

typedef struct
{
  GdkDragContextClass parent_class;
} GDKWDragContextClass;

static GList *live_contexts = NULL;
static guint  contexts_alive = 0;
static guint  dest_events_queued = 0;

static void gdkw_drag_context_finalize (GObject *object);

G_DEFINE_TYPE (GDKWDragContext, gdkw_drag_context, GDK_TYPE_DRAG_CONTEXT)

static void
gdkw_drag_context_init (GDKWDragContext *context)
{
  /* windowing_data stays what the parent init made it */
  GDK_DRAG_CONTEXT (context)->protocol = GDK_DRAG_PROTO_LOCAL;
  context->state = GDKW_DRAG_NEW;

  contexts_alive++;
  live_contexts = g_list_prepend (live_contexts, context);
}

static void
gdkw_drag_context_class_init (GDKWDragContextClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = gdkw_drag_context_finalize;
}

static GDKWDataDevice *
dnd_dd (GdkDisplay *display)
{
  if (display == NULL || !GDKW_IS_DISPLAY_WAYLAND (display))
    return NULL;

  return _gdk_wayland_display_get_data_device (display);
}

static guint32
dnd_seat_time (GdkDisplay *display)
{
  GDKWSeat *seat;

  if (display == NULL || !GDKW_IS_DISPLAY_WAYLAND (display))
    return GDK_CURRENT_TIME;

  seat = _gdk_wayland_display_get_seat (display);

  return seat->last_time != 0 ? seat->last_time : GDK_CURRENT_TIME;
}

static gboolean
window_alive (GdkWindow *window)
{
  return window != NULL && GDK_IS_WINDOW (window) &&
         !gdk_window_is_destroyed (window);
}

static void
gdkw_drag_context_finalize (GObject *object)
{
  GDKWDragContext *context = GDKW_DRAG_CONTEXT (object);

  live_contexts = g_list_remove (live_contexts, context);
  if (contexts_alive > 0)
    contexts_alive--;

  if (context->start_timeout != 0)
    {
      g_source_remove (context->start_timeout);
      context->start_timeout = 0;
    }

  if (context->source != NULL)
    {
      GDKWDataDevice *dd = dnd_dd (context->display);

      /* only when the device still lists it: the device frees every source
       * itself at display finalize */
      if (dd != NULL && g_list_find (dd->sources, context->source) != NULL)
        _gdk_wayland_dd_source_free (context->source);
      context->source = NULL;
    }

  if (context->source_map != NULL)
    {
      _gdk_wayland_selection_source_map_free (context->source_map);
      context->source_map = NULL;
    }

  if (context->offer != NULL)
    {
      _gdk_wayland_dd_offer_unref (context->offer);
      context->offer = NULL;
    }

  if (context->icon_window != NULL)
    {
      g_object_remove_weak_pointer (G_OBJECT (context->icon_window),
                                    (gpointer *) &context->icon_window);
      context->icon_window = NULL;
    }

  G_OBJECT_CLASS (gdkw_drag_context_parent_class)->finalize (object);
}

/* ---- events --------------------------------------------------------------- */

void
_gdk_wayland_dnd_put_event (GdkEventType     type,
                            GdkWindow       *window,
                            GDKWDragContext *context,
                            guint32          time,
                            gint             x_root,
                            gint             y_root)
{
  GdkEvent *event;

  if (context == NULL || !window_alive (window))
    return;

  event = gdk_event_new (type);
  event->dnd.window = g_object_ref (window);
  event->dnd.send_event = FALSE;
  event->dnd.context = g_object_ref (GDK_DRAG_CONTEXT (context));
  event->dnd.time = time;
  event->dnd.x_root = (gshort) x_root;
  event->dnd.y_root = (gshort) y_root;
  gdk_event_set_screen (event, gdk_window_get_screen (window));
  gdk_event_put (event);
  gdk_event_free (event);
}

static void
dest_put_event (GdkEventType type, GDKWDragContext *context)
{
  GdkWindow *window = GDK_DRAG_CONTEXT (context)->dest_window;

  if (!window_alive (window))
    return;

  _gdk_wayland_dnd_put_event (type, window, context, context->last_time,
                              context->x_root, context->y_root);
  dest_events_queued++;
}

/* ---- destination bookkeeping --------------------------------------------- */

/* The context was entered on a registered toplevel and has not ended. */
static gboolean
dest_ctx_live (GDKWDataDevice *dd, GdkDragContext *context)
{
  GDKWDragContext *ctx;

  if (context == NULL || dd == NULL || !GDKW_IS_DRAG_CONTEXT (context) ||
      context->is_source)
    return FALSE;

  ctx = GDKW_DRAG_CONTEXT (context);

  return ctx->state != GDKW_DRAG_DONE && ctx->offer != NULL &&
         dd->dnd.dest == ctx;
}

/* Ends the destination: drops the references on the offer (destroying the
 * protocol object when @destroy) and releases the context.  @context may be
 * NULL to drop only the offer that an ignored enter or a drop left behind.
 * No event is queued. */
static void
dest_end (GDKWDataDevice *dd, GDKWDragContext *context, gboolean destroy)
{
  GDKWOffer *held, *mine = NULL;
  gboolean   release_ctx = FALSE;

  if (dd == NULL)
    return;

  if (context != NULL)
    {
      mine = context->offer;
      context->offer = NULL;
      context->state = GDKW_DRAG_DONE;
      if (dd->dnd.dest == context)
        {
          dd->dnd.dest = NULL;
          release_ctx = TRUE;
        }
    }

  held = dd->dnd.dest_offer;
  dd->dnd.dest_offer = NULL;
  dd->dnd.dest_dropped = FALSE;

  /* the pointers are detached first, so the datadevice finds nothing to clear
   * and our own references are the only thing left to release */
  if (destroy)
    {
      if (held != NULL)
        _gdk_wayland_dd_offer_destroy (held);
      if (mine != NULL && mine != held)
        _gdk_wayland_dd_offer_destroy (mine);
    }

  if (held != NULL)
    _gdk_wayland_dd_offer_unref (held);
  if (mine != NULL)
    _gdk_wayland_dd_offer_unref (mine);

  if (release_ctx)
    g_object_unref (context);
}

static gboolean
window_registered (GdkWindow *window)
{
  GdkWindow *top;

  if (!window_alive (window))
    return FALSE;

  if (g_object_get_data (G_OBJECT (window), GDKW_DND_REGISTERED_KEY) != NULL)
    return TRUE;

  top = gdk_window_get_toplevel (window);

  return top != NULL && top != window &&
         g_object_get_data (G_OBJECT (top), GDKW_DND_REGISTERED_KEY) != NULL;
}

static void
dest_set_position (GDKWDragContext *context, gdouble x, gdouble y)
{
  GdkWindow *window = GDK_DRAG_CONTEXT (context)->dest_window;
  gint       px = 0, py = 0;

  if (window_alive (window))
    gdk_window_get_position (window, &px, &py);

  /* ytk subtracts the toplevel position from x_root, see DECISIONS */
  context->x_root = (gint) (gshort) x + px;
  context->y_root = (gint) (gshort) y + py;
}

/* ---- datadevice -> dnd entry points --------------------------------------- */

void
_gdk_wayland_dnd_dest_enter (GDKWDataDevice        *dd,
                             guint32                serial,
                             GDKWWindowImplWayland *impl,
                             GDKWOffer             *offer,
                             gdouble                x,
                             gdouble                y)
{
  GdkDisplay      *display;
  GdkWindow       *window;
  GDKWDragContext *ctx, *src;
  GdkDragContext  *base;
  GdkWindow       *source_window = NULL;

  if (dd == NULL || dd->display == NULL)
    return;

  display = GDK_DISPLAY_OBJECT (dd->display);

  /* leftovers of the previous destination */
  if (dd->dnd.dest != NULL)
    {
      GDKWDragContext *old = dd->dnd.dest;

      if (old->state != GDKW_DRAG_DONE && !old->dropped)
        dest_put_event (GDK_DRAG_LEAVE, old);
      dest_end (dd, old, TRUE);
    }
  else
    dest_end (dd, NULL, TRUE);

  dd->dnd.dest_serial = serial;
  dd->dnd.dest_ignored = FALSE;
  dd->dnd.dest_dropped = FALSE;

  if (offer != NULL)
    {
      dd->dnd.dest_offer = _gdk_wayland_dd_offer_ref (offer);
      offer->enter_serial = serial;
    }

  window = impl != NULL ? impl->wrapper : NULL;

  if (offer == NULL || !window_registered (window))
    {
      dd->dnd.dest_ignored = TRUE;
      if (offer != NULL)
        {
          _gdk_wayland_dd_offer_accept (offer, serial, NULL);
          _gdk_wayland_dd_offer_set_actions (offer, 0, 0);
        }
      return;
    }

  ctx = g_object_new (GDKW_TYPE_DRAG_CONTEXT, NULL);
  base = GDK_DRAG_CONTEXT (ctx);

  ctx->display = display;
  ctx->state = GDKW_DRAG_NEW;
  ctx->offer = _gdk_wayland_dd_offer_ref (offer);
  ctx->enter_serial = serial;
  ctx->last_time = dnd_seat_time (display);

  base->is_source = FALSE;
  base->protocol = GDK_DRAG_PROTO_LOCAL;
  base->dest_window = g_object_ref (window);
  base->targets = _gdk_wayland_selection_targets_for_offer (offer);
  base->actions = offer->got_source_actions
    ? _gdk_wayland_dnd_actions_from_wl (offer->source_actions)
    : (GDK_ACTION_COPY | GDK_ACTION_MOVE);
  base->suggested_action = offer->got_action
    ? _gdk_wayland_dnd_actions_from_wl (offer->action)
    : GDK_ACTION_COPY;
  base->action = 0;
  base->start_time = ctx->last_time;

  src = dd->dnd.source;
  if (src != NULL && src->state != GDKW_DRAG_DONE &&
      window_alive (GDK_DRAG_CONTEXT (src)->source_window))
    {
      source_window = GDK_DRAG_CONTEXT (src)->source_window;
      src->local_dest = TRUE;
    }
  if (source_window == NULL)
    source_window = gdk_screen_get_root_window (gdk_window_get_screen (window));
  base->source_window = source_window != NULL ? g_object_ref (source_window)
                                              : NULL;

  dest_set_position (ctx, x, y);

  dd->dnd.dest = ctx;           /* the creation reference */

  dest_put_event (GDK_DRAG_ENTER, ctx);
  dest_put_event (GDK_DRAG_MOTION, ctx);
}

void
_gdk_wayland_dnd_dest_motion (GDKWDataDevice *dd,
                              guint32         time,
                              gdouble         x,
                              gdouble         y)
{
  GDKWDragContext *ctx;

  if (dd == NULL || dd->dnd.dest == NULL)
    return;

  ctx = dd->dnd.dest;
  if (ctx->state == GDKW_DRAG_DONE || ctx->dropped)
    return;

  if (!window_alive (GDK_DRAG_CONTEXT (ctx)->dest_window))
    {
      dest_end (dd, ctx, TRUE);
      return;
    }

  ctx->last_time = time;
  dest_set_position (ctx, x, y);
  dest_put_event (GDK_DRAG_MOTION, ctx);
}

void
_gdk_wayland_dnd_dest_leave (GDKWDataDevice *dd)
{
  GDKWDragContext *ctx;

  if (dd == NULL)
    return;

  ctx = dd->dnd.dest;

  /* a leave after a drop is ignored: the offer is still needed */
  if (ctx != NULL && (ctx->dropped || dd->dnd.dest_dropped))
    return;

  if (ctx == NULL)
    {
      /* an ignored enter, or nothing: drop the offer */
      dest_end (dd, NULL, TRUE);
      return;
    }

  if (ctx->state != GDKW_DRAG_DONE)
    dest_put_event (GDK_DRAG_LEAVE, ctx);
  dest_end (dd, ctx, TRUE);
}

void
_gdk_wayland_dnd_dest_drop (GDKWDataDevice *dd)
{
  GDKWDragContext *ctx;

  if (dd == NULL)
    return;

  ctx = dd->dnd.dest;
  if (ctx == NULL || ctx->state == GDKW_DRAG_DONE)
    {
      /* a drop on an ignored surface: nothing can be received */
      if (ctx == NULL)
        dest_end (dd, NULL, TRUE);
      return;
    }

  if (ctx->dropped)
    return;

  if (!window_alive (GDK_DRAG_CONTEXT (ctx)->dest_window))
    {
      dest_end (dd, ctx, TRUE);
      return;
    }

  ctx->state = GDKW_DRAG_DROPPED;
  ctx->dropped = TRUE;
  dd->dnd.dest_dropped = TRUE;
  dest_put_event (GDK_DROP_START, ctx);
}

void
_gdk_wayland_dnd_offer_changed (GDKWDataDevice *dd,
                                GDKWOffer      *offer)
{
  GDKWDragContext *ctx;
  GdkDragContext  *base;

  if (dd == NULL || offer == NULL)
    return;

  ctx = dd->dnd.dest;
  if (ctx == NULL || ctx->offer != offer || ctx->state == GDKW_DRAG_DONE)
    return;

  base = GDK_DRAG_CONTEXT (ctx);

  if (offer->got_source_actions)
    base->actions = _gdk_wayland_dnd_actions_from_wl (offer->source_actions);
  if (offer->got_action)
    {
      base->suggested_action = _gdk_wayland_dnd_actions_from_wl (offer->action);
      /* after the drop the compositor's action is the final one */
      if (ctx->dropped)
        base->action = base->suggested_action;
    }
}

void
_gdk_wayland_dnd_window_destroyed (GdkWindow *window)
{
  GdkDisplay     *display;
  GDKWDataDevice *dd;

  if (window == NULL || !GDK_IS_WINDOW (window))
    return;

  display = gdk_window_get_display (window);
  dd = dnd_dd (display);
  if (dd == NULL)
    return;

  if (dd->dnd.dest != NULL &&
      GDK_DRAG_CONTEXT (dd->dnd.dest)->dest_window == window)
    dest_end (dd, dd->dnd.dest, TRUE);

  if (dd->dnd.source != NULL &&
      GDK_DRAG_CONTEXT (dd->dnd.source)->source_window == window)
    _gdk_wayland_dnd_source_end (dd->dnd.source);
}

void
_gdk_wayland_dnd_display_finalize (GdkDisplay *display)
{
  GDKWDataDevice *dd = dnd_dd (display);
  GList          *copy, *l;

  if (dd != NULL)
    {
      dest_end (dd, dd->dnd.dest, TRUE);
      if (dd->dnd.source != NULL)
        _gdk_wayland_dnd_source_end (dd->dnd.source);
    }

  /* contexts that live on (held by events or the application) must not touch
   * the display any more */
  copy = g_list_copy (live_contexts);
  for (l = copy; l != NULL; l = l->next)
    g_object_ref (l->data);

  for (l = copy; l != NULL; l = l->next)
    {
      GDKWDragContext *ctx = l->data;

      if (ctx->display != display)
        continue;

      if (ctx->start_timeout != 0)
        {
          g_source_remove (ctx->start_timeout);
          ctx->start_timeout = 0;
        }
      if (ctx->source != NULL)
        {
          if (dd != NULL && g_list_find (dd->sources, ctx->source) != NULL)
            _gdk_wayland_dd_source_free (ctx->source);
          ctx->source = NULL;
        }
      if (ctx->offer != NULL)
        {
          _gdk_wayland_dd_offer_unref (ctx->offer);
          ctx->offer = NULL;
        }
      ctx->state = GDKW_DRAG_DONE;
      ctx->display = NULL;
    }

  for (l = copy; l != NULL; l = l->next)
    g_object_unref (l->data);
  g_list_free (copy);
}

/* Shared with the source half.  Ends a source context without an event: the
 * fallback timer goes, the drag source is freed (the compositor cancels the
 * drag) and the device lets go of the context. */
void
_gdk_wayland_dnd_source_end (GDKWDragContext *context)
{
  GDKWDataDevice *dd;
  GDKWSource     *src;

  if (context == NULL)
    return;

  g_object_ref (context);

  if (context->start_timeout != 0)
    {
      g_source_remove (context->start_timeout);
      context->start_timeout = 0;
    }

  context->state = GDKW_DRAG_DONE;

  dd = dnd_dd (context->display);

  src = context->source;
  context->source = NULL;
  if (src != NULL && dd != NULL && g_list_find (dd->sources, src) != NULL)
    _gdk_wayland_dd_source_free (src);

  if (dd != NULL && dd->dnd.source == context)
    {
      dd->dnd.source = NULL;
      g_object_unref (context);
    }

  g_object_unref (context);
}

/* ---- GDK entry points ----------------------------------------------------- */

GdkDisplay *
_gdk_wayland_drag_context_get_display (GdkDragContext *context)
{
  if (context == NULL)
    return NULL;

  if (GDKW_IS_DRAG_CONTEXT (context) && GDKW_DRAG_CONTEXT (context)->display)
    return GDKW_DRAG_CONTEXT (context)->display;

  if (window_alive (context->source_window))
    return gdk_drawable_get_display (GDK_DRAWABLE (context->source_window));
  else if (window_alive (context->dest_window))
    return gdk_drawable_get_display (GDK_DRAWABLE (context->dest_window));

  return NULL;
}

void
_gdk_wayland_drag_status (GdkDragContext *context,
                          GdkDragAction   action,
                          guint32         time)
{
  GDKWDragContext *ctx;
  GDKWDataDevice  *dd;
  const gchar     *mime = NULL;
  guint            i;

  if (context == NULL || !GDKW_IS_DRAG_CONTEXT (context))
    return;

  ctx = GDKW_DRAG_CONTEXT (context);
  dd = dnd_dd (ctx->display);
  if (!dest_ctx_live (dd, context))
    return;

  context->action = action;

  if (ctx->sent_valid && ctx->sent_action == action)
    return;
  ctx->sent_valid = TRUE;
  ctx->sent_action = action;

  if (action == 0)
    {
      _gdk_wayland_dd_offer_accept (ctx->offer, ctx->enter_serial, NULL);
      _gdk_wayland_dd_offer_set_actions (ctx->offer, 0, 0);
      return;
    }

  for (i = 0; ctx->offer->mimes != NULL && i < ctx->offer->mimes->len; i++)
    {
      const gchar *m = g_ptr_array_index (ctx->offer->mimes, i);

      if (m != NULL && m[0] != '\0')
        {
          mime = m;
          break;
        }
    }

  _gdk_wayland_dd_offer_accept (ctx->offer, ctx->enter_serial, mime);
  _gdk_wayland_dd_offer_set_actions (ctx->offer,
                                     _gdk_wayland_dnd_actions_to_wl (action),
                                     _gdk_wayland_dnd_preferred_to_wl (action));
}

GdkAtom
_gdk_wayland_drag_get_selection (GdkDragContext *context)
{
  if (context == NULL || !GDKW_IS_DRAG_CONTEXT (context))
    return GDK_NONE;

  return _gdk_wayland_selection_dnd_atom ();
}

void
_gdk_wayland_drop_reply (GdkDragContext *context,
                         gboolean        ok,
                         guint32         time)
{
  GDKWDragContext *ctx;
  GDKWDataDevice  *dd;

  if (context == NULL || !GDKW_IS_DRAG_CONTEXT (context))
    return;

  ctx = GDKW_DRAG_CONTEXT (context);
  dd = dnd_dd (ctx->display);
  if (!dest_ctx_live (dd, context) || ok)
    return;

  _gdk_wayland_dd_offer_accept (ctx->offer, ctx->enter_serial, NULL);
  _gdk_wayland_dd_offer_set_actions (ctx->offer, 0, 0);
  dest_end (dd, ctx, TRUE);
}

void
_gdk_wayland_drop_finish (GdkDragContext *context,
                          gboolean        success,
                          guint32         time)
{
  GDKWDragContext *ctx;
  GDKWDataDevice  *dd;

  if (context == NULL || !GDKW_IS_DRAG_CONTEXT (context))
    return;

  ctx = GDKW_DRAG_CONTEXT (context);
  dd = dnd_dd (ctx->display);
  if (!dest_ctx_live (dd, context))
    return;

  g_object_ref (ctx);
  if (success && context->action != 0)
    _gdk_wayland_dd_offer_finish (ctx->offer);
  dest_end (dd, ctx, TRUE);
  g_object_unref (ctx);
}

void
_gdk_wayland_window_register_dnd (GdkWindow *window)
{
  GdkWindow *top;

  if (!window_alive (window))
    return;

  g_object_set_data (G_OBJECT (window), GDKW_DND_REGISTERED_KEY,
                     GINT_TO_POINTER (1));

  top = gdk_window_get_toplevel (window);
  if (top != NULL && top != window)
    g_object_set_data (G_OBJECT (top), GDKW_DND_REGISTERED_KEY,
                       GINT_TO_POINTER (1));
}

/* ---- test hooks ------------------------------------------------------------ */

gboolean
_gdk_wayland_test_drag_context_info (GdkDragContext          *context,
                                     GDKWTestDragContextInfo *info)
{
  if (context == NULL || info == NULL ||
      !G_TYPE_CHECK_INSTANCE_TYPE (context, GDK_TYPE_DRAG_CONTEXT))
    return FALSE;

  memset (info, 0, sizeof (*info));
  info->is_wayland = GDKW_IS_DRAG_CONTEXT (context);
  info->protocol = context->protocol;
  info->is_source = context->is_source;
  info->n_targets = g_list_length (context->targets);

  return TRUE;
}

guint
_gdk_wayland_test_dnd_contexts_alive (void)
{
  return contexts_alive;
}

gboolean
_gdk_wayland_test_dnd_dest_info (GdkDisplay          *display,
                                 GDKWTestDndDestInfo *info)
{
  GDKWDataDevice *dd = dnd_dd (display);

  if (dd == NULL || info == NULL)
    return FALSE;

  memset (info, 0, sizeof (*info));
  info->active = dd->dnd.dest != NULL;
  info->ignored = dd->dnd.dest_ignored;
  info->dropped = dd->dnd.dest_dropped;
  info->context = dd->dnd.dest != NULL ? GDK_DRAG_CONTEXT (dd->dnd.dest) : NULL;
  if (dd->dnd.dest != NULL)
    {
      info->x_root = dd->dnd.dest->x_root;
      info->y_root = dd->dnd.dest->y_root;
      info->dest_window = GDK_DRAG_CONTEXT (dd->dnd.dest)->dest_window;
    }
  info->events_queued = dest_events_queued;

  return TRUE;
}
