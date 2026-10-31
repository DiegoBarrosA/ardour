/*
 * gdkwindow-hints-wayland.c
 *
 * ydk-wayland fork, M6b: window manager hint functions for the Wayland
 * backend (titles, transient parents, size hints, window state requests,
 * interactive move and resize, foreign window stubs).
 *
 * Per window state lives in a private struct that is allocated on first
 * use and attached to the window impl with g_object_set_data_full ().
 * Before the first show there is no xdg_toplevel, so values are only
 * stored. gdk_window_wayland_show () calls _gdk_wayland_window_hints_apply ()
 * right after the toplevel is created, before the first commit. After show
 * a change is stored and sent at once. Hint changes never commit the
 * surface. The next present commit carries them.
 *
 * Rules for every function here: a call that does not fit the window
 * (NULL, destroyed, child, root, other backend) returns silently. There is
 * never a critical.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "config.h"

#include <string.h>

#include <wayland-client.h>

#include <ydk/gdk.h>
#include <ydk/gdkinternals.h>

#include "xdg-shell-client.h"

#include "gdkprivate-wayland.h"
#include "gdkseat-wayland.h"
#include "gdkm8-wayland.h"
#include "gdkm9-wayland.h"

/* M6B PROTOTYPES */
void _gdk_wayland_window_hints_apply (GDKWWindowImplWayland *impl);
void _gdk_wayland_window_hints_configure (GDKWWindowImplWayland *impl,
                                          const guint32 *states,
                                          gsize n_states);
void _gdk_wayland_window_hints_free (GDKWWindowImplWayland *impl);
void _gdk_wayland_window_hints_mapped (GDKWWindowImplWayland *impl);
void _gdk_wayland_window_hints_set_initial_title (GDKWWindowImplWayland *impl,
                                                 const gchar *title);

void _gdk_wayland_window_set_title (GdkWindow *window, const gchar *title);
void _gdk_wayland_window_set_role (GdkWindow *window, const gchar *role);
void _gdk_wayland_window_set_startup_id (GdkWindow *window, const gchar *startup_id);
void _gdk_wayland_window_set_transient_for (GdkWindow *window, GdkWindow *parent);
void _gdk_wayland_window_set_type_hint (GdkWindow *window, GdkWindowTypeHint hint);
GdkWindowTypeHint _gdk_wayland_window_get_type_hint (GdkWindow *window);
void _gdk_wayland_window_set_modal_hint (GdkWindow *window, gboolean modal);
void _gdk_wayland_window_set_skip_taskbar_hint (GdkWindow *window, gboolean skips_taskbar);
void _gdk_wayland_window_set_skip_pager_hint (GdkWindow *window, gboolean skips_pager);
void _gdk_wayland_window_set_urgency_hint (GdkWindow *window, gboolean urgent);
void _gdk_wayland_window_set_hints (GdkWindow *window, gint x, gint y,
                                    gint min_width, gint min_height,
                                    gint max_width, gint max_height, gint flags);
void _gdk_wayland_window_set_geometry_hints (GdkWindow *window,
                                             const GdkGeometry *geometry,
                                             GdkWindowHints geom_mask);
void _gdk_wayland_window_focus (GdkWindow *window, guint32 timestamp);
void _gdk_wayland_window_set_accept_focus (GdkWindow *window, gboolean accept_focus);
void _gdk_wayland_window_set_focus_on_map (GdkWindow *window, gboolean focus_on_map);
void _gdk_wayland_window_set_override_redirect (GdkWindow *window, gboolean override_redirect);
void _gdk_wayland_window_set_icon (GdkWindow *window, GdkWindow *icon_window,
                                   GdkPixmap *pixmap, GdkBitmap *mask);
void _gdk_wayland_window_set_icon_list (GdkWindow *window, GList *pixbufs);
void _gdk_wayland_window_set_icon_name (GdkWindow *window, const gchar *name);
void _gdk_wayland_window_iconify (GdkWindow *window);
void _gdk_wayland_window_deiconify (GdkWindow *window);
void _gdk_wayland_window_stick (GdkWindow *window);
void _gdk_wayland_window_unstick (GdkWindow *window);
void _gdk_wayland_window_maximize (GdkWindow *window);
void _gdk_wayland_window_unmaximize (GdkWindow *window);
void _gdk_wayland_window_fullscreen (GdkWindow *window);
void _gdk_wayland_window_unfullscreen (GdkWindow *window);
void _gdk_wayland_window_set_keep_above (GdkWindow *window, gboolean setting);
void _gdk_wayland_window_set_keep_below (GdkWindow *window, gboolean setting);
GdkWindow *_gdk_wayland_window_get_group (GdkWindow *window);
void _gdk_wayland_window_set_group (GdkWindow *window, GdkWindow *leader);
void _gdk_wayland_window_set_decorations (GdkWindow *window, GdkWMDecoration decorations);
gboolean _gdk_wayland_window_get_decorations (GdkWindow *window, GdkWMDecoration *decorations);
void _gdk_wayland_window_set_functions (GdkWindow *window, GdkWMFunction functions);
void _gdk_wayland_window_set_opacity (GdkWindow *window, gdouble opacity);
void _gdk_wayland_window_begin_resize_drag (GdkWindow *window, GdkWindowEdge edge,
                                            gint button, gint root_x, gint root_y,
                                            guint32 timestamp);
