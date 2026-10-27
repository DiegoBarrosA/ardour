/*
 * gdkm7-wayland.h
 *
 * ydk-wayland fork, M7 (cursors, shapes, fonts): the one shared contract for
 * the three M7 pieces, the decisions behind them, the function ownership
 * split and the headless test hooks the demo drives.
 *
 * Layout of this header
 *
 *   1. DECISIONS and OWNERSHIP (this comment).
 *   2. PUBLIC section (no GDK_COMPILATION needed).  Only test hooks, the
 *      GDKWTest*Info structs and two pure name helpers.  Depends on
 *      <ydk/gdk.h> alone, so the demo includes it as <ydk/gdkm7-wayland.h>
 *      once the integrator installs it (flake.nix postInstall).
 *   3. INTERNAL section (#ifdef GDK_COMPILATION).  Prototypes of every
 *      function the pieces add, each tagged with its owner, plus the
 *      RECOMMENDED private layouts.
 *
 * Include order inside the library: gdkprivate-wayland.h, gdkseat-wayland.h
 * (if needed), gdkpixmap-wayland.h (if needed), this header, then (last of
 * all) gdkalias.h, like the other wayland sources.
 *
 * ---------------------------------------------------------------------------
 * OWNERSHIP.  No file is touched by two pieces.  Shared files belong to the
 * integrator only: ydk/gdkbackend.c, ydk/x11/ydk/gdkwindowing-x11.h,
 * ydk/wayland/ydk/gdkprivate-wayland.h, wscript, flake.nix, demo/, scripts/,
 * PLANNING.md.  Pieces never edit this header.  If a contract here looks
 * wrong, the piece implements the contract anyway and reports the problem.
 * The M7 work needs NO new dispatcher in gdkbackend.c and NO new rename in
 * gdkwindowing-x11.h: every public name it touches is already dispatched.
 *
 *   piece "cursor"
 *       ydk/wayland/gdkcursor-wayland.c       (rewrite; owns all cursor state)
 *       ydk/wayland/gdkevents-wayland.c       (2 call sites, see below)
 *       ydk/wayland/gdkdisplay-wayland.c      (4 call sites + delete 3 stubs)
 *       ydk/wayland/gdkx11-guards-wayland.c   (1 call site)
 *   piece "shape"
 *       ydk/wayland/gdkshape-wayland.c        (NEW file; the integrator adds
 *                                              it to wscript)
 *       ydk/wayland/gdkwindowimpl-wayland.c   (vfunc forwards + 2 call sites)
 *       ydk/wayland/gdkdisplay-misc-wayland.c (supports_shapes, 2 lines)
 *   piece "font"
 *       ydk/wayland/gdkfont-wayland.c
 *       ydk/wayland/gdkdrawable-wayland.c     (draw_text, draw_text_wc only)
 *
 * Cross-piece call sites (the call is written in the file of the piece that
 * owns that file; the callee is defined by the other piece):
 *
 *   gdkevents-wayland.c (cursor)    wl_pointer_enter ()
 *       after seat->px / py are set, before queue_crossing ():
 *           _gdk_wayland_cursor_pointer_enter (GDK_DISPLAY_OBJECT (wd), serial);
 *   gdkevents-wayland.c (cursor)    pointer_focus_lost (), first statement:
 *           _gdk_wayland_cursor_pointer_leave (GDK_DISPLAY_OBJECT (wd));
 *   gdkdisplay-wayland.c (cursor)   gdkw_display_wayland_dispose () and
 *       gdkw_display_wayland_finalize (), immediately before the existing
 *       _gdk_wayland_events_finalize () call (in finalize this is before
 *       wl_shm and wl_compositor are destroyed):
 *           _gdk_wayland_cursor_display_finalize (GDK_DISPLAY_OBJECT (display));
 *   gdkdisplay-wayland.c (cursor)   _gdk_wayland_windowing_pointer_grab (),
 *       just before returning GDK_GRAB_SUCCESS, and at the end of
 *       _gdk_wayland_display_pointer_ungrab ():
 *           _gdk_wayland_cursor_update (display);
 *   gdkdisplay-wayland.c (cursor)   DELETE the three stub bodies
 *       _gdk_wayland_windowing_window_get_shape,
 *       _gdk_wayland_windowing_window_get_input_shape,
 *       _gdk_wayland_windowing_get_shape_for_mask.  The shape piece defines
 *       them in gdkshape-wayland.c.  Their prototypes stay in
 *       gdkprivate-wayland.h.
 *   gdkx11-guards-wayland.c (cursor) _gdk_wayland_x11_display_set_cursor_theme ():
 *           _gdk_wayland_cursor_theme_set (display, theme, size);
 *   gdkwindowimpl-wayland.c (shape) gdk_window_wayland_set_cursor ():
 *           _gdk_wayland_cursor_window_set_cursor (window, cursor);
 *   gdkwindowimpl-wayland.c (shape) gdk_window_wayland_shape_combine_region ()
 *       and gdk_window_wayland_input_shape_combine_region () forward their
 *       arguments to _gdk_wayland_shape_combine_region () and
 *       _gdk_wayland_shape_input_combine_region ().
 *   gdkwindowimpl-wayland.c (shape) gdk_window_wayland_show (), after
 *       _gdk_wayland_window_hints_apply () and before the first
 *       wl_surface_commit ():
 *           _gdk_wayland_shape_surface_created (impl);
 *   gdkwindowimpl-wayland.c (shape) _gdk_wayland_window_impl_present (), after
 *       the buffer exists and before wl_surface_attach ():
 *           _gdk_wayland_shape_before_present (impl);
 *
 * ---------------------------------------------------------------------------
 * DECISIONS: CURSORS
 *
 * Theme loading.  The theme is loaded lazily, the first time a cursor image is
 * needed, with wl_cursor_theme_load (name, size, display->wl_shm).  Name comes
 * from XCURSOR_THEME (unset or empty: NULL, which libwayland-cursor maps to
 * "default").  Size comes from XCURSOR_SIZE (integer 1 to 512, else 24).
 * Both are read once, when the per display state is created, and can be
 * changed later by gdk_x11_display_set_cursor_theme () (NULL or empty theme =
 * keep the name, size <= 0 = keep the size).  Scale is 1 (M10 multiplies the
 * size and sets the buffer scale).  libwayland-cursor reads XCURSOR_PATH at
 * every load, so a test can point it at a generated theme.
 *
 * "Usable" theme: wl_cursor_theme_get_cursor (theme, "left_ptr") or "default"
 * is not NULL.  MEASURED (libwayland-cursor 1.26, headless Weston): when no
 * theme files are found (empty XCURSOR_PATH directory) wl_cursor_theme_load
 * still returns a theme, and that theme holds a BUILT IN set (left_ptr,
 * default, xterm, watch, ... 10x16 images, but no "ew-resize"), so it is
 * usable.  A generated theme directory containing only left_ptr, xterm and
 * ew-resize loads exactly those three (8x6 hot 1,2 for left_ptr) and has no
 * "default" and no "watch".  The result also depends on the user's real
 * XCURSOR_PATH when the variable is unset, so tests must always pass a
 * search path.  A theme counts as unusable only when wl_cursor_theme_load
 * returns NULL (no wl_shm, out of memory) or both lookups fail; the 1x1 buffer
 * below covers those cases and is tested through
 * _gdk_wayland_test_cursor_disable_theme ().
 *
 * Resolution of a GdkCursor to an image (first match wins):
 *   1. GDK_BLANK_CURSOR: kind HIDDEN.  wl_pointer.set_cursor (serial, NULL, 0, 0).
 *   2. cursor with image data (new_from_pixmap / new_from_pixbuf): kind
 *      CUSTOM.  Own wl_buffer (ARGB8888, premultiplied), own hotspot.
 *   3. typed cursor (GdkCursorType) or named cursor: kind THEME.  Candidate
 *      names come from _gdk_wayland_cursor_name_candidates (); the first one
 *      the theme has wins.  A named cursor uses [name], or the alias list when
 *      name is one of the primary names below.  Image 0 only (no animation in
 *      M7).
 *   4. theme usable but no candidate found: THEME with "left_ptr", else
 *      "default".
 *   5. nothing found (theme load returned NULL or no usable theme): kind
 *      FALLBACK, a built in 1x1 fully
 *      transparent ARGB8888 buffer, hotspot 0,0.  The pointer is invisible
 *      then.  This is deliberate and logged once with g_debug.  (A built in
 *      arrow is a follow up, not M7.)
 *
 * gdk_cursor_new_from_name (): with a usable theme returns NULL if
 * wl_cursor_theme_get_cursor (theme, name) is NULL (exact name, no aliases),
 * like GDK2 on X11.  With no usable theme it accepts every name, because it
 * cannot validate.  Typed cursors are never validated.
 *
 * GdkCursorType to Xcursor name.  GDK cursor values are the X cursor font
 * glyph numbers, so the name is table[type / 2] for even type 0..152:
 *
 *   0 X_cursor            2 arrow               4 based_arrow_down
 *   6 based_arrow_up      8 boat               10 bogosity
 *  12 bottom_left_corner 14 bottom_right_corner 16 bottom_side
 *  18 bottom_tee         20 box_spiral         22 center_ptr
 *  24 circle             26 clock              28 coffee_mug
 *  30 cross              32 cross_reverse      34 crosshair
 *  36 diamond_cross      38 dot                40 dotbox
 *  42 double_arrow       44 draft_large        46 draft_small
 *  48 draped_box         50 exchange           52 fleur
 *  54 gobbler            56 gumby              58 hand1
 *  60 hand2              62 heart              64 icon
 *  66 iron_cross         68 left_ptr           70 left_side
 *  72 left_tee           74 leftbutton         76 ll_angle
 *  78 lr_angle           80 man                82 middlebutton
 *  84 mouse              86 pencil             88 pirate
 *  90 plus               92 question_arrow     94 right_ptr
 *  96 right_side         98 right_tee         100 rightbutton
 * 102 rtl_logo          104 sailboat          106 sb_down_arrow
 * 108 sb_h_double_arrow 110 sb_left_arrow     112 sb_right_arrow
 * 114 sb_up_arrow       116 sb_v_double_arrow 118 shuttle
 * 120 sizing            122 spider            124 spraycan
 * 126 star              128 target            130 tcross
 * 132 top_left_arrow    134 top_left_corner   136 top_right_corner
 * 138 top_side          140 top_tee           142 trek
 * 144 ul_angle          146 umbrella          148 ur_angle
 * 150 watch             152 xterm
 *
 * Odd values, GDK_LAST_CURSOR, GDK_BLANK_CURSOR, GDK_CURSOR_IS_PIXMAP and
 * out of range values map to NULL.  Alias lists (candidate 0 is the primary
 * name; types not listed have only the primary name):
 *
 *   left_ptr           left_ptr default arrow
 *   arrow              arrow right_ptr default
 *   top_left_arrow     top_left_arrow left_ptr default
 *   xterm              xterm text ibeam
 *   watch              watch wait progress
 *   hand1              hand1 pointer hand2
 *   hand2              hand2 pointer hand1
 *   crosshair          crosshair cross tcross
 *   cross              cross crosshair
 *   tcross             tcross crosshair cross
 *   fleur              fleur move all-scroll size_all
 *   sb_h_double_arrow  sb_h_double_arrow ew-resize h_double_arrow col-resize
 *                      split_h size_hor
 *   sb_v_double_arrow  sb_v_double_arrow ns-resize v_double_arrow row-resize
 *                      split_v size_ver
 *   double_arrow       double_arrow sb_v_double_arrow ns-resize
 *   top_left_corner    top_left_corner nw-resize size_fdiag
 *   top_right_corner   top_right_corner ne-resize size_bdiag
 *   bottom_left_corner bottom_left_corner sw-resize size_bdiag
 *   bottom_right_corner bottom_right_corner se-resize size_fdiag
 *   top_side           top_side n-resize
 *   bottom_side        bottom_side s-resize
 *   left_side          left_side w-resize
 *   right_side         right_side e-resize
 *   question_arrow     question_arrow help whats_this
 *   X_cursor           X_cursor crossed_circle
 *
 * When wl_pointer.set_cursor is sent.  Wayland only accepts it for the
 * surface that has pointer focus, with the serial of the latest wl_pointer.enter
 * (GDKWSeat.pointer_enter_serial).  The effective cursor is
 *     explicit pointer grab with a cursor (GDKWSeat.grab.active and
 *     grab.cursor)  ->  grab.cursor, wherever the pointer is
 *     else the cursor the core last passed to the set_cursor vfunc for the
 *     toplevel that has pointer focus (the core has already resolved
 *     window_under_pointer, parent inheritance and implicit grabs, see
 *     update_cursor () in gdkwindow.c)
 *     else (that was NULL or never set) the cursor of the screen's root window
 *     else the default cursor, "left_ptr".
 * It is applied
 *   a. on wl_pointer.enter, with the new serial, always (the compositor forgets
 *      the cursor on every enter);
 *   b. from the set_cursor vfunc, when the impl is GDKWSeat.pointer_focus;
 *      otherwise the cursor is only remembered (g_object qdata on the impl,
 *      one ref) and applied by (a);
 *   c. when an explicit pointer grab starts or ends (_gdk_wayland_cursor_update);
 *   d. from gdk_x11_display_set_cursor_theme (), after the reload.
 * Nothing is sent when there is no pointer focus.  An application is deduped:
 * a request is skipped when the resolved image (kind, buffer and hotspot) and
 * the serial equal the last applied ones.  A new serial always sends.
 * Without a wl_pointer (headless Weston has no seat) everything except the
 * final wl_pointer_set_cursor () request still happens for real: theme load,
 * cursor wl_surface, attach, damage, commit.  GDKWTestCursorInfo.to_compositor
 * tells which case the last application was.
 *
 * Custom cursors.
 *   new_from_pixmap (source, mask, fg, bg, x, y): source and mask are depth 1
 *   Wayland pixmaps of equal size.  Pixel (px, py): mask bit 0 = fully
 *   transparent (0x00000000); mask bit 1 = opaque, colour fg if the source bit
 *   is 1, else bg (GdkColor 16 bit channels, c >> 8; .pixel is ignored, as
 *   X11 does).  Bits are read from the A1 cairo surface of the pixmap impl
 *   (LSB first inside a byte on little endian, as gdkdrawable-wayland.c
 *   writes them).  Returns NULL, with g_debug only, if a pixmap is not depth 1
 *   or the sizes differ.  Hotspot outside the image: g_return_val_if_fail, as
 *   new_from_pixbuf already does.
 *   new_from_pixbuf: drawn on an ARGB32 cairo image surface with
 *   gdk_cairo_set_source_pixbuf () (premultiplies); hotspot parsing stays.
 *   Both keep the surface in GDKWCursor.image; the wl_buffer is created on
 *   first application (memfd + mmap + wl_shm_pool, pool destroyed at once, as
 *   ensure_shm_buffer () does) and lives until the cursor is destroyed or the
 *   display is closed.  The buffer is immutable, so no release tracking.
 *   gdk_cursor_get_image (): custom cursors return a non premultiplied RGBA
 *   GdkPixbuf with options "x_hot" and "y_hot" (and "name" if named); theme,
 *   typed and blank cursors return NULL (libwayland-cursor does not expose the
 *   pixels; the API allows NULL).
 *   A GdkCursor keeps its display only as a weak pointer.  The per display state
 *   keeps the list of cursors that own a wl_buffer and destroys the buffers in
 *   _gdk_wayland_cursor_display_finalize (), because the proxies must not
 *   outlive wl_display_disconnect ().
 *
 * ---------------------------------------------------------------------------
 * DECISIONS: SHAPES
 *
 * Only toplevels (GDK_WINDOW_TOPLEVEL and GDK_WINDOW_TEMP impls that have a
 * wl_surface) are shaped.  The core also calls the vfuncs for native children
 * (clip as shape) and with a NULL region when nothing is set: for anything else
 * the piece returns at once and sends nothing.  Children are always
 * client side clip regions, already honoured by the core.
 *
 * Bounding shape B (vfunc shape_combine_region, region in window coordinates
 * plus the offset arguments; NULL unsets):
 *   - stored as a GdkRegion copy per impl (qdata with a destroy notify);
 *   - visual: before every present, pixels of impl->backing outside B are set
 *     to 0x00000000 (CAIRO_OPERATOR_CLEAR over the complement, a region of
 *     window rectangles).  The core clips all drawing to B already, so the
 *     cleared pixels stay clear; paint_background () and a new buffer are
 *     covered because the clear runs on every present;
 *   - opaque region: B, but only when the window depth is 24 (the buffer is
 *     ARGB8888 filled opaque); for a depth 32 (rgba) window nothing is claimed
 *     and set_opaque_region (NULL) is sent; unshaped windows send NULL too (an
 *     opaque region for whole unshaped windows is a follow up);
 *   - input region: see below.
 * Input shape I (vfunc input_shape_combine_region): stored the same way, no
 * visual effect.
 * Effective input region E, sent with wl_surface.set_input_region:
 *     B and I set  ->  B intersect I   (the core intersects both to pick the
 *                                       window, see remove_child_area ())
 *     only B       ->  B
 *     only I       ->  I
 *     neither      ->  NULL (the whole surface, the protocol default)
 * so an input shape different from B is visible in the request lists.
 * An empty region is a real region: empty opaque / input region, and every
 * pixel cleared, as X11 hides such a window.
 *
 * Conversion.  A GdkRegion becomes a wl_region with
 * wl_compositor_create_region (), one wl_region_add () per rectangle of
 * gdk_region_get_rectangles (), wl_surface_set_opaque_region () or
 * wl_surface_set_input_region (), then wl_region_destroy () at once (allowed;
 * the surface copies the state).  wl_compositor is bound at version 1, which is
 * enough.  Window coordinates equal surface coordinates (toplevel at 0, 0,
 * scale 1).  State is double buffered: the requests take effect on the next
 * wl_surface_commit (), which _gdk_wayland_window_impl_present () does; the
 * shape code calls that function after sending, and it is a no-op until the
 * xdg_surface is configured.  Requests are sent only when the effective region
 * changed (gdk_region_equal against the last sent copy).  If the window has no
 * wl_surface yet, the regions are only stored and
 * _gdk_wayland_shape_surface_created () sends them from show ().
 *
 * Mask to region.  _gdk_wayland_windowing_get_shape_for_mask (mask) is
 * _gdk_wayland_bitmap_to_region (mask) (M5, set bits only; NULL if mask is not a
 * depth 1 Wayland pixmap, with g_debug).  The core adds the x, y offset of
 * gdk_window_shape_combine_mask ().
 *
 * Read back.  _gdk_wayland_windowing_window_get_shape () / _get_input_shape ()
 * return a copy of the stored B / I in window coordinates, NULL if unset or
 * the window is not a shaped toplevel.  The core only calls them for FOREIGN
 * parents, so the test reads the lists through _gdk_wayland_test_shape_info ().
 * gdk_display_supports_shapes () and _input_shapes () become TRUE.
 *
 * ---------------------------------------------------------------------------
 * DECISIONS: FONTS
 *
 * A GdkFont stays the M4 wrapper (PangoFontDescription plus one PangoLayout over
 * a pangocairo font map context), with these changes so that what is measured is
 * what is drawn:
 *   - the context gets fixed options: resolution GDKW_FONT_DPI (96), cairo font
 *     options antialias GRAY, hint style SLIGHT, hint metrics ON (integer glyph
 *     advances), pango_context_set_round_glyph_positions (TRUE) when available,
 *     and the layout is in single paragraph mode with no width, no wrapping.
 *     No call to pango_cairo_update_context () for the target surface, which
 *     would change the metrics per surface;
 *   - GdkFontPrivateWayland gets a guint32 magic (GDKW_FONT_MAGIC) right after
 *     the GdkFontPrivate base.  Entry points that receive a GdkFont check it
 *     and treat a font of another backend as "draw nothing, measure 0", with
 *     g_debug only (never a g_critical);
 *   - measure (): width = logical width of line 0 in pixels, lbearing / rbearing
 *     / ascent / descent from the ink rectangle (X text extents convention) as
 *     today.  Text conversion stays text_to_utf8 () (UTF-8 if valid, else
 *     locale, else Latin-1) and wc_to_utf8 ().  text_length is in bytes, < 0
 *     means strlen.  Drawing and measuring use the SAME utf8 string.
 * Drawing (gdkw_draw_text, gdkw_draw_text_wc in gdkdrawable-wayland.c):
 *     utf8 = text conversion;  cr = gdkw_begin (drawable, gc,
 *     GDKW_GC_USE_FILL, &impl)  (the GC to cairo helper of gdkgc-wayland.c:
 *     clip, operator, solid or tile or stipple source);
 *     _gdk_wayland_font_show_utf8 (font, cr, x, y, utf8, as_path)  with
 *     as_path = (depth of impl == 1);  cairo_destroy (cr).
 * x is the left edge and y the BASELINE (X convention, same as gdk_draw_text).
 * _gdk_wayland_font_show_utf8 () lays out utf8 on the font's layout, takes line 0
 * and, for as_path FALSE, does cairo_move_to (cr, x, y) and
 * pango_cairo_show_layout_line (cr, line).  For depth 1 targets it uses
 * pango_cairo_layout_line_path () and cairo_fill (), because the cr antialias
 * mode does not apply to glyph drawing but does apply to fills, and bitmaps
 * need exact edges.  No background is painted (XDrawString semantics).
 * gdk_draw_string () is gdk_draw_text () in the core; there is no third vfunc.
 * The one time g_warning "GdkFont text drawing unsupported" is removed.
 *
 * ---------------------------------------------------------------------------
 * HOOK OWNERS (every hook returns FALSE, never warns, when its precondition
 * fails: not a Wayland display or window, no state yet)
 *
 *   cursor  (gdkcursor-wayland.c)  _gdk_wayland_test_cursor_info,
 *           _gdk_wayland_test_cursor_reset, _gdk_wayland_test_cursor_theme_reload,
 *           _gdk_wayland_test_cursor_disable_theme,
 *           _gdk_wayland_test_cursor_pixel, _gdk_wayland_cursor_type_to_name,
 *           _gdk_wayland_cursor_name_candidates
 *   shape   (gdkshape-wayland.c)   _gdk_wayland_test_shape_info,
 *           _gdk_wayland_test_window_pixel
 *   font    (gdkfont-wayland.c)    _gdk_wayland_test_font_info
 * ---------------------------------------------------------------------------
 */

