/*
 * gdkpopup-wayland.c
 *
 * ydk-wayland fork, M9 (piece "popup"): window roles, xdg_popup lifecycle,
 * positioner, grab policy, popup_done handling and the popup test hooks.
 *
 * The decisions (D1 to D12) and the contract with the other pieces and the
 * integrator are in ydk/wayland/ydk/gdkm9-wayland.h.  This file owns:
 *
 *   - the role choice of a window (D1);
 *   - the popup record GDKWPopup, hung off GDKWWindowImplWayland.popup;
 *   - the per display ledger (family stack, pending list, commit idle,
 *     positioner counters, test switches);
 *   - the deferred first commit (D5), parent choice (D3), positioner (D4);
 *   - configure feedback (D6), grab policy (D7), popup_done (D8);
 *   - destruction order (D9), size and move (D10), tooltip regions (D11);
 *   - the xdg_popup listener and the hooks for the demo.
 *
 * Protocol rule kept everywhere: the xdg_popup, xdg_surface and wl_surface of
 * a popup are destroyed in that order, and a popup is destroyed only after
 * every popup above it in its family (topmost first).
 *
 * No Xlib here.  No public gdk_* name is dispatched from this file.
 */

#include "config.h"

#include <string.h>

#include <wayland-client.h>

#include <ydk/gdk.h>
#include <ydk/gdkinternals.h>

#include "xdg-shell-client.h"

#include "gdkprivate-wayland.h"
#include "gdkseat-wayland.h"
#include "gdkm7-wayland.h"
#include "gdkm9-wayland.h"
#include "gdkm10-wayland.h"

#ifdef GDK_COMPILATION
#include "gdkalias.h"
#endif

/* hints file (gdkwindow-hints-wayland.c); not in a shared header */
GdkWindowTypeHint _gdk_wayland_window_get_type_hint (GdkWindow *window);

#define POPUP_DISPLAY_KEY  "ydk-wayland-popups"
#define FALLBACK_KEY       "ydk-wayland-popup-fallback"

/* =========================================================================
 * Types
 * ========================================================================= */

struct _GDKWPopup
{
  GDKWWindowImplWayland *impl;       /* owner, no ref */
  GDKWDisplayWayland    *display;
  GDKWPopupState         state;

  struct xdg_popup      *xdg_popup;  /* NULL while PENDING */
  GDKWWindowImplWayland *parent;     /* xdg parent, NULL while PENDING */
  GDKWWindowImplWayland *root;       /* base toplevel of the family */
  guint                  depth;

  gboolean               in_stack;
  gboolean               in_pending;
  gboolean               releasing;  /* dismissal in progress */
  gboolean               no_grab;    /* retry mode: never grab */
  gboolean               kbd_focus_at_commit; /* the family had the keyboard focus when the popup was committed */

  /* position relative to the parent geometry, last known */
  gint                   rel_x, rel_y;

  /* record for the hooks */
  gint                   pos_width, pos_height;
  gint                   anchor_x, anchor_y, anchor_w, anchor_h;
  guint                  anchor, gravity, constraint_adjustment;
  gint                   offset_x, offset_y;
  guint                  get_popup_requests;
  guint                  reposition_requests;
  guint32                reposition_token;
  guint32                repositioned_token;

  gboolean               grab_wanted;
  gboolean               grab_sent;
  gboolean               grab_virtual;
  guint32                grab_serial;
  gboolean               retried;

  gboolean               have_conf;
  gint                   conf_x, conf_y, conf_w, conf_h;
  guint                  configure_events;
  guint                  acked_configures;
  guint                  popup_done_events;
  gboolean               input_region_empty;
};

typedef struct
{
  GDKWDisplayWayland *display;
  GList              *stack;          /* GDKWPopup *, commit order */
  GList              *pending;        /* GDKWPopup *, creation order */
  guint               commit_source;
  gboolean            committing;
  guint               positioners_created;
  guint               positioners_destroyed;
  guint32             token;
  gboolean            virtual_seat;
  guint               force_version;
} PopupDisplay;

typedef struct
{
  GDKWWindowImplWayland *impl;        /* ref held */
  GDKWPopup             *popup;       /* identity only after callbacks */
} SnapItem;

/* =========================================================================
 * Small helpers
 * ========================================================================= */

static GDKWDisplayWayland *
impl_display (GDKWWindowImplWayland *impl)
{
  GdkDisplay *d;

  if (impl == NULL || impl->screen == NULL)
    return NULL;
  d = gdk_screen_get_display (impl->screen);
  if (d == NULL || !GDKW_IS_DISPLAY_WAYLAND (d))
    return NULL;

  return GDKW_DISPLAY_WAYLAND (d);
}

static void
pd_free (gpointer data)
{
  PopupDisplay *pd = data;

  if (pd == NULL)
    return;
  if (pd->commit_source != 0)
    g_source_remove (pd->commit_source);
  g_list_free (pd->stack);
  g_list_free (pd->pending);
  g_free (pd);
}

static PopupDisplay *
get_pd (GDKWDisplayWayland *display, gboolean create)
{
  PopupDisplay *pd;

  if (display == NULL)
    return NULL;
  pd = g_object_get_data (G_OBJECT (display), POPUP_DISPLAY_KEY);
  if (pd == NULL && create)
    {
      pd = g_new0 (PopupDisplay, 1);
      pd->display = display;
      g_object_set_data_full (G_OBJECT (display), POPUP_DISPLAY_KEY, pd,
                              pd_free);
    }

  return pd;
}

static gboolean
window_alive (GdkWindow *wrapper)
{
  return wrapper != NULL && GDK_IS_WINDOW (wrapper) &&
         !GDK_WINDOW_DESTROYED (wrapper);
}

static gboolean
window_mapped (GdkWindow *wrapper)
{
  return window_alive (wrapper) && GDK_WINDOW_IS_MAPPED (wrapper);
}

/* O (impl) of D2 */
static void
impl_origin (GDKWWindowImplWayland *impl, gint *ox, gint *oy)
{
  if (impl != NULL && impl->popup != NULL)
    {
      *ox = impl->x;
      *oy = impl->y;
    }
  else
    {
      *ox = 0;
      *oy = 0;
    }
}

static GdkEvent *
new_event (GdkWindow *wrapper, GdkEventType type)
{
  GdkEvent *ev = gdk_event_new (type);

  ev->any.window = g_object_ref (wrapper);
  ev->any.send_event = FALSE;
  gdk_event_set_screen (ev, gdk_drawable_get_screen (GDK_DRAWABLE (wrapper)));

  return ev;
}

static void
queue_unmap (GdkWindow *wrapper)
{
  GdkEvent *ev;

  if (!window_alive (wrapper))
    return;
  if (!(((GdkWindowObject *) wrapper)->event_mask & GDK_STRUCTURE_MASK))
    return;

  ev = new_event (wrapper, GDK_UNMAP);
  gdk_event_put (ev);
  gdk_event_free (ev);
}

static void
queue_configure (GdkWindow *wrapper, gint x, gint y, gint w, gint h)
{
  GdkEvent *ev;

  if (!window_alive (wrapper))
    return;
  if (!(((GdkWindowObject *) wrapper)->event_mask & GDK_STRUCTURE_MASK))
    return;

  ev = new_event (wrapper, GDK_CONFIGURE);
  ev->configure.x = x;
  ev->configure.y = y;
  ev->configure.width = w;
  ev->configure.height = h;
  gdk_event_put (ev);
  gdk_event_free (ev);
}

static guint
effective_version (PopupDisplay *pd, GDKWDisplayWayland *display)
{
  if (pd != NULL && pd->force_version != 0)
    return pd->force_version;
  if (display->xdg_wm_base == NULL)
    return 0;

  return xdg_wm_base_get_version (display->xdg_wm_base);
}

static gboolean
is_fallback (GDKWWindowImplWayland *impl)
{
  return g_object_get_data (G_OBJECT (impl), FALLBACK_KEY) != NULL;
}

static void
set_fallback (GDKWWindowImplWayland *impl, gboolean value)
{
  g_object_set_data (G_OBJECT (impl), FALLBACK_KEY,
                     value ? GINT_TO_POINTER (1) : NULL);
}

