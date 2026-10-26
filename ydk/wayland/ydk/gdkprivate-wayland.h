/*
 * gdkprivate-wayland.h
 *
 * ydk-wayland fork: private Wayland backend declarations.
 *
 * The Wayland backend follows GTK2's single-backend structure, but with
 * every symbol prefixed `_gdk_wayland_` (or `gdk_wayland_`) so that it can
 * coexist with the X11 backend inside one libydk.  The dispatcher
 * (ydk/gdkbackend.c) owns the canonical `_gdk_windowing_*` names and the
 * public `gdk_display_open`, and forwards to whichever backend the
 * GDK_BACKEND environment variable selects.
 *
 * Skeleton status (see DESIGN.md / PLANNING.md): this milestone implements
 * display open/close and the internal windowing contract as safe no-ops.
 * Window creation, input, frame callbacks and fractional scaling land in
 * the M1/M2/M3 milestones; the symbols are present now so the library
 * links with both backends compiled in.
 */

#ifndef __YDK_GDKP_PRIVATE_WAYLAND_H__
#define __YDK_GDKP_PRIVATE_WAYLAND_H__

#include <glib-object.h>
#include <wayland-client.h>

#include <ydk/gdkdisplay.h>
#include <ydk/gdkscreen.h>
#include <ydk/gdkcolor.h>
#include <ydk/gdkkeys.h>
#include <ydk/gdkwindow.h>

#ifdef GDK_COMPILATION
#define GDKW_TYPE_DISPLAY_WAYLAND          (gdkw_display_wayland_get_type ())
#define GDKW_DISPLAY_WAYLAND(object)       (G_TYPE_CHECK_INSTANCE_CAST ((object), GDKW_TYPE_DISPLAY_WAYLAND, GDKWDisplayWayland))
#define GDKW_DISPLAY_WAYLAND_CLASS(klass)  (G_TYPE_CHECK_CLASS_CAST ((klass), GDKW_TYPE_DISPLAY_WAYLAND, GDKWDisplayWaylandClass))
#define GDKW_IS_DISPLAY_WAYLAND(object)    (G_TYPE_CHECK_INSTANCE_TYPE ((object), GDKW_TYPE_DISPLAY_WAYLAND))
#define GDKW_IS_DISPLAY_WAYLAND_CLASS(klass)(G_TYPE_CHECK_CLASS_TYPE ((klass), GDKW_TYPE_DISPLAY_WAYLAND))
#define GDKW_DISPLAY_WAYLAND_GET_CLASS(obj)(G_TYPE_INSTANCE_GET_CLASS ((obj), GDKW_TYPE_DISPLAY_WAYLAND, GDKWDisplayWaylandClass))

#define GDKW_TYPE_SCREEN_WAYLAND           (gdkw_screen_wayland_get_type ())
#define GDKW_SCREEN_WAYLAND(object)        (G_TYPE_CHECK_INSTANCE_CAST ((object), GDKW_TYPE_SCREEN_WAYLAND, GDKWScreenWayland))
#define GDKW_SCREEN_WAYLAND_CLASS(klass)   (G_TYPE_CHECK_CLASS_CAST ((klass), GDKW_TYPE_SCREEN_WAYLAND, GDKWScreenWaylandClass))
#define GDKW_IS_SCREEN_WAYLAND(object)     (G_TYPE_CHECK_INSTANCE_TYPE ((object), GDKW_TYPE_SCREEN_WAYLAND))
#define GDKW_IS_SCREEN_WAYLAND_CLASS(klass)(G_TYPE_CHECK_CLASS_TYPE ((klass), GDKW_TYPE_SCREEN_WAYLAND))
#define GDKW_SCREEN_WAYLAND_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS ((obj), GDKW_TYPE_SCREEN_WAYLAND, GDKWScreenWaylandClass))