#ifndef __YDK_GDKM7_WAYLAND_H__
#define __YDK_GDKM7_WAYLAND_H__

#include <glib.h>
#include <glib-object.h>

#include <ydk/gdk.h>

G_BEGIN_DECLS

/* =========================================================================
 * 2. PUBLIC: pure helpers and headless test hooks
 * ========================================================================= */

/* Capacity of every rectangle list in the info structs.  The n_* counters are
 * the TRUE number of rectangles; arrays hold the first GDKW_TEST_MAX_RECTS. */
#define GDKW_TEST_MAX_RECTS 128

/* ---- cursor ------------------------------------------------------------- */

/* Pure helpers (no display needed).  Owner: cursor.
 *
 * _gdk_wayland_cursor_type_to_name (): the primary Xcursor name from the
 * table in the header comment, or NULL for odd values, GDK_LAST_CURSOR,
 * GDK_BLANK_CURSOR, GDK_CURSOR_IS_PIXMAP and anything out of range.  Static
 * string, never freed.
 *
 * _gdk_wayland_cursor_name_candidates (): NULL terminated array of theme names
 * to try for @type, candidate 0 being the primary name; NULL for the same
 * types as above.  Static, never freed. */
const gchar        *_gdk_wayland_cursor_type_to_name    (GdkCursorType type);
const gchar * const *_gdk_wayland_cursor_name_candidates (GdkCursorType type);

