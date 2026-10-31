/*
 * T0.16  A surface that changes scale right after its first commit is
 *        painted in full.
 *
 * Diego saw blank regions on sway with two outputs (a 1.5 scaled panel that
 * wl_output reports as scale 2, and a scale 1 output).  A new surface starts at
 * the scale of the first output, 2, gets its first commit, and a moment later
 * wl_surface.enter and preferred_buffer_scale say 1.  The content was only
 * partly painted after the flip: the combos of a dialog were invisible and a
 * tall popup showed only the rows that had been hovered.
 *
 * The test builds a window with labels, entries, combo boxes, a spin button,
 * a tree view and a drawing area that paints a known pattern (8x8 logical
 * cells, one colour per cell).  It delivers the scale change through the M10
 * hooks in the same dispatch as the first commit (wl_surface.enter for fake
 * outputs and preferred_buffer_scale), in four orders:
 *   expose-before-configure  the window was drawn before the first commit
 *   configure-then-flip      the first commit holds only the background
 *   flip-while-waiting       the frame callback is held, the flip comes while
 *                            the first commit is on screen
 *   double-flip              2, 1, 2, 1 without a redraw between
 * Then it requires, for the window in its final state:
 *   - the back store has the size of the final scale,
 *   - every cell of the pattern has its colour,
 *   - the last committed buffer equals the back store,
 *   - after a full invalidate and redraw, the back store does not change:
 *     no region was left unpainted (compare with a fresh draw at that scale).
 * It also pops up a tall menu again and again (with T_HAVE_SEAT=1 on a real seat, else on the
 * virtual seat of the M9 hooks, so weston runs it too) and
 * flips their scale in the same way (hook, and the real events of the
 * compositor when the outputs differ).  The first window of the process also
 * takes the real events of the compositor, so on a sway with a scale 2 and a
 * scale 1 output the flip is real.  On X11 only the checks without hooks run
 * (no flip, same regions compared).
 */
#include "common.h"
#include <ydk/gdkm9-wayland.h>
#include <ydk/gdkm10-wayland.h>

#define WIN_W 560
#define WIN_H 600
#define CELL 8
#define N_ITEMS 24

typedef struct
{
  int      w, h;       /* pixels */
  int      scale;      /* device pixels per logical pixel, 1 when read without hooks */
  guint32 *px;
} Snap;

static GtkWidget *g_da;
static void (*g_expose_hook) (void);

static guint32
cell_color (int cx, int cy)
{
  guint r = (cx * 37 + 11) & 0xff, g = (cy * 53 + 29) & 0xff;
  guint b = ((cx + cy) & 1) ? 0xf0 : 0x30;
  /* never the window background, never black */
  return (r << 16) | (g << 8) | b;
}

static gboolean
da_expose (GtkWidget *w, GdkEventExpose *e, gpointer data)
{
  cairo_t *cr = gdk_cairo_create (w->window);
  int x, y;

  if (g_expose_hook)
    {
      void (*h) (void) = g_expose_hook;
      g_expose_hook = NULL;
      h ();            /* the flip arrives in the middle of the first paint */
      cairo_destroy (cr);
      cr = gdk_cairo_create (w->window);
    }

  gdk_cairo_rectangle (cr, &e->area);
  cairo_clip (cr);
  for (y = 0; y < w->allocation.height; y += CELL)
    for (x = 0; x < w->allocation.width; x += CELL)
      {
        guint32 c = cell_color (x / CELL, y / CELL);
        cairo_set_source_rgb (cr, ((c >> 16) & 0xff) / 255.0, ((c >> 8) & 0xff) / 255.0, (c & 0xff) / 255.0);
        cairo_rectangle (cr, x, y, CELL, CELL);
        cairo_fill (cr);
      }
  cairo_destroy (cr);
  return TRUE;
}

static GtkWidget *
make_combo (int base)
{
  GtkWidget *c = gtk_combo_box_new_text ();
  int i;
  for (i = 0; i < N_ITEMS; i++)
    {
      char *s = g_strdup_printf ("Entry %d (%d samples)", base + i, 16 << (i % 8));
      gtk_combo_box_append_text (GTK_COMBO_BOX (c), s);
      g_free (s);
    }
  gtk_combo_box_set_active (GTK_COMBO_BOX (c), 2);
  return c;
}

