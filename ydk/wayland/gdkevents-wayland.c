/*
 * gdkevents-wayland.c
 *
 * Event loop integration and input translation for the Wayland backend.
 *
 * Mirrors the X11 backend: a GSource watches the Wayland display fd, and
 * dispatch pulls protocol events (which generate GdkEvents through the
 * keyboard / pointer listeners) into the core event queue.  Every input
 * event is a heap GdkEvent that goes through _gdk_windowing_got_event (),
 * which is the core event engine (child window proxying, implicit grabs,
 * crossing synthesis, double clicks).
 *
 * All seat state lives in GDKWSeat (gdkseat-wayland.h).  The test hooks at
 * the bottom of this file push synthetic protocol events through the real
 * listeners, because a headless compositor has no input devices.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "config.h"

#include <errno.h>
#include <math.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/input-event-codes.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include <ydk/gdk.h>
#include <ydk/gdkcairo.h>
#include <ydk/gdkkeysyms.h>
#include <ydk/gdkinternals.h>
#include <ydk/gdkinput.h>

#include "xdg-shell-client.h"

#include "gdkprivate-wayland.h"
#include "gdkseat-wayland.h"
#include "gdkm7-wayland.h"
#include "gdkm8-wayland.h"
#include "gdkm9-wayland.h"

#ifdef GDK_COMPILATION
#include "gdkalias.h"
#endif

#define EVENT_SOURCE_DATA_KEY "ydk-wayland-event-source"

/* ---- display event source (mirrors X11 GdkDisplaySource) ------------- */

typedef struct _WaylandEventSource WaylandEventSource;

struct _WaylandEventSource
{
  GSource source;

  GdkDisplay *display;
  GPollFD poll_fd;
  gboolean reading;     /* wl_display_prepare_read () is outstanding */
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

/* ---- connection loss -------------------------------------------------- */

static void
lost_connection (void)
{
  g_printerr ("Gdk: lost connection to the Wayland compositor\n");
  exit (1);
}

static void
check_connection (GDKWDisplayWayland *wd)
{
  if (wl_display_get_error (wd->wl_display) != 0)
    lost_connection ();
}

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
 * to fetch pending server-side events into the GDK queue.  Must never
 * block: it reads the socket only when data is already there. */
/* The canonical _gdk_events_queue belongs to this backend (the X11 one is
 * renamed _gdk_x11_events_queue by gdkwindowing-x11.h), so an X11 display
 * that reaches it is forwarded. */
extern void _gdk_x11_events_queue (GdkDisplay *display);

/* The display source leaves a wl_display_prepare_read () intent outstanding
 * while the main loop polls.  A second reader in the same thread (the
 * non-blocking queue / pending helpers below, run from another source's
 * callbacks) would make wl_display_read_events () wait for the first one
 * forever, so drop the source's intent first; its check () then sees
 * reading == FALSE and just reports the fd state. */
static void
source_cancel_read (GdkDisplay *display)
{
  GSource *source = g_object_get_data (G_OBJECT (display), EVENT_SOURCE_DATA_KEY);
  WaylandEventSource *ws = (WaylandEventSource *) source;

  if (ws != NULL && ws->reading)
    {
      wl_display_cancel_read (GDKW_DISPLAY_WAYLAND (display)->wl_display);
      ws->reading = FALSE;
    }
}

void
_gdk_events_queue (GdkDisplay *display)
{
  GDKWDisplayWayland *wd;
  struct wl_display *d;
  struct pollfd pfd;

  if (!GDKW_IS_DISPLAY_WAYLAND (display))
    {
      _gdk_x11_events_queue (display);
      return;
    }

  wd = GDKW_DISPLAY_WAYLAND (display);
  d = wd->wl_display;

  source_cancel_read (display);

  while (wl_display_prepare_read (d) != 0)
    {
      if (wl_display_dispatch_pending (d) < 0)
	{
	  check_connection (wd);
	  return;
	}
    }

  if (wl_display_flush (d) < 0 && errno != EAGAIN)
    {
      wl_display_cancel_read (d);
      lost_connection ();
    }

  pfd.fd = wl_display_get_fd (d);
  pfd.events = POLLIN;
  pfd.revents = 0;

  if (poll (&pfd, 1, 0) > 0 && (pfd.revents & POLLIN))
    {
      if (wl_display_read_events (d) < 0)
	lost_connection ();
    }
  else
    {
      wl_display_cancel_read (d);
      if (pfd.revents & (POLLERR | POLLHUP))
	lost_connection ();
    }

  if (wl_display_dispatch_pending (d) < 0)
    check_connection (wd);
}

/* Backs the gdk_events_pending () dispatcher. */
gboolean
_gdk_wayland_events_pending (GdkDisplay *display)
{
  GDKWDisplayWayland *wd;
  struct wl_display *d;
  struct pollfd pfd;
  gboolean readable;

  if (display == NULL || !GDKW_IS_DISPLAY_WAYLAND (display))
    return FALSE;

  if (_gdk_event_queue_find_first (display) != NULL)
    return TRUE;

  wd = GDKW_DISPLAY_WAYLAND (display);
  d = wd->wl_display;

  source_cancel_read (display);

  /* libwayland already holds undispatched protocol events */
  if (wl_display_prepare_read (d) != 0)
    return TRUE;

  pfd.fd = wl_display_get_fd (d);
  pfd.events = POLLIN;
  pfd.revents = 0;
  readable = poll (&pfd, 1, 0) > 0 && (pfd.revents & POLLIN);

  wl_display_cancel_read (d);

  return readable;
}

static gboolean
wayland_event_prepare (GSource *source, gint *timeout)
{
  WaylandEventSource *ws = (WaylandEventSource *) source;
  GDKWDisplayWayland *display = GDKW_DISPLAY_WAYLAND (ws->display);

  *timeout = -1;

  if (_gdk_event_queue_find_first (ws->display) != NULL)
    {
      *timeout = 0;
      return TRUE;
    }

  if (wl_display_prepare_read (display->wl_display) != 0)
    {
      /* libwayland holds undispatched events */
      *timeout = 0;
      return TRUE;
    }

  ws->reading = TRUE;

  if (wl_display_flush (display->wl_display) < 0)
    {
      if (errno != EAGAIN)
	{
	  wl_display_cancel_read (display->wl_display);
	  ws->reading = FALSE;
	  lost_connection ();
	}

      /* socket buffer full: also wake up when it drains */
      ws->poll_fd.events |= G_IO_OUT;
    }
  else
    ws->poll_fd.events &= ~G_IO_OUT;

  return FALSE;
}

static gboolean
wayland_event_check (GSource *source)
{
  WaylandEventSource *ws = (WaylandEventSource *) source;
  GDKWDisplayWayland *display = GDKW_DISPLAY_WAYLAND (ws->display);

  if (ws->reading)
    {
      if (ws->poll_fd.revents & G_IO_IN)
	{
	  if (wl_display_read_events (display->wl_display) < 0)
	    {
	      ws->reading = FALSE;
	      lost_connection ();
	    }
	}
      else
	wl_display_cancel_read (display->wl_display);

      ws->reading = FALSE;
    }

  if ((ws->poll_fd.revents & G_IO_OUT) &&
      wl_display_flush (display->wl_display) < 0 && errno != EAGAIN)
    lost_connection ();

  if (ws->poll_fd.revents & (G_IO_ERR | G_IO_HUP))
    return TRUE;

  return (ws->poll_fd.revents & G_IO_IN) != 0 ||
	 _gdk_event_queue_find_first (ws->display) != NULL;
}

static gboolean
wayland_event_dispatch (GSource *source,
			GSourceFunc callback,
			gpointer user_data)
{
  WaylandEventSource *ws = (WaylandEventSource *) source;
  GdkDisplay *display = ws->display;
  GDKWDisplayWayland *wd = GDKW_DISPLAY_WAYLAND (display);

  if (wl_display_dispatch_pending (wd->wl_display) < 0)
    check_connection (wd);

  if ((ws->poll_fd.revents & (G_IO_ERR | G_IO_HUP)) &&
      !(ws->poll_fd.revents & G_IO_IN))
    lost_connection ();

  check_connection (wd);

  drain_gdk_events (display);

  return TRUE;
}

/* ---- queueing ---------------------------------------------------------- */

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

