/*
 * T0.7  A modal dialog with a transient parent.
 *
 * A parent window is shown.  A modal GtkDialog with transient_for = parent is
 * run three times with gtk_dialog_run.  While it runs a timeout checks: the
 * dialog is mapped and exposed, it holds the GTK grab, it is modal and
 * transient for the parent, and its pixels are drawn.  The timeout then ends
 * the run with a response.  After each run the parent must still expose
 * (queue_draw gives an expose) and the dialog must be gone.  Close and reopen
 * three times without a freeze (stage 3 criterion).
 */
#include "common.h"

typedef struct
{
  GtkWidget *parent, *dialog;
  TWatch     dwatch;
  gboolean   mapped, grab_ok, modal, transient, expose_ok;
  int        ink;
  int        response;
} Run;

static gboolean
inspect_and_respond (gpointer data)
{
  Run *r = data;
  r->mapped = gtk_widget_get_mapped (r->dialog);
  r->grab_ok = gtk_grab_get_current () == r->dialog;
  r->modal = gtk_window_get_modal (GTK_WINDOW (r->dialog));
  r->transient = gtk_window_get_transient_for (GTK_WINDOW (r->dialog)) == GTK_WINDOW (r->parent);
  r->expose_ok = r->dwatch.expose > 0;
  if (r->mapped && r->dialog->window)
    {
      TShot *s = t_capture_toplevel (r->dialog);
      if (s)
        {
          TRect all = { 0, 0, s->w, s->h };
          r->ink = t_count_not (s, all, t_color_rgb (&gtk_widget_get_style (r->dialog)->bg[GTK_STATE_NORMAL]), NULL);
          t_shot_free (s);
        }
    }
  gtk_dialog_response (GTK_DIALOG (r->dialog), GTK_RESPONSE_OK);
  return FALSE;
}

static gboolean
exposed (gpointer p)
{
  return ((TWatch *) p)->expose > 0;
}

static int exp_base;
static gboolean
more_exposes (gpointer p)
{
  return ((TWatch *) p)->expose > exp_base;
}

int
main (int argc, char **argv)
{
  GtkWidget *parent;
  TWatch pwatch;
  int i;

  t_begin ("T0.7", &argc, &argv);

  parent = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  gtk_window_set_default_size (GTK_WINDOW (parent), 300, 200);
  gtk_container_add (GTK_CONTAINER (parent), gtk_label_new ("parent window"));
  t_watch (parent, &pwatch);
  gtk_widget_show_all (parent);
  T_CHECK (t_wait_until (exposed, &pwatch, 2000), "parent: no expose in 2 s");
  t_settle (300);

  for (i = 0; i < 3; i++)
    {
      Run r;
      int resp, before;
      memset (&r, 0, sizeof r);
      r.parent = parent;
      r.dialog = gtk_dialog_new_with_buttons ("Modal", GTK_WINDOW (parent),
                                              GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                              GTK_STOCK_OK, GTK_RESPONSE_OK, GTK_STOCK_CANCEL,
                                              GTK_RESPONSE_CANCEL, NULL);
      gtk_container_add (GTK_CONTAINER (GTK_DIALOG (r.dialog)->vbox), gtk_label_new ("Really do it?"));
      gtk_widget_show_all (GTK_DIALOG (r.dialog)->vbox);
      t_watch (r.dialog, &r.dwatch);
      g_timeout_add (t_ms (800), inspect_and_respond, &r);
      resp = gtk_dialog_run (GTK_DIALOG (r.dialog));
      T_INFO ("run %d: response %d, mapped=%d grab=%d modal=%d transient=%d dialog expose events=%d ink=%d",
              i + 1, resp, r.mapped, r.grab_ok, r.modal, r.transient, r.dwatch.expose, r.ink);
      T_CHECK (resp == GTK_RESPONSE_OK, "run %d: response %d, want OK", i + 1, resp);
      T_CHECK (r.mapped, "run %d: the dialog was not mapped while running", i + 1);
      T_CHECK (r.grab_ok, "run %d: the dialog does not hold the GTK grab while running", i + 1);
      T_CHECK (r.modal, "run %d: the dialog is not modal", i + 1);
      T_CHECK (r.transient, "run %d: the dialog is not transient for the parent", i + 1);
      T_CHECK (r.expose_ok, "run %d: the dialog got no expose event", i + 1);
      T_CHECK (r.ink > 200, "run %d: the dialog drew only %d non-background pixels", i + 1, r.ink);
      gtk_widget_destroy (r.dialog);
      t_settle (200);
      T_CHECK (gtk_grab_get_current () == NULL, "run %d: a GTK grab is left after the dialog", i + 1);

      before = pwatch.expose;
      exp_base = before;
      gtk_widget_queue_draw (parent);
      T_CHECK (t_wait_until (more_exposes, &pwatch, 1000), "run %d: the parent does not expose after the dialog closed", i + 1);
    }

  gtk_widget_destroy (parent);
  t_settle (100);
  return t_finish ();
}
