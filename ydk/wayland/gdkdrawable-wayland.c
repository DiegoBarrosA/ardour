/*
 * gdkdrawable-wayland.c
 *
 * ydk-wayland M5, piece B: GdkDrawableClass draw vfuncs over cairo.
 *
 * One installer, _gdk_wayland_drawable_install_vfuncs(), fills in the draw
 * vfuncs shared by the pixmap impl and the window impl.  Nothing is cached
 * across calls: every primitive creates a cairo_t on the impl's surface,
 * lets the GC (piece C) configure it, paints, and destroys it.
 *
 * Coordinates arrive in impl space (gdkwindow.c has already applied the
 * window offset and merged the client-side window clip into the GC), so the
 * GC clip is the complete clip and is only honoured here.
 */

#include "config.h"

#include <string.h>

#include <ydk/gdk.h>
#include <ydk/gdkinternals.h>

#include "gdkpixmap-wayland.h"

#ifdef GDK_COMPILATION
#include "gdkalias.h"
#endif

#define GDKW_HOST_BYTE_ORDER \
  (G_BYTE_ORDER == G_LITTLE_ENDIAN ? GDK_LSB_FIRST : GDK_MSB_FIRST)

typedef void (*GDKWStrokeFunc) (cairo_t *cr, GdkGC *gc, gpointer data);

/********************************************************
 * Cairo acquisition
 ********************************************************/

/* Returns a prepared cairo_t for @drawable and @gc, or NULL when nothing
 * must be drawn.  @gc may be NULL (bare context). */
static cairo_t *
gdkw_begin (GdkDrawable *drawable,
	    GdkGC       *gc,
	    GDKWGCFlags  flags,
	    GdkDrawable **impl_out)
{
  GdkDrawable *impl;
  cairo_surface_t *surface;
  cairo_t *cr;

  impl = _gdk_wayland_drawable_get_impl (drawable);
  if (impl_out)
    *impl_out = impl;

  surface = _gdk_drawable_ref_cairo_surface (impl);
  if (!surface)
    return NULL;

  /* bitmaps are never antialiased: edge pixels must be exact */
  if (gdk_drawable_get_depth (impl) == 1)
    flags &= ~GDKW_GC_ANTIALIAS;

  cr = cairo_create (surface);
  cairo_surface_destroy (surface);

  if (gc && !_gdk_wayland_gc_setup_cairo (gc, cr, impl, flags))
    {
      cairo_destroy (cr);
      return NULL;
    }

  return cr;
}

/* Runs @func once per stroke pass: GDK_LINE_DOUBLE_DASH first paints a solid
 * background-coloured pass. */
static void
gdkw_run_stroke (GdkDrawable    *drawable,
		 GdkGC          *gc,
		 GDKWGCFlags     extra,
		 GDKWStrokeFunc  func,
		 gpointer        data)
{
  GDKWGCWayland *wgc = GDKW_GC_WAYLAND (gc);
  cairo_t *cr;

  if (wgc->line_style == GDK_LINE_DOUBLE_DASH)
    {
      cr = gdkw_begin (drawable, gc, GDKW_GC_USE_BG | GDKW_GC_USE_STROKE | extra, NULL);
      if (cr)
	{
	  func (cr, gc, data);
	  cairo_destroy (cr);
	}
    }

  cr = gdkw_begin (drawable, gc, GDKW_GC_USE_FILL | GDKW_GC_USE_STROKE | extra, NULL);
  if (cr)
    {
      func (cr, gc, data);
      cairo_destroy (cr);
    }
}

static gboolean
gdkw_gc_is_thin (GdkGC *gc)
{
  return GDKW_GC_WAYLAND (gc)->line_width <= 1;
}

/********************************************************
 * Points, segments, lines
 ********************************************************/

static void
gdkw_draw_points (GdkDrawable *drawable,
		  GdkGC       *gc,
		  GdkPoint    *points,
		  gint         npoints)
{
  cairo_t *cr;
  gint i;

  if (npoints <= 0)
    return;

  cr = gdkw_begin (drawable, gc, GDKW_GC_USE_FILL, NULL);
  if (!cr)
    return;

  /* EVEN_ODD (set by the GC) would cancel duplicated points */
  cairo_set_fill_rule (cr, CAIRO_FILL_RULE_WINDING);
  for (i = 0; i < npoints; i++)
    cairo_rectangle (cr, points[i].x, points[i].y, 1, 1);
  cairo_fill (cr);
  cairo_destroy (cr);
}

