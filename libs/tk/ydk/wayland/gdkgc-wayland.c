/*
 * gdkgc-wayland.c
 *
 * ydk-wayland M5 piece C: the GdkGC subclass for the Wayland backend and
 * the translation of a GC into cairo state.
 *
 * The core GdkGC (ydk/gdkgc.c) already stores the generic state (clip
 * region/mask, fill, tile, stipple, fg/bg pixel, ...).  GDKWGCWayland only
 * adds what the core does not keep (function, font, line attributes,
 * dashes) and _gdk_wayland_gc_setup_cairo() turns the combination into a
 * configured cairo_t for piece B's draw vfuncs.
 *
 * Symbols are _gdk_wayland_* / gdkw_*; the dispatchers in ydk/gdkbackend.c
 * own the public gdk_* names.
 */

#undef GDK_DISABLE_DEPRECATED

#include "config.h"

#include <string.h>

#include <ydk/gdk.h>
#include <ydk/gdkinternals.h>

#include "gdkpixmap-wayland.h"

#ifdef GDK_COMPILATION
#include "gdkalias.h"
#endif

static void gdkw_gc_wayland_finalize   (GObject         *object);
static void gdkw_gc_wayland_get_values (GdkGC           *gc,
					GdkGCValues     *values);
static void gdkw_gc_wayland_set_values (GdkGC           *gc,
					GdkGCValues     *values,
					GdkGCValuesMask  values_mask);
static void gdkw_gc_wayland_set_dashes (GdkGC           *gc,
					gint             dash_offset,
					gint8            dash_list[],
					gint             n);

G_DEFINE_TYPE (GDKWGCWayland, gdkw_gc_wayland, GDK_TYPE_GC)

static void
gdkw_gc_wayland_class_init (GDKWGCWaylandClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GdkGCClass *gc_class = GDK_GC_CLASS (klass);

  object_class->finalize = gdkw_gc_wayland_finalize;

  gc_class->get_values = gdkw_gc_wayland_get_values;
  gc_class->set_values = gdkw_gc_wayland_set_values;
  gc_class->set_dashes = gdkw_gc_wayland_set_dashes;
}

static void
gdkw_gc_wayland_init (GDKWGCWayland *gc)
{
  gc->function = GDK_COPY;
  gc->line_width = 0;
  gc->line_style = GDK_LINE_SOLID;
  gc->cap_style = GDK_CAP_BUTT;
  gc->join_style = GDK_JOIN_MITER;
}

static void
gdkw_gc_wayland_finalize (GObject *object)
{
  GDKWGCWayland *wgc = GDKW_GC_WAYLAND (object);

  g_free (wgc->dashes);
  wgc->dashes = NULL;

  if (wgc->font)
    {
      gdk_font_unref (wgc->font);
      wgc->font = NULL;
    }

  G_OBJECT_CLASS (gdkw_gc_wayland_parent_class)->finalize (object);
}

GdkGC *
_gdk_wayland_gc_new (GdkDrawable      *drawable,
		     GdkGCValues      *values,
		     GdkGCValuesMask   mask)
{
  GdkGC *gc;
  GDKWGCWayland *wgc;

  g_return_val_if_fail (GDK_IS_DRAWABLE (drawable), NULL);

  gc = g_object_new (gdkw_gc_wayland_get_type (), NULL);
  wgc = GDKW_GC_WAYLAND (gc);

  _gdk_gc_init (gc, drawable, values, mask);

  /* No ref on the drawable: screens cache scratch GCs. */
  wgc->screen = gdk_drawable_get_screen (drawable);
  wgc->depth = gdk_drawable_get_depth (drawable);

  wgc->function = GDK_COPY;
  wgc->line_width = 0;
  wgc->line_style = GDK_LINE_SOLID;
  wgc->cap_style = GDK_CAP_BUTT;
  wgc->join_style = GDK_JOIN_MITER;
  wgc->dash_offset = 0;
  wgc->dashes = NULL;
  wgc->n_dashes = 0;
  wgc->font = NULL;

  if (values && mask)
    gdkw_gc_wayland_set_values (gc, values, mask);

  return gc;
}

