/*
 * gdkbuffer-wayland.c
 *
 * ydk-wayland fork, M10 (piece "buffers"): the back store, every cairo
 * surface over it, the shm slot pool and its states, wl_buffer release,
 * per slot stale regions, device pixel damage, the copy into a free slot,
 * paint_background, the window pixel reader and the buffer test hooks.
 *
 * The decisions (D1 to D17) and the contract with the other pieces and the
 * integrator are in ydk/wayland/ydk/gdkm10-wayland.h.  This file owns D1, D3,
 * D4 and D5 and the "buffers" hooks of D16.
 *
 * Rules kept here:
 *   - GDK draws on the back store (plain memory).  The compositor never sees
 *     it.  A slot is written ONLY while it is FREE (or an unattached COPYING
 *     leftover), and only by copying its stale region from the store.
 *   - Every cairo surface over the store holds one store reference through
 *     user data, so nothing frees memory under a live surface.
 *   - Only this file creates, writes and destroys wl_buffers.
 *
 * No Xlib here.  No public gdk_* name is dispatched from this file.
 */

#include "config.h"

#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include <wayland-client.h>
#include <cairo.h>

#include <ydk/gdk.h>
#include <ydk/gdkinternals.h>

#include "gdkprivate-wayland.h"
#include "gdkseat-wayland.h"
#include "gdkm7-wayland.h"
#include "gdkm9-wayland.h"
#include "gdkm10-wayland.h"

/* =========================================================================
 * Types and statics
 * ========================================================================= */

typedef struct
{
  gint     refcount;
  guchar  *data;
  gsize    size;
  gint     stride;
  gint     px_w, px_h;          /* device pixels */
  gint     lw, lh;              /* logical */
  guint    scale120;
  guint64  generation;
  guint    surfaces;            /* cairo surfaces holding this store */
} GDKWBackStore;

typedef struct
{
  GDKWBuffers      *owner;
  guint             index;
  GDKWSlotState     state;
  struct wl_buffer *wl_buffer;
  guchar           *data;
  gsize             size;
  gint              stride;
  gint              w, h;       /* device pixels */
  guint64           generation;
  GdkRegion        *stale;      /* device pixels still to copy */
  guint             times_attached;
  guint             released_count;
  gboolean          held_release;
  gboolean          poisoned;
} GDKWSlot;

struct _GDKWBuffers
{
  GDKWWindowImplWayland *impl;
  GDKWBackStore         *store;       /* the record's own reference */
  GPtrArray             *slots;       /* GDKWSlot *, NULL = hole */
  GdkRegion             *damage;      /* frame damage, device pixels */
  gboolean               damage_all;
  gboolean               unknown_dirty;
};

static cairo_user_data_key_t store_key;

static guint64 g_generation;
static guint   g_stores_created, g_stores_freed;
static guint   g_wrappers_created;
static guint   g_size_mismatches;
static guint   g_buffers_created, g_buffers_destroyed;
static guint   g_live_buffers;
static guint   g_retired;
static GDKWTestBufferMode g_mode = GDKW_TEST_BUFFER_MODE_NORMAL;
static guint   g_limit = 0;

static const struct wl_buffer_listener slot_listener;

/* =========================================================================
 * Small helpers
 * ========================================================================= */

static GDKWDisplayWayland *
impl_display (GDKWWindowImplWayland *impl)
{
  GdkDisplay *d;

  if (impl == NULL || impl->screen == NULL)
    return NULL;
  d = gdk_screen_get_display (impl->screen);
  if (d == NULL || !GDKW_IS_DISPLAY_WAYLAND (d))
    return NULL;
  return GDKW_DISPLAY_WAYLAND (d);
}

static guint
max_slots (void)
{
  return g_limit != 0 ? g_limit : GDKW_FRAME_MAX_BUFFERS;
}

static guint
current_scale (GDKWWindowImplWayland *impl)
{
  guint s = _gdk_wayland_scale_surface_scale120 (impl);

  if (s < GDKW_SCALE_MIN)
    s = GDKW_SCALE_MIN;
  if (s > GDKW_SCALE_MAX)
    s = GDKW_SCALE_MAX;
  return s;
}

static GDKWWindowImplWayland *
impl_from_window (GdkWindow *window)
{
  GdkWindowObject *wobj;

  if (window == NULL || !GDK_IS_WINDOW (window))
    return NULL;
  wobj = (GdkWindowObject *) window;
  if (wobj->impl == NULL || !GDKW_IS_WINDOW_IMPL_WAYLAND (wobj->impl))
    return NULL;
  return GDKW_WINDOW_IMPL_WAYLAND (wobj->impl);
}

static gboolean
display_ok (GdkDisplay *display)
{
  return display != NULL && GDKW_IS_DISPLAY_WAYLAND (display);
}