  impl = _gdk_wayland_window_get_impl (event->any.window);
  if (impl == NULL)
    return TRUE;

  switch (event->type)
    {
    case GDK_KEY_PRESS:
      return impl_wants (impl, GDK_KEY_PRESS_MASK);

    case GDK_KEY_RELEASE:
      return impl_wants (impl, GDK_KEY_RELEASE_MASK);

    case GDK_FOCUS_CHANGE:
      return impl_wants (impl, GDK_FOCUS_CHANGE_MASK);

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

    case GDK_SCROLL:
      /* the core filters by the masks of the child windows */
      return TRUE;

    default:
      return TRUE;
    }
}

/* Takes ownership of @event (a heap event).  After this call the event may
 * already be freed. */
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

  _gdk_windowing_got_event (display, node, event,
			    _gdk_wayland_event_serial (display));
}

/* A heap event of @type for @impl's toplevel, or NULL if the window is gone. */
static GdkEvent *
new_event (GDKWDisplayWayland *wd, GdkEventType type,
	   GDKWWindowImplWayland *impl)
{
  GdkEvent *event;

  if (impl == NULL || impl->wrapper == NULL ||
      GDK_WINDOW_DESTROYED (impl->wrapper))
    return NULL;

  event = gdk_event_new (type);
  event->any.window = g_object_ref (impl->wrapper);
  event->any.send_event = FALSE;
  if (impl->screen)
    gdk_event_set_screen (event, impl->screen);

  return event;
}

