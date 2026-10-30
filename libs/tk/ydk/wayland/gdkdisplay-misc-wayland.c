/*
 * gdkdisplay-misc-wayland.c
 *
 * ydk-wayland fork, M6b (piece "display"): the remaining display level
 * public functions that used to be X11 bound.  Where Wayland has an answer
 * the answer is real.  Otherwise the function returns a safe value and
 * emits no critical on valid input.
 *
 * Symbols are namespaced _gdk_wayland_*; the dispatchers in
 * ydk/gdkbackend.c own the public gdk_* names.  No Xlib here.
 */

#undef GDK_DISABLE_DEPRECATED

#include "config.h"

#include <string.h>
#include <locale.h>

#include <glib.h>

#include <ydk/gdk.h>
#include <ydk/gdkinternals.h>
#include "gdkprivate-wayland.h"
#include "gdkseat-wayland.h"

/* M6B PROTOTYPES */
const gchar *_gdk_wayland_display_get_name (GdkDisplay *display);
gboolean _gdk_wayland_display_supports_shapes (GdkDisplay *display);
gboolean _gdk_wayland_display_supports_input_shapes (GdkDisplay *display);
gboolean _gdk_wayland_display_supports_composite (GdkDisplay *display);
gboolean _gdk_wayland_display_supports_selection_notification (GdkDisplay *display);
gboolean _gdk_wayland_display_supports_clipboard_persistence (GdkDisplay *display);
gboolean _gdk_wayland_display_request_selection_notification (GdkDisplay *display,
                                                              GdkAtom     selection);
void     _gdk_wayland_display_store_clipboard (GdkDisplay    *display,
                                               GdkWindow     *clipboard_window,
                                               guint32        time_,
                                               const GdkAtom *targets,
                                               gint           n_targets);
GdkWindow *_gdk_wayland_display_get_default_group (GdkDisplay *display);
gchar   *_gdk_wayland_get_display (void);
void     _gdk_wayland_notify_startup_complete (void);
void     _gdk_wayland_notify_startup_complete_with_id (const gchar *startup_id);
void     _gdk_wayland_add_client_message_filter (GdkAtom       message_type,
                                                 GdkFilterFunc func,
                                                 gpointer      data);
void     _gdk_wayland_display_add_client_message_filter (GdkDisplay   *display,
                                                         GdkAtom       message_type,
                                                         GdkFilterFunc func,
                                                         gpointer      data);
gboolean _gdk_wayland_event_send_client_message_for_display (GdkDisplay      *display,
                                                             GdkEvent        *event,
                                                             GdkNativeWindow  winid);
GdkEvent *_gdk_wayland_event_get_graphics_expose (GdkWindow *window);
GList   *_gdk_wayland_devices_list (void);
GList   *_gdk_wayland_display_list_devices (GdkDisplay *display);
gboolean _gdk_wayland_device_get_history (GdkDevice      *device,
                                          GdkWindow      *window,
                                          guint32         start,
                                          guint32         stop,
                                          GdkTimeCoord ***events,
                                          gint           *n_events);
void     _gdk_wayland_input_set_extension_events (GdkWindow       *window,
                                                  gint             mask,
                                                  GdkExtensionMode mode);
gboolean _gdk_wayland_spawn_on_screen (GdkScreen            *screen,
                                       const gchar          *working_directory,
                                       gchar               **argv,
                                       gchar               **envp,
                                       GSpawnFlags           flags,
                                       GSpawnChildSetupFunc  child_setup,
                                       gpointer              user_data,
                                       gint                 *child_pid,
                                       GError              **error);
gboolean _gdk_wayland_spawn_on_screen_with_pipes (GdkScreen            *screen,
                                                  const gchar          *working_directory,
                                                  gchar               **argv,
                                                  gchar               **envp,
                                                  GSpawnFlags           flags,
                                                  GSpawnChildSetupFunc  child_setup,
                                                  gpointer              user_data,
                                                  gint                 *child_pid,
                                                  gint                 *standard_input,
                                                  gint                 *standard_output,
                                                  gint                 *standard_error,
                                                  GError              **error);
gboolean _gdk_wayland_spawn_command_line_on_screen (GdkScreen   *screen,
                                                    const gchar *command_line,
                                                    GError     **error);