typedef enum
{
  GDKW_TEST_CURSOR_NONE = 0,      /* nothing applied yet (or after reset) */
  GDKW_TEST_CURSOR_THEME,         /* image of the wl_cursor_theme */
  GDKW_TEST_CURSOR_CUSTOM,        /* pixmap or pixbuf cursor, own buffer */
  GDKW_TEST_CURSOR_FALLBACK,      /* built in 1x1 transparent buffer */
  GDKW_TEST_CURSOR_HIDDEN         /* GDK_BLANK_CURSOR: set_cursor (NULL surface) */
} GDKWTestCursorKind;

/* What the cursor code applied last.  "Applied" means: attached to the cursor
 * wl_surface and committed, and wl_pointer.set_cursor sent when a wl_pointer
 * exists. */
typedef struct
{
  guint              set_count;     /* applications since the last reset */
  guint32            serial;        /* serial used by the last application */
  GDKWTestCursorKind kind;
  gchar              name[64];      /* theme cursor name that matched ("left_ptr"
                                     * when the policy fell back), "" otherwise */
  gint               width, height; /* pixels of the attached buffer, 0 for
                                     * NONE and HIDDEN */
  gint               hot_x, hot_y;
  gboolean           to_compositor; /* the last application sent a real
                                     * wl_pointer.set_cursor request */
  GdkCursor         *cursor;        /* the GdkCursor resolved, NULL when the
                                     * default cursor was used; borrowed */
  gboolean           theme_loaded;  /* wl_cursor_theme_load () was run */
  gboolean           theme_usable;  /* theme has "left_ptr" or "default" */
  gchar              theme_name[64];/* "" for the default theme */
  gint               theme_size;    /* size passed to wl_cursor_theme_load () */
} GDKWTestCursorInfo;

