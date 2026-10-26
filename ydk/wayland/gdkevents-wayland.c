/*
 * gdkevents-wayland.c
 *
 * Event loop integration and input translation for the Wayland backend.
 *
 * Mirrors the X11 backend: a GSource watches the Wayland display fd, and
 * dispatch pulls protocol events (which generate GdkEvents through the
 * keyboard / pointer listeners) into the core event queue.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "config.h"

#include <string.h>
#include <linux/input-event-codes.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include <ydk/gdk.h>
#include <ydk/gdkcairo.h>
#include <ydk/gdkkeysyms.h>

#include "xdg-shell-client.h"

#include "gdkprivate-wayland.h"

#ifdef GDK_COMPILATION
#include "gdkalias.h"
#endif

/* ---- display event source (mirrors X11 GdkDisplaySource) ------------- */

typedef struct _WaylandEventSource WaylandEventSource;

struct _WaylandEventSource
{
  GSource source;

  GdkDisplay *display;
  GPollFD poll_fd;
};

static gboolean wayland_event_prepare  (GSource *source, gint *timeout);
static gboolean wayland_event_check    (GSource *source);
static gboolean wayland_event_dispatch (GSource *source,
					GSourceFunc callback,
					gpointer user_data);

static GSourceFuncs wayland_event_funcs = {
  wayland_event_prepare,
  wayland_event_check,
  wayland_event_dispatch,
  NULL
};

/* ---- drain helper ---------------------------------------------------- */

static void
drain_gdk_events (GdkDisplay *display)
{
  GdkEvent *event;

  while ((event = _gdk_event_unqueue (display)) != NULL)
    {
      if (_gdk_event_func)
	(*_gdk_event_func) (event, _gdk_event_data);
      gdk_event_free (event);
    }
}

/* _gdk_events_queue: called from gdk_display_get_event() (gdkdisplay.c)
 * to fetch pending server-side events into the GDK queue.  In the Wayland
 * backend this reads and dispatches Wayland protocol events, which may
 * generate GdkEvents through the keyboard / pointer listeners. */
void
_gdk_events_queue (GdkDisplay *display)
{
  GDKWDisplayWayland *wd = GDKW_DISPLAY_WAYLAND (display);

  wl_display_dispatch (wd->wl_display);
}

static gboolean
wayland_event_prepare (GSource *source, gint *timeout)
{
  WaylandEventSource *ws = (WaylandEventSource *) source;
  GDKWDisplayWayland *display = GDKW_DISPLAY_WAYLAND (ws->display);

  *timeout = -1;
  wl_display_flush (display->wl_display);

  return FALSE;
}

static gboolean
wayland_event_check (GSource *source)
{
  WaylandEventSource *ws = (WaylandEventSource *) source;

  return (ws->poll_fd.revents & G_IO_IN) != 0;
}

static gboolean
wayland_event_dispatch (GSource *source,
			GSourceFunc callback,
			gpointer user_data)
{
  GdkDisplay *display = ((WaylandEventSource *) source)->display;

  _gdk_events_queue (display);
  drain_gdk_events (display);

  return TRUE;
}

/* ---- translation helpers --------------------------------------------- */

static GDKWWindowImplWayland *
impl_from_surface (GDKWDisplayWayland *display,
		   struct wl_surface *surface)
{
  return _gdk_wayland_impl_from_surface (display, surface);
}

static inline gboolean
impl_wants (GDKWWindowImplWayland *impl, GdkEventMask mask)
{
  return impl != NULL && (impl->events & mask) != 0;
}

static inline gboolean
event_is_useful (GdkEvent *event)
{
  GDKWWindowImplWayland *impl;

  if (!event->any.window)
    return TRUE;

  {
    GdkWindowObject *wobj = GDK_WINDOW_OBJECT (event->any.window);
    if (!wobj || !wobj->impl)
      return TRUE;
    impl = GDKW_WINDOW_IMPL_WAYLAND (wobj->impl);
  }

  switch (event->type)
    {
    case GDK_EXPOSE:
      return TRUE;

    case GDK_KEY_PRESS:
    case GDK_KEY_RELEASE:
      return impl_wants (impl, GDK_KEY_PRESS_MASK);

    case GDK_BUTTON_PRESS:
      return impl_wants (impl, GDK_BUTTON_PRESS_MASK);

    case GDK_BUTTON_RELEASE:
      return impl_wants (impl, GDK_BUTTON_RELEASE_MASK);

    case GDK_MOTION_NOTIFY:
      return impl_wants (impl, GDK_POINTER_MOTION_MASK);

    case GDK_ENTER_NOTIFY:
      return impl_wants (impl, GDK_ENTER_NOTIFY_MASK);

    case GDK_LEAVE_NOTIFY:
      return impl_wants (impl, GDK_LEAVE_NOTIFY_MASK);

    default:
      return TRUE;
    }
}

