/*
 * gdkcursor-wayland.c
 *
 * ydk-wayland fork: Wayland GdkCursor implementation (M7).
 *
 * Counterpart of x11/gdkcursor-x11.c.  Every entry point is authored as
 * `_gdk_wayland_*`; ydk/gdkbackend.c owns the canonical public names and
 * forwards here when the Wayland backend is selected.
 *
 * The decisions (theme loading, name table, aliases, when set_cursor is
 * sent, custom cursors) are in the header comment of ydk/gdkm7-wayland.h.
 * This file owns all cursor state: one GDKWCursorState per display (qdata),
 * one GDKWCursor per GdkCursor, and the remembered cursor of every window
 * impl (qdata on the impl).
 */

#include "config.h"

#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>

#include <glib.h>

#include <wayland-client.h>
#include <wayland-cursor.h>

#include <ydk/gdk.h>
#include <ydk/gdkcursor.h>
#include <ydk/gdkcairo.h>
#include <ydk/gdkinternals.h>
#include <ydk-pixbuf/ydk-pixbuf.h>

#include "gdkprivate-wayland.h"
#include "gdkseat-wayland.h"
#include "gdkpixmap-wayland.h"
#include "gdkm7-wayland.h"
#include "gdkm10-wayland.h"

/* declared only under GDK_PIXBUF_ENABLE_BACKEND, but part of the library */
gboolean gdk_pixbuf_set_option (GdkPixbuf *pixbuf, const gchar *key,
				const gchar *value);

#ifdef GDK_COMPILATION
#include "gdkalias.h"
#endif

/* ------------------------------------------------------------------------
 * Name table and aliases
 * ------------------------------------------------------------------------ */

#define N_CURSOR_NAMES 77
#define MAX_CANDIDATES 8

static const gchar *const cursor_names[N_CURSOR_NAMES] = {
  "X_cursor", "arrow", "based_arrow_down",
  "based_arrow_up", "boat", "bogosity",
  "bottom_left_corner", "bottom_right_corner", "bottom_side",
  "bottom_tee", "box_spiral", "center_ptr",
  "circle", "clock", "coffee_mug",
  "cross", "cross_reverse", "crosshair",
  "diamond_cross", "dot", "dotbox",
  "double_arrow", "draft_large", "draft_small",
  "draped_box", "exchange", "fleur",
  "gobbler", "gumby", "hand1",
  "hand2", "heart", "icon",
  "iron_cross", "left_ptr", "left_side",
  "left_tee", "leftbutton", "ll_angle",
  "lr_angle", "man", "middlebutton",
  "mouse", "pencil", "pirate",
  "plus", "question_arrow", "right_ptr",
  "right_side", "right_tee", "rightbutton",
  "rtl_logo", "sailboat", "sb_down_arrow",
  "sb_h_double_arrow", "sb_left_arrow", "sb_right_arrow",
  "sb_up_arrow", "sb_v_double_arrow", "shuttle",
  "sizing", "spider", "spraycan",
  "star", "target", "tcross",
  "top_left_arrow", "top_left_corner", "top_right_corner",
  "top_side", "top_tee", "trek",
  "ul_angle", "umbrella", "ur_angle",
  "watch", "xterm"
};

/* Alternatives after the primary name; each row is NULL terminated. */
static const gchar *const alias_rows[][MAX_CANDIDATES] = {
  { "left_ptr", "default", "arrow", NULL },
  { "arrow", "right_ptr", "default", NULL },
  { "top_left_arrow", "left_ptr", "default", NULL },
  { "xterm", "text", "ibeam", NULL },
  { "watch", "wait", "progress", NULL },
  { "hand1", "pointer", "hand2", NULL },
  { "hand2", "pointer", "hand1", NULL },
  { "crosshair", "cross", "tcross", NULL },
  { "cross", "crosshair", NULL },
  { "tcross", "crosshair", "cross", NULL },
  { "fleur", "move", "all-scroll", "size_all", NULL },
  { "sb_h_double_arrow", "ew-resize", "h_double_arrow", "col-resize",
    "split_h", "size_hor", NULL },
  { "sb_v_double_arrow", "ns-resize", "v_double_arrow", "row-resize",
    "split_v", "size_ver", NULL },
  { "double_arrow", "sb_v_double_arrow", "ns-resize", NULL },
  { "top_left_corner", "nw-resize", "size_fdiag", NULL },
  { "top_right_corner", "ne-resize", "size_bdiag", NULL },
  { "bottom_left_corner", "sw-resize", "size_bdiag", NULL },
  { "bottom_right_corner", "se-resize", "size_fdiag", NULL },
  { "top_side", "n-resize", NULL },
  { "bottom_side", "s-resize", NULL },
  { "left_side", "w-resize", NULL },
  { "right_side", "e-resize", NULL },
  { "question_arrow", "help", "whats_this", NULL },
  { "X_cursor", "crossed_circle", NULL }
};

static const gchar *candidate_lists[N_CURSOR_NAMES][MAX_CANDIDATES + 1];
static gsize candidate_lists_once = 0;

static gint
primary_index (const gchar *name)
{
  gint i;

  for (i = 0; i < N_CURSOR_NAMES; i++)
    if (strcmp (cursor_names[i], name) == 0)
      return i;
  return -1;
}

