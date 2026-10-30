/*
 * T0.8  A drag started without a seat fails cleanly (G3).
 *
 * gtk_drag_begin () grabs the pointer first.  On headless weston there is no
 * wl_seat, so the grab fails and gtk_drag_begin must return NULL, release its
 * IPC widget, log nothing critical and leave GTK usable.  With a seat (Xvfb)
 * the grab succeeds, the drag starts (grab_dnd_keys runs) and the program
 * ends it by dropping the grabs.  Either way: no crash, no CRITICAL, and a
 * second try behaves the same as the first.
 *
 * Without a seat the grab_dnd_keys / ungrab_dnd_keys code of G3 is not
 * reached; stage 3 on sway covers it.  This test says which path ran.
 */
#include "common.h"

int
main (int argc, char **argv)
{
  GtkWidget *win, *button, *dest;
  GtkTargetEntry te[] = { { (char *) "text/plain", 0, 0 }, { (char *) "UTF8_STRING", 0, 1 } };
  GtkTargetList *tl;
  GdkDragContext *ctx;
  int i, started = 0, failed = 0;

  t_begin ("T0.8", &argc, &argv);

  win = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  button = gtk_button_new_with_label ("drag source");
  dest = gtk_label_new ("drop target");
  {
    GtkWidget *box = gtk_vbox_new (FALSE, 4);
    gtk_box_pack_start (GTK_BOX (box), button, TRUE, TRUE, 0);
    gtk_box_pack_start (GTK_BOX (box), dest, TRUE, TRUE, 0);
    gtk_container_add (GTK_CONTAINER (win), box);
  }
  gtk_drag_source_set (button, GDK_BUTTON1_MASK, te, G_N_ELEMENTS (te), GDK_ACTION_COPY);
  gtk_drag_dest_set (dest, GTK_DEST_DEFAULT_ALL, te, G_N_ELEMENTS (te), GDK_ACTION_COPY);
  gtk_widget_show_all (win);
  t_settle (300);

  tl = gtk_target_list_new (te, G_N_ELEMENTS (te));
  for (i = 0; i < 2; i++)
    {
      ctx = gtk_drag_begin (button, tl, GDK_ACTION_COPY, 1, NULL);
      T_INFO ("attempt %d: gtk_drag_begin returned %s", i + 1, ctx ? "a context" : "NULL");
      if (ctx)
        {
          started++;
          t_pump_ms (200);
          /* end the drag: drop the grabs, GTK cancels it on the broken grab */
          gdk_pointer_ungrab (GDK_CURRENT_TIME);
          gdk_keyboard_ungrab (GDK_CURRENT_TIME);
          t_pump_ms (200);
        }
      else
        failed++;
      t_settle (150);
    }
  gtk_target_list_unref (tl);

  if (t_is_wayland ())
    {
      /* headless weston: the virtual seat of the backend may or may not exist;
       * both outcomes are legal as long as they are clean and repeatable */
      T_CHECK (started == 0 || failed == 0, "attempts disagree: %d started, %d failed", started, failed);
      T_INFO ("wayland: %d started, %d failed cleanly", started, failed);
    }
  else
    T_CHECK (started == 2, "X11 with a pointer: %d of 2 drags started", started);

  /* GTK must still work after the failed or ended drag */
  gtk_widget_queue_draw (win);
  t_settle (300);
  T_CHECK (gtk_widget_get_mapped (win), "window no longer mapped");
  T_CHECK (gtk_grab_get_current () == NULL || started > 0, "a GTK grab is left after the failed drag");

  gtk_widget_destroy (win);
  t_settle (100);
  return t_finish ();
}