typedef struct
{
  GdkSegment *segs;
  gint        nsegs;
} GDKWSegmentsData;

static void
gdkw_stroke_segments (cairo_t *cr, GdkGC *gc, gpointer data)
{
  GDKWSegmentsData *d = data;
  gboolean thin = gdkw_gc_is_thin (gc);
  cairo_line_cap_t cap = cairo_get_line_cap (cr);
  gboolean solid = (GDKW_GC_WAYLAND (gc)->line_style == GDK_LINE_SOLID);
  gint i;

  for (i = 0; i < d->nsegs; i++)
    {
      GdkSegment *s = &d->segs[i];

      if (thin && s->y1 == s->y2)
	{
	  /* start at the first endpoint so the dash phase follows the
	   * direction the caller gave, as in X */
	  cairo_set_line_cap (cr, CAIRO_LINE_CAP_BUTT);
	  if (s->x1 <= s->x2)
	    {
	      cairo_move_to (cr, s->x1, s->y1 + 0.5);
	      cairo_line_to (cr, s->x2 + 1, s->y1 + 0.5);
	    }
	  else
	    {
	      cairo_move_to (cr, s->x1 + 1, s->y1 + 0.5);
	      cairo_line_to (cr, s->x2, s->y1 + 0.5);
	    }
	}
      else if (thin && s->x1 == s->x2)
	{
	  cairo_set_line_cap (cr, CAIRO_LINE_CAP_BUTT);
	  if (s->y1 <= s->y2)
	    {
	      cairo_move_to (cr, s->x1 + 0.5, s->y1);
	      cairo_line_to (cr, s->x1 + 0.5, s->y2 + 1);
	    }
	  else
	    {
	      cairo_move_to (cr, s->x1 + 0.5, s->y1 + 1);
	      cairo_line_to (cr, s->x1 + 0.5, s->y2);
	    }
	}
      else
	{
	  /* path through pixel centres: a square cap lights both end pixels
	   * (undashed thin lines only, see gdkw_stroke_poly) */
	  cairo_set_line_cap (cr, (thin && solid && cap == CAIRO_LINE_CAP_BUTT)
			      ? CAIRO_LINE_CAP_SQUARE : cap);
	  cairo_move_to (cr, s->x1 + 0.5, s->y1 + 0.5);
	  cairo_line_to (cr, s->x2 + 0.5, s->y2 + 0.5);
	}
      cairo_stroke (cr);
    }
}

static void
gdkw_draw_segments (GdkDrawable *drawable,
		    GdkGC       *gc,
		    GdkSegment  *segs,
		    gint         nsegs)
{
  GDKWSegmentsData d;

  if (nsegs <= 0)
    return;

  d.segs = segs;
  d.nsegs = nsegs;
  gdkw_run_stroke (drawable, gc, 0, gdkw_stroke_segments, &d);
}

typedef struct
{
  GdkPoint *points;
  gint      npoints;
  gboolean  close;
} GDKWPolyData;

static void
gdkw_poly_path (cairo_t *cr, GDKWPolyData *d)
{
  gint i;

  for (i = 0; i < d->npoints; i++)
    {
      if (i == 0)
	cairo_move_to (cr, d->points[i].x + 0.5, d->points[i].y + 0.5);
      else
	cairo_line_to (cr, d->points[i].x + 0.5, d->points[i].y + 0.5);
    }

  if (d->close)
    {
      if (d->npoints > 1 &&
	  (d->points[0].x != d->points[d->npoints - 1].x ||
	   d->points[0].y != d->points[d->npoints - 1].y))
	cairo_line_to (cr, d->points[0].x + 0.5, d->points[0].y + 0.5);
      cairo_close_path (cr);
    }
}

static void
gdkw_stroke_poly (cairo_t *cr, GdkGC *gc, gpointer data)
{
  GDKWPolyData *d = data;
  GDKWGCWayland *wgc = GDKW_GC_WAYLAND (gc);

  /* The path runs through pixel centres, so a BUTT cap would end exactly on
   * the centre of the end pixel and whether that pixel is lit would depend on
   * cairo's tie-break.  A square cap extends the open ends by half a pixel
   * and lights both end pixels like X does.  Dashed lines keep BUTT, since a
   * square cap would lengthen every dash. */
  if (!d->close && wgc->line_width <= 1 &&
      wgc->line_style == GDK_LINE_SOLID &&
      (wgc->cap_style == GDK_CAP_BUTT || wgc->cap_style == GDK_CAP_NOT_LAST))
    cairo_set_line_cap (cr, CAIRO_LINE_CAP_SQUARE);

  gdkw_poly_path (cr, d);
  cairo_stroke (cr);
}