static GtkWidget *
make_window (void)
{
  GtkWidget *win = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  GtkWidget *box = gtk_vbox_new (FALSE, 4), *tab = gtk_table_new (6, 2, FALSE), *sw, *tv, *da;
  GtkListStore *st = gtk_list_store_new (2, G_TYPE_STRING, G_TYPE_INT);
  GtkTreeIter it;
  int i;

  gtk_window_set_default_size (GTK_WINDOW (win), WIN_W, WIN_H);
  gtk_window_set_resizable (GTK_WINDOW (win), FALSE);
  gtk_container_set_border_width (GTK_CONTAINER (box), 6);
  gtk_table_set_col_spacings (GTK_TABLE (tab), 8);
  gtk_table_set_row_spacings (GTK_TABLE (tab), 4);
  gtk_table_attach (GTK_TABLE (tab), gtk_label_new ("Driver:"), 0, 1, 0, 1, GTK_FILL, GTK_FILL, 0, 0);
  gtk_table_attach (GTK_TABLE (tab), make_combo (0), 1, 2, 0, 1, GTK_EXPAND | GTK_FILL, GTK_FILL, 0, 0);
  gtk_table_attach (GTK_TABLE (tab), gtk_label_new ("Device:"), 0, 1, 1, 2, GTK_FILL, GTK_FILL, 0, 0);
  gtk_table_attach (GTK_TABLE (tab), make_combo (100), 1, 2, 1, 2, GTK_EXPAND | GTK_FILL, GTK_FILL, 0, 0);
  gtk_table_attach (GTK_TABLE (tab), gtk_label_new ("Sample rate:"), 0, 1, 2, 3, GTK_FILL, GTK_FILL, 0, 0);
  gtk_table_attach (GTK_TABLE (tab), make_combo (200), 1, 2, 2, 3, GTK_EXPAND | GTK_FILL, GTK_FILL, 0, 0);
  gtk_table_attach (GTK_TABLE (tab), gtk_label_new ("Name:"), 0, 1, 3, 4, GTK_FILL, GTK_FILL, 0, 0);
  {
    GtkWidget *e = gtk_entry_new ();
    gtk_entry_set_text (GTK_ENTRY (e), "Some text in an entry");
    gtk_widget_set_can_focus (e, FALSE);
    gtk_table_attach (GTK_TABLE (tab), e, 1, 2, 3, 4, GTK_EXPAND | GTK_FILL, GTK_FILL, 0, 0);
  }
  gtk_table_attach (GTK_TABLE (tab), gtk_label_new ("Buffer:"), 0, 1, 4, 5, GTK_FILL, GTK_FILL, 0, 0);
  {
    GtkWidget *s = gtk_spin_button_new_with_range (0, 4096, 16);
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (s), 256);
    gtk_widget_set_can_focus (s, FALSE);
    gtk_table_attach (GTK_TABLE (tab), s, 1, 2, 4, 5, GTK_EXPAND | GTK_FILL, GTK_FILL, 0, 0);
  }
  gtk_table_attach (GTK_TABLE (tab), gtk_check_button_new_with_label ("Enable"), 0, 1, 5, 6, GTK_FILL, GTK_FILL, 0, 0);
  gtk_table_attach (GTK_TABLE (tab), gtk_button_new_with_label ("Start"), 1, 2, 5, 6, GTK_EXPAND | GTK_FILL, GTK_FILL, 0, 0);
  gtk_box_pack_start (GTK_BOX (box), tab, FALSE, FALSE, 0);

  for (i = 0; i < 30; i++)
    {
      char *s = g_strdup_printf ("Row %d", i);
      gtk_list_store_append (st, &it);
      gtk_list_store_set (st, &it, 0, s, 1, i, -1);
      g_free (s);
    }
  tv = gtk_tree_view_new_with_model (GTK_TREE_MODEL (st));
  g_object_unref (st);
  gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (tv), -1, "Name", gtk_cell_renderer_text_new (), "text", 0, NULL);
  gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (tv), -1, "Value", gtk_cell_renderer_text_new (), "text", 1, NULL);
  gtk_widget_set_can_focus (tv, FALSE);
  sw = gtk_scrolled_window_new (NULL, NULL);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sw), GTK_POLICY_NEVER, GTK_POLICY_NEVER);
  gtk_container_add (GTK_CONTAINER (sw), tv);
  gtk_widget_set_size_request (sw, -1, 110);
  gtk_box_pack_start (GTK_BOX (box), sw, FALSE, FALSE, 0);

  da = gtk_drawing_area_new ();
  g_signal_connect (da, "expose-event", G_CALLBACK (da_expose), NULL);
  gtk_box_pack_start (GTK_BOX (box), da, TRUE, TRUE, 0);
  gtk_widget_set_size_request (da, WIN_W - 12, 280);
  g_da = da;
  gtk_container_add (GTK_CONTAINER (win), box);
  return win;
}

