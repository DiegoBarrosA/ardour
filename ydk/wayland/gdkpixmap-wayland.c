/*
 * gdkpixmap-wayland.c
 *
 * ydk-wayland fork, M5 piece A: pixmaps, bitmaps and GdkImage on the
 * Wayland backend.
 *
 * Counterpart of x11/gdkpixmap-x11.c and the image half of
 * x11/gdkimage-x11.c.  Pixel storage is a cairo image surface (A1 for
 * depth 1, ARGB32 for depth 32, RGB24 otherwise).  Every entry point is
 * authored as `_gdk_wayland_*`; ydk/gdkbackend.c owns the canonical public
 * names and forwards here when the Wayland backend is selected.
 *
 * GdkImage: the GType stays defined in gdkimage-x11.c.  Wayland images are
 * plain g_object_new (gdk_image_get_type ()) objects whose pixel memory is
 * attached with g_object_set_qdata_full (), so the X11 finalizer (which sees
 * no XImage) leaks nothing.
 */

#include "config.h"

#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <cairo.h>

#include <ydk/gdk.h>
#include <ydk/gdkinternals.h>

#include "gdkpixmap-wayland.h"

#ifdef GDK_COMPILATION
#include "gdkalias.h"
#endif

#define HOST_BYTE_ORDER (G_BYTE_ORDER == G_LITTLE_ENDIAN ? GDK_LSB_FIRST : GDK_MSB_FIRST)

static void gdkw_pixmap_impl_wayland_dispose  (GObject *object);
static void gdkw_pixmap_impl_wayland_finalize (GObject *object);

G_DEFINE_TYPE (GDKWPixmapImplWayland, gdkw_pixmap_impl_wayland, GDK_TYPE_DRAWABLE)

GType
_gdk_wayland_pixmap_impl_get_type (void)
{
  return gdkw_pixmap_impl_wayland_get_type ();
}

/*************** A1 helpers ***************/

/* cairo A1 is native endian: on little endian bit 0 of a byte is the
 * leftmost pixel, on big endian bit 7 is. */
static inline gboolean
a1_get (const guchar *row, gint x)
{
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
  return (row[x >> 3] >> (x & 7)) & 1;
#else
  return (row[x >> 3] >> (7 - (x & 7))) & 1;
#endif
}

static inline void
a1_set (guchar *row, gint x, gboolean value)
{
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
  guchar mask = 1 << (x & 7);
#else
  guchar mask = 0x80 >> (x & 7);
#endif

  if (value)
    row[x >> 3] |= mask;
  else
    row[x >> 3] &= ~mask;
}

/* XBM bit order: bit 0 of each byte is the leftmost pixel. */
static inline gboolean
xbm_get (const guchar *data, gint bpl, gint x, gint y)
{
  return (data[y * bpl + (x >> 3)] >> (x & 7)) & 1;
}

/*************** drawable vfuncs ***************/

static gint
gdkw_pixmap_impl_wayland_get_depth (GdkDrawable *drawable)
{
  return GDKW_PIXMAP_IMPL_WAYLAND (drawable)->depth;
}

static void
gdkw_pixmap_impl_wayland_get_size (GdkDrawable *drawable,
				   gint        *width,
				   gint        *height)
{
  GDKWPixmapImplWayland *impl = GDKW_PIXMAP_IMPL_WAYLAND (drawable);

  if (width)
    *width = impl->width;
  if (height)
    *height = impl->height;
}

static GdkScreen *
gdkw_pixmap_impl_wayland_get_screen (GdkDrawable *drawable)
{
  return GDKW_PIXMAP_IMPL_WAYLAND (drawable)->screen;
}

static void
gdkw_pixmap_impl_wayland_set_colormap (GdkDrawable *drawable,
				       GdkColormap *cmap)
{
  GDKWPixmapImplWayland *impl = GDKW_PIXMAP_IMPL_WAYLAND (drawable);

  if (impl->colormap == cmap)
    return;

  if (cmap)
    g_object_ref (cmap);
  if (impl->colormap)
    g_object_unref (impl->colormap);
  impl->colormap = cmap;
}

