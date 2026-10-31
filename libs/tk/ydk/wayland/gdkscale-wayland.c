/*
 * gdkscale-wayland.c
 *
 * ydk-wayland fork, M10 piece "scale": wl_output list and listeners, the
 * wl_surface enter / leave / preferred_buffer_scale listener, the per surface
 * scale state, wp_fractional_scale_v1 and wp_viewport objects, the screen and
 * monitor answers in logical units, the cursor scale answer, and the one
 * function that sends set_buffer_scale and the viewport destination.
 *
 * The contract is ydk/gdkm10-wayland.h (decisions D10 to D13, section 4).
 *
 * Ownership rules of this file:
 *  - Only _gdk_wayland_scale_send_state () calls wl_surface_set_buffer_scale
 *    and wp_viewport_* requests.
 *  - A fake output (test hook) has no wl_output proxy.  The surface listener
 *    finds an output by pointer comparison only: a real output by its proxy,
 *    a fake output by the address of its own record.  A fake proxy pointer is
 *    never dereferenced.
 *  - Units: everything exported by the screen and monitor functions is
 *    logical.  The scale is a scale120 (120 is 1.0).
 */

#include "config.h"

#include <string.h>
#include <math.h>

#include <wayland-client.h>
#include <cairo.h>

#include <ydk/gdk.h>
#include <ydk/gdkinternals.h>

#include "gdkprivate-wayland.h"
#include "gdkseat-wayland.h"
#include "gdkm9-wayland.h"
#include "gdkm10-wayland.h"
#include "fractional-scale-v1-client.h"
#include "viewporter-client.h"
#include "xdg-output-unstable-v1-client.h"

#define GDKW_SCALE_DATA_KEY   "ydk-wayland-m10-scale"
#define GDKW_FAKE_NAME_BASE   0x80000000u
#define GDKW_FALLBACK_WIDTH   1024
#define GDKW_FALLBACK_HEIGHT  768

typedef struct _GDKWScaleDisplay GDKWScaleDisplay;

typedef struct
{
  GDKWScaleDisplay *st;
  guint             id;               /* 1 based, bind order */
  guint32           global_name;
  guint32           version;          /* bound version */
  struct wl_output *proxy;            /* NULL for a fake output */
  gboolean          fake;

  gint     x, y;
  gint     mode_w, mode_h;            /* device pixels */
  gint     refresh;
  gint     mm_w, mm_h;
  gint     subpixel;
  gint     transform;
  gint     scale;                     /* applied, at least 1 */
  gint     pending_scale;
  gboolean has_pending_scale;
  gboolean have_geometry;
  gboolean have_mode;
  gboolean done_seen;
  gchar   *make;
  gchar   *model;
  gchar    name[32];

  gint     logical_w, logical_h;      /* xdg-output size, else mode / scale, axes swapped if odd */

  /* zxdg_output_v1: the compositor's own logical size.  wl_output.scale is an
   * integer (a fractional output reports the next integer up), so
   * mode / scale is wrong for a scale like 1.25.  The size already has the
   * transform applied. */
  struct zxdg_output_v1 *xdg;
  guint    xdg_version;               /* version of the xdg_output object, 3 and up have no done */
  gint     pending_xdg_w, pending_xdg_h;
  gint     xdg_w, xdg_h;              /* 0 until the first xdg_output.done */

  /* The position in the layout.  wl_output.geometry has it for compositors
   * that send it, but wlroots (sway) sends 0,0 there and puts the real
   * position in xdg_output.logical_position.  So every output sat at 0,0. */
  gint     pending_xdg_x, pending_xdg_y;
  gboolean has_pending_xdg;           /* a logical_size is pending */
  gboolean has_pending_xdg_pos;       /* a logical_position is pending */
  gint     xdg_x, xdg_y;
  gboolean xdg_pos_valid;
} GDKWOutput;

struct _GDKWSurfaceScale
{
  GDKWScaleDisplay      *st;
  GDKWWindowImplWayland *impl;
  struct wl_surface     *surface;
  struct wp_fractional_scale_v1 *fractional;
  struct wp_viewport    *viewport;

  GList   *entered;                   /* GDKWOutput *, enter order */
  guint    preferred_int;             /* 0 none */
  guint    preferred_fractional;      /* 0 none */

  guint               scale120;       /* effective */
  GDKWTestScaleSource source;
  guint               scale_changes;

  gint     buffer_scale_sent;         /* 1 at start */
  guint    buffer_scale_requests;
  gint     dest_w, dest_h;            /* last sent, -1 -1 unset */
  guint    viewport_requests;
};

struct _GDKWScaleDisplay
{
  GDKWDisplayWayland *display;
  GList              *outputs;        /* GDKWOutput *, bind order */
  GList              *surfaces;       /* GDKWSurfaceScale * */
  guint               next_id;

  struct wp_viewporter *viewporter;
  guint32               viewporter_name;
  struct wp_fractional_scale_manager_v1 *manager;
  guint32               manager_name;
  struct zxdg_output_manager_v1 *xdg_manager;
  guint32               xdg_manager_name;
  gboolean              force_fractional;
  guint                 last_toplevel_scale120; /* scale of the last toplevel that learned it from the compositor, 0 none */
  gchar                *emitted_sig;     /* what the screen signals last reported */
  gboolean              finalized;

  gboolean                cursor_seen;
  GDKWTestCursorScaleInfo cursor;
};

/* ------------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------------ */

static void surface_update (GDKWSurfaceScale *ss);
static void update_all_surfaces (GDKWScaleDisplay *st);
static void output_apply (GDKWOutput *out);

static GDKWDisplayWayland *
display_checked (GdkDisplay *display)
{
  if (display == NULL || !GDKW_IS_DISPLAY_WAYLAND (display))
    return NULL;
  return GDKW_DISPLAY_WAYLAND (display);
}

static void
scale_state_free (gpointer data)
{
  GDKWScaleDisplay *st = data;
  GList *l;

  if (st == NULL)
    return;

  /* Memory only: the proxies are destroyed by display_finalize () while the
   * connection is still alive. */
  for (l = st->outputs; l; l = l->next)
    {
      GDKWOutput *out = l->data;

      g_free (out->make);
      g_free (out->model);
      g_free (out);
    }
  g_list_free (st->outputs);
  g_free (st->emitted_sig);

  for (l = st->surfaces; l; l = l->next)
    {
      GDKWSurfaceScale *ss = l->data;

      g_list_free (ss->entered);
      g_free (ss);
    }
  g_list_free (st->surfaces);
  g_free (st);
}

static GDKWScaleDisplay *
scale_state_get (GDKWDisplayWayland *display, gboolean create)
{
  GDKWScaleDisplay *st;

  if (display == NULL)
    return NULL;

  st = g_object_get_data (G_OBJECT (display), GDKW_SCALE_DATA_KEY);
  if (st == NULL && create)
    {
      st = g_new0 (GDKWScaleDisplay, 1);
      st->display = display;
      st->next_id = 1;
      g_object_set_data_full (G_OBJECT (display), GDKW_SCALE_DATA_KEY, st,
                              scale_state_free);
    }
  return st;
}

static GDKWDisplayWayland *
impl_display (GDKWWindowImplWayland *impl)
{
  GdkDisplay *display;

  if (impl == NULL || impl->screen == NULL ||
      !GDKW_IS_SCREEN_WAYLAND (impl->screen))
    return NULL;
  display = GDKW_SCREEN_WAYLAND (impl->screen)->display;
  return display_checked (display);
}

