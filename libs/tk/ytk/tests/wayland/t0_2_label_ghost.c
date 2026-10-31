/*
 * T0.2  The H1 ghost test: text that shrinks must not leave old ink.
 *
 * Four cases, each with a long bold text that is replaced by a short one:
 *   A  a GtkLabel directly in a toplevel with a flat background,
 *   B  a GtkLabel (no window) in a GtkEventBox with its own background,
 *   C  a GtkLabel in a GtkViewport (the label draws on the bin window, whose
 *      background GTK does not paint again on expose),
 *   D  a GtkDrawingArea that draws only a Pango layout in its expose handler
 *      (the usual custom widget: it relies on the window being cleared to its
 *      background before the expose handler runs).
 *
 * For every case:
 *   1. Show the long text and let the first frame commit.
 *   2. Record the old allocation and check the long text really drew ink in
 *      the right half of it (otherwise the test proves nothing).
 *   3. Replace the text by a short one and let the redraw run.
 *   4. Read the toplevel pixels.  Every pixel of the old allocation outside
 *      the ink box of the new text must have the background colour, and the
 *      right half of the old allocation must hold no ink at all.
 *
 * A backend that does not clear the exposed region before the double buffered
 * widget draws leaves the old glyph pixels behind (ghosting, finding H1).
 * GtkWindow, GtkEventBox and GtkViewport paint their own background on expose,
 * so cases A, B and C pass even without the backend fix.  Case D is the one
 * that detects it: with the G12 commit of the Wayland backend reverted, D
 * fails (the drawing area window is never cleared to its background, 3 checks
 * fail) while A, B and C and the X11 backend still pass.  Case D is the
 * negative control that proves the test can fail.
 * Pixels are read through the TOPLEVEL window.
 */
#include "common.h"

#define LONG_TEXT "MMMMMMMMMMMMMMMMMMMM"

typedef enum { K_LABEL_WINDOW, K_LABEL_EVENTBOX, K_LABEL_VIEWPORT, K_DRAWING_AREA } Kind;

typedef struct
{
  const char *tag;
  Kind        kind;
  const char *win_bg;  /* toplevel */
  const char *box_bg;  /* eventbox, viewport or drawing area */
  const char *short_text;
} Case;

static gboolean
exposed (gpointer p)
{
  return ((TWatch *) p)->expose > 0;
}

/* ---- case D: the drawing area draws text and nothing else ---- */

static char *da_text;

static PangoLayout *
da_layout (GtkWidget *w)
{
  PangoLayout *l = gtk_widget_create_pango_layout (w, da_text);
  PangoFontDescription *fd = pango_font_description_from_string ("DejaVu Sans Bold 14");
  pango_layout_set_font_description (l, fd);
  pango_font_description_free (fd);
  return l;
}

static gboolean
da_expose (GtkWidget *w, GdkEventExpose *e, gpointer data)
{
  PangoLayout *l = da_layout (w);
  gdk_draw_layout (w->window, w->style->fg_gc[GTK_STATE_NORMAL], 0, 10, l);
  g_object_unref (l);
  return FALSE;
}

/* ---- the check of step 4 ---- */

static void
check_after (const Case *c, GtkWidget *win, TRect old_rect, TRect right_half, TRect ink,
             guint32 want)
{
  TShot *after = t_capture_toplevel (win);
  int n_all, n_right, n_new;

  T_CHECK (after != NULL, "%s: cannot read the window pixels after the change", c->tag);
  if (!after)
    return;
  n_all = t_count_not (after, old_rect, want, &ink);
  n_right = t_count_not (after, right_half, want, NULL);
  n_new = t_count_not (after, ink, want, NULL);
  T_INFO ("%s: after the change: %d stale pixels outside the new ink box, %d in the right half, %d ink pixels of the new text",
          c->tag, n_all, n_right, n_new);
  T_CHECK (n_right == 0, "%s: ghosting, %d pixels of old ink in the right half of the old allocation",
           c->tag, n_right);
  T_CHECK (n_all == 0, "%s: ghosting, %d pixels of old ink outside the new text", c->tag, n_all);
  T_CHECK (n_new > 0, "%s: the new text drew no ink", c->tag);
  if (n_all || n_right)
    t_save (after, c->tag);
  t_shot_free (after);
}

