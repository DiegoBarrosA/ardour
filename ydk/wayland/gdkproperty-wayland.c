/*
 * gdkproperty-wayland.c
 *
 * ydk-wayland fork: Wayland backend atoms and window properties.
 *
 * Wayland has no server-side atoms or window properties.  GdkAtom is kept
 * purely as a process-local string <-> small integer table (a GHashTable
 * plus a GPtrArray for the reverse lookup).  The predefined atoms occupy
 * the same indices as in the X11 backend, because public constants such as
 * GDK_SELECTION_PRIMARY are compile-time indices (see gdkselection.h).
 * Property get/change/delete are safe no-ops.
 *
 * Symbols are namespaced `_gdk_wayland_*`; ydk/gdkbackend.c owns the
 * canonical public names and forwards here when Wayland is selected.
 */

#include "config.h"

#include <string.h>

#include <glib.h>

#include <ydk/gdk.h>
#include "gdkprivate-wayland.h"

#ifdef GDK_COMPILATION
#include "ydk/gdkalias.h"
#endif

#define ATOM_TO_INDEX(atom) (GPOINTER_TO_UINT (atom))
#define INDEX_TO_ATOM(index) ((GdkAtom) GUINT_TO_POINTER (index))

G_LOCK_DEFINE_STATIC (atom_table);

static GPtrArray *atom_array;
static GHashTable *atom_hash;

/* Index 0 is reserved for GDK_NONE.  Order must match the predefined
 * atom numbering used by gdkselection.h (X11 predefined atoms 1..68,
 * then CLIPBOARD = 69). */
static const gchar * const predefined_atoms[] = {
  "",
  "PRIMARY", "SECONDARY", "ARC", "ATOM", "BITMAP", "CARDINAL", "COLORMAP",
  "CURSOR", "CUT_BUFFER0", "CUT_BUFFER1", "CUT_BUFFER2", "CUT_BUFFER3",
  "CUT_BUFFER4", "CUT_BUFFER5", "CUT_BUFFER6", "CUT_BUFFER7", "DRAWABLE",
  "FONT", "INTEGER", "PIXMAP", "POINT", "RECTANGLE", "RESOURCE_MANAGER",
  "RGB_COLOR_MAP", "RGB_BEST_MAP", "RGB_BLUE_MAP", "RGB_DEFAULT_MAP",
  "RGB_GRAY_MAP", "RGB_GREEN_MAP", "RGB_RED_MAP", "STRING", "VISUALID",
  "WINDOW", "WM_COMMAND", "WM_HINTS", "WM_CLIENT_MACHINE", "WM_ICON_NAME",
  "WM_ICON_SIZE", "WM_NAME", "WM_NORMAL_HINTS", "WM_SIZE_HINTS",
  "WM_ZOOM_HINTS", "MIN_SPACE", "NORM_SPACE", "MAX_SPACE", "END_SPACE",
  "SUPERSCRIPT_X", "SUPERSCRIPT_Y", "SUBSCRIPT_X", "SUBSCRIPT_Y",
  "UNDERLINE_POSITION", "UNDERLINE_THICKNESS", "STRIKEOUT_ASCENT",
  "STRIKEOUT_DESCENT", "ITALIC_ANGLE", "X_HEIGHT", "QUAD_WIDTH", "WEIGHT",
  "POINT_SIZE", "RESOLUTION", "COPYRIGHT", "NOTICE", "FONT_NAME",
  "FAMILY_NAME", "FULL_NAME", "CAP_HEIGHT", "WM_CLASS", "WM_TRANSIENT_FOR",
  "CLIPBOARD"
};

static void
atom_table_check_init (void)
{
  guint i;

  if (atom_hash)
    return;

  atom_hash = g_hash_table_new (g_str_hash, g_str_equal);
  atom_array = g_ptr_array_new ();

  for (i = 0; i < G_N_ELEMENTS (predefined_atoms); i++)
    {
      g_ptr_array_add (atom_array, (gchar *) predefined_atoms[i]);
      /* index 0 is GDK_NONE: it must not be reachable by name, since a
       * stored 0 is indistinguishable from "not found" in the lookup. */
      if (i > 0)
	g_hash_table_insert (atom_hash, (gchar *) predefined_atoms[i],
			     GUINT_TO_POINTER (i));
    }
}

static GdkAtom
intern_atom (const gchar *atom_name,
	     gboolean     dup,
	     gboolean     only_if_exists)
{
  GdkAtom result;

  g_return_val_if_fail (atom_name != NULL, GDK_NONE);

  G_LOCK (atom_table);

  atom_table_check_init ();

  result = INDEX_TO_ATOM (GPOINTER_TO_UINT (g_hash_table_lookup (atom_hash,
								 atom_name)));
  if (!result && !only_if_exists && atom_name[0] != '\0')
    {
      guint index = atom_array->len;
      gchar *name = dup ? g_strdup (atom_name) : (gchar *) atom_name;

      result = INDEX_TO_ATOM (index);
      g_ptr_array_add (atom_array, name);
      g_hash_table_insert (atom_hash, name, GUINT_TO_POINTER (index));
    }

  G_UNLOCK (atom_table);

  return result;
}

GdkAtom
_gdk_wayland_atom_intern (const gchar *atom_name,
			  gboolean     only_if_exists)
{
  return intern_atom (atom_name, TRUE, only_if_exists);
}

GdkAtom
_gdk_wayland_atom_intern_static_string (const gchar *atom_name)
{
  return intern_atom (atom_name, FALSE, FALSE);
}

gchar *
_gdk_wayland_atom_name (GdkAtom atom)
{
  gchar *result = NULL;

  G_LOCK (atom_table);

  atom_table_check_init ();

  if (ATOM_TO_INDEX (atom) < atom_array->len)
    result = g_strdup (g_ptr_array_index (atom_array, ATOM_TO_INDEX (atom)));

  G_UNLOCK (atom_table);

  return result;
}

gboolean
_gdk_wayland_property_get (GdkWindow   *window,
			   GdkAtom      property,
			   GdkAtom      type,
			   gulong       offset,
			   gulong       length,
			   gint         pdelete,
			   GdkAtom     *actual_property_type,
			   gint        *actual_format_type,
			   gint        *actual_length,
			   guchar     **data)
{
  /* Wayland has no window properties. */
  return FALSE;
}

void
_gdk_wayland_property_change (GdkWindow    *window,
			      GdkAtom       property,
			      GdkAtom       type,
			      gint          format,
			      GdkPropMode   mode,
			      const guchar *data,
			      gint          nelements)
{
}

void
_gdk_wayland_property_delete (GdkWindow *window,
			      GdkAtom    property)
{
}