gchar   *_gdk_wayland_set_locale (void);
gchar   *_gdk_wayland_wcstombs (const GdkWChar *src);
gint     _gdk_wayland_mbstowcs (GdkWChar *dest, const gchar *src, gint dest_max);
void     _gdk_wayland_error_trap_push (void);
gint     _gdk_wayland_error_trap_pop (void);
gboolean _gdk_wayland_get_use_xshm (void);
GdkFont *_gdk_wayland_fontset_load (const gchar *fontset_name);

#ifdef GDK_COMPILATION
#include "ydk/gdkalias.h"
#endif

static const gchar device_list_key[] = "ydk-wayland-display-device-list";

/* ------------------------------------------------------------------ */
/* Name                                                               */
/* ------------------------------------------------------------------ */

const gchar *
_gdk_wayland_display_get_name (GdkDisplay *display)
{
  if (display == NULL || !GDK_IS_DISPLAY (display))
    return NULL;

  return GDK_DISPLAY_GET_CLASS (display)->get_display_name (display);
}

gchar *
_gdk_wayland_get_display (void)
{
  GdkDisplay *display = gdk_display_get_default ();

  if (display == NULL)
    return NULL;

  return g_strdup (_gdk_wayland_display_get_name (display));
}

/* ------------------------------------------------------------------ */
/* Capability queries                                                 */
/* ------------------------------------------------------------------ */

/* M7: shapes map to wl_region (gdkshape-wayland.c). */
gboolean
_gdk_wayland_display_supports_shapes (GdkDisplay *display)
{
  return TRUE;
}

/* M7: input shapes map to wl_region. */
gboolean
_gdk_wayland_display_supports_input_shapes (GdkDisplay *display)
{
  return TRUE;
}

/* Stays FALSE: set_composited is a no-op. */
gboolean
_gdk_wayland_display_supports_composite (GdkDisplay *display)
{
  return FALSE;
}

/* May change in M8 (selections). */
gboolean
_gdk_wayland_display_supports_selection_notification (GdkDisplay *display)
{
  return FALSE;
}

/* May change in M8 (selections). */
gboolean
_gdk_wayland_display_supports_clipboard_persistence (GdkDisplay *display)
{
  return FALSE;
}

/* May change in M8 (selections). */
gboolean
_gdk_wayland_display_request_selection_notification (GdkDisplay *display,
                                                     GdkAtom     selection)
{
  return FALSE;
}

/* May change in M8 (selections).  Accepts NULL targets and n_targets 0. */
void
_gdk_wayland_display_store_clipboard (GdkDisplay    *display,
                                      GdkWindow     *clipboard_window,
                                      guint32        time_,
                                      const GdkAtom *targets,
                                      gint           n_targets)
{
}

/* ------------------------------------------------------------------ */
/* Startup, groups, client messages                                   */
/* ------------------------------------------------------------------ */

/* Startup notification has no Wayland equivalent in this fork. */
void
_gdk_wayland_notify_startup_complete (void)
{
}

void
_gdk_wayland_notify_startup_complete_with_id (const gchar *startup_id)
{
}

/*
 * X11 returns a leader window that is never NULL.  The root window of the
 * default screen plays that role.  set_group ignores it on Wayland.
 */
GdkWindow *
_gdk_wayland_display_get_default_group (GdkDisplay *display)
{
  GdkScreen *screen;

  if (display == NULL || !GDKW_IS_DISPLAY_WAYLAND (display))
    return NULL;

  screen = GDKW_DISPLAY_WAYLAND (display)->default_screen;
  if (screen == NULL)
    return NULL;

  return gdk_screen_get_root_window (screen);
}

/* The compositor never sends client messages, so no filter can match. */
void
_gdk_wayland_add_client_message_filter (GdkAtom       message_type,
                                        GdkFilterFunc func,
                                        gpointer      data)
{
}

void
_gdk_wayland_display_add_client_message_filter (GdkDisplay   *display,
                                                GdkAtom       message_type,
                                                GdkFilterFunc func,
                                                gpointer      data)
{
}

