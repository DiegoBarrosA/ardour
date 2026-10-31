/*
 * T0.5  A scrolled tree view: scrolling must equal a full redraw.
 *
 * A GtkTreeView with 3000 rows sits in a GtkScrolledWindow.  The program
 * scrolls to several offsets (the tree view moves the bin window with
 * gdk_window_scroll and only exposes the newly uncovered strip).  After each
 * scroll it reads the toplevel pixels (A), invalidates the whole widget so
 * everything is drawn again, and reads the pixels again (B).  A must equal B.
 * It also checks that the picture really changed when the offset changed, and
 * that a row selection draws.
 */
#include "common.h"

static gboolean
exposed (gpointer p)
{
  return ((TWatch *) p)->expose > 0;
}

int
main (int argc, char **argv)
{
  GtkWidget *win, *sw, *tv;
  GtkListStore *store;
  GtkCellRenderer *r;
  TWatch watch;
  GtkAdjustment *vadj;
  TShot *prev = NULL;
  double offsets[] = { 300.0, 17.0, 1234.0, 5.0 };
  int i;

  t_begin ("T0.5", &argc, &argv);

  store = gtk_list_store_new (3, G_TYPE_STRING, G_TYPE_INT, G_TYPE_STRING);
  for (i = 0; i < 3000; i++)
    {
      GtkTreeIter it;
      char *a = g_strdup_printf ("Row number %d", i);
      char *b = g_strdup_printf ("%x%x", i * 2654435761u, i);
      gtk_list_store_append (store, &it);
      gtk_list_store_set (store, &it, 0, a, 1, i, 2, b, -1);
      g_free (a);
      g_free (b);
    }
  tv = gtk_tree_view_new_with_model (GTK_TREE_MODEL (store));
  g_object_unref (store);
  r = gtk_cell_renderer_text_new ();
  gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (tv), -1, "Name", r, "text", 0, NULL);
  r = gtk_cell_renderer_text_new ();
  gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (tv), -1, "Number", r, "text", 1, NULL);
  r = gtk_cell_renderer_text_new ();
  gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (tv), -1, "Hash", r, "text", 2, NULL);
  gtk_tree_view_set_rules_hint (GTK_TREE_VIEW (tv), TRUE);

  sw = gtk_scrolled_window_new (NULL, NULL);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_ALWAYS);
  gtk_container_add (GTK_CONTAINER (sw), tv);
  win = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  gtk_window_set_default_size (GTK_WINDOW (win), 360, 260);
  gtk_container_add (GTK_CONTAINER (win), sw);
  t_watch (win, &watch);
  gtk_widget_show_all (win);
  T_CHECK (t_wait_until (exposed, &watch, 3000), "no expose within 3 s");
  t_settle (400);

  vadj = gtk_tree_view_get_vadjustment (GTK_TREE_VIEW (tv));
  T_INFO ("adjustment upper=%.0f page=%.0f", gtk_adjustment_get_upper (vadj), gtk_adjustment_get_page_size (vadj));
  T_CHECK (gtk_adjustment_get_upper (vadj) > gtk_adjustment_get_page_size (vadj) * 5,
           "the tree view is not taller than its window");

  prev = t_capture_toplevel (win);
  T_CHECK (prev != NULL, "cannot read the window pixels");
  if (!prev)
    return t_finish ();

  for (i = 0; i < (int) G_N_ELEMENTS (offsets); i++)
    {
      TShot *a, *b;
      int d, moved;
      char tag[32];

      gtk_adjustment_set_value (vadj, offsets[i]);
      t_settle (300);
      a = t_capture_toplevel (win);

      gtk_widget_queue_draw (win);
      t_settle (300);
      b = t_capture_toplevel (win);

      T_CHECK (a && b, "offset %.0f: cannot read pixels", offsets[i]);
      if (a && b)
        {
          d = t_diff (a, b);
          moved = t_diff (a, prev);
          T_INFO ("offset %.0f: scroll vs full redraw differ in %d pixels, differ from the previous offset in %d",
                  offsets[i], d, moved);
          T_CHECK (d == 0, "offset %.0f: %d pixels differ between the scrolled picture and a full redraw",
                   offsets[i], d);
          T_CHECK (moved > 500, "offset %.0f: the picture did not change (%d pixels)", offsets[i], moved);
          if (d)
            {
              g_snprintf (tag, sizeof tag, "scrolled-%d", (int) offsets[i]);
              t_save (a, tag);
              g_snprintf (tag, sizeof tag, "redraw-%d", (int) offsets[i]);
              t_save (b, tag);
            }
        }
      t_shot_free (prev);
      prev = b;
      t_shot_free (a);
    }

  /* selection draws, and scrolling to a cell moves the view */
  {
    GtkTreePath *path = gtk_tree_path_new_from_indices (60, -1);
    TShot *a, *b;
    int d, sel;
    gtk_tree_view_scroll_to_cell (GTK_TREE_VIEW (tv), path, NULL, TRUE, 0.5f, 0.0f);
    gtk_tree_selection_select_path (gtk_tree_view_get_selection (GTK_TREE_VIEW (tv)), path);
    gtk_tree_path_free (path);
    t_settle (300);
    a = t_capture_toplevel (win);
    gtk_widget_queue_draw (win);
    t_settle (300);
    b = t_capture_toplevel (win);
    if (a && b)
      {
        d = t_diff (a, b);
        sel = t_diff (b, prev);
        T_INFO ("scroll_to_cell and select: scroll vs redraw differ in %d pixels", d);
        T_CHECK (d == 0, "after scroll_to_cell and select: %d pixels differ between scrolled and redrawn", d);
        T_CHECK (sel > 500, "scroll_to_cell and select did not change the picture");
      }
    else
      T_CHECK (FALSE, "cannot read pixels after scroll_to_cell");
    t_shot_free (a);
    t_shot_free (b);
  }
  t_shot_free (prev);

  gtk_widget_destroy (win);
  t_settle (100);
  return t_finish ();
}
