/*
 * T0.3  Common widgets draw under the default theme and the clearlooks engine.
 *
 * Phase 1 builds a window with a button, check and radio buttons, an entry, a
 * combo box, a spin button, a scale, a progress bar, a frame, a notebook, a
 * tree view and a statusbar, using the built-in default theme.
 * Phase 2 loads the clearlooks engine of the Ardour tree
 * (build/libs/clearlooks-newer, found through GTK_PATH) with an rc string and
 * builds the same window again.
 *
 * Checks per phase: the window maps and exposes, every widget got an
 * allocation, every widget painted something that differs from the window
 * background, and the window shows many distinct colours.  For phase 2 the
 * button style must be a ClearlooksStyle.  Set T_CLEARLOOKS=0 to skip phase 2
 * when the engine is not built.
 */
#include "common.h"

#define N_PROBES 12

typedef struct
{
  GtkWidget *win;
  GtkWidget *probe[N_PROBES];
  const char *pname[N_PROBES];
  TWatch     watch;
} Ui;

static gboolean
exposed (gpointer p)
{
  return ((TWatch *) p)->expose > 0;
}

static void
add_probe (Ui *ui, int *n, const char *name, GtkWidget *w)
{
  ui->probe[*n] = w;
  ui->pname[*n] = name;
  (*n)++;
}

static GtkWidget *
build_ui (Ui *ui)
{
  GtkWidget *vbox, *hbox, *button, *check, *radio1, *radio2, *entry, *combo, *spin, *scale,
    *progress, *frame, *nb, *tv, *status, *sw;
  GtkListStore *store;
  GtkCellRenderer *r;
  int n = 0, i;

  memset (ui, 0, sizeof *ui);
  ui->win = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  gtk_window_set_default_size (GTK_WINDOW (ui->win), 460, 420);
  vbox = gtk_vbox_new (FALSE, 4);
  gtk_container_set_border_width (GTK_CONTAINER (vbox), 6);
  gtk_container_add (GTK_CONTAINER (ui->win), vbox);

  hbox = gtk_hbox_new (FALSE, 6);
  button = gtk_button_new_with_label ("Button");
  check = gtk_check_button_new_with_label ("Check");
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (check), TRUE);
  radio1 = gtk_radio_button_new_with_label (NULL, "Radio 1");
  radio2 = gtk_radio_button_new_with_label_from_widget (GTK_RADIO_BUTTON (radio1), "Radio 2");
  gtk_box_pack_start (GTK_BOX (hbox), button, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (hbox), check, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (hbox), radio1, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (hbox), radio2, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (vbox), hbox, FALSE, FALSE, 0);
  add_probe (ui, &n, "button", button);
  add_probe (ui, &n, "check", check);
  add_probe (ui, &n, "radio", radio1);

  entry = gtk_entry_new ();
  gtk_entry_set_text (GTK_ENTRY (entry), "Entry text");
  gtk_box_pack_start (GTK_BOX (vbox), entry, FALSE, FALSE, 0);
  add_probe (ui, &n, "entry", entry);

  hbox = gtk_hbox_new (FALSE, 6);
  combo = gtk_combo_box_new_text ();
  gtk_combo_box_append_text (GTK_COMBO_BOX (combo), "One");
  gtk_combo_box_append_text (GTK_COMBO_BOX (combo), "Two");
  gtk_combo_box_set_active (GTK_COMBO_BOX (combo), 0);
  spin = gtk_spin_button_new_with_range (0, 100, 1);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (spin), 42);
  gtk_box_pack_start (GTK_BOX (hbox), combo, TRUE, TRUE, 0);
  gtk_box_pack_start (GTK_BOX (hbox), spin, TRUE, TRUE, 0);
  gtk_box_pack_start (GTK_BOX (vbox), hbox, FALSE, FALSE, 0);
  add_probe (ui, &n, "combo", combo);
  add_probe (ui, &n, "spin", spin);

  scale = gtk_hscale_new_with_range (0, 100, 1);
  gtk_range_set_value (GTK_RANGE (scale), 30);
  gtk_box_pack_start (GTK_BOX (vbox), scale, FALSE, FALSE, 0);
  add_probe (ui, &n, "scale", scale);

  progress = gtk_progress_bar_new ();
  gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (progress), 0.5);
  gtk_progress_bar_set_text (GTK_PROGRESS_BAR (progress), "50 percent");
  gtk_box_pack_start (GTK_BOX (vbox), progress, FALSE, FALSE, 0);
  add_probe (ui, &n, "progress", progress);

  frame = gtk_frame_new ("Frame");
  gtk_container_add (GTK_CONTAINER (frame), gtk_label_new ("inside the frame"));
  gtk_box_pack_start (GTK_BOX (vbox), frame, FALSE, FALSE, 0);
  add_probe (ui, &n, "frame", frame);

  nb = gtk_notebook_new ();
  gtk_notebook_append_page (GTK_NOTEBOOK (nb), gtk_label_new ("page one"), gtk_label_new ("Tab 1"));
  gtk_notebook_append_page (GTK_NOTEBOOK (nb), gtk_label_new ("page two"), gtk_label_new ("Tab 2"));
  gtk_box_pack_start (GTK_BOX (vbox), nb, FALSE, FALSE, 0);
  add_probe (ui, &n, "notebook", nb);

  store = gtk_list_store_new (2, G_TYPE_STRING, G_TYPE_INT);
  for (i = 0; i < 20; i++)
    {
      GtkTreeIter it;
      char *s = g_strdup_printf ("Row %d", i);
      gtk_list_store_append (store, &it);
      gtk_list_store_set (store, &it, 0, s, 1, i * 7, -1);
      g_free (s);
    }
  tv = gtk_tree_view_new_with_model (GTK_TREE_MODEL (store));
  g_object_unref (store);
  r = gtk_cell_renderer_text_new ();
  gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (tv), -1, "Name", r, "text", 0, NULL);
  r = gtk_cell_renderer_text_new ();
  gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (tv), -1, "Value", r, "text", 1, NULL);
  sw = gtk_scrolled_window_new (NULL, NULL);
  gtk_container_add (GTK_CONTAINER (sw), tv);
  gtk_box_pack_start (GTK_BOX (vbox), sw, TRUE, TRUE, 0);
  add_probe (ui, &n, "treeview", tv);

  status = gtk_statusbar_new ();
  gtk_statusbar_push (GTK_STATUSBAR (status), 0, "Statusbar message");
  gtk_box_pack_end (GTK_BOX (vbox), status, FALSE, FALSE, 0);
  add_probe (ui, &n, "statusbar", status);

  g_assert (n <= N_PROBES);
  return button;
}

