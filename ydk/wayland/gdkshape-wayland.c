/*
 * gdkshape-wayland.c
 *
 * ydk-wayland fork, M7 (piece "shape"): window shapes on Wayland.
 *
 * The bounding shape B and the input shape I of a toplevel are stored as
 * GdkRegion copies (qdata on the window impl).  B clears the pixels of the
 * shm backing outside it before every present and, for a depth 24 window,
 * becomes the opaque region.  The effective input region is B intersect I,
 * B, I or NULL (see ydk/gdkm7-wayland.h, DECISIONS: SHAPES).
 *
 * No Xlib here.  The public gdk_* names are dispatched by ydk/gdkbackend.c.
 */

#include "config.h"

#include <string.h>

#include <wayland-client.h>
#include <cairo.h>

#include <ydk/gdk.h>
#include <ydk/gdkcairo.h>
#include <ydk/gdkinternals.h>

#include "gdkprivate-wayland.h"
#include "gdkseat-wayland.h"
#include "gdkpixmap-wayland.h"
#include "gdkm7-wayland.h"

/* ------------------------------------------------------------------ */
/* State                                                              */
/* ------------------------------------------------------------------ */

static void
shape_state_free (gpointer data)
{
  GDKWShapeState *st = data;

  if (!st)
    return;
  if (st->bounding)
    gdk_region_destroy (st->bounding);
  if (st->input)
    gdk_region_destroy (st->input);
  if (st->sent_opaque)
    gdk_region_destroy (st->sent_opaque);
  if (st->sent_input)
    gdk_region_destroy (st->sent_input);
  g_free (st);
}

/* The impl of a live toplevel / temp window, else NULL. */
static GDKWWindowImplWayland *
shape_impl_from_window (GdkWindow *window)
{
  GdkWindowObject *private;

  if (!window || !GDK_IS_WINDOW (window))
    return NULL;

  private = (GdkWindowObject *) window;
  if (private->destroyed || !private->impl)
    return NULL;
  if (private->window_type != GDK_WINDOW_TOPLEVEL &&
      private->window_type != GDK_WINDOW_TEMP)
    return NULL;
  if (!GDKW_IS_WINDOW_IMPL_WAYLAND (private->impl))
    return NULL;

  return GDKW_WINDOW_IMPL_WAYLAND (private->impl);
}

static GDKWShapeState *
shape_state_get (GDKWWindowImplWayland *impl, gboolean create)
{
  GDKWShapeState *st;

  st = g_object_get_data (G_OBJECT (impl), GDKW_SHAPE_DATA_KEY);
  if (!st && create)
    {
      st = g_new0 (GDKWShapeState, 1);
      g_object_set_data_full (G_OBJECT (impl), GDKW_SHAPE_DATA_KEY, st,
			      shape_state_free);
    }
  return st;
}

static gint
impl_depth (GDKWWindowImplWayland *impl)
{
  GdkVisual *visual;

  if (!impl->colormap)
    return 0;
  visual = gdk_colormap_get_visual (impl->colormap);
  return visual ? visual->depth : 0;
}

/* ------------------------------------------------------------------ */
/* Sending                                                            */
/* ------------------------------------------------------------------ */

static gboolean
regions_same (const GdkRegion *a, const GdkRegion *b)
{
  if (a == NULL || b == NULL)
    return a == b;
  return gdk_region_equal ((GdkRegion *) a, (GdkRegion *) b);
}

static struct wl_region *
region_to_wl (GDKWDisplayWayland *display, const GdkRegion *region)
{
  struct wl_region *wl;
  GdkRectangle *rects = NULL;
  gint n = 0, i;

  wl = wl_compositor_create_region (display->wl_compositor);
  gdk_region_get_rectangles ((GdkRegion *) region, &rects, &n);
  for (i = 0; i < n; i++)
    wl_region_add (wl, rects[i].x, rects[i].y, rects[i].width,
		   rects[i].height);
  g_free (rects);
  return wl;
}

/* Sends the effective regions if they differ from the last sent ones.
 * Returns TRUE if at least one request went out. */
