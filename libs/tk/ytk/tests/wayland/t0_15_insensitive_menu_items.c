/*
 * T0.15  Text of insensitive menu items is drawn.
 *
 * Diego saw the unselectable items of a dropdown menu on sway as empty menu
 * background.  T0.14 covers the insensitive widgets but not GtkMenuItem inside
 * a GtkMenu.  This test builds a menu with four kinds of items (label, check,
 * markup label, submenu), each as a sensitive and an insensitive item, and a
 * separator between them.  The ink of an item is the number of pixels in the
 * band between its indicator columns that differ from the menu colour at the
 * edge of the item.  A sensitive item must have ink (control), an insensitive
 * item must have ink too.  The numbers are printed, so the same run on X11
 * and on Wayland can be compared.
 *
 * Phase "in-window": the items sit in a vertical menu bar in a toplevel (a GtkMenu has
 * its own toplevel and cannot be a window child).  It needs no seat, so
 * it runs on weston and Xvfb.  Phase "popup": the real popup, run only when the
 * runner sets T_HAVE_SEAT=1 (Xvfb, sway) or T_FORCE=1.  Each phase runs under
 * the default theme and under the clearlooks engine (T_CLEARLOOKS=0 skips the
 * second).
 */
#include "common.h"

#define N_KINDS 4
#define MIN_INK 12
#define MARGIN 28

static const char *kind_name[N_KINDS] = { "label", "check", "markup", "submenu" };

typedef struct
{
  GtkWidget *menu;
  GtkWidget *item[N_KINDS][2];  /* [kind][0] sensitive, [kind][1] insensitive */
} TMenu;

static void
build_menu (TMenu *m, gboolean bar)
{
  int k, s;

  if (bar)
    {
      m->menu = gtk_menu_bar_new ();
      gtk_menu_bar_set_pack_direction (GTK_MENU_BAR (m->menu), GTK_PACK_DIRECTION_TTB);
    }
  else
    m->menu = gtk_menu_new ();
  for (k = 0; k < N_KINDS; k++)
    for (s = 0; s < 2; s++)
      {
        GtkWidget *it = NULL, *sub;

        switch (k)
          {
          case 0:
            it = gtk_menu_item_new_with_label ("Sample label");
            break;
          case 1:
            it = gtk_check_menu_item_new_with_label ("Sample check");
            gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (it), TRUE);
            break;
          case 2:
            it = gtk_menu_item_new_with_label ("");
            gtk_label_set_markup (GTK_LABEL (gtk_bin_get_child (GTK_BIN (it))),
                                  "<span foreground=\"#cc2222\">Sample markup</span>");
            break;
          case 3:
            it = gtk_menu_item_new_with_label ("Sample submenu");
            sub = gtk_menu_new ();
            gtk_menu_shell_append (GTK_MENU_SHELL (sub), gtk_menu_item_new_with_label ("Inner"));
            gtk_menu_item_set_submenu (GTK_MENU_ITEM (it), sub);
            break;
          }
        gtk_widget_set_sensitive (it, s == 0);
        gtk_menu_shell_append (GTK_MENU_SHELL (m->menu), it);
        m->item[k][s] = it;
      }
  gtk_menu_shell_insert (GTK_MENU_SHELL (m->menu), gtk_separator_menu_item_new (), 2);
  gtk_widget_show_all (m->menu);
}

/* Largest green-channel distance between a pixel of the band and the menu colour. */
static int
band_contrast (const TShot *shot, TRect band, guint32 bg)
{
  int x, y, best = 0;

  for (y = MAX (band.y, 0); y < MIN (band.y + band.h, shot->h); y++)
    for (x = MAX (band.x, 0); x < MIN (band.x + band.w, shot->w); x++)
      best = MAX (best, abs ((int) ((t_px (shot, x, y) >> 8) & 0xff) - (int) ((bg >> 8) & 0xff)));
  return best;
}

static int last_contrast;