static void
candidate_lists_init (void)
{
  if (g_once_init_enter (&candidate_lists_once))
    {
      guint i, j;

      for (i = 0; i < N_CURSOR_NAMES; i++)
	candidate_lists[i][0] = cursor_names[i];

      for (i = 0; i < G_N_ELEMENTS (alias_rows); i++)
	{
	  gint idx = primary_index (alias_rows[i][0]);

	  g_assert (idx >= 0);
	  for (j = 1; alias_rows[i][j] != NULL; j++)
	    candidate_lists[idx][j] = alias_rows[i][j];
	}
      g_once_init_leave (&candidate_lists_once, 1);
    }
}

const gchar *
_gdk_wayland_cursor_type_to_name (GdkCursorType type)
{
  gint v = (gint) type;

  if (v < 0 || v > 152 || (v & 1))
    return NULL;
  return cursor_names[v / 2];
}

const gchar * const *
_gdk_wayland_cursor_name_candidates (GdkCursorType type)
{
  gint v = (gint) type;

  if (v < 0 || v > 152 || (v & 1))
    return NULL;
  candidate_lists_init ();
  return candidate_lists[v / 2];
}

/* ------------------------------------------------------------------------
 * State
 * ------------------------------------------------------------------------ */

/* GDKWCursorState plus what this file needs on top of the recommended
 * layout.  base is first, so a GDKWCursorState * is also a CursorPriv *. */
typedef struct
{
  GDKWCursorState base;
  gboolean        applied_valid;   /* dedupe record is valid (not after leave) */
  gint            applied_hot_x, applied_hot_y;
  guchar         *applied_map;     /* pixels of the last CUSTOM / FALLBACK */
  gboolean        theme_disabled;  /* test hook */
  gchar          *search_path;     /* XCURSOR_PATH given to the reload hook */
  gboolean        fallback_logged;
  gint            loaded_scale;    /* M10: integer scale the theme was loaded at */
  gint            applied_bs;      /* M10: buffer scale of the last application */
} CursorPriv;

/* A resolved image, ready to attach. */
typedef struct
{
  GDKWTestCursorKind kind;
  struct wl_buffer  *buffer;
  guchar            *map;
  gint               width, height;
  gint               hot_x, hot_y;
  gint               buffer_scale;   /* M10: theme = loaded_scale, rest = 1 */
  gchar              name[64];
} Resolved;

static void
cursor_state_free (gpointer data)
{
  CursorPriv *p = data;

  /* No wl call here: the objects were destroyed by ..._display_finalize ()
   * while the connection was alive. */
  g_free (p->base.theme_name);
  g_free (p->search_path);
  g_slist_free (p->base.custom);
  g_free (p);
}

static CursorPriv *
cursor_priv_peek (GdkDisplay *display)
{
  if (display == NULL || !GDKW_IS_DISPLAY_WAYLAND (display))
    return NULL;
  return g_object_get_data (G_OBJECT (display), GDKW_CURSOR_STATE_DATA_KEY);
}

GDKWCursorState *
_gdk_wayland_display_get_cursor_state (GdkDisplay *display)
{
  CursorPriv *p;

  if (display == NULL || !GDKW_IS_DISPLAY_WAYLAND (display))
    return NULL;

  p = cursor_priv_peek (display);
  if (p == NULL)
    {
      const gchar *env;
      gint64 size = GDKW_CURSOR_DEFAULT_SIZE;

      p = g_new0 (CursorPriv, 1);
      p->base.display = display;

      env = g_getenv ("XCURSOR_THEME");
      if (env != NULL && *env != '\0')
	p->base.theme_name = g_strdup (env);

      env = g_getenv ("XCURSOR_SIZE");
      if (env != NULL && *env != '\0')
	{
	  gchar *end = NULL;
	  gint64 v = g_ascii_strtoll (env, &end, 10);

	  if (end != env && *end == '\0' && v >= 1 && v <= 512)
	    size = v;
	}
      p->base.theme_size = (gint) size;

      g_object_set_data_full (G_OBJECT (display), GDKW_CURSOR_STATE_DATA_KEY,
			      p, cursor_state_free);
    }

  return &p->base;
}

static CursorPriv *
cursor_priv_get (GdkDisplay *display)
{
  return (CursorPriv *) _gdk_wayland_display_get_cursor_state (display);
}

/* ------------------------------------------------------------------------
 * shm buffers
 * ------------------------------------------------------------------------ */

static struct wl_buffer *
shm_buffer_new (GDKWDisplayWayland *wd,
		gint                width,
		gint                height,
		guchar            **map_out,
		gsize              *size_out)
{
  struct wl_shm_pool *pool;
  struct wl_buffer *buffer;
  gsize stride, size;
  guchar *map;
  int fd;

  *map_out = NULL;
  *size_out = 0;

  if (wd == NULL || wd->wl_shm == NULL || width <= 0 || height <= 0)
    return NULL;

  stride = (gsize) width * 4;
  size = stride * (gsize) height;

  fd = memfd_create ("ydk-wayland-cursor", MFD_CLOEXEC);
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

  pool = wl_shm_create_pool (wd->wl_shm, fd, size);
  buffer = wl_shm_pool_create_buffer (pool, 0, width, height, stride,
				      WL_SHM_FORMAT_ARGB8888);
  wl_shm_pool_destroy (pool);
  close (fd);

  *map_out = map;
  *size_out = size;
  return buffer;
}