static GDKWWindowImplWayland *
impl_from_window (GdkWindow *window)
{
  GdkWindowObject *private;

  if (window == NULL || !GDK_IS_WINDOW (window))
    return NULL;
  private = (GdkWindowObject *) window;
  if (private->destroyed || private->impl == NULL)
    return NULL;
  if (private->window_type != GDK_WINDOW_TOPLEVEL &&
      private->window_type != GDK_WINDOW_TEMP)
    return NULL;
  if (!GDKW_IS_WINDOW_IMPL_WAYLAND (private->impl))
    return NULL;
  return GDKW_WINDOW_IMPL_WAYLAND (private->impl);
}

static inline gint
output_scale_of (const GDKWOutput *out)
{
  return out->scale > 0 ? out->scale : 1;
}

/* The pointer the wl_surface listener sees for an output. */
static inline struct wl_output *
output_key (GDKWOutput *out)
{
  return out->proxy != NULL ? out->proxy : (struct wl_output *) out;
}

static GDKWOutput *
output_by_key (GDKWScaleDisplay *st, struct wl_output *key)
{
  GList *l;

  if (key == NULL)
    return NULL;
  for (l = st->outputs; l; l = l->next)
    if (output_key (l->data) == key)
      return l->data;
  return NULL;
}

static GDKWOutput *
output_by_id (GDKWScaleDisplay *st, guint id)
{
  GList *l;

  for (l = st->outputs; l; l = l->next)
    if (((GDKWOutput *) l->data)->id == id)
      return l->data;
  return NULL;
}

static GDKWOutput *
output_by_name (GDKWScaleDisplay *st, guint32 name)
{
  GList *l;

  for (l = st->outputs; l; l = l->next)
    if (((GDKWOutput *) l->data)->global_name == name)
      return l->data;
  return NULL;
}

/* Largest integer scale of all outputs, 1 if none (the HINT, F4). */
static gint
max_output_scale (GDKWScaleDisplay *st)
{
  GList *l;
  gint best = 1;

  if (st == NULL)
    return 1;
  for (l = st->outputs; l; l = l->next)
    best = MAX (best, output_scale_of (l->data));
  return best;
}

static guint
clamp_scale120 (guint64 s)
{
  if (s < GDKW_SCALE_MIN)
    return GDKW_SCALE_MIN;
  if (s > GDKW_SCALE_MAX)
    return GDKW_SCALE_MAX;
  return (guint) s;
}

static inline gint
px_to_mm (gint px)
{
  return MAX (1, (gint) floor (px * 25.4 / 96.0 + 0.5));
}

/* ------------------------------------------------------------------------
 * Scale evaluation (D10)
 * ------------------------------------------------------------------------ */

static gboolean
fractional_active (GDKWScaleDisplay *st)
{
  return (st->manager != NULL && st->viewporter != NULL) || st->force_fractional;
}

static guint
surface_eval (GDKWSurfaceScale *ss, GDKWTestScaleSource *source)
{
  GDKWScaleDisplay *st = ss->st;
  guint s;

  if (fractional_active (st) && ss->preferred_fractional > 0)
    {
      s = clamp_scale120 (ss->preferred_fractional);
      /* Without wp_viewporter a fractional size cannot be shown without a
       * protocol error: use the next integer (documented blur gap, D11). */
      if (s % GDKW_SCALE_DENOM != 0 && st->viewporter == NULL)
        s = clamp_scale120 (((s + GDKW_SCALE_DENOM - 1) / GDKW_SCALE_DENOM) *
                            (guint64) GDKW_SCALE_DENOM);
      *source = GDKW_TEST_SCALE_FRACTIONAL;
      return s;
    }

  if (ss->preferred_int > 0)
    {
      *source = GDKW_TEST_SCALE_PREFERRED_INT;
      return clamp_scale120 ((guint64) ss->preferred_int * GDKW_SCALE_DENOM);
    }

  if (ss->entered != NULL)
    {
      GList *l;
      gint best = 1;

      for (l = ss->entered; l; l = l->next)
        best = MAX (best, output_scale_of (l->data));
      *source = GDKW_TEST_SCALE_OUTPUT;
      return clamp_scale120 ((guint64) best * GDKW_SCALE_DENOM);
    }

  *source = GDKW_TEST_SCALE_HINT;
  return clamp_scale120 ((guint64) max_output_scale (st) * GDKW_SCALE_DENOM);
}

static void
surface_update (GDKWSurfaceScale *ss)
{
  GDKWTestScaleSource source;
  guint s = surface_eval (ss, &source);

  ss->source = source;
  /* what the next toplevel starts with (a guess better than the hint) */
  if (source != GDKW_TEST_SCALE_HINT && ss->impl != NULL && ss->impl->popup == NULL)
    ss->st->last_toplevel_scale120 = s;
  if (s == ss->scale120)
    return;

  ss->scale120 = s;
  ss->scale_changes++;
  if (ss->impl != NULL)
    _gdk_wayland_frame_set_scale (ss->impl, s);
}

static void
update_all_surfaces (GDKWScaleDisplay *st)
{
  GList *copy = g_list_copy (st->surfaces);
  GList *l;

  for (l = copy; l; l = l->next)
    if (g_list_find (st->surfaces, l->data) != NULL)
      surface_update (l->data);
  g_list_free (copy);
}

/* ------------------------------------------------------------------------
 * wl_surface, wp_fractional_scale_v1 listeners
 * ------------------------------------------------------------------------ */

static void
surface_enter (void *data, struct wl_surface *surface, struct wl_output *output)
{
  GDKWSurfaceScale *ss = data;
  GDKWOutput *out = output_by_key (ss->st, output);

  if (out == NULL || g_list_find (ss->entered, out) != NULL)
    return;
  ss->entered = g_list_append (ss->entered, out);
  surface_update (ss);
}

static void
surface_leave (void *data, struct wl_surface *surface, struct wl_output *output)
{
  GDKWSurfaceScale *ss = data;
  GDKWOutput *out = output_by_key (ss->st, output);

  if (out == NULL || g_list_find (ss->entered, out) == NULL)
    return;
  ss->entered = g_list_remove (ss->entered, out);
  surface_update (ss);
}

static void
surface_preferred_buffer_scale (void *data, struct wl_surface *surface,
                                int32_t factor)
{
  GDKWSurfaceScale *ss = data;

  ss->preferred_int = factor > 0 ? (guint) factor : 0;
  surface_update (ss);
}

static void
surface_preferred_buffer_transform (void *data, struct wl_surface *surface,
                                    uint32_t transform)
{
  /* ignored (D17) */
}

static const struct wl_surface_listener surface_listener = {
  surface_enter,
  surface_leave,
  surface_preferred_buffer_scale,
  surface_preferred_buffer_transform
};

static void
fractional_preferred_scale (void *data,
                            struct wp_fractional_scale_v1 *obj,
                            uint32_t scale)
{
  GDKWSurfaceScale *ss = data;

  ss->preferred_fractional = scale;
  surface_update (ss);
}

static const struct wp_fractional_scale_v1_listener fractional_listener = {
  fractional_preferred_scale
};

/* ------------------------------------------------------------------------
 * Per surface objects
 * ------------------------------------------------------------------------ */

static void
surface_make_fractional (GDKWSurfaceScale *ss)
{
  if (ss->fractional != NULL || ss->st->manager == NULL || ss->surface == NULL)
    return;
  ss->fractional =
    wp_fractional_scale_manager_v1_get_fractional_scale (ss->st->manager,
                                                         ss->surface);
  if (ss->fractional != NULL)
    wp_fractional_scale_v1_add_listener (ss->fractional, &fractional_listener,
                                         ss);
}

static void
surface_make_viewport (GDKWSurfaceScale *ss)
{
  if (ss->viewport != NULL || ss->st->viewporter == NULL || ss->surface == NULL)
    return;
  ss->viewport = wp_viewporter_get_viewport (ss->st->viewporter, ss->surface);
  ss->dest_w = -1;
  ss->dest_h = -1;
}

