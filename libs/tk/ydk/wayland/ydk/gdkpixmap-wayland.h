/*
 * gdkpixmap-wayland.h
 *
 * ydk-wayland M5 (offscreen drawing core): the ONE header shared by the
 * three independently implemented pieces
 *
 *   (A) ydk/wayland/gdkpixmap-wayland.c    pixmap impl, bitmaps, GdkImage
 *   (B) ydk/wayland/gdkdrawable-wayland.c  GdkDrawableClass draw vfuncs
 *   (C) ydk/wayland/gdkgc-wayland.c        GdkGC subclass + clip wiring
 *
 * Everything the pieces need to know about each other is declared here;
 * nothing else is shared.  Each declaration below is tagged with the piece
 * that DEFINES it, so a piece can be written and compile-checked against
 * the header alone, and linked with the other two later.
 *
 * Ground rules (all three pieces):
 *   - Wayland-native symbols are `_gdk_wayland_*` / `gdkw_*`, never a bare
 *     `gdk_*` / `_gdk_*` name; the dispatchers in ydk/gdkbackend.c own those.
 *   - No Xlib.  Every .c file starts with the usual
 *       #include "config.h" ... <ydk/gdk.h> <ydk/gdkinternals.h>
 *       #include "gdkpixmap-wayland.h"
 *       #ifdef GDK_COMPILATION / #include "gdkalias.h" / #endif
 *   - The only visual on this backend is the hand-built 24-bit TRUE_COLOR
 *     visual (red 0xff0000, green 0x00ff00, blue 0x0000ff, little endian).
 *     Pixel values are therefore always 0x00RRGGBB; there is no colormap
 *     allocation.  See _gdk_wayland_pixel_to_rgb().
 *   - Pixmap pixel storage is a cairo image surface; depth maps to a cairo
 *     format with _gdk_wayland_cairo_format_for_depth():
 *         depth 1   -> CAIRO_FORMAT_A1   (bit set = 1, bit clear = 0)
 *         depth 32  -> CAIRO_FORMAT_ARGB32
 *         otherwise -> CAIRO_FORMAT_RGB24
 *     Windows keep their existing shm-backed ARGB32 surface.
 */

#ifndef __YDK_GDKPIXMAP_WAYLAND_H__
#define __YDK_GDKPIXMAP_WAYLAND_H__

#include <glib-object.h>
#include <cairo.h>

#include <ydk/gdk.h>
#include <ydk/gdkinternals.h>

#include "gdkprivate-wayland.h"

G_BEGIN_DECLS

/********************************************************
 * Types
 ********************************************************/

#ifdef GDK_COMPILATION
#define GDKW_TYPE_PIXMAP_IMPL_WAYLAND      (gdkw_pixmap_impl_wayland_get_type ())
#define GDKW_PIXMAP_IMPL_WAYLAND(obj)      (G_TYPE_CHECK_INSTANCE_CAST ((obj), GDKW_TYPE_PIXMAP_IMPL_WAYLAND, GDKWPixmapImplWayland))
#define GDKW_PIXMAP_IMPL_WAYLAND_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST ((klass), GDKW_TYPE_PIXMAP_IMPL_WAYLAND, GDKWPixmapImplWaylandClass))
#define GDKW_IS_PIXMAP_IMPL_WAYLAND(obj)   (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GDKW_TYPE_PIXMAP_IMPL_WAYLAND))
#define GDKW_IS_PIXMAP_IMPL_WAYLAND_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), GDKW_TYPE_PIXMAP_IMPL_WAYLAND))
#define GDKW_PIXMAP_IMPL_WAYLAND_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS ((obj), GDKW_TYPE_PIXMAP_IMPL_WAYLAND, GDKWPixmapImplWaylandClass))