/* Gives @c its wl_buffer, filled from c->image.  FALSE on failure. */
static gboolean
custom_buffer_ensure (CursorPriv *p, GDKWCursor *c)
{
  GDKWDisplayWayland *wd;
  guchar *map;
  gsize size;
  const guchar *src;
  gint y, stride;

  if (c->buffer != NULL)
    return TRUE;
  if (p->base.closed || c->image == NULL)
    return FALSE;

  wd = GDKW_DISPLAY_WAYLAND (p->base.display);
  c->buffer = shm_buffer_new (wd, c->width, c->height, &map, &size);
  if (c->buffer == NULL)
    return FALSE;

  cairo_surface_flush (c->image);
  src = cairo_image_surface_get_data (c->image);
  stride = cairo_image_surface_get_stride (c->image);
  for (y = 0; y < c->height; y++)
    memcpy (map + (gsize) y * c->width * 4, src + (gsize) y * stride,
	    (gsize) c->width * 4);

  c->map = map;
  c->map_size = size;
  p->base.custom = g_slist_prepend (p->base.custom, c);
  return TRUE;
}

/* Releases the wl_buffer of @c.  Only while the display is alive. */
static void
custom_buffer_release (CursorPriv *p, GDKWCursor *c)
{
  if (c->buffer == NULL)
    return;

  if (p->base.applied_buffer == c->buffer)
    {
      p->base.applied_buffer = NULL;
      p->applied_valid = FALSE;
    }
  if (p->applied_map == c->map)
    p->applied_map = NULL;

  wl_buffer_destroy (c->buffer);
  c->buffer = NULL;
  if (c->map != NULL)
    munmap (c->map, c->map_size);
  c->map = NULL;
  c->map_size = 0;
  p->base.custom = g_slist_remove (p->base.custom, c);
}

/* ------------------------------------------------------------------------
 * Theme
 * ------------------------------------------------------------------------ */

static void
theme_destroy (CursorPriv *p)
{
  if (p->base.theme != NULL)
    {
      wl_cursor_theme_destroy (p->base.theme);
      p->base.theme = NULL;
    }
  if (p->base.applied.kind == GDKW_TEST_CURSOR_THEME)
    {
      p->base.applied_buffer = NULL;
      p->applied_valid = FALSE;
    }
  p->base.theme_loaded = FALSE;
  p->base.theme_usable = FALSE;
}

static void
theme_usable_update (CursorPriv *p)
{
  p->base.theme_usable =
    p->base.theme != NULL &&
    (wl_cursor_theme_get_cursor (p->base.theme, GDKW_CURSOR_DEFAULT_NAME) != NULL ||
     wl_cursor_theme_get_cursor (p->base.theme, "default") != NULL);
}

/* Loads now.  @search_path, if not NULL, is XCURSOR_PATH during the load. */
static void
theme_load (CursorPriv *p, const gchar *search_path)
{
  GDKWDisplayWayland *wd = GDKW_DISPLAY_WAYLAND (p->base.display);
  gchar *saved = NULL;
  gboolean had = FALSE;

  theme_destroy (p);
  p->base.theme_loaded = TRUE;

  if (p->theme_disabled || p->base.closed || wd->wl_shm == NULL)
    {
      p->base.theme_usable = FALSE;
      return;
    }

  if (search_path != NULL)
    {
      const gchar *old = g_getenv ("XCURSOR_PATH");

      had = (old != NULL);
      saved = g_strdup (old);
      g_setenv ("XCURSOR_PATH", search_path, TRUE);
    }

  /* M10 D13: the pixel size is theme_size * the integer scale of the pointer
   * focus window; set_buffer_scale and the hotspot divide it out again */
  p->loaded_scale =
    MAX (1, _gdk_wayland_scale_cursor_scale (p->base.display));
  p->base.theme = wl_cursor_theme_load (p->base.theme_name,
					p->base.theme_size * p->loaded_scale,
					wd->wl_shm);

  if (search_path != NULL)
    {
      if (had)
	g_setenv ("XCURSOR_PATH", saved, TRUE);
      else
	g_unsetenv ("XCURSOR_PATH");
      g_free (saved);
    }

  theme_usable_update (p);
}

static void
theme_ensure (CursorPriv *p)
{
  if (!p->base.theme_loaded)
    theme_load (p, NULL);
}

/* ------------------------------------------------------------------------
 * Resolution
 * ------------------------------------------------------------------------ */

static gboolean
theme_find (CursorPriv         *p,
	    const gchar * const *cands,
	    struct wl_cursor   **out,
	    const gchar        **name_out)
{
  struct wl_cursor *wc;
  gint i;

  if (p->base.theme == NULL)
    return FALSE;

  for (i = 0; cands != NULL && cands[i] != NULL; i++)
    {
      wc = wl_cursor_theme_get_cursor (p->base.theme, cands[i]);
      if (wc != NULL && wc->image_count > 0)
	{
	  *out = wc;
	  *name_out = cands[i];
	  return TRUE;
	}
    }
  return FALSE;
}

static void
resolve_fallback (CursorPriv *p, Resolved *r)
{
  GDKWDisplayWayland *wd = GDKW_DISPLAY_WAYLAND (p->base.display);
  gsize size;

  if (p->base.fallback_buffer == NULL)
    p->base.fallback_buffer = shm_buffer_new (wd, 1, 1, &p->base.fallback_map,
					      &size);
  if (!p->fallback_logged)
    {
      p->fallback_logged = TRUE;
      g_debug ("ydk-wayland: no usable cursor theme, the pointer is invisible");
    }

  r->kind = GDKW_TEST_CURSOR_FALLBACK;
  r->buffer = p->base.fallback_buffer;
  r->map = p->base.fallback_map;
  r->width = r->height = 1;
  r->hot_x = r->hot_y = 0;
  r->buffer_scale = 1;
  r->name[0] = '\0';
}