static GdkColormap *
gdkw_pixmap_impl_wayland_get_colormap (GdkDrawable *drawable)
{
  return GDKW_PIXMAP_IMPL_WAYLAND (drawable)->colormap;
}

static GdkVisual *
gdkw_pixmap_impl_wayland_get_visual (GdkDrawable *drawable)
{
  GDKWPixmapImplWayland *impl = GDKW_PIXMAP_IMPL_WAYLAND (drawable);

  return impl->colormap ? gdk_colormap_get_visual (impl->colormap) : NULL;
}

static cairo_surface_t *
gdkw_pixmap_impl_wayland_ref_cairo_surface (GdkDrawable *drawable)
{
  return cairo_surface_reference (GDKW_PIXMAP_IMPL_WAYLAND (drawable)->surface);
}

static cairo_surface_t *
gdkw_pixmap_impl_wayland_create_cairo_surface (GdkDrawable *drawable,
					       int          width,
					       int          height)
{
  return _gdk_wayland_pixmap_impl_create_cairo_surface (drawable, width, height);
}

/*************** object plumbing ***************/

static void
gdkw_pixmap_impl_wayland_init (GDKWPixmapImplWayland *impl)
{
  impl->width = 1;
  impl->height = 1;
}

static void
gdkw_pixmap_impl_wayland_class_init (GDKWPixmapImplWaylandClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GdkDrawableClass *drawable_class = GDK_DRAWABLE_CLASS (klass);

  object_class->dispose = gdkw_pixmap_impl_wayland_dispose;
  object_class->finalize = gdkw_pixmap_impl_wayland_finalize;

  /* Draw vfuncs and create_gc first, then the pixmap-owned ones. */
  _gdk_wayland_drawable_install_vfuncs (drawable_class);

  drawable_class->get_size = gdkw_pixmap_impl_wayland_get_size;
  drawable_class->get_depth = gdkw_pixmap_impl_wayland_get_depth;
  drawable_class->get_screen = gdkw_pixmap_impl_wayland_get_screen;
  drawable_class->set_colormap = gdkw_pixmap_impl_wayland_set_colormap;
  drawable_class->get_colormap = gdkw_pixmap_impl_wayland_get_colormap;
  drawable_class->get_visual = gdkw_pixmap_impl_wayland_get_visual;
  drawable_class->ref_cairo_surface = gdkw_pixmap_impl_wayland_ref_cairo_surface;
  drawable_class->create_cairo_surface = gdkw_pixmap_impl_wayland_create_cairo_surface;
  drawable_class->get_image = _gdk_wayland_drawable_get_image;
  drawable_class->_copy_to_image = _gdk_wayland_drawable_copy_to_image;
}

static void
gdkw_pixmap_impl_wayland_dispose (GObject *object)
{
  GDKWPixmapImplWayland *impl = GDKW_PIXMAP_IMPL_WAYLAND (object);

  if (impl->colormap)
    {
      g_object_unref (impl->colormap);
      impl->colormap = NULL;
    }

  G_OBJECT_CLASS (gdkw_pixmap_impl_wayland_parent_class)->dispose (object);
}

static void
gdkw_pixmap_impl_wayland_finalize (GObject *object)
{
  GDKWPixmapImplWayland *impl = GDKW_PIXMAP_IMPL_WAYLAND (object);

  if (impl->surface)
    {
      cairo_surface_destroy (impl->surface);
      impl->surface = NULL;
    }

  G_OBJECT_CLASS (gdkw_pixmap_impl_wayland_parent_class)->finalize (object);
}

/*************** pixmap creation ***************/