/* Owner: cursor.  Copies the state; loads nothing. */
gboolean _gdk_wayland_test_cursor_info   (GdkDisplay         *display,
                                          GDKWTestCursorInfo *info);

/* Owner: cursor.  Zeroes set_count and forgets the applied image (kind NONE),
 * so the next application is a fresh one.  Keeps the theme. */
gboolean _gdk_wayland_test_cursor_reset  (GdkDisplay *display);

/* Owner: cursor.  Destroys the current wl_cursor_theme (if any), stores
 * @theme / @size (NULL and <= 0 keep the current values), loads the theme NOW
 * with wl_cursor_theme_load (), recomputes theme_usable and re-applies the
 * effective cursor if the pointer has focus.  When @search_path is not NULL the
 * environment variable XCURSOR_PATH is set to it for the duration of the load
 * only and restored (or unset) afterwards.  An existing but empty directory as
 * @search_path gives a deterministic "no theme installed" state; a directory
 * written by the test gives a known theme. */
gboolean _gdk_wayland_test_cursor_theme_reload (GdkDisplay  *display,
                                                const gchar *theme,
                                                gint         size,
                                                const gchar *search_path);

/* Owner: cursor.  @disable TRUE: destroys the theme and behaves as if
 * wl_cursor_theme_load () had returned NULL (theme_loaded TRUE, theme_usable
 * FALSE, theme NULL) until called with FALSE, _gdk_wayland_test_cursor_theme_reload
 * or _gdk_wayland_cursor_theme_set.  Re-applies the effective cursor if the
 * pointer has focus.  This is the only deterministic way to reach the
 * FALLBACK (1x1 transparent) path, because libwayland-cursor ships a built in
 * theme. */