static void
resolve (CursorPriv *p, GdkCursor *cursor, Resolved *r)
{
  GDKWCursor *c = (GDKWCursor *) cursor;
  const gchar * const *cands = NULL;
  const gchar *single[2] = { NULL, NULL };
  struct wl_cursor *wc = NULL;
  const gchar *matched = NULL;
  struct wl_cursor_image *img;

  memset (r, 0, sizeof *r);

  /* 1. blank */
  if (cursor != NULL && cursor->type == GDK_BLANK_CURSOR)
    {
      r->kind = GDKW_TEST_CURSOR_HIDDEN;
      return;
    }

  /* 2. image data */
  if (cursor != NULL && c->image != NULL)
    {
      if (custom_buffer_ensure (p, c))
	{
	  r->kind = GDKW_TEST_CURSOR_CUSTOM;
	  r->buffer = c->buffer;
	  r->map = c->map;
	  r->width = c->width;
	  r->height = c->height;
	  r->hot_x = c->hot_x;
	  r->hot_y = c->hot_y;
	  r->buffer_scale = 1;
	  return;
	}
      resolve_fallback (p, r);
      return;
    }

  theme_ensure (p);

  /* 3. typed or named */
  if (cursor == NULL)
    cands = _gdk_wayland_cursor_name_candidates (GDK_LEFT_PTR);
  else if (c->name != NULL)
    {
      gint idx = primary_index (c->name);

      if (idx >= 0)
	cands = _gdk_wayland_cursor_name_candidates ((GdkCursorType) (idx * 2));
      else
	{
	  single[0] = c->name;
	  cands = single;
	}
    }
  else
    cands = _gdk_wayland_cursor_name_candidates (cursor->type);

  if (!theme_find (p, cands, &wc, &matched))
    {
      /* 4. theme usable but nothing matched */
      const gchar *def[3] = { GDKW_CURSOR_DEFAULT_NAME, "default", NULL };

      if (!p->base.theme_usable || !theme_find (p, def, &wc, &matched))
	{
	  /* 5. nothing */
	  resolve_fallback (p, r);
	  return;
	}
    }

  img = wc->images[0];
  r->buffer = wl_cursor_image_get_buffer (img);
  if (r->buffer == NULL)
    {
      resolve_fallback (p, r);
      return;
    }
  r->kind = GDKW_TEST_CURSOR_THEME;
  r->width = img->width;
  r->height = img->height;
  r->hot_x = img->hotspot_x;
  r->hot_y = img->hotspot_y;
  r->buffer_scale = MAX (1, p->loaded_scale);
  g_strlcpy (r->name, matched, sizeof r->name);
}

/* ------------------------------------------------------------------------
 * Application
 * ------------------------------------------------------------------------ */

static void
apply_cursor (CursorPriv *p, GdkCursor *cursor, guint32 serial)
{
  GDKWDisplayWayland *wd;
  GDKWCursorState *s = &p->base;
  Resolved r;
  gint sent_hot_x, sent_hot_y;

  if (s->closed)
    return;
  wd = GDKW_DISPLAY_WAYLAND (s->display);

  resolve (p, cursor, &r);
  if (r.buffer_scale < 1)
    r.buffer_scale = 1;

  if (p->applied_valid &&
      s->applied.kind == r.kind &&
      s->applied_buffer == r.buffer &&
      p->applied_bs == r.buffer_scale &&
      p->applied_hot_x == r.hot_x && p->applied_hot_y == r.hot_y &&
      s->applied.serial == serial)
    {
      s->applied.cursor = cursor;
      return;
    }

  /* M10 D13: the hotspot is in surface-local (logical) units */
  sent_hot_x = (r.hot_x + r.buffer_scale / 2) / r.buffer_scale;
  sent_hot_y = (r.hot_y + r.buffer_scale / 2) / r.buffer_scale;

  if (r.kind == GDKW_TEST_CURSOR_HIDDEN)
    {
      if (wd->wl_pointer != NULL)
	wl_pointer_set_cursor (wd->wl_pointer, serial, NULL, 0, 0);
    }
  else
    {
      if (s->surface == NULL && wd->wl_compositor != NULL)
	s->surface = wl_compositor_create_surface (wd->wl_compositor);

      if (s->surface != NULL && r.buffer != NULL)
	{
	  /* buffer scale needs wl_surface v3; the compositor is bound at
	   * MIN (version, 6) */
	  if (wl_proxy_get_version ((struct wl_proxy *) s->surface) >= 3)
	    wl_surface_set_buffer_scale (s->surface, r.buffer_scale);
	  wl_surface_attach (s->surface, r.buffer, 0, 0);
	  wl_surface_damage (s->surface, 0, 0,
			     r.width / r.buffer_scale,
			     r.height / r.buffer_scale);
	  wl_surface_commit (s->surface);
	}
      if (wd->wl_pointer != NULL && s->surface != NULL)
	wl_pointer_set_cursor (wd->wl_pointer, serial, s->surface,
			       sent_hot_x, sent_hot_y);
    }

  s->applied.set_count++;
  s->applied.serial = serial;
  s->applied.kind = r.kind;
  g_strlcpy (s->applied.name, r.name, sizeof s->applied.name);
  s->applied.width = r.width;
  s->applied.height = r.height;
  s->applied.hot_x = r.hot_x;
  s->applied.hot_y = r.hot_y;
  s->applied.to_compositor = (wd->wl_pointer != NULL);
  s->applied.cursor = cursor;
  s->applied_buffer = r.buffer;
  p->applied_hot_x = r.hot_x;
  p->applied_hot_y = r.hot_y;
  p->applied_bs = r.buffer_scale;
  _gdk_wayland_scale_note_cursor (s->display, r.buffer_scale,
				  r.width, r.height, sent_hot_x, sent_hot_y,
				  s->theme_size * MAX (1, p->loaded_scale));
  p->applied_map = (r.kind == GDKW_TEST_CURSOR_CUSTOM ||
		    r.kind == GDKW_TEST_CURSOR_FALLBACK) ? r.map : NULL;
  p->applied_valid = TRUE;
}