static gboolean
shape_send (GDKWWindowImplWayland *impl, GDKWShapeState *st)
{
  GDKWDisplayWayland *display;
  GdkRegion *opaque = NULL, *input = NULL;
  gboolean sent = FALSE;

  if (!impl->wl_surface || !impl->screen)
    return FALSE;
  if (gdk_screen_get_display (impl->screen) == NULL)
    return FALSE;
  display = GDKW_DISPLAY_WAYLAND (gdk_screen_get_display (impl->screen));
  if (!display->wl_compositor)
    return FALSE;

  /* opaque: B for a depth 24 window, else nothing */
  if (st->bounding && impl_depth (impl) == 24)
    opaque = st->bounding;

  /* effective input region */
  if (st->bounding && st->input)
    {
      input = gdk_region_copy (st->bounding);
      gdk_region_intersect (input, st->input);
    }
  else if (st->bounding)
    input = gdk_region_copy (st->bounding);
  else if (st->input)
    input = gdk_region_copy (st->input);

  if (!regions_same (opaque, st->sent_opaque))
    {
      if (opaque)
	{
	  struct wl_region *wl = region_to_wl (display, opaque);

	  wl_surface_set_opaque_region (impl->wl_surface, wl);
	  wl_region_destroy (wl);
	}
      else
	wl_surface_set_opaque_region (impl->wl_surface, NULL);

      if (st->sent_opaque)
	gdk_region_destroy (st->sent_opaque);
      st->sent_opaque = opaque ? gdk_region_copy (opaque) : NULL;
      st->opaque_known = TRUE;
      st->opaque_requests++;
      sent = TRUE;
    }

  if (!regions_same (input, st->sent_input))
    {
      if (input)
	{
	  struct wl_region *wl = region_to_wl (display, input);

	  wl_surface_set_input_region (impl->wl_surface, wl);
	  wl_region_destroy (wl);
	}
      else
	wl_surface_set_input_region (impl->wl_surface, NULL);

      if (st->sent_input)
	gdk_region_destroy (st->sent_input);
      st->sent_input = input;
      input = NULL;
      st->input_known = TRUE;
      st->input_requests++;
      sent = TRUE;
    }

  if (input)
    gdk_region_destroy (input);

  return sent;
}

static void
shape_set (GdkWindow *window, gboolean is_input, const GdkRegion *region,
	   gint offset_x, gint offset_y)
{
  GDKWWindowImplWayland *impl = shape_impl_from_window (window);
  GDKWShapeState *st;
  GdkRegion **slot;
  GdkRegion *copy = NULL;

  if (!impl)
    return;

  st = shape_state_get (impl, region != NULL);
  if (!st)
    return;

  if (region)
    {
      copy = gdk_region_copy ((GdkRegion *) region);
      if (offset_x || offset_y)
	gdk_region_offset (copy, offset_x, offset_y);
    }

  slot = is_input ? &st->input : &st->bounding;
  if (*slot)
    gdk_region_destroy (*slot);
  *slot = copy;

  if (shape_send (impl, st))
    _gdk_wayland_window_impl_present (impl);
}

/* ------------------------------------------------------------------ */
/* Contract                                                           */
/* ------------------------------------------------------------------ */

void
_gdk_wayland_shape_combine_region (GdkWindow       *window,
				   const GdkRegion *region,
				   gint             offset_x,
				   gint             offset_y)
{
  shape_set (window, FALSE, region, offset_x, offset_y);
}

void
_gdk_wayland_shape_input_combine_region (GdkWindow       *window,
					 const GdkRegion *region,
					 gint             offset_x,
					 gint             offset_y)
{
  shape_set (window, TRUE, region, offset_x, offset_y);
}

void
_gdk_wayland_shape_surface_created (GDKWWindowImplWayland *impl)
{
  GDKWShapeState *st;

  if (!impl)
    return;
  st = shape_state_get (impl, FALSE);
  if (!st)
    return;

  /* no present: the caller commits the first (buffer-less) frame */
  shape_send (impl, st);
}

