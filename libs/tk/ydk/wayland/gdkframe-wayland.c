/*
 * gdkframe-wayland.c
 *
 * ydk-wayland fork, M10 "frame" piece (see gdkm10-wayland.h, D5 to D9).
 *
 * Every wl_surface attach, damage, frame and commit request for a window
 * surface is made here, so every request is counted and observable by the
 * test hooks.  The file owns:
 *
 *   - the frame state machine (UNMAPPED, IDLE, WAITING), the coalescing of
 *     commit requests while a frame callback is outstanding, the timeout
 *     with backoff, and the settle idle;
 *   - the commit procedure: ensure the store, shape clear, prepare (copy of
 *     the stale region into a FREE slot, buffers piece), scale state (scale
 *     piece), attach, damage, frame request, commit, commit_done;
 *   - the compositor resize, the client resize, the scale change, the unmap
 *     of a toplevel (including the destroy of its xdg role objects) and the
 *     teardown;
 *   - the three process-updates hooks, present_all, set_cairo_surface_size,
 *     and the two functions moved out of gdkwindowimpl-wayland.c
 *     (present, resize_buffer).
 *
 * The back store, the slot pool and the damage lists belong to the buffers
 * piece (gdkbuffer-wayland.c); this file only calls its functions.  The
 * scale state belongs to the scale piece (gdkscale-wayland.c).
 *
 * A frame record exists per impl that has a wl_surface (created on demand,
 * freed by _gdk_wayland_frame_teardown).  Display wide test state (mode,
 * timeout override, counters of live sources) hangs on the GdkDisplay as
 * object data, like the seat state.
 */

#include "config.h"

#include <string.h>

#include <wayland-client.h>
#include <cairo.h>

#include <ydk/gdk.h>
#include <ydk/gdkinternals.h>

#include "xdg-shell-client.h"

#include "gdkprivate-wayland.h"
#include "gdkseat-wayland.h"
#include "gdkpixmap-wayland.h"
#include "gdkm7-wayland.h"
#include "gdkm9-wayland.h"
#include "gdkm10-wayland.h"


/* ------------------------------------------------------------------------
 * Records
 * ------------------------------------------------------------------------ */

#define FRAME_DISPLAY_KEY "gdkw-m10-frame-display"

/* Display wide state: test modes, timeout override, live source counters. */
typedef struct
{
  GDKWTestFrameMode mode;
  guint first_ms;           /* 0: GDKW_FRAME_TIMEOUT_MS */
  guint max_ms;             /* 0: GDKW_FRAME_TIMEOUT_MAX_MS */
  guint timers;
  guint settle_idles;
  guint callbacks;
} FrameDisplay;

struct _GDKWFrame
{
  GDKWWindowImplWayland *impl;

  GDKWFrameState state;
  gboolean commit_wanted;
  gboolean ever_committed;
  gboolean need_full;            /* next commit must carry FULL damage */
  gboolean held_done;
  guint32  held_time;

  struct wl_callback *callback;

  guint timeout_id;
  guint settle_id;
  guint timeout_ms;
  guint consecutive_timeouts;

  guint64 serial;                /* commits since creation, never reset */
  guint64 settle_mark;           /* serial when the settle idle was armed */

  gint    scope_depth;           /* open process_updates scopes */

  /* geometry of the last commit */
  gint    commit_lw, commit_lh;
  guint   commit_scale120;
  gpointer last_store;           /* impl->shm_data at the last commit */

  /* counters and last commit fields (state fields are filled on query) */
  GDKWTestFrameInfo c;
};

/* Open process_all_updates cycles.  The hooks have no display argument. */
static guint update_cycle_depth = 0;

static FrameDisplay fallback_display_state;

static const struct wl_callback_listener frame_listener;

/* ------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------ */

static gboolean
window_alive (GdkWindow *window)
{
  return window != NULL && GDK_IS_WINDOW (window) &&
         !GDK_WINDOW_DESTROYED (window);
}

static GDKWWindowImplWayland *
impl_from_window (GdkWindow *window)
{
  GdkWindowObject *private;

  if (!window_alive (window))
    return NULL;

  private = (GdkWindowObject *) window;
  if (private->impl == NULL ||
      !G_TYPE_CHECK_INSTANCE_TYPE (private->impl,
                                   GDKW_TYPE_WINDOW_IMPL_WAYLAND))
    return NULL;

  return GDKW_WINDOW_IMPL_WAYLAND (private->impl);
}

static GdkDisplay *
impl_display (GDKWWindowImplWayland *impl)
{
  if (impl == NULL || impl->screen == NULL)
    return NULL;

  return gdk_screen_get_display (impl->screen);
}

static gboolean
is_wayland_display (GdkDisplay *display)
{
  return display != NULL &&
         G_TYPE_CHECK_INSTANCE_TYPE (display, GDKW_TYPE_DISPLAY_WAYLAND);
}

static FrameDisplay *
frame_display_for (GdkDisplay *display)
{
  FrameDisplay *ds;

  if (display == NULL)
    return &fallback_display_state;

  ds = g_object_get_data (G_OBJECT (display), FRAME_DISPLAY_KEY);
  if (ds == NULL)
    {
      ds = g_new0 (FrameDisplay, 1);
      g_object_set_data_full (G_OBJECT (display), FRAME_DISPLAY_KEY, ds,
                              g_free);
    }

  return ds;
}

static FrameDisplay *
frame_ds (GDKWWindowImplWayland *impl)
{
  return frame_display_for (impl_display (impl));
}