void _gdk_wayland_window_begin_move_drag (GdkWindow *window, gint button,
                                          gint root_x, gint root_y,
                                          guint32 timestamp);
void _gdk_wayland_window_get_root_origin (GdkWindow *window, gint *x, gint *y);
void _gdk_wayland_window_get_frame_extents (GdkWindow *window, GdkRectangle *rect);
void _gdk_wayland_window_enable_synchronized_configure (GdkWindow *window);
void _gdk_wayland_window_configure_finished (GdkWindow *window);
void _gdk_wayland_window_destroy_notify (GdkWindow *window);
GdkWindow *_gdk_wayland_window_foreign_new_for_display (GdkDisplay *display,
                                                        GdkNativeWindow anid);
GdkWindow *_gdk_wayland_window_lookup_for_display (GdkDisplay *display,
                                                   GdkNativeWindow anid);
GdkWindow *_gdk_wayland_window_lookup (GdkNativeWindow anid);

#ifdef GDK_COMPILATION
#include "gdkalias.h"
#endif

#define HINTS_KEY "ydk-wayland-hints"

/* xdg_toplevel state values (xdg-shell) */
#define XDG_STATE_MAXIMIZED   1
#define XDG_STATE_FULLSCREEN  2
#define XDG_STATE_RESIZING    3
#define XDG_STATE_ACTIVATED   4

/* A chain longer than this is treated as a cycle. */
#define MAX_PARENT_CHAIN 256

typedef struct
{
  gchar *title;
  gchar *role;
  gchar *startup_id;
  gchar *icon_name;

  GdkWindowTypeHint type_hint;
  gboolean modal;
  gboolean skip_taskbar;
  gboolean skip_pager;
  gboolean urgency;
  gboolean accept_focus;
  gboolean focus_on_map;
  gboolean override_redirect;
  gboolean keep_above;
  gboolean keep_below;
  gboolean stuck;

  gboolean has_decorations;
  GdkWMDecoration decorations;
  GdkWMFunction functions;
  gdouble opacity;
  GList *icon_list;

  /* weak pointers, cleared by GObject when the window is finalized */
  GdkWindow *transient_for;
  gboolean parent_sent;
  GdkWindow *group;

  /* size hints, 0 = unset; the pair last sent or waiting for show.  These
   * are the requested values clamped so min never exceeds a non zero max
   * (the protocol rejects that). The requested values are kept apart, so a
   * later larger max restores the original min. */
  gint min_width, min_height, max_width, max_height;
  gint req_min_width, req_min_height, req_max_width, req_max_height;

  gboolean maximized;
  gboolean fullscreen;
  gboolean iconify_pending;
  guint minimize_requests;

  guint move_requests;
  guint resize_requests;
  guint32 drag_serial;
  gboolean drag_sent;
  guint last_resize_edge;

  /* compositor states, as GDKW_TEST_STATE_* bits */
  guint configure_states;
} GDKWHints;

/*************** helpers ***************/

static void
clear_weak (GdkWindow **slot)
{
  if (*slot != NULL)
    {
      g_object_remove_weak_pointer (G_OBJECT (*slot), (gpointer *) slot);
      *slot = NULL;
    }
}

static void
set_weak (GdkWindow **slot, GdkWindow *window)
{
  clear_weak (slot);
  if (window != NULL)
    {
      *slot = window;
      g_object_add_weak_pointer (G_OBJECT (window), (gpointer *) slot);
    }
}

static void
hints_destroy (gpointer data)
{
  GDKWHints *h = data;

  if (h == NULL)
    return;

  clear_weak (&h->transient_for);
  clear_weak (&h->group);
  g_free (h->title);
  g_free (h->role);
  g_free (h->startup_id);
  g_free (h->icon_name);
  g_list_free_full (h->icon_list, g_object_unref);
  g_free (h);
}

static GDKWHints *
hints_peek (GDKWWindowImplWayland *impl)
{
  return g_object_get_data (G_OBJECT (impl), HINTS_KEY);
}

static GDKWHints *
hints_get (GDKWWindowImplWayland *impl)
{
  GDKWHints *h = hints_peek (impl);

  if (h == NULL)
    {
      h = g_new0 (GDKWHints, 1);
      h->type_hint = GDK_WINDOW_TYPE_HINT_NORMAL;
      h->accept_focus = TRUE;
      h->focus_on_map = TRUE;
      h->opacity = 1.0;
      g_object_set_data_full (G_OBJECT (impl), HINTS_KEY, h, hints_destroy);
    }

  return h;
}

/* The impl a hint call applies to, or NULL: a live toplevel or temp window
 * whose Wayland impl belongs to it. */