static void
queue_event (GdkDisplay *display, GdkEvent *event)
{
  GList *node;

  ((GdkEventPrivate *) event)->flags |= GDK_EVENT_PENDING;
  node = _gdk_event_queue_append (display, event);

  if (!event_is_useful (event))
    {
      _gdk_event_queue_remove_link (display, node);
      g_list_free_1 (node);
      gdk_event_free (event);
      return;
    }

  ((GdkEventPrivate *) event)->flags &= ~GDK_EVENT_PENDING;
}

/* simple key string: utf-8 conversion with control-character collapsing. */
static void
fill_key_string (GdkEventKey *key)
{
  gunichar c = 0;

  key->string = NULL;

  if (key->keyval != GDK_KEY_VoidSymbol)
    c = gdk_keyval_to_unicode (key->keyval);

  if (c)
    {
      gchar buf[7];
      gint len;
      gsize bytes_written;

      if (key->state & GDK_CONTROL_MASK)
	{
	  if ((c >= '@' && c < '\177') || c == ' ') c &= 0x1F;
	  else if (c == '2')
	    {
	      key->string = g_memdup ("\0\0", 2);
	      key->length = 1;
	      buf[0] = '\0';
	      goto out;
	    }
	  else if (c >= '3' && c <= '7') c -= ('3' - '\033');
	  else if (c == '8') c = '\177';
	  else if (c == '/') c = '_' & 0x1F;
	}

      len = g_unichar_to_utf8 (c, buf);
      buf[len] = '\0';

      key->string = g_locale_from_utf8 (buf, len,
					NULL, &bytes_written,
					NULL);
      if (key->string)
	key->length = bytes_written;
    }
  else if (key->keyval == GDK_Escape)
    {
      key->length = 1;
      key->string = g_strdup ("\033");
    }
  else if (key->keyval == GDK_Return ||
	   key->keyval == GDK_KP_Enter)
    {
      key->length = 1;
      key->string = g_strdup ("\r");
    }

out:
  if (!key->string)
    {
      key->length = 0;
      key->string = g_strdup ("");
    }
}

/* ---- keyboard -------------------------------------------------------- */

static GDKWWindowImplWayland *keyboard_focus = NULL;

static void
wl_keyboard_keymap (void *data,
		    struct wl_keyboard *keyboard,
		    uint32_t format,
		    int32_t fd,
		    uint32_t size)
{
  GDKWDisplayWayland *display = data;

  if (!display->keymap)
    gdkw_display_wayland_get_keymap (GDK_DISPLAY_OBJECT (display));

  _gdk_wayland_keymap_set_keymap (display, format, fd, size);
}

static void
wl_keyboard_enter (void *data,
		   struct wl_keyboard *keyboard,
		   uint32_t serial,
		   struct wl_surface *surface,
		   struct wl_array *keys)
{
  GDKWDisplayWayland *display = data;
  GDKWWindowImplWayland *impl = impl_from_surface (display, surface);

  keyboard_focus = impl;
}

static void
wl_keyboard_leave (void *data,
		   struct wl_keyboard *keyboard,
		   uint32_t serial,
		   struct wl_surface *surface)
{
  keyboard_focus = NULL;
}

static void
wl_keyboard_key (void *data,
		 struct wl_keyboard *keyboard,
		 uint32_t serial,
		 uint32_t time,
		 uint32_t key,
		 uint32_t state)
{
  GDKWDisplayWayland *display = data;
  GdkEvent event;
  GdkKeymap *keymap;

  if (!keyboard_focus || !display->keymap)
    return;

  memset (&event, 0, sizeof event);

  event.key.type = state ? GDK_KEY_PRESS : GDK_KEY_RELEASE;
  event.key.window = keyboard_focus->wrapper;
  event.key.time = time;
  event.key.hardware_keycode = key;

  keymap = display->keymap;

  event.key.group = _gdk_wayland_keymap_group (keymap);
  event.key.keyval = GDK_KEY_VoidSymbol;

  gdk_keymap_translate_keyboard_state (keymap,
				       event.key.hardware_keycode,
				       (GdkModifierType) 0,
				       event.key.group,
				       &event.key.keyval,
				       NULL, NULL,
				       (GdkModifierType *) &event.key.state);

  event.key.is_modifier =
    GDK_KEYMAP_GET_CLASS (keymap)->key_is_modifier (keymap,
						    event.key.hardware_keycode);

  fill_key_string (&event.key);

  queue_event (GDK_DISPLAY_OBJECT (display), &event);
}