static inline guint32
seat_time (const GDKWSeat *seat)
{
  return seat->last_time != 0 ? seat->last_time : GDK_CURRENT_TIME;
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

static gboolean
is_parent_of (GdkWindow *parent, GdkWindow *child)
{
  GdkWindow *w = child;

  while (w != NULL)
    {
      if (w == parent)
	return TRUE;
      w = gdk_window_get_parent (w);
    }

  return FALSE;
}

static gboolean
keymap_ready (GDKWDisplayWayland *wd)
{
  return wd->keymap != NULL &&
	 _gdk_wayland_keymap_get_xkb_state (wd->keymap) != NULL;
}

static gboolean
key_is_modifier (GdkKeymap *keymap, guint hardware_keycode)
{
  return GDK_KEYMAP_GET_CLASS (keymap)->key_is_modifier (keymap,
							 hardware_keycode);
}

/* Builds and queues one key event for the focused impl.  Used for real
 * presses / releases and for repeats.  Returns FALSE when nothing was
 * queued. */
static gboolean
emit_key (GDKWDisplayWayland *wd, guint32 time, guint32 evdev_key,
	  gboolean pressed)
{
  GdkDisplay *display = GDK_DISPLAY_OBJECT (wd);
  GDKWSeat *seat = _gdk_wayland_display_get_seat (display);
  GdkKeymap *keymap = wd->keymap;
  GdkEvent *event;
  guint keyval = GDK_KEY_VoidSymbol;
  guint hw = evdev_key + 8;
  gint group;

  if (!seat->keyboard_focus || !keymap_ready (wd))
    return FALSE;

  event = new_event (wd, pressed ? GDK_KEY_PRESS : GDK_KEY_RELEASE,
		     seat->keyboard_focus);
  if (event == NULL)
    return FALSE;

  group = _gdk_wayland_keymap_group (keymap);

  gdk_keymap_translate_keyboard_state (keymap, hw, seat->modifier_mask,
				       group, &keyval, NULL, NULL, NULL);

  event->key.time = time;
  event->key.state = _gdk_wayland_seat_get_state (seat);
  event->key.keyval = keyval;
  event->key.hardware_keycode = hw;
  event->key.group = group;
  event->key.is_modifier = key_is_modifier (keymap, hw) ? 1 : 0;
  fill_key_string (&event->key);

  /* Apply keyboard grabs (same rule as gdkevents-x11.c) */
  if (display->keyboard_grab.window != NULL &&
      !GDK_WINDOW_DESTROYED (display->keyboard_grab.window) &&
      (!display->keyboard_grab.owner_events ||
       !is_parent_of (display->keyboard_grab.window, event->any.window)))
    {
      GdkWindow *old = event->any.window;

      event->any.window = g_object_ref (display->keyboard_grab.window);
      g_object_unref (old);
    }

  queue_event (display, event);

  return TRUE;
}

static gboolean repeat_cb (gpointer data);

static void
repeat_arm (GDKWDisplayWayland *wd, guint interval_ms)
{
  GDKWSeat *seat = _gdk_wayland_display_get_seat (GDK_DISPLAY_OBJECT (wd));

  seat->repeat.source_id = g_timeout_add_full (GDK_PRIORITY_EVENTS,
					       interval_ms, repeat_cb,
					       wd, NULL);
}

static gboolean
repeat_cb (gpointer data)
{
  GDKWDisplayWayland *wd = data;
  GDKWSeat *seat = _gdk_wayland_display_get_seat (GDK_DISPLAY_OBJECT (wd));
  guint32 time;

  /* this source is finishing; avoids a double g_source_remove */
  seat->repeat.source_id = 0;

  if (!seat->keyboard_focus || seat->repeat.key == 0 ||
      seat->repeat.generation != seat->keymap_generation ||
      seat->repeat.rate <= 0)
    {
      _gdk_wayland_seat_repeat_cancel (seat);
      return G_SOURCE_REMOVE;
    }

  time = seat->repeat.press_time +
	 (guint32) ((g_get_monotonic_time () - seat->repeat.start_us) / 1000);

  emit_key (wd, time, seat->repeat.key, TRUE);

  seat->repeat.count++;
  repeat_arm (wd, MAX (1, 1000 / seat->repeat.rate));

  return G_SOURCE_REMOVE;
}

static void
repeat_start (GDKWDisplayWayland *wd, guint32 time, guint32 evdev_key)
{
  GDKWSeat *seat = _gdk_wayland_display_get_seat (GDK_DISPLAY_OBJECT (wd));

  _gdk_wayland_seat_repeat_cancel (seat);

  seat->repeat.key = evdev_key;
  seat->repeat.press_time = time;
  seat->repeat.start_us = g_get_monotonic_time ();
  seat->repeat.generation = seat->keymap_generation;
  seat->repeat.count = 0;

  repeat_arm (wd, seat->repeat.delay > 0 ? seat->repeat.delay : 1);
}

static void
wl_keyboard_keymap (void *data,
		    struct wl_keyboard *keyboard,
		    uint32_t format,
		    int32_t fd,
		    uint32_t size)
{
  GDKWDisplayWayland *wd = data;
  GDKWSeat *seat = _gdk_wayland_display_get_seat (GDK_DISPLAY_OBJECT (wd));
  struct xkb_state *state;

  if (!wd->keymap)
    gdkw_display_wayland_get_keymap (GDK_DISPLAY_OBJECT (wd));

  seat->keymap_generation++;
  _gdk_wayland_seat_repeat_cancel (seat);

  /* closes fd on every path */
  _gdk_wayland_keymap_set_keymap (wd, format, fd, size);

  /* carry the modifier state over to the new keymap */
  state = _gdk_wayland_keymap_get_xkb_state (wd->keymap);
  if (state)
    {
      xkb_state_update_mask (state,
			     seat->mods_depressed, seat->mods_latched,
			     seat->mods_locked, 0, 0, seat->mods_group);
      seat->modifier_mask =
	_gdk_wayland_keymap_serialize_mods (wd->keymap,
					    xkb_state_serialize_mods (state, XKB_STATE_MODS_EFFECTIVE));
    }
  else
    seat->modifier_mask = 0;
}

/* M9 D12: GDK keyboard focus of a popup family is its BASE toplevel.  A
 * grabbing popup takes wl_keyboard focus, which would send FOCUS_CHANGE out
 * on the base toplevel and make ytk think the window lost focus while a menu
 * is open.  The out event is deferred one idle; an enter in the same family
 * before it runs cancels it.  Key events still reach the menu through
 * display->keyboard_grab (emit_key). */

#define KBD_PENDING_KEY "ydk-wayland-kbd-pending-out"

typedef struct
{
  GDKWDisplayWayland *wd;
  GDKWWindowImplWayland *impl;  /* family root whose out is pending */
  guint source;
} KbdPending;

static void
kbd_pending_free (gpointer data)
{
  KbdPending *k = data;

  if (k->source != 0)
    g_source_remove (k->source);
  g_free (k);
}

static KbdPending *
kbd_pending_get (GDKWDisplayWayland *wd, gboolean create)
{
  KbdPending *k = g_object_get_data (G_OBJECT (wd), KBD_PENDING_KEY);

  if (k == NULL && create)
    {
      k = g_new0 (KbdPending, 1);
      k->wd = wd;
      g_object_set_data_full (G_OBJECT (wd), KBD_PENDING_KEY, k,
			      kbd_pending_free);
    }

  return k;
}

static void
kbd_pending_cancel (GDKWDisplayWayland *wd)
{
  KbdPending *k = kbd_pending_get (wd, FALSE);

  if (k == NULL)
    return;

  if (k->source != 0)
    g_source_remove (k->source);
  k->source = 0;
  k->impl = NULL;
}

static void
kbd_pending_send (KbdPending *k)
{
  GDKWDisplayWayland *wd = k->wd;
  GDKWWindowImplWayland *impl = k->impl;
  GdkEvent *event;

  k->impl = NULL;

  /* the impl may have been torn down since */
  if (impl == NULL || g_slist_find (wd->windows, impl) == NULL)
    return;

  event = new_event (wd, GDK_FOCUS_CHANGE, impl);
  if (event != NULL)
    {
      event->focus_change.in = FALSE;
      queue_event (GDK_DISPLAY_OBJECT (wd), event);
    }
}

static gboolean
kbd_pending_cb (gpointer data)
{
  KbdPending *k = data;

  k->source = 0;
  kbd_pending_send (k);

  return G_SOURCE_REMOVE;
}

static void
wl_keyboard_enter (void *data,
		   struct wl_keyboard *keyboard,
		   uint32_t serial,
		   struct wl_surface *surface,
		   struct wl_array *keys)
{
  GDKWDisplayWayland *wd = data;
  GDKWSeat *seat = _gdk_wayland_display_get_seat (GDK_DISPLAY_OBJECT (wd));
  GDKWWindowImplWayland *impl = _gdk_wayland_impl_from_surface (wd, surface);
  GDKWWindowImplWayland *root;
  GDKWWindowImplWayland *prev_focus;
  KbdPending *pending;
  GdkEvent *event;

  seat->keyboard_enter_serial = serial;
  seat->last_serial = serial;

  /* D12: the family root holds the GDK focus */
  root = impl != NULL ? _gdk_wayland_popup_family_root (impl) : NULL;
  if (root == NULL)
    root = impl;

  prev_focus = seat->keyboard_focus;
  seat->keyboard_focus = root;

  if (root == NULL)
    return;

  /* an out for this very family is still pending: focus never left */
  pending = kbd_pending_get (wd, FALSE);
  if (pending != NULL && pending->source != 0 && pending->impl == root)
    {
      kbd_pending_cancel (wd);
      return;
    }

  /* an out for another window goes out now, ahead of the new in */
  if (pending != NULL && pending->source != 0)
    {
      GDKWWindowImplWayland *old = pending->impl;

      kbd_pending_cancel (wd);
      if (old != NULL && g_slist_find (wd->windows, old) != NULL)
	{
	  event = new_event (wd, GDK_FOCUS_CHANGE, old);
	  if (event != NULL)
	    {
	      event->focus_change.in = FALSE;
	      queue_event (GDK_DISPLAY_OBJECT (wd), event);
	    }
	}
    }

  /* entering a popup of a family that already holds the focus */
  if (impl != root && prev_focus == root)
    return;

  event = new_event (wd, GDK_FOCUS_CHANGE, root);
  if (event == NULL)
    return;

  event->focus_change.in = TRUE;
  queue_event (GDK_DISPLAY_OBJECT (wd), event);
}

static void
wl_keyboard_leave (void *data,
		   struct wl_keyboard *keyboard,
		   uint32_t serial,
		   struct wl_surface *surface)
{
  GDKWDisplayWayland *wd = data;
  GDKWSeat *seat = _gdk_wayland_display_get_seat (GDK_DISPLAY_OBJECT (wd));
  GDKWWindowImplWayland *impl = _gdk_wayland_impl_from_surface (wd, surface);
  GDKWWindowImplWayland *root = NULL;
  gboolean defer = FALSE;
  struct xkb_state *state;
  GdkEvent *event;

  if (impl == NULL)
    impl = seat->keyboard_focus;

  /* D12: leaving a surface of a family with live popups is deferred */
  if (impl != NULL && _gdk_wayland_popup_family_active (impl))
    {
      root = _gdk_wayland_popup_family_root (impl);
      defer = (root != NULL);
    }

  seat->last_serial = serial;
  _gdk_wayland_seat_repeat_cancel (seat);
  seat->keyboard_focus = NULL;
  seat->modifier_mask = 0;
  seat->mods_depressed = seat->mods_latched = seat->mods_locked = 0;
  seat->mods_group = 0;

  state = wd->keymap ? _gdk_wayland_keymap_get_xkb_state (wd->keymap) : NULL;
  if (state)
    xkb_state_update_mask (state, 0, 0, 0, 0, 0, 0);

  if (defer)
    {
      KbdPending *pending = kbd_pending_get (wd, TRUE);

      /* an older pending out of another window goes out first */
      if (pending->source != 0 && pending->impl != root)
	{
	  g_source_remove (pending->source);
	  pending->source = 0;
	  kbd_pending_send (pending);
	}
      if (pending->source == 0)
	{
	  pending->impl = root;
	  pending->source = g_idle_add_full (G_PRIORITY_HIGH, kbd_pending_cb,
					     pending, NULL);
	}
      return;
    }

  if (impl == NULL)
    return;

  event = new_event (wd, GDK_FOCUS_CHANGE, impl);
  if (event == NULL)
    return;

  event->focus_change.in = FALSE;
  queue_event (GDK_DISPLAY_OBJECT (wd), event);
}

static void
wl_keyboard_key (void *data,
		 struct wl_keyboard *keyboard,
		 uint32_t serial,
		 uint32_t time,
		 uint32_t key,
		 uint32_t state)
{
  GDKWDisplayWayland *wd = data;
  GDKWSeat *seat = _gdk_wayland_display_get_seat (GDK_DISPLAY_OBJECT (wd));
  gboolean pressed = (state == WL_KEYBOARD_KEY_STATE_PRESSED);

  if (!seat->keyboard_focus || !keymap_ready (wd))
    return;

  seat->key_serial = serial;
  seat->last_serial = serial;
  seat->last_time = time;

  if (!pressed && seat->repeat.key == key)
    _gdk_wayland_seat_repeat_cancel (seat);

  if (!emit_key (wd, time, key, pressed))
    return;

  if (pressed &&
      seat->repeat.rate > 0 &&
      !key_is_modifier (wd->keymap, key + 8) &&
      _gdk_wayland_keymap_key_repeats (wd->keymap, key + 8))
    repeat_start (wd, time, key);
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
  GDKWDisplayWayland *wd = data;
  GDKWSeat *seat = _gdk_wayland_display_get_seat (GDK_DISPLAY_OBJECT (wd));
  struct xkb_state *state;
  guint32 old_locked = seat->mods_locked;

  seat->last_serial = serial;

  if (!wd->keymap)
    return;

  state = _gdk_wayland_keymap_get_xkb_state (wd->keymap);
  if (!state)
    return;

  xkb_state_update_mask (state,
			 mods_depressed, mods_latched, mods_locked,
			 0, 0, group);

  seat->mods_depressed = mods_depressed;
  seat->mods_latched = mods_latched;
  seat->mods_locked = mods_locked;
  seat->mods_group = group;
  seat->modifier_mask =
    _gdk_wayland_keymap_serialize_mods (wd->keymap,
					xkb_state_serialize_mods (state, XKB_STATE_MODS_EFFECTIVE));

  if (old_locked != mods_locked)
    g_signal_emit_by_name (wd->keymap, "state-changed");
}

static void
wl_keyboard_repeat_info (void *data,
			 struct wl_keyboard *keyboard,
			 int32_t rate,
			 int32_t delay)
{
  GDKWDisplayWayland *wd = data;
  GDKWSeat *seat = _gdk_wayland_display_get_seat (GDK_DISPLAY_OBJECT (wd));

  seat->repeat.rate = rate > 0 ? rate : 0;
  seat->repeat.delay = delay > 0 ? delay : 0;
}

static const struct wl_keyboard_listener wl_keyboard_listener = {
  .keymap = wl_keyboard_keymap,
  .enter = wl_keyboard_enter,
  .leave = wl_keyboard_leave,
  .key = wl_keyboard_key,
  .modifiers = wl_keyboard_modifiers,
  .repeat_info = wl_keyboard_repeat_info
};

/* ---- pointer --------------------------------------------------------- */

/* Linux BTN_* to GDK button number; 0 means "drop the event".  GDK buttons
 * 4 to 7 stay reserved for the legacy scroll buttons. */
static guint
translate_button (guint button)
{
  switch (button)
    {
    case BTN_LEFT:    return 1;
    case BTN_MIDDLE:  return 2;
    case BTN_RIGHT:   return 3;
    case BTN_SIDE:    return 8;
    case BTN_EXTRA:   return 9;
    case BTN_FORWARD: return 10;
    case BTN_BACK:    return 11;
    case BTN_TASK:    return 12;
    default:          return 0;
    }
}

static void
queue_crossing (GDKWDisplayWayland *wd, GdkEventType type,
		GDKWWindowImplWayland *impl)
{
  GDKWSeat *seat = _gdk_wayland_display_get_seat (GDK_DISPLAY_OBJECT (wd));
  GdkEvent *event = new_event (wd, type, impl);

  if (event == NULL)
    return;

  event->crossing.subwindow = NULL;
  event->crossing.time = seat_time (seat);
  event->crossing.x = seat->px;
  event->crossing.y = seat->py;
  _gdk_wayland_event_root_coords (impl, seat->px, seat->py,
				  &event->crossing.x_root,
				  &event->crossing.y_root);
  event->crossing.mode = GDK_CROSSING_NORMAL;
  event->crossing.detail = GDK_NOTIFY_NONLINEAR;
  event->crossing.focus = (seat->keyboard_focus == impl);
  event->crossing.state = _gdk_wayland_seat_get_state (seat);

  queue_event (GDK_DISPLAY_OBJECT (wd), event);
}

/* Ends every open-ended implicit core grab.  Compositors normally deliver the
 * button release before the leave, so this only matters for focus theft or a
 * lost pointer capability while a button is down. */
static void
end_implicit_grabs (GdkDisplay *display)
{
  gulong serial = _gdk_wayland_event_serial (display);
  gboolean changed = FALSE;
  GList *l;

  for (l = display->pointer_grabs; l != NULL; l = l->next)
    {
      GdkPointerGrabInfo *info = l->data;

      if (info->implicit && info->serial_end == G_MAXULONG)
	{
	  info->serial_end = serial;
	  info->implicit_ungrab = TRUE;
	  changed = TRUE;
	}
    }

  if (changed)
    _gdk_display_pointer_grab_update (display, serial);
}

/* The pointer left (or is being forced out of) the focused surface: tell the
 * core, then drop the per-focus state.  last_pointer_impl, px and py stay so
 * get_pointer () can still answer. */
static void
pointer_focus_lost (GDKWDisplayWayland *wd)
{
  GdkDisplay *display = GDK_DISPLAY_OBJECT (wd);
  GDKWSeat *seat = _gdk_wayland_display_get_seat (display);
  GDKWWindowImplWayland *impl = seat->pointer_focus;

  _gdk_wayland_cursor_pointer_leave (display);

  seat->pointer_focus = NULL;

  if (impl != NULL)
    queue_crossing (wd, GDK_LEAVE_NOTIFY, impl);

  seat->button_mask = 0;
  seat->buttons_down = 0;
  memset (&seat->axis_frame, 0, sizeof seat->axis_frame);
  seat->scroll_accum[0] = seat->scroll_accum[1] = 0.0;

  end_implicit_grabs (display);
}

static void
wl_pointer_enter (void *data,
		  struct wl_pointer *pointer,
		  uint32_t serial,
		  struct wl_surface *surface,
		  wl_fixed_t surface_x,
		  wl_fixed_t surface_y)
{
  GDKWDisplayWayland *wd = data;
  GDKWSeat *seat = _gdk_wayland_display_get_seat (GDK_DISPLAY_OBJECT (wd));
  GDKWWindowImplWayland *impl;

  /* a compositor that omitted the leave for the previous surface */
  if (seat->pointer_focus != NULL)
    pointer_focus_lost (wd);

  seat->pointer_enter_serial = serial;
  seat->last_serial = serial;
  seat->button_mask = 0;
  seat->buttons_down = 0;

  impl = surface != NULL ? _gdk_wayland_impl_from_surface (wd, surface) : NULL;
  if (impl == NULL)
    {
      seat->pointer_focus = NULL;
      return;
    }

  seat->pointer_focus = impl;
  seat->last_pointer_impl = impl;
  seat->px = wl_fixed_to_double (surface_x);
  seat->py = wl_fixed_to_double (surface_y);

  _gdk_wayland_cursor_pointer_enter (GDK_DISPLAY_OBJECT (wd), serial);

  queue_crossing (wd, GDK_ENTER_NOTIFY, impl);
}

static void
wl_pointer_leave (void *data,
		  struct wl_pointer *pointer,
		  uint32_t serial,
		  struct wl_surface *surface)
{
  GDKWDisplayWayland *wd = data;
  GDKWSeat *seat = _gdk_wayland_display_get_seat (GDK_DISPLAY_OBJECT (wd));

  seat->last_serial = serial;

  pointer_focus_lost (wd);
}

static void
wl_pointer_motion (void *data,
		   struct wl_pointer *pointer,
		   uint32_t time,
		   wl_fixed_t surface_x,
		   wl_fixed_t surface_y)
{
  GDKWDisplayWayland *wd = data;
  GDKWSeat *seat = _gdk_wayland_display_get_seat (GDK_DISPLAY_OBJECT (wd));
  GdkEvent *event;

  if (!seat->pointer_focus)
    return;

  seat->px = wl_fixed_to_double (surface_x);
  seat->py = wl_fixed_to_double (surface_y);
  seat->last_time = time;

  event = new_event (wd, GDK_MOTION_NOTIFY, seat->pointer_focus);
  if (event == NULL)
    return;

  event->motion.time = time;
  event->motion.x = seat->px;
  event->motion.y = seat->py;
  _gdk_wayland_event_root_coords (seat->pointer_focus, seat->px, seat->py,
				  &event->motion.x_root,
				  &event->motion.y_root);
  event->motion.state = _gdk_wayland_seat_get_state (seat);
  event->motion.is_hint = FALSE;
  event->motion.device = seat->core_pointer;
  event->motion.axes = NULL;

  queue_event (GDK_DISPLAY_OBJECT (wd), event);
}

static void
wl_pointer_button (void *data,
		   struct wl_pointer *pointer,
		   uint32_t serial,
		   uint32_t time,
		   uint32_t button,
		   uint32_t state)
{
  GDKWDisplayWayland *wd = data;
  GDKWSeat *seat = _gdk_wayland_display_get_seat (GDK_DISPLAY_OBJECT (wd));
  gboolean pressed = (state == WL_POINTER_BUTTON_STATE_PRESSED);
  guint gdk_button;
  guint32 bit;
  guint state_before;
  GdkEvent *event;

  if (!seat->pointer_focus)
    return;

  gdk_button = translate_button (button);
  if (gdk_button == 0)
    return;

  bit = 1u << (gdk_button - 1);
  if (pressed && (seat->buttons_down & bit))
    return;
  if (!pressed && !(seat->buttons_down & bit))
    return;

  state_before = _gdk_wayland_seat_get_state (seat);

  if (pressed)
    seat->buttons_down |= bit;
  else
    seat->buttons_down &= ~bit;

  if (gdk_button <= 5)
    {
      guint mask = GDK_BUTTON1_MASK << (gdk_button - 1);

      if (pressed)
	seat->button_mask |= mask;
      else
	seat->button_mask &= ~mask;
    }

  /* Only a press serial can start a grab (move, resize, drag): a release
   * serial would be refused when another button is still held. */
  if (pressed)
    seat->button_serial = serial;
  seat->last_serial = serial;
  seat->last_time = time;

  event = new_event (wd, pressed ? GDK_BUTTON_PRESS : GDK_BUTTON_RELEASE,
		     seat->pointer_focus);
  if (event == NULL)
    return;

  event->button.time = time;
  event->button.x = seat->px;
  event->button.y = seat->py;
  _gdk_wayland_event_root_coords (seat->pointer_focus, seat->px, seat->py,
				  &event->button.x_root,
				  &event->button.y_root);
  event->button.state = state_before;
  event->button.button = gdk_button;
  event->button.device = seat->core_pointer;
  event->button.axes = NULL;

  queue_event (GDK_DISPLAY_OBJECT (wd), event);
}

/* ---- scroll (wl_pointer v5 axis grouping) ----------------------------- */

static void
emit_scroll (GDKWDisplayWayland *wd, GdkScrollDirection direction,
	     guint32 time)
{
  GDKWSeat *seat = _gdk_wayland_display_get_seat (GDK_DISPLAY_OBJECT (wd));
  GdkEvent *event;

  if (!seat->pointer_focus)
    return;

  event = new_event (wd, GDK_SCROLL, seat->pointer_focus);
  if (event == NULL)
    return;

  event->scroll.time = time;
  event->scroll.x = seat->px;
  event->scroll.y = seat->py;
  _gdk_wayland_event_root_coords (seat->pointer_focus, seat->px, seat->py,
				  &event->scroll.x_root,
				  &event->scroll.y_root);
  event->scroll.state = _gdk_wayland_seat_get_state (seat);
  event->scroll.direction = direction;
  event->scroll.device = seat->core_pointer;
  event->scroll.has_deltas = FALSE;

  queue_event (GDK_DISPLAY_OBJECT (wd), event);
}

static GdkScrollDirection
scroll_direction (guint axis, gboolean positive)
{
  if (axis == 0)
    return positive ? GDK_SCROLL_DOWN : GDK_SCROLL_UP;

  return positive ? GDK_SCROLL_RIGHT : GDK_SCROLL_LEFT;
}

static void
flush_axis_frame (GDKWDisplayWayland *wd)
{
  GDKWSeat *seat = _gdk_wayland_display_get_seat (GDK_DISPLAY_OBJECT (wd));
  GDKWAxisFrame *frame = &seat->axis_frame;
  guint32 time = frame->time != 0 ? frame->time : seat_time (seat);
  guint a;

  for (a = 0; a < 2; a++)
    {
      gboolean has_discrete = frame->axis[a].has_discrete;
      gboolean has_value = frame->axis[a].has_value;
      gdouble value = frame->axis[a].value;
      gboolean wheel;

      if (!has_value && !has_discrete && !frame->axis[a].stop)
	continue;

      if (!seat->pointer_focus)
	continue;

      if (frame->axis[a].stop)
	{
	  /* kinetic scroll stop: drop the remainder, emit nothing */
	  seat->scroll_accum[a] = 0.0;
	  continue;
	}

      wheel = frame->has_source
	? (frame->source == GDKW_TEST_AXIS_SOURCE_WHEEL ||
	   frame->source == GDKW_TEST_AXIS_SOURCE_WHEEL_TILT)
	: has_discrete;

      if (wheel)
	{
	  gint steps = 0;
	  gint n, i;

	  if (has_discrete)
	    steps = frame->axis[a].discrete;
	  else if (has_value)
	    {
	      steps = (gint) lround (value / GDKW_SCROLL_UNITS_PER_STEP);
	      if (steps == 0 && value != 0.0)
		steps = value > 0 ? 1 : -1;
	    }

	  n = steps < 0 ? -steps : steps;
	  for (i = 0; i < n; i++)
	    emit_scroll (wd, scroll_direction (a, steps > 0), time);
	}
      else if (has_value)
	{
	  seat->scroll_accum[a] += value / GDKW_SCROLL_UNITS_PER_STEP;

	  while (seat->scroll_accum[a] >= 1.0)
	    {
	      emit_scroll (wd, scroll_direction (a, TRUE), time);
	      seat->scroll_accum[a] -= 1.0;
	    }
	  while (seat->scroll_accum[a] <= -1.0)
	    {
	      emit_scroll (wd, scroll_direction (a, FALSE), time);
	      seat->scroll_accum[a] += 1.0;
	    }
	}
    }
}

static void
wl_pointer_frame (void *data, struct wl_pointer *pointer)
{
  GDKWDisplayWayland *wd = data;
  GDKWSeat *seat = _gdk_wayland_display_get_seat (GDK_DISPLAY_OBJECT (wd));

  flush_axis_frame (wd);
  memset (&seat->axis_frame, 0, sizeof seat->axis_frame);
}

static void
wl_pointer_axis_source (void *data,
			struct wl_pointer *pointer,
			uint32_t axis_source)
{
  GDKWDisplayWayland *wd = data;
  GDKWSeat *seat = _gdk_wayland_display_get_seat (GDK_DISPLAY_OBJECT (wd));

  seat->axis_frame.has_source = TRUE;
  seat->axis_frame.source = axis_source;
}

static void
wl_pointer_axis (void *data,
		 struct wl_pointer *pointer,
		 uint32_t time,
		 uint32_t axis,
		 wl_fixed_t value)
{
  GDKWDisplayWayland *wd = data;
  GDKWSeat *seat = _gdk_wayland_display_get_seat (GDK_DISPLAY_OBJECT (wd));

  if (axis > 1)
    return;

  seat->last_time = time;
  seat->axis_frame.time = time;
  seat->axis_frame.axis[axis].has_value = TRUE;
  seat->axis_frame.axis[axis].value = wl_fixed_to_double (value);

  /* no frame event will ever follow below version 5 */
  if (seat->seat_version < 5)
    {
      flush_axis_frame (wd);
      memset (&seat->axis_frame, 0, sizeof seat->axis_frame);
    }
}

static void
wl_pointer_axis_stop (void *data,
		      struct wl_pointer *pointer,
		      uint32_t time,
		      uint32_t axis)
{
  GDKWDisplayWayland *wd = data;
  GDKWSeat *seat = _gdk_wayland_display_get_seat (GDK_DISPLAY_OBJECT (wd));

  if (axis > 1)
    return;

  seat->last_time = time;
  seat->axis_frame.time = time;
  seat->axis_frame.axis[axis].stop = TRUE;
}

static void
wl_pointer_axis_discrete (void *data,
			  struct wl_pointer *pointer,
			  uint32_t axis,
			  int32_t discrete)
{
  GDKWDisplayWayland *wd = data;
  GDKWSeat *seat = _gdk_wayland_display_get_seat (GDK_DISPLAY_OBJECT (wd));

  if (axis > 1)
    return;

  seat->axis_frame.axis[axis].has_discrete = TRUE;
  seat->axis_frame.axis[axis].discrete = discrete;
}

static const struct wl_pointer_listener wl_pointer_listener = {
  .enter = wl_pointer_enter,
  .leave = wl_pointer_leave,
  .motion = wl_pointer_motion,
  .button = wl_pointer_button,
  .axis = wl_pointer_axis,
  .frame = wl_pointer_frame,
  .axis_source = wl_pointer_axis_source,
  .axis_stop = wl_pointer_axis_stop,
  .axis_discrete = wl_pointer_axis_discrete
};

/* ---- seat capabilities ------------------------------------------------ */

static void
release_pointer (GDKWDisplayWayland *wd, gboolean notify)
{
  GDKWSeat *seat = _gdk_wayland_display_get_seat (GDK_DISPLAY_OBJECT (wd));

  /* capability loss while a button is down: synthesise the leave and end the
   * implicit grab, as wl_pointer.leave would.  Skipped during finalize, when
   * the windows are already gone. */
  if (notify && seat->pointer_focus != NULL)
    pointer_focus_lost (wd);

  if (wd->wl_pointer)
    {
      if (wl_pointer_get_version (wd->wl_pointer) >= 3)
	wl_pointer_release (wd->wl_pointer);
      else
	wl_pointer_destroy (wd->wl_pointer);
      wd->wl_pointer = NULL;
    }

  seat->pointer_focus = NULL;
  seat->last_pointer_impl = NULL;
  seat->button_mask = 0;
  seat->buttons_down = 0;
  memset (&seat->axis_frame, 0, sizeof seat->axis_frame);
  seat->scroll_accum[0] = seat->scroll_accum[1] = 0.0;
}

static void
release_keyboard (GDKWDisplayWayland *wd)
{
  GDKWSeat *seat = _gdk_wayland_display_get_seat (GDK_DISPLAY_OBJECT (wd));

  if (wd->wl_keyboard)
    {
      if (wl_keyboard_get_version (wd->wl_keyboard) >= 3)
	wl_keyboard_release (wd->wl_keyboard);
      else
	wl_keyboard_destroy (wd->wl_keyboard);
      wd->wl_keyboard = NULL;
    }

  _gdk_wayland_seat_repeat_cancel (seat);
  kbd_pending_cancel (wd);
  seat->keyboard_focus = NULL;
  seat->modifier_mask = 0;
}

/* M9 (I10b).  The popup piece calls this whenever it releases the role of an
 * impl that stays alive: the compositor will not send the pointer leave or
 * keyboard leave for a destroyed surface, so the seat stops pointing at it
 * and the LEAVE is synthesized here. */
void
_gdk_wayland_seat_surface_gone (GDKWDisplayWayland *wd,
				GDKWWindowImplWayland *impl)
{
  GDKWSeat *seat;

  if (wd == NULL || impl == NULL)
    return;

  seat = _gdk_wayland_display_get_seat (GDK_DISPLAY_OBJECT (wd));

  if (seat->pointer_focus == impl)
    pointer_focus_lost (wd);

  if (seat->keyboard_focus == impl)
    {
      seat->keyboard_focus = NULL;
      _gdk_wayland_seat_repeat_cancel (seat);
    }
}

static void
wl_seat_capabilities (void *data,
		      struct wl_seat *wl_seat,
		      uint32_t capabilities)
{
  GDKWDisplayWayland *wd = data;
  GDKWSeat *seat = _gdk_wayland_display_get_seat (GDK_DISPLAY_OBJECT (wd));

  seat->capabilities = capabilities;

  /* keyboard */
  if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !wd->wl_keyboard)
    {
      wd->wl_keyboard = wl_seat_get_keyboard (wl_seat);
      wl_keyboard_add_listener (wd->wl_keyboard, &wl_keyboard_listener, wd);
    }
  else if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && wd->wl_keyboard)
    release_keyboard (wd);

  /* pointer */
  if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !wd->wl_pointer)
    {
      wd->wl_pointer = wl_seat_get_pointer (wl_seat);
      wl_pointer_add_listener (wd->wl_pointer, &wl_pointer_listener, wd);
    }
  else if (!(capabilities & WL_SEAT_CAPABILITY_POINTER) && wd->wl_pointer)
    release_pointer (wd, TRUE);
}