#define GDKW_TYPE_WINDOW_IMPL_WAYLAND      (gdkw_window_impl_wayland_get_type ())
#define GDKW_WINDOW_IMPL_WAYLAND(obj)      (G_TYPE_CHECK_INSTANCE_CAST ((obj), GDKW_TYPE_WINDOW_IMPL_WAYLAND, GDKWWindowImplWayland))
#define GDKW_WINDOW_IMPL_WAYLAND_CLASS(klass)(G_TYPE_CHECK_CLASS_CAST ((klass), GDKW_TYPE_WINDOW_IMPL_WAYLAND, GDKWWindowImplWaylandClass))
#define GDKW_IS_WINDOW_IMPL_WAYLAND(object)(G_TYPE_CHECK_INSTANCE_TYPE ((object), GDKW_TYPE_WINDOW_IMPL_WAYLAND))
#define GDKW_IS_WINDOW_IMPL_WAYLAND_CLASS(klass)(G_TYPE_CHECK_CLASS_TYPE ((klass), GDKW_TYPE_WINDOW_IMPL_WAYLAND))
#define GDKW_WINDOW_IMPL_WAYLAND_GET_CLASS(obj)(G_TYPE_INSTANCE_GET_CLASS ((obj), GDKW_TYPE_WINDOW_IMPL_WAYLAND, GDKWWindowImplWaylandClass))
#endif /* GDK_COMPILATION */

typedef struct _GDKWDisplayWayland GDKWDisplayWayland;
typedef struct _GDKWDisplayWaylandClass GDKWDisplayWaylandClass;
typedef struct _GDKWScreenWayland GDKWScreenWayland;
typedef struct _GDKWScreenWaylandClass GDKWScreenWaylandClass;
typedef struct _GDKWWindowImplWayland GDKWWindowImplWayland;
typedef struct _GDKWWindowImplWaylandClass GDKWWindowImplWaylandClass;

struct _GDKWDisplayWayland
{
  GdkDisplay parent_instance;

  /*< private >*/
  struct wl_display *wl_display;
  struct wl_registry *wl_registry;
  struct wl_output *wl_output;
  guint32 serial;

  /* M1: the single logical screen, tracked from the wl_output that the
   * registry walk below found.  Wayland has no per-screen objects; the
   * whole display is one screen. */
  GdkScreen *default_screen;
  gint output_width;
  gint output_height;

  /* M2: shm + xdg-shell globals; every toplevel impl registers itself here
   * so display sync can present them before roundtripping.  Root windows
   * are not in the list (they have no wl_surface). */
  struct wl_compositor *wl_compositor;
  struct wl_shm *wl_shm;
  struct xdg_wm_base *xdg_wm_base;
  GSList *windows;
  gboolean xdg_shell_supported;

  /* M3/M6: seat proxies.  wl_seat is bound at up to v5 (gdkevents-wayland.c)
   * and wl_pointer/wl_keyboard are created/destroyed from the capabilities
   * callback.  All other seat state (focus, position, buttons, modifiers,
   * key repeat, grab record) lives in GDKWSeat, see gdkseat-wayland.h. */
  struct wl_seat *wl_seat;
  struct wl_pointer *wl_pointer;
  struct wl_keyboard *wl_keyboard;

  /* M3: per-display keymap; created lazily and filled from the wl_keyboard
   * keymap event (see gdkkeys-wayland.c). */
  GdkKeymap *keymap;

  /* M6b: the socket name this display really connected to (the open
   * argument, else $WAYLAND_DISPLAY, else libwayland's default
   * "wayland-0").  Used for the display name and for spawned children. */
  gchar *socket_name;
};

struct _GDKWDisplayWaylandClass
{
  GdkDisplayClass parent_class;
};

struct _GDKWScreenWayland
{
  GdkScreen parent_instance;

  /*< private >*/
  GdkDisplay *display;
  gint width;
  gint height;

  /* M2: hand-built TRUE_COLOR system visual/colormap, plus the real root
   * window whose GdkWindowImplWayland has no wl_surface (window creation
   * never targets it; it is the parent for toplevels). */
  GdkVisual *system_visual;
  GdkColormap *system_colormap;
  GdkWindow *root_window;
};

struct _GDKWScreenWaylandClass
{
  GdkScreenClass parent_class;
};

/* M2 window implementation.  Must subclass GdkDrawable (the core
 * get_colormap path in gdkwindow.c dispatches gdk_drawable_get_colormap()
 * off the impl drawable) and implement the GdkWindowImpl interface. */
typedef struct xdg_surface xdg_surface;
typedef struct xdg_toplevel xdg_toplevel;

struct _GDKWWindowImplWayland
{
  GdkDrawable parent_instance;