/* ---- snapshots ------------------------------------------------------- */

static void
snap_free (Snap *s)
{
  g_free (s->px);
  s->px = NULL;
}

/* The pixels of the window: the device pixels of the back store on Wayland,
 * the logical pixels of the window read-back elsewhere. */
static gboolean
snap_take (GdkWindow *win, Snap *s)
{
  GDKWTestBackingInfo bi;
  int x, y;

  memset (s, 0, sizeof *s);
  if (t_is_wayland () && _gdk_wayland_test_backing_info (win, &bi) && bi.data)
    {
      s->w = bi.px_width;
      s->h = bi.px_height;
      s->scale = bi.scale120 / 120;
      s->px = g_new (guint32, (gsize) s->w * s->h);
      for (y = 0; y < s->h; y++)
        for (x = 0; x < s->w; x++)
          s->px[(gsize) y * s->w + x] = ((guint32 *) ((guchar *) bi.data + (gsize) y * bi.stride))[x] & 0xffffff;
      return TRUE;
    }
  {
    int w, h;
    TShot *sh;
    gdk_drawable_get_size (win, &w, &h);
    sh = t_capture (win, 0, 0, w, h);
    if (!sh)
      return FALSE;
    s->w = w;
    s->h = h;
    s->scale = 1;
    s->px = g_new (guint32, (gsize) w * h);
    for (y = 0; y < h; y++)
      for (x = 0; x < w; x++)
        s->px[(gsize) y * w + x] = t_px (sh, x, y);
    t_shot_free (sh);
    return TRUE;
  }
}

/* Differing pixels; prints the first few 32x32 blocks that differ. */
static int
snap_diff (const char *what, const Snap *a, const Snap *b)
{
  int x, y, n = 0, shown = 0;
  GHashTable *seen;

  if (a->w != b->w || a->h != b->h)
    {
      T_INFO ("%s: sizes differ %dx%d and %dx%d", what, a->w, a->h, b->w, b->h);
      return G_MAXINT;
    }
  seen = g_hash_table_new (g_direct_hash, g_direct_equal);
  for (y = 0; y < a->h; y++)
    for (x = 0; x < a->w; x++)
      if (a->px[(gsize) y * a->w + x] != b->px[(gsize) y * a->w + x])
        {
          gpointer k = GINT_TO_POINTER (((y / 32) << 12 | (x / 32)) + 1);
          n++;
          if (!g_hash_table_contains (seen, k) && shown < 6)
            {
              g_hash_table_insert (seen, k, NULL);
              T_INFO ("%s: block at device (%d,%d) differs, %06x before, %06x after", what, x / 32 * 32,
                      y / 32 * 32, a->px[(gsize) y * a->w + x], b->px[(gsize) y * a->w + x]);
              shown++;
            }
        }
  g_hash_table_destroy (seen);
  return n;
}

