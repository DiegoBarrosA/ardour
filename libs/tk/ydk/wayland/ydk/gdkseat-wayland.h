/*
 * gdkseat-wayland.h
 *
 * ydk-wayland fork, M6 (input completeness): the one shared contract for
 * seat / input state, the function ownership split between the three M6
 * pieces, and the headless test hooks the demo drives.
 *
 * Layout of this header
 *
 *   1. PUBLIC section (no GDK_COMPILATION needed).  Only the test hooks and
 *      GDKWTestSeatInfo.  Depends on <ydk/gdk.h> alone, so the demo can
 *      include it as <ydk/gdkseat-wayland.h> once the integrator installs it
 *      (flake.nix postInstall).
 *   2. INTERNAL section (#ifdef GDK_COMPILATION).  The seat struct, the
 *      output info struct, static inline accessors, and prototypes of every
 *      function the three pieces add, each tagged with its owner.
 *
 * Include order inside the library: gdkprivate-wayland.h, then this header,
 * then (last of all) gdkalias.h, exactly like the other wayland sources.
 *
 * ---------------------------------------------------------------------------
 * OWNERSHIP (no file is touched by two pieces; shared files belong to the
 * integrator only: gdkbackend.c, x11/ydk/gdkwindowing-x11.h,
 * gdkprivate-wayland.h, wscript, flake.nix, scripts/, demo/)
 *
 *   piece "screen"   ydk/wayland/gdkscreen-wayland.c
 *   piece "events"   ydk/wayland/gdkevents-wayland.c
 *                    ydk/wayland/gdkkeys-wayland.c
 *   piece "pointer"  ydk/wayland/gdkdisplay-wayland.c
 *                    ydk/wayland/gdkwindowimpl-wayland.c
 *
 * No piece adds a .c file (that would need wscript, which is shared).
 * Pieces never edit this header; if a contract here looks wrong, the piece
 * implements the contract anyway and reports the problem in its summary.
 *
 * ---------------------------------------------------------------------------
 * CONVENTIONS EVERY PIECE RELIES ON (the spec has the reasoning)
 *
 *  - Seat state has exactly one home, GDKWSeat below, reached through
 *    _gdk_wayland_display_get_seat ().  The legacy fields
 *    GDKWDisplayWayland.px / py / pointer_focus / keyboard_focus are dead:
 *    nobody reads or writes them any more (the integrator deletes them).
 *  - Events handed to the GDK queue are ALWAYS heap events from
 *    gdk_event_new (), with a ref on event->any.window and
 *    gdk_event_set_screen () applied.  Never a stack GdkEvent.
 *  - The serial passed to _gdk_windowing_got_event () is
 *    _gdk_wayland_event_serial (display), i.e. GDKWDisplayWayland.serial.
 *    Only _gdk_wayland_windowing_window_get_next_serial () increments it
 *    (once per grab / ungrab request), never an input event.
 *  - hardware_keycode in GdkEventKey and in every GdkKeymap call is the
 *    evdev code from wl_keyboard.key PLUS 8, which is also the xkb keycode.
 *  - Event state is the state BEFORE the event (X11 semantics): a button
 *    release still carries that button's mask, a press does not.
 *  - Coordinates: window relative to the toplevel surface; x_root / y_root
 *    equal x / y (the toplevel sits at the root origin, matching
 *    get_root_coords ()).
 *  - Nothing in the M6 code paths may emit a g_critical (the demo makes
 *    CRITICAL fatal): validate instead of g_return_if_fail on inputs a
 *    caller can legitimately pass.
 * ---------------------------------------------------------------------------
 */

#ifndef __YDK_GDKSEAT_WAYLAND_H__
#define __YDK_GDKSEAT_WAYLAND_H__

#include <string.h>

#include <glib.h>
#include <glib-object.h>

#include <ydk/gdk.h>

G_BEGIN_DECLS