static void
gdkw_draw_lines (GdkDrawable *drawable,
		 GdkGC       *gc,
		 GdkPoint    *points,
		 gint         npoints)
{
  GDKWPolyData d;

  if (npoints < 2)
    return;

  d.points = points;
  d.npoints = npoints;
  d.close = FALSE;
  gdkw_run_stroke (drawable, gc, 0, gdkw_stroke_poly, &d);
}

static void
gdkw_draw_polygon (GdkDrawable *drawable,
		   GdkGC       *gc,
		   gboolean     filled,
		   GdkPoint    *points,
		   gint         npoints)
{
  GDKWPolyData d;

  if (npoints < 2)
    return;

  d.points = points;
  d.npoints = npoints;
  d.close = TRUE;

  if (filled)
    {
      cairo_t *cr = gdkw_begin (drawable, gc, GDKW_GC_USE_FILL, NULL);
      gint i;

      if (!cr)
	return;

      /* X fills the pixels whose centre lies inside the polygon, with the
       * vertices on pixel corners; right and bottom edges are excluded.  A
       * non-antialiased cairo fill of the integer path has the same rule.
       * A single fill (no extra boundary stroke) keeps GDK_XOR and
       * GDK_INVERT from toggling boundary pixels twice. */
      for (i = 0; i < npoints; i++)
	{
	  if (i == 0)
	    cairo_move_to (cr, points[i].x, points[i].y);
	  else
	    cairo_line_to (cr, points[i].x, points[i].y);
	}
      cairo_close_path (cr);
      cairo_fill (cr);
      cairo_destroy (cr);
    }
  else
    gdkw_run_stroke (drawable, gc, 0, gdkw_stroke_poly, &d);
}

/********************************************************
 * Rectangles and arcs
 ********************************************************/

typedef struct
{
  gint x, y, width, height;
} GDKWRectData;

static void
gdkw_stroke_rect (cairo_t *cr, GdkGC *gc, gpointer data)
{
  GDKWRectData *d = data;

  if (d->height == 0)
    {
      /* degenerate: X draws a (width + 1) x 1 line */
      cairo_set_line_cap (cr, CAIRO_LINE_CAP_BUTT);
      cairo_move_to (cr, d->x, d->y + 0.5);
      cairo_line_to (cr, d->x + d->width + 1, d->y + 0.5);
    }
  else if (d->width == 0)
    {
      cairo_set_line_cap (cr, CAIRO_LINE_CAP_BUTT);
      cairo_move_to (cr, d->x + 0.5, d->y);
      cairo_line_to (cr, d->x + 0.5, d->y + d->height + 1);
    }
  else
    cairo_rectangle (cr, d->x + 0.5, d->y + 0.5, d->width, d->height);
  cairo_stroke (cr);
}

static void
gdkw_draw_rectangle (GdkDrawable *drawable,
		     GdkGC       *gc,
		     gboolean     filled,
		     gint         x,
		     gint         y,
		     gint         width,
		     gint         height)
{
  if (width < 0 || height < 0)
    return;

  if (filled)
    {
      cairo_t *cr = gdkw_begin (drawable, gc, GDKW_GC_USE_FILL, NULL);

      if (!cr)
	return;
      cairo_rectangle (cr, x, y, width, height);
      cairo_fill (cr);
      cairo_destroy (cr);
    }
  else
    {
      GDKWRectData d;

      d.x = x;
      d.y = y;
      d.width = width;
      d.height = height;
      gdkw_run_stroke (drawable, gc, 0, gdkw_stroke_rect, &d);
    }
}

typedef struct
{
  gint     x, y, width, height;
  gint     angle1, angle2;
  gboolean filled;
} GDKWArcData;