static GdkCursor *
impl_cursor (GDKWWindowImplWayland *impl)
{
  if (impl == NULL)
    return NULL;
  return g_object_get_data (G_OBJECT (impl), GDKW_CURSOR_IMPL_DATA_KEY);
}

/* grab cursor, else focus toplevel cursor, else root cursor, else NULL. */
static GdkCursor *
effective_cursor (GdkDisplay *display, GDKWSeat *seat)
{
  GdkCursor *cursor = NULL;

  if (seat->grab.active && seat->grab.cursor != NULL)
    return seat->grab.cursor;

  cursor = impl_cursor (seat->pointer_focus);
  if (cursor != NULL)
    return cursor;

  {
    GdkScreen *screen = GDKW_DISPLAY_WAYLAND (display)->default_screen;

    if (screen != NULL && GDKW_IS_SCREEN_WAYLAND (screen) &&
	GDKW_SCREEN_WAYLAND (screen)->root_window != NULL)
      cursor = impl_cursor (_gdk_wayland_window_get_impl
			    (GDKW_SCREEN_WAYLAND (screen)->root_window));
  }
  return cursor;
}

static void
apply_effective (GdkDisplay *display)
{
  CursorPriv *p;
  GDKWSeat *seat;

  p = cursor_priv_get (display);
  if (p == NULL || p->base.closed)
    return;
  seat = _gdk_wayland_display_get_seat (display);
  if (seat->pointer_focus == NULL)
    return;

  apply_cursor (p, effective_cursor (display, seat),
		seat->pointer_enter_serial);
}

void
_gdk_wayland_cursor_pointer_enter (GdkDisplay *display,
				   guint32     serial)
{
  CursorPriv *p;
  GDKWSeat *seat;

  p = cursor_priv_get (display);
  if (p == NULL || p->base.closed)
    return;
  seat = _gdk_wayland_display_get_seat (display);
  if (seat->pointer_focus == NULL)
    return;

  p->applied_valid = FALSE;

  /* M10: the focus window may live on an output of another scale */
  if (p->base.theme_loaded && !p->theme_disabled &&
      MAX (1, _gdk_wayland_scale_cursor_scale (display)) != p->loaded_scale)
    theme_load (p, p->search_path);

  apply_cursor (p, effective_cursor (display, seat), serial);
}

/* M10 I8d.  The scale piece calls this when the effective scale of the
 * pointer focus window (or the largest output scale) changed. */
void
_gdk_wayland_cursor_scale_changed (GdkDisplay *display)
{
  CursorPriv *p = cursor_priv_peek (display);

  if (p == NULL || p->base.closed || !p->base.theme_loaded)
    return;

  if (MAX (1, _gdk_wayland_scale_cursor_scale (display)) == p->loaded_scale)
    return;

  if (!p->theme_disabled)
    theme_load (p, p->search_path);
  p->applied_valid = FALSE;
  apply_effective (display);
}

void
_gdk_wayland_cursor_pointer_leave (GdkDisplay *display)
{
  CursorPriv *p = cursor_priv_peek (display);

  if (p != NULL)
    p->applied_valid = FALSE;
}

void
_gdk_wayland_cursor_update (GdkDisplay *display)
{
  if (display == NULL || !GDKW_IS_DISPLAY_WAYLAND (display))
    return;
  apply_effective (display);
}

void
_gdk_wayland_cursor_window_set_cursor (GdkWindow *window,
				       GdkCursor *cursor)
{
  GDKWWindowImplWayland *impl;
  GdkDisplay *display = NULL;

  impl = _gdk_wayland_window_get_impl (window);
  if (impl == NULL)
    return;

  /* remember it, whatever the state of the display */
  if (cursor != NULL)
    g_object_set_data_full (G_OBJECT (impl), GDKW_CURSOR_IMPL_DATA_KEY,
			    gdk_cursor_ref (cursor),
			    (GDestroyNotify) gdk_cursor_unref);
  else
    g_object_set_data (G_OBJECT (impl), GDKW_CURSOR_IMPL_DATA_KEY, NULL);

  if (impl->screen != NULL && GDKW_IS_SCREEN_WAYLAND (impl->screen))
    display = GDKW_SCREEN_WAYLAND (impl->screen)->display;
  if (display == NULL || !GDKW_IS_DISPLAY_WAYLAND (display))
    return;

  if (_gdk_wayland_display_get_seat (display)->pointer_focus == impl)
    apply_effective (display);
}