static void
arm_idle (PopupDisplay *pd);

/* =========================================================================
 * D1: role choice
 * ========================================================================= */

GDKWRole
_gdk_wayland_window_choose_role (GdkWindow *window, GDKWNoRole *reason)
{
  GdkWindowObject *priv;

  if (reason)
    *reason = GDKW_NOROLE_NONE;

  if (window == NULL || !GDK_IS_WINDOW (window))
    return GDKW_ROLE_NONE;

  priv = (GdkWindowObject *) window;

  if (priv->window_type == GDK_WINDOW_ROOT ||
      priv->window_type == GDK_WINDOW_FOREIGN ||
      priv->window_type == GDK_WINDOW_OFFSCREEN)
    return GDKW_ROLE_NONE;

  if (priv->input_only)
    {
      if (reason)
        *reason = GDKW_NOROLE_INPUT_ONLY;
      return GDKW_ROLE_NONE;
    }

  if (priv->window_type == GDK_WINDOW_CHILD)
    {
      if (reason)
        *reason = GDKW_NOROLE_NATIVE_CHILD;
      return GDKW_ROLE_NONE;
    }

  if (_gdk_wayland_window_get_type_hint (window) == GDK_WINDOW_TYPE_HINT_DND)
    {
      if (reason)
        *reason = GDKW_NOROLE_DND;
      return GDKW_ROLE_NONE;
    }

  if (priv->window_type == GDK_WINDOW_TEMP ||
      _gdk_wayland_window_get_override_redirect (window))
    return GDKW_ROLE_POPUP;

  return GDKW_ROLE_TOPLEVEL;
}

/* =========================================================================
 * Family stack helpers
 * ========================================================================= */

/* Topmost live popup of the family of @root, else @root itself.  With
 * @grabbing_only, popups that did not ask for xdg_popup.grab are skipped: the
 * parent of a grabbing popup must be a toplevel or a grabbing popup. */
static GDKWWindowImplWayland *
topmost_of_family (PopupDisplay *pd, GDKWWindowImplWayland *root,
                   gboolean grabbing_only)
{
  GList *l;

  for (l = g_list_last (pd->stack); l != NULL; l = l->prev)
    {
      GDKWPopup *q = l->data;

      if (q->root == root && !q->releasing &&
          (!grabbing_only || q->grab_sent))
        return q->impl;
    }

  return root;
}

static void
snap_free (GArray *snap)
{
  guint i;

  if (snap == NULL)
    return;
  for (i = 0; i < snap->len; i++)
    g_object_unref (g_array_index (snap, SnapItem, i).impl);
  g_array_free (snap, TRUE);
}

/* Items of the family of @root, topmost first.  With @above_of set, only the
 * popups above it in the stack.  With @include_self (needs @above_of), that
 * popup comes last. */
static GArray *
snap_family (PopupDisplay *pd,
             GDKWWindowImplWayland *root,
             GDKWPopup *above_of,
             gboolean include_self)
{
  GArray *snap = g_array_new (FALSE, FALSE, sizeof (SnapItem));
  GList *l;
  SnapItem item;

  for (l = g_list_last (pd->stack); l != NULL; l = l->prev)
    {
      GDKWPopup *q = l->data;

      if (above_of != NULL && q == above_of)
        break;
      if (q->root != root)
        continue;

      item.impl = g_object_ref (q->impl);
      item.popup = q;
      g_array_append_val (snap, item);
    }

  if (above_of != NULL && include_self)
    {
      item.impl = g_object_ref (above_of->impl);
      item.popup = above_of;
      g_array_append_val (snap, item);
    }

  return snap;
}

/* =========================================================================
 * Release machinery (D9)
 * ========================================================================= */

/* Destroys the protocol objects of the role and frees the record.  The
 * popups above must be gone already.  @unregister removes the impl from
 * display->windows (a hide); the re-create path keeps it. */
static void
popup_free (GDKWPopup *p, gboolean unregister, gboolean notify_seat)
{
  GDKWWindowImplWayland *impl = p->impl;
  GDKWDisplayWayland *display = p->display;
  PopupDisplay *pd = get_pd (display, FALSE);

  if (pd != NULL)
    {
      pd->stack = g_list_remove (pd->stack, p);
      pd->pending = g_list_remove (pd->pending, p);
    }
  p->in_stack = FALSE;
  p->in_pending = FALSE;

  if (notify_seat)
    _gdk_wayland_seat_surface_gone (display, impl);

  if (p->xdg_popup)
    {
      xdg_popup_destroy (p->xdg_popup);
      p->xdg_popup = NULL;
    }
  if (impl->xdg_surface)
    {
      xdg_surface_destroy (impl->xdg_surface);
      _gdk_wayland_ledger_xdg (FALSE, FALSE);
      impl->xdg_surface = NULL;
    }
  if (impl->wl_surface)
    {
      _gdk_wayland_scale_surface_destroy (impl);   /* M10 */
      wl_surface_destroy (impl->wl_surface);
      impl->wl_surface = NULL;
    }
  _gdk_wayland_window_impl_resize_buffer (impl, impl->width, impl->height);
  impl->configured = FALSE;

  if (unregister)
    display->windows = g_slist_remove (display->windows, impl);

  impl->popup = NULL;
  g_free (p);
}

static void
popup_dismiss (GDKWPopup *p, gboolean hide_windows);

static void
dismiss_snap (GArray *snap, gboolean hide_windows)
{
  guint i;

  for (i = 0; i < snap->len; i++)
    {
      SnapItem *item = &g_array_index (snap, SnapItem, i);

      if (item->impl->popup == item->popup &&
          item->popup->in_stack && !item->popup->releasing)
        popup_dismiss (item->popup, hide_windows);
    }
}

static void
release_above (PopupDisplay *pd, GDKWPopup *p, gboolean hide_windows)
{
  GArray *snap;

  if (!p->in_stack || p->root == NULL)
    return;

  snap = snap_family (pd, p->root, p, FALSE);
  dismiss_snap (snap, hide_windows);
  snap_free (snap);
}

/* Releases @p and everything above it.  @hide_windows: the GDK windows are
 * hidden through gdk_window_hide (); otherwise only the role is released. */
static void
popup_dismiss (GDKWPopup *p, gboolean hide_windows)
{
  GDKWWindowImplWayland *impl = p->impl;
  GdkWindow *wrapper = impl->wrapper;
  PopupDisplay *pd = get_pd (p->display, FALSE);

  if (p->releasing || pd == NULL)
    return;

  p->releasing = TRUE;
  g_object_ref (impl);
  if (wrapper != NULL)
    g_object_ref (wrapper);

  release_above (pd, p, hide_windows);

  if (hide_windows && impl->popup == p && window_mapped (wrapper))
    gdk_window_hide (wrapper);    /* re-enters _gdk_wayland_popup_hide () */

  if (impl->popup == p)
    popup_free (p, TRUE, TRUE);

  if (wrapper != NULL)
    g_object_unref (wrapper);
  g_object_unref (impl);
}

/* Role objects of a fallback toplevel (a candidate that found no parent). */
static void
fallback_release (GDKWWindowImplWayland *impl)
{
  GDKWDisplayWayland *display = impl_display (impl);

  if (display != NULL)
    _gdk_wayland_seat_surface_gone (display, impl);

  if (impl->xdg_toplevel)
    {
      xdg_toplevel_destroy (impl->xdg_toplevel);
      _gdk_wayland_ledger_xdg (TRUE, FALSE);
      impl->xdg_toplevel = NULL;
    }
  if (impl->xdg_surface)
    {
      xdg_surface_destroy (impl->xdg_surface);
      _gdk_wayland_ledger_xdg (FALSE, FALSE);
      impl->xdg_surface = NULL;
    }
  if (impl->wl_surface)
    {
      _gdk_wayland_scale_surface_destroy (impl);   /* M10 */
      wl_surface_destroy (impl->wl_surface);
      impl->wl_surface = NULL;
    }
  _gdk_wayland_window_impl_resize_buffer (impl, impl->width, impl->height);
  impl->configured = FALSE;
  if (display != NULL)
    display->windows = g_slist_remove (display->windows, impl);
  set_fallback (impl, FALSE);
}

