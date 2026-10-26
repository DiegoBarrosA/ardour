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
 *      GDK_BACKEND=wayland also every M4, M5, M5C, M5V and M6 check passed
 *      (see m4_exercise, m5_exercise, m5c_exercise, m5v_exercise,
 *      m6_exercise)
 *   77 skipped: backend has no window support yet
 *   78 skipped: display opened but primary screen unavailable
 *   1  nothing worked (display open failed), an M4 / M5 / M5C / M5V / M6 check
 *      failed, or a g_critical / g_error was logged (see fatal_log_handler)
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <glib.h>
#include <ydk/gdk.h>
#include <ydk/gdkcairo.h>
#include <ydk/gdkkeysyms.h>
#include <ydk/gdkseat-wayland.h>

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
  gdk_selection_owner_set_for_display (display, main_window, clipboard,
				       GDK_CURRENT_TIME, FALSE);

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
	    "selection: stub convert should refuse (property NONE)");

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

static void
m4_drag (void)
{
  GList *targets = NULL;
  GdkDragContext *ctx;

  targets = g_list_append (targets,
			   GDK_ATOM_TO_POINTER (gdk_atom_intern ("text/plain",
								 FALSE)));
  ctx = gdk_drag_begin (main_window, targets);
  M4_CHECK (ctx == NULL, "drag: begin is a stub (M8) and returns NULL");
  if (ctx)
    g_object_unref (ctx);
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

  /* properties are safe no-ops on Wayland */
  gdk_property_change (main_window, a, GDK_TARGET_STRING, 8,
		       GDK_PROP_MODE_REPLACE, bytes, 3);
  M4_CHECK (!gdk_property_get (main_window, a, GDK_NONE, 0, 16, FALSE,
			       &actual_type, &actual_format, &actual_length,
			       &data), "property: get should find nothing");
  gdk_property_delete (main_window, a);
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

int
main (int argc, char **argv)
{
  GdkWindowAttr attr;
  GdkColor bg;
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
      if (failed)
	return 1;

      /* the M5 window checks leave test marks in the window; repaint the
       * scene so a screenshot can be compared with the X11 baseline */
      if (g_getenv ("YDK_DEMO_NO_MARKS"))
	draw_scene (main_window);
      g_print ("ydk_demo: exercises done\n");
    }

  g_timeout_add (TICK_TIMEOUT_MS, tick_timeout, NULL);
  g_timeout_add (QUIT_TIMEOUT_MS, quit_timeout, NULL);

  g_main_loop_run (main_loop);

  gdk_window_destroy (main_window);
  gdk_display_close (gdk_display_get_default ());

  g_main_loop_unref (main_loop);

  return 0;
}
