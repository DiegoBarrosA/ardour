/*
 * gdkdisplay-wayland.c
 *
 * ydk-wayland fork: Wayland GdkDisplay subclass + the display-oriented
 * members of the internal `_gdk_wayland_*` contract.
 *
 * Skeleton status: connects to the compositor and gives back a valid
 * GdkDisplay so that `gdk_init_check`/`gdk_display_open` succeed under a
 * Wayland session.  Window/styling machinery lands in M2.
 */

#include "config.h"

#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <wayland-client.h>

#include <ydk/gdk.h>
#include <ydk/gdkapplaunchcontext.h>
#include <ydk/gdkcairo.h>

#include "xdg-shell-client.h"

#include <ydk/gdkinternals.h>

#include "gdkprivate-wayland.h"
#include "gdkseat-wayland.h"

#ifdef GDK_COMPILATION
#include "gdkalias.h"
#endif

static void gdkw_display_wayland_finalize (GObject *object);
static void gdkw_display_wayland_dispose (GObject *object);

G_DEFINE_TYPE (GDKWDisplayWayland, gdkw_display_wayland, GDK_TYPE_DISPLAY)

static const gchar *
gdkw_display_wayland_get_display_name (GdkDisplay *display)
{
  GDKWDisplayWayland *wd = GDKW_DISPLAY_WAYLAND (display);

  return wd->socket_name ? wd->socket_name : "wayland-0";
}

static gint
gdkw_display_wayland_get_n_screens (GdkDisplay *display)
{
  /* Wayland has no separate screens. */
  return 1;
}

static GdkScreen *
gdkw_display_wayland_get_screen (GdkDisplay *display, gint screen_num)
{
  GDKWDisplayWayland *wayland_display = GDKW_DISPLAY_WAYLAND (display);

  return screen_num == 0 ? wayland_display->default_screen : NULL;
}

static GdkScreen *
gdkw_display_wayland_get_default_screen (GdkDisplay *display)
{
  return GDKW_DISPLAY_WAYLAND (display)->default_screen;
}

void
_gdk_wayland_display_present_all (GDKWDisplayWayland *display)
{
  GSList *l;

  for (l = display->windows; l; l = l->next)
    {
      GDKWWindowImplWayland *impl = l->data;

      _gdk_wayland_window_impl_present (impl);
    }
}

static void
gdkw_display_wayland_sync (GdkDisplay *display)
{
  GDKWDisplayWayland *wayland_display = GDKW_DISPLAY_WAYLAND (display);

  if (wayland_display->wl_display == NULL)
    return;

  /* Present any outstanding window content, then block until the
   * compositor has consumed it.  The roundtrip is what delivers the
   * xdg_surface configure events that let toplevels map. */
  _gdk_wayland_display_present_all (wayland_display);
  wl_display_flush (wayland_display->wl_display);
  wl_display_roundtrip (wayland_display->wl_display);
}

static void
gdkw_display_wayland_flush (GdkDisplay *display)
{
  GDKWDisplayWayland *wayland_display = GDKW_DISPLAY_WAYLAND (display);

  if (wayland_display->wl_display == NULL)
    return;

  _gdk_wayland_display_present_all (wayland_display);
  wl_display_flush (wayland_display->wl_display);
}