void
_gdk_wayland_shape_before_present (GDKWWindowImplWayland *impl)
{
  GDKWShapeState *st;
  GdkRegion *outside;
  GdkRectangle *rects = NULL;
  gint n = 0, i;
  cairo_t *cr;

  if (!impl || !impl->backing || impl->width == 0 || impl->height == 0)
    return;
  st = shape_state_get (impl, FALSE);
  if (!st || !st->bounding)
    return;

  {
    GdkRectangle all = { 0, 0, impl->width, impl->height };

    outside = gdk_region_rectangle (&all);
  }
  gdk_region_subtract (outside, st->bounding);
  gdk_region_get_rectangles (outside, &rects, &n);
  gdk_region_destroy (outside);

  if (n > 0)
    {
      cairo_surface_flush (impl->backing);
      cr = cairo_create (impl->backing);
      cairo_set_operator (cr, CAIRO_OPERATOR_CLEAR);
      for (i = 0; i < n; i++)
	cairo_rectangle (cr, rects[i].x, rects[i].y, rects[i].width,
			 rects[i].height);
      cairo_fill (cr);
      cairo_destroy (cr);
      cairo_surface_mark_dirty (impl->backing);
    }
  g_free (rects);
}

GdkRegion *
_gdk_wayland_windowing_window_get_shape (GdkWindow *window)
{
  GDKWWindowImplWayland *impl = shape_impl_from_window (window);
  GDKWShapeState *st;

  if (!impl)
    return NULL;
  st = shape_state_get (impl, FALSE);
  if (!st || !st->bounding)
    return NULL;
  return gdk_region_copy (st->bounding);
}

GdkRegion *
_gdk_wayland_windowing_window_get_input_shape (GdkWindow *window)
{
  GDKWWindowImplWayland *impl = shape_impl_from_window (window);
  GDKWShapeState *st;

  if (!impl)
    return NULL;
  st = shape_state_get (impl, FALSE);
  if (!st || !st->input)
    return NULL;
  return gdk_region_copy (st->input);
}

GdkRegion *
_gdk_wayland_windowing_get_shape_for_mask (GdkBitmap *mask)
{
  if (!mask)
    return NULL;
  return _gdk_wayland_bitmap_to_region (mask);
}

/* ------------------------------------------------------------------ */
/* Test hooks                                                         */
/* ------------------------------------------------------------------ */

static gint
region_to_list (const GdkRegion *region, GdkRectangle *out)
{
  GdkRectangle *rects = NULL;
  gint n = 0, i;

  if (!region)
    return 0;
  gdk_region_get_rectangles ((GdkRegion *) region, &rects, &n);
  if (n > GDKW_TEST_MAX_RECTS)
    n = GDKW_TEST_MAX_RECTS;
  for (i = 0; i < n; i++)
    out[i] = rects[i];
  g_free (rects);
  return n;
}

gboolean
_gdk_wayland_test_shape_info (GdkWindow *window, GDKWTestShapeInfo *info)
{
  GDKWWindowImplWayland *impl;
  GDKWShapeState *st;

  if (info)
    memset (info, 0, sizeof *info);
  impl = shape_impl_from_window (window);
  if (!impl || !info)
    return FALSE;

  info->has_surface = impl->wl_surface != NULL;
  st = shape_state_get (impl, FALSE);
  if (!st)
    return TRUE;

  info->shaped = st->bounding != NULL;
  info->input_shaped = st->input != NULL;
  info->n_bounding = region_to_list (st->bounding, info->bounding);
  info->n_input_shape = region_to_list (st->input, info->input_shape);
  info->opaque_sent = st->sent_opaque != NULL;
  info->n_opaque = region_to_list (st->sent_opaque, info->opaque);
  info->input_sent = st->sent_input != NULL;
  info->n_input = region_to_list (st->sent_input, info->input);
  info->opaque_requests = st->opaque_requests;
  info->input_requests = st->input_requests;
  return TRUE;
}

gboolean
_gdk_wayland_test_window_pixel (GdkWindow *window, gint x, gint y,
				guint32 *argb)
{
  GDKWWindowImplWayland *impl = shape_impl_from_window (window);

  if (!impl || !impl->shm_data || !impl->backing)
    return FALSE;
  if (x < 0 || y < 0 || x >= (gint) impl->width || y >= (gint) impl->height)
    return FALSE;

  if (argb)
    *argb = *(guint32 *) (impl->shm_data + (gsize) y * impl->stride + x * 4);
  return TRUE;
}

#ifdef GDK_COMPILATION
#include "gdkalias.h"
#endif