/* =========================================================================
 * 1. PUBLIC: headless test hooks
 *
 * Each hook pushes ONE synthetic protocol event through the REAL static
 * wl_keyboard / wl_pointer / xdg_toplevel listener function, so the whole
 * translation layer, the GDK event queue, _gdk_windowing_got_event (),
 * grab bookkeeping, key repeat timers and get_pointer state are exercised.
 * The proxies passed to the listeners are NULL (listeners must not touch
 * them) and the protocol serial is a private incrementing counter.
 *
 * Every hook returns TRUE when the event was delivered, FALSE when the
 * precondition failed (not a Wayland display, no wl_surface for @window yet,
 * no keymap for key hooks).  They never emit warnings.
 *
 * Event delivery to the application still happens from the main loop: after
 * calling hooks the demo runs g_main_context_iteration (NULL, FALSE) until
 * it returns FALSE, with its own gdk_event_handler_set () recorder in place.
 *
 * Owner: "events" (gdkevents-wayland.c) unless marked otherwise.
 * ========================================================================= */

/* wl_pointer axis ids and wl_pointer.axis_source values, mirrored here so
 * the demo needs no wayland headers */
enum
{
  GDKW_TEST_AXIS_VERTICAL = 0,
  GDKW_TEST_AXIS_HORIZONTAL = 1
};

enum
{
  GDKW_TEST_AXIS_SOURCE_WHEEL = 0,
  GDKW_TEST_AXIS_SOURCE_FINGER = 1,
  GDKW_TEST_AXIS_SOURCE_CONTINUOUS = 2,
  GDKW_TEST_AXIS_SOURCE_WHEEL_TILT = 3
};

/* Linux input-event-codes the demo passes to the button / key hooks */
enum
{
  GDKW_TEST_BTN_LEFT = 0x110,
  GDKW_TEST_BTN_RIGHT = 0x111,
  GDKW_TEST_BTN_MIDDLE = 0x112,
  GDKW_TEST_BTN_SIDE = 0x113,
  GDKW_TEST_BTN_EXTRA = 0x114,

  GDKW_TEST_KEY_ESC = 1,
  GDKW_TEST_KEY_ENTER = 28,
  GDKW_TEST_KEY_LEFTCTRL = 29,
  GDKW_TEST_KEY_A = 30,
  GDKW_TEST_KEY_S = 31,
  GDKW_TEST_KEY_LEFTSHIFT = 42,
  GDKW_TEST_KEY_LEFTALT = 56,
  GDKW_TEST_KEY_SPACE = 57
};

/* Snapshot of the seat for assertions; pointers are NOT referenced. */
typedef struct
{
  GdkWindow *pointer_focus;     /* wrapper window under the pointer, or NULL */
  GdkWindow *keyboard_focus;    /* wrapper window with keyboard focus, or NULL */
  gdouble    x, y;              /* last pointer position, surface coords */
  guint      button_mask;       /* GDK_BUTTON1_MASK ... GDK_BUTTON5_MASK bits */
  guint      modifier_mask;     /* keyboard modifier bits (no button bits) */
  guint      repeat_key;        /* evdev code currently auto-repeating, 0 none */
  guint      repeat_count;      /* repeats synthesised since the last press */
  guint      seat_version;      /* wl_seat version the pointer code assumes */
  gboolean   explicit_grab_active; /* backend record of a gdk_pointer_grab () */
} GDKWTestSeatInfo;

/* keyboard ---------------------------------------------------------------*/

/* Builds an xkb keymap (layout "us" through xkb_keymap_new_from_names, and if
 * that fails the built-in minimal keymap from gdkkeys-wayland.c), writes its
 * text plus the trailing NUL into a memfd and calls the real
 * wl_keyboard_keymap listener with WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1.  Safe to
 * call repeatedly (a second call is a "keymap change"). */
gboolean _gdk_wayland_test_keyboard_keymap      (GdkDisplay *display);
/* wl_keyboard.repeat_info */
gboolean _gdk_wayland_test_keyboard_repeat_info (GdkDisplay *display,
                                                 gint        rate,
                                                 gint        delay);
gboolean _gdk_wayland_test_keyboard_enter       (GdkDisplay *display,
                                                 GdkWindow  *window);
gboolean _gdk_wayland_test_keyboard_leave       (GdkDisplay *display,
                                                 GdkWindow  *window);
/* @evdev_key is a Linux KEY_* code (no +8); @time in ms */
gboolean _gdk_wayland_test_keyboard_key         (GdkDisplay *display,
                                                 guint32     time,
                                                 guint32     evdev_key,
                                                 gboolean    pressed);