  /*< private >*/
  GdkScreen *screen;
  GdkColormap *colormap;

  /* The GdkWindowObject we implement + its geometry (kept in sync by
   * move_resize/get_geometry). */
  GdkWindow *wrapper;
  gint x, y;
  guint width, height;
  GdkEventMask events;
  GdkColor background;

  /* wl objects; NULL until a toplevel is shown. */
  struct wl_surface *wl_surface;
  xdg_surface *xdg_surface;
  xdg_toplevel *xdg_toplevel;
  struct wl_buffer *wl_buffer;

  /* M3: wmclass from the creation attributes; surfaced to the compositor
   * as the xdg_toplevel app id the first time the window is shown. */
  gchar *wmclass;

  /* shm backing: the cairo image surface used for drawing is created
   * directly over this buffer (zero-copy presents). */
  int shm_fd;
  guchar *shm_data;
  unsigned int shm_size;
  unsigned int stride;
  cairo_surface_t *backing;

  gboolean configured;
};

struct _GDKWWindowImplWaylandClass
{
  GdkDrawableClass parent_class;
};

GType gdkw_display_wayland_get_type (void) G_GNUC_CONST;
GType gdkw_screen_wayland_get_type (void) G_GNUC_CONST;
GType gdkw_window_impl_wayland_get_type (void) G_GNUC_CONST;

/********************************************************
 * Internal windowing contract, namespaced to wayland
 ********************************************************/

void     _gdk_wayland_windowing_init                    (void);
void     _gdk_wayland_windowing_exit                    (void);
extern const GOptionEntry _gdk_wayland_windowing_args[];

void     _gdk_wayland_windowing_set_default_display     (GdkDisplay *display);
gchar   *_gdk_wayland_windowing_substitute_screen_number (const gchar *display_name,
							  gint screen_number);
gulong   _gdk_wayland_windowing_window_get_next_serial  (GdkDisplay *display);
GdkRegion *_gdk_wayland_windowing_window_get_shape      (GdkWindow  *window);
GdkRegion *_gdk_wayland_windowing_window_get_input_shape(GdkWindow  *window);
GdkRegion *_gdk_wayland_windowing_get_shape_for_mask    (GdkBitmap *mask);
void     _gdk_wayland_windowing_window_beep             (GdkWindow *window);

void     _gdk_wayland_windowing_get_pointer             (GdkDisplay *display,
							GdkScreen **screen,
							gint *x, gint *y,
							GdkModifierType *mask);
GdkWindow *_gdk_wayland_windowing_window_at_pointer     (GdkDisplay *display,
							gint *win_x, gint *win_y,
							GdkModifierType *mask,
							gboolean get_toplevel);
GdkGrabStatus _gdk_wayland_windowing_pointer_grab       (GdkWindow *window,
							GdkWindow *native,
							gboolean owner_events,
							GdkEventMask event_mask,
							GdkWindow *confine_to,
							GdkCursor *cursor,
							guint32 time);
void     _gdk_wayland_windowing_window_process_updates_recurse (GdkWindow *window,
								GdkRegion *expose_region);
void     _gdk_wayland_windowing_before_process_all_updates (void);
void     _gdk_wayland_windowing_after_process_all_updates  (void);
gint     _gdk_wayland_windowing_get_bits_for_depth      (GdkDisplay *display,
							gint depth);
void     _gdk_wayland_windowing_window_destroy_foreign  (GdkWindow *window);
void     _gdk_wayland_windowing_display_set_sm_client_id (GdkDisplay *display,
							const gchar *sm_client_id);
void     _gdk_wayland_windowing_window_set_composited   (GdkWindow *window,
							gboolean composited);
void     _gdk_wayland_windowing_gc_set_clip_region      (GdkGC *gc,
							const GdkRegion *region,
							gboolean reset_origin);
void     _gdk_wayland_windowing_gc_copy                 (GdkGC *dst_gc,
							GdkGC *src_gc);
char    *_gdk_wayland_windowing_get_startup_notify_id   (GAppLaunchContext *context,
							GAppInfo *info,
							GList *files);
void     _gdk_wayland_windowing_launch_failed           (GAppLaunchContext *context,
							const char *startup_notify_id);
void     _gdk_wayland_windowing_set_cairo_surface_size  (cairo_surface_t *surface,
							int width, int height);