/* Wayland has no native window ids to address, so nothing is sent. */
gboolean
_gdk_wayland_event_send_client_message_for_display (GdkDisplay      *display,
                                                    GdkEvent        *event,
                                                    GdkNativeWindow  winid)
{
  return FALSE;
}

/* There are no graphics expose events on Wayland. */
GdkEvent *
_gdk_wayland_event_get_graphics_expose (GdkWindow *window)
{
  return NULL;
}

/* ------------------------------------------------------------------ */
/* Devices                                                            */
/* ------------------------------------------------------------------ */

GList *
_gdk_wayland_display_list_devices (GdkDisplay *display)
{
  /* Ownership: like GDK2 on X11 the list belongs to the display.  Callers
   * must not free it and must not keep it past the next call or the display. */
  GList *list;

  if (display == NULL || !GDK_IS_DISPLAY (display))
    return NULL;

  if (display->core_pointer == NULL)
    return NULL;

  list = g_object_get_data (G_OBJECT (display), device_list_key);
  if (list == NULL || list->data != (gpointer) display->core_pointer)
    {
      /* The list does not own the device, so the notify is g_list_free. */
      list = g_list_prepend (NULL, display->core_pointer);
      g_object_set_data_full (G_OBJECT (display), device_list_key, list,
                              (GDestroyNotify) g_list_free);
    }

  return list;
}

GList *
_gdk_wayland_devices_list (void)
{
  GdkDisplay *display = gdk_display_get_default ();

  if (display == NULL)
    return NULL;

  return _gdk_wayland_display_list_devices (display);
}

/* No motion history on Wayland. */
gboolean
_gdk_wayland_device_get_history (GdkDevice      *device,
                                 GdkWindow      *window,
                                 guint32         start,
                                 guint32         stop,
                                 GdkTimeCoord ***events,
                                 gint           *n_events)
{
  if (events == NULL || n_events == NULL)
    return FALSE;

  *events = NULL;
  *n_events = 0;

  return FALSE;
}

/* No extension input devices on Wayland.  Nothing to enable. */
void
_gdk_wayland_input_set_extension_events (GdkWindow       *window,
                                         gint             mask,
                                         GdkExtensionMode mode)
{
  if (window == NULL || !GDK_IS_WINDOW (window))
    return;
}

/* ------------------------------------------------------------------ */
/* Spawn                                                              */
/* ------------------------------------------------------------------ */

typedef struct
{
  gchar                *display_name;
  GSpawnChildSetupFunc  child_setup;
  gpointer              user_data;
} UserChildSetup;

static void
wayland_child_setup (gpointer data)
{
  UserChildSetup *setup = data;

  /* An empty name means the display is already closed: leave the
   * parent's environment alone. */
  if (setup->display_name != NULL && setup->display_name[0] != '\0')
    g_setenv ("WAYLAND_DISPLAY", setup->display_name, TRUE);

  if (setup->child_setup != NULL)
    setup->child_setup (setup->user_data);
}

gboolean
_gdk_wayland_spawn_on_screen (GdkScreen            *screen,
                              const gchar          *working_directory,
                              gchar               **argv,
                              gchar               **envp,
                              GSpawnFlags           flags,
                              GSpawnChildSetupFunc  child_setup,
                              gpointer              user_data,
                              gint                 *child_pid,
                              GError              **error)
{
  UserChildSetup setup;
  gboolean retval;

  /* Not a screen (NULL, or from another backend): use the default screen,
   * and spawn without touching the environment if there is none. */
  if (screen == NULL || !GDK_IS_SCREEN (screen))
    screen = gdk_screen_get_default ();

  setup.display_name = (screen != NULL && GDK_IS_SCREEN (screen))
    ? gdk_screen_make_display_name (screen) : NULL;
  setup.child_setup = child_setup;
  setup.user_data = user_data;

  retval = g_spawn_async (working_directory, argv, envp, flags,
                          wayland_child_setup, &setup, child_pid, error);

  g_free (setup.display_name);

  return retval;
}

