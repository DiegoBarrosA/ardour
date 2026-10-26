/*
 * gdkvisual-wayland.c
 *
 * ydk-wayland fork: Wayland half of the GdkVisual / default-colormap /
 * rgba query family (gap F8).
 *
 * The X11 implementations (x11/gdkvisual-x11.c, x11/gdkscreen-x11.c) read
 * visual tables out of a GdkScreenX11 (GDK_SCREEN_X11 (screen)->visuals,
 * ->available_depths, ->default_colormap, ->rgba_visual, ...).  Run against
 * the Wayland screen object that cast lands on an unrelated struct: the
 * result is garbage or a segfault.  gdkrgb.c and gdkvisual.c reach those
 * functions through gdk_screen_list_visuals (), which is why every
 * gdk_rgb_* entry point that picks a visual crashed as well.
 *
 * A Wayland compositor only ever offers two pixel layouts to a client:
 *
 *   XRGB8888  depth 24  the screen's system visual, built by
 *                       gdkscreen-wayland.c
 *   ARGB8888  depth 32  the "rgba" visual, built here on first use (same
 *                       masks as the system visual, depth 32)
 *
 * both TrueColor, 8 bits per channel, no palette.  The two visuals are
 * exposed through the same functions the X11 backend implements, in the
 * same order X11 uses (largest depth first, so "best" is the ARGB one):
 *
 *   list            { argb(32), system(24) }
 *   depths          { 32, 24 }
 *   types           { TRUE_COLOR }
 *
 * gdk_rgb_choose_visual () still lands on the system visual: the rgb scorer
 * rates depth 32 as 0 and depth 24 TrueColor as the best quality.
 *
 * Ownership: the system visual and colormap belong to the screen.  The
 * argb visual, the argb colormap and an explicitly set default colormap are
 * held by the screen object through g_object_set_data_full (): the screen
 * struct is shared and not edited by this file.  Colormaps do not ref their
 * visual (see gdkcolor-wayland.c), so the destroy order of those entries at
 * screen finalization does not matter.
 *
 * Naming: every definition is `_gdk_wayland_*`, the target of the dispatchers
 * in gdkbackend.c and of the rename macros in x11/ydk/gdkwindowing-x11.h.
 * There are deliberately no alias blocks in this file, and no Xlib.
 *
 * Not defined here (already generic, or X11-only API, see the wiring notes):
 * gdk_visual_get_screen () lives in gdkscreen-wayland.c, the system
 * visual/colormap are GdkScreenClass vfuncs, gdk_visual_get_type () stays
 * X11-owned.
 */

#include "config.h"

#include <glib.h>
#include <glib-object.h>

#include <ydk/gdk.h>
#include <ydk/gdkcolor.h>
#include <ydk/gdkscreen.h>
#include <ydk/gdkvisual.h>

#include "gdkprivate-wayland.h"

#ifdef GDK_COMPILATION
#include "gdkalias.h"
#endif

#define ARGB_VISUAL_KEY      "ydk-wayland-argb-visual"
#define ARGB_COLORMAP_KEY    "ydk-wayland-argb-colormap"
#define DEFAULT_COLORMAP_KEY "ydk-wayland-default-colormap"

#define N_VISUALS 2

/* Largest first, as the X11 backend sorts them.  Never freed, never
 * written: gdk_query_depths () / gdk_query_visual_types () hand these out
 * and document that the caller must not free them. */
static gint available_depths[N_VISUALS] = { 32, 24 };
static GdkVisualType available_types[1] = { GDK_VISUAL_TRUE_COLOR };

/* ---- helpers ---- */

/* The ARGB visual of @screen, built on first use.  Same structure as the
 * system visual (little-endian, 0xff0000/0x00ff00/0x0000ff, 8 bits per
 * channel), only the depth differs: the extra 8 bits are alpha, as for the
 * X11 "rgba visual". */
static GdkVisual *
screen_argb_visual (GdkScreen *screen)
{
  GdkVisual *visual;

  visual = g_object_get_data (G_OBJECT (screen), ARGB_VISUAL_KEY);
  if (!visual)
    {
      visual = _gdk_wayland_screen_create_system_visual ();
      visual->depth = 32;

      g_object_set_data_full (G_OBJECT (screen), ARGB_VISUAL_KEY,
			      visual, g_object_unref);
    }

  return visual;
}

/* Visual @i of @screen in list order: 0 is argb, 1 is system. */
static GdkVisual *
screen_visual_at (GDKWScreenWayland *screen, guint i)
{
  if (i == 0)
    return screen_argb_visual (GDK_SCREEN (screen));

  return screen->system_visual;
}

/* The Wayland default screen, or NULL (with a critical) when there is none.
 * The visual functions without a screen argument all work on it, like their
 * X11 counterparts. */
static GDKWScreenWayland *
default_wayland_screen (void)
{
  GdkScreen *screen = gdk_screen_get_default ();

  g_return_val_if_fail (screen != NULL, NULL);
  g_return_val_if_fail (GDKW_IS_SCREEN_WAYLAND (screen), NULL);

  return GDKW_SCREEN_WAYLAND (screen);
}

/* ---- default-screen visual queries ---- */

/* Best available depth: the largest one, 32. */
gint
_gdk_wayland_visual_get_best_depth (void)
{
  return available_depths[0];
}

/* Best available visual type.  The only type there is. */
GdkVisualType
_gdk_wayland_visual_get_best_type (void)
{
  return available_types[0];
}

