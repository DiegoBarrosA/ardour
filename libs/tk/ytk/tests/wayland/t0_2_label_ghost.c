/*
 * T0.2  The H1 ghost test: a label that shrinks must not leave old ink.
 *
 * Case A: a GtkLabel directly in a toplevel with a flat background.
 * Case B: a GtkLabel (no window) inside a GtkEventBox with its own flat
 *         background, inside a toplevel with another background.
 *
 * Both cases do the same:
 *   1. Show a long bold string and let the first frame commit.
 *   2. Record the label allocation and check the long text really drew ink in
 *      the right half of it (otherwise the test proves nothing).
 *   3. Shrink the text to "i" (or "M") and let the redraw run.
 *   4. Read the toplevel pixels.  Every pixel of the old allocation outside
 *      the ink box of the new text must have the background colour.  In
 *      particular the right half of the old allocation must hold no ink.
 *
 * A backend that does not clear the exposed region before the double buffered
 * widget draws leaves the old glyph pixels behind (ghosting, finding H1).
 * Pixels are read through the TOPLEVEL window.
 */
#include "common.h"

#define LONG_TEXT  "MMMMMMMMMMMMMMMMMMMM"

typedef struct
{
  const char *tag;
  gboolean    use_eventbox;
  const char *win_bg;
  const char *box_bg;
  const char *long_text;
  const char *short_text;
} Case;

static gboolean
exposed (gpointer p)
{
  return ((TWatch *) p)->expose > 0;
}

static void
run_case (const Case *c)
{
  GtkWidget *win, *label, *box = NULL;
  GdkColor wbg, bbg;
  TWatch watch;
  PangoAttrList *attrs;
  TRect old_rect, right_half, ink;
  TShot *before, *after;
  guint32 want;
  int ox, oy, n_before, n_after_all, n_after_right, n_new_ink;
  PangoRectangle pink;
  int lx, ly;

  g_print ("%s: case %s\n", t_name, c->tag);

  win = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  gtk_window_set_default_size (GTK_WINDOW (win), 200, 60);
  gdk_color_parse (c->win_bg, &wbg);
  gtk_widget_modify_bg (win, GTK_STATE_NORMAL, &wbg);
  want = t_color_rgb (&wbg);

  label = gtk_label_new (c->long_text);
  gtk_misc_set_alignment (GTK_MISC (label), 0.0f, 0.5f);
  attrs = pango_attr_list_new ();
  pango_attr_list_insert (attrs, pango_attr_weight_new (PANGO_WEIGHT_BOLD));
  pango_attr_list_insert (attrs, pango_attr_size_new (14 * PANGO_SCALE));
  gtk_label_set_attributes (GTK_LABEL (label), attrs);
  pango_attr_list_unref (attrs);

  if (c->use_eventbox)
    {
      box = gtk_event_box_new ();
      gdk_color_parse (c->box_bg, &bbg);
      gtk_widget_modify_bg (box, GTK_STATE_NORMAL, &bbg);
      want = t_color_rgb (&bbg);
      gtk_container_add (GTK_CONTAINER (box), label);
      gtk_container_add (GTK_CONTAINER (win), box);
    }
  else
    gtk_container_add (GTK_CONTAINER (win), label);

  t_watch (c->use_eventbox ? box : win, &watch); /* the event box covers the toplevel */
  gtk_widget_show_all (win);
  T_CHECK (t_wait_until (exposed, &watch, 2000), "%s: no expose within 2 s", c->tag);
  t_settle (300);
  t_shot_point (c->use_eventbox ? "B-long" : "A-long");

  /* step 2 */
  old_rect = t_widget_rect (label, win);
  T_INFO ("%s: old label allocation %d,%d %dx%d, window %dx%d", c->tag, old_rect.x,
          old_rect.y, old_rect.w, old_rect.h, win->allocation.width, win->allocation.height);
  T_CHECK (old_rect.w > 40 && old_rect.h > 8, "%s: label allocation %dx%d too small", c->tag,
           old_rect.w, old_rect.h);
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
  T_INFO ("%s: long text left %d ink pixels in the right half of the old allocation", c->tag,
          n_before);
  T_CHECK (n_before > 30, "%s: long text drew only %d pixels in the right half, the test would be vacuous",
           c->tag, n_before);
  t_shot_free (before);

  /* step 3 */
  gtk_label_set_text (GTK_LABEL (label), c->short_text);
  t_settle (300);
  t_shot_point (c->use_eventbox ? "B-short" : "A-short");

  /* step 4 */
  gtk_label_get_layout_offsets (GTK_LABEL (label), &ox, &oy);
  pango_layout_get_pixel_extents (gtk_label_get_layout (GTK_LABEL (label)), &pink, NULL);
  /* ox,oy are in the coordinates of the label's parent window; label->allocation
   * is in the same system, so subtract it and add the toplevel position. */
  lx = old_rect.x + (ox - label->allocation.x);
  ly = old_rect.y + (oy - label->allocation.y);
  ink.x = lx + pink.x - 3;
  ink.y = ly + pink.y - 3;
  ink.w = pink.width + 6;
  ink.h = pink.height + 6;
  T_INFO ("%s: new text ink box %d,%d %dx%d", c->tag, ink.x, ink.y, ink.w, ink.h);

  after = t_capture_toplevel (win);
  T_CHECK (after != NULL, "%s: cannot read the window pixels after the change", c->tag);
  if (after)
    {
      n_after_all = t_count_not (after, old_rect, want, &ink);
      n_after_right = t_count_not (after, right_half, want, NULL);
      n_new_ink = t_count_not (after, ink, want, NULL);
      T_INFO ("%s: after the change: %d stale pixels outside the new ink box, %d in the right half, %d ink pixels of the new text",
              c->tag, n_after_all, n_after_right, n_new_ink);
      T_CHECK (n_after_right == 0, "%s: ghosting, %d pixels of old ink in the right half of the old allocation",
               c->tag, n_after_right);
      T_CHECK (n_after_all == 0, "%s: ghosting, %d pixels of old ink outside the new text", c->tag,
               n_after_all);
      T_CHECK (n_new_ink > 0, "%s: the new text drew no ink", c->tag);
      if (n_after_all || n_after_right)
        t_save (after, c->tag);
      t_shot_free (after);
    }

  gtk_widget_destroy (win);
  t_settle (100);
}

int
main (int argc, char **argv)
{
  Case a = { "A-toplevel", FALSE, "#f2e6c8", NULL, LONG_TEXT, "i" };
  Case b = { "B-eventbox", TRUE, "#c8d8f2", "#a8e0a8", LONG_TEXT, "M" };

  t_begin ("T0.2", &argc, &argv);
  run_case (&a);
  run_case (&b);
  return t_finish ();
}