gboolean
_gdk_wayland_spawn_on_screen_with_pipes (GdkScreen            *screen,
                                         const gchar          *working_directory,
                                         gchar               **argv,
                                         gchar               **envp,
                                         GSpawnFlags           flags,
                                         GSpawnChildSetupFunc  child_setup,
                                         gpointer              user_data,
                                         gint                 *child_pid,
                                         gint                 *standard_input,
                                         gint                 *standard_output,
                                         gint                 *standard_error,
                                         GError              **error)
{
  UserChildSetup setup;
  gboolean retval;

  /* Not a screen (NULL, or from another backend): use the default screen,
   * and spawn without touching the environment if there is none. */
  if (screen == NULL || !GDK_IS_SCREEN (screen))
    screen = gdk_screen_get_default ();

  setup.display_name = (screen != NULL && GDK_IS_SCREEN (screen))
    ? gdk_screen_make_display_name (screen) : NULL;
  setup.child_setup = child_setup;
  setup.user_data = user_data;

  retval = g_spawn_async_with_pipes (working_directory, argv, envp, flags,
                                     wayland_child_setup, &setup, child_pid,
                                     standard_input, standard_output,
                                     standard_error, error);

  g_free (setup.display_name);

  return retval;
}

gboolean
_gdk_wayland_spawn_command_line_on_screen (GdkScreen   *screen,
                                           const gchar *command_line,
                                           GError     **error)
{
  gchar **argv = NULL;
  gboolean retval;

  if (command_line == NULL)
    {
      g_set_error_literal (error, G_SHELL_ERROR, G_SHELL_ERROR_EMPTY_STRING,
			   "empty command line");
      return FALSE;
    }

  if (!g_shell_parse_argv (command_line, NULL, &argv, error))
    return FALSE;

  retval = _gdk_wayland_spawn_on_screen (screen, NULL, argv, NULL,
                                         G_SPAWN_SEARCH_PATH,
                                         NULL, NULL, NULL, error);

  g_strfreev (argv);

  return retval;
}

/* ------------------------------------------------------------------ */
/* Locale and multibyte                                               */
/* ------------------------------------------------------------------ */

gchar *
_gdk_wayland_set_locale (void)
{
  if (!setlocale (LC_ALL, ""))
    g_warning ("locale not supported by C library");

  return setlocale (LC_ALL, NULL);
}

gchar *
_gdk_wayland_wcstombs (const GdkWChar *src)
{
  gchar *utf8;
  gchar *result;

  if (src == NULL)
    return NULL;

  utf8 = g_ucs4_to_utf8 ((const gunichar *) src, -1, NULL, NULL, NULL);
  if (utf8 == NULL)
    return NULL;

  result = g_locale_from_utf8 (utf8, -1, NULL, NULL, NULL);
  g_free (utf8);

  return result;
}

gint
_gdk_wayland_mbstowcs (GdkWChar *dest, const gchar *src, gint dest_max)
{
  gchar *utf8;
  gunichar *ucs4;
  glong n = 0;
  glong count;

  if (src == NULL || dest == NULL || dest_max < 0)
    return -1;

  utf8 = g_locale_to_utf8 (src, -1, NULL, NULL, NULL);
  if (utf8 == NULL)
    return -1;

  ucs4 = g_utf8_to_ucs4 (utf8, -1, NULL, &n, NULL);
  g_free (utf8);
  if (ucs4 == NULL)
    return -1;

  count = MIN (n, (glong) dest_max);
  if (count > 0)
    memcpy (dest, ucs4, count * sizeof (GdkWChar));
  g_free (ucs4);

  return (gint) count;
}

/* ------------------------------------------------------------------ */
/* Error traps, xshm, fontsets                                        */
/* ------------------------------------------------------------------ */

static guint error_trap_depth = 0;

/* No X errors exist here.  The depth is only tracked, never reported. */
void
_gdk_wayland_error_trap_push (void)
{
  error_trap_depth++;
}

gint
_gdk_wayland_error_trap_pop (void)
{
  /* An unbalanced pop is tolerated: no critical (the demo makes them fatal). */
  if (error_trap_depth > 0)
    error_trap_depth--;

  return 0;
}

gboolean
_gdk_wayland_get_use_xshm (void)
{
  return FALSE;
}

GdkFont *
_gdk_wayland_fontset_load (const gchar *fontset_name)
{
  GdkDisplay *display = gdk_display_get_default ();

  if (display == NULL)
    return NULL;

  return gdk_fontset_load_for_display (display, fontset_name);
}