cairo_surface_t *_gdk_wayland_windowing_create_cairo_surface (GdkDrawable *drawable,
							      int width, int height);

/* public per-backend functions */
GdkDisplay *_gdk_wayland_display_open (const gchar *display_name);
GdkScreen  *_gdk_wayland_screen_new (GdkDisplay *display,
				     gint width, gint height);

/* M2: per-backend window impl constructor (canonical `_gdk_window_impl_new`
 * is dispatched from ydk/gdkbackend.c) and display-sync helpers. */
GdkDrawable *_gdk_wayland_window_impl_new (GdkWindow      *window,
					   GdkWindow      *real_parent,
					   GdkScreen      *screen,
					   GdkVisual      *visual,
					   GdkEventMask    event_mask,
					   GdkWindowAttr  *attributes,
					   gint            attributes_mask);
void _gdk_wayland_display_present_all (GDKWDisplayWayland *wayland_display);

/* Attaches and commits a window impl's shm buffer (creating it on demand).
 * Safe to call any number of times; no-ops until the xdg_surface has been
 * acked-configured (the first configure has to be acked before a buffer
 * may be committed). */
void _gdk_wayland_window_impl_present (GDKWWindowImplWayland *impl);

/* Releases a window impl's wl objects and shm; idempotent.  Used by the
 * impl iface destroy and by display finalize cleanup. */
void _gdk_wayland_window_impl_teardown (GDKWWindowImplWayland *impl);

/* M2: builds the hand-rolled TRUE_COLOR system visual, the colormap over
 * it, and the real root window for a screen.  Called from screen_new. */
GdkVisual *_gdk_wayland_screen_create_system_visual (void);
GdkColormap *_gdk_wayland_screen_create_colormap (GdkScreen *screen,
						  GdkVisual *visual);
GdkScreen *_gdk_wayland_visual_get_screen (GdkVisual *visual);

/* F7: colormap family (gdkcolor-wayland.c).  The Wayland colormap is a
 * private GdkColormap subclass; see the file header. */
GdkColormap *_gdk_wayland_colormap_new_for_screen (GdkScreen *screen, GdkVisual *visual);
GdkColormap *_gdk_wayland_colormap_new (GdkVisual *visual, gboolean allocate);
GdkScreen   *_gdk_wayland_colormap_get_screen (GdkColormap *cmap);
gint         _gdk_wayland_colormap_get_system_size (void);
void         _gdk_wayland_colormap_change (GdkColormap *colormap, gint ncolors);
gint         _gdk_wayland_colors_alloc (GdkColormap *colormap, gboolean contiguous,
					gulong *planes, gint nplanes,
					gulong *pixels, gint npixels);
void         _gdk_wayland_colors_free (GdkColormap *colormap, gulong *pixels,
				       gint npixels, gulong planes);
void         _gdk_wayland_colormap_free_colors (GdkColormap *colormap,
						const GdkColor *colors, gint n_colors);
gint         _gdk_wayland_colormap_alloc_colors (GdkColormap *colormap, GdkColor *colors,
						 gint n_colors, gboolean writeable,
						 gboolean best_match, gboolean *success);
void         _gdk_wayland_colormap_query_color (GdkColormap *colormap, gulong pixel,
						GdkColor *result);
gint         _gdk_wayland_color_change (GdkColormap *colormap, GdkColor *color);

/* F8: visual / default-colormap / rgba family (gdkvisual-wayland.c) */
gint          _gdk_wayland_visual_get_best_depth (void);
GdkVisualType _gdk_wayland_visual_get_best_type (void);
GdkVisual    *_gdk_wayland_visual_get_best (void);
GdkVisual    *_gdk_wayland_visual_get_best_with_depth (gint depth);
GdkVisual    *_gdk_wayland_visual_get_best_with_type (GdkVisualType visual_type);
GdkVisual    *_gdk_wayland_visual_get_best_with_both (gint depth,
						      GdkVisualType visual_type);
void          _gdk_wayland_query_depths (gint **depths, gint *count);
void          _gdk_wayland_query_visual_types (GdkVisualType **visual_types,
					       gint *count);
