/*
 * gdkselection-wayland.c
 *
 * ydk-wayland fork: Wayland backend counterpart of x11/gdkselection-x11.c,
 * namespaced `_gdk_wayland_*` so both backends can live in one libydk.
 * The canonical public names are owned by ydk/gdkbackend.c.
 *
 * Selection ownership and transfer are safe stubs until the real
 * wl_data_device integration lands in M8: no client ever owns a selection,
 * conversions are refused with a SelectionNotify(property=NONE), and property reads return no data.
 * The text-property helpers need no windowing system and are implemented
 * with plain GLib charset conversion (no Xlib, no COMPOUND_TEXT).
 */

#include "config.h"

#include <string.h>

#include <glib.h>

#include <ydk/gdk.h>
#include "gdkprivate-wayland.h"

#ifdef GDK_COMPILATION
#include "ydk/gdkalias.h"
#endif

gboolean
_gdk_wayland_selection_owner_set_for_display (GdkDisplay *display,
					      GdkWindow  *owner,
					      GdkAtom     selection,
					      guint32     time,
					      gboolean    send_event)
{
  g_return_val_if_fail (GDK_IS_DISPLAY (display), FALSE);
  g_return_val_if_fail (selection != GDK_NONE, FALSE);
  g_return_val_if_fail (owner == NULL || GDK_IS_WINDOW (owner), FALSE);

  /* M8: wl_data_source / wl_data_device_set_selection. */
  return FALSE;
}

GdkWindow *
_gdk_wayland_selection_owner_get_for_display (GdkDisplay *display,
					      GdkAtom     selection)
{
  g_return_val_if_fail (GDK_IS_DISPLAY (display), NULL);
  g_return_val_if_fail (selection != GDK_NONE, NULL);

  /* Wayland clients cannot discover other clients' selection owners. */
  return NULL;
}

void
_gdk_wayland_selection_convert (GdkWindow *requestor,
				GdkAtom    selection,
				GdkAtom    target,
				guint32    time)
{
  GdkEvent *event;

  g_return_if_fail (selection != GDK_NONE);
  g_return_if_fail (requestor == NULL || GDK_IS_WINDOW (requestor));

  if (!requestor)
    return;

  /* M8: wl_data_offer_receive.  Until then every conversion is refused:
   * deliver a SelectionNotify with property == GDK_NONE so a caller waiting
   * for the reply does not hang forever. */
  event = gdk_event_new (GDK_SELECTION_NOTIFY);
  event->selection.window = g_object_ref (requestor);
  event->selection.send_event = FALSE;
  event->selection.selection = selection;
  event->selection.target = target;
  event->selection.property = GDK_NONE;
  event->selection.time = time;
  event->selection.requestor = 0;

  gdk_event_put (event);
  gdk_event_free (event);
}

gint
_gdk_wayland_selection_property_get (GdkWindow  *requestor,
				     guchar    **data,
				     GdkAtom    *ret_type,
				     gint       *ret_format)
{
  g_return_val_if_fail (requestor != NULL, 0);
  g_return_val_if_fail (GDK_IS_WINDOW (requestor), 0);

  if (data)
    *data = NULL;
  if (ret_type)
    *ret_type = GDK_NONE;
  if (ret_format)
    *ret_format = 0;

  return 0;
}

void
_gdk_wayland_selection_send_notify_for_display (GdkDisplay      *display,
						GdkNativeWindow  requestor,
						GdkAtom          selection,
						GdkAtom          target,
						GdkAtom          property,
						guint32          time)
{
  g_return_if_fail (GDK_IS_DISPLAY (display));

  /* Nothing to answer: no SelectionRequest can arrive before M8. */
}

/* Split @text (NUL separated, @length bytes) into a NULL terminated vector
 * of strings, converting each from @from_charset to @to_charset.  Strings
 * that fail to convert are dropped.  Returns the number of strings. */
static gint
make_list (const gchar  *text,
	   gint          length,
	   const gchar  *from_charset,
	   const gchar  *to_charset,
	   gchar      ***list)
{
  GPtrArray *strings = g_ptr_array_new ();
  const gchar *p = text;
  gint n_strings;

  while (p < text + length)
    {
      const gchar *q = p;
      gchar *str;

      while (q < text + length && *q)
	q++;

      if (strcmp (from_charset, to_charset) == 0)
	{
	  str = g_strndup (p, q - p);
	  if (strcmp (to_charset, "UTF-8") == 0 &&
	      !g_utf8_validate (str, -1, NULL))
	    {
	      g_warning ("Error converting selection from UTF8_STRING");
	      g_free (str);
	      str = NULL;
	    }
	}
      else
	{
	  GError *error = NULL;

	  str = g_convert (p, q - p, to_charset, from_charset,
			   NULL, NULL, &error);
	  if (!str)
	    {
	      g_warning ("Error converting selection from '%s' to '%s': %s",
			 from_charset, to_charset, error->message);
	      g_error_free (error);
	    }
	}

      if (str)
	g_ptr_array_add (strings, str);

      p = q + 1;
    }

  n_strings = strings->len;

  if (list)
    {
      g_ptr_array_add (strings, NULL);
      *list = (gchar **) g_ptr_array_free (strings, FALSE);
    }
  else
    {
      g_ptr_array_foreach (strings, (GFunc) g_free, NULL);
      g_ptr_array_free (strings, TRUE);
    }

  return n_strings;
}