static guint
base_ms (FrameDisplay *ds)
{
  return ds->first_ms != 0 ? ds->first_ms : GDKW_FRAME_TIMEOUT_MS;
}

static guint
cap_ms (FrameDisplay *ds)
{
  guint cap = ds->max_ms != 0 ? ds->max_ms : GDKW_FRAME_TIMEOUT_MAX_MS;

  return MAX (cap, base_ms (ds));
}

/* The record of @impl.  Normal paths create it only for an impl that has a
 * wl_surface; hooks pass @force. */
static GDKWFrame *
frame_get (GDKWWindowImplWayland *impl, gboolean create, gboolean force)
{
  GDKWFrame *f;

  if (impl == NULL)
    return NULL;

  f = impl->frame;
  if (f != NULL || !create)
    return f;

  if (!force && impl->wl_surface == NULL)
    return NULL;

  f = g_new0 (GDKWFrame, 1);
  f->impl = impl;
  f->state = GDKW_TEST_FRAME_UNMAPPED;
  f->need_full = TRUE;
  f->timeout_ms = base_ms (frame_ds (impl));
  impl->frame = f;

  return f;
}

static inline guint
effective_scale (GDKWWindowImplWayland *impl)
{
  guint s = _gdk_wayland_scale_surface_scale120 (impl);

  return CLAMP (s, GDKW_SCALE_MIN, GDKW_SCALE_MAX);
}

/* Heap event, as everywhere else (gdk_event_put () copies it).  The size is
 * the impl size, because the core updates GdkWindowObject width and height
 * only after the iface move_resize returns (client resize).  A hidden window
 * gets no event (D8). */
static void
queue_configure (GDKWWindowImplWayland *impl, GDKWFrame *f)
{
  GdkWindow *wrapper;
  GdkWindowObject *private;
  GdkEvent *ev;

  wrapper = impl->wrapper;
  if (!window_alive (wrapper) || !GDK_WINDOW_IS_MAPPED (wrapper))
    return;

  private = (GdkWindowObject *) wrapper;
  if (!(private->event_mask & GDK_STRUCTURE_MASK))
    return;

  ev = gdk_event_new (GDK_CONFIGURE);
  ev->any.window = g_object_ref (wrapper);
  ev->any.send_event = FALSE;
  gdk_event_set_screen (ev, gdk_drawable_get_screen (GDK_DRAWABLE (wrapper)));
  ev->configure.x = private->x;
  ev->configure.y = private->y;
  ev->configure.width = impl->width;
  ev->configure.height = impl->height;
  gdk_event_put (ev);
  gdk_event_free (ev);

  if (f != NULL)
    f->c.configure_events_queued++;
}

/* ------------------------------------------------------------------------
 * Sources: callback, timeout, settle idle
 * ------------------------------------------------------------------------ */

static void
frame_destroy_callback (GDKWFrame *f)
{
  if (f->callback != NULL)
    {
      FrameDisplay *ds = frame_ds (f->impl);

      wl_callback_destroy (f->callback);
      f->callback = NULL;
      if (ds->callbacks > 0)
        ds->callbacks--;
    }
  f->held_done = FALSE;
}

static void
frame_remove_timeout (GDKWFrame *f)
{
  if (f->timeout_id != 0)
    {
      FrameDisplay *ds = frame_ds (f->impl);

      g_source_remove (f->timeout_id);
      f->timeout_id = 0;
      if (ds->timers > 0)
        ds->timers--;
    }
}

static void
frame_remove_settle (GDKWFrame *f)
{
  if (f->settle_id != 0)
    {
      FrameDisplay *ds = frame_ds (f->impl);

      g_source_remove (f->settle_id);
      f->settle_id = 0;
      if (ds->settle_idles > 0)
        ds->settle_idles--;
    }
}

static gboolean frame_timeout_cb (gpointer data);
static gboolean frame_settle_cb (gpointer data);

static void
frame_arm_timeout (GDKWFrame *f)
{
  FrameDisplay *ds;

  if (f->timeout_id != 0)
    return;

  ds = frame_ds (f->impl);
  if (ds->mode == GDKW_TEST_FRAME_MODE_HOLD)
    return; /* suspended */

  f->timeout_id = g_timeout_add (f->timeout_ms, frame_timeout_cb, f);
  ds->timers++;
}

static void
frame_arm_settle (GDKWFrame *f)
{
  FrameDisplay *ds;

  /* a later request refreshes the mark: a commit between the first arm and
   * the idle was for the older geometry */
  f->settle_mark = f->serial;
  if (f->settle_id != 0)
    return;

  ds = frame_ds (f->impl);
  f->settle_id = g_idle_add_full (G_PRIORITY_DEFAULT_IDLE,
                                  frame_settle_cb, f, NULL);
  ds->settle_idles++;
}

/* Back to the state of a never mapped surface.  Used by unmap, by
 * resize_buffer without a wl_surface, and by teardown (before the free). */
static void
frame_state_reset (GDKWFrame *f)
{
  FrameDisplay *ds = frame_ds (f->impl);

  frame_destroy_callback (f);
  frame_remove_timeout (f);
  frame_remove_settle (f);
  f->commit_wanted = FALSE;
  f->held_done = FALSE;
  f->state = GDKW_TEST_FRAME_UNMAPPED;
  f->ever_committed = FALSE;
  f->need_full = TRUE;
  f->timeout_ms = base_ms (ds);
  f->consecutive_timeouts = 0;
}

/* ------------------------------------------------------------------------
 * The commit
 * ------------------------------------------------------------------------ */

static gboolean frame_commit (GDKWWindowImplWayland *impl, GDKWFrame *f,
                              GDKWCommitReason reason, gboolean immediate);