static void
xdg_wm_base_ping (void *data,
		  struct xdg_wm_base *xdg_wm_base,
		  uint32_t serial)
{
  xdg_wm_base_pong (xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
  xdg_wm_base_ping
};

/* Runs from gdk_display_close () (g_object_run_dispose) and again at the
 * final unref, so it must be idempotent.  Destroys every window of the
 * default screen while the display is still fully valid, so no GdkWindow
 * reaches core code with a dead display.  Also valid on a half built
 * display (no screen, no seat, no event source). */
static void
gdkw_display_wayland_dispose (GObject *object)
{
  GDKWDisplayWayland *display = GDKW_DISPLAY_WAYLAND (object);

  if (display->default_screen != NULL &&
      GDKW_SCREEN_WAYLAND (display->default_screen)->display ==
      GDK_DISPLAY_OBJECT (display))
    {
      GdkWindow *root = gdk_screen_get_root_window (display->default_screen);

      if (root != NULL)
	{
	  GList *children = gdk_window_get_children (root);
	  GList *l;

	  for (l = children; l; l = l->next)
	    g_object_ref (l->data);
	  for (l = children; l; l = l->next)
	    {
	      if (!GDK_WINDOW_DESTROYED (l->data))
		gdk_window_destroy (l->data);
	    }
	  for (l = children; l; l = l->next)
	    g_object_unref (l->data);
	  g_list_free (children);
	}
    }

  /* Stops the event source and the seat proxies.  A second call finds
   * everything already released and does nothing. */
  _gdk_wayland_events_finalize (GDK_DISPLAY_OBJECT (display));

  G_OBJECT_CLASS (gdkw_display_wayland_parent_class)->dispose (object);
}

static void
gdkw_display_wayland_finalize (GObject *object)
{
  GDKWDisplayWayland *display = GDKW_DISPLAY_WAYLAND (object);

  /* give the running window impls (their wl objects are already gone if
   * gdk_window_destroy ran) a last chance to release their surfaces */
  if (display->windows)
    {
      /* teardown removes its node from display->windows, so walk a copy */
      GSList *copy = g_slist_copy (display->windows);
      GSList *l;

      for (l = copy; l; l = l->next)
	_gdk_wayland_window_impl_teardown (l->data);
      g_slist_free (copy);
      g_slist_free (display->windows);
      display->windows = NULL;
    }

  /* Tear down the event source and seat / keyboard / pointer proxies. */
  _gdk_wayland_events_finalize (GDK_DISPLAY_OBJECT (display));

  if (display->keymap)
    {
      display->keymap->display = NULL;
      g_object_unref (display->keymap);
      display->keymap = NULL;
    }

  if (display->wl_registry)
    {
      wl_registry_destroy (display->wl_registry);
      display->wl_registry = NULL;
    }
  if (display->wl_shm)
    {
      wl_shm_destroy (display->wl_shm);
      display->wl_shm = NULL;
    }
  if (display->xdg_wm_base)
    {
      xdg_wm_base_destroy (display->xdg_wm_base);
      display->xdg_wm_base = NULL;
    }
  if (display->wl_compositor)
    {
      wl_compositor_destroy (display->wl_compositor);
      display->wl_compositor = NULL;
    }
  if (display->wl_output)
    {
      wl_output_destroy (display->wl_output);
      display->wl_output = NULL;
    }

  if (display->wl_display)
    {
      wl_display_flush (display->wl_display);
      wl_display_disconnect (display->wl_display);
      display->wl_display = NULL;
    }

  /* The screen is never freed (spec section 5), so a window that outlives
   * the display must not see a dangling screen->display. */
  if (display->default_screen != NULL)
    GDKW_SCREEN_WAYLAND (display->default_screen)->display = NULL;

  g_free (display->socket_name);
  display->socket_name = NULL;

  G_OBJECT_CLASS (gdkw_display_wayland_parent_class)->finalize (object);
}

static void
gdkw_display_wayland_class_init (GDKWDisplayWaylandClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GdkDisplayClass *display_class = GDK_DISPLAY_CLASS (klass);

  object_class->dispose = gdkw_display_wayland_dispose;
  object_class->finalize = gdkw_display_wayland_finalize;

  display_class->get_display_name = gdkw_display_wayland_get_display_name;
  display_class->get_n_screens = gdkw_display_wayland_get_n_screens;
  display_class->get_screen = gdkw_display_wayland_get_screen;
  display_class->get_default_screen = gdkw_display_wayland_get_default_screen;
  display_class->flush = gdkw_display_wayland_flush;
  display_class->sync = gdkw_display_wayland_sync;
  display_class->keymap = gdkw_display_wayland_get_keymap;
}

static void
gdkw_display_wayland_init (GDKWDisplayWayland *display)
{
  display->wl_display = NULL;
  display->wl_registry = NULL;
  display->wl_compositor = NULL;
  display->wl_shm = NULL;
  display->xdg_wm_base = NULL;
  display->wl_output = NULL;
  display->serial = 0;
  display->default_screen = NULL;
  display->output_width = 0;
  display->output_height = 0;
  display->windows = NULL;
  display->xdg_shell_supported = FALSE;
}

/*************** wl_registry + wl_output (screen geometry) ***************/

static void
wl_output_geometry (void *data,
		    struct wl_output *output,
		    int32_t x,
		    int32_t y,
		    int32_t physical_width,
		    int32_t physical_height,
		    int32_t subpixel,
		    const char *make,
		    const char *model,
		    int32_t transform)
{
  GDKWDisplayWayland *display = data;
  GDKWOutputInfo *info =
    _gdk_wayland_display_get_output_info (GDK_DISPLAY_OBJECT (display));

  /* Only the first wl_output is bound, so there is one record. */
  info->have_geometry = TRUE;
  info->x = x;
  info->y = y;
  info->mm_width = physical_width;
  info->mm_height = physical_height;
  info->subpixel = subpixel;
  info->transform = transform;

  g_free (info->make);
  g_free (info->model);
  info->make = g_strdup (make);
  info->model = g_strdup (model);
}

static void
wl_output_mode (void *data,
		struct wl_output *output,
		uint32_t flags,
		int32_t width,
		int32_t height,
		int32_t refresh)
{
  GDKWDisplayWayland *display = data;

  /* M1 tracks a single output (the first with a current mode); multihead
   * handling can extend this when window support lands. */
  if (flags & WL_OUTPUT_MODE_CURRENT)
    {
      GDKWOutputInfo *info =
	_gdk_wayland_display_get_output_info (GDK_DISPLAY_OBJECT (display));

      display->output_width = width;
      display->output_height = height;

      info->have_mode = TRUE;
      info->width = width;
      info->height = height;
      info->refresh = refresh;
    }
}

static void
wl_output_done (void *data, struct wl_output *output)
{
}

static const struct wl_output_listener wl_output_listener = {
  wl_output_geometry,
  wl_output_mode,
  wl_output_done
};

static void
wl_registry_global (void *data,
		    struct wl_registry *registry,
		    uint32_t name,
		    const char *interface,
		    uint32_t version)
{
  GDKWDisplayWayland *display = data;

  if (strcmp (interface, "wl_output") == 0 && !display->wl_output)
    {
      display->wl_output =
	wl_registry_bind (registry, name, &wl_output_interface, 1);
      wl_output_add_listener (display->wl_output, &wl_output_listener,
			      display);
    }
  else if (strcmp (interface, "wl_compositor") == 0 && !display->wl_compositor)
    {
      display->wl_compositor =
	wl_registry_bind (registry, name, &wl_compositor_interface, 1);
    }
  else if (strcmp (interface, "wl_shm") == 0 && !display->wl_shm)
    {
      display->wl_shm =
	wl_registry_bind (registry, name, &wl_shm_interface, 1);
    }
  else if (strcmp (interface, "xdg_wm_base") == 0 && !display->xdg_wm_base)
    {
      display->xdg_wm_base =
	wl_registry_bind (registry, name, &xdg_wm_base_interface, 1);
      xdg_wm_base_add_listener (display->xdg_wm_base,
				&xdg_wm_base_listener, display);
      display->xdg_shell_supported = TRUE;
    }
  else if (strcmp (interface, "wl_seat") == 0 && !display->wl_seat)
    {
      /* the events piece binds at MIN (version, 5) and adds the listener */
      _gdk_wayland_seat_bind (display, registry, name, version);
    }
}

static void
wl_registry_global_remove (void *data,
			   struct wl_registry *registry,
			   uint32_t name)
{
}

static const struct wl_registry_listener wl_registry_listener = {
  wl_registry_global,
  wl_registry_global_remove
};

GdkDisplay *
_gdk_wayland_display_open (const gchar *display_name)
{
  GDKWDisplayWayland *display;
  struct wl_display *wl_display;

  if (display_name)
    wl_display = wl_display_connect (display_name);
  else
    wl_display = wl_display_connect (NULL);

  if (!wl_display)
    return NULL;

  display = g_object_new (GDKW_TYPE_DISPLAY_WAYLAND, NULL);
  display->wl_display = wl_display;
  if (display_name != NULL)
    display->socket_name = g_strdup (display_name);
  else if (g_getenv ("WAYLAND_DISPLAY") != NULL &&
	   g_getenv ("WAYLAND_DISPLAY")[0] != '\0')
    display->socket_name = g_strdup (g_getenv ("WAYLAND_DISPLAY"));
  else
    display->socket_name = g_strdup ("wayland-0");
  display->serial = 0;

  /* Walk the registry for wl_output so the screen below has compositor
   * geometry; wl_shm/xdg_wm_base are picked up with window support.  A
   * second roundtrip settles the output mode events. */
  display->wl_registry = wl_display_get_registry (wl_display);
  wl_registry_add_listener (display->wl_registry, &wl_registry_listener,
			    display);

  if (wl_display_roundtrip (wl_display) < 0 ||
      wl_display_roundtrip (wl_display) < 0)
    {
      g_object_unref (display);
      return NULL;
    }

  wl_registry_destroy (display->wl_registry);
  display->wl_registry = NULL;

  /* M1: one logical screen per display.  The depth/scale of the output is
   * read later if the fallback is not used. */
  if (display->output_width <= 0 || display->output_height <= 0)
    {
      display->output_width = 1024;
      display->output_height = 768;
    }

  display->default_screen =
    _gdk_wayland_screen_new (GDK_DISPLAY_OBJECT (display),
			     display->output_width, display->output_height);

  /* Wire up the input event source; seat listeners are registered as the
   * compositor announces wl_seat globals. */
  _gdk_wayland_events_init (GDK_DISPLAY_OBJECT (display));

  return GDK_DISPLAY_OBJECT (display);
}

/*************** windowning contract (stubs until M1) ***************/

void
_gdk_wayland_windowing_set_default_display (GdkDisplay *display)
{
}

GdkRegion *
_gdk_wayland_windowing_window_get_shape (GdkWindow *window)
{
  return NULL;
}

GdkRegion *
_gdk_wayland_windowing_window_get_input_shape (GdkWindow *window)
{
  return NULL;
}

GdkRegion *
_gdk_wayland_windowing_get_shape_for_mask (GdkBitmap *mask)
{
  return NULL;
}

void
_gdk_wayland_windowing_window_beep (GdkWindow *window)
{
}

/*************** pointer state, grabs (M6) ***************/

/* Same wraparound comparison as XSERVER_TIME_IS_LATER in gdkdisplay-x11.c */
#define WTIME_IS_LATER(time1, time2)                              \
  ( (( time1 > time2 ) && ( time1 - time2 < ((guint32)-1)/2 )) || \
    (( time1 < time2 ) && ( time2 - time1 > ((guint32)-1)/2 ))    \
  )

void
_gdk_wayland_windowing_get_pointer (GdkDisplay *display,
				    GdkScreen **screen,
				    gint *x,
				    gint *y,
				    GdkModifierType *mask)
{
  GDKWSeat *seat = _gdk_wayland_display_get_seat (display);

  if (screen)
    *screen = GDKW_DISPLAY_WAYLAND (display)->default_screen;

  /* px / py survive wl_pointer.leave, so this is also the last known
   * position when the pointer is outside every surface of ours.  Toplevels
   * sit at the root origin, so surface coordinates are root coordinates.  A
   * pointer that was never seen reads (0, 0). */
  if (x)
    *x = (gint) seat->px;
  if (y)
    *y = (gint) seat->py;
  if (mask)
    *mask = _gdk_wayland_seat_get_state (seat);
}

GdkWindow *
_gdk_wayland_windowing_window_at_pointer (GdkDisplay *display,
					  gint *win_x,
					  gint *win_y,
					  GdkModifierType *mask,
					  gboolean get_toplevel)
{
  GDKWSeat *seat = _gdk_wayland_display_get_seat (display);
  GDKWWindowImplWayland *focus = seat->pointer_focus;

  if (mask)
    *mask = _gdk_wayland_seat_get_state (seat);

  /* Toplevels are the only native windows; the core descends into children
   * itself, so get_toplevel does not change the answer. */
  if (focus != NULL && focus->wrapper != NULL &&
      !GDK_WINDOW_DESTROYED (focus->wrapper))
    {
      if (win_x)
	*win_x = (gint) seat->px;
      if (win_y)
	*win_y = (gint) seat->py;
      return focus->wrapper;
    }

  if (win_x)
    *win_x = 0;
  if (win_y)
    *win_y = 0;

  return NULL;
}

/* Wayland limits.  There is no active pointer grab: a client only ever sees
 * pointer events for its own surfaces, and the compositor already routes
 * motion to the surface that holds a pressed button until it is released.
 * confine_to is recorded but ignored (it would need zwp_pointer_constraints)
 * and the grab cursor is only remembered for the cursor milestone.
 *
 * The core (gdk_pointer_grab) does the owner_events / event_mask routing, the
 * crossing events and the GdkPointerGrabInfo list through
 * _gdk_display_add_pointer_grab () AFTER this returns GDK_GRAB_SUCCESS.  The
 * implicit grab on button press and its release are done by the core inside
 * _gdk_windowing_got_event (), so none of that is reimplemented here.  This
 * hook only validates the request and keeps the backend's own record. */
GdkGrabStatus
_gdk_wayland_windowing_pointer_grab (GdkWindow *window,
				     GdkWindow *native,
				     gboolean owner_events,
				     GdkEventMask event_mask,
				     GdkWindow *confine_to,
				     GdkCursor *cursor,
				     guint32 time_)
{
  GdkDisplay *display;
  GDKWSeat *seat;
  GDKWWindowImplWayland *impl;

  impl = _gdk_wayland_window_get_impl (native);
  if (impl == NULL || GDK_WINDOW_DESTROYED (native) ||
      !GDK_WINDOW_IS_MAPPED (native) || impl->wl_surface == NULL)
    return GDK_GRAB_NOT_VIEWABLE;

  display = gdk_drawable_get_display (GDK_DRAWABLE (native));
  seat = _gdk_wayland_display_get_seat (display);

  if (time_ != GDK_CURRENT_TIME && seat->grab.active &&
      seat->grab.time != GDK_CURRENT_TIME &&
      WTIME_IS_LATER (seat->grab.time, time_))
    return GDK_GRAB_INVALID_TIME;

  _gdk_wayland_seat_grab_clear (seat);

  seat->grab.active = TRUE;
  seat->grab.window = g_object_ref (window);
  seat->grab.native = native;
  seat->grab.owner_events = owner_events;
  seat->grab.event_mask = event_mask;
  seat->grab.confine_to = confine_to;
  seat->grab.cursor = cursor ? gdk_cursor_ref (cursor) : NULL;
  seat->grab.time = time_;
  seat->grab.serial = _gdk_wayland_event_serial (display);

  return GDK_GRAB_SUCCESS;
}

/* gdk_display_pointer_ungrab () */
void
_gdk_wayland_display_pointer_ungrab (GdkDisplay *display, guint32 time_)
{
  GDKWSeat *seat;
  GdkPointerGrabInfo *grab;

  if (display == NULL || !GDKW_IS_DISPLAY_WAYLAND (display))
    return;

  seat = _gdk_wayland_display_get_seat (display);

  if (time_ == GDK_CURRENT_TIME || !seat->grab.active ||
      seat->grab.time == GDK_CURRENT_TIME ||
      !WTIME_IS_LATER (seat->grab.time, time_))
    _gdk_wayland_seat_grab_clear (seat);

  grab = _gdk_display_get_last_pointer_grab (display);
  if (grab &&
      (time_ == GDK_CURRENT_TIME ||
       grab->time == GDK_CURRENT_TIME ||
       !WTIME_IS_LATER (grab->time, time_)))
    {
      gulong serial = _gdk_windowing_window_get_next_serial (display);

      grab->serial_end = serial;

      /* X11 finishes this from an async roundtrip callback.  Here there is no
       * server round trip, and the update is what emits the UNGRAB crossing
       * events and frees the grab, so gdk_pointer_is_grabbed () is FALSE as
       * soon as gdk_pointer_ungrab () returns. */
      _gdk_display_pointer_grab_update (display, serial);
    }
}

/* gdk_display_keyboard_ungrab () */
void
_gdk_wayland_display_keyboard_ungrab (GdkDisplay *display, guint32 time_)
{
  if (display == NULL || !GDKW_IS_DISPLAY_WAYLAND (display))
    return;

  if (time_ == GDK_CURRENT_TIME ||
      display->keyboard_grab.time == GDK_CURRENT_TIME ||
      !WTIME_IS_LATER (display->keyboard_grab.time, time_))
    _gdk_display_unset_has_keyboard_grab (display, FALSE);
}

/* gdk_keyboard_grab ().  The grab is purely client side: the keyboard
 * focus belongs to the compositor.  The key redirection itself is done by
 * emit_key () in gdkevents-wayland.c from display->keyboard_grab. */
GdkGrabStatus
_gdk_wayland_keyboard_grab (GdkWindow *window,
			    gboolean owner_events,
			    guint32 time_)
{
  GdkWindow *native;
  GdkDisplay *display;
  GDKWWindowImplWayland *impl;
  gulong serial;

  if (window == NULL || !GDK_IS_WINDOW (window))
    return GDK_GRAB_NOT_VIEWABLE;

  native = gdk_window_get_toplevel (window);

  /* same leniency as the X11 backend for windows it cannot proxy */
  impl = _gdk_wayland_window_get_impl (native);
  if (impl == NULL)
    return GDK_GRAB_SUCCESS;

  if (GDK_WINDOW_DESTROYED (native))
    return GDK_GRAB_ALREADY_GRABBED;

  if (!GDK_WINDOW_IS_MAPPED (native) || impl->wl_surface == NULL)
    return GDK_GRAB_NOT_VIEWABLE;

  display = gdk_drawable_get_display (GDK_DRAWABLE (native));
  serial = _gdk_windowing_window_get_next_serial (display);

  _gdk_display_set_has_keyboard_grab (display, window, native,
				      owner_events, serial, time_);

  return GDK_GRAB_SUCCESS;
}

/* Wayland clients cannot warp the pointer, and there is no beep protocol. */
void
_gdk_wayland_display_warp_pointer (GdkDisplay *display,
				   GdkScreen *screen,
				   gint x,
				   gint y)
{
}

void
_gdk_wayland_display_beep (GdkDisplay *display)
{
}

void
_gdk_wayland_windowing_window_process_updates_recurse (GdkWindow *window,
						       GdkRegion *expose_region)
{
  /* Expose dispatch is fully handled by the core: the backend hook only
   * needs to be a passthrough (the x11 backend does the same thing). */
  _gdk_window_process_updates_recurse (window, expose_region);
}

void
_gdk_wayland_windowing_before_process_all_updates (void)
{
}

void
_gdk_wayland_windowing_after_process_all_updates (void)
{
}

gint
_gdk_wayland_windowing_get_bits_for_depth (GdkDisplay *display, gint depth)
{
  return depth == 1 ? 1 : 32;
}

void
_gdk_wayland_windowing_window_destroy_foreign (GdkWindow *window)
{
}

void
_gdk_wayland_windowing_display_set_sm_client_id (GdkDisplay *display,
						 const gchar *sm_client_id)
{
}

void
_gdk_wayland_windowing_window_set_composited (GdkWindow *window,
					      gboolean composited)
{
}

char *
_gdk_wayland_windowing_get_startup_notify_id (GAppLaunchContext *context,
					      GAppInfo *info,
					      GList *files)
{
  return NULL;
}

void
_gdk_wayland_windowing_launch_failed (GAppLaunchContext *context,
				      const char *startup_notify_id)
{
}

void
_gdk_wayland_windowing_set_cairo_surface_size (cairo_surface_t *surface,
					       int width,
					       int height)
{
}

/* `_gdk_wayland_windowing_create_cairo_surface` lives in the window impl
 * TU (gdkwindowimpl-wayland.c): the cairo target for a window is the
 * shm-backed image surface, so it must be created where the impl lives. */