/* Converts @gdk_mods (GDK_SHIFT_MASK, GDK_CONTROL_MASK, GDK_MOD1_MASK,
 * GDK_LOCK_MASK, ...) to xkb masks with the live keymap and calls the real
 * wl_keyboard_modifiers listener with them as "depressed". */
gboolean _gdk_wayland_test_keyboard_modifiers   (GdkDisplay     *display,
                                                 GdkModifierType gdk_mods);

/* pointer ----------------------------------------------------------------*/

gboolean _gdk_wayland_test_pointer_enter        (GdkDisplay *display,
                                                 GdkWindow  *window,
                                                 gdouble     x,
                                                 gdouble     y);
gboolean _gdk_wayland_test_pointer_leave        (GdkDisplay *display,
                                                 GdkWindow  *window);
gboolean _gdk_wayland_test_pointer_motion       (GdkDisplay *display,
                                                 guint32     time,
                                                 gdouble     x,
                                                 gdouble     y);
/* @evdev_button is a Linux BTN_* code */
gboolean _gdk_wayland_test_pointer_button       (GdkDisplay *display,
                                                 guint32     time,
                                                 guint32     evdev_button,
                                                 gboolean    pressed);
gboolean _gdk_wayland_test_pointer_axis_source  (GdkDisplay *display,
                                                 guint32     source);
gboolean _gdk_wayland_test_pointer_axis         (GdkDisplay *display,
                                                 guint32     time,
                                                 guint32     axis,
                                                 gdouble     value);
gboolean _gdk_wayland_test_pointer_axis_discrete(GdkDisplay *display,
                                                 guint32     axis,
                                                 gint32      discrete);
gboolean _gdk_wayland_test_pointer_axis_stop    (GdkDisplay *display,
                                                 guint32     time,
                                                 guint32     axis);
gboolean _gdk_wayland_test_pointer_frame        (GdkDisplay *display);

/* seat -------------------------------------------------------------------*/

/* Forces the wl_seat version the pointer code believes it is bound at
 * (headless compositors may expose no seat at all).  The demo calls this
 * with 5 so wl_pointer.frame grouping is exercised. */
gboolean _gdk_wayland_test_seat_set_version     (GdkDisplay *display,
                                                 guint       version);
gboolean _gdk_wayland_test_seat_info            (GdkDisplay       *display,
                                                 GDKWTestSeatInfo *info);

/* window ------------------------------------------------------------------*/

/* Calls the real xdg_toplevel close listener for @window's impl.
 * Owner: "pointer" (gdkwindowimpl-wayland.c). */
gboolean _gdk_wayland_test_toplevel_close       (GdkWindow *window);

/* ---- M6b: toplevel hint observability ---------------------------------
 * xdg_toplevel state bits for _gdk_wayland_test_toplevel_configure () */
enum
{
  GDKW_TEST_STATE_MAXIMIZED = 1 << 0,
  GDKW_TEST_STATE_FULLSCREEN = 1 << 1,
  GDKW_TEST_STATE_RESIZING = 1 << 2,
  GDKW_TEST_STATE_ACTIVATED = 1 << 3
};

/* What the backend last sent to the compositor for one toplevel.  Pointers
 * are NOT owned (transient_for is a borrowed GdkWindow). */
typedef struct
{
  gboolean   has_toplevel;
  gboolean   has_title;
  gchar      title[256];
  gboolean   has_app_id;
  gchar      app_id[128];
  gboolean   has_role;
  gchar      role[128];
  GdkWindow *transient_for;
  gint       min_width, min_height, max_width, max_height;
  gboolean   maximized, fullscreen;
  guint      minimize_requests;
  guint      move_requests, resize_requests;
  guint32    drag_serial;
  gboolean   drag_sent;
  guint      last_resize_edge;
  guint      configure_states;   /* GDKW_TEST_STATE_* of the last configure */
} GDKWTestToplevelInfo;

/* Owner: hints (gdkwindow-hints-wayland.c).  FALSE for a non Wayland
 * window or one without a toplevel. */
gboolean _gdk_wayland_test_toplevel_info        (GdkWindow            *window,
                                                 GDKWTestToplevelInfo *info);
/* Owner: gdkwindowimpl-wayland.c.  Builds a wl_array of xdg states from
 * @states (GDKW_TEST_STATE_*) and calls the real xdg_toplevel configure
 * listener.  @width / @height <= 0 keep the current size. */