static void
gdkw_gc_wayland_get_values (GdkGC       *gc,
			    GdkGCValues *values)
{
  GDKWGCWayland *wgc = GDKW_GC_WAYLAND (gc);

  memset (values, 0, sizeof (GdkGCValues));

  values->foreground.pixel = _gdk_gc_get_fg_pixel (gc);
  values->background.pixel = _gdk_gc_get_bg_pixel (gc);
  values->font = wgc->font;
  values->function = wgc->function;
  values->fill = _gdk_gc_get_fill (gc);
  values->tile = _gdk_gc_get_tile (gc);
  values->stipple = _gdk_gc_get_stipple (gc);
  values->clip_mask = _gdk_gc_get_clip_mask (gc);
  values->subwindow_mode = _gdk_gc_get_subwindow (gc);
  values->ts_x_origin = gc->ts_x_origin;
  values->ts_y_origin = gc->ts_y_origin;
  values->clip_x_origin = gc->clip_x_origin;
  values->clip_y_origin = gc->clip_y_origin;
  values->graphics_exposures = _gdk_gc_get_exposures (gc);
  values->line_width = wgc->line_width;
  values->line_style = wgc->line_style;
  values->cap_style = wgc->cap_style;
  values->join_style = wgc->join_style;
}

static void
gdkw_gc_wayland_set_values (GdkGC           *gc,
			    GdkGCValues     *values,
			    GdkGCValuesMask  values_mask)
{
  GDKWGCWayland *wgc = GDKW_GC_WAYLAND (gc);

  /* The core has already stored the generic fields. */

  if (values_mask & GDK_GC_FUNCTION)
    wgc->function = values->function;

  if (values_mask & GDK_GC_FONT)
    {
      GdkFont *font = values->font;

      if (font)
	gdk_font_ref (font);
      if (wgc->font)
	gdk_font_unref (wgc->font);
      wgc->font = font;
    }

  if (values_mask & GDK_GC_LINE_WIDTH)
    wgc->line_width = values->line_width;
  if (values_mask & GDK_GC_LINE_STYLE)
    wgc->line_style = values->line_style;
  if (values_mask & GDK_GC_CAP_STYLE)
    wgc->cap_style = values->cap_style;
  if (values_mask & GDK_GC_JOIN_STYLE)
    wgc->join_style = values->join_style;
}

static void
gdkw_gc_wayland_set_dashes (GdkGC *gc,
			    gint   dash_offset,
			    gint8  dash_list[],
			    gint   n)
{
  GDKWGCWayland *wgc = GDKW_GC_WAYLAND (gc);

  g_free (wgc->dashes);
  wgc->dashes = NULL;
  wgc->n_dashes = 0;

  if (dash_list && n > 0)
    {
      wgc->dashes = g_malloc (n);
      memcpy (wgc->dashes, dash_list, n);
      wgc->n_dashes = n;
    }
  wgc->dash_offset = dash_offset;
}

/********************************************************
 * Windowing hooks (declared in gdkprivate-wayland.h)
 ********************************************************/

void
_gdk_wayland_windowing_gc_set_clip_region (GdkGC          *gc,
					   const GdkRegion *region,
					   gboolean        reset_origin)
{
  /* The core has already stored the region; there is no cached clip here
   * (the core state is the single source of truth). */
  if (reset_origin)
    {
      gc->clip_x_origin = 0;
      gc->clip_y_origin = 0;
    }
}

void
_gdk_wayland_windowing_gc_copy (GdkGC *dst_gc,
				GdkGC *src_gc)
{
  GDKWGCWayland *dst, *src;

  g_return_if_fail (GDKW_IS_GC_WAYLAND (dst_gc));
  g_return_if_fail (GDKW_IS_GC_WAYLAND (src_gc));

  dst = GDKW_GC_WAYLAND (dst_gc);
  src = GDKW_GC_WAYLAND (src_gc);

  /* dst keeps its own screen and depth. */
  dst->function = src->function;

  if (src->font)
    gdk_font_ref (src->font);
  if (dst->font)
    gdk_font_unref (dst->font);
  dst->font = src->font;

  dst->line_width = src->line_width;
  dst->line_style = src->line_style;
  dst->cap_style = src->cap_style;
  dst->join_style = src->join_style;
  dst->dash_offset = src->dash_offset;

  g_free (dst->dashes);
  dst->dashes = NULL;
  dst->n_dashes = 0;
  if (src->dashes && src->n_dashes > 0)
    {
      dst->dashes = g_malloc (src->n_dashes);
      memcpy (dst->dashes, src->dashes, src->n_dashes);
      dst->n_dashes = src->n_dashes;
    }
}