static GDKWWindowImplWayland *
resolve (GdkWindow *window)
{
  GDKWWindowImplWayland *impl;
  GdkWindowType type;

  if (window == NULL || !GDK_IS_WINDOW (window))
    return NULL;
  if (GDK_WINDOW_DESTROYED (window))
    return NULL;

  type = GDK_WINDOW_TYPE (window);
  if (type != GDK_WINDOW_TOPLEVEL && type != GDK_WINDOW_TEMP)
    return NULL;

  impl = _gdk_wayland_window_get_impl (window);
  if (impl == NULL || impl->wrapper != window)
    return NULL;

  return impl;
}

static GdkDisplay *
impl_display (GDKWWindowImplWayland *impl)
{
  if (impl->screen == NULL)
    return NULL;

  return gdk_screen_get_display (impl->screen);
}

/* TRUE if a request may be sent now. */
static gboolean
can_send (GDKWWindowImplWayland *impl)
{
  return impl->xdg_toplevel != NULL && impl_display (impl) != NULL;
}

static gchar *
valid_copy (const gchar *str)
{
  return g_utf8_make_valid (str ? str : "", -1);
}

static const gchar *
app_id_of (GDKWWindowImplWayland *impl)
{
  const gchar *id = impl->wmclass;

  if (id == NULL)
    id = gdk_get_program_class ();
  if (id == NULL)
    id = g_get_prgname ();

  return id;
}

/* A Wayland message may not exceed 4096 bytes on the wire.  libwayland
 * drops a longer request and logs an error, so cap strings well below that.
 * The cut never splits a multibyte character. */
#define MAX_WIRE_STRING 1024

static gchar *
wire_copy (const gchar *valid_utf8)
{
  gsize len = strlen (valid_utf8);
  gchar *copy;

  if (len < MAX_WIRE_STRING)
    return g_strdup (valid_utf8);

  copy = g_strndup (valid_utf8, MAX_WIRE_STRING - 1);
  {
    /* drop a partial trailing character */
    const gchar *valid_end;

    if (!g_utf8_validate (copy, -1, &valid_end))
      copy[valid_end - copy] = '\0';
  }

  return copy;
}

static void
send_app_id (GDKWWindowImplWayland *impl)
{
  const gchar *id = app_id_of (impl);

  if (id != NULL)
    {
      gchar *valid = g_utf8_make_valid (id, -1);
      gchar *wire = wire_copy (valid);

      xdg_toplevel_set_app_id (impl->xdg_toplevel, wire);
      g_free (wire);
      g_free (valid);
    }
}

static void
send_title (GDKWWindowImplWayland *impl, GDKWHints *h)
{
  if (h->title != NULL)
    {
      gchar *wire = wire_copy (h->title);

      xdg_toplevel_set_title (impl->xdg_toplevel, wire);
      g_free (wire);
    }
}

static void
send_sizes (GDKWWindowImplWayland *impl, GDKWHints *h)
{
  xdg_toplevel_set_min_size (impl->xdg_toplevel, h->min_width, h->min_height);
  xdg_toplevel_set_max_size (impl->xdg_toplevel, h->max_width, h->max_height);
}

/* TRUE if @window is reachable from @start by following stored parents. */
static gboolean
chain_contains (GdkWindow *start, GdkWindow *window)
{
  GdkWindow *p = start;
  gint n = 0;

  while (p != NULL)
    {
      GDKWWindowImplWayland *pi;
      GDKWHints *ph;

      if (p == window || ++n > MAX_PARENT_CHAIN)
        return TRUE;

      pi = _gdk_wayland_window_get_impl (p);
      ph = pi ? hints_peek (pi) : NULL;
      p = ph ? ph->transient_for : NULL;
    }

  return FALSE;
}

/* Sends the stored parent when the child has a toplevel and the parent is
 * mapped (configured). Clears the xdg parent when none is stored. */
static void
sync_parent (GDKWWindowImplWayland *impl, GDKWHints *h)
{
  GDKWWindowImplWayland *pi;

  if (!can_send (impl))
    return;

  if (h->transient_for == NULL)
    return;

  if (h->parent_sent)
    return;

  pi = _gdk_wayland_window_get_impl (h->transient_for);
  if (pi == NULL || pi->xdg_toplevel == NULL || !pi->configured ||
      GDK_WINDOW_DESTROYED (h->transient_for))
    return;

  if (impl_display (pi) != impl_display (impl))
    return;

  xdg_toplevel_set_parent (impl->xdg_toplevel, pi->xdg_toplevel);
  h->parent_sent = TRUE;
}

/* Sends the stored parent of every window whose parent is @parent_impl.
 * A child shown before its parent was configured would otherwise never
 * get its xdg parent. */
static void
sync_children (GDKWWindowImplWayland *parent_impl)
{
  GdkDisplay *display = impl_display (parent_impl);
  GSList *l;

  if (display == NULL || !GDKW_IS_DISPLAY_WAYLAND (display) ||
      parent_impl->wrapper == NULL)
    return;

  for (l = GDKW_DISPLAY_WAYLAND (display)->windows; l != NULL; l = l->next)
    {
      GDKWWindowImplWayland *ci = l->data;
      GDKWHints *ch = hints_peek (ci);

      if (ch != NULL && ch->transient_for == parent_impl->wrapper &&
          !ch->parent_sent)
        sync_parent (ci, ch);
    }
}