gboolean _gdk_wayland_test_toplevel_configure   (GdkWindow *window,
                                                 gint       width,
                                                 gint       height,
                                                 guint      states);

/* Owner: gdkevents-wayland.c.  Calls the real wl_keyboard.modifiers
 * listener with @depressed and @locked (GDK masks) converted through the live
 * keymap and @group as the effective layout. */
gboolean _gdk_wayland_test_keyboard_state       (GdkDisplay     *display,
                                                 GdkModifierType depressed,
                                                 GdkModifierType locked,
                                                 guint           group);
/* Like _gdk_wayland_test_keyboard_keymap () but with caller supplied keymap
 * text (XKB_V1), for multi layout tests. */
gboolean _gdk_wayland_test_keyboard_keymap_text (GdkDisplay  *display,
                                                 const gchar *text);

G_END_DECLS

/* =========================================================================
 * 2. INTERNAL
 * ========================================================================= */

#ifdef GDK_COMPILATION

#include <wayland-client.h>
#include <ydk/gdkinternals.h>

#include "gdkprivate-wayland.h"

G_BEGIN_DECLS

typedef struct _GDKWSeat       GDKWSeat;
typedef struct _GDKWOutputInfo GDKWOutputInfo;

/* One pending wl_pointer v5 "frame" worth of axis events.  Index 0 is
 * GDKW_TEST_AXIS_VERTICAL, index 1 GDKW_TEST_AXIS_HORIZONTAL. */
typedef struct
{
  gboolean has_source;
  guint32  source;                /* GDKW_TEST_AXIS_SOURCE_* */
  guint32  time;                  /* time of the most recent axis event */
  struct
  {
    gboolean has_value;
    gdouble  value;               /* wl_fixed_to_double of axis */
    gboolean has_discrete;
    gint32   discrete;            /* axis_discrete steps */
    gboolean stop;                /* axis_stop seen */
  } axis[2];
} GDKWAxisFrame;

/* Pointer grab as seen by _gdk_windowing_pointer_grab ().  The core keeps
 * the authoritative GdkPointerGrabInfo list in GdkDisplay.pointer_grabs;
 * this is the backend's own record (cursor for M7, bookkeeping for
 * ungrab).  window and cursor are owned references. */
typedef struct
{
  gboolean      active;
  GdkWindow    *window;           /* ref held */
  GdkWindow    *native;           /* toplevel wrapper, no ref */
  gboolean      owner_events;
  GdkEventMask  event_mask;       /* native mask the core computed */
  GdkWindow    *confine_to;       /* recorded only; Wayland cannot confine */
  GdkCursor    *cursor;           /* ref held, applied in M7 */
  guint32       time;
  gulong        serial;           /* gdk serial of the grab request */
} GDKWPointerGrab;

struct _GDKWSeat
{
  GDKWDisplayWayland *display;    /* back pointer, no ref */

  /* ---- wl_seat -------------------------------------------------------
   * The wl_seat / wl_pointer / wl_keyboard proxies stay in
   * GDKWDisplayWayland (wl_seat, wl_pointer, wl_keyboard). */
  guint32  seat_version;          /* bound version, 1..5 (test hook may force) */
  guint32  capabilities;          /* last WL_SEAT_CAPABILITY_* mask */

  /* ---- serials and time ---------------------------------------------- */
  guint32  last_serial;           /* newest protocol serial seen on any input event */
  guint32  pointer_enter_serial;  /* wl_pointer.enter, needed by set_cursor (M7) */
  guint32  button_serial;         /* last wl_pointer.button PRESS, needed by popups (M9) / DnD (M8) */
  guint32  keyboard_enter_serial;
  guint32  key_serial;            /* last wl_keyboard.key PRESS (a release serial is no valid grab serial) */
  guint32  last_time;             /* newest ms timestamp from any seat event */

  /* ---- core pointer device (GdkEvent.device) --------------------------
   * Same object as GdkDisplay.core_pointer; a ref is held here. */
  GdkDevice *core_pointer;

