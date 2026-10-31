/*
 * T0.10  Popup menu and combo box popup.  Needs a seat; skipped on headless
 *        weston.
 *
 * The runner sets T_HAVE_SEAT=1 on displays with a pointer and a keyboard
 * (Xvfb, sway).  Without it the program prints SKIP.  T_FORCE=1 runs it
 * anyway.
 *
 * Menu: a GtkMenu with items and a submenu is popped up over a window.  The
 * menu toplevel must map, expose and draw; selecting an item must change the
 * picture; activating an item must emit "activate" and close the menu.
 * Combo: gtk_combo_box_popup must show a popup toplevel and gtk_combo_box_popdown
 * must remove it.
 */
#include "common.h"

static gboolean activated = FALSE;
static gboolean deactivated = FALSE;

static void
on_activate (GtkMenuItem *item, gpointer data)
{
  activated = TRUE;
}

static void
on_deactivate (GtkMenuShell *shell, gpointer data)
{
  deactivated = TRUE;
}

static void
pos_func (GtkMenu *menu, gint *x, gint *y, gboolean *push_in, gpointer data)
{
  GtkWidget *win = data;
  gint ox = 0, oy = 0;
  gdk_window_get_origin (win->window, &ox, &oy);
  *x = ox + 20;
  *y = oy + 20;
  *push_in = TRUE;
}

static int
count_mapped_toplevels (void)
{
  GList *l = gtk_window_list_toplevels (), *i;
  int n = 0;
  for (i = l; i; i = i->next)
    if (gtk_widget_get_mapped (i->data))
      n++;
  g_list_free (l);
  return n;
}

static GtkWidget *popup_toplevel;

static gboolean
menu_mapped (gpointer p)
{
  GtkWidget *tl = GTK_MENU (p)->toplevel;
  return tl && gtk_widget_get_mapped (tl) && gtk_widget_get_mapped (p);
}

static gboolean
menu_unmapped (gpointer p)
{
  return !gtk_widget_get_mapped (GTK_WIDGET (p));
}

static int base_count;
static gboolean
more_toplevels (gpointer p)
{
  return count_mapped_toplevels () > base_count;
}

static gboolean
back_to_base (gpointer p)
{
  return count_mapped_toplevels () <= base_count;
}

int
main (int argc, char **argv)
{
  GtkWidget *win, *box, *label, *menu, *item1, *item2, *item3, *sub, *subitem, *combo;
  const char *seat = g_getenv ("T_HAVE_SEAT");

  t_begin ("T0.10", &argc, &argv);
  if (!(seat && strcmp (seat, "1") == 0) && !g_getenv ("T_FORCE"))
    return t_skip ("no seat on this display, set T_HAVE_SEAT=1 or T_FORCE=1");

  win = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  gtk_window_set_default_size (GTK_WINDOW (win), 320, 220);
  box = gtk_vbox_new (FALSE, 6);
  label = gtk_label_new ("menu parent");
  combo = gtk_combo_box_new_text ();
  {
    int i;
    for (i = 0; i < 6; i++)
      {
        char *s = g_strdup_printf ("Choice %d", i);
        gtk_combo_box_append_text (GTK_COMBO_BOX (combo), s);
        g_free (s);
      }
  }
  gtk_combo_box_set_active (GTK_COMBO_BOX (combo), 1);
  gtk_box_pack_start (GTK_BOX (box), label, TRUE, TRUE, 0);
  gtk_box_pack_start (GTK_BOX (box), combo, FALSE, FALSE, 0);
  gtk_container_add (GTK_CONTAINER (win), box);
  gtk_widget_show_all (win);
  t_settle (400);

  /* ---- menu ---- */
  menu = gtk_menu_new ();
  item1 = gtk_menu_item_new_with_label ("First item");
  item2 = gtk_menu_item_new_with_label ("Second item");
  item3 = gtk_menu_item_new_with_label ("More");
  sub = gtk_menu_new ();
  subitem = gtk_menu_item_new_with_label ("Sub item");
  gtk_menu_shell_append (GTK_MENU_SHELL (sub), subitem);
  gtk_menu_item_set_submenu (GTK_MENU_ITEM (item3), sub);
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), item1);
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), item2);
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), item3);
  g_signal_connect (item2, "activate", G_CALLBACK (on_activate), NULL);
  g_signal_connect (menu, "deactivate", G_CALLBACK (on_deactivate), NULL);
  gtk_widget_show_all (menu);

  gtk_menu_popup (GTK_MENU (menu), NULL, NULL, pos_func, win, 0, gtk_get_current_event_time ());
  T_CHECK (t_wait_until (menu_mapped, menu, 2000), "the menu did not map within 2 s");
  t_settle (300);
  popup_toplevel = GTK_MENU (menu)->toplevel;
  T_CHECK (popup_toplevel && popup_toplevel->window && gdk_window_is_visible (popup_toplevel->window),
           "the menu toplevel is not visible");
  if (popup_toplevel && popup_toplevel->window)
    {
      TShot *s0 = t_capture_toplevel (popup_toplevel), *s1;
      int w, h;
      gdk_drawable_get_size (popup_toplevel->window, &w, &h);
      T_INFO ("menu toplevel size %dx%d", w, h);
      T_CHECK (w > 40 && h > 30, "menu toplevel %dx%d is too small", w, h);
      T_CHECK (s0 != NULL, "cannot read the menu pixels");
      gtk_menu_shell_select_item (GTK_MENU_SHELL (menu), item1);
      t_settle (300);
      s1 = t_capture_toplevel (popup_toplevel);
      if (s0 && s1)
        {
          int d = t_diff (s0, s1);
          T_INFO ("selecting the first item changed %d pixels", d);
          T_CHECK (d > 50, "selecting an item changed only %d pixels", d);
        }
      t_shot_free (s0);
      t_shot_free (s1);
    }
  gtk_menu_shell_select_item (GTK_MENU_SHELL (menu), item2);
  gtk_menu_shell_activate_item (GTK_MENU_SHELL (menu), item2, TRUE);
  t_settle (300);
  T_CHECK (activated, "activating the second item did not emit activate");
  T_CHECK (t_wait_until (menu_unmapped, menu, 2000), "the menu is still mapped after activation");
  T_CHECK (deactivated, "the menu did not emit deactivate");

  /* popup again and pop down by code */
  gtk_menu_popup (GTK_MENU (menu), NULL, NULL, pos_func, win, 0, gtk_get_current_event_time ());
  T_CHECK (t_wait_until (menu_mapped, menu, 2000), "the menu did not map the second time");
  gtk_menu_popdown (GTK_MENU (menu));
  T_CHECK (t_wait_until (menu_unmapped, menu, 2000), "gtk_menu_popdown left the menu mapped");

  /* ---- combo box popup ---- */
  base_count = count_mapped_toplevels ();
  gtk_combo_box_popup (GTK_COMBO_BOX (combo));
  T_CHECK (t_wait_until (more_toplevels, NULL, 2000), "the combo popup did not map a new toplevel (%d mapped)", base_count);
  t_settle (300);
  gtk_combo_box_popdown (GTK_COMBO_BOX (combo));
  T_CHECK (t_wait_until (back_to_base, NULL, 2000), "the combo popup is still mapped after popdown");

  gtk_widget_destroy (win);
  t_settle (100);
  return t_finish ();
}