static void
wl_keyboard_modifiers (void *data,
		       struct wl_keyboard *keyboard,
		       uint32_t serial,
		       uint32_t mods_depressed,
		       uint32_t mods_latched,
		       uint32_t mods_locked,
		       uint32_t group)
{
  GDKWDisplayWayland *display = data;
  struct xkb_state *state;

  if (!display->keymap)
    return;

  state = _gdk_wayland_keymap_get_xkb_state (display->keymap);

  if (state)
    xkb_state_update_mask (state,
			   mods_depressed, mods_latched, mods_locked,
			   0, 0, group);
}

static void
wl_keyboard_repeat_info (void *data,
			 struct wl_keyboard *keyboard,
			 int32_t rate,
			 int32_t delay)
{
}

static const struct wl_keyboard_listener wl_keyboard_listener = {
  wl_keyboard_keymap,
  wl_keyboard_enter,
  wl_keyboard_leave,
  wl_keyboard_key,
  wl_keyboard_modifiers,
  wl_keyboard_repeat_info
};

/* ---- pointer --------------------------------------------------------- */

static GDKWWindowImplWayland *pointer_focus = NULL;

static guint
translate_button (guint button)
{
  if (button >= BTN_MOUSE)
    return button - BTN_MOUSE + 1;

  return button;
}

static void
wl_pointer_enter (void *data,
		  struct wl_pointer *pointer,
		  uint32_t serial,
		  struct wl_surface *surface,
		  wl_fixed_t surface_x,
		  wl_fixed_t surface_y)
{
  GDKWDisplayWayland *display = data;
  GDKWWindowImplWayland *impl;
  GdkEvent event;

  impl = impl_from_surface (display, surface);

  pointer_focus = impl;
  display->pointer_focus = impl;
  display->px = wl_fixed_to_double (surface_x);
  display->py = wl_fixed_to_double (surface_y);

  if (!impl || !impl->wrapper)
    return;

  memset (&event, 0, sizeof event);
  event.type = GDK_ENTER_NOTIFY;
  event.crossing.window = impl->wrapper;
  event.crossing.time = GDK_CURRENT_TIME;
  event.crossing.x = display->px;
  event.crossing.y = display->py;
  event.crossing.mode = GDK_CROSSING_NORMAL;
  event.crossing.detail = GDK_NOTIFY_INFERIOR;

  queue_event (GDK_DISPLAY_OBJECT (display), &event);
}

static void
wl_pointer_leave (void *data,
		  struct wl_pointer *pointer,
		  uint32_t serial,
		  struct wl_surface *surface)
{
  GDKWDisplayWayland *display = data;
  GDKWWindowImplWayland *impl;
  GdkEvent event;

  impl = pointer_focus;

  pointer_focus = NULL;
  display->pointer_focus = NULL;

  if (!impl || !impl->wrapper)
    return;

  memset (&event, 0, sizeof event);
  event.type = GDK_LEAVE_NOTIFY;
  event.crossing.window = impl->wrapper;
  event.crossing.time = GDK_CURRENT_TIME;
  event.crossing.mode = GDK_CROSSING_NORMAL;
  event.crossing.detail = GDK_NOTIFY_INFERIOR;

  queue_event (GDK_DISPLAY_OBJECT (display), &event);
}

static void
wl_pointer_motion (void *data,
		   struct wl_pointer *pointer,
		   uint32_t time,
		   wl_fixed_t surface_x,
		   wl_fixed_t surface_y)
{
  GDKWDisplayWayland *display = data;
  GdkEvent event;

  if (!pointer_focus)
    return;

  display->px = wl_fixed_to_double (surface_x);
  display->py = wl_fixed_to_double (surface_y);

  memset (&event, 0, sizeof event);
  event.type = GDK_MOTION_NOTIFY;
  event.motion.window = pointer_focus->wrapper;
  event.motion.time = time;
  event.motion.x = display->px;
  event.motion.y = display->py;
  event.motion.is_hint = 0;
  event.motion.device = NULL;

  queue_event (GDK_DISPLAY_OBJECT (display), &event);
}

static void
wl_pointer_button (void *data,
		   struct wl_pointer *pointer,
		   uint32_t serial,
		   uint32_t time,
		   uint32_t button,
		   uint32_t state)
{
  GDKWDisplayWayland *display = data;
  GdkEvent event;

  if (!pointer_focus)
    return;

  memset (&event, 0, sizeof event);
  event.type = state ? GDK_BUTTON_PRESS : GDK_BUTTON_RELEASE;
  event.button.window = pointer_focus->wrapper;
  event.button.time = time;
  event.button.button = translate_button (button);
  event.button.x = display->px;
  event.button.y = display->py;
  event.button.device = NULL;

  queue_event (GDK_DISPLAY_OBJECT (display), &event);
}

