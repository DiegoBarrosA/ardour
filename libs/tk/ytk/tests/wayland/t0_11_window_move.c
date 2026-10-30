/*
 * T0.11  Moving a window before or during show keeps its size.
 *
 * gdk_window_move () calls the move_resize vfunc with width and height -1,
 * which means "keep the size".  The Wayland backend once stored the -1 and
 * sent it in set_window_geometry, so the window never got a buffer (commit
 * 24c52de598).  Each case below shows a 320x200 toplevel with one drawing
 * area that paints a solid colour, and moves it in a different way:
 *
 *   gtk_window_move before show
 *   gdk_window_move on the realized, unmapped window
 *   gtk_window_set_position GTK_WIN_POS_CENTER
 *   gtk_window_move inside the show handler (during show)
 *   gdk_window_move_resize with width and height -1 on the mapped window
 *
 * After gtk_widget_show each case requires, within 2 s: a map event, an
 * expose event, the gdk window size equal to 320x200, and the painted colour
 * in the middle of the read-back picture.  The last case also requires that
 * the size and the picture stay the same after the move_resize call.
 */
#include "common.h"

#define FILL "#336699"
#define WANT_W 320
#define WANT_H 200

typedef enum
{
  C_MOVE_BEFORE_SHOW,
  C_GDK_MOVE_REALIZED,
  C_POS_CENTER,
  C_MOVE_IN_SHOW,
  C_MOVE_RESIZE_MAPPED
} Case;

static gboolean
da_expose (GtkWidget *w, GdkEventExpose *e, gpointer data)
{
  cairo_t *cr = gdk_cairo_create (w->window);
  GdkColor c;
  gdk_color_parse (FILL, &c);
  gdk_cairo_rectangle (cr, &e->area);
  cairo_clip (cr);
  cairo_set_source_rgb (cr, c.red / 65535.0, c.green / 65535.0, c.blue / 65535.0);
  cairo_paint (cr);
  cairo_destroy (cr);
  return FALSE;
}

static void
on_show_move (GtkWidget *w, gpointer data)
{
  gtk_window_move (GTK_WINDOW (w), 50, 70);
}

typedef struct
{
  TWatch win;   /* map and configure events of the toplevel */
  TWatch area;  /* expose events of the drawing area, which covers the toplevel */
} Ctx;

static gboolean
mapped_and_exposed (gpointer p)
{
  Ctx *c = p;
  return c->win.map > 0 && c->area.expose > 0;
}

static void
check_picture (GtkWidget *win, guint32 want, const char *what)
{
  TShot *s = t_capture_toplevel (win);
  if (!s)
    {
      T_CHECK (FALSE, "%s: cannot read pixels", what);
      return;
    }
  T_CHECK (s->w == WANT_W && s->h == WANT_H, "%s: picture is %dx%d, want %dx%d", what, s->w, s->h, WANT_W, WANT_H);
  T_CHECK (t_px (s, s->w / 2, s->h / 2) == want, "%s: pixel in the middle is %06x, want %06x (no drawn content)",
           what, t_px (s, s->w / 2, s->h / 2), want);
  T_CHECK (t_px (s, s->w - 2, s->h - 2) == want, "%s: pixel (w-2,h-2) is %06x, want %06x", what,
           t_px (s, s->w - 2, s->h - 2), want);
  if (t_nfail)
    t_save (s, what);
  t_shot_free (s);
}

static void
run_case (Case c, const char *what)
{
  GtkWidget *win, *da;
  Ctx tw;
  guint32 want = t_parse_rgb (FILL);
  int w, h;
  gboolean ok;

  win = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  gtk_window_set_default_size (GTK_WINDOW (win), WANT_W, WANT_H);
  da = gtk_drawing_area_new ();
  gtk_widget_set_size_request (da, 50, 50);
  g_signal_connect (da, "expose-event", G_CALLBACK (da_expose), NULL);
  gtk_container_add (GTK_CONTAINER (win), da);
  t_watch (win, &tw.win);
  t_watch (da, &tw.area);

  switch (c)
    {
    case C_MOVE_BEFORE_SHOW:
      gtk_window_move (GTK_WINDOW (win), 30, 40);
      break;
    case C_GDK_MOVE_REALIZED:
      gtk_widget_show (da);
      gtk_widget_realize (win);
      gdk_window_move (win->window, 30, 40);
      break;
    case C_POS_CENTER:
      gtk_window_set_position (GTK_WINDOW (win), GTK_WIN_POS_CENTER);
      break;
    case C_MOVE_IN_SHOW:
      g_signal_connect (win, "show", G_CALLBACK (on_show_move), NULL);
      break;
    case C_MOVE_RESIZE_MAPPED:
      break;
    }

  gtk_widget_show_all (win);
  ok = t_wait_until (mapped_and_exposed, &tw, 2000);
  T_CHECK (tw.win.map > 0, "%s: no map event in 2 s", what);
  T_CHECK (ok && tw.area.expose > 0, "%s: no expose event in 2 s (map events %d)", what, tw.win.map);
  t_settle (300);
  gdk_drawable_get_size (win->window, &w, &h);
  T_CHECK (w == WANT_W && h == WANT_H, "%s: window is %dx%d after show, want %dx%d", what, w, h, WANT_W, WANT_H);
  T_CHECK (GTK_WIDGET_MAPPED (win), "%s: window is not mapped", what);
  check_picture (win, want, what);

  if (c == C_MOVE_RESIZE_MAPPED)
    {
      int before = tw.area.expose;
      gdk_window_move_resize (win->window, 60, 80, -1, -1);
      t_settle (300);
      gdk_drawable_get_size (win->window, &w, &h);
      T_CHECK (w == WANT_W && h == WANT_H, "%s: gdk_window_move_resize with -1 changed the size to %dx%d", what, w, h);
      gtk_widget_queue_draw (win);
      t_settle (300);
      T_CHECK (tw.area.expose > before, "%s: no expose after gdk_window_move_resize", what);
      check_picture (win, want, "move-resize-mapped-after");
    }

  T_INFO ("%s: map=%d expose=%d configure=%d size %dx%d", what, tw.win.map, tw.area.expose, tw.win.configure, w, h);
  gtk_widget_destroy (win);
  t_settle (100);
}

int
main (int argc, char **argv)
{
  t_begin ("T0.11", &argc, &argv);
  run_case (C_MOVE_BEFORE_SHOW, "gtk_window_move-before-show");
  run_case (C_GDK_MOVE_REALIZED, "gdk_window_move-realized");
  run_case (C_POS_CENTER, "win-pos-center");
  run_case (C_MOVE_IN_SHOW, "gtk_window_move-in-show");
  run_case (C_MOVE_RESIZE_MAPPED, "move-resize-mapped");
  return t_finish ();
}
