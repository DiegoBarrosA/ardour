/*
 * gdkcursor-wayland.c
 *
 * ydk-wayland fork: Wayland GdkCursor implementation.
 *
 * Counterpart of x11/gdkcursor-x11.c.  Every entry point is authored as
 * `_gdk_wayland_*`; ydk/gdkbackend.c owns the canonical public names and
 * forwards here when the Wayland backend is selected.
 *
 * Skeleton status (M4): a cursor only records the request (type and/or
 * theme name).  There is no Xlib and no wl_cursor_theme yet; M7 resolves the
 * stored type/name into a real wl_cursor image and attaches it to the
 * pointer surface.
 */

#include "config.h"

#include <string.h>
#include <errno.h>

#include <glib.h>

#include <ydk/gdk.h>
#include <ydk/gdkcursor.h>
#include <ydk-pixbuf/ydk-pixbuf.h>

#include "gdkprivate-wayland.h"

#ifdef GDK_COMPILATION
#include "gdkalias.h"
#endif

/* Everything a Wayland cursor remembers until M7 can realise it. */
typedef struct _GDKWCursorWayland GDKWCursorWayland;

struct _GDKWCursorWayland
{
  GdkCursor cursor;		/* type + ref_count; must be first */
  GdkDisplay *display;
  gchar *name;			/* theme name for named cursors, else NULL */
  gint hot_x;			/* hotspot of pixmap/pixbuf cursors */
  gint hot_y;
};

static GdkCursor *
cursor_alloc (GdkDisplay    *display,
	      GdkCursorType  type,
	      const gchar   *name,
	      gint           hot_x,
	      gint           hot_y)
{
  GDKWCursorWayland *private;

  private = g_new0 (GDKWCursorWayland, 1);
  private->display = display;
  private->name = g_strdup (name);
  private->hot_x = hot_x;
  private->hot_y = hot_y;

  private->cursor.type = type;
  private->cursor.ref_count = 1;

  return (GdkCursor *) private;
}

GdkCursor *
_gdk_wayland_cursor_new_for_display (GdkDisplay    *display,
				     GdkCursorType  cursor_type)
{
  g_return_val_if_fail (GDK_IS_DISPLAY (display), NULL);

  return cursor_alloc (display, cursor_type, NULL, 0, 0);
}

GdkCursor *
_gdk_wayland_cursor_new_from_pixmap (GdkPixmap      *source,
				     GdkPixmap      *mask,
				     const GdkColor *fg,
				     const GdkColor *bg,
				     gint            x,
				     gint            y)
{
  GdkDisplay *display;

  g_return_val_if_fail (GDK_IS_PIXMAP (source), NULL);
  g_return_val_if_fail (GDK_IS_PIXMAP (mask), NULL);
  g_return_val_if_fail (fg != NULL, NULL);
  g_return_val_if_fail (bg != NULL, NULL);

  display = gdk_drawable_get_display (GDK_DRAWABLE (source));

  /* Image data is dropped until M7. */
  return cursor_alloc (display, GDK_CURSOR_IS_PIXMAP, NULL, x, y);
}

GdkCursor *
_gdk_wayland_cursor_new_from_pixbuf (GdkDisplay *display,
				     GdkPixbuf  *pixbuf,
				     gint        x,
				     gint        y)
{
  const char *option;
  char *end;
  gint64 value;

  g_return_val_if_fail (GDK_IS_DISPLAY (display), NULL);
  g_return_val_if_fail (GDK_IS_PIXBUF (pixbuf), NULL);

  if (x == -1 && (option = gdk_pixbuf_get_option (pixbuf, "x_hot")))
    {
      errno = 0;
      end = NULL;
      value = g_ascii_strtoll (option, &end, 10);
      if (errno == 0 && end != option && value >= 0 && value < G_MAXINT)
	x = (gint) value;
    }
  if (y == -1 && (option = gdk_pixbuf_get_option (pixbuf, "y_hot")))
    {
      errno = 0;
      end = NULL;
      value = g_ascii_strtoll (option, &end, 10);
      if (errno == 0 && end != option && value >= 0 && value < G_MAXINT)
	y = (gint) value;
    }

  g_return_val_if_fail (0 <= x && x < gdk_pixbuf_get_width (pixbuf), NULL);
  g_return_val_if_fail (0 <= y && y < gdk_pixbuf_get_height (pixbuf), NULL);

  return cursor_alloc (display, GDK_CURSOR_IS_PIXMAP, NULL, x, y);
}

GdkCursor *
_gdk_wayland_cursor_new_from_name (GdkDisplay  *display,
				   const gchar *name)
{
  g_return_val_if_fail (GDK_IS_DISPLAY (display), NULL);
  g_return_val_if_fail (name != NULL, NULL);

  /* No theme to validate against yet: accept every name.  M7 returns NULL
   * for names the wl_cursor theme does not have, as GDK2 does. */
  return cursor_alloc (display, GDK_CURSOR_IS_PIXMAP, name, 0, 0);
}

void
_gdk_wayland_cursor_destroy (GdkCursor *cursor)
{
  GDKWCursorWayland *private;

  g_return_if_fail (cursor != NULL);
  g_return_if_fail (cursor->ref_count == 0);

  private = (GDKWCursorWayland *) cursor;

  g_free (private->name);
  g_free (private);
}

GdkDisplay *
_gdk_wayland_cursor_get_display (GdkCursor *cursor)
{
  g_return_val_if_fail (cursor != NULL, NULL);

  return ((GDKWCursorWayland *) cursor)->display;
}

GdkPixbuf *
_gdk_wayland_cursor_get_image (GdkCursor *cursor)
{
  g_return_val_if_fail (cursor != NULL, NULL);

  /* No image is available before M7. */
  return NULL;
}

gboolean
_gdk_wayland_display_supports_cursor_alpha (GdkDisplay *display)
{
  g_return_val_if_fail (GDK_IS_DISPLAY (display), FALSE);

  /* wl_cursor images are ARGB. */
  return TRUE;
}

gboolean
_gdk_wayland_display_supports_cursor_color (GdkDisplay *display)
{
  g_return_val_if_fail (GDK_IS_DISPLAY (display), FALSE);

  return TRUE;
}

guint
_gdk_wayland_display_get_default_cursor_size (GdkDisplay *display)
{
  g_return_val_if_fail (GDK_IS_DISPLAY (display), 0);

  /* Conventional default of cursor themes (XCURSOR_SIZE). */
  return 24;
}

void
_gdk_wayland_display_get_maximal_cursor_size (GdkDisplay *display,
					      guint      *width,
					      guint      *height)
{
  g_return_if_fail (GDK_IS_DISPLAY (display));

  /* Wayland imposes no limit; pick a sane ceiling matching X11's query. */
  if (width)
    *width = 128;
  if (height)
    *height = 128;
}