static void
wl_seat_name (void *data, struct wl_seat *wl_seat, const char *name)
{
}

static const struct wl_seat_listener wl_seat_listener = {
  .capabilities = wl_seat_capabilities,
  .name = wl_seat_name
};

/* ---- public interface ------------------------------------------------- */

void
_gdk_wayland_seat_bind (GDKWDisplayWayland *display,
			struct wl_registry *registry,
			uint32_t name,
			uint32_t version)
{
  GDKWSeat *seat = _gdk_wayland_display_get_seat (GDK_DISPLAY_OBJECT (display));

  /* only the first seat is used */
  if (display->wl_seat != NULL)
    return;

  display->wl_seat =
    wl_registry_bind (registry, name, &wl_seat_interface, MIN (version, 5));

  if (!display->wl_seat)
    return;

  seat->seat_version = wl_seat_get_version (display->wl_seat);

  wl_seat_add_listener (display->wl_seat, &wl_seat_listener, display);

  _gdk_wayland_dd_seat_ready (display);
}

static GdkDeviceAxis core_axes[] = {
  { GDK_AXIS_X, 0, 0 },
  { GDK_AXIS_Y, 0, 0 }
};

void
_gdk_wayland_events_init (GdkDisplay *display)
{
  GDKWDisplayWayland *wayland_display = GDKW_DISPLAY_WAYLAND (display);
  GDKWSeat *seat = _gdk_wayland_display_get_seat (display);
  GSource *source;
  WaylandEventSource *ws;

  /* The X11 backend creates the core pointer in _gdk_init_input_core ();
   * got_event () compares event devices against display->core_pointer.  The
   * X11-private tail of the device stays zeroed and its dispose is a no-op
   * when the private display is NULL. */
  if (display->core_pointer == NULL)
    {
      display->core_pointer = g_object_new (GDK_TYPE_DEVICE, NULL);
      display->core_pointer->name = "Core Pointer";
      display->core_pointer->source = GDK_SOURCE_MOUSE;
      display->core_pointer->mode = GDK_MODE_SCREEN;
      display->core_pointer->has_cursor = TRUE;
      display->core_pointer->num_axes = 2;
      display->core_pointer->axes = core_axes;
      display->core_pointer->num_keys = 0;
      display->core_pointer->keys = NULL;
    }

  if (seat->core_pointer == NULL)
    seat->core_pointer = g_object_ref (display->core_pointer);

  source = g_source_new (&wayland_event_funcs, sizeof (WaylandEventSource));
  g_source_set_name (source, "GDK Wayland event source");
  g_source_set_priority (source, GDK_PRIORITY_EVENTS);

  ws = (WaylandEventSource *) source;
  ws->display = display;
  ws->reading = FALSE;
  ws->poll_fd.fd = wl_display_get_fd (wayland_display->wl_display);
  ws->poll_fd.events = G_IO_IN | G_IO_ERR | G_IO_HUP;
  ws->poll_fd.revents = 0;

  g_source_add_poll (source, &ws->poll_fd);
  g_source_attach (source, g_main_context_default ());

  /* the data slot owns the creation reference */
  g_object_set_data_full (G_OBJECT (display), EVENT_SOURCE_DATA_KEY, source,
			  (GDestroyNotify) g_source_unref);
}

