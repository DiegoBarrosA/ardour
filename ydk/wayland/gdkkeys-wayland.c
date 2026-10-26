/*
 * gdkkeys-wayland.c
 *
 * Wayland keymap implementation based on libxkbcommon.
 *
 * The compositor hands over a complete XKB keymap through a file descriptor
 * in the wl_keyboard keymap event; we mmap it, build an xkb_keymap / state,
 * and translate keycodes to GDK keyvals/modifiers for the event path.
 *
 * Keycode convention: every hardware_keycode that crosses the GdkKeymap API
 * is the evdev code plus 8, which is also the xkb keycode.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "config.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <xkbcommon/xkbcommon.h>
#include <pango/pango.h>

#include <ydk/gdk.h>
#include <ydk/gdkkeys.h>
#include <ydk/gdkkeysyms.h>

#include "gdkprivate-wayland.h"
#include "gdkseat-wayland.h"

#ifdef GDK_COMPILATION
#include "gdkalias.h"
#endif

/* M6B PROTOTYPES */
PangoDirection _gdk_wayland_keymap_get_direction (GdkKeymap *keymap);
gboolean _gdk_wayland_keymap_have_bidi_layouts (GdkKeymap *keymap);
gboolean _gdk_wayland_keymap_get_caps_lock_state (GdkKeymap *keymap);
gboolean _gdk_wayland_keymap_get_entries_for_keyval (GdkKeymap *keymap, guint keyval, GdkKeymapKey **keys, gint *n_keys);
gboolean _gdk_wayland_keymap_get_entries_for_keycode (GdkKeymap *keymap, guint hardware_keycode, GdkKeymapKey **keys, guint **keyvals, gint *n_entries);
guint _gdk_wayland_keymap_lookup_key (GdkKeymap *keymap, const GdkKeymapKey *key);
void _gdk_wayland_keymap_add_virtual_modifiers (GdkKeymap *keymap, GdkModifierType *state);
gboolean _gdk_wayland_keymap_map_virtual_modifiers (GdkKeymap *keymap, GdkModifierType *state);
gchar *_gdk_wayland_keyval_name (guint keyval);
guint _gdk_wayland_keyval_from_name (const gchar *keyval_name);
void _gdk_wayland_keyval_convert_case (guint symbol, guint *lower, guint *upper);

typedef struct _GdkKeymapWayland GdkKeymapWayland;
typedef struct _GdkKeymapWaylandClass GdkKeymapWaylandClass;

#define GDK_KEYMAP_WAYLAND(o) \
  (G_TYPE_CHECK_INSTANCE_CAST ((o), GDK_TYPE_KEYMAP_WAYLAND, GdkKeymapWayland))

struct _GdkKeymapWayland
{
  GdkKeymap parent_instance;

  GdkDisplay *display;
  struct xkb_context *context;
  struct xkb_keymap *xkb_keymap;
  struct xkb_state *xkb_state;
  /* One scratch state per keymap, used by translate_keyboard_state so the
   * caller's state and group arguments are honoured instead of the live
   * state. */
  struct xkb_state *scratch_state;
  /* Cached layout directions (PangoDirection), valid bit per layout. */
  PangoDirection dirs[32];
  guint32 dirs_valid;
  /* Real modifier bit (0..7) to virtual GDK masks (SUPER, HYPER, META). */
  guint modmap[8];
  gboolean modmap_valid;
};

struct _GdkKeymapWaylandClass
{
  GdkKeymapClass parent_class;
};

static GType gdk_keymap_wayland_get_type (void) G_GNUC_CONST;
static gboolean gdk_keymap_wayland_translate_keyboard_state
  (GdkKeymap *keymap, guint hardware_keycode, GdkModifierType state,
   gint group, guint *keyval, gint *effective_group, gint *level,
   GdkModifierType *consumed_modifiers);
static gboolean gdk_keymap_wayland_key_is_modifier
  (GdkKeymap *keymap, guint hardware_keycode);

G_DEFINE_TYPE (GdkKeymapWayland, gdk_keymap_wayland, GDK_TYPE_KEYMAP)

static void
gdk_keymap_wayland_finalize (GObject *object)
{
  GdkKeymapWayland *self = GDK_KEYMAP_WAYLAND (object);

  if (self->scratch_state)
    xkb_state_unref (self->scratch_state);
  if (self->xkb_state)
    xkb_state_unref (self->xkb_state);
  if (self->xkb_keymap)
    xkb_keymap_unref (self->xkb_keymap);
  if (self->context)
    xkb_context_unref (self->context);

  G_OBJECT_CLASS (gdk_keymap_wayland_parent_class)->finalize (object);
}

static void
gdk_keymap_wayland_class_init (GdkKeymapWaylandClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = gdk_keymap_wayland_finalize;

  GDK_KEYMAP_CLASS (klass)->translate_keyboard_state =
    gdk_keymap_wayland_translate_keyboard_state;
  GDK_KEYMAP_CLASS (klass)->key_is_modifier =
    gdk_keymap_wayland_key_is_modifier;
}

static void
gdk_keymap_wayland_init (GdkKeymapWayland *self)
{
  self->context = NULL;
  self->xkb_keymap = NULL;
  self->xkb_state = NULL;
  self->scratch_state = NULL;
  self->dirs_valid = 0;
  self->modmap_valid = FALSE;
}

/* ---- display keymap factory ------------------------------------------ */

GdkKeymap *
gdkw_display_wayland_get_keymap (GdkDisplay *display)
{
  GDKWDisplayWayland *wd = GDKW_DISPLAY_WAYLAND (display);

  if (wd->keymap)
    return wd->keymap;

  wd->keymap = g_object_new (gdk_keymap_wayland_get_type (), NULL);
  GDK_KEYMAP (wd->keymap)->display = display;
  wd->keymap->display = display;

  return wd->keymap;
}

