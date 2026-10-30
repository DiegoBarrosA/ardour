/*
 * T0.12  Menus, combo boxes and tooltips with a real seat (sway).
 *
 * Needs a compositor with a pointer and a keyboard and an outside driver that
 * clicks.  Run it with run-seat-tests.sh.  Without T12_DRIVEN=1 it prints SKIP,
 * so the weston and valgrind runs of run-tests.sh do not wait for a driver.
 *
 * The window holds the widgets that Ardour uses:
 *   - a GtkMenuBar with File, Edit and View.  File has a cascade of three
 *     levels (Open Recent, More, Deeper), Edit has check, radio and plain items.
 *   - a context menu on a right click on the grey area.
 *   - a GtkComboBox (menu popup), a GtkOptionMenu, and two dropdown buttons that
 *     pop a GtkMenu below the button (one on button-press like ArdourDropdown,
 *     one on "clicked").
 *   - a button with a tooltip.
 *
 * The program logs to stdout, one line per record, each with the time in ms
 * since start:
 *   SIG    show, hide, activate, select, deselect, toggled, selection-done,
 *          deactivate, changed and notify::popup-shown of the widgets
 *   EV     every GDK event (T12_ALL=1 adds expose and property events)
 *   GEOM, TOPLEVEL, MENU, MITEM   geometry dumps on request
 * The driver writes one command per line to stdin:
 *   geom       dump the geometry of the widgets, the mapped toplevels, and the
 *              items of every mapped menu, relative to the main window
 *   mark TEXT  log a MARK line (the driver uses it to cut the log into steps)
 *   win2, win2-hide   show or hide the second toplevel
 *   title TEXT        set the title of the main window
 *   quit       destroy the window and print the verdict
 * End of file on stdin is the same as quit.
 *
 * Checks at the end: no menu and no popup toplevel is mapped, every menu that
 * was shown was also hidden.
 *
 * Origin: Wayland backend of the ydk fork, snapshot commit 23f48f0.
 */
#include "common.h"
#include <stdarg.h>
#include <time.h>

static gint64 t_start;
static GtkWidget *main_win, *win2;
static GPtrArray *menus;          /* all menus we made, with t12-label */
static GHashTable *named;         /* name -> widget, for GEOM */
static GHashTable *shown;         /* menu -> show count minus hide count */
static int log_all;
static int quitting;
static GMainLoop *loop;

static void
lg (const char *kind, const char *fmt, ...)
{
  va_list ap;
  char *msg;
  va_start (ap, fmt);
  msg = g_strdup_vprintf (fmt, ap);
  va_end (ap);
  gint64 wall = g_get_real_time ();
  time_t secs = (time_t) (wall / G_USEC_PER_SEC);
  struct tm tm;
  gmtime_r (&secs, &tm);
  /* the wall time (UTC) matches the time stamps of WAYLAND_DEBUG=1 */
  g_print ("T0.12: [%9.3f] %02d:%02d:%02d.%06d %-6s %s\n", (g_get_monotonic_time () - t_start) / 1000.0, tm.tm_hour,
           tm.tm_min, tm.tm_sec, (int) (wall % G_USEC_PER_SEC), kind, msg);
  g_free (msg);
}

static const char *
widget_label (GtkWidget *w)
{
  const char *l = g_object_get_data (G_OBJECT (w), "t12-label");
  return l ? l : G_OBJECT_TYPE_NAME (w);
}

/* ---- window and event names -------------------------------------------- */

static GtkWidget *
find_menu_in (GtkWidget *w)
{
  if (GTK_IS_MENU (w))
    return w;
  if (GTK_IS_BIN (w) && gtk_bin_get_child (GTK_BIN (w)))
    return find_menu_in (gtk_bin_get_child (GTK_BIN (w)));
  return NULL;
}