void
_gdk_wayland_scale_surface_created (GDKWWindowImplWayland *impl)
{
  GDKWDisplayWayland *display;
  GDKWScaleDisplay *st;
  GDKWSurfaceScale *ss;

  if (impl == NULL || impl->wl_surface == NULL)
    return;

  if (impl->wscale != NULL)
    {
      if (impl->wscale->surface == impl->wl_surface)
        return;
      _gdk_wayland_scale_surface_destroy (impl);
    }

  display = impl_display (impl);
  if (display == NULL)
    return;
  st = scale_state_get (display, TRUE);
  if (st->finalized)
    return;

  ss = g_new0 (GDKWSurfaceScale, 1);
  ss->st = st;
  ss->impl = impl;
  ss->surface = impl->wl_surface;
  ss->buffer_scale_sent = 1;
  ss->dest_w = -1;
  ss->dest_h = -1;

  wl_surface_add_listener (ss->surface, &surface_listener, ss);
  surface_make_fractional (ss);
  surface_make_viewport (ss);

  st->surfaces = g_list_prepend (st->surfaces, ss);
  impl->wscale = ss;

  /* The initial value is a guess, source HINT.  The frame piece reads it through
   * _gdk_wayland_scale_surface_scale120 () when it makes the first store.  A guess that
   * is wrong costs a flip and a full repaint, so use the best one we have: the scale of
   * the last toplevel of the client, else of the surface with the pointer or the keyboard
   * focus, else the largest output scale (the compositor sends enter only after the first
   * commit).  A popup takes the scale of its parent when the parent is chosen (below). */
  ss->scale120 = surface_eval (ss, &ss->source);
  if (ss->source == GDKW_TEST_SCALE_HINT)
    {
      GDKWSeat *seat = _gdk_wayland_display_get_seat (GDK_DISPLAY_OBJECT (display));
      GDKWWindowImplWayland *focus = seat != NULL ? (seat->pointer_focus != NULL ? seat->pointer_focus : seat->keyboard_focus) : NULL;

      if (st->last_toplevel_scale120 > 0)
        ss->scale120 = st->last_toplevel_scale120;
      else if (focus != NULL && focus != impl && focus->wscale != NULL)
        ss->scale120 = focus->wscale->scale120;
    }
}

void
_gdk_wayland_scale_surface_destroy (GDKWWindowImplWayland *impl)
{
  GDKWSurfaceScale *ss;

  if (impl == NULL || impl->wscale == NULL)
    return;

  ss = impl->wscale;
  impl->wscale = NULL;

  if (ss->fractional != NULL)
    {
      wp_fractional_scale_v1_destroy (ss->fractional);
      ss->fractional = NULL;
    }
  if (ss->viewport != NULL)
    {
      wp_viewport_destroy (ss->viewport);
      ss->viewport = NULL;
    }

  ss->st->surfaces = g_list_remove (ss->st->surfaces, ss);
  g_list_free (ss->entered);
  g_free (ss);
}

/* A popup starts at the scale of its parent: both are on the same output, and a popup that
 * starts at the hint and flips shows a half painted buffer.  Only while nothing better is
 * known (source HINT); a store that exists is replaced by the frame piece. */
void
_gdk_wayland_scale_inherit_parent (GDKWWindowImplWayland *impl,
                                  GDKWWindowImplWayland *parent)
{
  GDKWSurfaceScale *ss = impl != NULL ? impl->wscale : NULL;

  if (ss == NULL || parent == NULL || parent->wscale == NULL ||
      ss->source != GDKW_TEST_SCALE_HINT || ss->scale120 == parent->wscale->scale120)
    return;

  ss->scale120 = parent->wscale->scale120;
  _gdk_wayland_frame_set_scale (impl, ss->scale120);
}

guint
_gdk_wayland_scale_surface_scale120 (GDKWWindowImplWayland *impl)
{
  GDKWScaleDisplay *st;

  if (impl == NULL)
    return GDKW_SCALE_DENOM;
  if (impl->wscale != NULL)
    return impl->wscale->scale120;

  st = scale_state_get (impl_display (impl), FALSE);
  return clamp_scale120 ((guint64) max_output_scale (st) * GDKW_SCALE_DENOM);
}

void
_gdk_wayland_scale_send_state (GDKWWindowImplWayland *impl,
                               guint scale120,
                               gint logical_width,
                               gint logical_height)
{
  GDKWSurfaceScale *ss = impl != NULL ? impl->wscale : NULL;
  gint bs;
  gint dw = -1, dh = -1;

  if (ss == NULL || ss->surface == NULL || impl->wl_surface != ss->surface)
    return;

  scale120 = clamp_scale120 (scale120);

  if (scale120 % GDKW_SCALE_DENOM == 0)
    bs = scale120 / GDKW_SCALE_DENOM;
  else if (ss->viewport != NULL && logical_width > 0 && logical_height > 0)
    {
      bs = 1;
      dw = logical_width;
      dh = logical_height;
    }
  else
    bs = (scale120 + GDKW_SCALE_DENOM - 1) / GDKW_SCALE_DENOM;

  /* set_buffer_scale needs wl_surface version 3 */
  if (wl_surface_get_version (ss->surface) < 3)
    bs = 1;

  if (bs != ss->buffer_scale_sent)
    {
      GDKW_WIRE_SET_BUFFER_SCALE (ss->surface, bs);
      ss->buffer_scale_sent = bs;
      ss->buffer_scale_requests++;
    }

  if (ss->viewport != NULL && (dw != ss->dest_w || dh != ss->dest_h))
    {
      GDKW_WIRE_SET_DESTINATION (ss->surface, ss->viewport, dw, dh);
      ss->dest_w = dw;
      ss->dest_h = dh;
      ss->viewport_requests++;
    }
}

/* ------------------------------------------------------------------------
 * Outputs: geometry, screen and monitors
 * ------------------------------------------------------------------------ */

static gboolean
transform_is_odd (gint transform)
{
  switch (transform)
    {
    case WL_OUTPUT_TRANSFORM_90:
    case WL_OUTPUT_TRANSFORM_270:
    case WL_OUTPUT_TRANSFORM_FLIPPED_90:
    case WL_OUTPUT_TRANSFORM_FLIPPED_270:
      return TRUE;
    default:
      return FALSE;
    }
}

/* wl_output.geometry gives the physical size of the panel in its own
 * orientation; the logical axes swap for a 90 or 270 degree transform. */
static gint
output_mm_w (const GDKWOutput *out)
{
  return transform_is_odd (out->transform) ? out->mm_h : out->mm_w;
}

static gint
output_mm_h (const GDKWOutput *out)
{
  return transform_is_odd (out->transform) ? out->mm_w : out->mm_h;
}

static void
output_compute_logical (GDKWOutput *out)
{
  gint s = output_scale_of (out);
  gint w = out->mode_w / s;
  gint h = out->mode_h / s;

  if (out->have_mode && out->xdg_w > 0 && out->xdg_h > 0)
    {
      out->logical_w = out->xdg_w;
      out->logical_h = out->xdg_h;
      return;
    }

  if (out->have_mode)
    {
      w = MAX (1, w);
      h = MAX (1, h);
    }
  else
    w = h = 0;

  if (transform_is_odd (out->transform))
    {
      out->logical_w = h;
      out->logical_h = w;
    }
  else
    {
      out->logical_w = w;
      out->logical_h = h;
    }
}

static gboolean
output_is_monitor (const GDKWOutput *out)
{
  return out->have_mode && out->logical_w > 0 && out->logical_h > 0;
}