  /* ---- pointer --------------------------------------------------------
   * pointer_focus is the impl whose wl_surface currently has pointer focus
   * (NULL when the pointer is outside all our surfaces).  last_pointer_impl
   * survives wl_pointer.leave so get_pointer () can still answer.  px / py
   * are in last_pointer_impl's surface coordinates. */
  GDKWWindowImplWayland *pointer_focus;
  /* The window GDK believes the pointer is in.  It is pointer_focus, except
   * while a button is down: the compositor then keeps focus on the surface of
   * the press, and the pointer may be over a popup of the same family. */
  GDKWWindowImplWayland *pointer_target;
  GDKWWindowImplWayland *last_pointer_impl;
  gdouble  px, py;
  guint    button_mask;           /* GDK_BUTTONn_MASK bits, n = 1..5 */
  guint32  buttons_down;          /* bit (n - 1) set while GDK button n is down, n = 1..32 */
  guint    held_settle_source;    /* idle that forgets the held buttons when no surface got the focus back, 0 none */
  GDKWAxisFrame axis_frame;       /* pending v5 frame, cleared by wl_pointer.frame */
  gdouble  scroll_accum[2];       /* continuous-source remainder in steps, per axis */

  /* ---- keyboard -------------------------------------------------------- */
  GDKWWindowImplWayland *keyboard_focus;
  GdkModifierType modifier_mask;  /* keyboard modifiers only, from xkb effective mods */
  guint32  mods_depressed, mods_latched, mods_locked, mods_group;
  guint    keymap_generation;     /* ++ on every wl_keyboard.keymap */

  /* ---- key repeat ------------------------------------------------------
   * wl_keyboard.repeat_info (v4) or the defaults 25 / 400 before it.  The
   * timer is a GSource (g_timeout_source_new) attached to the default main
   * context, the same context as the display event source. */
  struct
  {
    gint32   rate;                /* repeats per second, 0 = disabled */
    gint32   delay;               /* ms before the first repeat */
    guint32  key;                 /* evdev code repeating, 0 = none */
    guint32  press_time;          /* compositor ms timestamp of the press */
    gint64   start_us;            /* g_get_monotonic_time () at the press */
    guint    source_id;           /* GLib source id, 0 = no timer armed */
    guint    count;               /* repeats synthesised since the press */
    guint    generation;          /* keymap_generation at the press */
  } repeat;

  /* ---- explicit pointer grab (record, see GDKWPointerGrab) ------------- */
  GDKWPointerGrab grab;
};

/* Output geometry, filled by the wl_output listeners in
 * gdkdisplay-wayland.c ("pointer" piece) and read by gdkscreen-wayland.c
 * ("screen" piece).  Only the first wl_output is tracked. */
struct _GDKWOutputInfo
{
  gboolean have_geometry;         /* wl_output.geometry seen */
  gboolean have_mode;             /* wl_output.mode with CURRENT seen */
  gint     x, y;                  /* position in compositor space */
  gint     width, height;         /* current mode, pixels */
  gint     refresh;               /* mHz, 0 unknown */
  gint     mm_width, mm_height;   /* physical size, 0 unknown */
  gint     subpixel;              /* WL_OUTPUT_SUBPIXEL_* */
  gint     transform;             /* WL_OUTPUT_TRANSFORM_* */
  gint     scale;                 /* 1 until wl_output v2 lands (M10) */
  gchar   *make;                  /* owned, may be NULL */
  gchar   *model;                 /* owned, may be NULL */
};

/* ---------------------------------------------------------------------------
 * Accessors.  All static inline so a piece compiles and links without
 * depending on another piece's object file.
 * ------------------------------------------------------------------------ */

#define GDKW_SEAT_DATA_KEY   "ydk-wayland-seat"
#define GDKW_OUTPUT_DATA_KEY "ydk-wayland-output"

/* Number of repeats per second and delay used until repeat_info arrives */
#define GDKW_DEFAULT_REPEAT_RATE  25
#define GDKW_DEFAULT_REPEAT_DELAY 400

/* Wheel sources report 10 units per click; continuous sources are divided
 * by this to get "steps" */
#define GDKW_SCROLL_UNITS_PER_STEP 10.0

static inline void
_gdk_wayland_seat_repeat_cancel (GDKWSeat *seat)
{
  if (seat->repeat.source_id != 0)
    {
      g_source_remove (seat->repeat.source_id);
      seat->repeat.source_id = 0;
    }
  seat->repeat.key = 0;
  seat->repeat.count = 0;
}