static void
gdkw_arc_path (cairo_t *cr, GDKWArcData *d)
{
  gdouble cx, cy, a1, a2, end;
  gint angle1 = d->angle1, angle2 = d->angle2;

  if (angle2 >= 360 * 64 || angle2 <= -360 * 64)
    {
      angle1 = 0;
      angle2 = 360 * 64;
    }

  a1 = angle1 * (G_PI / 180.0 / 64.0);
  a2 = angle2 * (G_PI / 180.0 / 64.0);
  end = -(a1 + a2);

  if (d->filled)
    {
      cx = d->x + d->width / 2.0;
      cy = d->y + d->height / 2.0;
    }
  else
    {
      cx = d->x + d->width / 2.0 + 0.5;
      cy = d->y + d->height / 2.0 + 0.5;
    }

  cairo_save (cr);
  cairo_translate (cr, cx, cy);
  cairo_scale (cr, d->width / 2.0, d->height / 2.0);
  if (d->filled)
    cairo_move_to (cr, 0, 0);
  if (angle2 >= 0)
    cairo_arc_negative (cr, 0, 0, 1, -a1, end);
  else
    cairo_arc (cr, 0, 0, 1, -a1, end);
  if (d->filled)
    cairo_close_path (cr);
  cairo_restore (cr);
}

static void
gdkw_stroke_arc (cairo_t *cr, GdkGC *gc, gpointer data)
{
  gdkw_arc_path (cr, data);
  cairo_stroke (cr);
}

static void
gdkw_draw_arc (GdkDrawable *drawable,
	       GdkGC       *gc,
	       gboolean     filled,
	       gint         x,
	       gint         y,
	       gint         width,
	       gint         height,
	       gint         angle1,
	       gint         angle2)
{
  GDKWArcData d;

  if (width <= 0 || height <= 0)
    return;

  d.x = x;
  d.y = y;
  d.width = width;
  d.height = height;
  d.angle1 = angle1;
  d.angle2 = angle2;
  d.filled = filled;

  if (filled)
    {
      cairo_t *cr = gdkw_begin (drawable, gc,
				GDKW_GC_USE_FILL | GDKW_GC_ANTIALIAS, NULL);
      if (!cr)
	return;
      gdkw_arc_path (cr, &d);
      cairo_fill (cr);
      cairo_destroy (cr);
    }
  else
    gdkw_run_stroke (drawable, gc, GDKW_GC_ANTIALIAS, gdkw_stroke_arc, &d);
}

/********************************************************
 * Blits
 ********************************************************/

/* Paint the w x h rectangle at (dx, dy) of @drawable from @ss, whose pixel
 * (sx, sy) lands on (dx, dy).  @ss has depth @sdepth.  The source rectangle
 * must already be clamped to @ss. */
static void
gdkw_blit_surface (GdkDrawable    *drawable,
		   GdkGC          *gc,
		   cairo_surface_t *ss,
		   gint            sdepth,
		   gint            sx,
		   gint            sy,
		   gint            dx,
		   gint            dy,
		   gint            w,
		   gint            h)
{
  GdkDrawable *impl;
  cairo_surface_t *tmp = NULL;
  cairo_t *cr;
  gint ddepth;
  gboolean mask;

  if (w <= 0 || h <= 0)
    return;

  /* validate depth combination before touching anything */
  ddepth = gdk_drawable_get_depth (_gdk_wayland_drawable_get_impl (drawable));
  if (ddepth == 1 && sdepth != 1)
    {
      g_warning ("Attempt to draw a drawable with depth %d to a drawable with depth 1",
		 sdepth);
      return;
    }
  mask = (sdepth == 1 && ddepth != 1);

  cr = gdkw_begin (drawable, gc,
		   mask ? GDKW_GC_USE_FILL : GDKW_GC_REPLACE, &impl);
  if (!cr)
    return;

  if (cairo_get_target (cr) == ss)
    {
      /* overlapping copy (scroll): go through a temporary */
      cairo_t *tcr;

      tmp = cairo_image_surface_create (_gdk_wayland_cairo_format_for_depth (sdepth == 1 ? 1 : 32),
					w, h);
      tcr = cairo_create (tmp);
      cairo_set_operator (tcr, CAIRO_OPERATOR_SOURCE);
      cairo_set_source_surface (tcr, ss, -sx, -sy);
      cairo_paint (tcr);
      cairo_destroy (tcr);
      cairo_surface_flush (tmp);

      ss = tmp;
      sx = 0;
      sy = 0;
    }

  if (mask)
    {
      cairo_rectangle (cr, dx, dy, w, h);
      cairo_clip (cr);
      cairo_mask_surface (cr, ss, dx - sx, dy - sy);
    }
  else
    {
      cairo_set_source_surface (cr, ss, dx - sx, dy - sy);
      cairo_rectangle (cr, dx, dy, w, h);
      cairo_fill (cr);
    }

  cairo_destroy (cr);
  if (tmp)
    cairo_surface_destroy (tmp);
}