static void
run_phase (const char *phase, gboolean want_clearlooks)
{
  Ui ui;
  GtkWidget *button;
  TShot *shot;
  guint32 bg;
  int i, painted = 0, colours;
  const char *style_name;

  g_print ("%s: phase %s\n", t_name, phase);
  button = build_ui (&ui);
  t_watch (ui.win, &ui.watch);
  gtk_widget_show_all (ui.win);
  T_CHECK (t_wait_until (exposed, &ui.watch, 2000), "%s: no expose within 2 s", phase);
  t_settle (400);

  style_name = G_OBJECT_TYPE_NAME (gtk_widget_get_style (button));
  T_INFO ("%s: button style type is %s", phase, style_name);
  if (want_clearlooks)
    T_CHECK (strcmp (style_name, "ClearlooksStyle") == 0,
             "%s: expected ClearlooksStyle, the engine did not load (GTK_PATH=%s)", phase,
             g_getenv ("GTK_PATH") ? g_getenv ("GTK_PATH") : "(unset)");
  else
    T_CHECK (strcmp (style_name, "ClearlooksStyle") != 0, "%s: the clearlooks engine is active in the default phase", phase);

  bg = t_color_rgb (&gtk_widget_get_style (ui.win)->bg[GTK_STATE_NORMAL]);
  shot = t_capture_toplevel (ui.win);
  T_CHECK (shot != NULL, "%s: cannot read the window pixels", phase);
  if (shot)
    {
      for (i = 0; i < N_PROBES && ui.probe[i]; i++)
        {
          GtkWidget *w = ui.probe[i];
          TRect r = t_widget_rect (w, ui.win);
          int ink;
          T_CHECK (gtk_widget_get_mapped (w), "%s: %s is not mapped", phase, ui.pname[i]);
          T_CHECK (r.w > 4 && r.h > 4, "%s: %s has allocation %dx%d", phase, ui.pname[i], r.w, r.h);
          ink = t_count_not (shot, r, bg, NULL);
          T_CHECK (ink > 20, "%s: %s painted only %d pixels that differ from the window background %06x",
                   phase, ui.pname[i], ink, bg);
          if (ink > 20)
            painted++;
        }
      colours = t_distinct_colors (shot, 0);
      T_INFO ("%s: %d widgets painted, %d distinct colours in the window", phase, painted, colours);
      T_CHECK (colours >= 12, "%s: only %d distinct colours", phase, colours);
      if (t_nfail)
        t_save (shot, phase);
      t_shot_free (shot);
    }
  t_shot_point (phase);

  gtk_widget_destroy (ui.win);
  t_settle (100);
}

int
main (int argc, char **argv)
{
  const char *cl = g_getenv ("T_CLEARLOOKS");

  t_begin ("T0.3", &argc, &argv);
  run_phase ("default-theme", FALSE);

  if (cl && strcmp (cl, "0") == 0)
    T_INFO ("clearlooks phase skipped (T_CLEARLOOKS=0)");
  else
    {
      gtk_rc_parse_string ("style \"cl\" { engine \"clearlooks\" { } }\n"
                           "class \"GtkWidget\" style \"cl\"\n");
      run_phase ("clearlooks", TRUE);
    }
  return t_finish ();
}