/* Checks every cell of the pattern in the drawing area.  Returns the number of wrong cells. */
static int
check_pattern (const char *what, GtkWidget *toplevel, const Snap *s)
{
  int cx, cy, bad = 0, first_x = -1, first_y = -1;
  guint32 first_got = 0;
  GtkWidget *da = g_da;
  int ox = 0, oy = 0;

  gtk_widget_translate_coordinates (da, toplevel, 0, 0, &ox, &oy);
  for (cy = 0; cy * CELL + CELL <= da->allocation.height; cy++)
    for (cx = 0; cx * CELL + CELL <= da->allocation.width; cx++)
      {
        int px = (ox + cx * CELL + CELL / 2) * s->scale, py = (oy + cy * CELL + CELL / 2) * s->scale;
        guint32 got = (px < s->w && py < s->h) ? s->px[(gsize) py * s->w + px] & 0xffffff : 0xdeadbe;
        if (got != cell_color (cx, cy))
          {
            if (!bad)
              {
                first_x = cx;
                first_y = cy;
                first_got = got;
              }
            bad++;
          }
      }
  if (bad)
    T_INFO ("%s: %d pattern cells wrong, first at cell (%d,%d): %06x, wanted %06x", what, bad, first_x, first_y,
            first_got, cell_color (first_x, first_y));
  return bad;
}

/* ---- waiting --------------------------------------------------------- */

static gboolean
is_quiet (gpointer p)
{
  GDKWTestFrameInfo fi;

  if (!_gdk_wayland_test_frame_info (p, &fi))
    return TRUE;
  return !fi.commit_wanted && !fi.settle_armed && fi.state != GDKW_TEST_FRAME_UNMAPPED;
}

/* Lets redraws, frame callbacks and deferred commits finish. */
static void
quiesce (GdkWindow *win)
{
  int i;
  for (i = 0; i < 4; i++)
    {
      t_settle (150);
      if (t_is_wayland ())
        t_wait_until (is_quiet, win, 2000);
    }
  t_settle (150);
}

static gboolean
committed (gpointer p)
{
  GDKWTestFrameInfo fi;
  return _gdk_wayland_test_frame_info (p, &fi) && fi.commits >= 1;
}

/* ---- one window, one verdict ------------------------------------------ */

static void
verify (const char *what, GtkWidget *toplevel, gboolean pattern)
{
  GdkWindow *win = toplevel->window;
  Snap s1, s2;
  int bad;

  quiesce (win);
  if (t_is_wayland ())
    {
      GDKWTestFrameInfo fi;
      GDKWTestScaleInfo si;
      GDKWTestBackingInfo bi;
      guint mism = 1;
      int lw, lh;

      gdk_drawable_get_size (win, &lw, &lh);
      T_CHECK (_gdk_wayland_test_scale_info (win, &si) && _gdk_wayland_test_backing_info (win, &bi), "%s: no scale info", what);
      T_INFO ("%s: scale120 %u after %u changes, store %dx%d, logical %dx%d", what, si.scale120, si.scale_changes,
              bi.px_width, bi.px_height, lw, lh);
      T_CHECK (bi.px_width == GDKW_TEST_SCALE_PX (lw, si.scale120) && bi.px_height == GDKW_TEST_SCALE_PX (lh, si.scale120),
               "%s: store %dx%d does not match scale120 %u", what, bi.px_width, bi.px_height, si.scale120);
      T_CHECK (_gdk_wayland_test_frame_info (win, &fi), "%s: no frame info", what);
      T_CHECK (fi.last_attach_width == bi.px_width && fi.last_attach_height == bi.px_height,
               "%s: the last commit attached %dx%d, the store is %dx%d", what, fi.last_attach_width,
               fi.last_attach_height, bi.px_width, bi.px_height);
      T_CHECK (si.buffer_scale_sent == (gint) ((si.scale120 + 119) / 120),
               "%s: set_buffer_scale %d does not match scale120 %u", what, si.buffer_scale_sent, si.scale120);
      T_CHECK (_gdk_wayland_test_buffer_vs_backing (win, fi.last_slot, &mism) && mism == 0,
               "%s: the last committed buffer differs from the back store in %u pixels", what, mism);
    }

  T_CHECK (snap_take (win, &s1), "%s: cannot read the window", what);
  if (!s1.px)
    return;
  if (pattern)
    {
      bad = check_pattern (what, toplevel, &s1);
      T_CHECK (bad == 0, "%s: %d cells of the pattern are not painted", what, bad);
    }

  /* a fresh draw at this scale: invalidate everything, redraw, compare */
  gdk_window_invalidate_rect (win, NULL, TRUE);
  quiesce (win);
  T_CHECK (snap_take (win, &s2), "%s: cannot read the window again", what);
  if (s2.px)
    {
      bad = snap_diff (what, &s1, &s2);
      T_CHECK (bad == 0, "%s: %d pixels were not painted before the full redraw", what, bad);
    }
  if (t_is_wayland ())
    {
      GDKWTestFrameInfo fi;
      guint mism = 1;
      if (_gdk_wayland_test_frame_info (win, &fi))
        T_CHECK (_gdk_wayland_test_buffer_vs_backing (win, fi.last_slot, &mism) && mism == 0,
                 "%s: after the redraw the committed buffer differs from the store in %u pixels", what, mism);
    }
  snap_free (&s1);
  snap_free (&s2);
}