static inline void
_gdk_wayland_seat_grab_clear (GDKWSeat *seat)
{
  if (seat->grab.window)
    g_object_unref (seat->grab.window);
  if (seat->grab.cursor)
    gdk_cursor_unref (seat->grab.cursor);
  memset (&seat->grab, 0, sizeof seat->grab);
}

/* GDestroyNotify for the qdata slot; also usable directly */
static inline void
_gdk_wayland_seat_free (gpointer data)
{
  GDKWSeat *seat = data;

  if (seat == NULL)
    return;

  _gdk_wayland_seat_repeat_cancel (seat);
  _gdk_wayland_seat_grab_clear (seat);
  if (seat->core_pointer)
    g_object_unref (seat->core_pointer);
  g_free (seat);
}

/* The seat state of @display, created zeroed (with the repeat defaults and
 * seat_version 1) on first use.  Never returns NULL for a Wayland display.
 * Freed with the display object. */
static inline GDKWSeat *
_gdk_wayland_display_get_seat (GdkDisplay *display)
{
  GDKWSeat *seat = g_object_get_data (G_OBJECT (display), GDKW_SEAT_DATA_KEY);

  if (G_UNLIKELY (seat == NULL))
    {
      seat = g_new0 (GDKWSeat, 1);
      seat->display = (GDKWDisplayWayland *) display;
      seat->seat_version = 1;
      seat->repeat.rate = GDKW_DEFAULT_REPEAT_RATE;
      seat->repeat.delay = GDKW_DEFAULT_REPEAT_DELAY;
      g_object_set_data_full (G_OBJECT (display), GDKW_SEAT_DATA_KEY, seat,
                              _gdk_wayland_seat_free);
    }

  return seat;
}

/* Output geometry of @display, created zeroed (scale 1) on first use. */
static inline void
_gdk_wayland_output_info_free (gpointer data)
{
  GDKWOutputInfo *info = data;

  if (info == NULL)
    return;
  g_free (info->make);
  g_free (info->model);
  g_free (info);
}

static inline GDKWOutputInfo *
_gdk_wayland_display_get_output_info (GdkDisplay *display)
{
  GDKWOutputInfo *info = g_object_get_data (G_OBJECT (display),
                                            GDKW_OUTPUT_DATA_KEY);

  if (G_UNLIKELY (info == NULL))
    {
      info = g_new0 (GDKWOutputInfo, 1);
      info->scale = 1;
      g_object_set_data_full (G_OBJECT (display), GDKW_OUTPUT_DATA_KEY, info,
                              _gdk_wayland_output_info_free);
    }

  return info;
}

/* The gdk serial every input event is tagged with for
 * _gdk_windowing_got_event ().  See CONVENTIONS above. */
static inline gulong
_gdk_wayland_event_serial (GdkDisplay *display)
{
  return ((GDKWDisplayWayland *) display)->serial;
}

/* Wayland impl behind a GdkWindow wrapper, or NULL (not a Wayland window or
 * already destroyed). */
static inline GDKWWindowImplWayland *
_gdk_wayland_window_get_impl (GdkWindow *window)
{
  GdkWindowObject *wobj;

  if (window == NULL || !GDK_IS_WINDOW (window))
    return NULL;
  wobj = (GdkWindowObject *) window;
  if (wobj->impl == NULL || !GDKW_IS_WINDOW_IMPL_WAYLAND (wobj->impl))
    return NULL;
  return GDKW_WINDOW_IMPL_WAYLAND (wobj->impl);
}

/* Current "state" field for pointer events: keyboard modifiers | buttons. */
static inline guint
_gdk_wayland_seat_get_state (const GDKWSeat *seat)
{
  return (guint) seat->modifier_mask | seat->button_mask;
}

/* Called when an impl goes away (window destroy, display teardown): drops
 * every raw pointer to it, stops key repeat aimed at it and clears an
 * explicit grab that belonged to its toplevel.  Idempotent.  Called by the
 * "pointer" piece from _gdk_wayland_window_impl_teardown () and the destroy
 * vfunc. */
