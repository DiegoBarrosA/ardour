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
 *   0  window created and drawn (X11 baseline and Wayland since M2)
 *   77 skipped: backend has no window support yet
 *   78 skipped: display opened but primary screen unavailable
 *   1  nothing worked (display open failed)
 */

#include <glib.h>
#include <ydk/gdk.h>
#include <ydk/gdkcairo.h>
#include <ydk/gdkkeysyms.h>

#define WINDOW_WIDTH  640
#define WINDOW_HEIGHT 480
#define QUIT_TIMEOUT_MS  2500
#define TICK_TIMEOUT_MS    50

static GMainLoop *main_loop;
static GdkWindow *main_window;

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

int
main (int argc, char **argv)
{
  GdkWindowAttr attr;
  GdkColor bg;
  GdkScreen *screen;

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

  g_timeout_add (TICK_TIMEOUT_MS, tick_timeout, NULL);
  g_timeout_add (QUIT_TIMEOUT_MS, quit_timeout, NULL);

  g_main_loop_run (main_loop);

  gdk_window_destroy (main_window);
  gdk_display_close (gdk_display_get_default ());

  g_main_loop_unref (main_loop);

  return 0;
}