void
_gdk_wayland_events_finalize (GdkDisplay *display)
{
  GDKWDisplayWayland *wayland_display = GDKW_DISPLAY_WAYLAND (display);
  GDKWSeat *seat = _gdk_wayland_display_get_seat (display);
  GSource *source;

  _gdk_wayland_seat_repeat_cancel (seat);

  source = g_object_get_data (G_OBJECT (display), EVENT_SOURCE_DATA_KEY);
  if (source != NULL)
    g_source_destroy (source);

  release_keyboard (wayland_display);
  seat->pointer_focus = NULL;
  release_pointer (wayland_display, FALSE);

  seat->keyboard_focus = NULL;
  seat->pointer_focus = NULL;
  seat->last_pointer_impl = NULL;

  if (wayland_display->wl_seat)
    {
      if (wl_seat_get_version (wayland_display->wl_seat) >= 5)
	wl_seat_release (wayland_display->wl_seat);
      else
	wl_seat_destroy (wayland_display->wl_seat);
      wayland_display->wl_seat = NULL;
    }

  /* the display's own reference; the seat keeps its own until it is freed */
  if (display->core_pointer != NULL)
    {
      g_object_unref (display->core_pointer);
      display->core_pointer = NULL;
    }
}

/* ===========================================================================
 * Test hooks (declared in the public section of gdkseat-wayland.h).
 *
 * Each hook pushes one synthetic protocol event through the real listener
 * with a NULL proxy and a private serial counter.
 * ======================================================================== */