gboolean _gdk_wayland_test_cursor_disable_theme (GdkDisplay *display,
                                                 gboolean    disable);

/* Owner: cursor.  Reads pixel (@x, @y) of the buffer last applied, as stored:
 * premultiplied ARGB, 0xAARRGGBB.  Only for kind CUSTOM and FALLBACK (the
 * pixels of a theme image are not readable); FALSE otherwise or when out of
 * range. */
gboolean _gdk_wayland_test_cursor_pixel  (GdkDisplay *display,
                                          gint        x,
                                          gint        y,
                                          guint32    *argb);

/* ---- shape -------------------------------------------------------------- */

/* What the shape code stores and last sent for one window.  Rectangle lists are
 * in surface (= window) coordinates, in the order gdk_region_get_rectangles ()
 * returns them; compare them as regions, not as lists. */
typedef struct
{
  gboolean     has_surface;       /* the window has a wl_surface */
  gboolean     shaped;            /* a bounding shape B is stored */
  gboolean     input_shaped;      /* an input shape I is stored */
  gint         n_bounding;        /* B */
  GdkRectangle bounding[GDKW_TEST_MAX_RECTS];
  gint         n_input_shape;     /* I */
  GdkRectangle input_shape[GDKW_TEST_MAX_RECTS];

  gboolean     opaque_sent;       /* last set_opaque_region carried a region
                                   * (FALSE: NULL, or nothing sent yet) */
  gint         n_opaque;
  GdkRectangle opaque[GDKW_TEST_MAX_RECTS];
  gboolean     input_sent;        /* last set_input_region carried a region
                                   * (FALSE: NULL = whole surface) */
  gint         n_input;           /* effective input region E */
  GdkRectangle input[GDKW_TEST_MAX_RECTS];

  guint        opaque_requests;   /* set_opaque_region requests sent, NULL ones
                                   * included */
  guint        input_requests;    /* set_input_region requests sent */
} GDKWTestShapeInfo;

