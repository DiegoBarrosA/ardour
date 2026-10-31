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

#include <math.h>
#include <string.h>

#include "gdkprivate-wayland.h"
#include "gdkseat-wayland.h"
#include "gdkm10-wayland.h"

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

/* Wayland half of the dispatched gdk_visual_get_screen ().  The hand-built
 * visual carries no screen back pointer (the X11 one is a GdkVisualPrivate,
 * this one is a bare GdkVisual) and a Wayland client has one screen, so the
 * default screen is the answer.  The X11 function would read past the end
 * of the object. */
GdkScreen *
_gdk_wayland_visual_get_screen (GdkVisual *visual)
{
  g_return_val_if_fail (GDK_IS_VISUAL (visual), NULL);

  return gdk_screen_get_default ();
}

GdkColormap *
_gdk_wayland_screen_create_colormap (GdkScreen *screen, GdkVisual *visual)
{
  /* A plain GDK_TYPE_COLORMAP is the X11 type: its finalizer dereferences a
   * NULL private->screen.  Use the Wayland subclass (gdkcolor-wayland.c). */
  return _gdk_wayland_colormap_new_for_screen (screen, visual);
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
/* ---------------------------------------------------------------------------
 * M6: the remaining public screen functions.  Wayland has one logical screen
 * and (for now) one monitor; sizes come from wl_output data with fallbacks.
 * Nothing here logs a critical for an out of range monitor number.
 * ------------------------------------------------------------------------ */

static gint
screen_dim_mm (GdkScreen *screen, gint pixels, gint output_mm)
{
  gdouble dpi;

  if (output_mm > 0)
    return output_mm;
  if (pixels <= 0)
    return 0;

  dpi = gdk_screen_get_resolution (screen);
  if (dpi <= 0)
    dpi = 96.0;

  return MAX (1, (gint) floor (pixels * 25.4 / dpi + 0.5));
}

static GDKWOutputInfo *
screen_output_info (GdkScreen *screen)
{
  GdkDisplay *display = GDKW_SCREEN_WAYLAND (screen)->display;

  if (display == NULL)
    return NULL;
  return _gdk_wayland_display_get_output_info (display);
}

gint
_gdk_wayland_screen_get_width_mm (GdkScreen *screen)
{
  GDKWOutputInfo *info = screen_output_info (screen);

  /* M10: mm of the logical bounding box of all outputs */
  if (GDKW_SCREEN_WAYLAND (screen)->display != NULL)
    {
      gint mm = _gdk_wayland_scale_screen_width_mm
        (GDKW_SCREEN_WAYLAND (screen)->display);

      if (mm > 0)
        return mm;
    }

  return screen_dim_mm (screen, GDKW_SCREEN_WAYLAND (screen)->width,
                        info ? info->mm_width : 0);
}

gint
_gdk_wayland_screen_get_height_mm (GdkScreen *screen)
{
  GDKWOutputInfo *info = screen_output_info (screen);

  if (GDKW_SCREEN_WAYLAND (screen)->display != NULL)
    {
      gint mm = _gdk_wayland_scale_screen_height_mm
        (GDKW_SCREEN_WAYLAND (screen)->display);

      if (mm > 0)
        return mm;
    }

  return screen_dim_mm (screen, GDKW_SCREEN_WAYLAND (screen)->height,
                        info ? info->mm_height : 0);
}

gint
_gdk_wayland_screen_get_number (GdkScreen *screen)
{
  return 0;
}

gint
_gdk_wayland_screen_get_n_monitors (GdkScreen *screen)
{
  GdkDisplay *display = screen ? GDKW_SCREEN_WAYLAND (screen)->display : NULL;

  /* M10: every bound wl_output is a monitor; one without any output */
  if (display != NULL)
    return _gdk_wayland_scale_n_monitors (display);

  return 1;
}

gint
_gdk_wayland_screen_get_primary_monitor (GdkScreen *screen)
{
  return 0;
}

void
_gdk_wayland_screen_get_monitor_geometry (GdkScreen    *screen,
                                          gint          monitor_num,
                                          GdkRectangle *dest)
{
  if (dest == NULL)
    return;

  dest->x = 0;
  dest->y = 0;
  dest->width = 0;
  dest->height = 0;

  if (screen != NULL && GDKW_SCREEN_WAYLAND (screen)->display != NULL)
    {
      /* M10: logical geometry of the output */
      _gdk_wayland_scale_monitor_geometry
        (GDKW_SCREEN_WAYLAND (screen)->display, monitor_num, dest);
      return;
    }

  if (monitor_num == 0 && screen != NULL)
    {
      dest->width = GDKW_SCREEN_WAYLAND (screen)->width;
      dest->height = GDKW_SCREEN_WAYLAND (screen)->height;
    }
}

gint
_gdk_wayland_screen_get_monitor_width_mm (GdkScreen *screen, gint monitor_num)
{
  if (screen != NULL && GDKW_SCREEN_WAYLAND (screen)->display != NULL)
    return _gdk_wayland_scale_monitor_width_mm
      (GDKW_SCREEN_WAYLAND (screen)->display, monitor_num);

  return monitor_num == 0 ? _gdk_wayland_screen_get_width_mm (screen) : -1;
}

gint
_gdk_wayland_screen_get_monitor_height_mm (GdkScreen *screen, gint monitor_num)
{
  if (screen != NULL && GDKW_SCREEN_WAYLAND (screen)->display != NULL)
    return _gdk_wayland_scale_monitor_height_mm
      (GDKW_SCREEN_WAYLAND (screen)->display, monitor_num);

  return monitor_num == 0 ? _gdk_wayland_screen_get_height_mm (screen) : -1;
}

gchar *
_gdk_wayland_screen_get_monitor_plug_name (GdkScreen *screen, gint monitor_num)
{
  GDKWOutputInfo *info;

  if (screen != NULL && GDKW_SCREEN_WAYLAND (screen)->display != NULL)
    return _gdk_wayland_scale_monitor_plug_name
      (GDKW_SCREEN_WAYLAND (screen)->display, monitor_num);

  if (monitor_num != 0)
    return NULL;

  info = screen_output_info (screen);
  if (info != NULL && info->model != NULL && info->model[0] != '\0')
    return g_strdup (info->model);

  return g_strdup ("WL-1");
}

gboolean
_gdk_wayland_screen_is_composited (GdkScreen *screen)
{
  return TRUE;
}

gchar *
_gdk_wayland_screen_make_display_name (GdkScreen *screen)
{
  GdkDisplay *display = GDKW_SCREEN_WAYLAND (screen)->display;
  GdkDisplayClass *klass;
  const gchar *name = NULL;

  if (display != NULL)
    {
      klass = GDK_DISPLAY_GET_CLASS (display);
      if (klass->get_display_name != NULL)
        name = klass->get_display_name (display);
    }

  return g_strdup (name ? name : "");
}

GdkWindow *
_gdk_wayland_screen_get_active_window (GdkScreen *screen)
{
  GdkDisplay *display = GDKW_SCREEN_WAYLAND (screen)->display;
  GDKWSeat *seat;
  GdkWindow *wrapper;

  if (display == NULL)
    return NULL;

  seat = _gdk_wayland_display_get_seat (display);
  if (seat->keyboard_focus == NULL)
    return NULL;

  wrapper = seat->keyboard_focus->wrapper;
  if (wrapper == NULL || GDK_WINDOW_DESTROYED (wrapper))
    return NULL;

  return g_object_ref (wrapper);
}

GList *
_gdk_wayland_screen_get_window_stack (GdkScreen *screen)
{
  GdkDisplay *display = GDKW_SCREEN_WAYLAND (screen)->display;
  GList *stack = NULL;
  GSList *l;

  if (display == NULL)
    return NULL;

  /* display->windows is prepended on creation, so walking it front to back
   * and prepending again yields oldest first, i.e. bottom to top */
  for (l = ((GDKWDisplayWayland *) display)->windows; l != NULL; l = l->next)
    {
      GDKWWindowImplWayland *impl = l->data;
      GdkWindow *wrapper = impl ? impl->wrapper : NULL;

      if (wrapper == NULL || GDK_WINDOW_DESTROYED (wrapper) ||
          !GDK_WINDOW_IS_MAPPED (wrapper))
        continue;

      stack = g_list_prepend (stack, g_object_ref (wrapper));
    }

  return stack;
}

void
_gdk_wayland_screen_broadcast_client_message (GdkScreen *screen,
                                              GdkEvent  *event)
{
  /* Wayland has no client messages */
}

gboolean
_gdk_wayland_screen_get_setting (GdkScreen   *screen,
                                 const gchar *name,
                                 GValue      *value)
{
  static const struct
  {
    const gchar *name;
    gint         value;
  } int_settings[] =
  {
    { "gtk-double-click-time",     400 },
    { "gtk-double-click-distance", 5 },
    { "gtk-dnd-drag-threshold",    8 },
    { "gtk-cursor-blink-time",     1200 },
    { "gtk-timeout-initial",       200 },
    { "gtk-timeout-repeat",        20 }
  };
  static const struct
  {
    const gchar *name;
    gboolean     value;
  } bool_settings[] =
  {
    { "gtk-cursor-blink",      TRUE },
    { "gtk-enable-animations", TRUE }
  };
  GValue tmp = { 0, };
  gboolean found = FALSE;
  gboolean ok = FALSE;
  guint i;

  if (name == NULL || value == NULL)
    return FALSE;

  for (i = 0; i < G_N_ELEMENTS (int_settings) && !found; i++)
    if (strcmp (name, int_settings[i].name) == 0)
      {
        g_value_init (&tmp, G_TYPE_INT);
        g_value_set_int (&tmp, int_settings[i].value);
        found = TRUE;
      }

  for (i = 0; i < G_N_ELEMENTS (bool_settings) && !found; i++)
    if (strcmp (name, bool_settings[i].name) == 0)
      {
        g_value_init (&tmp, G_TYPE_BOOLEAN);
        g_value_set_boolean (&tmp, bool_settings[i].value);
        found = TRUE;
      }

  if (!found)
    return FALSE;

  if (G_VALUE_TYPE (value) != 0 &&
      g_value_type_transformable (G_VALUE_TYPE (&tmp), G_VALUE_TYPE (value)))
    ok = g_value_transform (&tmp, value);

  g_value_unset (&tmp);
  return ok;
}