/* Returns the source charset for @encoding, or NULL if unsupported. */
static const gchar *
encoding_charset (GdkAtom encoding)
{
  if (encoding == GDK_TARGET_STRING)
    return "ISO-8859-1";
  else if (encoding == gdk_atom_intern_static_string ("UTF8_STRING"))
    return "UTF-8";

  return NULL;
}

gint
_gdk_wayland_text_property_to_text_list_for_display (GdkDisplay   *display,
						     GdkAtom       encoding,
						     gint          format,
						     const guchar *text,
						     gint          length,
						     gchar      ***list)
{
  const gchar *from_charset;
  const gchar *locale_charset = NULL;

  g_return_val_if_fail (GDK_IS_DISPLAY (display), 0);

  if (list)
    *list = NULL;

  /* COMPOUND_TEXT and other X encodings are not supported on Wayland. */
  from_charset = encoding_charset (encoding);
  if (!from_charset || !text || length <= 0)
    return 0;

  g_get_charset (&locale_charset);

  return make_list ((const gchar *) text, length,
		    from_charset, locale_charset, list);
}

gint
_gdk_wayland_text_property_to_utf8_list_for_display (GdkDisplay    *display,
						     GdkAtom        encoding,
						     gint           format,
						     const guchar  *text,
						     gint           length,
						     gchar       ***list)
{
  const gchar *from_charset;

  g_return_val_if_fail (text != NULL, 0);
  g_return_val_if_fail (length >= 0, 0);
  g_return_val_if_fail (GDK_IS_DISPLAY (display), 0);

  from_charset = encoding_charset (encoding);
  if (!from_charset)
    {
      if (list)
	*list = NULL;
      return 0;
    }

  return make_list ((const gchar *) text, length,
		    from_charset, "UTF-8", list);
}

void
_gdk_wayland_free_text_list (gchar **list)
{
  g_return_if_fail (list != NULL);

  g_strfreev (list);
}

void
_gdk_wayland_free_compound_text (guchar *ctext)
{
  g_free (ctext);
}

/* Strip C0/C1 controls other than \n and \t and canonicalize \r and \r\n
 * to \n, as the X11 backend does before producing text properties. */
static gchar *
sanitize_utf8 (const gchar *src)
{
  GString *result = g_string_sized_new (strlen (src));
  const gchar *p = src;

  while (*p)
    {
      if (*p == '\r')
	{
	  p++;
	  if (*p == '\n')
	    p++;

	  g_string_append_c (result, '\n');
	}
      else
	{
	  gunichar ch = g_utf8_get_char (p);

	  if (!((ch < 0x20 && ch != '\t' && ch != '\n') ||
		(ch >= 0x7f && ch < 0xa0)))
	    g_string_append_unichar (result, ch);

	  p = g_utf8_next_char (p);
	}
    }

  return g_string_free (result, FALSE);
}

static void
clear_ctext_out (GdkAtom *encoding,
		 gint    *format,
		 guchar **ctext,
		 gint    *length)
{
  if (encoding)
    *encoding = GDK_NONE;
  if (format)
    *format = 0;
  if (ctext)
    *ctext = NULL;
  if (length)
    *length = 0;
}

/* "Compound text" on Wayland is plain UTF-8 tagged UTF8_STRING: that is
 * what Wayland clients exchange (text/plain;charset=utf-8).  Returns 0 on
 * success, non-zero on failure, matching the X11 contract. */
gint
_gdk_wayland_string_to_compound_text_for_display (GdkDisplay  *display,
						  const gchar *str,
						  GdkAtom     *encoding,
						  gint        *format,
						  guchar     **ctext,
						  gint        *length)
{
  const gchar *charset = NULL;
  gchar *utf8, *clean;

  g_return_val_if_fail (GDK_IS_DISPLAY (display), 1);
  g_return_val_if_fail (str != NULL, 1);

  if (g_get_charset (&charset))
    utf8 = g_strdup (str);
  else
    utf8 = g_convert (str, -1, "UTF-8", charset, NULL, NULL, NULL);

  if (!utf8 || !g_utf8_validate (utf8, -1, NULL))
    {
      g_free (utf8);
      clear_ctext_out (encoding, format, ctext, length);
      return 1;
    }

  clean = sanitize_utf8 (utf8);
  g_free (utf8);

  if (encoding)
    *encoding = gdk_atom_intern_static_string ("UTF8_STRING");
  if (format)
    *format = 8;
  if (length)
    *length = strlen (clean);
  if (ctext)
    *ctext = (guchar *) clean;
  else
    g_free (clean);

  return 0;
}

gboolean
_gdk_wayland_utf8_to_compound_text_for_display (GdkDisplay  *display,
						const gchar *str,
						GdkAtom     *encoding,
						gint        *format,
						guchar     **ctext,
						gint        *length)
{
  gchar *clean;

  g_return_val_if_fail (str != NULL, FALSE);
  g_return_val_if_fail (GDK_IS_DISPLAY (display), FALSE);

  /* sanitize_utf8 steps through the string by character, so it must only
   * ever see valid UTF-8 (a truncated sequence would run past the NUL). */
  if (!g_utf8_validate (str, -1, NULL))
    {
      clear_ctext_out (encoding, format, ctext, length);
      return FALSE;
    }

  clean = sanitize_utf8 (str);

  if (encoding)
    *encoding = gdk_atom_intern_static_string ("UTF8_STRING");
  if (format)
    *format = 8;
  if (length)
    *length = strlen (clean);
  if (ctext)
    *ctext = (guchar *) clean;
  else
    g_free (clean);

  return TRUE;
}