GdkPixmap *
_gdk_wayland_pixmap_new (GdkDrawable *drawable,
			 gint         width,
			 gint         height,
			 gint         depth)
{
  GdkPixmap *pixmap;
  GDKWPixmapImplWayland *impl;
  GdkScreen *screen;
  GdkColormap *cmap;
  gint window_depth;

  g_return_val_if_fail (drawable == NULL || GDK_IS_DRAWABLE (drawable), NULL);
  g_return_val_if_fail ((drawable != NULL) || (depth != -1), NULL);
  g_return_val_if_fail ((width > 0) && (height > 0), NULL);

  if (!drawable)
    drawable = gdk_screen_get_root_window (gdk_screen_get_default ());

  if (GDK_IS_WINDOW (drawable) && GDK_WINDOW_DESTROYED (drawable))
    return NULL;

  screen = gdk_drawable_get_screen (drawable);
  window_depth = gdk_drawable_get_depth (drawable);
  if (depth == -1)
    depth = window_depth;

  pixmap = g_object_new (gdk_pixmap_get_type (), NULL);
  impl = GDKW_PIXMAP_IMPL_WAYLAND (GDK_PIXMAP_OBJECT (pixmap)->impl);
  impl->wrapper = GDK_DRAWABLE (pixmap);
  impl->screen = screen;
  impl->width = width;
  impl->height = height;
  impl->depth = depth;
  GDK_PIXMAP_OBJECT (pixmap)->depth = depth;

  impl->surface = cairo_image_surface_create (_gdk_wayland_cairo_format_for_depth (depth),
					      width, height);
  if (cairo_surface_status (impl->surface) != CAIRO_STATUS_SUCCESS)
    {
      g_warning ("Could not allocate a %dx%d depth %d pixmap", width, height, depth);
      cairo_surface_destroy (impl->surface);
      impl->surface = NULL;
      g_object_unref (pixmap);
      return NULL;
    }

  if (depth == window_depth)
    {
      cmap = gdk_drawable_get_colormap (drawable);
      if (cmap)
	gdk_drawable_set_colormap (pixmap, cmap);
    }

  return pixmap;
}

static GdkPixmap *
pixmap_from_xbm (GdkDrawable    *drawable,
		 const gchar    *data,
		 gint            width,
		 gint            height,
		 gint            depth,
		 guint32         fg_pixel,
		 guint32         bg_pixel)
{
  GdkPixmap *pixmap;
  GDKWPixmapImplWayland *impl;
  cairo_surface_t *surface;
  guchar *pixels;
  gint stride, bpl, x, y;

  pixmap = _gdk_wayland_pixmap_new (drawable, width, height, depth);
  if (!pixmap)
    return NULL;

  impl = GDKW_PIXMAP_IMPL_WAYLAND (GDK_PIXMAP_OBJECT (pixmap)->impl);
  depth = impl->depth;
  surface = impl->surface;
  bpl = (width + 7) / 8;

  cairo_surface_flush (surface);
  pixels = cairo_image_surface_get_data (surface);
  stride = cairo_image_surface_get_stride (surface);

  for (y = 0; y < height; y++)
    {
      guchar *row = pixels + y * stride;

      if (depth == 1)
	{
	  /* Start from a clean row, then set the bits. */
	  memset (row, 0, stride);
	  for (x = 0; x < width; x++)
	    {
	      gboolean on = xbm_get ((const guchar *) data, bpl, x, y);
	      guint32 pixel = on ? fg_pixel : bg_pixel;

	      a1_set (row, x, pixel & 1);
	    }
	}
      else
	{
	  guint32 *words = (guint32 *) row;

	  for (x = 0; x < width; x++)
	    {
	      gboolean on = xbm_get ((const guchar *) data, bpl, x, y);
	      guint32 pixel = on ? fg_pixel : bg_pixel;

	      words[x] = 0xff000000u | (pixel & 0x00ffffffu);
	    }
	}
    }

  cairo_surface_mark_dirty (surface);
  return pixmap;
}