/* Outputs that count as monitors: the ones that have a mode. */
static GDKWOutput *
monitor_nth (GDKWScaleDisplay *st, gint n)
{
  GList *l;
  gint i = 0;

  if (st == NULL || n < 0)
    return NULL;
  for (l = st->outputs; l; l = l->next)
    {
      GDKWOutput *out = l->data;

      if (!output_is_monitor (out))
        continue;
      if (i++ == n)
        return out;
    }
  return NULL;
}

static gint
monitor_count (GDKWScaleDisplay *st)
{
  gint n = 0;

  while (monitor_nth (st, n) != NULL)
    n++;
  return n;
}

/* ------------------------------------------------------------------------
 * The monitor list as the toplevel in use sees it (M11, finding 6)
 *
 * Wayland gives a client no global position, and the backend puts every
 * toplevel at the origin of the root window (M9 decision D2).  GTK picks the
 * monitor of a window by the root coordinates of its points, so a window on
 * the second output, at (0,0), was taken for a window on the first output:
 * menus were clamped to the wrong monitor and Ardour read the size of the
 * wrong monitor.
 *
 * The honest model is that a toplevel sits at the origin of its own output.
 * The monitor list is therefore reported relative to the output of the
 * toplevel that the user works in (the reference output):
 *   - monitor 0 is the reference output, at (0,0);
 *   - the other outputs follow in bind order, at their compositor position
 *     minus the position of the reference output;
 *   - the reference output is the output of the family that has the keyboard
 *     focus (a toplevel and its popups), else of the one that has the pointer
 *     focus, else of any toplevel with an output, else the first monitor.  The
 *     output of a toplevel is the first one of its wl_surface.enter list.
 * With one monitor that monitor is the reference output, so it sits at (0,0)
 * as well, whatever position the compositor gives it (a sole output at 864,40
 * is common when the output left of it is off).  Every toplevel is at (0,0),
 * so a rectangle at 864,40 would clamp the menus away from the window.  The
 * screen size is the bounding box of the compositor layout, measured from the
 * top left corner of the layout, so it does not change when the reference
 * changes, and no signal is emitted for that.
 * A window on another output than the reference output is not described
 * correctly until the user works in it; a menu or dialog is opened by that
 * work, which makes its output the reference output.
 * ------------------------------------------------------------------------ */

static GDKWOutput *
impl_output (GDKWWindowImplWayland *impl)
{
  GDKWWindowImplWayland *root;
  GList *l;

  if (impl == NULL)
    return NULL;
  root = _gdk_wayland_popup_family_root (impl);
  if (root == NULL || root->wscale == NULL)
    return NULL;
  for (l = root->wscale->entered; l; l = l->next)
    if (output_is_monitor (l->data))
      return l->data;
  return NULL;
}

static GDKWOutput *
reference_output (GDKWScaleDisplay *st)
{
  GDKWSeat *seat;
  GDKWOutput *out;
  GSList *l;

  if (st->display == NULL)
    return monitor_nth (st, 0);
  seat = _gdk_wayland_display_get_seat (GDK_DISPLAY_OBJECT (st->display));
  if ((out = impl_output (seat->keyboard_focus)) != NULL)
    return out;
  if ((out = impl_output (seat->pointer_focus)) != NULL)
    return out;
  for (l = st->display->windows; l; l = l->next)
    {
      GDKWWindowImplWayland *impl = l->data;

      if (impl->xdg_toplevel != NULL && (out = impl_output (impl)) != NULL)
        return out;
    }
  return monitor_nth (st, 0);
}

/* The n-th monitor of the list that GTK sees. */
static GDKWOutput *
view_nth (GDKWScaleDisplay *st, gint n)
{
  GDKWOutput *ref, *out;
  gint i, k = 0;

  if (monitor_count (st) < 2)
    return monitor_nth (st, n);
  ref = reference_output (st);
  if (n == 0)
    return ref;
  for (i = 0; (out = monitor_nth (st, i)) != NULL; i++)
    {
      if (out == ref)
        continue;
      if (++k == n)
        return out;
    }
  return NULL;
}

static void
bounding_box (GDKWScaleDisplay *st, gint *width, gint *height)
{
  gint n = monitor_count (st);
  gint i, w = 0, h = 0, x0 = G_MAXINT, y0 = G_MAXINT;

  for (i = 0; i < n; i++)
    {
      GDKWOutput *out = monitor_nth (st, i);

      x0 = MIN (x0, out->x);
      y0 = MIN (y0, out->y);
      w = i == 0 ? out->x + out->logical_w : MAX (w, out->x + out->logical_w);
      h = i == 0 ? out->y + out->logical_h : MAX (h, out->y + out->logical_h);
    }
  if (n > 0)
    {
      w -= x0;
      h -= y0;
    }
  if (w <= 0 || h <= 0)
    {
      w = GDKW_FALLBACK_WIDTH;
      h = GDKW_FALLBACK_HEIGHT;
    }
  *width = w;
  *height = h;
}

/* Mirrors the first output into the legacy fields (D12). */
static void
mirror_first (GDKWScaleDisplay *st)
{
  GDKWDisplayWayland *display = st->display;
  GDKWOutputInfo *info;
  GDKWOutput *head = st->outputs != NULL ? st->outputs->data : NULL;

  if (display == NULL || st->finalized)
    return;

  info = _gdk_wayland_display_get_output_info (GDK_DISPLAY_OBJECT (display));
  g_free (info->make);
  g_free (info->model);
  info->make = NULL;
  info->model = NULL;

  if (head == NULL)
    {
      display->wl_output = NULL;
      display->output_width = 0;
      display->output_height = 0;
      info->have_geometry = FALSE;
      info->have_mode = FALSE;
      info->x = info->y = 0;
      info->width = info->height = 0;
      info->refresh = 0;
      info->mm_width = info->mm_height = 0;
      info->subpixel = 0;
      info->transform = 0;
      info->scale = 1;
      return;
    }

  display->wl_output = head->proxy;
  display->output_width = head->have_mode ? head->mode_w : 0;
  display->output_height = head->have_mode ? head->mode_h : 0;

  info->have_geometry = head->have_geometry;
  info->have_mode = head->have_mode;
  info->x = head->x;
  info->y = head->y;
  info->width = head->mode_w;
  info->height = head->mode_h;
  info->refresh = head->refresh;
  info->mm_width = head->mm_w;
  info->mm_height = head->mm_h;
  info->subpixel = head->subpixel;
  info->transform = head->transform;
  info->scale = output_scale_of (head);
  info->make = g_strdup (head->make);
  info->model = g_strdup (head->model);
}

/* What the two screen signals report: the monitors of the layout and the
 * screen size.  An update that does not change it emits nothing. */
static gchar *
screen_signature (GDKWScaleDisplay *st)
{
  GString *s = g_string_new (NULL);
  GDKWOutput *out;
  gint i, w, h;

  for (i = 0; (out = monitor_nth (st, i)) != NULL; i++)
    g_string_append_printf (s, "%d,%d,%d,%d,%d;", out->x, out->y,
                            out->logical_w, out->logical_h,
                            output_scale_of (out));
  bounding_box (st, &w, &h);
  g_string_append_printf (s, "%dx%d", w, h);
  return g_string_free (s, FALSE);
}

/* An atomic output update ends with wl_output.done.  A compositor that binds
 * xdg_output below version 3 sends xdg_output.done as well, and both end up
 * here.  The signals fire once per change of the visible state, so the
 * second done of one update reports nothing. */
static void
emit_screen_signals (GDKWScaleDisplay *st)
{
  GdkScreen *screen;
  gchar *sig;

  if (st->finalized || st->display == NULL)
    return;
  screen = st->display->default_screen;
  if (screen == NULL || !GDKW_IS_SCREEN_WAYLAND (screen))
    return;

  sig = screen_signature (st);
  if (st->emitted_sig != NULL && strcmp (st->emitted_sig, sig) == 0)
    {
      g_free (sig);
      return;
    }
  g_free (st->emitted_sig);
  st->emitted_sig = sig;

  g_object_ref (screen);
  g_signal_emit_by_name (screen, "size-changed");
  g_signal_emit_by_name (screen, "monitors-changed");
  g_object_unref (screen);
}

