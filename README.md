# Ardour with a native Wayland backend (work in progress)

This is a fork of [Ardour](https://ardour.org/). It adds a Wayland backend to `ydk`, the GDK fork in `libs/tk/ydk`. With it, the Ardour GUI can run as a native Wayland client. X11 is still the default. This fork is not an upstream release. Upstream documentation: <https://ardour.org/development.html>.

Status on 2026-10-31: the backend runs the Ardour editor, mixer, dialogs, menus and plugin windows on one machine (sway 1.12, Dummy audio engine). It is not ready for daily use. See [Known problems](#known-problems).

## Run it

```
GDK_BACKEND=wayland ardour9
```

Without `GDK_BACKEND=wayland` the X11 backend is used, even inside a Wayland session. Both backends are built into one `libydk`.

## Build it

Linux only. Build as usual (`./waf configure && ./waf build`, see the upstream page above). New build dependencies: `wayland-client` and `wayland-cursor` 1.20 or newer, `xkbcommon` 0.7 or newer, `cairo` 1.14 or newer, `glib` 2.68 or newer. The generated protocol code is committed, so `wayland-scanner` is not needed. `waf` needs a git tag to derive the version, so build from a full clone.

macOS and Windows are not tested. The generic code is guarded so that those backends should still compile, but nobody has built them.

## What is in the tree

| Path | Content |
|---|---|
| `libs/tk/ydk/wayland/` | The Wayland backend: windows, input, popups, clipboard, drag and drop, cursors, shapes, fonts, scale. |
| `libs/tk/ydk/gdkbackend.c` | Picks X11 or Wayland at run time and forwards the public GDK calls. |
| `libs/tk/ytk/` and `gtk2_ardour/` | Guards: code that assumed an X11 display now checks `gdk_x11_display_is_x11()`. |
| `libs/tk/ytk/tests/wayland/` | The tests, see below. |

Design notes for the backend (milestones M1 to M10) are in `PLANNING.md` on the branch `ydk-wayland` of this repository. That branch is the standalone copy where the backend was first built and tested. This tree is the merged result.

## Tests

All tests run without a real desktop session.

| Script | What it runs |
|---|---|
| `libs/tk/ytk/tests/wayland/run-tests.sh` | Programs T0.1 to T0.15 on headless weston, under valgrind, and on Xvfb (X11). Needs `weston`, `Xvfb`, `valgrind`. |
| `libs/tk/ytk/tests/wayland/run-seat-tests.sh` | T0.10, T0.12 and T0.15 on a headless sway with a virtual pointer and keyboard. Several output layouts. Needs `sway`, `grim`. |
| `libs/tk/ytk/tests/wayland/run-snapshot-demo.sh` | The demo of the standalone copy, run against this tree's `libydk`. One check is known to fail: M9 "audit a" asserts an old key serial rule that was changed on purpose. |

Headless weston has no seat, so pointer grabs, popups and menus are tested on the headless sway.

## Known problems

- **Menus close when focus moves.** Sway closes all popups of a window when keyboard focus moves to another window of the same program. With `focus_follows_mouse yes` this happens when the pointer reaches a detached mixer or plugin window. Sway behaviour, not fixable here.
- **Blank areas on mixed-scale layouts (open).** A new window starts at the scale of the first output and switches after the compositor reports the real one. On a two-output setup (one output at scale 2, one at scale 1) part of a new window or popup can stay blank until something redraws it: greyed-out combo boxes and the unselectable rows of a dropdown are the visible cases. A fix is on the branch `wayland/m11` (commits "Fix a scale change that left a popup half painted on Wayland" and "Start a new Wayland surface at a better scale than the hint") but is not merged here: its new test T0.16 passes at scale 1 and 2 and fails on fractional-scale layouts, probably because the test expects an integer buffer scale while sway uses `wp_fractional_scale_v1` with a viewport. The code is not proven on a fractional layout, so it stays out of this branch until the test is fixed.
- **Two outputs.** Monitors are reported relative to the output of the window in use. Windows on the other output, and tooltips or plugin windows there, still see the wrong rectangle.
- **Greyed text is faint.** Ardour's theme sets insensitive text darker than the background. It is equally faint on X11.
- **A click that moves focus to another window also reaches that window.** X11 swallows it.
- **Not implemented:** absolute window positions (Wayland has none), subsurfaces (native child windows and drag icons are not drawn), INCR clipboard transfers, `xdg-activation`, `xdg-decoration`, text-input, idle-inhibit, touch, tablet, cursor animation, HiDPI sign-off, a build without Wayland (`--without-wayland`).
- **Plugin windows.** X11 plugin editors (LXVST, VST3 X11, LV2 X11UI) fall back to the generic plugin window. Not tested with real plugins.
- **Crash before a session exists (upstream, not Wayland).** Moving the mouse over the empty editor before a session is loaded divides by zero in `Temporal` (the superclock rate is still 0). It happens on X11 too. This tree does not contain a workaround.
- **Compositors.** Only sway 1.12 was used (headless, plus one real session). Mutter and KWin were not run.

## Credits and licence

Ardour is free software under the GPL v2 or later. The Wayland work in this fork is released under the same licence.