GdkPixmap *
_gdk_wayland_bitmap_create_from_data (GdkDrawable *drawable,
				      const gchar *data,
				      gint         width,
				      gint         height)
{
  GdkPixmap *pixmap;
  GDKWPixmapImplWayland *impl;
  cairo_surface_t *surface;
  guchar *pixels;
  gint stride, bpl, x, y;

  g_return_val_if_fail (drawable == NULL || GDK_IS_DRAWABLE (drawable), NULL);
  g_return_val_if_fail (data != NULL, NULL);
  g_return_val_if_fail ((width > 0) && (height > 0), NULL);

  pixmap = _gdk_wayland_pixmap_new (drawable, width, height, 1);
  if (!pixmap)
    return NULL;

  impl = GDKW_PIXMAP_IMPL_WAYLAND (GDK_PIXMAP_OBJECT (pixmap)->impl);
  surface = impl->surface;
  bpl = (width + 7) / 8;

  cairo_surface_flush (surface);
  pixels = cairo_image_surface_get_data (surface);
  stride = cairo_image_surface_get_stride (surface);

  for (y = 0; y < height; y++)
    {
      guchar *row = pixels + y * stride;
      const guchar *src = (const guchar *) data + y * bpl;

      memset (row, 0, stride);
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
      memcpy (row, src, bpl);
#else
      for (x = 0; x < bpl; x++)
	{
	  guchar b = src[x], r = 0;
	  gint i;

	  for (i = 0; i < 8; i++)
	    if (b & (1 << i))
	      r |= 0x80 >> i;
	  row[x] = r;
	}
#endif
    }

  /* Padding bits past the last pixel are irrelevant, but keep them clear. */
  if (width & 7)
    for (y = 0; y < height; y++)
      for (x = width; x < bpl * 8; x++)
	a1_set (pixels + y * stride, x, FALSE);

  cairo_surface_mark_dirty (surface);
  return pixmap;
}

GdkPixmap *
_gdk_wayland_pixmap_create_from_data (GdkDrawable    *drawable,
				      const gchar    *data,
				      gint            width,
				      gint            height,
				      gint            depth,
				      const GdkColor *fg,
				      const GdkColor *bg)
{
  g_return_val_if_fail (drawable == NULL || GDK_IS_DRAWABLE (drawable), NULL);
  g_return_val_if_fail (data != NULL, NULL);
  g_return_val_if_fail (drawable != NULL || depth != -1, NULL);
  g_return_val_if_fail (fg != NULL, NULL);
  g_return_val_if_fail (bg != NULL, NULL);
  g_return_val_if_fail ((width > 0) && (height > 0), NULL);

  if (depth == -1)
    {
      GdkVisual *visual = gdk_drawable_get_visual (drawable);

      depth = visual ? visual->depth : gdk_drawable_get_depth (drawable);
    }

  return pixmap_from_xbm (drawable, data, width, height, depth,
			  fg->pixel, bg->pixel);
}

/*************** foreign / lookup: not available on Wayland ***************/

GdkPixmap *
_gdk_wayland_pixmap_foreign_new_for_display (GdkDisplay     *display,
					     GdkNativeWindow anid)
{
  g_debug ("gdk_pixmap_foreign_new_for_display is not supported on Wayland");
  return NULL;
}

GdkPixmap *
_gdk_wayland_pixmap_foreign_new_for_screen (GdkScreen      *screen,
					    GdkNativeWindow anid,
					    gint            width,
					    gint            height,
					    gint            depth)
{
  g_debug ("gdk_pixmap_foreign_new_for_screen is not supported on Wayland");
  return NULL;
}

GdkPixmap *
_gdk_wayland_pixmap_foreign_new (GdkNativeWindow anid)
{
  g_debug ("gdk_pixmap_foreign_new is not supported on Wayland");
  return NULL;
}

GdkPixmap *
_gdk_wayland_pixmap_lookup (GdkNativeWindow anid)
{
  return NULL;
}

GdkPixmap *
_gdk_wayland_pixmap_lookup_for_display (GdkDisplay     *display,
					GdkNativeWindow anid)
{
  return NULL;
}

/*************** cairo surface sharing ***************/

