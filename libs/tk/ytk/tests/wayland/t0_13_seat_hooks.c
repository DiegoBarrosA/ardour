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
static GtkWidget *main_win;
static guint32 clock_ms = 100000;

static guint32
tick (void)
{
  clock_ms += 40;
  return clock_ms;
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
  menu = make_menu ();
  gtk_menu_popup (GTK_MENU (menu), NULL, NULL, pos_func, NULL, 0, gtk_get_current_event_time ());
  T_CHECK (t_wait_until (menu_committed, menu, 2000), "A newest: committed after key, then button");
  pi = popup_info (menu);
  T_CHECK (pi.grab_sent && pi.grab_serial == bpress, "A newest: button press newer than key press, serial %u, button press %u",
           pi.grab_serial, bpress);
  close_menu (menu);
}

/* ---- main --------------------------------------------------------------- */

int
main (int argc, char **argv)
{
  t_begin ("T0.13", &argc, &argv);
  if (!t_is_wayland ())
    return t_skip ("the test hooks exist in the Wayland backend only");
  disp = gdk_display_get_default ();

  main_win = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title (GTK_WINDOW (main_win), "T0.13 seat hooks");
  gtk_window_set_default_size (GTK_WINDOW (main_win), 400, 300);
  gtk_widget_show_all (main_win);
  t_settle (600);

  HOOK (_gdk_wayland_test_popup_virtual_seat (disp, TRUE), "virtual seat");
  HOOK (_gdk_wayland_test_keyboard_keymap (disp), "keymap");
  HOOK (_gdk_wayland_test_keyboard_enter (disp, main_win->window), "keyboard enter");
  HOOK (_gdk_wayland_test_pointer_enter (disp, main_win->window, 200, 150), "pointer enter");
  t_settle (100);

  test_grab_serial_key ();
  test_grab_serial_newest_press ();

  HOOK (_gdk_wayland_test_pointer_leave (disp, main_win->window), "pointer leave");
  HOOK (_gdk_wayland_test_keyboard_leave (disp, main_win->window), "keyboard leave");
  HOOK (_gdk_wayland_test_popup_virtual_seat (disp, FALSE), "virtual seat off");
  gtk_widget_destroy (main_win);
  t_settle (100);
  return t_finish ();
}