#define GDKW_TYPE_GC_WAYLAND               (gdkw_gc_wayland_get_type ())
#define GDKW_GC_WAYLAND(obj)               (G_TYPE_CHECK_INSTANCE_CAST ((obj), GDKW_TYPE_GC_WAYLAND, GDKWGCWayland))
#define GDKW_GC_WAYLAND_CLASS(klass)       (G_TYPE_CHECK_CLASS_CAST ((klass), GDKW_TYPE_GC_WAYLAND, GDKWGCWaylandClass))
#define GDKW_IS_GC_WAYLAND(obj)            (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GDKW_TYPE_GC_WAYLAND))
#define GDKW_IS_GC_WAYLAND_CLASS(klass)    (G_TYPE_CHECK_CLASS_TYPE ((klass), GDKW_TYPE_GC_WAYLAND))
#define GDKW_GC_WAYLAND_GET_CLASS(obj)     (G_TYPE_INSTANCE_GET_CLASS ((obj), GDKW_TYPE_GC_WAYLAND, GDKWGCWaylandClass))
#endif /* GDK_COMPILATION */

typedef struct _GDKWPixmapImplWayland      GDKWPixmapImplWayland;
typedef struct _GDKWPixmapImplWaylandClass GDKWPixmapImplWaylandClass;
typedef struct _GDKWGCWayland              GDKWGCWayland;
typedef struct _GDKWGCWaylandClass         GDKWGCWaylandClass;

/* The delegate object behind every GdkPixmap on the Wayland backend
 * (GdkPixmapObject.impl).  A GdkDrawable subclass, mirroring
 * GDKWWindowImplWayland.  Created and owned by piece A; B and C only read
 * it through the generic GdkDrawable API (gdk_drawable_get_depth,
 * _gdk_drawable_ref_cairo_surface, ...), never through these fields. */
struct _GDKWPixmapImplWayland
{
  GdkDrawable parent_instance;

  /*< private >*/
  GdkDrawable *wrapper;		/* the GdkPixmap wrapping us; NOT ref'd */
  GdkScreen *screen;		/* NOT ref'd (as in the X11 impl) */
  GdkColormap *colormap;	/* ref'd; NULL for depth != window depth */
  gint width, height;
  gint depth;			/* 1, 24, 32 ...; mirrored in wrapper->depth */

  /* Never NULL between instance init and dispose once _gdk_wayland_pixmap_new
   * returned.  Format per _gdk_wayland_cairo_format_for_depth().  The pixel
   * bytes live in malloc'ed memory owned by cairo (cairo_image_surface_create),
   * so cairo_surface_get_data() is valid for depth 1/24/32. */
  cairo_surface_t *surface;
};

struct _GDKWPixmapImplWaylandClass
{
  GdkDrawableClass parent_class;
};

/* The GdkGC subclass handed out by every Wayland drawable's create_gc.
 * Owned by piece C.  The core GdkGC (ydk/gdkgc.c) already stores, and
 * exposes through _gdk_gc_get_*(): clip region, clip mask, fill, tile,
 * stipple, foreground/background PIXEL, subwindow mode, exposures, plus
 * the public fields clip_x_origin / clip_y_origin / ts_x_origin /
 * ts_y_origin / colormap.  This struct holds only what the core does not. */
struct _GDKWGCWayland
{
  GdkGC parent_instance;

  /*< private >*/
  GdkScreen *screen;		/* NOT ref'd; answers gdk_gc_get_screen() */
  gint depth;			/* depth of the drawable it was created for */

  GdkFunction function;		/* default GDK_COPY */
  GdkFont *font;		/* ref'd, or NULL */
  gint line_width;		/* default 0 (thin) */
  GdkLineStyle line_style;	/* default GDK_LINE_SOLID */
  GdkCapStyle cap_style;	/* default GDK_CAP_BUTT */
  GdkJoinStyle join_style;	/* default GDK_JOIN_MITER */
  gint dash_offset;
  gint8 *dashes;		/* g_malloc'ed copy; NULL = default {4, 4} */
  gint n_dashes;
};

struct _GDKWGCWaylandClass
{
  GdkGCClass parent_class;
};