cairo_surface_t *
_gdk_wayland_pixmap_impl_create_cairo_surface (GdkDrawable *drawable,
					       int          width,
					       int          height)
{
  GDKWPixmapImplWayland *impl;

  g_return_val_if_fail (GDKW_IS_PIXMAP_IMPL_WAYLAND (drawable), NULL);

  impl = GDKW_PIXMAP_IMPL_WAYLAND (drawable);
  if (!impl->surface)
    return NULL;

  /* A second surface over the same bytes, so a wrapper may set its own
   * device offset without disturbing the impl's surface. */
  return cairo_image_surface_create_for_data (cairo_image_surface_get_data (impl->surface),
					      cairo_image_surface_get_format (impl->surface),
					      impl->width,
					      impl->height,
					      cairo_image_surface_get_stride (impl->surface));
}

/*************** GdkImage ***************/

static GQuark
image_mem_quark (void)
{
  return g_quark_from_static_string ("ydk-wayland-image-mem");
}

GdkImage *
_gdk_wayland_image_new_for_depth (GdkScreen    *screen,
				  GdkImageType  type,
				  GdkVisual    *visual,
				  gint          width,
				  gint          height,
				  gint          depth)
{
  GdkImage *image;
  gint bits, bpl;
  gpointer mem;

  g_return_val_if_fail (width > 0 && height > 0, NULL);
  g_return_val_if_fail (visual != NULL || depth != -1, NULL);

  if (visual)
    depth = visual->depth;

  if (depth == 1)
    bits = 1;
  else if (depth <= 8)
    bits = 8;
  else if (depth <= 16)
    bits = 16;
  else
    bits = 32;

  bpl = ((width * bits + 31) / 32) * 4;
  mem = g_malloc0 ((gsize) bpl * height);

  image = g_object_new (gdk_image_get_type (), NULL);
  image->type = GDK_IMAGE_NORMAL;
  image->visual = visual;
  image->byte_order = HOST_BYTE_ORDER;
  image->width = width;
  image->height = height;
  image->depth = depth;
  image->bits_per_pixel = bits;
  image->bpp = (bits + 7) / 8;
  image->bpl = bpl;
  image->mem = mem;

  g_object_set_qdata_full (G_OBJECT (image), image_mem_quark (), mem, g_free);

  return image;
}

GdkImage *
_gdk_wayland_image_new_bitmap (GdkVisual *visual,
			       gpointer   data,
			       gint       width,
			       gint       height)
{
  GdkImage *image;

  g_return_val_if_fail (data != NULL, NULL);
  g_return_val_if_fail (width > 0 && height > 0, NULL);

  image = g_object_new (gdk_image_get_type (), NULL);
  image->type = GDK_IMAGE_NORMAL;
  image->visual = visual;
  image->byte_order = GDK_MSB_FIRST;
  image->width = width;
  image->height = height;
  image->depth = 1;
  image->bits_per_pixel = 1;
  image->bpp = 1;
  image->bpl = (width + 7) / 8;
  image->mem = data;

  g_object_set_qdata_full (G_OBJECT (image), image_mem_quark (), data, free);

  return image;
}

guint32
_gdk_wayland_image_get_pixel (GdkImage *image,
			      gint      x,
			      gint      y)
{
  guchar *base;

  g_return_val_if_fail (image != NULL, 0);
  g_return_val_if_fail (x >= 0 && x < image->width, 0);
  g_return_val_if_fail (y >= 0 && y < image->height, 0);

  base = (guchar *) image->mem + (gsize) y * image->bpl;

  switch (image->bits_per_pixel)
    {
    case 1:
      if (image->byte_order == GDK_MSB_FIRST)
	return (base[x >> 3] & (0x80 >> (x & 7))) != 0;
      else
	return (base[x >> 3] & (1 << (x & 7))) != 0;

    case 8:
      return base[x];

    case 16:
      {
	guint16 v;

	memcpy (&v, base + x * 2, 2);
	if (image->byte_order != HOST_BYTE_ORDER)
	  v = GUINT16_SWAP_LE_BE (v);
	return v;
      }

    case 32:
      {
	guint32 v;

	memcpy (&v, base + x * 4, 4);
	if (image->byte_order != HOST_BYTE_ORDER)
	  v = GUINT32_SWAP_LE_BE (v);
	return v;
      }

    default:
      g_warning ("gdk_image_get_pixel: unsupported bits per pixel %d",
		 image->bits_per_pixel);
      return 0;
    }
}