static char *
win_label (GdkWindow *w)
{
  gpointer ud = NULL;
  GtkWidget *wd;
  const char *wt;
  if (!w)
    return g_strdup ("(null)");
  switch (gdk_window_get_window_type (w))
    {
    case GDK_WINDOW_TOPLEVEL: wt = "top"; break;
    case GDK_WINDOW_TEMP:     wt = "temp"; break;
    case GDK_WINDOW_CHILD:    wt = "child"; break;
    default:                  wt = "other"; break;
    }
  gdk_window_get_user_data (w, &ud);
  if (!ud)
    return g_strdup_printf ("%s:%p(no widget)", wt, (void *) w);
  wd = GTK_WIDGET (ud);
  if (GTK_IS_WINDOW (wd))
    {
      GtkWidget *m = find_menu_in (wd);
      if (m)
        return g_strdup_printf ("%s:%p popup[%s]", wt, (void *) w, widget_label (m));
      if (wd == main_win)
        return g_strdup_printf ("%s:%p main", wt, (void *) w);
      if (wd == win2)
        return g_strdup_printf ("%s:%p win2", wt, (void *) w);
      return g_strdup_printf ("%s:%p %s%s", wt, (void *) w, G_OBJECT_TYPE_NAME (wd),
                              GTK_WINDOW (wd)->type == GTK_WINDOW_POPUP ? " popup" : "");
    }
  if (GTK_IS_MENU_ITEM (wd))
    return g_strdup_printf ("%s:%p item[%s]", wt, (void *) w, widget_label (wd));
  return g_strdup_printf ("%s:%p %s", wt, (void *) w, widget_label (wd));
}

static const char *
ev_name (GdkEventType t)
{
  switch (t)
    {
    case GDK_NOTHING: return "NOTHING";
    case GDK_DELETE: return "DELETE";
    case GDK_DESTROY: return "DESTROY";
    case GDK_EXPOSE: return "EXPOSE";
    case GDK_MOTION_NOTIFY: return "MOTION";
    case GDK_BUTTON_PRESS: return "BUTTON_PRESS";
    case GDK_2BUTTON_PRESS: return "2BUTTON_PRESS";
    case GDK_3BUTTON_PRESS: return "3BUTTON_PRESS";
    case GDK_BUTTON_RELEASE: return "BUTTON_RELEASE";
    case GDK_KEY_PRESS: return "KEY_PRESS";
    case GDK_KEY_RELEASE: return "KEY_RELEASE";
    case GDK_ENTER_NOTIFY: return "ENTER";
    case GDK_LEAVE_NOTIFY: return "LEAVE";
    case GDK_FOCUS_CHANGE: return "FOCUS";
    case GDK_CONFIGURE: return "CONFIGURE";
    case GDK_MAP: return "MAP";
    case GDK_UNMAP: return "UNMAP";
    case GDK_PROPERTY_NOTIFY: return "PROPERTY";
    case GDK_SELECTION_CLEAR: return "SEL_CLEAR";
    case GDK_SELECTION_REQUEST: return "SEL_REQUEST";
    case GDK_SELECTION_NOTIFY: return "SEL_NOTIFY";
    case GDK_PROXIMITY_IN: return "PROX_IN";
    case GDK_PROXIMITY_OUT: return "PROX_OUT";
    case GDK_DRAG_ENTER: return "DRAG_ENTER";
    case GDK_DRAG_LEAVE: return "DRAG_LEAVE";
    case GDK_DRAG_MOTION: return "DRAG_MOTION";
    case GDK_DRAG_STATUS: return "DRAG_STATUS";
    case GDK_DROP_START: return "DROP_START";
    case GDK_DROP_FINISHED: return "DROP_FINISHED";
    case GDK_CLIENT_EVENT: return "CLIENT";
    case GDK_VISIBILITY_NOTIFY: return "VISIBILITY";
    case GDK_NO_EXPOSE: return "NO_EXPOSE";
    case GDK_SCROLL: return "SCROLL";
    case GDK_WINDOW_STATE: return "WINDOW_STATE";
    case GDK_SETTING: return "SETTING";
    case GDK_OWNER_CHANGE: return "OWNER_CHANGE";
    case GDK_GRAB_BROKEN: return "GRAB_BROKEN";
    case GDK_DAMAGE: return "DAMAGE";
    default: return "?";
    }
}