GList        *_gdk_wayland_screen_list_visuals (GdkScreen *screen);
GdkVisual    *_gdk_wayland_screen_get_rgba_visual (GdkScreen *screen);
GdkColormap  *_gdk_wayland_screen_get_rgba_colormap (GdkScreen *screen);
GdkColormap  *_gdk_wayland_screen_get_default_colormap (GdkScreen *screen);
void          _gdk_wayland_screen_set_default_colormap (GdkScreen *screen,
							GdkColormap *colormap);


/********************************************************
 * M3: seat input + events + keymap
 ********************************************************/

/* Per-display keymap, dispatched from core gdk_keymap_get_for_display()
 * through GdkDisplayClass::keymap.  The xkb keymap arrives asynchronously
 * via the wl_keyboard keymap event. */
GType gdkw_keymap_wayland_get_type (void) G_GNUC_CONST;
GdkKeymap *gdkw_display_wayland_get_keymap (GdkDisplay *display);

/* Fills @impl's map state from the xkb keymap handed over by the
 * compositor.  Called from the wl_keyboard keymap event listener. */
void _gdk_wayland_keymap_set_keymap (GDKWDisplayWayland *display,
				     int format,
				     int fd,
				     guint size);

/* Serialises an xkb modifier mask into GDK modifier bits.  Used by the
 * keyboard and pointer listeners in gdkevents-wayland.c. */
GdkModifierType _gdk_wayland_keymap_serialize_mods (GdkKeymap *keymap,
						    guint mask);

/* The active keyboard layout index (group) for KEY_PRESS/RELEASE events. */
gint _gdk_wayland_keymap_group (GdkKeymap *keymap);

/* The current xkb_state pointer, used by the key listener to update the
 * modifier state on key presses/releases. */
struct xkb_state *_gdk_wayland_keymap_get_xkb_state (GdkKeymap *keymap);

/* Wires the Wayland display fd into the GLib main loop (a GSource owned by
 * gdkevents-wayland.c) and binds the input globals.  Called at the end of
 * display open after the registry walk; the reverse in display finalize. */
void _gdk_wayland_events_init (GdkDisplay *display);
void _gdk_wayland_events_finalize (GdkDisplay *display);

/* Finds the toplevel impl whose surface is @surface; NULL if unknown. */
GDKWWindowImplWayland *_gdk_wayland_impl_from_surface (
    GDKWDisplayWayland *display, struct wl_surface *surface);

/********************************************************
 * M4: cursor, font, selection, drag, property (dispatched from
 * ydk/gdkbackend.c)
 ********************************************************/

/* ---- cursor (gdk---- cursor-wayland.c) */
GdkCursor *_gdk_wayland_cursor_new_for_display (GdkDisplay *display, GdkCursorType cursor_type);
GdkCursor *_gdk_wayland_cursor_new_from_pixmap (GdkPixmap *source, GdkPixmap *mask, const GdkColor *fg, const GdkColor *bg, gint x, gint y);
GdkCursor *_gdk_wayland_cursor_new_from_pixbuf (GdkDisplay *display, GdkPixbuf *pixbuf, gint x, gint y);
GdkCursor *_gdk_wayland_cursor_new_from_name (GdkDisplay *display, const gchar *name);
void _gdk_wayland_cursor_destroy (GdkCursor *cursor);
GdkDisplay *_gdk_wayland_cursor_get_display (GdkCursor *cursor);
GdkPixbuf *_gdk_wayland_cursor_get_image (GdkCursor *cursor);
gboolean _gdk_wayland_display_supports_cursor_alpha (GdkDisplay *display);
gboolean _gdk_wayland_display_supports_cursor_color (GdkDisplay *display);
guint _gdk_wayland_display_get_default_cursor_size (GdkDisplay *display);
void _gdk_wayland_display_get_maximal_cursor_size (GdkDisplay *display, guint *width, guint *height);