static void
send_minimize (GDKWWindowImplWayland *impl, GDKWHints *h)
{
  xdg_toplevel_set_minimized (impl->xdg_toplevel);
  h->minimize_requests++;
  h->iconify_pending = FALSE;
}

/*************** internal entry points ***************/

void
_gdk_wayland_window_hints_apply (GDKWWindowImplWayland *impl)
{
  GDKWHints *h;

  if (impl == NULL || !can_send (impl))
    return;

  send_app_id (impl);

  h = hints_peek (impl);
  if (h == NULL)
    return;

  send_title (impl, h);

  if (h->min_width || h->min_height || h->max_width || h->max_height)
    send_sizes (impl, h);

  h->parent_sent = FALSE;
  sync_parent (impl, h);

  if (h->maximized)
    xdg_toplevel_set_maximized (impl->xdg_toplevel);
  if (h->fullscreen)
    xdg_toplevel_set_fullscreen (impl->xdg_toplevel, NULL);
  if (h->iconify_pending)
    send_minimize (impl, h);
}

void
_gdk_wayland_window_hints_configure (GDKWWindowImplWayland *impl,
                                     const guint32 *states,
                                     gsize n_states)
{
  GDKWHints *h;
  guint bits = 0;
  guint changed;
  GdkWindowState set = 0, unset = 0;
  gsize i;

  if (impl == NULL)
    return;

  for (i = 0; states != NULL && i < n_states; i++)
    {
      switch (states[i])
        {
        case XDG_STATE_MAXIMIZED:
          bits |= GDKW_TEST_STATE_MAXIMIZED;
          break;
        case XDG_STATE_FULLSCREEN:
          bits |= GDKW_TEST_STATE_FULLSCREEN;
          break;
        case XDG_STATE_RESIZING:
          bits |= GDKW_TEST_STATE_RESIZING;
          break;
        case XDG_STATE_ACTIVATED:
          bits |= GDKW_TEST_STATE_ACTIVATED;
          break;
        default:
          break;
        }
    }

  h = hints_get (impl);
  changed = bits ^ h->configure_states;
  h->configure_states = bits;

  /* the parent may have been mapped since the last attempt */
  sync_parent (impl, h);

  if (changed == 0)
    return;
  if (impl->wrapper == NULL || !GDK_IS_WINDOW (impl->wrapper) ||
      GDK_WINDOW_DESTROYED (impl->wrapper))
    return;

#define TRACK(bit, flag) \
  if (changed & (bit)) { if (bits & (bit)) set |= (flag); else unset |= (flag); }
  TRACK (GDKW_TEST_STATE_MAXIMIZED, GDK_WINDOW_STATE_MAXIMIZED);
  TRACK (GDKW_TEST_STATE_FULLSCREEN, GDK_WINDOW_STATE_FULLSCREEN);
  /* ACTIVATED has no GDK2 window state (GDK_WINDOW_STATE_FOCUSED does not
   * exist). It is kept in configure_states only. */
#undef TRACK

  if (set != 0 || unset != 0)
    gdk_synthesize_window_state (impl->wrapper, unset, set);
}

void
_gdk_wayland_window_hints_mapped (GDKWWindowImplWayland *impl)
{
  GDKWHints *h;

  if (impl == NULL)
    return;

  /* The surface was just acked: it can now be a parent, and it can send
   * its own parent if that one was already mapped. */
  h = hints_peek (impl);
  if (h != NULL)
    sync_parent (impl, h);

  sync_children (impl);
}

void
_gdk_wayland_window_hints_free (GDKWWindowImplWayland *impl)
{
  if (impl == NULL)
    return;

  g_object_set_data (G_OBJECT (impl), HINTS_KEY, NULL);
}

/*************** strings and flags ***************/

/* The title a window is created with (GDK_WA_TITLE, or the default one),
 * kept until the xdg_toplevel exists.  gdk_window_new () has no impl to
 * resolve yet, so this takes the impl.  gdk_window_set_title () changes it
 * later. */
void
_gdk_wayland_window_hints_set_initial_title (GDKWWindowImplWayland *impl,
                                             const gchar *title)
{
  GDKWHints *h;

  if (impl == NULL)
    return;

  h = hints_get (impl);
  g_free (h->title);
  h->title = valid_copy (title);
}

void
_gdk_wayland_window_set_title (GdkWindow *window, const gchar *title)
{
  GDKWWindowImplWayland *impl = resolve (window);
  GDKWHints *h;

  if (impl == NULL)
    return;

  h = hints_get (impl);
  g_free (h->title);
  h->title = valid_copy (title);

  if (can_send (impl))
    send_title (impl, h);
}

#define STORE_STRING(fn, field)                                   \
void                                                              \
fn (GdkWindow *window, const gchar *value)                        \
{                                                                 \
  GDKWWindowImplWayland *impl = resolve (window);                 \
  GDKWHints *h;                                                   \
  if (impl == NULL)                                               \
    return;                                                       \
  h = hints_get (impl);                                           \
  g_free (h->field);                                              \
  h->field = value ? g_utf8_make_valid (value, -1) : NULL;        \
}