void
_gdk_wayland_image_put_pixel (GdkImage *image,
			      gint      x,
			      gint      y,
			      guint32   pixel)
{
  guchar *base;

  g_return_if_fail (image != NULL);
  g_return_if_fail (x >= 0 && x < image->width);
  g_return_if_fail (y >= 0 && y < image->height);

  base = (guchar *) image->mem + (gsize) y * image->bpl;

  switch (image->bits_per_pixel)
    {
    case 1:
      {
	guchar mask = (image->byte_order == GDK_MSB_FIRST) ? (0x80 >> (x & 7))
							   : (1 << (x & 7));

	if (pixel & 1)
	  base[x >> 3] |= mask;
	else
	  base[x >> 3] &= ~mask;
      }
      break;

    case 8:
      base[x] = pixel;
      break;

    case 16:
      {
	guint16 v = pixel;

	if (image->byte_order != HOST_BYTE_ORDER)
	  v = GUINT16_SWAP_LE_BE (v);
	memcpy (base + x * 2, &v, 2);
      }
      break;

    case 32:
      {
	guint32 v = pixel;

	if (image->byte_order != HOST_BYTE_ORDER)
	  v = GUINT32_SWAP_LE_BE (v);
	memcpy (base + x * 4, &v, 4);
      }
      break;

    default:
      g_warning ("gdk_image_put_pixel: unsupported bits per pixel %d",
		 image->bits_per_pixel);
      break;
    }
}

/*************** read-back from any Wayland drawable ***************/