GdkScreen *
_gdk_wayland_gc_get_screen (GdkGC *gc)
{
  g_return_val_if_fail (GDKW_IS_GC_WAYLAND (gc), NULL);

  return GDKW_GC_WAYLAND (gc)->screen;
}

/********************************************************
 * Clip
 ********************************************************/

GdkRegion *
_gdk_wayland_gc_get_clip_device_region (GdkGC *gc)
{
  GdkRegion *clip_region = _gdk_gc_get_clip_region (gc);
  GdkBitmap *clip_mask = _gdk_gc_get_clip_mask (gc);
  GdkRegion *result = NULL;

  if (!clip_region && !clip_mask)
    return NULL;

  if (clip_region)
    result = gdk_region_copy (clip_region);

  if (clip_mask)
    {
      GdkRegion *mask_region = _gdk_wayland_bitmap_to_region (clip_mask);

      if (mask_region)
	{
	  if (result)
	    {
	      /* The core may leave both a mask and an empty region: the
	       * intersection is empty, so nothing is drawn. */
	      gdk_region_intersect (result, mask_region);
	      gdk_region_destroy (mask_region);
	    }
	  else
	    result = mask_region;
	}
      else
	g_debug ("GdkGC clip mask is not a depth-1 Wayland pixmap, ignored");
    }

  if (!result)
    return NULL;

  gdk_region_offset (result, gc->clip_x_origin, gc->clip_y_origin);
  return result;
}

/********************************************************
 * cairo setup
 ********************************************************/

static void
debug_unsupported_function (GdkFunction function)
{
  static guint32 reported = 0;

  if (function < 32 && !(reported & (1u << function)))
    {
      reported |= 1u << function;
      g_debug ("GdkFunction %d is not supported on Wayland, using GDK_COPY",
	       (int) function);
    }
}

static void
set_rgb_source (cairo_t *cr, guint32 pixel, gboolean invert)
{
  gdouble r, g, b;

  _gdk_wayland_pixel_to_rgb (pixel, &r, &g, &b);
  if (invert)
    {
      r = 1.0 - r;
      g = 1.0 - g;
      b = 1.0 - b;
    }
  cairo_set_source_rgba (cr, r, g, b, 1.0);
}

static void
set_pattern_source (cairo_t         *cr,
		    GdkGC           *gc,
		    cairo_surface_t *tile_surface)
{
  cairo_pattern_t *pattern = cairo_pattern_create_for_surface (tile_surface);
  cairo_matrix_t user_to_device;
  cairo_matrix_t user_to_pattern;
  cairo_matrix_t device_to_pattern;

  /* Tiles and stipples live in device space: pattern matrix is
   * CTM * Translate(-ts_x, -ts_y), as _gdk_gc_update_context() does. */
  cairo_get_matrix (cr, &user_to_device);
  cairo_matrix_init_translate (&device_to_pattern,
			       - gc->ts_x_origin, - gc->ts_y_origin);
  cairo_matrix_multiply (&user_to_pattern,
			 &user_to_device, &device_to_pattern);

  cairo_pattern_set_matrix (pattern, &user_to_pattern);
  cairo_pattern_set_extend (pattern, CAIRO_EXTEND_REPEAT);
  cairo_set_source (cr, pattern);
  cairo_pattern_destroy (pattern);
}

