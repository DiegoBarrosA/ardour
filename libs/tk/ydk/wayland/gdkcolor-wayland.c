/*
 * gdkcolor-wayland.c
 *
 * ydk-wayland fork: Wayland GdkColormap backend (gap F7).
 *
 * The colormap family used to live only in x11/gdkcolor-x11.c, which keeps
 * a GdkColormapPrivateX11 (screen, Colormap XID, hash...) behind
 * colormap->windowing_data.  A colormap built by the Wayland screen has
 * private->screen == NULL, so gdk_colormap_get_screen, the finalizer and
 * (for non TrueColor paths) alloc/query would dereference NULL.
 *
 * Wayland only ever has a TrueColor visual, so the semantics are trivial:
 *
 *   pixel = 0x00RRGGBB            depth 24 (the system visual)
 *   pixel = 0xAARRGGBB, AA = ff   depth 32 (an ARGB visual)
 *
 * which is exactly what _gdk_wayland_pixel_to_rgb () decodes.  The pixel is
 * computed from the visual's masks/shifts/precisions, the same way the X11
 * TrueColor branch does it, so a visual with other masks still works.
 * Nothing can fail, there is nothing to free, and no Xlib is involved.
 *
 * Type layout: GDK_TYPE_COLORMAP (gdk_colormap_get_type) stays owned by the
 * X11 file, because the shared headers and gdkscreen-wayland.c need one
 * GType.  Its finalizer would dereference the (NULL) X11 private screen, so
 * Wayland colormaps are instances of a private SUBCLASS, GDKWColormapWayland,
 * whose finalizer skips the GdkColormap (X11) finalizer and chains straight
 * to GObject.  It also carries the screen, since the X11 slot is empty.
 *
 * Naming: every definition is `_gdk_wayland_colormap_*`, the targets of the
 * IA__ rename macros / dispatchers (see the wiring notes with this change).
 * There are deliberately no alias blocks in this file.
 */

#include "config.h"

#include <glib.h>
#include <glib-object.h>

#include <ydk/gdk.h>
#include <ydk/gdkcolor.h>
#include <ydk/gdkscreen.h>
#include <ydk/gdkvisual.h>

#include "gdkprivate-wayland.h"
#include "gdkpixmap-wayland.h"

#ifdef GDK_COMPILATION
#include "gdkalias.h"
#endif

/* ---- the Wayland colormap subclass ---- */

typedef struct _GDKWColormapWayland      GDKWColormapWayland;
typedef struct _GDKWColormapWaylandClass GDKWColormapWaylandClass;

#define GDKW_TYPE_COLORMAP_WAYLAND        (gdkw_colormap_wayland_get_type ())
#define GDKW_COLORMAP_WAYLAND(obj)        (G_TYPE_CHECK_INSTANCE_CAST ((obj), GDKW_TYPE_COLORMAP_WAYLAND, GDKWColormapWayland))
#define GDKW_IS_COLORMAP_WAYLAND(obj)     (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GDKW_TYPE_COLORMAP_WAYLAND))

struct _GDKWColormapWayland
{
  GdkColormap parent_instance;

  /* NOT ref'd: the screen owns its system colormap, a ref would be a
   * cycle.  Colormaps made by gdk_colormap_new () follow the X11 backend
   * and do not keep the screen alive either. */
  GdkScreen *screen;
};

struct _GDKWColormapWaylandClass
{
  GdkColormapClass parent_class;
};

static GType gdkw_colormap_wayland_get_type (void) G_GNUC_CONST;

static void gdkw_colormap_wayland_finalize (GObject *object);

G_DEFINE_TYPE (GDKWColormapWayland, gdkw_colormap_wayland, GDK_TYPE_COLORMAP)

static void
gdkw_colormap_wayland_init (GDKWColormapWayland *colormap)
{
  colormap->screen = NULL;
}

static void
gdkw_colormap_wayland_class_init (GDKWColormapWaylandClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = gdkw_colormap_wayland_finalize;
}