/* ---- font (gdk---- font-wayland.c) */
GdkDisplay *_gdk_wayland_font_get_display (GdkFont *font);
GdkFont *_gdk_wayland_font_load_for_display (GdkDisplay *display, const gchar *font_name);
GdkFont *_gdk_wayland_font_from_description_for_display (GdkDisplay *display, PangoFontDescription *font_desc);
GdkFont *_gdk_wayland_fontset_load_for_display (GdkDisplay *display, const gchar *fontset_name);
void _gdk_wayland_font_destroy (GdkFont *font);
gint _gdk_wayland_font_strlen (GdkFont *font, const gchar *str);
gint _gdk_wayland_font_id (const GdkFont *font);
gboolean _gdk_wayland_font_equal (const GdkFont *fonta, const GdkFont *fontb);
gint _gdk_wayland_text_width (GdkFont *font, const gchar *text, gint text_length);
gint _gdk_wayland_text_width_wc (GdkFont *font, const GdkWChar *text, gint text_length);
void _gdk_wayland_text_extents (GdkFont *font, const gchar *text, gint text_length, gint *lbearing, gint *rbearing, gint *width, gint *ascent, gint *descent);
void _gdk_wayland_text_extents_wc (GdkFont *font, const GdkWChar *text, gint text_length, gint *lbearing, gint *rbearing, gint *width, gint *ascent, gint *descent);

/* ---- selection (gdk---- selection-wayland.c) */
gboolean _gdk_wayland_selection_owner_set_for_display (GdkDisplay *display, GdkWindow *owner, GdkAtom selection, guint32 time, gboolean send_event);
GdkWindow *_gdk_wayland_selection_owner_get_for_display (GdkDisplay *display, GdkAtom selection);
void _gdk_wayland_selection_convert (GdkWindow *requestor, GdkAtom selection, GdkAtom target, guint32 time);
gint _gdk_wayland_selection_property_get (GdkWindow *requestor, guchar **data, GdkAtom *ret_type, gint *ret_format);
void _gdk_wayland_selection_send_notify_for_display (GdkDisplay *display, GdkNativeWindow requestor, GdkAtom selection, GdkAtom target, GdkAtom property, guint32 time);
gint _gdk_wayland_text_property_to_text_list_for_display (GdkDisplay *display, GdkAtom encoding, gint format, const guchar *text, gint length, gchar ***list);
gint _gdk_wayland_text_property_to_utf8_list_for_display (GdkDisplay *display, GdkAtom encoding, gint format, const guchar *text, gint length, gchar ***list);
void _gdk_wayland_free_text_list (gchar **list);
void _gdk_wayland_free_compound_text (guchar *ctext);
gint _gdk_wayland_string_to_compound_text_for_display (GdkDisplay *display, const gchar *str, GdkAtom *encoding, gint *format, guchar **ctext, gint *length);
gboolean _gdk_wayland_utf8_to_compound_text_for_display (GdkDisplay *display, const gchar *str, GdkAtom *encoding, gint *format, guchar **ctext, gint *length);

/* ---- drag (gdk---- drag-wayland.c) */
GdkDragContext *_gdk_wayland_drag_begin (GdkWindow *window, GList *targets);
GdkNativeWindow _gdk_wayland_drag_get_protocol_for_display (GdkDisplay *display, GdkNativeWindow xid, GdkDragProtocol *protocol);
void _gdk_wayland_drag_find_window_for_screen (GdkDragContext *context, GdkWindow *drag_window, GdkScreen *screen, gint x_root, gint y_root, GdkWindow **dest_window, GdkDragProtocol *protocol);
gboolean _gdk_wayland_drag_motion (GdkDragContext *context, GdkWindow *dest_window, GdkDragProtocol protocol, gint x_root, gint y_root, GdkDragAction suggested_action, GdkDragAction possible_actions, guint32 time_);
void _gdk_wayland_drag_drop (GdkDragContext *context, guint32 time_);
void _gdk_wayland_drag_abort (GdkDragContext *context, guint32 time_);
void _gdk_wayland_drag_status (GdkDragContext *context, GdkDragAction action, guint32 time_);
GdkAtom _gdk_wayland_drag_get_selection (GdkDragContext *context);
gboolean _gdk_wayland_drag_drop_succeeded (GdkDragContext *context);
void _gdk_wayland_drop_reply (GdkDragContext *context, gboolean ok, guint32 time_);
void _gdk_wayland_drop_finish (GdkDragContext *context, gboolean success, guint32 time_);
void _gdk_wayland_window_register_dnd (GdkWindow *window);