static void
gdkw_draw_drawable_with_src (GdkDrawable *drawable,
			     GdkGC       *gc,
			     GdkDrawable *src,
			     gint         xsrc,
			     gint         ysrc,
			     gint         xdest,
			     gint         ydest,
			     gint         width,
			     gint         height,
			     GdkDrawable *original_src)
{
  GdkDrawable *simpl;
  cairo_surface_t *ss;
  gint sw, sh, sdepth;

  simpl = _gdk_wayland_drawable_get_impl (src);
  ss = _gdk_drawable_ref_cairo_surface (simpl);
  if (!ss)
    return;

  gdk_drawable_get_size (simpl, &sw, &sh);
  sdepth = gdk_drawable_get_depth (simpl);

  /* clamp the source rectangle, shifting the destination along */
  if (xsrc < 0)
    {
      width += xsrc;
      xdest -= xsrc;
      xsrc = 0;
    }
  if (ysrc < 0)
    {
      height += ysrc;
      ydest -= ysrc;
      ysrc = 0;
    }
  if (xsrc + width > sw)
    width = sw - xsrc;
  if (ysrc + height > sh)
    height = sh - ysrc;

  if (width > 0 && height > 0)
    {
      cairo_surface_flush (ss);
      gdkw_blit_surface (drawable, gc, ss, sdepth,
			 xsrc, ysrc, xdest, ydest, width, height);
    }

  cairo_surface_destroy (ss);
}

static void
gdkw_draw_image (GdkDrawable *drawable,
		 GdkGC       *gc,
		 GdkImage    *image,
		 gint         xsrc,
		 gint         ysrc,
		 gint         xdest,
		 gint         ydest,
		 gint         width,
		 gint         height)
{
  cairo_surface_t *ss;
  gint x, y;

  if (!image || !image->mem)
    return;

  if (xsrc < 0)
    {
      width += xsrc;
      xdest -= xsrc;
      xsrc = 0;
    }
  if (ysrc < 0)
    {
      height += ysrc;
      ydest -= ysrc;
      ysrc = 0;
    }
  if (xsrc + width > image->width)
    width = image->width - xsrc;
  if (ysrc + height > image->height)
    height = image->height - ysrc;
  if (width <= 0 || height <= 0)
    return;

  if (image->bits_per_pixel == 32 &&
      image->byte_order == GDKW_HOST_BYTE_ORDER &&
      image->bpl % 4 == 0)
    {
      ss = cairo_image_surface_create_for_data ((guchar *) image->mem +
						ysrc * image->bpl + xsrc * 4,
						CAIRO_FORMAT_RGB24,
						width, height, image->bpl);
      if (cairo_surface_status (ss) == CAIRO_STATUS_SUCCESS)
	gdkw_blit_surface (drawable, gc, ss, 24, 0, 0, xdest, ydest, width, height);
      cairo_surface_destroy (ss);
      return;
    }

  if (image->depth == 1)
    {
      gint stride = cairo_format_stride_for_width (CAIRO_FORMAT_A1, width);
      guchar *data;

      ss = cairo_image_surface_create (CAIRO_FORMAT_A1, width, height);
      if (cairo_surface_status (ss) != CAIRO_STATUS_SUCCESS)
	{
	  cairo_surface_destroy (ss);
	  return;
	}
      stride = cairo_image_surface_get_stride (ss);
      data = cairo_image_surface_get_data (ss);
      cairo_surface_flush (ss);
      for (y = 0; y < height; y++)
	{
	  memset (data + y * stride, 0, stride);
	  for (x = 0; x < width; x++)
	    if (_gdk_wayland_image_get_pixel (image, xsrc + x, ysrc + y) & 1)
	      {
		guchar bit = (G_BYTE_ORDER == G_LITTLE_ENDIAN)
		  ? (1 << (x & 7)) : (0x80 >> (x & 7));
		data[y * stride + (x >> 3)] |= bit;
	      }
	}
      cairo_surface_mark_dirty (ss);
      gdkw_blit_surface (drawable, gc, ss, 1, 0, 0, xdest, ydest, width, height);
      cairo_surface_destroy (ss);
    }
  else if (image->depth == 24 || image->depth == 32)
    {
      gint stride;
      guchar *data;

      ss = cairo_image_surface_create (CAIRO_FORMAT_RGB24, width, height);
      if (cairo_surface_status (ss) != CAIRO_STATUS_SUCCESS)
	{
	  cairo_surface_destroy (ss);
	  return;
	}
      stride = cairo_image_surface_get_stride (ss);
      data = cairo_image_surface_get_data (ss);
      cairo_surface_flush (ss);
      for (y = 0; y < height; y++)
	{
	  guint32 *row = (guint32 *) (data + y * stride);
	  for (x = 0; x < width; x++)
	    row[x] = 0xff000000 |
	      (_gdk_wayland_image_get_pixel (image, xsrc + x, ysrc + y) & 0xffffff);
	}
      cairo_surface_mark_dirty (ss);
      gdkw_blit_surface (drawable, gc, ss, 24, 0, 0, xdest, ydest, width, height);
      cairo_surface_destroy (ss);
    }
  else
    g_debug ("draw_image: unsupported image depth %d on Wayland", image->depth);
}