/* ---- the flips --------------------------------------------------------- */

static guint out_f1, out_f2; /* fake outputs of scale 1 and 2 */

/* wl_surface.enter and preferred_buffer_scale for the scale @to, through the real listeners. */
static void
deliver_scale (GdkWindow *win, int to)
{
  _gdk_wayland_test_surface_leave (win, out_f1);
  _gdk_wayland_test_surface_leave (win, out_f2);
  _gdk_wayland_test_surface_enter (win, to == 2 ? out_f2 : out_f1);
  _gdk_wayland_test_preferred_buffer_scale (win, to);
  /* a compositor with wp_fractional_scale (sway) decides with this one first */
  _gdk_wayland_test_preferred_fractional (win, to * 120);
}

typedef enum
{
  V_EXPOSE_FIRST,
  V_CONFIGURE_FIRST,
  V_WHILE_WAITING,
  V_DOUBLE,
  V_MID_PAINT,
  V_WANTED
} Variant;

static const char *variant_name[] = { "expose-before-configure", "configure-then-flip", "flip-while-waiting", "double-flip", "flip-in-expose", "flip-with-wanted-commit" };

static GdkWindow *g_flip_win;
static int g_flip_other, g_flip_final;

static void
flip_now (void)
{
  /* a new surface starts at scale 2 (the hint): going to 1 is ONE change, like on sway */
  if (g_flip_final == 2)
    deliver_scale (g_flip_win, 1);
  deliver_scale (g_flip_win, g_flip_final);
}

static void
run_variant (Variant v, int final_scale)
{
  GdkDisplay *d = gdk_display_get_default ();
  GtkWidget *win = make_window ();
  GDKWTestScaleInfo si;
  char *what = g_strdup_printf ("%s to %d", variant_name[v], final_scale);
  int other = final_scale == 2 ? 1 : 2;

  g_print ("%s: variant %s\n", t_name, what);
  if (t_is_wayland ())
    {
      _gdk_wayland_test_frame_mode (d, (v == V_WHILE_WAITING || v == V_WANTED) ? GDKW_TEST_FRAME_MODE_HOLD : GDKW_TEST_FRAME_MODE_NORMAL);
    }
  gtk_widget_show_all (win);
  if (t_is_wayland ())
    {
      if (v == V_EXPOSE_FIRST)
        gdk_window_process_all_updates ();
      gdk_display_sync (d);      /* the configure arrives and the first commit is made */
      T_CHECK (committed (win->window), "%s: no first commit after the configure", what);
      if (v == V_WANTED)
        {
          /* a redraw while the first commit waits for its callback: a commit is wanted */
          gtk_widget_queue_draw (win);
          gdk_window_process_all_updates ();
        }
      g_flip_win = win->window;
      g_flip_other = other;
      g_flip_final = final_scale;
      if (v == V_MID_PAINT)
        g_expose_hook = flip_now;
      else
        flip_now ();
      if (v == V_DOUBLE)
        {
          deliver_scale (win->window, other);
          deliver_scale (win->window, final_scale);
        }
      _gdk_wayland_test_scale_info (win->window, &si);
      T_INFO ("%s: after the hooks scale120 %u, %u changes", what, si.scale120, si.scale_changes);
      if (v == V_WHILE_WAITING || v == V_WANTED)
        {
          t_settle (200);
          _gdk_wayland_test_frame_mode (d, GDKW_TEST_FRAME_MODE_NORMAL);
          while (_gdk_wayland_test_frame_release_held (win->window))
            ;
          _gdk_wayland_test_frame_fire (win->window);
        }
    }
  verify (what, win, TRUE);
  gtk_widget_destroy (win);
  t_settle (100);
  g_free (what);
}

