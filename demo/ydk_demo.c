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
 *   0  window created and drawn (X11 baseline)
 *   77 skipped: backend has no window support yet (Wayland until M1)
 *   78 skipped: display opened but primary screen unavailable
 *   1  nothing worked (display open failed)
 */

#include <unistd.h>

#include <ydk/gdk.h>
#include <ydk/gdkcairo.h>

#define WINDOW_WIDTH  640
#define WINDOW_HEIGHT 480

static void
draw_scene (GdkWindow *window)
{
  cairo_t *cr;

  /* The scene is deterministic so screenshots can be diffed: solid
   * background, one disc, one rectangle. */
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

int
main (int argc, char **argv)
{
  GdkWindowAttr attr;
  GdkWindow *window;
  GdkColor bg;
  GdkScreen *screen;
  gint width, height;

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

  width = gdk_screen_get_width (screen);
  height = gdk_screen_get_height (screen);

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

  window = gdk_window_new (gdk_get_default_root_window (), &attr,
                           GDK_WA_X | GDK_WA_Y | GDK_WA_WMCLASS);
  if (!window)
    {
      /* Expected for the Wayland backend until M1. */
      g_printerr ("ydk_demo: window creation not supported on this "
                  "backend (exit 77)\n");
      return 77;
    }

  gdk_color_parse ("#ffffff", &bg);
  gdk_window_set_background (window, &bg);

  gdk_window_show (window);

  /* Let the server commit the mapping before we capture it. */
  gdk_flush ();
  usleep (300 * 1000);
  gdk_flush ();

  draw_scene (window);

  /* Leave the frame on screen long enough for the harness screenshot. */
  gdk_flush ();
  usleep (1500 * 1000);
  gdk_flush ();

  gdk_window_destroy (window);
  gdk_display_close (gdk_display_get_default ());

  return 0;
}