static void
gdkw_colormap_wayland_finalize (GObject *object)
{
  GdkColormap *colormap = GDK_COLORMAP (object);
  GObjectClass *gobject_class;

  /* The visual is owned by the screen (not ref'd here).  colormap->colors
   * is always NULL for TrueColor, but free it in case a caller set it. */
  g_free (colormap->colors);
  colormap->colors = NULL;
  GDKW_COLORMAP_WAYLAND (object)->screen = NULL;

  /* Do NOT chain to gdkw_colormap_wayland_parent_class: that is the X11
   * GdkColormap finalizer, which dereferences private->screen (NULL here)
   * and calls XFreeColormap.  Chain to GObject directly instead. */
  gobject_class = g_type_class_peek_parent (gdkw_colormap_wayland_parent_class);
  gobject_class->finalize (object);
}

/* ---- pixel <-> GdkColor helpers (TrueColor only) ---- */

static guint32
colormap_color_to_pixel (GdkVisual *visual, const GdkColor *color)
{
  guint32 padding, unused;

  /* If bits not used for color are used for something other than padding,
   * it is alpha, so set them to 1s (opaque).  Same rule as the X11
   * TrueColor branch.  Shifting by >= 32 is undefined in C. */
  if (visual->depth >= 32)
    padding = 0;
  else
    padding = ((~(guint32) 0)) << visual->depth;

  unused = ~(guint32) (visual->red_mask | visual->green_mask |
		       visual->blue_mask | padding);

  return unused +
    (((guint32) (color->red   >> (16 - visual->red_prec))   << visual->red_shift) +
     ((guint32) (color->green >> (16 - visual->green_prec)) << visual->green_shift) +
     ((guint32) (color->blue  >> (16 - visual->blue_prec))  << visual->blue_shift));
}

static guint16
colormap_channel_from_pixel (guint32 pixel, guint32 mask, gint shift, gint prec)
{
  guint32 max = (1u << prec) - 1;

  if (max == 0)
    return 0;

  return (guint16) (65535. * (gdouble) ((pixel & mask) >> shift) / (gdouble) max + 0.5);
}

/* ---- construction (called by gdkscreen-wayland.c and gdk_colormap_new) ---- */

/* Builds a TrueColor colormap over @visual for @screen.  Replaces the
 * g_object_new (GDK_TYPE_COLORMAP) in _gdk_wayland_screen_create_colormap,
 * whose X11 finalizer would crash on a NULL private->screen. */
GdkColormap *
_gdk_wayland_colormap_new_for_screen (GdkScreen *screen, GdkVisual *visual)
{
  GdkColormap *colormap;

  g_return_val_if_fail (visual != NULL, NULL);

  colormap = g_object_new (GDKW_TYPE_COLORMAP_WAYLAND, NULL);
  GDKW_COLORMAP_WAYLAND (colormap)->screen = screen;
  colormap->visual = visual;
  colormap->size = visual->colormap_size;
  colormap->colors = NULL;

  return colormap;
}

/* gdk_colormap_new: @allocate is meaningless for TrueColor (there is no
 * palette to allocate), so it is ignored. */
GdkColormap *
_gdk_wayland_colormap_new (GdkVisual *visual, gboolean allocate)
{
  g_return_val_if_fail (GDK_IS_VISUAL (visual), NULL);

  return _gdk_wayland_colormap_new_for_screen (gdk_screen_get_default (), visual);
}

/* ---- the public colormap family ---- */

GdkScreen *
_gdk_wayland_colormap_get_screen (GdkColormap *cmap)
{
  g_return_val_if_fail (GDK_IS_COLORMAP (cmap), NULL);
  g_return_val_if_fail (GDKW_IS_COLORMAP_WAYLAND (cmap), NULL);

  return GDKW_COLORMAP_WAYLAND (cmap)->screen;
}

/* Number of entries of the default colormap.  X11 answers DisplayCells,
 * which for a TrueColor 8-bit-per-channel root is 256. */
