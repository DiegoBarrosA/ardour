/*
 * ydk_demo.c
 *
 * Deterministic GDK-only test client used by the smoke test and the
 * baseline screenshot-diff harness (see flake.nix apps).
 *
 * It opens the backend chosen by GDK_BACKEND (this project's extension;
 * see ydk/gdkbackend.c), draws a fixed scene into a 640x480 toplevel and
 * exits.  Exit codes are meaningful to the harness:
 *
 *   0  window created and drawn (X11 baseline and Wayland since M2); under
 *      GDK_BACKEND=wayland also every M4, M5, M5C, M5V, M6, M6B, M7 and M8 check
 *      passed (see m4_exercise, m5_exercise, m5c_exercise, m5v_exercise,
 *      m6_exercise, m6b_exercise, m7_exercise, m8_exercise); under GDK_BACKEND=x11 the dispatched M6b
 *      functions are checked to still reach X11 (m6b_x11_exercise)
 *   77 skipped: backend has no window support yet
 *   78 skipped: display opened but primary screen unavailable
 *   1  nothing worked (display open failed), an M4 / M5 / M5C / M5V / M6 / M6B / M7 / M8 check
 *      failed, or a g_critical / g_error was logged (see fatal_log_handler)
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <ydk/gdk.h>
#include <ydk/gdkcairo.h>
#include <ydk/gdkkeysyms.h>
#include <ydk/gdkseat-wayland.h>
#include <ydk/gdkm7-wayland.h>
#include <ydk/gdkm8-wayland.h>

#define WINDOW_WIDTH  640
#define WINDOW_HEIGHT 480
#define QUIT_TIMEOUT_MS  2500
#define TICK_TIMEOUT_MS    50

static GMainLoop *main_loop;
static GdkWindow *main_window;
static gboolean selection_notify_seen;
static gboolean selection_notify_refused;

/* ---- fatal CRITICALs ---------------------------------------------------
 *
 * A g_return_if_fail on a Wayland path logs a CRITICAL and returns, which
 * hides the very bugs this demo is meant to catch (a visual or colormap
 * function reading X11 state, for one).  Any
 * CRITICAL or ERROR therefore ends the run at once with a clear message
 * and exit code 1; smoke-test.sh also fails if one shows up in the log.
 * The X11 baseline run logs none, so the handler is installed for both
 * backends and the baseline stays comparable.
 * Lower levels (warning, message, ...) still go to the default handler.
 */

static void
fatal_log_handler (const gchar   *log_domain,
		   GLogLevelFlags log_level,
		   const gchar   *message,
		   gpointer       user_data)
{
  if (log_level & (G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_ERROR))
    {
      fflush (stdout);
      g_printerr ("ydk_demo: FATAL: %s-%s **: %s\n",
		  log_domain ? log_domain : "(none)",
		  (log_level & G_LOG_LEVEL_ERROR) ? "ERROR" : "CRITICAL",
		  message ? message : "(null)");
      /* _exit: gdk state may be inconsistent, do not run atexit handlers */
      _exit (1);
    }

  g_log_default_handler (log_domain, log_level, message, user_data);
}

static void
install_fatal_criticals (void)
{
  g_log_set_always_fatal (G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_ERROR);
  g_log_set_default_handler (fatal_log_handler, NULL);
}

static void
draw_scene (GdkWindow *window)
{
  cairo_t *cr;

  cr = gdk_cairo_create (window);

  cairo_set_source_rgb (cr, 1.0, 1.0, 1.0);
  cairo_paint (cr);

  cairo_set_source_rgb (cr, 0.85, 0.1, 0.1);
  cairo_arc (cr, 120.0, 120.0, 80.0, 0.0, 2.0 * G_PI);
  cairo_fill (cr);

  cairo_set_source_rgb (cr, 0.1, 0.1, 0.85);
  cairo_rectangle (cr, 400.0, 260.0, 180.0, 160.0);
  cairo_fill (cr);

  cairo_destroy (cr);

  gdk_window_flush (window);
}

static void
event_handler (GdkEvent *event, gpointer data)
{
  switch (event->type)
    {
    case GDK_EXPOSE:
      draw_scene (event->expose.window);
      break;

    case GDK_KEY_PRESS:
      if (event->key.keyval == GDK_Escape ||
	  event->key.keyval == GDK_q)
	g_main_loop_quit (main_loop);
      break;

    case GDK_BUTTON_PRESS:
      break;

    case GDK_SELECTION_NOTIFY:
      selection_notify_seen = TRUE;
      selection_notify_refused = (event->selection.property == GDK_NONE);
      break;

    default:
      break;
    }
}

static gboolean
quit_timeout (gpointer data)
{
  g_main_loop_quit (main_loop);
  return FALSE;
}

static gboolean
tick_timeout (gpointer data)
{
  gdk_window_process_all_updates ();
  return TRUE;
}

/* ---- M4 exercise ------------------------------------------------------
 *
 * Under GDK_BACKEND=wayland, drives one or more entry points from each of
 * the cursor, font, selection, drag and property families through the
 * public API (so every call crosses the gdkbackend.c dispatcher into the
 * Wayland backend).  Most of these are safe stubs until M7/M8; the checks
 * assert the stub contract (no crash, sane return values).
 */

static int m4_failures;

#define M4_CHECK(cond, what)						\
  G_STMT_START {							\
    if (!(cond))							\
      {									\
	g_printerr ("ydk_demo: M4 check failed: %s\n", what);		\
	m4_failures++;							\
      }									\
  } G_STMT_END

static void
m4_cursor (GdkDisplay *display)
{
  GdkCursor *arrow, *named;
  guint w = 0, h = 0;

  arrow = gdk_cursor_new_for_display (display, GDK_LEFT_PTR);
  M4_CHECK (arrow != NULL, "cursor: new_for_display returned NULL");
  if (arrow)
    {
      M4_CHECK (arrow->type == GDK_LEFT_PTR, "cursor: type not recorded");
      M4_CHECK (gdk_cursor_get_display (arrow) == display,
		"cursor: get_display mismatch");
      gdk_cursor_get_image (arrow);	/* NULL until M7: must not crash */
    }

  named = gdk_cursor_new_from_name (display, "left_ptr");
  M4_CHECK (named != NULL, "cursor: new_from_name returned NULL");

  gdk_display_supports_cursor_alpha (display);
  gdk_display_supports_cursor_color (display);
  M4_CHECK (gdk_display_get_default_cursor_size (display) > 0,
	    "cursor: default cursor size is 0");
  gdk_display_get_maximal_cursor_size (display, &w, &h);
  M4_CHECK (w > 0 && h > 0, "cursor: maximal cursor size is 0");

  if (named)
    gdk_cursor_unref (named);
  if (arrow)
    gdk_cursor_unref (arrow);
}

static void
m4_font (GdkDisplay *display)
{
  PangoFontDescription *desc;
  GdkFont *font, *same;
  gint width, lbearing, rbearing, w2, ascent, descent;

  desc = pango_font_description_from_string ("Sans 12");
  font = gdk_font_from_description_for_display (display, desc);
  pango_font_description_free (desc);
  M4_CHECK (font != NULL, "font: from_description returned NULL");
  if (!font)
    return;

  M4_CHECK (gdk_font_get_display (font) == display,
	    "font: get_display mismatch");

  width = gdk_text_width (font, "hello", 5);
  M4_CHECK (width > 0, "font: text_width of \"hello\" is not positive");
  M4_CHECK (gdk_text_width (font, "hello world", 11) > width,
	    "font: text_width does not grow with the text");

  gdk_text_extents (font, "hello", 5, &lbearing, &rbearing, &w2,
		    &ascent, &descent);
  M4_CHECK (w2 > 0 && ascent > 0, "font: text_extents has no extent");

  same = gdk_font_ref (font);
  M4_CHECK (gdk_font_equal (font, same), "font: equal to its own ref");
  gdk_font_unref (same);

  /* The legacy loader takes a font name; Pango may or may not know it, so
   * only the absence of a crash is asserted. */
  same = gdk_font_load_for_display (display, "Monospace 10");
  if (same)
    gdk_font_unref (same);

  gdk_font_unref (font);
}

static void
m4_selection (GdkDisplay *display)
{
  GdkAtom clipboard = GDK_SELECTION_CLIPBOARD;
  guchar *data = NULL;
  GdkAtom type = GDK_NONE;
  gint format = 0, i;
  GdkAtom enc;
  guchar *ctext = NULL;
  gint len = 0;

  M4_CHECK (gdk_selection_owner_get_for_display (display, clipboard) == NULL,
	    "selection: unexpected owner on a fresh display");
  /* M8: a local owner is recorded for real, and a conversion with an owner
   * becomes a SELECTION_REQUEST that nobody answers here.  Release the
   * ownership first: the convert below is then the "no owner, no offer" case
   * and is refused. */
  M4_CHECK (gdk_selection_owner_set_for_display (display, main_window,
						 clipboard, GDK_CURRENT_TIME,
						 FALSE),
	    "selection: owner_set failed");
  M4_CHECK (gdk_selection_owner_get_for_display (display, clipboard)
	    == main_window, "selection: owner_get after owner_set");
  gdk_selection_owner_set_for_display (display, NULL, clipboard,
				       GDK_CURRENT_TIME, FALSE);
  M4_CHECK (gdk_selection_owner_get_for_display (display, clipboard) == NULL,
	    "selection: owner_get after release");

  /* A conversion must be answered (here: refused) rather than hang. */
  selection_notify_seen = FALSE;
  gdk_selection_convert (main_window, clipboard,
			 gdk_atom_intern_static_string ("UTF8_STRING"),
			 GDK_CURRENT_TIME);
  for (i = 0; i < 20 && !selection_notify_seen; i++)
    g_main_context_iteration (NULL, FALSE);
  M4_CHECK (selection_notify_seen,
	    "selection: convert produced no SelectionNotify");
  M4_CHECK (!selection_notify_seen || selection_notify_refused,
	    "selection: convert without owner should refuse (property NONE)");

  M4_CHECK (gdk_selection_property_get (main_window, &data, &type,
					&format) == 0 && data == NULL,
	    "selection: property_get should return no data");

  /* text helpers need no windowing system */
  M4_CHECK (gdk_utf8_to_compound_text_for_display (display, "h\xc3\xa9llo",
						   &enc, &format, &ctext, &len)
	    && ctext && len == 6, "selection: utf8_to_compound_text");
  gdk_free_compound_text (ctext);

  ctext = (guchar *) "unset";
  M4_CHECK (!gdk_utf8_to_compound_text_for_display (display, "bad\xe2\x82",
						    &enc, &format, &ctext, &len)
	    && ctext == NULL && len == 0,
	    "selection: invalid UTF-8 should fail cleanly");

  {
    gchar **list = NULL;
    gint n = gdk_text_property_to_utf8_list_for_display
      (display, gdk_atom_intern_static_string ("UTF8_STRING"), 8,
       (const guchar *) "one\0two", 7, &list);
    M4_CHECK (n == 2 && list && strcmp (list[1], "two") == 0,
	      "selection: text_property_to_utf8_list");
    if (list)
      gdk_free_text_list (list);
  }
}

/* M8: gdk_drag_begin always returns a context.  Without a data device (the
 * headless Weston has no seat) or a pressed button the context is FAILED and a
 * GDK_DROP_FINISHED for it is queued; ytk ends its drag on that event. */
static gint m4_drop_finished;

static void
m4_drag_recorder (GdkEvent *event, gpointer data)
{
  if (event->type == GDK_DROP_FINISHED && event->dnd.window == main_window)
    m4_drop_finished++;
  else
    event_handler (event, data);
}

static void
m4_drag (void)
{
  GList *targets = NULL;
  GdkDragContext *ctx;
  gint i;

  targets = g_list_append (targets,
			   GDK_ATOM_TO_POINTER (gdk_atom_intern ("text/plain",
								 FALSE)));
  gdk_event_handler_set (m4_drag_recorder, NULL, NULL);
  m4_drop_finished = 0;
  ctx = gdk_drag_begin (main_window, targets);
  M4_CHECK (ctx != NULL, "drag: begin returns a context (M8)");
  if (ctx)
    {
      for (i = 0; i < 50 && m4_drop_finished == 0; i++)
	g_main_context_iteration (NULL, FALSE);
      M4_CHECK (m4_drop_finished == 1,
		"drag: a failed begin queues one GDK_DROP_FINISHED");
      M4_CHECK (!gdk_drag_drop_succeeded (ctx),
		"drag: a failed begin did not succeed");
      g_object_unref (ctx);
    }
  gdk_event_handler_set (event_handler, NULL, NULL);
  g_list_free (targets);

  /* destination-side hooks must not run Xlib on a Wayland window */
  gdk_window_register_dnd (main_window);
}


static void
m4_property (void)
{
  GdkAtom a, b;
  gchar *name;
  GdkAtom actual_type = GDK_NONE;
  gint actual_format = 0, actual_length = 0;
  guchar *data = NULL;
  const guchar bytes[] = { 1, 2, 3 };

  a = gdk_atom_intern ("YDK_M4_TEST_ATOM", FALSE);
  M4_CHECK (a != GDK_NONE, "property: intern returned GDK_NONE");
  b = gdk_atom_intern ("YDK_M4_TEST_ATOM", FALSE);
  M4_CHECK (a == b, "property: intern is not stable");
  M4_CHECK (gdk_atom_intern ("YDK_M4_NEVER_INTERNED", TRUE) == GDK_NONE,
	    "property: only_if_exists interned a new atom");
  M4_CHECK (gdk_atom_intern_static_string ("YDK_M4_TEST_ATOM") == a,
	    "property: static intern disagrees with intern");

  name = gdk_atom_name (a);
  M4_CHECK (name && strcmp (name, "YDK_M4_TEST_ATOM") == 0,
	    "property: atom_name round trip");
  g_free (name);

  name = gdk_atom_name (GDK_SELECTION_PRIMARY);
  M4_CHECK (name && strcmp (name, "PRIMARY") == 0,
	    "property: predefined PRIMARY atom");
  g_free (name);
  M4_CHECK (gdk_atom_intern ("CLIPBOARD", FALSE) == GDK_SELECTION_CLIPBOARD,
	    "property: predefined CLIPBOARD atom");

  /* M8: window properties are a real per window table */
  gdk_property_change (main_window, a, GDK_TARGET_STRING, 8,
		       GDK_PROP_MODE_REPLACE, bytes, 3);
  M4_CHECK (gdk_property_get (main_window, a, GDK_NONE, 0, 16, FALSE,
			      &actual_type, &actual_format, &actual_length,
			      &data)
	    && actual_type == GDK_TARGET_STRING && actual_format == 8
	    && actual_length == 3 && data && memcmp (data, bytes, 3) == 0,
	    "property: change/get round trip");
  g_free (data);
  data = NULL;
  gdk_property_delete (main_window, a);
  M4_CHECK (!gdk_property_get (main_window, a, GDK_NONE, 0, 16, FALSE,
			       &actual_type, &actual_format, &actual_length,
			       &data), "property: get after delete finds nothing");
}


static int
m4_exercise (void)
{
  GdkDisplay *display = gdk_display_get_default ();

  m4_cursor (display);
  m4_font (display);
  m4_selection (display);
  m4_drag ();
  m4_property ();

  if (m4_failures)
    g_printerr ("ydk_demo: M4: FAIL (%d)\n", m4_failures);
  else
    g_print ("ydk_demo: M4: PASS\n");

  return m4_failures;
}

/* ---- M5 exercise ------------------------------------------------------
 *
 * Under GDK_BACKEND=wayland, builds an offscreen pixmap, draws into it with
 * GC primitives (rectangles, thin lines, points, polygon, arc, pixbuf) under
 * a clip region, a clip origin and a depth-1 clip mask, blits it to the
 * window, and reads everything back with gdk_drawable_get_image and
 * gdk_pixbuf_get_from_drawable.  Colours are hand-set GdkColor.pixel values
 * (0x00RRGGBB); the colormap family is covered separately by m5c_exercise.
 */

static int m5_failures;

#define M5_CHECK(cond, what)						\
  G_STMT_START {							\
    if (!(cond))							\
      {									\
	g_printerr ("ydk_demo: M5 check failed: %s\n", what);		\
	m5_failures++;							\
      }									\
  } G_STMT_END

#define M5_WHITE 0xffffff
#define M5_BLACK 0x000000
#define M5_RED   0xff0000
#define M5_GREEN 0x00ff00
#define M5_BLUE  0x0000ff

static void
m5_set_fg (GdkGC *gc, guint32 pixel)
{
  GdkColor c;

  c.pixel = pixel;
  c.red = ((pixel >> 16) & 0xff) * 257;
  c.green = ((pixel >> 8) & 0xff) * 257;
  c.blue = (pixel & 0xff) * 257;
  gdk_gc_set_foreground (gc, &c);
}

/* checks a read back image against an expected 0x00RRGGBB pixel */
static void
m5_expect (GdkImage *img, int x, int y, guint32 want, const char *what)
{
  guint32 got = gdk_image_get_pixel (img, x, y) & 0xffffff;

  if (got != want)
    {
      g_printerr ("ydk_demo: M5 check failed: %s at (%d,%d): got %06x want %06x\n",
		  what, x, y, got, want);
      m5_failures++;
    }
}

static void
m5_fill (GdkDrawable *d, GdkGC *gc, guint32 pixel, int x, int y, int w, int h)
{
  m5_set_fg (gc, pixel);
  gdk_draw_rectangle (d, gc, TRUE, x, y, w, h);
}

static void
m5_pixmap_drawing (void)
{
  GdkPixmap *pm;
  GdkGC *gc;
  GdkImage *img;
  GdkRectangle clip = { 8, 8, 8, 8 };
  GdkPoint tri[3] = { { 20, 20 }, { 30, 20 }, { 20, 30 } };
  GdkPoint lines[3] = { { 14, 14 }, { 20, 14 }, { 20, 18 } };
  GdkGCValues values;
  GdkGC *gc2;
  GdkPixbuf *pb;
  guchar *px;
  int i;

  pm = gdk_pixmap_new (main_window, 32, 32, -1);
  M5_CHECK (pm != NULL, "pixmap: gdk_pixmap_new returned NULL");
  if (!pm)
    return;
  M5_CHECK (gdk_drawable_get_depth (pm) == gdk_drawable_get_depth (main_window),
	    "pixmap: depth should follow the window");
  M5_CHECK (gdk_drawable_get_colormap (pm) != NULL,
	    "pixmap: window-depth pixmap should have a colormap");

  gc = gdk_gc_new (pm);
  M5_CHECK (gc != NULL, "gc: gdk_gc_new on a pixmap");
  if (!gc)
    return;
  M5_CHECK (gdk_gc_get_screen (gc) == gdk_drawable_get_screen (pm),
	    "gc: gdk_gc_get_screen should match the drawable");

  /* ---- plain primitives */
  m5_fill (pm, gc, M5_WHITE, 0, 0, 32, 32);
  m5_fill (pm, gc, M5_RED, 4, 4, 8, 8);
  m5_set_fg (gc, M5_GREEN);
  gdk_draw_rectangle (pm, gc, FALSE, 16, 4, 6, 6);
  m5_set_fg (gc, M5_BLUE);
  gdk_draw_line (pm, gc, 2, 20, 10, 20);
  gdk_draw_line (pm, gc, 2, 22, 2, 28);
  gdk_draw_point (pm, gc, 30, 30);
  gdk_draw_point (pm, gc, 30, 30);
  gdk_draw_lines (pm, gc, lines, 3);
  gdk_gc_set_line_attributes (gc, 0, GDK_LINE_ON_OFF_DASH, GDK_CAP_BUTT, GDK_JOIN_MITER);
  gdk_draw_line (pm, gc, 0, 31, 15, 31);
  gdk_gc_set_line_attributes (gc, 0, GDK_LINE_SOLID, GDK_CAP_BUTT, GDK_JOIN_MITER);

  img = gdk_drawable_get_image (pm, 0, 0, 32, 32);
  M5_CHECK (img != NULL, "pixmap: gdk_drawable_get_image returned NULL");
  if (img)
    {
      m5_expect (img, 0, 0, M5_WHITE, "background");
      m5_expect (img, 4, 4, M5_RED, "filled rect first pixel");
      m5_expect (img, 11, 11, M5_RED, "filled rect last pixel");
      m5_expect (img, 12, 12, M5_WHITE, "filled rect past the end");
      m5_expect (img, 3, 4, M5_WHITE, "filled rect before the start");
      m5_expect (img, 16, 4, M5_GREEN, "outline rect top left");
      m5_expect (img, 22, 10, M5_GREEN, "outline rect bottom right (w+1 x h+1)");
      m5_expect (img, 23, 10, M5_WHITE, "outline rect beyond right");
      m5_expect (img, 19, 7, M5_WHITE, "outline rect interior");
      m5_expect (img, 2, 20, M5_BLUE, "hline first end");
      m5_expect (img, 10, 20, M5_BLUE, "hline last end");
      m5_expect (img, 11, 20, M5_WHITE, "hline past the end");
      m5_expect (img, 1, 20, M5_WHITE, "hline before the start");
      m5_expect (img, 2, 22, M5_BLUE, "vline first end");
      m5_expect (img, 2, 28, M5_BLUE, "vline last end");
      m5_expect (img, 2, 29, M5_WHITE, "vline past the end");
      m5_expect (img, 30, 30, M5_BLUE, "duplicated point");
      m5_expect (img, 14, 14, M5_BLUE, "polyline: first end");
      m5_expect (img, 20, 14, M5_BLUE, "polyline: corner");
      m5_expect (img, 20, 18, M5_BLUE, "polyline: last end");
      m5_expect (img, 13, 14, M5_WHITE, "polyline: before the start");
      m5_expect (img, 0, 31, M5_BLUE, "dashed line: first dash start");
      m5_expect (img, 3, 31, M5_BLUE, "dashed line: first dash end");
      m5_expect (img, 4, 31, M5_WHITE, "dashed line: first gap");
      m5_expect (img, 7, 31, M5_WHITE, "dashed line: gap end");
      m5_expect (img, 8, 31, M5_BLUE, "dashed line: second dash");
      m5_expect (img, 20, 19, M5_WHITE, "polyline: past the end");
      g_object_unref (img);
    }

  /* ---- GC attribute round trip and copy */
  gdk_gc_get_values (gc, &values);
  M5_CHECK ((values.foreground.pixel & 0xffffff) == M5_BLUE, "gc: get_values foreground");
  gdk_gc_set_line_attributes (gc, 3, GDK_LINE_ON_OFF_DASH, GDK_CAP_ROUND, GDK_JOIN_BEVEL);
  gdk_gc_get_values (gc, &values);
  M5_CHECK (values.line_width == 3 && values.line_style == GDK_LINE_ON_OFF_DASH &&
	    values.cap_style == GDK_CAP_ROUND && values.join_style == GDK_JOIN_BEVEL,
	    "gc: get_values line attributes");
  gc2 = gdk_gc_new (pm);
  gdk_gc_copy (gc2, gc);
  gdk_gc_get_values (gc2, &values);
  M5_CHECK (values.line_width == 3 && values.line_style == GDK_LINE_ON_OFF_DASH,
	    "gc: gdk_gc_copy carries line attributes");
  g_object_unref (gc2);
  gdk_gc_set_line_attributes (gc, 0, GDK_LINE_SOLID, GDK_CAP_BUTT, GDK_JOIN_MITER);

  /* ---- clip region, then clip origin, then reset */
  m5_fill (pm, gc, M5_WHITE, 0, 0, 32, 32);
  gdk_gc_set_clip_rectangle (gc, &clip);
  m5_fill (pm, gc, M5_BLACK, 0, 0, 32, 32);
  gdk_gc_set_clip_origin (gc, 4, 4);
  m5_fill (pm, gc, M5_RED, 0, 0, 32, 32);

  img = gdk_drawable_get_image (pm, 0, 0, 32, 32);
  if (img)
    {
      m5_expect (img, 7, 7, M5_WHITE, "clip: outside the region stays untouched");
      m5_expect (img, 8, 8, M5_BLACK, "clip: first black pixel");
      m5_expect (img, 10, 10, M5_BLACK, "clip: inner pixel stays black");
      m5_expect (img, 15, 15, M5_RED, "clip origin: overlap is repainted");
      m5_expect (img, 12, 12, M5_RED, "clip origin: shifted region is red");
      m5_expect (img, 19, 19, M5_RED, "clip origin: shifted region end");
      m5_expect (img, 20, 20, M5_WHITE, "clip origin: beyond the shifted region");
      m5_expect (img, 31, 31, M5_WHITE, "clip: far corner untouched");
      g_object_unref (img);
    }

  gdk_gc_set_clip_region (gc, NULL);
  M5_CHECK (gdk_gc_get_screen (gc) != NULL, "gc: screen after clip reset");
  m5_fill (pm, gc, M5_WHITE, 0, 0, 32, 32);

  /* ---- polygon, arc, pixbuf */
  m5_set_fg (gc, M5_RED);
  gdk_draw_polygon (pm, gc, TRUE, tri, 3);
  m5_set_fg (gc, M5_GREEN);
  gdk_draw_arc (pm, gc, TRUE, 0, 0, 16, 16, 0, 360 * 64);

  pb = gdk_pixbuf_new (GDK_COLORSPACE_RGB, TRUE, 8, 4, 4);
  px = gdk_pixbuf_get_pixels (pb);
  for (i = 0; i < 16; i++)
    {
      px[i * 4 + 0] = 0x00;
      px[i * 4 + 1] = 0x00;
      px[i * 4 + 2] = 0xff;
      px[i * 4 + 3] = 0xff;
    }
  gdk_draw_pixbuf (pm, NULL, pb, 0, 0, 24, 2, 4, 4, GDK_RGB_DITHER_NONE, 0, 0);
  g_object_unref (pb);

  img = gdk_drawable_get_image (pm, 0, 0, 32, 32);
  if (img)
    {
      m5_expect (img, 22, 22, M5_RED, "polygon: interior");
      m5_expect (img, 29, 29, M5_WHITE, "polygon: outside the hypotenuse");
      m5_expect (img, 8, 8, M5_GREEN, "arc: centre");
      m5_expect (img, 0, 0, M5_WHITE, "arc: bounding box corner");
      m5_expect (img, 25, 3, M5_BLUE, "pixbuf: opaque pixel");
      m5_expect (img, 28, 3, M5_WHITE, "pixbuf: beyond its width");
      g_object_unref (img);
    }

  /* ---- blit to the window and read back from it */
  m5_fill (pm, gc, M5_WHITE, 0, 0, 32, 32);
  m5_fill (pm, gc, M5_RED, 4, 4, 8, 8);
  gdk_draw_drawable (main_window, gc, pm, 0, 0, 100, 100, 32, 32);

  img = gdk_drawable_get_image (main_window, 100, 100, 32, 32);
  M5_CHECK (img != NULL, "window: gdk_drawable_get_image returned NULL");
  if (img)
    {
      m5_expect (img, 0, 0, M5_WHITE, "window blit: background");
      m5_expect (img, 4, 4, M5_RED, "window blit: red first pixel");
      m5_expect (img, 11, 11, M5_RED, "window blit: red last pixel");
      m5_expect (img, 12, 12, M5_WHITE, "window blit: past the red");
      g_object_unref (img);
    }

  /* a blit with part of the source outside it is clamped, not cleared */
  gdk_draw_drawable (main_window, gc, pm, -4, -4, 200, 200, 16, 16);
  img = gdk_drawable_get_image (main_window, 200, 200, 16, 16);
  if (img)
    {
      m5_expect (img, 8, 8, M5_RED, "window blit: negative source origin");
      g_object_unref (img);
    }

  pb = gdk_pixbuf_get_from_drawable (NULL, pm, gdk_drawable_get_colormap (pm),
				     0, 0, 0, 0, 32, 32);
  M5_CHECK (pb != NULL, "pixbuf_get_from_drawable returned NULL");
  if (pb)
    {
      guchar *p = gdk_pixbuf_get_pixels (pb) + 5 * gdk_pixbuf_get_rowstride (pb) + 5 * 3;

      M5_CHECK (p[0] == 0xff && p[1] == 0 && p[2] == 0,
		"pixbuf_get_from_drawable: red pixel");
      g_object_unref (pb);
    }

  g_object_unref (gc);
  g_object_unref (pm);
}

static void
m5_bitmaps (void)
{
  /* 8x4, bit 0 = leftmost pixel: rows 0xa5 = x 0,2,5,7; 0xff; 0x00; 0x01 */
  static const gchar bits[] = { (gchar) 0xa5, (gchar) 0xff, 0x00, 0x01 };
  GdkBitmap *bm;
  GdkPixmap *pm;
  GdkGC *gc, *gcm;
  GdkImage *img;
  int x, y;
  gboolean ok = TRUE;

  bm = gdk_bitmap_create_from_data (NULL, bits, 8, 4);
  M5_CHECK (bm != NULL, "bitmap: create_from_data returned NULL");
  if (!bm)
    return;
  M5_CHECK (gdk_drawable_get_depth (bm) == 1, "bitmap: depth should be 1");

  img = gdk_drawable_get_image (bm, 0, 0, 8, 4);
  M5_CHECK (img != NULL, "bitmap: get_image returned NULL");
  if (img)
    {
      for (y = 0; y < 4; y++)
	for (x = 0; x < 8; x++)
	  {
	    guint32 want = (((guchar) bits[y]) >> x) & 1;

	    if ((gdk_image_get_pixel (img, x, y) & 1) != want)
	      ok = FALSE;
	  }
      M5_CHECK (ok, "bitmap: bits do not match the XBM source");
      g_object_unref (img);
    }

  pm = gdk_pixmap_new (main_window, 8, 4, -1);
  gc = gdk_gc_new (pm);
  m5_fill (pm, gc, M5_WHITE, 0, 0, 8, 4);

  /* bitmap as clip mask */
  gcm = gdk_gc_new (pm);
  gdk_gc_set_clip_mask (gcm, bm);
  m5_fill (pm, gcm, M5_RED, 0, 0, 8, 4);
  img = gdk_drawable_get_image (pm, 0, 0, 8, 4);
  if (img)
    {
      ok = TRUE;
      for (y = 0; y < 4; y++)
	for (x = 0; x < 8; x++)
	  {
	    guint32 want = ((((guchar) bits[y]) >> x) & 1) ? M5_RED : M5_WHITE;

	    if ((gdk_image_get_pixel (img, x, y) & 0xffffff) != want)
	      ok = FALSE;
	  }
      M5_CHECK (ok, "bitmap clip mask: only the set bits are painted");
      g_object_unref (img);
    }

  /* a depth-1 pixmap can be drawn into too */
  {
    GdkBitmap *b2 = gdk_pixmap_new (NULL, 8, 8, 1);
    GdkGC *g1;
    GdkColor one, zero;

    M5_CHECK (b2 != NULL && gdk_drawable_get_depth (b2) == 1,
	      "bitmap: gdk_pixmap_new (NULL, 8, 8, 1)");
    if (b2)
      {
	one.pixel = 1;
	zero.pixel = 0;
	g1 = gdk_gc_new (b2);
	gdk_gc_set_foreground (g1, &zero);
	gdk_draw_rectangle (b2, g1, TRUE, 0, 0, 8, 8);
	gdk_gc_set_foreground (g1, &one);
	gdk_draw_rectangle (b2, g1, TRUE, 2, 2, 3, 3);
	img = gdk_drawable_get_image (b2, 0, 0, 8, 8);
	if (img)
	  {
	    M5_CHECK ((gdk_image_get_pixel (img, 2, 2) & 1) == 1, "depth-1 draw: set pixel");
	    M5_CHECK ((gdk_image_get_pixel (img, 4, 4) & 1) == 1, "depth-1 draw: last set pixel");
	    M5_CHECK ((gdk_image_get_pixel (img, 5, 5) & 1) == 0, "depth-1 draw: clear pixel");
	    M5_CHECK ((gdk_image_get_pixel (img, 1, 1) & 1) == 0, "depth-1 draw: clear before");
	    g_object_unref (img);
	  }
	g_object_unref (g1);
	g_object_unref (b2);
      }
  }

  M5_CHECK (gdk_pixmap_foreign_new (1) == NULL, "pixmap_foreign_new should be NULL");
  M5_CHECK (gdk_pixmap_lookup (1) == NULL, "pixmap_lookup should be NULL");

  g_object_unref (gcm);
  g_object_unref (gc);
  g_object_unref (pm);
  g_object_unref (bm);
}

static int
m5_exercise (void)
{
  m5_pixmap_drawing ();
  m5_bitmaps ();

  if (m5_failures)
    g_printerr ("ydk_demo: M5: FAIL (%d)\n", m5_failures);
  else
    g_print ("ydk_demo: M5: PASS\n");

  return m5_failures;
}

/* ---- M5C exercise -----------------------------------------------------
 *
 * The colormap family (gap F7) under GDK_BACKEND=wayland: allocate a colour
 * through gdk_colormap_alloc_color, use gdk_gc_set_rgb_fg_color to draw a
 * filled rectangle into a pixmap and read back the exact pixel, then drop
 * and recreate colormap references (the Wayland colormap is a private
 * subclass whose finalizer must not run the X11 one).
 */

static int m5c_failures;

#define M5C_CHECK(cond, what)						\
  G_STMT_START {							\
    if (!(cond))							\
      {									\
	g_printerr ("ydk_demo: M5C check failed: %s\n", what);		\
	m5c_failures++;							\
      }									\
  } G_STMT_END

static void
m5c_expect_pixel (GdkImage *img, int x, int y, guint32 want, const char *what)
{
  guint32 got = gdk_image_get_pixel (img, x, y) & 0xffffff;

  if (got != want)
    {
      g_printerr ("ydk_demo: M5C check failed: %s at (%d,%d): got %06x want %06x\n",
		  what, x, y, got, want);
      m5c_failures++;
    }
}

/* fills a rectangle through gdk_gc_set_rgb_fg_color and reads it back */
static void
m5c_rgb_fill (GdkDrawable *pm, GdkGC *gc, const GdkColor *c, guint32 want,
	      const char *what)
{
  GdkImage *img;

  gdk_gc_set_rgb_fg_color (gc, c);
  gdk_draw_rectangle (pm, gc, TRUE, 2, 2, 6, 6);
  img = gdk_drawable_get_image (pm, 0, 0, 16, 16);
  M5C_CHECK (img != NULL, "get_image after rgb fill");
  if (img)
    {
      m5c_expect_pixel (img, 2, 2, want, what);
      m5c_expect_pixel (img, 7, 7, want, what);
      g_object_unref (img);
    }
}

static int
m5c_exercise (void)
{
  GdkScreen *screen = gdk_screen_get_default ();
  GdkColormap *sys, *cm, *cm2;
  GdkVisual *visual;
  GdkPixmap *pm;
  GdkGC *gc;
  GdkColor c, q, c2;
  GdkColor arr[2];
  gboolean ok[2];
  guint32 depth_hi;
  int i;

  sys = gdk_drawable_get_colormap (main_window);
  M5C_CHECK (sys != NULL, "window has a colormap");
  if (!sys)
    goto done;

  visual = gdk_colormap_get_visual (sys);
  M5C_CHECK (visual != NULL, "colormap has a visual");
  M5C_CHECK (gdk_colormap_get_screen (sys) == screen,
	     "gdk_colormap_get_screen on the system colormap");
  M5C_CHECK (gdk_colormap_get_system () == sys ||
	     gdk_colormap_get_system () != NULL,
	     "gdk_colormap_get_system");
  M5C_CHECK (gdk_colormap_get_system_size () == 256,
	     "gdk_colormap_get_system_size is 256");
  gdk_colormap_change (sys, 0);

  /* gdk_colormap_alloc_color: 0xffff, 0x8080, 0x0000 -> 0xff, 0x80, 0x00 */
  depth_hi = (visual->depth >= 32) ? 0xff000000u : 0;
  c.red = 0xffff; c.green = 0x8080; c.blue = 0x0000; c.pixel = 0xdeadbeef;
  M5C_CHECK (gdk_colormap_alloc_color (sys, &c, FALSE, TRUE),
	     "gdk_colormap_alloc_color succeeds");
  M5C_CHECK (c.pixel == (depth_hi | 0x00ff8000),
	     "alloc_color pixel is 0x00ff8000");

  /* query_color round trip is exact for 8-bit channels */
  memset (&q, 0, sizeof q);
  gdk_colormap_query_color (sys, c.pixel, &q);
  M5C_CHECK (q.red == 0xffff && q.green == 0x8080 && q.blue == 0x0000,
	     "query_color round trip");
  M5C_CHECK (q.pixel == c.pixel, "query_color keeps the pixel");

  /* batch allocation, none left over, then the no-op frees */
  arr[0].red = 0; arr[0].green = 0; arr[0].blue = 0xffff;
  arr[1].red = 0x1212; arr[1].green = 0x3434; arr[1].blue = 0x5656;
  ok[0] = ok[1] = FALSE;
  M5C_CHECK (gdk_colormap_alloc_colors (sys, arr, 2, FALSE, TRUE, ok) == 0 &&
	     ok[0] && ok[1], "alloc_colors allocates all");
  M5C_CHECK ((arr[0].pixel & 0xffffff) == 0x0000ff, "alloc_colors blue pixel");
  M5C_CHECK ((arr[1].pixel & 0xffffff) == 0x123456, "alloc_colors mixed pixel");
  gdk_colormap_free_colors (sys, arr, 2);

  /* writable cells do not exist in TrueColor */
  {
    gulong pixels[1];

    M5C_CHECK (!gdk_colors_alloc (sys, FALSE, NULL, 0, pixels, 1),
	       "gdk_colors_alloc fails on TrueColor");
    gdk_colors_free (sys, pixels, 0, 0);
  }
  c2 = c;
  M5C_CHECK (!gdk_color_change (sys, &c2), "gdk_color_change reports failure");

  /* gdk_gc_set_rgb_fg_color -> filled rectangle -> exact pixel */
  pm = gdk_pixmap_new (main_window, 16, 16, -1);
  M5C_CHECK (pm != NULL, "pixmap for the rgb fill");
  if (pm)
    {
      GdkColor black = { 0, 0, 0, 0 };

      gc = gdk_gc_new (pm);
      M5C_CHECK (gc != NULL, "gc for the rgb fill");
      if (gc)
	{
	  gdk_gc_set_rgb_fg_color (gc, &black);
	  gdk_draw_rectangle (pm, gc, TRUE, 0, 0, 16, 16);
	  m5c_rgb_fill (pm, gc, &c, 0x00ff8000, "rgb fg 0xff8000");
	  m5c_rgb_fill (pm, gc, &arr[1], 0x00123456, "rgb fg 0x123456");

	  /* drop and recreate colormap references: a private colormap, the
	   * last unref runs the subclass finalizer (not the X11 one) */
	  for (i = 0; i < 3; i++)
	    {
	      cm = gdk_colormap_new (visual, FALSE);
	      M5C_CHECK (cm != NULL, "gdk_colormap_new");
	      if (!cm)
		continue;
	      M5C_CHECK (gdk_colormap_get_screen (cm) == screen,
			 "gdk_colormap_get_screen on a new colormap");
	      M5C_CHECK (gdk_colormap_get_visual (cm) == visual,
			 "new colormap keeps the visual");

	      g_object_ref (cm);
	      gdk_gc_set_colormap (gc, cm);
	      m5c_rgb_fill (pm, gc, &c, 0x00ff8000, "rgb fg with a new colormap");

	      /* the GC drops its ref when the next colormap replaces it */
	      cm2 = gdk_colormap_new (visual, TRUE);
	      M5C_CHECK (cm2 != NULL, "gdk_colormap_new (allocate)");
	      gdk_gc_set_colormap (gc, cm2);
	      g_object_unref (cm);	/* the extra ref */
	      g_object_unref (cm);	/* the creator's: finalizes cm */
	      m5c_rgb_fill (pm, gc, &c, 0x00ff8000, "rgb fg after dropping cm");
	      g_object_unref (cm2);	/* the GC still holds cm2 */
	    }

	  /* back to the system colormap, still usable */
	  gdk_gc_set_colormap (gc, sys);
	  m5c_rgb_fill (pm, gc, &c, 0x00ff8000, "rgb fg back on the system colormap");
	  g_object_unref (gc);
	}
      g_object_unref (pm);
    }

  /* the system colormap survives extra ref/unref churn */
  for (i = 0; i < 4; i++)
    {
      g_object_ref (sys);
      g_object_unref (sys);
    }
  M5C_CHECK (gdk_screen_get_system_colormap (screen) == sys,
	     "system colormap unchanged after ref churn");

done:
  if (m5c_failures)
    g_printerr ("ydk_demo: M5C: FAIL (%d)\n", m5c_failures);
  else
    g_print ("ydk_demo: M5C: PASS\n");

  return m5c_failures;
}

/* ---- M5V exercise -----------------------------------------------------
 *
 * The visual / default-colormap / rgba / gdkrgb family (gap F8) under
 * GDK_BACKEND=wayland.  These are the calls that used to segfault because
 * they read a GdkScreenX11 out of the Wayland screen: gdk_screen_list_visuals
 * (and through it every gdk_rgb_* entry point that picks a visual),
 * gdk_screen_get_rgba_colormap and gdk_screen_get_default_colormap.  The
 * checks assert real values, not just non-NULL.  The colormap bits the audit
 * listed as untested (gdk_colors_free / gdk_colormap_free_colors with
 * npixels > 0, query_color rounding, a depth-32 pixmap) are covered at the
 * end.  CRITICALs are fatal in this process, so a g_return_if_fail on any of
 * these paths aborts the run instead of scrolling past.
 */

static int m5v_failures;

#define M5V_CHECK(cond, what)						\
  G_STMT_START {							\
    if (!(cond))							\
      {									\
	g_printerr ("ydk_demo: M5V check failed: %s\n", what);		\
	m5v_failures++;							\
      }									\
  } G_STMT_END

#define M5V_EQ_HEX(got, want, what)					\
  G_STMT_START {							\
    gulong g_ = (gulong) (got), w_ = (gulong) (want);			\
    if (g_ != w_)							\
      {									\
	g_printerr ("ydk_demo: M5V check failed: %s: got %08lx want %08lx\n", \
		    what, g_, w_);					\
	m5v_failures++;							\
      }									\
  } G_STMT_END

/* reads pixel (x, y) of a drawable with all 32 bits (alpha included) */
static guint32
m5v_pixel (GdkDrawable *d, int x, int y)
{
  GdkImage *img = gdk_drawable_get_image (d, 0, 0, 16, 16);
  guint32 p = 0xdeadbeef;

  M5V_CHECK (img != NULL, "get_image for the pixel read");
  if (img)
    {
      p = gdk_image_get_pixel (img, x, y);
      g_object_unref (img);
    }
  return p;
}

static void
m5v_visuals (GdkScreen *screen, GdkVisual *sysv, GdkVisual *rgba)
{
  GList *list, *l;
  gint *depths = NULL, count = 0;
  GdkVisualType *types = NULL;
  int n_sys = 0, n_rgba = 0, n_true = 0;

  /* system visual: the 24-bit TrueColor one gdkrgb must pick */
  M5V_CHECK (sysv != NULL, "system visual exists");
  M5V_CHECK (sysv->depth == 24, "system visual depth is 24");
  M5V_CHECK (sysv->type == GDK_VISUAL_TRUE_COLOR, "system visual is TrueColor");
  M5V_CHECK (gdk_visual_get_system () == sysv, "gdk_visual_get_system");
  M5V_CHECK (gdk_visual_get_screen (sysv) == screen,
	     "gdk_visual_get_screen (system visual) == screen");

  /* rgba visual: depth 32, same channel layout, stable, owned by the screen */
  M5V_CHECK (rgba != NULL, "gdk_screen_get_rgba_visual");
  if (!rgba)
    return;
  M5V_CHECK (rgba != sysv, "rgba visual is not the system visual");
  M5V_CHECK (rgba->depth == 32, "rgba visual depth is 32");
  M5V_CHECK (rgba->type == GDK_VISUAL_TRUE_COLOR, "rgba visual is TrueColor");
  M5V_EQ_HEX (rgba->red_mask, sysv->red_mask, "rgba red mask");
  M5V_EQ_HEX (rgba->green_mask, sysv->green_mask, "rgba green mask");
  M5V_EQ_HEX (rgba->blue_mask, sysv->blue_mask, "rgba blue mask");
  M5V_EQ_HEX (rgba->red_mask, 0xff0000, "rgba red mask value");
  M5V_CHECK (gdk_screen_get_rgba_visual (screen) == rgba,
	     "rgba visual is stable across calls");
  M5V_CHECK (gdk_visual_get_screen (rgba) == screen,
	     "gdk_visual_get_screen (rgba visual) == screen");

  /* gdk_screen_list_visuals: argb first (X11 largest-first order), system second */
  list = gdk_screen_list_visuals (screen);
  M5V_CHECK (g_list_length (list) == 2, "gdk_screen_list_visuals has 2 entries");
  for (l = list; l; l = l->next)
    {
      GdkVisual *v = l->data;

      n_sys += (v == sysv);
      n_rgba += (v == rgba);
      n_true += (v->type == GDK_VISUAL_TRUE_COLOR);
    }
  M5V_CHECK (n_sys == 1, "list_visuals includes the system visual once");
  M5V_CHECK (n_rgba == 1, "list_visuals includes the rgba visual once");
  M5V_CHECK (n_true == 2, "list_visuals entries are all TrueColor");
  M5V_CHECK (list && list->data == rgba, "list_visuals: rgba first");
  M5V_CHECK (list && list->next && list->next->data == sysv,
	     "list_visuals: system second");
  g_list_free (list);

  /* generic gdk_list_visuals () goes through the same screen function */
  list = gdk_list_visuals ();
  M5V_CHECK (g_list_length (list) == 2, "gdk_list_visuals has 2 entries");
  g_list_free (list);

  /* best-visual queries */
  M5V_CHECK (gdk_visual_get_best_depth () == 32, "best depth is 32");
  M5V_CHECK (gdk_visual_get_best_type () == GDK_VISUAL_TRUE_COLOR,
	     "best type is TrueColor");
  M5V_CHECK (gdk_visual_get_best () == rgba, "gdk_visual_get_best is the rgba visual");
  M5V_CHECK (gdk_visual_get_best ()->depth == 32, "best visual has depth 32");
  M5V_CHECK (gdk_visual_get_best_with_depth (24) == sysv, "best with depth 24");
  M5V_CHECK (gdk_visual_get_best_with_depth (32) == rgba, "best with depth 32");
  M5V_CHECK (gdk_visual_get_best_with_depth (8) == NULL, "best with depth 8 is NULL");
  M5V_CHECK (gdk_visual_get_best_with_type (GDK_VISUAL_TRUE_COLOR) == rgba,
	     "best with type TrueColor");
  M5V_CHECK (gdk_visual_get_best_with_type (GDK_VISUAL_PSEUDO_COLOR) == NULL,
	     "best with type PseudoColor is NULL");
  M5V_CHECK (gdk_visual_get_best_with_both (24, GDK_VISUAL_TRUE_COLOR) == sysv,
	     "best with 24 + TrueColor");
  M5V_CHECK (gdk_visual_get_best_with_both (32, GDK_VISUAL_TRUE_COLOR) == rgba,
	     "best with 32 + TrueColor");
  M5V_CHECK (gdk_visual_get_best_with_both (24, GDK_VISUAL_STATIC_GRAY) == NULL,
	     "best with 24 + StaticGray is NULL");

  gdk_query_depths (&depths, &count);
  M5V_CHECK (count == 2 && depths != NULL, "gdk_query_depths count is 2");
  if (count == 2 && depths)
    {
      M5V_CHECK (depths[0] == 32, "query_depths[0] is 32");
      M5V_CHECK (depths[1] == 24, "query_depths[1] is 24");
    }
  gdk_query_visual_types (&types, &count);
  M5V_CHECK (count == 1 && types != NULL && types[0] == GDK_VISUAL_TRUE_COLOR,
	     "gdk_query_visual_types is { TrueColor }");
}

static void
m5v_rgb (GdkScreen *screen, GdkVisual *sysv, GdkColormap *sys)
{
  GdkColormap *rgb_cmap = gdk_rgb_get_colormap ();
  GdkPixmap *pm;
  GdkGC *gc;

  /* gdkrgb picks the system visual: the rgb scorer rates depth 32 as 0 */
  M5V_CHECK (gdk_rgb_get_visual () == sysv, "gdk_rgb_get_visual is the system visual");
  M5V_CHECK (gdk_screen_get_rgb_visual (screen) == sysv,
	     "gdk_screen_get_rgb_visual is the system visual");
  M5V_CHECK (rgb_cmap != NULL, "gdk_rgb_get_colormap");
  M5V_CHECK (gdk_screen_get_rgb_colormap (screen) == rgb_cmap,
	     "gdk_screen_get_rgb_colormap == gdk_rgb_get_colormap");
  if (rgb_cmap)
    {
      M5V_CHECK (gdk_colormap_get_visual (rgb_cmap) == sysv,
		 "rgb colormap is over the system visual");
      M5V_CHECK (gdk_colormap_get_screen (rgb_cmap) == screen,
		 "rgb colormap belongs to the screen");
    }
  M5V_CHECK (gdk_rgb_ditherable () == FALSE ||
	     gdk_rgb_ditherable () == TRUE, "gdk_rgb_ditherable returns");

  /* exact pixel values for a depth-24 TrueColor visual */
  M5V_EQ_HEX (gdk_rgb_xpixel_from_rgb (0xff8000), 0x00ff8000, "xpixel_from_rgb ff8000");
  M5V_EQ_HEX (gdk_rgb_xpixel_from_rgb (0x123456), 0x00123456, "xpixel_from_rgb 123456");
  M5V_EQ_HEX (gdk_rgb_xpixel_from_rgb (0x000000), 0x00000000, "xpixel_from_rgb black");
  M5V_EQ_HEX (gdk_rgb_xpixel_from_rgb (0xffffff), 0x00ffffff, "xpixel_from_rgb white");

  /* gdk_rgb_gc_set_foreground -> fill -> exact readback */
  pm = gdk_pixmap_new (main_window, 16, 16, -1);
  M5V_CHECK (pm != NULL, "pixmap for the rgb gc fill");
  if (!pm)
    return;
  gc = gdk_gc_new (pm);
  gdk_rgb_gc_set_foreground (gc, 0x123456);
  gdk_draw_rectangle (pm, gc, TRUE, 0, 0, 16, 16);
  M5V_EQ_HEX (m5v_pixel (pm, 3, 3) & 0xffffff, 0x123456, "gdk_rgb_gc_set_foreground fill");
  g_object_unref (gc);
  g_object_unref (pm);

  (void) sys;
}

static void
m5v_default_colormap (GdkScreen *screen, GdkColormap *sys, GdkColormap *rgba_cm)
{
  GdkColormap *cm;
  guint base_rc;

  /* until something sets it, the default colormap is the system one */
  M5V_CHECK (gdk_screen_get_default_colormap (screen) == sys,
	     "default colormap starts as the system colormap");

  base_rc = G_OBJECT (rgba_cm)->ref_count;
  gdk_screen_set_default_colormap (screen, rgba_cm);
  M5V_CHECK (gdk_screen_get_default_colormap (screen) == rgba_cm,
	     "set_default_colormap (rgba) round trip");
  M5V_CHECK (G_OBJECT (rgba_cm)->ref_count == base_rc + 1,
	     "set_default_colormap takes exactly one reference");

  /* setting the one that is already the default must not drop it */
  gdk_screen_set_default_colormap (screen, rgba_cm);
  M5V_CHECK (gdk_screen_get_default_colormap (screen) == rgba_cm,
	     "set the same default colormap twice");
  M5V_CHECK (G_OBJECT (rgba_cm)->ref_count == base_rc + 1,
	     "setting the same default twice keeps one reference");

  /* back to the system colormap, the rgba one gets its reference back */
  gdk_screen_set_default_colormap (screen, sys);
  M5V_CHECK (gdk_screen_get_default_colormap (screen) == sys,
	     "restore the system colormap as default");
  M5V_CHECK (G_OBJECT (rgba_cm)->ref_count == base_rc,
	     "replacing the default releases the old reference");
  gdk_screen_set_default_colormap (screen, sys);
  M5V_CHECK (gdk_screen_get_default_colormap (screen) == sys,
	     "set the system colormap twice");

  /* a colormap that only the screen holds is finalized when replaced */
  cm = gdk_colormap_new (gdk_screen_get_rgba_visual (screen), FALSE);
  M5V_CHECK (cm != NULL, "gdk_colormap_new (rgba visual)");
  if (cm)
    {
      gdk_screen_set_default_colormap (screen, cm);
      g_object_unref (cm);
      M5V_CHECK (gdk_screen_get_default_colormap (screen) == cm,
		 "the screen keeps the default colormap alive");
      gdk_screen_set_default_colormap (screen, sys);
    }
  M5V_CHECK (gdk_screen_get_default_colormap (screen) == sys,
	     "default colormap is the system one again");
}

static void
m5v_colormap_bits (GdkScreen *screen, GdkColormap *sys, GdkColormap *rgba_cm)
{
  GdkColor c, q, arr[3];
  gboolean ok[3];
  gulong pixels[3] = { 0x00ff8000, 0x0000ff00, 0x12345678 };

  /* gdk_colors_free with npixels > 0: nothing was ever allocated, so it is
   * a no-op, and later allocations are not disturbed */
  gdk_colors_free (sys, pixels, 3, 0);
  gdk_colors_free (rgba_cm, pixels, 3, 0);
  c.red = 0xffff; c.green = 0x8080; c.blue = 0;
  M5V_CHECK (gdk_colormap_alloc_color (sys, &c, FALSE, TRUE),
	     "alloc_color after colors_free");
  M5V_EQ_HEX (c.pixel, 0x00ff8000, "alloc_color pixel after colors_free");

  /* gdk_colormap_free_colors with n_colors > 0 on both colormaps */
  arr[0].red = 0x1234; arr[0].green = 0x5678; arr[0].blue = 0x9abc;
  arr[1].red = 0;      arr[1].green = 0xffff; arr[1].blue = 0;
  arr[2].red = 0x00ff; arr[2].green = 0x0100; arr[2].blue = 0x80ff;
  ok[0] = ok[1] = ok[2] = FALSE;
  M5V_CHECK (gdk_colormap_alloc_colors (sys, arr, 3, FALSE, TRUE, ok) == 0 &&
	     ok[0] && ok[1] && ok[2], "alloc_colors allocates all three");
  M5V_EQ_HEX (arr[0].pixel, 0x0012569a, "non-exact input truncates to 8 bits");
  M5V_EQ_HEX (arr[1].pixel, 0x0000ff00, "alloc_colors green");
  M5V_EQ_HEX (arr[2].pixel, 0x00000180, "alloc_colors truncation of 0x00ff/0x0100/0x80ff");
  gdk_colormap_free_colors (sys, arr, 3);
  gdk_colormap_free_colors (rgba_cm, arr, 3);
  M5V_EQ_HEX (arr[0].pixel, 0x0012569a, "free_colors leaves the pixel values alone");

  /* query_color with an input that is not a round 8-bit value: 0x1234 was
   * truncated to 0x12, which reads back as 0x1212 (255 steps over 65535) */
  memset (&q, 0, sizeof q);
  gdk_colormap_query_color (sys, arr[0].pixel, &q);
  M5V_EQ_HEX (q.red, 0x1212, "query_color red rounding");
  M5V_EQ_HEX (q.green, 0x5656, "query_color green rounding");
  M5V_EQ_HEX (q.blue, 0x9a9a, "query_color blue rounding");
  M5V_EQ_HEX (q.pixel, arr[0].pixel, "query_color keeps the pixel");
  gdk_colormap_query_color (sys, arr[2].pixel, &q);
  M5V_EQ_HEX (q.red, 0x0000, "query_color 0x00ff -> channel 0");
  M5V_EQ_HEX (q.green, 0x0101, "query_color 0x0100 -> channel 1");
  M5V_EQ_HEX (q.blue, 0x8080, "query_color 0x80ff -> channel 0x80");

  /* the rgba colormap sets the alpha byte, query ignores it */
  c.red = 0x1234; c.green = 0x5678; c.blue = 0x9abc;
  M5V_CHECK (gdk_colormap_alloc_color (rgba_cm, &c, FALSE, TRUE),
	     "alloc_color on the rgba colormap");
  M5V_EQ_HEX (c.pixel, 0xff12569a, "rgba alloc_color pixel is opaque");
  gdk_colormap_query_color (rgba_cm, c.pixel, &q);
  M5V_CHECK (q.red == 0x1212 && q.green == 0x5656 && q.blue == 0x9a9a,
	     "rgba query_color ignores alpha");
  M5V_CHECK (gdk_colormap_get_screen (rgba_cm) == screen,
	     "rgba colormap belongs to the screen");
}

static void
m5v_rgba_pixmap (GdkScreen *screen, GdkVisual *rgba, GdkColormap *rgba_cm)
{
  GdkPixmap *pm;
  GdkGC *gc;
  GdkColor c = { 0, 0xffff, 0x8080, 0x0000 };

  pm = gdk_pixmap_new (main_window, 16, 16, 32);
  M5V_CHECK (pm != NULL, "depth-32 pixmap");
  if (!pm)
    return;
  M5V_CHECK (gdk_drawable_get_depth (pm) == 32, "pixmap depth is 32");

  gdk_drawable_set_colormap (pm, rgba_cm);
  M5V_CHECK (gdk_drawable_get_colormap (pm) == rgba_cm, "pixmap takes the rgba colormap");
  M5V_CHECK (gdk_drawable_get_visual (pm) == rgba, "pixmap visual is the rgba visual");
  M5V_CHECK (gdk_drawable_get_screen (pm) == screen, "pixmap screen");

  gc = gdk_gc_new (pm);
  M5V_CHECK (gdk_gc_get_colormap (gc) == rgba_cm, "gc picks up the pixmap colormap");

  /* the rgb API on an rgba colormap: the pixel carries an opaque alpha byte */
  gdk_gc_set_rgb_fg_color (gc, &c);
  gdk_draw_rectangle (pm, gc, TRUE, 0, 0, 16, 16);
  M5V_EQ_HEX (m5v_pixel (pm, 4, 4), 0xffff8000, "rgb fg on a depth-32 pixmap");
  M5V_EQ_HEX (m5v_pixel (pm, 15, 15), 0xffff8000, "rgb fg on a depth-32 pixmap, far corner");

  /* allocated colour through the colormap, partial overwrite */
  c.red = 0; c.green = 0; c.blue = 0xffff;
  M5V_CHECK (gdk_colormap_alloc_color (rgba_cm, &c, FALSE, TRUE), "alloc blue");
  gdk_gc_set_foreground (gc, &c);
  gdk_draw_rectangle (pm, gc, TRUE, 4, 4, 4, 4);
  M5V_EQ_HEX (m5v_pixel (pm, 5, 5), 0xff0000ff, "allocated blue inside");
  M5V_EQ_HEX (m5v_pixel (pm, 9, 9), 0xffff8000, "orange untouched outside");

  g_object_unref (gc);
  g_object_unref (pm);
}

static int
m5v_exercise (void)
{
  GdkScreen *screen = gdk_screen_get_default ();
  GdkVisual *sysv = gdk_screen_get_system_visual (screen);
  GdkColormap *sys = gdk_screen_get_system_colormap (screen);
  GdkVisual *rgba = gdk_screen_get_rgba_visual (screen);
  GdkColormap *rgba_cm = gdk_screen_get_rgba_colormap (screen);

  m5v_visuals (screen, sysv, rgba);

  M5V_CHECK (rgba_cm != NULL, "gdk_screen_get_rgba_colormap");
  if (rgba_cm)
    {
      M5V_CHECK (gdk_colormap_get_visual (rgba_cm) == rgba,
		 "rgba colormap is over the rgba visual");
      M5V_CHECK (rgba_cm != sys, "rgba colormap is not the system colormap");
      M5V_CHECK (gdk_screen_get_rgba_colormap (screen) == rgba_cm,
		 "rgba colormap is stable across calls");
    }

  m5v_rgb (screen, sysv, sys);

  if (rgba_cm)
    {
      m5v_default_colormap (screen, sys, rgba_cm);
      m5v_colormap_bits (screen, sys, rgba_cm);
      if (rgba)
	m5v_rgba_pixmap (screen, rgba, rgba_cm);
    }

  if (m5v_failures)
    g_printerr ("ydk_demo: M5V: FAIL (%d)\n", m5v_failures);
  else
    g_print ("ydk_demo: M5V: PASS\n");

  return m5v_failures;
}

/* ---- M6 exercise ------------------------------------------------------
 *
 * Input completeness and the remaining screen functions.  A headless
 * Weston has no input devices, so protocol events are injected through the
 * _gdk_wayland_test_* hooks (gdkseat-wayland.h).  Each hook calls the REAL
 * wl_keyboard / wl_pointer / xdg_toplevel listener, so the translation
 * layer, the GDK queue, _gdk_windowing_got_event (child proxying, implicit
 * grabs, crossing events, double clicks), explicit grabs, the key repeat
 * timer and get_pointer all run for real.  The events are recorded with
 * their own gdk_event_handler_set () handler (the demo's one is restored at
 * the end), and the checks look them up by (type, window).
 *
 * Geometry (main_window coordinates; the pointer hooks take them):
 *   main_window  0,0 640x480       child  (100,100) 100x100
 * so (50,60) is on main only, (150,160) enters the child, (155,165) is
 * child-relative (55,65).
 */

static int m6_failures;

#define M6_CHECK(cond, what)						\
  G_STMT_START {							\
    if (!(cond))							\
      {									\
	g_printerr ("ydk_demo: M6 check failed: %s\n", what);		\
	m6_failures++;							\
      }									\
  } G_STMT_END

#define M6_EQ_INT(got, want, what)					\
  G_STMT_START {							\
    long g_ = (long) (got), w_ = (long) (want);				\
    if (g_ != w_)							\
      {									\
	g_printerr ("ydk_demo: M6 check failed: %s: got %ld want %ld\n", \
		    what, g_, w_);					\
	m6_failures++;							\
      }									\
  } G_STMT_END

/* every hook must report that it delivered the event */
#define M6_HOOK(call, what)						\
  G_STMT_START {							\
    if (!(call))							\
      {									\
	g_printerr ("ydk_demo: M6 check failed: %s: hook did not deliver\n", \
		    what);						\
	m6_failures++;							\
      }									\
  } G_STMT_END

static GQueue m6_events = G_QUEUE_INIT;

static void
m6_recorder (GdkEvent *event, gpointer data)
{
  g_queue_push_tail (&m6_events, gdk_event_copy (event));
}

static void
m6_clear (void)
{
  GdkEvent *ev;

  while ((ev = g_queue_pop_head (&m6_events)) != NULL)
    gdk_event_free (ev);
}

static void
m6_drain (void)
{
  while (g_main_context_iteration (NULL, FALSE))
    ;
}

static gboolean
m6_flag_cb (gpointer data)
{
  *(gboolean *) data = TRUE;
  return FALSE;
}

/* runs the main loop (timers, the display source) for @ms milliseconds */
static void
m6_run_ms (guint ms)
{
  gboolean done = FALSE;

  g_timeout_add (ms, m6_flag_cb, &done);
  while (!done)
    g_main_context_iteration (NULL, TRUE);
}

/* first recorded event of @type on @window (NULL window matches any) */
static GdkEvent *
m6_find (GdkEventType type, GdkWindow *window)
{
  GList *l;

  for (l = m6_events.head; l; l = l->next)
    {
      GdkEvent *ev = l->data;

      if (ev->type == type && (window == NULL || ev->any.window == window))
	return ev;
    }

  return NULL;
}

static int
m6_count (GdkEventType type, GdkWindow *window)
{
  GList *l;
  int n = 0;

  for (l = m6_events.head; l; l = l->next)
    {
      GdkEvent *ev = l->data;

      if (ev->type == type && (window == NULL || ev->any.window == window))
	n++;
    }

  return n;
}

/* number of KEY_PRESS events with @keyval */
static int
m6_count_keyval (guint keyval)
{
  GList *l;
  int n = 0;

  for (l = m6_events.head; l; l = l->next)
    {
      GdkEvent *ev = l->data;

      if (ev->type == GDK_KEY_PRESS && ev->key.keyval == keyval)
	n++;
    }

  return n;
}

/* scroll events on @window, direction in order; returns how many */
static int
m6_scrolls (GdkWindow *window, GdkScrollDirection *dirs, int max)
{
  GList *l;
  int n = 0;

  for (l = m6_events.head; l; l = l->next)
    {
      GdkEvent *ev = l->data;

      if (ev->type == GDK_SCROLL && ev->any.window == window)
	{
	  if (dirs && n < max)
	    dirs[n] = ev->scroll.direction;
	  n++;
	}
    }

  return n;
}

/* one wheel or finger frame on the vertical or horizontal axis */
static void
m6_scroll_frame (GdkDisplay *d, guint32 source, guint axis, gdouble value,
		 gint discrete, gboolean has_discrete, gboolean stop)
{
  static guint32 t = 70000;

  t += 20;
  _gdk_wayland_test_pointer_axis_source (d, source);
  if (value != 0.0)
    _gdk_wayland_test_pointer_axis (d, t, axis, value);
  if (has_discrete)
    _gdk_wayland_test_pointer_axis_discrete (d, axis, discrete);
  if (stop)
    _gdk_wayland_test_pointer_axis_stop (d, t, axis);
}

static void
m6_keymap_and_focus (GdkDisplay *d)
{
  GdkKeymap *keymap;
  guint kv = 0;
  GdkEvent *ev;

  M6_HOOK (_gdk_wayland_test_keyboard_keymap (d), "keymap");

  keymap = gdk_keymap_get_for_display (d);
  M6_CHECK (keymap != NULL, "keymap for display");
  if (keymap)
    {
      /* hardware keycodes are evdev + 8 */
      gdk_keymap_translate_keyboard_state (keymap, GDKW_TEST_KEY_A + 8, 0, 0,
					   &kv, NULL, NULL, NULL);
      M6_EQ_INT (kv, GDK_a, "translate A, no modifiers -> a");
      gdk_keymap_translate_keyboard_state (keymap, GDKW_TEST_KEY_A + 8,
					   GDK_SHIFT_MASK, 0,
					   &kv, NULL, NULL, NULL);
      M6_EQ_INT (kv, GDK_A, "translate A + Shift -> A");
      gdk_keymap_translate_keyboard_state (keymap, GDKW_TEST_KEY_A + 8,
					   GDK_LOCK_MASK, 0,
					   &kv, NULL, NULL, NULL);
      M6_EQ_INT (kv, GDK_A, "translate A + Lock -> A");
      /* the live modifier state must not leak into a translation that
       * asks for state 0 */
      M6_HOOK (_gdk_wayland_test_keyboard_modifiers (d, GDK_SHIFT_MASK),
	       "modifiers shift");
      gdk_keymap_translate_keyboard_state (keymap, GDKW_TEST_KEY_A + 8, 0, 0,
					   &kv, NULL, NULL, NULL);
      M6_EQ_INT (kv, GDK_a, "translate honours its state argument");
      M6_HOOK (_gdk_wayland_test_keyboard_modifiers (d, 0), "modifiers clear");
    }

  /* focus */
  m6_clear ();
  M6_HOOK (_gdk_wayland_test_keyboard_enter (d, main_window), "kbd enter");
  m6_drain ();
  ev = m6_find (GDK_FOCUS_CHANGE, main_window);
  M6_CHECK (ev != NULL, "focus-in event on main window");
  if (ev)
    M6_CHECK (ev->focus_change.in, "focus-in has in == TRUE");

  m6_clear ();
  M6_HOOK (_gdk_wayland_test_keyboard_leave (d, main_window), "kbd leave");
  m6_drain ();
  ev = m6_find (GDK_FOCUS_CHANGE, main_window);
  M6_CHECK (ev != NULL, "focus-out event on main window");
  if (ev)
    M6_CHECK (!ev->focus_change.in, "focus-out has in == FALSE");

  M6_HOOK (_gdk_wayland_test_keyboard_enter (d, main_window), "kbd re-enter");
  m6_drain ();
  m6_clear ();
}

static void
m6_keys (GdkDisplay *d)
{
  GdkEvent *ev;

  /* press and release A */
  M6_HOOK (_gdk_wayland_test_keyboard_key (d, 1000, GDKW_TEST_KEY_A, TRUE),
	   "key A press");
  M6_HOOK (_gdk_wayland_test_keyboard_key (d, 1010, GDKW_TEST_KEY_A, FALSE),
	   "key A release");
  m6_drain ();
  ev = m6_find (GDK_KEY_PRESS, main_window);
  M6_CHECK (ev != NULL, "KEY_PRESS on main window");
  if (ev)
    {
      M6_EQ_INT (ev->key.keyval, GDK_a, "press keyval");
      M6_EQ_INT (ev->key.hardware_keycode, GDKW_TEST_KEY_A + 8,
		 "press hardware_keycode is evdev + 8");
      M6_EQ_INT (ev->key.time, 1000, "press time");
      M6_EQ_INT (ev->key.state, 0, "press state");
      M6_CHECK (ev->key.string && strcmp (ev->key.string, "a") == 0,
		"press string is \"a\"");
      M6_CHECK (!ev->key.is_modifier, "A is not a modifier");
    }
  ev = m6_find (GDK_KEY_RELEASE, main_window);
  M6_CHECK (ev != NULL, "KEY_RELEASE on main window");
  if (ev)
    {
      M6_EQ_INT (ev->key.keyval, GDK_a, "release keyval");
      M6_EQ_INT (ev->key.time, 1010, "release time");
    }
  m6_clear ();

  /* modifier state comes from wl_keyboard.modifiers only */
  M6_HOOK (_gdk_wayland_test_keyboard_modifiers (d, GDK_SHIFT_MASK),
	   "modifiers shift");
  M6_HOOK (_gdk_wayland_test_keyboard_key (d, 1100, GDKW_TEST_KEY_A, TRUE),
	   "shifted A press");
  M6_HOOK (_gdk_wayland_test_keyboard_key (d, 1110, GDKW_TEST_KEY_A, FALSE),
	   "shifted A release");
  m6_drain ();
  ev = m6_find (GDK_KEY_PRESS, main_window);
  M6_CHECK (ev != NULL, "shifted KEY_PRESS");
  if (ev)
    {
      M6_EQ_INT (ev->key.keyval, GDK_A, "shifted keyval");
      M6_CHECK ((ev->key.state & GDK_SHIFT_MASK) != 0,
		"shifted state carries GDK_SHIFT_MASK");
      M6_CHECK (ev->key.string && strcmp (ev->key.string, "A") == 0,
		"shifted string is \"A\"");
    }
  M6_HOOK (_gdk_wayland_test_keyboard_modifiers (d, 0), "modifiers clear");
  m6_clear ();

  /* modifier key */
  M6_HOOK (_gdk_wayland_test_keyboard_key (d, 1200, GDKW_TEST_KEY_LEFTCTRL, TRUE),
	   "ctrl press");
  M6_HOOK (_gdk_wayland_test_keyboard_key (d, 1210, GDKW_TEST_KEY_LEFTCTRL, FALSE),
	   "ctrl release");
  m6_drain ();
  ev = m6_find (GDK_KEY_PRESS, main_window);
  M6_CHECK (ev != NULL, "Control_L KEY_PRESS");
  if (ev)
    {
      M6_EQ_INT (ev->key.keyval, GDK_Control_L, "Control_L keyval");
      M6_CHECK (ev->key.is_modifier, "Control_L is_modifier");
    }
  m6_clear ();

  /* Escape: the demo's own quit handler is not installed during M6 */
  M6_HOOK (_gdk_wayland_test_keyboard_key (d, 1300, GDKW_TEST_KEY_ESC, TRUE),
	   "escape press");
  M6_HOOK (_gdk_wayland_test_keyboard_key (d, 1310, GDKW_TEST_KEY_ESC, FALSE),
	   "escape release");
  m6_drain ();
  ev = m6_find (GDK_KEY_PRESS, main_window);
  M6_CHECK (ev != NULL && ev->key.keyval == GDK_Escape, "Escape keyval");
  m6_clear ();
}

static void
m6_repeat (GdkDisplay *d)
{
  GDKWTestSeatInfo info;
  GdkEvent *ev;
  GList *l;
  guint32 last;
  gboolean increasing = TRUE;
  int n;

  /* rate 50/s, delay 50 ms: repeats at 50, 70, 90 ... ms */
  M6_HOOK (_gdk_wayland_test_keyboard_repeat_info (d, 50, 50), "repeat_info");
  m6_clear ();
  M6_HOOK (_gdk_wayland_test_keyboard_key (d, 2000, GDKW_TEST_KEY_A, TRUE),
	   "repeat: press A");
  m6_run_ms (400);

  n = m6_count_keyval (GDK_a) - 1;
  M6_CHECK (n >= 5 && n <= 20, "repeat: 5..20 extra KEY_PRESS in 400 ms");
  if (n < 5 || n > 20)
    g_printerr ("ydk_demo: M6: repeat count was %d\n", n);

  last = 0;
  for (l = m6_events.head; l; l = l->next)
    {
      ev = l->data;
      if (ev->type != GDK_KEY_PRESS)
	continue;
      if (ev->key.keyval != GDK_a)
	increasing = FALSE;
      if (last != 0 && ev->key.time <= last)
	increasing = FALSE;
      last = ev->key.time;
    }
  M6_CHECK (increasing, "repeat: same keyval, strictly increasing time");

  M6_CHECK (_gdk_wayland_test_seat_info (d, &info) &&
	    info.repeat_key == GDKW_TEST_KEY_A,
	    "repeat: seat reports KEY_A repeating");

  /* release stops it */
  M6_HOOK (_gdk_wayland_test_keyboard_key (d, 2500, GDKW_TEST_KEY_A, FALSE),
	   "repeat: release A");
  m6_drain ();
  m6_clear ();
  m6_run_ms (150);
  M6_EQ_INT (m6_count (GDK_KEY_PRESS, NULL), 0, "repeat: none after release");
  M6_CHECK (_gdk_wayland_test_seat_info (d, &info) && info.repeat_key == 0,
	    "repeat: seat repeat_key cleared by release");

  /* keyboard leave stops it */
  m6_clear ();
  M6_HOOK (_gdk_wayland_test_keyboard_key (d, 3000, GDKW_TEST_KEY_A, TRUE),
	   "repeat: press before leave");
  m6_run_ms (100);
  M6_CHECK (m6_count_keyval (GDK_a) >= 2, "repeat: at least one repeat before leave");
  M6_HOOK (_gdk_wayland_test_keyboard_leave (d, main_window), "repeat: leave");
  m6_drain ();
  m6_clear ();
  m6_run_ms (150);
  M6_EQ_INT (m6_count (GDK_KEY_PRESS, NULL), 0, "repeat: none after keyboard leave");
  M6_HOOK (_gdk_wayland_test_keyboard_enter (d, main_window), "repeat: re-enter");
  m6_drain ();

  /* a new keymap stops it */
  m6_clear ();
  M6_HOOK (_gdk_wayland_test_keyboard_key (d, 4000, GDKW_TEST_KEY_A, TRUE),
	   "repeat: press before keymap");
  m6_run_ms (100);
  M6_CHECK (m6_count_keyval (GDK_a) >= 2, "repeat: at least one repeat before keymap");
  M6_HOOK (_gdk_wayland_test_keyboard_keymap (d), "repeat: second keymap");
  m6_drain ();
  m6_clear ();
  m6_run_ms (150);
  M6_EQ_INT (m6_count (GDK_KEY_PRESS, NULL), 0, "repeat: none after a keymap change");
  M6_HOOK (_gdk_wayland_test_keyboard_key (d, 4500, GDKW_TEST_KEY_A, FALSE),
	   "repeat: release after keymap");
  m6_drain ();

  /* modifier keys never repeat */
  m6_clear ();
  M6_HOOK (_gdk_wayland_test_keyboard_key (d, 5000, GDKW_TEST_KEY_LEFTSHIFT, TRUE),
	   "repeat: press Shift");
  m6_run_ms (150);
  M6_EQ_INT (m6_count (GDK_KEY_PRESS, NULL), 1, "repeat: Shift does not repeat");
  M6_HOOK (_gdk_wayland_test_keyboard_key (d, 5200, GDKW_TEST_KEY_LEFTSHIFT, FALSE),
	   "repeat: release Shift");
  m6_drain ();

  /* rate 0 disables repeat */
  M6_HOOK (_gdk_wayland_test_keyboard_repeat_info (d, 0, 0), "repeat_info 0");
  m6_clear ();
  M6_HOOK (_gdk_wayland_test_keyboard_key (d, 6000, GDKW_TEST_KEY_A, TRUE),
	   "repeat: press with rate 0");
  m6_run_ms (150);
  M6_EQ_INT (m6_count (GDK_KEY_PRESS, NULL), 1, "repeat: rate 0 disables repeat");
  M6_HOOK (_gdk_wayland_test_keyboard_key (d, 6200, GDKW_TEST_KEY_A, FALSE),
	   "repeat: release with rate 0");
  m6_drain ();

  M6_HOOK (_gdk_wayland_test_keyboard_repeat_info (d, 25, 400),
	   "repeat_info restore");
  m6_clear ();
}

static void
m6_pointer_position (GdkDisplay *d, GdkWindow *child)
{
  GdkWindow *w;
  GdkScreen *scr;
  gint x, y, wx, wy;
  GdkModifierType mask;
  GdkEvent *ev;

  m6_clear ();
  M6_HOOK (_gdk_wayland_test_pointer_enter (d, main_window, 50, 60),
	   "pointer enter");
  m6_drain ();
  M6_CHECK (m6_find (GDK_ENTER_NOTIFY, main_window) != NULL,
	    "ENTER_NOTIFY on main window");

  mask = 0;
  gdk_window_get_pointer (main_window, &x, &y, &mask);
  M6_CHECK (x == 50 && y == 60, "gdk_window_get_pointer position (50,60)");
  M6_CHECK ((mask & GDK_BUTTON1_MASK) == 0, "get_pointer mask has no buttons");

  scr = NULL;
  gdk_display_get_pointer (d, &scr, &x, &y, &mask);
  M6_CHECK (x == 50 && y == 60, "gdk_display_get_pointer position (50,60)");
  M6_CHECK (scr == gdk_screen_get_default (), "gdk_display_get_pointer screen");

  w = gdk_display_get_window_at_pointer (d, &wx, &wy);
  M6_CHECK (w == main_window, "window_at_pointer is main window");
  M6_CHECK (wx == 50 && wy == 60, "window_at_pointer coordinates (50,60)");

  /* into the child: the first move yields only crossing events */
  m6_clear ();
  M6_HOOK (_gdk_wayland_test_pointer_motion (d, 10000, 150, 160), "motion into child");
  m6_drain ();
  M6_CHECK (m6_find (GDK_ENTER_NOTIFY, child) != NULL,
	    "ENTER_NOTIFY on the child");

  m6_clear ();
  M6_HOOK (_gdk_wayland_test_pointer_motion (d, 10010, 155, 165), "motion in child");
  m6_drain ();
  ev = m6_find (GDK_MOTION_NOTIFY, child);
  M6_CHECK (ev != NULL, "MOTION_NOTIFY on the child");
  if (ev)
    {
      M6_EQ_INT ((long) ev->motion.x, 55, "motion child-relative x");
      M6_EQ_INT ((long) ev->motion.y, 65, "motion child-relative y");
      M6_CHECK (ev->motion.device == gdk_device_get_core_pointer (),
		"motion device is the core pointer");
      M6_EQ_INT (ev->motion.time, 10010, "motion time");
    }

  w = gdk_display_get_window_at_pointer (d, &wx, &wy);
  M6_CHECK (w == child, "window_at_pointer is the child");
  M6_CHECK (wx == 55 && wy == 65, "window_at_pointer child coordinates (55,65)");
  m6_clear ();
}

static void
m6_buttons (GdkDisplay *d, GdkWindow *child)
{
  static const struct
  {
    guint evdev;
    guint gdk;
    const char *name;
  } map[] = {
    { GDKW_TEST_BTN_RIGHT,  3, "BTN_RIGHT -> 3" },
    { GDKW_TEST_BTN_MIDDLE, 2, "BTN_MIDDLE -> 2" },
    { GDKW_TEST_BTN_LEFT,   1, "BTN_LEFT -> 1" },
    { GDKW_TEST_BTN_SIDE,   8, "BTN_SIDE -> 8" }
  };
  guint32 t = 20000;
  GdkWindow *gw = NULL;
  gboolean oe;
  GdkEvent *ev;
  GdkModifierType mask;
  gint x, y;
  guint i;

  for (i = 0; i < G_N_ELEMENTS (map); i++)
    {
      m6_clear ();
      M6_HOOK (_gdk_wayland_test_pointer_button (d, t, map[i].evdev, TRUE),
	       map[i].name);
      t += 20;
      M6_HOOK (_gdk_wayland_test_pointer_button (d, t, map[i].evdev, FALSE),
	       map[i].name);
      t += 500;
      m6_drain ();
      ev = m6_find (GDK_BUTTON_PRESS, child);
      M6_CHECK (ev != NULL, map[i].name);
      if (ev)
	M6_EQ_INT (ev->button.button, map[i].gdk, map[i].name);
      M6_CHECK (m6_find (GDK_BUTTON_RELEASE, child) != NULL,
		"button release reaches the child");
    }

  /* implicit grab, state and release */
  m6_clear ();
  M6_HOOK (_gdk_wayland_test_pointer_button (d, t, GDKW_TEST_BTN_LEFT, TRUE),
	   "left press");
  m6_drain ();
  ev = m6_find (GDK_BUTTON_PRESS, child);
  M6_CHECK (ev != NULL, "left press on child");
  if (ev)
    M6_CHECK ((ev->button.state & GDK_BUTTON1_MASK) == 0,
	      "press state is the state before the press");

  mask = 0;
  gdk_window_get_pointer (main_window, &x, &y, &mask);
  M6_CHECK ((mask & GDK_BUTTON1_MASK) != 0, "get_pointer mask has BUTTON1 while down");
  M6_CHECK (gdk_pointer_grab_info_libgtk_only (d, &gw, &oe) && gw == child,
	    "implicit grab on the child while the button is down");
  M6_CHECK (!gdk_pointer_is_grabbed (), "implicit grab is not gdk_pointer_is_grabbed");

  /* Motion outside the child is still delivered to it.  As on X11 the
   * first move out only yields the crossing events (proxy_pointer_event
   * uses else-if), so look at the second one. */
  M6_HOOK (_gdk_wayland_test_pointer_motion (d, t + 8, 300, 300),
	   "first motion outside while down");
  m6_drain ();
  m6_clear ();
  M6_HOOK (_gdk_wayland_test_pointer_motion (d, t + 10, 305, 305),
	   "motion outside while down");
  m6_drain ();
  ev = m6_find (GDK_MOTION_NOTIFY, child);
  M6_CHECK (ev != NULL, "motion outside is delivered to the grabbing child");
  M6_CHECK (m6_find (GDK_MOTION_NOTIFY, main_window) == NULL,
	    "no motion for main while the implicit grab is active");

  m6_clear ();
  M6_HOOK (_gdk_wayland_test_pointer_button (d, t + 20, GDKW_TEST_BTN_LEFT, FALSE),
	   "left release");
  m6_drain ();
  ev = m6_find (GDK_BUTTON_RELEASE, child);
  M6_CHECK (ev != NULL, "left release on child");
  if (ev)
    {
      M6_EQ_INT (ev->button.button, 1, "release button");
      M6_CHECK ((ev->button.state & GDK_BUTTON1_MASK) != 0,
		"release state still carries BUTTON1");
    }
  M6_CHECK (!gdk_pointer_grab_info_libgtk_only (d, &gw, &oe),
	    "implicit grab gone after release");

  m6_clear ();
  M6_HOOK (_gdk_wayland_test_pointer_motion (d, t + 40, 305, 305),
	   "motion after release");
  m6_drain ();
  M6_CHECK (m6_find (GDK_MOTION_NOTIFY, main_window) != NULL,
	    "motion goes to main after the grab ended");
  M6_CHECK (m6_find (GDK_MOTION_NOTIFY, child) == NULL,
	    "no motion for the child after the grab ended");
  t += 1000;

  /* double and triple click: three presses 100 ms apart, same spot */
  M6_HOOK (_gdk_wayland_test_pointer_motion (d, t, 155, 165), "motion for clicks");
  t += 600;
  m6_clear ();
  for (i = 0; i < 3; i++)
    {
      M6_HOOK (_gdk_wayland_test_pointer_button (d, t, GDKW_TEST_BTN_LEFT, TRUE),
	       "click press");
      M6_HOOK (_gdk_wayland_test_pointer_button (d, t + 40, GDKW_TEST_BTN_LEFT, FALSE),
	       "click release");
      t += 100;
      m6_drain ();
    }
  M6_CHECK (m6_find (GDK_2BUTTON_PRESS, NULL) != NULL, "GDK_2BUTTON_PRESS generated");
  M6_CHECK (m6_find (GDK_3BUTTON_PRESS, NULL) != NULL, "GDK_3BUTTON_PRESS generated");
  m6_clear ();
}

static void
m6_scroll (GdkDisplay *d, GdkWindow *child)
{
  GdkScrollDirection dirs[8];
  int n;

  m6_clear ();
  M6_HOOK (_gdk_wayland_test_pointer_motion (d, 30000, 155, 165), "motion for scroll");
  m6_drain ();
  m6_clear ();

  /* wheel, vertical down: nothing before the frame */
  m6_scroll_frame (d, GDKW_TEST_AXIS_SOURCE_WHEEL, GDKW_TEST_AXIS_VERTICAL,
		   10.0, 1, TRUE, FALSE);
  m6_drain ();
  M6_EQ_INT (m6_scrolls (child, NULL, 0), 0, "scroll: nothing before the frame");
  M6_HOOK (_gdk_wayland_test_pointer_frame (d), "frame");
  m6_drain ();
  n = m6_scrolls (child, dirs, 8);
  M6_EQ_INT (n, 1, "scroll: wheel down is one event");
  M6_CHECK (n >= 1 && dirs[0] == GDK_SCROLL_DOWN, "scroll: wheel down");
  m6_clear ();

  m6_scroll_frame (d, GDKW_TEST_AXIS_SOURCE_WHEEL, GDKW_TEST_AXIS_VERTICAL,
		   -10.0, -1, TRUE, FALSE);
  _gdk_wayland_test_pointer_frame (d);
  m6_drain ();
  n = m6_scrolls (child, dirs, 8);
  M6_EQ_INT (n, 1, "scroll: wheel up is one event");
  M6_CHECK (n >= 1 && dirs[0] == GDK_SCROLL_UP, "scroll: wheel up");
  m6_clear ();

  m6_scroll_frame (d, GDKW_TEST_AXIS_SOURCE_WHEEL, GDKW_TEST_AXIS_HORIZONTAL,
		   10.0, 1, TRUE, FALSE);
  _gdk_wayland_test_pointer_frame (d);
  m6_drain ();
  n = m6_scrolls (child, dirs, 8);
  M6_EQ_INT (n, 1, "scroll: wheel right is one event");
  M6_CHECK (n >= 1 && dirs[0] == GDK_SCROLL_RIGHT, "scroll: wheel right");
  m6_clear ();

  m6_scroll_frame (d, GDKW_TEST_AXIS_SOURCE_WHEEL, GDKW_TEST_AXIS_HORIZONTAL,
		   -10.0, -1, TRUE, FALSE);
  _gdk_wayland_test_pointer_frame (d);
  m6_drain ();
  n = m6_scrolls (child, dirs, 8);
  M6_EQ_INT (n, 1, "scroll: wheel left is one event");
  M6_CHECK (n >= 1 && dirs[0] == GDK_SCROLL_LEFT, "scroll: wheel left");
  m6_clear ();

  /* three steps at once */
  m6_scroll_frame (d, GDKW_TEST_AXIS_SOURCE_WHEEL, GDKW_TEST_AXIS_VERTICAL,
		   30.0, 3, TRUE, FALSE);
  _gdk_wayland_test_pointer_frame (d);
  m6_drain ();
  M6_EQ_INT (m6_scrolls (child, NULL, 0), 3, "scroll: discrete 3 is three events");
  m6_clear ();

  /* finger: 25.0 is two steps with 0.5 carried over */
  m6_scroll_frame (d, GDKW_TEST_AXIS_SOURCE_FINGER, GDKW_TEST_AXIS_VERTICAL,
		   25.0, 0, FALSE, FALSE);
  _gdk_wayland_test_pointer_frame (d);
  m6_drain ();
  n = m6_scrolls (child, dirs, 8);
  M6_EQ_INT (n, 2, "scroll: finger 25.0 is two events");
  M6_CHECK (n >= 1 && dirs[0] == GDK_SCROLL_DOWN, "scroll: finger down");
  m6_clear ();

  /* +5.0 completes the carried half step */
  m6_scroll_frame (d, GDKW_TEST_AXIS_SOURCE_FINGER, GDKW_TEST_AXIS_VERTICAL,
		   5.0, 0, FALSE, FALSE);
  _gdk_wayland_test_pointer_frame (d);
  m6_drain ();
  M6_EQ_INT (m6_scrolls (child, NULL, 0), 1, "scroll: finger remainder adds up");
  m6_clear ();

  /* axis_stop drops the remainder: 5.0 + stop is nothing, and a later 5.0
   * starts from zero */
  m6_scroll_frame (d, GDKW_TEST_AXIS_SOURCE_FINGER, GDKW_TEST_AXIS_VERTICAL,
		   5.0, 0, FALSE, TRUE);
  _gdk_wayland_test_pointer_frame (d);
  m6_drain ();
  M6_EQ_INT (m6_scrolls (child, NULL, 0), 0, "scroll: axis_stop frame emits nothing");
  m6_scroll_frame (d, GDKW_TEST_AXIS_SOURCE_FINGER, GDKW_TEST_AXIS_VERTICAL,
		   5.0, 0, FALSE, FALSE);
  _gdk_wayland_test_pointer_frame (d);
  m6_drain ();
  M6_EQ_INT (m6_scrolls (child, NULL, 0), 0,
	     "scroll: remainder was reset by axis_stop");
  /* leave nothing behind for the next section */
  m6_scroll_frame (d, GDKW_TEST_AXIS_SOURCE_FINGER, GDKW_TEST_AXIS_VERTICAL,
		   0.0, 0, FALSE, TRUE);
  _gdk_wayland_test_pointer_frame (d);
  m6_drain ();
  m6_clear ();
}

static void
m6_grabs (GdkDisplay *d, GdkWindow *child)
{
  const GdkEventMask grab_mask = GDK_BUTTON_PRESS_MASK |
    GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK;
  GdkWindowAttr attr;
  GdkWindow *w2;
  GdkEvent *ev;
  guint32 t = 40000;

  M6_HOOK (_gdk_wayland_test_pointer_motion (d, t, 155, 165), "motion for grabs");
  m6_drain ();

  /* explicit grab, owner_events FALSE: the press goes to the grab window */
  M6_EQ_INT (gdk_pointer_grab (main_window, FALSE, grab_mask, NULL, NULL,
			       GDK_CURRENT_TIME),
	     GDK_GRAB_SUCCESS, "pointer_grab on main");
  M6_CHECK (gdk_pointer_is_grabbed (), "pointer is grabbed");
  m6_clear ();
  t += 100;
  M6_HOOK (_gdk_wayland_test_pointer_button (d, t, GDKW_TEST_BTN_LEFT, TRUE),
	   "grabbed press");
  M6_HOOK (_gdk_wayland_test_pointer_button (d, t + 20, GDKW_TEST_BTN_LEFT, FALSE),
	   "grabbed release");
  m6_drain ();
  ev = m6_find (GDK_BUTTON_PRESS, main_window);
  M6_CHECK (ev != NULL, "grabbed press is delivered to the grab window");
  if (ev)
    {
      M6_EQ_INT ((long) ev->button.x, 155, "grabbed press x is main relative");
      M6_EQ_INT ((long) ev->button.y, 165, "grabbed press y is main relative");
    }
  M6_CHECK (m6_find (GDK_BUTTON_PRESS, child) == NULL,
	    "the child gets no press during the grab");
  M6_CHECK (gdk_pointer_is_grabbed (), "explicit grab survives the release");

  gdk_pointer_ungrab (GDK_CURRENT_TIME);
  M6_CHECK (!gdk_pointer_is_grabbed (), "pointer_ungrab ends the grab at once");
  m6_drain ();

  m6_clear ();
  t += 600;
  M6_HOOK (_gdk_wayland_test_pointer_button (d, t, GDKW_TEST_BTN_LEFT, TRUE),
	   "ungrabbed press");
  M6_HOOK (_gdk_wayland_test_pointer_button (d, t + 20, GDKW_TEST_BTN_LEFT, FALSE),
	   "ungrabbed release");
  m6_drain ();
  M6_CHECK (m6_find (GDK_BUTTON_PRESS, child) != NULL,
	    "after ungrab the press goes to the child again");
  M6_CHECK (m6_find (GDK_BUTTON_PRESS, main_window) == NULL,
	    "after ungrab main gets no press");

  /* owner_events TRUE: a press over the child still goes to the child */
  M6_EQ_INT (gdk_pointer_grab (main_window, TRUE, grab_mask, NULL, NULL,
			       GDK_CURRENT_TIME),
	     GDK_GRAB_SUCCESS, "pointer_grab owner_events");
  m6_clear ();
  t += 600;
  M6_HOOK (_gdk_wayland_test_pointer_button (d, t, GDKW_TEST_BTN_LEFT, TRUE),
	   "owner press");
  M6_HOOK (_gdk_wayland_test_pointer_button (d, t + 20, GDKW_TEST_BTN_LEFT, FALSE),
	   "owner release");
  m6_drain ();
  M6_CHECK (m6_find (GDK_BUTTON_PRESS, child) != NULL,
	    "owner_events press goes to the child");
  gdk_pointer_ungrab (GDK_CURRENT_TIME);
  M6_CHECK (!gdk_pointer_is_grabbed (), "owner_events grab ended");
  m6_drain ();

  /* a grab on a window that is not mapped */
  memset (&attr, 0, sizeof attr);
  attr.window_type = GDK_WINDOW_TOPLEVEL;
  attr.wclass = GDK_INPUT_OUTPUT;
  attr.width = 50;
  attr.height = 50;
  attr.event_mask = GDK_BUTTON_PRESS_MASK;
  w2 = gdk_window_new (gdk_get_default_root_window (), &attr, 0);
  M6_CHECK (w2 != NULL, "second toplevel created");
  if (w2)
    {
      M6_EQ_INT (gdk_pointer_grab (w2, FALSE, GDK_BUTTON_PRESS_MASK, NULL,
				   NULL, GDK_CURRENT_TIME),
		 GDK_GRAB_NOT_VIEWABLE, "grab on an unmapped window");
      M6_CHECK (!gdk_pointer_is_grabbed (), "failed grab leaves no grab");
      gdk_window_destroy (w2);
    }

  /* keyboard grab redirects key events */
  m6_clear ();
  M6_EQ_INT (gdk_keyboard_grab (child, FALSE, GDK_CURRENT_TIME),
	     GDK_GRAB_SUCCESS, "keyboard_grab on the child");
  M6_HOOK (_gdk_wayland_test_keyboard_key (d, 50000, GDKW_TEST_KEY_A, TRUE),
	   "grabbed key press");
  M6_HOOK (_gdk_wayland_test_keyboard_key (d, 50010, GDKW_TEST_KEY_A, FALSE),
	   "grabbed key release");
  m6_drain ();
  M6_CHECK (m6_find (GDK_KEY_PRESS, child) != NULL,
	    "key press is redirected to the keyboard grab window");
  M6_CHECK (m6_find (GDK_KEY_PRESS, main_window) == NULL,
	    "main gets no key press during the keyboard grab");

  gdk_keyboard_ungrab (GDK_CURRENT_TIME);
  m6_clear ();
  M6_HOOK (_gdk_wayland_test_keyboard_key (d, 50100, GDKW_TEST_KEY_A, TRUE),
	   "ungrabbed key press");
  M6_HOOK (_gdk_wayland_test_keyboard_key (d, 50110, GDKW_TEST_KEY_A, FALSE),
	   "ungrabbed key release");
  m6_drain ();
  M6_CHECK (m6_find (GDK_KEY_PRESS, main_window) != NULL,
	    "after the ungrab key presses go to main again");
  m6_clear ();
}

static void
m6_close (GdkDisplay *d)
{
  m6_clear ();
  M6_HOOK (_gdk_wayland_test_toplevel_close (main_window), "toplevel close");
  m6_drain ();
  M6_EQ_INT (m6_count (GDK_DELETE, main_window), 1,
	     "exactly one GDK_DELETE on the main window");
  m6_clear ();
}

static void
m6_screen (GdkDisplay *d)
{
  GdkScreen *screen = gdk_screen_get_default ();
  GdkRectangle r;
  GdkWindow *active;
  GdkEvent *cev;
  GList *l, *stack;
  GValue v = { 0, };
  const gchar *env;
  gboolean found;
  gchar *s;
  gint wmm, hmm;

  M6_EQ_INT (gdk_screen_get_number (screen), 0, "screen number");
  M6_EQ_INT (gdk_screen_get_n_monitors (screen), 1, "n_monitors");
  M6_EQ_INT (gdk_screen_get_primary_monitor (screen), 0, "primary monitor");

  memset (&r, 0xff, sizeof r);
  gdk_screen_get_monitor_geometry (screen, 0, &r);
  M6_CHECK (r.x == 0 && r.y == 0 &&
	    r.width == gdk_screen_get_width (screen) &&
	    r.height == gdk_screen_get_height (screen),
	    "monitor 0 geometry is the whole screen");
  M6_EQ_INT (gdk_screen_get_monitor_at_point (screen, 5, 5), 0, "monitor_at_point");
  M6_EQ_INT (gdk_screen_get_monitor_at_window (screen, main_window), 0,
	     "monitor_at_window");

  wmm = gdk_screen_get_width_mm (screen);
  hmm = gdk_screen_get_height_mm (screen);
  M6_CHECK (wmm > 0 && hmm > 0, "screen size in mm is positive");
  M6_EQ_INT (gdk_screen_get_monitor_width_mm (screen, 0), wmm,
	     "monitor 0 width_mm equals the screen's");
  M6_EQ_INT (gdk_screen_get_monitor_height_mm (screen, 0), hmm,
	     "monitor 0 height_mm equals the screen's");

  s = gdk_screen_get_monitor_plug_name (screen, 0);
  M6_CHECK (s != NULL && s[0] != '\0', "monitor plug name");
  g_free (s);

  M6_CHECK (gdk_screen_is_composited (screen), "screen is composited");

  s = gdk_screen_make_display_name (screen);
  M6_CHECK (s != NULL, "make_display_name");
  env = g_getenv ("WAYLAND_DISPLAY");
  if (s && env)
    M6_CHECK (strcmp (s, env) == 0, "make_display_name is WAYLAND_DISPLAY");
  g_free (s);

  /* active window follows keyboard focus */
  M6_HOOK (_gdk_wayland_test_keyboard_enter (d, main_window), "screen: kbd enter");
  active = gdk_screen_get_active_window (screen);
  M6_CHECK (active == main_window, "active window is main after focus in");
  if (active)
    g_object_unref (active);
  M6_HOOK (_gdk_wayland_test_keyboard_leave (d, main_window), "screen: kbd leave");
  active = gdk_screen_get_active_window (screen);
  M6_CHECK (active == NULL, "no active window after focus out");
  if (active)
    g_object_unref (active);
  m6_drain ();

  found = FALSE;
  stack = gdk_screen_get_window_stack (screen);
  for (l = stack; l; l = l->next)
    {
      if (l->data == main_window)
	found = TRUE;
      g_object_unref (l->data);
    }
  g_list_free (stack);
  M6_CHECK (found, "window stack contains the main window");

  found = FALSE;
  stack = gdk_screen_get_toplevel_windows (screen);
  for (l = stack; l; l = l->next)
    if (l->data == main_window)
      found = TRUE;
  g_list_free (stack);
  M6_CHECK (found, "toplevel windows contain the main window");

  g_value_init (&v, G_TYPE_INT);
  M6_CHECK (gdk_screen_get_setting (screen, "gtk-double-click-time", &v) &&
	    g_value_get_int (&v) == 400, "setting gtk-double-click-time is 400");
  g_value_unset (&v);
  g_value_init (&v, G_TYPE_INT);
  M6_CHECK (!gdk_screen_get_setting (screen, "no-such-setting", &v),
	    "unknown setting is not found");
  g_value_unset (&v);

  cev = gdk_event_new (GDK_CLIENT_EVENT);
  cev->client.window = g_object_ref (main_window);
  cev->client.message_type = gdk_atom_intern_static_string ("YDK_M6_TEST");
  cev->client.data_format = 32;
  gdk_screen_broadcast_client_message (screen, cev);
  gdk_event_free (cev);
  M6_CHECK (TRUE, "broadcast_client_message returned");
}

static int
m6_exercise (void)
{
  GdkDisplay *display = gdk_display_get_default ();
  GdkWindowAttr attr;
  GdkWindow *child;

  /* the recorder replaces the demo handler for the duration of M6 */
  gdk_event_handler_set (m6_recorder, NULL, NULL);

  gdk_window_set_events (main_window,
			 GDK_EXPOSURE_MASK | GDK_KEY_PRESS_MASK |
			 GDK_KEY_RELEASE_MASK | GDK_BUTTON_PRESS_MASK |
			 GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK |
			 GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK |
			 GDK_FOCUS_CHANGE_MASK | GDK_SCROLL_MASK);

  memset (&attr, 0, sizeof attr);
  attr.window_type = GDK_WINDOW_CHILD;
  attr.wclass = GDK_INPUT_OUTPUT;
  attr.x = 100;
  attr.y = 100;
  attr.width = 100;
  attr.height = 100;
  attr.event_mask = GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
    GDK_POINTER_MOTION_MASK | GDK_ENTER_NOTIFY_MASK |
    GDK_LEAVE_NOTIFY_MASK | GDK_SCROLL_MASK;
  child = gdk_window_new (main_window, &attr, GDK_WA_X | GDK_WA_Y);
  M6_CHECK (child != NULL, "child window created");
  if (child == NULL)
    {
      g_printerr ("ydk_demo: M6: FAIL (%d)\n", m6_failures);
      gdk_event_handler_set (event_handler, NULL, NULL);
      return m6_failures ? m6_failures : 1;
    }
  gdk_window_show (child);

  /* exercise the wl_pointer v5 grouping whatever the compositor offers */
  M6_HOOK (_gdk_wayland_test_seat_set_version (display, 5), "seat version");
  m6_drain ();
  m6_clear ();

  m6_keymap_and_focus (display);
  m6_keys (display);
  m6_repeat (display);
  m6_pointer_position (display, child);
  m6_buttons (display, child);
  m6_scroll (display, child);
  m6_grabs (display, child);
  m6_close (display);
  m6_screen (display);

  /* cleanup */
  _gdk_wayland_test_pointer_leave (display, main_window);
  _gdk_wayland_test_keyboard_leave (display, main_window);
  m6_drain ();
  m6_clear ();
  gdk_window_destroy (child);
  gdk_event_handler_set (event_handler, NULL, NULL);
  /* the scene was not repainted while the recorder was installed */
  gdk_window_invalidate_rect (main_window, NULL, TRUE);

  if (m6_failures)
    g_printerr ("ydk_demo: M6: FAIL (%d)\n", m6_failures);
  else
    g_print ("ydk_demo: M6: PASS\n");

  return m6_failures;
}

/* ---- M6b -----------------------------------------------------------------
 *
 * Every public function that M6b moved behind the backend dispatcher: the
 * keymap and keyval functions, the display level functions, and the toplevel
 * hints.  Plus the gaps found by the M6 audit (pointer state with a button
 * held, warp, beep, events_pending, destroying windows that hold input
 * state, closing a display that still has windows).
 *
 * Toplevel hints are observed with _gdk_wayland_test_toplevel_info (), which
 * reports what the backend last sent to the compositor, and
 * _gdk_wayland_test_toplevel_configure (), which feeds an xdg_toplevel
 * configure with chosen states through the real listener.  Keyboard state and
 * keymap text come from the two hooks added for M6b.  Same style as M6:
 * every check runs, failures are counted, CRITICAL stays fatal.
 */

static int m6b_failures;

#define M6B_CHECK(cond, what)						\
  G_STMT_START {							\
    if (!(cond))							\
      {									\
	g_printerr ("ydk_demo: M6B check failed: %s\n", what);		\
	m6b_failures++;							\
      }									\
  } G_STMT_END

#define M6B_EQ_INT(got, want, what)					\
  G_STMT_START {							\
    long g_ = (long) (got), w_ = (long) (want);				\
    if (g_ != w_)							\
      {									\
	g_printerr ("ydk_demo: M6B check failed: %s: got %ld want %ld\n", \
		    what, g_, w_);					\
	m6b_failures++;							\
      }									\
  } G_STMT_END

#define M6B_EQ_STR(got, want, what)					\
  G_STMT_START {							\
    const char *g_ = (got), *w_ = (want);				\
    if (g_strcmp0 (g_, w_) != 0)					\
      {									\
	g_printerr ("ydk_demo: M6B check failed: %s: got \"%s\" want \"%s\"\n", \
		    what, g_ ? g_ : "(null)", w_ ? w_ : "(null)");	\
	m6b_failures++;							\
      }									\
  } G_STMT_END

#define M6B_HOOK(call, what)						\
  G_STMT_START {							\
    if (!(call))							\
      {									\
	g_printerr ("ydk_demo: M6B check failed: %s: hook did not deliver\n", \
		    what);						\
	m6b_failures++;							\
      }									\
  } G_STMT_END

/* exported by libydk but declared only in the private gdkprivate.h */
extern void gdk_window_destroy_notify (GdkWindow *window);

#define DEMO_WINDOW_DESTROYED(w) (((GdkWindowObject *) (w))->destroyed)

static gboolean
m6b_info (GdkWindow *w, GDKWTestToplevelInfo *info)
{
  memset (info, 0, sizeof *info);
  return _gdk_wayland_test_toplevel_info (w, info);
}

static GdkWindow *
m6b_toplevel (gint w, gint h, gint event_mask, const gchar *wmclass_name,
	      const gchar *wmclass_class)
{
  GdkWindowAttr attr;
  gint mask = 0;

  memset (&attr, 0, sizeof attr);
  attr.window_type = GDK_WINDOW_TOPLEVEL;
  attr.wclass = GDK_INPUT_OUTPUT;
  attr.width = w;
  attr.height = h;
  attr.event_mask = event_mask;
  if (wmclass_name)
    {
      attr.wmclass_name = (gchar *) wmclass_name;
      attr.wmclass_class = (gchar *) wmclass_class;
      mask |= GDK_WA_WMCLASS;
    }

  return gdk_window_new (gdk_get_default_root_window (), &attr, mask);
}

static GdkFilterReturn
m6b_dummy_filter (GdkXEvent *xevent, GdkEvent *event, gpointer data)
{
  return GDK_FILTER_CONTINUE;
}

/* ---- keymap ---------------------------------------------------------- */

/* Two layouts: "us"-like latin and a hebrew aleph group, for bidi tests. */
static const gchar m6b_bidi_keymap[] =
  "xkb_keymap {\n"
  "xkb_keycodes \"ydk\" {\n"
  "  minimum = 8;\n"
  "  maximum = 255;\n"
  "  <AC01> = 38;\n"
  "  <LFSH> = 50;\n"
  "};\n"
  "xkb_types \"ydk\" {\n"
  "  type \"ONE_LEVEL\" {\n"
  "    modifiers = none;\n"
  "    map[none] = Level1;\n"
  "    level_name[Level1] = \"Any\";\n"
  "  };\n"
  "  type \"TWO_LEVEL\" {\n"
  "    modifiers = Shift;\n"
  "    map[Shift] = Level2;\n"
  "    level_name[Level1] = \"Base\";\n"
  "    level_name[Level2] = \"Shift\";\n"
  "  };\n"
  "};\n"
  "xkb_compatibility \"ydk\" {\n"
  "  interpret Any+AnyOf(all) {\n"
  "    action = SetMods(modifiers=modMapMods,clearLocks);\n"
  "  };\n"
  "};\n"
  "xkb_symbols \"ydk\" {\n"
  "  key <AC01> { type = \"TWO_LEVEL\",\n"
  "    symbols[Group1] = [ a, A ],\n"
  "    symbols[Group2] = [ hebrew_aleph, hebrew_aleph ] };\n"
  "  key <LFSH> { [ Shift_L ] };\n"
  "  modifier_map Shift { <LFSH> };\n"
  "};\n"
  "};\n";

static void
m6b_keymap (GdkDisplay *d)
{
  GdkKeymap *keymap;
  GdkKeymapKey *keys = NULL;
  guint *keyvals = NULL;
  gint n = 0, i;
  gboolean found;
  GdkModifierType state;
  GdkKeymapKey key;
  guint lower, upper;
  gboolean alt_is_meta;

  M6B_HOOK (_gdk_wayland_test_keyboard_keymap (d), "m6b keymap");
  m6_drain ();
  keymap = gdk_keymap_get_for_display (d);
  M6B_CHECK (keymap != NULL, "keymap for display");
  if (keymap == NULL)
    return;

  M6B_EQ_INT (gdk_keymap_get_direction (keymap), PANGO_DIRECTION_LTR,
	      "default keymap direction is LTR");
  M6B_CHECK (!gdk_keymap_have_bidi_layouts (keymap),
	     "default keymap has no bidi layouts");

  /* caps lock follows the live modifier state */
  M6B_CHECK (!gdk_keymap_get_caps_lock_state (keymap), "caps lock off");
  M6B_HOOK (_gdk_wayland_test_keyboard_state (d, 0, GDK_LOCK_MASK, 0),
	    "caps lock on");
  M6B_CHECK (gdk_keymap_get_caps_lock_state (keymap), "caps lock on after hook");
  M6B_HOOK (_gdk_wayland_test_keyboard_state (d, 0, 0, 0), "caps lock off hook");
  M6B_CHECK (!gdk_keymap_get_caps_lock_state (keymap), "caps lock off again");

  /* entries for a keyval; evdev A is 30, hardware keycode 38 */
  keys = NULL;
  n = 0;
  M6B_CHECK (gdk_keymap_get_entries_for_keyval (keymap, GDK_a, &keys, &n) &&
	     n > 0 && keys != NULL, "entries for GDK_a");
  found = FALSE;
  for (i = 0; keys && i < n; i++)
    if (keys[i].keycode == 38 && keys[i].group == 0 && keys[i].level == 0)
      found = TRUE;
  M6B_CHECK (found, "GDK_a is keycode 38 group 0 level 0");
  g_free (keys);

  keys = NULL;
  n = 0;
  M6B_CHECK (gdk_keymap_get_entries_for_keyval (keymap, GDK_A, &keys, &n),
	     "entries for GDK_A");
  found = FALSE;
  for (i = 0; keys && i < n; i++)
    if (keys[i].keycode == 38 && keys[i].group == 0 && keys[i].level == 1)
      found = TRUE;
  M6B_CHECK (found, "GDK_A is keycode 38 group 0 level 1");
  g_free (keys);

  keys = (GdkKeymapKey *) 0x1;
  n = 99;
  M6B_CHECK (!gdk_keymap_get_entries_for_keyval (keymap, GDK_hebrew_aleph,
						 &keys, &n),
	     "no entry for hebrew aleph in a latin keymap");
  M6B_CHECK (keys == NULL && n == 0, "failed lookup clears the outputs");

  /* entries for a keycode */
  keys = NULL;
  keyvals = NULL;
  n = 0;
  M6B_CHECK (gdk_keymap_get_entries_for_keycode (keymap, 38, &keys, &keyvals,
						 &n) && n >= 2,
	     "entries for keycode 38");
  if (keyvals && n >= 2)
    {
      M6B_EQ_INT (keyvals[0], GDK_a, "keycode 38 entry 0 is a");
      M6B_EQ_INT (keyvals[1], GDK_A, "keycode 38 entry 1 is A");
    }
  g_free (keys);
  g_free (keyvals);
  M6B_CHECK (!gdk_keymap_get_entries_for_keycode (keymap, 0, NULL, NULL, &n),
	     "keycode 0 has no entries");
  M6B_CHECK (!gdk_keymap_get_entries_for_keycode (keymap, 999, NULL, NULL, &n),
	     "keycode 999 has no entries");

  /* lookup_key */
  key.keycode = 38;
  key.group = 0;
  key.level = 0;
  M6B_EQ_INT (gdk_keymap_lookup_key (keymap, &key), GDK_a, "lookup 38/0/0");
  key.level = 1;
  M6B_EQ_INT (gdk_keymap_lookup_key (keymap, &key), GDK_A, "lookup 38/0/1");
  key.group = 5;
  key.level = 0;
  M6B_EQ_INT (gdk_keymap_lookup_key (keymap, &key), 0, "lookup 38/5/0");

  /* virtual modifiers.  Meta shares Mod1 with Alt only when the Alt key
   * carries a Meta keysym (a stock pc105 map does), so ask the keymap. */
  state = GDK_META_MASK;
  gdk_keymap_map_virtual_modifiers (keymap, &state);
  alt_is_meta = (state & GDK_MOD1_MASK) != 0;

  state = GDK_MOD4_MASK;
  gdk_keymap_add_virtual_modifiers (keymap, &state);
  M6B_CHECK ((state & GDK_SUPER_MASK) && (state & GDK_MOD4_MASK),
	     "add_virtual_modifiers: Mod4 gains Super");
  state = GDK_SHIFT_MASK;
  gdk_keymap_add_virtual_modifiers (keymap, &state);
  M6B_EQ_INT (state, GDK_SHIFT_MASK, "add_virtual_modifiers: Shift unchanged");
  state = GDK_MOD1_MASK;
  gdk_keymap_add_virtual_modifiers (keymap, &state);
  M6B_CHECK (((state & GDK_META_MASK) != 0) == alt_is_meta,
	     "add_virtual_modifiers: Mod1 gains Meta only if Alt is Meta");
  M6B_CHECK (state & GDK_MOD1_MASK, "add_virtual_modifiers keeps Mod1");

  state = GDK_SUPER_MASK;
  M6B_CHECK (gdk_keymap_map_virtual_modifiers (keymap, &state),
	     "map_virtual_modifiers: Super maps");
  M6B_CHECK (state & GDK_MOD4_MASK, "map_virtual_modifiers: Super gains Mod4");
  state = GDK_SUPER_MASK | GDK_MOD4_MASK;
  M6B_CHECK (!gdk_keymap_map_virtual_modifiers (keymap, &state),
	     "map_virtual_modifiers: Super with Mod4 set returns FALSE");

  /* names */
  M6B_EQ_STR (gdk_keyval_name (GDK_a), "a", "name of a");
  M6B_EQ_STR (gdk_keyval_name (GDK_Return), "Return", "name of Return");
  M6B_EQ_STR (gdk_keyval_name (GDK_Page_Up), "Page_Up", "name of Page_Up");
  M6B_CHECK (gdk_keyval_name (0) == NULL, "name of 0 is NULL");
  M6B_EQ_STR (gdk_keyval_name (0x10020AC), "U20AC", "name of U+20AC");
  M6B_EQ_STR (gdk_keyval_name (0x1000041), "U0041",
	      "name of the low unicode range, same as X11");
  M6B_EQ_INT (gdk_keyval_from_name ("a"), GDK_a, "from_name a");
  M6B_EQ_INT (gdk_keyval_from_name ("Return"), GDK_Return, "from_name Return");
  M6B_EQ_INT (gdk_keyval_from_name ("Page_Up"), GDK_Page_Up,
	      "from_name Page_Up");
  M6B_EQ_INT (gdk_keyval_from_name ("U20AC"), 0x10020AC, "from_name U20AC");
  M6B_EQ_INT (gdk_keyval_from_name ("NoSuchKeysym"), 0,
	      "from_name of an unknown name is 0, same as X11");

  /* case conversion */
  gdk_keyval_convert_case (GDK_a, &lower, &upper);
  M6B_CHECK (lower == GDK_a && upper == GDK_A, "convert_case a");
  gdk_keyval_convert_case (GDK_eacute, &lower, &upper);
  M6B_CHECK (lower == GDK_eacute && upper == GDK_Eacute, "convert_case eacute");
  gdk_keyval_convert_case (GDK_Greek_DELTA, &lower, &upper);
  M6B_CHECK (lower == GDK_Greek_delta && upper == GDK_Greek_DELTA,
	     "convert_case Greek_DELTA");
  gdk_keyval_convert_case (GDK_ssharp, &lower, &upper);
  M6B_EQ_INT (lower, GDK_ssharp, "ssharp lower is ssharp");
  gdk_keyval_convert_case (0x1000394, &lower, &upper);
  /* same as the X11 backend: through gdk_unicode_to_keyval, which prefers
   * the legacy keysym (Greek_delta) when there is one */
  M6B_EQ_INT (lower, gdk_unicode_to_keyval (0x3b4), "U+0394 lower is U+03B4");
  M6B_CHECK (gdk_keyval_is_upper (GDK_A) && !gdk_keyval_is_upper (GDK_a),
	     "keyval_is_upper agrees");
  M6B_CHECK (gdk_keyval_is_lower (GDK_a) && !gdk_keyval_is_lower (GDK_A),
	     "keyval_is_lower agrees");
  M6B_EQ_INT (gdk_keyval_to_upper (GDK_a), GDK_A, "keyval_to_upper agrees");
  M6B_EQ_INT (gdk_keyval_to_lower (GDK_A), GDK_a, "keyval_to_lower agrees");

  /* two layouts: latin and hebrew.  Bidi and direction follow the group. */
  M6B_HOOK (_gdk_wayland_test_keyboard_keymap_text (d, m6b_bidi_keymap),
	    "two layout keymap");
  m6_drain ();
  keymap = gdk_keymap_get_for_display (d);
  M6B_CHECK (gdk_keymap_have_bidi_layouts (keymap),
	     "two layout keymap has a bidi layout");
  M6B_HOOK (_gdk_wayland_test_keyboard_state (d, 0, 0, 1), "group 1");
  M6B_EQ_INT (gdk_keymap_get_direction (keymap), PANGO_DIRECTION_RTL,
	      "direction is RTL in the hebrew group");
  M6B_HOOK (_gdk_wayland_test_keyboard_state (d, 0, 0, 0), "group 0");
  M6B_EQ_INT (gdk_keymap_get_direction (keymap), PANGO_DIRECTION_LTR,
	      "direction is LTR in the latin group");
  /* a one layout key still answers for the second group, no crash */
  key.keycode = 50;
  key.group = 1;
  key.level = 0;
  (void) gdk_keymap_lookup_key (keymap, &key);

  /* back to the default keymap for the rest of the demo */
  M6B_HOOK (_gdk_wayland_test_keyboard_keymap (d), "restore keymap");
  m6_drain ();
  M6B_HOOK (_gdk_wayland_test_keyboard_state (d, 0, 0, 0), "restore state");
  m6_clear ();
}

/* ---- display --------------------------------------------------------- */

static int
m6b_display_wait_status (GPid pid)
{
  int status = -1;

  if (waitpid (pid, &status, 0) < 0)
    return -1;
  return WIFEXITED (status) ? WEXITSTATUS (status) : -1;
}

static void
m6b_display (GdkDisplay *d)
{
  GdkScreen *screen = gdk_screen_get_default ();
  const gchar *env = g_getenv ("WAYLAND_DISPLAY");
  gchar *name;
  GdkWindow *grp, *root;
  GdkEvent *cev;
  GList *devs;
  GdkDevice *core;
  GdkTimeCoord **events = (GdkTimeCoord **) 0x1;
  gint n_events = 5;
  gdouble axes[2] = { -1, -1 };
  GdkModifierType mask = 0;
  GdkAtom atom = gdk_atom_intern_static_string ("YDK_M6B_TEST");

  /* name */
  if (env != NULL)
    M6B_EQ_STR (gdk_display_get_name (d), env, "display name is WAYLAND_DISPLAY");
  name = gdk_get_display ();
  M6B_EQ_STR (name, gdk_display_get_name (d), "gdk_get_display equals the name");
  g_free (name);

  /* capabilities */
  M6B_CHECK (gdk_display_supports_shapes (d), "supports_shapes TRUE (M7)");
  M6B_CHECK (gdk_display_supports_input_shapes (d), "supports_input_shapes TRUE (M7)");
  M6B_CHECK (!gdk_display_supports_composite (d), "supports_composite FALSE");
  M6B_CHECK (!gdk_display_supports_selection_notification (d),
	     "supports_selection_notification FALSE");
  M6B_CHECK (!gdk_display_supports_clipboard_persistence (d),
	     "supports_clipboard_persistence FALSE");
  M6B_CHECK (!gdk_display_request_selection_notification
	     (d, gdk_atom_intern_static_string ("CLIPBOARD")),
	     "request_selection_notification FALSE");
  gdk_display_store_clipboard (d, main_window, GDK_CURRENT_TIME, NULL, 0);
  gdk_notify_startup_complete ();
  gdk_notify_startup_complete_with_id ("x");
  M6B_CHECK (TRUE, "startup and clipboard calls returned");

  /* default group */
  grp = gdk_display_get_default_group (d);
  root = gdk_screen_get_root_window (screen);
  M6B_CHECK (grp != NULL && GDK_IS_WINDOW (grp), "default group is a window");
  M6B_CHECK (grp == root, "default group is the root window");
  M6B_CHECK (gdk_display_get_default_group (d) == grp, "default group is stable");

  /* client messages */
  gdk_add_client_message_filter (atom, m6b_dummy_filter, NULL);
  gdk_display_add_client_message_filter (d, atom, m6b_dummy_filter, NULL);
  cev = gdk_event_new (GDK_CLIENT_EVENT);
  cev->client.window = g_object_ref (main_window);
  cev->client.message_type = atom;
  cev->client.data_format = 32;
  M6B_CHECK (!gdk_event_send_client_message_for_display (d, cev, 1),
	     "send_client_message_for_display returns FALSE");
  gdk_event_free (cev);
  M6B_CHECK (gdk_event_get_graphics_expose (main_window) == NULL,
	     "no graphics expose event");

  /* devices */
  core = gdk_display_get_core_pointer (d);
  devs = gdk_display_list_devices (d);
  M6B_CHECK (devs != NULL, "display device list is not empty");
  M6B_CHECK (devs != NULL && devs->data == core, "first device is the core pointer");
  M6B_CHECK (gdk_devices_list () != NULL && gdk_devices_list ()->data == core,
	     "gdk_devices_list has the same first device");
  if (core != NULL)
    {
      M6B_EQ_STR (gdk_device_get_name (core), "Core Pointer", "device name");
      M6B_EQ_INT (core->source, GDK_SOURCE_MOUSE, "device source");
      M6B_EQ_INT (core->mode, GDK_MODE_SCREEN, "device mode");
      M6B_CHECK (core->has_cursor, "device has a cursor");
      M6B_EQ_INT (core->num_axes, 2, "device axes");
      M6B_CHECK (!gdk_device_get_history (core, main_window, 0, 0, &events,
					  &n_events),
		 "device history unsupported");
      M6B_EQ_INT (n_events, 0, "device history has no events");
    }
  gdk_input_set_extension_events (main_window, GDK_POINTER_MOTION_MASK,
				  GDK_EXTENSION_EVENTS_NONE);
  gdk_input_set_extension_events (main_window, GDK_POINTER_MOTION_MASK,
				  GDK_EXTENSION_EVENTS_ALL);
  gdk_input_set_extension_events (main_window, GDK_POINTER_MOTION_MASK,
				  GDK_EXTENSION_EVENTS_CURSOR);
  M6B_HOOK (_gdk_wayland_test_pointer_enter (d, main_window, 50, 60),
	    "enter for device state");
  m6_drain ();
  if (core != NULL)
    {
      gdk_device_get_state (core, main_window, axes, &mask);
      M6B_CHECK (axes[0] == 50 && axes[1] == 60, "device state reports (50,60)");
    }
  _gdk_wayland_test_pointer_leave (d, main_window);
  m6_drain ();
  m6_clear ();

  /* spawn */
  {
    GError *err = NULL;
    GPid pid = 0;
    gchar *argv_env[] = { "sh", "-c",
      "test \"$WAYLAND_DISPLAY\" = \"$YDK_M6B_EXPECT\" && "
      "test \"${DISPLAY-}\" = \"$YDK_M6B_ORIG\"", NULL };
    gchar *argv_ok[] = { "sh", "-c", "printf ok", NULL };
    gint out_fd = -1;
    gchar buf[16] = { 0 };
    gssize got;

    g_setenv ("YDK_M6B_EXPECT", gdk_display_get_name (d), TRUE);
    g_setenv ("YDK_M6B_ORIG", g_getenv ("DISPLAY") ? g_getenv ("DISPLAY") : "",
	      TRUE);

    M6B_CHECK (gdk_spawn_on_screen (screen, NULL, argv_env, NULL,
				    G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
				    NULL, NULL, &pid, &err),
	       "spawn_on_screen");
    if (err)
      {
	g_printerr ("ydk_demo: spawn error: %s\n", err->message);
	g_clear_error (&err);
      }
    else
      M6B_EQ_INT (m6b_display_wait_status (pid), 0,
		  "spawned child sees WAYLAND_DISPLAY and an untouched DISPLAY");

    pid = 0;
    M6B_CHECK (gdk_spawn_on_screen_with_pipes (screen, NULL, argv_ok, NULL,
					       G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
					       NULL, NULL, &pid, NULL, &out_fd,
					       NULL, &err),
	       "spawn_on_screen_with_pipes");
    if (err)
      g_clear_error (&err);
    else
      {
	got = read (out_fd, buf, sizeof buf - 1);
	M6B_CHECK (got == 2 && strcmp (buf, "ok") == 0, "pipe child printed ok");
	close (out_fd);
	m6b_display_wait_status (pid);
      }

    M6B_CHECK (gdk_spawn_command_line_on_screen (screen, "true", &err),
	       "spawn_command_line_on_screen");
    g_clear_error (&err);
    /* a screen that is not a screen falls back to the default one */
    M6B_CHECK (gdk_spawn_command_line_on_screen (NULL, "true", &err),
	       "spawn_command_line_on_screen with a NULL screen");
    g_clear_error (&err);
    M6B_CHECK (!gdk_spawn_command_line_on_screen (screen, "", &err) && err != NULL,
	       "empty command line fails with an error");
    g_clear_error (&err);
    M6B_CHECK (!gdk_spawn_command_line_on_screen (screen, NULL, &err) && err != NULL,
	       "NULL command line fails with an error");
    g_clear_error (&err);
    /* the reaped children above are the only ones; wait for stray ones */
    while (waitpid (-1, NULL, WNOHANG) > 0)
      ;
  }

  /* error traps: no X errors, unbalanced pops tolerated */
  gdk_error_trap_push ();
  gdk_error_trap_push ();
  M6B_EQ_INT (gdk_error_trap_pop (), 0, "error_trap_pop 1");
  M6B_EQ_INT (gdk_error_trap_pop (), 0, "error_trap_pop 2");
  M6B_EQ_INT (gdk_error_trap_pop (), 0, "unbalanced error_trap_pop is tolerated");
  M6B_CHECK (!gdk_get_use_xshm (), "no xshm");
  {
    GdkFont *f1 = gdk_fontset_load ("fixed");
    GdkFont *f2 = gdk_fontset_load_for_display (d, "fixed");

    M6B_CHECK ((f1 == NULL) == (f2 == NULL), "fontset_load matches the for_display call");
    if (f1 && f2)
      M6B_CHECK (gdk_font_equal (f1, f2), "fontset_load equals fontset_load_for_display");
    if (f1)
      gdk_font_unref (f1);
    if (f2)
      gdk_font_unref (f2);
  }
}

/* locale and multibyte run last: gdk_set_locale () changes the process
 * locale. */
static void
m6b_locale (void)
{
  GdkWChar wbuf[32];
  gchar *s;
  gint n;
  gboolean utf8;

  M6B_CHECK (gdk_set_locale () != NULL, "gdk_set_locale returns a name");
  utf8 = g_get_charset (NULL);

  memset (wbuf, 0, sizeof wbuf);
  n = gdk_mbstowcs (wbuf, "abc", 31);
  M6B_EQ_INT (n, 3, "mbstowcs abc");
  s = gdk_wcstombs (wbuf);
  M6B_EQ_STR (s, "abc", "wcstombs round trip");
  g_free (s);

  if (utf8)
    {
      const gchar *text = "h\xc3\xa9llo \xe2\x82\xac";

      memset (wbuf, 0, sizeof wbuf);
      n = gdk_mbstowcs (wbuf, text, 31);
      M6B_EQ_INT (n, g_utf8_strlen (text, -1), "mbstowcs utf-8 length");
      s = gdk_wcstombs (wbuf);
      M6B_EQ_STR (s, text, "wcstombs utf-8 round trip");
      g_free (s);
    }

  M6B_EQ_INT (gdk_mbstowcs (NULL, "abc", 4), -1, "mbstowcs NULL dest fails");
  M6B_EQ_INT (gdk_mbstowcs (wbuf, NULL, 4), -1, "mbstowcs NULL src fails");
  M6B_CHECK (gdk_wcstombs (NULL) == NULL, "wcstombs NULL fails");
}

/* ---- toplevel hints -------------------------------------------------- */

static void
m6b_hints_strings (GdkDisplay *d, GdkWindow *w3, GdkWindow *w4)
{
  GDKWTestToplevelInfo info;
  gchar *longtitle;
  GdkWindow *w5;

  /* before show on w4: stored, sent at show */
  gdk_window_set_title (w4, "Pre-show \xc3\xa9 title \xe2\x9c\x93");
  gdk_window_set_role (w4, "role-w4");
  gdk_window_set_startup_id (w4, "startup-w4");
  gdk_window_set_icon_name (w4, "icon-w4");
  gdk_window_show (w4);
  gdk_display_sync (d);
  M6B_CHECK (m6b_info (w4, &info), "w4 has a toplevel after show");
  M6B_CHECK (info.has_title, "w4 title sent");
  M6B_EQ_STR (info.title, "Pre-show \xc3\xa9 title \xe2\x9c\x93", "w4 title");
  M6B_CHECK (info.has_role, "w4 role recorded");
  M6B_EQ_STR (info.role, "role-w4", "w4 role");

  /* after show on w3 */
  gdk_window_set_title (w3, "After show \xc3\x9c" "n\xc3\xaf" "code");
  M6B_CHECK (m6b_info (w3, &info), "w3 info");
  M6B_EQ_STR (info.title, "After show \xc3\x9c" "n\xc3\xaf" "code", "w3 title");

  /* invalid UTF-8 is repaired, not sent raw */
  gdk_window_set_title (w3, "bad \xff\xfe title");
  M6B_CHECK (m6b_info (w3, &info) && g_utf8_validate (info.title, -1, NULL),
	     "invalid UTF-8 title is made valid");

  /* a very long title must not exceed the Wayland message size */
  longtitle = g_strnfill (5000, 'a');
  gdk_window_set_title (w3, longtitle);
  g_free (longtitle);
  gdk_display_sync (d);
  M6B_CHECK (m6b_info (w3, &info), "w3 survives a 5000 byte title");
  gdk_window_set_title (w3, "w3");

  /* app id: from the wmclass class, else the program class */
  M6B_CHECK (m6b_info (main_window, &info), "main window info");
  M6B_EQ_STR (info.app_id, "YdkDemo", "main window app id is its wmclass class");
  gdk_set_program_class ("YdkDemoClass");
  w5 = m6b_toplevel (50, 50, 0, NULL, NULL);
  M6B_CHECK (w5 != NULL, "window without wmclass");
  if (w5)
    {
      gdk_window_show (w5);
      M6B_CHECK (m6b_info (w5, &info), "w5 info");
      M6B_EQ_STR (info.app_id, "YdkDemoClass", "app id falls back to the program class");
      gdk_window_destroy (w5);
    }
}

static void
m6b_hints_transient (GdkDisplay *d, GdkWindow *w3)
{
  GDKWTestToplevelInfo info;
  GdkWindow *w6;

  gdk_window_set_transient_for (w3, main_window);
  M6B_CHECK (m6b_info (w3, &info) && info.transient_for == main_window,
	     "transient_for main window");
  gdk_window_set_transient_for (w3, NULL);
  M6B_CHECK (m6b_info (w3, &info) && info.transient_for == NULL,
	     "NULL clears the transient parent");

  gdk_window_set_transient_for (w3, w3);
  M6B_CHECK (m6b_info (w3, &info) && info.transient_for == NULL,
	     "a window is not its own parent");

  /* a cycle: main -> w3 is fine, w3 -> main must be refused */
  gdk_window_set_transient_for (main_window, w3);
  gdk_window_set_transient_for (w3, main_window);
  M6B_CHECK (m6b_info (w3, &info) && info.transient_for == NULL,
	     "a transient cycle is refused");
  gdk_window_set_transient_for (main_window, NULL);

  /* a parent that is not shown yet is stored and sent after both map */
  w6 = m6b_toplevel (60, 60, 0, NULL, NULL);
  M6B_CHECK (w6 != NULL, "unshown parent");
  if (w6)
    {
      gdk_window_set_transient_for (w3, w6);
      M6B_CHECK (m6b_info (w3, &info) && info.transient_for == w6,
		 "unshown parent is stored");
      gdk_window_show (w6);
      gdk_display_sync (d);
      gdk_display_sync (d);
      M6B_CHECK (m6b_info (w3, &info) && info.transient_for == w6,
		 "parent still set after the parent maps");
      gdk_window_set_transient_for (w3, NULL);
      gdk_window_destroy (w6);
    }

  /* a parent of the wrong kind is ignored */
  gdk_window_set_transient_for (w3, gdk_get_default_root_window ());
  M6B_CHECK (m6b_info (w3, &info) && info.transient_for == NULL,
	     "the root window is not a transient parent");
}

static void
m6b_hints_flags (GdkDisplay *d, GdkWindow *w3, GdkWindow *w4)
{
  GdkWMDecoration decor = 0;
  GdkWindow *win[2];
  GdkWindowAttr attr;
  GdkWindow *child;
  int i;

  M6B_EQ_INT (gdk_window_get_type_hint (w3), GDK_WINDOW_TYPE_HINT_NORMAL,
	      "default type hint is NORMAL");
  gdk_window_set_type_hint (w3, GDK_WINDOW_TYPE_HINT_DIALOG);
  M6B_EQ_INT (gdk_window_get_type_hint (w3), GDK_WINDOW_TYPE_HINT_DIALOG,
	      "type hint round trip");
  gdk_window_set_type_hint (w3, GDK_WINDOW_TYPE_HINT_NORMAL);

  M6B_CHECK (!gdk_window_get_decorations (w3, &decor),
	     "no decorations before set_decorations");
  gdk_window_set_decorations (w3, GDK_DECOR_BORDER | GDK_DECOR_TITLE);
  M6B_CHECK (gdk_window_get_decorations (w3, &decor), "decorations after set");
  M6B_EQ_INT (decor, GDK_DECOR_BORDER | GDK_DECOR_TITLE, "decorations value");

  /* shown and unshown, they must all be accepted silently */
  win[0] = w3;
  win[1] = w4;
  for (i = 0; i < 2; i++)
    {
      gdk_window_set_modal_hint (win[i], TRUE);
      gdk_window_set_modal_hint (win[i], FALSE);
      gdk_window_set_skip_taskbar_hint (win[i], TRUE);
      gdk_window_set_skip_pager_hint (win[i], TRUE);
      gdk_window_set_urgency_hint (win[i], TRUE);
      gdk_window_set_urgency_hint (win[i], FALSE);
      gdk_window_set_accept_focus (win[i], TRUE);
      gdk_window_set_focus_on_map (win[i], FALSE);
      gdk_window_set_override_redirect (win[i], FALSE);
      gdk_window_set_keep_above (win[i], TRUE);
      gdk_window_set_keep_above (win[i], FALSE);
      gdk_window_set_keep_below (win[i], TRUE);
      gdk_window_set_keep_below (win[i], FALSE);
      gdk_window_stick (win[i]);
      gdk_window_unstick (win[i]);
      gdk_window_set_functions (win[i], GDK_FUNC_ALL);
      gdk_window_set_opacity (win[i], 0.5);
      gdk_window_set_opacity (win[i], 2.0);
      gdk_window_set_opacity (win[i], -1.0);
    }

  /* windows that do not fit: silent no-ops, never a critical */
  memset (&attr, 0, sizeof attr);
  attr.window_type = GDK_WINDOW_CHILD;
  attr.wclass = GDK_INPUT_OUTPUT;
  attr.width = 20;
  attr.height = 20;
  child = gdk_window_new (main_window, &attr, 0);
  if (child)
    {
      gdk_window_set_title (child, "child");
      gdk_window_set_transient_for (child, main_window);
      gdk_window_set_type_hint (child, GDK_WINDOW_TYPE_HINT_DIALOG);
      gdk_window_maximize (child);
      gdk_window_set_group (child, main_window);
      M6B_EQ_INT (gdk_window_get_type_hint (child), GDK_WINDOW_TYPE_HINT_NORMAL,
		  "a child window has the default type hint");
      gdk_window_destroy (child);
    }
  gdk_window_set_title (gdk_get_default_root_window (), "root");
  gdk_window_set_title (NULL, "nothing");
  gdk_window_set_type_hint (NULL, GDK_WINDOW_TYPE_HINT_DIALOG);
  M6B_EQ_INT (gdk_window_get_type_hint (NULL), GDK_WINDOW_TYPE_HINT_NORMAL,
	      "type hint of NULL");
  gdk_window_maximize (NULL);
  gdk_window_iconify (NULL);
  gdk_window_focus (NULL, GDK_CURRENT_TIME);
  M6B_CHECK (TRUE, "hint calls on unfit windows returned");
  (void) d;
}

static void
m6b_hints_geometry (GdkWindow *w3)
{
  GDKWTestToplevelInfo info;
  GdkGeometry g;

  memset (&g, 0, sizeof g);
  g.min_width = 100;
  g.min_height = 80;
  g.max_width = 400;
  g.max_height = 300;
  gdk_window_set_geometry_hints (w3, &g, GDK_HINT_MIN_SIZE | GDK_HINT_MAX_SIZE);
  M6B_CHECK (m6b_info (w3, &info), "geometry info");
  M6B_EQ_INT (info.min_width, 100, "min_width");
  M6B_EQ_INT (info.min_height, 80, "min_height");
  M6B_EQ_INT (info.max_width, 400, "max_width");
  M6B_EQ_INT (info.max_height, 300, "max_height");

  /* a min above the max is clamped */
  g.min_width = 500;
  g.min_height = 500;
  g.max_width = 300;
  g.max_height = 300;
  gdk_window_set_geometry_hints (w3, &g, GDK_HINT_MIN_SIZE | GDK_HINT_MAX_SIZE);
  m6b_info (w3, &info);
  M6B_EQ_INT (info.min_width, 300, "min clamped to the max (width)");
  M6B_EQ_INT (info.min_height, 300, "min clamped to the max (height)");

  /* the clamp is not permanent: a larger max restores the requested min */
  g.max_width = 800;
  g.max_height = 800;
  gdk_window_set_geometry_hints (w3, &g, GDK_HINT_MAX_SIZE);
  m6b_info (w3, &info);
  M6B_EQ_INT (info.min_width, 500, "a larger max restores the requested min");
  M6B_EQ_INT (info.max_width, 800, "max after restore");

  /* a mask with only MAX_SIZE keeps the old min */
  g.min_width = 100;
  g.min_height = 80;
  gdk_window_set_geometry_hints (w3, &g, GDK_HINT_MIN_SIZE);
  g.max_width = 400;
  g.max_height = 300;
  g.min_width = 1;
  g.min_height = 1;
  gdk_window_set_geometry_hints (w3, &g, GDK_HINT_MAX_SIZE);
  m6b_info (w3, &info);
  M6B_EQ_INT (info.min_width, 100, "MAX_SIZE only keeps the min width");
  M6B_EQ_INT (info.min_height, 80, "MAX_SIZE only keeps the min height");
  M6B_EQ_INT (info.max_width, 400, "MAX_SIZE only sets the max width");

  /* the old API gives the same result */
  gdk_window_set_geometry_hints (w3, NULL, 0);
  gdk_window_set_hints (w3, 0, 0, 100, 80, 400, 300,
			GDK_HINT_MIN_SIZE | GDK_HINT_MAX_SIZE);
  m6b_info (w3, &info);
  M6B_CHECK (info.min_width == 100 && info.min_height == 80 &&
	     info.max_width == 400 && info.max_height == 300,
	     "set_hints gives the same four numbers");

  /* clear */
  gdk_window_set_geometry_hints (w3, NULL, 0);
  m6b_info (w3, &info);
  M6B_CHECK (info.min_width == 0 && info.min_height == 0 &&
	     info.max_width == 0 && info.max_height == 0,
	     "NULL geometry with mask 0 clears the hints");
}

static int
m6b_state_events (GdkWindow *w, GdkWindowState changed)
{
  GList *l;
  int n = 0;

  for (l = m6_events.head; l; l = l->next)
    {
      GdkEvent *ev = l->data;

      if (ev->type == GDK_WINDOW_STATE && ev->any.window == w &&
	  (ev->window_state.changed_mask & changed))
	n++;
    }

  return n;
}

/* Requests such as set_maximized go to the real compositor, which answers
 * with its own configure.  Let those answers arrive first, then put the
 * window in a known state with a synthetic configure, so that the checks
 * below see the effect of the hook alone. */
static void
m6b_settle (GdkDisplay *d, GdkWindow *w)
{
  gdk_display_sync (d);
  m6_drain ();
  _gdk_wayland_test_toplevel_configure (w, 0, 0, 0);
  m6_drain ();
  m6_clear ();
}

/* TRUE if a recorded window-state event for @w changed @flag and its new
 * state has (@set) or lacks (!@set) the flag */
static gboolean
m6b_state_event_with (GdkWindow *w, GdkWindowState flag, gboolean set)
{
  GList *l;

  for (l = m6_events.head; l; l = l->next)
    {
      GdkEvent *ev = l->data;

      if (ev->type == GDK_WINDOW_STATE && ev->any.window == w &&
	  (ev->window_state.changed_mask & flag) &&
	  ((ev->window_state.new_window_state & flag) != 0) == set)
	return TRUE;
    }

  return FALSE;
}

static void
m6b_hints_state (GdkDisplay *d, GdkWindow *w3)
{
  GDKWTestToplevelInfo info;
  GdkWindow *w7;
  gint gw = -1, gh = -1;

  m6_clear ();

  /* maximize */
  gdk_window_maximize (w3);
  M6B_CHECK (m6b_info (w3, &info) && info.maximized, "maximize recorded");
  m6b_settle (d, w3);
  M6B_HOOK (_gdk_wayland_test_toplevel_configure (w3, 0, 0,
						  GDKW_TEST_STATE_MAXIMIZED),
	    "configure maximized");
  m6_drain ();
  M6B_CHECK (gdk_window_get_state (w3) & GDK_WINDOW_STATE_MAXIMIZED,
	     "window state has MAXIMIZED");
  M6B_EQ_INT (m6b_state_events (w3, GDK_WINDOW_STATE_MAXIMIZED), 1,
	      "one window-state event for MAXIMIZED");
  M6B_CHECK (m6b_state_event_with (w3, GDK_WINDOW_STATE_MAXIMIZED, TRUE),
	     "the event carries the new state");
  /* the same configure again changes nothing */
  _gdk_wayland_test_toplevel_configure (w3, 0, 0, GDKW_TEST_STATE_MAXIMIZED);
  m6_drain ();
  M6B_EQ_INT (m6b_state_events (w3, GDK_WINDOW_STATE_MAXIMIZED), 1,
	      "a repeated configure sends no second event");
  gdk_window_unmaximize (w3);
  M6B_CHECK (m6b_info (w3, &info) && !info.maximized, "unmaximize recorded");
  gdk_display_sync (d);
  _gdk_wayland_test_toplevel_configure (w3, 0, 0, 0);
  m6_drain ();
  M6B_CHECK (!(gdk_window_get_state (w3) & GDK_WINDOW_STATE_MAXIMIZED),
	     "MAXIMIZED cleared by the next configure");
  M6B_CHECK (m6b_state_event_with (w3, GDK_WINDOW_STATE_MAXIMIZED, FALSE),
	     "an event clears MAXIMIZED");

  /* fullscreen */
  gdk_window_fullscreen (w3);
  M6B_CHECK (m6b_info (w3, &info) && info.fullscreen, "fullscreen recorded");
  m6b_settle (d, w3);
  _gdk_wayland_test_toplevel_configure (w3, 0, 0, GDKW_TEST_STATE_FULLSCREEN);
  m6_drain ();
  M6B_CHECK (gdk_window_get_state (w3) & GDK_WINDOW_STATE_FULLSCREEN,
	     "window state has FULLSCREEN");
  M6B_EQ_INT (m6b_state_events (w3, GDK_WINDOW_STATE_FULLSCREEN), 1,
	      "one window-state event for FULLSCREEN");
  gdk_window_unfullscreen (w3);
  M6B_CHECK (m6b_info (w3, &info) && !info.fullscreen, "unfullscreen recorded");
  gdk_display_sync (d);
  _gdk_wayland_test_toplevel_configure (w3, 0, 0, 0);
  m6_drain ();
  M6B_CHECK (!(gdk_window_get_state (w3) & GDK_WINDOW_STATE_FULLSCREEN),
	     "FULLSCREEN cleared");
  M6B_CHECK (m6b_state_event_with (w3, GDK_WINDOW_STATE_FULLSCREEN, FALSE),
	     "an event clears FULLSCREEN");

  /* ACTIVATED has no GDK2 window state: it is recorded, no event */
  m6b_settle (d, w3);
  _gdk_wayland_test_toplevel_configure (w3, 0, 0, GDKW_TEST_STATE_ACTIVATED);
  m6_drain ();
  M6B_CHECK (m6b_info (w3, &info) &&
	     (info.configure_states & GDKW_TEST_STATE_ACTIVATED),
	     "ACTIVATED recorded");
  M6B_EQ_INT (m6_count (GDK_WINDOW_STATE, w3), 0, "ACTIVATED sends no state event");
  _gdk_wayland_test_toplevel_configure (w3, 0, 0, 0);
  M6B_CHECK (m6b_info (w3, &info) &&
	     !(info.configure_states & GDKW_TEST_STATE_ACTIVATED),
	     "ACTIVATED cleared");

  /* a configure with a size resizes the window */
  _gdk_wayland_test_toplevel_configure (w3, 220, 170, 0);
  m6_drain ();
  gdk_window_get_geometry (w3, NULL, NULL, &gw, &gh, NULL);
  M6B_EQ_INT (gw, 220, "configure size, width");
  M6B_EQ_INT (gh, 170, "configure size, height");

  /* iconify: one request, no ICONIFIED state */
  gdk_window_iconify (w3);
  M6B_CHECK (m6b_info (w3, &info) && info.minimize_requests == 1,
	     "one minimize request");
  M6B_CHECK (!(gdk_window_get_state (w3) & GDK_WINDOW_STATE_ICONIFIED),
	     "ICONIFIED is never set");

  /* focus on a mapped window: nothing happens, nothing breaks */
  gdk_window_focus (w3, GDK_CURRENT_TIME);

  /* an iconify before the first show is applied at show */
  w7 = m6b_toplevel (40, 40, GDK_STRUCTURE_MASK, NULL, NULL);
  if (w7)
    {
      gdk_window_iconify (w7);
      gdk_window_show (w7);
      M6B_CHECK (m6b_info (w7, &info) && info.minimize_requests == 1,
		 "iconify before show is sent at show");
      /* deiconify on a hidden window maps it */
      gdk_window_hide (w7);
      M6B_CHECK (!gdk_window_is_visible (w7), "w7 hidden");
      gdk_window_deiconify (w7);
      M6B_CHECK (gdk_window_is_visible (w7), "deiconify maps a hidden window");
      gdk_window_destroy (w7);
    }
  gdk_display_sync (d);
  m6_clear ();
}

static void
m6b_hints_drags (GdkDisplay *d, GdkWindow *w3)
{
  static const GdkWindowEdge edges[8] = {
    GDK_WINDOW_EDGE_NORTH_WEST, GDK_WINDOW_EDGE_NORTH,
    GDK_WINDOW_EDGE_NORTH_EAST, GDK_WINDOW_EDGE_WEST,
    GDK_WINDOW_EDGE_EAST, GDK_WINDOW_EDGE_SOUTH_WEST,
    GDK_WINDOW_EDGE_SOUTH, GDK_WINDOW_EDGE_SOUTH_EAST
  };
  static const guint xdg_edges[8] = { 5, 1, 9, 4, 8, 6, 2, 10 };
  GDKWTestToplevelInfo info;
  guint before;
  guint32 t = 70000;
  int i;

  M6B_HOOK (_gdk_wayland_test_pointer_enter (d, w3, 20, 20), "enter w3");
  m6_drain ();

  /* no button held: the request is dropped */
  m6b_info (w3, &info);
  before = info.move_requests;
  gdk_window_begin_move_drag (w3, 1, 20, 20, GDK_CURRENT_TIME);
  m6b_info (w3, &info);
  M6B_EQ_INT (info.move_requests, before, "move drag without a button is dropped");

  M6B_HOOK (_gdk_wayland_test_pointer_button (d, t, GDKW_TEST_BTN_LEFT, TRUE),
	    "press on w3");
  m6_drain ();
  gdk_window_begin_move_drag (w3, 1, 20, 20, GDK_CURRENT_TIME);
  m6b_info (w3, &info);
  M6B_EQ_INT (info.move_requests, before + 1, "one move request");
  M6B_CHECK (info.drag_serial != 0, "the move uses a non zero serial");

  /* each edge, in order */
  for (i = 0; i < 8; i++)
    {
      m6b_info (w3, &info);
      before = info.resize_requests;
      gdk_window_begin_resize_drag (w3, edges[i], 1, 20, 20, GDK_CURRENT_TIME);
      m6b_info (w3, &info);
      M6B_EQ_INT (info.resize_requests, before + 1, "one resize request per edge");
      M6B_EQ_INT (info.last_resize_edge, xdg_edges[i], "resize edge value");
    }

  M6B_HOOK (_gdk_wayland_test_pointer_button (d, t + 50, GDKW_TEST_BTN_LEFT, FALSE),
	    "release on w3");
  m6_drain ();

  /* after the release the old serial is no longer valid */
  m6b_info (w3, &info);
  before = info.move_requests;
  gdk_window_begin_move_drag (w3, 1, 20, 20, GDK_CURRENT_TIME);
  m6b_info (w3, &info);
  M6B_EQ_INT (info.move_requests, before, "move drag after the release is dropped");

  _gdk_wayland_test_pointer_leave (d, w3);
  m6_drain ();
  m6_clear ();
}

static void
m6b_hints_misc (GdkDisplay *d, GdkWindow *w3, GdkWindow *w4)
{
  GdkRectangle rect;
  GdkWindowAttr attr;
  GdkWindow *child, *leader;
  GdkPixbuf *pb;
  GList *icons;
  gint x = -1, y = -1, gw = -1, gh = -1;

  /* geometry queries */
  gdk_window_get_root_origin (w3, &x, &y);
  M6B_CHECK (x == 0 && y == 0, "root origin is (0,0)");
  gdk_window_get_frame_extents (w3, &rect);
  gdk_window_get_geometry (w3, NULL, NULL, &gw, &gh, NULL);
  M6B_CHECK (rect.x == 0 && rect.y == 0 && rect.width == gw && rect.height == gh,
	     "frame extents equal the window geometry");
  memset (&attr, 0, sizeof attr);
  attr.window_type = GDK_WINDOW_CHILD;
  attr.wclass = GDK_INPUT_OUTPUT;
  attr.width = 30;
  attr.height = 30;
  child = gdk_window_new (main_window, &attr, 0);
  if (child)
    {
      gdk_window_get_frame_extents (child, &rect);
      gdk_window_get_geometry (main_window, NULL, NULL, &gw, &gh, NULL);
      M6B_CHECK (rect.width == gw && rect.height == gh,
		 "frame extents of a child are the toplevel's");
      gdk_window_destroy (child);
    }

  /* group */
  gdk_window_set_group (w3, main_window);
  M6B_CHECK (gdk_window_get_group (w3) == main_window, "group is main window");
  gdk_window_set_group (w3, NULL);
  M6B_CHECK (gdk_window_get_group (w3) == NULL, "NULL group clears");
  leader = m6b_toplevel (30, 30, 0, NULL, NULL);
  if (leader)
    {
      gdk_window_set_group (w3, leader);
      M6B_CHECK (gdk_window_get_group (w3) == leader, "group is the leader");
      gdk_window_destroy (leader);
      m6_drain ();
      m6_clear ();
      M6B_CHECK (gdk_window_get_group (w3) == NULL,
		 "group is NULL after the leader is gone");
    }

  /* foreign windows are not supported */
  M6B_CHECK (gdk_window_lookup (1) == NULL, "lookup of an xid");
  M6B_CHECK (gdk_window_lookup_for_display (d, 1) == NULL, "lookup_for_display");
  M6B_CHECK (gdk_window_foreign_new_for_display (d, 1) == NULL,
	     "foreign_new_for_display");
  M6B_CHECK (gdk_window_foreign_new (1) == NULL, "foreign_new");

  /* destroy_notify destroys a window and drops one reference */
  g_object_ref (w4);
  gdk_window_destroy_notify (w4);
  M6B_CHECK (DEMO_WINDOW_DESTROYED (w4), "destroy_notify destroys the window");
  g_object_unref (w4);

  /* nothing to do, nothing to break */
  gdk_window_enable_synchronized_configure (w3);
  gdk_window_configure_finished (w3);

  /* icons are accepted */
  pb = gdk_pixbuf_new (GDK_COLORSPACE_RGB, TRUE, 8, 16, 16);
  icons = g_list_prepend (NULL, pb);
  gdk_window_set_icon_list (w3, icons);
  g_list_free (icons);
  g_object_unref (pb);
  gdk_window_set_icon (w3, NULL, NULL, NULL);
  gdk_window_set_icon_name (w3, "ydk-demo");
}

static void
m6b_hints (GdkDisplay *d)
{
  GdkWindow *w3, *w4;

  w3 = m6b_toplevel (200, 150, GDK_STRUCTURE_MASK | GDK_EXPOSURE_MASK |
		     GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
		     GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK |
		     GDK_POINTER_MOTION_MASK, "m6b_w3", "M6bW3");
  w4 = m6b_toplevel (100, 100, GDK_STRUCTURE_MASK, "m6b_w4", "M6bW4");
  M6B_CHECK (w3 != NULL && w4 != NULL, "hint test windows created");
  if (w3 == NULL || w4 == NULL)
    return;

  gdk_window_show (w3);
  gdk_display_sync (d);
  gdk_display_sync (d);
  m6_clear ();

  m6b_hints_strings (d, w3, w4);
  m6b_hints_transient (d, w3);
  m6b_hints_flags (d, w3, w4);
  m6b_hints_geometry (w3);
  m6b_hints_state (d, w3);
  m6b_hints_drags (d, w3);
  m6b_hints_misc (d, w3, w4);

  gdk_window_destroy (w3);
  /* w4 was destroyed by destroy_notify; the creation reference is gone with
   * the destroy, nothing is left to release */
  m6_drain ();
  m6_clear ();
}

/* ---- X11 handle accessors and X11 only API guards -------------------
 *
 * The demo is built without Xlib, so these are declared here instead of
 * coming from <ydk/gdkx.h>.  The declarations match the ABI of the real
 * ones (Display * is a pointer, XID is an unsigned long).
 *
 * On a Wayland display there is no X server: the handle accessors give NULL
 * and 0 for every kind of argument, without a CRITICAL (CRITICALs are fatal
 * in this demo, so a stray one fails the run).  The user time functions use
 * the time of the newest seat event. */
extern gpointer gdk_x11_display_get_xdisplay (GdkDisplay *display);
extern gpointer gdk_x11_drawable_get_xdisplay (GdkDrawable *drawable);
extern gulong gdk_x11_drawable_get_xid (GdkDrawable *drawable);
extern GdkDrawable *gdk_x11_window_get_drawable_impl (GdkWindow *window);
extern gboolean gdk_x11_display_is_x11 (GdkDisplay *display);
extern void gdk_x11_display_set_cursor_theme (GdkDisplay *display,
					      const gchar *theme,
					      const gint size);
extern void gdk_x11_window_set_user_time (GdkWindow *window, guint32 timestamp);
extern guint32 gdk_x11_display_get_user_time (GdkDisplay *display);
extern guint32 gdk_x11_get_server_time (GdkWindow *window);

static void
m6b_x11_handles (GdkDisplay *d, GdkWindow *child, GdkWindow *unmapped,
		 GdkPixmap *pix)
{
  GdkWindow *root = gdk_get_default_root_window ();
  GdkWindow *wins[4];
  const gchar *names[4] = { "main", "child", "unmapped", "root" };
  GdkDrawable *impl;
  gint i;

  wins[0] = main_window;
  wins[1] = child;
  wins[2] = unmapped;
  wins[3] = root;

  M6B_CHECK (!gdk_x11_display_is_x11 (d), "is_x11 is FALSE for a Wayland display");
  M6B_CHECK (!gdk_x11_display_is_x11 (NULL), "is_x11 is FALSE for NULL");
  M6B_CHECK (gdk_x11_display_get_xdisplay (d) == NULL, "xdisplay of the display is NULL");
  M6B_CHECK (gdk_x11_display_get_xdisplay (NULL) == NULL, "xdisplay of NULL display is NULL");

  for (i = 0; i < 4; i++)
    {
      gchar *what;

      what = g_strdup_printf ("%s window: xid is 0", names[i]);
      M6B_CHECK (gdk_x11_drawable_get_xid (GDK_DRAWABLE (wins[i])) == 0, what);
      g_free (what);
      what = g_strdup_printf ("%s window: drawable xdisplay is NULL", names[i]);
      M6B_CHECK (gdk_x11_drawable_get_xdisplay (GDK_DRAWABLE (wins[i])) == NULL, what);
      g_free (what);
    }

  /* GDK_WINDOW_XDISPLAY (win) is get_xdisplay (get_drawable_impl (win)) */
  for (i = 0; i < 3; i++)
    {
      gchar *what;

      impl = gdk_x11_window_get_drawable_impl (wins[i]);
      what = g_strdup_printf ("%s window: drawable impl is the window impl", names[i]);
      M6B_CHECK (impl != NULL && impl == GDK_WINDOW_OBJECT (wins[i])->impl, what);
      g_free (what);
      what = g_strdup_printf ("%s window: impl is a Wayland impl", names[i]);
      M6B_EQ_STR (impl ? G_OBJECT_TYPE_NAME (impl) : "(null)",
		  "GDKWWindowImplWayland", what);
      g_free (what);
      what = g_strdup_printf ("%s window: GDK_WINDOW_XDISPLAY is NULL", names[i]);
      M6B_CHECK (gdk_x11_drawable_get_xdisplay (impl) == NULL, what);
      g_free (what);
      what = g_strdup_printf ("%s window: xid of the impl is 0", names[i]);
      M6B_CHECK (gdk_x11_drawable_get_xid (impl) == 0, what);
      g_free (what);
    }

  /* a pixmap is not a window and has no X resource either */
  M6B_CHECK (gdk_x11_drawable_get_xid (GDK_DRAWABLE (pix)) == 0, "pixmap xid is 0");
  M6B_CHECK (gdk_x11_drawable_get_xdisplay (GDK_DRAWABLE (pix)) == NULL,
	     "pixmap xdisplay is NULL");
  M6B_CHECK (gdk_x11_window_get_drawable_impl ((GdkWindow *) pix) == NULL,
	     "drawable impl of a pixmap is NULL");

  M6B_CHECK (gdk_x11_drawable_get_xid (NULL) == 0, "xid of NULL is 0");
  M6B_CHECK (gdk_x11_drawable_get_xdisplay (NULL) == NULL, "xdisplay of NULL drawable is NULL");
  M6B_CHECK (gdk_x11_window_get_drawable_impl (NULL) == NULL, "drawable impl of NULL is NULL");
}

static void
m6b_x11_times (GdkDisplay *d, GdkWindow *child, GdkWindow *unmapped,
		GdkPixmap *pix)
{
  GdkWindow *root = gdk_get_default_root_window ();
  GdkWindow *wins[3];
  const gchar *names[3] = { "main", "child", "unmapped" };
  guint32 t1 = 91000, t2 = 91234;
  gint i;

  wins[0] = main_window;
  wins[1] = child;
  wins[2] = unmapped;

  /* no CRITICAL, no effect, for every kind of argument */
  gdk_x11_display_set_cursor_theme (d, "Adwaita", 24);
  gdk_x11_display_set_cursor_theme (d, NULL, 0);
  gdk_x11_display_set_cursor_theme (d, "", -1);
  gdk_x11_display_set_cursor_theme (NULL, "Adwaita", 24);
  M6B_CHECK (gdk_x11_display_get_xdisplay (d) == NULL,
	     "set_cursor_theme did not create an X display");

  /* the user time follows the newest seat event */
  M6B_HOOK (_gdk_wayland_test_pointer_enter (d, main_window, 20, 20), "x11 times enter");
  m6_drain ();
  M6B_HOOK (_gdk_wayland_test_pointer_motion (d, t1, 21, 21), "x11 times motion 1");
  m6_drain ();
  M6B_EQ_INT (gdk_x11_display_get_user_time (d), t1, "user time follows the seat");
  for (i = 0; i < 3; i++)
    {
      gchar *what = g_strdup_printf ("%s window: server time is the seat time", names[i]);

      M6B_EQ_INT (gdk_x11_get_server_time (wins[i]), t1, what);
      g_free (what);
    }

  M6B_HOOK (_gdk_wayland_test_pointer_motion (d, t2, 22, 22), "x11 times motion 2");
  m6_drain ();
  M6B_EQ_INT (gdk_x11_display_get_user_time (d), t2, "user time advances with the seat");
  M6B_EQ_INT (gdk_x11_get_server_time (main_window), t2, "server time advances too");

  /* set_user_time has no effect on Wayland */
  for (i = 0; i < 3; i++)
    gdk_x11_window_set_user_time (wins[i], 5);
  gdk_x11_window_set_user_time (root, 5);
  gdk_x11_window_set_user_time (NULL, 5);
  M6B_EQ_INT (gdk_x11_display_get_user_time (d), t2, "set_user_time does not change the user time");
  M6B_EQ_INT (gdk_x11_get_server_time (main_window), t2, "set_user_time does not change the server time");

  /* arguments without a time */
  M6B_EQ_INT (gdk_x11_display_get_user_time (NULL), 0, "user time of NULL display is 0");
  M6B_EQ_INT (gdk_x11_get_server_time (NULL), 0, "server time of NULL window is 0");
  M6B_EQ_INT (gdk_x11_get_server_time (root), t2,
	      "server time of the root window is the seat time");
  M6B_EQ_INT (gdk_x11_get_server_time ((GdkWindow *) pix), 0,
	      "server time of a pixmap is 0");

  M6B_HOOK (_gdk_wayland_test_pointer_leave (d, main_window), "x11 times leave");
  m6_drain ();
  m6_clear ();
}

/* X atom functions guarded in place in the X11 source.  Atom and XID are
 * unsigned long. */
extern gulong gdk_x11_atom_to_xatom (GdkAtom atom);
extern gulong gdk_x11_atom_to_xatom_for_display (GdkDisplay *display, GdkAtom atom);
extern GdkAtom gdk_x11_xatom_to_atom (gulong xatom);
extern GdkAtom gdk_x11_xatom_to_atom_for_display (GdkDisplay *display, gulong xatom);
extern gulong gdk_x11_get_xatom_by_name (const gchar *atom_name);
extern gulong gdk_x11_get_xatom_by_name_for_display (GdkDisplay *display, const gchar *atom_name);
extern const gchar *gdk_x11_get_xatom_name (gulong xatom);
extern const gchar *gdk_x11_get_xatom_name_for_display (GdkDisplay *display, gulong xatom);
extern GdkWindow *gdk_x11_window_foreign_new_for_display (GdkDisplay *display, gulong window);
extern void gdk_x11_free_compound_text (guchar *ctext);

static void
m6b_x11_atoms (GdkDisplay *d)
{
  GdkAtom atom = gdk_atom_intern_static_string ("YDK_M6B_X11_ATOM");
  GdkAtom enc;
  gint format, len;
  guchar *ctext = NULL;

  M6B_EQ_INT (gdk_x11_atom_to_xatom (atom), 0, "atom_to_xatom is None");
  M6B_EQ_INT (gdk_x11_atom_to_xatom_for_display (d, atom), 0,
	      "atom_to_xatom_for_display is None");
  M6B_EQ_INT (gdk_x11_atom_to_xatom (GDK_NONE), 0, "atom_to_xatom of GDK_NONE is None");
  M6B_CHECK (gdk_x11_xatom_to_atom (69) == GDK_NONE, "xatom_to_atom is GDK_NONE");
  M6B_CHECK (gdk_x11_xatom_to_atom_for_display (d, 69) == GDK_NONE,
	     "xatom_to_atom_for_display is GDK_NONE");
  M6B_EQ_INT (gdk_x11_get_xatom_by_name ("WM_NAME"), 0, "get_xatom_by_name is None");
  M6B_EQ_INT (gdk_x11_get_xatom_by_name_for_display (d, "WM_NAME"), 0,
	      "get_xatom_by_name_for_display is None");
  M6B_EQ_STR (gdk_x11_get_xatom_name (69), "", "get_xatom_name is empty, not NULL");
  M6B_EQ_STR (gdk_x11_get_xatom_name_for_display (d, 69), "",
	      "get_xatom_name_for_display is empty, not NULL");
  M6B_CHECK (gdk_x11_window_foreign_new_for_display (d, 0x400001) == NULL,
	     "foreign_new_for_display is NULL");

  /* a compound text buffer from the Wayland backend is released with g_free */
  M6B_CHECK (gdk_utf8_to_compound_text_for_display (d, "m6b", &enc, &format,
						    &ctext, &len) && ctext != NULL,
	     "compound text buffer for the free test");
  gdk_x11_free_compound_text (ctext);
  gdk_x11_free_compound_text (NULL);
}

static void
m6b_x11_api (GdkDisplay *d, GdkWindow *child)
{
  GdkWindow *unmapped;
  GdkPixmap *pix;

  unmapped = m6b_toplevel (64, 48, GDK_STRUCTURE_MASK, NULL, NULL);
  pix = gdk_pixmap_new (main_window, 8, 8, -1);
  M6B_CHECK (unmapped != NULL && pix != NULL, "x11 api test objects created");
  if (unmapped == NULL || pix == NULL)
    {
      if (unmapped)
	gdk_window_destroy (unmapped);
      if (pix)
	g_object_unref (pix);
      return;
    }
  M6B_CHECK (!gdk_window_is_visible (unmapped), "the third window is not mapped");

  m6b_x11_handles (d, child, unmapped, pix);
  m6b_x11_times (d, child, unmapped, pix);
  m6b_x11_atoms (d);

  g_object_unref (pix);
  gdk_window_destroy (unmapped);
  m6_drain ();
  m6_clear ();
}

/* ---- gaps from the M6 audit ----------------------------------------- */

static void
m6b_pointer_state (GdkDisplay *d, GdkWindow *child)
{
  GdkScreen *scr = NULL;
  GdkModifierType mask = 0, mask2 = 0;
  gint x = -1, y = -1, wx = -1, wy = -1;
  GdkWindow *w;
  guint32 t = 80000;

  m6_clear ();
  M6B_HOOK (_gdk_wayland_test_pointer_enter (d, main_window, 150, 160), "enter");
  m6_drain ();
  M6B_HOOK (_gdk_wayland_test_pointer_motion (d, t, 155, 165), "motion");
  m6_drain ();

  M6B_HOOK (_gdk_wayland_test_pointer_button (d, t + 10, GDKW_TEST_BTN_LEFT, TRUE),
	    "button down");
  m6_drain ();
  gdk_display_get_pointer (d, &scr, &x, &y, &mask);
  M6B_CHECK ((mask & GDK_BUTTON1_MASK) != 0, "get_pointer mask has BUTTON1");
  M6B_CHECK (x == 155 && y == 165, "get_pointer position with a button held");

  M6B_HOOK (_gdk_wayland_test_keyboard_modifiers (d, GDK_SHIFT_MASK), "shift");
  gdk_display_get_pointer (d, &scr, &x, &y, &mask);
  M6B_CHECK ((mask & GDK_BUTTON1_MASK) && (mask & GDK_SHIFT_MASK),
	     "get_pointer mask has button and shift");
  gdk_window_get_pointer (main_window, NULL, NULL, &mask2);
  M6B_EQ_INT (mask2 & (GDK_BUTTON1_MASK | GDK_SHIFT_MASK),
	      GDK_BUTTON1_MASK | GDK_SHIFT_MASK,
	      "window_get_pointer reports the same mask");

  w = gdk_display_get_window_at_pointer (d, &wx, &wy);
  M6B_CHECK (w == child, "window_at_pointer is the child with a button held");
  M6B_CHECK (wx == 55 && wy == 65, "window_at_pointer child coordinates");

  M6B_HOOK (_gdk_wayland_test_pointer_button (d, t + 20, GDKW_TEST_BTN_LEFT, FALSE),
	    "button up");
  m6_drain ();
  gdk_display_get_pointer (d, &scr, &x, &y, &mask);
  M6B_CHECK (!(mask & GDK_BUTTON1_MASK) && (mask & GDK_SHIFT_MASK),
	     "release clears BUTTON1 only");
  M6B_HOOK (_gdk_wayland_test_keyboard_modifiers (d, 0), "shift off");

  /* window_at_pointer follows the pointer out of the child, and the move
   * synthesises the crossing events */
  m6_clear ();
  M6B_HOOK (_gdk_wayland_test_pointer_motion (d, t + 30, 10, 10), "motion out");
  m6_drain ();
  M6B_CHECK (m6_find (GDK_LEAVE_NOTIFY, child) != NULL,
	     "moving out of the child leaves it");
  wx = wy = -1;
  w = gdk_display_get_window_at_pointer (d, &wx, &wy);
  M6B_CHECK (w == main_window, "window_at_pointer is main outside the child");
  M6B_CHECK (wx == 10 && wy == 10, "window_at_pointer main coordinates");
  m6_clear ();
}

static void
m6b_warp_beep_pending (GdkDisplay *d)
{
  GdkScreen *scr = gdk_screen_get_default ();
  gint x0 = -1, y0 = -1, x1 = -1, y1 = -1;
  int before;

  gdk_display_get_pointer (d, NULL, &x0, &y0, NULL);
  gdk_display_warp_pointer (d, scr, 10, 10);
  gdk_display_get_pointer (d, NULL, &x1, &y1, NULL);
  M6B_CHECK (x0 == x1 && y0 == y1, "warp leaves the reported position alone");

  m6_clear ();
  gdk_display_beep (d);
  gdk_beep ();
  gdk_window_beep (main_window);
  m6_drain ();
  before = g_queue_get_length (&m6_events);
  M6B_EQ_INT (before, 0, "beep produces no event");

  m6_clear ();
  M6B_HOOK (_gdk_wayland_test_pointer_motion (d, 90000, 160, 170), "motion queued");
  M6B_CHECK (gdk_events_pending (), "events pending after a hook queued one");
  m6_drain ();
  M6B_CHECK (!gdk_events_pending (), "no events pending after the drain");
  m6_clear ();
}

static void
m6b_destroy_while_repeating (GdkDisplay *d)
{
  GDKWTestSeatInfo info;
  GdkWindow *w2;
  gint64 deadline;
  int presses;

  w2 = m6b_toplevel (50, 50, GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK |
		     GDK_FOCUS_CHANGE_MASK, NULL, NULL);
  M6B_CHECK (w2 != NULL, "repeat window");
  if (w2 == NULL)
    return;
  gdk_window_show (w2);
  gdk_display_sync (d);

  m6_clear ();
  M6B_HOOK (_gdk_wayland_test_keyboard_enter (d, w2), "enter repeat window");
  M6B_HOOK (_gdk_wayland_test_keyboard_repeat_info (d, 50, 50), "repeat info");
  M6B_HOOK (_gdk_wayland_test_keyboard_key (d, 100000, GDKW_TEST_KEY_A, TRUE),
	    "press A");

  deadline = g_get_monotonic_time () + 2 * G_USEC_PER_SEC;
  memset (&info, 0, sizeof info);
  while (g_get_monotonic_time () < deadline)
    {
      m6_run_ms (20);
      if (_gdk_wayland_test_seat_info (d, &info) && info.repeat_count >= 2)
	break;
    }
  M6B_CHECK (info.repeat_count >= 2, "the key repeated at least twice");

  gdk_window_destroy (w2);
  m6_drain ();
  memset (&info, 0, sizeof info);
  M6B_CHECK (_gdk_wayland_test_seat_info (d, &info), "seat info after destroy");
  M6B_EQ_INT (info.repeat_key, 0, "repeat stopped by the window destroy");
  M6B_CHECK (info.keyboard_focus == NULL, "keyboard focus cleared by the destroy");

  m6_clear ();
  presses = m6_count (GDK_KEY_PRESS, NULL);
  m6_run_ms (250);
  M6B_EQ_INT (m6_count (GDK_KEY_PRESS, NULL), presses,
	      "no key press after the destroy");

  _gdk_wayland_test_keyboard_key (d, 100500, GDKW_TEST_KEY_A, FALSE);
  M6B_HOOK (_gdk_wayland_test_keyboard_repeat_info (d, 25, 400), "restore repeat");
  M6B_HOOK (_gdk_wayland_test_keyboard_enter (d, main_window), "focus main again");
  m6_drain ();
  m6_clear ();
}

static gboolean
m6b_seat_grab_active (GdkDisplay *d)
{
  GDKWTestSeatInfo info;

  memset (&info, 0, sizeof info);
  if (!_gdk_wayland_test_seat_info (d, &info))
    return FALSE;
  return info.explicit_grab_active;
}

/* A window that holds an implicit grab (button held) is destroyed.  The
 * rest of the gesture must not reach the dead window and nothing may
 * crash; later pointer input must work again. */
static void
m6b_destroy_with_implicit_grab (GdkDisplay *d)
{
  GdkWindow *w2, *gw = NULL;
  gboolean oe;
  GDKWTestSeatInfo info;
  GList *l;
  gboolean stale = FALSE;
  guint32 t = 120000;

  w2 = m6b_toplevel (50, 50, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
		     GDK_POINTER_MOTION_MASK | GDK_ENTER_NOTIFY_MASK |
		     GDK_LEAVE_NOTIFY_MASK, NULL, NULL);
  M6B_CHECK (w2 != NULL, "implicit grab window");
  if (w2 == NULL)
    return;
  gdk_window_show (w2);
  gdk_display_sync (d);

  m6_clear ();
  M6B_HOOK (_gdk_wayland_test_pointer_enter (d, w2, 20, 20), "enter w2");
  m6_drain ();
  M6B_HOOK (_gdk_wayland_test_pointer_button (d, t, GDKW_TEST_BTN_LEFT, TRUE),
	    "press on w2");
  m6_drain ();
  M6B_CHECK (gdk_pointer_grab_info_libgtk_only (d, &gw, &oe) && gw == w2,
	     "implicit grab is on w2");

  g_object_ref (w2);
  gdk_window_destroy (w2);
  m6_drain ();
  M6B_CHECK (!gdk_pointer_grab_info_libgtk_only (d, &gw, &oe) || gw != w2,
	     "the destroy dropped the implicit grab on w2");
  memset (&info, 0, sizeof info);
  M6B_CHECK (_gdk_wayland_test_seat_info (d, &info) && info.pointer_focus != w2,
	     "pointer focus no longer points at w2");

  /* the compositor can still send the rest of the gesture for a moment */
  m6_clear ();
  _gdk_wayland_test_pointer_motion (d, t + 10, 25, 25);
  _gdk_wayland_test_pointer_button (d, t + 20, GDKW_TEST_BTN_LEFT, FALSE);
  m6_drain ();
  for (l = m6_events.head; l; l = l->next)
    if (((GdkEvent *) l->data)->any.window == w2)
      stale = TRUE;
  M6B_CHECK (!stale, "no event is delivered to the destroyed window");
  g_object_unref (w2);

  memset (&info, 0, sizeof info);
  M6B_CHECK (_gdk_wayland_test_seat_info (d, &info) && info.button_mask == 0,
	     "no button is left pressed in the seat");
  M6B_CHECK (!gdk_pointer_grab_info_libgtk_only (d, &gw, &oe),
	     "no grab is left after the release");

  /* pointer input works again */
  m6_clear ();
  M6B_HOOK (_gdk_wayland_test_pointer_enter (d, main_window, 5, 5),
	    "enter main after the destroy");
  m6_drain ();
  M6B_HOOK (_gdk_wayland_test_pointer_motion (d, t + 30, 6, 6),
	    "motion after the destroy");
  m6_drain ();
  M6B_CHECK (m6_find (GDK_MOTION_NOTIFY, main_window) != NULL,
	     "motion still reaches main after the destroy");
  m6_clear ();
}

static void
m6b_destroy_with_grab (GdkDisplay *d)
{
  const GdkEventMask grab_mask = GDK_BUTTON_PRESS_MASK |
    GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK;
  GdkWindow *w2, *gw = NULL;
  gboolean oe;

  /* pointer grab */
  w2 = m6b_toplevel (50, 50, GDK_BUTTON_PRESS_MASK | GDK_KEY_PRESS_MASK,
		     NULL, NULL);
  M6B_CHECK (w2 != NULL, "grab window");
  if (w2 == NULL)
    return;
  gdk_window_show (w2);
  gdk_display_sync (d);

  M6B_EQ_INT (gdk_pointer_grab (w2, FALSE, grab_mask, NULL, NULL,
				GDK_CURRENT_TIME),
	      GDK_GRAB_SUCCESS, "pointer grab on w2");
  M6B_CHECK (gdk_pointer_is_grabbed (), "pointer grab is reported");
  M6B_CHECK (m6b_seat_grab_active (d), "seat records the explicit grab");
  gdk_window_destroy (w2);
  m6_drain ();
  M6B_CHECK (!gdk_pointer_is_grabbed (), "the destroy ended the pointer grab");
  M6B_CHECK (!m6b_seat_grab_active (d), "the destroy released the seat grab");
  M6B_EQ_INT (gdk_pointer_grab (main_window, FALSE, grab_mask, NULL, NULL,
				GDK_CURRENT_TIME),
	      GDK_GRAB_SUCCESS, "a new pointer grab works");
  M6B_CHECK (m6b_seat_grab_active (d), "seat records the second grab");
  gdk_pointer_ungrab (GDK_CURRENT_TIME);
  M6B_CHECK (!gdk_pointer_is_grabbed (), "new pointer grab released");
  M6B_CHECK (!m6b_seat_grab_active (d), "explicit ungrab released the seat grab");

  /* keyboard grab */
  w2 = m6b_toplevel (50, 50, GDK_BUTTON_PRESS_MASK | GDK_KEY_PRESS_MASK,
		     NULL, NULL);
  M6B_CHECK (w2 != NULL, "second grab window");
  if (w2 == NULL)
    return;
  gdk_window_show (w2);
  gdk_display_sync (d);
  M6B_EQ_INT (gdk_keyboard_grab (w2, FALSE, GDK_CURRENT_TIME),
	      GDK_GRAB_SUCCESS, "keyboard grab on w2");
  M6B_CHECK (gdk_keyboard_grab_info_libgtk_only (d, &gw, &oe),
	     "keyboard grab is recorded");
  gdk_window_destroy (w2);
  m6_drain ();
  M6B_CHECK (!gdk_keyboard_grab_info_libgtk_only (d, &gw, &oe),
	     "the destroy ended the keyboard grab");
  M6B_EQ_INT (gdk_keyboard_grab (main_window, FALSE, GDK_CURRENT_TIME),
	      GDK_GRAB_SUCCESS, "a new keyboard grab works");
  gdk_keyboard_ungrab (GDK_CURRENT_TIME);
  m6_clear ();
}

static void
m6b_close_display_with_windows (GdkDisplay *d)
{
  const gchar *name = g_getenv ("WAYLAND_DISPLAY");
  GdkDisplay *d2;
  GdkScreen *s2;
  GdkWindow *root2, *a, *b;
  GdkWindowAttr attr;
  int variant;

  if (name == NULL)
    return;

  for (variant = 0; variant < 2; variant++)
    {
      d2 = gdk_display_open (name);
      M6B_CHECK (d2 != NULL, "second display opened");
      if (d2 == NULL)
	return;

      s2 = gdk_display_get_default_screen (d2);
      root2 = gdk_screen_get_root_window (s2);
      memset (&attr, 0, sizeof attr);
      attr.window_type = GDK_WINDOW_TOPLEVEL;
      attr.wclass = GDK_INPUT_OUTPUT;
      attr.width = 60;
      attr.height = 60;
      attr.event_mask = GDK_BUTTON_PRESS_MASK;
      a = gdk_window_new (root2, &attr, 0);
      b = gdk_window_new (root2, &attr, 0);
      M6B_CHECK (a != NULL && b != NULL, "windows on the second display");
      if (a == NULL || b == NULL)
	return;

      gdk_window_show (a);
      gdk_window_show (b);
      gdk_display_sync (d2);
      gdk_window_set_title (a, "second display");

      if (variant == 1)
	M6B_EQ_INT (gdk_pointer_grab (a, FALSE, GDK_BUTTON_PRESS_MASK, NULL, NULL,
				      GDK_CURRENT_TIME),
		    GDK_GRAB_SUCCESS, "pointer grab on the second display");

      /* the application still holds both windows */
      g_object_ref (a);
      g_object_ref (b);
      gdk_display_close (d2);

      M6B_CHECK (DEMO_WINDOW_DESTROYED (a), "window a destroyed by the close");
      M6B_CHECK (DEMO_WINDOW_DESTROYED (b), "window b destroyed by the close");
      M6B_CHECK (gdk_window_get_display (a) == NULL,
		 "window a has no display after the close");
      g_object_unref (a);
      g_object_unref (b);

      M6B_CHECK (gdk_display_get_default () == d, "default display is unchanged");
      M6B_CHECK (!gdk_pointer_is_grabbed (), "no grab survives the close");
      gdk_display_sync (d);
      M6B_CHECK (gdk_window_is_visible (main_window), "main window still works");
    }
}

static int
m6b_exercise (void)
{
  GdkDisplay *display = gdk_display_get_default ();
  GdkWindowAttr attr;
  GdkWindow *child;

  gdk_event_handler_set (m6_recorder, NULL, NULL);

  gdk_window_set_events (main_window,
			 GDK_EXPOSURE_MASK | GDK_KEY_PRESS_MASK |
			 GDK_KEY_RELEASE_MASK | GDK_BUTTON_PRESS_MASK |
			 GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK |
			 GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK |
			 GDK_FOCUS_CHANGE_MASK | GDK_SCROLL_MASK);

  memset (&attr, 0, sizeof attr);
  attr.window_type = GDK_WINDOW_CHILD;
  attr.wclass = GDK_INPUT_OUTPUT;
  attr.x = 100;
  attr.y = 100;
  attr.width = 100;
  attr.height = 100;
  attr.event_mask = GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
    GDK_POINTER_MOTION_MASK | GDK_ENTER_NOTIFY_MASK |
    GDK_LEAVE_NOTIFY_MASK | GDK_SCROLL_MASK;
  child = gdk_window_new (main_window, &attr, GDK_WA_X | GDK_WA_Y);
  M6B_CHECK (child != NULL, "child window created");
  if (child == NULL)
    {
      g_printerr ("ydk_demo: M6B: FAIL (%d)\n", m6b_failures);
      gdk_event_handler_set (event_handler, NULL, NULL);
      return m6b_failures ? m6b_failures : 1;
    }
  gdk_window_show (child);
  M6B_HOOK (_gdk_wayland_test_seat_set_version (display, 5), "seat version");
  m6_drain ();
  m6_clear ();

  m6b_keymap (display);
  m6b_display (display);
  m6b_hints (display);
  m6b_x11_api (display, child);
  m6b_pointer_state (display, child);
  m6b_warp_beep_pending (display);
  m6b_destroy_while_repeating (display);
  m6b_destroy_with_grab (display);
  m6b_destroy_with_implicit_grab (display);
  m6b_close_display_with_windows (display);
  m6b_locale ();

  /* cleanup */
  _gdk_wayland_test_pointer_leave (display, main_window);
  _gdk_wayland_test_keyboard_leave (display, main_window);
  m6_drain ();
  m6_clear ();
  gdk_window_destroy (child);
  gdk_event_handler_set (event_handler, NULL, NULL);
  gdk_window_invalidate_rect (main_window, NULL, TRUE);

  if (m6b_failures)
    g_printerr ("ydk_demo: M6B: FAIL (%d)\n", m6b_failures);
  else
    g_print ("ydk_demo: M6B: PASS\n");

  return m6b_failures;
}

/* X11 forwarding sanity: the same public functions, now dispatched, must
 * still reach the X11 backend.  This is what proves the rename map in
 * Phase 1 of the smoke test. */
static int
m6b_x11_exercise (void)
{
  GdkDisplay *d = gdk_display_get_default ();
  const gchar *env = g_getenv ("DISPLAY");
  GdkKeymap *keymap = gdk_keymap_get_for_display (d);
  guint lower, upper;

  M6B_EQ_STR (gdk_keyval_name (GDK_a), "a", "x11: keyval_name");
  M6B_EQ_INT (gdk_keyval_from_name ("Return"), GDK_Return, "x11: from_name");
  gdk_keyval_convert_case (GDK_a, &lower, &upper);
  M6B_CHECK (lower == GDK_a && upper == GDK_A, "x11: convert_case");
  gdk_keyval_convert_case (0x1000394, &lower, &upper);
  M6B_EQ_INT (lower, gdk_unicode_to_keyval (0x3b4), "x11: convert_case of U+0394");
  M6B_CHECK (gdk_keymap_get_direction (keymap) != 0xffff, "x11: keymap direction");
  if (env != NULL)
    M6B_EQ_STR (gdk_display_get_name (d), env, "x11: display name is DISPLAY");
  M6B_CHECK (gdk_devices_list () != NULL, "x11: device list is not empty");
  M6B_CHECK (gdk_display_list_devices (d) != NULL, "x11: display device list");
  gdk_window_set_title (main_window, "x");
  M6B_EQ_INT (gdk_window_get_type_hint (main_window), GDK_WINDOW_TYPE_HINT_NORMAL,
	      "x11: type hint");
  M6B_CHECK (gdk_window_lookup (0) == NULL, "x11: window_lookup of 0");

  /* the dispatched X11 handle accessors and guards still reach Xlib */
  M6B_CHECK (gdk_x11_display_is_x11 (d), "x11: is_x11 is TRUE");
  M6B_CHECK (gdk_x11_display_get_xdisplay (d) != NULL, "x11: xdisplay is not NULL");
  M6B_CHECK (gdk_x11_drawable_get_xid (GDK_DRAWABLE (main_window)) != 0,
	     "x11: window xid is not 0");
  M6B_CHECK (gdk_x11_drawable_get_xdisplay (gdk_x11_window_get_drawable_impl (main_window))
	     == gdk_x11_display_get_xdisplay (d),
	     "x11: GDK_WINDOW_XDISPLAY is the display xdisplay");
  M6B_CHECK (gdk_x11_window_get_drawable_impl (main_window)
	     == GDK_WINDOW_OBJECT (main_window)->impl,
	     "x11: drawable impl is the window impl");
  gdk_x11_display_set_cursor_theme (d, NULL, 0);
  gdk_x11_window_set_user_time (main_window, 1);
  (void) gdk_x11_display_get_user_time (d);
  M6B_CHECK (TRUE, "x11: cursor theme, set_user_time and get_user_time returned");

  if (m6b_failures)
    g_printerr ("ydk_demo: M6B-X11: FAIL (%d)\n", m6b_failures);
  else
    g_print ("ydk_demo: M6B-X11: PASS\n");

  return m6b_failures;
}


/* ---- M7 ------------------------------------------------------------------
 *
 * Cursors, shapes and fonts, all headless.  Cursors: a generated Xcursor
 * theme (three files) is loaded through the search path of the reload hook,
 * the pointer is "moved" with the M6 pointer hooks and the cursor code
 * reports what it applied (_gdk_wayland_test_cursor_info), since a headless
 * Weston has no seat and no wl_pointer.  Shapes: bounding and input shapes
 * are read back as the regions the backend sent
 * (_gdk_wayland_test_shape_info) and as backing pixels
 * (_gdk_wayland_test_window_pixel).  Fonts: text is drawn into pixmaps and
 * the ink box is compared with the metrics Pango gave.  Same style as M6:
 * every check runs, failures are counted, CRITICAL stays fatal.
 */

static int m7_failures;
static GdkDisplay *m7_d;
static guint32 m7_time = 1000;

#define M7_CHECK(cond, what)						\
  G_STMT_START {							\
    if (!(cond))							\
      {									\
	g_printerr ("ydk_demo: M7 check failed: %s\n", what);		\
	m7_failures++;							\
      }									\
  } G_STMT_END

#define M7_EQ_INT(got, want, what)					\
  G_STMT_START {							\
    long m7_got = (long) (got), m7_want = (long) (want);		\
    if (m7_got != m7_want)						\
      {									\
	g_printerr ("ydk_demo: M7 check failed: %s: got %ld want %ld\n", \
		    what, m7_got, m7_want);				\
	m7_failures++;							\
      }									\
  } G_STMT_END

#define M7_EQ_STR(got, want, what)					\
  G_STMT_START {							\
    const char *m7_got = (got), *m7_want = (want);			\
    if (g_strcmp0 (m7_got, m7_want) != 0)				\
      {									\
	g_printerr ("ydk_demo: M7 check failed: %s: got \"%s\" want \"%s\"\n", \
		    what, m7_got ? m7_got : "(null)", m7_want);		\
	m7_failures++;							\
      }									\
  } G_STMT_END

#define M7_HOOK(call, what)						\
  G_STMT_START {							\
    if (!(call))							\
      {									\
	g_printerr ("ydk_demo: M7 check failed: %s: hook did not deliver\n", \
		    what);						\
	m7_failures++;							\
      }									\
  } G_STMT_END

extern gboolean gdk_pixbuf_set_option (GdkPixbuf *pixbuf, const gchar *key,
				       const gchar *value);

static void
m7_drain (void)
{
  while (g_main_context_iteration (NULL, FALSE))
    ;
}

/* repaint a window completely and push it to the compositor */
static void
m7_repaint (GdkWindow *w)
{
  gdk_window_invalidate_rect (w, NULL, TRUE);
  gdk_window_process_all_updates ();
  gdk_flush ();
  m7_drain ();
}

/* ---- cursors ---- */

/* independent literal table: the primary Xcursor name of each even type */
static const char *const m7_names[77] = {
  "X_cursor", "arrow", "based_arrow_down", "based_arrow_up", "boat",
  "bogosity", "bottom_left_corner", "bottom_right_corner", "bottom_side",
  "bottom_tee", "box_spiral", "center_ptr", "circle", "clock", "coffee_mug",
  "cross", "cross_reverse", "crosshair", "diamond_cross", "dot", "dotbox",
  "double_arrow", "draft_large", "draft_small", "draped_box", "exchange",
  "fleur", "gobbler", "gumby", "hand1", "hand2", "heart", "icon",
  "iron_cross", "left_ptr", "left_side", "left_tee", "leftbutton",
  "ll_angle", "lr_angle", "man", "middlebutton", "mouse", "pencil", "pirate",
  "plus", "question_arrow", "right_ptr", "right_side", "right_tee",
  "rightbutton", "rtl_logo", "sailboat", "sb_down_arrow",
  "sb_h_double_arrow", "sb_left_arrow", "sb_right_arrow", "sb_up_arrow",
  "sb_v_double_arrow", "shuttle", "sizing", "spider", "spraycan", "star",
  "target", "tcross", "top_left_arrow", "top_left_corner",
  "top_right_corner", "top_side", "top_tee", "trek", "ul_angle", "umbrella",
  "ur_angle", "watch", "xterm"
};

static void
m7_put32 (GByteArray *a, guint32 v)
{
  guint8 b[4];

  b[0] = v & 0xff;
  b[1] = (v >> 8) & 0xff;
  b[2] = (v >> 16) & 0xff;
  b[3] = (v >> 24) & 0xff;
  g_byte_array_append (a, b, 4);
}

/* one image Xcursor file; layout verified against libwayland-cursor */
static gboolean
m7_write_xcursor (const gchar *path, int w, int h, int xhot, int yhot)
{
  GByteArray *a = g_byte_array_new ();
  gboolean ok;
  int i;

  m7_put32 (a, 0x72756358);	/* "Xcur" */
  m7_put32 (a, 16);
  m7_put32 (a, 0x00010000);
  m7_put32 (a, 1);
  m7_put32 (a, 0xfffd0002);	/* TOC: image, nominal size 24, offset 28 */
  m7_put32 (a, 24);
  m7_put32 (a, 28);
  m7_put32 (a, 36);		/* chunk header */
  m7_put32 (a, 0xfffd0002);
  m7_put32 (a, 24);
  m7_put32 (a, 1);
  m7_put32 (a, w);
  m7_put32 (a, h);
  m7_put32 (a, xhot);
  m7_put32 (a, yhot);
  m7_put32 (a, 0);
  for (i = 0; i < w * h; i++)
    m7_put32 (a, 0xff000000u);
  ok = g_file_set_contents (path, (const gchar *) a->data, a->len, NULL);
  g_byte_array_free (a, TRUE);
  return ok;
}

static void
m7_info (GDKWTestCursorInfo *info)
{
  memset (info, 0, sizeof *info);
  M7_HOOK (_gdk_wayland_test_cursor_info (m7_d, info), "cursor info");
}

static void
m7_expect (GDKWTestCursorKind kind, const char *name, int w, int h,
	   int hx, int hy, const char *what)
{
  GDKWTestCursorInfo i;
  gchar *msg;

  m7_info (&i);
  msg = g_strdup_printf ("%s: kind", what);
  M7_EQ_INT (i.kind, kind, msg);
  g_free (msg);
  if (name)
    {
      msg = g_strdup_printf ("%s: name", what);
      M7_EQ_STR (i.name, name, msg);
      g_free (msg);
    }
  msg = g_strdup_printf ("%s: size", what);
  M7_CHECK (i.width == w && i.height == h, msg);
  g_free (msg);
  msg = g_strdup_printf ("%s: hotspot", what);
  M7_CHECK (i.hot_x == hx && i.hot_y == hy, msg);
  g_free (msg);
}

static void
m7_set_type (GdkWindow *w, GdkCursorType type)
{
  GdkCursor *c = gdk_cursor_new_for_display (m7_d, type);

  gdk_window_set_cursor (w, c);
  if (c)
    gdk_cursor_unref (c);
  m7_drain ();
}

static void
m7_motion (gdouble x, gdouble y)
{
  m7_time += 10;
  M7_HOOK (_gdk_wayland_test_pointer_motion (m7_d, m7_time, x, y), "motion");
  m7_drain ();
}

static void
m7_enter (gdouble x, gdouble y)
{
  M7_HOOK (_gdk_wayland_test_pointer_enter (m7_d, main_window, x, y), "enter");
  m7_drain ();
}

static void
m7_leave (void)
{
  M7_HOOK (_gdk_wayland_test_pointer_leave (m7_d, main_window), "leave");
  m7_drain ();
}

static void
m7_cursor_names (void)
{
  const gchar * const *cand;
  int t, i;
  gboolean found;

  for (t = 0; t <= 152; t += 2)
    {
      gchar *msg = g_strdup_printf ("type %d name", t);

      M7_EQ_STR (_gdk_wayland_cursor_type_to_name (t), m7_names[t / 2], msg);
      g_free (msg);
    }
  M7_EQ_STR (_gdk_wayland_cursor_type_to_name (GDK_LEFT_PTR), "left_ptr", "68");
  M7_EQ_STR (_gdk_wayland_cursor_type_to_name (GDK_HAND2), "hand2", "60");
  M7_EQ_STR (_gdk_wayland_cursor_type_to_name (GDK_SB_H_DOUBLE_ARROW),
	     "sb_h_double_arrow", "108");
  M7_EQ_STR (_gdk_wayland_cursor_type_to_name (GDK_WATCH), "watch", "150");
  M7_EQ_STR (_gdk_wayland_cursor_type_to_name (GDK_XTERM), "xterm", "152");
  M7_EQ_STR (_gdk_wayland_cursor_type_to_name (GDK_FLEUR), "fleur", "52");
  M7_CHECK (_gdk_wayland_cursor_type_to_name (1) == NULL, "odd type 1");
  M7_CHECK (_gdk_wayland_cursor_type_to_name (151) == NULL, "odd type 151");
  M7_CHECK (_gdk_wayland_cursor_type_to_name (GDK_LAST_CURSOR) == NULL,
	    "GDK_LAST_CURSOR");
  M7_CHECK (_gdk_wayland_cursor_type_to_name (GDK_BLANK_CURSOR) == NULL,
	    "GDK_BLANK_CURSOR");
  M7_CHECK (_gdk_wayland_cursor_type_to_name (GDK_CURSOR_IS_PIXMAP) == NULL,
	    "GDK_CURSOR_IS_PIXMAP");
  M7_CHECK (_gdk_wayland_cursor_type_to_name (154) == NULL, "154");
  M7_CHECK (_gdk_wayland_cursor_type_to_name (-5) == NULL, "-5");
  M7_CHECK (_gdk_wayland_cursor_name_candidates (GDK_BLANK_CURSOR) == NULL,
	    "no candidates for blank");

  cand = _gdk_wayland_cursor_name_candidates (GDK_SB_H_DOUBLE_ARROW);
  M7_CHECK (cand != NULL && cand[0] != NULL, "candidates exist");
  if (cand && cand[0])
    {
      M7_EQ_STR (cand[0], "sb_h_double_arrow", "candidate 0 is the primary name");
      found = FALSE;
      for (i = 1; cand[i]; i++)
	if (g_strcmp0 (cand[i], "ew-resize") == 0)
	  found = TRUE;
      M7_CHECK (found, "ew-resize is a candidate of sb_h_double_arrow");
    }
}

/* builds a depth 1 bitmap from a predicate on (column, row) */
static GdkBitmap *
m7_bitmap (int w, int h, int (*pred) (int col, int row))
{
  int stride = (w + 7) / 8, r, c;
  guchar *bits = g_malloc0 (stride * h);
  GdkBitmap *bm;

  for (r = 0; r < h; r++)
    for (c = 0; c < w; c++)
      if (pred (c, r))
	bits[r * stride + c / 8] |= 1 << (c % 8);
  bm = gdk_bitmap_create_from_data (main_window, (const gchar *) bits, w, h);
  g_free (bits);
  return bm;
}

static const guchar m7_src_bits[8] = { 0xff, 0x0f, 0xaa, 0x55, 0xf0, 0x00, 0xff, 0x81 };
static const guchar m7_mask_bits[8] = { 0xff, 0xff, 0xf0, 0x3c, 0x0f, 0xff, 0x00, 0x81 };

static void
m7_custom_cursors (void)
{
  GdkBitmap *src, *mask, *b16;
  GdkPixmap *bad_depth;
  GdkColor fg = { 0, 0xffff, 0, 0 }, bg = { 0, 0, 0, 0xffff };
  GdkCursor *c, *c2;
  GdkPixbuf *pb, *img;
  GDKWTestCursorInfo i;
  guint32 px;
  int x, y, bad = 0, bad_img = 0;
  gboolean seen[3] = { FALSE, FALSE, FALSE };
  const guchar *p;
  guchar big[32] = { 0 };

  src = gdk_bitmap_create_from_data (main_window, (const gchar *) m7_src_bits, 8, 8);
  mask = gdk_bitmap_create_from_data (main_window, (const gchar *) m7_mask_bits, 8, 8);
  M7_CHECK (src != NULL && mask != NULL, "custom: bitmaps created");
  if (!src || !mask)
    return;

  c = gdk_cursor_new_from_pixmap (src, mask, &fg, &bg, 3, 2);
  M7_CHECK (c != NULL, "custom: new_from_pixmap");
  if (c)
    {
      gdk_window_set_cursor (main_window, c);
      m7_drain ();
      m7_expect (GDKW_TEST_CURSOR_CUSTOM, NULL, 8, 8, 3, 2, "custom pixmap cursor");
      m7_info (&i);
      M7_CHECK (i.cursor == c, "custom: resolved cursor is the pixmap cursor");

      for (y = 0; y < 8; y++)
	for (x = 0; x < 8; x++)
	  {
	    int s = (m7_src_bits[y] >> x) & 1, m = (m7_mask_bits[y] >> x) & 1;
	    guint32 want = !m ? 0x00000000u : (s ? 0xffff0000u : 0xff0000ffu);

	    seen[!m ? 0 : (s ? 1 : 2)] = TRUE;
	    if (!_gdk_wayland_test_cursor_pixel (m7_d, x, y, &px) || px != want)
	      bad++;
	  }
      M7_EQ_INT (bad, 0, "custom: pixels follow the XBM rule");
      M7_CHECK (seen[0] && seen[1] && seen[2], "custom: all three pixel classes tested");

      img = gdk_cursor_get_image (c);
      M7_CHECK (img != NULL, "custom: get_image");
      if (img)
	{
	  M7_EQ_INT (gdk_pixbuf_get_width (img), 8, "custom image width");
	  M7_EQ_INT (gdk_pixbuf_get_height (img), 8, "custom image height");
	  M7_EQ_STR (gdk_pixbuf_get_option (img, "x_hot"), "3", "custom image x_hot");
	  M7_EQ_STR (gdk_pixbuf_get_option (img, "y_hot"), "2", "custom image y_hot");
	  if (gdk_pixbuf_get_width (img) == 8 && gdk_pixbuf_get_height (img) == 8 &&
	      gdk_pixbuf_get_n_channels (img) == 4)
	    for (y = 0; y < 8; y++)
	      for (x = 0; x < 8; x++)
		{
		  int s = (m7_src_bits[y] >> x) & 1, m = (m7_mask_bits[y] >> x) & 1;

		  p = gdk_pixbuf_get_pixels (img) + y * gdk_pixbuf_get_rowstride (img) + x * 4;
		  if (!m)
		    bad_img += p[3] != 0;
		  else if (s)
		    bad_img += !(p[0] == 255 && p[1] == 0 && p[2] == 0 && p[3] == 255);
		  else
		    bad_img += !(p[0] == 0 && p[1] == 0 && p[2] == 255 && p[3] == 255);
		}
	  M7_EQ_INT (bad_img, 0, "custom: get_image RGBA matches");
	  g_object_unref (img);
	}

      /* unref while applied: the buffer must stay valid until replaced */
      gdk_cursor_unref (c);
      m7_drain ();
    }

  /* a second custom cursor, unreffed while it is the applied one */
  c2 = gdk_cursor_new_from_pixmap (src, mask, &fg, &bg, 0, 0);
  if (c2)
    {
      gdk_window_set_cursor (main_window, c2);
      m7_drain ();
      gdk_cursor_unref (c2);
      m7_drain ();
    }
  m7_set_type (main_window, GDK_XTERM);

  /* bad arguments give NULL */
  b16 = gdk_bitmap_create_from_data (main_window, (const gchar *) big, 16, 16);
  M7_CHECK (gdk_cursor_new_from_pixmap (src, b16, &fg, &bg, 0, 0) == NULL,
	    "custom: mismatched sizes give NULL");
  if (b16)
    g_object_unref (b16);
  bad_depth = gdk_pixmap_new (main_window, 8, 8, 24);
  M7_CHECK (gdk_cursor_new_from_pixmap (bad_depth, mask, &fg, &bg, 0, 0) == NULL,
	    "custom: depth 24 source gives NULL");
  if (bad_depth)
    g_object_unref (bad_depth);
  g_object_unref (src);
  g_object_unref (mask);

  /* pixbuf cursor */
  pb = gdk_pixbuf_new (GDK_COLORSPACE_RGB, TRUE, 8, 3, 2);
  M7_CHECK (pb != NULL, "pixbuf: created");
  if (pb)
    {
      guchar *d = gdk_pixbuf_get_pixels (pb);
      int rs = gdk_pixbuf_get_rowstride (pb);

      gdk_pixbuf_fill (pb, 0);
      d[0] = 255; d[1] = 255; d[2] = 255; d[3] = 255;
      d[4] = 200; d[5] = 100; d[6] = 50; d[7] = 128;
      d[rs + 8] = 0; d[rs + 9] = 0; d[rs + 10] = 255; d[rs + 11] = 255;
      gdk_pixbuf_set_option (pb, "x_hot", "1");
      gdk_pixbuf_set_option (pb, "y_hot", "0");
      c = gdk_cursor_new_from_pixbuf (m7_d, pb, -1, -1);
      M7_CHECK (c != NULL, "pixbuf: new_from_pixbuf");
      if (c)
	{
	  gdk_window_set_cursor (main_window, c);
	  m7_drain ();
	  m7_expect (GDKW_TEST_CURSOR_CUSTOM, NULL, 3, 2, 1, 0, "pixbuf cursor");
	  if (_gdk_wayland_test_cursor_pixel (m7_d, 1, 0, &px))
	    {
	      int a = px >> 24, r = (px >> 16) & 0xff, g = (px >> 8) & 0xff, b = px & 0xff;

	      M7_CHECK (abs (a - 128) <= 1 && abs (r - 100) <= 1 &&
			abs (g - 50) <= 1 && abs (b - 25) <= 1,
			"pixbuf: pixel is premultiplied (100,50,25,128)");
	    }
	  else
	    M7_CHECK (FALSE, "pixbuf: cursor_pixel (1,0)");
	  M7_CHECK (_gdk_wayland_test_cursor_pixel (m7_d, 0, 0, &px) && px == 0xffffffffu,
		    "pixbuf: opaque white pixel");
	  gdk_cursor_unref (c);
	}
      g_object_unref (pb);
    }
  m7_set_type (main_window, GDK_XTERM);
}

static void
m7_remove_in (const gchar *dir, const gchar *name)
{
  gchar *file = g_build_filename (dir, name, NULL);

  g_remove (file);
  g_free (file);
}

static void
m7_cursors (void)
{
  GdkWindowAttr attr;
  GdkWindow *child;
  GdkCursor *cur, *cc, *xterm;
  GDKWTestCursorInfo i, j;
  GdkEventMask mask = GDK_BUTTON_PRESS_MASK | GDK_POINTER_MOTION_MASK;
  gchar *tmp, *dir, *file, *empty, *saved;
  guint32 s1, px;
  guint n;

  tmp = g_dir_make_tmp ("ydk-m7-XXXXXX", NULL);
  M7_CHECK (tmp != NULL, "cursor: temp dir");
  if (!tmp)
    return;
  dir = g_build_filename (tmp, "ydktest", "cursors", NULL);
  empty = g_build_filename (tmp, "empty", NULL);
  g_mkdir_with_parents (dir, 0700);
  g_mkdir_with_parents (empty, 0700);
  file = g_build_filename (dir, "left_ptr", NULL);
  M7_CHECK (m7_write_xcursor (file, 8, 6, 1, 2), "write left_ptr");
  g_free (file);
  file = g_build_filename (dir, "xterm", NULL);
  M7_CHECK (m7_write_xcursor (file, 5, 7, 2, 3), "write xterm");
  g_free (file);
  file = g_build_filename (dir, "ew-resize", NULL);
  M7_CHECK (m7_write_xcursor (file, 9, 9, 4, 4), "write ew-resize");
  g_free (file);

  gdk_window_set_events (main_window,
			 GDK_EXPOSURE_MASK | GDK_KEY_PRESS_MASK |
			 GDK_BUTTON_PRESS_MASK | GDK_POINTER_MOTION_MASK |
			 GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);

  /* 1. names */
  m7_cursor_names ();

  /* 2. injected theme */
  M7_HOOK (_gdk_wayland_test_cursor_theme_reload (m7_d, "ydktest", 24, tmp),
	   "theme reload");
  m7_info (&i);
  M7_CHECK (i.theme_loaded && i.theme_usable, "theme: loaded and usable");
  M7_EQ_STR (i.theme_name, "ydktest", "theme name");
  M7_EQ_INT (i.theme_size, 24, "theme size");
  cur = gdk_cursor_new_from_name (m7_d, "left_ptr");
  M7_CHECK (cur != NULL, "new_from_name left_ptr");
  if (cur)
    gdk_cursor_unref (cur);
  cur = gdk_cursor_new_from_name (m7_d, "no_such_cursor_zz");
  M7_CHECK (cur == NULL, "new_from_name of a missing name is NULL");
  if (cur)
    gdk_cursor_unref (cur);

  M7_HOOK (_gdk_wayland_test_cursor_reset (m7_d), "cursor reset");
  m7_enter (10, 10);
  m7_expect (GDKW_TEST_CURSOR_THEME, "left_ptr", 8, 6, 1, 2, "enter: default cursor");
  m7_info (&i);
  M7_CHECK (i.set_count >= 1, "enter applies a cursor");
  M7_CHECK (!i.to_compositor, "no wl_pointer: nothing goes to the compositor");
  s1 = i.serial;

  m7_set_type (main_window, GDK_XTERM);
  m7_expect (GDKW_TEST_CURSOR_THEME, "xterm", 5, 7, 2, 3, "xterm");
  m7_set_type (main_window, GDK_SB_H_DOUBLE_ARROW);
  m7_expect (GDKW_TEST_CURSOR_THEME, "ew-resize", 9, 9, 4, 4, "ew-resize alias");
  m7_set_type (main_window, GDK_WATCH);
  m7_expect (GDKW_TEST_CURSOR_THEME, "left_ptr", 8, 6, 1, 2, "watch falls back to left_ptr");
  m7_set_type (main_window, GDK_BLANK_CURSOR);
  m7_expect (GDKW_TEST_CURSOR_HIDDEN, NULL, 0, 0, 0, 0, "blank cursor");

  m7_set_type (main_window, GDK_XTERM);
  m7_info (&i);
  n = i.set_count;
  m7_set_type (main_window, GDK_XTERM);
  m7_info (&i);
  M7_EQ_INT (i.set_count, n, "same cursor again is deduplicated");

  /* 3. child windows */
  memset (&attr, 0, sizeof attr);
  attr.window_type = GDK_WINDOW_CHILD;
  attr.wclass = GDK_INPUT_OUTPUT;
  attr.x = 150;
  attr.y = 50;
  attr.width = 80;
  attr.height = 80;
  attr.event_mask = GDK_POINTER_MOTION_MASK | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK;
  child = gdk_window_new (main_window, &attr, GDK_WA_X | GDK_WA_Y);
  M7_CHECK (child != NULL, "cursor: child window");
  if (child)
    {
      m7_set_type (main_window, GDK_SB_H_DOUBLE_ARROW);
      gdk_window_show (child);
      m7_drain ();
      cc = gdk_cursor_new_for_display (m7_d, GDK_XTERM);
      gdk_window_set_cursor (child, cc);
      m7_drain ();
      m7_motion (160, 60);
      m7_info (&i);
      M7_CHECK (i.cursor == cc, "child: pointer in the child uses its cursor");
      m7_expect (GDKW_TEST_CURSOR_THEME, "xterm", 5, 7, 2, 3, "child cursor");
      m7_motion (10, 10);
      m7_expect (GDKW_TEST_CURSOR_THEME, "ew-resize", 9, 9, 4, 4, "back in the parent");
      m7_motion (160, 60);
      gdk_window_set_cursor (child, NULL);
      m7_drain ();
      m7_expect (GDKW_TEST_CURSOR_THEME, "ew-resize", 9, 9, 4, 4,
		 "clearing the child cursor restores the parent's");
      m7_motion (10, 10);
      gdk_cursor_unref (cc);
      gdk_window_destroy (child);
      m7_drain ();
    }

  /* cursor set while the pointer is outside: remembered, applied by enter */
  m7_leave ();
  m7_info (&i);
  n = i.set_count;
  xterm = gdk_cursor_new_for_display (m7_d, GDK_XTERM);
  gdk_window_set_cursor (main_window, xterm);
  m7_drain ();
  m7_info (&i);
  M7_EQ_INT (i.set_count, n, "set_cursor outside the window sends nothing");

  /* 4. serial */
  m7_enter (10, 10);
  m7_info (&i);
  /* enter applies the last remembered (possibly stale) cursor with the new
   * serial, then the core's crossing handling resolves the real one and the
   * vfunc re-applies it: one or two sends, the final cursor is what counts */
  M7_CHECK (i.set_count == n + 1 || i.set_count == n + 2, "enter applies the cursor");
  M7_CHECK (i.cursor == xterm, "enter applied the cursor set while outside");
  M7_CHECK (i.serial > s1, "new enter has a newer serial");
  m7_leave ();
  m7_info (&j);
  M7_EQ_INT (j.set_count, i.set_count, "leave sends nothing");
  m7_enter (10, 10);
  m7_info (&j);
  M7_EQ_INT (j.set_count, i.set_count + 1, "re-enter with the same cursor sends again");
  M7_CHECK (j.serial > i.serial, "re-enter serial grows");

  /* 5. explicit grab */
  m7_set_type (main_window, GDK_SB_H_DOUBLE_ARROW);
  M7_EQ_INT (gdk_pointer_grab (main_window, FALSE, mask, NULL, xterm,
			       GDK_CURRENT_TIME), GDK_GRAB_SUCCESS, "cursor: pointer grab");
  m7_drain ();
  m7_expect (GDKW_TEST_CURSOR_THEME, "xterm", 5, 7, 2, 3, "grab cursor wins");
  gdk_pointer_ungrab (GDK_CURRENT_TIME);
  m7_drain ();
  m7_expect (GDKW_TEST_CURSOR_THEME, "ew-resize", 9, 9, 4, 4, "after ungrab");
  gdk_cursor_unref (xterm);

  /* 7 and 8. custom pixmap and pixbuf cursors */
  m7_custom_cursors ();

  /* 6. built in theme, disabled theme */
  M7_HOOK (_gdk_wayland_test_cursor_theme_reload (m7_d, NULL, 0, empty), "builtin theme");
  m7_info (&i);
  M7_CHECK (i.theme_usable, "empty search path: built in theme is usable");
  cur = gdk_cursor_new_from_name (m7_d, "left_ptr");
  M7_CHECK (cur != NULL, "builtin theme: left_ptr resolves");
  if (cur)
    gdk_cursor_unref (cur);

  M7_HOOK (_gdk_wayland_test_cursor_disable_theme (m7_d, TRUE), "disable theme");
  m7_info (&i);
  M7_CHECK (!i.theme_usable, "disabled theme is not usable");
  cur = gdk_cursor_new_from_name (m7_d, "anything");
  M7_CHECK (cur != NULL, "unusable theme accepts any name");
  if (cur)
    gdk_cursor_unref (cur);
  m7_set_type (main_window, GDK_LEFT_PTR);
  m7_expect (GDKW_TEST_CURSOR_FALLBACK, NULL, 1, 1, 0, 0, "fallback cursor");
  M7_CHECK (_gdk_wayland_test_cursor_pixel (m7_d, 0, 0, &px) && px == 0,
	    "fallback pixel is transparent");
  m7_set_type (main_window, GDK_BLANK_CURSOR);
  m7_expect (GDKW_TEST_CURSOR_HIDDEN, NULL, 0, 0, 0, 0, "blank with a disabled theme");
  M7_HOOK (_gdk_wayland_test_cursor_disable_theme (m7_d, FALSE), "enable theme");
  m7_info (&i);
  M7_CHECK (i.theme_usable, "theme usable again");

  /* 9. theme change through the public function */
  saved = g_strdup (g_getenv ("XCURSOR_PATH"));
  g_setenv ("XCURSOR_PATH", tmp, TRUE);
  gdk_x11_display_set_cursor_theme (m7_d, "ydktest", 32);
  if (saved)
    g_setenv ("XCURSOR_PATH", saved, TRUE);
  else
    g_unsetenv ("XCURSOR_PATH");
  g_free (saved);
  m7_info (&i);
  M7_EQ_STR (i.theme_name, "ydktest", "set_cursor_theme: name");
  M7_EQ_INT (i.theme_size, 32, "set_cursor_theme: size");
  M7_EQ_INT (gdk_display_get_default_cursor_size (m7_d), 32, "default cursor size follows");
  gdk_x11_display_set_cursor_theme (NULL, "ydktest", 24);
  gdk_x11_display_set_cursor_theme (m7_d, NULL, -1);
  gdk_x11_display_set_cursor_theme (m7_d, "", 0);
  gdk_x11_display_set_cursor_theme (m7_d, "ydktest", -1);
  m7_info (&i);
  M7_EQ_STR (i.theme_name, "ydktest", "bad arguments keep the theme name");
  M7_EQ_INT (i.theme_size, 32, "bad arguments keep the size");

  /* back to a known theme for the rest of the run, then clean up */
  M7_HOOK (_gdk_wayland_test_cursor_theme_reload (m7_d, "ydktest", 24, tmp), "reload");
  m7_set_type (main_window, GDK_LEFT_PTR);
  m7_leave ();
  gdk_window_set_cursor (main_window, NULL);
  m7_drain ();
  M7_HOOK (_gdk_wayland_test_cursor_theme_reload (m7_d, NULL, 0, empty), "reload builtin");

  m7_remove_in (dir, "left_ptr");
  m7_remove_in (dir, "xterm");
  m7_remove_in (dir, "ew-resize");
  g_rmdir (dir);
  file = g_build_filename (tmp, "ydktest", NULL);
  g_rmdir (file);
  g_free (file);
  g_rmdir (empty);
  g_rmdir (tmp);
  g_free (dir);
  g_free (empty);
  g_free (tmp);
}

/* cursors on a second display that is closed while they are alive */
static void
m7_cursor_lifetime (void)
{
  const gchar *name = g_getenv ("WAYLAND_DISPLAY");
  GdkDisplay *d2;
  GdkWindow *root2, *a;
  GdkWindowAttr attr;
  GdkPixbuf *pb;
  GdkCursor *c, *t;

  if (name == NULL)
    return;
  d2 = gdk_display_open (name);
  M7_CHECK (d2 != NULL, "lifetime: second display");
  if (d2 == NULL)
    return;
  root2 = gdk_screen_get_root_window (gdk_display_get_default_screen (d2));
  memset (&attr, 0, sizeof attr);
  attr.window_type = GDK_WINDOW_TOPLEVEL;
  attr.wclass = GDK_INPUT_OUTPUT;
  attr.width = 40;
  attr.height = 40;
  a = gdk_window_new (root2, &attr, 0);
  pb = gdk_pixbuf_new (GDK_COLORSPACE_RGB, TRUE, 8, 4, 4);
  gdk_pixbuf_fill (pb, 0xff0000ff);
  c = gdk_cursor_new_from_pixbuf (d2, pb, 1, 1);
  t = gdk_cursor_new_for_display (d2, GDK_XTERM);
  M7_CHECK (c != NULL && t != NULL, "lifetime: cursors on the second display");
  if (a)
    {
      gdk_window_show (a);
      gdk_display_sync (d2);
      gdk_window_set_cursor (a, c);
    }
  g_object_unref (pb);
  gdk_display_close (d2);
  /* the cursors outlive their display */
  if (c)
    gdk_cursor_unref (c);
  if (t)
    gdk_cursor_unref (t);
}

/* ---- shapes ---- */

static int
m7_pred_checker (int c, int r)
{
  return (r < 8 && c < 8) || (r >= 8 && c >= 8);
}

static int
m7_pred_cols (int c, int r)
{
  return c <= 5;
}

/* compares two rectangle lists as regions, never as ordered lists */
static gboolean
m7_rects_equal (const GdkRectangle *r, int n, const GdkRectangle *want, int nwant)
{
  GdkRegion *a = gdk_region_new (), *b = gdk_region_new ();
  gboolean eq;
  int i;

  if (n > GDKW_TEST_MAX_RECTS)
    n = GDKW_TEST_MAX_RECTS;
  for (i = 0; i < n; i++)
    gdk_region_union_with_rect (a, &r[i]);
  for (i = 0; i < nwant; i++)
    gdk_region_union_with_rect (b, &want[i]);
  eq = gdk_region_equal (a, b);
  gdk_region_destroy (a);
  gdk_region_destroy (b);
  return eq;
}

static void
m7_shape_info (GdkWindow *w, GDKWTestShapeInfo *i)
{
  memset (i, 0, sizeof *i);
  M7_HOOK (_gdk_wayland_test_shape_info (w, i), "shape info");
}

static void
m7_pixel_class (GdkWindow *w, int x, int y, gboolean opaque, const char *what)
{
  guint32 px = 0xdeadbeef;

  if (!_gdk_wayland_test_window_pixel (w, x, y, &px))
    {
      g_printerr ("ydk_demo: M7 check failed: %s: no pixel at (%d,%d)\n", what, x, y);
      m7_failures++;
      return;
    }
  if (opaque ? (px >> 24) != 0xff : px != 0)
    {
      g_printerr ("ydk_demo: M7 check failed: %s at (%d,%d): pixel %08x\n",
		  what, x, y, px);
      m7_failures++;
    }
}

static void
m7_shapes (void)
{
  static const GdkRectangle b_want[2] = { { 4, 4, 8, 8 }, { 12, 12, 8, 8 } };
  static const GdkRectangle i_want[1] = { { 4, 4, 6, 16 } };
  static const GdkRectangle e_want[1] = { { 4, 4, 6, 8 } };
  GdkWindow *w = main_window;
  GdkBitmap *bm, *bi;
  GDKWTestShapeInfo s, s2;
  GdkWindowAttr attr;
  GdkWindow *top, *child;
  GdkRegion *empty;
  GdkColormap *rgba;
  guint32 px;
  guint reqs;

  /* 1. capabilities */
  M7_CHECK (gdk_display_supports_shapes (m7_d), "supports_shapes TRUE");
  M7_CHECK (gdk_display_supports_input_shapes (m7_d), "supports_input_shapes TRUE");

  m7_repaint (w);

  /* 2. bounding shape */
  bm = m7_bitmap (16, 16, m7_pred_checker);
  bi = m7_bitmap (16, 16, m7_pred_cols);
  M7_CHECK (bm != NULL && bi != NULL, "shape: bitmaps");
  if (!bm || !bi)
    return;
  gdk_window_shape_combine_mask (w, bm, 4, 4);
  m7_drain ();
  m7_shape_info (w, &s);
  M7_CHECK (s.has_surface, "shape: toplevel has a surface");
  M7_CHECK (s.shaped, "shape: shaped");
  M7_CHECK (m7_rects_equal (s.bounding, s.n_bounding, b_want, 2), "shape: bounding rectangles");
  M7_CHECK (s.opaque_sent && m7_rects_equal (s.opaque, s.n_opaque, b_want, 2),
	    "shape: opaque region equals B (depth 24)");
  M7_CHECK (s.input_sent && m7_rects_equal (s.input, s.n_input, b_want, 2),
	    "shape: input region equals B");
  M7_CHECK (s.opaque_requests >= 1 && s.input_requests >= 1, "shape: requests sent");

  /* 3. pixels */
  m7_repaint (w);
  m7_pixel_class (w, 6, 6, TRUE, "inside the first square");
  m7_pixel_class (w, 14, 14, TRUE, "inside the second square");
  m7_pixel_class (w, 0, 0, FALSE, "outside the shape");
  m7_pixel_class (w, 15, 6, FALSE, "between the squares");

  /* 4. input shape */
  gdk_window_input_shape_combine_mask (w, bi, 4, 4);
  m7_drain ();
  m7_shape_info (w, &s);
  M7_CHECK (s.input_shaped, "input shape: stored");
  M7_CHECK (m7_rects_equal (s.input_shape, s.n_input_shape, i_want, 1), "input shape: rectangles");
  M7_CHECK (s.input_sent && m7_rects_equal (s.input, s.n_input, e_want, 1),
	    "input shape: effective region is B intersect I");
  M7_CHECK (!m7_rects_equal (s.input, s.n_input, b_want, 2), "input shape: differs from B");
  M7_CHECK (s.opaque_sent && m7_rects_equal (s.opaque, s.n_opaque, b_want, 2),
	    "input shape: opaque region still B");
  reqs = s.opaque_requests + s.input_requests;
  gdk_window_input_shape_combine_mask (w, bi, 4, 4);
  gdk_window_shape_combine_mask (w, bm, 4, 4);
  m7_drain ();
  m7_shape_info (w, &s2);
  M7_EQ_INT (s2.opaque_requests + s2.input_requests, reqs,
	     "repeating the same shapes sends nothing");

  /* 5. unset */
  gdk_window_shape_combine_mask (w, NULL, 0, 0);
  m7_drain ();
  m7_shape_info (w, &s);
  M7_CHECK (!s.shaped, "unset B: not shaped");
  M7_CHECK (!s.opaque_sent, "unset B: opaque region NULL");
  M7_CHECK (s.input_sent && m7_rects_equal (s.input, s.n_input, i_want, 1),
	    "unset B: input region is I");
  gdk_window_input_shape_combine_mask (w, NULL, 0, 0);
  m7_drain ();
  m7_shape_info (w, &s);
  M7_CHECK (!s.input_shaped && !s.input_sent, "unset I: input region NULL");
  m7_repaint (w);
  M7_CHECK (_gdk_wayland_test_window_pixel (w, 0, 0, &px) && px != 0,
	    "unshaped: pixel (0,0) is not forced transparent");
  M7_CHECK (_gdk_wayland_test_window_pixel (w, 15, 6, &px) && px != 0,
	    "unshaped: pixel (15,6) is not forced transparent");

  /* 6. rgba toplevel */
  rgba = gdk_screen_get_rgba_colormap (gdk_screen_get_default ());
  if (rgba != NULL)
    {
      memset (&attr, 0, sizeof attr);
      attr.window_type = GDK_WINDOW_TOPLEVEL;
      attr.wclass = GDK_INPUT_OUTPUT;
      attr.width = 64;
      attr.height = 64;
      attr.colormap = rgba;
      attr.visual = gdk_colormap_get_visual (rgba);
      top = gdk_window_new (gdk_get_default_root_window (), &attr,
			    GDK_WA_COLORMAP | GDK_WA_VISUAL);
      M7_CHECK (top != NULL, "rgba: toplevel");
      if (top)
	{
	  gdk_window_show (top);
	  m7_drain ();
	  gdk_window_shape_combine_mask (top, bm, 4, 4);
	  m7_drain ();
	  m7_shape_info (top, &s);
	  M7_CHECK (s.shaped && !s.opaque_sent, "rgba: no opaque region");
	  M7_CHECK (s.input_sent && m7_rects_equal (s.input, s.n_input, b_want, 2),
		    "rgba: input region is B");
	  gdk_window_destroy (top);
	  m7_drain ();
	}
    }

  /* 7. empty region */
  empty = gdk_region_new ();
  gdk_window_shape_combine_region (w, empty, 0, 0);
  m7_drain ();
  m7_shape_info (w, &s);
  M7_CHECK (s.shaped && s.n_bounding == 0, "empty region: shaped, no rectangles");
  M7_CHECK (s.input_sent && s.n_input == 0, "empty region: empty input region");
  m7_repaint (w);
  m7_pixel_class (w, 6, 6, FALSE, "empty region clears the pixel");
  gdk_window_shape_combine_region (w, NULL, 0, 0);
  m7_drain ();
  gdk_region_destroy (empty);
  m7_repaint (w);

  /* 8. shape before show */
  memset (&attr, 0, sizeof attr);
  attr.window_type = GDK_WINDOW_TOPLEVEL;
  attr.wclass = GDK_INPUT_OUTPUT;
  attr.width = 64;
  attr.height = 64;
  top = gdk_window_new (gdk_get_default_root_window (), &attr, 0);
  M7_CHECK (top != NULL, "before show: toplevel");
  if (top)
    {
      gdk_window_shape_combine_mask (top, bm, 4, 4);
      gdk_window_show (top);
      m7_drain ();
      gdk_flush ();
      m7_drain ();
      m7_shape_info (top, &s);
      M7_CHECK (s.has_surface, "before show: surface exists after show");
      M7_CHECK (s.shaped && m7_rects_equal (s.bounding, s.n_bounding, b_want, 2),
		"before show: bounding shape kept");
      M7_CHECK (s.opaque_sent && m7_rects_equal (s.opaque, s.n_opaque, b_want, 2),
		"before show: opaque region sent");
      M7_CHECK (s.input_sent && m7_rects_equal (s.input, s.n_input, b_want, 2),
		"before show: input region sent");
      M7_EQ_INT (s.opaque_requests, 1, "before show: one opaque request");
      M7_EQ_INT (s.input_requests, 1, "before show: one input request");

      /* 9. a child shape changes nothing in the toplevel */
      attr.window_type = GDK_WINDOW_CHILD;
      attr.x = 8;
      attr.y = 8;
      attr.width = 32;
      attr.height = 32;
      child = gdk_window_new (top, &attr, GDK_WA_X | GDK_WA_Y);
      if (child)
	{
	  gdk_window_show (child);
	  m7_drain ();
	  gdk_window_shape_combine_mask (child, bi, 0, 0);
	  gdk_window_input_shape_combine_mask (child, bi, 0, 0);
	  m7_drain ();
	  m7_shape_info (top, &s2);
	  M7_CHECK (s2.shaped == s.shaped && s2.n_bounding == s.n_bounding &&
		    s2.n_opaque == s.n_opaque && s2.n_input == s.n_input &&
		    s2.opaque_requests == s.opaque_requests &&
		    s2.input_requests == s.input_requests,
		    "child shape leaves the toplevel info alone");
	  gdk_window_destroy (child);
	}
      gdk_window_destroy (top);
      m7_drain ();
    }

  g_object_unref (bm);
  g_object_unref (bi);
}

/* ---- fonts ---- */

#define M7_PW 100
#define M7_PH 50

typedef struct { int count, x0, y0, x1, y1; } M7Ink;

/* fills the whole target with black (pixel 0) or white (pixel @white) */
static void
m7_fill (GdkDrawable *pm, GdkGC *gc, guint32 pixel, guint16 level)
{
  GdkColor c;

  c.pixel = pixel;
  c.red = c.green = c.blue = level;
  gdk_gc_set_foreground (gc, &c);
  gdk_draw_rectangle (pm, gc, TRUE, 0, 0, M7_PW, M7_PH);
}

/* reads the target (24 bit colour masked to 0xffffff); ink is every pixel
 * that differs from @bg */
static M7Ink
m7_read (GdkDrawable *pm, guint32 *buf, guint32 bg)
{
  GdkImage *img = gdk_drawable_get_image (pm, 0, 0, M7_PW, M7_PH);
  M7Ink ink = { 0, M7_PW, M7_PH, -1, -1 };
  int x, y;

  M7_CHECK (img != NULL, "font: get_image");
  if (!img)
    return ink;
  for (y = 0; y < M7_PH; y++)
    for (x = 0; x < M7_PW; x++)
      {
	guint32 p = gdk_image_get_pixel (img, x, y) & 0xffffff;

	if (buf)
	  buf[y * M7_PW + x] = p;
	if (p != bg)
	  {
	    ink.count++;
	    ink.x0 = MIN (ink.x0, x);
	    ink.y0 = MIN (ink.y0, y);
	    ink.x1 = MAX (ink.x1, x);
	    ink.y1 = MAX (ink.y1, y);
	  }
      }
  g_object_unref (img);
  return ink;
}

static GdkFont *
m7_font (const char *desc_str)
{
  PangoFontDescription *desc = pango_font_description_from_string (desc_str);
  GdkFont *f = gdk_font_from_description_for_display (m7_d, desc);

  pango_font_description_free (desc);
  return f;
}

static void
m7_fonts (void)
{
  GdkFont *font, *bold, *fs;
  GdkPixmap *pm, *bm;
  GdkGC *gc, *bgc, *wgc;
  GdkColor black = { 0, 0, 0, 0 }, red = { 0, 0xffff, 0, 0 };
  GdkColormap *cm = gdk_drawable_get_colormap (main_window);
  GDKWTestFontInfo fi;
  gint l, r, wd, a, d;
  M7Ink ink, ink2, ink1;
  static guint32 buf1[M7_PW * M7_PH], buf2[M7_PW * M7_PH];
  GdkRectangle clip = { 20, 0, 20, 50 };
  GdkWChar wc[6];
  const char *hello = "Hello";
  int i, all_red, exact_red;
  const guint32 white = 0xffffff;

  gdk_colormap_alloc_color (cm, &black, FALSE, TRUE);
  gdk_colormap_alloc_color (cm, &red, FALSE, TRUE);

  font = m7_font ("Sans 12");
  M7_CHECK (font != NULL, "font: Sans 12");
  if (!font)
    return;

  /* 1. font info */
  M7_HOOK (_gdk_wayland_test_font_info (font, &fi), "font info");
  M7_CHECK (fi.is_wayland_font, "font: is a Wayland font");
  M7_CHECK (fi.dpi == 96.0, "font: 96 dpi");
  M7_CHECK (fi.hint_metrics_on, "font: hint metrics on");
  M7_CHECK (fi.ascent == font->ascent && fi.descent == font->descent,
	    "font: info ascent and descent equal the GdkFont fields");

  /* 2. extents */
  gdk_string_extents (font, hello, &l, &r, &wd, &a, &d);
  M7_EQ_INT (wd, gdk_string_width (font, hello), "font: extents width equals string_width");
  M7_CHECK (l <= r, "font: lbearing <= rbearing");
  M7_CHECK (a > 0 && d >= 0, "font: ascent positive, descent not negative");
  M7_CHECK (a + d <= font->ascent + font->descent + 2, "font: extents within font metrics");

  pm = gdk_pixmap_new (main_window, M7_PW, M7_PH, 24);
  M7_CHECK (pm != NULL, "font: pixmap");
  if (!pm)
    {
      gdk_font_unref (font);
      return;
    }
  gc = gdk_gc_new (pm);

  /* 3. position and size of the ink */
  m7_fill (pm, gc, white, 0xffff);
  gdk_gc_set_foreground (gc, &black);
  gdk_draw_string (pm, font, gc, 10, 30, hello);
  ink = m7_read (pm, buf1, white);
  M7_CHECK (ink.count >= 30, "text: at least 30 ink pixels");
  M7_CHECK (ink.x0 >= 10 + l - 1 && ink.x0 <= 10 + l + 2, "text: left edge");
  M7_CHECK (ink.x1 >= 10 + r - 3 && ink.x1 <= 10 + r, "text: right edge");
  M7_CHECK (ink.y0 >= 30 - a - 1 && ink.y0 <= 30 - a + 2, "text: top edge");
  M7_CHECK (ink.y1 <= 30 + d, "text: bottom edge");
  ink1 = ink;

  /* 4. width consistency: "HI" equals "H" then "I" */
  m7_fill (pm, gc, white, 0xffff);
  gdk_gc_set_foreground (gc, &black);
  gdk_draw_string (pm, font, gc, 10, 30, "HI");
  m7_read (pm, buf1, white);
  m7_fill (pm, gc, white, 0xffff);
  gdk_gc_set_foreground (gc, &black);
  gdk_draw_string (pm, font, gc, 10, 30, "H");
  gdk_draw_string (pm, font, gc, 10 + gdk_string_width (font, "H"), 30, "I");
  m7_read (pm, buf2, white);
  M7_CHECK (memcmp (buf1, buf2, sizeof buf1) == 0, "text: HI equals H then I");

  /* 5. wide characters */
  for (i = 0; i < 5; i++)
    wc[i] = hello[i];
  wc[5] = 0;
  m7_fill (pm, gc, white, 0xffff);
  gdk_gc_set_foreground (gc, &black);
  gdk_draw_string (pm, font, gc, 10, 30, hello);
  m7_read (pm, buf1, white);
  m7_fill (pm, gc, white, 0xffff);
  gdk_gc_set_foreground (gc, &black);
  gdk_draw_text_wc (pm, font, gc, 10, 30, wc, 5);
  m7_read (pm, buf2, white);
  M7_CHECK (memcmp (buf1, buf2, sizeof buf1) == 0, "text: draw_text_wc equals draw_string");

  /* 6. clip */
  m7_fill (pm, gc, white, 0xffff);
  gdk_gc_set_foreground (gc, &black);
  gdk_gc_set_clip_rectangle (gc, &clip);
  gdk_draw_string (pm, font, gc, 10, 30, "Hello World");
  gdk_gc_set_clip_rectangle (gc, NULL);
  ink2 = m7_read (pm, buf1, white);
  M7_CHECK (ink2.count > 0, "clip: ink inside");
  M7_CHECK (ink2.count == 0 || (ink2.x0 >= 20 && ink2.x1 < 40), "clip: no ink outside [20,40)");

  /* 7. colour */
  bold = m7_font ("Sans Bold 24");
  M7_CHECK (bold != NULL, "colour: Sans Bold 24");
  if (bold)
    {
      m7_fill (pm, gc, white, 0xffff);
      gdk_gc_set_foreground (gc, &red);
      gdk_draw_string (pm, bold, gc, 10, 40, "H");
      m7_read (pm, buf1, white);
      all_red = 1;
      exact_red = 0;
      ink2.count = 0;
      for (i = 0; i < M7_PW * M7_PH; i++)
	{
	  guint32 p = buf1[i];
	  int pr = (p >> 16) & 0xff, pg = (p >> 8) & 0xff, pb = p & 0xff;

	  if (p == white)
	    continue;
	  ink2.count++;
	  if (pr < pg || pr < pb)
	    all_red = 0;
	  if (p == 0xff0000)
	    exact_red = 1;
	}
      M7_CHECK (ink2.count > 0, "colour: ink drawn");
      M7_CHECK (all_red, "colour: every ink pixel is reddish");
      M7_CHECK (exact_red, "colour: a pure 0xff0000 pixel exists");
      gdk_font_unref (bold);
    }

  /* 8. depth 1 */
  bm = gdk_pixmap_new (NULL, M7_PW, M7_PH, 1);
  M7_CHECK (bm != NULL, "bitmap: created");
  if (bm)
    {
      GdkColor c0 = { 0, 0, 0, 0 }, c1 = { 1, 0xffff, 0xffff, 0xffff };

      bgc = gdk_gc_new (bm);
      gdk_gc_set_foreground (bgc, &c0);
      gdk_draw_rectangle (bm, bgc, TRUE, 0, 0, M7_PW, M7_PH);
      gdk_gc_set_foreground (bgc, &c1);
      gdk_draw_string (bm, font, bgc, 10, 30, hello);
      ink2 = m7_read (bm, NULL, 0);
      M7_CHECK (ink2.count >= 30, "bitmap text: at least 30 ink pixels");
      M7_CHECK (abs (ink2.x0 - ink1.x0) <= 1 && abs (ink2.x1 - ink1.x1) <= 1 &&
		abs (ink2.y0 - ink1.y0) <= 1 && abs (ink2.y1 - ink1.y1) <= 1,
		"bitmap text: box within 1 pixel of the colour box");
      g_object_unref (bgc);
      g_object_unref (bm);
    }

  /* 9. window target and edge cases */
  wgc = gdk_gc_new (main_window);
  gdk_gc_set_foreground (wgc, &black);
  gdk_draw_string (main_window, font, wgc, 300, 300, "Window text");
  gdk_flush ();
  gdk_draw_text (pm, font, gc, 10, 30, "abc", 0);
  gdk_draw_text (pm, font, gc, 10, 30, "", 0);
  gdk_draw_string (pm, font, gc, 10, 30, "");
  gdk_draw_string (pm, font, gc, 10, 30, "\xe9");
  M7_CHECK (gdk_string_width (font, "\xe9") > 0, "edge: Latin-1 byte has a width");
  M7_EQ_INT (gdk_text_width (font, "abc", 0), 0, "edge: zero length has no width");
  fs = gdk_fontset_load_for_display (m7_d, "Sans 10");
  if (fs)
    {
      gdk_draw_string (pm, fs, gc, 10, 30, "fontset");
      M7_CHECK (gdk_string_width (fs, "fontset") > 0, "edge: fontset width");
      gdk_font_unref (fs);
    }
  g_object_unref (wgc);
  m7_repaint (main_window);

  g_object_unref (gc);
  g_object_unref (pm);
  gdk_font_unref (font);
}

static int
m7_exercise (void)
{
  m7_d = gdk_display_get_default ();

  m7_cursors ();
  m7_cursor_lifetime ();
  m7_shapes ();
  m7_fonts ();

  m7_drain ();
  gdk_window_invalidate_rect (main_window, NULL, TRUE);

  if (m7_failures)
    g_printerr ("ydk_demo: M7: FAIL (%d)\n", m7_failures);
  else
    g_print ("ydk_demo: M7: PASS\n");

  return m7_failures;
}


/* ---- M8 exercise ------------------------------------------------------
 *
 * Selection, clipboard and drag and drop (gdkm8-wayland.h).  Headless Weston
 * has no seat, so there is no wl_data_device: the protocol side runs on the
 * VIRTUAL data device (_gdk_wayland_test_dd_init), which calls the real
 * listeners with fake proxies and records every request.  The recorder below
 * doubles as the application: it answers SELECTION_REQUEST exactly like ytk's
 * _gtk_selection_request (wrap the requestor with gdk_window_foreign_new_for_
 * display, gdk_property_change, gdk_selection_send_notify_for_display).
 * With a real device the protocol parts print SKIP; the property and loopback
 * parts always run.
 *
 *   A = main_window (toplevel, registered for DnD)   B, C, D other toplevels
 *   ipc = an unmapped TEMP window, the "GtkInvisible" of a drag source
 */

static int m8_failures;
static int m8_checks;

#define M8_CHECK(cond, what)						\
  G_STMT_START {							\
    m8_checks++;							\
    if (!(cond))							\
      {									\
	g_printerr ("ydk_demo: M8 check failed: %s\n", what);		\
	m8_failures++;							\
      }									\
  } G_STMT_END

#define M8_EQ_INT(got, want, what)					\
  G_STMT_START {							\
    m8_checks++;							\
    long g_ = (long) (got), w_ = (long) (want);				\
    if (g_ != w_)							\
      {									\
	g_printerr ("ydk_demo: M8 check failed: %s: got %ld want %ld\n", \
		    what, g_, w_);					\
	m8_failures++;							\
      }									\
  } G_STMT_END

#define M8_HOOK(call, what)						\
  G_STMT_START {							\
    m8_checks++;							\
    if (!(call))							\
      {									\
	g_printerr ("ydk_demo: M8 check failed: %s: hook refused\n", what); \
	m8_failures++;							\
      }									\
  } G_STMT_END

#define M8_ATOM(s)  gdk_atom_intern (s, FALSE)
#define M8_SELPROP  M8_ATOM ("GDK_SELECTION")
#define M8_CLIP     GDK_SELECTION_CLIPBOARD
#define M8_PRIMARY  GDK_SELECTION_PRIMARY
#define M8_XDND     M8_ATOM ("XdndSelection")
#define M8_BTN_LEFT   0x110
#define M8_BTN_RIGHT  0x111

static GdkDisplay *m8_d;
static GdkWindow  *m8_A, *m8_B, *m8_C, *m8_ipc;
static GQueue      m8_events = G_QUEUE_INIT;
static guint32     m8_time = 5000;
static guint8     *m8_big;
static gsize       m8_big_len;
static const gchar m8_small[] = "h\xc3\xa9llo w\xc3\xb6rld \xe2\x9c\x93\nline2";

/* ---- the application: a scriptable selection owner ---------------------- */

typedef struct
{
  GdkAtom        target;
  GdkAtom        type;
  const guint8  *data;
  gsize          len;
} M8Row;

enum { M8_ANSWER, M8_SILENT, M8_REFUSE, M8_INCR, M8_DESTROY };

static struct
{
  gboolean enabled;
  gint     mode;
  M8Row    rows[8];
  gint     n_rows;
  GdkAtom  targets[16];
  gint     n_targets;
  guint    answered;
} m8_resp;

static void
m8_resp_off (void)
{
  memset (&m8_resp, 0, sizeof m8_resp);
}

static void
m8_resp_reset (void)
{
  m8_resp_off ();
  m8_resp.enabled = TRUE;
}

static void
m8_resp_add (const gchar *target, const gchar *type, const guint8 *data,
	     gsize len)
{
  M8Row *r = &m8_resp.rows[m8_resp.n_rows++];

  r->target = M8_ATOM (target);
  r->type = M8_ATOM (type);
  r->data = data;
  r->len = len;
}

/* NULL terminated vector of target names the TARGETS reply carries */
static void
m8_resp_targets (const gchar * const *names)
{
  m8_resp.n_targets = 0;
  for (; *names != NULL; names++)
    m8_resp.targets[m8_resp.n_targets++] = M8_ATOM (*names);
}

static void
m8_answer (GdkEvent *ev)
{
  GdkNativeWindow id = ev->selection.requestor;
  GdkAtom sel = ev->selection.selection, target = ev->selection.target;
  GdkAtom prop = ev->selection.property;
  const M8Row *row = NULL;
  GdkWindow *req;
  gint i;

  if (m8_resp.mode == M8_SILENT)
    return;
  if (m8_resp.mode == M8_DESTROY)
    {
      m8_resp.mode = M8_ANSWER;
      gdk_window_destroy (ev->selection.window);
      return;
    }

  m8_resp.answered++;
  if (m8_resp.mode == M8_REFUSE)
    {
      gdk_selection_send_notify_for_display (m8_d, id, sel, target, GDK_NONE,
					     ev->selection.time);
      return;
    }

  req = gdk_window_foreign_new_for_display (m8_d, id);
  if (target != M8_ATOM ("TARGETS"))
    for (i = 0; i < m8_resp.n_rows; i++)
      if (m8_resp.rows[i].target == target)
	row = &m8_resp.rows[i];

  if (req == NULL || prop == GDK_NONE ||
      (target != M8_ATOM ("TARGETS") && row == NULL))
    {
      gdk_selection_send_notify_for_display (m8_d, id, sel, target, GDK_NONE,
					     ev->selection.time);
      if (req)
	g_object_unref (req);
      return;
    }

  if (target == M8_ATOM ("TARGETS"))
    gdk_property_change (req, prop, M8_ATOM ("ATOM"), 32, GDK_PROP_MODE_REPLACE,
			 (const guchar *) m8_resp.targets, m8_resp.n_targets);
  else
    gdk_property_change (req, prop,
			 m8_resp.mode == M8_INCR ? M8_ATOM ("INCR") : row->type,
			 8, GDK_PROP_MODE_REPLACE, row->data, row->len);
  gdk_selection_send_notify_for_display (m8_d, id, sel, target, prop,
					 ev->selection.time);
  g_object_unref (req);
}

static void
m8_recorder (GdkEvent *event, gpointer data)
{
  g_queue_push_tail (&m8_events, gdk_event_copy (event));
  if (event->type == GDK_SELECTION_REQUEST && m8_resp.enabled)
    m8_answer (event);
}

/* ---- event queue helpers ------------------------------------------------ */

static void
m8_drain (void)
{
  while (g_main_context_iteration (NULL, FALSE))
    ;
}

static void
m8_clear (void)
{
  GdkEvent *ev;

  while ((ev = g_queue_pop_head (&m8_events)) != NULL)
    gdk_event_free (ev);
}

static GdkEvent *
m8_find (GdkEventType type, GdkWindow *window)
{
  GList *l;

  for (l = m8_events.head; l; l = l->next)
    {
      GdkEvent *ev = l->data;

      if (ev->type == type && (window == NULL || ev->any.window == window))
	return ev;
    }
  return NULL;
}

static int
m8_count (GdkEventType type, GdkWindow *window)
{
  GList *l;
  int n = 0;

  for (l = m8_events.head; l; l = l->next)
    {
      GdkEvent *ev = l->data;

      if (ev->type == type && (window == NULL || ev->any.window == window))
	n++;
    }
  return n;
}

static GdkEvent *
m8_find_req (GdkWindow *window, GdkAtom target)
{
  GList *l;

  for (l = m8_events.head; l; l = l->next)
    {
      GdkEvent *ev = l->data;

      if (ev->type == GDK_SELECTION_REQUEST &&
	  (window == NULL || ev->selection.window == window) &&
	  ev->selection.target == target)
	return ev;
    }
  return NULL;
}

static void
m8_run_ms (guint ms)
{
  gint64 end = g_get_monotonic_time () + (gint64) ms * 1000;

  do
    {
      m8_drain ();
      g_usleep (500);
    }
  while (g_get_monotonic_time () < end);
  m8_drain ();
}

static gboolean
m8_wait_notify (GdkWindow *window, guint ms)
{
  gint64 end = g_get_monotonic_time () + (gint64) ms * 1000;

  for (;;)
    {
      m8_drain ();
      if (m8_find (GDK_SELECTION_NOTIFY, window))
	return TRUE;
      if (g_get_monotonic_time () > end)
	return FALSE;
      g_usleep (500);
    }
}

/* ---- paste -------------------------------------------------------------- */

typedef struct
{
  gboolean notified;
  GdkAtom  property;
  GdkAtom  type;
  gint     format;
  gint     len;
  guchar  *data;
} M8Paste;

/* gdk_selection_convert + wait for the NOTIFY + read the property and delete
 * it, like gtk_selection_data_get.  The events of the transfer stay in the
 * queue for the caller. */
static void
m8_paste (GdkWindow *req, GdkAtom sel, const gchar *target, guint ms,
	  M8Paste *p)
{
  GdkEvent *ev;

  memset (p, 0, sizeof *p);
  m8_drain ();
  m8_clear ();
  gdk_selection_convert (req, sel, M8_ATOM (target), GDK_CURRENT_TIME);
  M8_CHECK (m8_find (GDK_SELECTION_NOTIFY, req) == NULL,
	    "paste: NOTIFY delivered from inside convert");
  p->notified = m8_wait_notify (req, ms);
  if (!p->notified)
    return;

  ev = m8_find (GDK_SELECTION_NOTIFY, req);
  M8_CHECK (ev->selection.selection == sel && ev->selection.target == M8_ATOM (target),
	    "paste: NOTIFY selection or target differs");
  M8_CHECK (m8_count (GDK_SELECTION_NOTIFY, req) == 1,
	    "paste: more than one NOTIFY");
  p->property = ev->selection.property;
  if (p->property != GDK_NONE)
    {
      p->len = gdk_selection_property_get (req, &p->data, &p->type, &p->format);
      gdk_property_delete (req, M8_SELPROP);
    }
}

static void
m8_paste_free (M8Paste *p)
{
  g_free (p->data);
  p->data = NULL;
}

static gboolean
m8_paste_is (const M8Paste *p, const guint8 *data, gsize len)
{
  return p->notified && p->property == M8_SELPROP && p->data != NULL &&
    (gsize) p->len == len && memcmp (p->data, data, len) == 0 &&
    p->data[len] == 0;
}

/* @p carries an ATOM array equal to the NULL terminated @names */
static gboolean
m8_atoms_are (const M8Paste *p, const gchar * const *names)
{
  gint i, n = 0;
  GdkAtom *atoms = (GdkAtom *) p->data;

  while (names[n] != NULL)
    n++;
  if (!p->notified || p->property != M8_SELPROP || p->data == NULL ||
      p->type != M8_ATOM ("ATOM") || p->format != 32 ||
      (gsize) p->len != n * sizeof (GdkAtom))
    return FALSE;
  for (i = 0; i < n; i++)
    if (atoms[i] != M8_ATOM (names[i]))
      return FALSE;
  return TRUE;
}

static gboolean
m8_strv_is (gchar **v, const gchar * const *want)
{
  gint i;

  if (v == NULL)
    return FALSE;
  for (i = 0; want[i] != NULL; i++)
    if (v[i] == NULL || strcmp (v[i], want[i]) != 0)
      return FALSE;
  return v[i] == NULL;
}

/* @info mimes of set_selection[@kind] equal the NULL terminated @want */
static gboolean
m8_set_mimes_are (const GDKWTestDDInfo *info, gint kind,
		  const gchar * const *want)
{
  guint i;

  for (i = 0; want[i] != NULL; i++)
    if (i >= info->set_selection[kind].n_mimes ||
	strcmp (info->set_selection[kind].mimes[i], want[i]) != 0)
      return FALSE;
  return i == info->set_selection[kind].n_mimes;
}

static GdkWindow *
m8_toplevel (gint w, gint h)
{
  GdkWindowAttr attr;

  memset (&attr, 0, sizeof attr);
  attr.window_type = GDK_WINDOW_TOPLEVEL;
  attr.wclass = GDK_INPUT_OUTPUT;
  attr.width = w;
  attr.height = h;
  attr.event_mask = GDK_EXPOSURE_MASK;
  return gdk_window_new (gdk_get_default_root_window (), &attr, 0);
}

static GdkWindow *
m8_temp (void)
{
  GdkWindowAttr attr;

  memset (&attr, 0, sizeof attr);
  attr.window_type = GDK_WINDOW_TEMP;
  attr.wclass = GDK_INPUT_OUTPUT;
  attr.width = 1;
  attr.height = 1;
  return gdk_window_new (gdk_get_default_root_window (), &attr, 0);
}

static GDKWTestDDInfo
m8_dd (void)
{
  GDKWTestDDInfo info;

  memset (&info, 0, sizeof info);
  if (!_gdk_wayland_test_dd_info (m8_d, &info))
    {
      g_printerr ("ydk_demo: M8 check failed: dd_info refused\n");
      m8_failures++;
    }
  return info;
}

static GDKWTestSelectionInfo
m8_sel (GdkAtom selection)
{
  GDKWTestSelectionInfo si;

  memset (&si, 0, sizeof si);
  if (!_gdk_wayland_test_selection_info (m8_d, selection, &si))
    {
      g_printerr ("ydk_demo: M8 check failed: selection_info refused\n");
      m8_failures++;
    }
  return si;
}

static GdkEvent *
m8_check_no_events (const gchar *what)
{
  m8_drain ();
  if (g_queue_get_length (&m8_events) != 0)
    {
      g_printerr ("ydk_demo: M8 check failed: %s: %u unexpected event(s)\n",
		  what, g_queue_get_length (&m8_events));
      m8_failures++;
    }
  return NULL;
}

static GList *
m8_targets (const gchar *first, ...)
{
  GList *l = NULL;
  const gchar *s;
  va_list ap;

  va_start (ap, first);
  for (s = first; s != NULL; s = va_arg (ap, const gchar *))
    l = g_list_append (l, GDK_ATOM_TO_POINTER (M8_ATOM (s)));
  va_end (ap);
  return l;
}

/* ---- P: property table (no compositor) --------------------------------- */

static void
m8_property (void)
{
  GdkWindow *w = m8_B;
  GdkAtom p = M8_ATOM ("YDK_M8_P");
  GdkAtom t = GDK_NONE;
  GdkAtom string = GDK_TARGET_STRING, cardinal = M8_ATOM ("CARDINAL");
  GdkAtom atom = M8_ATOM ("ATOM");
  gint f = 0, l = 0;
  guchar *data = NULL;
  const guint16 shorts[4] = { 1, 2, 0xfffe, 4 };
  const glong longs[3] = { 7, 0x7fffffff, -3 };
  GdkAtom atoms[3];
  GdkWindow *tmp;
  gint i;

  M8_EQ_INT (_gdk_wayland_test_property_count (w), 0, "P: starts empty");

  /* format 8, slicing in 32 bit units */
  gdk_property_change (w, p, string, 8, GDK_PROP_MODE_REPLACE,
		       (const guchar *) "abcdefgh", 8);
  M8_EQ_INT (_gdk_wayland_test_property_count (w), 1, "P: count after change");
  M8_EQ_INT (_gdk_wayland_test_property_bytes (w), 8, "P: bytes after change");
  M8_CHECK (gdk_property_get (w, p, GDK_NONE, 0, 100, FALSE, &t, &f, &l, &data)
	    && t == string && f == 8 && l == 8 && data
	    && memcmp (data, "abcdefgh", 8) == 0, "P: format 8 round trip");
  g_free (data);
  data = NULL;
  M8_CHECK (gdk_property_get (w, p, GDK_NONE, 0, 4, FALSE, &t, &f, &l, &data)
	    && l == 4 && memcmp (data, "abcd", 4) == 0, "P: length 4 slice");
  g_free (data);
  data = NULL;
  M8_CHECK (gdk_property_get (w, p, GDK_NONE, 0, 1, FALSE, &t, &f, &l, &data)
	    && l == 4 && memcmp (data, "abcd", 4) == 0,
	    "P: length 1 rounds up to 4 bytes");
  g_free (data);
  data = NULL;
  M8_CHECK (gdk_property_get (w, p, GDK_NONE, 1, 100, FALSE, &t, &f, &l, &data)
	    && l == 4 && memcmp (data, "efgh", 4) == 0, "P: offset 1 slice");
  g_free (data);
  data = NULL;
  M8_CHECK (!gdk_property_get (w, p, GDK_NONE, 3, 100, FALSE, &t, &f, &l, &data),
	    "P: offset beyond the end fails");
  M8_CHECK (!gdk_property_get (w, M8_ATOM ("YDK_M8_NOPE"), GDK_NONE, 0, 4,
			       FALSE, &t, &f, &l, &data),
	    "P: unknown property fails");

  /* type mismatch: FALSE, actual type and format filled */
  t = GDK_NONE;
  f = 0;
  M8_CHECK (!gdk_property_get (w, p, cardinal, 0, 100, FALSE, &t, &f, &l, &data)
	    && t == string && f == 8, "P: type mismatch fills the actual type");

  /* PREPEND / APPEND */
  gdk_property_change (w, p, string, 8, GDK_PROP_MODE_REPLACE,
		       (const guchar *) "bcd", 3);
  gdk_property_change (w, p, string, 8, GDK_PROP_MODE_APPEND,
		       (const guchar *) "ef", 2);
  gdk_property_change (w, p, string, 8, GDK_PROP_MODE_PREPEND,
		       (const guchar *) "a", 1);
  gdk_property_change (w, p, cardinal, 8, GDK_PROP_MODE_APPEND,
		       (const guchar *) "ZZ", 2);   /* type mismatch: ignored */
  gdk_property_change (w, p, string, 16, GDK_PROP_MODE_APPEND,
		       (const guchar *) "ZZ", 1);   /* format mismatch: ignored */
  M8_CHECK (gdk_property_get (w, p, GDK_NONE, 0, 100, FALSE, &t, &f, &l, &data)
	    && l == 6 && memcmp (data, "abcdef", 6) == 0,
	    "P: prepend and append, mismatches ignored");
  g_free (data);
  data = NULL;

  /* pdelete deletes when the whole value was returned */
  M8_CHECK (gdk_property_get (w, p, GDK_NONE, 0, 4, TRUE, &t, &f, &l, &data)
	    && l == 4, "P: partial get with pdelete");
  g_free (data);
  data = NULL;
  M8_EQ_INT (_gdk_wayland_test_property_count (w), 1,
	     "P: partial get keeps the property");
  M8_CHECK (gdk_property_get (w, p, GDK_NONE, 0, 100, TRUE, &t, &f, &l, &data)
	    && l == 6, "P: full get with pdelete");
  g_free (data);
  data = NULL;
  M8_EQ_INT (_gdk_wayland_test_property_count (w), 0,
	     "P: full get with pdelete deletes");
  M8_EQ_INT (_gdk_wayland_test_property_bytes (w), 0, "P: bytes back to 0");

  /* format 16 */
  gdk_property_change (w, p, cardinal, 16, GDK_PROP_MODE_REPLACE,
		       (const guchar *) shorts, 4);
  M8_CHECK (gdk_property_get (w, p, cardinal, 0, 100, FALSE, &t, &f, &l, &data)
	    && f == 16 && l == 8 && memcmp (data, shorts, 8) == 0,
	    "P: format 16 round trip");
  g_free (data);
  data = NULL;

  /* format 32 is C longs; offset and length count items (32 bit wire units) */
  gdk_property_change (w, p, cardinal, 32, GDK_PROP_MODE_REPLACE,
		       (const guchar *) longs, 3);
  M8_CHECK (gdk_property_get (w, p, GDK_NONE, 0, 100, FALSE, &t, &f, &l, &data)
	    && f == 32 && l == 3 * (gint) sizeof (long)
	    && memcmp (data, longs, 3 * sizeof (long)) == 0,
	    "P: format 32 round trip");
  g_free (data);
  data = NULL;
  M8_CHECK (gdk_property_get (w, p, GDK_NONE, 1, 4, FALSE, &t, &f, &l, &data)
	    && l == (gint) sizeof (long)
	    && memcmp (data, &longs[1], sizeof (long)) == 0,
	    "P: format 32 partial read (offset 1, one item)");
  g_free (data);
  data = NULL;
  M8_CHECK (gdk_property_get (w, p, GDK_NONE, 1, 100, FALSE, &t, &f, &l, &data)
	    && l == 2 * (gint) sizeof (long)
	    && memcmp (data, &longs[1], 2 * sizeof (long)) == 0,
	    "P: format 32 partial read (offset 1, rest)");
  g_free (data);
  data = NULL;
  M8_CHECK (gdk_property_get (w, p, GDK_NONE, 0, 8, FALSE, &t, &f, &l, &data)
	    && l == 2 * (gint) sizeof (long)
	    && memcmp (data, longs, 2 * sizeof (long)) == 0,
	    "P: format 32 length 8 bytes is 2 wire units");
  g_free (data);
  data = NULL;

  /* ATOM stays GdkAtom verbatim */
  atoms[0] = string;
  atoms[1] = cardinal;
  atoms[2] = M8_ATOM ("UTF8_STRING");
  gdk_property_change (w, p, atom, 32, GDK_PROP_MODE_REPLACE,
		       (const guchar *) atoms, 3);
  M8_CHECK (gdk_property_get (w, p, atom, 0, 100, FALSE, &t, &f, &l, &data)
	    && t == atom && f == 32 && l == 3 * (gint) sizeof (GdkAtom)
	    && memcmp (data, atoms, sizeof atoms) == 0, "P: ATOM round trip");
  g_free (data);
  data = NULL;

  /* an empty property exists and returns no data */
  gdk_property_change (w, p, string, 8, GDK_PROP_MODE_REPLACE, NULL, 0);
  M8_EQ_INT (_gdk_wayland_test_property_count (w), 1, "P: empty property stored");
  M8_CHECK (gdk_property_get (w, p, GDK_NONE, 0, 100, FALSE, &t, &f, &l, &data)
	    && l == 0 && data == NULL, "P: empty property returns TRUE, no data");

  /* delete, and delete of something that is not there */
  gdk_property_delete (w, p);
  gdk_property_delete (w, p);
  M8_EQ_INT (_gdk_wayland_test_property_count (w), 0, "P: count after delete");
  M8_EQ_INT (_gdk_wayland_test_property_bytes (w), 0, "P: bytes after delete");

  /* the cap per window is 64 MiB: a change that would exceed it is dropped */
  for (i = 0; i < 3; i++)
    gdk_property_change (w, p, string, 8, GDK_PROP_MODE_REPLACE,
			 (const guchar *) "x", 1);
  gdk_property_delete (w, p);

  /* a destroyed window: FALSE, no CRITICAL, nothing stored */
  tmp = m8_toplevel (20, 20);
  g_object_ref (tmp);
  gdk_property_change (tmp, p, string, 8, GDK_PROP_MODE_REPLACE,
		       (const guchar *) "x", 1);
  M8_EQ_INT (_gdk_wayland_test_property_count (tmp), 1, "P: tmp window property");
  gdk_window_destroy (tmp);
  M8_CHECK (!gdk_property_get (tmp, p, GDK_NONE, 0, 4, FALSE, &t, &f, &l, &data),
	    "P: get on a destroyed window fails");
  gdk_property_change (tmp, p, string, 8, GDK_PROP_MODE_REPLACE,
		       (const guchar *) "x", 1);
  gdk_property_delete (tmp, p);
  g_object_unref (tmp);
}

/* ---- pure mime helpers -------------------------------------------------- */

static void
m8_helpers (void)
{
  GdkAtom t1[] = { M8_ATOM ("TARGETS"), M8_ATOM ("UTF8_STRING"),
		   GDK_TARGET_STRING, M8_ATOM ("TIMESTAMP"),
		   M8_ATOM ("text/uri-list"), M8_ATOM ("application/x-ydk"),
		   M8_ATOM ("MULTIPLE") };
  GdkAtom t2[] = { GDK_TARGET_STRING };
  GdkAtom t3[] = { M8_ATOM ("application/x-ydk"), M8_ATOM ("text/plain"),
		   M8_ATOM ("application/x-ydk"), M8_ATOM ("image/png") };
  const gchar *w1[] = { "text/plain;charset=utf-8", "text/plain", "UTF8_STRING",
			"text/uri-list", "application/x-ydk", NULL };
  const gchar *w2[] = { "text/plain;charset=utf-8", "text/plain", "UTF8_STRING",
			NULL };
  const gchar *w3[] = { "application/x-ydk", "text/plain;charset=utf-8",
			"text/plain", "UTF8_STRING", "image/png", NULL };
  const gchar *i1[] = { "text/plain;charset=utf-8", "text/uri-list",
			"application/x-ydk", NULL };
  const gchar *i2[] = { "text/plain", "text/uri-list", NULL };
  const gchar *i3[] = { "TARGETS", "", NULL };
  const gchar *i4[] = { "x-ardour/region", "UTF8_STRING", "text/plain", NULL };
  const gchar *e1[] = { "UTF8_STRING", "STRING", "text/uri-list",
			"application/x-ydk", NULL };
  const gchar *e2[] = { "UTF8_STRING", "STRING", "text/uri-list", NULL };
  const gchar *e4[] = { "x-ardour/region", "UTF8_STRING", "STRING", NULL };
  gchar **v;
  GdkAtom *a;
  gint n = -1, i;

  v = _gdk_wayland_selection_mimes_for_targets (t1, G_N_ELEMENTS (t1));
  M8_CHECK (m8_strv_is (v, w1), "helpers: outbound mimes, pseudo targets ignored");
  g_strfreev (v);
  v = _gdk_wayland_selection_mimes_for_targets (t2, G_N_ELEMENTS (t2));
  M8_CHECK (m8_strv_is (v, w2), "helpers: STRING alone offers the text group");
  g_strfreev (v);
  v = _gdk_wayland_selection_mimes_for_targets (t3, G_N_ELEMENTS (t3));
  M8_CHECK (m8_strv_is (v, w3),
	    "helpers: text group at its first member, duplicates dropped");
  g_strfreev (v);
  v = _gdk_wayland_selection_mimes_for_targets (NULL, 0);
  M8_CHECK (v != NULL && v[0] == NULL, "helpers: no targets, empty vector");
  g_strfreev (v);

  a = _gdk_wayland_selection_targets_for_mimes (i1, &n);
  M8_CHECK (a != NULL && n == 4 && a[0] == M8_ATOM (e1[0]) &&
	    a[1] == M8_ATOM (e1[1]) && a[2] == M8_ATOM (e1[2]) &&
	    a[3] == M8_ATOM (e1[3]), "helpers: inbound text/plain;charset=utf-8");
  g_free (a);
  a = _gdk_wayland_selection_targets_for_mimes (i2, &n);
  M8_CHECK (a != NULL && n == 3 && a[0] == M8_ATOM (e2[0]) &&
	    a[1] == M8_ATOM (e2[1]) && a[2] == M8_ATOM (e2[2]),
	    "helpers: inbound text/plain");
  g_free (a);
  a = _gdk_wayland_selection_targets_for_mimes (i3, &n);
  M8_CHECK (a == NULL && n == 0, "helpers: inbound pseudo names ignored");
  g_free (a);
  a = _gdk_wayland_selection_targets_for_mimes (i4, &n);
  M8_CHECK (a != NULL && n == 3, "helpers: inbound order");
  for (i = 0; a != NULL && i < 3 && i < n; i++)
    M8_CHECK (a[i] == M8_ATOM (e4[i]), "helpers: inbound order entry");
  g_free (a);
}

/* ---- S1/S6: loopback copy A, paste B ------------------------------------ */

/* @probe: a TARGETS probe, a wl source and set_selection[@kind] are expected
 * (a usable device and a protocol selection); otherwise none of them. */
static void
m8_loopback (GdkAtom sel, gboolean probe, gint kind, const gchar *tag)
{
  const gchar *tnames[] = { "UTF8_STRING", "application/x-ydk", NULL };
  const gchar *want_mimes[] = { "text/plain;charset=utf-8", "text/plain",
				"UTF8_STRING", "application/x-ydk", NULL };
  static const guint8 ydk_bytes[] = { 1, 2, 0, 0xff, 0x80, 0 };
  const guint8 *pay[2] = { (const guint8 *) m8_small, m8_big };
  gsize pay_len[2] = { strlen (m8_small), 0 };
  GDKWTestDDInfo info;
  GDKWTestSelectionInfo si;
  M8Paste p;
  GdkEvent *ev;
  guint probes0;
  gchar what[160];
  gint i;

  pay_len[1] = m8_big_len;
  probes0 = m8_sel (sel).probes_started;

  for (i = 0; i < 2; i++)
    {
      m8_resp_reset ();
      m8_resp_targets (tnames);
      m8_resp_add ("UTF8_STRING", "UTF8_STRING", pay[i], pay_len[i]);
      m8_resp_add ("application/x-ydk", "application/x-ydk", ydk_bytes,
		   sizeof ydk_bytes);
      m8_drain ();
      m8_clear ();
      _gdk_wayland_test_dd_reset_requests (m8_d);

      g_snprintf (what, sizeof what, "%s: owner_set (%s)", tag, i ? "big" : "small");
      M8_CHECK (gdk_selection_owner_set_for_display (m8_d, m8_A, sel,
						     GDK_CURRENT_TIME, FALSE),
		what);
      M8_CHECK (gdk_selection_owner_get_for_display (m8_d, sel) == m8_A,
		"loopback: owner_get returns the owner window");
      M8_CHECK (m8_find (GDK_SELECTION_REQUEST, NULL) == NULL,
		"loopback: the probe is asynchronous, never inside owner_set");
      m8_drain ();

      ev = m8_find_req (m8_A, M8_ATOM ("TARGETS"));
      info = m8_dd ();
      si = m8_sel (sel);
      if (probe)
	{
	  g_snprintf (what, sizeof what, "%s: TARGETS probe request", tag);
	  M8_CHECK (ev != NULL && ev->selection.requestor != 0 &&
		    ev->selection.selection == sel, what);
	  M8_EQ_INT (si.probes_started, probes0 + i + 1, "loopback: probes_started");
	  M8_EQ_INT (si.probes_completed, probes0 + i + 1,
		     "loopback: probes_completed");
	  M8_EQ_INT (info.set_selection[kind].count, 1,
		     "loopback: exactly one set_selection");
	  M8_EQ_INT (info.set_selection[kind].serial,
		     _gdk_wayland_test_dd_seat_serial (m8_d),
		     "loopback: set_selection carries the seat serial");
	  M8_CHECK (info.set_selection[kind].serial != 0,
		    "loopback: seat serial is not 0");
	  M8_CHECK (!info.set_selection[kind].cleared &&
		    m8_set_mimes_are (&info, kind, want_mimes),
		    "loopback: set_selection mimes");
	  M8_CHECK (si.source_alive, "loopback: a wl source exists");
	}
      else
	{
	  M8_CHECK (ev == NULL, "loopback: no probe without device or protocol");
	  M8_EQ_INT (si.probes_started, probes0, "loopback: probes_started unchanged");
	  M8_EQ_INT (info.set_selection[0].count + info.set_selection[1].count,
		     0, "loopback: no set_selection");
	  M8_CHECK (!si.source_alive, "loopback: no wl source");
	}

      /* paste in B */
      m8_paste (m8_B, sel, "UTF8_STRING", 5000, &p);
      g_snprintf (what, sizeof what, "%s: UTF8_STRING %s payload", tag,
		  i ? "big" : "small");
      M8_CHECK (m8_paste_is (&p, pay[i], pay_len[i]), what);
      M8_CHECK (p.type == M8_ATOM ("UTF8_STRING") && p.format == 8,
		"loopback: type UTF8_STRING format 8");
      ev = m8_find_req (m8_A, M8_ATOM ("UTF8_STRING"));
      M8_CHECK (ev != NULL, "loopback: SELECTION_REQUEST on the owner");
      if (ev)
	{
	  M8_EQ_INT (ev->selection.requestor, _gdk_wayland_test_native_id (m8_B),
		     "loopback: requestor is the native id of B");
	  M8_CHECK (ev->selection.property == M8_SELPROP &&
		    ev->selection.selection == sel,
		    "loopback: request property GDK_SELECTION");
	}
      M8_EQ_INT (_gdk_wayland_test_property_count (m8_B), 0,
		 "loopback: nothing left on the requestor");
      info = m8_dd ();
      M8_EQ_INT (info.receive_count, 0, "loopback: no compositor round trip");
      m8_paste_free (&p);
    }

  /* a binary custom target survives (NULs, high bytes) */
  m8_paste (m8_B, sel, "application/x-ydk", 5000, &p);
  M8_CHECK (m8_paste_is (&p, ydk_bytes, sizeof ydk_bytes) &&
	    p.type == M8_ATOM ("application/x-ydk"), "loopback: binary target");
  m8_paste_free (&p);

  /* an unknown target: the owner answers NONE, NOTIFY carries NONE */
  m8_paste (m8_B, sel, "application/x-nope", 5000, &p);
  M8_CHECK (p.notified && p.property == GDK_NONE,
	    "loopback: refused target gives NOTIFY with NONE");
  M8_EQ_INT (_gdk_wayland_test_property_count (m8_B), 0,
	     "loopback: refusal leaves nothing on the requestor");

  /* TARGETS reaches the application, which answers ATOM / 32 */
  m8_paste (m8_B, sel, "TARGETS", 5000, &p);
  M8_CHECK (m8_atoms_are (&p, tnames), "loopback: TARGETS is ATOM, 32, n atoms");
  m8_paste_free (&p);

  /* a requestor that is the owner itself works too */
  m8_paste (m8_A, sel, "UTF8_STRING", 5000, &p);
  M8_CHECK (m8_paste_is (&p, pay[1], pay_len[1]), "loopback: paste into the owner");
  m8_paste_free (&p);

  /* release: no CLEAR, no owner */
  m8_clear ();
  M8_CHECK (gdk_selection_owner_set_for_display (m8_d, NULL, sel,
						 GDK_CURRENT_TIME, FALSE),
	    "loopback: release owner_set (NULL)");
  m8_drain ();
  M8_CHECK (gdk_selection_owner_get_for_display (m8_d, sel) == NULL,
	    "loopback: no owner after release");
  M8_EQ_INT (m8_count (GDK_SELECTION_CLEAR, NULL), 0,
	     "loopback: release queues no SELECTION_CLEAR");
  m8_resp_off ();
}

/* ---- S2/S3: external offers --------------------------------------------- */

static void
m8_offer_targets_and_paste (GdkAtom sel, GDKWSelKind kind, const gchar *tag)
{
  const gchar *mimes[] = { "text/plain;charset=utf-8", "text/uri-list",
			   "application/x-ydk", NULL };
  const gchar *advert[] = { "UTF8_STRING", "STRING", "text/uri-list",
			    "application/x-ydk", NULL };
  GDKWTestDDInfo info, base;
  GDKWTestSelectionInfo si;
  M8Paste p;
  guint id;
  guint8 latin[] = "a\xc3\xa9\xe2\x9c\x93";    /* a, e acute, check mark */
  const guint8 latin_want[] = { 'a', 0xe9, '?' };
  const guint8 uri[] = "file:///tmp/a\r\nfile:///tmp/b\r\n";
  gchar what[160];
  guint reads0;

  base = m8_dd ();
  reads0 = base.receive_count;
  M8_CHECK (gdk_selection_owner_get_for_display (m8_d, sel) == NULL,
	    "offer: starts without a local owner");

  id = _gdk_wayland_test_dd_offer_new (m8_d, kind, 1, mimes);
  M8_CHECK (id != 0, "offer: offer_new");
  M8_CHECK (m8_dd ().offers_alive == base.offers_alive + 1,
	    "offer: offers_alive counts the pending offer");
  M8_HOOK (_gdk_wayland_test_dd_selection (m8_d, kind, id), "offer: selection");
  si = m8_sel (sel);
  M8_CHECK (si.external_offer && si.external_offer_id == id,
	    "offer: selection piece sees the external offer");
  M8_CHECK (gdk_selection_owner_get_for_display (m8_d, sel) == NULL,
	    "offer: owner_get stays NULL for an external owner");

  /* S2 TARGETS from the offer */
  m8_paste (m8_B, sel, "TARGETS", 5000, &p);
  g_snprintf (what, sizeof what, "%s: external TARGETS", tag);
  M8_CHECK (m8_atoms_are (&p, advert), what);
  M8_CHECK (m8_find_req (NULL, M8_ATOM ("TARGETS")) == NULL,
	    "offer: TARGETS asked nobody");
  m8_paste_free (&p);

  /* S3: 100 KB crosses the 64 KiB pipe */
  M8_HOOK (_gdk_wayland_test_dd_offer_data (m8_d, id, "text/plain;charset=utf-8",
					    m8_big, m8_big_len, GDKW_TEST_FEED_NORMAL),
	   "offer: offer_data");
  _gdk_wayland_test_dd_reset_requests (m8_d);
  m8_paste (m8_A, sel, "UTF8_STRING", 8000, &p);
  g_snprintf (what, sizeof what, "%s: external UTF8_STRING 100 KB", tag);
  M8_CHECK (m8_paste_is (&p, m8_big, m8_big_len), what);
  M8_CHECK (p.type == M8_ATOM ("UTF8_STRING") && p.format == 8,
	    "offer: type UTF8_STRING");
  m8_paste_free (&p);
  info = m8_dd ();
  M8_EQ_INT (info.receive_count, 1, "offer: one receive");
  M8_CHECK (strcmp (info.receive_mime, "text/plain;charset=utf-8") == 0,
	    "offer: receive mime");
  M8_EQ_INT (info.receive_kind, kind, "offer: receive kind");
  M8_EQ_INT (info.pending_reads, 0, "offer: no pipe left open");
  M8_EQ_INT (_gdk_wayland_test_property_count (m8_A), 0,
	     "offer: nothing left on the requestor");
  M8_CHECK (gdk_selection_owner_get_for_display (m8_d, sel) == NULL,
	    "offer: owner_get still NULL after paste");

  /* STRING: UTF-8 to Latin-1, '?' for what does not fit */
  M8_HOOK (_gdk_wayland_test_dd_offer_data (m8_d, id, "text/plain;charset=utf-8",
					    latin, strlen ((char *) latin),
					    GDKW_TEST_FEED_NORMAL), "offer: offer_data 2");
  m8_paste (m8_A, sel, "STRING", 5000, &p);
  M8_CHECK (m8_paste_is (&p, latin_want, sizeof latin_want) &&
	    p.type == GDK_TARGET_STRING, "offer: STRING is Latin-1 with '?'");
  m8_paste_free (&p);

  /* text/uri-list verbatim */
  M8_HOOK (_gdk_wayland_test_dd_offer_data (m8_d, id, "text/uri-list", uri,
					    strlen ((char *) uri),
					    GDKW_TEST_FEED_NORMAL), "offer: offer_data uri");
  m8_paste (m8_A, sel, "text/uri-list", 5000, &p);
  M8_CHECK (m8_paste_is (&p, uri, strlen ((char *) uri)) &&
	    p.type == M8_ATOM ("text/uri-list"), "offer: text/uri-list verbatim");
  m8_paste_free (&p);

  /* refused: unsupported target, TIMESTAMP, MULTIPLE; no receive for them */
  _gdk_wayland_test_dd_reset_requests (m8_d);
  m8_paste (m8_A, sel, "application/x-nope", 5000, &p);
  M8_CHECK (p.notified && p.property == GDK_NONE, "offer: unsupported target");
  m8_paste (m8_A, sel, "TIMESTAMP", 5000, &p);
  M8_CHECK (p.notified && p.property == GDK_NONE, "offer: TIMESTAMP refused");
  m8_paste (m8_A, sel, "MULTIPLE", 5000, &p);
  M8_CHECK (p.notified && p.property == GDK_NONE, "offer: MULTIPLE refused");
  M8_EQ_INT (m8_dd ().receive_count, 0, "offer: refusals read nothing");

  /* STALL: idle timeout, NOTIFY NONE, nothing left behind */
  M8_HOOK (_gdk_wayland_test_dd_set_timeouts (m8_d, 200, 0, 0, 0), "offer: timeouts");
  M8_HOOK (_gdk_wayland_test_dd_offer_data (m8_d, id, "text/plain;charset=utf-8",
					    m8_big, 1000, GDKW_TEST_FEED_STALL),
	   "offer: offer_data stall");
  m8_paste (m8_A, sel, "UTF8_STRING", 5000, &p);
  M8_CHECK (p.notified && p.property == GDK_NONE, "offer: stalled read gives NONE");
  M8_EQ_INT (_gdk_wayland_test_property_count (m8_A), 0,
	     "offer: stalled read leaves no property");
  M8_EQ_INT (m8_dd ().pending_reads, 0, "offer: stalled read closed its pipe");
  M8_HOOK (_gdk_wayland_test_dd_set_timeouts (m8_d, 5000, 10000, 2000, 5000),
	   "offer: timeouts restored");

  /* ERROR feed: the pipe closes at once, an empty value is a valid answer */
  M8_HOOK (_gdk_wayland_test_dd_offer_data (m8_d, id, "text/plain;charset=utf-8",
					    NULL, 0, GDKW_TEST_FEED_ERROR),
	   "offer: offer_data error");
  m8_paste (m8_A, sel, "UTF8_STRING", 5000, &p);
  M8_CHECK (p.notified && p.property == M8_SELPROP && p.len == 0 && p.data != NULL,
	    "offer: empty read gives an empty property and a non NULL buffer");
  m8_paste_free (&p);

  /* the offer goes away: the paste is refused */
  M8_HOOK (_gdk_wayland_test_dd_selection (m8_d, kind, 0), "offer: selection (0)");
  M8_CHECK (!m8_sel (sel).external_offer, "offer: cleared");
  m8_paste (m8_A, sel, "UTF8_STRING", 5000, &p);
  M8_CHECK (p.notified && p.property == GDK_NONE, "offer: refused after clear");
  M8_EQ_INT (m8_dd ().offers_alive, base.offers_alive, "offer: offer record freed");
  (void) reads0;
}

/* ---- S4: ownership change and clear -------------------------------------- */

static void
m8_ownership (GdkAtom sel, gint kind, GDKWSelKind skind, const gchar *tag)
{
  const gchar *tnames[] = { "UTF8_STRING", NULL };
  const gchar *tnames2[] = { "application/x-ydk", "text/uri-list", NULL };
  const gchar *mimes1[] = { "text/plain;charset=utf-8", "text/plain",
			    "UTF8_STRING", NULL };
  const gchar *mimes2[] = { "application/x-ydk", "text/uri-list", NULL };
  GDKWTestDDInfo info;
  GDKWTestSelectionInfo si, si1;
  guint clears0, completed0;
  gchar what[160];

  m8_resp_reset ();
  m8_resp_targets (tnames);
  m8_drain ();
  m8_clear ();

  M8_CHECK (gdk_selection_owner_set_for_display (m8_d, m8_A, sel,
						 GDK_CURRENT_TIME, FALSE),
	    "own: A owns");
  m8_drain ();
  M8_CHECK (m8_sel (sel).source_alive, "own: A has a source");

  /* replace A by B inside the process: no CLEAR, old source destroyed, new set */
  m8_resp_targets (tnames2);
  _gdk_wayland_test_dd_reset_requests (m8_d);
  m8_clear ();
  M8_CHECK (gdk_selection_owner_set_for_display (m8_d, m8_B, sel,
						 GDK_CURRENT_TIME, FALSE),
	    "own: B takes over");
  M8_CHECK (gdk_selection_owner_get_for_display (m8_d, sel) == m8_B,
	    "own: owner_get is B");
  m8_drain ();
  M8_EQ_INT (m8_count (GDK_SELECTION_CLEAR, NULL), 0,
	     "own: replacing an owner queues no SELECTION_CLEAR");
  info = m8_dd ();
  g_snprintf (what, sizeof what, "%s: old source destroyed", tag);
  M8_CHECK (info.source_destroy_count == 1, what);
  M8_EQ_INT (info.set_selection[kind].count, 1, "own: new set_selection");
  M8_CHECK (m8_set_mimes_are (&info, kind, mimes2), "own: new mimes");

  /* rapid A then B before the first probe is answered */
  m8_resp_targets (tnames);
  m8_drain ();
  m8_clear ();
  _gdk_wayland_test_dd_reset_requests (m8_d);
  completed0 = m8_sel (sel).probes_completed;
  M8_CHECK (gdk_selection_owner_set_for_display (m8_d, m8_A, sel,
						 GDK_CURRENT_TIME, FALSE) &&
	    gdk_selection_owner_set_for_display (m8_d, m8_B, sel,
						 GDK_CURRENT_TIME, FALSE),
	    "own: rapid A then B");
  m8_drain ();
  info = m8_dd ();
  si = m8_sel (sel);
  M8_EQ_INT (info.set_selection[kind].count, 1,
	     "own: the stale probe answer is ignored, one set_selection");
  M8_CHECK (m8_set_mimes_are (&info, kind, mimes1), "own: mimes of the last owner");
  M8_EQ_INT (si.probes_completed, completed0 + 1,
	     "own: probes_completed counts only the current probe");
  M8_CHECK (si.owner == m8_B, "own: B is the owner");

  /* the compositor takes the selection away: exactly one CLEAR */
  clears0 = m8_sel (sel).clear_events;
  m8_clear ();
  M8_HOOK (_gdk_wayland_test_dd_source_event (m8_d, skind, GDKW_TEST_SRC_CANCELLED,
					      NULL, 0), "own: cancelled");
  m8_drain ();
  M8_EQ_INT (m8_count (GDK_SELECTION_CLEAR, NULL), 1, "own: one SELECTION_CLEAR");
  {
    GdkEvent *ev = m8_find (GDK_SELECTION_CLEAR, NULL);

    M8_CHECK (ev != NULL && ev->selection.window == m8_B &&
	      ev->selection.selection == sel, "own: CLEAR names B and the selection");
  }
  M8_CHECK (gdk_selection_owner_get_for_display (m8_d, sel) == NULL,
	    "own: no owner after cancelled");
  si1 = m8_sel (sel);
  M8_EQ_INT (si1.clear_events, clears0 + 1, "own: clear_events");
  M8_CHECK (!si1.source_alive, "own: source gone after cancelled");
  M8_CHECK (!_gdk_wayland_test_dd_source_event (m8_d, skind,
						GDKW_TEST_SRC_CANCELLED, NULL, 0),
	    "own: a second cancelled finds no source");
  m8_drain ();
  M8_EQ_INT (m8_count (GDK_SELECTION_CLEAR, NULL), 1,
	     "own: the second cancelled queued nothing");

  /* the time rule */
  m8_clear ();
  M8_CHECK (gdk_selection_owner_set_for_display (m8_d, m8_A, sel, 1000, FALSE),
	    "own: owner_set at time 1000");
  M8_CHECK (!gdk_selection_owner_set_for_display (m8_d, m8_B, sel, 500, FALSE),
	    "own: owner_set with an older time returns FALSE");
  M8_CHECK (gdk_selection_owner_get_for_display (m8_d, sel) == m8_A,
	    "own: the older time did not take over");
  M8_CHECK (gdk_selection_owner_set_for_display (m8_d, m8_B, sel, 2000, FALSE),
	    "own: a newer time takes over");
  m8_drain ();
  M8_CHECK (gdk_selection_owner_set_for_display (m8_d, NULL, sel, 0, FALSE),
	    "own: release");
  m8_drain ();
  M8_CHECK (gdk_selection_owner_get_for_display (m8_d, sel) == NULL,
	    "own: released");
  M8_EQ_INT (m8_count (GDK_SELECTION_CLEAR, NULL), 0, "own: release queues no CLEAR");
  M8_CHECK (!m8_sel (sel).source_alive, "own: release destroys the source");
  m8_resp_off ();
}

/* ---- S5: serving ---------------------------------------------------------- */

static gboolean
m8_serve (GDKWSelKind kind, const gchar *mime, guint8 **data, gsize *len)
{
  *data = NULL;
  *len = 0;
  return _gdk_wayland_test_dd_source_send (m8_d, kind, mime, data, len);
}

static void
m8_serving (void)
{
  const gchar *t_utf8[] = { "UTF8_STRING", "application/x-ydk", NULL };
  const gchar *t_plain[] = { "text/plain;charset=utf-8", NULL };
  const gchar *t_string[] = { "STRING", NULL };
  const gchar *t_custom[] = { "x-ardour/region", NULL };
  static const guint8 raw[] = { 0, 1, 2, 0xff, 0x80, 0, 9 };
  guint8 *out;
  gsize len;
  GdkEvent *ev;
  gchar *name;
  GDKWTestDDInfo info;

  /* UTF8_STRING owner, 100 KB */
  m8_resp_reset ();
  m8_resp_targets (t_utf8);
  m8_resp_add ("UTF8_STRING", "UTF8_STRING", m8_big, m8_big_len);
  m8_resp_add ("application/x-ydk", "application/x-ydk", raw, sizeof raw);
  m8_clear ();
  M8_CHECK (gdk_selection_owner_set_for_display (m8_d, m8_A, M8_CLIP,
						 GDK_CURRENT_TIME, FALSE), "serve: own");
  m8_drain ();
  m8_clear ();

  M8_CHECK (m8_serve (GDKW_SEL_CLIPBOARD, "text/plain;charset=utf-8", &out, &len) &&
	    len == m8_big_len && memcmp (out, m8_big, len) == 0,
	    "serve: text/plain;charset=utf-8 from UTF8_STRING, 100 KB");
  g_free (out);
  ev = m8_find_req (m8_A, M8_ATOM ("UTF8_STRING"));
  M8_CHECK (ev != NULL && ev->selection.requestor != 0 &&
	    ev->selection.selection == M8_CLIP, "serve: SELECTION_REQUEST on the owner");
  if (ev)
    {
      name = gdk_atom_name (ev->selection.property);
      M8_CHECK (name && strncmp (name, "YDK_WL_SEND_", 12) == 0,
		"serve: reply property is a YDK_WL_SEND_ slot");
      g_free (name);
      M8_CHECK (ev->selection.requestor != _gdk_wayland_test_native_id (m8_B),
		"serve: requestor is a SEND token, not a window id");
    }
  M8_EQ_INT (_gdk_wayland_test_property_count (m8_A), 0,
	     "serve: reply property deleted");
  M8_EQ_INT (m8_sel (M8_CLIP).send_pending, 0, "serve: send_pending back to 0");
  M8_EQ_INT (m8_dd ().pending_writes, 0, "serve: pending_writes back to 0");

  M8_CHECK (m8_serve (GDKW_SEL_CLIPBOARD, "text/plain", &out, &len) &&
	    len == m8_big_len && memcmp (out, m8_big, len) == 0,
	    "serve: text/plain from the same target");
  g_free (out);
  M8_CHECK (m8_serve (GDKW_SEL_CLIPBOARD, "UTF8_STRING", &out, &len) &&
	    len == m8_big_len && memcmp (out, m8_big, len) == 0,
	    "serve: UTF8_STRING raw");
  g_free (out);
  M8_CHECK (m8_serve (GDKW_SEL_CLIPBOARD, "application/x-ydk", &out, &len) &&
	    len == sizeof raw && memcmp (out, raw, len) == 0,
	    "serve: a custom mime is served raw");
  g_free (out);

  /* refusals give EOF with no byte: TRUE and length 0 */
  M8_CHECK (m8_serve (GDKW_SEL_CLIPBOARD, "application/x-notoffered", &out, &len) &&
	    len == 0, "serve: a mime that is not offered");
  g_free (out);
  m8_resp.mode = M8_REFUSE;
  M8_CHECK (m8_serve (GDKW_SEL_CLIPBOARD, "UTF8_STRING", &out, &len) && len == 0,
	    "serve: owner answers NONE");
  g_free (out);
  m8_resp.mode = M8_INCR;
  M8_CHECK (m8_serve (GDKW_SEL_CLIPBOARD, "UTF8_STRING", &out, &len) && len == 0,
	    "serve: INCR reply refused");
  g_free (out);
  M8_EQ_INT (_gdk_wayland_test_property_count (m8_A), 0,
	     "serve: INCR reply property deleted");
  m8_resp.mode = M8_SILENT;
  M8_HOOK (_gdk_wayland_test_dd_set_timeouts (m8_d, 0, 200, 0, 0), "serve: timeouts");
  M8_CHECK (m8_serve (GDKW_SEL_CLIPBOARD, "UTF8_STRING", &out, &len) && len == 0,
	    "serve: no reply times out");
  g_free (out);
  M8_HOOK (_gdk_wayland_test_dd_set_timeouts (m8_d, 5000, 10000, 2000, 5000),
	   "serve: timeouts restored");
  M8_EQ_INT (m8_sel (M8_CLIP).send_pending, 0, "serve: no pending send after refusals");
  m8_resp.mode = M8_ANSWER;
  M8_CHECK (m8_serve (GDKW_SEL_CLIPBOARD, "UTF8_STRING", &out, &len) && len == m8_big_len,
	    "serve: a slot is free again");
  g_free (out);

  /* 20 refused transfers do not exhaust the 16 slots */
  {
    gint i;
    gboolean ok = TRUE;

    m8_resp.mode = M8_REFUSE;
    for (i = 0; i < 20; i++)
      {
	ok &= m8_serve (GDKW_SEL_CLIPBOARD, "UTF8_STRING", &out, &len) && len == 0;
	g_free (out);
      }
    M8_CHECK (ok, "serve: 20 refused transfers");
    m8_resp.mode = M8_ANSWER;
    M8_CHECK (m8_serve (GDKW_SEL_CLIPBOARD, "UTF8_STRING", &out, &len) &&
	      len == m8_big_len, "serve: still works after 20 refusals");
    g_free (out);
  }

  /* text/plain target only: CRLF becomes LF */
  m8_resp_reset ();
  m8_resp_targets (t_plain);
  m8_resp_add ("text/plain;charset=utf-8", "text/plain;charset=utf-8",
	       (const guint8 *) "a\r\nb", 4);
  M8_CHECK (gdk_selection_owner_set_for_display (m8_d, m8_A, M8_CLIP,
						 GDK_CURRENT_TIME, FALSE), "serve: own 2");
  m8_drain ();
  M8_CHECK (m8_serve (GDKW_SEL_CLIPBOARD, "text/plain;charset=utf-8", &out, &len) &&
	    len == 3 && memcmp (out, "a\nb", 3) == 0,
	    "serve: CRLF to LF for text/plain;charset=utf-8");
  g_free (out);
  M8_CHECK (m8_serve (GDKW_SEL_CLIPBOARD, "UTF8_STRING", &out, &len) &&
	    len == 3 && memcmp (out, "a\nb", 3) == 0,
	    "serve: the whole text group is served from that target");
  g_free (out);

  /* STRING owner: Latin-1 to UTF-8 */
  m8_resp_reset ();
  m8_resp_targets (t_string);
  m8_resp_add ("STRING", "STRING", (const guint8 *) "caf\xe9", 4);
  M8_CHECK (gdk_selection_owner_set_for_display (m8_d, m8_A, M8_CLIP,
						 GDK_CURRENT_TIME, FALSE), "serve: own 3");
  m8_drain ();
  M8_CHECK (m8_serve (GDKW_SEL_CLIPBOARD, "text/plain;charset=utf-8", &out, &len) &&
	    len == 5 && memcmp (out, "caf\xc3\xa9", 5) == 0,
	    "serve: STRING owner, Latin-1 becomes UTF-8");
  g_free (out);

  /* a custom atom is offered under its own name */
  m8_resp_reset ();
  m8_resp_targets (t_custom);
  m8_resp_add ("x-ardour/region", "x-ardour/region", raw, sizeof raw);
  M8_CHECK (gdk_selection_owner_set_for_display (m8_d, m8_A, M8_CLIP,
						 GDK_CURRENT_TIME, FALSE), "serve: own 4");
  m8_drain ();
  info = m8_dd ();
  M8_CHECK (info.set_selection[0].n_mimes == 1 &&
	    strcmp (info.set_selection[0].mimes[0], "x-ardour/region") == 0,
	    "serve: custom mime offered under its own name");
  M8_CHECK (m8_serve (GDKW_SEL_CLIPBOARD, "x-ardour/region", &out, &len) &&
	    len == sizeof raw && memcmp (out, raw, len) == 0,
	    "serve: custom mime raw");
  g_free (out);

  /* the owner window is destroyed during the transfer: no crash, fd closed */
  {
    GdkWindow *victim = m8_toplevel (30, 30);
    GDKWTestSelectionInfo si;

    g_object_ref (victim);
    gdk_window_show (victim);
    m8_drain ();
    m8_resp_reset ();
    m8_resp_targets (t_custom);
    m8_resp_add ("x-ardour/region", "x-ardour/region", raw, sizeof raw);
    M8_CHECK (gdk_selection_owner_set_for_display (m8_d, victim, M8_CLIP,
						   GDK_CURRENT_TIME, FALSE),
	      "serve: victim owns");
    m8_drain ();
    m8_resp.mode = M8_DESTROY;
    M8_CHECK (m8_serve (GDKW_SEL_CLIPBOARD, "x-ardour/region", &out, &len) &&
	      len == 0, "serve: owner destroyed mid transfer gives EOF");
    g_free (out);
    m8_drain ();
    si = m8_sel (M8_CLIP);
    M8_CHECK (!si.has_owner && !si.source_alive && si.send_pending == 0,
	      "serve: destroying the owner dropped record, source and transfer");
    M8_CHECK (gdk_selection_owner_get_for_display (m8_d, M8_CLIP) == NULL,
	      "serve: no owner after the window died");
    M8_EQ_INT (m8_dd ().pending_writes, 0, "serve: no write left after destroy");
    g_object_unref (victim);
  }

  M8_CHECK (gdk_selection_owner_set_for_display (m8_d, NULL, M8_CLIP, 0, FALSE),
	    "serve: release");
  m8_resp_off ();
  m8_drain ();
  m8_clear ();
}

/* ---- the selection tests in order -------------------------------------- */

static void
m8_selection_all (gboolean protocol)
{
  GDKWTestDDInfo info;

  /* S1: CLIPBOARD, with or without the data device */
  m8_loopback (M8_CLIP, protocol, 0, "S1 clipboard");
  if (protocol)
    {
      /* S6: PRIMARY is a protocol selection too; SECONDARY is local */
      m8_loopback (M8_PRIMARY, TRUE, 1, "S6 primary");
      info = m8_dd ();
      M8_EQ_INT (info.set_selection[0].count, 0,
		 "S6: PRIMARY traffic never touches CLIPBOARD");
      m8_loopback (GDK_SELECTION_SECONDARY, FALSE, -1, "S6 secondary");

      /* S2, S3 */
      m8_offer_targets_and_paste (M8_CLIP, GDKW_SEL_CLIPBOARD, "S3 clipboard");
      m8_offer_targets_and_paste (M8_PRIMARY, GDKW_SEL_PRIMARY, "S6 primary");

      /* S4 */
      m8_ownership (M8_CLIP, 0, GDKW_SEL_CLIPBOARD, "S4 clipboard");
      m8_ownership (M8_PRIMARY, 1, GDKW_SEL_PRIMARY, "S6 primary");

      /* S5 */
      m8_serving ();
    }
}

/* ---- D: drag and drop ----------------------------------------------------- */

static guint32
m8_tick (void)
{
  m8_time += 10;
  return m8_time;
}

static guint32 m8_seat_serial (void) { return _gdk_wayland_test_dd_seat_serial (m8_d); }

static void
m8_pointer_down (GdkWindow *w, guint32 button)
{
  M8_HOOK (_gdk_wayland_test_pointer_enter (m8_d, w, 100, 100), "pointer enter");
  M8_HOOK (_gdk_wayland_test_pointer_button (m8_d, m8_tick (), button, TRUE),
	   "pointer button down");
  m8_drain ();
  m8_clear ();
}

static void
m8_pointer_release (guint32 button)
{
  M8_HOOK (_gdk_wayland_test_pointer_button (m8_d, m8_tick (), button, FALSE),
	   "pointer button up");
  m8_drain ();
}

/* begin a drag with the standard targets and make it start through motion */
static GdkDragContext *
m8_drag_start (GList *targets, GdkDragAction possible)
{
  GdkDragContext *ctx = gdk_drag_begin (m8_ipc, targets);
  GdkWindow *dest = NULL;
  GdkDragProtocol proto = GDK_DRAG_PROTO_XDND;

  M8_CHECK (ctx != NULL, "drag: begin returns a context");
  if (ctx == NULL)
    return NULL;
  gdk_drag_find_window_for_screen (ctx, NULL, gdk_screen_get_default (), 100, 100,
				   &dest, &proto);
  M8_CHECK (dest == NULL && proto == GDK_DRAG_PROTO_NONE,
	    "drag: find_window gives no destination");
  M8_CHECK (!gdk_drag_motion (ctx, NULL, GDK_DRAG_PROTO_NONE, 100, 100,
			      GDK_ACTION_COPY, possible, m8_tick ()),
	    "drag: motion returns FALSE");
  return ctx;
}

static void
m8_drag_source (void)
{
  const gchar *want_mimes[] = { "text/plain;charset=utf-8", "text/plain",
				"UTF8_STRING", "text/uri-list", NULL };
  const guint8 uri[] = "file:///tmp/a\r\n";
  GDKWTestDDInfo info;
  GDKWTestDndSourceInfo si;
  GDKWTestDragContextInfo ci;
  GDKWTestSelectionInfo xs;
  GdkDragContext *ctx;
  GdkEvent *ev, *req;
  GList *targets;
  guint contexts0, sources0, probes0, i;
  guint32 right_serial;
  guint8 *out;
  gsize len;

  contexts0 = _gdk_wayland_test_dnd_contexts_alive ();
  sources0 = m8_dd ().sources_alive;
  targets = m8_targets ("UTF8_STRING", "text/uri-list", NULL);

  /* D1, D2: no pressed button, the begin is FAILED */
  m8_drain ();
  m8_clear ();
  _gdk_wayland_test_dd_reset_requests (m8_d);
  ctx = gdk_drag_begin (m8_ipc, targets);
  M8_CHECK (ctx != NULL && GDK_IS_DRAG_CONTEXT (ctx), "D1: begin returns a GdkDragContext");
  M8_HOOK (_gdk_wayland_test_drag_context_info (ctx, &ci), "D1: context info");
  M8_CHECK (ci.is_wayland && ci.protocol == GDK_DRAG_PROTO_LOCAL && ci.is_source &&
	    ci.n_targets == 2, "D1: context is a Wayland source, protocol LOCAL");
  M8_CHECK (ctx->source_window == m8_ipc, "D1: source_window is the ipc window");
  m8_drain ();
  ev = m8_find (GDK_DROP_FINISHED, m8_ipc);
  M8_CHECK (ev != NULL && ev->dnd.context == ctx, "D2: DROP_FINISHED for a failed begin");
  M8_EQ_INT (m8_count (GDK_DROP_FINISHED, NULL), 1, "D2: exactly one");
  M8_HOOK (_gdk_wayland_test_dnd_source_info (m8_d, &si), "D2: source info");
  M8_CHECK (si.failed && !si.started && !si.active && !si.succeeded,
	    "D2: failed, not started, not active");
  M8_EQ_INT (m8_dd ().start_drag_count, 0, "D2: no start_drag");
  M8_CHECK (!gdk_drag_drop_succeeded (ctx), "D2: drop_succeeded FALSE");
  gdk_drag_abort (ctx, m8_tick ());                /* harmless on a dead drag */
  g_object_unref (ctx);
  m8_clear ();
  M8_EQ_INT (_gdk_wayland_test_dnd_contexts_alive (), contexts0,
	     "D2: no context left");
  M8_EQ_INT (m8_dd ().sources_alive, sources0, "D2: no source left");

  /* D3: two buttons, the second released: the grab serial is a press serial */
  m8_pointer_down (m8_A, M8_BTN_LEFT);
  M8_HOOK (_gdk_wayland_test_pointer_button (m8_d, m8_tick (), M8_BTN_RIGHT, TRUE),
	   "D3: right press");
  right_serial = m8_seat_serial ();
  m8_pointer_release (M8_BTN_RIGHT);
  M8_CHECK (m8_seat_serial () != right_serial, "D3: the release has its own serial");
  m8_clear ();
  _gdk_wayland_test_dd_reset_requests (m8_d);
  probes0 = m8_sel (M8_XDND).probes_started;

  ctx = gdk_drag_begin (m8_ipc, targets);
  M8_CHECK (ctx != NULL, "D3: begin");
  M8_EQ_INT (m8_dd ().start_drag_count, 0, "D3: begin does not start the drag");
  {
    GdkWindow *dest = (GdkWindow *) 0x1;
    GdkDragProtocol proto = GDK_DRAG_PROTO_XDND;

    gdk_drag_find_window_for_screen (ctx, NULL, gdk_screen_get_default (), 100,
				     100, &dest, &proto);
    M8_CHECK (dest == NULL && proto == GDK_DRAG_PROTO_NONE,
	      "D3: find_window: no destination, protocol NONE");
    M8_CHECK (!gdk_drag_motion (ctx, NULL, GDK_DRAG_PROTO_NONE, 100, 100,
				GDK_ACTION_COPY, GDK_ACTION_COPY | GDK_ACTION_MOVE,
				m8_tick ()),
	      "D3: motion returns FALSE");
  }
  info = m8_dd ();
  M8_EQ_INT (info.start_drag_count, 1, "D3: motion started the drag");
  M8_EQ_INT (info.start_drag_serial, right_serial,
	     "D3: start_drag uses the newest PRESS serial");
  M8_CHECK (info.start_drag_serial != m8_seat_serial (),
	    "D3: not the release serial");
  M8_CHECK (info.start_drag_origin == m8_A && !info.start_drag_icon,
	    "D3: origin is the toplevel of the pointer focus, no icon");
  M8_CHECK (info.start_drag_n_mimes == 4, "D3: four mimes");
  for (i = 0; i < 4 && i < info.start_drag_n_mimes; i++)
    M8_CHECK (strcmp (info.start_drag_mimes[i], want_mimes[i]) == 0,
	      "D3: start_drag mime");
  M8_CHECK (info.source_actions_set && info.source_actions_mask == 3,
	    "D3: source actions COPY|MOVE");
  M8_CHECK (gdk_drag_get_selection (ctx) == M8_XDND, "D3: get_selection is XdndSelection");
  M8_HOOK (_gdk_wayland_test_dnd_source_info (m8_d, &si), "D3: source info");
  M8_CHECK (si.started && si.active && !si.failed, "D3: started and active");

  /* the ipc window owns XdndSelection: no probe, no set_selection */
  m8_resp_reset ();
  m8_resp_add ("text/uri-list", "text/uri-list", uri, strlen ((char *) uri));
  M8_CHECK (gdk_selection_owner_set_for_display (m8_d, m8_ipc, M8_XDND,
						 GDK_CURRENT_TIME, FALSE),
	    "D3: owner_set XdndSelection");
  m8_drain ();
  xs = m8_sel (M8_XDND);
  M8_EQ_INT (xs.probes_started, probes0, "D3: XdndSelection is never probed");
  info = m8_dd ();
  M8_EQ_INT (info.set_selection[0].count + info.set_selection[1].count, 0,
	     "D3: XdndSelection does not set_selection");
  M8_CHECK (m8_find_req (m8_ipc, M8_ATOM ("TARGETS")) == NULL, "D3: no TARGETS request");

  /* source events */
  m8_clear ();
  M8_HOOK (_gdk_wayland_test_dd_source_event (m8_d, GDKW_SEL_DND, GDKW_TEST_SRC_TARGET,
					      "text/uri-list", 0), "D3: target event");
  m8_drain ();
  M8_EQ_INT (m8_count (GDK_DRAG_STATUS, NULL), 0, "D3: TARGET (v3) raises nothing");
  M8_HOOK (_gdk_wayland_test_dd_source_event (m8_d, GDKW_SEL_DND, GDKW_TEST_SRC_ACTION,
					      NULL, GDKW_TEST_WL_ACTION_COPY), "D3: action copy");
  m8_drain ();
  ev = m8_find (GDK_DRAG_STATUS, m8_ipc);
  M8_CHECK (ev != NULL && ev->dnd.context == ctx && ctx->action == GDK_ACTION_COPY,
	    "D3: ACTION(COPY) raises DRAG_STATUS with COPY");
  M8_HOOK (_gdk_wayland_test_dd_source_event (m8_d, GDKW_SEL_DND, GDKW_TEST_SRC_ACTION,
					      NULL, GDKW_TEST_WL_ACTION_MOVE), "D3: action move");
  m8_drain ();
  M8_EQ_INT (m8_count (GDK_DRAG_STATUS, m8_ipc), 2, "D3: ACTION(MOVE) raises a second one");
  M8_CHECK (ctx->action == GDK_ACTION_MOVE, "D3: selected action MOVE");
  M8_HOOK (_gdk_wayland_test_dd_source_event (m8_d, GDKW_SEL_DND, GDKW_TEST_SRC_ACTION,
					      NULL, GDKW_TEST_WL_ACTION_MOVE), "D3: action move again");
  m8_drain ();
  M8_EQ_INT (m8_count (GDK_DRAG_STATUS, m8_ipc), 2, "D3: a repeated action raises nothing");

  /* the compositor asks for the data */
  M8_CHECK (m8_serve (GDKW_SEL_DND, "text/uri-list", &out, &len) &&
	    len == strlen ((char *) uri) && memcmp (out, uri, len) == 0,
	    "D3: source_send(DND, text/uri-list)");
  g_free (out);
  req = m8_find_req (m8_ipc, M8_ATOM ("text/uri-list"));
  M8_CHECK (req != NULL && req->selection.selection == M8_XDND,
	    "D3: the request names XdndSelection");

  /* drop performed raises nothing; finished with MOVE and no local destination */
  m8_clear ();
  M8_HOOK (_gdk_wayland_test_dd_source_event (m8_d, GDKW_SEL_DND,
					      GDKW_TEST_SRC_DROP_PERFORMED, NULL, 0),
	   "D3: drop performed");
  m8_check_no_events ("D3: DROP_PERFORMED");
  M8_HOOK (_gdk_wayland_test_dd_source_event (m8_d, GDKW_SEL_DND, GDKW_TEST_SRC_FINISHED,
					      NULL, 0), "D3: finished");
  m8_drain ();
  req = m8_find_req (m8_ipc, M8_ATOM ("DELETE"));
  ev = m8_find (GDK_DROP_FINISHED, m8_ipc);
  M8_CHECK (req != NULL && req->selection.selection == M8_XDND &&
	    ev != NULL && ev->dnd.context == ctx,
	    "D3: DELETE request and DROP_FINISHED");
  M8_CHECK (req && ev && g_queue_index (&m8_events, req) < g_queue_index (&m8_events, ev),
	    "D3: the DELETE request comes before DROP_FINISHED");
  M8_CHECK (gdk_drag_drop_succeeded (ctx), "D3: drop_succeeded TRUE");
  M8_HOOK (_gdk_wayland_test_dnd_source_info (m8_d, &si), "D3: source info 2");
  M8_CHECK (si.succeeded && si.delete_requested && !si.active && !si.local_dest,
	    "D3: succeeded, delete requested, not active, no local destination");
  info = m8_dd ();
  M8_EQ_INT (info.sources_alive, sources0, "D3: the drag source is freed");
  M8_EQ_INT (info.source_destroy_count, 1, "D3: source destroyed once");
  M8_CHECK (gdk_selection_owner_set_for_display (m8_d, NULL, M8_XDND, 0, FALSE),
	    "D3: release XdndSelection");
  m8_resp_off ();
  g_object_unref (ctx);
  m8_clear ();
  m8_drain ();

  /* the fallback timer starts a drag that never gets a motion call */
  _gdk_wayland_test_dd_reset_requests (m8_d);
  ctx = gdk_drag_begin (m8_ipc, targets);
  M8_EQ_INT (m8_dd ().start_drag_count, 0, "D3b: not started inside begin");
  for (i = 0; i < 100 && m8_dd ().start_drag_count == 0; i++)
    m8_run_ms (20);
  info = m8_dd ();
  M8_EQ_INT (info.start_drag_count, 1, "D3b: the 100 ms fallback started it");
  M8_CHECK (info.source_actions_set && info.source_actions_mask == 3,
	    "D3b: fallback actions COPY|MOVE");

  /* D4: cancelled */
  m8_clear ();
  M8_HOOK (_gdk_wayland_test_dd_source_event (m8_d, GDKW_SEL_DND,
					      GDKW_TEST_SRC_CANCELLED, NULL, 0),
	   "D4: cancelled");
  m8_drain ();
  M8_EQ_INT (m8_count (GDK_DROP_FINISHED, m8_ipc), 1, "D4: DROP_FINISHED");
  M8_EQ_INT (m8_count (GDK_SELECTION_REQUEST, NULL), 0, "D4: no DELETE request");
  M8_EQ_INT (m8_count (GDK_DRAG_STATUS, NULL), 0, "D4: no status");
  M8_CHECK (!gdk_drag_drop_succeeded (ctx), "D4: not succeeded");
  M8_HOOK (_gdk_wayland_test_dnd_source_info (m8_d, &si), "D4: source info");
  M8_CHECK (!si.active && si.cancelled && !si.succeeded, "D4: cancelled and inactive");
  M8_EQ_INT (m8_dd ().sources_alive, sources0, "D4: source freed");
  g_object_unref (ctx);
  m8_clear ();

  /* D5: abort after the start: source destroyed, no event */
  _gdk_wayland_test_dd_reset_requests (m8_d);
  ctx = m8_drag_start (targets, GDK_ACTION_COPY | GDK_ACTION_MOVE);
  M8_EQ_INT (m8_dd ().start_drag_count, 1, "D5: started");
  gdk_drag_abort (ctx, m8_tick ());
  m8_drain ();
  M8_EQ_INT (g_queue_get_length (&m8_events), 0, "D5: abort queues no event");
  M8_HOOK (_gdk_wayland_test_dnd_source_info (m8_d, &si), "D5: source info");
  M8_CHECK (!si.active, "D5: inactive after abort");
  info = m8_dd ();
  M8_EQ_INT (info.sources_alive, sources0, "D5: source destroyed");
  M8_EQ_INT (info.source_destroy_count, 1, "D5: destroyed once");
  gdk_drag_abort (ctx, m8_tick ());              /* twice is harmless */
  g_object_unref (ctx);
  m8_clear ();

  /* D5b: abort of a drag that never started */
  ctx = gdk_drag_begin (m8_ipc, targets);
  gdk_drag_abort (ctx, m8_tick ());
  m8_run_ms (250);
  M8_EQ_INT (m8_dd ().sources_alive, sources0, "D5b: aborted before start, no source");
  M8_EQ_INT (m8_dd ().start_drag_count, 1, "D5b: and it never started (still 1)");
  g_object_unref (ctx);
  m8_clear ();

  /* D5c: no offerable target, the begin fails */
  {
    GList *none = m8_targets ("TARGETS", "TIMESTAMP", NULL);

    _gdk_wayland_test_dd_reset_requests (m8_d);
    ctx = gdk_drag_begin (m8_ipc, none);
    m8_drain ();
    M8_EQ_INT (m8_count (GDK_DROP_FINISHED, m8_ipc), 1, "D5c: empty source map fails");
    M8_EQ_INT (m8_dd ().start_drag_count, 0, "D5c: no start_drag");
    g_object_unref (ctx);
    m8_clear ();
    g_list_free (none);
  }

  /* D5d: a begin on a window that is not alive still gives a context */
  ctx = gdk_drag_begin (NULL, targets);
  M8_CHECK (ctx != NULL, "D5d: begin (NULL) returns a context");
  g_object_unref (ctx);
  m8_clear ();

  m8_pointer_release (M8_BTN_LEFT);
  M8_HOOK (_gdk_wayland_test_pointer_leave (m8_d, m8_A), "pointer leave");
  m8_drain ();
  m8_clear ();
  g_list_free (targets);
  M8_EQ_INT (_gdk_wayland_test_dnd_contexts_alive (), contexts0, "D5: no context left");
}

/* ---- destination ----------------------------------------------------------- */

static guint
m8_new_dnd_offer (guint version)
{
  const gchar *mimes[] = { "text/plain;charset=utf-8", "text/uri-list", NULL };
  guint id = _gdk_wayland_test_dd_offer_new (m8_d, GDKW_SEL_DND, version, mimes);

  M8_CHECK (id != 0, "dest: offer_new");
  if (version >= 3)
    {
      M8_HOOK (_gdk_wayland_test_dd_offer_source_actions (m8_d, id, 3),
	       "dest: source actions");
      M8_HOOK (_gdk_wayland_test_dd_offer_action (m8_d, id, GDKW_TEST_WL_ACTION_COPY),
	       "dest: action");
    }
  return id;
}

/* variant 0: finish (TRUE) with a data transfer, 1: finish (FALSE), 2: reply (FALSE) */
static void
m8_dest_cycle (gint variant)
{
  const gchar *want_t[] = { "UTF8_STRING", "STRING", "text/uri-list", NULL };
  GDKWTestDDInfo info;
  GDKWTestDndDestInfo di;
  GdkEvent *enter, *motion, *drop;
  GdkDragContext *ctx;
  M8Paste p;
  GList *l;
  guint offers0, id, i;
  guint32 serial = 0;
  gint px = 0, py = 0;
  gchar what[96];

  g_snprintf (what, sizeof what, "dest[%d]", variant);
  gdk_window_get_position (m8_A, &px, &py);
  offers0 = m8_dd ().offers_alive;
  id = m8_new_dnd_offer (3);
  m8_drain ();
  m8_clear ();
  _gdk_wayland_test_dd_reset_requests (m8_d);

  M8_HOOK (_gdk_wayland_test_dd_dnd_enter (m8_d, m8_A, id, 120, 40, &serial), "dest: enter");
  m8_drain ();
  enter = m8_find (GDK_DRAG_ENTER, m8_A);
  motion = m8_find (GDK_DRAG_MOTION, m8_A);
  M8_CHECK (enter != NULL && motion != NULL &&
	    g_queue_index (&m8_events, enter) < g_queue_index (&m8_events, motion),
	    "D6: DRAG_ENTER then DRAG_MOTION");
  if (enter == NULL || motion == NULL)
    return;
  M8_EQ_INT (motion->dnd.x_root, 120 + px, "D6: x_root is x + toplevel x");
  M8_EQ_INT (motion->dnd.y_root, 40 + py, "D6: y_root is y + toplevel y");
  ctx = enter->dnd.context;
  M8_CHECK (ctx == motion->dnd.context, "D6: one context for both events");
  M8_CHECK (ctx->protocol == GDK_DRAG_PROTO_LOCAL && !ctx->is_source &&
	    ctx->dest_window == m8_A, "D6: destination context, protocol LOCAL");
  M8_CHECK (ctx->source_window == gdk_get_default_root_window (),
	    "D6: source_window is the screen root without a local source");
  M8_CHECK (ctx->actions == (GDK_ACTION_COPY | GDK_ACTION_MOVE) &&
	    ctx->suggested_action == GDK_ACTION_COPY, "D6: actions and suggested action");
  M8_EQ_INT (g_list_length (ctx->targets), 3, "D6: three targets");
  for (l = ctx->targets, i = 0; l != NULL && want_t[i] != NULL; l = l->next, i++)
    M8_CHECK (GDK_POINTER_TO_ATOM (l->data) == M8_ATOM (want_t[i]), "D6: target order");
  M8_HOOK (_gdk_wayland_test_dnd_dest_info (m8_d, &di), "D6: dest info");
  M8_CHECK (di.active && !di.ignored && !di.dropped && di.context == ctx &&
	    di.dest_window == m8_A, "D6: dest info");
  M8_EQ_INT (m8_dd ().last_enter_serial, serial, "D6: enter serial recorded");

  m8_clear ();
  M8_HOOK (_gdk_wayland_test_dd_dnd_motion (m8_d, m8_tick (), 130, 50), "D6: motion");
  m8_drain ();
  motion = m8_find (GDK_DRAG_MOTION, m8_A);
  M8_CHECK (motion != NULL && motion->dnd.x_root == 130 + px &&
	    motion->dnd.y_root == 50 + py && motion->dnd.context == ctx,
	    "D6: motion coordinates");
  M8_EQ_INT (m8_count (GDK_DRAG_ENTER, NULL), 0, "D6: no second ENTER");

  /* gdk_drag_status maps to accept + set_actions, deduplicated */
  _gdk_wayland_test_dd_reset_requests (m8_d);
  gdk_drag_status (ctx, GDK_ACTION_COPY, m8_tick ());
  info = m8_dd ();
  M8_EQ_INT (info.accept_count, 1, "D6: status sends accept");
  M8_CHECK (info.accept_serial == serial && !info.accept_null &&
	    strcmp (info.accept_mime, "text/plain;charset=utf-8") == 0,
	    "D6: accept (enter serial, first offer mime)");
  M8_CHECK (info.set_actions_count == 1 && info.offer_actions_mask == 1 &&
	    info.offer_actions_preferred == 1, "D6: set_actions (COPY, COPY)");
  gdk_drag_status (ctx, GDK_ACTION_COPY, m8_tick ());
  info = m8_dd ();
  M8_CHECK (info.accept_count == 1 && info.set_actions_count == 1,
	    "D6: the same status again sends nothing");
  gdk_drag_status (ctx, 0, m8_tick ());
  info = m8_dd ();
  M8_CHECK (info.accept_count == 2 && info.accept_null && info.offer_actions_mask == 0,
	    "D6: status (0) is accept NULL with mask 0");
  gdk_drag_status (ctx, GDK_ACTION_MOVE, m8_tick ());
  info = m8_dd ();
  M8_CHECK (info.offer_actions_mask == 2 && info.offer_actions_preferred == 2 &&
	    !info.accept_null, "D6: status (MOVE) is mask 2");
  M8_CHECK (ctx->action == GDK_ACTION_MOVE, "D6: context action follows the status");

  /* drop, then a leave after the drop is ignored */
  m8_clear ();
  M8_HOOK (_gdk_wayland_test_dd_dnd_drop (m8_d), "D6: drop");
  m8_drain ();
  drop = m8_find (GDK_DROP_START, m8_A);
  M8_CHECK (drop != NULL && drop->dnd.x_root == 130 + px && drop->dnd.y_root == 50 + py &&
	    drop->dnd.context == ctx, "D6: DROP_START at the last position");
  m8_clear ();
  M8_HOOK (_gdk_wayland_test_dd_dnd_leave (m8_d), "D6: leave after drop");
  m8_check_no_events ("D6: a leave after a drop");
  M8_EQ_INT (m8_dd ().offers_alive, offers0 + 1, "D6: the offer stays alive after drop");
  M8_HOOK (_gdk_wayland_test_dnd_dest_info (m8_d, &di), "D6: dest info 2");
  M8_CHECK (di.active && di.dropped, "D6: still active and dropped");

  if (variant == 0)
    {
      /* cut on a line end so that the prefix is valid UTF-8 */
      gsize cut = 70000;

      while (m8_big[cut - 1] != 0x0a)
	cut++;
      M8_HOOK (_gdk_wayland_test_dd_offer_data (m8_d, id, "text/plain;charset=utf-8",
						m8_big, cut, GDKW_TEST_FEED_NORMAL),
	       "D6: offer_data");
      _gdk_wayland_test_dd_reset_requests (m8_d);
      M8_CHECK (gdk_drag_get_selection (ctx) == M8_XDND, "D6: destination get_selection");
      m8_paste (m8_A, gdk_drag_get_selection (ctx), "UTF8_STRING", 5000, &p);
      M8_CHECK (m8_paste_is (&p, m8_big, cut), "D6: data over XdndSelection");
      m8_paste_free (&p);
      M8_EQ_INT (m8_dd ().receive_kind, GDKW_SEL_DND, "D6: receive kind DND");
      m8_paste (m8_A, M8_XDND, "DELETE", 5000, &p);
      M8_CHECK (p.notified && p.property != GDK_NONE, "D6: DELETE answered as success");
      m8_paste_free (&p);
      M8_EQ_INT (_gdk_wayland_test_property_count (m8_A), 0, "D6: nothing left on A");

      _gdk_wayland_test_dd_reset_requests (m8_d);
      gdk_drop_finish (ctx, TRUE, m8_tick ());
      info = m8_dd ();
      M8_EQ_INT (info.finish_count, 1, "D6: drop_finish (TRUE) sends finish");
      M8_EQ_INT (info.offer_destroy_count, 1, "D6: and destroys the offer");
    }
  else if (variant == 1)
    {
      _gdk_wayland_test_dd_reset_requests (m8_d);
      gdk_drop_finish (ctx, FALSE, m8_tick ());
      info = m8_dd ();
      M8_EQ_INT (info.finish_count, 0, "D6b: drop_finish (FALSE) sends no finish");
      M8_EQ_INT (info.offer_destroy_count, 1, "D6b: but destroys the offer");
    }
  else
    {
      _gdk_wayland_test_dd_reset_requests (m8_d);
      gdk_drop_reply (ctx, FALSE, m8_tick ());
      info = m8_dd ();
      M8_CHECK (info.accept_count == 1 && info.accept_null && info.offer_actions_mask == 0,
		"D6c: drop_reply (FALSE) is accept NULL and set_actions 0");
      M8_EQ_INT (info.finish_count, 0, "D6c: no finish");
      M8_EQ_INT (info.offer_destroy_count, 1, "D6c: offer destroyed");
    }
  M8_EQ_INT (m8_dd ().offers_alive, offers0, what);
  M8_HOOK (_gdk_wayland_test_dnd_dest_info (m8_d, &di), "D6: dest info 3");
  M8_CHECK (!di.active, "D6: inactive after finish");
  m8_clear ();
}

static void
m8_drag_dest (void)
{
  const gchar *mimes[] = { "text/plain;charset=utf-8", NULL };
  GDKWTestDndDestInfo di;
  GDKWTestDndSourceInfo si;
  GDKWTestDDInfo info;
  GdkDragContext *ctx, *dctx;
  GdkEvent *ev;
  GdkWindow *D;
  GList *targets;
  guint contexts0 = _gdk_wayland_test_dnd_contexts_alive ();
  guint offers0 = m8_dd ().offers_alive;
  M8Paste p;
  guint id;
  guint32 serial = 0;

  gdk_window_register_dnd (m8_A);

  m8_dest_cycle (0);
  m8_dest_cycle (1);
  m8_dest_cycle (2);

  /* D7: leave without a drop */
  id = m8_new_dnd_offer (3);
  m8_clear ();
  M8_HOOK (_gdk_wayland_test_dd_dnd_enter (m8_d, m8_A, id, 50, 60, &serial), "D7: enter");
  M8_HOOK (_gdk_wayland_test_dd_dnd_motion (m8_d, m8_tick (), 55, 65), "D7: motion");
  M8_HOOK (_gdk_wayland_test_dd_dnd_leave (m8_d), "D7: leave");
  m8_drain ();
  M8_CHECK (m8_count (GDK_DRAG_ENTER, m8_A) == 1 && m8_count (GDK_DRAG_MOTION, m8_A) == 2 &&
	    m8_count (GDK_DRAG_LEAVE, m8_A) == 1 && m8_count (GDK_DROP_START, NULL) == 0,
	    "D7: ENTER, MOTION x2, LEAVE");
  M8_HOOK (_gdk_wayland_test_dnd_dest_info (m8_d, &di), "D7: dest info");
  M8_CHECK (!di.active, "D7: inactive after leave");
  M8_EQ_INT (m8_dd ().offers_alive, offers0, "D7: offer destroyed by leave");
  m8_clear ();

  /* D8: a window that was not registered */
  id = m8_new_dnd_offer (3);
  gdk_window_show (m8_C);
  m8_drain ();
  _gdk_wayland_test_dd_reset_requests (m8_d);
  m8_clear ();
  M8_HOOK (_gdk_wayland_test_dd_dnd_enter (m8_d, m8_C, id, 10, 10, &serial), "D8: enter on C");
  m8_drain ();
  info = m8_dd ();
  M8_CHECK (info.accept_count == 1 && info.accept_null, "D8: accept (NULL)");
  M8_HOOK (_gdk_wayland_test_dnd_dest_info (m8_d, &di), "D8: dest info");
  M8_CHECK (di.ignored && !di.active, "D8: ignored, no context");
  M8_EQ_INT (g_queue_get_length (&m8_events), 0, "D8: no GDK DnD event");
  M8_HOOK (_gdk_wayland_test_dd_dnd_motion (m8_d, m8_tick (), 12, 12), "D8: motion");
  M8_HOOK (_gdk_wayland_test_dd_dnd_leave (m8_d), "D8: leave");
  m8_drain ();
  M8_EQ_INT (g_queue_get_length (&m8_events), 0, "D8: still no event");
  M8_EQ_INT (m8_dd ().offers_alive, offers0, "D8: offer released by leave");

  /* D10: nothing to convert */
  m8_paste (m8_A, M8_XDND, "UTF8_STRING", 5000, &p);
  M8_CHECK (p.notified && p.property == GDK_NONE, "D10: no offer, no owner: refused");

  /* D9: a drag inside the application */
  targets = m8_targets ("UTF8_STRING", "text/uri-list", NULL);
  m8_pointer_down (m8_A, M8_BTN_LEFT);
  _gdk_wayland_test_dd_reset_requests (m8_d);
  ctx = m8_drag_start (targets, GDK_ACTION_COPY | GDK_ACTION_MOVE);
  id = _gdk_wayland_test_dd_offer_new (m8_d, GDKW_SEL_DND, 3, mimes);
  M8_HOOK (_gdk_wayland_test_dd_offer_source_actions (m8_d, id, 3), "D9: source actions");
  m8_clear ();
  M8_HOOK (_gdk_wayland_test_dd_dnd_enter (m8_d, m8_A, id, 20, 20, &serial), "D9: enter");
  m8_drain ();
  ev = m8_find (GDK_DRAG_ENTER, m8_A);
  dctx = ev ? ev->dnd.context : NULL;
  M8_CHECK (dctx != NULL && dctx->source_window == m8_ipc,
	    "D9: destination source_window is the local source ipc window");
  M8_HOOK (_gdk_wayland_test_dnd_source_info (m8_d, &si), "D9: source info");
  M8_CHECK (si.local_dest, "D9: source_info.local_dest");
  M8_HOOK (_gdk_wayland_test_dd_dnd_leave (m8_d), "D9: leave");
  M8_HOOK (_gdk_wayland_test_dd_source_event (m8_d, GDKW_SEL_DND, GDKW_TEST_SRC_ACTION,
					      NULL, GDKW_TEST_WL_ACTION_MOVE), "D9: action");
  m8_clear ();
  M8_HOOK (_gdk_wayland_test_dd_source_event (m8_d, GDKW_SEL_DND, GDKW_TEST_SRC_FINISHED,
					      NULL, 0), "D9: finished");
  m8_drain ();
  M8_EQ_INT (m8_count (GDK_DROP_FINISHED, m8_ipc), 1, "D9: DROP_FINISHED");
  M8_EQ_INT (m8_count (GDK_SELECTION_REQUEST, NULL), 0,
	     "D9: a local destination gets no DELETE request");
  g_object_unref (ctx);
  m8_clear ();
  m8_pointer_release (M8_BTN_LEFT);
  _gdk_wayland_test_pointer_leave (m8_d, m8_A);
  m8_drain ();
  m8_clear ();
  g_list_free (targets);

  /* D11: the destination window dies while a context is active */
  D = m8_toplevel (80, 60);
  g_object_ref (D);
  gdk_window_show (D);
  m8_drain ();
  gdk_window_register_dnd (D);
  id = m8_new_dnd_offer (3);
  M8_HOOK (_gdk_wayland_test_dd_dnd_enter (m8_d, D, id, 5, 5, &serial), "D11: enter");
  m8_drain ();
  M8_HOOK (_gdk_wayland_test_dnd_dest_info (m8_d, &di), "D11: dest info");
  M8_CHECK (di.active && di.dest_window == D, "D11: active on D");
  gdk_window_destroy (D);
  m8_drain ();
  M8_HOOK (_gdk_wayland_test_dnd_dest_info (m8_d, &di), "D11: dest info 2");
  M8_CHECK (!di.active, "D11: inactive after the window died");
  M8_EQ_INT (m8_dd ().offers_alive, offers0, "D11: offer released");
  g_object_unref (D);
  m8_clear ();
  M8_EQ_INT (_gdk_wayland_test_dnd_contexts_alive (), contexts0, "D11: no context left");
}

static void
m8_payload (void)
{
  GString *s = g_string_new (NULL);
  guint i = 0;

  /* valid UTF-8, no CR, every code point class: 100 KB crosses a 64 KiB pipe */
  while (s->len < 100 * 1024)
    g_string_append_printf (s, "%u \xc3\xa9\xc3\xb6 \xe2\x9c\x93 \xf0\x9f\x8e\xb5\n", i++);
  m8_big_len = s->len;
  m8_big = (guint8 *) g_string_free (s, FALSE);
}

static void
m8_gauges_zero (const gchar *when)
{
  GDKWTestDDInfo info = m8_dd ();
  gchar what[96];

  g_snprintf (what, sizeof what, "%s: offers_alive", when);
  M8_EQ_INT (info.offers_alive, 0, what);
  g_snprintf (what, sizeof what, "%s: sources_alive", when);
  M8_EQ_INT (info.sources_alive, 0, what);
  g_snprintf (what, sizeof what, "%s: pending_reads", when);
  M8_EQ_INT (info.pending_reads, 0, what);
  g_snprintf (what, sizeof what, "%s: pending_writes", when);
  M8_EQ_INT (info.pending_writes, 0, what);
}

static int
m8_exercise (void)
{
  GDKWTestDDInfo info;
  gboolean virtual_dd;
  guint contexts0;
  gint i;

  m8_d = gdk_display_get_default ();
  m8_A = main_window;
  contexts0 = _gdk_wayland_test_dnd_contexts_alive ();

  /* the recorder is the application for the whole of M8 */
  gdk_event_handler_set (m8_recorder, NULL, NULL);

  m8_B = m8_toplevel (200, 150);
  m8_C = m8_toplevel (100, 100);
  m8_ipc = m8_temp ();
  M8_CHECK (m8_B != NULL && m8_C != NULL && m8_ipc != NULL, "windows created");
  if (m8_B == NULL || m8_C == NULL || m8_ipc == NULL)
    {
      gdk_event_handler_set (event_handler, NULL, NULL);
      g_printerr ("ydk_demo: M8: FAIL (%d)\n", m8_failures ? m8_failures : 1);
      return m8_failures ? m8_failures : 1;
    }
  gdk_window_show (m8_B);
  m8_drain ();
  m8_clear ();
  m8_payload ();

  /* P: the property table needs no compositor and no device */
  m8_property ();
  m8_helpers ();

  /* the loopback needs no device either: S1 first without one (the headless
   * compositor has no seat, so there is none), then with the virtual one */
  M8_EQ_INT (m8_sel (M8_CLIP).probes_started, 0, "no probe so far");
  info = m8_dd ();
  M8_CHECK (!info.have_device && !info.virtual_device,
	    "headless: no data device before the virtual one");
  m8_selection_all (FALSE);

  virtual_dd = _gdk_wayland_test_dd_init (m8_d);
  if (!virtual_dd)
    g_print ("ydk_demo: M8: SKIP protocol parts (a real data device exists)\n");
  else
    {
      info = m8_dd ();
      M8_CHECK (info.virtual_device && info.have_device && info.have_primary &&
		info.manager_version == 3, "virtual device is up, both managers, v3");

      /* real serials from the M6 seat hooks */
      M8_HOOK (_gdk_wayland_test_pointer_enter (m8_d, m8_A, 100, 100), "serial: enter");
      M8_HOOK (_gdk_wayland_test_pointer_button (m8_d, m8_tick (), M8_BTN_LEFT, TRUE),
	       "serial: press");
      M8_HOOK (_gdk_wayland_test_pointer_button (m8_d, m8_tick (), M8_BTN_LEFT, FALSE),
	       "serial: release");
      M8_HOOK (_gdk_wayland_test_pointer_leave (m8_d, m8_A), "serial: leave");
      m8_drain ();
      m8_clear ();
      M8_CHECK (m8_seat_serial () != 0, "the seat hooks produced a serial");
      M8_HOOK (_gdk_wayland_test_dd_set_timeouts (m8_d, 5000, 10000, 2000, 5000),
	       "timeouts");

      m8_selection_all (TRUE);
      m8_drag_source ();
      m8_drag_dest ();

      /* D12 teardown */
      m8_drain ();
      m8_clear ();
      m8_gauges_zero ("D12 before fini");
      M8_HOOK (_gdk_wayland_test_dd_fini (m8_d), "D12: dd_fini");
      info = m8_dd ();
      M8_CHECK (!info.virtual_device && !info.have_device, "D12: device gone");
      m8_gauges_zero ("D12 after fini");
      M8_EQ_INT (_gdk_wayland_test_dnd_contexts_alive (), contexts0,
		 "D12: no drag context alive");

      /* the loopback still works once the device is gone (no seat at all) */
      m8_selection_all (FALSE);
    }

  /* leave no property and no event behind */
  for (i = 0; i < 3; i++)
    {
      GdkWindow *w = i == 0 ? m8_A : i == 1 ? m8_B : m8_C;

      M8_EQ_INT (_gdk_wayland_test_property_count (w), 0, "no property left behind");
    }
  m8_drain ();
  m8_clear ();

  gdk_window_destroy (m8_ipc);
  gdk_window_destroy (m8_C);
  gdk_window_destroy (m8_B);
  m8_drain ();
  g_free (m8_big);
  m8_big = NULL;
  m8_resp_off ();
  m8_clear ();
  M8_EQ_INT (_gdk_wayland_test_dnd_contexts_alive (), contexts0, "no drag context alive at the end");

  gdk_event_handler_set (event_handler, NULL, NULL);
  gdk_window_invalidate_rect (main_window, NULL, TRUE);

  g_print ("ydk_demo: M8: %d checks run\n", m8_checks);
  if (m8_failures)
    g_printerr ("ydk_demo: M8: FAIL (%d)\n", m8_failures);
  else
    g_print ("ydk_demo: M8: PASS\n");

  return m8_failures;
}


int
main (int argc, char **argv)
{
  GdkWindowAttr attr;
  /* initialised: the Wayland backend reads .pixel through the colormap */
  GdkColor bg = { 0, 0xffff, 0xffff, 0xffff };
  GdkScreen *screen;
  gboolean wayland = g_strcmp0 (g_getenv ("GDK_BACKEND"), "wayland") == 0;

  /* before gdk_init_check, so display open and screen setup are covered too.
   * The X11 baseline run is clean (checked under Xvfb), so it is fatal too. */
  install_fatal_criticals ();

  if (!gdk_init_check (&argc, &argv))
    {
      g_printerr ("ydk_demo: display open failed\n");
      return 1;
    }

  screen = gdk_screen_get_default ();
  if (!screen)
    {
      g_printerr ("ydk_demo: no default screen\n");
      return 78;
    }

  attr.window_type = GDK_WINDOW_TOPLEVEL;
  attr.wclass = GDK_INPUT_OUTPUT;
  attr.x = 10;
  attr.y = 10;
  attr.width = WINDOW_WIDTH;
  attr.height = WINDOW_HEIGHT;
  attr.event_mask = GDK_EXPOSURE_MASK | GDK_KEY_PRESS_MASK |
                    GDK_BUTTON_PRESS_MASK;
  attr.wmclass_name = "ydk_demo";
  attr.wmclass_class = "YdkDemo";

  main_window = gdk_window_new (gdk_get_default_root_window (), &attr,
                                GDK_WA_X | GDK_WA_Y | GDK_WA_WMCLASS);
  if (!main_window)
    {
      g_printerr ("ydk_demo: window creation not supported on this "
                  "backend (exit 77)\n");
      return 77;
    }

  gdk_color_parse ("#ffffff", &bg);
  gdk_colormap_alloc_color (gdk_screen_get_system_colormap (screen), &bg,
			    FALSE, TRUE);
  gdk_window_set_background (main_window, &bg);

  gdk_event_handler_set (event_handler, NULL, NULL);

  main_loop = g_main_loop_new (NULL, FALSE);

  gdk_window_show (main_window);

  if (wayland)
    {
      /* run every exercise so one failure does not hide the others; each
       * prints its own PASS or FAIL line */
      int failed = 0;

      failed |= m4_exercise () != 0;
      failed |= m5_exercise () != 0;
      failed |= m5c_exercise () != 0;
      failed |= m5v_exercise () != 0;
      failed |= m6_exercise () != 0;
      failed |= m6b_exercise () != 0;
      failed |= m7_exercise () != 0;
      failed |= m8_exercise () != 0;
      if (failed)
	return 1;

      /* the M5 window checks leave test marks in the window; repaint the
       * scene so a screenshot can be compared with the X11 baseline */
      if (g_getenv ("YDK_DEMO_NO_MARKS"))
	draw_scene (main_window);
      g_print ("ydk_demo: exercises done\n");
    }
  else if (g_strcmp0 (g_getenv ("GDK_BACKEND"), "x11") == 0 &&
	   m6b_x11_exercise () != 0)
    return 1;

  /* valgrind runs this 20 to 50 times slower, see scripts/valgrind-smoke.sh */
  g_timeout_add (g_getenv ("YDK_DEMO_VALGRIND") ? TICK_TIMEOUT_MS * 20 : TICK_TIMEOUT_MS,
		 tick_timeout, NULL);
  g_timeout_add (g_getenv ("YDK_DEMO_VALGRIND") ? QUIT_TIMEOUT_MS * 20 : QUIT_TIMEOUT_MS,
		 quit_timeout, NULL);

  g_main_loop_run (main_loop);

  gdk_window_destroy (main_window);
  gdk_display_close (gdk_display_get_default ());

  g_main_loop_unref (main_loop);

  return 0;
}