GType gdkw_pixmap_impl_wayland_get_type (void) G_GNUC_CONST;	/* A */
GType gdkw_gc_wayland_get_type (void) G_GNUC_CONST;		/* C */

/********************************************************
 * Inline helpers shared by all three pieces (defined here so that
 * nobody depends on anybody else's .c file for them)
 ********************************************************/

/* The impl behind any drawable: GdkWindow -> its impl, GdkPixmap -> its
 * impl, anything else (already an impl) -> itself.  Drawing code must
 * always resolve to the impl before asking for a cairo surface: the window
 * WRAPPER's cairo surface carries a per-window device offset, the impl's
 * does not.  Source drawables of copies (gdk_draw_drawable) may arrive as a
 * wrapper and must be resolved the same way. */
static inline GdkDrawable *
_gdk_wayland_drawable_get_impl (GdkDrawable *drawable)
{
  if (GDK_IS_WINDOW (drawable))
    return GDK_WINDOW_OBJECT (drawable)->impl;
  if (GDK_IS_PIXMAP (drawable))
    return GDK_PIXMAP_OBJECT (drawable)->impl;
  return drawable;
}

static inline cairo_format_t
_gdk_wayland_cairo_format_for_depth (gint depth)
{
  if (depth == 1)
    return CAIRO_FORMAT_A1;
  if (depth == 32)
    return CAIRO_FORMAT_ARGB32;
  return CAIRO_FORMAT_RGB24;
}

/* GdkColor.pixel / GdkGC fg,bg pixel -> 0..1 components.  Valid for
 * depth 24 and 32 drawables (the only colour visual on this backend); the
 * alpha byte, if any, is ignored.  For depth-1 targets do NOT use this:
 * pixel 1 means "set the bit", pixel 0 means "clear the bit". */
static inline void
_gdk_wayland_pixel_to_rgb (guint32 pixel, gdouble *r, gdouble *g, gdouble *b)
{
  *r = ((pixel >> 16) & 0xff) / 255.0;
  *g = ((pixel >>  8) & 0xff) / 255.0;
  *b = ( pixel        & 0xff) / 255.0;
}

/********************************************************
 * Piece C: GC wiring (ydk/wayland/gdkgc-wayland.c)
 ********************************************************/

/* How _gdk_wayland_gc_setup_cairo() should prepare a cairo_t. */
typedef enum
{
  /* source := the GC's fill: solid foreground, GDK_TILED tile, or
   * GDK_STIPPLED / GDK_OPAQUE_STIPPLED stipple. */
  GDKW_GC_USE_FILL    = 1 << 0,

  /* source := solid BACKGROUND pixel (the GDK_LINE_DOUBLE_DASH under-pass).
   * Wins over GDKW_GC_USE_FILL if both are given.  With GDKW_GC_USE_STROKE
   * it also forces a SOLID (undashed) stroke, so B can paint the whole line
   * in the background colour first and then stroke the dashes over it. */
  GDKW_GC_USE_BG      = 1 << 1,

  /* also apply line width, cap, join and dash pattern for cairo_stroke(). */
  GDKW_GC_USE_STROKE  = 1 << 2,

  /* GDK_COPY maps to CAIRO_OPERATOR_SOURCE instead of OVER.  Blits
   * (draw_drawable, draw_image, bitmap-to-bitmap copies) pass this. */
  GDKW_GC_REPLACE     = 1 << 3,

  /* leave cairo's default antialiasing on.  Without it the setup selects
   * CAIRO_ANTIALIAS_NONE (X11 semantics; required for exact depth-1 output). */
  GDKW_GC_ANTIALIAS   = 1 << 4
} GDKWGCFlags;

/* The create_gc implementation for every Wayland drawable class.  Builds a
 * GDKWGCWayland, calls _gdk_gc_init (gc, drawable, values, mask), records
 * screen/depth from @drawable (an impl), applies the attribute defaults
 * listed on the struct, then values/mask through the same code path as
 * set_values.  Installed by B (as GdkDrawableClass.create_gc). */