/* Owner: shape.  FALSE for a window that is not a Wayland toplevel impl.  For a
 * toplevel that was never shaped the info is all zero except has_surface. */
gboolean _gdk_wayland_test_shape_info    (GdkWindow         *window,
                                          GDKWTestShapeInfo *info);

/* Owner: shape.  Reads pixel (@x, @y) of the window's shm backing as stored,
 * 0xAARRGGBB, with no conversion.  FALSE if the window has no backing yet or
 * the point is outside.  Used to check that pixels outside the bounding shape
 * are 0x00000000 after gdk_flush () and the ones inside are opaque. */
gboolean _gdk_wayland_test_window_pixel  (GdkWindow *window,
                                          gint       x,
                                          gint       y,
                                          guint32   *argb);

/* ---- font --------------------------------------------------------------- */

#define GDKW_FONT_DPI 96.0

typedef struct
{
  gboolean is_wayland_font;       /* magic matched */
  gchar    family[64];            /* family of the face Pango resolved ("" if none) */
  gdouble  dpi;                   /* resolution of the font's context, 96.0 */
  gboolean hint_metrics_on;       /* cairo font option of the context */
  gint     ascent, descent;       /* equal to GdkFont.ascent / descent */
} GDKWTestFontInfo;

/* Owner: font.  FALSE (and info zeroed) for a font of another backend or NULL. */
gboolean _gdk_wayland_test_font_info     (GdkFont          *font,
                                          GDKWTestFontInfo *info);

