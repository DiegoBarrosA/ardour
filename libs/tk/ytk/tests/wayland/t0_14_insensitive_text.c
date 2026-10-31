/*
 * T0.14  Text of insensitive widgets is drawn.
 *
 * Diego saw greyed out combo boxes, entries and labels as empty boxes on
 * sway.  This test builds eight widget kinds (label, entry, combo box, combo
 * box entry, button, check button, spin button, tree view) in four copies:
 * sensitive with text, sensitive without text, insensitive with text,
 * insensitive without text.  All copies of one kind have the same size
 * request.  The ink of a copy is the number of pixels in its rectangle that
 * differ from the same pixels of the copy without text.  A sensitive widget
 * must have ink (control), an insensitive one must have ink too.
 * Run under the default theme and under the clearlooks engine.
 * Set T_CLEARLOOKS=0 to skip the clearlooks phase.
 */
#include "common.h"

#define N_KINDS 11
#define MIN_INK 12

static const char *kind_name[N_KINDS] = { "label", "entry", "combo", "comboentry",
                                          "button", "check", "spin", "treeview",
                                          "label-markup", "entry-modified", "combo-modified" };

static GtkWidget *
make (int kind, gboolean with_text)
{
  const char *txt = with_text ? "Sample" : "";
  GtkWidget *w = NULL;

  switch (kind)
    {
    case 0:
      w = gtk_label_new (txt);
      gtk_misc_set_alignment (GTK_MISC (w), 0, 0.5);
      break;
    case 1:
      w = gtk_entry_new ();
      gtk_entry_set_text (GTK_ENTRY (w), txt);
      break;
    case 2:
      w = gtk_combo_box_new_text ();
      gtk_combo_box_append_text (GTK_COMBO_BOX (w), with_text ? "Sample" : " ");
      gtk_combo_box_set_active (GTK_COMBO_BOX (w), 0);
      break;
    case 3:
      w = gtk_combo_box_entry_new_text ();
      gtk_entry_set_text (GTK_ENTRY (gtk_bin_get_child (GTK_BIN (w))), txt);
      break;
    case 4:
      w = gtk_button_new_with_label (txt);
      break;
    case 5:
      w = gtk_check_button_new_with_label (txt);
      break;
    case 6:
      w = gtk_spin_button_new_with_range (0, 100, 1);
      gtk_spin_button_set_value (GTK_SPIN_BUTTON (w), 42);
      gtk_entry_set_text (GTK_ENTRY (w), with_text ? "42" : "");
      break;
    case 7:
      {
        GtkListStore *st = gtk_list_store_new (1, G_TYPE_STRING);
        GtkTreeIter it;
        int i;
        for (i = 0; with_text && i < 3; i++)
          {
            gtk_list_store_append (st, &it);
            gtk_list_store_set (st, &it, 0, "Sample", -1);
          }
        w = gtk_tree_view_new_with_model (GTK_TREE_MODEL (st));
        g_object_unref (st);
        gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (w), -1, "Col",
                                                     gtk_cell_renderer_text_new (), "text", 0, NULL);
        gtk_tree_view_set_headers_visible (GTK_TREE_VIEW (w), FALSE);
        break;
      }
    case 8:
      w = gtk_label_new (NULL);
      gtk_label_set_markup (GTK_LABEL (w), with_text ? "<span foreground=\"#cc2222\">Sample</span>" : "");
      gtk_misc_set_alignment (GTK_MISC (w), 0, 0.5);
      break;
    case 9:
      {
        GdkColor c = { 0, 0x9999, 0x3333, 0x3333 };
        w = gtk_entry_new ();
        gtk_entry_set_text (GTK_ENTRY (w), txt);
        gtk_widget_modify_text (w, GTK_STATE_INSENSITIVE, &c);
        break;
      }
    case 10:
      {
        GdkColor c = { 0, 0x9999, 0x3333, 0x3333 };
        w = gtk_combo_box_new_text ();
        gtk_combo_box_append_text (GTK_COMBO_BOX (w), with_text ? "Sample" : " ");
        gtk_combo_box_set_active (GTK_COMBO_BOX (w), 0);
        gtk_widget_modify_text (w, GTK_STATE_INSENSITIVE, &c);
        gtk_widget_modify_fg (w, GTK_STATE_INSENSITIVE, &c);
        break;
      }
    }
  gtk_widget_set_size_request (w, 130, kind == 7 ? 70 : 30);
  return w;
}