gint
_gdk_wayland_colormap_get_system_size (void)
{
  GdkScreen *screen = gdk_screen_get_default ();
  GdkVisual *visual;

  g_return_val_if_fail (screen != NULL, 0);

  visual = gdk_screen_get_system_visual (screen);
  g_return_val_if_fail (visual != NULL, 0);

  return 1 << visual->bits_per_rgb;
}

/* Obsolete, palette only.  Nothing to change on a TrueColor colormap. */
void
_gdk_wayland_colormap_change (GdkColormap *colormap, gint ncolors)
{
  g_return_if_fail (GDK_IS_COLORMAP (colormap));
}

/* Obsolete cell allocation.  There are no writable cells in TrueColor, so
 * this fails, as XAllocColorCells does on an X11 TrueColor colormap. */
gint
_gdk_wayland_colors_alloc (GdkColormap *colormap,
			   gboolean     contiguous,
			   gulong      *planes,
			   gint         nplanes,
			   gulong      *pixels,
			   gint         npixels)
{
  g_return_val_if_fail (GDK_IS_COLORMAP (colormap), 0);

  return FALSE;
}

/* Nothing was allocated, so nothing is freed. */
void
_gdk_wayland_colors_free (GdkColormap *colormap,
			  gulong      *pixels,
			  gint         npixels,
			  gulong       planes)
{
  g_return_if_fail (GDK_IS_COLORMAP (colormap));
}

void
_gdk_wayland_colormap_free_colors (GdkColormap    *colormap,
				   const GdkColor *colors,
				   gint            n_colors)
{
  g_return_if_fail (GDK_IS_COLORMAP (colormap));
  g_return_if_fail (colors != NULL);
}

/* Never fails: every color maps to a pixel.  Returns the number of colors
 * NOT allocated, which is always 0.  @writeable and @best_match are moot
 * (no palette, no approximation needed). */
gint
_gdk_wayland_colormap_alloc_colors (GdkColormap *colormap,
				    GdkColor    *colors,
				    gint         n_colors,
				    gboolean     writeable,
				    gboolean     best_match,
				    gboolean    *success)
{
  GdkVisual *visual;
  gint i;

  g_return_val_if_fail (GDK_IS_COLORMAP (colormap), n_colors);
  g_return_val_if_fail (colors != NULL, n_colors);
  g_return_val_if_fail (success != NULL, n_colors);

  visual = colormap->visual;
  g_return_val_if_fail (visual != NULL, n_colors);

  for (i = 0; i < n_colors; i++)
    {
      colors[i].pixel = colormap_color_to_pixel (visual, &colors[i]);
      success[i] = TRUE;
    }

  return 0;
}

void
_gdk_wayland_colormap_query_color (GdkColormap *colormap,
				   gulong       pixel,
				   GdkColor    *result)
{
  GdkVisual *visual;
  guint32 p = (guint32) pixel;

  g_return_if_fail (GDK_IS_COLORMAP (colormap));
  g_return_if_fail (result != NULL);

  visual = colormap->visual;
  g_return_if_fail (visual != NULL);

  result->pixel = pixel;
  result->red   = colormap_channel_from_pixel (p, visual->red_mask,
					       visual->red_shift, visual->red_prec);
  result->green = colormap_channel_from_pixel (p, visual->green_mask,
					       visual->green_shift, visual->green_prec);
  result->blue  = colormap_channel_from_pixel (p, visual->blue_mask,
					       visual->blue_shift, visual->blue_prec);
}

/* Obsolete.  A TrueColor colormap has no writable cells; X11 returns TRUE
 * after a no-op XStoreColor error, but reporting failure is the honest
 * answer and callers only use this for palette visuals. */
gint
_gdk_wayland_color_change (GdkColormap *colormap, GdkColor *color)
{
  g_return_val_if_fail (GDK_IS_COLORMAP (colormap), FALSE);
  g_return_val_if_fail (color != NULL, FALSE);

  return FALSE;
}