GdkGC *_gdk_wayland_gc_new (GdkDrawable      *drawable,
			    GdkGCValues      *values,
			    GdkGCValuesMask   mask);

/* Prepares @cr (a context freshly obtained for @impl, see B) to draw with
 * @gc.  Returns FALSE when nothing at all should be drawn (empty clip
 * region, or GDK_NOOP), in which case B just destroys @cr.  Contract:
 *   - ADDS the GC clip on top of whatever clip @cr already has (never
 *     cairo_reset_clip()s it): region translated by
 *     (gc->clip_x_origin, gc->clip_y_origin) in device space, and the clip
 *     mask (if any) via _gdk_wayland_bitmap_to_region() translated likewise.
 *     The region already contains the GdkWindow client-side clip emulation
 *     that gdkwindow.c start_draw_helper() merged in with
 *     _gdk_gc_add_drawable_clip().
 *   - MUST NOT call _gdk_gc_remove_drawable_clip() and MUST NOT call
 *     _gdk_gc_update_context(): the first throws away the window clip, the
 *     second warns on colormap-less (depth-1) GCs.
 *   - sets the operator from gc->function (table in the M5 spec), the
 *     source from @flags and the target depth (gdk_drawable_get_depth (@impl)),
 *     antialias, CAIRO_FILL_RULE_EVEN_ODD, and, with GDKW_GC_USE_STROKE, line
 *     attributes (width 0 is set as 1.0).
 *   - depth-1 targets: pixel & 1 -> opaque source with OVER, pixel 0 ->
 *     CAIRO_OPERATOR_CLEAR; tile/stipple ignored (treated as solid). */
gboolean _gdk_wayland_gc_setup_cairo (GdkGC        *gc,
				      cairo_t      *cr,
				      GdkDrawable  *impl,
				      GDKWGCFlags   flags);

/* The effective clip in destination device space: gc clip region (+ the
 * region form of the clip mask) translated by the clip origin.  Returns a
 * NEW region the caller destroys, or NULL when the GC is unclipped.  An
 * empty region means "everything is clipped".  Used by
 * _gdk_wayland_gc_setup_cairo() itself and available to B for early-outs. */
GdkRegion *_gdk_wayland_gc_get_clip_device_region (GdkGC *gc);

/* Wayland half of the dispatched public gdk_gc_get_screen(). */
GdkScreen *_gdk_wayland_gc_get_screen (GdkGC *gc);

/* _gdk_wayland_windowing_gc_set_clip_region() and
 * _gdk_wayland_windowing_gc_copy() are already declared in
 * gdkprivate-wayland.h and already dispatched by gdkbackend.c; piece C
 * only supplies their real bodies (and the integrator deletes the empty
 * stubs, see the spec). */

/********************************************************
 * Piece B: drawable vfuncs (ydk/wayland/gdkdrawable-wayland.c)
 ********************************************************/

/* Fills in, on @klass: create_gc, draw_rectangle, draw_arc, draw_polygon,
 * draw_text, draw_text_wc, draw_drawable_with_src, draw_points,
 * draw_segments, draw_lines, draw_image, draw_pixbuf.  Does NOT touch
 * get_size/get_depth/colormap/visual/screen/cairo-surface/get_image/
 * _copy_to_image (the impl owns those).  Called first in class_init of
 * GDKWPixmapImplWayland (A) and GDKWWindowImplWayland (integrator adds one
 * line to gdkw_window_impl_wayland_class_init). */
void _gdk_wayland_drawable_install_vfuncs (GdkDrawableClass *klass);

/********************************************************
 * Piece A: pixmaps, bitmaps, images (ydk/wayland/gdkpixmap-wayland.c)
 ********************************************************/

/* Wayland halves of the dispatched pixmap entry points (X11 versions are
 * renamed _gdk_x11_*; dispatchers live in ydk/gdkbackend.c). */
GType      _gdk_wayland_pixmap_impl_get_type (void) G_GNUC_CONST;
GdkPixmap *_gdk_wayland_pixmap_new (GdkDrawable *drawable,
				    gint         width,
				    gint         height,
				    gint         depth);
