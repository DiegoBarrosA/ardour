/*
 * common.h -- helpers shared by the stage 0 ytk test programs (T0.1 to T0.10).
 *
 * Each test prints one verdict line of the form
 *     T0.N: PASS | FAIL | SKIP ...
 * and exits 0 for PASS and SKIP, 1 for FAIL.
 *
 * Environment read by the helpers:
 *   T_TIMEOUT_SCALE  multiplies every wait (set it above 1 under valgrind)
 *   T_SYNC_DIR       when set, t_shot_point() lets the runner take a screenshot
 *   T_SAVE_DIR       when set, failed pixel checks save the capture as PNG
 *   T_HAVE_SEAT      set to 1 by the runner when the display has a pointer
 *                    and a keyboard (Xvfb, sway); T0.10 is skipped without it
 *
 * Origin: Wayland backend of the ydk fork, snapshot commit 23f48f0.
 */
#ifndef YTK_WL_TEST_COMMON_H
#define YTK_WL_TEST_COMMON_H

#include <ytk/ytk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *t_name = "T0.?";
static int t_nfail = 0;
static int t_nchk = 0;

#define T_CHECK(cond, ...)                                                   \
  do {                                                                       \
    t_nchk++;                                                                \
    if (!(cond)) {                                                           \
      char *t_msg_ = g_strdup_printf (__VA_ARGS__);                          \
      t_nfail++;                                                             \
      g_print ("%s: CHECK FAILED (%s:%d): %s\n", t_name, __FILE__, __LINE__, \
               t_msg_);                                                      \
      g_free (t_msg_);                                                       \
    }                                                                        \
  } while (0)

#define T_INFO(...)                                                          \
  do {                                                                       \
    char *t_msg_ = g_strdup_printf (__VA_ARGS__);                            \
    g_print ("%s: info: %s\n", t_name, t_msg_);                              \
    g_free (t_msg_);                                                         \
  } while (0)

/* ---- time ------------------------------------------------------------ */

static inline double
t_scale (void)
{
  static double s = -1;
  if (s < 0)
    {
      const char *e = g_getenv ("T_TIMEOUT_SCALE");
      s = e ? g_ascii_strtod (e, NULL) : 1.0;
      if (s <= 0)
        s = 1.0;
    }
  return s;
}

static inline guint
t_ms (guint ms)
{
  return (guint) (ms * t_scale ());
}

static gboolean
t_quit_cb (gpointer loop)
{
  g_main_loop_quit (loop);
  return FALSE;
}

/* Runs the main loop for @ms milliseconds (scaled). */
static inline void
t_pump_ms (guint ms)
{
  GMainLoop *l = g_main_loop_new (NULL, FALSE);
  g_timeout_add (t_ms (ms), t_quit_cb, l);
  g_main_loop_run (l);
  g_main_loop_unref (l);
}

typedef gboolean (*TPred) (gpointer data);

typedef struct
{
  TPred      pred;
  gpointer   data;
  GMainLoop *loop;
  gint64     deadline;
  gboolean   ok;
} TWait;

static gboolean
t_wait_tick (gpointer p)
{
  TWait *w = p;
  if (w->pred (w->data))
    {
      w->ok = TRUE;
      g_main_loop_quit (w->loop);
      return FALSE;
    }
  if (g_get_monotonic_time () >= w->deadline)
    {
      g_main_loop_quit (w->loop);
      return FALSE;
    }
  return TRUE;
}

/* Runs the main loop until @pred returns TRUE or @ms (scaled) pass. */
static inline gboolean
t_wait_until (TPred pred, gpointer data, guint ms)
{
  TWait w;

  if (pred (data))
    return TRUE;
  w.pred = pred;
  w.data = data;
  w.ok = FALSE;
  w.loop = g_main_loop_new (NULL, FALSE);
  w.deadline = g_get_monotonic_time () + (gint64) t_ms (ms) * 1000;
  /* t_wait_tick returns FALSE when it quits the loop, so the source is gone. */
  g_timeout_add (10, t_wait_tick, &w);
  g_main_loop_run (w.loop);
  g_main_loop_unref (w.loop);
  return w.ok;
}

/* Lets pending redraws run and the compositor/server catch up. */
static inline void
t_settle (guint ms)
{
  GdkDisplay *d = gdk_display_get_default ();
  gdk_window_process_all_updates ();
  gdk_display_sync (d);
  t_pump_ms (ms);
  gdk_window_process_all_updates ();
  gdk_display_sync (d);
}

/* ---- backend --------------------------------------------------------- */