/* ---- modifier helpers ------------------------------------------------ */

static const struct
{
  const char     *name;
  GdkModifierType gdk_mask;
} mod_table[] = {
  { XKB_MOD_NAME_SHIFT, GDK_SHIFT_MASK },
  { XKB_MOD_NAME_CAPS,  GDK_LOCK_MASK },
  { XKB_MOD_NAME_CTRL,  GDK_CONTROL_MASK },
  { XKB_MOD_NAME_ALT,   GDK_MOD1_MASK },
  { XKB_MOD_NAME_NUM,   GDK_MOD2_MASK },
  { XKB_MOD_NAME_MOD3,  GDK_MOD3_MASK },
  { XKB_MOD_NAME_LOGO,  GDK_MOD4_MASK },
  { XKB_MOD_NAME_MOD5,  GDK_MOD5_MASK }
};

static GdkModifierType
gdk_modmask_for_xkb (struct xkb_keymap *keymap, xkb_mod_mask_t mask)
{
  GdkModifierType result = 0;
  guint i;

  if (keymap == NULL)
    return 0;

  for (i = 0; i < G_N_ELEMENTS (mod_table); i++)
    {
      xkb_mod_index_t idx = xkb_keymap_mod_get_index (keymap,
						      mod_table[i].name);

      /* absent modifiers report XKB_MOD_INVALID; 1u << that is undefined */
      if (idx == XKB_MOD_INVALID || idx >= 32)
	continue;
      if (mask & (1u << idx))
	result |= mod_table[i].gdk_mask;
    }

  return result;
}

static xkb_mod_mask_t
xkb_modmask_for_gdk (struct xkb_keymap *keymap, GdkModifierType mods)
{
  xkb_mod_mask_t result = 0;
  guint i;

  if (keymap == NULL)
    return 0;

  for (i = 0; i < G_N_ELEMENTS (mod_table); i++)
    {
      xkb_mod_index_t idx;

      if (!(mods & mod_table[i].gdk_mask))
	continue;

      idx = xkb_keymap_mod_get_index (keymap, mod_table[i].name);
      if (idx == XKB_MOD_INVALID || idx >= 32)
	continue;

      result |= 1u << idx;
    }

  return result;
}

/* ---- translate_keyboard_state ---------------------------------------- */

static gboolean
gdk_keymap_wayland_translate_keyboard_state (GdkKeymap       *keymap,
					     guint            hardware_keycode,
					     GdkModifierType  state,
					     gint             group,
					     guint           *keyval,
					     gint            *effective_group,
					     gint            *level,
					     GdkModifierType *consumed_modifiers)
{
  GdkKeymapWayland *self = GDK_KEYMAP_WAYLAND (keymap);
  xkb_layout_index_t layout;
  xkb_layout_index_t nlayouts;
  xkb_keysym_t sym;
  xkb_mod_mask_t consumed;

  if (keyval)
    *keyval = GDK_KEY_VoidSymbol;
  if (effective_group)
    *effective_group = 0;
  if (level)
    *level = 0;
  if (consumed_modifiers)
    *consumed_modifiers = 0;

  if (!self->scratch_state || !self->xkb_keymap)
    return FALSE;

  nlayouts = xkb_keymap_num_layouts_for_key (self->xkb_keymap,
					     (xkb_keycode_t) hardware_keycode);
  if (nlayouts == 0)
    return FALSE;

  layout = group < 0 ? 0 : (xkb_layout_index_t) group;
  if (layout >= nlayouts)
    layout = nlayouts - 1;

  xkb_state_update_mask (self->scratch_state,
			 xkb_modmask_for_gdk (self->xkb_keymap, state),
			 0, 0, 0, 0, layout);

  sym = xkb_state_key_get_one_sym (self->scratch_state,
				   (xkb_keycode_t) hardware_keycode);
  if (sym == XKB_KEY_NoSymbol)
    return FALSE;

  consumed = xkb_state_key_get_consumed_mods2 (self->scratch_state,
					       (xkb_keycode_t) hardware_keycode,
					       XKB_CONSUMED_MODE_GTK);

  if (keyval)
    *keyval = sym;
  if (effective_group)
    *effective_group = (gint) layout;
  if (level)
    *level = (gint) xkb_state_key_get_level (self->scratch_state,
					     (xkb_keycode_t) hardware_keycode,
					     layout);
  if (consumed_modifiers)
    *consumed_modifiers = gdk_modmask_for_xkb (self->xkb_keymap, consumed);

  return TRUE;
}

/* ---- key_is_modifier ------------------------------------------------- */

static gboolean
gdk_keymap_wayland_key_is_modifier (GdkKeymap *keymap,
				    guint      hardware_keycode)
{
  GdkKeymapWayland *self = GDK_KEYMAP_WAYLAND (keymap);
  const xkb_keysym_t *syms;
  int nsyms, i;

  if (!self->xkb_keymap)
    return FALSE;

  nsyms = xkb_keymap_key_get_syms_by_level (self->xkb_keymap,
					    (xkb_keycode_t) hardware_keycode,
					    0, 0, &syms);

  for (i = 0; i < nsyms; i++)
    {
      xkb_keysym_t s = syms[i];

      if ((s >= XKB_KEY_Shift_L && s <= XKB_KEY_Hyper_R) ||
	  s == XKB_KEY_Mode_switch ||
	  s == XKB_KEY_Num_Lock ||
	  (s >= XKB_KEY_ISO_Level2_Latch && s <= XKB_KEY_ISO_Last_Group_Lock) ||
	  (s >= XKB_KEY_ISO_Level5_Shift && s <= XKB_KEY_ISO_Level5_Lock))
	return TRUE;
    }

  return FALSE;
}