GdkPixmap *_gdk_wayland_bitmap_create_from_data (GdkDrawable *drawable,
						 const gchar *data,
						 gint         width,
						 gint         height);
GdkPixmap *_gdk_wayland_pixmap_create_from_data (GdkDrawable    *drawable,
						 const gchar    *data,
						 gint            width,
						 gint            height,
						 gint            depth,
						 const GdkColor *fg,
						 const GdkColor *bg);
/* Wayland has no native pixmap handles: the foreign_new family returns
 * NULL (with a g_debug), the lookup family returns NULL. */
GdkPixmap *_gdk_wayland_pixmap_foreign_new_for_display (GdkDisplay      *display,
							GdkNativeWindow  anid);
GdkPixmap *_gdk_wayland_pixmap_foreign_new_for_screen  (GdkScreen       *screen,
							GdkNativeWindow  anid,
							gint             width,
							gint             height,
							gint             depth);
GdkPixmap *_gdk_wayland_pixmap_foreign_new             (GdkNativeWindow  anid);
GdkPixmap *_gdk_wayland_pixmap_lookup                  (GdkNativeWindow  anid);
GdkPixmap *_gdk_wayland_pixmap_lookup_for_display      (GdkDisplay      *display,
							GdkNativeWindow  anid);

/* GdkImage family (the GdkImage GType itself stays defined in
 * gdkimage-x11.c; Wayland images are plain g_object_new (gdk_image_get_type ())
 * objects whose pixel memory is attached with g_object_set_qdata_full () so
 * the X11 finalizer, which sees ximage == NULL, leaks nothing). */
GdkImage *_gdk_wayland_image_new_for_depth (GdkScreen    *screen,
					    GdkImageType  type,
					    GdkVisual    *visual,
					    gint          width,
					    gint          height,
					    gint          depth);
GdkImage *_gdk_wayland_image_new_bitmap (GdkVisual *visual,
					 gpointer   data,
					 gint       width,
					 gint       height);
guint32   _gdk_wayland_image_get_pixel (GdkImage *image, gint x, gint y);
void      _gdk_wayland_image_put_pixel (GdkImage *image, gint x, gint y,
					guint32 pixel);

/* Generic read-back from ANY Wayland drawable impl (pixmap or window),
 * built on _gdk_drawable_ref_cairo_surface (impl).  The window impl class
 * installs these two as its get_image and _copy_to_image vfuncs. */
GdkImage *_gdk_wayland_drawable_get_image (GdkDrawable *drawable,
					   gint         x,
					   gint         y,
					   gint         width,
					   gint         height);
GdkImage *_gdk_wayland_drawable_copy_to_image (GdkDrawable *drawable,
					       GdkImage    *image,
					       gint         src_x,
					       gint         src_y,
					       gint         dest_x,
					       gint         dest_y,
					       gint         width,
					       gint         height);

/* New cairo surface over the SAME pixel bytes as the pixmap impl's surface
 * (cairo_image_surface_create_for_data), so each wrapper can carry its own
 * device offset.  Target of _gdk_wayland_windowing_create_cairo_surface ()
 * when the drawable is a GDKWPixmapImplWayland. */
cairo_surface_t *_gdk_wayland_pixmap_impl_create_cairo_surface (GdkDrawable *impl,
								int          width,
								int          height);

/* Depth-1 bitmap -> region of its SET bits (rows coalesced into
 * rectangles).  Accepts the bitmap wrapper or impl.  Used by C for
 * gdk_gc_set_clip_mask(), and by M7 as the body of
 * _gdk_wayland_windowing_get_shape_for_mask().  Returns a new region
 * (possibly empty) or NULL if @bitmap is not a depth-1 Wayland pixmap. */
GdkRegion *_gdk_wayland_bitmap_to_region (GdkBitmap *bitmap);

G_END_DECLS

#endif /* __YDK_GDKPIXMAP_WAYLAND_H__ */