static gint
region_area (const GdkRegion *region)
{
  GdkRectangle *rects = NULL;
  gint n = 0, i, area = 0;

  if (region == NULL)
    return 0;
  gdk_region_get_rectangles (region, &rects, &n);
  for (i = 0; i < n; i++)
    area += rects[i].width * rects[i].height;
  g_free (rects);
  return area;
}

static void
set_state (GDKWSlot *s, GDKWSlotState state)
{
  if (s->state == GDKW_TEST_SLOT_RETIRED && g_retired > 0)
    g_retired--;
  if (state == GDKW_TEST_SLOT_RETIRED)
    g_retired++;
  s->state = state;
}

/* =========================================================================
 * Back store
 * ========================================================================= */

static GDKWBackStore *
store_ref (GDKWBackStore *store)
{
  store->refcount++;
  return store;
}

static void
store_unref (GDKWBackStore *store)
{
  if (store == NULL)
    return;
  if (--store->refcount > 0)
    return;
  g_free (store->data);
  g_free (store);
  g_stores_freed++;
}

/* destroy notify of every surface that holds the store */
static void
surface_store_release (gpointer data)
{
  GDKWBackStore *store = data;

  if (store->surfaces > 0)
    store->surfaces--;
  store_unref (store);
}

static GDKWBackStore *
store_new (gint lw, gint lh, guint scale120)
{
  GDKWBackStore *store;
  gint px_w = GDKW_SCALE_PX (lw, scale120);
  gint px_h = GDKW_SCALE_PX (lh, scale120);
  gint stride;
  gsize size, i;
  guint32 *pixels;

  if (px_w < 1)
    px_w = 1;
  if (px_h < 1)
    px_h = 1;
  if (px_w > 32767 || px_h > 32767)
    return NULL;

  stride = cairo_format_stride_for_width (CAIRO_FORMAT_ARGB32, px_w);
  if (stride <= 0)
    return NULL;
  size = (gsize) stride * px_h;
  if (size > G_MAXINT32)
    return NULL;

  store = g_try_new0 (GDKWBackStore, 1);
  if (store == NULL)
    return NULL;
  store->data = g_try_malloc (size);
  if (store->data == NULL)
    {
      g_free (store);
      return NULL;
    }

  store->refcount = 1;
  store->size = size;
  store->stride = stride;
  store->px_w = px_w;
  store->px_h = px_h;
  store->lw = lw;
  store->lh = lh;
  store->scale120 = scale120;
  store->generation = ++g_generation;

  /* opaque black, matching an X11 background pixel of 0 */
  pixels = (guint32 *) store->data;
  for (i = 0; i < size / 4; i++)
    pixels[i] = 0xff000000;

  g_stores_created++;
  return store;
}

/* A fresh cairo image surface over the store with the device scale set and
 * one store reference in its user data.  NULL on failure. */
static cairo_surface_t *
surface_over_store (GDKWBackStore *store)
{
  cairo_surface_t *surface;
  gdouble f = store->scale120 / (gdouble) GDKW_SCALE_DENOM;

  surface = cairo_image_surface_create_for_data (store->data,
                                                 CAIRO_FORMAT_ARGB32,
                                                 store->px_w, store->px_h,
                                                 store->stride);
  if (cairo_surface_status (surface) != CAIRO_STATUS_SUCCESS)
    {
      cairo_surface_destroy (surface);
      return NULL;
    }

  if (cairo_surface_set_user_data (surface, &store_key, store_ref (store),
                                   surface_store_release)
      != CAIRO_STATUS_SUCCESS)
    {
      store_unref (store);
      cairo_surface_destroy (surface);
      return NULL;
    }
  store->surfaces++;

  cairo_surface_set_device_scale (surface, f, f);
  return surface;
}

/* =========================================================================
 * Slots
 * ========================================================================= */

static GDKWSlot *
slot_at (GDKWBuffers *rec, guint index)
{
  if (rec == NULL || index >= rec->slots->len)
    return NULL;
  return g_ptr_array_index (rec->slots, index);
}

static void
slot_destroy (GDKWSlot *s)
{
  GDKWBuffers *rec = s->owner;

  if (s->state == GDKW_TEST_SLOT_RETIRED && g_retired > 0)
    g_retired--;

  if (rec->impl != NULL && rec->impl->wl_buffer == s->wl_buffer)
    rec->impl->wl_buffer = NULL;

  if (s->wl_buffer != NULL)
    wl_buffer_destroy (s->wl_buffer);
  if (s->data != NULL)
    munmap (s->data, s->size);
  if (s->stale != NULL)
    gdk_region_destroy (s->stale);

  g_ptr_array_index (rec->slots, s->index) = NULL;
  g_free (s);

  if (g_live_buffers > 0)
    g_live_buffers--;
  g_buffers_destroyed++;
}

static void
slot_mark_full_stale (GDKWSlot *s)
{
  GdkRectangle r;

  if (s->stale != NULL)
    gdk_region_destroy (s->stale);
  r.x = 0;
  r.y = 0;
  r.width = s->w;
  r.height = s->h;
  s->stale = gdk_region_rectangle (&r);
}