/* Returns TRUE if the fill source was set from a tile/stipple. */
static gboolean
set_fill_source (cairo_t *cr, GdkGC *gc, guint32 fg, guint32 bg)
{
  GdkFill fill = _gdk_gc_get_fill (gc);

  switch (fill)
    {
    case GDK_TILED:
      {
	GdkPixmap *tile = _gdk_gc_get_tile (gc);
	cairo_surface_t *tile_surface;

	if (!tile)
	  break;
	tile_surface = _gdk_drawable_ref_cairo_surface (_gdk_wayland_drawable_get_impl (GDK_DRAWABLE (tile)));
	if (!tile_surface)
	  break;
	set_pattern_source (cr, gc, tile_surface);
	cairo_surface_destroy (tile_surface);
	return TRUE;
      }

    case GDK_STIPPLED:
    case GDK_OPAQUE_STIPPLED:
      {
	GdkBitmap *stipple = _gdk_gc_get_stipple (gc);
	cairo_surface_t *alpha_surface;
	cairo_surface_t *tile_surface;
	cairo_t *tmp_cr;
	gint width, height;

	if (!stipple)
	  break;
	alpha_surface = _gdk_drawable_ref_cairo_surface (_gdk_wayland_drawable_get_impl (GDK_DRAWABLE (stipple)));
	if (!alpha_surface)
	  break;

	gdk_drawable_get_size (stipple, &width, &height);
	tile_surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32,
						   width, height);
	tmp_cr = cairo_create (tile_surface);

	cairo_set_operator (tmp_cr, CAIRO_OPERATOR_SOURCE);
	if (fill == GDK_OPAQUE_STIPPLED)
	  set_rgb_source (tmp_cr, bg, FALSE);
	else
	  cairo_set_source_rgba (tmp_cr, 0, 0, 0, 0);
	cairo_paint (tmp_cr);

	cairo_set_operator (tmp_cr, CAIRO_OPERATOR_OVER);
	set_rgb_source (tmp_cr, fg, FALSE);
	cairo_mask_surface (tmp_cr, alpha_surface, 0, 0);

	cairo_destroy (tmp_cr);
	cairo_surface_destroy (alpha_surface);

	set_pattern_source (cr, gc, tile_surface);
	cairo_surface_destroy (tile_surface);
	return TRUE;
      }

    default:
      break;
    }

  set_rgb_source (cr, fg, FALSE);
  return FALSE;
}