G_END_DECLS

/* =========================================================================
 * 3. INTERNAL
 * ========================================================================= */

#ifdef GDK_COMPILATION

#include <cairo.h>
#include <wayland-client.h>

#include "gdkprivate-wayland.h"

G_BEGIN_DECLS

#define GDKW_CURSOR_DEFAULT_NAME     "left_ptr"
#define GDKW_CURSOR_DEFAULT_SIZE     24
#define GDKW_CURSOR_STATE_DATA_KEY   "ydk-wayland-cursor-state"
#define GDKW_CURSOR_IMPL_DATA_KEY    "ydk-wayland-impl-cursor"
#define GDKW_SHAPE_DATA_KEY          "ydk-wayland-shape-state"
#define GDKW_FONT_MAGIC              0x57464e54u   /* "WFNT" */

/* ---- RECOMMENDED private layouts (not a contract; they are here so that the
 * owning piece and a reviewer use the same names) -------------------------- */

typedef struct _GDKWCursor GDKWCursor;
struct _GDKWCursor                 /* cursor piece only */
{
  GdkCursor        cursor;         /* type + ref_count, must be first */
  GdkDisplay      *display;        /* weak pointer, NULL after display finalize */
  gchar           *name;           /* named cursors, else NULL */
  gint             hot_x, hot_y;   /* custom cursors */
  gint             width, height;  /* custom cursors */
  cairo_surface_t *image;          /* custom: ARGB32 premultiplied */
  struct wl_buffer *buffer;        /* custom: created on first application */
  guchar          *map;            /* mmap of the buffer */
  gsize            map_size;
};

typedef struct _GDKWCursorState GDKWCursorState;
struct _GDKWCursorState            /* cursor piece only; qdata on the display */
{
  GdkDisplay             *display;
  gboolean                closed;          /* set by ..._display_finalize () */
  gchar                  *theme_name;      /* NULL = "default" */
  gint                    theme_size;
  struct wl_cursor_theme *theme;
  gboolean                theme_loaded;
  gboolean                theme_usable;
  struct wl_surface      *surface;         /* the cursor surface, created lazily */
  struct wl_buffer       *fallback_buffer; /* 1x1 transparent */
  guchar                 *fallback_map;
  GSList                 *custom;          /* GDKWCursor with a live buffer */
  GDKWTestCursorInfo      applied;         /* also what the info hook returns */
  struct wl_buffer       *applied_buffer;  /* for dedupe */
};

typedef struct _GDKWShapeState GDKWShapeState;
struct _GDKWShapeState             /* shape piece only; qdata on the impl */
{
  GdkRegion *bounding;             /* B, window coords, NULL = unshaped */
  GdkRegion *input;                /* I, NULL = none */
  GdkRegion *sent_opaque;          /* last sent, NULL = NULL region sent */
  GdkRegion *sent_input;
  gboolean   opaque_known;         /* something was sent at least once */
  gboolean   input_known;
  guint      opaque_requests;
  guint      input_requests;
};

/* ---- owner "cursor": gdkcursor-wayland.c -------------------------------
 *
 * The 11 existing _gdk_wayland_cursor_* / _gdk_wayland_display_*cursor*
 * functions keep their prototypes in gdkprivate-wayland.h and get the real
 * bodies described in the DECISIONS.  New: */

/* Idempotent.  Destroys the theme, the cursor surface, the fallback buffer and
 * every custom cursor buffer, clears GDKWCursor.display of the cursors it
 * knew, and marks the state closed (later applications do nothing).  Safe on
 * a display that never created a state. */
void  _gdk_wayland_cursor_display_finalize (GdkDisplay *display);