/* ---- public helpers -------------------------------------------------- */

void
_gdk_wayland_keymap_set_keymap (GDKWDisplayWayland *display,
				int format,
				int fd,
				guint size)
{
  GdkKeymapWayland *self;
  gchar *text = NULL;

  if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || size == 0 ||
      display->keymap == NULL)
    {
      close (fd);
      return;
    }

  self = GDK_KEYMAP_WAYLAND (display->keymap);

  if (!self->context)
    {
      self->context = xkb_context_new (XKB_CONTEXT_NO_FLAGS);
      if (!self->context)
	{
	  close (fd);
	  return;
	}
    }

  {
    const gchar *buf = mmap (NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);

    if (buf != MAP_FAILED)
      {
	/* copy so the text is NUL terminated whatever the compositor sent */
	text = g_strndup (buf, strnlen (buf, size));
	munmap ((gpointer) buf, size);
      }
  }

  close (fd);

  if (text == NULL)
    return;

  if (self->scratch_state)
    {
      xkb_state_unref (self->scratch_state);
      self->scratch_state = NULL;
    }
  if (self->xkb_state)
    {
      xkb_state_unref (self->xkb_state);
      self->xkb_state = NULL;
    }
  if (self->xkb_keymap)
    {
      xkb_keymap_unref (self->xkb_keymap);
      self->xkb_keymap = NULL;
    }

  self->dirs_valid = 0;
  self->modmap_valid = FALSE;
  memset (self->modmap, 0, sizeof self->modmap);

  self->xkb_keymap = xkb_keymap_new_from_string (self->context, text,
						 XKB_KEYMAP_FORMAT_TEXT_V1,
						 XKB_KEYMAP_COMPILE_NO_FLAGS);
  g_free (text);

  if (!self->xkb_keymap)
    return;

  self->xkb_state = xkb_state_new (self->xkb_keymap);
  self->scratch_state = xkb_state_new (self->xkb_keymap);

  /* The class slot is only the signal's default handler offset and nothing
   * assigns it, so call through the signal like gdkkeys-x11.c does.  This
   * also reaches handlers GTK connects. */
  g_signal_emit_by_name (self, "keys-changed");
}

GdkModifierType
_gdk_wayland_keymap_serialize_mods (GdkKeymap *keymap, guint mask)
{
  GdkKeymapWayland *self;

  if (!keymap)
    return 0;

  self = GDK_KEYMAP_WAYLAND (keymap);

  if (!self->xkb_keymap)
    return 0;

  return gdk_modmask_for_xkb (self->xkb_keymap, mask);
}

gint
_gdk_wayland_keymap_group (GdkKeymap *keymap)
{
  GdkKeymapWayland *self;
  xkb_layout_mask_t layout;

  if (!keymap)
    return 0;

  self = GDK_KEYMAP_WAYLAND (keymap);

  if (!self->xkb_state)
    return 0;

  layout = xkb_state_serialize_layout (self->xkb_state,
				       XKB_STATE_LAYOUT_EFFECTIVE);

  return (gint) layout;
}

struct xkb_state *
_gdk_wayland_keymap_get_xkb_state (GdkKeymap *keymap)
{
  GdkKeymapWayland *self;

  if (!keymap)
    return NULL;

  self = GDK_KEYMAP_WAYLAND (keymap);

  return self->xkb_state;
}

guint
_gdk_wayland_keymap_xkb_mods_from_gdk (GdkKeymap      *keymap,
				       GdkModifierType mods)
{
  GdkKeymapWayland *self;

  if (!keymap)
    return 0;

  self = GDK_KEYMAP_WAYLAND (keymap);

  return xkb_modmask_for_gdk (self->xkb_keymap, mods);
}

gboolean
_gdk_wayland_keymap_key_repeats (GdkKeymap *keymap,
				 guint      hardware_keycode)
{
  GdkKeymapWayland *self;

  if (!keymap)
    return FALSE;

  self = GDK_KEYMAP_WAYLAND (keymap);

  if (!self->xkb_keymap)
    return FALSE;

  return xkb_keymap_key_repeats (self->xkb_keymap,
				 (xkb_keycode_t) hardware_keycode) != 0;
}

/* ---- default keymap text ---------------------------------------------- */

/* Small keymap used when the xkb rule files are not installed (sandboxed
 * builds, minimal containers).  It covers the keys the test hooks use. */