static void
log_event (GdkEvent *e)
{
  char *w, *extra = NULL;
  GdkEventType t = e->type;
  if (!log_all && (t == GDK_EXPOSE || t == GDK_NO_EXPOSE || t == GDK_DAMAGE || t == GDK_PROPERTY_NOTIFY ||
                   t == GDK_SETTING || t == GDK_NOTHING))
    return;
  w = win_label (e->any.window);
  switch (t)
    {
    case GDK_MOTION_NOTIFY:
      extra = g_strdup_printf ("x=%.0f y=%.0f root=%.0f,%.0f", e->motion.x, e->motion.y, e->motion.x_root, e->motion.y_root);
      break;
    case GDK_BUTTON_PRESS:
    case GDK_2BUTTON_PRESS:
    case GDK_BUTTON_RELEASE:
      extra = g_strdup_printf ("button=%u x=%.0f y=%.0f root=%.0f,%.0f time=%u", e->button.button, e->button.x, e->button.y,
                               e->button.x_root, e->button.y_root, e->button.time);
      break;
    case GDK_ENTER_NOTIFY:
    case GDK_LEAVE_NOTIFY:
      extra = g_strdup_printf ("x=%.0f y=%.0f mode=%d detail=%d focus=%d subwindow=%p", e->crossing.x, e->crossing.y,
                               e->crossing.mode, e->crossing.detail, e->crossing.focus, (void *) e->crossing.subwindow);
      break;
    case GDK_KEY_PRESS:
    case GDK_KEY_RELEASE:
      extra = g_strdup_printf ("keyval=0x%x state=0x%x", e->key.keyval, e->key.state);
      break;
    case GDK_CONFIGURE:
      extra = g_strdup_printf ("x=%d y=%d %dx%d", e->configure.x, e->configure.y, e->configure.width, e->configure.height);
      break;
    case GDK_FOCUS_CHANGE:
      extra = g_strdup_printf ("in=%d", e->focus_change.in);
      break;
    case GDK_GRAB_BROKEN:
      {
        char *gw = e->grab_broken.grab_window ? win_label (e->grab_broken.grab_window) : g_strdup ("NULL");
        extra = g_strdup_printf ("keyboard=%d implicit=%d grab_window=%s", e->grab_broken.keyboard, e->grab_broken.implicit, gw);
        g_free (gw);
      }
      break;
    case GDK_SCROLL:
      extra = g_strdup_printf ("dir=%d", e->scroll.direction);
      break;
    default:
      break;
    }
  lg ("EV", "%s %s %s", ev_name (t), w, extra ? extra : "");
  g_free (w);
  g_free (extra);
}

static void
ev_handler (GdkEvent *e, gpointer data)
{
  log_event (e);
  gtk_main_do_event (e);
}

/* ---- signals ------------------------------------------------------------ */

static void
on_menu_show (GtkWidget *m, gpointer d)
{
  int n = GPOINTER_TO_INT (g_hash_table_lookup (shown, m)) + 1;
  g_hash_table_insert (shown, m, GINT_TO_POINTER (n));
  lg ("SIG", "show menu[%s]", widget_label (m));
}

static void
on_menu_hide (GtkWidget *m, gpointer d)
{
  int n = GPOINTER_TO_INT (g_hash_table_lookup (shown, m)) - 1;
  g_hash_table_insert (shown, m, GINT_TO_POINTER (n));
  lg ("SIG", "hide menu[%s]", widget_label (m));
}

static void
on_shell_done (GtkMenuShell *s, gpointer d)
{
  lg ("SIG", "selection-done shell[%s]", widget_label (GTK_WIDGET (s)));
}

static void
on_deactivate (GtkMenuShell *s, gpointer d)
{
  lg ("SIG", "deactivate shell[%s]", widget_label (GTK_WIDGET (s)));
}

static void
on_item_activate (GtkMenuItem *i, gpointer d)
{
  lg ("SIG", "activate item[%s]", widget_label (GTK_WIDGET (i)));
}

static void
on_item_select (GtkMenuItem *i, gpointer d)
{
  lg ("SIG", "select item[%s]", widget_label (GTK_WIDGET (i)));
}

static void
on_item_deselect (GtkMenuItem *i, gpointer d)
{
  lg ("SIG", "deselect item[%s]", widget_label (GTK_WIDGET (i)));
}

static void
on_toggled (GtkCheckMenuItem *i, gpointer d)
{
  lg ("SIG", "toggled item[%s] active=%d", widget_label (GTK_WIDGET (i)), gtk_check_menu_item_get_active (i));
}

