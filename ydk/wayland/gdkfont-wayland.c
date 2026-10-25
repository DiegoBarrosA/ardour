/*
 * gdkfont-wayland.c
 *
 * ydk-wayland fork: legacy GdkFont API for the Wayland backend.
 *
 * Wayland has no server-side fonts, so a GdkFont here is a thin wrapper
 * over a PangoFontDescription plus a PangoLayout (pangocairo, no Xlib).
 * Metrics are approximations: text is laid out with Pango and the ink and
 * logical extents are reported in the X text-extents convention.  XLFD
 * names passed to the loaders are parsed best-effort into a description;
 * anything else is handed to pango_font_description_from_string().
 *
 * Symbols are namespaced _gdk_wayland_font_*; the dispatcher in
 * ydk/gdkbackend.c owns the public gdk_font_* names.
 */

#undef GDK_DISABLE_DEPRECATED

#include "config.h"

#include <string.h>
#include <stdlib.h>

#include <pango/pangocairo.h>

#include <ydk/gdk.h>
#include <ydk/gdkfont.h>
#include <ydk/gdkinternals.h>

#include "gdkprivate-wayland.h"

#ifdef GDK_COMPILATION
#include "gdkalias.h"
#endif

typedef struct _GdkFontPrivateWayland GdkFontPrivateWayland;

struct _GdkFontPrivateWayland
{
  GdkFontPrivate base;
  GdkDisplay *display;
  PangoFontDescription *desc;
  PangoLayout *layout;
  gchar *name;			/* name used to load, for the cache */
};

static GHashTable *
font_hash_get (GdkDisplay *display,
	       GdkFontType type)
{
  GHashTable *result;
  static GQuark font_quark = 0;
  static GQuark fontset_quark = 0;
  GQuark quark;

  if (!font_quark)
    {
      font_quark = g_quark_from_static_string ("gdk-wayland-font-hash");
      fontset_quark = g_quark_from_static_string ("gdk-wayland-fontset-hash");
    }
  quark = (type == GDK_FONT_FONT) ? font_quark : fontset_quark;

  result = g_object_get_qdata (G_OBJECT (display), quark);
  if (!result)
    {
      result = g_hash_table_new (g_str_hash, g_str_equal);
      g_object_set_qdata_full (G_OBJECT (display), quark, result,
			       (GDestroyNotify) g_hash_table_destroy);
    }

  return result;
}

static GdkFont *
font_hash_lookup (GdkDisplay  *display,
		  GdkFontType  type,
		  const gchar *name)
{
  GdkFont *result = g_hash_table_lookup (font_hash_get (display, type), name);

  if (result)
    gdk_font_ref (result);

  return result;
}

/* Parse field N (0 based, after the leading '-') of an XLFD.  Returns a
 * newly allocated string, or NULL if absent or a wildcard. */
static gchar *
xlfd_field (const gchar *xlfd,
	    gint         n)
{
  gchar **parts;
  gchar *result = NULL;

  parts = g_strsplit (xlfd + 1, "-", -1);
  if (g_strv_length (parts) > (guint) n &&
      parts[n][0] != '\0' &&
      strcmp (parts[n], "*") != 0)
    result = g_strdup (parts[n]);
  g_strfreev (parts);

  return result;
}

static PangoFontDescription *
font_description_from_name (const gchar *name)
{
  PangoFontDescription *desc;

  if (name[0] != '-')
    return pango_font_description_from_string (name);

  /* -foundry-family-weight-slant-setwidth-addstyle-pixel-point-resx-resy-
   *  spacing-avgwidth-registry-encoding */
  {
    gchar *family = xlfd_field (name, 1);
    gchar *weight = xlfd_field (name, 2);
    gchar *slant = xlfd_field (name, 3);
    gchar *pixel = xlfd_field (name, 6);
    gchar *point = xlfd_field (name, 7);

    desc = pango_font_description_new ();
    pango_font_description_set_family (desc, family ? family : "sans");

    if (weight && g_ascii_strcasecmp (weight, "bold") == 0)
      pango_font_description_set_weight (desc, PANGO_WEIGHT_BOLD);
    if (slant && (g_ascii_strcasecmp (slant, "i") == 0 ||
		  g_ascii_strcasecmp (slant, "o") == 0))
      pango_font_description_set_style (desc, slant[0] == 'i' || slant[0] == 'I'
					? PANGO_STYLE_ITALIC
					: PANGO_STYLE_OBLIQUE);

    if (point && atoi (point) > 0)
      pango_font_description_set_size (desc, atoi (point) * PANGO_SCALE / 10);
    else if (pixel && atoi (pixel) > 0)
      pango_font_description_set_absolute_size (desc, atoi (pixel) * PANGO_SCALE);
    else
      pango_font_description_set_size (desc, 12 * PANGO_SCALE);

    g_free (family);
    g_free (weight);
    g_free (slant);
    g_free (pixel);
    g_free (point);
  }

  return desc;
}