/* Frame damage to the compositor.  Returns the number of requests. */
static guint
send_damage (GDKWWindowImplWayland *impl, const GDKWCommitInfo *info,
             gboolean full)
{
  guint n = 0;
  gint i;

  if (!_gdkw_damage_pre_v4 && wl_surface_get_version (impl->wl_surface) >= 4)
    {
      if (full)
        {
          GDKW_WIRE_DAMAGE_BUFFER (impl->wl_surface, 0, 0,
                                   info->buffer_width, info->buffer_height);
          n = 1;
        }
      else
        for (i = 0; i < info->n_rects; i++)
          {
            const GdkRectangle *r = &info->rects[i];

            GDKW_WIRE_DAMAGE_BUFFER (impl->wl_surface,
                                     r->x, r->y, r->width, r->height);
            n++;
          }
    }
  else
    {
      /* below version 4: logical bounding rectangle */
      gint x0 = 0, y0 = 0, x1 = info->logical_width, y1 = info->logical_height;

      if (!full)
        {
          gint dx0 = G_MAXINT, dy0 = G_MAXINT, dx1 = 0, dy1 = 0;
          guint s = MAX (info->scale120, 1u);

          for (i = 0; i < info->n_rects; i++)
            {
              const GdkRectangle *r = &info->rects[i];

              dx0 = MIN (dx0, r->x);
              dy0 = MIN (dy0, r->y);
              dx1 = MAX (dx1, r->x + r->width);
              dy1 = MAX (dy1, r->y + r->height);
            }
          x0 = (gint) (((guint64) dx0 * GDKW_SCALE_DENOM) / s);
          y0 = (gint) (((guint64) dy0 * GDKW_SCALE_DENOM) / s);
          x1 = (gint) (((guint64) dx1 * GDKW_SCALE_DENOM + s - 1) / s);
          y1 = (gint) (((guint64) dy1 * GDKW_SCALE_DENOM + s - 1) / s);
          x1 = MIN (x1, info->logical_width);
          y1 = MIN (y1, info->logical_height);
        }
      GDKW_WIRE_DAMAGE (impl->wl_surface, x0, y0,
                        MAX (x1 - x0, 0), MAX (y1 - y0, 0));
      n = 1;
    }

  return n;
}

/* A commit that failed for a reason that can pass (shm allocation) is
 * retried by the timeout with its backoff; without this the request would be
 * lost until some unrelated request arrives.  A stall for "no free slot" is
 * retried by the release, a timer cannot help there. */
static void
frame_retry_later (GDKWWindowImplWayland *impl, GDKWFrame *f)
{
  GdkDisplay *display = impl_display (impl);

  if (impl->width == 0 || impl->height == 0 || !is_wayland_display (display) ||
      GDKW_DISPLAY_WAYLAND (display)->wl_shm == NULL)
    return;

  f->commit_wanted = TRUE;
  frame_arm_timeout (f);
}

/* One commit now.  TRUE if a buffer was committed.  A failed commit because
 * of "no free slot" leaves commit_wanted set. */
static gboolean
frame_commit (GDKWWindowImplWayland *impl, GDKWFrame *f,
              GDKWCommitReason reason, gboolean immediate)
{
  GDKWCommitInfo info;
  GDKWPrepareResult res;
  FrameDisplay *ds;
  gpointer store_before;
  gboolean full;
  gint i;

  if (impl->wl_surface == NULL || !impl->configured)
    return FALSE;

  /* make the store match the impl size and scale */
  store_before = impl->shm_data;
  if (!_gdk_wayland_buffers_ensure (impl))
    {
      frame_retry_later (impl, f);
      return FALSE;
    }

  if (!f->ever_committed || f->need_full || impl->shm_data != store_before ||
      impl->shm_data != f->last_store)
    _gdk_wayland_buffers_damage_all (impl);

  /* stored shape: clear outside it in the store, before the copy.  The clear
   * has no damage of its own, so a shaped window always sends FULL. */
  if (_gdk_wayland_shape_before_present (impl))
    _gdk_wayland_buffers_damage_all (impl);

  res = _gdk_wayland_buffers_prepare (impl, &info);
  if (res == GDKW_PREPARE_NO_FREE_SLOT)
    {
      f->commit_wanted = TRUE;
      f->c.stalled_no_buffer++;
      return FALSE;
    }
  if (res != GDKW_PREPARE_OK || info.wl_buffer == NULL)
    {
      frame_retry_later (impl, f);
      return FALSE;
    }

  ds = frame_ds (impl);

  /* set_buffer_scale and the viewport go out in this very commit */
  _gdk_wayland_scale_send_state (impl, info.scale120, info.logical_width,
                                 info.logical_height);

  GDKW_WIRE_ATTACH (impl->wl_surface, info.wl_buffer);
  f->c.attaches++;

  full = info.full || info.n_rects <= 0;
  f->c.damage_calls += send_damage (impl, &info, full);

  frame_destroy_callback (f);
  f->callback = GDKW_WIRE_FRAME (impl->wl_surface);
  wl_callback_add_listener (f->callback, &frame_listener, f);
  ds->callbacks++;
  f->c.frame_requests++;

  GDKW_WIRE_COMMIT (impl->wl_surface);

  _gdk_wayland_buffers_commit_done (impl, &info);

  /* state */
  f->state = GDKW_TEST_FRAME_WAITING;
  f->ever_committed = TRUE;
  f->need_full = FALSE;
  f->commit_wanted = FALSE;
  f->serial++;
  f->commit_lw = info.logical_width;
  f->commit_lh = info.logical_height;
  f->commit_scale120 = info.scale120;
  f->last_store = impl->shm_data;
  frame_arm_timeout (f);

  /* counters and the record of the last commit */
  f->c.commits++;
  switch (reason)
    {
    case GDKW_COMMIT_CALLBACK: f->c.commits_from_callback++; break;
    case GDKW_COMMIT_RELEASE:  f->c.commits_from_release++;  break;
    case GDKW_COMMIT_TIMEOUT:  f->c.commits_from_timeout++;  break;
    case GDKW_COMMIT_SETTLE:   f->c.commits_from_settle++;   break;
    default:
      if (immediate)
        f->c.commits_immediate++;
      break;
    }

  f->c.last_commit_full = full;
  f->c.last_damage_n = 0;
  f->c.last_damage_area = 0;
  if (full)
    {
      f->c.last_damage[0].x = 0;
      f->c.last_damage[0].y = 0;
      f->c.last_damage[0].width = info.buffer_width;
      f->c.last_damage[0].height = info.buffer_height;
      f->c.last_damage_n = 1;
      f->c.last_damage_area = info.buffer_width * info.buffer_height;
    }
  else
    for (i = 0; i < info.n_rects; i++)
      {
        if (i < GDKW_TEST_MAX_DAMAGE)
          {
            f->c.last_damage[i] = info.rects[i];
            f->c.last_damage_n = i + 1;
          }
        f->c.last_damage_area += info.rects[i].width * info.rects[i].height;
      }
  f->c.last_attach_width = info.buffer_width;
  f->c.last_attach_height = info.buffer_height;
  f->c.last_slot = info.slot;
  f->c.last_bytes_copied = info.bytes_copied;

  /* a settle idle that has not run yet has nothing left to do */
  return TRUE;
}