/* From wl_pointer_enter () in gdkevents-wayland.c, after seat->pointer_focus
 * was set.  Applies the effective cursor with @serial (force).  No-op if the
 * seat has no pointer focus. */
void  _gdk_wayland_cursor_pointer_enter   (GdkDisplay *display,
                                           guint32     serial);

/* From pointer_focus_lost () in gdkevents-wayland.c.  Forgets the applied
 * record so the next enter is a fresh application.  Does not touch set_count. */
void  _gdk_wayland_cursor_pointer_leave   (GdkDisplay *display);

/* Re-evaluates the effective cursor for the current pointer focus and applies
 * it with seat->pointer_enter_serial if it changed.  Called when an explicit
 * pointer grab starts or ends.  No-op without pointer focus. */
void  _gdk_wayland_cursor_update          (GdkDisplay *display);

/* Body of the GdkWindowImpl set_cursor vfunc (called by the shape piece's
 * gdkwindowimpl-wayland.c).  @cursor may be NULL.  Must work while the display
 * is half built or already closed (root window creation calls it): returns
 * silently when the impl has no screen / display or the display is not
 * Wayland.  Remembers the cursor on the impl (one ref, replaced on every
 * call), then applies it when the impl is the seat's pointer focus. */
void  _gdk_wayland_cursor_window_set_cursor (GdkWindow *window,
                                             GdkCursor *cursor);

/* Body of the Wayland half of gdk_x11_display_set_cursor_theme (): same
 * semantics as the reload hook without the search path.  Tolerates a NULL or
 * non Wayland display, a NULL or empty @theme and @size <= 0, silently. */
void  _gdk_wayland_cursor_theme_set       (GdkDisplay  *display,
                                           const gchar *theme,
                                           gint         size);

/* The per display state, created on first use (reads XCURSOR_THEME and
 * XCURSOR_SIZE only; no wl call).  NULL for a non Wayland display. */
GDKWCursorState *_gdk_wayland_display_get_cursor_state (GdkDisplay *display);

/* ---- owner "shape": gdkshape-wayland.c ---------------------------------- */

/* Bodies of the two shape vfuncs.  Return silently for windows that are not
 * shaped toplevels, for NULL with no stored state, and for a destroyed
 * window. */
void  _gdk_wayland_shape_combine_region       (GdkWindow       *window,
                                               const GdkRegion *region,
                                               gint             offset_x,
                                               gint             offset_y);
void  _gdk_wayland_shape_input_combine_region (GdkWindow       *window,
                                               const GdkRegion *region,
                                               gint             offset_x,
                                               gint             offset_y);

/* From gdk_window_wayland_show () once the wl_surface exists and before its
 * first commit: sends the stored regions (if any). */
void  _gdk_wayland_shape_surface_created      (GDKWWindowImplWayland *impl);

/* From _gdk_wayland_window_impl_present () before wl_surface_attach (): clears
 * the pixels of impl->backing outside the bounding shape.  Cheap no-op for an
 * unshaped window. */
void  _gdk_wayland_shape_before_present       (GDKWWindowImplWayland *impl);

/* The three windowing contract functions declared in gdkprivate-wayland.h
 * (_gdk_wayland_windowing_window_get_shape, _..._get_input_shape,
 * _..._get_shape_for_mask) are DEFINED by the shape piece in
 * gdkshape-wayland.c.  The cursor piece deletes the old stubs. */

/* ---- owner "font": gdkfont-wayland.c ------------------------------------ */

/* g_malloc'ed UTF-8 copies of legacy text (never NULL).  @text_length < 0 means
 * NUL terminated.  These are the existing static text_to_utf8 () and
 * wc_to_utf8 () made visible to gdkdrawable-wayland.c. */
gchar   *_gdk_wayland_font_text_to_utf8 (const gchar    *text,
                                         gint            text_length);
gchar   *_gdk_wayland_font_wc_to_utf8   (const GdkWChar *text,
                                         gint            text_length);

/* Lays @utf8 out on the font's layout and draws line 0 with its baseline origin
 * at (@x, @y) in @cr's user space, using @cr's current source, operator and
 * clip.  @as_path TRUE: pango_cairo_layout_line_path () + cairo_fill ()
 * (bitmaps).  Returns FALSE and draws nothing for NULL, a font of another
 * backend, or an empty / NULL @utf8. */
gboolean _gdk_wayland_font_show_utf8    (GdkFont        *font,
                                         cairo_t        *cr,
                                         gint            x,
                                         gint            y,
                                         const gchar    *utf8,
                                         gboolean        as_path);

/* ---- owner "font": gdkdrawable-wayland.c --------------------------------
 * gdkw_draw_text () and gdkw_draw_text_wc () (static, installed in
 * _gdk_wayland_drawable_install_vfuncs () already) get the drawing described in
 * DECISIONS: FONTS.  No new non static function. */

G_END_DECLS

#endif /* GDK_COMPILATION */

#endif /* __YDK_GDKM7_WAYLAND_H__ */