GdkImage *
_gdk_wayland_drawable_copy_to_image (GdkDrawable *drawable,
				     GdkImage    *image,
				     gint         src_x,
				     gint         src_y,
				     gint         dest_x,
				     gint         dest_y,
				     gint         width,
				     gint         height)
{
  GdkDrawable *impl;
  cairo_surface_t *surface;
  const guchar *pixels;
  gint stride, sw, sh, depth;
  gint x0, y0, x1, y1, x, y;
  gboolean fresh = FALSE;

  g_return_val_if_fail (GDK_IS_DRAWABLE (drawable), NULL);
  g_return_val_if_fail (width > 0 && height > 0, NULL);

  impl = _gdk_wayland_drawable_get_impl (drawable);
  surface = _gdk_drawable_ref_cairo_surface (impl);
  if (!surface)
    return NULL;

  cairo_surface_flush (surface);

  depth = gdk_drawable_get_depth (impl);

  if (!image)
    {
      image = _gdk_wayland_image_new_for_depth (gdk_drawable_get_screen (impl),
						GDK_IMAGE_NORMAL,
						gdk_drawable_get_visual (impl),
						width, height, depth);
      if (!image)
	{
	  cairo_surface_destroy (surface);
	  return NULL;
	}
      fresh = TRUE;
      dest_x = 0;
      dest_y = 0;
    }

  if (cairo_surface_get_type (surface) != CAIRO_SURFACE_TYPE_IMAGE)
    {
      cairo_surface_destroy (surface);
      return image;
    }

  pixels = cairo_image_surface_get_data (surface);
  stride = cairo_image_surface_get_stride (surface);
  sw = cairo_image_surface_get_width (surface);
  sh = cairo_image_surface_get_height (surface);

  /* Clamp the rectangle, in destination coordinates, to both the image and
   * the part of it that the source covers. */
  x0 = MAX (dest_x, 0);
  y0 = MAX (dest_y, 0);
  x0 = MAX (x0, dest_x - src_x);
  y0 = MAX (y0, dest_y - src_y);
  x1 = MIN (dest_x + width, image->width);
  y1 = MIN (dest_y + height, image->height);
  x1 = MIN (x1, dest_x - src_x + sw);
  y1 = MIN (y1, dest_y - src_y + sh);

  if (!fresh)
    {
      /* Zero the whole requested rectangle first; the intersection is
       * overwritten below. */
      gint cx0 = MAX (dest_x, 0), cy0 = MAX (dest_y, 0);
      gint cx1 = MIN (dest_x + width, image->width);
      gint cy1 = MIN (dest_y + height, image->height);

      for (y = cy0; y < cy1; y++)
	for (x = cx0; x < cx1; x++)
	  _gdk_wayland_image_put_pixel (image, x, y, 0);
    }

  if (x0 < x1 && y0 < y1)
    {
      /* Depth-24 images report 0x00RRGGBB like X11 does, so the (undefined or
       * 0xff) cairo RGB24/ARGB32 padding byte is masked off.  Depth-32 images
       * keep it verbatim. */
      gboolean mask24 = (image->depth == 24);
      gboolean fast32 = (cairo_image_surface_get_format (surface) != CAIRO_FORMAT_A1 &&
			 !mask24 &&
			 image->bits_per_pixel == 32 &&
			 image->byte_order == HOST_BYTE_ORDER);

      for (y = y0; y < y1; y++)
	{
	  const guchar *srow = pixels + (gsize) (y - dest_y + src_y) * stride;

	  if (fast32)
	    {
	      memcpy ((guchar *) image->mem + (gsize) y * image->bpl + x0 * 4,
		      srow + (x0 - dest_x + src_x) * 4,
		      (gsize) (x1 - x0) * 4);
	      continue;
	    }

	  for (x = x0; x < x1; x++)
	    {
	      gint sx = x - dest_x + src_x;
	      guint32 pixel;

	      if (cairo_image_surface_get_format (surface) == CAIRO_FORMAT_A1)
		pixel = a1_get (srow, sx);
	      else
		memcpy (&pixel, srow + sx * 4, 4);
	      if (mask24 && cairo_image_surface_get_format (surface) != CAIRO_FORMAT_A1)
		pixel &= 0x00ffffff;

	      _gdk_wayland_image_put_pixel (image, x, y, pixel);
	    }
	}
    }

  cairo_surface_destroy (surface);
  return image;
}

GdkImage *
_gdk_wayland_drawable_get_image (GdkDrawable *drawable,
				 gint         x,
				 gint         y,
				 gint         width,
				 gint         height)
{
  return _gdk_wayland_drawable_copy_to_image (drawable, NULL, x, y, 0, 0,
					      width, height);
}

/*************** bitmap -> region ***************/

GdkRegion *
_gdk_wayland_bitmap_to_region (GdkBitmap *bitmap)
{
  GdkDrawable *impl;
  GDKWPixmapImplWayland *pix;
  GdkRegion *region;
  const guchar *pixels;
  gint stride, x, y;

  g_return_val_if_fail (bitmap != NULL, NULL);

  impl = _gdk_wayland_drawable_get_impl (GDK_DRAWABLE (bitmap));
  if (!GDKW_IS_PIXMAP_IMPL_WAYLAND (impl))
    return NULL;

  pix = GDKW_PIXMAP_IMPL_WAYLAND (impl);
  if (pix->depth != 1 || !pix->surface)
    return NULL;

  cairo_surface_flush (pix->surface);
  pixels = cairo_image_surface_get_data (pix->surface);
  stride = cairo_image_surface_get_stride (pix->surface);

  region = gdk_region_new ();

  for (y = 0; y < pix->height; y++)
    {
      const guchar *row = pixels + (gsize) y * stride;

      x = 0;
      while (x < pix->width)
	{
	  GdkRectangle rect;

	  if (!a1_get (row, x))
	    {
	      x++;
	      continue;
	    }

	  rect.x = x;
	  rect.y = y;
	  rect.height = 1;
	  while (x < pix->width && a1_get (row, x))
	    x++;
	  rect.width = x - rect.x;

	  gdk_region_union_with_rect (region, &rect);
	}
    }

  return region;
}