/* The visual with the most colors: the first of the list, ARGB8888. */
GdkVisual *
_gdk_wayland_visual_get_best (void)
{
  GDKWScreenWayland *screen = default_wayland_screen ();

  if (!screen)
    return NULL;

  return screen_visual_at (screen, 0);
}

GdkVisual *
_gdk_wayland_visual_get_best_with_depth (gint depth)
{
  GDKWScreenWayland *screen = default_wayland_screen ();
  guint i;

  if (!screen)
    return NULL;

  for (i = 0; i < N_VISUALS; i++)
    {
      GdkVisual *visual = screen_visual_at (screen, i);

      if (visual && visual->depth == depth)
	return visual;
    }

  return NULL;
}

GdkVisual *
_gdk_wayland_visual_get_best_with_type (GdkVisualType visual_type)
{
  GDKWScreenWayland *screen = default_wayland_screen ();
  guint i;

  if (!screen)
    return NULL;

  for (i = 0; i < N_VISUALS; i++)
    {
      GdkVisual *visual = screen_visual_at (screen, i);

      if (visual && visual->type == visual_type)
	return visual;
    }

  return NULL;
}

GdkVisual *
_gdk_wayland_visual_get_best_with_both (gint          depth,
					GdkVisualType visual_type)
{
  GDKWScreenWayland *screen = default_wayland_screen ();
  guint i;

  if (!screen)
    return NULL;

  for (i = 0; i < N_VISUALS; i++)
    {
      GdkVisual *visual = screen_visual_at (screen, i);

      if (visual && visual->depth == depth && visual->type == visual_type)
	return visual;
    }

  return NULL;
}

/* The arrays are static and owned by this file; callers must not free them. */
void
_gdk_wayland_query_depths (gint **depths,
			   gint  *count)
{
  g_return_if_fail (depths != NULL);
  g_return_if_fail (count != NULL);

  *count = G_N_ELEMENTS (available_depths);
  *depths = available_depths;
}

void
_gdk_wayland_query_visual_types (GdkVisualType **visual_types,
				 gint           *count)
{
  g_return_if_fail (visual_types != NULL);
  g_return_if_fail (count != NULL);

  *count = G_N_ELEMENTS (available_types);
  *visual_types = available_types;
}

/* ---- per-screen visual queries ---- */

/* Returns a new list, argb first; free the list, not the visuals. */
GList *
_gdk_wayland_screen_list_visuals (GdkScreen *screen)
{
  GList *list = NULL;
  guint i;

  g_return_val_if_fail (GDK_IS_SCREEN (screen), NULL);
  g_return_val_if_fail (GDKW_IS_SCREEN_WAYLAND (screen), NULL);

  for (i = 0; i < N_VISUALS; i++)
    {
      GdkVisual *visual = screen_visual_at (GDKW_SCREEN_WAYLAND (screen), i);

      if (visual)
	list = g_list_append (list, visual);
    }

  return list;
}

/* Wayland compositors always composite, so the rgba visual is always
 * available, unlike X11 where it needs a depth-32 TrueColor visual. */
GdkVisual *
_gdk_wayland_screen_get_rgba_visual (GdkScreen *screen)
{
  g_return_val_if_fail (GDK_IS_SCREEN (screen), NULL);
  g_return_val_if_fail (GDKW_IS_SCREEN_WAYLAND (screen), NULL);

  return screen_argb_visual (screen);
}

GdkColormap *
_gdk_wayland_screen_get_rgba_colormap (GdkScreen *screen)
{
  GdkColormap *colormap;

  g_return_val_if_fail (GDK_IS_SCREEN (screen), NULL);
  g_return_val_if_fail (GDKW_IS_SCREEN_WAYLAND (screen), NULL);

  colormap = g_object_get_data (G_OBJECT (screen), ARGB_COLORMAP_KEY);
  if (!colormap)
    {
      /* Not gdk_colormap_new (): that one targets the default screen. */
      colormap = _gdk_wayland_colormap_new_for_screen (screen,
						       screen_argb_visual (screen));
      g_object_set_data_full (G_OBJECT (screen), ARGB_COLORMAP_KEY,
			      colormap, g_object_unref);
    }

  return colormap;
}

/* ---- default colormap ---- */

/* The X11 backend sets the default colormap to the system colormap when it
 * builds the root window and returns whatever was last set.  Same here:
 * until gdk_screen_set_default_colormap () is called, the system colormap
 * is the default. */
GdkColormap *
_gdk_wayland_screen_get_default_colormap (GdkScreen *screen)
{
  GdkColormap *colormap;

  g_return_val_if_fail (GDK_IS_SCREEN (screen), NULL);
  g_return_val_if_fail (GDKW_IS_SCREEN_WAYLAND (screen), NULL);

  colormap = g_object_get_data (G_OBJECT (screen), DEFAULT_COLORMAP_KEY);
  if (colormap)
    return colormap;

  return GDKW_SCREEN_WAYLAND (screen)->system_colormap;
}

/* The screen takes a reference.  The new reference is taken before the old
 * entry is replaced (and unref'd by the destroy notify), so setting the
 * colormap that is already the default is safe. */
void
_gdk_wayland_screen_set_default_colormap (GdkScreen   *screen,
					  GdkColormap *colormap)
{
  g_return_if_fail (GDK_IS_SCREEN (screen));
  g_return_if_fail (GDKW_IS_SCREEN_WAYLAND (screen));
  g_return_if_fail (GDK_IS_COLORMAP (colormap));

  g_object_set_data_full (G_OBJECT (screen), DEFAULT_COLORMAP_KEY,
			  g_object_ref (colormap), g_object_unref);
}
