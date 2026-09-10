/*
 * gdkscreen-wayland.c
 *
 * ydk-wayland fork: Wayland GdkScreen subclass.
 *
 * Wayland treats the whole display as one logical screen, so this object
 * carries the compositor geometry (taken from the wl_output found during
 * display open) and answers the core dispatch queries that used to be
 * hardcoded in the X11 backend.
 *
 * M2 status: the screen owns a TRUE_COLOR system visual + colormap (both
 * hand-built: the X11 ones are useless here) and a real root window so
 * that gdk_window_new(..., NULL parent) succeeds.
 */

#include "config.h"

#include <wayland-client.h>

#include <ydk/gdk.h>
#include <ydk/gdkscreen.h>

#include "gdkprivate-wayland.h"

#ifdef GDK_COMPILATION
#include "gdkalias.h"
#endif

static void gdkw_screen_wayland_finalize (GObject *object);

G_DEFINE_TYPE (GDKWScreenWayland, gdkw_screen_wayland, GDK_TYPE_SCREEN)

static GdkDisplay *
gdkw_screen_wayland_get_display (GdkScreen *screen)
{
  return GDKW_SCREEN_WAYLAND (screen)->display;
}

static gint
gdkw_screen_wayland_get_width (GdkScreen *screen)
{
  return GDKW_SCREEN_WAYLAND (screen)->width;
}

static gint
gdkw_screen_wayland_get_height (GdkScreen *screen)
{
  return GDKW_SCREEN_WAYLAND (screen)->height;
}

static GdkWindow *
gdkw_screen_wayland_get_root_window (GdkScreen *screen)
{
  return GDKW_SCREEN_WAYLAND (screen)->root_window;
}

static GdkVisual *
gdkw_screen_wayland_get_system_visual (GdkScreen *screen)
{
  return GDKW_SCREEN_WAYLAND (screen)->system_visual;
}

static GdkColormap *
gdkw_screen_wayland_get_system_colormap (GdkScreen *screen)
{
  return GDKW_SCREEN_WAYLAND (screen)->system_colormap;
}

static void
gdkw_screen_wayland_finalize (GObject *object)
{
  GDKWScreenWayland *screen = GDKW_SCREEN_WAYLAND (object);

  if (screen->root_window)
    g_object_unref (screen->root_window);
  if (screen->system_colormap)
    g_object_unref (screen->system_colormap);
  if (screen->system_visual)
    g_object_unref (screen->system_visual);

  screen->display = NULL;

  G_OBJECT_CLASS (gdkw_screen_wayland_parent_class)->finalize (object);
}

static void
gdkw_screen_wayland_class_init (GDKWScreenWaylandClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GdkScreenClass *screen_class = GDK_SCREEN_CLASS (klass);

  object_class->finalize = gdkw_screen_wayland_finalize;

  screen_class->get_display = gdkw_screen_wayland_get_display;
  screen_class->get_width = gdkw_screen_wayland_get_width;
  screen_class->get_height = gdkw_screen_wayland_get_height;
  screen_class->get_root_window = gdkw_screen_wayland_get_root_window;
  screen_class->get_system_visual = gdkw_screen_wayland_get_system_visual;
  screen_class->get_system_colormap = gdkw_screen_wayland_get_system_colormap;
}

static void
gdkw_screen_wayland_init (GDKWScreenWayland *screen)
{
  screen->display = NULL;
  screen->width = 0;
  screen->height = 0;
  screen->system_visual = NULL;
  screen->system_colormap = NULL;
  screen->root_window = NULL;
}

/* A Wayland compositor is always 32 bits per pixel, ARGB.  The system
 * visual mirrors that: little-endian RGBA byte order, standard 8-bit
 * channel masks for a depth-24 overlay.  All structural fields are created
 * by hand since the X11 visual machinery does not exist here. */
GdkVisual *
_gdk_wayland_screen_create_system_visual (void)
{
  GdkVisual *visual;

  visual = g_object_new (GDK_TYPE_VISUAL, NULL);

  visual->type = GDK_VISUAL_TRUE_COLOR;
  visual->depth = 24;
  visual->byte_order = GDK_LSB_FIRST;
  visual->colormap_size = 0;
  visual->bits_per_rgb = 8;

  visual->red_mask = 0xff0000;
  visual->red_shift = 16;
  visual->red_prec = 8;

  visual->green_mask = 0x00ff00;
  visual->green_shift = 8;
  visual->green_prec = 8;

  visual->blue_mask = 0x0000ff;
  visual->blue_shift = 0;
  visual->blue_prec = 8;

  return visual;
}

GdkColormap *
_gdk_wayland_screen_create_colormap (GdkScreen *screen, GdkVisual *visual)
{
  GdkColormap *colormap;

  colormap = g_object_new (GDK_TYPE_COLORMAP, NULL);
  colormap->visual = visual;
  colormap->size = visual->colormap_size;
  colormap->colors = NULL;

  return colormap;
}

/* Root window construction mirrors the X11 backend (gdkwindow-x11.c):
 * the root cannot go through gdk_window_new() because GDK_WINDOW_ROOT is
 * rejected by the type switch, so it is built by hand.  Its window impl
 * has no wl_surface (nothing is ever drawn or presented on the root). */
static GdkWindow *
create_root_window (GDKWScreenWayland *screen)
{
  GdkWindow *root;
  GdkWindowObject *private;
  GDKWWindowImplWayland *impl;

  root = g_object_new (GDK_TYPE_WINDOW, NULL);

  private = (GdkWindowObject *) root;
  private->impl = g_object_new (GDKW_TYPE_WINDOW_IMPL_WAYLAND, NULL);
  private->impl_window = private;

  impl = GDKW_WINDOW_IMPL_WAYLAND (private->impl);
  impl->screen = GDK_SCREEN (screen);
  impl->wrapper = root;
  impl->colormap = screen->system_colormap;
  g_object_ref (impl->colormap);

  private->window_type = GDK_WINDOW_ROOT;
  private->depth = screen->system_visual->depth;

  private->x = 0;
  private->y = 0;
  private->abs_x = 0;
  private->abs_y = 0;
  private->width = screen->width;
  private->height = screen->height;
  private->viewable = TRUE;

  /* structure events on the root: nothing is wired to deliver them yet */
  private->event_mask = GDK_STRUCTURE_MASK;

  return root;
}

GdkScreen *
_gdk_wayland_screen_new (GdkDisplay *display,
			 gint        width,
			 gint        height)
{
  GDKWScreenWayland *screen;

  screen = g_object_new (GDKW_TYPE_SCREEN_WAYLAND, NULL);
  screen->display = display;
  screen->width = width;
  screen->height = height;

  screen->system_visual = _gdk_wayland_screen_create_system_visual ();
  screen->system_colormap =
    _gdk_wayland_screen_create_colormap (GDK_SCREEN (screen),
					 screen->system_visual);

  screen->root_window = create_root_window (screen);

  return GDK_SCREEN (screen);
}