static void
on_changed (GtkWidget *w, gpointer d)
{
  int a = -1;
  if (GTK_IS_COMBO_BOX (w))
    a = gtk_combo_box_get_active (GTK_COMBO_BOX (w));
  else if (GTK_IS_OPTION_MENU (w))
    a = gtk_option_menu_get_history (GTK_OPTION_MENU (w));
  lg ("SIG", "changed %s active=%d", widget_label (w), a);
}

static void
on_popup_shown (GObject *o, GParamSpec *p, gpointer d)
{
  gboolean v = FALSE;
  g_object_get (o, "popup-shown", &v, NULL);
  lg ("SIG", "notify::popup-shown %s %d", widget_label (GTK_WIDGET (o)), v);
}

static void
on_clicked (GtkWidget *b, gpointer d)
{
  lg ("SIG", "clicked button[%s]", widget_label (b));
}

static void
label_widget (GtkWidget *w, const char *l, gboolean geom)
{
  g_object_set_data_full (G_OBJECT (w), "t12-label", g_strdup (l), g_free);
  if (geom)
    g_hash_table_insert (named, g_strdup (l), w);
}

static GtkWidget *
make_menu (const char *label)
{
  GtkWidget *m = gtk_menu_new ();
  label_widget (m, label, FALSE);
  g_ptr_array_add (menus, m);
  g_signal_connect (m, "show", G_CALLBACK (on_menu_show), NULL);
  g_signal_connect (m, "hide", G_CALLBACK (on_menu_hide), NULL);
  g_signal_connect (m, "selection-done", G_CALLBACK (on_shell_done), NULL);
  g_signal_connect (m, "deactivate", G_CALLBACK (on_deactivate), NULL);
  return m;
}

enum { K_PLAIN, K_CHECK, K_RADIO, K_SEP };

static GtkWidget *
add_item (GtkWidget *shell, const char *label, int kind, GSList **group)
{
  GtkWidget *i;
  switch (kind)
    {
    case K_CHECK: i = gtk_check_menu_item_new_with_label (label); break;
    case K_RADIO:
      i = gtk_radio_menu_item_new_with_label (*group, label);
      *group = gtk_radio_menu_item_get_group (GTK_RADIO_MENU_ITEM (i));
      break;
    case K_SEP: i = gtk_separator_menu_item_new (); break;
    default: i = gtk_menu_item_new_with_label (label); break;
    }
  if (kind != K_SEP)
    {
      label_widget (i, label, FALSE);
      g_signal_connect (i, "activate", G_CALLBACK (on_item_activate), NULL);
      g_signal_connect (i, "select", G_CALLBACK (on_item_select), NULL);
      g_signal_connect (i, "deselect", G_CALLBACK (on_item_deselect), NULL);
      if (kind == K_CHECK || kind == K_RADIO)
        g_signal_connect (i, "toggled", G_CALLBACK (on_toggled), NULL);
    }
  gtk_widget_show (i);
  gtk_menu_shell_append (GTK_MENU_SHELL (shell), i);
  return i;
}

static void
set_sub (GtkWidget *item, GtkWidget *sub)
{
  gtk_menu_item_set_submenu (GTK_MENU_ITEM (item), sub);
}

/* ---- widgets ------------------------------------------------------------ */

static GtkWidget *ctx_menu, *dd_menu1, *dd_menu2;

static void
pos_below (GtkMenu *menu, gint *x, gint *y, gboolean *push_in, gpointer data)
{
  GtkWidget *b = data;
  gint ox = 0, oy = 0;
  gdk_window_get_origin (b->window, &ox, &oy);
  *x = ox + b->allocation.x;
  *y = oy + b->allocation.y + b->allocation.height;
  *push_in = TRUE;
}

static gboolean
dd_press (GtkWidget *b, GdkEventButton *e, gpointer menu)
{
  /* like ArdourDropdown: pop on the press, button and time of the event */
  gtk_menu_popup (GTK_MENU (menu), NULL, NULL, pos_below, b, e->button, e->time);
  return TRUE;
}

static void
dd_clicked (GtkWidget *b, gpointer menu)
{
  on_clicked (b, NULL);
  gtk_menu_popup (GTK_MENU (menu), NULL, NULL, pos_below, b, 0, gtk_get_current_event_time ());
}