STORE_STRING (_gdk_wayland_window_set_role, role)
STORE_STRING (_gdk_wayland_window_set_startup_id, startup_id)
STORE_STRING (_gdk_wayland_window_set_icon_name, icon_name)

#undef STORE_STRING

#define STORE_BOOL(fn, field)                                     \
void                                                              \
fn (GdkWindow *window, gboolean value)                            \
{                                                                 \
  GDKWWindowImplWayland *impl = resolve (window);                 \
  if (impl == NULL)                                               \
    return;                                                       \
  hints_get (impl)->field = value ? TRUE : FALSE;                 \
}

STORE_BOOL (_gdk_wayland_window_set_modal_hint, modal)
STORE_BOOL (_gdk_wayland_window_set_skip_taskbar_hint, skip_taskbar)
STORE_BOOL (_gdk_wayland_window_set_skip_pager_hint, skip_pager)
STORE_BOOL (_gdk_wayland_window_set_urgency_hint, urgency)
STORE_BOOL (_gdk_wayland_window_set_accept_focus, accept_focus)
STORE_BOOL (_gdk_wayland_window_set_focus_on_map, focus_on_map)
STORE_BOOL (_gdk_wayland_window_set_override_redirect, override_redirect)
STORE_BOOL (_gdk_wayland_window_set_keep_above, keep_above)
STORE_BOOL (_gdk_wayland_window_set_keep_below, keep_below)

#undef STORE_BOOL

/* Sticky, ABOVE and BELOW are stored only. The window states are never
 * synthesized because the compositor does not report them. */
void
_gdk_wayland_window_stick (GdkWindow *window)
{
  GDKWWindowImplWayland *impl = resolve (window);

  if (impl != NULL)
    hints_get (impl)->stuck = TRUE;
}

void
_gdk_wayland_window_unstick (GdkWindow *window)
{
  GDKWWindowImplWayland *impl = resolve (window);

  if (impl != NULL)
    hints_get (impl)->stuck = FALSE;
}

void
_gdk_wayland_window_set_type_hint (GdkWindow *window, GdkWindowTypeHint hint)
{
  GDKWWindowImplWayland *impl = resolve (window);

  if (impl != NULL)
    hints_get (impl)->type_hint = hint;
}

GdkWindowTypeHint
_gdk_wayland_window_get_type_hint (GdkWindow *window)
{
  GDKWWindowImplWayland *impl = resolve (window);
  GDKWHints *h = impl ? hints_peek (impl) : NULL;

  return h ? h->type_hint : GDK_WINDOW_TYPE_HINT_NORMAL;
}

void
_gdk_wayland_window_set_decorations (GdkWindow *window,
                                     GdkWMDecoration decorations)
{
  GDKWWindowImplWayland *impl = resolve (window);
  GDKWHints *h;

  if (impl == NULL)
    return;

  h = hints_get (impl);
  h->decorations = decorations;
  h->has_decorations = TRUE;
}

gboolean
_gdk_wayland_window_get_decorations (GdkWindow *window,
                                     GdkWMDecoration *decorations)
{
  GDKWWindowImplWayland *impl = resolve (window);
  GDKWHints *h = impl ? hints_peek (impl) : NULL;

  if (h == NULL || !h->has_decorations)
    return FALSE;

  if (decorations != NULL)
    *decorations = h->decorations;

  return TRUE;
}

void
_gdk_wayland_window_set_functions (GdkWindow *window, GdkWMFunction functions)
{
  GDKWWindowImplWayland *impl = resolve (window);

  if (impl != NULL)
    hints_get (impl)->functions = functions;
}

void
_gdk_wayland_window_set_opacity (GdkWindow *window, gdouble opacity)
{
  GDKWWindowImplWayland *impl = resolve (window);

  if (impl == NULL)
    return;

  if (!(opacity >= 0.0))
    opacity = 0.0;
  else if (opacity > 1.0)
    opacity = 1.0;

  /* No visible effect: needs wp_alpha_modifier or a premultiplied present. */
  hints_get (impl)->opacity = opacity;
}

void
_gdk_wayland_window_set_icon_list (GdkWindow *window, GList *pixbufs)
{
  GDKWWindowImplWayland *impl = resolve (window);
  GDKWHints *h;
  GList *copy = NULL, *l, *old;

  if (impl == NULL)
    return;

  for (l = pixbufs; l != NULL; l = l->next)
    if (l->data != NULL && GDK_IS_PIXBUF (l->data))
      copy = g_list_prepend (copy, g_object_ref (l->data));
  copy = g_list_reverse (copy);

  h = hints_get (impl);
  old = h->icon_list;
  h->icon_list = copy;
  g_list_free_full (old, g_object_unref);
}

void
_gdk_wayland_window_set_icon (GdkWindow *window, GdkWindow *icon_window,
                              GdkPixmap *pixmap, GdkBitmap *mask)
{
  /* Nothing is stored: xdg-shell has no icon request. */
}