static void
gdkw_draw_pixbuf (GdkDrawable *drawable,
		  GdkGC       *gc,
		  GdkPixbuf   *pixbuf,
		  gint         src_x,
		  gint         src_y,
		  gint         dest_x,
		  gint         dest_y,
		  gint         width,
		  gint         height,
		  GdkRgbDither dither,
		  gint         x_dither,
		  gint         y_dither)
{
  GdkDrawable *impl;
  cairo_t *cr;

  g_return_if_fail (GDK_IS_PIXBUF (pixbuf));

  if (width <= 0 || height <= 0)
    return;

  impl = _gdk_wayland_drawable_get_impl (drawable);
  if (gdk_drawable_get_depth (impl) == 1)
    {
      g_debug ("draw_pixbuf: depth-1 destination unsupported on Wayland");
      return;
    }

  cr = gdkw_begin (drawable, gc, 0, NULL);
  if (!cr)
    return;

  cairo_set_operator (cr, CAIRO_OPERATOR_OVER);
  gdk_cairo_set_source_pixbuf (cr, pixbuf, dest_x - src_x, dest_y - src_y);
  cairo_rectangle (cr, dest_x, dest_y, width, height);
  cairo_fill (cr);
  cairo_destroy (cr);
}

/********************************************************
 * Text (GdkFont) is not supported; use pango
 ********************************************************/

static void
gdkw_text_warn (void)
{
  static gboolean warned = FALSE;

  if (!warned)
    {
      warned = TRUE;
      g_warning ("GdkFont text drawing unsupported on Wayland, use pango");
    }
}

static void
gdkw_draw_text (GdkDrawable *drawable,
		GdkFont     *font,
		GdkGC       *gc,
		gint         x,
		gint         y,
		const gchar *text,
		gint         text_length)
{
  gdkw_text_warn ();
}

static void
gdkw_draw_text_wc (GdkDrawable    *drawable,
		   GdkFont        *font,
		   GdkGC          *gc,
		   gint            x,
		   gint            y,
		   const GdkWChar *text,
		   gint            text_length)
{
  gdkw_text_warn ();
}

/********************************************************
 * Installer
 ********************************************************/

static GdkGC *
gdkw_drawable_create_gc (GdkDrawable    *drawable,
			 GdkGCValues    *values,
			 GdkGCValuesMask mask)
{
  return _gdk_wayland_gc_new (_gdk_wayland_drawable_get_impl (drawable),
			      values, mask);
}

void
_gdk_wayland_drawable_install_vfuncs (GdkDrawableClass *klass)
{
  klass->create_gc = gdkw_drawable_create_gc;
  klass->draw_rectangle = gdkw_draw_rectangle;
  klass->draw_arc = gdkw_draw_arc;
  klass->draw_polygon = gdkw_draw_polygon;
  klass->draw_text = gdkw_draw_text;
  klass->draw_text_wc = gdkw_draw_text_wc;
  klass->draw_drawable_with_src = gdkw_draw_drawable_with_src;
  klass->draw_points = gdkw_draw_points;
  klass->draw_segments = gdkw_draw_segments;
  klass->draw_lines = gdkw_draw_lines;
  klass->draw_image = gdkw_draw_image;
  klass->draw_pixbuf = gdkw_draw_pixbuf;
}
