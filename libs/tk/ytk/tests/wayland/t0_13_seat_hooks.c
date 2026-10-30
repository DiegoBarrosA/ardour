/*
 * T0.13  Seat and popup rules, driven through the test hooks of libydk.
 *
 * Headless weston has no wl_seat, so the checks push synthetic wl_pointer and wl_keyboard
 * events through the REAL listeners (the _gdk_wayland_test_* hooks of gdkseat-wayland.h)
 * and use the virtual seat of the popup code (xdg_popup.grab is recorded, not sent).  T0.12
 * does the same things with a real seat on sway; this test makes the cases deterministic
 * that a compositor delivers only by luck of timing.
 *
 *   A  grab serial     xdg_popup.grab takes the serial of a key or button PRESS.  The
 *                      release of the key may arrive before the deferred popup commit.
 *
 * The program SKIPs on X11.
 *
 * Origin: Wayland backend of the ydk fork, snapshot commit 23f48f0.
 */
#include "common.h"
#include <ydk/gdkseat-wayland.h>
#include <ydk/gdkm9-wayland.h>
#include <ydk/gdkm10-wayland.h>

static GdkDisplay *disp;
static GtkWidget *main_win, *bar_file, *bar_menu;
static guint32 clock_ms = 100000;

static guint32
tick (void)
{
  clock_ms += 40;
  return clock_ms;
}

/* GTK ignores a release less than 500 ms after the press that opened a menu */
static guint32
tick_long (void)
{
  clock_ms += 700;
  return clock_ms;
}

/* what the GDK event queue delivered */
static struct
{
  int press, release, motion, activate;
  GdkWindow *last_release_window;
} ev;

static void
on_event (GdkEvent *e, gpointer data)
{
  switch (e->type)
    {
    case GDK_BUTTON_PRESS: ev.press++; break;
    case GDK_BUTTON_RELEASE:
      ev.release++;
      ev.last_release_window = e->any.window;
      break;
    case GDK_MOTION_NOTIFY: ev.motion++; break;
    default: break;
    }
  gtk_main_do_event (e);
}

static void
on_item_activate (GtkMenuItem *i, gpointer data)
{
  ev.activate++;
}

#define HOOK(call, what) T_CHECK ((call), "hook failed: %s", what)

/* ---- the widgets -------------------------------------------------------- */

static GtkWidget *
make_menu (void)
{
  GtkWidget *m = gtk_menu_new ();
  int i;

  for (i = 0; i < 4; i++)
    {
      char *s = g_strdup_printf ("Item %d", i);
      GtkWidget *it = gtk_menu_item_new_with_label (s);
      gtk_widget_show (it);
      gtk_menu_shell_append (GTK_MENU_SHELL (m), it);
      g_free (s);
    }
  return m;
}

static void
pos_func (GtkMenu *menu, gint *x, gint *y, gboolean *push_in, gpointer data)
{
  gint ox = 0, oy = 0;

  gdk_window_get_origin (main_win->window, &ox, &oy);
  *x = ox + 40;
  *y = oy + 40;
  *push_in = TRUE;
}

static GDKWTestPopupInfo
popup_info (GtkWidget *menu)
{
  GDKWTestPopupInfo pi;

  memset (&pi, 0, sizeof pi);
  if (GTK_MENU (menu)->toplevel != NULL && GTK_WIDGET (GTK_MENU (menu)->toplevel)->window != NULL)
    _gdk_wayland_test_popup_info (GTK_WIDGET (GTK_MENU (menu)->toplevel)->window, &pi);
  return pi;
}

static gboolean
menu_committed (gpointer p)
{
  GDKWTestPopupInfo pi = popup_info (p);

  return pi.state == GDKW_TEST_POPUP_COMMITTED || pi.state == GDKW_TEST_POPUP_MAPPED;
}

static void
close_menu (GtkWidget *menu)
{
  gtk_menu_popdown (GTK_MENU (menu));
  t_settle (150);
}

