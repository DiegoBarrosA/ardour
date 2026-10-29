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
 * Window properties are a real per-window table (see the end of this file).
 *
 * Symbols are namespaced `_gdk_wayland_*`; ydk/gdkbackend.c owns the
 * canonical public names and forwards here when Wayland is selected.
 */

#include "config.h"

#include <string.h>

#include <glib.h>

#include <ydk/gdk.h>
#include "gdkprivate-wayland.h"
#include "gdkm8-wayland.h"

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


/* ---------------------------------------------------------------------------
 * Window property table (M8, piece "property")
 *
 * One GHashTable per GdkWindow (atom -> GDKWProp), hung on the window as qdata
 * GDKW_PROPERTY_DATA_KEY.  X11 semantics, minus the server: no PropertyNotify,
 * ATOM and ATOM_PAIR values are kept as GdkAtom verbatim.
 * ------------------------------------------------------------------------ */

typedef struct
{
  GdkAtom  type;
  gint     format;
  gsize    nbytes;
  guchar  *data;     /* NULL when nbytes == 0 */
} GDKWProp;

typedef struct
{
  GHashTable *props; /* GdkAtom (direct) -> GDKWProp * */
  gsize       total; /* sum of nbytes */
} GDKWPropTable;

static void
prop_free (gpointer p)
{
  GDKWProp *prop = p;

  g_free (prop->data);
  g_slice_free (GDKWProp, prop);
}

static void
prop_table_free (gpointer p)
{
  GDKWPropTable *table = p;

  g_hash_table_destroy (table->props);
  g_slice_free (GDKWPropTable, table);
}

/* The window a property call acts on: NULL means the default root.  Returns
 * NULL for destroyed and non Wayland windows. */
static GdkWindow *
prop_window (GdkWindow *window)
{
  if (window == NULL)
    window = gdk_get_default_root_window ();

  if (window == NULL || !GDK_IS_WINDOW (window) ||
      GDK_WINDOW_DESTROYED (window) ||
      _gdk_wayland_window_get_impl (window) == NULL)
    return NULL;

  return window;
}

static GDKWPropTable *
prop_table_peek (GdkWindow *window)
{
  return g_object_get_data (G_OBJECT (window), GDKW_PROPERTY_DATA_KEY);
}

static GDKWPropTable *
prop_table_ensure (GdkWindow *window)
{
  GDKWPropTable *table = prop_table_peek (window);

  if (table == NULL)
    {
      table = g_slice_new0 (GDKWPropTable);
      table->props = g_hash_table_new_full (g_direct_hash, g_direct_equal,
					    NULL, prop_free);
      g_object_set_data_full (G_OBJECT (window), GDKW_PROPERTY_DATA_KEY,
			      table, prop_table_free);
    }

  return table;
}

static gsize
prop_item_size (gint format)
{
  switch (format)
    {
    case 8:  return 1;
    case 16: return 2;
    case 32: return sizeof (long);
    default: return 0;
    }
}

