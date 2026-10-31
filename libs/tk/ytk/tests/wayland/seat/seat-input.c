/*
 * seat-input.c -- a small virtual pointer and keyboard for a wlroots compositor.
 *
 * The seat of a headless sway has no input device, so a client gets
 * wl_seat.capabilities(0) and no wl_pointer.  This program creates one virtual
 * pointer (wlr-virtual-pointer) and one virtual keyboard (virtual-keyboard) on
 * the seat and keeps them until it exits.  It reads commands from a FIFO,
 * one per line.
 *
 *   move X Y         absolute pointer position in output pixels (default 1920x1080)
 *   down N | up N    press or release button N (1 left, 2 middle, 3 right)
 *   axis V           scroll vertically by V (positive scrolls down), one step is 10
 *   key NAME         press and release the key of keysym NAME (xkb name, "a", "Down")
 *   keydown NAME | keyup NAME
 *   mods N           set the depressed modifiers: 1 shift, 4 control, 8 alt
 *   sleep MS         wait before the next command
 *   sync PATH        create the file PATH (so a script can wait for the queue)
 *   quit
 *
 * usage: seat-input FIFO [WIDTH HEIGHT]
 *
 * Build:  see start-sway.sh (it builds this file with wayland-scanner and gcc).
 * Origin: Wayland backend of the ydk fork, snapshot commit 23f48f0.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

static struct wl_seat *seat;
static struct zwlr_virtual_pointer_manager_v1 *pmgr;
static struct zwp_virtual_keyboard_manager_v1 *kmgr;
static struct zwlr_virtual_pointer_v1 *ptr;
static struct zwp_virtual_keyboard_v1 *kbd;
static struct xkb_context *xctx;
static struct xkb_keymap *xmap;
static uint32_t ext_w = 1920, ext_h = 1080;
static struct wl_display *g_dpy;

static void
reg_global (void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver)
{
  if (!strcmp (iface, "wl_seat") && !seat)
    seat = wl_registry_bind (r, name, &wl_seat_interface, 1);
  else if (!strcmp (iface, "zwlr_virtual_pointer_manager_v1"))
    pmgr = wl_registry_bind (r, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
  else if (!strcmp (iface, "zwp_virtual_keyboard_manager_v1"))
    kmgr = wl_registry_bind (r, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
}

static void
reg_remove (void *d, struct wl_registry *r, uint32_t name)
{
}

static const struct wl_registry_listener reg_l = { reg_global, reg_remove };

static uint32_t
now_ms (void)
{
  struct timespec ts;
  clock_gettime (CLOCK_MONOTONIC, &ts);
  return (uint32_t) (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* keycode (evdev = xkb keycode - 8) of a keysym name; 0 when not found */
static uint32_t
keycode_of (const char *name, int *need_shift)
{
  xkb_keysym_t want = xkb_keysym_from_name (name, XKB_KEYSYM_NO_FLAGS);
  xkb_keycode_t k, min = xkb_keymap_min_keycode (xmap), max = xkb_keymap_max_keycode (xmap);
  if (want == XKB_KEY_NoSymbol)
    return 0;
  *need_shift = 0;
  for (k = min; k <= max; k++)
    {
      int lv, nl = xkb_keymap_num_levels_for_key (xmap, k, 0);
      for (lv = 0; lv < nl; lv++)
        {
          const xkb_keysym_t *syms;
          if (xkb_keymap_key_get_syms_by_level (xmap, k, 0, lv, &syms) > 0 && syms[0] == want)
            {
              *need_shift = lv == 1;
              return k - 8;
            }
        }
    }
  return 0;
}