static guint32
last_key_serial (void)
{
  guint32 b = 0, k = 0, l = 0;

  HOOK (_gdk_wayland_test_seat_serials (disp, &b, &k, &l), "seat serials");
  return k;
}

static guint32
last_button_serial (void)
{
  guint32 b = 0, k = 0, l = 0;

  HOOK (_gdk_wayland_test_seat_serials (disp, &b, &k, &l), "seat serials");
  return b;
}

/* ---- A: grab serial ----------------------------------------------------- */

/* The keyboard opens a menu: the press and the release of the key are both read before the
 * deferred commit of the popup (the idle runs after the code that showed the menu). */
static void
test_grab_serial_key (void)
{
  GtkWidget *menu = make_menu ();
  GDKWTestPopupInfo pi;
  guint32 press;

  HOOK (_gdk_wayland_test_keyboard_key (disp, tick (), GDKW_TEST_KEY_A, TRUE), "key down");
  press = last_key_serial ();
  HOOK (_gdk_wayland_test_keyboard_key (disp, tick (), GDKW_TEST_KEY_A, FALSE), "key up");
  T_CHECK (press != 0, "the key press has a serial");
  T_CHECK (last_key_serial () == press, "a key release does not replace the key serial (%u, press %u)", last_key_serial (),
           press);

  gtk_menu_popup (GTK_MENU (menu), NULL, NULL, pos_func, NULL, 0, gtk_get_current_event_time ());
  T_CHECK (t_wait_until (menu_committed, menu, 2000), "A key: the menu popup is committed");
  pi = popup_info (menu);
  T_CHECK (pi.grab_sent, "A key: the popup asked for a grab");
  T_CHECK (pi.grab_virtual, "A key: the grab was recorded on the virtual seat");
  T_CHECK (pi.grab_serial == press, "A key: the grab carries the serial of the key press (%u, press %u)", pi.grab_serial,
           press);
  close_menu (menu);
}

/* The newest PRESS wins, a later release of the button or of the key does not count. */
static void
test_grab_serial_newest_press (void)
{
  GtkWidget *menu = make_menu ();
  GDKWTestPopupInfo pi;
  guint32 bpress, kpress;

  /* button press and release, then key press and release: the key press is newer */
  HOOK (_gdk_wayland_test_pointer_button (disp, tick (), GDKW_TEST_BTN_LEFT, TRUE), "button down");
  bpress = last_button_serial ();
  HOOK (_gdk_wayland_test_pointer_button (disp, tick (), GDKW_TEST_BTN_LEFT, FALSE), "button up");
  HOOK (_gdk_wayland_test_keyboard_key (disp, tick (), GDKW_TEST_KEY_A, TRUE), "key down");
  kpress = last_key_serial ();
  HOOK (_gdk_wayland_test_keyboard_key (disp, tick (), GDKW_TEST_KEY_A, FALSE), "key up");
  T_CHECK ((gint32) (kpress - bpress) > 0, "the key press is newer than the button press");
  t_pump_ms (30); /* GTK reads the four events before the menu opens, as it does with a real seat */
  gtk_menu_popup (GTK_MENU (menu), NULL, NULL, pos_func, NULL, 0, gtk_get_current_event_time ());
  T_CHECK (t_wait_until (menu_committed, menu, 2000), "A newest: committed after button, then key");
  pi = popup_info (menu);
  T_CHECK (pi.grab_sent && pi.grab_serial == kpress, "A newest: key press newer than button press, serial %u, key press %u",
           pi.grab_serial, kpress);
  close_menu (menu);

  /* key press and release, then button press and release: the button press is newer */
  HOOK (_gdk_wayland_test_keyboard_key (disp, tick (), GDKW_TEST_KEY_A, TRUE), "key down 2");
  kpress = last_key_serial ();
  HOOK (_gdk_wayland_test_keyboard_key (disp, tick (), GDKW_TEST_KEY_A, FALSE), "key up 2");
  HOOK (_gdk_wayland_test_pointer_button (disp, tick (), GDKW_TEST_BTN_LEFT, TRUE), "button down 2");
  bpress = last_button_serial ();
  HOOK (_gdk_wayland_test_pointer_button (disp, tick (), GDKW_TEST_BTN_LEFT, FALSE), "button up 2");
  t_pump_ms (30);
  menu = make_menu ();
  gtk_menu_popup (GTK_MENU (menu), NULL, NULL, pos_func, NULL, 0, gtk_get_current_event_time ());
  T_CHECK (t_wait_until (menu_committed, menu, 2000), "A newest: committed after key, then button");
  pi = popup_info (menu);
  T_CHECK (pi.grab_sent && pi.grab_serial == bpress, "A newest: button press newer than key press, serial %u, button press %u",
           pi.grab_serial, bpress);
  close_menu (menu);
}

