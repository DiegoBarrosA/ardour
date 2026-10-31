/*
 * T0.9  Pango layouts: sizes, ellipsis, wrap, and drawing to the window.
 *
 * Checks the layout API (size grows with font and text, ellipsize and wrap
 * work), then draws one layout three ways and compares the ink boxes:
 *   1. gdk_draw_layout on the window of a drawing area,
 *   2. pango_cairo_show_layout on gdk_cairo_create of the same window,
 *   3. pango_cairo_show_layout on a cairo image surface (the reference).
 * The boxes must have equal size within 2 pixels, and 1 and 2 must have the
 * same ink box (a little antialiasing difference is allowed).
 * The pictures are read from the toplevel.
 */
#include "common.h"

#define BGSPEC "#ffffff"

static PangoLayout *the_layout;

static gboolean
da_expose (GtkWidget *w, GdkEventExpose *e, gpointer data)
{
  /* 1: gdk_draw_layout at (10, 10) */
  gdk_draw_layout (w->window, w->style->fg_gc[GTK_STATE_NORMAL], 10, 10, the_layout);
  /* 2: pango_cairo on the window at (10, 90) */
  {
    cairo_t *cr = gdk_cairo_create (w->window);
    cairo_set_source_rgb (cr, 0, 0, 0);
    cairo_move_to (cr, 10, 90);
    pango_cairo_show_layout (cr, the_layout);
    cairo_destroy (cr);
  }
  return FALSE; /* let the after handler of the watch run */
}

static gboolean
exposed (gpointer p)
{
  return ((TWatch *) p)->expose > 0;
}