static const gchar builtin_keymap_text[] =
  "xkb_keymap {\n"
  "xkb_keycodes \"ydk\" {\n"
  "  minimum = 8;\n"
  "  maximum = 255;\n"
  "  <ESC>  = 9;\n"
  "  <RTRN> = 36;\n"
  "  <LCTL> = 37;\n"
  "  <AC01> = 38;\n"
  "  <AC02> = 39;\n"
  "  <LFSH> = 50;\n"
  "  <LALT> = 64;\n"
  "  <SPCE> = 65;\n"
  "  <CAPS> = 66;\n"
  "};\n"
  "xkb_types \"ydk\" {\n"
  "  virtual_modifiers NumLock;\n"
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
  "  type \"ALPHABETIC\" {\n"
  "    modifiers = Shift+Lock;\n"
  "    map[Shift] = Level2;\n"
  "    map[Lock] = Level2;\n"
  "    level_name[Level1] = \"Base\";\n"
  "    level_name[Level2] = \"Caps\";\n"
  "  };\n"
  "};\n"
  "xkb_compatibility \"ydk\" {\n"
  "  interpret Any+AnyOf(all) {\n"
  "    action = SetMods(modifiers=modMapMods,clearLocks);\n"
  "  };\n"
  "};\n"
  "xkb_symbols \"ydk\" {\n"
  "  key <ESC>  { [ Escape ] };\n"
  "  key <RTRN> { [ Return ] };\n"
  "  key <SPCE> { [ space ] };\n"
  "  key <AC01> { type = \"ALPHABETIC\", [ a, A ] };\n"
  "  key <AC02> { type = \"ALPHABETIC\", [ s, S ] };\n"
  "  key <LCTL> { [ Control_L ] };\n"
  "  key <LFSH> { [ Shift_L ] };\n"
  "  key <LALT> { [ Alt_L ] };\n"
  "  key <CAPS> { [ Caps_Lock ] };\n"
  "  modifier_map Shift   { <LFSH> };\n"
  "  modifier_map Lock    { <CAPS> };\n"
  "  modifier_map Control { <LCTL> };\n"
  "  modifier_map Mod1    { <LALT> };\n"
  "};\n"
  "};\n";