static uint32_t test_serial = 1000;

static GDKWDisplayWayland *
test_display (GdkDisplay *display)
{
  if (display == NULL || !GDKW_IS_DISPLAY_WAYLAND (display))
    return NULL;

  return GDKW_DISPLAY_WAYLAND (display);
}

static GDKWWindowImplWayland *
test_impl (GdkWindow *window)
{
  GDKWWindowImplWayland *impl = _gdk_wayland_window_get_impl (window);

  if (impl == NULL || impl->wl_surface == NULL)
    return NULL;

  return impl;
}

/* Feeds @text (XKB_V1 keymap text) through the real wl_keyboard_keymap
 * listener, the way a compositor sends it. */
static gboolean
test_keymap_send_text (GDKWDisplayWayland *wd, const gchar *text, gsize len)
{
  int fd;

  fd = memfd_create ("ydk-test-keymap", MFD_CLOEXEC);
  if (fd < 0)
    return FALSE;

  /* len + 1 includes the NUL, like a real compositor */
  if (write (fd, text, len + 1) != (ssize_t) (len + 1) ||
      lseek (fd, 0, SEEK_SET) != 0)
    {
      close (fd);
      return FALSE;
    }

  /* the listener closes fd */
  wl_keyboard_keymap (wd, NULL, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd,
		      (uint32_t) (len + 1));

  return TRUE;
}