/* ---- B: a held button survives a focus change inside the client ---------------- */

/* The compositor owns the button state.  A press that reached this client is still down until
 * the release arrives, also when the compositor moves the pointer focus to another surface of
 * the client in between (an xdg_popup grab does that on some compositors) and also when it sends
 * an enter for the surface that already has the focus.  The release has to reach GTK, or the
 * menu stays open and the item under the pointer is never activated. */

static GDKWTestSeatInfo
seat_info (void)
{
  GDKWTestSeatInfo si;

  memset (&si, 0, sizeof si);
  HOOK (_gdk_wayland_test_seat_info (disp, &si), "seat info");
  return si;
}

static gboolean
bar_menu_mapped (gpointer p)
{
  GDKWTestPopupInfo pi = popup_info (bar_menu);

  return pi.state == GDKW_TEST_POPUP_MAPPED;
}

static gboolean
bar_menu_gone (gpointer p)
{
  return !gtk_widget_get_mapped (bar_menu);
}

/* centre of a widget in the surface coordinates of the main window */
static void
centre_of (GtkWidget *w, gdouble *x, gdouble *y)
{
  TRect r = t_widget_rect (w, main_win);

  *x = r.x + r.w / 2.0;
  *y = r.y + r.h / 2.0;
}

/* centre of item  of the bar menu in the surface coordinates of its popup window */
static void
menu_item_centre (int n, gdouble *x, gdouble *y)
{
  GList *c = gtk_container_get_children (GTK_CONTAINER (bar_menu));
  GtkWidget *it = g_list_nth_data (c, n), *top = GTK_WIDGET (GTK_MENU (bar_menu)->toplevel);
  TRect r = t_widget_rect (it, top);

  g_list_free (c);
  *x = r.x + r.w / 2.0;
  *y = r.y + r.h / 2.0;
}

/* Presses on "File" and waits for the menu.  Leaves the pointer focus on the main window and the
 * button down. */
static gboolean
open_bar_menu_held (void)
{
  gdouble x, y;

  centre_of (bar_file, &x, &y);
  HOOK (_gdk_wayland_test_pointer_enter (disp, main_win->window, x, y), "enter on File");
  HOOK (_gdk_wayland_test_pointer_motion (disp, tick (), x + 1, y), "motion on File");
  t_pump_ms (50);
  HOOK (_gdk_wayland_test_pointer_button (disp, tick (), GDKW_TEST_BTN_LEFT, TRUE), "press on File");
  if (!t_wait_until (bar_menu_mapped, NULL, 3000))
    {
      T_CHECK (FALSE, "B: the File menu did not map after the press");
      return FALSE;
    }
  T_CHECK (seat_info ().button_mask & GDK_BUTTON1_MASK, "B: the press is recorded");
  return TRUE;
}

/* leaves everything closed and the seat idle for the next case */
static void
reset_after_case (void)
{
  GDKWTestSeatInfo si = seat_info ();

  if (si.button_mask & GDK_BUTTON1_MASK)
    HOOK (_gdk_wayland_test_pointer_button (disp, tick_long (), GDKW_TEST_BTN_LEFT, FALSE), "cleanup release");
  if (gtk_widget_get_mapped (bar_menu))
    {
      gtk_menu_shell_deactivate (GTK_MENU_SHELL (GTK_WIDGET (bar_file)->parent));
      gtk_menu_popdown (GTK_MENU (bar_menu));
    }
  t_settle (150);
  HOOK (_gdk_wayland_test_pointer_leave (disp, main_win->window), "cleanup leave");
  t_pump_ms (50);
}