void
_gdk_wayland_property_window_destroyed (GdkWindow *window)
{
  if (window == NULL || !GDK_IS_WINDOW (window))
    return;

  /* Frees the table through the qdata destroy notify.  Idempotent. */
  if (prop_table_peek (window) != NULL)
    g_object_set_data (G_OBJECT (window), GDKW_PROPERTY_DATA_KEY, NULL);
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
  GDKWPropTable *table;
  GDKWProp *prop;
  gsize start, rest, want, unit;

  window = prop_window (window);
  if (window == NULL || property == GDK_NONE || length == 0)
    return FALSE;

  table = prop_table_peek (window);
  prop = table ? g_hash_table_lookup (table->props, property) : NULL;
  if (prop == NULL)
    return FALSE;

  if (actual_property_type)
    *actual_property_type = prop->type;
  if (actual_format_type)
    *actual_format_type = prop->format;

  if (type != GDK_NONE && type != prop->type)
    return FALSE;

  /* offset and length count 32 bit units of the X wire format.  Formats 8
   * and 16 are packed, so a unit is 4 bytes.  Format 32 is stored as C longs
   * (as Xlib hands it out), so a unit is one item, sizeof (long) bytes.
   * Beyond the end is X's BadValue. */
  unit = (prop->format == 32) ? sizeof (long) : 4;
  if (offset > G_MAXSIZE / unit || offset * unit > prop->nbytes)
    return FALSE;
  start = offset * unit;
  rest = prop->nbytes - start;

  /* @length is in bytes (the X11 backend turns it into (length + 3) / 4 wire
   * units for XGetWindowProperty); G_MAXULONG and any value that cannot be
   * scaled mean "the rest". */
  if (length == G_MAXULONG || length > G_MAXSIZE / unit - 3)
    want = rest;
  else
    want = MIN ((gsize) ((length + 3) / 4) * unit, rest);

  if (actual_length)
    *actual_length = (gint) MIN (want, (gsize) G_MAXINT);
  if (data)
    *data = want > 0 ? g_memdup2 (prop->data + start, want) : NULL;

  /* Delete when the whole remainder was returned (X: bytes_after == 0). */
  if (pdelete && want == rest)
    {
      table->total -= prop->nbytes;
      g_hash_table_remove (table->props, property);
    }

  return TRUE;
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
  GDKWPropTable *table;
  GDKWProp *old, *prop;
  gsize isize, add, keep, newlen;

  window = prop_window (window);
  isize = prop_item_size (format);
  if (window == NULL || property == GDK_NONE || isize == 0 ||
      nelements < 0 || (nelements > 0 && data == NULL))
    return;

  add = (gsize) nelements * isize;

  table = prop_table_ensure (window);
  old = g_hash_table_lookup (table->props, property);

  /* PREPEND / APPEND to a missing property is a plain store. */
  if (old == NULL)
    mode = GDK_PROP_MODE_REPLACE;
  else if (mode != GDK_PROP_MODE_REPLACE &&
	   (old->type != type || old->format != format))
    return; /* X: BadMatch */

  keep = (mode == GDK_PROP_MODE_REPLACE) ? 0 : old->nbytes;
  newlen = keep + add;

  if (newlen > GDKW_SELECTION_MAX_BYTES ||
      table->total - (old ? old->nbytes : 0) + newlen > GDKW_SELECTION_MAX_BYTES)
    {
      g_debug ("ydk-wayland: property change refused, window would exceed %d bytes",
	       GDKW_SELECTION_MAX_BYTES);
      return;
    }

  prop = g_slice_new0 (GDKWProp);
  prop->type = type;
  prop->format = format;
  prop->nbytes = newlen;
  if (newlen > 0)
    {
      prop->data = g_malloc (newlen);
      if (mode == GDK_PROP_MODE_PREPEND)
	{
	  if (add)
	    memcpy (prop->data, data, add);
	  if (keep)
	    memcpy (prop->data + add, old->data, keep);
	}
      else
	{
	  if (keep)
	    memcpy (prop->data, old->data, keep);
	  if (add)
	    memcpy (prop->data + keep, data, add);
	}
    }

  table->total = table->total - (old ? old->nbytes : 0) + newlen;
  g_hash_table_replace (table->props, property, prop); /* frees old */
}

void
_gdk_wayland_property_delete (GdkWindow *window,
			      GdkAtom    property)
{
  GDKWPropTable *table;
  GDKWProp *prop;

  window = prop_window (window);
  if (window == NULL || property == GDK_NONE)
    return;

  table = prop_table_peek (window);
  prop = table ? g_hash_table_lookup (table->props, property) : NULL;
  if (prop == NULL)
    return;

  table->total -= prop->nbytes;
  g_hash_table_remove (table->props, property);
}

/*************** test hooks (M8) ***************/

gint
_gdk_wayland_test_property_count (GdkWindow *window)
{
  GDKWPropTable *table;

  if (_gdk_wayland_window_get_impl (window) == NULL)
    return -1;

  table = prop_table_peek (window);
  return table ? (gint) g_hash_table_size (table->props) : 0;
}

gint
_gdk_wayland_test_property_bytes (GdkWindow *window)
{
  GDKWPropTable *table;

  if (_gdk_wayland_window_get_impl (window) == NULL)
    return -1;

  table = prop_table_peek (window);
  return table ? (gint) MIN (table->total, (gsize) G_MAXINT) : 0;
}