gboolean
_gdk_wayland_test_keyboard_keymap (GdkDisplay *display)
{
  GDKWDisplayWayland *wd = test_display (display);
  gchar *text;
  gsize len;
  gboolean ok;

  if (wd == NULL)
    return FALSE;

  text = _gdk_wayland_keymap_default_text (&len);
  ok = test_keymap_send_text (wd, text, len);
  g_free (text);

  return ok;
}

gboolean
_gdk_wayland_test_keyboard_keymap_text (GdkDisplay *display, const gchar *text)
{
  GDKWDisplayWayland *wd = test_display (display);

  if (wd == NULL || text == NULL)
    return FALSE;

  return test_keymap_send_text (wd, text, strlen (text));
}

gboolean
_gdk_wayland_test_keyboard_repeat_info (GdkDisplay *display,
					gint        rate,
					gint        delay)
{
  GDKWDisplayWayland *wd = test_display (display);

  if (wd == NULL)
    return FALSE;

  wl_keyboard_repeat_info (wd, NULL, rate, delay);

  return TRUE;
}

gboolean
_gdk_wayland_test_keyboard_enter (GdkDisplay *display, GdkWindow *window)
{
  GDKWDisplayWayland *wd = test_display (display);
  GDKWWindowImplWayland *impl = test_impl (window);
  struct wl_array keys;

  if (wd == NULL || impl == NULL)
    return FALSE;

  wl_array_init (&keys);
  wl_keyboard_enter (wd, NULL, ++test_serial, impl->wl_surface, &keys);
  wl_array_release (&keys);

  return TRUE;
}