static int
item_ink (const TShot *shot, TRect r)
{
  TRect band = { r.x + MARGIN, r.y, r.w - 2 * MARGIN, r.h };
  guint32 bg = t_px (shot, r.x + r.w - 2, r.y + r.h / 2);

  last_contrast = band_contrast (shot, band, bg);
  return t_count_not (shot, band, bg, NULL);
}

static void
check_items (const char *phase, TMenu *m, GtkWidget *tl)
{
  TShot *shot = t_capture_toplevel (tl);
  int k;

  T_CHECK (shot != NULL, "%s: cannot read the menu pixels", phase);
  if (!shot)
    return;
  for (k = 0; k < N_KINDS; k++)
    {
      int ink_s = item_ink (shot, t_widget_rect (m->item[k][0], tl));
      int ink_i = item_ink (shot, t_widget_rect (m->item[k][1], tl));

      T_INFO ("%s: %s ink sensitive %d insensitive %d, contrast insensitive %d of 255", phase, kind_name[k], ink_s, ink_i, last_contrast);
      T_CHECK (ink_s > MIN_INK, "%s: sensitive %s item has only %d ink pixels", phase, kind_name[k], ink_s);
      T_CHECK (ink_i > MIN_INK, "%s: insensitive %s item has only %d ink pixels (text not drawn)",
               phase, kind_name[k], ink_i);
    }
  if (t_nfail)
    t_save (shot, phase);
  t_shot_free (shot);
}

static gboolean
menu_mapped (gpointer p)
{
  GtkWidget *tl = GTK_MENU (p)->toplevel;
  return tl && gtk_widget_get_mapped (tl) && gtk_widget_get_mapped (p);
}

static void
pos_func (GtkMenu *menu, gint *x, gint *y, gboolean *push_in, gpointer data)
{
  gint ox = 0, oy = 0;
  gdk_window_get_origin (GTK_WIDGET (data)->window, &ox, &oy);
  *x = ox + 20;
  *y = oy + 20;
  *push_in = TRUE;
}

static void
run_window (const char *phase)
{
  TMenu m;
  GtkWidget *win = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  char *name = g_strdup_printf ("%s-in-window", phase);

  build_menu (&m, TRUE);
  gtk_container_add (GTK_CONTAINER (win), m.menu);
  gtk_widget_show_all (win);
  t_settle (600);
  check_items (name, &m, win);
  t_shot_point (name);
  gtk_widget_destroy (win);
  t_settle (100);
  g_free (name);
}

static void
run_popup (const char *phase)
{
  TMenu m;
  GtkWidget *win = gtk_window_new (GTK_WINDOW_TOPLEVEL), *tl;
  char *name = g_strdup_printf ("%s-popup", phase);

  gtk_window_set_default_size (GTK_WINDOW (win), 320, 420);
  gtk_widget_show_all (win);
  t_settle (400);
  build_menu (&m, FALSE);
  gtk_menu_popup (GTK_MENU (m.menu), NULL, NULL, pos_func, win, 0, gtk_get_current_event_time ());
  T_CHECK (t_wait_until (menu_mapped, m.menu, 2000), "%s: the menu did not map within 2 s", name);
  t_settle (400);
  tl = GTK_MENU (m.menu)->toplevel;
  if (tl && tl->window)
    {
      check_items (name, &m, tl);
      t_shot_point (name);
    }
  gtk_menu_popdown (GTK_MENU (m.menu));
  t_settle (200);
  gtk_widget_destroy (win);
  t_settle (100);
  g_free (name);
}

static GtkWidget *
find_popup_menu (GtkWidget *skip)
{
  GList *l = gtk_window_list_toplevels (), *i;
  GtkWidget *menu = NULL;

  for (i = l; i; i = i->next)
    {
      GtkWidget *w = i->data, *c;
      if (w == skip || !gtk_widget_get_mapped (w) || !(c = gtk_bin_get_child (GTK_BIN (w))) || !GTK_IS_MENU (c))
        continue;
      menu = w;
    }
  g_list_free (l);
  return menu;
}

/* A combo box whose rows 1, 3 and 5 are insensitive: the menu items of a combo box draw the text
 * through a cell view with the "text" colours of the theme, not the "fg" ones of a label. */