void
_gdk_wayland_window_set_group (GdkWindow *window, GdkWindow *leader)
{
  GDKWWindowImplWayland *impl = resolve (window);
  GDKWHints *h;

  if (impl == NULL)
    return;

  if (leader != NULL && !GDK_IS_WINDOW (leader))
    return;

  h = hints_get (impl);
  set_weak (&h->group, leader);
}

GdkWindow *
_gdk_wayland_window_get_group (GdkWindow *window)
{
  GDKWWindowImplWayland *impl = resolve (window);
  GDKWHints *h = impl ? hints_peek (impl) : NULL;

  return h ? h->group : NULL;
}

/*************** transient parent ***************/

void
_gdk_wayland_window_set_transient_for (GdkWindow *window, GdkWindow *parent)
{
  GDKWWindowImplWayland *impl = resolve (window);
  GDKWHints *h;

  if (impl == NULL)
    return;

  h = hints_get (impl);

  if (parent == NULL)
    {
      clear_weak (&h->transient_for);
      h->parent_sent = FALSE;
      if (can_send (impl))
        xdg_toplevel_set_parent (impl->xdg_toplevel, NULL);
      _gdk_wayland_popup_transient_changed (window);
      return;
    }

  /* Reject silently: self, a cycle, or a parent that is not a Wayland
   * toplevel. */
  if (parent == window)
    return;
  if (resolve (parent) == NULL)
    return;
  if (chain_contains (parent, window))
    return;

  set_weak (&h->transient_for, parent);
  h->parent_sent = FALSE;
  sync_parent (impl, h);

  /* M9: a live popup is re-created under the new parent (D10) */
  _gdk_wayland_popup_transient_changed (window);
}

/* M9 accessors for the popup piece (declared in gdkm9-wayland.h) */

GdkWindow *
_gdk_wayland_window_get_transient_for (GdkWindow *window)
{
  GDKWWindowImplWayland *impl = resolve (window);
  GDKWHints *h = impl ? hints_peek (impl) : NULL;

  return h ? h->transient_for : NULL;
}

gboolean
_gdk_wayland_window_get_override_redirect (GdkWindow *window)
{
  GDKWWindowImplWayland *impl = resolve (window);
  GDKWHints *h = impl ? hints_peek (impl) : NULL;

  return h ? h->override_redirect : FALSE;
}

/*************** size hints ***************/

static void
apply_size_hints (GDKWWindowImplWayland *impl,
                  const GdkGeometry *geometry,
                  GdkWindowHints mask)
{
  GDKWHints *h = hints_get (impl);

  if (geometry == NULL && mask == 0)
    {
      h->req_min_width = h->req_min_height = 0;
      h->req_max_width = h->req_max_height = 0;
    }
  else if (geometry != NULL)
    {
      if (mask & GDK_HINT_MIN_SIZE)
        {
          h->req_min_width = MAX (geometry->min_width, 0);
          h->req_min_height = MAX (geometry->min_height, 0);
        }
      if (mask & GDK_HINT_MAX_SIZE)
        {
          h->req_max_width = MAX (geometry->max_width, 0);
          h->req_max_height = MAX (geometry->max_height, 0);
        }
    }

  h->min_width = h->req_min_width;
  h->min_height = h->req_min_height;
  h->max_width = h->req_max_width;
  h->max_height = h->req_max_height;

  /* A min above a non zero max is a protocol error: clamp what is sent,
   * not what was requested. */
  if (h->min_width > 0 && h->max_width > 0 && h->min_width > h->max_width)
    h->min_width = h->max_width;
  if (h->min_height > 0 && h->max_height > 0 && h->min_height > h->max_height)
    h->min_height = h->max_height;

  if (can_send (impl))
    send_sizes (impl, h);
}

void
_gdk_wayland_window_set_geometry_hints (GdkWindow *window,
                                        const GdkGeometry *geometry,
                                        GdkWindowHints geom_mask)
{
  GDKWWindowImplWayland *impl = resolve (window);

  if (impl != NULL)
    apply_size_hints (impl, geometry, geom_mask);
}

void
_gdk_wayland_window_set_hints (GdkWindow *window, gint x, gint y,
                               gint min_width, gint min_height,
                               gint max_width, gint max_height, gint flags)
{
  GDKWWindowImplWayland *impl = resolve (window);
  GdkGeometry geometry;
  GdkWindowHints mask = 0;

  if (impl == NULL)
    return;

  memset (&geometry, 0, sizeof geometry);
  geometry.min_width = min_width;
  geometry.min_height = min_height;
  geometry.max_width = max_width;
  geometry.max_height = max_height;

  if (flags & GDK_HINT_MIN_SIZE)
    mask |= GDK_HINT_MIN_SIZE;
  if (flags & GDK_HINT_MAX_SIZE)
    mask |= GDK_HINT_MAX_SIZE;

  if (mask == 0)
    return;

  apply_size_hints (impl, &geometry, mask);
}

/*************** window state requests ***************/