static gboolean
ctx_press (GtkWidget *w, GdkEventButton *e, gpointer d)
{
  if (e->button == 3)
    {
      gtk_menu_popup (GTK_MENU (ctx_menu), NULL, NULL, NULL, NULL, e->button, e->time);
      return TRUE;
    }
  return FALSE;
}

static GtkWidget *
build_menubar (void)
{
  GtkWidget *bar = gtk_menu_bar_new (), *file, *edit, *view, *m, *recent, *more, *deeper, *sub, *sub2, *sub3, *vm;
  GSList *grp = NULL;

  label_widget (bar, "menubar", FALSE);
  g_signal_connect (bar, "selection-done", G_CALLBACK (on_shell_done), NULL);
  g_signal_connect (bar, "deactivate", G_CALLBACK (on_deactivate), NULL);

  /* File: New, Open Recent > (Project A, Project B, More > (Deep 1, Deeper > (Deepest 1, Deepest 2))), ---, Close */
  file = add_item (bar, "File", K_PLAIN, NULL);
  g_hash_table_insert (named, g_strdup ("bar:File"), file);
  m = make_menu ("File");
  add_item (m, "New", K_PLAIN, NULL);
  recent = add_item (m, "Open Recent", K_PLAIN, NULL);
  sub = make_menu ("Recent");
  add_item (sub, "Project A", K_PLAIN, NULL);
  add_item (sub, "Project B", K_PLAIN, NULL);
  more = add_item (sub, "More", K_PLAIN, NULL);
  sub2 = make_menu ("More");
  add_item (sub2, "Deep 1", K_PLAIN, NULL);
  deeper = add_item (sub2, "Deeper", K_PLAIN, NULL);
  sub3 = make_menu ("Deeper");
  add_item (sub3, "Deepest 1", K_PLAIN, NULL);
  add_item (sub3, "Deepest 2", K_PLAIN, NULL);
  set_sub (deeper, sub3);
  set_sub (more, sub2);
  set_sub (recent, sub);
  add_item (m, NULL, K_SEP, NULL);
  add_item (m, "Close", K_PLAIN, NULL);
  set_sub (file, m);

  edit = add_item (bar, "Edit", K_PLAIN, NULL);
  g_hash_table_insert (named, g_strdup ("bar:Edit"), edit);
  m = make_menu ("Edit");
  add_item (m, "Undo", K_PLAIN, NULL);
  add_item (m, "Toggle me", K_CHECK, NULL);
  add_item (m, NULL, K_SEP, NULL);
  add_item (m, "Mode A", K_RADIO, &grp);
  add_item (m, "Mode B", K_RADIO, &grp);
  set_sub (edit, m);

  view = add_item (bar, "View", K_PLAIN, NULL);
  g_hash_table_insert (named, g_strdup ("bar:View"), view);
  vm = make_menu ("View");
  add_item (vm, "Zoom in", K_PLAIN, NULL);
  add_item (vm, "Zoom out", K_PLAIN, NULL);
  set_sub (view, vm);
  return bar;
}

static GtkWidget *
make_dropdown_menu (const char *label, const char *first)
{
  GtkWidget *m = make_menu (label);
  char *s;
  int i;
  add_item (m, first, K_PLAIN, NULL);
  for (i = 1; i < 5; i++)
    {
      s = g_strdup_printf ("%s %d", label, i);
      add_item (m, s, K_PLAIN, NULL);
      g_free (s);
    }
  return m;
}