/* An enter for the surface that has the focus, with the button down. */
static void
test_enter_while_held (void)
{
  gdouble x, y;
  int rel0;

  if (!open_bar_menu_held ())
    return;
  rel0 = ev.release;
  centre_of (bar_file, &x, &y);
  HOOK (_gdk_wayland_test_pointer_enter (disp, main_win->window, x + 2, y + 2), "enter while held");
  T_CHECK (seat_info ().button_mask & GDK_BUTTON1_MASK, "B enter: the press is still recorded after the enter");
  HOOK (_gdk_wayland_test_pointer_motion (disp, tick (), 300, 250), "motion outside the menu");
  t_pump_ms (50);
  HOOK (_gdk_wayland_test_pointer_button (disp, tick_long (), GDKW_TEST_BTN_LEFT, FALSE), "release outside");
  T_CHECK (t_wait_until (bar_menu_gone, NULL, 2000), "B enter: the release outside hides the menu");
  T_CHECK (ev.release == rel0 + 1, "B enter: the release reached GTK (%d, before %d)", ev.release, rel0);
  T_CHECK (!(seat_info ().button_mask & GDK_BUTTON1_MASK), "B enter: no button is held at the end");
  reset_after_case ();
}

/* leave for the main window, enter for the popup, with the button down: the focus moves to
 * another surface of the client.  The release over an item has to activate it. */
static void
test_leave_enter_popup_while_held (void)
{
  gdouble x, y;
  int rel0, act0;

  if (!open_bar_menu_held ())
    return;
  rel0 = ev.release;
  act0 = ev.activate;
  menu_item_centre (1, &x, &y);
  HOOK (_gdk_wayland_test_pointer_leave (disp, main_win->window), "leave main while held");
  HOOK (_gdk_wayland_test_pointer_enter (disp, GTK_WIDGET (GTK_MENU (bar_menu)->toplevel)->window, x, y),
        "enter popup while held");
  T_CHECK (seat_info ().button_mask & GDK_BUTTON1_MASK, "B pair: the press is still recorded after leave and enter");
  HOOK (_gdk_wayland_test_pointer_motion (disp, tick (), x + 1, y), "motion on the item");
  t_pump_ms (50);
  HOOK (_gdk_wayland_test_pointer_button (disp, tick_long (), GDKW_TEST_BTN_LEFT, FALSE), "release on the item");
  T_CHECK (t_wait_until (bar_menu_gone, NULL, 2000), "B pair: the release over an item closes the menu");
  T_CHECK (ev.release == rel0 + 1, "B pair: the release reached GTK (%d, before %d)", ev.release, rel0);
  T_CHECK (ev.activate == act0 + 1, "B pair: the item activated (%d, before %d)", ev.activate, act0);
  T_CHECK (!(seat_info ().button_mask & GDK_BUTTON1_MASK), "B pair: no button is held at the end");
  reset_after_case ();
}

/* The focus leaves and no surface of the client gets it back: no release will come.  The seat
 * must not keep the button down for ever. */
static void
test_leave_without_enter (void)
{
  int press0;

  if (!open_bar_menu_held ())
    return;
  HOOK (_gdk_wayland_test_pointer_leave (disp, main_win->window), "leave main, nobody gets the focus");
  t_pump_ms (100);
  T_CHECK (!(seat_info ().button_mask & GDK_BUTTON1_MASK), "B leave: the held button is forgotten when no surface gets the focus");
  T_CHECK (seat_info ().pointer_focus == NULL, "B leave: no pointer focus");
  /* the user still holds the button, the focus comes back and the release arrives: the menu waits
   * for it, so it must be delivered */
  {
    int rel0 = ev.release;

    HOOK (_gdk_wayland_test_pointer_enter (disp, main_win->window, 300, 250), "enter again");
    HOOK (_gdk_wayland_test_pointer_button (disp, tick_long (), GDKW_TEST_BTN_LEFT, FALSE), "late release");
    T_CHECK (t_wait_until (bar_menu_gone, NULL, 2000), "B leave: a late release closes the menu that holds the grab");
    T_CHECK (ev.release == rel0 + 1, "B leave: the late release reached GTK (%d, before %d)", ev.release, rel0);
  }
  reset_after_case ();

  /* the seat works again: a new press is delivered */
  press0 = ev.press;
  if (!open_bar_menu_held ())
    return;
  T_CHECK (ev.press == press0 + 1, "B leave: a new press is delivered afterwards");
  reset_after_case ();
}