/* Recomputes the logical sizes, the screen and the root window.  Emits the
 * two GdkScreen signals once when @emit is TRUE. */
static void
outputs_changed (GDKWScaleDisplay *st, gboolean emit)
{
  GDKWDisplayWayland *display = st->display;
  GdkScreen *screen;
  GList *l;
  gint w, h;

  for (l = st->outputs; l; l = l->next)
    output_compute_logical (l->data);

  mirror_first (st);

  if (st->finalized || display == NULL)
    return;

  screen = display->default_screen;
  if (screen == NULL || !GDKW_IS_SCREEN_WAYLAND (screen))
    return;

  bounding_box (st, &w, &h);
  {
    GDKWScreenWayland *ws = GDKW_SCREEN_WAYLAND (screen);

    if (ws->width != w || ws->height != h)
      {
        ws->width = w;
        ws->height = h;

        if (ws->root_window != NULL)
          {
            GdkWindowObject *root = (GdkWindowObject *) ws->root_window;

            root->width = w;
            root->height = h;
            if (root->impl != NULL && GDKW_IS_WINDOW_IMPL_WAYLAND (root->impl))
              {
                GDKW_WINDOW_IMPL_WAYLAND (root->impl)->width = w;
                GDKW_WINDOW_IMPL_WAYLAND (root->impl)->height = h;
              }
            _gdk_window_update_size (ws->root_window);
          }
      }
  }

  if (emit)
    emit_screen_signals (st);
}

/* What wl_output.done does (and what each event does for a v1 output). */
static void
output_apply (GDKWOutput *out)
{
  GDKWScaleDisplay *st = out->st;

  if (out->has_pending_xdg)
    {
      out->xdg_w = out->pending_xdg_w;
      out->xdg_h = out->pending_xdg_h;
      out->has_pending_xdg = FALSE;
    }
  if (out->has_pending_xdg_pos)
    {
      out->xdg_x = out->pending_xdg_x;
      out->xdg_y = out->pending_xdg_y;
      out->xdg_pos_valid = TRUE;
      out->x = out->xdg_x;
      out->y = out->xdg_y;
      out->has_pending_xdg_pos = FALSE;
    }

  if (out->has_pending_scale)
    {
      out->scale = out->pending_scale > 0 ? out->pending_scale : 1;
      out->has_pending_scale = FALSE;
    }
  out->done_seen = TRUE;

  /* surfaces first, so a signal handler sees the new per surface scale */
  outputs_changed (st, FALSE);
  update_all_surfaces (st);
  emit_screen_signals (st);
}

static void
output_geometry (void *data, struct wl_output *wl_output,
                 int32_t x, int32_t y,
                 int32_t physical_width, int32_t physical_height,
                 int32_t subpixel, const char *make, const char *model,
                 int32_t transform)
{
  GDKWOutput *out = data;

  out->have_geometry = TRUE;
  if (!out->xdg_pos_valid)
    {
      out->x = x;
      out->y = y;
    }
  out->mm_w = MAX (0, physical_width);
  out->mm_h = MAX (0, physical_height);
  out->subpixel = subpixel;
  out->transform = transform;
  g_free (out->make);
  g_free (out->model);
  out->make = g_strdup (make);
  out->model = g_strdup (model);

  if (out->version < 2)
    output_apply (out);
  else
    mirror_first (out->st);
}

static void
output_mode (void *data, struct wl_output *wl_output, uint32_t flags,
             int32_t width, int32_t height, int32_t refresh)
{
  GDKWOutput *out = data;

  if (!(flags & WL_OUTPUT_MODE_CURRENT))
    return;

  out->have_mode = TRUE;
  out->mode_w = width;
  out->mode_h = height;
  out->refresh = refresh;

  if (out->version < 2)
    output_apply (out);
  else
    mirror_first (out->st);
}

static void
output_done (void *data, struct wl_output *wl_output)
{
  output_apply (data);
}

static void
output_scale (void *data, struct wl_output *wl_output, int32_t factor)
{
  GDKWOutput *out = data;

  out->pending_scale = factor;
  out->has_pending_scale = TRUE;
}

static void
output_name (void *data, struct wl_output *wl_output, const char *name)
{
  GDKWOutput *out = data;

  g_strlcpy (out->name, name != NULL ? name : "", sizeof out->name);
}

static void
output_description (void *data, struct wl_output *wl_output,
                    const char *description)
{
}

static const struct wl_output_listener output_listener = {
  output_geometry,
  output_mode,
  output_done,
  output_scale,
  output_name,
  output_description
};

static void
xdg_output_logical_position (void *data, struct zxdg_output_v1 *xdg,
                             int32_t x, int32_t y)
{
  GDKWOutput *out = data;

  out->pending_xdg_x = x;
  out->pending_xdg_y = y;
  out->has_pending_xdg_pos = TRUE;
}

static void
xdg_output_logical_size (void *data, struct zxdg_output_v1 *xdg,
                         int32_t width, int32_t height)
{
  GDKWOutput *out = data;

  out->pending_xdg_w = width;
  out->pending_xdg_h = height;
  out->has_pending_xdg = TRUE;
}

static void
xdg_output_done (void *data, struct zxdg_output_v1 *xdg)
{
  GDKWOutput *out = data;

  /* Deprecated from version 3: wl_output.done ends the update. */
  if (out->xdg_version >= 3)
    return;
  output_apply (out);
}

static void
xdg_output_name (void *data, struct zxdg_output_v1 *xdg, const char *name)
{
}

static void
xdg_output_description (void *data, struct zxdg_output_v1 *xdg,
                        const char *description)
{
}

static const struct zxdg_output_v1_listener xdg_output_listener = {
  xdg_output_logical_position,
  xdg_output_logical_size,
  xdg_output_done,
  xdg_output_name,
  xdg_output_description
};

/* Asks for the xdg_output of a real output when the manager is there. */
static void
output_make_xdg (GDKWScaleDisplay *st, GDKWOutput *out)
{
  if (st->xdg_manager == NULL || out->proxy == NULL || out->xdg != NULL ||
      st->finalized)
    return;

  out->xdg = zxdg_output_manager_v1_get_xdg_output (st->xdg_manager,
                                                   out->proxy);
  if (out->xdg != NULL)
    {
      out->xdg_version = zxdg_output_v1_get_version (out->xdg);
      zxdg_output_v1_add_listener (out->xdg, &xdg_output_listener, out);
    }
}

static void
output_destroy_xdg (GDKWOutput *out)
{
  if (out->xdg == NULL)
    return;
  zxdg_output_v1_destroy (out->xdg);
  out->xdg = NULL;
  out->xdg_w = out->xdg_h = 0;
  out->xdg_pos_valid = FALSE;
}

static GDKWOutput *
output_new (GDKWScaleDisplay *st, struct wl_output *proxy, guint32 name,
            guint32 version, gboolean fake)
{
  GDKWOutput *out = g_new0 (GDKWOutput, 1);

  out->st = st;
  out->id = st->next_id++;
  out->global_name = name;
  out->version = version;
  out->proxy = proxy;
  out->fake = fake;
  out->scale = 1;
  st->outputs = g_list_append (st->outputs, out);
  return out;
}

static void
output_destroy_proxy (GDKWOutput *out)
{
  output_destroy_xdg (out);
  if (out->proxy == NULL)
    return;
  if (out->version >= WL_OUTPUT_RELEASE_SINCE_VERSION)
    wl_output_release (out->proxy);
  else
    wl_output_destroy (out->proxy);
  out->proxy = NULL;
}