gboolean
_gdk_wayland_gc_setup_cairo (GdkGC       *gc,
			     cairo_t     *cr,
			     GdkDrawable *impl,
			     GDKWGCFlags  flags)
{
  GDKWGCWayland *wgc;
  GdkRegion *region;
  gint d;
  guint32 fg, bg;
  gboolean replace = (flags & GDKW_GC_REPLACE) != 0;
  cairo_operator_t copy_op = replace ? CAIRO_OPERATOR_SOURCE : CAIRO_OPERATOR_OVER;

  g_return_val_if_fail (GDKW_IS_GC_WAYLAND (gc), FALSE);

  wgc = GDKW_GC_WAYLAND (gc);

  if (wgc->function == GDK_NOOP)
    return FALSE;

  /* (1) clip, added to whatever clip cr already has */
  region = _gdk_wayland_gc_get_clip_device_region (gc);
  if (region)
    {
      if (gdk_region_empty (region))
	{
	  gdk_region_destroy (region);
	  return FALSE;
	}

      cairo_save (cr);
      cairo_identity_matrix (cr);
      gdk_cairo_region (cr, region);
      cairo_restore (cr);
      cairo_clip (cr);
      gdk_region_destroy (region);
    }

  /* (2) antialiasing and fill rule */
  if (!(flags & GDKW_GC_ANTIALIAS))
    cairo_set_antialias (cr, CAIRO_ANTIALIAS_NONE);
  cairo_set_fill_rule (cr, CAIRO_FILL_RULE_EVEN_ODD);

  /* (3) operator and source */
  d = gdk_drawable_get_depth (impl);
  fg = _gdk_gc_get_fg_pixel (gc);
  bg = _gdk_gc_get_bg_pixel (gc);

  if (!(flags & (GDKW_GC_USE_FILL | GDKW_GC_USE_BG)))
    {
      /* Blit: the caller sets its own source. */
      cairo_set_operator (cr, copy_op);
    }
  else if (d == 1)
    {
      guint32 pixel = (flags & GDKW_GC_USE_BG) ? bg : fg;
      gboolean bit = (pixel & 1) != 0;

      if (!(flags & GDKW_GC_USE_BG))
	{
	  switch (wgc->function)
	    {
	    case GDK_COPY:
	      break;
	    case GDK_CLEAR:
	      bit = FALSE;
	      break;
	    case GDK_SET:
	      bit = TRUE;
	      break;
	    case GDK_COPY_INVERT:
	      bit = !bit;
	      break;
	    case GDK_INVERT:
	    case GDK_XOR:
	      break;
	    default:
	      debug_unsupported_function (wgc->function);
	      break;
	    }
	}

      if (bit)
	{
	  cairo_set_operator (cr, copy_op);
	  cairo_set_source_rgba (cr, 0, 0, 0, 1);
	}
      else
	{
	  cairo_set_operator (cr, CAIRO_OPERATOR_CLEAR);
	  cairo_set_source_rgba (cr, 0, 0, 0, 1);
	}
    }
  else if (flags & GDKW_GC_USE_BG)
    {
      cairo_set_operator (cr, copy_op);
      set_rgb_source (cr, bg, FALSE);
    }
  else
    {
      switch (wgc->function)
	{
	case GDK_CLEAR:
	  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
	  cairo_set_source_rgba (cr, 0, 0, 0, 1);
	  break;

	case GDK_SET:
	  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
	  cairo_set_source_rgba (cr, 1, 1, 1, 1);
	  break;

	case GDK_INVERT:
	  cairo_set_operator (cr, CAIRO_OPERATOR_DIFFERENCE);
	  cairo_set_source_rgba (cr, 1, 1, 1, 1);
	  break;

	case GDK_XOR:
	  cairo_set_operator (cr, CAIRO_OPERATOR_DIFFERENCE);
	  set_rgb_source (cr, fg, FALSE);
	  break;

	case GDK_COPY_INVERT:
	  cairo_set_operator (cr, copy_op);
	  set_rgb_source (cr, fg, TRUE);
	  break;

	case GDK_COPY:
	  cairo_set_operator (cr, copy_op);
	  set_fill_source (cr, gc, fg, bg);
	  break;

	default:
	  debug_unsupported_function (wgc->function);
	  cairo_set_operator (cr, copy_op);
	  set_fill_source (cr, gc, fg, bg);
	  break;
	}
    }

  /* (5) stroke attributes */
  if (flags & GDKW_GC_USE_STROKE)
    {
      cairo_set_line_width (cr, wgc->line_width > 1 ? wgc->line_width : 1);

      switch (wgc->cap_style)
	{
	case GDK_CAP_ROUND:
	  cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
	  break;
	case GDK_CAP_PROJECTING:
	  cairo_set_line_cap (cr, CAIRO_LINE_CAP_SQUARE);
	  break;
	case GDK_CAP_NOT_LAST:
	case GDK_CAP_BUTT:
	default:
	  cairo_set_line_cap (cr, CAIRO_LINE_CAP_BUTT);
	  break;
	}

      switch (wgc->join_style)
	{
	case GDK_JOIN_ROUND:
	  cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);
	  break;
	case GDK_JOIN_BEVEL:
	  cairo_set_line_join (cr, CAIRO_LINE_JOIN_BEVEL);
	  break;
	case GDK_JOIN_MITER:
	default:
	  cairo_set_line_join (cr, CAIRO_LINE_JOIN_MITER);
	  break;
	}

      if (!(flags & GDKW_GC_USE_BG) &&
	  (wgc->line_style == GDK_LINE_ON_OFF_DASH ||
	   wgc->line_style == GDK_LINE_DOUBLE_DASH))
	{
	  gdouble stack_dashes[2] = { 4.0, 4.0 };
	  gdouble *dashes = stack_dashes;
	  gint n = 2;
	  gdouble sum = 0.0;
	  gint i;

	  if (wgc->dashes && wgc->n_dashes > 0)
	    {
	      n = wgc->n_dashes;
	      dashes = g_new (gdouble, n);
	      for (i = 0; i < n; i++)
		{
		  dashes[i] = (guint8) wgc->dashes[i];
		  sum += dashes[i];
		}
	      if (sum <= 0.0)
		{
		  g_free (dashes);
		  dashes = stack_dashes;
		  n = 2;
		}
	    }

	  cairo_set_dash (cr, dashes, n, (gdouble) wgc->dash_offset);

	  if (dashes != stack_dashes)
	    g_free (dashes);
	}
    }

  return TRUE;
}