static void
send_keymap (void)
{
  char *s;
  size_t len;
  int fd;
  xctx = xkb_context_new (XKB_CONTEXT_NO_FLAGS);
  xmap = xkb_keymap_new_from_names (xctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
  if (!xmap)
    {
      fprintf (stderr, "seat-input: cannot compile a keymap\n");
      exit (1);
    }
  s = xkb_keymap_get_as_string (xmap, XKB_KEYMAP_FORMAT_TEXT_V1);
  len = strlen (s) + 1;
  fd = memfd_create ("keymap", MFD_CLOEXEC);
  if (fd < 0 || write (fd, s, len) != (ssize_t) len)
    {
      perror ("keymap fd");
      exit (1);
    }
  zwp_virtual_keyboard_v1_keymap (kbd, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, len);
  close (fd);
  free (s);
}

static void
do_key (const char *name, int down, int up)
{
  int shift = 0;
  uint32_t kc = keycode_of (name, &shift);
  if (!kc)
    {
      fprintf (stderr, "seat-input: unknown key %s\n", name);
      return;
    }
  if (shift && down)
    zwp_virtual_keyboard_v1_modifiers (kbd, 1, 0, 0, 0);
  if (down)
    zwp_virtual_keyboard_v1_key (kbd, now_ms (), kc, WL_KEYBOARD_KEY_STATE_PRESSED);
  if (up)
    zwp_virtual_keyboard_v1_key (kbd, now_ms (), kc, WL_KEYBOARD_KEY_STATE_RELEASED);
  if (shift && up)
    zwp_virtual_keyboard_v1_modifiers (kbd, 0, 0, 0, 0);
}

/* Returns the sleep time in ms requested by the line, 0 if none, -1 for quit. */
static int
run_line (char *line)
{
  char a[256], b[256];
  int n;
  double x, y;
  if (sscanf (line, "move %lf %lf", &x, &y) == 2)
    {
      zwlr_virtual_pointer_v1_motion_absolute (ptr, now_ms (), (uint32_t) x, (uint32_t) y, ext_w, ext_h);
      zwlr_virtual_pointer_v1_frame (ptr);
    }
  else if (sscanf (line, "down %d", &n) == 1 || sscanf (line, "up %d", &n) == 1)
    {
      uint32_t btn = n == 1 ? 0x110 : n == 2 ? 0x112 : 0x111; /* BTN_LEFT, BTN_MIDDLE, BTN_RIGHT */
      zwlr_virtual_pointer_v1_button (ptr, now_ms (), btn,
                                      line[0] == 'd' ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
      zwlr_virtual_pointer_v1_frame (ptr);
    }
  else if (sscanf (line, "axis %lf", &x) == 1)
    {
      zwlr_virtual_pointer_v1_axis_source (ptr, WL_POINTER_AXIS_SOURCE_WHEEL);
      zwlr_virtual_pointer_v1_axis_discrete (ptr, now_ms (), WL_POINTER_AXIS_VERTICAL_SCROLL,
                                             wl_fixed_from_double (x), x > 0 ? 1 : -1);
      zwlr_virtual_pointer_v1_frame (ptr);
    }
  else if (sscanf (line, "key %255s", a) == 1)
    do_key (a, 1, 1);
  else if (sscanf (line, "keydown %255s", a) == 1)
    do_key (a, 1, 0);
  else if (sscanf (line, "keyup %255s", a) == 1)
    do_key (a, 0, 1);
  else if (sscanf (line, "mods %d", &n) == 1)
    zwp_virtual_keyboard_v1_modifiers (kbd, n, 0, 0, 0);
  else if (sscanf (line, "sleep %d", &n) == 1)
    return n > 0 ? n : 0;
  else if (sscanf (line, "sync %255s", b) == 1)
    {
      int fd;
      wl_display_roundtrip (g_dpy); /* the compositor has the events before the file shows up */
      fd = open (b, O_CREAT | O_WRONLY, 0600);
      if (fd >= 0)
        close (fd);
    }
  else if (!strncmp (line, "quit", 4))
    return -1;
  else if (line[0] && line[0] != '#')
    fprintf (stderr, "seat-input: bad command: %s\n", line);
  return 0;
}

int
main (int argc, char **argv)
{
  struct wl_display *dpy;
  struct wl_registry *reg;
  char buf[8192];
  size_t have = 0;
  int ffd;
  int64_t wake = 0;

  if (argc < 2)
    {
      fprintf (stderr, "usage: seat-input FIFO [WIDTH HEIGHT]\n");
      return 2;
    }
  if (argc >= 4)
    {
      ext_w = atoi (argv[2]);
      ext_h = atoi (argv[3]);
    }
  dpy = g_dpy = wl_display_connect (NULL);
  if (!dpy)
    {
      fprintf (stderr, "seat-input: cannot connect to the compositor\n");
      return 1;
    }
  reg = wl_display_get_registry (dpy);
  wl_registry_add_listener (reg, &reg_l, NULL);
  wl_display_roundtrip (dpy);
  if (!seat || !pmgr || !kmgr)
    {
      fprintf (stderr, "seat-input: missing seat=%p pointer manager=%p keyboard manager=%p\n",
               (void *) seat, (void *) pmgr, (void *) kmgr);
      return 1;
    }
  ptr = zwlr_virtual_pointer_manager_v1_create_virtual_pointer (pmgr, seat);
  kbd = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard (kmgr, seat);
  send_keymap ();
  wl_display_roundtrip (dpy);

  ffd = open (argv[1], O_RDWR | O_NONBLOCK);
  if (ffd < 0)
    {
      perror ("fifo");
      return 1;
    }
  printf ("seat-input: ready\n");
  fflush (stdout);

  for (;;)
    {
      struct pollfd pf[2];
      int timeout = -1;
      char *nl;

      wl_display_flush (dpy);
      pf[0].fd = wl_display_get_fd (dpy);
      pf[0].events = POLLIN;
      pf[1].fd = ffd;
      pf[1].events = POLLIN;
      if (wake)
        {
          int64_t d = wake - now_ms ();
          timeout = d > 0 ? (int) d : 0;
        }
      if (have && !wake)
        timeout = 0;
      poll (pf, 2, timeout);
      if (pf[0].revents & POLLIN)
        wl_display_dispatch (dpy);
      if (pf[1].revents & POLLIN)
        {
          ssize_t r = read (ffd, buf + have, sizeof buf - have - 1);
          if (r > 0)
            have += r;
        }
      if (wake && (int64_t) now_ms () < wake)
        continue;
      wake = 0;
      while (!wake && (nl = memchr (buf, '\n', have)))
        {
          int rc;
          *nl = 0;
          rc = run_line (buf);
          have -= (nl - buf) + 1;
          memmove (buf, nl + 1, have);
          if (rc < 0)
            goto out;
          if (rc > 0)
            wake = (int64_t) now_ms () + rc;
          wl_display_flush (dpy);
        }
    }
out:
  zwlr_virtual_pointer_v1_destroy (ptr);
  zwp_virtual_keyboard_v1_destroy (kbd);
  wl_display_roundtrip (dpy);
  wl_display_disconnect (dpy);
  return 0;
}