static inline void
_gdk_wayland_seat_forget_impl (GDKWSeat *seat, GDKWWindowImplWayland *impl)
{
  if (seat->pointer_focus == impl)
    {
      /* With no focus the button events that finish the gesture are
       * ignored, so a button held on the dead window would stay pressed
       * in get_pointer () until the next wl_pointer.enter. */
      seat->pointer_focus = NULL;
      seat->pointer_target = NULL;
      seat->button_mask = 0;
      seat->buttons_down = 0;
    }
  if (seat->pointer_target == impl)
    seat->pointer_target = NULL;
  if (seat->last_pointer_impl == impl)
    seat->last_pointer_impl = NULL;
  if (seat->keyboard_focus == impl)
    {
      seat->keyboard_focus = NULL;
      _gdk_wayland_seat_repeat_cancel (seat);
    }
  if (seat->grab.active && seat->grab.native == impl->wrapper)
    _gdk_wayland_seat_grab_clear (seat);
}

/* ---------------------------------------------------------------------------
 * Prototypes and owners
 * ------------------------------------------------------------------------ */

/* ---- owner "events": gdkevents-wayland.c ---------------------------------
 *
 * Replaces _gdk_wayland_seat_init (): binds wl_seat at
 * MIN (@version, 5) and adds the seat listener (capabilities AND name).
 * The caller is gdkdisplay-wayland.c (registry global), which passes the
 * advertised version.  _gdk_wayland_seat_init () is deleted; its stale
 * declaration in gdkprivate-wayland.h is removed by the integrator. */
void  _gdk_wayland_seat_bind (GDKWDisplayWayland *display,
                              struct wl_registry *registry,
                              uint32_t            name,
                              uint32_t            version);

/* Backs the dispatcher for gdk_events_pending () (tier 2, see the spec):
 * TRUE if the GDK queue holds a dispatchable event or libwayland has
 * undispatched protocol events. */
gboolean _gdk_wayland_events_pending (GdkDisplay *display);

/* ---- owner "events": gdkkeys-wayland.c ---------------------------------- */

/* GDK modifier bits to an xkb_mod_mask_t for @keymap (0 when it has no
 * keymap yet).  Used by the modifiers test hook. */
guint    _gdk_wayland_keymap_xkb_mods_from_gdk (GdkKeymap      *keymap,
                                                GdkModifierType mods);
/* xkb_keymap_key_repeats () for @hardware_keycode (xkb keycode, evdev + 8) */
gboolean _gdk_wayland_keymap_key_repeats       (GdkKeymap *keymap,
                                                guint      hardware_keycode);
/* Keymap text in xkb text format v1, g_malloc'ed, NUL terminated, length
 * (without the NUL) in *@length.  "us" through the rule files when the
 * xkb data is installed, else a small built-in keymap that covers at least
 * Esc, Enter, Space, A, S, Left Ctrl, Left Shift, Left Alt, Caps Lock.
 * Used by the keymap test hook. */
gchar   *_gdk_wayland_keymap_default_text      (gsize *length);

/* ---- owner "pointer": gdkdisplay-wayland.c -------------------------------
 *
 * The three windowing-contract functions already declared in
 * gdkprivate-wayland.h get real bodies:
 *   _gdk_wayland_windowing_get_pointer ()
 *   _gdk_wayland_windowing_window_at_pointer ()
 *   _gdk_wayland_windowing_pointer_grab ()
 * and these new display level functions (the integrator dispatches the
 * public name to the _gdk_wayland_ one in gdkbackend.c and renames the X11
 * one in gdkwindowing-x11.h):
 *
 *   gdk_display_pointer_ungrab   -> _gdk_wayland_display_pointer_ungrab
 *   gdk_display_keyboard_ungrab  -> _gdk_wayland_display_keyboard_ungrab
 *   gdk_keyboard_grab            -> _gdk_wayland_keyboard_grab
 *   gdk_display_warp_pointer     -> _gdk_wayland_display_warp_pointer
 *   gdk_display_beep             -> _gdk_wayland_display_beep
 */
void          _gdk_wayland_display_pointer_ungrab  (GdkDisplay *display,
                                                    guint32     time_);
void          _gdk_wayland_display_keyboard_ungrab (GdkDisplay *display,
                                                    guint32     time_);
GdkGrabStatus _gdk_wayland_keyboard_grab           (GdkWindow *window,
                                                    gboolean    owner_events,
                                                    guint32     time_);