/* The state machine entry for every reason (D6). */
static void
frame_request (GDKWWindowImplWayland *impl, GDKWFrame *f,
               GDKWCommitReason reason)
{
  gboolean external = (reason == GDKW_COMMIT_EXPOSE ||
                       reason == GDKW_COMMIT_FULL ||
                       reason == GDKW_COMMIT_CONFIGURE);

  if (external)
    f->c.commit_requests++;

  /* nothing to show: no surface, or xdg-shell does not allow a buffer yet */
  if (impl->wl_surface == NULL || !impl->configured)
    return;

  if (reason == GDKW_COMMIT_FULL)
    {
      _gdk_wayland_buffers_damage_all (impl);
      _gdk_wayland_buffers_unknown_dirty (impl, TRUE);
    }

  if (f->state == GDKW_TEST_FRAME_UNMAPPED)
    {
      /* first configure acked (or first present of a popup) */
      f->state = GDKW_TEST_FRAME_IDLE;
      f->need_full = TRUE;
    }

  if (f->state == GDKW_TEST_FRAME_WAITING)
    {
      f->commit_wanted = TRUE;
      if (external)
        f->c.coalesced++;
      return;
    }

  frame_commit (impl, f, reason, TRUE);
}

/* ------------------------------------------------------------------------
 * wl_callback done, timeout, settle
 * ------------------------------------------------------------------------ */

/* The shared body of the real listener and the fire / release_held hooks.
 * @bypass TRUE: ignore the HOLD and DROP modes. */
static void
frame_handle_done (GDKWFrame *f, struct wl_callback *cb, guint32 time,
                   gboolean bypass)
{
  GDKWWindowImplWayland *impl = f->impl;
  FrameDisplay *ds = frame_ds (impl);

  if (cb != f->callback)
    return;

  if (!bypass)
    {
      if (ds->mode == GDKW_TEST_FRAME_MODE_HOLD)
        {
          f->held_done = TRUE;
          f->held_time = time;
          return;
        }
      if (ds->mode == GDKW_TEST_FRAME_MODE_DROP)
        return;
    }

  frame_destroy_callback (f);
  frame_remove_timeout (f);
  f->c.frame_callbacks_done++;
  f->consecutive_timeouts = 0;
  f->timeout_ms = base_ms (ds);
  if (f->state == GDKW_TEST_FRAME_WAITING)
    f->state = GDKW_TEST_FRAME_IDLE;

  if (f->commit_wanted)
    frame_commit (impl, f, GDKW_COMMIT_CALLBACK, FALSE);
}

static void
frame_done (void *data, struct wl_callback *cb, uint32_t time)
{
  frame_handle_done (data, cb, time, FALSE);
}

static const struct wl_callback_listener frame_listener = {
  frame_done
};

static gboolean
frame_timeout_cb (gpointer data)
{
  GDKWFrame *f = data;
  GDKWWindowImplWayland *impl = f->impl;
  FrameDisplay *ds = frame_ds (impl);

  f->timeout_id = 0;
  if (ds->timers > 0)
    ds->timers--;

  if (ds->mode == GDKW_TEST_FRAME_MODE_HOLD)
    return G_SOURCE_REMOVE; /* suspended; re-armed when HOLD ends */

  f->c.frame_timeouts++;
  f->consecutive_timeouts++;

  frame_destroy_callback (f);
  if (f->state == GDKW_TEST_FRAME_WAITING)
    f->state = GDKW_TEST_FRAME_IDLE;

  /* backoff: 250, 500, 1000, 2000 ms (or the test override) */
  f->timeout_ms = MIN (f->timeout_ms * 2, cap_ms (ds));

  if (f->commit_wanted)
    frame_commit (impl, f, GDKW_COMMIT_TIMEOUT, FALSE);

  return G_SOURCE_REMOVE;
}