/* D9 (c) */
void
_gdk_wayland_popup_release_children (GDKWWindowImplWayland *impl,
                                     gboolean hide_windows)
{
  GDKWDisplayWayland *display = impl_display (impl);
  PopupDisplay *pd = get_pd (display, FALSE);
  GArray *snap;

  if (impl == NULL || pd == NULL)
    return;

  if (impl->popup != NULL)
    {
      if (!impl->popup->in_stack || impl->popup->root == NULL)
        return;
      snap = snap_family (pd, impl->popup->root, impl->popup, FALSE);
    }
  else
    snap = snap_family (pd, impl, NULL, FALSE);

  dismiss_snap (snap, hide_windows);
  snap_free (snap);
}

void
_gdk_wayland_popup_teardown (GDKWWindowImplWayland *impl)
{
  GDKWDisplayWayland *display = impl_display (impl);
  PopupDisplay *pd = get_pd (display, FALSE);
  GDKWPopup *p;

  if (impl == NULL)
    return;

  if (pd != NULL)
    _gdk_wayland_popup_release_children (impl, FALSE);

  p = impl->popup;
  if (p == NULL)
    return;

  if (pd != NULL)
    {
      pd->stack = g_list_remove (pd->stack, p);
      pd->pending = g_list_remove (pd->pending, p);
    }
  /* with a live display the proxy is destroyed even when the ledger is
   * gone; without one the connection is closed and the proxy is not ours */
  if (display != NULL && p->xdg_popup)
    xdg_popup_destroy (p->xdg_popup);
  p->xdg_popup = NULL;
  impl->popup = NULL;
  g_free (p);
}

void
_gdk_wayland_popup_display_finalize (GdkDisplay *gdk_display)
{
  GDKWDisplayWayland *display;
  PopupDisplay *pd;

  if (gdk_display == NULL || !GDKW_IS_DISPLAY_WAYLAND (gdk_display))
    return;

  display = GDKW_DISPLAY_WAYLAND (gdk_display);
  pd = get_pd (display, FALSE);
  if (pd == NULL)
    return;

  if (pd->commit_source != 0)
    {
      g_source_remove (pd->commit_source);
      pd->commit_source = 0;
    }

  while (pd->stack != NULL)
    {
      GDKWPopup *p = g_list_last (pd->stack)->data;

      popup_free (p, TRUE, FALSE);
    }
  while (pd->pending != NULL)
    {
      GDKWPopup *p = pd->pending->data;

      popup_free (p, TRUE, FALSE);
    }

  g_object_set_data (G_OBJECT (display), POPUP_DISPLAY_KEY, NULL);
}

/* Role objects go, the record stays and goes back to PENDING (D8 retry, D10
 * old compositor, transient change). */
static void
popup_recreate (GDKWPopup *p)
{
  GDKWWindowImplWayland *impl = p->impl;
  GDKWDisplayWayland *display = p->display;
  PopupDisplay *pd = get_pd (display, FALSE);

  if (pd == NULL || p->releasing || p->xdg_popup == NULL)
    return;

  g_object_ref (impl);
  p->releasing = TRUE;
  release_above (pd, p, TRUE);
  p->releasing = FALSE;

  if (impl->popup != p)
    {
      g_object_unref (impl);
      return;
    }

  pd->stack = g_list_remove (pd->stack, p);
  p->in_stack = FALSE;

  _gdk_wayland_seat_surface_gone (display, impl);

  xdg_popup_destroy (p->xdg_popup);
  p->xdg_popup = NULL;
  if (impl->xdg_surface)
    {
      xdg_surface_destroy (impl->xdg_surface);
      _gdk_wayland_ledger_xdg (FALSE, FALSE);
      impl->xdg_surface = NULL;
    }
  if (impl->wl_surface)
    {
      _gdk_wayland_scale_surface_destroy (impl);   /* M10 */
      wl_surface_destroy (impl->wl_surface);
      impl->wl_surface = NULL;
    }
  _gdk_wayland_window_impl_resize_buffer (impl, impl->width, impl->height);
  impl->configured = FALSE;

  p->parent = NULL;
  p->root = NULL;
  p->depth = 0;
  p->have_conf = FALSE;
  p->state = GDKW_POPUP_PENDING;

  if (display->wl_compositor != NULL)
    {
      impl->wl_surface = wl_compositor_create_surface (display->wl_compositor);
      if (impl->wl_surface != NULL)
        _gdk_wayland_scale_surface_created (impl);   /* M10 */
    }

  if (impl->wl_surface != NULL)
    {
      pd->pending = g_list_append (pd->pending, p);
      p->in_pending = TRUE;
      arm_idle (pd);
    }
  else
    popup_free (p, TRUE, FALSE);

  g_object_unref (impl);
}

/* =========================================================================
 * D3: parent choice
 * ========================================================================= */

static gboolean
parent_usable (GDKWPopup *self, GDKWWindowImplWayland *c)
{
  if (c == NULL || c == self->impl)
    return FALSE;
  if (impl_display (c) != self->display)
    return FALSE;
  if (!window_mapped (c->wrapper))
    return FALSE;
  if (c->xdg_surface == NULL || c->wl_surface == NULL)
    return FALSE;

  if (c->popup != NULL)
    return !c->popup->releasing &&
           c->popup->state != GDKW_POPUP_PENDING &&
           c->popup->xdg_popup != NULL;

  return c->configured;
}

static GDKWWindowImplWayland *
normalise_candidate (PopupDisplay *pd, GDKWPopup *self,
                     GDKWWindowImplWayland *c, gboolean wants_grab)
{
  GDKWWindowImplWayland *root = c->popup != NULL ? c->popup->root : c;
  GDKWWindowImplWayland *top;

  if (root == NULL)
    return c;

  top = topmost_of_family (pd, root, wants_grab);
  if (top != root && top != c && parent_usable (self, top))
    return top;

  /* A grabbing popup may not hang from a popup without a grab (a tooltip,
   * a popup retried without grab): xdg-shell makes that a protocol error.
   * Go down to the family root. */
  if (wants_grab && c->popup != NULL && !c->popup->grab_sent &&
      top == root && parent_usable (self, root))
    return root;

  return c;
}

static GDKWWindowImplWayland *
choose_parent (GDKWPopup *p, gboolean *from_transient, gboolean wants_grab)
{
  GDKWDisplayWayland *display = p->display;
  PopupDisplay *pd = get_pd (display, TRUE);
  GDKWSeat *seat = _gdk_wayland_display_get_seat (GDK_DISPLAY_OBJECT (display));
  GDKWWindowImplWayland *cand;
  GdkWindow *tr;
  GSList *l;
  guint i;

  *from_transient = FALSE;

  tr = _gdk_wayland_window_get_transient_for (p->impl->wrapper);
  cand = tr != NULL ? _gdk_wayland_window_get_impl (tr) : NULL;
  if (parent_usable (p, cand))
    {
      *from_transient = TRUE;
      return cand;
    }

  {
    GDKWWindowImplWayland *list[3];

    list[0] = seat->pointer_focus;
    list[1] = seat->keyboard_focus;
    list[2] = seat->last_pointer_impl;

    for (i = 0; i < G_N_ELEMENTS (list); i++)
      if (parent_usable (p, list[i]))
        return normalise_candidate (pd, p, list[i], wants_grab);
  }

  for (l = display->windows; l != NULL; l = l->next)
    {
      GDKWWindowImplWayland *c = l->data;

      if (c->xdg_toplevel != NULL && parent_usable (p, c))
        return normalise_candidate (pd, p, c, wants_grab);
    }

  return NULL;
}

/* =========================================================================
 * D4 positioner, D7 grab policy
 * ========================================================================= */