void
_gdk_wayland_window_maximize (GdkWindow *window)
{
  GDKWWindowImplWayland *impl = resolve (window);

  if (impl == NULL)
    return;

  hints_get (impl)->maximized = TRUE;
  if (can_send (impl))
    xdg_toplevel_set_maximized (impl->xdg_toplevel);
}

void
_gdk_wayland_window_unmaximize (GdkWindow *window)
{
  GDKWWindowImplWayland *impl = resolve (window);

  if (impl == NULL)
    return;

  hints_get (impl)->maximized = FALSE;
  if (can_send (impl))
    xdg_toplevel_unset_maximized (impl->xdg_toplevel);
}

void
_gdk_wayland_window_fullscreen (GdkWindow *window)
{
  GDKWWindowImplWayland *impl = resolve (window);

  if (impl == NULL)
    return;

  hints_get (impl)->fullscreen = TRUE;
  if (can_send (impl))
    xdg_toplevel_set_fullscreen (impl->xdg_toplevel, NULL);
}

void
_gdk_wayland_window_unfullscreen (GdkWindow *window)
{
  GDKWWindowImplWayland *impl = resolve (window);

  if (impl == NULL)
    return;

  hints_get (impl)->fullscreen = FALSE;
  if (can_send (impl))
    xdg_toplevel_unset_fullscreen (impl->xdg_toplevel);
}

/* ICONIFIED is never synthesized: Wayland does not report it. */
void
_gdk_wayland_window_iconify (GdkWindow *window)
{
  GDKWWindowImplWayland *impl = resolve (window);
  GDKWHints *h;

  if (impl == NULL)
    return;

  h = hints_get (impl);
  if (can_send (impl))
    send_minimize (impl, h);
  else
    h->iconify_pending = TRUE;
}

/* The protocol has no unminimize request. Showing the window is the
 * closest action. */
void
_gdk_wayland_window_deiconify (GdkWindow *window)
{
  GDKWWindowImplWayland *impl = resolve (window);

  if (impl == NULL)
    return;

  hints_get (impl)->iconify_pending = FALSE;
  gdk_window_show (window);
}

/* xdg-activation is not bound, so only an unmapped window is shown. */
void
_gdk_wayland_window_focus (GdkWindow *window, guint32 timestamp)
{
  if (resolve (window) == NULL)
    return;

  if (!gdk_window_is_visible (window))
    gdk_window_show (window);
}

/*************** interactive move and resize ***************/

static void
begin_drag (GdkWindow *window, gboolean resize, guint xdg_edge)
{
  GDKWWindowImplWayland *impl;
  GDKWHints *h;
  GdkDisplay *display;
  GDKWDisplayWayland *wd;
  GDKWSeat *seat;

  if (window == NULL || !GDK_IS_WINDOW (window) ||
      GDK_WINDOW_DESTROYED (window))
    return;

  impl = resolve (gdk_window_get_toplevel (window));
  if (impl == NULL || impl->xdg_toplevel == NULL)
    return;

  display = impl_display (impl);
  if (display == NULL || !GDKW_IS_DISPLAY_WAYLAND (display))
    return;

  wd = GDKW_DISPLAY_WAYLAND (display);
  seat = _gdk_wayland_display_get_seat (display);
  /* The compositor only accepts the serial of a button press.  The last
   * button event may have been a release, so require a held button. */
  if (seat == NULL || seat->button_serial == 0 || seat->button_mask == 0)
    return;

  /* Same as X11 wmspec_moveresize: the WM takes over the pointer. */
  gdk_display_pointer_ungrab (display, GDK_CURRENT_TIME);

  h = hints_get (impl);
  if (resize)
    {
      h->resize_requests++;
      h->last_resize_edge = xdg_edge;
    }
  else
    h->move_requests++;
  h->drag_serial = seat->button_serial;
  h->drag_sent = FALSE;

  /* A headless compositor may have no wl_seat. */
  if (wd->wl_seat != NULL && wd->wl_display != NULL)
    {
      if (resize)
        xdg_toplevel_resize (impl->xdg_toplevel, wd->wl_seat,
                             seat->button_serial, xdg_edge);
      else
        xdg_toplevel_move (impl->xdg_toplevel, wd->wl_seat,
                           seat->button_serial);
      wl_display_flush (wd->wl_display);
      h->drag_sent = TRUE;
    }
}

void
_gdk_wayland_window_begin_move_drag (GdkWindow *window, gint button,
                                     gint root_x, gint root_y,
                                     guint32 timestamp)
{
  begin_drag (window, FALSE, 0);
}

void
_gdk_wayland_window_begin_resize_drag (GdkWindow *window, GdkWindowEdge edge,
                                       gint button, gint root_x, gint root_y,
                                       guint32 timestamp)
{
  guint xdg_edge;

  switch (edge)
    {
    case GDK_WINDOW_EDGE_NORTH_WEST: xdg_edge = 5; break;
    case GDK_WINDOW_EDGE_NORTH:      xdg_edge = 1; break;
    case GDK_WINDOW_EDGE_NORTH_EAST: xdg_edge = 9; break;
    case GDK_WINDOW_EDGE_WEST:       xdg_edge = 4; break;
    case GDK_WINDOW_EDGE_EAST:       xdg_edge = 8; break;
    case GDK_WINDOW_EDGE_SOUTH_WEST: xdg_edge = 6; break;
    case GDK_WINDOW_EDGE_SOUTH:      xdg_edge = 2; break;
    case GDK_WINDOW_EDGE_SOUTH_EAST: xdg_edge = 10; break;
    default:
      return;
    }

  begin_drag (window, TRUE, xdg_edge);
}