static inline gboolean
t_is_wayland (void)
{
  const char *n = G_OBJECT_TYPE_NAME (gdk_display_get_default ());
  return strstr (n, "Wayland") != NULL;
}

static inline void
t_begin (const char *name, int *argc, char ***argv)
{
  GdkDisplay *d;
  t_name = name;
  setvbuf (stdout, NULL, _IOLBF, 0);
  gtk_init (argc, argv);
  d = gdk_display_get_default ();
  g_print ("%s: start display_class=%s display=%s\n", name,
           G_OBJECT_TYPE_NAME (d), gdk_display_get_name (d));
}

static inline int
t_finish (void)
{
  if (t_nfail == 0)
    {
      g_print ("%s: PASS (%d checks)\n", t_name, t_nchk);
      return 0;
    }
  g_print ("%s: FAIL (%d of %d checks failed)\n", t_name, t_nfail, t_nchk);
  return 1;
}

static inline int
t_skip (const char *why)
{
  g_print ("%s: SKIP (%s)\n", t_name, why);
  return 0;
}

/* ---- window watch ---------------------------------------------------- */

typedef struct
{
  int    map;
  int    expose;
  int    configure;
  int    cw, ch;          /* last configure size */
  gint64 show_us;         /* set by the test before gtk_widget_show */
  gint64 first_expose_us;
} TWatch;

static gboolean
t_on_map (GtkWidget *w, GdkEvent *e, gpointer p)
{
  ((TWatch *) p)->map++;
  return FALSE;
}

static gboolean
t_on_expose (GtkWidget *w, GdkEventExpose *e, gpointer p)
{
  TWatch *t = p;
  t->expose++;
  if (!t->first_expose_us)
    t->first_expose_us = g_get_monotonic_time ();
  return FALSE;
}

static gboolean
t_on_configure (GtkWidget *w, GdkEventConfigure *e, gpointer p)
{
  TWatch *t = p;
  t->configure++;
  t->cw = e->width;
  t->ch = e->height;
  return FALSE;
}

static inline void
t_watch (GtkWidget *w, TWatch *t)
{
  memset (t, 0, sizeof *t);
  g_signal_connect (w, "map-event", G_CALLBACK (t_on_map), t);
  g_signal_connect_after (w, "expose-event", G_CALLBACK (t_on_expose), t);
  g_signal_connect (w, "configure-event", G_CALLBACK (t_on_configure), t);
}

/* ---- pixels ---------------------------------------------------------- */

typedef struct
{
  int        w, h;
  GdkPixbuf *pb;
} TShot;

typedef struct
{
  int x, y, w, h;
} TRect;

static inline guint32
t_color_rgb (const GdkColor *c)
{
  return ((guint32) (c->red >> 8) << 16) | ((guint32) (c->green >> 8) << 8) |
         (guint32) (c->blue >> 8);
}

static inline guint32
t_parse_rgb (const char *spec)
{
  GdkColor c;
  if (!gdk_color_parse (spec, &c))
    g_error ("bad colour %s", spec);
  return t_color_rgb (&c);
}

/* Reads w x h pixels at x,y from @win.  Read through TOPLEVEL windows only:
 * the Wayland pixel reader returned 0 for client-side child windows. */
static inline TShot *
t_capture (GdkWindow *win, int x, int y, int w, int h)
{
  TShot *s;
  GdkPixbuf *pb = gdk_pixbuf_get_from_drawable (NULL, win, NULL, x, y, 0, 0, w, h);
  if (!pb)
    return NULL;
  s = g_new0 (TShot, 1);
  s->w = w;
  s->h = h;
  s->pb = pb;
  return s;
}

static inline void
t_shot_free (TShot *s)
{
  if (!s)
    return;
  g_object_unref (s->pb);
  g_free (s);
}

static inline guint32
t_px (const TShot *s, int x, int y)
{
  const guchar *p;
  int n = gdk_pixbuf_get_n_channels (s->pb);
  if (x < 0 || y < 0 || x >= s->w || y >= s->h)
    return 0xdeadbeef;
  p = gdk_pixbuf_get_pixels (s->pb) + (gsize) y * gdk_pixbuf_get_rowstride (s->pb) + (gsize) x * n;
  return ((guint32) p[0] << 16) | ((guint32) p[1] << 8) | p[2];
}

static inline gboolean
t_in_rect (const TRect *r, int x, int y)
{
  return r && x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h;
}

/* Counts pixels inside @area (clipped to the shot) that differ from @bg and
 * are not inside @skip (may be NULL). */