static struct xdg_positioner *
make_positioner (GDKWPopup *p, gint w, gint h, gint ax, gint ay)
{
  PopupDisplay *pd = get_pd (p->display, TRUE);
  struct xdg_positioner *pos;

  if (w < 1)
    w = 1;
  if (h < 1)
    h = 1;

  pos = xdg_wm_base_create_positioner (p->display->xdg_wm_base);
  pd->positioners_created++;

  xdg_positioner_set_size (pos, w, h);
  xdg_positioner_set_anchor_rect (pos, ax, ay, 1, 1);
  xdg_positioner_set_anchor (pos, XDG_POSITIONER_ANCHOR_TOP_LEFT);
  xdg_positioner_set_gravity (pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
  xdg_positioner_set_offset (pos, 0, 0);
  xdg_positioner_set_constraint_adjustment
    (pos, XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
          XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);

  p->pos_width = w;
  p->pos_height = h;
  p->anchor_x = ax;
  p->anchor_y = ay;
  p->anchor_w = 1;
  p->anchor_h = 1;
  p->anchor = XDG_POSITIONER_ANCHOR_TOP_LEFT;
  p->gravity = XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT;
  p->offset_x = 0;
  p->offset_y = 0;
  p->constraint_adjustment = XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
                             XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y;

  return pos;
}

static void
destroy_positioner (GDKWPopup *p, struct xdg_positioner *pos)
{
  PopupDisplay *pd = get_pd (p->display, TRUE);

  xdg_positioner_destroy (pos);
  pd->positioners_destroyed++;
}

/* D7 without the parent rule: would this popup ask for a grab at all? */
static gboolean
grab_policy_base (GDKWPopup *p, guint32 *serial_out)
{
  GDKWDisplayWayland *display = p->display;
  GdkDisplay *gd = GDK_DISPLAY_OBJECT (display);
  PopupDisplay *pd = get_pd (display, TRUE);
  GDKWSeat *seat = _gdk_wayland_display_get_seat (gd);
  GdkWindowTypeHint hint;
  GdkPointerGrabInfo *grab;
  guint32 serial;

  if (p->no_grab)
    return FALSE;

  hint = _gdk_wayland_window_get_type_hint (p->impl->wrapper);
  if (hint != GDK_WINDOW_TYPE_HINT_POPUP_MENU &&
      hint != GDK_WINDOW_TYPE_HINT_DROPDOWN_MENU &&
      hint != GDK_WINDOW_TYPE_HINT_COMBO)
    return FALSE;

  if (!pd->virtual_seat)
    {
      if (display->wl_seat == NULL ||
          !(seat->capabilities & (WL_SEAT_CAPABILITY_POINTER |
                                  WL_SEAT_CAPABILITY_KEYBOARD)))
        return FALSE;
    }

  grab = _gdk_display_get_last_pointer_grab (gd);
  if (!((grab != NULL && !grab->implicit && grab->serial_end == G_MAXULONG) ||
        gd->keyboard_grab.window != NULL))
    return FALSE;

  if (seat->buttons_down != 0)
    serial = seat->button_serial;
  else
    serial = ((gint32) (seat->key_serial - seat->button_serial) > 0)
      ? seat->key_serial : seat->button_serial;

  if (serial == 0)
    return FALSE;

  *serial_out = serial;

  return TRUE;
}

/* The parent rule of xdg_popup.grab: the parent of a grabbing popup is an
 * xdg_toplevel or another popup that itself has an explicit grab.  Anything
 * else is a protocol error in strict compositors, so no grab is asked. */
static gboolean
parent_can_hold_grab (GDKWWindowImplWayland *parent)
{
  if (parent == NULL)
    return FALSE;
  if (parent->popup == NULL)
    return parent->xdg_toplevel != NULL;

  return parent->popup->grab_sent && !parent->popup->releasing;
}

static gboolean
grab_policy (GDKWPopup *p, GDKWWindowImplWayland *parent, guint32 *serial_out)
{
  if (!parent_can_hold_grab (parent))
    return FALSE;

  return grab_policy_base (p, serial_out);
}

/* =========================================================================
 * xdg_popup listener
 * ========================================================================= */

static void
apply_geometry (GDKWPopup *p, gint nx, gint ny, gint nw, gint nh)
{
  GDKWWindowImplWayland *impl = p->impl;
  PopupDisplay *pd = get_pd (p->display, FALSE);
  GdkWindowObject *priv = (GdkWindowObject *) impl->wrapper;
  gboolean size_changed, changed;
  GList *l;

  if (nw < 1)
    nw = 1;
  if (nh < 1)
    nh = 1;

  size_changed = (impl->width != (guint) nw || impl->height != (guint) nh);
  if (size_changed)
    _gdk_wayland_window_impl_resize_buffer (impl, nw, nh);
  impl->x = nx;
  impl->y = ny;

  if (priv != NULL && window_alive (impl->wrapper))
    {
      changed = priv->x != nx || priv->y != ny ||
                priv->width != nw || priv->height != nh;
      priv->x = nx;
      priv->y = ny;
      priv->width = nw;
      priv->height = nh;

      if (changed)
        {
          _gdk_window_update_size (impl->wrapper);
          if (size_changed)
            gdk_window_invalidate_rect (impl->wrapper, NULL, TRUE);
          queue_configure (impl->wrapper, nx, ny, nw, nh);
        }
    }

  /* descendants keep their offset to this popup */
  if (pd != NULL)
    for (l = pd->stack; l != NULL; l = l->next)
      {
        GDKWPopup *c = l->data;

        if (c->parent == impl && c != p)
          apply_geometry (c, nx + c->rel_x, ny + c->rel_y,
                          c->impl->width, c->impl->height);
      }
}

void
_gdk_wayland_popup_surface_configure (GDKWWindowImplWayland *impl,
                                      struct xdg_surface *xdg_surface,
                                      guint32 serial)
{
  GDKWPopup *p = impl != NULL ? impl->popup : NULL;
  gint w, h, ox, oy;

  if (p == NULL)
    return;

  w = impl->width;
  h = impl->height;
  if (p->have_conf)
    {
      if (p->conf_w > 0)
        w = p->conf_w;
      if (p->conf_h > 0)
        h = p->conf_h;
    }

  if (serial != 0 && xdg_surface != NULL)
    {
      xdg_surface_set_window_geometry (xdg_surface, 0, 0, w, h);
      xdg_surface_ack_configure (xdg_surface, serial);
      p->acked_configures++;
      impl->configured = TRUE;
    }

  if (p->xdg_popup != NULL)
    p->state = GDKW_POPUP_MAPPED;

  if (p->have_conf)
    {
      p->rel_x = p->conf_x;
      p->rel_y = p->conf_y;
      p->have_conf = FALSE;
    }

  if (p->parent != NULL)
    impl_origin (p->parent, &ox, &oy);
  else
    ox = oy = 0;

  apply_geometry (p, ox + p->rel_x, oy + p->rel_y, w, h);
}

static void
xdg_popup_handle_configure (void *data, struct xdg_popup *xdg_popup,
                            int32_t x, int32_t y, int32_t width, int32_t height)
{
  GDKWWindowImplWayland *impl = data;
  GDKWPopup *p = impl != NULL ? impl->popup : NULL;

  if (p == NULL)
    return;

  p->have_conf = TRUE;
  p->conf_x = x;
  p->conf_y = y;
  p->conf_w = width;
  p->conf_h = height;
  p->configure_events++;
}

/* The family had the keyboard focus when the popup was committed and has not
 * now.  A compositor ends the popups of a toplevel that loses the keyboard
 * focus (sway: view_close_popups).  That popup_done is no refusal of the grab,
 * and a popup created again without a grab would stay on the screen with no
 * grab while GTK still thinks it holds one. */
static gboolean
kbd_focus_lost_since_commit (GDKWPopup *p)
{
  GDKWSeat *seat = _gdk_wayland_display_get_seat (GDK_DISPLAY_OBJECT (p->display));

  return p->kbd_focus_at_commit &&
    (seat->keyboard_focus == NULL ||
     _gdk_wayland_popup_family_root (seat->keyboard_focus) != p->root);
}

static void
xdg_popup_handle_popup_done (void *data, struct xdg_popup *xdg_popup)
{
  GDKWWindowImplWayland *impl = data;
  GDKWPopup *p = impl != NULL ? impl->popup : NULL;
  PopupDisplay *pd;
  GDKWDisplayWayland *display;
  GdkDisplay *gd;
  GArray *snap;
  gboolean had_grab, parent_grabs;
  guint i;

  if (p == NULL || p->releasing)
    return;

  display = p->display;
  pd = get_pd (display, FALSE);
  if (pd == NULL)
    return;

  p->popup_done_events++;

  /* D8.1: refused before the first configure, grab was part of the request.
   * Not when the parent lost the keyboard focus since the commit: then the
   * compositor closes the popup, and the chain below hides it and breaks the
   * GDK grabs, so that GTK closes its menu. */
  if (p->state == GDKW_POPUP_COMMITTED && p->grab_sent && !p->retried &&
      !kbd_focus_lost_since_commit (p))
    {
      p->retried = TRUE;
      p->no_grab = TRUE;
      popup_recreate (p);
      return;
    }

  /* D8.2: the chain */
  if (!p->in_stack || p->root == NULL)
    {
      /* not a committed popup: just hide it */
      if (window_mapped (impl->wrapper))
        {
          GdkWindow *wrapper = g_object_ref (impl->wrapper);

          gdk_window_hide (wrapper);
          queue_unmap (wrapper);
          g_object_unref (wrapper);
        }
      return;
    }

  snap = snap_family (pd, p->root, p, TRUE);

  /* D8.3: GDK grabs go before the windows.  The chain held the grab if any
   * member asked for one whose parent is not a live grabbing popup outside
   * the chain.  Looking only at @p would miss a lower popup that gets
   * popup_done while a grabbing popup sits above it. */
  had_grab = FALSE;
  for (i = 0; i < snap->len; i++)
    {
      GDKWPopup *q = g_array_index (snap, SnapItem, i).popup;
      GDKWPopup *qp;
      gboolean in_chain = FALSE;
      guint j;

      if (!q->grab_sent)
        continue;

      qp = (q->parent != NULL) ? q->parent->popup : NULL;
      for (j = 0; qp != NULL && j < snap->len; j++)
        if (g_array_index (snap, SnapItem, j).popup == qp)
          in_chain = TRUE;

      parent_grabs = qp != NULL && !in_chain &&
                     qp->grab_sent && !qp->releasing;
      if (!parent_grabs)
        had_grab = TRUE;
    }

  if (had_grab)
    {
      gulong serial;
      GDKWSeat *seat;

      gd = GDK_DISPLAY_OBJECT (display);
      seat = _gdk_wayland_display_get_seat (gd);
      serial = _gdk_windowing_window_get_next_serial (gd);
      _gdk_display_end_pointer_grab (gd, serial, NULL, TRUE);
      _gdk_display_pointer_grab_update (gd, serial);
      _gdk_wayland_seat_grab_clear (seat);
      if (gd->keyboard_grab.window != NULL)
        _gdk_display_unset_has_keyboard_grab (gd, TRUE);
    }

  /* D8.4: topmost first */
  for (i = 0; i < snap->len; i++)
    {
      SnapItem *item = &g_array_index (snap, SnapItem, i);
      GDKWWindowImplWayland *it = item->impl;
      GdkWindow *wrapper = it->wrapper != NULL ? g_object_ref (it->wrapper)
                                               : NULL;
      gboolean was_mapped = window_mapped (wrapper);

      if (it->popup == item->popup)
        {
          item->popup->releasing = TRUE;
          if (was_mapped)
            gdk_window_hide (wrapper);
          if (it->popup == item->popup)
            popup_free (item->popup, TRUE, TRUE);
        }

      if (was_mapped)
        queue_unmap (wrapper);

      if (wrapper != NULL)
        g_object_unref (wrapper);
    }

  snap_free (snap);
}

static void
xdg_popup_handle_repositioned (void *data, struct xdg_popup *xdg_popup,
                               uint32_t token)
{
  GDKWWindowImplWayland *impl = data;
  GDKWPopup *p = impl != NULL ? impl->popup : NULL;

  if (p != NULL)
    p->repositioned_token = token;
}

static const struct xdg_popup_listener xdg_popup_listener = {
  xdg_popup_handle_configure,
  xdg_popup_handle_popup_done,
  xdg_popup_handle_repositioned
};

/* =========================================================================
 * D5: deferred first commit
 * ========================================================================= */

static void
popup_fallback (GDKWPopup *p)
{
  GDKWWindowImplWayland *impl = p->impl;
  PopupDisplay *pd = get_pd (p->display, FALSE);

  if (pd != NULL)
    {
      pd->stack = g_list_remove (pd->stack, p);
      pd->pending = g_list_remove (pd->pending, p);
    }
  impl->popup = NULL;
  g_free (p);

  set_fallback (impl, TRUE);
  if (impl->xdg_surface == NULL && impl->wl_surface != NULL)
    _gdk_wayland_window_impl_create_toplevel (impl);
}

static void
commit_one (PopupDisplay *pd, GDKWPopup *p)
{
  GDKWWindowImplWayland *impl = p->impl;
  GDKWDisplayWayland *display = p->display;
  GDKWWindowImplWayland *parent;
  struct xdg_positioner *pos;
  gboolean from_transient, wants_grab;
  gint ox, oy;
  guint32 serial = 0;

  pd->pending = g_list_remove (pd->pending, p);
  p->in_pending = FALSE;

  if (!window_mapped (impl->wrapper) || impl->wl_surface == NULL ||
      impl->xdg_surface != NULL)
    {
      popup_free (p, TRUE, TRUE);
      return;
    }

  g_object_ref (impl);

  {
    guint32 unused;

    wants_grab = grab_policy_base (p, &unused);
  }
  parent = choose_parent (p, &from_transient, wants_grab);
  if (parent == NULL)
    {
      popup_fallback (p);
      g_object_unref (impl);
      return;
    }

  if (from_transient)
    {
      GArray *snap;

      /* an explicit transient parent dismisses the popups above it */
      if (parent->popup != NULL && parent->popup->in_stack &&
          parent->popup->root != NULL)
        snap = snap_family (pd, parent->popup->root, parent->popup, FALSE);
      else
        snap = snap_family (pd, parent, NULL, FALSE);
      dismiss_snap (snap, TRUE);
      snap_free (snap);

      if (impl->popup != p)
        {
          g_object_unref (impl);
          return;
        }
      if (!window_mapped (impl->wrapper) || impl->wl_surface == NULL)
        {
          popup_free (p, TRUE, TRUE);
          g_object_unref (impl);
          return;
        }
      if (!parent_usable (p, parent))
        {
          popup_fallback (p);
          g_object_unref (impl);
          return;
        }
    }

  p->parent = parent;
  p->root = parent->popup != NULL ? parent->popup->root : parent;
  p->depth = parent->popup != NULL ? parent->popup->depth + 1 : 0;

  {
    GDKWSeat *seat = _gdk_wayland_display_get_seat (GDK_DISPLAY_OBJECT (display));

    p->kbd_focus_at_commit = seat->keyboard_focus != NULL &&
      _gdk_wayland_popup_family_root (seat->keyboard_focus) == p->root;
  }

  impl_origin (parent, &ox, &oy);
  p->rel_x = impl->x - ox;
  p->rel_y = impl->y - oy;

  impl->xdg_surface = xdg_wm_base_get_xdg_surface (display->xdg_wm_base,
                                                   impl->wl_surface);
  _gdk_wayland_ledger_xdg (FALSE, TRUE);
  xdg_surface_add_listener (impl->xdg_surface,
                            _gdk_wayland_window_impl_xdg_surface_listener (),
                            impl);

  pos = make_positioner (p, impl->width, impl->height, p->rel_x, p->rel_y);
  p->xdg_popup = xdg_surface_get_popup (impl->xdg_surface,
                                        parent->xdg_surface, pos);
  p->get_popup_requests++;
  destroy_positioner (p, pos);
  xdg_popup_add_listener (p->xdg_popup, &xdg_popup_listener, impl);

  /* D7 */
  p->grab_wanted = FALSE;
  p->grab_sent = FALSE;
  p->grab_virtual = FALSE;
  p->grab_serial = 0;
  if (grab_policy (p, parent, &serial))
    {
      p->grab_wanted = TRUE;
      p->grab_serial = serial;
      p->grab_sent = TRUE;
      GDKW_WIRE_LOG (GDKW_TEST_WIRE_POPUP_GRAB, p->xdg_popup, serial,
                     pd->virtual_seat != FALSE, 0, 0);
      if (pd->virtual_seat)
        p->grab_virtual = TRUE;
      else
        xdg_popup_grab (p->xdg_popup, display->wl_seat, serial);
    }

  /* D11, after the stored shapes of M7 */
  _gdk_wayland_shape_surface_created (impl);
  p->input_region_empty = FALSE;
  {
    GdkWindowTypeHint hint = _gdk_wayland_window_get_type_hint (impl->wrapper);

    if (hint == GDK_WINDOW_TYPE_HINT_TOOLTIP ||
        hint == GDK_WINDOW_TYPE_HINT_NOTIFICATION)
      {
        GdkRegion *input = _gdk_wayland_windowing_window_get_input_shape
          (impl->wrapper);

        if (input == NULL)
          {
            struct wl_region *region =
              wl_compositor_create_region (display->wl_compositor);

            wl_surface_set_input_region (impl->wl_surface, region);
            wl_region_destroy (region);
            p->input_region_empty = TRUE;
          }
        else
          gdk_region_destroy (input);
      }
  }

  wl_surface_commit (impl->wl_surface);

  p->state = GDKW_POPUP_COMMITTED;
  pd->stack = g_list_append (pd->stack, p);
  p->in_stack = TRUE;

  g_object_unref (impl);
}

void
_gdk_wayland_popup_commit_pending (GDKWDisplayWayland *display)
{
  PopupDisplay *pd = get_pd (display, FALSE);
  GArray *snap;
  GList *l;
  guint i;

  if (pd == NULL)
    return;

  if (pd->commit_source != 0)
    {
      g_source_remove (pd->commit_source);
      pd->commit_source = 0;
    }

  if (pd->committing)
    {
      if (pd->pending != NULL)
        arm_idle (pd);
      return;
    }

  if (pd->pending == NULL)
    return;

  pd->committing = TRUE;

  snap = g_array_new (FALSE, FALSE, sizeof (SnapItem));
  for (l = pd->pending; l != NULL; l = l->next)
    {
      GDKWPopup *q = l->data;
      SnapItem item;

      item.impl = g_object_ref (q->impl);
      item.popup = q;
      g_array_append_val (snap, item);
    }

  for (i = 0; i < snap->len; i++)
    {
      SnapItem *item = &g_array_index (snap, SnapItem, i);

      if (item->impl->popup == item->popup && item->popup->in_pending &&
          item->popup->state == GDKW_POPUP_PENDING)
        commit_one (pd, item->popup);
    }
  snap_free (snap);

  pd->committing = FALSE;

  if (pd->pending != NULL)
    arm_idle (pd);
}

static gboolean
commit_idle_cb (gpointer data)
{
  PopupDisplay *pd = data;

  pd->commit_source = 0;
  _gdk_wayland_popup_commit_pending (pd->display);

  return G_SOURCE_REMOVE;
}

static void
arm_idle (PopupDisplay *pd)
{
  if (pd->commit_source == 0)
    pd->commit_source = g_idle_add_full (GDKW_POPUP_COMMIT_PRIORITY,
                                         commit_idle_cb, pd, NULL);
}

/* =========================================================================
 * show, hide, move_resize, transient, origin
 * ========================================================================= */

gboolean
_gdk_wayland_popup_show (GdkWindow *window)
{
  GDKWWindowImplWayland *impl = _gdk_wayland_window_get_impl (window);
  GDKWDisplayWayland *display;
  PopupDisplay *pd;
  GDKWPopup *p;
  GDKWRole role;

  if (impl == NULL)
    return FALSE;

  role = _gdk_wayland_window_choose_role (window, NULL);
  if (role == GDKW_ROLE_NONE)
    return TRUE;
  if (role == GDKW_ROLE_TOPLEVEL)
    return FALSE;

  /* POPUP candidate */
  if (impl->popup != NULL)
    return TRUE;
  if (impl->wl_surface != NULL)
    return FALSE;               /* fallback toplevel: already shown */

  display = impl_display (impl);
  if (display == NULL || display->wl_compositor == NULL)
    return FALSE;

  pd = get_pd (display, TRUE);

  impl->wl_surface = wl_compositor_create_surface (display->wl_compositor);
  if (impl->wl_surface == NULL)
    return TRUE;
  _gdk_wayland_scale_surface_created (impl);   /* M10 */

  if (g_slist_find (display->windows, impl) == NULL)
    display->windows = g_slist_prepend (display->windows, impl);

  set_fallback (impl, FALSE);

  p = g_new0 (GDKWPopup, 1);
  p->impl = impl;
  p->display = display;
  p->state = GDKW_POPUP_PENDING;
  impl->popup = p;

  pd->pending = g_list_append (pd->pending, p);
  p->in_pending = TRUE;
  arm_idle (pd);

  return TRUE;
}

gboolean
_gdk_wayland_popup_hide (GdkWindow *window)
{
  GDKWWindowImplWayland *impl = _gdk_wayland_window_get_impl (window);

  if (impl == NULL)
    return TRUE;

  if (impl->popup != NULL)
    {
      GDKWPopup *p = impl->popup;
      PopupDisplay *pd = get_pd (p->display, FALSE);

      if (pd != NULL)
        release_above (pd, p, FALSE);
      if (impl->popup == p)
        popup_free (p, TRUE, TRUE);
      return TRUE;
    }

  if (is_fallback (impl))
    {
      fallback_release (impl);
      return TRUE;
    }

  if (_gdk_wayland_window_choose_role (window, NULL) == GDKW_ROLE_NONE &&
      impl->wl_surface == NULL)
    return TRUE;

  return FALSE;
}

static void
send_reposition (GDKWPopup *p)
{
  GDKWWindowImplWayland *impl = p->impl;
  PopupDisplay *pd = get_pd (p->display, TRUE);
  struct xdg_positioner *pos;
  gint ox, oy;

  impl_origin (p->parent, &ox, &oy);
  pos = make_positioner (p, impl->width, impl->height,
                         impl->x - ox, impl->y - oy);
  pd->token++;
  p->reposition_token = pd->token;
  p->reposition_requests++;
  xdg_popup_reposition (p->xdg_popup, pos, pd->token);
  destroy_positioner (p, pos);
}

void
_gdk_wayland_popup_move_resize (GdkWindow *window,
                                gboolean with_move,
                                gint x, gint y,
                                gint width, gint height)
{
  GDKWWindowImplWayland *impl = _gdk_wayland_window_get_impl (window);
  GdkWindowObject *priv = (GdkWindowObject *) window;
  GDKWPopup *p;
  PopupDisplay *pd;
  gint nx, ny, nw, nh, ox, oy;
  gboolean same;

  if (impl == NULL || impl->popup == NULL || !window_alive (window))
    return;

  p = impl->popup;
  pd = get_pd (p->display, TRUE);

  nx = with_move ? x : priv->x;
  ny = with_move ? y : priv->y;
  nw = priv->width;
  nh = priv->height;
  if (!(width < 0 && height < 0))
    {
      nw = width < 1 ? 1 : width;
      nh = height < 1 ? 1 : height;
    }

  /* an override redirect window holds the values it was given */
  if (impl->width != (guint) nw || impl->height != (guint) nh)
    _gdk_wayland_window_impl_resize_buffer (impl, nw, nh);
  priv->x = nx;
  priv->y = ny;
  priv->width = nw;
  priv->height = nh;
  impl->x = nx;
  impl->y = ny;

  if (p->state == GDKW_POPUP_PENDING || p->xdg_popup == NULL || p->releasing)
    return;

  impl_origin (p->parent, &ox, &oy);
  same = p->pos_width == nw && p->pos_height == nh &&
         p->anchor_x == nx - ox && p->anchor_y == ny - oy;
  if (same)
    {
      /* the compositor may have corrected the position since */
      if (p->state == GDKW_POPUP_MAPPED)
        {
          impl->x = priv->x = ox + p->rel_x;
          impl->y = priv->y = oy + p->rel_y;
        }
      return;
    }

  if (effective_version (pd, p->display) >= 3)
    send_reposition (p);
  else
    popup_recreate (p);
}

void
_gdk_wayland_popup_transient_changed (GdkWindow *window)
{
  GDKWWindowImplWayland *impl = _gdk_wayland_window_get_impl (window);
  GDKWPopup *p;
  GdkWindow *tr;
  GDKWWindowImplWayland *cand;
  GList *l;
  PopupDisplay *pd;

  if (impl == NULL || impl->popup == NULL)
    return;

  p = impl->popup;
  if (p->state == GDKW_POPUP_PENDING || p->xdg_popup == NULL || p->releasing)
    return;

  tr = _gdk_wayland_window_get_transient_for (window);
  cand = tr != NULL ? _gdk_wayland_window_get_impl (tr) : NULL;
  if (cand == NULL || cand == p->parent || !parent_usable (p, cand))
    return;

  /* never parent a popup to one of its own descendants */
  pd = get_pd (p->display, FALSE);
  if (pd == NULL)
    return;
  for (l = pd->stack; l != NULL; l = l->next)
    {
      GDKWPopup *q = l->data;
      GDKWWindowImplWayland *a;

      for (a = q->parent; a != NULL; a = a->popup ? a->popup->parent : NULL)
        if (a == impl && q->impl == cand)
          return;
    }

  popup_recreate (p);
}

gboolean
_gdk_wayland_popup_root_origin (GDKWWindowImplWayland *impl,
                                gint *ox, gint *oy)
{
  if (ox)
    *ox = 0;
  if (oy)
    *oy = 0;

  if (impl == NULL || impl->popup == NULL)
    return FALSE;

  if (ox)
    *ox = impl->x;
  if (oy)
    *oy = impl->y;

  return TRUE;
}

GDKWWindowImplWayland *
_gdk_wayland_popup_family_root (GDKWWindowImplWayland *impl)
{
  GDKWDisplayWayland *display = impl_display (impl);

  if (display == NULL || g_slist_find (display->windows, impl) == NULL)
    return NULL;

  if (impl->popup != NULL && impl->popup->root != NULL)
    return impl->popup->root;

  return impl;
}

/* The window of the family of @focus that holds the root point (@rx, @ry),
 * topmost popup first, then the base toplevel.  NULL when no window of the
 * family holds it.  Popups with an empty input region (tooltips) never hold
 * the pointer. */
GDKWWindowImplWayland *
_gdk_wayland_popup_family_hit (GDKWWindowImplWayland *focus,
                               gdouble rx, gdouble ry)
{
  GDKWWindowImplWayland *root = _gdk_wayland_popup_family_root (focus);
  PopupDisplay *pd = get_pd (impl_display (focus), FALSE);
  GList *l;

  if (root == NULL)
    return NULL;

  if (pd != NULL)
    for (l = g_list_last (pd->stack); l != NULL; l = l->prev)
      {
        GDKWPopup *p = l->data;
        GdkWindowObject *w;

        if (p->root != root || p->state != GDKW_POPUP_MAPPED ||
            p->input_region_empty || !window_mapped (p->impl->wrapper))
          continue;

        w = GDK_WINDOW_OBJECT (p->impl->wrapper);
        if (rx >= p->impl->x && rx < p->impl->x + w->width &&
            ry >= p->impl->y && ry < p->impl->y + w->height)
          return p->impl;
      }

  if (window_alive (root->wrapper))
    {
      GdkWindowObject *w = GDK_WINDOW_OBJECT (root->wrapper);

      if (rx >= 0 && rx < w->width && ry >= 0 && ry < w->height)
        return root;
    }

  return NULL;
}

gboolean
_gdk_wayland_popup_family_active (GDKWWindowImplWayland *impl)
{
  GDKWDisplayWayland *display = impl_display (impl);
  PopupDisplay *pd = get_pd (display, FALSE);
  GList *l;

  if (impl == NULL)
    return FALSE;
  if (impl->popup != NULL)
    return TRUE;
  if (pd == NULL)
    return FALSE;

  for (l = pd->stack; l != NULL; l = l->next)
    if (((GDKWPopup *) l->data)->root == impl)
      return TRUE;

  return FALSE;
}

/* =========================================================================
 * Test hooks
 * ========================================================================= */

gboolean
_gdk_wayland_test_popup_commit (GdkDisplay *display)
{
  if (display == NULL || !GDKW_IS_DISPLAY_WAYLAND (display))
    return FALSE;

  _gdk_wayland_popup_commit_pending (GDKW_DISPLAY_WAYLAND (display));

  return TRUE;
}

gboolean
_gdk_wayland_test_role_info (GdkWindow *window, GDKWTestRoleInfo *info)
{
  GdkWindowObject *wobj;
  GDKWWindowImplWayland *impl;

  if (info == NULL)
    return FALSE;
  memset (info, 0, sizeof *info);

  if (window == NULL || !GDK_IS_WINDOW (window))
    return FALSE;
  wobj = (GdkWindowObject *) window;
  if (wobj->impl == NULL || !GDKW_IS_WINDOW_IMPL_WAYLAND (wobj->impl))
    return FALSE;
  impl = GDKW_WINDOW_IMPL_WAYLAND (wobj->impl);

  info->own_impl = (wobj->impl_window == wobj);
  if (!info->own_impl)
    {
      info->role = GDKW_TEST_ROLE_NONE;
      info->norole_reason = GDKW_TEST_NOROLE_CLIENT_SIDE;
      return TRUE;
    }

  info->has_wl_surface = impl->wl_surface != NULL;
  info->has_xdg_surface = impl->xdg_surface != NULL;
  info->has_xdg_toplevel = impl->xdg_toplevel != NULL;
  info->has_xdg_popup = impl->popup != NULL && impl->popup->xdg_popup != NULL;
  info->configured = impl->configured;
  info->fallback_toplevel = is_fallback (impl);

  if (info->has_xdg_popup)
    {
      info->role = GDKW_TEST_ROLE_POPUP;
      info->norole_reason = GDKW_TEST_NOROLE_HAS_ROLE;
    }
  else if (info->has_xdg_toplevel)
    {
      info->role = GDKW_TEST_ROLE_TOPLEVEL;
      info->norole_reason = GDKW_TEST_NOROLE_HAS_ROLE;
    }
  else
    {
      GDKWNoRole nr = GDKW_NOROLE_NONE;

      info->role = GDKW_TEST_ROLE_NONE;
      info->norole_reason = GDKW_TEST_NOROLE_NOT_SHOWN;
      if (impl->popup == NULL)
        {
          _gdk_wayland_window_choose_role (window, &nr);
          switch (nr)
            {
            case GDKW_NOROLE_INPUT_ONLY:
              info->norole_reason = GDKW_TEST_NOROLE_INPUT_ONLY;
              break;
            case GDKW_NOROLE_DND:
              info->norole_reason = GDKW_TEST_NOROLE_DND;
              break;
            case GDKW_NOROLE_NATIVE_CHILD:
              info->norole_reason = GDKW_TEST_NOROLE_NATIVE_CHILD;
              break;
            default:
              break;
            }
        }
    }

  return TRUE;
}

gboolean
_gdk_wayland_test_popup_info (GdkWindow *window, GDKWTestPopupInfo *info)
{
  GdkWindowObject *wobj;
  GDKWWindowImplWayland *impl;
  GDKWPopup *p;

  if (info == NULL)
    return FALSE;
  memset (info, 0, sizeof *info);

  if (window == NULL || !GDK_IS_WINDOW (window))
    return FALSE;
  wobj = (GdkWindowObject *) window;
  if (wobj->impl == NULL || !GDKW_IS_WINDOW_IMPL_WAYLAND (wobj->impl))
    return FALSE;
  impl = GDKW_WINDOW_IMPL_WAYLAND (wobj->impl);

  info->state = GDKW_TEST_POPUP_NONE;
  p = (wobj->impl_window == wobj) ? impl->popup : NULL;
  if (p == NULL)
    return TRUE;

  switch (p->state)
    {
    case GDKW_POPUP_PENDING:
      info->state = GDKW_TEST_POPUP_PENDING;
      break;
    case GDKW_POPUP_COMMITTED:
      info->state = GDKW_TEST_POPUP_COMMITTED;
      break;
    default:
      info->state = GDKW_TEST_POPUP_MAPPED;
      break;
    }

  info->parent = p->parent != NULL ? p->parent->wrapper : NULL;
  info->depth = p->depth;
  info->root_x = impl->x;
  info->root_y = impl->y;

  info->pos_width = p->pos_width;
  info->pos_height = p->pos_height;
  info->anchor_x = p->anchor_x;
  info->anchor_y = p->anchor_y;
  info->anchor_w = p->anchor_w;
  info->anchor_h = p->anchor_h;
  info->anchor = p->anchor;
  info->gravity = p->gravity;
  info->constraint_adjustment = p->constraint_adjustment;
  info->offset_x = p->offset_x;
  info->offset_y = p->offset_y;
  info->reactive = FALSE;
  info->get_popup_requests = p->get_popup_requests;
  info->reposition_requests = p->reposition_requests;
  info->reposition_token = p->reposition_token;
  info->repositioned_token = p->repositioned_token;

  info->grab_wanted = p->grab_wanted;
  info->grab_sent = p->grab_sent;
  info->grab_virtual = p->grab_virtual;
  info->grab_serial = p->grab_serial;
  info->retried_without_grab = p->retried;

  info->conf_x = p->conf_x;
  info->conf_y = p->conf_y;
  info->conf_w = p->conf_w;
  info->conf_h = p->conf_h;
  info->configure_events = p->configure_events;
  info->acked_configures = p->acked_configures;
  info->popup_done_events = p->popup_done_events;
  info->input_region_empty = p->input_region_empty;

  return TRUE;
}

/* [0] created, [1] destroyed */
static guint ledger_xdg_surfaces[2];
static guint ledger_xdg_toplevels[2];

void
_gdk_wayland_ledger_xdg (gboolean toplevel, gboolean created)
{
  (toplevel ? ledger_xdg_toplevels : ledger_xdg_surfaces)[created ? 0 : 1]++;
}

gboolean
_gdk_wayland_test_object_counts (GdkDisplay *gdk_display,
                                 GDKWTestObjectCounts *counts)
{
  GDKWDisplayWayland *display;
  PopupDisplay *pd;
  GSList *l;

  if (counts == NULL)
    return FALSE;
  memset (counts, 0, sizeof *counts);

  if (gdk_display == NULL || !GDKW_IS_DISPLAY_WAYLAND (gdk_display))
    return FALSE;

  display = GDKW_DISPLAY_WAYLAND (gdk_display);
  pd = get_pd (display, FALSE);

  for (l = display->windows; l != NULL; l = l->next)
    {
      GDKWWindowImplWayland *impl = l->data;

      if (impl->wl_surface)
        counts->wl_surfaces++;
      if (impl->xdg_surface)
        counts->xdg_surfaces++;
      if (impl->xdg_toplevel)
        counts->xdg_toplevels++;
      if (impl->popup != NULL && impl->popup->xdg_popup)
        counts->xdg_popups++;
    }
  counts->registered_windows = g_slist_length (display->windows);

  if (pd != NULL)
    {
      counts->popups_pending = g_list_length (pd->pending);
      counts->popup_stack = g_list_length (pd->stack);
      counts->positioners_created = pd->positioners_created;
      counts->positioners_destroyed = pd->positioners_destroyed;
      counts->commit_sources = pd->commit_source != 0 ? 1 : 0;
    }

  counts->xdg_surfaces_created = ledger_xdg_surfaces[0];
  counts->xdg_surfaces_destroyed = ledger_xdg_surfaces[1];
  counts->xdg_toplevels_created = ledger_xdg_toplevels[0];
  counts->xdg_toplevels_destroyed = ledger_xdg_toplevels[1];

  counts->xdg_wm_base_version = display->xdg_wm_base != NULL
    ? xdg_wm_base_get_version (display->xdg_wm_base) : 0;

  return TRUE;
}

guint
_gdk_wayland_test_popup_stack (GdkDisplay *gdk_display,
                               GdkWindow **out,
                               guint max)
{
  PopupDisplay *pd;
  GList *l;
  guint n = 0;

  if (gdk_display == NULL || !GDKW_IS_DISPLAY_WAYLAND (gdk_display))
    return 0;
  pd = get_pd (GDKW_DISPLAY_WAYLAND (gdk_display), FALSE);
  if (pd == NULL)
    return 0;

  for (l = pd->stack; l != NULL; l = l->next)
    {
      GDKWPopup *p = l->data;

      if (out != NULL && n < max)
        out[n] = p->impl->wrapper;
      n++;
    }

  return n;
}

static GDKWWindowImplWayland *
hook_popup_impl (GdkWindow *window)
{
  GDKWWindowImplWayland *impl = _gdk_wayland_window_get_impl (window);

  if (impl == NULL || impl->popup == NULL || impl->popup->xdg_popup == NULL)
    return NULL;

  return impl;
}

gboolean
_gdk_wayland_test_popup_configure (GdkWindow *window,
                                   gint x, gint y, gint width, gint height)
{
  GDKWWindowImplWayland *impl = hook_popup_impl (window);

  if (impl == NULL)
    return FALSE;

  xdg_popup_handle_configure (impl, impl->popup->xdg_popup,
                              x, y, width, height);
  _gdk_wayland_popup_surface_configure (impl, impl->xdg_surface, 0);
  if (impl->configured)
    _gdk_wayland_window_impl_present (impl);

  return TRUE;
}

gboolean
_gdk_wayland_test_popup_done (GdkWindow *window)
{
  GDKWWindowImplWayland *impl = hook_popup_impl (window);

  if (impl == NULL)
    return FALSE;

  xdg_popup_handle_popup_done (impl, impl->popup->xdg_popup);

  return TRUE;
}

gboolean
_gdk_wayland_test_popup_repositioned (GdkWindow *window, guint32 token)
{
  GDKWWindowImplWayland *impl = hook_popup_impl (window);

  if (impl == NULL)
    return FALSE;

  xdg_popup_handle_repositioned (impl, impl->popup->xdg_popup, token);

  return TRUE;
}

gboolean
_gdk_wayland_test_popup_virtual_seat (GdkDisplay *gdk_display, gboolean enable)
{
  PopupDisplay *pd;

  if (gdk_display == NULL || !GDKW_IS_DISPLAY_WAYLAND (gdk_display))
    return FALSE;
  pd = get_pd (GDKW_DISPLAY_WAYLAND (gdk_display), TRUE);
  pd->virtual_seat = enable ? TRUE : FALSE;

  return TRUE;
}

gboolean
_gdk_wayland_test_popup_force_version (GdkDisplay *gdk_display, guint version)
{
  PopupDisplay *pd;

  if (gdk_display == NULL || !GDKW_IS_DISPLAY_WAYLAND (gdk_display))
    return FALSE;
  pd = get_pd (GDKW_DISPLAY_WAYLAND (gdk_display), TRUE);
  pd->force_version = version;

  return TRUE;
}

gboolean
_gdk_wayland_test_seat_serials (GdkDisplay *gdk_display,
                                guint32 *button_serial,
                                guint32 *key_serial,
                                guint32 *last_serial)
{
  GDKWSeat *seat;

  if (gdk_display == NULL || !GDKW_IS_DISPLAY_WAYLAND (gdk_display))
    return FALSE;

  seat = _gdk_wayland_display_get_seat (gdk_display);
  if (button_serial)
    *button_serial = seat->button_serial;
  if (key_serial)
    *key_serial = seat->key_serial;
  if (last_serial)
    *last_serial = seat->last_serial;

  return TRUE;
}
