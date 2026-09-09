/* gdkconfig.h
 *
 * This is a generated file.  Please modify `configure.in'
 */

#ifndef GDKCONFIG_H
#define GDKCONFIG_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#ifndef GSEAL
/* introduce GSEAL() here for all of Gdk and Gtk+ without the need to modify GLib */
#  ifdef GSEAL_ENABLE
#    define GSEAL(ident)      _g_sealed__ ## ident
#  else
#    define GSEAL(ident)      ident
#  endif
#endif /* !GSEAL */


/*
 * ydk-wayland fork: both the X11 and Wayland backends are compiled into
 * libydk and selected at runtime via GDK_BACKEND (see gdkbackend.c).
 * The Quartz and Win32 backends, which live on in Ardour's tree, were
 * pruned from this project; see README.md for the divergence notes.
 */
#define GDK_WINDOWING_X11
#define GDK_WINDOWING_WAYLAND

#define GDK_HAVE_WCHAR_H 1
#define GDK_HAVE_WCTYPE_H 1

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* GDKCONFIG_H */