/* ---- property (gdk---- property-wayland.c) */
gboolean _gdk_wayland_property_get (GdkWindow *window, GdkAtom property, GdkAtom type, gulong offset, gulong length, gint pdelete, GdkAtom *actual_property_type, gint *actual_format_type, gint *actual_length, guchar **data);
void _gdk_wayland_property_change (GdkWindow *window, GdkAtom property, GdkAtom type, gint format, GdkPropMode mode, const guchar *data, gint nelements);
void _gdk_wayland_property_delete (GdkWindow *window, GdkAtom property);
GdkAtom _gdk_wayland_atom_intern (const gchar *atom_name, gboolean only_if_exists);
GdkAtom _gdk_wayland_atom_intern_static_string (const gchar *atom_name);
gchar *_gdk_wayland_atom_name (GdkAtom atom);


/********************************************************
 * M6b: keymap, display misc and toplevel hint functions
 * (dispatched from ydk/gdkbackend.c).  Each has the signature of the
 * public gdk_* function of the same name without the prefix.
 ********************************************************/

/* ---- keymap and keyval (gdkkeys-wayland.c) */
PangoDirection _gdk_wayland_keymap_get_direction (GdkKeymap *keymap);
gboolean _gdk_wayland_keymap_have_bidi_layouts (GdkKeymap *keymap);
gboolean _gdk_wayland_keymap_get_caps_lock_state (GdkKeymap *keymap);
gboolean _gdk_wayland_keymap_get_entries_for_keyval (GdkKeymap *keymap, guint keyval, GdkKeymapKey **keys, gint *n_keys);
gboolean _gdk_wayland_keymap_get_entries_for_keycode (GdkKeymap *keymap, guint hardware_keycode, GdkKeymapKey **keys, guint **keyvals, gint *n_entries);
guint _gdk_wayland_keymap_lookup_key (GdkKeymap *keymap, const GdkKeymapKey *key);
void _gdk_wayland_keymap_add_virtual_modifiers (GdkKeymap *keymap, GdkModifierType *state);
gboolean _gdk_wayland_keymap_map_virtual_modifiers (GdkKeymap *keymap, GdkModifierType *state);
gchar * _gdk_wayland_keyval_name (guint keyval);
guint _gdk_wayland_keyval_from_name (const gchar *keyval_name);
void _gdk_wayland_keyval_convert_case (guint symbol, guint *lower, guint *upper);

/* ---- display misc (gdkdisplay-misc-wayland.c) */
const gchar * _gdk_wayland_display_get_name (GdkDisplay *display);
gboolean _gdk_wayland_display_supports_shapes (GdkDisplay *display);
gboolean _gdk_wayland_display_supports_input_shapes (GdkDisplay *display);
gboolean _gdk_wayland_display_supports_composite (GdkDisplay *display);
gboolean _gdk_wayland_display_supports_selection_notification (GdkDisplay *display);
gboolean _gdk_wayland_display_supports_clipboard_persistence (GdkDisplay *display);
gboolean _gdk_wayland_display_request_selection_notification (GdkDisplay *display, GdkAtom selection);
void _gdk_wayland_display_store_clipboard (GdkDisplay *display, GdkWindow *clipboard_window, guint32 time_, const GdkAtom *targets, gint n_targets);
GdkWindow * _gdk_wayland_display_get_default_group (GdkDisplay *display);
gchar * _gdk_wayland_get_display (void);
void _gdk_wayland_notify_startup_complete (void);
void _gdk_wayland_notify_startup_complete_with_id (const gchar *startup_id);
void _gdk_wayland_add_client_message_filter (GdkAtom message_type, GdkFilterFunc func, gpointer data);
void _gdk_wayland_display_add_client_message_filter (GdkDisplay *display, GdkAtom message_type, GdkFilterFunc func, gpointer data);
gboolean _gdk_wayland_event_send_client_message_for_display (GdkDisplay *display, GdkEvent *event, GdkNativeWindow winid);
GdkEvent * _gdk_wayland_event_get_graphics_expose (GdkWindow *window);
GList * _gdk_wayland_devices_list (void);
GList * _gdk_wayland_display_list_devices (GdkDisplay *display);
gboolean _gdk_wayland_device_get_history (GdkDevice *device, GdkWindow *window, guint32 start, guint32 stop, GdkTimeCoord ***events, gint *n_events);
void _gdk_wayland_input_set_extension_events (GdkWindow *window, gint mask, GdkExtensionMode mode);
gboolean _gdk_wayland_spawn_on_screen (GdkScreen *screen, const gchar *working_directory, gchar **argv, gchar **envp, GSpawnFlags flags, GSpawnChildSetupFunc child_setup, gpointer user_data, gint *child_pid, GError **error);
gboolean _gdk_wayland_spawn_on_screen_with_pipes (GdkScreen *screen, const gchar *working_directory, gchar **argv, gchar **envp, GSpawnFlags flags, GSpawnChildSetupFunc child_setup, gpointer user_data, gint *child_pid, gint *standard_input, gint *standard_output, gint *standard_error, GError **error);
gboolean _gdk_wayland_spawn_command_line_on_screen (GdkScreen *screen, const gchar *command_line, GError **error);
gchar * _gdk_wayland_set_locale (void);
gchar * _gdk_wayland_wcstombs (const GdkWChar *src);
gint _gdk_wayland_mbstowcs (GdkWChar *dest, const gchar *src, gint dest_max);
void _gdk_wayland_error_trap_push (void);
gint _gdk_wayland_error_trap_pop (void);
gboolean _gdk_wayland_get_use_xshm (void);
GdkFont * _gdk_wayland_fontset_load (const gchar *fontset_name);