/* One window shown, flipped and hidden again and again. */
static void
run_reuse (void)
{
  GdkDisplay *d = gdk_display_get_default ();
  GtkWidget *win = make_window ();
  int i;

  for (i = 0; i < 4; i++)
    {
      char *what = g_strdup_printf ("shown again, round %d", i + 1);

      g_print ("%s: variant %s\n", t_name, what);
      gtk_widget_show_all (win);
      gdk_display_sync (d);
      g_flip_win = win->window;
      g_flip_other = (i & 1) ? 1 : 2;
      g_flip_final = (i & 1) ? 2 : 1;
      flip_now ();
      verify (what, win, TRUE);
      gtk_widget_hide (win);
      t_settle (150);
      g_free (what);
    }
  gtk_widget_destroy (win);
  t_settle (100);
}

/* ---- popups (need a seat) ---------------------------------------------- */

static void
pos_func (GtkMenu *menu, gint *x, gint *y, gboolean *push_in, gpointer data)
{
  gint ox = 0, oy = 0;
  gdk_window_get_origin (GTK_WIDGET (data)->window, &ox, &oy);
  *x = ox + 30;
  *y = oy + 40;
  *push_in = TRUE;
}

static gboolean
mapped_menu (gpointer p)
{
  GtkWidget *tl = GTK_MENU (p)->toplevel;
  return tl && gtk_widget_get_mapped (tl) && gtk_widget_get_mapped (p);
}

static GtkWidget *
make_menu (void)
{
  GtkWidget *menu = gtk_menu_new ();
  int i;

  for (i = 0; i < 30; i++)
    {
      char *s = g_strdup_printf ("%d samples", 16 << (i % 9));
      gtk_menu_shell_append (GTK_MENU_SHELL (menu), gtk_menu_item_new_with_label (s));
      g_free (s);
    }
  gtk_widget_show_all (menu);
  return menu;
}

/* The menu is used again and again, like the combo box popup of a dialog: a GtkMenu keeps its
 * GdkWindow, only the Wayland surface of the popup is new each time. */
static void
popup_case (GtkWidget *parent, GtkWidget *menu, int nth, int flip, int final_scale)
{
  GdkDisplay *d = gdk_display_get_default ();
  GtkWidget *tl;
  char *what = g_strdup_printf ("tall menu popup %d%s", nth, flip == 1 ? " flipped early" : flip == 2 ? " flipped after the first paint" : "");

  g_print ("%s: popup %s\n", t_name, what);
  gtk_menu_popup (GTK_MENU (menu), NULL, NULL, pos_func, parent, 0, gtk_get_current_event_time ());
  if (t_is_wayland () && flip)
    {
      /* map and first commit, then the flip in the same dispatch.  Mode 2 lets the menu paint
       * and commit first, and a hover redraws one item after the flip, like the pointer does. */
      GList *items = gtk_container_get_children (GTK_CONTAINER (menu));

      t_wait_until (mapped_menu, menu, 2000);
      tl = GTK_MENU (menu)->toplevel;
      if (flip == 2 && tl && tl->window)
        quiesce (tl->window);
      if (tl && tl->window)
        {
          if (flip == 1 && final_scale == 2)
            deliver_scale (tl->window, 1);
          deliver_scale (tl->window, final_scale);
        }
      if (flip == 2)
        {
          gtk_menu_shell_select_item (GTK_MENU_SHELL (menu), g_list_nth_data (items, 3));
          t_settle (100);
        }
      g_list_free (items);
    }
  T_CHECK (t_wait_until (mapped_menu, menu, 2000), "%s: the menu did not map", what);
  tl = GTK_MENU (menu)->toplevel;
  if (tl && tl->window)
    {
      GdkWindow *win = tl->window;
      Snap s1, s2;
      int bad;

      (void) d;
      quiesce (win);
      if (t_is_wayland ())
        {
          GDKWTestScaleInfo si;
          GDKWTestBackingInfo bi;

          if (_gdk_wayland_test_scale_info (win, &si) && _gdk_wayland_test_backing_info (win, &bi))
            {
              T_INFO ("%s: scale120 %u after %u changes, store scale120 %u", what, si.scale120, si.scale_changes, bi.scale120);
              T_CHECK (bi.scale120 == si.scale120, "%s: the store has scale120 %u, the surface %u", what, bi.scale120, si.scale120);
            }
        }
      if (t_is_wayland ())
        {
          GDKWTestFrameInfo fi;
          guint mism = 1;
          if (_gdk_wayland_test_frame_info (win, &fi))
            T_CHECK (_gdk_wayland_test_buffer_vs_backing (win, fi.last_slot, &mism) && mism == 0,
                     "%s: the committed buffer differs from the store in %u pixels", what, mism);
        }
      if (snap_take (win, &s1))
        {
          gdk_window_invalidate_rect (win, NULL, TRUE);
          quiesce (win);
          if (snap_take (win, &s2))
            {
              bad = snap_diff (what, &s1, &s2);
              T_CHECK (bad == 0, "%s: %d pixels were not painted before the full redraw", what, bad);
              snap_free (&s2);
            }
          snap_free (&s1);
        }
    }
  gtk_menu_popdown (GTK_MENU (menu));
  t_settle (200);
  g_free (what);
}