static gboolean
frame_settle_cb (gpointer data)
{
  GDKWFrame *f = data;
  GDKWWindowImplWayland *impl = f->impl;
  FrameDisplay *ds = frame_ds (impl);

  f->settle_id = 0;
  if (ds->settle_idles > 0)
    ds->settle_idles--;

  /* the redraw already committed: nothing to force */
  if (f->serial != f->settle_mark)
    return G_SOURCE_REMOVE;

  if (impl->wl_surface == NULL || !impl->configured)
    return G_SOURCE_REMOVE;

  _gdk_wayland_buffers_damage_all (impl);
  f->need_full = TRUE;
  frame_request (impl, f, GDKW_COMMIT_SETTLE);

  return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------------------
 * Public (internal) entries of the header
 * ------------------------------------------------------------------------ */

void
_gdk_wayland_frame_commit_request (GDKWWindowImplWayland *impl,
                                   GDKWCommitReason       reason)
{
  GDKWFrame *f;

  f = frame_get (impl, TRUE, FALSE);
  if (f == NULL)
    return;

  frame_request (impl, f, reason);
}

gboolean
_gdk_wayland_frame_in_expose (GDKWWindowImplWayland *impl)
{
  GDKWFrame *f = frame_get (impl, FALSE, FALSE);

  return f != NULL && f->scope_depth > 0;
}

void
_gdk_wayland_frame_configured (GDKWWindowImplWayland *impl)
{
  GDKWFrame *f = frame_get (impl, TRUE, FALSE);

  if (f == NULL)
    return;

  if (!f->ever_committed)
    {
      _gdk_wayland_buffers_damage_all (impl);
      f->need_full = TRUE;
      frame_request (impl, f, GDKW_COMMIT_CONFIGURE);
      return;
    }

  /* later configures: no commit.  A size or scale that differs from the last
   * commit is repaired by the settle idle if no redraw gets there first. */
  if ((gint) impl->width != f->commit_lw ||
      (gint) impl->height != f->commit_lh ||
      effective_scale (impl) != f->commit_scale120)
    frame_arm_settle (f);
}

/* D9.  The size part shared by the compositor resize and the client resize. */
static void
frame_replace_store (GDKWWindowImplWayland *impl, GDKWFrame *f)
{
  /* a store that does not exist yet is built at the new size when first used */
  if (impl->backing != NULL)
    _gdk_wayland_buffers_ensure (impl);
  if (f != NULL)
    f->need_full = TRUE;
}

gboolean
_gdk_wayland_frame_apply_configure_size (GDKWWindowImplWayland *impl,
                                         gint width, gint height)
{
  GDKWFrame *f;
  GdkWindowObject *private;

  if (impl == NULL || width <= 0 || height <= 0)
    return FALSE;
  if ((guint) width == impl->width && (guint) height == impl->height)
    return FALSE;

  f = frame_get (impl, TRUE, FALSE);

  impl->width = width;
  impl->height = height;

  /* new store first: detaches the cached cairo surfaces of the core, so the
   * size update below finds none to resize */
  frame_replace_store (impl, f);

  if (window_alive (impl->wrapper))
    {
      private = (GdkWindowObject *) impl->wrapper;
      private->width = width;
      private->height = height;
      _gdk_window_update_size (impl->wrapper);

      if (GDK_WINDOW_IS_MAPPED (impl->wrapper))
        gdk_window_invalidate_rect (impl->wrapper, NULL, TRUE);

      /* one GDK_CONFIGURE, before the state event of the same configure */
      queue_configure (impl, f);
    }

  if (f != NULL)
    frame_arm_settle (f);

  return TRUE;
}

void
_gdk_wayland_frame_client_resize (GDKWWindowImplWayland *impl,
                                  gint width, gint height)
{
  GDKWFrame *f;

  if (impl == NULL || width <= 0 || height <= 0)
    return;

  /* buffers already match: nothing changed */
  if (impl->backing != NULL)
    {
      guint s = effective_scale (impl);

      if (cairo_image_surface_get_width (impl->backing) ==
            GDKW_SCALE_PX (width, s) &&
          cairo_image_surface_get_height (impl->backing) ==
            GDKW_SCALE_PX (height, s))
        return;
    }

  f = frame_get (impl, TRUE, FALSE);

  impl->width = width;
  impl->height = height;

  frame_replace_store (impl, f);

  /* Wayland never answers a client size change with a configure */
  if (impl->xdg_surface != NULL && impl->popup == NULL && impl->configured)
    xdg_surface_set_window_geometry (impl->xdg_surface, 0, 0, width, height);

  if (window_alive (impl->wrapper))
    {
      /* the new store holds only the background */
      if (GDK_WINDOW_IS_MAPPED (impl->wrapper))
        gdk_window_invalidate_rect (impl->wrapper, NULL, TRUE);
      queue_configure (impl, f);
    }

  if (f != NULL)
    frame_arm_settle (f);
}

/* Scale of the store as set on impl->backing, 0 without a store. */
static guint
backing_scale120 (GDKWWindowImplWayland *impl)
{
  gdouble sx = 1.0, sy = 1.0;

  if (impl->backing == NULL)
    return 0;

  cairo_surface_get_device_scale (impl->backing, &sx, &sy);
  return (guint) (sx * GDKW_SCALE_DENOM + 0.5);
}

void
_gdk_wayland_frame_set_scale (GDKWWindowImplWayland *impl, guint scale120)
{
  GDKWFrame *f;
  GdkDisplay *display;
  guint current;

  if (impl == NULL)
    return;

  scale120 = CLAMP (scale120, GDKW_SCALE_MIN, GDKW_SCALE_MAX);

  f = frame_get (impl, TRUE, FALSE);
  if (f == NULL)
    {
      /* no wl_surface: only the store (if any) follows */
      if (backing_scale120 (impl) != 0 && backing_scale120 (impl) != scale120)
        _gdk_wayland_buffers_ensure (impl);
      return;
    }

  /* S1: the store already has this scale.  Do not compare with the last scale this
   * record saw: the record outlives the wl_surface (a GtkMenu window is shown again and
   * again, each time with a new surface that starts at the hint), so the last value
   * can equal the new one while the store of the new surface has the other scale. */
  current = backing_scale120 (impl);
  if (current == scale120)
    return;

  /* S2: new store, retired slots, detached core surfaces, FULL damage */
  frame_replace_store (impl, f);

  /* S3: the next commit calls _gdk_wayland_scale_send_state () */

  /* S4: only a mapped, configured window shows anything to repaint */
  if (impl->configured && window_alive (impl->wrapper) &&
      GDK_WINDOW_IS_MAPPED (impl->wrapper))
    {
      gdk_window_invalidate_rect (impl->wrapper, NULL, TRUE);
      queue_configure (impl, f);
    }

  /* S5 */
  if (impl->configured)
    frame_arm_settle (f);

  /* S6 */
  display = impl_display (impl);
  if (is_wayland_display (display) &&
      _gdk_wayland_display_get_seat (display)->pointer_focus == impl)
    _gdk_wayland_cursor_scale_changed (display);
}

void
_gdk_wayland_frame_unmap (GDKWWindowImplWayland *impl)
{
  GDKWFrame *f;

  if (impl == NULL)
    return;

  f = frame_get (impl, FALSE, FALSE);

  /* attach NULL + commit: the surface is not shown any more */
  if (impl->wl_surface != NULL)
    {
      f = frame_get (impl, TRUE, FALSE);
      GDKW_WIRE_ATTACH (impl->wl_surface, NULL);
      GDKW_WIRE_COMMIT (impl->wl_surface);
      if (f != NULL)
        {
          f->c.attach_null++;
          f->c.empty_commits++;
        }
    }

  if (f != NULL)
    frame_state_reset (f);

  /* the store and the slots go (cairo surfaces keep the store alive) */
  _gdk_wayland_buffers_reset (impl);

  /* The role objects go, the wl_surface stays (D8, F7).  A popup has its own
   * hide path and never comes here. */
  if (impl->popup == NULL)
    {
      if (impl->xdg_toplevel != NULL)
        {
          xdg_toplevel_destroy (impl->xdg_toplevel);
          _gdk_wayland_ledger_xdg (TRUE, FALSE);
          impl->xdg_toplevel = NULL;
        }
      if (impl->xdg_surface != NULL)
        {
          xdg_surface_destroy (impl->xdg_surface);
          _gdk_wayland_ledger_xdg (FALSE, FALSE);
          impl->xdg_surface = NULL;
        }
    }
  impl->configured = FALSE;
}

void
_gdk_wayland_frame_buffer_freed (GDKWWindowImplWayland *impl)
{
  GDKWFrame *f = frame_get (impl, FALSE, FALSE);

  if (f == NULL)
    return;

  if (f->commit_wanted && f->state != GDKW_TEST_FRAME_WAITING)
    {
      if (f->state == GDKW_TEST_FRAME_UNMAPPED)
        {
          if (impl->wl_surface == NULL || !impl->configured)
            {
              f->commit_wanted = FALSE;
              return;
            }
          f->state = GDKW_TEST_FRAME_IDLE;
        }
      frame_commit (impl, f, GDKW_COMMIT_RELEASE, FALSE);
    }
}

void
_gdk_wayland_frame_teardown (GDKWWindowImplWayland *impl)
{
  GDKWFrame *f;

  if (impl == NULL)
    return;

  f = impl->frame;
  if (f != NULL)
    {
      frame_destroy_callback (f);
      frame_remove_timeout (f);
      frame_remove_settle (f);
      impl->frame = NULL;
      g_free (f);
    }

  _gdk_wayland_buffers_free (impl);
}

/* ------------------------------------------------------------------------
 * Moved functions
 * ------------------------------------------------------------------------ */

/* FULL damage + a commit request through the real path. */
void
_gdk_wayland_window_impl_present (GDKWWindowImplWayland *impl)
{
  if (!impl)
    return;

  /* xdg-shell forbids committing a buffer before the xdg_surface has been
   * acked-configured at least once.  Until then, no-ops. */
  if (!impl->wl_surface || !impl->configured)
    return;

  /* clear outside the stored shape now, so the store is right even when the
   * commit itself waits for a frame callback (the commit clears again) */
  _gdk_wayland_shape_before_present (impl);

  _gdk_wayland_frame_commit_request (impl, GDKW_COMMIT_FULL);
}

/* Discards the store and the slots, sets the impl size, FULL damage when the
 * next store is built, detaches the core surfaces.  Valid when the wl_surface
 * is already gone. */
void
_gdk_wayland_window_impl_resize_buffer (GDKWWindowImplWayland *impl,
                                        gint width, gint height)
{
  GDKWFrame *f;

  if (impl == NULL)
    return;

  f = frame_get (impl, FALSE, FALSE);

  _gdk_wayland_buffers_reset (impl);

  impl->width = width;
  impl->height = height;

  if (f != NULL)
    {
      f->need_full = TRUE;
      /* the surface is gone: the frame cannot be waited for */
      if (impl->wl_surface == NULL)
        frame_state_reset (f);
    }
}

void
_gdk_wayland_display_present_all (GDKWDisplayWayland *display)
{
  GSList *copy, *l;
  gboolean in_cycle;

  /* M9: pending popups make their first commit here (gdk_flush and display
   * sync both come through).  It may add nodes to display->windows. */
  _gdk_wayland_popup_commit_pending (display);

  in_cycle = update_cycle_depth > 0;

  copy = g_slist_copy (display->windows);
  for (l = copy; l != NULL; l = l->next)
    {
      GDKWWindowImplWayland *impl = l->data;

      if (g_slist_find (display->windows, impl) == NULL)
        continue;
      if (!impl->wl_surface || !impl->configured)
        continue;

      /* Inside an update cycle the process_updates hook committed what was
       * exposed; only direct drawing that nobody announced is left. */
      if (in_cycle && !_gdk_wayland_buffers_unknown_dirty (impl, FALSE))
        continue;

      _gdk_wayland_frame_commit_request (impl, GDKW_COMMIT_FULL);
    }
  g_slist_free (copy);
}

void
_gdk_wayland_windowing_window_process_updates_recurse (GdkWindow *window,
                                                       GdkRegion *expose_region)
{
  GDKWWindowImplWayland *impl;
  GdkRegion *damage;
  GDKWFrame *f;

  impl = impl_from_window (window);
  if (impl == NULL || impl->wl_surface == NULL ||
      (f = frame_get (impl, TRUE, FALSE)) == NULL)
    {
      _gdk_window_process_updates_recurse (window, expose_region);
      return;
    }

  /* the core cuts children out of the region it is given */
  damage = gdk_region_copy (expose_region);

  g_object_ref (window);
  f->scope_depth++;
  if (!gdk_region_empty (damage))
    f->c.expose_scopes++;

  _gdk_window_process_updates_recurse (window, expose_region);

  /* the expose handlers may have destroyed or hidden the window */
  if (window_alive (window))
    {
      impl = impl_from_window (window);
      f = frame_get (impl, FALSE, FALSE);
      if (f != NULL)
        {
          if (f->scope_depth > 0)
            f->scope_depth--;

          if (!gdk_region_empty (damage) && impl->wl_surface != NULL)
            {
              _gdk_wayland_buffers_ensure (impl);
              _gdk_wayland_buffers_damage_region (impl, damage);
              _gdk_wayland_frame_commit_request (impl, GDKW_COMMIT_EXPOSE);
            }
        }
    }

  gdk_region_destroy (damage);
  g_object_unref (window);
}

void
_gdk_wayland_windowing_before_process_all_updates (void)
{
  update_cycle_depth++;
}

void
_gdk_wayland_windowing_after_process_all_updates (void)
{
  if (update_cycle_depth > 0)
    update_cycle_depth--;
}

void
_gdk_wayland_windowing_set_cairo_surface_size (cairo_surface_t *surface,
                                               int width,
                                               int height)
{
  /* Surfaces are replaced, not resized.  A surface of ours whose extent does
   * not cover the request is counted by the buffers piece. */
  if (surface != NULL)
    _gdk_wayland_buffers_surface_extent_ok (surface, width, height);
}

/* ------------------------------------------------------------------------
 * Test hooks
 * ------------------------------------------------------------------------ */

gboolean
_gdk_wayland_test_frame_info (GdkWindow *window, GDKWTestFrameInfo *info)
{
  GDKWWindowImplWayland *impl = impl_from_window (window);
  GDKWFrame *f;

  if (impl == NULL || info == NULL)
    return FALSE;

  f = frame_get (impl, TRUE, TRUE);
  if (f == NULL)
    return FALSE;

  *info = f->c;
  info->state = f->state;
  info->commit_wanted = f->commit_wanted;
  info->callback_outstanding = f->callback != NULL;
  info->held_done = f->held_done;
  info->ever_committed = f->ever_committed;
  info->timeout_armed = f->timeout_id != 0;
  info->settle_armed = f->settle_id != 0;
  info->unknown_dirty = _gdk_wayland_buffers_unknown_dirty (impl, FALSE);
  info->timeout_ms = f->timeout_ms;
  info->consecutive_timeouts = f->consecutive_timeouts;

  return TRUE;
}

gboolean
_gdk_wayland_test_frame_counts (GdkDisplay *display, GDKWTestFrameCounts *counts)
{
  FrameDisplay *ds;

  if (!is_wayland_display (display) || counts == NULL)
    return FALSE;

  ds = frame_display_for (display);
  counts->timers = ds->timers;
  counts->settle_idles = ds->settle_idles;
  counts->callbacks = ds->callbacks;
  counts->update_cycle_depth = update_cycle_depth;

  return TRUE;
}

gboolean
_gdk_wayland_test_frame_mode (GdkDisplay *display, GDKWTestFrameMode mode)
{
  FrameDisplay *ds;
  GSList *l;

  if (!is_wayland_display (display))
    return FALSE;
  if (mode != GDKW_TEST_FRAME_MODE_NORMAL &&
      mode != GDKW_TEST_FRAME_MODE_HOLD &&
      mode != GDKW_TEST_FRAME_MODE_DROP)
    return FALSE;

  ds = frame_display_for (display);
  ds->mode = mode;

  for (l = GDKW_DISPLAY_WAYLAND (display)->windows; l != NULL; l = l->next)
    {
      GDKWWindowImplWayland *impl = l->data;
      GDKWFrame *f = impl->frame;

      if (f == NULL)
        continue;

      if (mode == GDKW_TEST_FRAME_MODE_HOLD)
        frame_remove_timeout (f);               /* suspended */
      else if (f->state == GDKW_TEST_FRAME_WAITING && f->callback != NULL)
        frame_arm_timeout (f);
    }

  return TRUE;
}

gboolean
_gdk_wayland_test_frame_fire (GdkWindow *window)
{
  GDKWWindowImplWayland *impl = impl_from_window (window);
  GDKWFrame *f;

  if (impl == NULL)
    return FALSE;
  f = frame_get (impl, FALSE, FALSE);
  if (f == NULL || f->callback == NULL)
    return FALSE;

  frame_handle_done (f, f->callback, (guint32) (g_get_monotonic_time () / 1000),
                     TRUE);
  return TRUE;
}

gboolean
_gdk_wayland_test_frame_release_held (GdkWindow *window)
{
  GDKWWindowImplWayland *impl = impl_from_window (window);
  GDKWFrame *f;

  if (impl == NULL)
    return FALSE;
  f = frame_get (impl, FALSE, FALSE);
  if (f == NULL || !f->held_done || f->callback == NULL)
    return FALSE;

  frame_handle_done (f, f->callback, f->held_time, TRUE);
  return TRUE;
}

gboolean
_gdk_wayland_test_frame_timeouts (GdkDisplay *display, guint first_ms,
                                  guint max_ms)
{
  FrameDisplay *ds;
  GSList *l;

  if (!is_wayland_display (display))
    return FALSE;

  ds = frame_display_for (display);
  ds->first_ms = first_ms;
  ds->max_ms = max_ms;

  for (l = GDKW_DISPLAY_WAYLAND (display)->windows; l != NULL; l = l->next)
    {
      GDKWWindowImplWayland *impl = l->data;
      GDKWFrame *f = impl->frame;

      if (f != NULL)
        {
          f->timeout_ms = base_ms (ds);
          f->consecutive_timeouts = 0;
        }
    }

  return TRUE;
}

gboolean
_gdk_wayland_test_frame_reset_counters (GdkWindow *window)
{
  GDKWWindowImplWayland *impl = impl_from_window (window);
  GDKWFrame *f;
  GDKWTestFrameInfo saved;

  if (impl == NULL)
    return FALSE;
  f = frame_get (impl, TRUE, TRUE);
  if (f == NULL)
    return FALSE;

  /* keep the record of the last commit, zero the counters */
  saved = f->c;
  memset (&f->c, 0, sizeof (f->c));
  f->c.last_commit_full = saved.last_commit_full;
  f->c.last_damage_n = saved.last_damage_n;
  memcpy (f->c.last_damage, saved.last_damage, sizeof (f->c.last_damage));
  f->c.last_damage_area = saved.last_damage_area;
  f->c.last_attach_width = saved.last_attach_width;
  f->c.last_attach_height = saved.last_attach_height;
  f->c.last_slot = saved.last_slot;
  f->c.last_bytes_copied = saved.last_bytes_copied;

  return TRUE;
}

gboolean
_gdk_wayland_test_frame_present (GdkWindow *window)
{
  GDKWWindowImplWayland *impl = impl_from_window (window);

  if (impl == NULL)
    return FALSE;

  _gdk_wayland_window_impl_present (impl);
  return TRUE;
}

#ifdef GDK_COMPILATION
#include "gdkalias.h"
#endif

/* ------------------------------------------------------------------------
 * Wire log and pre-v4 damage switch (audit gaps of M10)
 *
 * The GDKW_WIRE_* wrappers of gdkm10-wayland.h call _gdk_wayland_wire_log
 * only while _gdkw_wire_enabled is TRUE.  The log is a fixed array; entries
 * past the end are counted, not stored.
 * ------------------------------------------------------------------------ */

gboolean _gdkw_wire_enabled = FALSE;
gboolean _gdkw_damage_pre_v4 = FALSE;

static GDKWTestWireEntry wire_log[GDKW_TEST_WIRE_MAX];
static guint wire_n = 0;
static guint wire_dropped = 0;

void
_gdk_wayland_wire_log (GDKWTestWireKind kind, gconstpointer surface,
                       gint a, gint b, gint c, gint d)
{
  GDKWTestWireEntry *e;

  if (wire_n >= GDKW_TEST_WIRE_MAX)
    {
      wire_dropped++;
      return;
    }
  e = &wire_log[wire_n++];
  e->kind = kind;
  e->surface = surface;
  e->a = a;
  e->b = b;
  e->c = c;
  e->d = d;
}

gboolean
_gdk_wayland_test_wire_enable (gboolean on)
{
  wire_n = 0;
  wire_dropped = 0;
  _gdkw_wire_enabled = on != FALSE;
  return TRUE;
}

void
_gdk_wayland_test_wire_clear (void)
{
  wire_n = 0;
  wire_dropped = 0;
}

guint
_gdk_wayland_test_wire_count (guint *dropped)
{
  if (dropped != NULL)
    *dropped = wire_dropped;
  return wire_n;
}

gboolean
_gdk_wayland_test_wire_get (guint index, GDKWTestWireEntry *entry)
{
  if (entry == NULL || index >= wire_n)
    return FALSE;
  *entry = wire_log[index];
  return TRUE;
}

gconstpointer
_gdk_wayland_test_wire_surface (GdkWindow *window)
{
  GDKWWindowImplWayland *impl = impl_from_window (window);

  return impl != NULL ? impl->wl_surface : NULL;
}

gboolean
_gdk_wayland_test_damage_pre_v4_force (gboolean enable)
{
  _gdkw_damage_pre_v4 = enable != FALSE;
  return TRUE;
}