/* Removes @out from every list, destroys the proxy, re-evaluates. */
static void
output_remove (GDKWScaleDisplay *st, GDKWOutput *out)
{
  GList *l;

  st->outputs = g_list_remove (st->outputs, out);
  for (l = st->surfaces; l; l = l->next)
    {
      GDKWSurfaceScale *ss = l->data;

      ss->entered = g_list_remove (ss->entered, out);
    }
  output_destroy_proxy (out);
  g_free (out->make);
  g_free (out->model);
  g_free (out);

  /* surfaces first, so a signal handler sees the new per surface scale */
  outputs_changed (st, FALSE);
  update_all_surfaces (st);
  emit_screen_signals (st);
}

void
_gdk_wayland_scale_bind_output (GDKWDisplayWayland *display,
                                struct wl_registry *registry,
                                guint32 name, guint32 version)
{
  GDKWScaleDisplay *st = scale_state_get (display, TRUE);
  GDKWOutput *out;
  guint32 v = MIN (version, GDKW_OUTPUT_MAX_VERSION);
  struct wl_output *proxy;

  if (st->finalized || v == 0)
    return;
  proxy = wl_registry_bind (registry, name, &wl_output_interface, v);
  if (proxy == NULL)
    return;

  out = output_new (st, proxy, name, v, FALSE);
  wl_output_add_listener (proxy, &output_listener, out);
  output_make_xdg (st, out);
  mirror_first (st);
}

gboolean
_gdk_wayland_scale_bind_global (GDKWDisplayWayland *display,
                                struct wl_registry *registry,
                                guint32 name,
                                const char *interface,
                                guint32 version)
{
  GDKWScaleDisplay *st;
  GList *l;

  if (interface == NULL)
    return FALSE;

  if (strcmp (interface, wp_viewporter_interface.name) == 0)
    {
      st = scale_state_get (display, TRUE);
      if (st->viewporter == NULL && !st->finalized)
        {
          st->viewporter = wl_registry_bind (registry, name,
                                             &wp_viewporter_interface, 1);
          st->viewporter_name = name;
          for (l = st->surfaces; l; l = l->next)
            surface_make_viewport (l->data);
          update_all_surfaces (st);
        }
      return TRUE;
    }

  if (strcmp (interface, zxdg_output_manager_v1_interface.name) == 0)
    {
      st = scale_state_get (display, TRUE);
      if (st->xdg_manager == NULL && !st->finalized)
        {
          /* Version 2 adds name and description.  Version 3 deprecates done:
           * the compositor then sends no xdg_output.done and wl_output.done
           * ends the update, so one update is one done.  Pending xdg_output
           * state is applied by output_apply for both. */
          st->xdg_manager =
            wl_registry_bind (registry, name,
                              &zxdg_output_manager_v1_interface,
                              MIN (version, 3));
          st->xdg_manager_name = name;
          for (l = st->outputs; l; l = l->next)
            output_make_xdg (st, l->data);
        }
      return TRUE;
    }

  if (strcmp (interface, wp_fractional_scale_manager_v1_interface.name) == 0)
    {
      st = scale_state_get (display, TRUE);
      if (st->manager == NULL && !st->finalized)
        {
          st->manager =
            wl_registry_bind (registry, name,
                              &wp_fractional_scale_manager_v1_interface, 1);
          st->manager_name = name;
          for (l = st->surfaces; l; l = l->next)
            surface_make_fractional (l->data);
        }
      return TRUE;
    }

  return FALSE;
}

gboolean
_gdk_wayland_scale_global_remove (GDKWDisplayWayland *display, guint32 name)
{
  GDKWScaleDisplay *st = scale_state_get (display, FALSE);
  GDKWOutput *out;
  GList *l;

  if (st == NULL || st->finalized)
    return FALSE;

  out = output_by_name (st, name);
  if (out != NULL)
    {
      output_remove (st, out);
      return TRUE;
    }

  if (st->xdg_manager != NULL && st->xdg_manager_name == name)
    {
      for (l = st->outputs; l; l = l->next)
        output_destroy_xdg (l->data);
      zxdg_output_manager_v1_destroy (st->xdg_manager);
      st->xdg_manager = NULL;
      st->xdg_manager_name = 0;
      outputs_changed (st, TRUE);
      update_all_surfaces (st);
      return TRUE;
    }

  if (st->viewporter != NULL && st->viewporter_name == name)
    {
      for (l = st->surfaces; l; l = l->next)
        {
          GDKWSurfaceScale *ss = l->data;

          if (ss->viewport != NULL)
            {
              wp_viewport_destroy (ss->viewport);
              ss->viewport = NULL;
              ss->dest_w = -1;
              ss->dest_h = -1;
            }
        }
      wp_viewporter_destroy (st->viewporter);
      st->viewporter = NULL;
      st->viewporter_name = 0;
      update_all_surfaces (st);
      return TRUE;
    }

  if (st->manager != NULL && st->manager_name == name)
    {
      for (l = st->surfaces; l; l = l->next)
        {
          GDKWSurfaceScale *ss = l->data;

          if (ss->fractional != NULL)
            {
              wp_fractional_scale_v1_destroy (ss->fractional);
              ss->fractional = NULL;
            }
          ss->preferred_fractional = 0;
        }
      wp_fractional_scale_manager_v1_destroy (st->manager);
      st->manager = NULL;
      st->manager_name = 0;
      update_all_surfaces (st);
      return TRUE;
    }

  return FALSE;
}

void
_gdk_wayland_scale_display_finalize (GdkDisplay *gdk_display)
{
  GDKWDisplayWayland *display = display_checked (gdk_display);
  GDKWScaleDisplay *st;
  GList *l;

  if (display == NULL)
    return;
  st = g_object_steal_data (G_OBJECT (display), GDKW_SCALE_DATA_KEY);
  if (st == NULL)
    return;

  st->finalized = TRUE;

  /* leftover surfaces (should be none: windows are torn down first) */
  for (l = st->surfaces; l; l = l->next)
    {
      GDKWSurfaceScale *ss = l->data;

      if (ss->fractional != NULL)
        wp_fractional_scale_v1_destroy (ss->fractional);
      if (ss->viewport != NULL)
        wp_viewport_destroy (ss->viewport);
      ss->fractional = NULL;
      ss->viewport = NULL;
      if (ss->impl != NULL && ss->impl->wscale == ss)
        ss->impl->wscale = NULL;
    }

  for (l = st->outputs; l; l = l->next)
    output_destroy_proxy (l->data);

  if (st->xdg_manager != NULL)
    {
      zxdg_output_manager_v1_destroy (st->xdg_manager);
      st->xdg_manager = NULL;
    }
  if (st->viewporter != NULL)
    {
      wp_viewporter_destroy (st->viewporter);
      st->viewporter = NULL;
    }
  if (st->manager != NULL)
    {
      wp_fractional_scale_manager_v1_destroy (st->manager);
      st->manager = NULL;
    }

  display->wl_output = NULL;
  scale_state_free (st);
}

/* ------------------------------------------------------------------------
 * Screen and monitor answers (logical units)
 * ------------------------------------------------------------------------ */

void
_gdk_wayland_scale_screen_size (GDKWDisplayWayland *display,
                                gint *width, gint *height)
{
  GDKWScaleDisplay *st = scale_state_get (display, FALSE);
  gint w = GDKW_FALLBACK_WIDTH, h = GDKW_FALLBACK_HEIGHT;

  if (st != NULL)
    {
      GList *l;

      for (l = st->outputs; l; l = l->next)
        output_compute_logical (l->data);
      bounding_box (st, &w, &h);
    }
  if (width != NULL)
    *width = w;
  if (height != NULL)
    *height = h;
}