static void
run_popups (void)
{
  GtkWidget *parent = make_window (), *menu = make_menu ();

  gtk_widget_show_all (parent);
  quiesce (parent->window);

  popup_case (parent, menu, 1, 0, 1);
  if (t_is_wayland ())
    {
      popup_case (parent, menu, 2, 1, 1);
      popup_case (parent, menu, 3, 2, 1);
      popup_case (parent, menu, 4, 2, 1);
      popup_case (parent, menu, 5, 2, 2);
      popup_case (parent, menu, 6, 2, 2);
      popup_case (parent, menu, 7, 1, 1);
    }
  gtk_widget_destroy (menu);
  gtk_widget_destroy (parent);
  t_settle (100);
}

int
main (int argc, char **argv)
{
  const char *seat = g_getenv ("T_HAVE_SEAT");
  GdkDisplay *d;

  t_begin ("T0.16", &argc, &argv);
  d = gdk_display_get_default ();
  g_object_set (gtk_settings_get_default (), "gtk-cursor-blink", FALSE, NULL);

  if (t_is_wayland ())
    {
      /* two fake outputs, so that enter and leave have something to name */
      _gdk_wayland_test_output_add (d, 5000, 0, 800, 600, 1, 0, &out_f1);
      _gdk_wayland_test_output_add (d, 6000, 0, 1600, 1200, 2, 0, &out_f2);
    }

  /* the first window of the process takes the real events of the compositor */
  {
    GtkWidget *win = make_window ();
    gtk_widget_show_all (win);
    verify ("first window, real events", win, TRUE);
    gtk_widget_destroy (win);
    t_settle (100);
  }

  if (t_is_wayland ())
    {
      run_variant (V_EXPOSE_FIRST, 1);
      run_variant (V_CONFIGURE_FIRST, 1);
      run_variant (V_WHILE_WAITING, 1);
      run_variant (V_DOUBLE, 1);
      run_variant (V_MID_PAINT, 1);
      run_variant (V_WANTED, 1);
      run_variant (V_EXPOSE_FIRST, 2);
      run_variant (V_CONFIGURE_FIRST, 2);
      run_reuse ();
    }
  else
    T_INFO ("no Wayland: the flip variants are skipped");

  if ((seat && strcmp (seat, "1") == 0) || g_getenv ("T_FORCE"))
    run_popups ();
  else if (t_is_wayland ())
    {
      /* weston has no seat: the M9 virtual seat lets the popups map without a grab */
      _gdk_wayland_test_popup_virtual_seat (d, TRUE);
      run_popups ();
      _gdk_wayland_test_popup_virtual_seat (d, FALSE);
    }
  else
    T_INFO ("no seat: the popup cases are skipped");

  return t_finish ();
}