/*************** geometry queries ***************/

void
_gdk_wayland_window_get_root_origin (GdkWindow *window, gint *x, gint *y)
{
  GDKWWindowImplWayland *impl = NULL;
  gint ox = 0, oy = 0;

  /* Toplevels sit at the root origin, as in get_root_coords; a popup sits at
   * its root position (M9 D2). */
  if (window != NULL && GDK_IS_WINDOW (window) &&
      !GDK_WINDOW_DESTROYED (window))
    impl = _gdk_wayland_window_get_impl (gdk_window_get_toplevel (window));

  if (impl != NULL)
    _gdk_wayland_popup_root_origin (impl, &ox, &oy);

  if (x != NULL)
    *x = ox;
  if (y != NULL)
    *y = oy;
}

void
_gdk_wayland_window_get_frame_extents (GdkWindow *window, GdkRectangle *rect)
{
  GDKWWindowImplWayland *impl = NULL;

  if (rect == NULL)
    return;

  rect->x = rect->y = 0;
  rect->width = rect->height = 0;

  if (window != NULL && GDK_IS_WINDOW (window) &&
      !GDK_WINDOW_DESTROYED (window))
    impl = _gdk_wayland_window_get_impl (gdk_window_get_toplevel (window));

  /* No decorations are known, so the frame equals the window. */
  if (impl != NULL)
    {
      gint ox = 0, oy = 0;

      _gdk_wayland_popup_root_origin (impl, &ox, &oy);
      rect->x = ox;
      rect->y = oy;
      rect->width = impl->width;
      rect->height = impl->height;
    }
}

void
_gdk_wayland_window_enable_synchronized_configure (GdkWindow *window)
{
}

void
_gdk_wayland_window_configure_finished (GdkWindow *window)
{
}

/*************** lifetime and foreign windows ***************/

void
_gdk_wayland_window_destroy_notify (GdkWindow *window)
{
  if (window == NULL || !GDK_IS_WINDOW (window))
    return;

  if (!GDK_WINDOW_DESTROYED (window))
    _gdk_window_destroy (window, TRUE);

  g_object_unref (window);
}

/* "Native windows" are the selection piece's native ids (requestor tokens). */
GdkWindow *
_gdk_wayland_window_foreign_new_for_display (GdkDisplay *display,
                                             GdkNativeWindow anid)
{
  GdkWindow *w = _gdk_wayland_native_id_lookup (display, anid);

  return w ? g_object_ref (w) : NULL;
}

GdkWindow *
_gdk_wayland_window_lookup_for_display (GdkDisplay *display,
                                        GdkNativeWindow anid)
{
  return _gdk_wayland_native_id_lookup (display, anid);
}

GdkWindow *
_gdk_wayland_window_lookup (GdkNativeWindow anid)
{
  return _gdk_wayland_native_id_lookup (gdk_display_get_default (), anid);
}

/*************** test hook (M6b) ***************/

gboolean
_gdk_wayland_test_toplevel_info (GdkWindow *window, GDKWTestToplevelInfo *info)
{
  GDKWWindowImplWayland *impl;
  GDKWHints *h;
  const gchar *app_id;

  if (info == NULL)
    return FALSE;

  impl = (window != NULL && GDK_IS_WINDOW (window))
    ? _gdk_wayland_window_get_impl (window) : NULL;
  if (impl == NULL || impl->xdg_toplevel == NULL)
    return FALSE;

  h = hints_get (impl);
  memset (info, 0, sizeof *info);

  info->has_toplevel = TRUE;

  if (h->title != NULL)
    {
      info->has_title = TRUE;
      g_strlcpy (info->title, h->title, sizeof info->title);
    }

  app_id = app_id_of (impl);
  if (app_id != NULL)
    {
      info->has_app_id = TRUE;
      g_strlcpy (info->app_id, app_id, sizeof info->app_id);
    }

  if (h->role != NULL)
    {
      info->has_role = TRUE;
      g_strlcpy (info->role, h->role, sizeof info->role);
    }

  info->transient_for = h->transient_for;
  info->min_width = h->min_width;
  info->min_height = h->min_height;
  info->max_width = h->max_width;
  info->max_height = h->max_height;
  info->maximized = h->maximized;
  info->fullscreen = h->fullscreen;
  info->minimize_requests = h->minimize_requests;
  info->move_requests = h->move_requests;
  info->resize_requests = h->resize_requests;
  info->drag_serial = h->drag_serial;
  info->drag_sent = h->drag_sent;
  info->last_resize_edge = h->last_resize_edge;
  info->configure_states = h->configure_states;

  return TRUE;
}
