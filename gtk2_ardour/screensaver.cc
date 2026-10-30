/*
 * Copyright (C) 2019 Robin Gareus <robin@gareus.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "utils.h"

#ifdef PLATFORM_WINDOWS
/* http://www.blackwasp.co.uk/DisableScreensaver.aspx */
#include <windows.h>

void
ARDOUR_UI_UTILS::inhibit_screensaver (bool inhibit)
{
	if (inhibit) {
		SetThreadExecutionState (ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED | ES_CONTINUOUS);
	} else {
		SetThreadExecutionState (ES_CONTINUOUS);
	}
}

#elif defined __APPLE__

#include <IOKit/pwr_mgt/IOPMLib.h>

static IOReturn success = kIOReturnError;
static IOPMAssertionID assertion_id;

void
ARDOUR_UI_UTILS::inhibit_screensaver (bool inhibit)
{
	if (inhibit == (success == kIOReturnSuccess)) {
		return;
	}

	if (inhibit) {
		/* kIOPMAssertionTypeNoDisplaySleep prevents display sleep,
		 * kIOPMAssertionTypeNoIdleSleep prevents idle sleep
		 */
#ifdef __ppc__ /* OS X 10.5 compat API */
		success = IOPMAssertionCreate (kIOPMAssertionTypeNoDisplaySleep, kIOPMAssertionLevelOn, &assertion_id);
#else
		static const CFStringRef name = CFSTR("Ardour DAW");
		success = IOPMAssertionCreateWithName (kIOPMAssertionTypeNoDisplaySleep, kIOPMAssertionLevelOn, name, &assertion_id);
#endif
	} else {
		if (kIOReturnSuccess == IOPMAssertionRelease (assertion_id)) {
			success = kIOReturnError; // mark as inactive
		}
	}
}

#else /* Linux / X11 */

#include "ardour_ui.h"
#include "ardour/system_exec.h"

#include <gio/gio.h>
#include <ydk/gdkx.h>

static sigc::connection glib_timer;

/* Not X11 (Wayland): XResetScreenSaver has no effect, so ask the desktop to
 * keep the screen on. org.freedesktop.ScreenSaver is implemented by KDE and
 * by some Wayland helpers. The session bus is the only dependency, GIO is
 * already linked. If no service answers, this does nothing.
 */
static guint32 dbus_inhibit_cookie = 0;

static void
dbus_inhibit_screensaver (bool inhibit)
{
	static const char* const name  = "org.freedesktop.ScreenSaver";
	static const char* const path  = "/org/freedesktop/ScreenSaver";
	GError*                  error = NULL;

	if (inhibit == (dbus_inhibit_cookie != 0)) {
		return;
	}

	GDBusConnection* bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
	if (!bus) {
		g_clear_error (&error);
		return;
	}

	if (inhibit) {
		GVariant* res = g_dbus_connection_call_sync (bus, name, path, name, "Inhibit",
		                                             g_variant_new ("(ss)", PROGRAM_NAME, "Recording"),
		                                             G_VARIANT_TYPE ("(u)"), G_DBUS_CALL_FLAGS_NONE, 1000, NULL, &error);
		if (res) {
			g_variant_get (res, "(u)", &dbus_inhibit_cookie);
			g_variant_unref (res);
		}
	} else {
		GVariant* res = g_dbus_connection_call_sync (bus, name, path, name, "UnInhibit",
		                                             g_variant_new ("(u)", dbus_inhibit_cookie),
		                                             G_VARIANT_TYPE ("()"), G_DBUS_CALL_FLAGS_NONE, 1000, NULL, &error);
		if (res) {
			g_variant_unref (res);
		}
		dbus_inhibit_cookie = 0;
	}
	g_clear_error (&error);
	g_object_unref (bus);
}

static bool
reset_screensaver_timer ()
{
	Display* d = XOpenDisplay (NULL);
	if (!d) {
		return false;
	}
	XResetScreenSaver (d);
	XCloseDisplay (d);
	return true;
}

void
ARDOUR_UI_UTILS::inhibit_screensaver (bool inhibit)
{
	glib_timer.disconnect ();

	if (!display_is_x11 ()) {
		dbus_inhibit_screensaver (inhibit);
		return;
	}

	if (inhibit) {
		reset_screensaver_timer ();
		glib_timer = Glib::signal_timeout().connect_seconds (sigc::ptr_fun (&reset_screensaver_timer), 45, Glib::PRIORITY_DEFAULT_IDLE);
	}

#if 0
	Glib::RefPtr<Gdk::Window> win (ARDOUR_UI::instance()->main_window ().get_window ());
	if (!win) {
		return;
	}

	XID xid = GDK_WINDOW_XWINDOW ((win->gobj()));
	char tmp[32];
	snprintf (tmp, sizeof (tmp), "%lu", xid);

	char** args = (char**) malloc (6 * sizeof(char*));
	args[0] = strdup ("/bin/sh");
	args[1] = strdup ("-c");
	args[2] = strdup ("xdg-screensaver");
	if (inhibit) {
		args[3] = strdup ("suspend");
	} else {
		args[3] = strdup ("resume");
	}
	args[4] = strdup (tmp);
	args[5] = 0;

	ARDOUR::SystemExec xdg_exec ("/bin/sh", args);
	if (!xdg_exec.start ()) {
		xdg_exec.wait ();
	}
#endif
}

#endif