gint
_gdk_wayland_scale_n_monitors (GdkDisplay *display)
{
  gint n = monitor_count (scale_state_get (display_checked (display), FALSE));

  return MAX (1, n);
}

void
_gdk_wayland_scale_monitor_geometry (GdkDisplay *display, gint monitor,
                                     GdkRectangle *dest)
{
  GDKWScaleDisplay *st = scale_state_get (display_checked (display), FALSE);
  GDKWOutput *out;

  if (dest == NULL)
    return;
  dest->x = dest->y = dest->width = dest->height = 0;

  out = view_nth (st, monitor);
  if (out != NULL)
    {
      GDKWOutput *ref = reference_output (st);

      dest->x = out->x - (ref != NULL ? ref->x : 0);
      dest->y = out->y - (ref != NULL ? ref->y : 0);
      dest->width = out->logical_w;
      dest->height = out->logical_h;
    }
  else if (monitor == 0 && monitor_count (st) == 0)
    {
      dest->width = GDKW_FALLBACK_WIDTH;
      dest->height = GDKW_FALLBACK_HEIGHT;
    }
}

gint
_gdk_wayland_scale_monitor_width_mm (GdkDisplay *display, gint monitor)
{
  GDKWScaleDisplay *st = scale_state_get (display_checked (display), FALSE);
  GDKWOutput *out = view_nth (st, monitor);

  if (out != NULL)
    return output_mm_w (out) > 0 ? output_mm_w (out) : px_to_mm (out->logical_w);
  if (monitor == 0 && monitor_count (st) == 0)
    return px_to_mm (GDKW_FALLBACK_WIDTH);
  return -1;
}

gint
_gdk_wayland_scale_monitor_height_mm (GdkDisplay *display, gint monitor)
{
  GDKWScaleDisplay *st = scale_state_get (display_checked (display), FALSE);
  GDKWOutput *out = view_nth (st, monitor);

  if (out != NULL)
    return output_mm_h (out) > 0 ? output_mm_h (out) : px_to_mm (out->logical_h);
  if (monitor == 0 && monitor_count (st) == 0)
    return px_to_mm (GDKW_FALLBACK_HEIGHT);
  return -1;
}

gchar *
_gdk_wayland_scale_monitor_plug_name (GdkDisplay *display, gint monitor)
{
  GDKWScaleDisplay *st = scale_state_get (display_checked (display), FALSE);
  GDKWOutput *out = view_nth (st, monitor);

  if (out == NULL)
    return monitor == 0 ? g_strdup ("WL-1") : NULL;
  if (out->name[0] != '\0')
    return g_strdup (out->name);
  if (out->model != NULL && out->model[0] != '\0')
    return g_strdup (out->model);
  return g_strdup_printf ("WL-%u", out->id);
}

gint
_gdk_wayland_scale_screen_width_mm (GdkDisplay *display)
{
  GDKWScaleDisplay *st = scale_state_get (display_checked (display), FALSE);
  GDKWOutput *first = view_nth (st, 0);   /* the reference output, like monitor 0 */
  gint w, h;

  if (st == NULL)
    return px_to_mm (GDKW_FALLBACK_WIDTH);
  bounding_box (st, &w, &h);
  if (first != NULL && output_mm_w (first) > 0 && first->logical_w > 0)
    return MAX (1, (gint) floor ((gdouble) w * output_mm_w (first) /
                                 first->logical_w + 0.5));
  return px_to_mm (w);
}

gint
_gdk_wayland_scale_screen_height_mm (GdkDisplay *display)
{
  GDKWScaleDisplay *st = scale_state_get (display_checked (display), FALSE);
  GDKWOutput *first = view_nth (st, 0);   /* the reference output, like monitor 0 */
  gint w, h;

  if (st == NULL)
    return px_to_mm (GDKW_FALLBACK_HEIGHT);
  bounding_box (st, &w, &h);
  if (first != NULL && output_mm_h (first) > 0 && first->logical_h > 0)
    return MAX (1, (gint) floor ((gdouble) h * output_mm_h (first) /
                                 first->logical_h + 0.5));
  return px_to_mm (h);
}

/* ------------------------------------------------------------------------
 * Cursor (D13)
 * ------------------------------------------------------------------------ */

gint
_gdk_wayland_scale_cursor_scale (GdkDisplay *gdk_display)
{
  GDKWDisplayWayland *display = display_checked (gdk_display);
  GDKWScaleDisplay *st;
  GDKWSeat *seat;
  gint k;

  if (display == NULL)
    return 1;

  st = scale_state_get (display, FALSE);
  k = max_output_scale (st);

  seat = _gdk_wayland_display_get_seat (gdk_display);
  if (seat != NULL && seat->pointer_focus != NULL &&
      seat->pointer_focus->wscale != NULL)
    k = (gint) ((seat->pointer_focus->wscale->scale120 +
                 GDKW_SCALE_DENOM - 1) / GDKW_SCALE_DENOM);

  return MAX (1, k);
}

void
_gdk_wayland_scale_note_cursor (GdkDisplay *gdk_display,
                                gint buffer_scale,
                                gint buffer_width, gint buffer_height,
                                gint hot_x_sent, gint hot_y_sent,
                                gint theme_px)
{
  GDKWDisplayWayland *display = display_checked (gdk_display);
  GDKWScaleDisplay *st = scale_state_get (display, TRUE);

  if (st == NULL)
    return;
  st->cursor_seen = TRUE;
  st->cursor.buffer_scale = buffer_scale;
  st->cursor.buffer_width = buffer_width;
  st->cursor.buffer_height = buffer_height;
  st->cursor.hot_x_sent = hot_x_sent;
  st->cursor.hot_y_sent = hot_y_sent;
  st->cursor.theme_px = theme_px;
  st->cursor.applications++;
}

/* ------------------------------------------------------------------------
 * Test hooks (D16).  They drive the real listeners.
 * ------------------------------------------------------------------------ */

gboolean
_gdk_wayland_test_scale_info (GdkWindow *window, GDKWTestScaleInfo *info)
{
  GDKWWindowImplWayland *impl;
  GDKWSurfaceScale *ss;
  GList *l;

  if (info != NULL)
    memset (info, 0, sizeof *info);
  impl = impl_from_window (window);
  if (impl == NULL || info == NULL || impl->wscale == NULL)
    return FALSE;

  ss = impl->wscale;
  info->scale120 = ss->scale120;
  info->source = ss->source;
  info->buffer_scale_sent = ss->buffer_scale_sent;
  info->buffer_scale_requests = ss->buffer_scale_requests;
  info->viewport_dest_w = ss->dest_w;
  info->viewport_dest_h = ss->dest_h;
  info->viewport_requests = ss->viewport_requests;
  info->has_viewport = ss->viewport != NULL;
  info->has_fractional = ss->fractional != NULL;
  info->preferred_int = ss->preferred_int;
  info->preferred_fractional = ss->preferred_fractional;
  info->scale_changes = ss->scale_changes;
  for (l = ss->entered; l; l = l->next)
    {
      if (info->n_entered < GDKW_TEST_MAX_ENTERED)
        info->entered_ids[info->n_entered] = ((GDKWOutput *) l->data)->id;
      info->n_entered++;
    }
  info->n_entered = MIN (info->n_entered, GDKW_TEST_MAX_ENTERED);
  return TRUE;
}

guint
_gdk_wayland_test_output_count (GdkDisplay *display)
{
  GDKWScaleDisplay *st = scale_state_get (display_checked (display), FALSE);

  return st != NULL ? g_list_length (st->outputs) : 0;
}