static void
wl_pointer_axis (void *data,
		 struct wl_pointer *pointer,
		 uint32_t time,
		 uint32_t axis,
		 wl_fixed_t value)
{
  GDKWDisplayWayland *display = data;
  GdkEvent event;
  double v;

  if (!pointer_focus)
    return;

  v = wl_fixed_to_double (value);

  if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL || v == 0.0)
    return;

  memset (&event, 0, sizeof event);
  event.type = GDK_SCROLL;
  event.scroll.window = pointer_focus->wrapper;
  event.scroll.time = time;
  event.scroll.x = display->px;
  event.scroll.y = display->py;
  event.scroll.direction = v > 0 ? GDK_SCROLL_DOWN : GDK_SCROLL_UP;
  event.scroll.device = NULL;

  queue_event (GDK_DISPLAY_OBJECT (display), &event);
}

static const struct wl_pointer_listener wl_pointer_listener = {
  wl_pointer_enter,
  wl_pointer_leave,
  wl_pointer_motion,
  wl_pointer_button,
  wl_pointer_axis
};

/* ---- seat capabilities ------------------------------------------------ */

static void
wl_seat_capabilities (void *data,
		      struct wl_seat *seat,
		      uint32_t capabilities)
{
  GDKWDisplayWayland *display = data;

  /* keyboard */
  if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !display->wl_keyboard)
    {
      display->wl_keyboard = wl_seat_get_keyboard (seat);
      wl_keyboard_add_listener (display->wl_keyboard,
				&wl_keyboard_listener,
				display);
    }
  else if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD) &&
	   display->wl_keyboard)
    {
      wl_keyboard_destroy (display->wl_keyboard);
      display->wl_keyboard = NULL;
      keyboard_focus = NULL;
    }

  /* pointer */
  if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !display->wl_pointer)
    {
      display->wl_pointer = wl_seat_get_pointer (seat);
      wl_pointer_add_listener (display->wl_pointer,
			       &wl_pointer_listener,
			       display);
    }
  else if (!(capabilities & WL_SEAT_CAPABILITY_POINTER) &&
	   display->wl_pointer)
    {
      wl_pointer_destroy (display->wl_pointer);
      display->wl_pointer = NULL;
      pointer_focus = NULL;
      display->pointer_focus = NULL;
    }
}

static const struct wl_seat_listener wl_seat_listener = {
  wl_seat_capabilities
};

/* ---- public interface ------------------------------------------------- */

void
_gdk_wayland_seat_init (GDKWDisplayWayland *display,
			struct wl_registry *registry,
			uint32_t name)
{
  display->wl_seat =
    wl_registry_bind (registry, name, &wl_seat_interface, 2);

  if (!display->wl_seat)
    return;

  wl_seat_add_listener (display->wl_seat, &wl_seat_listener, display);
}

void
_gdk_wayland_events_init (GdkDisplay *display)
{
  GDKWDisplayWayland *wayland_display = GDKW_DISPLAY_WAYLAND (display);
  GSource *source;
  WaylandEventSource *ws;

  source = g_source_new (&wayland_event_funcs, sizeof (WaylandEventSource));
  g_source_set_name (source, "GDK Wayland event source");
  g_source_set_priority (source, GDK_PRIORITY_EVENTS);

  ws = (WaylandEventSource *) source;
  ws->display = display;
  ws->poll_fd.fd = wl_display_get_fd (wayland_display->wl_display);
  ws->poll_fd.events = G_IO_IN;
  ws->poll_fd.revents = 0;

  g_source_add_poll (source, &ws->poll_fd);
  g_source_attach (source, g_main_context_default ());
  g_source_unref (source);
}

void
_gdk_wayland_events_finalize (GdkDisplay *display)
{
  GDKWDisplayWayland *wayland_display = GDKW_DISPLAY_WAYLAND (display);

  keyboard_focus = NULL;
  pointer_focus = NULL;

  if (wayland_display->wl_keyboard)
    {
      wl_keyboard_destroy (wayland_display->wl_keyboard);
      wayland_display->wl_keyboard = NULL;
    }
  if (wayland_display->wl_pointer)
    {
      wl_pointer_destroy (wayland_display->wl_pointer);
      wayland_display->wl_pointer = NULL;
    }
  if (wayland_display->wl_seat)
    {
      wl_seat_destroy (wayland_display->wl_seat);
      wayland_display->wl_seat = NULL;
    }
}