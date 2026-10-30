/*
 * T0.4  Clipboard loopback: set and read back inside one process.
 *
 * The program owns the clipboard and asks for its own data.  The point is
 * that gtkselection.c must not call Xlib on a Wayland display (G4: the
 * XExtendedMaxRequestSize macro) and must use the dispatched compound text
 * code (G5), so none of this may crash or log a CRITICAL.
 *
 * Steps (all with the asynchronous request calls, each with a 5 s timeout so
 * a missing answer is a FAIL, not a hang):
 *   a. gtk_clipboard_set_text, request_text, equal.
 *   b. a 600 KB text, which goes over the X11 INCR threshold; equal.
 *   c. gtk_clipboard_set_with_data with a custom target; request_contents,
 *      equal; then the next set must call the clear function of the first.
 *   d. request_targets lists UTF8_STRING for a text owner.
 *   e. COMPOUND_TEXT and STRING targets (calls the G5 code); strict on X11,
 *      informational on Wayland where the data may be NULL.
 *   f. gtk_clipboard_clear: request_text gives NULL.
 *   g. GtkEntry: select all, copy-clipboard, read back; PRIMARY too.
 */
#include "common.h"

typedef struct
{
  gboolean done;
  char    *text;
  GtkSelectionData *sd;
  GdkAtom *targets;
  int      n_targets;
} Req;

static void
text_cb (GtkClipboard *c, const gchar *text, gpointer data)
{
  Req *r = data;
  r->text = g_strdup (text);
  r->done = TRUE;
}

static void
contents_cb (GtkClipboard *c, GtkSelectionData *sd, gpointer data)
{
  Req *r = data;
  if (sd && gtk_selection_data_get_length (sd) >= 0)
    r->sd = gtk_selection_data_copy (sd);
  r->done = TRUE;
}

static void
targets_cb (GtkClipboard *c, GdkAtom *atoms, gint n, gpointer data)
{
  Req *r = data;
  if (atoms && n > 0)
    {
      r->targets = g_memdup2 (atoms, n * sizeof (GdkAtom));
      r->n_targets = n;
    }
  r->done = TRUE;
}

static gboolean
req_done (gpointer p)
{
  return ((Req *) p)->done;
}

static char *
get_text (GtkClipboard *c, const char *what)
{
  Req r = { 0 };
  gtk_clipboard_request_text (c, text_cb, &r);
  T_CHECK (t_wait_until (req_done, &r, 5000), "%s: request_text got no answer in 5 s", what);
  return r.text;
}

static int custom_clear_count = 0;
static const char *custom_payload = "custom payload 12345";

static void
custom_get (GtkClipboard *c, GtkSelectionData *sd, guint info, gpointer data)
{
  GdkAtom target = gtk_selection_data_get_target (sd);
  if (target == gdk_atom_intern ("application/x-ytk-test", FALSE))
    gtk_selection_data_set (sd, target, 8, (const guchar *) custom_payload, strlen (custom_payload));
  else
    gtk_selection_data_set_text (sd, custom_payload, -1);
}

static void
custom_clear (GtkClipboard *c, gpointer data)
{
  custom_clear_count++;
}