/* A release that belongs to no press this client saw, and no grab is waiting for it. */
static void
test_stray_release (void)
{
  int rel0 = ev.release;

  HOOK (_gdk_wayland_test_pointer_enter (disp, main_win->window, 300, 250), "enter for the stray release");
  t_pump_ms (50);
  HOOK (_gdk_wayland_test_pointer_button (disp, tick_long (), GDKW_TEST_BTN_LEFT, FALSE), "stray release");
  t_pump_ms (100);
  T_CHECK (ev.release == rel0, "B stray: a release without a press and without a grab is dropped (%d, before %d)", ev.release, rel0);
  reset_after_case ();
}

/* ---- C: the popup under a held button goes away ------------------------------ */

/* With a button down the compositor keeps the focus on the surface of the press, and the
 * backend aims the events at the popup under the pointer.  When that popup is hidden (Escape)
 * or ended by the compositor (popup_done), the pointer is over the main window again.  The
 * target has to move at once: the next scroll and the leave of the pointer belong to the main
 * window, not to a window that is gone. */

static int scroll_main, scroll_other, leave_main, leave_other;

static void
on_event_c (GdkEvent *e, gpointer data)
{
  if (e->type == GDK_SCROLL)
    {
      if (e->any.window == main_win->window || gdk_window_get_toplevel (e->any.window) == main_win->window)
        scroll_main++;
      else
        scroll_other++;
    }
  else if (e->type == GDK_LEAVE_NOTIFY)
    {
      if (e->any.window == main_win->window)
        leave_main++;
      else if (gdk_window_get_toplevel (e->any.window) != main_win->window)
        leave_other++;
    }
  on_event (e, data);
}

static void
test_dismiss_under_held_button (gboolean compositor)
{
  gdouble x, y;
  GDKWTestSeatInfo si;
  const char *what = compositor ? "popup_done" : "hide";

  gdk_event_handler_set (on_event_c, NULL, NULL);
  if (!open_bar_menu_held ())
    return;
  menu_item_centre (1, &x, &y);
  /* the pointer moves over the popup: the target is the popup */
  HOOK (_gdk_wayland_test_pointer_motion (disp, tick (), 40, 40), "motion over the popup");
  t_pump_ms (50);
  si = seat_info ();
  T_CHECK (si.pointer_focus == main_win->window, "C %s: the focus stays on the main window", what);
  T_CHECK (si.pointer_target == GTK_WIDGET (GTK_MENU (bar_menu)->toplevel)->window,
           "C %s: the target is the popup while the pointer is over it", what);

  if (compositor)
    HOOK (_gdk_wayland_test_popup_done (GTK_WIDGET (GTK_MENU (bar_menu)->toplevel)->window), "popup_done");
  else
    {
      gtk_menu_shell_deactivate (GTK_MENU_SHELL (GTK_WIDGET (bar_file)->parent));
      gtk_menu_popdown (GTK_MENU (bar_menu));
    }
  t_pump_ms (100);
  si = seat_info ();
  T_CHECK (si.pointer_focus == main_win->window, "C %s: the focus is still the main window", what);
  T_CHECK (si.pointer_target == main_win->window,
           "C %s: the target is back on the main window as soon as the popup is gone", what);

  /* the first event after the dismissal: a scroll and then the leave */
  scroll_main = scroll_other = leave_main = leave_other = 0;
  HOOK (_gdk_wayland_test_pointer_axis_source (disp, GDKW_TEST_AXIS_SOURCE_WHEEL), "axis source");
  HOOK (_gdk_wayland_test_pointer_axis_discrete (disp, GDKW_TEST_AXIS_VERTICAL, 1), "axis discrete");
  HOOK (_gdk_wayland_test_pointer_axis (disp, tick (), GDKW_TEST_AXIS_VERTICAL, 10.0), "axis");
  HOOK (_gdk_wayland_test_pointer_frame (disp), "frame");
  t_pump_ms (50);
  T_CHECK (scroll_main >= 1 && scroll_other == 0, "C %s: the scroll reaches the main window (%d, other %d)", what,
           scroll_main, scroll_other);
  HOOK (_gdk_wayland_test_pointer_button (disp, tick_long (), GDKW_TEST_BTN_LEFT, FALSE), "release");
  HOOK (_gdk_wayland_test_pointer_leave (disp, main_win->window), "leave");
  t_pump_ms (50);
  T_CHECK (leave_other == 0, "C %s: no leave goes to a window of the dismissed menu (%d)", what, leave_other);
  gdk_event_handler_set (on_event, NULL, NULL);
  reset_after_case ();
}