int
main (int argc, char **argv)
{
  GtkWidget *win, *da;
  PangoFontDescription *fd;
  PangoLayout *l;
  TWatch watch;
  int w1, h1, w2, h2, w3, h3, wl, hl;
  guint32 white;
  TShot *shot;
  TRect box1, box2, box3, area1, area2, rect;
  cairo_surface_t *img;
  cairo_t *cr;
  GdkColor wc;
  const char *text = "Hello <i>pango</i> w\xc3\xb6rld 123";

  t_begin ("T0.9", &argc, &argv);

  win = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  da = gtk_drawing_area_new ();
  gtk_widget_set_size_request (da, 360, 170);
  gdk_color_parse (BGSPEC, &wc);
  gtk_widget_modify_bg (da, GTK_STATE_NORMAL, &wc);
  gtk_widget_modify_bg (win, GTK_STATE_NORMAL, &wc);
  white = t_color_rgb (&wc);
  gtk_container_add (GTK_CONTAINER (win), da);
  gtk_widget_realize (da);

  /* layout API */
  l = gtk_widget_create_pango_layout (da, NULL);
  fd = pango_font_description_from_string ("DejaVu Sans Bold 16");
  pango_layout_set_font_description (l, fd);
  pango_layout_set_markup (l, text, -1);
  pango_layout_get_pixel_size (l, &w1, &h1);
  T_INFO ("layout size %dx%d, %d lines", w1, h1, pango_layout_get_line_count (l));
  T_CHECK (w1 > 100 && h1 > 12, "layout size %dx%d is not sane", w1, h1);

  {
    PangoLayout *l2 = pango_layout_copy (l);
    PangoFontDescription *big = pango_font_description_from_string ("DejaVu Sans Bold 32");
    pango_layout_set_font_description (l2, big);
    pango_layout_get_pixel_size (l2, &w2, &h2);
    T_INFO ("32 pt layout size %dx%d", w2, h2);
    T_CHECK (w2 > w1 * 3 / 2 && h2 > h1 * 3 / 2, "a doubled font size did not grow the layout (%dx%d -> %dx%d)", w1, h1, w2, h2);
    pango_font_description_free (big);
    g_object_unref (l2);
  }
  {
    PangoLayout *l3 = pango_layout_copy (l);
    pango_layout_set_width (l3, 120 * PANGO_SCALE);
    pango_layout_set_ellipsize (l3, PANGO_ELLIPSIZE_END);
    pango_layout_set_single_paragraph_mode (l3, TRUE);
    pango_layout_get_pixel_size (l3, &w3, &h3);
    T_CHECK (pango_layout_is_ellipsized (l3), "layout is not ellipsized at 120 px");
    T_CHECK (w3 <= 121, "ellipsized layout is %d px wide", w3);
    pango_layout_set_ellipsize (l3, PANGO_ELLIPSIZE_NONE);
    pango_layout_set_single_paragraph_mode (l3, FALSE);
    pango_layout_set_wrap (l3, PANGO_WRAP_WORD_CHAR);
    T_CHECK (pango_layout_get_line_count (l3) >= 2, "wrap at 120 px gave %d lines", pango_layout_get_line_count (l3));
    g_object_unref (l3);
  }
  {
    int ww, hh;
    PangoLayout *l4 = pango_layout_copy (l);
    pango_layout_set_text (l4, "i", -1);
    pango_layout_get_pixel_size (l4, &ww, &hh);
    T_CHECK (ww < w1 / 3, "one letter is %d px wide, the phrase %d", ww, w1);
    g_object_unref (l4);
  }

  /* 3: reference render on an image surface */
  img = cairo_image_surface_create (CAIRO_FORMAT_RGB24, 360, 80);
  cr = cairo_create (img);
  cairo_set_source_rgb (cr, 1, 1, 1);
  cairo_paint (cr);
  cairo_set_source_rgb (cr, 0, 0, 0);
  cairo_move_to (cr, 10, 10);
  pango_cairo_show_layout (cr, l);
  cairo_destroy (cr);
  cairo_surface_flush (img);
  {
    /* ink box of the reference */
    int x, y, stride = cairo_image_surface_get_stride (img), minx = 9999, miny = 9999, maxx = -1, maxy = -1;
    guchar *d = cairo_image_surface_get_data (img);
    for (y = 0; y < 80; y++)
      for (x = 0; x < 360; x++)
        {
          guint32 px = *(guint32 *) (d + y * stride + x * 4) & 0xffffff;
          if (px != 0xffffff)
            {
              minx = MIN (minx, x); maxx = MAX (maxx, x); miny = MIN (miny, y); maxy = MAX (maxy, y);
            }
        }
    T_CHECK (maxx >= 0, "the reference image has no ink");
    box3.x = minx; box3.y = miny; box3.w = maxx - minx + 1; box3.h = maxy - miny + 1;
  }
  cairo_surface_destroy (img);

  the_layout = l;
  g_signal_connect (da, "expose-event", G_CALLBACK (da_expose), NULL);
  t_watch (da, &watch); /* the drawing area covers the toplevel */
  gtk_widget_show_all (win);
  T_CHECK (t_wait_until (exposed, &watch, 2000), "no expose in 2 s");
  t_settle (400);

  shot = t_capture_toplevel (win);
  T_CHECK (shot != NULL, "cannot read the window pixels");
  if (shot)
    {
      rect = t_widget_rect (da, win);
      area1 = (TRect) { rect.x, rect.y, rect.w, 80 };
      area2 = (TRect) { rect.x, rect.y + 80, rect.w, rect.h - 80 };
      wl = hl = 0;
      T_CHECK (t_ink_bbox (shot, area1, white, &box1), "gdk_draw_layout drew no ink");
      T_CHECK (t_ink_bbox (shot, area2, white, &box2), "pango_cairo_show_layout on the window drew no ink");
      T_INFO ("ink boxes: gdk_draw_layout %dx%d at %d,%d; pango_cairo window %dx%d at %d,%d; image surface %dx%d",
              box1.w, box1.h, box1.x - rect.x, box1.y - rect.y, box2.w, box2.h, box2.x - rect.x,
              box2.y - rect.y - 80, box3.w, box3.h);
      T_CHECK (abs (box1.w - box3.w) <= 2 && abs (box1.h - box3.h) <= 2,
               "gdk_draw_layout ink box %dx%d differs from the reference %dx%d", box1.w, box1.h, box3.w, box3.h);
      T_CHECK (abs (box2.w - box3.w) <= 2 && abs (box2.h - box3.h) <= 2,
               "pango_cairo ink box %dx%d differs from the reference %dx%d", box2.w, box2.h, box3.w, box3.h);
      T_CHECK (abs (box1.x - rect.x - (box2.x - rect.x)) <= 1 && abs ((box1.y - rect.y) - (box2.y - rect.y - 80)) <= 1,
               "the two window renderings are at different places");
      (void) wl; (void) hl;
      if (t_nfail)
        t_save (shot, "window");
      t_shot_free (shot);
    }
  t_shot_point ("layout");

  pango_font_description_free (fd);
  g_object_unref (l);
  gtk_widget_destroy (win);
  t_settle (100);
  return t_finish ();
}