gboolean
_gdk_wayland_test_keyboard_leave (GdkDisplay *display, GdkWindow *window)
{
  GDKWDisplayWayland *wd = test_display (display);
  GDKWWindowImplWayland *impl = test_impl (window);

  if (wd == NULL || impl == NULL)
    return FALSE;

  wl_keyboard_leave (wd, NULL, ++test_serial, impl->wl_surface);

  return TRUE;
}

gboolean
_gdk_wayland_test_keyboard_key (GdkDisplay *display,
				guint32     time,
				guint32     evdev_key,
				gboolean    pressed)
{
  GDKWDisplayWayland *wd = test_display (display);
  GDKWSeat *seat;

  if (wd == NULL)
    return FALSE;

  seat = _gdk_wayland_display_get_seat (display);
  if (!seat->keyboard_focus || !keymap_ready (wd))
    return FALSE;

  wl_keyboard_key (wd, NULL, ++test_serial, time, evdev_key,
		   pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
			   : WL_KEYBOARD_KEY_STATE_RELEASED);

  return TRUE;
}

gboolean
_gdk_wayland_test_keyboard_modifiers (GdkDisplay     *display,
				      GdkModifierType gdk_mods)
{
  GDKWDisplayWayland *wd = test_display (display);
  guint mask;

  if (wd == NULL || !keymap_ready (wd))
    return FALSE;

  mask = _gdk_wayland_keymap_xkb_mods_from_gdk (wd->keymap, gdk_mods);
  wl_keyboard_modifiers (wd, NULL, ++test_serial, mask, 0, 0, 0);

  return TRUE;
}

gboolean
_gdk_wayland_test_keyboard_state (GdkDisplay     *display,
				  GdkModifierType depressed,
				  GdkModifierType locked,
				  guint           group)
{
  GDKWDisplayWayland *wd = test_display (display);
  guint dep, lock;

  if (wd == NULL || !keymap_ready (wd))
    return FALSE;

  dep = _gdk_wayland_keymap_xkb_mods_from_gdk (wd->keymap, depressed);
  lock = _gdk_wayland_keymap_xkb_mods_from_gdk (wd->keymap, locked);
  wl_keyboard_modifiers (wd, NULL, ++test_serial, dep, 0, lock, group);

  return TRUE;
}

gboolean
_gdk_wayland_test_pointer_enter (GdkDisplay *display,
				 GdkWindow  *window,
				 gdouble     x,
				 gdouble     y)
{
  GDKWDisplayWayland *wd = test_display (display);
  GDKWWindowImplWayland *impl = test_impl (window);

  if (wd == NULL || impl == NULL)
    return FALSE;

  wl_pointer_enter (wd, NULL, ++test_serial, impl->wl_surface,
		    wl_fixed_from_double (x), wl_fixed_from_double (y));

  return TRUE;
}

gboolean
_gdk_wayland_test_pointer_leave (GdkDisplay *display, GdkWindow *window)
{
  GDKWDisplayWayland *wd = test_display (display);
  GDKWWindowImplWayland *impl = test_impl (window);

  if (wd == NULL || impl == NULL)
    return FALSE;

  wl_pointer_leave (wd, NULL, ++test_serial, impl->wl_surface);

  return TRUE;
}

gboolean
_gdk_wayland_test_pointer_motion (GdkDisplay *display,
				  guint32     time,
				  gdouble     x,
				  gdouble     y)
{
  GDKWDisplayWayland *wd = test_display (display);

  if (wd == NULL)
    return FALSE;

  wl_pointer_motion (wd, NULL, time,
		     wl_fixed_from_double (x), wl_fixed_from_double (y));

  return TRUE;
}

gboolean
_gdk_wayland_test_pointer_button (GdkDisplay *display,
				  guint32     time,
				  guint32     evdev_button,
				  gboolean    pressed)
{
  GDKWDisplayWayland *wd = test_display (display);

  if (wd == NULL)
    return FALSE;

  wl_pointer_button (wd, NULL, ++test_serial, time, evdev_button,
		     pressed ? WL_POINTER_BUTTON_STATE_PRESSED
			     : WL_POINTER_BUTTON_STATE_RELEASED);

  return TRUE;
}

gboolean
_gdk_wayland_test_pointer_axis_source (GdkDisplay *display, guint32 source)
{
  GDKWDisplayWayland *wd = test_display (display);

  if (wd == NULL)
    return FALSE;

  wl_pointer_axis_source (wd, NULL, source);

  return TRUE;
}

gboolean
_gdk_wayland_test_pointer_axis (GdkDisplay *display,
				guint32     time,
				guint32     axis,
				gdouble     value)
{
  GDKWDisplayWayland *wd = test_display (display);

  if (wd == NULL)
    return FALSE;

  wl_pointer_axis (wd, NULL, time, axis, wl_fixed_from_double (value));

  return TRUE;
}

gboolean
_gdk_wayland_test_pointer_axis_discrete (GdkDisplay *display,
					 guint32     axis,
					 gint32      discrete)
{
  GDKWDisplayWayland *wd = test_display (display);

  if (wd == NULL)
    return FALSE;

  wl_pointer_axis_discrete (wd, NULL, axis, discrete);

  return TRUE;
}

gboolean
_gdk_wayland_test_pointer_axis_stop (GdkDisplay *display,
				     guint32     time,
				     guint32     axis)
{
  GDKWDisplayWayland *wd = test_display (display);

  if (wd == NULL)
    return FALSE;

  wl_pointer_axis_stop (wd, NULL, time, axis);

  return TRUE;
}

gboolean
_gdk_wayland_test_pointer_frame (GdkDisplay *display)
{
  GDKWDisplayWayland *wd = test_display (display);

  if (wd == NULL)
    return FALSE;

  wl_pointer_frame (wd, NULL);

  return TRUE;
}

gboolean
_gdk_wayland_test_seat_set_version (GdkDisplay *display, guint version)
{
  GDKWDisplayWayland *wd = test_display (display);

  if (wd == NULL)
    return FALSE;

  _gdk_wayland_display_get_seat (display)->seat_version = version;

  return TRUE;
}

gboolean
_gdk_wayland_test_seat_info (GdkDisplay *display, GDKWTestSeatInfo *info)
{
  GDKWDisplayWayland *wd = test_display (display);
  GDKWSeat *seat;

  if (wd == NULL || info == NULL)
    return FALSE;

  seat = _gdk_wayland_display_get_seat (display);

  memset (info, 0, sizeof *info);
  info->pointer_focus = seat->pointer_focus ? seat->pointer_focus->wrapper : NULL;
  info->keyboard_focus = seat->keyboard_focus ? seat->keyboard_focus->wrapper : NULL;
  info->x = seat->px;
  info->y = seat->py;
  info->button_mask = seat->button_mask;
  info->modifier_mask = seat->modifier_mask;
  info->repeat_key = seat->repeat.key;
  info->repeat_count = seat->repeat.count;
  info->seat_version = seat->seat_version;
  info->explicit_grab_active = seat->grab.active;

  return TRUE;
}