/* ------------------------------------------------------------------------
 * Theme change
 * ------------------------------------------------------------------------ */

static void
theme_reconfigure (CursorPriv *p,
		   const gchar *theme,
		   gint         size,
		   const gchar *search_path)
{
  if (theme != NULL && *theme != '\0')
    {
      g_free (p->base.theme_name);
      p->base.theme_name = g_strdup (theme);
    }
  if (size > 0)
    p->base.theme_size = MIN (size, 512);

  p->theme_disabled = FALSE;
  theme_load (p, search_path);

  p->applied_valid = FALSE;
  apply_effective (p->base.display);
}

void
_gdk_wayland_cursor_theme_set (GdkDisplay  *display,
			       const gchar *theme,
			       gint         size)
{
  CursorPriv *p = cursor_priv_get (display);

  if (p == NULL || p->base.closed)
    return;
  theme_reconfigure (p, theme, size, NULL);
}

/* ------------------------------------------------------------------------
 * Display teardown
 * ------------------------------------------------------------------------ */

void
_gdk_wayland_cursor_display_finalize (GdkDisplay *display)
{
  CursorPriv *p = cursor_priv_get (display);
  GSList *l;

  if (p == NULL || p->base.closed)
    return;

  p->base.closed = TRUE;

  if (p->base.surface != NULL)
    {
      wl_surface_destroy (p->base.surface);
      p->base.surface = NULL;
    }

  for (l = p->base.custom; l != NULL; l = l->next)
    {
      GDKWCursor *c = l->data;

      if (c->buffer != NULL)
	wl_buffer_destroy (c->buffer);
      c->buffer = NULL;
      if (c->map != NULL)
	munmap (c->map, c->map_size);
      c->map = NULL;
      c->map_size = 0;
    }
  g_slist_free (p->base.custom);
  p->base.custom = NULL;

  if (p->base.fallback_buffer != NULL)
    {
      wl_buffer_destroy (p->base.fallback_buffer);
      p->base.fallback_buffer = NULL;
    }
  if (p->base.fallback_map != NULL)
    {
      munmap (p->base.fallback_map, 4);
      p->base.fallback_map = NULL;
    }

  if (p->base.theme != NULL)
    {
      wl_cursor_theme_destroy (p->base.theme);
      p->base.theme = NULL;
    }
  p->base.theme_usable = FALSE;
  p->base.applied_buffer = NULL;
  p->applied_map = NULL;
  p->applied_valid = FALSE;
  p->base.applied.cursor = NULL;
}

/* ------------------------------------------------------------------------
 * GdkCursor objects
 * ------------------------------------------------------------------------ */

static GdkCursor *
cursor_alloc (GdkDisplay    *display,
	      GdkCursorType  type,
	      const gchar   *name,
	      gint           hot_x,
	      gint           hot_y)
{
  GDKWCursor *c;

  c = g_new0 (GDKWCursor, 1);
  c->display = display;
  if (display != NULL)
    g_object_add_weak_pointer (G_OBJECT (display), (gpointer *) &c->display);
  c->name = g_strdup (name);
  c->hot_x = hot_x;
  c->hot_y = hot_y;

  c->cursor.type = type;
  c->cursor.ref_count = 1;

  return (GdkCursor *) c;
}

GdkCursor *
_gdk_wayland_cursor_new_for_display (GdkDisplay    *display,
				     GdkCursorType  cursor_type)
{
  g_return_val_if_fail (GDK_IS_DISPLAY (display), NULL);

  return cursor_alloc (display, cursor_type, NULL, 0, 0);
}

/* bit (x, y) of an A1 cairo image surface, LSB first on little endian */
static gboolean
a1_bit (const guchar *data, gint stride, gint x, gint y)
{
  guchar byte = data[(gsize) y * stride + x / 8];

#if G_BYTE_ORDER == G_LITTLE_ENDIAN
  return (byte >> (x % 8)) & 1;
#else
  return (byte >> (7 - x % 8)) & 1;
#endif
}