static void
run_case (const Case *c)
{
  GtkWidget *win, *label = NULL, *box = NULL, *da = NULL, *target;
  GdkColor wbg, bbg;
  TWatch watch;
  TRect old_rect, right_half, ink;
  TShot *before;
  guint32 want;
  int n_before;

  g_print ("%s: case %s\n", t_name, c->tag);

  win = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  gtk_window_set_default_size (GTK_WINDOW (win), 200, 60);
  gdk_color_parse (c->win_bg, &wbg);
  gtk_widget_modify_bg (win, GTK_STATE_NORMAL, &wbg);
  want = t_color_rgb (&wbg);

  if (c->kind == K_DRAWING_AREA)
    {
      da = gtk_drawing_area_new ();
      gtk_widget_set_size_request (da, 380, 60);
      gdk_color_parse (c->box_bg, &bbg);
      gtk_widget_modify_bg (da, GTK_STATE_NORMAL, &bbg);
      want = t_color_rgb (&bbg);
      g_free (da_text);
      da_text = g_strdup (LONG_TEXT);
      g_signal_connect (da, "expose-event", G_CALLBACK (da_expose), NULL);
      gtk_container_add (GTK_CONTAINER (win), da);
      target = da;
    }
  else
    {
      PangoAttrList *attrs = pango_attr_list_new ();
      label = gtk_label_new (LONG_TEXT);
      gtk_misc_set_alignment (GTK_MISC (label), 0.0f, 0.5f);
      pango_attr_list_insert (attrs, pango_attr_weight_new (PANGO_WEIGHT_BOLD));
      pango_attr_list_insert (attrs, pango_attr_size_new (14 * PANGO_SCALE));
      gtk_label_set_attributes (GTK_LABEL (label), attrs);
      pango_attr_list_unref (attrs);
      target = label;

      if (c->kind == K_LABEL_EVENTBOX)
        {
          box = gtk_event_box_new ();
          gtk_container_add (GTK_CONTAINER (box), label);
        }
      else if (c->kind == K_LABEL_VIEWPORT)
        {
          box = gtk_viewport_new (NULL, NULL);
          gtk_viewport_set_shadow_type (GTK_VIEWPORT (box), GTK_SHADOW_NONE);
          gtk_container_add (GTK_CONTAINER (box), label);
        }
      if (box)
        {
          gdk_color_parse (c->box_bg, &bbg);
          gtk_widget_modify_bg (box, GTK_STATE_NORMAL, &bbg);
          want = t_color_rgb (&bbg);
          gtk_container_add (GTK_CONTAINER (win), box);
        }
      else
        gtk_container_add (GTK_CONTAINER (win), label);
    }

  /* the widget that gets the expose events: a child window covers the toplevel */
  t_watch (c->kind == K_LABEL_WINDOW ? win : (box ? box : da), &watch);
  gtk_widget_show_all (win);
  T_CHECK (t_wait_until (exposed, &watch, 2000), "%s: no expose within 2 s", c->tag);
  t_settle (300);
  {
    char *tag = g_strdup_printf ("%s-long", c->tag);
    t_shot_point (tag);
    g_free (tag);
  }

  /* step 2 */
  old_rect = t_widget_rect (target, win);
  T_INFO ("%s: old allocation %d,%d %dx%d, window %dx%d", c->tag, old_rect.x, old_rect.y,
          old_rect.w, old_rect.h, win->allocation.width, win->allocation.height);
  T_CHECK (old_rect.w > 40 && old_rect.h > 8, "%s: allocation %dx%d too small", c->tag, old_rect.w,
           old_rect.h);
  right_half = old_rect;
  right_half.x = old_rect.x + old_rect.w / 2;
  right_half.w = old_rect.w - old_rect.w / 2;

  before = t_capture_toplevel (win);
  T_CHECK (before != NULL, "%s: cannot read the window pixels", c->tag);
  if (!before)
    {
      gtk_widget_destroy (win);
      return;
    }
  T_CHECK (t_px (before, 1, 1) == want, "%s: pixel (1,1) is %06x, want background %06x", c->tag,
           t_px (before, 1, 1), want);
  n_before = t_count_not (before, right_half, want, NULL);
  T_INFO ("%s: long text left %d ink pixels in the right half of the old allocation", c->tag, n_before);
  T_CHECK (n_before > 30,
           "%s: long text drew only %d pixels in the right half, the test would be vacuous", c->tag, n_before);
  t_shot_free (before);

  /* step 3 */
  if (c->kind == K_DRAWING_AREA)
    {
      PangoLayout *l;
      PangoRectangle pink;
      g_free (da_text);
      da_text = g_strdup (c->short_text);
      gtk_widget_queue_draw (da);
      l = da_layout (da);
      pango_layout_get_pixel_extents (l, &pink, NULL);
      g_object_unref (l);
      ink.x = old_rect.x + pink.x - 3;
      ink.y = old_rect.y + 10 + pink.y - 3;
      ink.w = pink.width + 6;
      ink.h = pink.height + 6;
    }
  else
    {
      PangoRectangle pink;
      int ox, oy;
      gtk_label_set_text (GTK_LABEL (label), c->short_text);
      t_settle (300);
      /* ox,oy are in the coordinates of the label's parent window and
       * label->allocation is in the same system: subtract it and add the
       * label position in the toplevel. */
      gtk_label_get_layout_offsets (GTK_LABEL (label), &ox, &oy);
      pango_layout_get_pixel_extents (gtk_label_get_layout (GTK_LABEL (label)), &pink, NULL);
      ink.x = old_rect.x + (ox - label->allocation.x) + pink.x - 3;
      ink.y = old_rect.y + (oy - label->allocation.y) + pink.y - 3;
      ink.w = pink.width + 6;
      ink.h = pink.height + 6;
    }
  t_settle (300);
  {
    char *tag = g_strdup_printf ("%s-short", c->tag);
    t_shot_point (tag);
    g_free (tag);
  }
  T_INFO ("%s: new text ink box %d,%d %dx%d", c->tag, ink.x, ink.y, ink.w, ink.h);

  /* step 4 */
  check_after (c, win, old_rect, right_half, ink, want);

  gtk_widget_destroy (win);
  t_settle (100);
}

int
main (int argc, char **argv)
{
  Case a = { "A-toplevel", K_LABEL_WINDOW, "#f2e6c8", NULL, "i" };
  Case b = { "B-eventbox", K_LABEL_EVENTBOX, "#c8d8f2", "#a8e0a8", "M" };
  Case c = { "C-viewport", K_LABEL_VIEWPORT, "#c8d8f2", "#e0b8d8", "M" };
  Case d = { "D-drawingarea", K_DRAWING_AREA, "#c8d8f2", "#f0d890", "i" };

  t_begin ("T0.2", &argc, &argv);
  run_case (&a);
  run_case (&b);
  run_case (&c);
  run_case (&d);
  return t_finish ();
}