static GdkFont *
font_new (GdkDisplay                 *display,
	  GdkFontType                 type,
	  const PangoFontDescription *desc,
	  const gchar                *name)
{
  GdkFontPrivateWayland *private;
  PangoContext *context;
  PangoFontMetrics *metrics;
  PangoFont *pfont;
  GdkFont *font;

  private = g_new0 (GdkFontPrivateWayland, 1);
  private->base.ref_count = 1;
  private->display = display;
  private->desc = pango_font_description_copy (desc);

  context = pango_font_map_create_context (pango_cairo_font_map_get_default ());
  private->layout = pango_layout_new (context);
  g_object_unref (context);
  pango_layout_set_font_description (private->layout, private->desc);

  font = (GdkFont *) private;
  font->type = type;
  font->ascent = 0;
  font->descent = 0;

  pfont = pango_context_load_font (pango_layout_get_context (private->layout),
				   private->desc);
  if (pfont)
    {
      metrics = pango_font_get_metrics (pfont, NULL);
      font->ascent = PANGO_PIXELS (pango_font_metrics_get_ascent (metrics));
      font->descent = PANGO_PIXELS (pango_font_metrics_get_descent (metrics));
      pango_font_metrics_unref (metrics);
      g_object_unref (pfont);
    }
  else
    {
      /* No such face at all: fail like XLoadQueryFont would. */
      g_object_unref (private->layout);
      pango_font_description_free (private->desc);
      g_free (private);
      return NULL;
    }

  if (name)
    {
      private->name = g_strdup (name);
      g_hash_table_insert (font_hash_get (display, type), private->name, font);
    }

  return font;
}

/**
 * _gdk_wayland_font_get_display:
 * @font: a #GdkFont.
 *
 * Returns the #GdkDisplay for @font.
 */
GdkDisplay *
_gdk_wayland_font_get_display (GdkFont *font)
{
  g_return_val_if_fail (font != NULL, NULL);

  return ((GdkFontPrivateWayland *) font)->display;
}

GdkFont *
_gdk_wayland_font_load_for_display (GdkDisplay  *display,
				    const gchar *font_name)
{
  GdkFont *font;
  PangoFontDescription *desc;

  g_return_val_if_fail (GDK_IS_DISPLAY (display), NULL);
  g_return_val_if_fail (font_name != NULL, NULL);

  font = font_hash_lookup (display, GDK_FONT_FONT, font_name);
  if (font)
    return font;

  desc = font_description_from_name (font_name);
  font = font_new (display, GDK_FONT_FONT, desc, font_name);
  pango_font_description_free (desc);

  return font;
}

GdkFont *
_gdk_wayland_font_from_description_for_display (GdkDisplay           *display,
						PangoFontDescription *font_desc)
{
  GdkFont *font;
  gchar *name;

  g_return_val_if_fail (GDK_IS_DISPLAY (display), NULL);
  g_return_val_if_fail (font_desc != NULL, NULL);

  name = pango_font_description_to_string (font_desc);
  font = font_hash_lookup (display, GDK_FONT_FONT, name);
  if (!font)
    font = font_new (display, GDK_FONT_FONT, font_desc, name);
  g_free (name);

  return font;
}

GdkFont *
_gdk_wayland_fontset_load_for_display (GdkDisplay  *display,
				       const gchar *fontset_name)
{
  GdkFont *font;
  PangoFontDescription *desc;
  gchar **names;

  g_return_val_if_fail (GDK_IS_DISPLAY (display), NULL);
  g_return_val_if_fail (fontset_name != NULL, NULL);

  font = font_hash_lookup (display, GDK_FONT_FONTSET, fontset_name);
  if (font)
    return font;

  /* Pango does its own fallback, so only the first component matters. */
  names = g_strsplit (fontset_name, ",", 2);
  desc = font_description_from_name (names[0] ? names[0] : "sans");
  g_strfreev (names);

  font = font_new (display, GDK_FONT_FONTSET, desc, fontset_name);
  pango_font_description_free (desc);

  return font;
}

void
_gdk_wayland_font_destroy (GdkFont *font)
{
  GdkFontPrivateWayland *private = (GdkFontPrivateWayland *) font;

  if (private->name)
    {
      g_hash_table_remove (font_hash_get (private->display, font->type),
			   private->name);
      g_free (private->name);
    }

  g_object_unref (private->layout);
  pango_font_description_free (private->desc);
  g_free (font);
}

gint
_gdk_wayland_font_strlen (GdkFont     *font,
			  const gchar *str)
{
  g_return_val_if_fail (font != NULL, -1);
  g_return_val_if_fail (str != NULL, -1);

  return strlen (str);
}

gint
_gdk_wayland_font_id (const GdkFont *font)
{
  const GdkFontPrivateWayland *private;
  gchar *str;
  gint id;

  g_return_val_if_fail (font != NULL, 0);

  if (font->type != GDK_FONT_FONT)
    return 0;

  private = (const GdkFontPrivateWayland *) font;
  str = pango_font_description_to_string (private->desc);
  id = (gint) g_str_hash (str);
  g_free (str);

  return id;
}