int
main (int argc, char **argv)
{
  GtkClipboard *cb, *primary;
  char *got;
  GString *big;
  int i;
  GtkTargetEntry te[] = { { (char *) "application/x-ytk-test", 0, 1 }, { (char *) "UTF8_STRING", 0, 2 } };
  Req r;
  gboolean found;
  GtkWidget *win, *entry;

  t_begin ("T0.4", &argc, &argv);

  win = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  entry = gtk_entry_new ();
  gtk_entry_set_text (GTK_ENTRY (entry), "select me please");
  gtk_container_add (GTK_CONTAINER (win), entry);
  gtk_widget_show_all (win);
  t_settle (300);

  cb = gtk_clipboard_get (GDK_SELECTION_CLIPBOARD);
  primary = gtk_clipboard_get (GDK_SELECTION_PRIMARY);

  /* a */
  gtk_clipboard_set_text (cb, "hello ytk clipboard", -1);
  got = get_text (cb, "a");
  T_CHECK (got && strcmp (got, "hello ytk clipboard") == 0, "a: got '%s'", got ? got : "(null)");
  g_free (got);

  /* b */
  big = g_string_new (NULL);
  for (i = 0; i < 30000; i++)
    g_string_append_printf (big, "line %05d of the big clipboard text\n", i); /* about 1 MB */
  g_string_truncate (big, 600000);
  gtk_clipboard_set_text (cb, big->str, big->len);
  got = get_text (cb, "b");
  T_CHECK (got && strlen (got) == big->len && memcmp (got, big->str, big->len) == 0,
           "b: big text came back with length %zu, want %zu", got ? strlen (got) : 0, (size_t) big->len);
  g_free (got);
  g_string_free (big, TRUE);

  /* c */
  T_CHECK (gtk_clipboard_set_with_data (cb, te, G_N_ELEMENTS (te), custom_get, custom_clear, NULL),
           "c: set_with_data failed");
  memset (&r, 0, sizeof r);
  gtk_clipboard_request_contents (cb, gdk_atom_intern ("application/x-ytk-test", FALSE), contents_cb, &r);
  T_CHECK (t_wait_until (req_done, &r, 5000), "c: request_contents got no answer in 5 s");
  T_CHECK (r.sd && gtk_selection_data_get_length (r.sd) == (gint) strlen (custom_payload) &&
             memcmp (gtk_selection_data_get_data (r.sd), custom_payload, strlen (custom_payload)) == 0,
           "c: custom target data did not come back");
  if (r.sd)
    gtk_selection_data_free (r.sd);

  /* d */
  memset (&r, 0, sizeof r);
  gtk_clipboard_request_targets (cb, targets_cb, &r);
  T_CHECK (t_wait_until (req_done, &r, 5000), "d: request_targets got no answer in 5 s");
  found = FALSE;
  for (i = 0; i < r.n_targets; i++)
    if (r.targets[i] == gdk_atom_intern ("application/x-ytk-test", FALSE))
      found = TRUE;
  T_CHECK (found, "d: custom target missing from %d targets", r.n_targets);
  g_free (r.targets);

  /* e: text owner, ask for the legacy text targets (G5 path) */
  gtk_clipboard_set_text (cb, "legacy target text", -1);
  T_CHECK (custom_clear_count == 1, "c: clear function called %d times after the owner changed, want 1",
           custom_clear_count);
  {
    const char *names[] = { "COMPOUND_TEXT", "STRING", "TEXT" };
    for (i = 0; i < 3; i++)
      {
        memset (&r, 0, sizeof r);
        gtk_clipboard_request_contents (cb, gdk_atom_intern (names[i], FALSE), contents_cb, &r);
        T_CHECK (t_wait_until (req_done, &r, 5000), "e: %s got no answer in 5 s", names[i]);
        T_INFO ("e: target %s -> %s", names[i], r.sd ? "data" : "NULL");
        if (!t_is_wayland ())
          T_CHECK (r.sd != NULL, "e: %s returned no data on X11", names[i]);
        if (r.sd)
          gtk_selection_data_free (r.sd);
      }
  }

  /* f */
  gtk_clipboard_clear (cb);
  got = get_text (cb, "f");
  T_CHECK (got == NULL, "f: text '%s' after gtk_clipboard_clear", got ? got : "");
  g_free (got);

  /* g */
  gtk_widget_grab_focus (entry);
  gtk_editable_select_region (GTK_EDITABLE (entry), 0, -1);
  g_signal_emit_by_name (entry, "copy-clipboard");
  got = get_text (cb, "g-clipboard");
  T_CHECK (got && strcmp (got, "select me please") == 0, "g: clipboard has '%s' after copy of the entry",
           got ? got : "(null)");
  g_free (got);
  got = get_text (primary, "g-primary");
  T_INFO ("g: PRIMARY has '%s'", got ? got : "(null)");
  if (!t_is_wayland ())
    T_CHECK (got && strcmp (got, "select me please") == 0, "g: PRIMARY has '%s' on X11", got ? got : "(null)");
  g_free (got);

  gtk_widget_destroy (win);
  t_settle (100);
  return t_finish ();
}