/* ---- toplevel hints (gdkwindow-hints-wayland.c) */
void _gdk_wayland_window_hints_apply (GDKWWindowImplWayland *impl);
void _gdk_wayland_window_hints_configure (GDKWWindowImplWayland *impl, const guint32 *states, gsize n_states);
void _gdk_wayland_window_hints_free (GDKWWindowImplWayland *impl);
void _gdk_wayland_window_hints_mapped (GDKWWindowImplWayland *impl);
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
void _gdk_wayland_window_set_hints (GdkWindow *window, gint x, gint y, gint min_width, gint min_height, gint max_width, gint max_height, gint flags);
void _gdk_wayland_window_set_geometry_hints (GdkWindow *window, const GdkGeometry *geometry, GdkWindowHints geom_mask);
void _gdk_wayland_window_focus (GdkWindow *window, guint32 timestamp);
void _gdk_wayland_window_set_accept_focus (GdkWindow *window, gboolean accept_focus);
void _gdk_wayland_window_set_focus_on_map (GdkWindow *window, gboolean focus_on_map);
void _gdk_wayland_window_set_override_redirect (GdkWindow *window, gboolean override_redirect);
void _gdk_wayland_window_set_icon (GdkWindow *window, GdkWindow *icon_window, GdkPixmap *pixmap, GdkBitmap *mask);
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
GdkWindow * _gdk_wayland_window_get_group (GdkWindow *window);
void _gdk_wayland_window_set_group (GdkWindow *window, GdkWindow *leader);
void _gdk_wayland_window_set_decorations (GdkWindow *window, GdkWMDecoration decorations);
gboolean _gdk_wayland_window_get_decorations (GdkWindow *window, GdkWMDecoration *decorations);
void _gdk_wayland_window_set_functions (GdkWindow *window, GdkWMFunction functions);
void _gdk_wayland_window_set_opacity (GdkWindow *window, gdouble opacity);
void _gdk_wayland_window_begin_resize_drag (GdkWindow *window, GdkWindowEdge edge, gint button, gint root_x, gint root_y, guint32 timestamp);
void _gdk_wayland_window_begin_move_drag (GdkWindow *window, gint button, gint root_x, gint root_y, guint32 timestamp);
void _gdk_wayland_window_get_root_origin (GdkWindow *window, gint *x, gint *y);
void _gdk_wayland_window_get_frame_extents (GdkWindow *window, GdkRectangle *rect);
void _gdk_wayland_window_enable_synchronized_configure (GdkWindow *window);
void _gdk_wayland_window_configure_finished (GdkWindow *window);
void _gdk_wayland_window_destroy_notify (GdkWindow *window);
GdkWindow * _gdk_wayland_window_foreign_new_for_display (GdkDisplay *display, GdkNativeWindow anid);
GdkWindow * _gdk_wayland_window_lookup_for_display (GdkDisplay *display, GdkNativeWindow anid);
GdkWindow * _gdk_wayland_window_lookup (GdkNativeWindow anid);

#endif /* __YDK_GDKP_PRIVATE_WAYLAND_H__ */