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

#include <ydk/gdk.h>
#include <ydk/gdkkeys.h>
#include <ydk/gdkkeysyms.h>

#include "gdkprivate-wayland.h"
#include "gdkseat-wayland.h"

#ifdef GDK_COMPILATION
#include "gdkalias.h"
#endif

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