gboolean
_gdk_wayland_test_output_info (GdkDisplay *display, guint index,
                               GDKWTestOutputInfo *info)
{
  GDKWScaleDisplay *st = scale_state_get (display_checked (display), FALSE);
  GDKWOutput *out;
  GList *l;

  if (info != NULL)
    memset (info, 0, sizeof *info);
  if (st == NULL || info == NULL)
    return FALSE;
  out = g_list_nth_data (st->outputs, index);
  if (out == NULL)
    return FALSE;

  info->id = out->id;
  info->fake = out->fake;
  info->x = out->x;
  info->y = out->y;
  info->mode_width = out->mode_w;
  info->mode_height = out->mode_h;
  info->scale = out->scale;
  info->transform = out->transform;
  info->logical_width = out->logical_w;
  info->logical_height = out->logical_h;
  info->mm_width = out->mm_w;
  info->mm_height = out->mm_h;
  g_strlcpy (info->name, out->name, sizeof info->name);
  info->done_seen = out->done_seen;
  for (l = st->surfaces; l; l = l->next)
    if (g_list_find (((GDKWSurfaceScale *) l->data)->entered, out) != NULL)
      info->surfaces_on++;
  return TRUE;
}

gboolean
_gdk_wayland_test_output_add (GdkDisplay *gdk_display,
                              gint x, gint y,
                              gint mode_width, gint mode_height,
                              gint scale, gint transform, guint *id_out)
{
  GDKWDisplayWayland *display = display_checked (gdk_display);
  GDKWScaleDisplay *st = scale_state_get (display, TRUE);
  GDKWOutput *out;
  struct wl_output *key;

  if (st == NULL || st->finalized)
    return FALSE;

  out = output_new (st, NULL, GDKW_FAKE_NAME_BASE | st->next_id, 4, TRUE);
  key = output_key (out);

  output_geometry (out, key, x, y, 0, 0, WL_OUTPUT_SUBPIXEL_UNKNOWN,
                   "ydk", "fake", transform);
  output_mode (out, key, WL_OUTPUT_MODE_CURRENT, mode_width, mode_height,
               60000);
  output_scale (out, key, scale);
  output_done (out, key);

  if (id_out != NULL)
    *id_out = out->id;
  return TRUE;
}

gboolean
_gdk_wayland_test_output_scale (GdkDisplay *display, guint id, gint scale)
{
  GDKWScaleDisplay *st = scale_state_get (display_checked (display), FALSE);
  GDKWOutput *out = st != NULL ? output_by_id (st, id) : NULL;

  if (out == NULL)
    return FALSE;
  output_scale (out, output_key (out), scale);
  output_done (out, output_key (out));
  return TRUE;
}

gboolean
_gdk_wayland_test_output_mode (GdkDisplay *display, guint id,
                               gint mode_width, gint mode_height)
{
  GDKWScaleDisplay *st = scale_state_get (display_checked (display), FALSE);
  GDKWOutput *out = st != NULL ? output_by_id (st, id) : NULL;

  if (out == NULL)
    return FALSE;
  output_mode (out, output_key (out), WL_OUTPUT_MODE_CURRENT, mode_width,
               mode_height, out->refresh);
  output_done (out, output_key (out));
  return TRUE;
}

gboolean
_gdk_wayland_test_output_xdg (GdkDisplay *display, guint id, guint version,
                              gint x, gint y, gint width, gint height,
                              gboolean send_done)
{
  GDKWScaleDisplay *st = scale_state_get (display_checked (display), FALSE);
  GDKWOutput *out = st != NULL ? output_by_id (st, id) : NULL;

  if (out == NULL)
    return FALSE;
  out->xdg_version = version;
  xdg_output_logical_position (out, NULL, x, y);
  xdg_output_logical_size (out, NULL, width, height);
  if (send_done)
    xdg_output_done (out, NULL);
  return TRUE;
}

gboolean
_gdk_wayland_test_output_done (GdkDisplay *display, guint id)
{
  GDKWScaleDisplay *st = scale_state_get (display_checked (display), FALSE);
  GDKWOutput *out = st != NULL ? output_by_id (st, id) : NULL;

  if (out == NULL)
    return FALSE;
  output_done (out, output_key (out));
  return TRUE;
}

gboolean
_gdk_wayland_test_output_mm (GdkDisplay *display, guint id,
                             gint mm_width, gint mm_height)
{
  GDKWScaleDisplay *st = scale_state_get (display_checked (display), FALSE);
  GDKWOutput *out = st != NULL ? output_by_id (st, id) : NULL;
  gchar *make, *model;

  if (out == NULL)
    return FALSE;
  make = g_strdup (out->make);
  model = g_strdup (out->model);
  output_geometry (out, output_key (out), out->x, out->y, mm_width, mm_height,
                   out->subpixel, make, model, out->transform);
  g_free (make);
  g_free (model);
  output_done (out, output_key (out));
  return TRUE;
}

gboolean
_gdk_wayland_test_output_remove (GdkDisplay *display, guint id)
{
  GDKWDisplayWayland *wd = display_checked (display);
  GDKWScaleDisplay *st = scale_state_get (wd, FALSE);
  GDKWOutput *out = st != NULL ? output_by_id (st, id) : NULL;

  if (out == NULL)
    return FALSE;
  return _gdk_wayland_scale_global_remove (wd, out->global_name);
}

gboolean
_gdk_wayland_test_surface_enter (GdkWindow *window, guint id)
{
  GDKWWindowImplWayland *impl = impl_from_window (window);
  GDKWSurfaceScale *ss = impl != NULL ? impl->wscale : NULL;
  GDKWOutput *out = ss != NULL ? output_by_id (ss->st, id) : NULL;

  if (out == NULL)
    return FALSE;
  surface_enter (ss, ss->surface, output_key (out));
  return TRUE;
}

gboolean
_gdk_wayland_test_surface_leave (GdkWindow *window, guint id)
{
  GDKWWindowImplWayland *impl = impl_from_window (window);
  GDKWSurfaceScale *ss = impl != NULL ? impl->wscale : NULL;
  GDKWOutput *out = ss != NULL ? output_by_id (ss->st, id) : NULL;

  if (out == NULL)
    return FALSE;
  surface_leave (ss, ss->surface, output_key (out));
  return TRUE;
}

gboolean
_gdk_wayland_test_preferred_buffer_scale (GdkWindow *window, gint scale)
{
  GDKWWindowImplWayland *impl = impl_from_window (window);
  GDKWSurfaceScale *ss = impl != NULL ? impl->wscale : NULL;

  if (ss == NULL)
    return FALSE;
  surface_preferred_buffer_scale (ss, ss->surface, scale);
  return TRUE;
}

gboolean
_gdk_wayland_test_preferred_fractional (GdkWindow *window, guint scale120)
{
  GDKWWindowImplWayland *impl = impl_from_window (window);
  GDKWSurfaceScale *ss = impl != NULL ? impl->wscale : NULL;

  if (ss == NULL)
    return FALSE;
  fractional_preferred_scale (ss, ss->fractional, scale120);
  return TRUE;
}

gboolean
_gdk_wayland_test_fractional_force (GdkDisplay *display, gboolean enable)
{
  GDKWDisplayWayland *wd = display_checked (display);
  GDKWScaleDisplay *st = scale_state_get (wd, TRUE);

  if (st == NULL || st->finalized)
    return FALSE;
  st->force_fractional = enable != FALSE;
  update_all_surfaces (st);
  return TRUE;
}

gboolean
_gdk_wayland_test_cursor_scale_info (GdkDisplay *display,
                                     GDKWTestCursorScaleInfo *info)
{
  GDKWScaleDisplay *st = scale_state_get (display_checked (display), FALSE);

  if (info != NULL)
    memset (info, 0, sizeof *info);
  if (st == NULL || info == NULL)
    return FALSE;
  *info = st->cursor;
  return TRUE;
}