static void
run_combo (const char *phase)
{
  GtkWidget *win = gtk_window_new (GTK_WINDOW_TOPLEVEL), *combo, *tl, *menu;
  GtkListStore *st = gtk_list_store_new (2, G_TYPE_STRING, G_TYPE_BOOLEAN);
  GtkCellRenderer *cr = gtk_cell_renderer_text_new ();
  GtkTreeIter it;
  char *name = g_strdup_printf ("%s-combo", phase);
  int i;

  for (i = 0; i < 7; i++)
    {
      char *s = g_strdup_printf ("%d samples", 16 << i);
      gtk_list_store_append (st, &it);
      gtk_list_store_set (st, &it, 0, s, 1, (i & 1) == 0, -1);
      g_free (s);
    }
  combo = gtk_combo_box_new_with_model (GTK_TREE_MODEL (st));
  gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (combo), cr, TRUE);
  gtk_cell_layout_set_attributes (GTK_CELL_LAYOUT (combo), cr, "text", 0, "sensitive", 1, NULL);
  gtk_combo_box_set_active (GTK_COMBO_BOX (combo), 0);
  gtk_container_add (GTK_CONTAINER (win), combo);
  gtk_window_set_default_size (GTK_WINDOW (win), 240, 300);
  gtk_widget_show_all (win);
  t_settle (400);
  gtk_combo_box_popup (GTK_COMBO_BOX (combo));
  t_settle (600);
  tl = find_popup_menu (win);
  T_CHECK (tl != NULL, "%s: the combo popup did not map", name);
  if (tl)
    {
      GList *kids = gtk_container_get_children (GTK_CONTAINER (gtk_bin_get_child (GTK_BIN (tl)))), *k;
      TShot *shot = t_capture_toplevel (tl);

      menu = gtk_bin_get_child (GTK_BIN (tl));
      T_CHECK (shot != NULL, "%s: cannot read the pixels", name);
      for (i = 0, k = kids; shot && k; i++, k = k->next)
        {
          TRect r = t_widget_rect (k->data, tl);
          TRect band = { r.x + 4, r.y, r.w - 8, r.h };
          guint32 bg = t_px (shot, r.x + r.w - 2, r.y + r.h / 2);
          int ink = t_count_not (shot, band, bg, NULL);

          T_INFO ("%s: row %d %s ink %d contrast %d of 255", name, i, (i & 1) == 0 ? "sensitive  " : "insensitive", ink,
                  band_contrast (shot, band, bg));
          T_CHECK (ink > MIN_INK, "%s: row %d (%s) has only %d ink pixels", name, i,
                   (i & 1) == 0 ? "sensitive" : "insensitive", ink);
        }
      if (t_nfail)
        t_save (shot, name);
      t_shot_free (shot);
      t_shot_point (name);
      (void) menu;
      g_list_free (kids);
    }
  gtk_combo_box_popdown (GTK_COMBO_BOX (combo));
  t_settle (200);
  gtk_widget_destroy (win);
  t_settle (100);
  g_free (name);
}

int
main (int argc, char **argv)
{
  const char *cl = g_getenv ("T_CLEARLOOKS"), *seat = g_getenv ("T_HAVE_SEAT");
  gboolean popup = (seat && strcmp (seat, "1") == 0) || g_getenv ("T_FORCE");

  t_begin ("T0.15", &argc, &argv);
  run_window ("default-theme");
  if (popup)
    {
      run_popup ("default-theme");
      run_combo ("default-theme");
    }
  if (cl && strcmp (cl, "0") == 0)
    T_INFO ("clearlooks phase skipped (T_CLEARLOOKS=0)");
  else
    {
      gtk_rc_parse_string ("style \"cl\" { engine \"clearlooks\" { } }\n"
                           "class \"GtkWidget\" style \"cl\"\n");
      run_window ("clearlooks");
      if (popup)
        {
          run_popup ("clearlooks");
          run_combo ("clearlooks");
        }
    }
  return t_finish ();
}
