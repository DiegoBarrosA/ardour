/*
 * gdkkeys-wayland.c
 *
 * Wayland keymap implementation based on libxkbcommon.
 *
 * The compositor hands over a complete XKB keymap through a file descriptor
 * in the wl_keyboard keymap event; we mmap it, build an xkb_keymap / state,
 * and translate keycodes to GDK keyvals/modifiers for the event path.
 */

#define _GNU_SOURCE

#include "config.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <xkbcommon/xkbcommon.h>

#include <ydk/gdk.h>
#include <ydk/gdkkeys.h>
#include <ydk/gdkkeysyms.h>

#include "gdkprivate-wayland.h"

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

static GdkModifierType
gdk_modmask_for_xkb (struct xkb_keymap *keymap, xkb_mod_mask_t mask)
{
  GdkModifierType result = 0;

#define CASE(name, gdk_mask) \
  if (mask & (1u << xkb_keymap_mod_get_index (keymap, (name)))) \
    result |= (gdk_mask)

  CASE (XKB_MOD_NAME_SHIFT,  GDK_SHIFT_MASK);
  CASE (XKB_MOD_NAME_CAPS,   GDK_LOCK_MASK);
  CASE (XKB_MOD_NAME_CTRL,   GDK_CONTROL_MASK);
  CASE (XKB_MOD_NAME_ALT,    GDK_MOD1_MASK);
  CASE (XKB_MOD_NAME_NUM,    GDK_MOD2_MASK);
  CASE (XKB_MOD_NAME_MOD3,   GDK_MOD3_MASK);
  CASE (XKB_MOD_NAME_LOGO,   GDK_MOD4_MASK);
  CASE (XKB_MOD_NAME_MOD5,   GDK_MOD5_MASK);
#undef CASE

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
  xkb_level_index_t lvl = 0;
  int nsyms;
  const xkb_keysym_t *syms;
  xkb_keysym_t sym = XKB_KEY_NoSymbol;
  xkb_mod_mask_t consumed = 0;

  if (!self->xkb_state)
    {
      if (keyval)
	*keyval = GDK_KEY_VoidSymbol;
      if (consumed_modifiers)
	*consumed_modifiers = 0;
      return FALSE;
    }

  nsyms = xkb_state_key_get_syms (self->xkb_state,
				  (xkb_keycode_t) hardware_keycode,
				  &syms);

  if (nsyms > 0)
    {
      lvl = xkb_state_key_get_level (self->xkb_state,
				     (xkb_keycode_t) hardware_keycode,
				     (xkb_layout_index_t) group);
      sym = syms[(guint) lvl < (guint) nsyms ? lvl : 0];
    }

  if (keyval)
    *keyval = sym;

  if (effective_group)
    *effective_group = group;

  if (level)
    *level = (gint) lvl;

  consumed = xkb_state_key_get_consumed_mods (self->xkb_state,
					      (xkb_keycode_t) hardware_keycode);

  if (consumed_modifiers)
    *consumed_modifiers = gdk_modmask_for_xkb (self->xkb_keymap, consumed);

  return nsyms > 0;
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
      if (syms[i] >= XKB_KEY_Shift_L && syms[i] <= XKB_KEY_Hyper_R)
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

  if (format != 0)
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
    const gchar *buf = mmap (NULL, size, PROT_READ, MAP_SHARED, fd, 0);

    if (buf == MAP_FAILED)
      {
	close (fd);
	return;
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

    self->xkb_keymap =
      xkb_keymap_new_from_string (self->context, buf,
				  XKB_KEYMAP_FORMAT_TEXT_V1, 0);
    munmap ((gpointer) buf, size);
  }

  close (fd);

  if (!self->xkb_keymap)
    return;

  self->xkb_state = xkb_state_new (self->xkb_keymap);

  GDK_KEYMAP_GET_CLASS (self)->keys_changed (GDK_KEYMAP (self));
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