gboolean
_gdk_wayland_font_equal (const GdkFont *fonta,
			 const GdkFont *fontb)
{
  const GdkFontPrivateWayland *a, *b;

  g_return_val_if_fail (fonta != NULL, FALSE);
  g_return_val_if_fail (fontb != NULL, FALSE);

  if (fonta->type != fontb->type)
    return FALSE;

  a = (const GdkFontPrivateWayland *) fonta;
  b = (const GdkFontPrivateWayland *) fontb;

  return pango_font_description_equal (a->desc, b->desc);
}

/* Convert legacy 8-bit/locale text to UTF-8.  Never fails; returns a
 * newly allocated string. */
static gchar *
text_to_utf8 (const gchar *text,
	      gint         text_length)
{
  gchar *copy, *result;

  if (text_length < 0)
    text_length = strlen (text);

  copy = g_strndup (text, text_length);
  if (g_utf8_validate (copy, -1, NULL))
    return copy;

  result = g_locale_to_utf8 (copy, -1, NULL, NULL, NULL);
  if (!result)
    result = g_convert_with_fallback (copy, -1, "UTF-8", "ISO-8859-1", "?",
				      NULL, NULL, NULL);
  g_free (copy);

  return result ? result : g_strdup ("");
}

static gchar *
wc_to_utf8 (const GdkWChar *text,
	    gint            text_length)
{
  GString *str = g_string_new (NULL);
  gint i;

  for (i = 0; i < text_length; i++)
    if (g_unichar_validate ((gunichar) text[i]))
      g_string_append_unichar (str, (gunichar) text[i]);

  return g_string_free (str, FALSE);
}

static void
measure (GdkFont     *font,
	 const gchar *utf8,
	 PangoRectangle *ink,
	 PangoRectangle *logical)
{
  GdkFontPrivateWayland *private = (GdkFontPrivateWayland *) font;
  PangoLayoutLine *line;

  pango_layout_set_text (private->layout, utf8, -1);
  line = pango_layout_get_line_readonly (private->layout, 0);
  if (line)
    pango_layout_line_get_pixel_extents (line, ink, logical);
  else
    {
      memset (ink, 0, sizeof (*ink));
      memset (logical, 0, sizeof (*logical));
    }
}

static void
extents_utf8 (GdkFont     *font,
	      const gchar *utf8,
	      gint        *lbearing,
	      gint        *rbearing,
	      gint        *width,
	      gint        *ascent,
	      gint        *descent)
{
  PangoRectangle ink, logical;

  measure (font, utf8, &ink, &logical);

  if (lbearing)
    *lbearing = ink.x;
  if (rbearing)
    *rbearing = ink.x + ink.width;
  if (width)
    *width = logical.width;
  if (ascent)
    *ascent = -ink.y;
  if (descent)
    *descent = ink.y + ink.height;
}

gint
_gdk_wayland_text_width (GdkFont     *font,
			 const gchar *text,
			 gint         text_length)
{
  gchar *utf8;
  gint width;

  g_return_val_if_fail (font != NULL, -1);
  g_return_val_if_fail (text != NULL, -1);

  utf8 = text_to_utf8 (text, text_length);
  extents_utf8 (font, utf8, NULL, NULL, &width, NULL, NULL);
  g_free (utf8);

  return width;
}

gint
_gdk_wayland_text_width_wc (GdkFont        *font,
			    const GdkWChar *text,
			    gint            text_length)
{
  gchar *utf8;
  gint width;

  g_return_val_if_fail (font != NULL, -1);
  g_return_val_if_fail (text != NULL, -1);

  utf8 = wc_to_utf8 (text, text_length);
  extents_utf8 (font, utf8, NULL, NULL, &width, NULL, NULL);
  g_free (utf8);

  return width;
}

void
_gdk_wayland_text_extents (GdkFont     *font,
			   const gchar *text,
			   gint         text_length,
			   gint        *lbearing,
			   gint        *rbearing,
			   gint        *width,
			   gint        *ascent,
			   gint        *descent)
{
  gchar *utf8;

  g_return_if_fail (font != NULL);
  g_return_if_fail (text != NULL);

  utf8 = text_to_utf8 (text, text_length);
  extents_utf8 (font, utf8, lbearing, rbearing, width, ascent, descent);
  g_free (utf8);
}

void
_gdk_wayland_text_extents_wc (GdkFont        *font,
			      const GdkWChar *text,
			      gint            text_length,
			      gint           *lbearing,
			      gint           *rbearing,
			      gint           *width,
			      gint           *ascent,
			      gint           *descent)
{
  gchar *utf8;

  g_return_if_fail (font != NULL);
  g_return_if_fail (text != NULL);

  utf8 = wc_to_utf8 (text, text_length);
  extents_utf8 (font, utf8, lbearing, rbearing, width, ascent, descent);
  g_free (utf8);
}