gchar *
_gdk_wayland_keymap_default_text (gsize *length)
{
  struct xkb_context *ctx;
  gchar *text = NULL;

  ctx = xkb_context_new (XKB_CONTEXT_NO_FLAGS);
  if (ctx)
    {
      struct xkb_rule_names names;
      struct xkb_keymap *km;

      memset (&names, 0, sizeof names);
      names.rules = "evdev";
      names.model = "pc105";
      names.layout = "us";

      km = xkb_keymap_new_from_names (ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
      if (km)
	{
	  char *t = xkb_keymap_get_as_string (km, XKB_KEYMAP_FORMAT_TEXT_V1);

	  if (t)
	    {
	      text = g_strdup (t);
	      free (t);
	    }
	  xkb_keymap_unref (km);
	}

      if (text == NULL)
	{
	  km = xkb_keymap_new_from_string (ctx, builtin_keymap_text,
					   XKB_KEYMAP_FORMAT_TEXT_V1,
					   XKB_KEYMAP_COMPILE_NO_FLAGS);
	  if (km)
	    {
	      text = g_strdup (builtin_keymap_text);
	      xkb_keymap_unref (km);
	    }
	}

      xkb_context_unref (ctx);
    }

  /* Never return NULL: the built-in text is the last resort so callers
   * always have something to feed the keymap listener. */
  if (text == NULL)
    text = g_strdup (builtin_keymap_text);

  if (length)
    *length = strlen (text);

  return text;
}

/* ---- M6b: public keymap and keyval functions ------------------------- */

/* Return the Wayland keymap for KEYMAP, or for the default keymap when
 * KEYMAP is NULL.  Return NULL when it is not a Wayland keymap. */
static GdkKeymapWayland *
keymap_wayland_for (GdkKeymap *keymap)
{
  if (keymap == NULL)
    keymap = gdk_keymap_get_default ();
  if (keymap == NULL ||
      !G_TYPE_CHECK_INSTANCE_TYPE (keymap, gdk_keymap_wayland_get_type ()))
    return NULL;
  return (GdkKeymapWayland *) keymap;
}

static PangoDirection
layout_direction (GdkKeymapWayland *self, xkb_layout_index_t layout)
{
  xkb_keycode_t key, min, max;
  guint rtl = 0, ltr = 0;
  PangoDirection dir;

  if (layout >= 32)
    return PANGO_DIRECTION_NEUTRAL;
  if (self->dirs_valid & (1u << layout))
    return self->dirs[layout];

  min = xkb_keymap_min_keycode (self->xkb_keymap);
  max = xkb_keymap_max_keycode (self->xkb_keymap);

  for (key = min; key <= max; key++)
    {
      const xkb_keysym_t *syms;
      guint32 ch;

      if (layout >= xkb_keymap_num_layouts_for_key (self->xkb_keymap, key))
	continue;
      if (xkb_keymap_key_get_syms_by_level (self->xkb_keymap, key, layout,
					    0, &syms) < 1)
	continue;

      ch = xkb_keysym_to_utf32 (syms[0]);
      if (ch == 0)
	continue;

      switch (pango_unichar_direction (ch))
	{
	case PANGO_DIRECTION_RTL:
	  rtl++;
	  break;
	case PANGO_DIRECTION_LTR:
	  ltr++;
	  break;
	default:
	  break;
	}
    }

  if (rtl == 0 && ltr == 0)
    dir = PANGO_DIRECTION_NEUTRAL;
  else
    dir = rtl > ltr ? PANGO_DIRECTION_RTL : PANGO_DIRECTION_LTR;

  self->dirs[layout] = dir;
  self->dirs_valid |= 1u << layout;
  return dir;
}

PangoDirection
_gdk_wayland_keymap_get_direction (GdkKeymap *keymap)
{
  GdkKeymapWayland *self = keymap_wayland_for (keymap);
  xkb_layout_index_t layout;

  if (self == NULL || self->xkb_keymap == NULL || self->xkb_state == NULL)
    return PANGO_DIRECTION_NEUTRAL;

  layout = xkb_state_serialize_layout (self->xkb_state,
				       XKB_STATE_LAYOUT_EFFECTIVE);
  return layout_direction (self, layout);
}

gboolean
_gdk_wayland_keymap_have_bidi_layouts (GdkKeymap *keymap)
{
  GdkKeymapWayland *self = keymap_wayland_for (keymap);
  xkb_layout_index_t i, n;
  gboolean have_ltr = FALSE, have_rtl = FALSE;

  if (self == NULL || self->xkb_keymap == NULL)
    return FALSE;

  n = xkb_keymap_num_layouts (self->xkb_keymap);
  for (i = 0; i < n; i++)
    {
      if (layout_direction (self, i) == PANGO_DIRECTION_RTL)
	have_rtl = TRUE;
      else
	have_ltr = TRUE;
    }

  return have_ltr && have_rtl;
}

gboolean
_gdk_wayland_keymap_get_caps_lock_state (GdkKeymap *keymap)
{
  GdkKeymapWayland *self = keymap_wayland_for (keymap);

  if (self == NULL || self->xkb_state == NULL)
    return FALSE;

  return xkb_state_mod_name_is_active (self->xkb_state, XKB_MOD_NAME_CAPS,
				       XKB_STATE_MODS_LOCKED) > 0;
}

gboolean
_gdk_wayland_keymap_get_entries_for_keyval (GdkKeymap     *keymap,
					    guint          keyval,
					    GdkKeymapKey **keys,
					    gint          *n_keys)
{
  GdkKeymapWayland *self = keymap_wayland_for (keymap);
  GArray *array;
  xkb_keycode_t key, min, max;
  gint n;

  if (keys)
    *keys = NULL;
  if (n_keys)
    *n_keys = 0;

  if (self == NULL || self->xkb_keymap == NULL)
    return FALSE;

  array = g_array_new (FALSE, FALSE, sizeof (GdkKeymapKey));
  min = xkb_keymap_min_keycode (self->xkb_keymap);
  max = xkb_keymap_max_keycode (self->xkb_keymap);

  for (key = min; key <= max; key++)
    {
      xkb_layout_index_t layout, nlayouts;

      nlayouts = xkb_keymap_num_layouts_for_key (self->xkb_keymap, key);
      for (layout = 0; layout < nlayouts; layout++)
	{
	  xkb_level_index_t level, nlevels;

	  nlevels = xkb_keymap_num_levels_for_key (self->xkb_keymap, key,
						   layout);
	  for (level = 0; level < nlevels; level++)
	    {
	      const xkb_keysym_t *syms;
	      int nsyms, i;

	      nsyms = xkb_keymap_key_get_syms_by_level (self->xkb_keymap, key,
							layout, level, &syms);
	      for (i = 0; i < nsyms; i++)
		{
		  if (syms[i] == keyval)
		    {
		      GdkKeymapKey k;

		      k.keycode = key;
		      k.group = (gint) layout;
		      k.level = (gint) level;
		      g_array_append_val (array, k);
		      break;
		    }
		}
	    }
	}
    }

  n = (gint) array->len;
  if (n > 0 && keys)
    *keys = (GdkKeymapKey *) g_array_free (array, FALSE);
  else
    g_array_free (array, TRUE);

  if (n_keys)
    *n_keys = n;

  return n > 0;
}

gboolean
_gdk_wayland_keymap_get_entries_for_keycode (GdkKeymap     *keymap,
					     guint          hardware_keycode,
					     GdkKeymapKey **keys,
					     guint        **keyvals,
					     gint          *n_entries)
{
  GdkKeymapWayland *self = keymap_wayland_for (keymap);
  GArray *karray, *varray;
  xkb_layout_index_t layout, nlayouts;
  gint n;

  if (keys)
    *keys = NULL;
  if (keyvals)
    *keyvals = NULL;
  if (n_entries)
    *n_entries = 0;

  if (self == NULL || self->xkb_keymap == NULL ||
      hardware_keycode < xkb_keymap_min_keycode (self->xkb_keymap) ||
      hardware_keycode > xkb_keymap_max_keycode (self->xkb_keymap))
    return FALSE;

  karray = g_array_new (FALSE, FALSE, sizeof (GdkKeymapKey));
  varray = g_array_new (FALSE, FALSE, sizeof (guint));

  nlayouts = xkb_keymap_num_layouts_for_key (self->xkb_keymap,
					     hardware_keycode);
  for (layout = 0; layout < nlayouts; layout++)
    {
      xkb_level_index_t level, nlevels;

      nlevels = xkb_keymap_num_levels_for_key (self->xkb_keymap,
					       hardware_keycode, layout);
      for (level = 0; level < nlevels; level++)
	{
	  const xkb_keysym_t *syms;
	  int nsyms;

	  nsyms = xkb_keymap_key_get_syms_by_level (self->xkb_keymap,
						    hardware_keycode, layout,
						    level, &syms);
	  if (nsyms > 0)
	    {
	      GdkKeymapKey k;
	      guint v = syms[0];

	      k.keycode = hardware_keycode;
	      k.group = (gint) layout;
	      k.level = (gint) level;
	      g_array_append_val (karray, k);
	      g_array_append_val (varray, v);
	    }
	}
    }

  n = (gint) karray->len;

  if (keys && n > 0)
    *keys = (GdkKeymapKey *) g_array_free (karray, FALSE);
  else
    g_array_free (karray, TRUE);

  if (keyvals && n > 0)
    *keyvals = (guint *) g_array_free (varray, FALSE);
  else
    g_array_free (varray, TRUE);

  if (n_entries)
    *n_entries = n;

  return n > 0;
}

guint
_gdk_wayland_keymap_lookup_key (GdkKeymap          *keymap,
				const GdkKeymapKey *key)
{
  GdkKeymapWayland *self = keymap_wayland_for (keymap);
  const xkb_keysym_t *syms;

  if (self == NULL || self->xkb_keymap == NULL || key == NULL ||
      key->group < 0 || key->level < 0)
    return 0;

  /* A group beyond the keymap is invalid.  A group that only this key lacks
   * wraps into range inside xkbcommon, the same as in translate_keyboard_state
   * (a one layout key like Return answers for every group). */
  if ((xkb_layout_index_t) key->group >=
      xkb_keymap_num_layouts (self->xkb_keymap))
    return 0;
  if (xkb_keymap_num_layouts_for_key (self->xkb_keymap, key->keycode) == 0)
    return 0;

  if (xkb_keymap_key_get_syms_by_level (self->xkb_keymap, key->keycode,
					key->group, key->level, &syms) != 1)
    return 0;

  return syms[0];
}

/* Virtual modifier a keysym stands for, or 0. */
static guint
virtual_mask_for_keysym (xkb_keysym_t sym)
{
  switch (sym)
    {
    case XKB_KEY_Super_L:
    case XKB_KEY_Super_R:
      return GDK_SUPER_MASK;
    case XKB_KEY_Hyper_L:
    case XKB_KEY_Hyper_R:
      return GDK_HYPER_MASK;
    case XKB_KEY_Meta_L:
    case XKB_KEY_Meta_R:
      return GDK_META_MASK;
    default:
      return 0;
    }
}

/* Build the real-bit to virtual-modifier table.  Find every key that has a
 * Super, Hyper or Meta keysym on any layout and level, press it in a scratch
 * state and see which real modifiers it sets.  Scanning all levels matters:
 * on a stock pc105 map the Alt key is { Alt_L, Meta_L }, so Meta shares
 * Mod1 with Alt, as it does on X11. */
static void
build_modmap (GdkKeymapWayland *self)
{
  struct xkb_state *st;
  xkb_keycode_t key, min, max;
  gboolean have_super = FALSE;

  memset (self->modmap, 0, sizeof self->modmap);
  self->modmap_valid = TRUE;

  if (self->xkb_keymap == NULL)
    return;

  st = xkb_state_new (self->xkb_keymap);
  min = xkb_keymap_min_keycode (self->xkb_keymap);
  max = xkb_keymap_max_keycode (self->xkb_keymap);

  for (key = min; st && key <= max; key++)
    {
      guint vmask = 0;
      xkb_mod_mask_t mods;
      xkb_layout_index_t layout, n_layouts;
      guint i;

      n_layouts = xkb_keymap_num_layouts_for_key (self->xkb_keymap, key);

      for (layout = 0; layout < n_layouts; layout++)
	{
	  xkb_level_index_t level, n_levels;

	  n_levels = xkb_keymap_num_levels_for_key (self->xkb_keymap, key,
						    layout);
	  for (level = 0; level < n_levels; level++)
	    {
	      const xkb_keysym_t *syms;
	      int n, k;

	      n = xkb_keymap_key_get_syms_by_level (self->xkb_keymap, key,
						    layout, level, &syms);
	      for (k = 0; k < n; k++)
		vmask |= virtual_mask_for_keysym (syms[k]);
	    }
	}

      if (vmask == 0)
	continue;

      xkb_state_update_key (st, key, XKB_KEY_DOWN);
      mods = xkb_state_serialize_mods (st, XKB_STATE_MODS_DEPRESSED);
      xkb_state_update_key (st, key, XKB_KEY_UP);

      for (i = 0; i < G_N_ELEMENTS (mod_table); i++)
	{
	  xkb_mod_index_t idx = xkb_keymap_mod_get_index (self->xkb_keymap,
							  mod_table[i].name);

	  if (idx == XKB_MOD_INVALID || idx >= 32 || !(mods & (1u << idx)))
	    continue;
	  self->modmap[i] |= vmask;
	  if (vmask & GDK_SUPER_MASK)
	    have_super = TRUE;
	}
    }

  if (st)
    xkb_state_unref (st);

  /* No Super key found: Mod4 is Super by convention. */
  if (!have_super)
    self->modmap[6] |= GDK_SUPER_MASK;
}

void
_gdk_wayland_keymap_add_virtual_modifiers (GdkKeymap       *keymap,
					   GdkModifierType *state)
{
  GdkKeymapWayland *self = keymap_wayland_for (keymap);
  int i;

  if (self == NULL || self->xkb_keymap == NULL || state == NULL)
    return;
  if (!self->modmap_valid)
    build_modmap (self);

  /* Mod1 to Mod5, like GTK2 on X11 (bit 3 is Mod1). */
  for (i = 3; i < 8; i++)
    {
      if ((1 << i) & *state)
	*state |= self->modmap[i] & (GDK_SUPER_MASK | GDK_HYPER_MASK |
				     GDK_META_MASK);
    }
}

gboolean
_gdk_wayland_keymap_map_virtual_modifiers (GdkKeymap       *keymap,
					   GdkModifierType *state)
{
  GdkKeymapWayland *self = keymap_wayland_for (keymap);
  static const guint vmods[] = { GDK_SUPER_MASK, GDK_HYPER_MASK, GDK_META_MASK };
  gboolean retval = TRUE;
  guint i, j;

  if (self == NULL || self->xkb_keymap == NULL || state == NULL)
    return TRUE;
  if (!self->modmap_valid)
    build_modmap (self);

  for (j = 0; j < G_N_ELEMENTS (vmods); j++)
    {
      if (!(*state & vmods[j]))
	continue;
      for (i = 3; i < 8; i++)
	{
	  if (!(self->modmap[i] & vmods[j]))
	    continue;
	  if (*state & (1 << i))
	    retval = FALSE;
	  else
	    *state |= 1 << i;
	}
    }

  return retval;
}

gchar *
_gdk_wayland_keyval_name (guint keyval)
{
  gchar buf[64];
  gint len;

  switch (keyval)
    {
    case GDK_KEY_Page_Up:
      return "Page_Up";
    case GDK_KEY_Page_Down:
      return "Page_Down";
    case GDK_KEY_KP_Page_Up:
      return "KP_Page_Up";
    case GDK_KEY_KP_Page_Down:
      return "KP_Page_Down";
    case 0:
      return NULL;
    }

  /* Same as the X11 backend: the whole 0x01000000 range is named Uxxxx. */
  if (keyval >= 0x01000000 && keyval <= 0x0110ffff)
    {
      g_snprintf (buf, sizeof buf, "U%04X", keyval & 0x00ffffff);
      return (gchar *) g_intern_string (buf);
    }

  len = xkb_keysym_get_name (keyval, buf, sizeof buf);
  if (len <= 0 || (buf[0] == '0' && buf[1] == 'x'))
    return NULL;

  return (gchar *) g_intern_string (buf);
}

guint
_gdk_wayland_keyval_from_name (const gchar *keyval_name)
{
  g_return_val_if_fail (keyval_name != NULL, 0);

  return xkb_keysym_from_name (keyval_name, XKB_KEYSYM_NO_FLAGS);
}


/* Keysyms where xkbcommon's case mapping differs from Xlib's XConvertCase.
 * Generated by comparing both over 0..0xffffff.  Sorted by symbol.  Each
 * row is { symbol, lower, upper }.  Needed so the Wayland results match the
 * X11 backend (Latin-2/3/4 legacy keysyms, a few Greek and Cyrillic ones). */
static const struct
{
  guint symbol;
  guint lower;
  guint upper;
} case_exceptions[] = {
  { 0x1a4, 0x1b4, 0x1a4 },
  { 0x1b4, 0x1b4, 0x1a4 },
  { 0x1c1, 0x1e1, 0x1c1 },
  { 0x1c2, 0x1e2, 0x1c2 },
  { 0x1c4, 0x1e4, 0x1c4 },
  { 0x1c7, 0x1e7, 0x1c7 },
  { 0x1c9, 0x1e9, 0x1c9 },
  { 0x1cb, 0x1eb, 0x1cb },
  { 0x1cd, 0x1ed, 0x1cd },
  { 0x1ce, 0x1ee, 0x1ce },
  { 0x1d3, 0x1f3, 0x1d3 },
  { 0x1d4, 0x1f4, 0x1d4 },
  { 0x1d6, 0x1f6, 0x1d6 },
  { 0x1d7, 0x1f7, 0x1d7 },
  { 0x1da, 0x1fa, 0x1da },
  { 0x1dc, 0x1fc, 0x1dc },
  { 0x1dd, 0x1fd, 0x1dd },
  { 0x1e1, 0x1e1, 0x1c1 },
  { 0x1e2, 0x1e2, 0x1c2 },
  { 0x1e4, 0x1e4, 0x1c4 },
  { 0x1e7, 0x1e7, 0x1c7 },
  { 0x1e9, 0x1e9, 0x1c9 },
  { 0x1eb, 0x1eb, 0x1cb },
  { 0x1ed, 0x1ed, 0x1cd },
  { 0x1ee, 0x1ee, 0x1ce },
  { 0x1f3, 0x1f3, 0x1d3 },
  { 0x1f4, 0x1f4, 0x1d4 },
  { 0x1f6, 0x1f6, 0x1d6 },
  { 0x1f7, 0x1f7, 0x1d7 },
  { 0x1fa, 0x1fa, 0x1da },
  { 0x1fc, 0x1fc, 0x1dc },
  { 0x1fd, 0x1fd, 0x1dd },
  { 0x2a2, 0x2b2, 0x2a2 },
  { 0x2a3, 0x2b3, 0x2a3 },
  { 0x2a4, 0x2b4, 0x2a4 },
  { 0x2a5, 0x2b5, 0x2a5 },
  { 0x2a9, 0x2a9, 0x2a9 },
  { 0x2b2, 0x2b2, 0x2a2 },
  { 0x2b3, 0x2b3, 0x2a3 },
  { 0x2b4, 0x2b4, 0x2a4 },
  { 0x2b5, 0x2b5, 0x2a5 },
  { 0x2b9, 0x2b9, 0x2b9 },
  { 0x2c7, 0x2e7, 0x2c7 },
  { 0x2c8, 0x2e8, 0x2c8 },
  { 0x2c9, 0x2e9, 0x2c9 },
  { 0x2ca, 0x2ea, 0x2ca },
  { 0x2cb, 0x2eb, 0x2cb },
  { 0x2cc, 0x2ec, 0x2cc },
  { 0x2cd, 0x2ed, 0x2cd },
  { 0x2ce, 0x2ee, 0x2ce },
  { 0x2cf, 0x2ef, 0x2cf },
  { 0x2d0, 0x2f0, 0x2d0 },
  { 0x2d1, 0x2f1, 0x2d1 },
  { 0x2d2, 0x2f2, 0x2d2 },
  { 0x2d3, 0x2f3, 0x2d3 },
  { 0x2d4, 0x2f4, 0x2d4 },
  { 0x2d6, 0x2f6, 0x2d6 },
  { 0x2d7, 0x2f7, 0x2d7 },
  { 0x2d9, 0x2f9, 0x2d9 },
  { 0x2da, 0x2fa, 0x2da },
  { 0x2db, 0x2fb, 0x2db },
  { 0x2dc, 0x2fc, 0x2dc },
  { 0x2e7, 0x2e7, 0x2c7 },
  { 0x2e8, 0x2e8, 0x2c8 },
  { 0x2e9, 0x2e9, 0x2c9 },
  { 0x2ea, 0x2ea, 0x2ca },
  { 0x2eb, 0x2eb, 0x2cb },
  { 0x2ec, 0x2ec, 0x2cc },
  { 0x2ed, 0x2ed, 0x2cd },
  { 0x2ee, 0x2ee, 0x2ce },
  { 0x2ef, 0x2ef, 0x2cf },
  { 0x2f0, 0x2f0, 0x2d0 },
  { 0x2f1, 0x2f1, 0x2d1 },
  { 0x2f2, 0x2f2, 0x2d2 },
  { 0x2f3, 0x2f3, 0x2d3 },
  { 0x2f4, 0x2f4, 0x2d4 },
  { 0x2f6, 0x2f6, 0x2d6 },
  { 0x2f7, 0x2f7, 0x2d7 },
  { 0x2f9, 0x2f9, 0x2d9 },
  { 0x2fa, 0x2fa, 0x2da },
  { 0x2fb, 0x2fb, 0x2db },
  { 0x2fc, 0x2fc, 0x2dc },
  { 0x3a4, 0x3b4, 0x3a4 },
  { 0x3a7, 0x3b7, 0x3a7 },
  { 0x3a8, 0x3b8, 0x3a8 },
  { 0x3a9, 0x3b9, 0x3a9 },
  { 0x3b4, 0x3b4, 0x3a4 },
  { 0x3b7, 0x3b7, 0x3a7 },
  { 0x3b8, 0x3b8, 0x3a8 },
  { 0x3b9, 0x3b9, 0x3a9 },
  { 0x3c1, 0x3e1, 0x3c1 },
  { 0x3c2, 0x3e2, 0x3c2 },
  { 0x3c3, 0x3e3, 0x3c3 },
  { 0x3c4, 0x3e4, 0x3c4 },
  { 0x3c5, 0x3e5, 0x3c5 },
  { 0x3c6, 0x3e6, 0x3c6 },
  { 0x3c8, 0x3e8, 0x3c8 },
  { 0x3c9, 0x3e9, 0x3c9 },
  { 0x3ca, 0x3ea, 0x3ca },
  { 0x3cb, 0x3eb, 0x3cb },
  { 0x3cd, 0x3ed, 0x3cd },
  { 0x3ce, 0x3ee, 0x3ce },
  { 0x3d0, 0x3f0, 0x3d0 },
  { 0x3d4, 0x3f4, 0x3d4 },
  { 0x3d5, 0x3f5, 0x3d5 },
  { 0x3d6, 0x3f6, 0x3d6 },
  { 0x3d7, 0x3f7, 0x3d7 },
  { 0x3d8, 0x3f8, 0x3d8 },
  { 0x3da, 0x3fa, 0x3da },
  { 0x3db, 0x3fb, 0x3db },
  { 0x3dc, 0x3fc, 0x3dc },
  { 0x3e1, 0x3e1, 0x3c1 },
  { 0x3e2, 0x3e2, 0x3c2 },
  { 0x3e3, 0x3e3, 0x3c3 },
  { 0x3e4, 0x3e4, 0x3c4 },
  { 0x3e5, 0x3e5, 0x3c5 },
  { 0x3e6, 0x3e6, 0x3c6 },
  { 0x3e8, 0x3e8, 0x3c8 },
  { 0x3e9, 0x3e9, 0x3c9 },
  { 0x3ea, 0x3ea, 0x3ca },
  { 0x3eb, 0x3eb, 0x3cb },
  { 0x3ed, 0x3ed, 0x3cd },
  { 0x3ee, 0x3ee, 0x3ce },
  { 0x3f0, 0x3f0, 0x3d0 },
  { 0x3f4, 0x3f4, 0x3d4 },
  { 0x3f5, 0x3f5, 0x3d5 },
  { 0x3f6, 0x3f6, 0x3d6 },
  { 0x3f7, 0x3f7, 0x3d7 },
  { 0x3f8, 0x3f8, 0x3d8 },
  { 0x3fa, 0x3fa, 0x3da },
  { 0x3fb, 0x3fb, 0x3db },
  { 0x3fc, 0x3fc, 0x3dc },
  { 0x7a6, 0x7b6, 0x7a6 },
  { 0x7aa, 0x7ba, 0x7aa },
  { 0x7d3, 0x7f3, 0x7d3 },
  { 0x8f6, 0x8f6, 0x8f6 },
};
void
_gdk_wayland_keyval_convert_case (guint  symbol,
				  guint *lower,
				  guint *upper)
{
  guint xlower = 0;
  guint xupper = 0;

  /* Directly encoded 24-bit UCS characters. */
  if ((symbol & 0xff000000) == 0x01000000)
    {
      if (lower)
	*lower = gdk_unicode_to_keyval (g_unichar_tolower (symbol & 0x00ffffff));
      if (upper)
	*upper = gdk_unicode_to_keyval (g_unichar_toupper (symbol & 0x00ffffff));
      return;
    }

  if (symbol)
    {
      guint lo = 0, hi = G_N_ELEMENTS (case_exceptions);

      xlower = xkb_keysym_to_lower (symbol);
      xupper = xkb_keysym_to_upper (symbol);

      while (lo < hi)
	{
	  guint mid = (lo + hi) / 2;

	  if (case_exceptions[mid].symbol == symbol)
	    {
	      xlower = case_exceptions[mid].lower;
	      xupper = case_exceptions[mid].upper;
	      break;
	    }
	  if (case_exceptions[mid].symbol < symbol)
	    lo = mid + 1;
	  else
	    hi = mid;
	}
    }

  if (lower)
    *lower = xlower;
  if (upper)
    *upper = xupper;
}
