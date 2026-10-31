/*
 * T0.1  A toplevel maps and exposes within 2 seconds.
 *
 * A GtkWindow with a flat background and a label is shown.  Checks: the map
 * event and the first expose event arrive within 2 s of gtk_widget_show_all,
 * the window has a sane size, the corner pixel read back from the window is
 * the background colour, and the label drew some ink.
 */
#include "common.h"

static gboolean
mapped_and_exposed (gpointer p)
{
  TWatch *w = p;
  return w->map > 0 && w->expose > 0;
}

/* gtk_widget_show_now () waits in a nested loop for the map-event.  If the
 * backend never sends one the loop would never end; this timeout sends a fake
 * map-event after 2 s so the test can finish and report the hang. */
static gboolean show_now_timed_out = FALSE;

static gboolean
show_now_unstick (gpointer p)
{
  GdkEventAny ev;
  memset (&ev, 0, sizeof ev);
  ev.type = GDK_MAP;
  ev.window = GTK_WIDGET (p)->window;
  show_now_timed_out = TRUE;
  gtk_widget_event (GTK_WIDGET (p), (GdkEvent *) &ev);
  return FALSE;
}

int
main (int argc, char **argv)
{
  TWatch watch;
  GtkWidget *win, *label;
  GdkColor bg;
  guint32 want;
  TShot *shot;
  int w, h, ink;
  double ms;

  t_begin ("T0.1", &argc, &argv);

  win = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title (GTK_WINDOW (win), "T0.1");
  gtk_window_set_default_size (GTK_WINDOW (win), 320, 200);
  gdk_color_parse ("#e8d8a0", &bg);
  gtk_widget_modify_bg (win, GTK_STATE_NORMAL, &bg);
  want = t_color_rgb (&bg);
  label = gtk_label_new ("T0.1 toplevel map and expose");
  gtk_container_add (GTK_CONTAINER (win), label);
  t_watch (win, &watch);

  watch.show_us = g_get_monotonic_time ();
  gtk_widget_show_all (win);

  T_CHECK (t_wait_until (mapped_and_exposed, &watch, 2000),
           "no map plus expose within 2 s (map=%d expose=%d)", watch.map, watch.expose);
  ms = watch.first_expose_us ? (watch.first_expose_us - watch.show_us) / 1000.0 : -1;
  T_INFO ("first expose after %.1f ms, map events=%d expose events=%d configure events=%d",
          ms, watch.map, watch.expose, watch.configure);
  T_CHECK (ms >= 0 && ms <= 2000.0 * t_scale (), "first expose took %.1f ms", ms);

  t_settle (300);
  T_CHECK (gtk_widget_get_mapped (win), "window is not mapped");
  T_CHECK (gdk_window_is_visible (win->window), "GdkWindow is not visible");

  gdk_drawable_get_size (win->window, &w, &h);
  T_INFO ("window size %dx%d, allocation %dx%d", w, h, win->allocation.width, win->allocation.height);
  T_CHECK (w >= 100 && h >= 50, "window size %dx%d is not sane", w, h);
  T_CHECK (win->allocation.width == w && win->allocation.height == h,
           "allocation %dx%d differs from window size %dx%d",
           win->allocation.width, win->allocation.height, w, h);

  shot = t_capture_toplevel (win);
  T_CHECK (shot != NULL, "could not read the window pixels");
  if (shot)
    {
      TRect all = { 0, 0, shot->w, shot->h };
      T_CHECK (t_px (shot, 2, 2) == want, "pixel (2,2) is %06x, want %06x", t_px (shot, 2, 2), want);
      T_CHECK (t_px (shot, w - 3, h - 3) == want, "pixel (w-3,h-3) is %06x, want %06x",
               t_px (shot, w - 3, h - 3), want);
      ink = t_count_not (shot, all, want, NULL);
      T_INFO ("%d non-background pixels", ink);
      T_CHECK (ink > 100, "label drew only %d ink pixels", ink);
      if (t_nfail)
        t_save (shot, "window");
      t_shot_free (shot);
    }
  t_shot_point ("mapped");

  gtk_widget_destroy (win);
  t_settle (100);

  /* gtk_widget_show_now () needs the map-event of the toplevel. */
  {
    GtkWidget *w2 = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    guint id;
    gtk_window_set_default_size (GTK_WINDOW (w2), 200, 100);
    gtk_container_add (GTK_CONTAINER (w2), gtk_label_new ("show_now"));
    gtk_widget_realize (w2);
    id = g_timeout_add (t_ms (2000), show_now_unstick, w2);
    gtk_widget_show_now (w2);
    if (!show_now_timed_out)
      g_source_remove (id);
    T_CHECK (!show_now_timed_out,
             "gtk_widget_show_now () did not return within 2 s: the toplevel got no map-event");
    gtk_widget_destroy (w2);
    t_settle (100);
  }
  return t_finish ();
}