static cairo_surface_t *
pixmap_surface (GdkPixmap *pixmap)
{
  GdkDrawable *impl = GDK_PIXMAP_OBJECT (pixmap)->impl;

  if (impl == NULL || !GDKW_IS_PIXMAP_IMPL_WAYLAND (impl))
    return NULL;
  return GDKW_PIXMAP_IMPL_WAYLAND (impl)->surface;
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
  GDKWCursor *c;
  cairo_surface_t *src, *msk, *image;
  const guchar *sdata, *mdata;
  guchar *idata;
  gint sstride, mstride, istride;
  gint w, h, sw, sh, px, py;
  guint32 fgv, bgv;

  g_return_val_if_fail (GDK_IS_PIXMAP (source), NULL);
  g_return_val_if_fail (GDK_IS_PIXMAP (mask), NULL);
  g_return_val_if_fail (fg != NULL, NULL);
  g_return_val_if_fail (bg != NULL, NULL);

  display = gdk_drawable_get_display (GDK_DRAWABLE (source));

  gdk_drawable_get_size (GDK_DRAWABLE (source), &w, &h);
  gdk_drawable_get_size (GDK_DRAWABLE (mask), &sw, &sh);
  if (gdk_drawable_get_depth (GDK_DRAWABLE (source)) != 1 ||
      gdk_drawable_get_depth (GDK_DRAWABLE (mask)) != 1 ||
      w != sw || h != sh || w <= 0 || h <= 0)
    {
      g_debug ("ydk-wayland: cursor pixmaps must be depth 1 and of equal size");
      return NULL;
    }

  src = pixmap_surface (source);
  msk = pixmap_surface (mask);
  if (src == NULL || msk == NULL ||
      cairo_surface_get_type (src) != CAIRO_SURFACE_TYPE_IMAGE ||
      cairo_surface_get_type (msk) != CAIRO_SURFACE_TYPE_IMAGE ||
      cairo_image_surface_get_format (src) != CAIRO_FORMAT_A1 ||
      cairo_image_surface_get_format (msk) != CAIRO_FORMAT_A1)
    {
      g_debug ("ydk-wayland: cursor pixmaps are not Wayland bitmaps");
      return NULL;
    }

  g_return_val_if_fail (0 <= x && x < w, NULL);
  g_return_val_if_fail (0 <= y && y < h, NULL);

  image = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, w, h);
  if (cairo_surface_status (image) != CAIRO_STATUS_SUCCESS)
    {
      cairo_surface_destroy (image);
      return NULL;
    }

  cairo_surface_flush (src);
  cairo_surface_flush (msk);
  sdata = cairo_image_surface_get_data (src);
  mdata = cairo_image_surface_get_data (msk);
  sstride = cairo_image_surface_get_stride (src);
  mstride = cairo_image_surface_get_stride (msk);
  idata = cairo_image_surface_get_data (image);
  istride = cairo_image_surface_get_stride (image);

  fgv = 0xff000000u | ((guint32) (fg->red >> 8) << 16) |
	((guint32) (fg->green >> 8) << 8) | (guint32) (fg->blue >> 8);
  bgv = 0xff000000u | ((guint32) (bg->red >> 8) << 16) |
	((guint32) (bg->green >> 8) << 8) | (guint32) (bg->blue >> 8);

  for (py = 0; py < h; py++)
    {
      guint32 *row = (guint32 *) (idata + (gsize) py * istride);

      for (px = 0; px < w; px++)
	{
	  if (!a1_bit (mdata, mstride, px, py))
	    row[px] = 0;
	  else
	    row[px] = a1_bit (sdata, sstride, px, py) ? fgv : bgv;
	}
    }
  cairo_surface_mark_dirty (image);

  c = (GDKWCursor *) cursor_alloc (display, GDK_CURSOR_IS_PIXMAP, NULL, x, y);
  c->image = image;
  c->width = w;
  c->height = h;
  return (GdkCursor *) c;
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
  GDKWCursor *c;
  cairo_surface_t *image;
  cairo_t *cr;
  gint w, h;

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

  w = gdk_pixbuf_get_width (pixbuf);
  h = gdk_pixbuf_get_height (pixbuf);
  g_return_val_if_fail (0 <= x && x < w, NULL);
  g_return_val_if_fail (0 <= y && y < h, NULL);

  image = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, w, h);
  if (cairo_surface_status (image) != CAIRO_STATUS_SUCCESS)
    {
      cairo_surface_destroy (image);
      return NULL;
    }
  cr = cairo_create (image);
  gdk_cairo_set_source_pixbuf (cr, pixbuf, 0, 0);
  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
  cairo_paint (cr);
  cairo_destroy (cr);
  cairo_surface_flush (image);

  c = (GDKWCursor *) cursor_alloc (display, GDK_CURSOR_IS_PIXMAP, NULL, x, y);
  c->image = image;
  c->width = w;
  c->height = h;
  return (GdkCursor *) c;
}

GdkCursor *
_gdk_wayland_cursor_new_from_name (GdkDisplay  *display,
				   const gchar *name)
{
  CursorPriv *p;

  g_return_val_if_fail (GDK_IS_DISPLAY (display), NULL);
  g_return_val_if_fail (name != NULL, NULL);

  p = cursor_priv_get (display);
  if (p != NULL && !p->base.closed)
    {
      theme_ensure (p);
      /* exact name, no aliases, like GDK2; an unusable theme cannot say */
      if (p->base.theme_usable &&
	  wl_cursor_theme_get_cursor (p->base.theme, name) == NULL)
	return NULL;
    }

  return cursor_alloc (display, GDK_CURSOR_IS_PIXMAP, name, 0, 0);
}

void
_gdk_wayland_cursor_destroy (GdkCursor *cursor)
{
  GDKWCursor *c;
  CursorPriv *p;

  g_return_if_fail (cursor != NULL);
  g_return_if_fail (cursor->ref_count == 0);

  c = (GDKWCursor *) cursor;

  if (c->display != NULL)
    {
      p = cursor_priv_peek (c->display);
      if (p != NULL)
	{
	  if (p->base.applied.cursor == cursor)
	    p->base.applied.cursor = NULL;
	  if (!p->base.closed)
	    custom_buffer_release (p, c);
	}
      g_object_remove_weak_pointer (G_OBJECT (c->display),
				    (gpointer *) &c->display);
      c->display = NULL;
    }

  if (c->image != NULL)
    cairo_surface_destroy (c->image);
  g_free (c->name);
  g_free (c);
}