void          _gdk_wayland_display_warp_pointer    (GdkDisplay *display,
                                                    GdkScreen  *screen,
                                                    gint        x,
                                                    gint        y);
void          _gdk_wayland_display_beep            (GdkDisplay *display);

/* ---- owner "pointer": gdkwindowimpl-wayland.c ----------------------------
 * No new non-static function beyond _gdk_wayland_test_toplevel_close ()
 * (declared in the public section).  The existing impl vfuncs gain real
 * bodies: xdg_toplevel_handle_close, get_pointer, hide / withdraw / destroy
 * (grab clean-up), and _gdk_wayland_window_impl_teardown () calls
 * _gdk_wayland_seat_forget_impl (). */

/* ---- owner "screen": gdkscreen-wayland.c ---------------------------------
 *
 * Every one of the 15 X11-bound public screen functions that is not yet
 * dispatched.  Public name on the left, Wayland implementation on the right;
 * the integrator adds the dispatcher in gdkbackend.c and the IA__ rename in
 * gdkwindowing-x11.h for each.
 *
 *   gdk_screen_get_width_mm              _gdk_wayland_screen_get_width_mm
 *   gdk_screen_get_height_mm             _gdk_wayland_screen_get_height_mm
 *   gdk_screen_get_number                _gdk_wayland_screen_get_number
 *   gdk_screen_get_n_monitors            _gdk_wayland_screen_get_n_monitors
 *   gdk_screen_get_primary_monitor       _gdk_wayland_screen_get_primary_monitor
 *   gdk_screen_get_monitor_geometry      _gdk_wayland_screen_get_monitor_geometry
 *   gdk_screen_get_monitor_width_mm      _gdk_wayland_screen_get_monitor_width_mm
 *   gdk_screen_get_monitor_height_mm     _gdk_wayland_screen_get_monitor_height_mm
 *   gdk_screen_get_monitor_plug_name     _gdk_wayland_screen_get_monitor_plug_name
 *   gdk_screen_is_composited             _gdk_wayland_screen_is_composited
 *   gdk_screen_make_display_name         _gdk_wayland_screen_make_display_name
 *   gdk_screen_get_active_window         _gdk_wayland_screen_get_active_window
 *   gdk_screen_get_window_stack          _gdk_wayland_screen_get_window_stack
 *   gdk_screen_broadcast_client_message  _gdk_wayland_screen_broadcast_client_message
 *   gdk_screen_get_setting               _gdk_wayland_screen_get_setting
 */
gint       _gdk_wayland_screen_get_width_mm             (GdkScreen *screen);
gint       _gdk_wayland_screen_get_height_mm            (GdkScreen *screen);
gint       _gdk_wayland_screen_get_number               (GdkScreen *screen);
gint       _gdk_wayland_screen_get_n_monitors           (GdkScreen *screen);
gint       _gdk_wayland_screen_get_primary_monitor      (GdkScreen *screen);
void       _gdk_wayland_screen_get_monitor_geometry     (GdkScreen    *screen,
                                                         gint          monitor_num,
                                                         GdkRectangle *dest);
gint       _gdk_wayland_screen_get_monitor_width_mm     (GdkScreen *screen,
                                                         gint       monitor_num);
gint       _gdk_wayland_screen_get_monitor_height_mm    (GdkScreen *screen,
                                                         gint       monitor_num);
gchar     *_gdk_wayland_screen_get_monitor_plug_name    (GdkScreen *screen,
                                                         gint       monitor_num);
gboolean   _gdk_wayland_screen_is_composited            (GdkScreen *screen);
gchar     *_gdk_wayland_screen_make_display_name        (GdkScreen *screen);
GdkWindow *_gdk_wayland_screen_get_active_window        (GdkScreen *screen);
GList     *_gdk_wayland_screen_get_window_stack         (GdkScreen *screen);
void       _gdk_wayland_screen_broadcast_client_message (GdkScreen *screen,
                                                         GdkEvent  *event);
gboolean   _gdk_wayland_screen_get_setting              (GdkScreen   *screen,
                                                         const gchar *name,
                                                         GValue      *value);

G_END_DECLS

#endif /* GDK_COMPILATION */

#endif /* __YDK_GDKSEAT_WAYLAND_H__ */