static GtkWidget *
build_body (void)
{
  GtkWidget *box = gtk_vbox_new (FALSE, 6), *row, *ev, *lab, *combo, *opt, *om, *b1, *b2, *tip;
  int i;

  /* grey area with the context menu */
  ev = gtk_event_box_new ();
  lab = gtk_label_new ("Right click here for a context menu");
  gtk_widget_set_size_request (ev, 360, 80);
  gtk_container_add (GTK_CONTAINER (ev), lab);
  label_widget (ev, "ctxarea", TRUE);
  gtk_widget_add_events (ev, GDK_BUTTON_PRESS_MASK);
  g_signal_connect (ev, "button-press-event", G_CALLBACK (ctx_press), NULL);
  ctx_menu = make_menu ("Context");
  add_item (ctx_menu, "Copy", K_PLAIN, NULL);
  add_item (ctx_menu, "Paste", K_PLAIN, NULL);
  {
    GtkWidget *p = add_item (ctx_menu, "Properties", K_PLAIN, NULL), *ps = make_menu ("Properties");
    add_item (ps, "Size", K_PLAIN, NULL);
    add_item (ps, "Colour", K_PLAIN, NULL);
    set_sub (p, ps);
  }
  gtk_box_pack_start (GTK_BOX (box), ev, FALSE, FALSE, 0);

  /* combo box, menu style */
  row = gtk_hbox_new (FALSE, 6);
  combo = gtk_combo_box_new_text ();
  for (i = 0; i < 6; i++)
    {
      char *s = g_strdup_printf ("Choice %d", i);
      gtk_combo_box_append_text (GTK_COMBO_BOX (combo), s);
      g_free (s);
    }
  gtk_combo_box_set_active (GTK_COMBO_BOX (combo), 1);
  label_widget (combo, "combo", TRUE);
  g_signal_connect (combo, "changed", G_CALLBACK (on_changed), NULL);
  g_signal_connect (combo, "notify::popup-shown", G_CALLBACK (on_popup_shown), NULL);
  gtk_box_pack_start (GTK_BOX (row), combo, TRUE, TRUE, 0);

  /* option menu */
  opt = gtk_option_menu_new ();
  om = make_menu ("Option");
  for (i = 0; i < 4; i++)
    {
      char *s = g_strdup_printf ("Option %d", i);
      add_item (om, s, K_PLAIN, NULL);
      g_free (s);
    }
  gtk_option_menu_set_menu (GTK_OPTION_MENU (opt), om);
  label_widget (opt, "optmenu", TRUE);
  g_signal_connect (opt, "changed", G_CALLBACK (on_changed), NULL);
  gtk_box_pack_start (GTK_BOX (row), opt, TRUE, TRUE, 0);
  gtk_box_pack_start (GTK_BOX (box), row, FALSE, FALSE, 0);

  /* two dropdown buttons */
  row = gtk_hbox_new (FALSE, 6);
  b1 = gtk_button_new_with_label ("No Grid (press)");
  label_widget (b1, "dd-press", TRUE);
  dd_menu1 = make_dropdown_menu ("DropPress", "No Grid");
  g_signal_connect (b1, "button-press-event", G_CALLBACK (dd_press), dd_menu1);
  gtk_box_pack_start (GTK_BOX (row), b1, TRUE, TRUE, 0);
  b2 = gtk_button_new_with_label ("Slide (clicked)");
  label_widget (b2, "dd-click", TRUE);
  dd_menu2 = make_dropdown_menu ("DropClick", "Slide");
  g_signal_connect (b2, "clicked", G_CALLBACK (dd_clicked), dd_menu2);
  gtk_box_pack_start (GTK_BOX (row), b2, TRUE, TRUE, 0);
  gtk_box_pack_start (GTK_BOX (box), row, FALSE, FALSE, 0);

  /* tooltip */
  tip = gtk_button_new_with_label ("Hover for a tooltip");
  label_widget (tip, "tipbtn", TRUE);
  gtk_widget_set_tooltip_text (tip, "This is a tooltip");
  gtk_box_pack_start (GTK_BOX (box), tip, FALSE, FALSE, 0);

  return box;
}

/* ---- geometry dump ------------------------------------------------------ */

static void
main_origin (int *x, int *y)
{
  *x = *y = 0;
  if (main_win && main_win->window)
    gdk_window_get_origin (main_win->window, x, y);
}

static const char *
root_of (GtkWidget *w)
{
  GtkWindow *t = GTK_WINDOW (w);
  int guard = 0;
  while (t && gtk_window_get_transient_for (t) && guard++ < 10)
    t = gtk_window_get_transient_for (t);
  return (t && GTK_WIDGET (t) == win2) ? "win2" : "main";
}