static GDKWSlot *
slot_create (GDKWBuffers *rec)
{
  GDKWWindowImplWayland *impl = rec->impl;
  GDKWBackStore *store = rec->store;
  GDKWDisplayWayland *display = impl_display (impl);
  struct wl_shm_pool *pool;
  GDKWSlot *s;
  gsize size;
  guint i, index;
  guchar *map;
  int fd;

  if (display == NULL || display->wl_shm == NULL || store == NULL)
    return NULL;

  size = store->size;
  fd = memfd_create ("ydk-wayland-buffer", MFD_CLOEXEC);
  if (fd < 0)
    return NULL;
  if (ftruncate (fd, size) < 0)
    {
      close (fd);
      return NULL;
    }
  map = mmap (NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (map == MAP_FAILED)
    {
      close (fd);
      return NULL;
    }

  s = g_new0 (GDKWSlot, 1);
  s->owner = rec;
  s->data = map;
  s->size = size;
  s->stride = store->stride;
  s->w = store->px_w;
  s->h = store->px_h;
  s->generation = store->generation;
  slot_mark_full_stale (s);

  pool = wl_shm_create_pool (display->wl_shm, fd, size);
  s->wl_buffer = wl_shm_pool_create_buffer (pool, 0, s->w, s->h, s->stride,
                                            WL_SHM_FORMAT_ARGB8888);
  wl_shm_pool_destroy (pool);
  close (fd);           /* libwayland dup'ed it into the request */
  wl_buffer_add_listener (s->wl_buffer, &slot_listener, s);

  index = rec->slots->len;
  for (i = 0; i < rec->slots->len; i++)
    if (g_ptr_array_index (rec->slots, i) == NULL)
      {
        index = i;
        break;
      }
  s->index = index;
  s->state = GDKW_TEST_SLOT_COPYING;
  if (index == rec->slots->len)
    g_ptr_array_add (rec->slots, s);
  else
    g_ptr_array_index (rec->slots, index) = s;

  g_live_buffers++;
  g_buffers_created++;
  return s;
}

/* D4: geometry is obsolete.  FREE and COPYING slots go now, ATTACHED and
 * SUPERSEDED become RETIRED and die at release or teardown. */
static void
retire_all (GDKWBuffers *rec)
{
  guint i;

  for (i = 0; i < rec->slots->len; i++)
    {
      GDKWSlot *s = g_ptr_array_index (rec->slots, i);

      if (s == NULL)
        continue;
      if (s->state == GDKW_TEST_SLOT_FREE ||
          s->state == GDKW_TEST_SLOT_COPYING)
        slot_destroy (s);
      else if (s->state != GDKW_TEST_SLOT_RETIRED)
        set_state (s, GDKW_TEST_SLOT_RETIRED);
    }

  /* A compositor that never releases (a hidden or occluded window that keeps
   * resizing) would otherwise keep one full size mapping per geometry change.
   * Destroying a wl_buffer the compositor still holds is legal for wl_shm; keep
   * only the newest few retired slots. */
  for (;;)
    {
      GDKWSlot *oldest = NULL;
      guint n = 0;

      for (i = 0; i < rec->slots->len; i++)
        {
          GDKWSlot *s = g_ptr_array_index (rec->slots, i);

          if (s == NULL || s->state != GDKW_TEST_SLOT_RETIRED)
            continue;
          n++;
          if (oldest == NULL || s->generation < oldest->generation)
            oldest = s;
        }
      if (n <= GDKW_FRAME_MAX_BUFFERS || oldest == NULL)
        break;
      slot_destroy (oldest);
    }
}

static void
destroy_all_slots (GDKWBuffers *rec)
{
  guint i;

  for (i = 0; i < rec->slots->len; i++)
    {
      GDKWSlot *s = g_ptr_array_index (rec->slots, i);

      if (s != NULL)
        slot_destroy (s);
    }
  g_ptr_array_set_size (rec->slots, 0);
}

static guint
live_unretired (GDKWBuffers *rec)
{
  guint i, n = 0;

  for (i = 0; i < rec->slots->len; i++)
    {
      GDKWSlot *s = g_ptr_array_index (rec->slots, i);

      if (s != NULL && s->state != GDKW_TEST_SLOT_RETIRED)
        n++;
    }
  return n;
}

/* The wl_buffer.release handler (real or hook driven). */
static void
slot_do_release (GDKWSlot *s)
{
  GDKWWindowImplWayland *impl = s->owner->impl;

  s->released_count++;
  s->held_release = FALSE;

  switch (s->state)
    {
    case GDKW_TEST_SLOT_RETIRED:
      slot_destroy (s);                         /* R4 */
      break;
    case GDKW_TEST_SLOT_ATTACHED:               /* R3, early release */
    case GDKW_TEST_SLOT_SUPERSEDED:
      set_state (s, GDKW_TEST_SLOT_FREE);
      break;
    default:
      break;                                    /* already free */
    }

  _gdk_wayland_frame_buffer_freed (impl);
}

static void
slot_handle_release (void *data, struct wl_buffer *wl_buffer)
{
  GDKWSlot *s = data;

  if (g_mode == GDKW_TEST_BUFFER_MODE_HOLD)
    {
      s->held_release = TRUE;
      return;
    }
  slot_do_release (s);
}

static const struct wl_buffer_listener slot_listener = {
  slot_handle_release
};

/* =========================================================================
 * Record
 * ========================================================================= */

GDKWBuffers *
_gdk_wayland_buffers_get (GDKWWindowImplWayland *impl, gboolean create)
{
  GDKWBuffers *rec;

  if (impl == NULL)
    return NULL;
  if (impl->buffers == NULL && create)
    {
      rec = g_new0 (GDKWBuffers, 1);
      rec->impl = impl;
      rec->slots = g_ptr_array_new ();
      rec->damage = gdk_region_new ();
      impl->buffers = rec;
    }
  return impl->buffers;
}

static void
detach_core_surfaces (GDKWWindowImplWayland *impl)
{
  if (impl->wrapper != NULL && !GDK_WINDOW_DESTROYED (impl->wrapper))
    _gdk_window_detach_cairo_surfaces (impl->wrapper);
}

static void
paint_store_background (GDKWWindowImplWayland *impl)
{
  cairo_t *cr;

  if (impl->backing == NULL)
    return;

  cr = cairo_create (impl->backing);
  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_rgb (cr,
                        impl->background.red / 65535.0,
                        impl->background.green / 65535.0,
                        impl->background.blue / 65535.0);
  cairo_paint (cr);
  cairo_destroy (cr);
}

/* Drops the store and the backing, retires the slots, clears the legacy
 * fields.  Does not touch damage flags or the core surfaces. */
static void
drop_store (GDKWBuffers *rec)
{
  GDKWWindowImplWayland *impl = rec->impl;

  retire_all (rec);

  if (impl->backing != NULL)
    {
      cairo_surface_destroy (impl->backing);
      impl->backing = NULL;
    }
  if (rec->store != NULL)
    {
      store_unref (rec->store);
      rec->store = NULL;
    }
  impl->shm_data = NULL;
  impl->shm_size = 0;
  impl->stride = 0;
  impl->shm_fd = -1;
}

static void
clear_damage (GDKWBuffers *rec)
{
  gdk_region_destroy (rec->damage);
  rec->damage = gdk_region_new ();
  rec->damage_all = FALSE;
}

gboolean
_gdk_wayland_buffers_ensure (GDKWWindowImplWayland *impl)
{
  GDKWDisplayWayland *display;
  GDKWBuffers *rec;
  GDKWBackStore *store;
  guint scale120;
  gboolean had_old;

  display = impl_display (impl);
  if (display == NULL || display->wl_shm == NULL)
    return FALSE;
  if (impl->width == 0 || impl->height == 0 ||
      impl->width > 32767 || impl->height > 32767)
    return FALSE;

  scale120 = current_scale (impl);
  rec = _gdk_wayland_buffers_get (impl, TRUE);

  if (rec->store != NULL && impl->backing != NULL &&
      rec->store->lw == (gint) impl->width &&
      rec->store->lh == (gint) impl->height &&
      rec->store->scale120 == scale120)
    return TRUE;

  had_old = (rec->store != NULL);
  drop_store (rec);

  store = store_new (impl->width, impl->height, scale120);
  if (store == NULL)
    {
      if (had_old)
        detach_core_surfaces (impl);
      return FALSE;
    }

  impl->backing = surface_over_store (store);
  if (impl->backing == NULL)
    {
      store_unref (store);
      if (had_old)
        detach_core_surfaces (impl);
      return FALSE;
    }
  rec->store = store;
  impl->shm_data = store->data;
  impl->shm_size = store->size;
  impl->stride = store->stride;
  impl->shm_fd = -1;

  paint_store_background (impl);
  clear_damage (rec);
  _gdk_wayland_buffers_damage_all (impl);

  if (had_old)
    detach_core_surfaces (impl);

  return TRUE;
}

cairo_surface_t *
_gdk_wayland_buffers_ref_backing (GDKWWindowImplWayland *impl)
{
  GDKWBuffers *rec;

  if (!_gdk_wayland_buffers_ensure (impl))
    return NULL;

  rec = impl->buffers;
  if (!_gdk_wayland_frame_in_expose (impl))
    rec->unknown_dirty = TRUE;

  return cairo_surface_reference (impl->backing);
}

cairo_surface_t *
_gdk_wayland_buffers_ref_backing_ro (GDKWWindowImplWayland *impl)
{
  if (!_gdk_wayland_buffers_ensure (impl))
    return NULL;
  return cairo_surface_reference (impl->backing);
}

cairo_surface_t *
_gdk_wayland_buffers_create_wrapper (GDKWWindowImplWayland *impl,
                                     gint width, gint height)
{
  cairo_surface_t *surface;

  /* the store follows the impl; a different request is ignored (D3) */
  if (!_gdk_wayland_buffers_ensure (impl))
    return NULL;

  surface = surface_over_store (impl->buffers->store);
  if (surface != NULL)
    g_wrappers_created++;
  return surface;
}

void
_gdk_wayland_buffers_paint_background (GDKWWindowImplWayland *impl)
{
  GDKWBuffers *rec = _gdk_wayland_buffers_get (impl, FALSE);

  if (rec == NULL || rec->store == NULL || impl->backing == NULL)
    return;

  paint_store_background (impl);
  _gdk_wayland_buffers_damage_all (impl);
}

/* =========================================================================
 * Damage (D5)
 * ========================================================================= */

static GdkRegion *
region_to_device (GDKWBackStore *store, const GdkRegion *region)
{
  GdkRegion *out = gdk_region_new ();
  GdkRectangle *rects = NULL;
  gint n = 0, i;

  gdk_region_get_rectangles (region, &rects, &n);
  for (i = 0; i < n; i++)
    {
      gint64 lx0 = MAX (rects[i].x, 0);
      gint64 ly0 = MAX (rects[i].y, 0);
      gint64 lx1 = (gint64) rects[i].x + rects[i].width;
      gint64 ly1 = (gint64) rects[i].y + rects[i].height;
      gint64 s = store->scale120;
      gint64 x0, y0, x1, y1;
      GdkRectangle r;

      if (lx1 <= lx0 || ly1 <= ly0)
        continue;
      /* floor on the low edge, ceil on the high edge, clamped (D2) */
      x0 = (lx0 * s) / GDKW_SCALE_DENOM;
      y0 = (ly0 * s) / GDKW_SCALE_DENOM;
      x1 = (lx1 * s + GDKW_SCALE_DENOM - 1) / GDKW_SCALE_DENOM;
      y1 = (ly1 * s + GDKW_SCALE_DENOM - 1) / GDKW_SCALE_DENOM;
      x1 = MIN (x1, store->px_w);
      y1 = MIN (y1, store->px_h);
      if (x1 <= x0 || y1 <= y0)
        continue;
      r.x = x0;
      r.y = y0;
      r.width = x1 - x0;
      r.height = y1 - y0;
      gdk_region_union_with_rect (out, &r);
    }
  g_free (rects);
  return out;
}

void
_gdk_wayland_buffers_damage_region (GDKWWindowImplWayland *impl,
                                    const GdkRegion *region)
{
  GDKWBuffers *rec = _gdk_wayland_buffers_get (impl, FALSE);
  GdkRegion *d;
  guint i;

  if (rec == NULL || rec->store == NULL || region == NULL)
    return;

  d = region_to_device (rec->store, region);
  if (!gdk_region_empty (d))
    {
      gdk_region_union (rec->damage, d);
      for (i = 0; i < rec->slots->len; i++)
        {
          GDKWSlot *s = g_ptr_array_index (rec->slots, i);

          if (s != NULL && s->state != GDKW_TEST_SLOT_RETIRED)
            gdk_region_union (s->stale, d);
        }
    }
  gdk_region_destroy (d);
}

void
_gdk_wayland_buffers_damage_all (GDKWWindowImplWayland *impl)
{
  GDKWBuffers *rec = _gdk_wayland_buffers_get (impl, FALSE);
  guint i;

  if (rec == NULL || rec->store == NULL)
    return;

  rec->damage_all = TRUE;
  for (i = 0; i < rec->slots->len; i++)
    {
      GDKWSlot *s = g_ptr_array_index (rec->slots, i);

      if (s != NULL && s->state != GDKW_TEST_SLOT_RETIRED)
        slot_mark_full_stale (s);
    }
}

gboolean
_gdk_wayland_buffers_has_damage (GDKWWindowImplWayland *impl)
{
  GDKWBuffers *rec = _gdk_wayland_buffers_get (impl, FALSE);

  if (rec == NULL)
    return FALSE;
  return rec->damage_all || !gdk_region_empty (rec->damage);
}

gboolean
_gdk_wayland_buffers_unknown_dirty (GDKWWindowImplWayland *impl,
                                    gboolean clear)
{
  GDKWBuffers *rec = _gdk_wayland_buffers_get (impl, FALSE);
  gboolean v;

  if (rec == NULL)
    return FALSE;
  v = rec->unknown_dirty;
  if (clear)
    rec->unknown_dirty = FALSE;
  return v;
}

/* =========================================================================
 * Prepare and commit_done
 * ========================================================================= */

static gsize
copy_stale (GDKWSlot *s, GDKWBackStore *st)
{
  GdkRectangle *rects = NULL;
  gint n = 0, i;
  gsize bytes = 0;

  gdk_region_get_rectangles (s->stale, &rects, &n);
  for (i = 0; i < n; i++)
    {
      gint x0 = MAX (rects[i].x, 0);
      gint y0 = MAX (rects[i].y, 0);
      gint x1 = MIN (rects[i].x + rects[i].width, st->px_w);
      gint y1 = MIN (rects[i].y + rects[i].height, st->px_h);
      gint y;

      if (x1 <= x0 || y1 <= y0)
        continue;
      for (y = y0; y < y1; y++)
        memcpy (s->data + (gsize) y * s->stride + (gsize) x0 * 4,
                st->data + (gsize) y * st->stride + (gsize) x0 * 4,
                (gsize) (x1 - x0) * 4);
      bytes += (gsize) (x1 - x0) * 4 * (y1 - y0);
    }
  g_free (rects);

  gdk_region_destroy (s->stale);
  s->stale = gdk_region_new ();
  return bytes;
}

static GDKWSlot *
pick_slot (GDKWBuffers *rec, GDKWPrepareResult *result)
{
  guint i;
  GDKWSlot *s;

  /* R2 (a FREE slot of an obsolete geometry goes at once) is done by
   * retire_all () when the geometry changes. */

  /* R1: lowest index FREE slot (an unattached COPYING leftover is safe too) */
  for (i = 0; i < rec->slots->len; i++)
    {
      s = g_ptr_array_index (rec->slots, i);
      if (s != NULL && s->generation == rec->store->generation &&
          (s->state == GDKW_TEST_SLOT_FREE ||
           s->state == GDKW_TEST_SLOT_COPYING))
        return s;
    }

  if (live_unretired (rec) < max_slots ())
    {
      s = slot_create (rec);
      if (s == NULL)
        *result = GDKW_PREPARE_NO_STORE;
      return s;
    }

  *result = GDKW_PREPARE_NO_FREE_SLOT;
  return NULL;
}

GDKWPrepareResult
_gdk_wayland_buffers_prepare (GDKWWindowImplWayland *impl,
                              GDKWCommitInfo *info)
{
  GDKWPrepareResult result = GDKW_PREPARE_OK;
  GDKWBuffers *rec;
  GDKWBackStore *st;
  GDKWSlot *s;

  if (info == NULL)
    return GDKW_PREPARE_NO_STORE;
  memset (info, 0, sizeof *info);

  if (!_gdk_wayland_buffers_ensure (impl))
    return GDKW_PREPARE_NO_STORE;
  rec = impl->buffers;
  st = rec->store;

  s = pick_slot (rec, &result);
  if (s == NULL)
    return result;

  set_state (s, GDKW_TEST_SLOT_COPYING);
  info->bytes_copied = copy_stale (s, st);
  s->poisoned = FALSE;

  info->wl_buffer = s->wl_buffer;
  info->slot = s->index;
  info->buffer_width = st->px_w;
  info->buffer_height = st->px_h;
  info->logical_width = st->lw;
  info->logical_height = st->lh;
  info->scale120 = st->scale120;

  if (rec->damage_all)
    {
      info->full = TRUE;
      info->n_rects = 1;
      info->rects[0].x = 0;
      info->rects[0].y = 0;
      info->rects[0].width = st->px_w;
      info->rects[0].height = st->px_h;
    }
  else
    {
      GdkRectangle *rects = NULL;
      gint n = 0, i;

      gdk_region_get_rectangles (rec->damage, &rects, &n);
      if (n > GDKW_MAX_DAMAGE_RECTS)
        {
          gdk_region_get_clipbox (rec->damage, &info->rects[0]);
          info->n_rects = 1;
        }
      else
        {
          for (i = 0; i < n; i++)
            info->rects[i] = rects[i];
          info->n_rects = n;
        }
      g_free (rects);

      if (info->n_rects == 1 &&
          info->rects[0].x == 0 && info->rects[0].y == 0 &&
          info->rects[0].width == st->px_w &&
          info->rects[0].height == st->px_h)
        info->full = TRUE;
    }

  return GDKW_PREPARE_OK;
}

void
_gdk_wayland_buffers_commit_done (GDKWWindowImplWayland *impl,
                                  const GDKWCommitInfo *info)
{
  GDKWBuffers *rec = _gdk_wayland_buffers_get (impl, FALSE);
  GDKWSlot *s, *o;
  guint i;

  if (rec == NULL || info == NULL)
    return;
  s = slot_at (rec, info->slot);
  if (s == NULL || s->wl_buffer != info->wl_buffer)
    return;

  for (i = 0; i < rec->slots->len; i++)
    {
      o = g_ptr_array_index (rec->slots, i);
      if (o != NULL && o != s && o->state == GDKW_TEST_SLOT_ATTACHED)
        set_state (o, GDKW_TEST_SLOT_SUPERSEDED);
    }

  set_state (s, GDKW_TEST_SLOT_ATTACHED);
  s->times_attached++;
  impl->wl_buffer = s->wl_buffer;
  clear_damage (rec);
}

/* =========================================================================
 * Reset and free
 * ========================================================================= */

void
_gdk_wayland_buffers_reset (GDKWWindowImplWayland *impl)
{
  GDKWBuffers *rec = _gdk_wayland_buffers_get (impl, FALSE);
  gboolean had_old;

  if (rec == NULL)
    return;

  had_old = (rec->store != NULL || impl->backing != NULL);
  drop_store (rec);
  clear_damage (rec);
  rec->unknown_dirty = FALSE;

  if (had_old)
    detach_core_surfaces (impl);
}

void
_gdk_wayland_buffers_free (GDKWWindowImplWayland *impl)
{
  GDKWBuffers *rec;

  if (impl == NULL || impl->buffers == NULL)
    return;
  rec = impl->buffers;

  destroy_all_slots (rec);
  if (impl->backing != NULL)
    {
      cairo_surface_destroy (impl->backing);
      impl->backing = NULL;
    }
  if (rec->store != NULL)
    {
      store_unref (rec->store);
      rec->store = NULL;
    }
  g_ptr_array_free (rec->slots, TRUE);
  gdk_region_destroy (rec->damage);
  g_free (rec);

  impl->buffers = NULL;
  impl->wl_buffer = NULL;
  impl->shm_data = NULL;
  impl->shm_size = 0;
  impl->stride = 0;
  impl->shm_fd = -1;
}

/* =========================================================================
 * Readers
 * ========================================================================= */

gboolean
_gdk_wayland_buffers_pixel (GDKWWindowImplWayland *impl, gint x, gint y,
                            guint32 *argb)
{
  GDKWBuffers *rec = _gdk_wayland_buffers_get (impl, FALSE);
  GDKWBackStore *st;
  gint64 dx, dy;

  if (rec == NULL || rec->store == NULL || x < 0 || y < 0)
    return FALSE;
  st = rec->store;
  if (x >= st->lw || y >= st->lh)
    return FALSE;

  dx = ((gint64) x * st->scale120) / GDKW_SCALE_DENOM;
  dy = ((gint64) y * st->scale120) / GDKW_SCALE_DENOM;
  if (dx >= st->px_w || dy >= st->px_h)
    return FALSE;

  if (argb != NULL)
    *argb = *(guint32 *) (st->data + (gsize) dy * st->stride + (gsize) dx * 4);
  return TRUE;
}

gboolean
_gdk_wayland_buffers_surface_extent_ok (cairo_surface_t *surface,
                                        gint width, gint height)
{
  GDKWBackStore *st;

  if (surface == NULL)
    return TRUE;
  st = cairo_surface_get_user_data (surface, &store_key);
  if (st == NULL)
    return TRUE;                /* not ours */

  if (st->lw >= width && st->lh >= height)
    return TRUE;

  g_size_mismatches++;
  g_debug ("ydk-wayland: cairo surface %dx%d does not cover %dx%d",
           st->lw, st->lh, width, height);
  return FALSE;
}

/* =========================================================================
 * Test hooks (D16)
 * ========================================================================= */

static GDKWSlot *
hook_slot (GdkWindow *window, guint index)
{
  GDKWWindowImplWayland *impl = impl_from_window (window);

  if (impl == NULL)
    return NULL;
  return slot_at (impl->buffers, index);
}

gboolean
_gdk_wayland_test_buffer_mode (GdkDisplay *display, GDKWTestBufferMode mode)
{
  if (!display_ok (display))
    return FALSE;
  if (mode != GDKW_TEST_BUFFER_MODE_NORMAL &&
      mode != GDKW_TEST_BUFFER_MODE_HOLD)
    return FALSE;
  g_mode = mode;
  return TRUE;
}

gboolean
_gdk_wayland_test_buffer_limit (GdkDisplay *display, guint max)
{
  if (!display_ok (display) || max > 64)
    return FALSE;
  g_limit = max;
  return TRUE;
}

guint
_gdk_wayland_test_buffer_count (GdkWindow *window)
{
  GDKWWindowImplWayland *impl = impl_from_window (window);
  guint i, n = 0;

  if (impl == NULL || impl->buffers == NULL)
    return 0;
  for (i = 0; i < impl->buffers->slots->len; i++)
    if (g_ptr_array_index (impl->buffers->slots, i) != NULL)
      n++;
  return n;
}

gboolean
_gdk_wayland_test_buffer_info (GdkWindow *window, guint index,
                               GDKWTestBufferInfo *info)
{
  GDKWSlot *s;

  if (info == NULL)
    return FALSE;
  memset (info, 0, sizeof *info);
  info->state = GDKW_TEST_SLOT_NONE;

  s = hook_slot (window, index);
  if (s == NULL)
    return FALSE;

  info->state = s->state;
  info->width = s->w;
  info->height = s->h;
  info->stride = s->stride;
  info->store_generation = s->generation;
  info->stale_area = region_area (s->stale);
  info->times_attached = s->times_attached;
  info->released_count = s->released_count;
  info->held_release = s->held_release;
  info->poisoned = s->poisoned;
  return TRUE;
}

gboolean
_gdk_wayland_test_buffer_release (GdkWindow *window, guint index)
{
  GDKWSlot *s = hook_slot (window, index);

  if (s == NULL)
    return FALSE;
  slot_do_release (s);
  return TRUE;
}

gboolean
_gdk_wayland_test_buffer_poison (GdkWindow *window, guint index,
                                 guint32 pattern)
{
  GDKWSlot *s = hook_slot (window, index);
  guint32 *p;
  gsize i;

  if (s == NULL)
    return FALSE;

  p = (guint32 *) s->data;
  for (i = 0; i < s->size / 4; i++)
    p[i] = pattern;
  s->poisoned = TRUE;
  slot_mark_full_stale (s);
  return TRUE;
}

gboolean
_gdk_wayland_test_buffer_pixel (GdkWindow *window, guint index,
                                gint x, gint y, guint32 *argb)
{
  GDKWSlot *s = hook_slot (window, index);

  if (s == NULL || x < 0 || y < 0 || x >= s->w || y >= s->h)
    return FALSE;
  if (argb != NULL)
    *argb = *(guint32 *) (s->data + (gsize) y * s->stride + (gsize) x * 4);
  return TRUE;
}

gboolean
_gdk_wayland_test_buffer_checksum (GdkWindow *window, guint index,
                                   guint64 *checksum)
{
  GDKWSlot *s = hook_slot (window, index);
  guint64 h = G_GUINT64_CONSTANT (0xcbf29ce484222325);
  gsize i;

  if (s == NULL || checksum == NULL)
    return FALSE;
  for (i = 0; i < s->size; i++)
    {
      h ^= s->data[i];
      h *= G_GUINT64_CONSTANT (0x100000001b3);
    }
  *checksum = h;
  return TRUE;
}

gboolean
_gdk_wayland_test_buffer_vs_backing (GdkWindow *window, guint index,
                                     guint *mismatched)
{
  GDKWWindowImplWayland *impl = impl_from_window (window);
  GDKWSlot *s = hook_slot (window, index);
  GDKWBackStore *st;
  gint x, y;
  guint n = 0;

  if (impl == NULL || s == NULL || mismatched == NULL ||
      impl->buffers == NULL || impl->buffers->store == NULL)
    return FALSE;
  st = impl->buffers->store;
  if (s->generation != st->generation || s->w != st->px_w || s->h != st->px_h)
    return FALSE;

  for (y = 0; y < s->h; y++)
    {
      const guint32 *a = (const guint32 *) (s->data + (gsize) y * s->stride);
      const guint32 *b = (const guint32 *) (st->data + (gsize) y * st->stride);

      for (x = 0; x < s->w; x++)
        if (a[x] != b[x])
          n++;
    }
  *mismatched = n;
  return TRUE;
}

gboolean
_gdk_wayland_test_backing_info (GdkWindow *window, GDKWTestBackingInfo *info)
{
  GDKWWindowImplWayland *impl = impl_from_window (window);
  GDKWBackStore *st;

  if (info == NULL)
    return FALSE;
  memset (info, 0, sizeof *info);
  info->device_scale_x = 1.0;
  info->device_scale_y = 1.0;

  if (impl == NULL || impl->buffers == NULL || impl->buffers->store == NULL)
    return FALSE;
  st = impl->buffers->store;

  info->data = st->data;
  info->stride = st->stride;
  info->px_width = st->px_w;
  info->px_height = st->px_h;
  info->logical_width = st->lw;
  info->logical_height = st->lh;
  info->scale120 = st->scale120;
  if (impl->backing != NULL)
    cairo_surface_get_device_scale (impl->backing, &info->device_scale_x,
                                    &info->device_scale_y);
  info->live_surfaces = st->surfaces;
  info->generation = st->generation;
  info->stores_created = g_stores_created;
  info->stores_freed = g_stores_freed;
  info->wrappers_created = g_wrappers_created;
  info->size_mismatches = g_size_mismatches;
  return TRUE;
}

gboolean
_gdk_wayland_test_buffer_counts (GdkDisplay *display,
                                 GDKWTestBufferCounts *counts)
{
  if (counts == NULL)
    return FALSE;
  memset (counts, 0, sizeof *counts);
  if (!display_ok (display))
    return FALSE;

  counts->live_wl_buffers = g_live_buffers;
  counts->retired_unreleased = g_retired;
  counts->live_stores = g_stores_created - g_stores_freed;
  counts->buffers_created = g_buffers_created;
  counts->buffers_destroyed = g_buffers_destroyed;
  counts->max_buffers = max_slots ();
  return TRUE;
}