GdkDisplay *
_gdk_wayland_cursor_get_display (GdkCursor *cursor)
{
  g_return_val_if_fail (cursor != NULL, NULL);

  return ((GDKWCursor *) cursor)->display;
}

GdkPixbuf *
_gdk_wayland_cursor_get_image (GdkCursor *cursor)
{
  GDKWCursor *c;
  GdkPixbuf *pixbuf;
  const guchar *src;
  guchar *dst;
  gint x, y, sstride, dstride;
  gchar buf[16];

  g_return_val_if_fail (cursor != NULL, NULL);

  c = (GDKWCursor *) cursor;
  if (c->image == NULL)
    return NULL;	/* libwayland-cursor does not expose theme pixels */

  pixbuf = gdk_pixbuf_new (GDK_COLORSPACE_RGB, TRUE, 8, c->width, c->height);
  if (pixbuf == NULL)
    return NULL;

  cairo_surface_flush (c->image);
  src = cairo_image_surface_get_data (c->image);
  sstride = cairo_image_surface_get_stride (c->image);
  dst = gdk_pixbuf_get_pixels (pixbuf);
  dstride = gdk_pixbuf_get_rowstride (pixbuf);

  for (y = 0; y < c->height; y++)
    {
      const guint32 *s = (const guint32 *) (src + (gsize) y * sstride);
      guchar *d = dst + (gsize) y * dstride;

      for (x = 0; x < c->width; x++, d += 4)
	{
	  guint32 v = s[x];
	  guint a = v >> 24;
	  guint r = (v >> 16) & 0xff;
	  guint g = (v >> 8) & 0xff;
	  guint b = v & 0xff;

	  if (a != 0 && a != 255)
	    {
	      r = MIN (255, (r * 255 + a / 2) / a);
	      g = MIN (255, (g * 255 + a / 2) / a);
	      b = MIN (255, (b * 255 + a / 2) / a);
	    }
	  d[0] = r;
	  d[1] = g;
	  d[2] = b;
	  d[3] = a;
	}
    }

  g_snprintf (buf, sizeof buf, "%d", c->hot_x);
  gdk_pixbuf_set_option (pixbuf, "x_hot", buf);
  g_snprintf (buf, sizeof buf, "%d", c->hot_y);
  gdk_pixbuf_set_option (pixbuf, "y_hot", buf);

  return pixbuf;
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
  GDKWCursorState *s;

  g_return_val_if_fail (GDK_IS_DISPLAY (display), 0);

  s = _gdk_wayland_display_get_cursor_state (display);
  return s != NULL ? (guint) s->theme_size : GDKW_CURSOR_DEFAULT_SIZE;
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

/* ------------------------------------------------------------------------
 * Test hooks
 * ------------------------------------------------------------------------ */

gboolean
_gdk_wayland_test_cursor_info (GdkDisplay         *display,
			       GDKWTestCursorInfo *info)
{
  CursorPriv *p;

  if (info == NULL)
    return FALSE;
  memset (info, 0, sizeof *info);

  p = cursor_priv_get (display);
  if (p == NULL)
    return FALSE;

  *info = p->base.applied;
  info->theme_loaded = p->base.theme_loaded;
  info->theme_usable = p->base.theme_usable;
  g_strlcpy (info->theme_name, p->base.theme_name ? p->base.theme_name : "",
	     sizeof info->theme_name);
  info->theme_size = p->base.theme_size;
  return TRUE;
}

gboolean
_gdk_wayland_test_cursor_reset (GdkDisplay *display)
{
  CursorPriv *p = cursor_priv_get (display);

  if (p == NULL)
    return FALSE;

  memset (&p->base.applied, 0, sizeof p->base.applied);
  p->base.applied_buffer = NULL;
  p->applied_map = NULL;
  p->applied_valid = FALSE;
  return TRUE;
}

gboolean
_gdk_wayland_test_cursor_theme_reload (GdkDisplay  *display,
				       const gchar *theme,
				       gint         size,
				       const gchar *search_path)
{
  CursorPriv *p = cursor_priv_get (display);

  if (p == NULL || p->base.closed)
    return FALSE;

  if (search_path != NULL)
    {
      g_free (p->search_path);
      p->search_path = g_strdup (search_path);
    }
  theme_reconfigure (p, theme, size, search_path);
  return TRUE;
}

gboolean
_gdk_wayland_test_cursor_disable_theme (GdkDisplay *display,
					gboolean    disable)
{
  CursorPriv *p = cursor_priv_get (display);

  if (p == NULL || p->base.closed)
    return FALSE;

  p->theme_disabled = disable ? TRUE : FALSE;
  theme_load (p, disable ? NULL : p->search_path);

  p->applied_valid = FALSE;
  apply_effective (display);
  return TRUE;
}

gboolean
_gdk_wayland_test_cursor_pixel (GdkDisplay *display,
				gint        x,
				gint        y,
				guint32    *argb)
{
  CursorPriv *p = cursor_priv_get (display);

  if (p == NULL || argb == NULL || p->applied_map == NULL ||
      (p->base.applied.kind != GDKW_TEST_CURSOR_CUSTOM &&
       p->base.applied.kind != GDKW_TEST_CURSOR_FALLBACK) ||
      x < 0 || y < 0 ||
      x >= p->base.applied.width || y >= p->base.applied.height)
    return FALSE;

  *argb = ((const guint32 *) p->applied_map)[(gsize) y * p->base.applied.width + x];
  return TRUE;
}