/* ---- main -------------------------------------------------------------- */

int
main (int argc, char **argv)
{
  t_begin ("T0.13", &argc, &argv);
  if (!t_is_wayland ())
    return t_skip ("the test hooks exist in the Wayland backend only");
  disp = gdk_display_get_default ();

  gdk_event_handler_set (on_event, NULL, NULL);
  main_win = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title (GTK_WINDOW (main_win), "T0.13 seat hooks");
  gtk_window_set_default_size (GTK_WINDOW (main_win), 400, 300);
  {
    GtkWidget *box = gtk_vbox_new (FALSE, 0), *bar = gtk_menu_bar_new (), *area = gtk_event_box_new ();
    GList *c, *i;

    bar_file = gtk_menu_item_new_with_label ("File");
    bar_menu = make_menu ();
    c = gtk_container_get_children (GTK_CONTAINER (bar_menu));
    for (i = c; i; i = i->next)
      g_signal_connect (i->data, "activate", G_CALLBACK (on_item_activate), NULL);
    g_list_free (c);
    gtk_menu_item_set_submenu (GTK_MENU_ITEM (bar_file), bar_menu);
    gtk_menu_shell_append (GTK_MENU_SHELL (bar), bar_file);
    gtk_widget_set_size_request (area, 400, 260);
    gtk_box_pack_start (GTK_BOX (box), bar, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (box), area, TRUE, TRUE, 0);
    gtk_container_add (GTK_CONTAINER (main_win), box);
  }
  gtk_widget_show_all (main_win);
  t_settle (600);

  HOOK (_gdk_wayland_test_popup_virtual_seat (disp, TRUE), "virtual seat");
  HOOK (_gdk_wayland_test_keyboard_keymap (disp), "keymap");
  HOOK (_gdk_wayland_test_keyboard_enter (disp, main_win->window), "keyboard enter");
  HOOK (_gdk_wayland_test_pointer_enter (disp, main_win->window, 200, 150), "pointer enter");
  t_settle (100);

  test_grab_serial_key ();
  test_grab_serial_newest_press ();
  test_enter_while_held ();
  test_leave_enter_popup_while_held ();
  test_leave_without_enter ();
  test_stray_release ();
  test_dismiss_under_held_button (FALSE);
  test_dismiss_under_held_button (TRUE);

  HOOK (_gdk_wayland_test_pointer_leave (disp, main_win->window), "pointer leave");
  HOOK (_gdk_wayland_test_keyboard_leave (disp, main_win->window), "keyboard leave");
  HOOK (_gdk_wayland_test_popup_virtual_seat (disp, FALSE), "virtual seat off");
  gtk_widget_destroy (main_win);
  t_settle (100);
  return t_finish ();
}
