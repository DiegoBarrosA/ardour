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

#ifdef GDK_COMPILATION
#define GDKW_TYPE_DISPLAY_WAYLAND          (gdkw_display_wayland_get_type ())
#define GDKW_DISPLAY_WAYLAND(object)       (G_TYPE_CHECK_INSTANCE_CAST ((object), GDKW_TYPE_DISPLAY_WAYLAND, GDKWDisplayWayland))
#define GDKW_DISPLAY_WAYLAND_CLASS(klass)  (G_TYPE_CHECK_CLASS_CAST ((klass), GDKW_TYPE_DISPLAY_WAYLAND, GDKWDisplayWaylandClass))
#define GDKW_IS_DISPLAY_WAYLAND(object)    (G_TYPE_CHECK_INSTANCE_TYPE ((object), GDKW_TYPE_DISPLAY_WAYLAND))
#define GDKW_IS_DISPLAY_WAYLAND_CLASS(klass)(G_TYPE_CHECK_CLASS_TYPE ((klass), GDKW_TYPE_DISPLAY_WAYLAND))
#define GDKW_DISPLAY_WAYLAND_GET_CLASS(obj)(G_TYPE_INSTANCE_GET_CLASS ((obj), GDKW_TYPE_DISPLAY_WAYLAND, GDKWDisplayWaylandClass))
#endif /* GDK_COMPILATION */

typedef struct _GDKWDisplayWayland GDKWDisplayWayland;
typedef struct _GDKWDisplayWaylandClass GDKWDisplayWaylandClass;

struct _GDKWDisplayWayland
{
  GdkDisplay parent_instance;

  /*< private >*/
  struct wl_display *wl_display;
  struct wl_registry *wl_registry;
  guint32 serial;
};

struct _GDKWDisplayWaylandClass
{
  GdkDisplayClass parent_class;
};

GType gdkw_display_wayland_get_type (void) G_GNUC_CONST;

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

#endif /* __YDK_GDKP_PRIVATE_WAYLAND_H__ */