static gboolean
exposed (gpointer p)
{
  return ((TWatch *) p)->expose > 0;
}

/* Pixels of @a that differ from the pixels of @b at the same offset. */
static int
rect_diff (const TShot *s, TRect a, TRect b)
{
  int x, y, n = 0;
  for (y = 0; y < a.h; y++)
    for (x = 0; x < a.w; x++)
      if (t_px (s, a.x + x, a.y + y) != t_px (s, b.x + x, b.y + y))
        n++;
  return n;
}

static void
run_phase (const char *phase)
{
  GtkWidget *win, *table, *w[N_KINDS][4];
  TWatch watch;
  TShot *shot;
  int k, c;

  g_print ("%s: phase %s\n", t_name, phase);
  win = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  table = gtk_table_new (N_KINDS, 4, FALSE);
  gtk_table_set_col_spacings (GTK_TABLE (table), 8);
  gtk_table_set_row_spacings (GTK_TABLE (table), 6);
  gtk_container_set_border_width (GTK_CONTAINER (table), 8);
  gtk_container_add (GTK_CONTAINER (win), table);
  for (k = 0; k < N_KINDS; k++)
    for (c = 0; c < 4; c++)
      {
        /* columns: sensitive text, sensitive empty, insensitive text, insensitive empty */
        w[k][c] = make (k, (c & 1) == 0);
        if (c >= 2)
          gtk_widget_set_sensitive (w[k][c], FALSE);
        gtk_table_attach (GTK_TABLE (table), w[k][c], c, c + 1, k, k + 1, GTK_FILL, GTK_FILL, 0, 0);
      }
  t_watch (win, &watch);
  gtk_widget_show_all (win);
  T_CHECK (t_wait_until (exposed, &watch, 2000), "%s: no expose within 2 s", phase);
  t_settle (400);
  /* A spin button rewrites its text while it is mapped, so empty it now. */
  gtk_entry_set_text (GTK_ENTRY (w[6][1]), "");
  gtk_entry_set_text (GTK_ENTRY (w[6][3]), "");
  t_settle (200);

  shot = t_capture_toplevel (win);
  T_CHECK (shot != NULL, "%s: cannot read the window pixels", phase);
  if (shot)
    {
      for (k = 0; k < N_KINDS; k++)
        {
          int ink_s = rect_diff (shot, t_widget_rect (w[k][0], win), t_widget_rect (w[k][1], win));
          int ink_i = rect_diff (shot, t_widget_rect (w[k][2], win), t_widget_rect (w[k][3], win));
          T_INFO ("%s: %s ink sensitive %d insensitive %d", phase, kind_name[k], ink_s, ink_i);
          T_CHECK (ink_s > MIN_INK, "%s: sensitive %s has only %d ink pixels", phase, kind_name[k], ink_s);
          T_CHECK (ink_i > MIN_INK, "%s: insensitive %s has only %d ink pixels (text not drawn)",
                   phase, kind_name[k], ink_i);
        }
      if (t_nfail)
        t_save (shot, phase);
      t_shot_free (shot);
    }
  t_shot_point (phase);
  gtk_widget_destroy (win);
  t_settle (100);
}

int
main (int argc, char **argv)
{
  const char *cl = g_getenv ("T_CLEARLOOKS");

  t_begin ("T0.14", &argc, &argv);
  run_phase ("default-theme");
  if (cl && strcmp (cl, "0") == 0)
    T_INFO ("clearlooks phase skipped (T_CLEARLOOKS=0)");
  else
    {
      gtk_rc_parse_string ("style \"cl\" { engine \"clearlooks\" { } }\n"
                           "class \"GtkWidget\" style \"cl\"\n");
      run_phase ("clearlooks");
    }
  return t_finish ();
}