static void
dump_geometry (void)
{
  GHashTableIter it;
  gpointer k, v;
  GList *tl = gtk_window_list_toplevels (), *i;
  int mx, my;

  main_origin (&mx, &my);
  lg ("GEOM", "main origin %d,%d", mx, my);
  g_hash_table_iter_init (&it, named);
  while (g_hash_table_iter_next (&it, &k, &v))
    {
      TRect r = t_widget_rect (GTK_WIDGET (v), gtk_widget_get_toplevel (GTK_WIDGET (v)));
      lg ("GEOM", "%s %d %d %d %d", (char *) k, r.x, r.y, r.w, r.h);
    }
  for (i = tl; i; i = i->next)
    {
      GtkWidget *w = i->data, *m;
      int ox = 0, oy = 0, ww = 0, wh = 0;
      if (!gtk_widget_get_mapped (w) || !w->window)
        continue;
      gdk_window_get_origin (w->window, &ox, &oy);
      gdk_drawable_get_size (w->window, &ww, &wh);
      lg ("TOPLEVEL", "%p %s type=%d at %d,%d (relative to main %d,%d) size %dx%d tf=%p root=%s", (void *) w,
          w == main_win ? "main" : w == win2 ? "win2" : G_OBJECT_TYPE_NAME (w), (int) GTK_WINDOW (w)->type, ox, oy, ox - mx, oy - my,
          ww, wh, (void *) gtk_window_get_transient_for (GTK_WINDOW (w)), root_of (w));
      m = find_menu_in (w);
      if (m && m->window)
        {
          GList *c = gtk_container_get_children (GTK_CONTAINER (m)), *j;
          int idx = 0;
          lg ("MENU", "%s window at %d,%d size %dx%d root=%s", widget_label (m), ox - mx, oy - my, ww, wh, root_of (w));
          for (j = c; j; j = j->next)
            {
              GtkWidget *it2 = j->data;
              int x = 0, y = 0;
              char *lab;
              idx++;
              if (!gtk_widget_get_visible (it2))
                continue;
              gtk_widget_translate_coordinates (it2, w, 0, 0, &x, &y);
              /* items without a label of ours (the combo box menu) are named by their index */
              if (GTK_IS_SEPARATOR_MENU_ITEM (it2))
                lab = g_strdup_printf ("---%d", idx - 1);
              else if (g_object_get_data (G_OBJECT (it2), "t12-label"))
                lab = g_strdup (widget_label (it2));
              else
                lab = g_strdup_printf ("#%d", idx - 1);
              lg ("MITEM", "%s | %s %d %d %d %d root=%s", widget_label (m), lab, ox - mx + x, oy - my + y, it2->allocation.width,
                  it2->allocation.height, root_of (w));
              g_free (lab);
            }
          g_list_free (c);
        }
    }
  g_list_free (tl);
}

/* ---- stdin commands ----------------------------------------------------- */

static void finish (void);
static void show_win2 (void);

/* A second toplevel with its own menu bar and a combo, like a detached mixer or a plugin window. */
static void
show_win2 (void)
{
  GtkWidget *box, *bar, *w2, *m, *combo;
  int i;
  if (win2)
    {
      gtk_widget_show (win2);
      return;
    }
  win2 = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  label_widget (win2, "win2", FALSE);
  gtk_window_set_title (GTK_WINDOW (win2), "T0.12 second");
  gtk_widget_set_size_request (win2, 260, 120);
  gtk_window_set_resizable (GTK_WINDOW (win2), FALSE);
  box = gtk_vbox_new (FALSE, 6);
  bar = gtk_menu_bar_new ();
  w2 = add_item (bar, "W2", K_PLAIN, NULL);
  g_hash_table_insert (named, g_strdup ("w2:W2"), w2);
  m = make_menu ("W2");
  add_item (m, "Other A", K_PLAIN, NULL);
  add_item (m, "Other B", K_PLAIN, NULL);
  set_sub (w2, m);
  gtk_box_pack_start (GTK_BOX (box), bar, FALSE, FALSE, 0);
  combo = gtk_combo_box_new_text ();
  for (i = 0; i < 3; i++)
    {
      char *s = g_strdup_printf ("Second %d", i);
      gtk_combo_box_append_text (GTK_COMBO_BOX (combo), s);
      g_free (s);
    }
  gtk_combo_box_set_active (GTK_COMBO_BOX (combo), 0);
  label_widget (combo, "combo2", FALSE);
  g_signal_connect (combo, "changed", G_CALLBACK (on_changed), NULL);
  gtk_box_pack_start (GTK_BOX (box), combo, FALSE, FALSE, 0);
  gtk_container_add (GTK_CONTAINER (win2), box);
  gtk_widget_show_all (win2);
}