static inline int
t_count_not (const TShot *s, TRect area, guint32 bg, const TRect *skip)
{
  int x, y, n = 0;
  int x1 = MIN (area.x + area.w, s->w), y1 = MIN (area.y + area.h, s->h);
  for (y = MAX (area.y, 0); y < y1; y++)
    for (x = MAX (area.x, 0); x < x1; x++)
      if (!t_in_rect (skip, x, y) && t_px (s, x, y) != bg)
        n++;
  return n;
}

/* Bounding box of pixels that differ from @bg; returns FALSE if none. */
static inline gboolean
t_ink_bbox (const TShot *s, TRect area, guint32 bg, TRect *out)
{
  int x, y, minx = G_MAXINT, miny = G_MAXINT, maxx = -1, maxy = -1;
  int x1 = MIN (area.x + area.w, s->w), y1 = MIN (area.y + area.h, s->h);
  for (y = MAX (area.y, 0); y < y1; y++)
    for (x = MAX (area.x, 0); x < x1; x++)
      if (t_px (s, x, y) != bg)
        {
          minx = MIN (minx, x);
          maxx = MAX (maxx, x);
          miny = MIN (miny, y);
          maxy = MAX (maxy, y);
        }
  if (maxx < 0)
    return FALSE;
  out->x = minx;
  out->y = miny;
  out->w = maxx - minx + 1;
  out->h = maxy - miny + 1;
  return TRUE;
}

static inline int
t_distinct_colors (const TShot *s, int limit)
{
  GHashTable *h = g_hash_table_new (g_direct_hash, g_direct_equal);
  int x, y, n;
  for (y = 0; y < s->h; y++)
    for (x = 0; x < s->w; x++)
      g_hash_table_insert (h, GUINT_TO_POINTER (t_px (s, x, y) + 1), NULL);
  n = g_hash_table_size (h);
  g_hash_table_destroy (h);
  (void) limit;
  return n;
}

/* Number of differing pixels between two shots of the same size. */
static inline int
t_diff (const TShot *a, const TShot *b)
{
  int x, y, n = 0;
  if (a->w != b->w || a->h != b->h)
    return G_MAXINT;
  for (y = 0; y < a->h; y++)
    for (x = 0; x < a->w; x++)
      if (t_px (a, x, y) != t_px (b, x, y))
        n++;
  return n;
}

static inline void
t_save (const TShot *s, const char *tag)
{
  const char *dir = g_getenv ("T_SAVE_DIR");
  char *path;
  if (!dir || !s)
    return;
  path = g_strdup_printf ("%s/%s-%s.png", dir, t_name, tag);
  gdk_pixbuf_save (s->pb, path, "png", NULL, NULL);
  g_print ("%s: saved %s\n", t_name, path);
  g_free (path);
}

/* The toplevel's pixels, whole window. */
static inline TShot *
t_capture_toplevel (GtkWidget *toplevel)
{
  int w, h;
  gdk_drawable_get_size (toplevel->window, &w, &h);
  return t_capture (toplevel->window, 0, 0, w, h);
}

/* Rectangle of @widget in @toplevel window coordinates. */
static inline TRect
t_widget_rect (GtkWidget *widget, GtkWidget *toplevel)
{
  TRect r = { 0, 0, widget->allocation.width, widget->allocation.height };
  gtk_widget_translate_coordinates (widget, toplevel, 0, 0, &r.x, &r.y);
  /* translate_coordinates is relative to the toplevel's allocation origin,
   * which is the toplevel GdkWindow origin for a GtkWindow. */
  return r;
}

/* ---- screenshot hand-shake with the runner --------------------------- */

/* When T_SYNC_DIR is set, writes ready-<test>-<tag> and waits (up to 20 s)
 * for done-<test>-<tag>.  The runner takes the screenshot in between. */
static inline void
t_shot_point (const char *tag)
{
  const char *dir = g_getenv ("T_SYNC_DIR");
  char *ready, *done;
  gint64 end;
  if (!dir)
    return;
  ready = g_strdup_printf ("%s/ready-%s-%s", dir, t_name, tag);
  done = g_strdup_printf ("%s/done-%s-%s", dir, t_name, tag);
  g_file_set_contents (ready, "x", 1, NULL);
  end = g_get_monotonic_time () + 20 * G_USEC_PER_SEC;
  while (!g_file_test (done, G_FILE_TEST_EXISTS) && g_get_monotonic_time () < end)
    t_pump_ms (50);
  g_free (ready);
  g_free (done);
}

#endif /* YTK_WL_TEST_COMMON_H */
