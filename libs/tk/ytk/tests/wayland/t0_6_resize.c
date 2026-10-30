/*
 * T0.6  Ten resizes, and the window still exposes.
 *
 * A toplevel holds one GtkDrawingArea that paints a solid colour in its expose
 * handler.  The program calls gtk_window_resize ten times (growing and
 * shrinking).  After each call it waits for the configure event with the new
 * size, lets the redraw run and requires: at least one new expose event, the
 * window size equal to the requested size, and the colour of the drawing area
 * in the corners of the read-back picture (the new area is painted, not stale
 * or empty).  At the end an explicit queue_draw must still produce an expose
 * within 1 s (the window did not freeze).  Risk R5 of the merge plan: the
 * configure request count in GtkWindow.
 */
#include "common.h"

#define FILL "#336699"

typedef struct
{
  TWatch win;      /* configure events of the toplevel */
  TWatch area;     /* expose events of the drawing area (it covers the toplevel) */
  int    want_w, want_h;
} Ctx;

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
  return FALSE; /* let the after handler of the watch run */
}

static gboolean
configured (gpointer p)
{
  Ctx *c = p;
  return c->win.cw == c->want_w && c->win.ch == c->want_h;
}

static int exp_base;

static gboolean
more_exposes (gpointer p)
{
  return ((Ctx *) p)->area.expose > exp_base;
}

int
main (int argc, char **argv)
{
  static const int sizes[10][2] = { { 260, 180 }, { 300, 220 }, { 240, 160 }, { 400, 300 }, { 320, 200 },
                                    { 500, 360 }, { 280, 190 }, { 350, 260 }, { 220, 150 }, { 420, 310 } };
  GtkWidget *win, *da;
  Ctx ctx;
  guint32 want;
  int i, w, h;
  TShot *s;

  t_begin ("T0.6", &argc, &argv);
  want = t_parse_rgb (FILL);

  win = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  gtk_window_set_default_size (GTK_WINDOW (win), 320, 200);
  da = gtk_drawing_area_new ();
  gtk_widget_set_size_request (da, 50, 50);
  g_signal_connect (da, "expose-event", G_CALLBACK (da_expose), NULL);
  gtk_container_add (GTK_CONTAINER (win), da);
  memset (&ctx, 0, sizeof ctx);
  t_watch (win, &ctx.win);
  t_watch (da, &ctx.area);
  gtk_widget_show_all (win);
  t_settle (500);
  T_INFO ("initial: %d expose events, %d configure events, size %dx%d", ctx.area.expose,
          ctx.win.configure, win->allocation.width, win->allocation.height);
  T_CHECK (ctx.area.expose > 0, "no expose after show");

  for (i = 0; i < 10; i++)
    {
      int before = ctx.area.expose;
      gboolean got_cfg;

      ctx.want_w = sizes[i][0];
      ctx.want_h = sizes[i][1];
      gtk_window_resize (GTK_WINDOW (win), ctx.want_w, ctx.want_h);
      got_cfg = t_wait_until (configured, &ctx, 2000);
      T_CHECK (got_cfg, "resize %d to %dx%d: no configure event with that size in 2 s (last %dx%d)", i + 1,
               ctx.want_w, ctx.want_h, ctx.win.cw, ctx.win.ch);
      t_settle (250);
      gdk_drawable_get_size (win->window, &w, &h);
      T_CHECK (w == ctx.want_w && h == ctx.want_h, "resize %d: window is %dx%d, want %dx%d", i + 1, w, h,
               ctx.want_w, ctx.want_h);
      T_CHECK (ctx.area.expose > before, "resize %d: no expose event after the resize (%d before and after)",
               i + 1, before);
      s = t_capture_toplevel (win);
      if (s)
        {
          T_CHECK (t_px (s, 1, 1) == want, "resize %d: pixel (1,1) is %06x, want %06x", i + 1, t_px (s, 1, 1), want);
          T_CHECK (t_px (s, s->w - 2, s->h - 2) == want, "resize %d: pixel (w-2,h-2) is %06x, want %06x (the new area is not painted)",
                   i + 1, t_px (s, s->w - 2, s->h - 2), want);
          T_CHECK (t_px (s, s->w - 2, 1) == want && t_px (s, 1, s->h - 2) == want,
                   "resize %d: top right or bottom left corner not painted", i + 1);
          if (t_nfail)
            t_save (s, "after-resize");
          t_shot_free (s);
        }
      else
        T_CHECK (FALSE, "resize %d: cannot read pixels", i + 1);
      T_INFO ("resize %d to %dx%d ok: %d configure and %d expose events so far", i + 1, ctx.want_w, ctx.want_h,
              ctx.win.configure, ctx.area.expose);
    }

  /* a pure move must not change the size (core passes width = height = -1) */
  gdk_drawable_get_size (win->window, &w, &h);
  {
    int w0 = w, h0 = h;
    gtk_window_move (GTK_WINDOW (win), 40, 60);
    t_settle (300);
    gdk_drawable_get_size (win->window, &w, &h);
    T_CHECK (w == w0 && h == h0, "gtk_window_move changed the window size from %dx%d to %dx%d", w0, h0, w, h);
    gtk_widget_queue_draw (win);
    t_settle (300);
    s = t_capture_toplevel (win);
    T_CHECK (s && s->w == w0 && s->h == h0 && t_px (s, w0 - 2, h0 - 2) == want,
             "after gtk_window_move the window picture is wrong");
    t_shot_free (s);
  }

  /* final liveness: an explicit invalidate still produces an expose */
  exp_base = ctx.area.expose;
  gtk_widget_queue_draw (win);
  T_CHECK (t_wait_until (more_exposes, &ctx, 1000), "window frozen: queue_draw after ten resizes gave no expose in 1 s");

  /* resize in quick succession without waiting, then settle */
  gtk_window_resize (GTK_WINDOW (win), 300, 200);
  gtk_window_resize (GTK_WINDOW (win), 360, 240);
  gtk_window_resize (GTK_WINDOW (win), 330, 210);
  t_settle (500);
  gdk_drawable_get_size (win->window, &w, &h);
  T_CHECK (w == 330 && h == 210, "burst of three resizes: window is %dx%d, want 330x210", w, h);
  exp_base = ctx.area.expose;
  gtk_widget_queue_draw (win);
  T_CHECK (t_wait_until (more_exposes, &ctx, 1000), "window frozen after a burst of resizes");

  gtk_widget_destroy (win);
  t_settle (100);
  return t_finish ();
}