static gboolean
stdin_cb (GIOChannel *ch, GIOCondition cond, gpointer d)
{
  char *line = NULL;
  gsize len;
  GIOStatus st = g_io_channel_read_line (ch, &line, &len, NULL, NULL);
  if (st == G_IO_STATUS_NORMAL && line)
    {
      g_strstrip (line);
      if (!strcmp (line, "geom"))
        dump_geometry ();
      else if (g_str_has_prefix (line, "mark "))
        lg ("MARK", "%s", line + 5);
      else if (!strcmp (line, "win2"))
        show_win2 ();
      else if (g_str_has_prefix (line, "title "))
        gtk_window_set_title (GTK_WINDOW (main_win), line + 6);
      else if (!strcmp (line, "win2-hide"))
        {
          if (win2)
            gtk_widget_hide (win2);
        }
      else if (!strcmp (line, "quit"))
        {
          g_free (line);
          finish ();
          return FALSE;
        }
      g_free (line);
      return TRUE;
    }
  if (st == G_IO_STATUS_AGAIN)
    return TRUE;
  finish ();
  return FALSE;
}

static void
finish (void)
{
  quitting = 1;
  g_main_loop_quit (loop);
}

int
main (int argc, char **argv)
{
  GtkWidget *box, *bar;
  GIOChannel *ch;
  const char *seat = g_getenv ("T_HAVE_SEAT");
  GList *tl, *i;
  GHashTableIter it;
  gpointer k, v;

  t_begin ("T0.12", &argc, &argv);
  if (!(seat && strcmp (seat, "1") == 0) && !g_getenv ("T_FORCE"))
    return t_skip ("no seat on this display, set T_HAVE_SEAT=1 or T_FORCE=1");
  if (!g_getenv ("T12_DRIVEN"))
    return t_skip ("needs a driver, run run-seat-tests.sh");
  log_all = g_getenv ("T12_ALL") != NULL;
  t_start = g_get_monotonic_time ();
  menus = g_ptr_array_new ();
  named = g_hash_table_new (g_str_hash, g_str_equal);
  shown = g_hash_table_new (g_direct_hash, g_direct_equal);

  gdk_event_handler_set (ev_handler, NULL, NULL);

  main_win = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title (GTK_WINDOW (main_win), "T0.12 menus");
  gtk_window_set_default_size (GTK_WINDOW (main_win), 520, 330);
  gtk_window_set_resizable (GTK_WINDOW (main_win), FALSE);
  box = gtk_vbox_new (FALSE, 6);
  bar = build_menubar ();
  gtk_box_pack_start (GTK_BOX (box), bar, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (box), build_body (), TRUE, TRUE, 6);
  gtk_container_add (GTK_CONTAINER (main_win), box);
  gtk_widget_show_all (main_win);
  t_settle (600);
  lg ("MARK", "ready");
  dump_geometry ();
  g_print ("T0.12: ready\n");

  ch = g_io_channel_unix_new (0);
  g_io_channel_set_encoding (ch, NULL, NULL);
  g_io_add_watch (ch, G_IO_IN | G_IO_HUP, stdin_cb, NULL);

  loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (loop);

  /* checks: nothing may stay open */
  tl = gtk_window_list_toplevels ();
  for (i = tl; i; i = i->next)
    if (i->data != main_win && gtk_widget_get_mapped (i->data))
      {
        char *w = win_label (GTK_WIDGET (i->data)->window);
        T_CHECK (FALSE, "a popup toplevel is still mapped at the end: %s", w);
        g_free (w);
      }
  g_list_free (tl);
  g_hash_table_iter_init (&it, shown);
  while (g_hash_table_iter_next (&it, &k, &v))
    T_CHECK (GPOINTER_TO_INT (v) == 0, "menu %s has show minus hide = %d", widget_label (GTK_WIDGET (k)), GPOINTER_TO_INT (v));
  {
    guint n;
    for (n = 0; n < menus->len; n++)
      T_CHECK (!gtk_widget_get_mapped (g_ptr_array_index (menus, n)), "menu %s is mapped at the end",
               widget_label (g_ptr_array_index (menus, n)));
  }
  gtk_widget_destroy (main_win);
  t_settle (100);
  return t_finish ();
}
