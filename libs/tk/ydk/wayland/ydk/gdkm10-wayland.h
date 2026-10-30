/*
 * gdkm10-wayland.h
 *
 * ydk-wayland fork, M10 (frame pacing, buffer lifecycle, output scale): the
 * one shared contract for the four M10 pieces, the decisions behind them, the
 * function ownership split, the edits the integrator makes in shared files,
 * and the headless test hooks the demo drives.
 *
 * Layout of this header
 *
 *   1. MEASURED FACTS, OWNERSHIP, DECISIONS, INTEGRATOR EDITS, TESTS
 *      (this comment).
 *   2. PUBLIC section (no GDK_COMPILATION needed).  Enums, the GDKWTest*Info
 *      structs, GDKW_TEST_SCALE_PX and the test hooks.  Depends on <ydk/gdk.h>
 *      alone, so the demo includes it as <ydk/gdkm10-wayland.h> once the
 *      integrator installs it (flake.nix postInstall).
 *   3. INTERNAL section (#ifdef GDK_COMPILATION).  The types that cross piece
 *      boundaries and the prototype of every function the pieces add, each
 *      tagged with its owner.
 *
 * Include order inside the library: gdkprivate-wayland.h, gdkseat-wayland.h,
 * gdkm7-wayland.h, gdkm9-wayland.h, this header, then (last of all)
 * gdkalias.h.
 *
 * ---------------------------------------------------------------------------
 * 1. MEASURED FACTS (probes against weston 16.0.0 headless, wayland 1.26.0,
 *    cairo 1.18.4, 2026-10-29; "read" means found by reading code, not run)
 *
 *  F1 Globals of the headless weston: wl_compositor v5 (so NO
 *     wl_surface.preferred_buffer_scale, that is v6), wl_output v4 (scale,
 *     done, name, description), wp_viewporter v1, zxdg_output_manager_v1 v2,
 *     wl_shm, xdg_wm_base v5.  There is NO wp_fractional_scale_manager_v1.
 *     The backend binds wl_compositor at 1 and wl_output at 1 today.
 *  F2 Frame callbacks arrive with the 60 Hz repaint: 16 to 17 ms apart in
 *     steady state, 30 to 165 ms after the very first commit.  Same with the
 *     default (noop) renderer, which scripts/smoke-test.sh uses (it starts
 *     weston with --no-config), and with --renderer=pixman.
 *  F3 wl_buffer.release.  Noop renderer: buffer 0 was released about 10 ms
 *     after its first commit, while it was still the current buffer (EARLY
 *     release), later ones one frame after the next commit.  Pixman: a buffer
 *     is released one frame after the next buffer is committed.  Two buffers
 *     were enough for six frames in both.  Tests must accept both timings.
 *  F4 wl_surface.enter is sent AFTER the first buffer commit, not at surface
 *     creation.  The first buffer therefore needs a scale hint (D10).
 *  F5 weston --scale=2 (headless option) gives wl_output.scale 2 and mode
 *     2048x1280, logical size 1024x640.  weston.ini "[output] name=headless
 *     scale=2 mode=800x600" gives mode 1600x1200.  The default logical size
 *     is 1024x640.  So logical = mode / scale.  The smoke test passes
 *     --no-config, so the scale run uses the command line option:
 *     "--scale=2 --renderer=pixman".  wl_output.geometry physical size is a
 *     placeholder in headless (it equals the logical size in mm).
 *  F6 wp_viewport with a 150x150 buffer and destination 100x100, buffer scale
 *     1, is accepted by weston, frame callbacks keep arriving.  Generating
 *     fractional-scale-v1 and viewporter with the same wayland-scanner 1.26.0
 *     works (see the build section I2).  wayland-protocols 1.49 has both XML
 *     files: staging/fractional-scale/fractional-scale-v1.xml and
 *     stable/viewporter/viewporter.xml.
 *  F7 Unmap.  attach NULL + commit, then an empty commit, makes weston send
 *     NO new xdg_surface.configure (0 in 400 ms).  Destroying xdg_toplevel
 *     and xdg_surface, then creating them again and committing without a
 *     buffer DOES give a new configure (this is what GTK3 does).  After that
 *     re-map, a frame callback for the first new buffer did not arrive in a
 *     2 s probe with either renderer.  The cause is not known (probe or
 *     weston).  The state machine must cope (timeout), and T4 does not assert
 *     a post re-map callback.
 *  F8 cairo device offset is in DEVICE pixels, not scaled by the device
 *     scale.  Probe: device scale 2 plus offset (10, 10), fill user rect
 *     (0, 0, 1, 1): ink at device pixels 10..11.  The order of the two calls
 *     does not matter.  gdkwindow.c sets the offset to (abs_x, abs_y) in two
 *     places (recompute_visible_regions_internal and
 *     gdk_window_ref_cairo_surface), so every client side child of a scaled
 *     window would draw at the wrong place.  Fix: C1 below.
 *  F9 (read) The M5 hazard.  gdkwindow.c caches one cairo surface per
 *     GdkWindow (private->cairo_surface) and drops it only in reparent,
 *     ensure_native and destroy, never on resize.  resize_buffer () munmaps
 *     shm_data, so those cached surfaces point at freed memory.
 *  F10 (read) gdk_window_wayland_move_resize () writes impl->width and height
 *     but touches no buffer.  The next create_cairo_surface () builds a
 *     surface of the NEW size over the OLD allocation: out of bounds when the
 *     window grows.
 *  F11 (read) _gdk_wayland_display_present_all () attaches, damages in full
 *     and commits EVERY window on every gdk_display_flush () and
 *     gdk_display_sync ().  gdk_window_process_all_updates () ends with
 *     flush_all_displays (), and gdk_flush () is also called by gdkimage.c
 *     (gdk_draw_rgb scratch images).  So today a redraw cycle commits all
 *     windows at once, unpaced.
 *  F12 (read) Only popups emit GDK_CONFIGURE (gdkpopup-wayland.c
 *     queue_configure).  A compositor resize of a toplevel only resizes the
 *     buffer: GdkWindowObject width and height stay old, no event.  A client
 *     resize (gdk_window_resize) emits nothing either.
 *  F13 (read) gdkevents-wayland.c uses wl_fixed_to_double of the wl_pointer
 *     surface-local coordinates with no scaling.  Surface-local coordinates
 *     are logical (buffer scale and viewport destination are applied by the
 *     compositor).  Pointer events need no change.
 *  F14 (read) Core paths that read window pixels by index
 *     (_gdk_wayland_drawable_copy_to_image, _gdk_wayland_test_window_pixel,
 *     gdkw_blit_surface temporary) assume device pixel == logical pixel.
 *     They need the edits I10, I11, I12.
 *
 * ---------------------------------------------------------------------------
 * 2. OWNERSHIP.  No file is touched by two pieces.  Pieces never edit this
 * header and never edit a shared file.  If a contract here looks wrong, the
 * piece implements the contract anyway and reports the problem.
 *
 *   piece "buffers"  ydk/wayland/gdkbuffer-wayland.c   (NEW)
 *                      the back store (GDKWBackStore), every cairo surface
 *                      over it, the shm slot pool and its states, wl_buffer
 *                      release, per slot stale regions, damage accumulation in
 *                      device pixels, the copy into a free slot,
 *                      paint_background, the window pixel reader, detaching
 *                      the core's cached surfaces when the store is replaced.
 *   piece "frame"    ydk/wayland/gdkframe-wayland.c    (NEW)
 *                      every wl_surface request that attaches, damages,
 *                      requests a frame callback or commits; the frame state
 *                      machine, coalescing, timeout, settle idle; the three
 *                      process-updates hooks; present_all; resize_buffer;
 *                      compositor and client resize; unmap; real
 *                      _gdk_windowing_set_cairo_surface_size; teardown.
 *   piece "scale"    ydk/wayland/gdkscale-wayland.c    (NEW)
 *                      wl_output list and its listener, wl_surface enter /
 *                      leave / preferred_buffer_scale listener, per surface
 *                      scale state, wp_fractional_scale_v1 and wp_viewport
 *                      objects, the screen and monitor answers in logical
 *                      units, the cursor scale answer, and the one function
 *                      that sends set_buffer_scale and the viewport
 *                      destination.
 *   piece "tests"    demo/ydk_demo_m10.inc             (NEW)
 *                      m10_exercise () and its helpers.  A plain C fragment
 *                      that the integrator includes into demo/ydk_demo.c with
 *                      one #include line (so it can use that file's static
 *                      helpers and macros).  The piece reads ydk_demo.c for
 *                      the helper names, never edits it.
 *
 * "integrator" (NOT a drafter piece): every edit in an existing file, the
 * generated protocol files, wscript, flake.nix, scripts/, demo/ydk_demo.c,
 * PLANNING.md.  Section 4 lists them in order with exact names.
 *
 * Rule: only the "frame" piece calls wl_surface_attach, wl_surface_damage,
 * wl_surface_damage_buffer, wl_surface_frame and wl_surface_commit for a
 * window surface, except the buffer-less initial commits that already exist
 * (create_toplevel, popup first commit).  Only the "scale" piece calls
 * wl_surface_set_buffer_scale and wp_viewport_* requests.  Only the "buffers"
 * piece creates, writes, and destroys wl_buffers.  That makes every request
 * observable by the hooks and every counter exact.
 *
 * ---------------------------------------------------------------------------
 * 3. DECISIONS
 *
 * D1  MODEL.  Three layers.
 *       a. BACK STORE: one refcounted block of plain memory per window and
 *          geometry.  GDK draws here.  The compositor never sees it.  Every
 *          cairo surface that GDK or the draw code holds points here.
 *       b. SLOT POOL: up to GDKW_FRAME_MAX_BUFFERS shm wl_buffers per window,
 *          one memfd per buffer.  A slot is written ONLY while it is FREE,
 *          and only by copying from the back store during a commit.
 *       c. FRAME STATE MACHINE: decides when a copy and commit may happen.
 *     Why not "wrapper surface over the CURRENT buffer, re-created on swap":
 *     the core caches private->cairo_surface per GdkWindow (F9) and only
 *     drops it for reparent, ensure_native and destroy.  A swap would have to
 *     re-create the cached surface of every descendant before every frame.
 *     A buffer taken back from the compositor also holds an older frame, so
 *     an incremental redraw would need the copy anyway.  The cost of the
 *     chosen model is one memcpy of the damaged rows per commit (a full
 *     1920x1080 ARGB frame is 8.3 MB, about 1 ms).  The benefits: GDK's
 *     surfaces stay valid across every frame, a resize cannot leave a cached
 *     surface over unmapped memory (refcount, D3), and the compositor can
 *     never read a buffer that is being written.
 *
 * D2  UNITS.  Two kinds, never mixed.
 *       LOGICAL (surface-local) units: impl->x, y, width, height; the
 *         GdkWindowObject geometry; get_size / get_geometry; xdg_surface
 *         set_window_geometry; xdg_positioner size, anchor rect and offset;
 *         the shape, input and opaque regions; wl_pointer coordinates and
 *         every GdkEvent coordinate; expose and invalidate regions; cairo user
 *         space; screen and monitor geometry; GdkScreen resolution.
 *       DEVICE (buffer) pixels: the back store size, stride and bytes; each
 *         wl_buffer and its stride; the shm sizes; wl_surface.damage_buffer;
 *         cairo_image_surface_get_width / height / stride of the surfaces over
 *         the store; the pixels of a theme cursor image.
 *     Scale is a guint "scale120": 120 is 1.0, 180 is 1.5, 240 is 2.0 (the
 *     unit of wp_fractional_scale_v1).  An integer scale k is 120 * k.
 *     Buffer size in device pixels = GDKW_SCALE_PX (logical, scale120) =
 *     (logical * scale120 + 60) / 120 in integer arithmetic: rounded half up,
 *     which is "halfway away from zero" for positive sizes, the rule of
 *     fractional-scale-v1.  For scale 1.5 it equals ceil (1.5 * size).  A
 *     logical rect maps to device pixels with floor on the low edge and ceil
 *     on the high edge, clamped to the buffer.
 *     Every cairo surface over the store has cairo_surface_set_device_scale
 *     (f, f) with f = scale120 / 120.0.  So drawing code (the gdkw_* vfuncs,
 *     pango text, the shape clear, ytk) keeps working in logical units and
 *     needs no change.
 *     Where each size lives (width, height, stride):
 *       impl->width, impl->height ........... logical, always, every piece
 *       impl->stride, impl->shm_size ........ device, back store (legacy
 *                                              fields, kept in sync by buffers)
 *       impl->shm_data ...................... back store pixels (legacy name)
 *       impl->shm_fd ........................ -1 always (the store is not shm)
 *       impl->backing ....................... cairo surface over the store,
 *                                              device scale set
 *       impl->wl_buffer ..................... the buffer of the last commit
 *       xdg_surface_set_window_geometry ..... logical (impl->width / height)
 *       xdg_positioner (gdkpopup) ........... logical (impl->width / height)
 *       wl_shm_pool_create_buffer ........... device (buffers piece)
 *       wl_surface_damage_buffer ............ device (frame piece, from the
 *                                              rects buffers fills in)
 *       wl_surface_damage (compositor < v4) . logical (frame piece converts)
 *       wl_surface_set_buffer_scale ......... integer k of the integer path
 *       wp_viewport_set_destination ......... logical size (fractional path)
 *       shape regions (gdkshape) ............ logical; shape_before_present
 *                                              clears through cairo, so it is
 *                                              correct at any scale
 *       cursor surface ...................... see D13
 *
 * D3  CAIRO SURFACES AND THE M5 HAZARD.
 *     GDKWBackStore {refcount, data, size, stride, px_w, px_h, logical w, h,
 *     scale120, generation}.  Plain memory (g_try_malloc), not shm, because
 *     the compositor never maps it.  Filled with opaque black, then
 *     paint_background () with CAIRO_OPERATOR_SOURCE.
 *     - impl->backing: a cairo image surface over the store with device scale.
 *       Used by the gdkw_* draw vfuncs and the shape code.
 *     - create_cairo_surface (the core's per window cache entry): a FRESH image
 *       surface over the same store with device scale and NO offset (the core
 *       sets the offset).  Every client side subwindow of the impl gets its own
 *       such wrapper from the core, all over the one store of the impl.
 *     - Every surface carries a user data record that holds one store
 *       reference.  The destroy notify drops the reference.  So a surface the
 *       core, an application or a half finished expose still holds keeps its
 *       memory.  The store is freed with its last surface.  That is the fix of
 *       the M5 use-after-free: nothing munmaps memory under a live surface.
 *     - When the geometry (logical size or scale120) changes, the buffers
 *       piece builds a NEW store, repoints impl->backing, and calls the core
 *       function C2 (_gdk_window_detach_cairo_surfaces) so the next
 *       gdk_window_ref_cairo_surface () creates a wrapper over the new store.
 *       Old surfaces stay valid and drawable (writes go to the old store and
 *       are discarded), so a cairo_t that lives across a configure is safe.
 *       C2 detaches (user data NULL, no cairo_surface_finish), unlike the
 *       core's own drop which finishes the surface.
 *     - The core change C1: the two cairo_surface_set_device_offset calls in
 *       gdkwindow.c multiply abs_x and abs_y by cairo_surface_get_device_scale
 *       of the surface (F8).  X11 surfaces have scale 1, so X11 is unchanged.
 *       wscript needs cairo >= 1.14 for the device scale API.
 *     - _gdk_windowing_set_cairo_surface_size (surface, w, h) is now real: it
 *       is the check that a surface the core asks to resize belongs to a store
 *       whose logical extent covers (w, h).  Surfaces are replaced, not
 *       resized, so a mismatch is a bug: the function counts it (backing info
 *       field size_mismatches) and logs with g_debug.  It never warns, never
 *       touches pixels, and ignores surfaces it did not create.
 *     - Lazy guard: buffers_ensure () compares (impl->width, impl->height,
 *       scale120) with the store and replaces it when they differ, so a
 *       writer that changed impl->width without calling resize_buffer cannot
 *       get a surface of the wrong size (F10).
 *
 * D4  SLOT POOL AND BUFFER STATES.  One record per slot (g_malloc'ed, stable
 *     address, the wl_buffer user data), index = position in the pool array,
 *     at most max_buffers slots (default GDKW_FRAME_MAX_BUFFERS = 4).
 *       FREE        compositor does not hold it; may be written
 *       COPYING     transient, inside prepare/commit_done; only the copy runs
 *       ATTACHED    the buffer of the last commit; compositor may read it
 *       SUPERSEDED  a newer buffer was attached; release not yet received
 *       RETIRED     geometry is obsolete; destroyed at release or teardown
 *     Transitions.
 *       new slot -> COPYING (stale = whole buffer)
 *       FREE -> COPYING                       prepare (the only write path)
 *       COPYING -> ATTACHED                   commit_done; the old ATTACHED
 *                                             slot becomes SUPERSEDED
 *       ATTACHED or SUPERSEDED -> FREE        wl_buffer.release
 *       RETIRED -> destroyed                  wl_buffer.release or teardown
 *       FREE -> destroyed                     retire request
 *       ATTACHED or SUPERSEDED -> RETIRED     retire request (resize, scale
 *                                             change, unmap, resize_buffer)
 *     Rules.
 *       R1 prepare picks the lowest index FREE slot of the current geometry;
 *          else a new slot while the pool has fewer than max_buffers; else it
 *          returns NO_FREE_SLOT and writes nothing.
 *       R2 A FREE slot of an obsolete geometry is destroyed at once.
 *       R3 An EARLY release (F3) of the ATTACHED slot makes it FREE.  It may
 *          be reused for the next commit.  The compositor said it is done.
 *       R4 Release for a slot already RETIRED destroys the wl_buffer.
 *       R5 Teardown destroys every wl_buffer in any state, unmaps and closes
 *          every fd.  Destroying a wl_buffer the compositor still shows is
 *          legal for shm (the server keeps its own mapping).
 *       R6 One memfd and pool per buffer: the pool is destroyed right after
 *          wl_shm_pool_create_buffer, as ensure_shm_buffer does today.  A
 *          shared pool is a non-goal.
 *     The release listener is static in the buffers file.  On a release it
 *     calls _gdk_wayland_frame_buffer_freed (impl) so a stalled commit
 *     proceeds.
 *
 * D5  DAMAGE.
 *     Sources, all feeding _gdk_wayland_buffers_damage_*:
 *       a. the expose region that the frame piece gets in
 *          _gdk_wayland_windowing_window_process_updates_recurse (native
 *          window logical coordinates, already clipped by the core);
 *       b. FULL damage after: store replaced, first configure, configure that
 *          changes size, scale change, shape change (present), set_background,
 *          explicit gdk_flush () / gdk_display_flush () outside an update
 *          cycle, remap;
 *       c. "unknown" damage: a writer entry (buffers_ref_backing, used by the
 *          gdkw_* vfuncs) called while no process_updates scope is open marks
 *          the impl unknown_dirty; the next flush turns it into FULL damage.
 *     Blind spot, documented: an application that draws with gdk_cairo_create
 *     on the core's cached surface outside an expose handler and never flushes
 *     is not seen.  GTK drawing is always inside an expose handler.
 *     Per slot "stale" region (device pixels): damage is unioned into the
 *     stale region of EVERY slot; a commit copies the stale region of the
 *     chosen slot from the store and clears it.  A new slot starts fully
 *     stale.  So a reused buffer ends equal to the store everywhere (T7
 *     checks this with buffer_vs_backing).
 *     Frame damage (what the compositor is told) is the union of the damage
 *     since the last commit, NOT the stale region.  It goes out as up to
 *     GDKW_MAX_DAMAGE_RECTS rects of wl_surface.damage_buffer; more rects
 *     collapse into their bounding box.  Below compositor version 4 the frame
 *     piece sends wl_surface.damage with the logical bounding rect.
 *     Order inside one commit (frame piece): shape_before_present, prepare
 *     (copy), set scale state if dirty (scale piece), attach, damage,
 *     frame request, commit_done.
 *
 * D6  FRAME CALLBACK STATE MACHINE (per impl, owned by the frame piece).
 *       state      event                    action                         next
 *       UNMAPPED   commit request           ignore                         UNMAPPED
 *       UNMAPPED   first configure acked    commit FULL now                WAITING
 *       IDLE       commit request           commit now (immediate)         WAITING
 *       WAITING    commit request           commit_wanted = TRUE,          WAITING
 *                                           coalesced++
 *       WAITING    frame done               destroy cb; if commit_wanted   WAITING
 *                                           commit now (from callback)     or IDLE
 *       WAITING    timeout                  destroy cb; if commit_wanted   WAITING
 *                                           commit now; backoff x2         or IDLE
 *       any        commit but no FREE slot  commit_wanted = TRUE,          unchanged
 *                                           stalled_no_buffer++
 *       any        buffer freed             if commit_wanted and state     as commit
 *                                           is not WAITING: commit now
 *       any        unmap                    destroy cb, stop timer, clear  UNMAPPED
 *                                           commit_wanted
 *     "commit now" = D5 order above; it requests wl_surface.frame, so the
 *     state is WAITING afterwards.  A commit request while WAITING never
 *     sends a commit; the back store already holds the newest pixels, so one
 *     commit at the callback carries all of them (coalescing).
 *     Timeout.  A GLib timeout source on the default main context, armed when
 *     the state becomes WAITING, delay timeout_ms.  First value
 *     GDKW_FRAME_TIMEOUT_MS (250: the first callback after a map took up to
 *     165 ms in F2).  Each consecutive timeout doubles it up to
 *     GDKW_FRAME_TIMEOUT_MAX_MS (2000).  A real callback resets it.  This
 *     covers a compositor that stops sending callbacks (occluded or minimised
 *     window, F7).  It never runs while the test mode is HOLD.
 *     Settle idle.  After a size or scale change the store holds only the
 *     background until ytk redraws.  Committing it at once would flash it.
 *     So those paths do not request a commit.  They arm one idle source at
 *     G_PRIORITY_DEFAULT_IDLE (after the redraw priority).  If no commit
 *     happened since it was armed, the idle requests a FULL commit.
 *     Test modes (D16): NORMAL; HOLD (a done event from the compositor is
 *     stashed, the timeout is suspended, the hook releases it); DROP (done
 *     events are ignored, the timeout runs).
 *
 * D7  COMMIT TRIGGERS (all go through _gdk_wayland_frame_commit_request).
 *       1. End of _gdk_wayland_windowing_window_process_updates_recurse for a
 *          native window, after _gdk_window_process_updates_recurse.  Damage
 *          is the expose region.  The hook runs inside a "scope" so writers
 *          inside it do not set unknown_dirty.
 *       2. _gdk_wayland_display_present_all () (flush and sync): outside an
 *          update cycle every mapped, configured impl gets FULL damage and a
 *          request; inside a cycle only impls with unknown_dirty or a
 *          deferred commit are touched.  The cycle is the span between
 *          _gdk_wayland_windowing_before_process_all_updates and
 *          _after_..., counted (nesting).  sync does NOT wait for frame
 *          callbacks: gdkrgb calls gdk_flush () in loops.  It stays
 *          flush + roundtrip.
 *       3. First xdg_surface.configure acked (and the first after a remap).
 *       4. _gdk_wayland_window_impl_present (): kept as the name popups and
 *          the shape code call.  It is "FULL damage + request".
 *       5. Frame done, timeout, buffer freed, settle idle (D6).
 *
 * D8  HIDE AND SHOW OF A TOPLEVEL.
 *     hide: _gdk_wayland_frame_unmap (impl): attach NULL and commit (this
 *     fixes the known gap that a hidden window stayed visible), destroy the
 *     frame callback, timers and idles, reset the store and retire the slots,
 *     destroy xdg_toplevel and xdg_surface, impl->configured = FALSE (F7).
 *     The wl_surface stays.  show: gdk_window_wayland_show tests
 *     impl->xdg_toplevel instead of impl->wl_surface; create_toplevel then
 *     builds the role objects again on the existing wl_surface and makes the
 *     buffer-less commit; the new configure leads to a FULL commit.  Popups
 *     keep the M9 hide path (their surface is destroyed).  A scale or size
 *     change while hidden only updates the numbers: no event, no commit.
 *
 * D9  RESIZE.
 *     - Compositor resize (xdg_toplevel.configure with w > 0 and h > 0 and a
 *       size different from impl): _gdk_wayland_frame_apply_configure_size ():
 *       resize_buffer (new store, detach, FULL damage), then GdkWindowObject
 *       width and height, _gdk_window_update_size (), gdk_window_invalidate_rect
 *       (window, NULL, TRUE), and one GDK_CONFIGURE (x, y from the window,
 *       new width and height) if GDK_STRUCTURE_MASK is selected.  This closes
 *       the known gap about maximize.  A configure with the same size, or with
 *       0 x 0, changes no size and queues no GDK_CONFIGURE.  The size is
 *       applied before the states, as today, so GDK_CONFIGURE precedes the
 *       window state event.  It sets the settle idle, no direct commit.
 *     - Client resize (tier 2).  gdk_window_wayland_move_resize with a size
 *       change on a non popup impl: _gdk_wayland_frame_client_resize ():
 *       same as above but the GdkWindowObject is already updated by the core;
 *       it also queues one GDK_CONFIGURE, because Wayland never answers a
 *       client size change with a configure and GTK waits for one.  M11
 *       verifies this against a real gtk window.  Popups keep their own path.
 *     - Races.
 *         configure while WAITING: the store is replaced at once; the pending
 *           callback stays; the next commit uses the new size.  Old size
 *           buffers retire.  The commit_wanted flag survives.
 *         burst of configures: each replaces the store (no commit in between);
 *           only one settle idle is armed.
 *         release of a RETIRED slot: R4.
 *         scale change while WAITING: like resize.  set_buffer_scale and the
 *           new buffer go out in the SAME commit (D11).
 *         hide while WAITING: D6 unmap.
 *         destroy while WAITING: teardown destroys callback, timers, slots.
 *     - Popup resize: gdkpopup-wayland.c apply_geometry / reposition already
 *       call _gdk_wayland_window_impl_resize_buffer ().  Its contract is
 *       "discard store and slots, set impl size, FULL damage, detach core
 *       surfaces", now in the frame piece, also valid when the wl_surface is
 *       already gone.
 *
 * D10 SCALE.  Source priority per surface, highest first:
 *       1. wp_fractional_scale_v1.preferred_scale (if the manager is bound and
 *          wp_viewporter exists, or the force hook is on);
 *       2. wl_surface.preferred_buffer_scale (compositor v6) times 120;
 *       3. the largest wl_output.scale of the outputs the surface has entered
 *          (wl_surface.enter / leave), times 120;
 *       4. HINT: the largest scale of all bound outputs, 1 if none (F4: the
 *          first buffer is made before enter arrives).
 *     Multi-output window: the maximum, never a mix.  Range 120..960.
 *     A wl_output.scale change applies at wl_output.done (v2+); a v1 output
 *     applies each event at once and never changes scale.  Output removal
 *     (registry global_remove) drops it from every entered list, then
 *     re-evaluates.
 *     Runtime change (frame piece, _gdk_wayland_frame_set_scale):
 *       S1 same scale120 as the store: return.
 *       S2 buffers_set_geometry: new store, retire slots, detach core
 *          surfaces, FULL damage.
 *       S3 mark the surface state dirty: the next commit calls
 *          _gdk_wayland_scale_send_state ().
 *       S4 if the window is mapped and configured: gdk_window_invalidate_rect
 *          (window, NULL, TRUE) and one GDK_CONFIGURE with unchanged x, y,
 *          width, height ("re-emit configure with the logical size unchanged").
 *       S5 arm the settle idle.
 *       S6 if impl is the seat pointer focus: _gdk_wayland_cursor_scale_changed.
 *       S7 the logical size, impl->width and height, and every logical
 *          structure stay as they are.
 *     GdkScreen resolution: unchanged on purpose (96 dpi logical, or the
 *     value the application set).  Text is drawn in logical units and
 *     rasterised at device scale.  The screen width / height in mm come from
 *     the output's physical size, or from logical pixels at 96 dpi when the
 *     compositor reports none.
 *
 * D11 PROTOCOL PATHS (only _gdk_wayland_scale_send_state touches them).
 *     Integer path (scale120 divisible by 120, k = scale120 / 120):
 *       wl_surface_set_buffer_scale (k), viewport destination unset (-1, -1).
 *       Needs wl_surface version >= 3, else k is forced to 1.
 *     Fractional path (scale120 not divisible by 120, manager and viewporter
 *       present or forced): set_buffer_scale (1), wp_viewport_set_destination
 *       (logical width, logical height); the buffer is GDKW_SCALE_PX larger.
 *     Fractional scale without wp_viewporter: integer path with
 *       k = ceil (scale120 / 120).  Documented gap (blur).
 *     The requests are made only when the (buffer scale, destination) pair
 *     differs from the last pair sent, and always in the same commit as the
 *     first buffer of the new geometry (frame piece calls send_state between
 *     the copy and the attach).  The last pair sent is stored by the scale
 *     piece and read by its hook.  wl_compositor is bound at
 *     MIN (advertised, GDKW_COMPOSITOR_MAX_VERSION = 6), wl_output at
 *     MIN (advertised, GDKW_OUTPUT_MAX_VERSION = 4), wp_viewporter at 1,
 *     wp_fractional_scale_manager_v1 at 1.  Both new protocols are vendored
 *     as generated C like xdg-shell (section I2).
 *
 * D12 OUTPUTS, SCREEN, MONITORS (logical units).
 *     All bound wl_outputs are kept (the old code kept the first only).  The
 *     first bound output stays display->wl_output and is mirrored into the
 *     legacy GDKWOutputInfo (scale, mode, mm, make, model) for old readers.
 *     Logical size of an output = mode / scale, width and height swapped for
 *     the odd wl_output transforms (90, 270, flipped 90, flipped 270).
 *     M11 CHANGE: xdg-output IS used (zxdg_output_manager_v1, bound at
 *     MIN (advertised, 3)).  On a fractionally scaled output the compositor
 *     reports the rounded up integer scale in wl_output, so mode / scale made
 *     the monitor look smaller than it is; the logical size of xdg_output
 *     replaces it (mode / scale stays for a compositor without xdg-output and
 *     for the fake outputs).  The position is xdg_output.logical_position when
 *     it is there, else wl_output.geometry x, y: wlroots sends 0,0 in
 *     wl_output.geometry.  The pending xdg_output state is applied by the
 *     done that ends the update: wl_output.done, and also xdg_output.done
 *     below version 3.  The two screen signals fire once per change of the
 *     visible state (monitor rectangles, scales, screen size), so an update
 *     that ends with two dones, or with one that changes nothing, signals
 *     once.  The monitor list is reported relative to the output of the
 *     window in use, see D2 in gdkm9-wayland.h (M11).
 *     Screen width and height = max (x + w), max (y + h) over the outputs
 *     (1024 x 768 when there is none).  The scale piece updates the
 *     GDKWScreenWayland size, the root window's width and height
 *     (_gdk_window_update_size), and emits "size-changed" and
 *     "monitors-changed" once per applied output change (once per wl_output
 *     done).  Monitor n is output n in bind order, monitor 0 primary.
 *     gdkscreen-wayland.c keeps its function names and delegates to
 *     _gdk_wayland_scale_* (I7).
 *
 * D13 CURSOR.  The cursor surface follows the scale of the window that has
 *     pointer focus (integer k = ceil (scale120 / 120)), so a fractional
 *     window uses the next integer.  Theme cursors: wl_cursor_theme_load with
 *     size theme_size * k pixels, wl_surface_set_buffer_scale (k) on the cursor
 *     surface, hotspot passed to wl_pointer.set_cursor = (hot + k / 2) / k in
 *     surface units.  Custom pixbuf and pixmap cursors and the 1x1 fallback
 *     stay at buffer scale 1 and keep their hotspot (app art is 1x).  A scale
 *     change of the focus window reloads the theme and applies again.  The
 *     cursor code reports each application to _gdk_wayland_scale_note_cursor,
 *     which stores it for the hook.
 *
 * D14 POPUPS, SHAPES, INPUT.  xdg_positioner sizes, anchor rects and offsets
 *     are logical (impl->width / height, root coordinates), so M9 needs no
 *     change and its popup_info hook reports identical values at any scale.
 *     A popup surface gets its own scale from its own wl_surface (enter,
 *     preferred).  Shape, input and opaque regions are logical rects.
 *
 * D15 POINTER.  Unchanged (F13).  The test asserts it.
 *
 * D16 TEST HOOK RULES.  Same as M9.  A hook returns TRUE when it did its job
 *     and FALSE when a precondition failed; it never warns or criticals.
 *     Hooks drive the REAL listener functions: frame done, wl_buffer release,
 *     wl_output geometry / mode / scale / done, wl_surface enter / leave /
 *     preferred_buffer_scale, wp_fractional_scale_v1 preferred_scale.  Modes
 *     (frame HOLD and DROP, buffer HOLD) live as small branches at the top of
 *     the real listener.  FAKE outputs have no wl_output proxy.  The surface
 *     listener finds an output by POINTER COMPARISON only (a fake output is
 *     identified by its own record address cast to struct wl_output *), and
 *     never dereferences the proxy.  The fractional force hook acts as if
 *     the manager were bound (the headless weston has none, F1); the viewport
 *     requests are still real because the headless weston has wp_viewporter.
 *     Nothing a hook does may run in a production path.
 *
 * D17 KNOWN GAPS (deliberate, go into PLANNING.md).
 *       - (M11: closed) xdg-output is used since M11, see D12.  A compositor
 *         without xdg-output still gets mode / scale, and a fractional output
 *         reports ceil there.
 *       - Fractional scale without wp_viewporter degrades to the integer
 *         path (blur).  Not seen on any real compositor of note.
 *       - Direct cairo drawing on the core's cached surface outside an expose
 *         handler, with no flush, is not seen (D5).
 *       - Custom cursors stay at buffer scale 1.
 *       - One memfd per buffer, no shared pool.
 *       - Native child windows and DND icons still have no surface (M9).
 *       - Window to pixmap blits of a scaled window downsample (pixmaps are 1x).
 *       - Hide of a toplevel destroys its xdg_toplevel (D8).  Transient
 *         children that sent set_parent against it keep hints_piece
 *         bookkeeping (parent_sent, the M9 gap) that does not know about the
 *         destroy.  xdg-shell reparents such children in the compositor; the
 *         backend does not resend set_parent after the parent is shown again.
 *       - wl_surface.preferred_buffer_transform is ignored.
 *       - A scale change repaints from scratch (the old content is not scaled
 *         for one frame).
 *       - Verified only against headless weston plus hooks (no real HiDPI
 *         compositor, no real fractional-scale manager).
 *
 * ---------------------------------------------------------------------------
 * 4. INTEGRATOR EDITS.  Order matters inside one file.  "Exact names" below.
 *
 *  I0  New includes.  After #include "gdkm9-wayland.h" add
 *      #include "gdkm10-wayland.h" to: gdkwindowimpl-wayland.c,
 *      gdkdisplay-wayland.c, gdkscreen-wayland.c, gdkcursor-wayland.c,
 *      gdkpopup-wayland.c, gdkshape-wayland.c, gdkpixmap-wayland.c.
 *      gdkdrawable-wayland.c: add the include only if I10b is done.
 *  I1  gdkprivate-wayland.h, struct _GDKWWindowImplWayland: add three
 *      pointers after "popup" (zero from g_object_new):
 *        struct _GDKWBuffers      *buffers;   struct _GDKWFrame *frame;
 *        struct _GDKWSurfaceScale *wscale;
 *      and forward declarations "struct _GDKWBuffers; struct _GDKWFrame;
 *      struct _GDKWSurfaceScale;" above it.  Change the comments of wl_buffer,
 *      shm_fd, shm_data, shm_size, stride, backing to the D2 table.  Keep the
 *      fields, the shape code and the test pixel hook read them.
 *  I2  Build.
 *      a. Generate and commit four files (scanner 1.26.0 like xdg-shell; the
 *         build never needs the scanner).  In the dev shell:
 *           P=$(pkg-config --variable=pkgdatadir wayland-protocols)
 *           wayland-scanner client-header $P/staging/fractional-scale/fractional-scale-v1.xml ydk/wayland/fractional-scale-v1-client.h
 *           wayland-scanner private-code  $P/staging/fractional-scale/fractional-scale-v1.xml ydk/wayland/fractional-scale-v1-protocol.c
 *           wayland-scanner client-header $P/stable/viewporter/viewporter.xml ydk/wayland/viewporter-client.h
 *           wayland-scanner private-code  $P/stable/viewporter/viewporter.xml ydk/wayland/viewporter-protocol.c
 *         (generated in the scratchpad: 264, 74, 394 and 75 lines.)
 *      b. wscript: libydk_wayland_sources gets
 *           ydk/wayland/gdkbuffer-wayland.c, ydk/wayland/gdkframe-wayland.c,
 *           ydk/wayland/gdkscale-wayland.c,
 *           ydk/wayland/fractional-scale-v1-protocol.c,
 *           ydk/wayland/viewporter-protocol.c
 *         in sorted position.  check_pkg ('cairo', 'CAIRO', '1.14').
 *      c. flake.nix postInstall: cp ydk/wayland/ydk/gdkm10-wayland.h
 *         "$out/include/ydk/".  No new package, no flake input change.
 *  I3  Core, ydk/gdkwindow.c (X11 behaviour unchanged).
 *      a. C1: add a static helper
 *           static void gdk_window_set_cairo_device_offset (GdkWindowObject *private)
 *         that reads cairo_surface_get_device_scale (private->cairo_surface,
 *         &sx, &sy) and calls cairo_surface_set_device_offset (surface,
 *         private->abs_x * sx, private->abs_y * sy).  Use it at the two
 *         existing call sites: recompute_visible_regions_internal () (after
 *         _gdk_windowing_set_cairo_surface_size) and
 *         gdk_window_ref_cairo_surface (). The paint stack call near line 3071
 *         is unchanged (paint pixmaps are never scaled).
 *      b. C2: add
 *           void _gdk_window_detach_cairo_surfaces (GdkWindow *window);
 *         It detaches private->cairo_surface of @window and of every
 *         descendant that is not native (gdk_window_has_impl () FALSE),
 *         recursively: cairo_surface_set_user_data (surface, &gdk_window_cairo_key,
 *         NULL, NULL), which runs gdk_window_cairo_surface_destroy and so
 *         clears the pointer and decrements outstanding_surfaces.  Do NOT call
 *         cairo_surface_finish.  No-op for a window with no cached surface.
 *         Declare it in ydk/ydk/gdkinternals.h (this header repeats the
 *         prototype; an identical redeclaration is legal C).
 *  I4  gdkdisplay-wayland.c, in this order:
 *      a. Add #include "gdkm10-wayland.h".
 *      b. Delete wl_output_geometry, wl_output_mode, wl_output_done and
 *         wl_output_listener.
 *      c. wl_registry_global (): the "wl_output" branch becomes
 *           _gdk_wayland_scale_bind_output (display, registry, name, version);
 *         (no "&& !display->wl_output" test: every output is bound).  The
 *         "wl_compositor" bind version becomes
 *         MIN (version, GDKW_COMPOSITOR_MAX_VERSION).  Add a final
 *           else if (_gdk_wayland_scale_bind_global (display, registry, name,
 *                                                     interface, version)) ;
 *         branch for wp_viewporter and wp_fractional_scale_manager_v1.
 *      d. wl_registry_global_remove (): call
 *           _gdk_wayland_scale_global_remove (display, name);
 *      e. _gdk_wayland_display_open (): replace the output_width /
 *         output_height fallback and the _gdk_wayland_screen_new call by
 *           _gdk_wayland_scale_screen_size (display, &w, &h);
 *           display->default_screen = _gdk_wayland_screen_new (..., w, h);
 *         display->output_width / output_height stay as the physical mode
 *         of the first output (the scale piece writes them).
 *      f. Delete the definitions of: _gdk_wayland_display_present_all,
 *         _gdk_wayland_windowing_window_process_updates_recurse,
 *         _gdk_wayland_windowing_before_process_all_updates,
 *         _gdk_wayland_windowing_after_process_all_updates,
 *         _gdk_wayland_windowing_set_cairo_surface_size.  The frame piece
 *         defines all five with the same signatures.  Keep the declarations
 *         (gdkprivate-wayland.h).
 *      g. gdkw_display_wayland_dispose () and _finalize (): after the windows
 *         are torn down and before wl_compositor_destroy, call
 *           _gdk_wayland_scale_display_finalize (GDK_DISPLAY_OBJECT (display));
 *         (idempotent: destroys viewporter, fractional manager, outputs) and
 *         delete the old "if (display->wl_output) wl_output_destroy" block.
 *         The sync and flush vfuncs keep calling _gdk_wayland_display_present_all.
 *  I5  gdkwindowimpl-wayland.c, in this order:
 *      a. Add the include.  Delete ensure_shm_buffer, resize_buffer,
 *         paint_background, _gdk_wayland_window_impl_present,
 *         _gdk_wayland_window_impl_resize_buffer, their static prototypes.
 *      b. gdkw_window_impl_wayland_create_cairo_surface () body:
 *           return _gdk_wayland_buffers_create_wrapper (impl, width, height);
 *         gdkw_window_impl_wayland_ref_cairo_surface () body:
 *           return _gdk_wayland_buffers_ref_backing (impl);
 *      c. xdg_surface_handle_configure (): the popup branch stays as it is.
 *         For toplevels, after ack and impl->configured = TRUE and
 *         _gdk_wayland_window_hints_mapped (impl), replace the final
 *         _gdk_wayland_window_impl_present (impl) by
 *           _gdk_wayland_frame_configured (impl);
 *      d. xdg_toplevel_handle_configure (): replace the whole resize block by
 *           if (width > 0 && height > 0)
 *             _gdk_wayland_frame_apply_configure_size (impl, width, height);
 *         placed BEFORE _gdk_wayland_window_hints_configure.
 *      e. _gdk_wayland_window_impl_create_toplevel () and
 *         gdk_window_wayland_show (): right after each wl_compositor_
 *         create_surface call that assigns impl->wl_surface add
 *           _gdk_wayland_scale_surface_created (impl);  (idempotent)
 *         In gdk_window_wayland_show () change the test
 *           if (impl->wl_surface) return;        (the "already shown" test)
 *         to
 *           if (impl->xdg_toplevel != NULL) return;
 *         and remove the duplicate surface creation so the surface is created
 *         once (create_toplevel creates it when NULL).
 *      f. gdk_window_wayland_hide (): replace "resize_buffer (...); if
 *         (impl->wl_surface) wl_surface_commit (...)" by
 *           _gdk_wayland_frame_unmap (impl);
 *         (it destroys xdg_toplevel and xdg_surface, D8).  Keep
 *         grab_check_unmap and the popup lines before it.
 *      g. gdk_window_wayland_move_resize (): after the width / height stores
 *         and the popup hook, add
 *           if (impl->popup == NULL && size changed)
 *             _gdk_wayland_frame_client_resize (impl, width, height);
 *         (tier 2 for the GDK_CONFIGURE part, the store part is required.)
 *      h. gdk_window_wayland_set_background (): replace paint_background (impl)
 *         by _gdk_wayland_buffers_paint_background (impl).
 *      i. _gdk_wayland_window_impl_teardown (): replace the block that
 *         destroys backing, wl_buffer, shm_data and shm_fd by
 *           _gdk_wayland_frame_teardown (impl);   (callbacks, timers, buffers)
 *         it must run before _gdk_wayland_popup_teardown.  Immediately before
 *         "if (impl->wl_surface) wl_surface_destroy" add
 *           _gdk_wayland_scale_surface_destroy (impl);
 *      j. gdkw_window_impl_wayland_init (): shm_fd = -1 stays, other shm
 *         fields stay 0.  No other change.
 *  I6  gdkpopup-wayland.c.  After each of the two wl_compositor_create_surface
 *      calls that assign impl->wl_surface (about lines 680 and 1434) add
 *      _gdk_wayland_scale_surface_created (impl).  Before each of the three
 *      wl_surface_destroy (impl->wl_surface) calls (about lines 433, 528, 667)
 *      add _gdk_wayland_scale_surface_destroy (impl).  The three following
 *      _gdk_wayland_window_impl_resize_buffer calls stay.  Nothing else:
 *      positioners are logical.
 *  I7  gdkscreen-wayland.c.  Add the include.  Bodies become:
 *        gdkw_screen_wayland_get_width / get_height: unchanged (the scale
 *          piece keeps GDKWScreenWayland.width / height current)
 *        _gdk_wayland_screen_get_width_mm / _height_mm:
 *          _gdk_wayland_scale_screen_mm (screen, &w, &h) fields
 *        _gdk_wayland_screen_get_n_monitors: _gdk_wayland_scale_n_monitors
 *        _gdk_wayland_screen_get_primary_monitor: 0
 *        _gdk_wayland_screen_get_monitor_geometry: _gdk_wayland_scale_monitor_geometry
 *        _gdk_wayland_screen_get_monitor_width_mm / height_mm:
 *          _gdk_wayland_scale_monitor_mm
 *        _gdk_wayland_screen_get_monitor_plug_name: _gdk_wayland_scale_monitor_plug_name
 *      Each delegate takes (display, ...); screen_dim_mm stays for the
 *      fallback.  The resolution code is untouched (D10).
 *  I8  gdkcursor-wayland.c.
 *      a. theme_load (): wl_cursor_theme_load (name, p->base.theme_size * k,
 *         shm) with k = _gdk_wayland_scale_cursor_scale (display); remember
 *         the k used in CursorPriv.loaded_scale.
 *      b. resolve (): a THEME result carries buffer_scale = loaded_scale,
 *         CUSTOM and FALLBACK carry 1.  Resolved gets a buffer_scale field.
 *      c. apply_cursor (): before attach call
 *         wl_surface_set_buffer_scale (s->surface, r.buffer_scale) when the
 *         surface version is >= 3; damage becomes wl_surface_damage (surface,
 *         0, 0, r.width / r.buffer_scale, r.height / r.buffer_scale); the
 *         hotspot passed to wl_pointer_set_cursor becomes
 *         (r.hot_x + k / 2) / k, same for y (k = r.buffer_scale).  After the
 *         application call
 *           _gdk_wayland_scale_note_cursor (display, r.buffer_scale, r.width,
 *              r.height, sent_hot_x, sent_hot_y, theme_px);
 *         where theme_px = theme_size * loaded_scale.  The dedupe test also
 *         compares buffer_scale.
 *      d. New function, exported:
 *           void _gdk_wayland_cursor_scale_changed (GdkDisplay *display);
 *         If _gdk_wayland_scale_cursor_scale (display) != loaded_scale:
 *         theme_load (p, NULL), applied_valid = FALSE, apply_effective (...).
 *      e. The pointer enter path needs nothing: apply_cursor reads the scale
 *         when it runs.
 *  I9  gdkevents-wayland.c: NO edit (F13).
 *  I10 gdkdrawable-wayland.c.
 *      a. gdkw_blit_surface (): the overlap temporary must keep the source's
 *         device scale.  Read cairo_surface_get_device_scale (ss, &sx, &sy),
 *         create tmp with ceil (w * sx) x ceil (h * sy) pixels, call
 *         cairo_surface_set_device_scale (tmp, sx, sy) before painting into it.
 *      b. (tier 2) New static helper gdkw_ref_surface_ro (impl): for a window
 *         impl returns _gdk_wayland_buffers_ref_backing_ro (impl), else
 *         _gdk_drawable_ref_cairo_surface (impl).  Use it in
 *         gdkw_draw_drawable_with_src for the SOURCE only.  Without it a
 *         window used as a blit source is marked unknown_dirty (one extra
 *         full commit, no wrong pixels).
 *  I11 gdkpixmap-wayland.c _gdk_wayland_drawable_copy_to_image (): after
 *      cairo_surface_flush, if cairo_surface_get_device_scale gives sx or sy
 *      != 1, replace @surface by a 1x ARGB32 image surface of size
 *      ceil (width / sx) x ceil (height / sy) filled with
 *      cairo_set_source_surface + CAIRO_OPERATOR_SOURCE + cairo_paint, and
 *      read pixels from that (destroy it at the end).  For a window use
 *      _gdk_wayland_buffers_ref_backing_ro () to get the source.  Pixel index
 *      == logical coordinate again.
 *  I12 gdkshape-wayland.c _gdk_wayland_test_window_pixel (): replace the raw
 *      shm_data read by _gdk_wayland_buffers_pixel (impl, x, y, argb).  The
 *      arguments stay LOGICAL; the buffers piece reads device pixel
 *      (x * scale120 / 120, y * scale120 / 120).  Nothing else in the file.
 *  I13 demo/ydk_demo.c.  Include <ydk/gdkm10-wayland.h>.  Before main () add
 *      #include "ydk_demo_m10.inc".  In main () after m9_exercise: add
 *      failed |= m10_exercise () != 0;.  Add the env switch
 *      YDK_DEMO_ONLY=m10real: skip m4..m9 and run only m10_real_exercise ()
 *      (tests piece defines it in the .inc).  No other demo edit.
 *  I14 flake.nix: install the header (I2c).  No new test derivation.
 *  I15 scripts/smoke-test.sh: require "ydk_demo: M10: PASS" in the weston
 *      phase.  New Phase 3: second weston with
 *        --backend=headless-backend.so --renderer=pixman --scale=2 --socket=smokescale
 *        --idle-time=0 --no-config
 *      then YDK_DEMO_ONLY=m10real GDK_BACKEND=wayland ydk-demo; require
 *      "ydk_demo: M10-REAL2: PASS".  scripts/valgrind-smoke.sh: the marker
 *      loop gets M10 (not the scale run).  Update the header comments.
 *      baseline-diff.sh stays as is and must still print MATCH.
 *  I16 PLANNING.md.  Rewrite the M10 section from these decisions (add a
 *      mermaid diagram of the frame state machine next to the table).  Add the
 *      D17 list to the known gaps.  Correct in place with [!warning]
 *      callouts: line 337 (the GDK_CONFIGURE gap is fixed), line 402 (cursor
 *      scale is done), the M9 line "What M10 still needs", the M5 note about
 *      resize_buffer and wrapper surfaces (fixed by the refcounted store),
 *      the "hide commits without NULL" gap.
 *
 *  Integration notes (2026-10-29, where the applied edits differ from the
 *  text above).
 *    - I7 delegates call the prototypes of the INTERNAL section below
 *      (_gdk_wayland_scale_screen_width_mm (display), not a _screen_mm helper).
 *    - I8d reloads the theme with the search path the M7 reload hook stored
 *      (CursorPriv.search_path), not NULL, and the pointer enter path also
 *      re-syncs the theme when the focus window has another scale.
 *    - _gdk_wayland_shape_before_present () returns gboolean (TRUE for a window
 *      with a bounding shape); the frame piece sends FULL damage for it,
 *      because the clear has no damage of its own.  _gdk_wayland_window_impl_
 *      present () also clears at once.
 *    - GDKWTestObjectCounts (gdkm9-wayland.h) gained the xdg_surface and
 *      xdg_toplevel created / destroyed ledger (_gdk_wayland_ledger_xdg), for
 *      the M9 audit tests.
 *    - The wscript check is cairo 1.14; a stale build/ needs "waf configure"
 *      (the older cache had no wayland-cursor).
 *    - gdkdisplay-wayland.c calls _gdk_wayland_scale_display_finalize from
 *      finalize only (dispose destroys the windows, finalize the outputs).
 *
 * ---------------------------------------------------------------------------
 * 5. TESTS.  m10_exercise () (all scales 1 on the default weston; hooks make
 * the others) prints "ydk_demo: M10: PASS".  m10_real_exercise () runs
 * against weston --scale=2 and prints "ydk_demo: M10-REAL2: PASS".  "real"
 * means the assertion follows protocol round trips with weston (a pump
 * helper dispatches events until a counter moves, up to 500 ms, 10 x that
 * under YDK_DEMO_VALGRIND).  "hook" means an injected event through the real
 * listener.  W is a new toplevel of 200 x 120 logical units, shown and
 * configured, with an expose handler that records and may draw.  Counters are
 * reset with _gdk_wayland_test_frame_reset_counters before each test.  Output
 * ids are never reused: the real headless output is id 1, and the text below
 * writes "id 2" and "id 3" for readability; the tests use the id_out value
 * that _gdk_wayland_test_output_add returns.  The tests assert frame_info
 * fields only for the window under test and read other state through the
 * hooks of the owning piece.
 *
 *  T1  Pacing and coalescing (real).  Wait for the first frame done; state
 *      IDLE.  One invalidate + gdk_window_process_updates (W, TRUE): commits
 *      1, commits_immediate 1, frame_requests 1, state WAITING.  Then four
 *      more invalidate + process_updates calls with disjoint 10x10 rects
 *      (0,0), (50,50), (100,20), (0,100), no main loop in between: commits 1,
 *      commit_requests 5, coalesced 4, commit_wanted TRUE, frame_requests 1.
 *      Pump to frame_callbacks_done 1: commits 2, commits_from_callback 1,
 *      commit_wanted FALSE, frame_requests 2.  last_damage covers exactly the
 *      four rects (total area 400, each rect contained).
 *  T2  Withheld and dropped callbacks (hook).  Frame mode HOLD: ten requests
 *      leave commits at 1 even after a 100 ms pump; held_done TRUE (the real
 *      callback arrived and was stashed); no timeout fired.
 *      _gdk_wayland_test_frame_release_held: commits 2.  Mode DROP with
 *      timeouts (30, 60) ms: pump 300 ms: frame_timeouts >= 3,
 *      commits_from_timeout >= 1, timeout_ms 60 (capped), consecutive_timeouts
 *      >= 3.  Back to NORMAL: the next real callback sets
 *      consecutive_timeouts 0 and timeout_ms 30 (base).
 *  T3  Immediate commit (hook).  IDLE (fire the outstanding callback with
 *      _gdk_wayland_test_frame_fire): one request gives commits +1 in the same
 *      call, commits_immediate +1.
 *  T4  Hidden window (real).  Hide W while WAITING with commit_wanted: state
 *      UNMAPPED, callback_outstanding FALSE, timeout_armed FALSE,
 *      commit_wanted FALSE, attach_null 1; object_counts xdg_toplevels and
 *      xdg_surfaces each -1 after a flush.  A direct present request while
 *      hidden: commits unchanged.  Show W: a new configure is delivered, acked
 *      (acked count +1), commits +1 with last_commit_full TRUE.  A callback
 *      after the re-map is NOT asserted (F7).
 *  T5  Release frees a buffer for reuse (real).  Six commits, each after the
 *      frame callback: buffer_counts.buffers_created <= 3 and at least one
 *      slot was attached twice (buffer_info.times_attached >= 2) and
 *      released_count >= 1 on some slot.  Accepts early release (F3).
 *  T6  Held release and poison (hook).  Frame mode HOLD (fire by hook),
 *      buffer mode HOLD.  Commit A (fill red); poison slot 0 with 0xDEADBEEF.
 *      Commits B, C, D (blue, green, white), each fired: slots 1, 2, 3 are
 *      used, slot 0 state SUPERSEDED, its checksum equals the checksum taken
 *      right after the poison, buffer_count 4.  Commit E: stalled_no_buffer 1,
 *      commit_wanted TRUE, buffer_count still 4, slot 0 unchanged.  Release
 *      slot 0 with _gdk_wayland_test_buffer_release: commits_from_release 1,
 *      slot 0 now ATTACHED, buffer_vs_backing mismatch 0, pixel (0,0) !=
 *      0xDEADBEEF.  With buffer_limit 2 the same test stalls at the third
 *      commit.
 *  T7  Stale region exactness (hook).  Buffer mode NORMAL, releases delivered
 *      by hook.  Fill W with pattern P (commit 1, slot 0), release.  Draw
 *      rect R1 = (10,10,30,20) (commit 2: slot 0 is free so it is reused or
 *      slot 1 is new; assert which with buffer_info).  Draw R2 = (100,50,40,40),
 *      commit 3 into the OTHER slot: last_bytes_copied == 4 * (area R1 +
 *      area R2) for a slot that was committed once before, and 4 * 200 * 120
 *      for a new slot.  After every commit buffer_vs_backing mismatch is 0.
 *  T8  Resize keeps old surfaces valid (hook + real, valgrind).  Take a ref
 *      of the target of gdk_cairo_create (W) as S_old; backing_info gives
 *      store pointer P_old and live_surfaces >= 2.  _gdk_wayland_test_
 *      toplevel_configure (W, 300, 200, 0): gdk_window_get_width 300, exactly
 *      one GDK_CONFIGURE (width 300, height 200) after the main loop, the
 *      store pointer differs, size_mismatches 0.  Draw a green rect on S_old
 *      with cairo: no crash, valgrind clean, the new store's pixels are NOT
 *      green.  stores_freed does not grow until S_old is destroyed, then
 *      grows by 1.  A new gdk_cairo_create (W) target is 300 x 200 pixels.
 *      After the settle idle (pump): a commit with attach size 300 x 200,
 *      last_commit_full TRUE, no commit before the settle idle ran.
 *  T9  Maximize gap (hook).  test_toplevel_configure (W, 400, 300, MAXIMIZED):
 *      order of events: GDK_CONFIGURE (400 x 300) then GDK_WINDOW_STATE with
 *      MAXIMIZED; gdk_window_get_width 400; W is fully invalidated.  Another
 *      configure with the same size and ACTIVATED: no GDK_CONFIGURE.  A
 *      configure with 0 x 0: no GDK_CONFIGURE, size kept.
 *  T10 Client resize (tier 2, real).  gdk_window_resize (W, 250, 150): one
 *      GDK_CONFIGURE (250 x 150) after the main loop; backing 250 x 150;
 *      the same call again gives no event; old surfaces valid.
 *  T11 Damage exactness (real).  invalidate (30,20,40,10) + process_updates
 *      at scale 1: last_damage is exactly that rect, last_commit_full FALSE.
 *      gdk_flush () outside a cycle then gives last_commit_full TRUE.  A
 *      gdk_draw_rectangle (W, ...) without invalidate marks the window
 *      unknown_dirty, and the next gdk_display_flush gives a FULL commit.
 *  T12 Integer scale 2 (hook, W 200 x 120).  Add fake output id 2 (scale 2,
 *      mode 2048 x 1280) and _gdk_wayland_test_surface_enter (W, id 2):
 *      scale_info.scale120 240, source OUTPUT; backing_info px 400 x 240,
 *      stride 1600, device scale (2, 2), logical 200 x 120;
 *      gdk_window_get_width still 200; one GDK_CONFIGURE (x, y, 200, 120);
 *      W invalidated; scale_info.buffer_scale_sent is still 1 and
 *      buffer_scale_requests unchanged BEFORE the next commit.  After the
 *      pump (settle or redraw commit): buffer_scale_sent 2, requests +1 in
 *      the same commit as the attach (frame_info.last_attach_buffer 400 x 240,
 *      both counters moved by exactly one), viewport destination (-1, -1),
 *      buffer_vs_backing mismatch 0, no buffer of 200 x 120 left after the
 *      releases, a roundtrip with weston still succeeds (no protocol error).
 *  T13 Pointer coordinates unchanged (hook).  Run the same sequence at scale 1
 *      and scale 2: pointer_enter (W, 33.5, 44.25), motion (W, 120.75, 60.5).
 *      Every GDK ENTER / MOTION has the injected x and y exactly; x_root and
 *      y_root equal the scale 1 run; gdk_window_get_pointer gives (120, 60).
 *  T14 Drawing in logical units (scale 2).  cairo rect (10,10,20,20) opaque
 *      red on gdk_cairo_create (W): backing device pixels (20..59, 20..59)
 *      are red, (19,19) and (60,60) are not; _gdk_wayland_test_window_pixel
 *      (W, 10, 10) is red.  A client side child C at (30, 40), 40 x 40:
 *      a cairo rect (0,0,5,5) on C lands at device pixels (60..69, 80..89)
 *      (C1, F8); its cached surface reports device offset (60, 80).
 *  T15 Back to scale 1.  Leave id 2 and remove the output: scale120 120, store
 *      200 x 120, device scale 1, after the commit buffer_scale_sent 1.
 *  T16 Multi-output (hook).  Outputs id 1 (scale 1) and id 2 (scale 2); enter
 *      both: 240.  Leave id 2: 120.  Enter id 2 again, then
 *      test_output_scale (id 2, 3): 360 and the store is 600 x 360.  Remove
 *      output 2 (global_remove path): 120 again.  Not entered anywhere: the
 *      HINT (largest output scale).
 *  T17 Fractional (hook).  fractional_force TRUE.  preferred_fractional (W,
 *      180): scale120 180, source FRACTIONAL, store 300 x 180, device scale
 *      (1.5, 1.5).  After the commit: set_buffer_scale last sent 1, viewport
 *      destination (200, 120), viewport_requests +1, weston still alive
 *      (real wp_viewporter).  A window of 101 x 51 gives a store of 152 x 77
 *      (round half up of 151.5 and 76.5) and destination (101, 51).
 *      preferred_fractional 240: integer path, destination (-1, -1),
 *      buffer scale 2.  fractional_force FALSE with 180 and no manager:
 *      priority falls through to the output / hint.  preferred_buffer_scale (W,
 *      3) with no fractional: source PREFERRED_INT, 360.
 *  T18 Screen and monitors (hook).  With the real weston output (1024 x 640,
 *      scale 1): n_monitors 1, monitor 0 geometry (0,0,1024,640), screen
 *      width 1024.  Add fake output id 3 at (1024, 0), mode 1600 x 1200,
 *      scale 2: n_monitors 2, monitor 1 geometry (1024, 0, 800, 600), screen
 *      1824 x 640, exactly one "size-changed" and one "monitors-changed" per
 *      done.  Transform 90 swaps width and height.  gdk_screen_get_resolution
 *      is the same before and after.  Remove id 3: back.
 *  T19 Cursor scale (hook, scale 2 focus).  The M7 generated theme gets 24 and
 *      48 pixel images (tests piece extends it).  pointer_enter (W): cursor
 *      scale_info buffer_scale 2, buffer 48 x 48, theme_px 48, hotspot sent =
 *      (hot + 1) / 2.  A pixbuf cursor: buffer_scale 1, hotspot unchanged.
 *      Changing W to scale 1 reapplies: buffer_scale 1, theme_px 24,
 *      set_count +1.
 *  T20 Popups at scale 2 (hook).  The M9 popup T1 sequence on a scale 2 base:
 *      popup_info (pos 80 x 40, anchor rect, root) equals the scale 1 values;
 *      the popup store is 160 x 80; its set_buffer_scale 2.
 *  T21 Shapes and get_image at scale 2.  shape_info regions equal the logical
 *      input; pixels outside the shape are transparent at device pixels;
 *      gdk_drawable_get_image (W, 0, 0, 200, 120) is 200 x 120 and pixel
 *      (10, 10) is the red of T14.
 *  T22 Scale change while hidden or WAITING.  Hidden: no GDK_CONFIGURE, no
 *      commit; after show the buffer is 2x.  WAITING (frame HOLD): the commit
 *      stays deferred; after fire exactly one commit with the new buffer and
 *      buffer_scale_requests +1 together.  Resize and scale change together:
 *      one commit, correct size.
 *  T23 Real HiDPI run, weston --scale=2 (m10_real_exercise).  Without hooks:
 *      monitor 0 geometry 1024 x 640; the main window has scale120 240 after
 *      its first configure (HINT, then OUTPUT after the real enter); the
 *      backing is twice the logical size; set_buffer_scale 2 was sent with the
 *      first buffer commit; ten paced commits with real callbacks and no
 *      protocol error; pointer coordinates (hook) unchanged; a hook fractional
 *      180 switches to the viewport path and weston stays alive.
 *  T24 Cycles and valgrind.  50 times: create W, show, draw, resize, scale
 *      change, hide, show, destroy.  After the loop and a pump: buffer_counts
 *      live_wl_buffers 0, live_stores 0 (drop S_old first), frame_counts
 *      timers 0, callbacks 0, settle idles 0.  scripts/valgrind-smoke.sh is
 *      clean (0 errors, no suppressions).
 *  T25 Compatibility.  m4 .. m9 exercises still print PASS on weston scale 1;
 *      baseline-diff prints MATCH.
 *
 * Mutation checks (a scratch copy with one fix undone at a time; each must
 * fail the demo): write to a slot that is not FREE (T6); skip the stale
 * region copy (T7); free the store at resize instead of refcounting (T8,
 * valgrind); ignore commit_wanted (T1); commit without waiting (T1); send
 * set_buffer_scale outside the commit (T12); do not scale the device offset
 * (T14); do not divide the cursor hotspot (T19); hide without attach NULL
 * (T4); ignore the 0 x 0 configure guard (T9).
 *
 * ---------------------------------------------------------------------------
 */

#ifndef __YDK_GDKM10_WAYLAND_H__
#define __YDK_GDKM10_WAYLAND_H__

#include <glib.h>
#include <glib-object.h>
#include <ydk/gdk.h>

G_BEGIN_DECLS

/* =========================================================================
 * PUBLIC: enums, info structs, headless test hooks
 *
 * Every hook returns TRUE when it did its job and FALSE when a precondition
 * failed (not a Wayland display or window, no state yet, bad index).  They
 * never emit warnings or criticals.  Event delivery to the application still
 * happens from the main loop, as for the M6 hooks.
 * ========================================================================= */

/* Scale120 is the unit of wp_fractional_scale_v1: 120 is 1.0. */
#define GDKW_TEST_SCALE_DENOM 120

/* Buffer size in device pixels for a logical size, D2 (half up). */
#define GDKW_TEST_SCALE_PX(logical, scale120) \
  ((gint) ((((guint64) (logical)) * (scale120) + 60) / 120))

typedef enum
{
  GDKW_TEST_FRAME_UNMAPPED = 0,
  GDKW_TEST_FRAME_IDLE,
  GDKW_TEST_FRAME_WAITING
} GDKWTestFrameState;

typedef enum
{
  GDKW_TEST_FRAME_MODE_NORMAL = 0, /* real callbacks act at once */
  GDKW_TEST_FRAME_MODE_HOLD,       /* real done events stashed, timeout off */
  GDKW_TEST_FRAME_MODE_DROP        /* real done events ignored, timeout runs */
} GDKWTestFrameMode;

typedef enum
{
  GDKW_TEST_BUFFER_MODE_NORMAL = 0, /* real releases act at once */
  GDKW_TEST_BUFFER_MODE_HOLD        /* real releases stashed per slot */
} GDKWTestBufferMode;

typedef enum
{
  GDKW_TEST_SLOT_FREE = 0,
  GDKW_TEST_SLOT_COPYING,
  GDKW_TEST_SLOT_ATTACHED,
  GDKW_TEST_SLOT_SUPERSEDED,
  GDKW_TEST_SLOT_RETIRED,
  GDKW_TEST_SLOT_NONE              /* no slot at that index */
} GDKWTestSlotState;

typedef enum
{
  GDKW_TEST_SCALE_HINT = 0,
  GDKW_TEST_SCALE_OUTPUT,
  GDKW_TEST_SCALE_PREFERRED_INT,
  GDKW_TEST_SCALE_FRACTIONAL
} GDKWTestScaleSource;

#define GDKW_TEST_MAX_DAMAGE 8
#define GDKW_TEST_MAX_ENTERED 8

/* ---- owner "frame": gdkframe-wayland.c -------------------------------- */

typedef struct
{
  GDKWTestFrameState state;
  gboolean commit_wanted;          /* a commit is deferred */
  gboolean callback_outstanding;   /* a wl_surface.frame callback is alive */
  gboolean held_done;              /* HOLD: a done event is stashed */
  gboolean ever_committed;         /* a buffer was committed since the map */
  gboolean timeout_armed;
  gboolean settle_armed;
  gboolean unknown_dirty;          /* a writer ran outside an expose scope */
  guint    timeout_ms;             /* current backoff value */
  guint    consecutive_timeouts;

  /* counters, zeroed by _gdk_wayland_test_frame_reset_counters */
  guint commit_requests;           /* every call of commit_request */
  guint commits;                   /* commits that attach a buffer */
  guint commits_immediate;         /* made inside the request call */
  guint commits_from_callback;
  guint commits_from_release;
  guint commits_from_timeout;
  guint commits_from_settle;
  guint coalesced;                 /* requests absorbed while WAITING */
  guint stalled_no_buffer;
  guint attaches;                  /* wl_surface_attach with a buffer */
  guint attach_null;
  guint empty_commits;             /* frame piece commits without a buffer */
  guint frame_requests;            /* wl_surface_frame */
  guint frame_callbacks_done;      /* done events acted upon (real or hook) */
  guint frame_timeouts;
  guint damage_calls;              /* damage_buffer (or damage) requests */
  guint expose_scopes;             /* process_updates hook runs, non empty */
  guint configure_events_queued;   /* GDK_CONFIGURE queued by the frame piece */

  /* the last commit that attached a buffer */
  gboolean     last_commit_full;
  gint         last_damage_n;
  GdkRectangle last_damage[GDKW_TEST_MAX_DAMAGE]; /* device pixels */
  gint         last_damage_area;                  /* device pixels */
  gint         last_attach_width, last_attach_height; /* device pixels */
  guint        last_slot;
  gsize        last_bytes_copied;
} GDKWTestFrameInfo;

typedef struct
{
  guint timers;                    /* armed frame timeout sources */
  guint settle_idles;              /* armed settle idle sources */
  guint callbacks;                 /* live frame callbacks */
  guint update_cycle_depth;        /* >0 inside process_all_updates */
} GDKWTestFrameCounts;

gboolean _gdk_wayland_test_frame_info           (GdkWindow         *window,
                                                 GDKWTestFrameInfo *info);
gboolean _gdk_wayland_test_frame_counts         (GdkDisplay          *display,
                                                 GDKWTestFrameCounts *counts);

/* Sets the mode for every window of the display.  Leaving HOLD does not
 * deliver stashed events (use release_held). */
gboolean _gdk_wayland_test_frame_mode           (GdkDisplay        *display,
                                                 GDKWTestFrameMode  mode);

/* Calls the REAL frame done listener for the outstanding callback of
 * @window (time = now in ms).  FALSE if no callback is outstanding.  The
 * listener destroys the callback, so a later real done is dropped. */
gboolean _gdk_wayland_test_frame_fire           (GdkWindow *window);

/* HOLD mode: runs the real done listener for a stashed event.  FALSE if none
 * is stashed. */
gboolean _gdk_wayland_test_frame_release_held   (GdkWindow *window);

/* Overrides the timeout base and cap for the display.  0, 0 restores
 * GDKW_FRAME_TIMEOUT_MS and GDKW_FRAME_TIMEOUT_MAX_MS. */
gboolean _gdk_wayland_test_frame_timeouts       (GdkDisplay *display,
                                                 guint       first_ms,
                                                 guint       max_ms);

gboolean _gdk_wayland_test_frame_reset_counters (GdkWindow *window);

/* Calls _gdk_wayland_window_impl_present (): FULL damage and a commit
 * request through the real path. */
gboolean _gdk_wayland_test_frame_present        (GdkWindow *window);

/* ---- owner "buffers": gdkbuffer-wayland.c ------------------------------ */

typedef struct
{
  GDKWTestSlotState state;         /* GDKW_TEST_SLOT_NONE if no such slot */
  gint     width, height;          /* device pixels */
  gint     stride;
  guint64  store_generation;       /* of the store it was made for */
  gint     stale_area;             /* device pixels still to copy */
  guint    times_attached;
  guint    released_count;
  gboolean held_release;           /* HOLD: a release is stashed */
  gboolean poisoned;               /* written by the poison hook */
} GDKWTestBufferInfo;

typedef struct
{
  gpointer data;                   /* back store pixels (for identity checks) */
  gint     stride;
  gint     px_width, px_height;    /* device pixels */
  gint     logical_width, logical_height;
  guint    scale120;
  gdouble  device_scale_x, device_scale_y; /* read back from impl->backing */
  guint    live_surfaces;          /* cairo surfaces holding the store */
  guint64  generation;
  guint    stores_created, stores_freed; /* since the display opened */
  guint    wrappers_created;
  guint    size_mismatches;        /* set_cairo_surface_size found a bad surface */
} GDKWTestBackingInfo;

typedef struct
{
  guint live_wl_buffers;           /* all states, all windows */
  guint retired_unreleased;
  guint live_stores;
  guint buffers_created;           /* since the display opened */
  guint buffers_destroyed;
  guint max_buffers;               /* the current per window limit */
} GDKWTestBufferCounts;

gboolean _gdk_wayland_test_buffer_mode      (GdkDisplay        *display,
                                             GDKWTestBufferMode mode);

/* Per window limit of slots; 0 restores GDKW_FRAME_MAX_BUFFERS (4). */
gboolean _gdk_wayland_test_buffer_limit     (GdkDisplay *display, guint max);

/* Number of slots (any state) of @window. */
guint    _gdk_wayland_test_buffer_count     (GdkWindow *window);

gboolean _gdk_wayland_test_buffer_info      (GdkWindow          *window,
                                             guint               index,
                                             GDKWTestBufferInfo *info);

/* Calls the REAL wl_buffer.release listener of slot @index, whether or not
 * the compositor sent one.  FALSE for a missing slot. */
gboolean _gdk_wayland_test_buffer_release   (GdkWindow *window, guint index);

/* Fills the whole memory of slot @index with @pattern (any state), then marks
 * the slot fully stale so a later reuse is still correct.  The hook for the
 * "never written while held" proof. */
gboolean _gdk_wayland_test_buffer_poison    (GdkWindow *window,
                                             guint      index,
                                             guint32    pattern);

/* Pixel (device x, y) of slot @index. */
gboolean _gdk_wayland_test_buffer_pixel     (GdkWindow *window, guint index,
                                             gint x, gint y, guint32 *argb);

/* FNV-1a 64 bit hash of the whole slot memory. */
gboolean _gdk_wayland_test_buffer_checksum  (GdkWindow *window, guint index,
                                             guint64 *checksum);

/* Number of device pixels where slot @index differs from the back store.
 * Only meaningful for the slot of the last commit, right after it. */
gboolean _gdk_wayland_test_buffer_vs_backing (GdkWindow *window, guint index,
                                              guint *mismatched);

gboolean _gdk_wayland_test_backing_info     (GdkWindow           *window,
                                             GDKWTestBackingInfo *info);

gboolean _gdk_wayland_test_buffer_counts    (GdkDisplay           *display,
                                             GDKWTestBufferCounts *counts);

/* ---- owner "scale": gdkscale-wayland.c --------------------------------- */

typedef struct
{
  guint               scale120;            /* effective */
  GDKWTestScaleSource source;
  gint                buffer_scale_sent;   /* last set_buffer_scale value, 1 at start */
  guint               buffer_scale_requests;
  gint                viewport_dest_w, viewport_dest_h; /* last sent, -1 -1 = unset */
  guint               viewport_requests;
  gboolean            has_viewport;        /* a wp_viewport object exists */
  gboolean            has_fractional;      /* a wp_fractional_scale_v1 exists */
  guint               preferred_int;       /* last preferred_buffer_scale, 0 none */
  guint               preferred_fractional;/* last preferred_scale, 0 none */
  guint               n_entered;
  guint               entered_ids[GDKW_TEST_MAX_ENTERED];
  guint               scale_changes;       /* times the effective scale changed */
} GDKWTestScaleInfo;

typedef struct
{
  guint    id;                    /* stable, 1 based, bind order */
  gboolean fake;                  /* made by the add hook */
  gint     x, y;                  /* wl_output.geometry */
  gint     mode_width, mode_height; /* device pixels */
  gint     scale;                 /* integer wl_output.scale */
  gint     transform;
  gint     logical_width, logical_height;
  gint     mm_width, mm_height;
  gchar    name[32];              /* "" until wl_output.name (v4) */
  gboolean done_seen;
  guint    surfaces_on;           /* windows that have entered it */
} GDKWTestOutputInfo;

typedef struct
{
  gint    buffer_scale;           /* of the last application */
  gint    buffer_width, buffer_height; /* device pixels */
  gint    hot_x_sent, hot_y_sent; /* passed to wl_pointer.set_cursor */
  gint    theme_px;               /* pixel size given to wl_cursor_theme_load */
  guint   applications;
} GDKWTestCursorScaleInfo;

gboolean _gdk_wayland_test_scale_info       (GdkWindow         *window,
                                             GDKWTestScaleInfo *info);

guint    _gdk_wayland_test_output_count     (GdkDisplay *display);
gboolean _gdk_wayland_test_output_info      (GdkDisplay         *display,
                                             guint               index,
                                             GDKWTestOutputInfo *info);

/* Adds a FAKE output (no proxy) and runs the real geometry, mode, scale and
 * done listeners for it.  @id_out gets the id. */
gboolean _gdk_wayland_test_output_add       (GdkDisplay *display,
                                             gint x, gint y,
                                             gint mode_width, gint mode_height,
                                             gint scale, gint transform,
                                             guint *id_out);

/* Real scale + done listeners.  Works for real and fake outputs. */
gboolean _gdk_wayland_test_output_scale     (GdkDisplay *display,
                                             guint id, gint scale);

/* Real mode + done listeners. */
gboolean _gdk_wayland_test_output_mode      (GdkDisplay *display, guint id,
                                             gint mode_width, gint mode_height);

/* The real global_remove path for the output (fake or real). */
gboolean _gdk_wayland_test_output_remove    (GdkDisplay *display, guint id);

/* Pushes the xdg_output events of an output through the real listeners: the
 * logical position and size and, with , xdg_output.done.  
 * is the version of the xdg_output object that the output pretends to have
 * (3 and up: no xdg_output.done is acted on, wl_output.done ends the update).
 * wl_output.done alone is _gdk_wayland_test_output_done. */
gboolean _gdk_wayland_test_output_xdg     (GdkDisplay *display, guint id,
                                           guint version,
                                           gint x, gint y,
                                           gint width, gint height,
                                           gboolean send_done);
gboolean _gdk_wayland_test_output_done    (GdkDisplay *display, guint id);

/* Real wl_surface enter / leave listeners with the output of @id. */
gboolean _gdk_wayland_test_surface_enter    (GdkWindow *window, guint id);
gboolean _gdk_wayland_test_surface_leave    (GdkWindow *window, guint id);

/* Real wl_surface.preferred_buffer_scale listener. */
gboolean _gdk_wayland_test_preferred_buffer_scale (GdkWindow *window,
                                                   gint       scale);

/* Real wp_fractional_scale_v1.preferred_scale listener. */
gboolean _gdk_wayland_test_preferred_fractional   (GdkWindow *window,
                                                   guint      scale120);

/* TRUE: behave as if wp_fractional_scale_manager_v1 were bound (needs
 * wp_viewporter for the viewport path).  FALSE: restore. */
gboolean _gdk_wayland_test_fractional_force (GdkDisplay *display,
                                             gboolean    enable);

gboolean _gdk_wayland_test_cursor_scale_info (GdkDisplay              *display,
                                              GDKWTestCursorScaleInfo *info);

/* ---- wire log (audit gaps of M10): what was really requested ------------
 *
 * Every wl_surface request of the frame, scale and cursor pieces goes through
 * a GDKW_WIRE_* wrapper (internal section).  The wrapper records the request
 * and its arguments in a fixed log while the log is enabled and then makes
 * the request.  Disabled, a wrapper costs one load of a global and a branch.
 * Removing the request from a call site removes the log entry too, so the
 * log shows what the compositor was sent, not what the bookkeeping says.
 *
 *   SET_BUFFER_SCALE  a = scale
 *   DAMAGE            a, b, c, d = x, y, width, height (surface units)
 *   DAMAGE_BUFFER     a, b, c, d = x, y, width, height (buffer units)
 *   SET_DESTINATION   a, b = width, height (-1, -1 unsets), on the viewport
 *                     of @surface
 *   ATTACH            a = 1 with a buffer, 0 for attach NULL
 *   FRAME, COMMIT     no arguments
 *   POPUP_GRAB        a = serial sent with xdg_popup.grab, b = 1 when the
 *                     seat is virtual and the request was recorded only;
 *                     @surface is the xdg_popup
 *   IN_KEY, IN_BUTTON the serial of a wl_keyboard.key or wl_pointer.button
 *                     event as it reached the listener (not a request): a =
 *                     serial, b = 1 for a press, 0 for a release.  A test can
 *                     compare the serial of a grab with the events that the
 *                     compositor really sent.
 */
typedef enum
{
  GDKW_TEST_WIRE_SET_BUFFER_SCALE = 1,
  GDKW_TEST_WIRE_DAMAGE,
  GDKW_TEST_WIRE_DAMAGE_BUFFER,
  GDKW_TEST_WIRE_SET_DESTINATION,
  GDKW_TEST_WIRE_ATTACH,
  GDKW_TEST_WIRE_FRAME,
  GDKW_TEST_WIRE_COMMIT,
  GDKW_TEST_WIRE_POPUP_GRAB,
  GDKW_TEST_WIRE_IN_KEY,
  GDKW_TEST_WIRE_IN_BUTTON
} GDKWTestWireKind;

#define GDKW_TEST_WIRE_MAX 1024

typedef struct
{
  GDKWTestWireKind kind;
  gconstpointer    surface;       /* the wl_surface (identity only) */
  gint             a, b, c, d;
} GDKWTestWireEntry;

/* Enables (and clears) or disables the log.  Process wide. */
gboolean _gdk_wayland_test_wire_enable  (gboolean on);
/* Empties the log; the log stays enabled. */
void     _gdk_wayland_test_wire_clear   (void);
/* Number of entries; @dropped (optional) counts entries lost to a full log. */
guint    _gdk_wayland_test_wire_count   (guint *dropped);
gboolean _gdk_wayland_test_wire_get     (guint index, GDKWTestWireEntry *entry);
/* Identity of the wl_surface of @window / of the pointer cursor surface. */
gconstpointer _gdk_wayland_test_wire_surface        (GdkWindow  *window);
gconstpointer _gdk_wayland_test_wire_cursor_surface (GdkDisplay *display);

/* TRUE: send_damage behaves as if the compositor bound wl_surface below
 * version 4 (surface unit damage computed from the device rects).  FALSE:
 * restore.  Process wide. */
gboolean _gdk_wayland_test_damage_pre_v4_force (gboolean enable);

G_END_DECLS

/* =========================================================================
 * INTERNAL
 * ========================================================================= */
#ifdef GDK_COMPILATION

#include <cairo.h>
#include <wayland-client.h>
#include <ydk/gdkinternals.h>
#include "gdkprivate-wayland.h"

G_BEGIN_DECLS

#define GDKW_SCALE_DENOM            120
#define GDKW_SCALE_MIN              120
#define GDKW_SCALE_MAX              960

#define GDKW_COMPOSITOR_MAX_VERSION 6
#define GDKW_OUTPUT_MAX_VERSION     4

#define GDKW_FRAME_MAX_BUFFERS      4
#define GDKW_FRAME_TIMEOUT_MS       250
#define GDKW_FRAME_TIMEOUT_MAX_MS   2000
#define GDKW_MAX_DAMAGE_RECTS       16

/* D2.  Device pixels of a logical length. */
#define GDKW_SCALE_PX(logical, scale120) \
  ((gint) ((((guint64) (logical)) * (scale120) + 60) / GDKW_SCALE_DENOM))

/* Wire log wrappers (see the public section).  The log and the pre-v4 switch
 * are owned by gdkframe-wayland.c. */
extern gboolean _gdkw_wire_enabled;
extern gboolean _gdkw_damage_pre_v4;
void _gdk_wayland_wire_log (GDKWTestWireKind kind, gconstpointer surface,
                            gint a, gint b, gint c, gint d);

#define GDKW_WIRE_LOG(kind, surf, a, b, c, d)                              \
  G_STMT_START {                                                           \
    if (G_UNLIKELY (_gdkw_wire_enabled))                                   \
      _gdk_wayland_wire_log ((kind), (surf), (a), (b), (c), (d));          \
  } G_STMT_END

#define GDKW_WIRE_SET_BUFFER_SCALE(surf, scale)                            \
  G_STMT_START {                                                           \
    GDKW_WIRE_LOG (GDKW_TEST_WIRE_SET_BUFFER_SCALE, (surf), (scale), 0, 0, 0); \
    wl_surface_set_buffer_scale ((surf), (scale));                         \
  } G_STMT_END
#define GDKW_WIRE_DAMAGE(surf, x, y, w, h)                                 \
  G_STMT_START {                                                           \
    GDKW_WIRE_LOG (GDKW_TEST_WIRE_DAMAGE, (surf), (x), (y), (w), (h));     \
    wl_surface_damage ((surf), (x), (y), (w), (h));                        \
  } G_STMT_END
#define GDKW_WIRE_DAMAGE_BUFFER(surf, x, y, w, h)                          \
  G_STMT_START {                                                           \
    GDKW_WIRE_LOG (GDKW_TEST_WIRE_DAMAGE_BUFFER, (surf), (x), (y), (w), (h)); \
    wl_surface_damage_buffer ((surf), (x), (y), (w), (h));                 \
  } G_STMT_END
#define GDKW_WIRE_SET_DESTINATION(surf, vp, w, h)                          \
  G_STMT_START {                                                           \
    GDKW_WIRE_LOG (GDKW_TEST_WIRE_SET_DESTINATION, (surf), (w), (h), 0, 0); \
    wp_viewport_set_destination ((vp), (w), (h));                          \
  } G_STMT_END
#define GDKW_WIRE_ATTACH(surf, buf)                                        \
  G_STMT_START {                                                           \
    GDKW_WIRE_LOG (GDKW_TEST_WIRE_ATTACH, (surf), (buf) != NULL, 0, 0, 0); \
    wl_surface_attach ((surf), (buf), 0, 0);                               \
  } G_STMT_END
#define GDKW_WIRE_COMMIT(surf)                                             \
  G_STMT_START {                                                           \
    GDKW_WIRE_LOG (GDKW_TEST_WIRE_COMMIT, (surf), 0, 0, 0, 0);             \
    wl_surface_commit ((surf));                                            \
  } G_STMT_END
/* an expression: returns the wl_callback */
#define GDKW_WIRE_FRAME(surf)                                              \
  (_gdkw_wire_enabled ? _gdk_wayland_wire_log (GDKW_TEST_WIRE_FRAME, (surf), 0, 0, 0, 0) : (void) 0, \
   wl_surface_frame ((surf)))

/* Opaque types.  The definition and every field belong to the owner. */
typedef struct _GDKWBuffers      GDKWBuffers;       /* "buffers" */
typedef struct _GDKWFrame        GDKWFrame;         /* "frame" */
typedef struct _GDKWSurfaceScale GDKWSurfaceScale;  /* "scale" */

typedef GDKWTestSlotState   GDKWSlotState;
typedef GDKWTestFrameState  GDKWFrameState;

/* Why a commit is requested.  The reason only picks the counter that the
 * commit increments.  Damage is added BEFORE the request by the caller, except
 * for GDKW_COMMIT_FULL, where the frame piece adds FULL damage itself. */
typedef enum
{
  GDKW_COMMIT_EXPOSE = 0,  /* damage came from an expose region */
  GDKW_COMMIT_FULL,        /* present (): FULL damage */
  GDKW_COMMIT_CALLBACK,
  GDKW_COMMIT_RELEASE,
  GDKW_COMMIT_TIMEOUT,
  GDKW_COMMIT_SETTLE,
  GDKW_COMMIT_CONFIGURE
} GDKWCommitReason;

typedef enum
{
  GDKW_PREPARE_OK = 0,
  GDKW_PREPARE_NO_FREE_SLOT, /* nothing was written; the commit stays wanted */
  GDKW_PREPARE_NO_STORE      /* no shm, or the window has no size */
} GDKWPrepareResult;

/* Everything the frame piece needs to attach, damage and commit one buffer.
 * Filled by _gdk_wayland_buffers_prepare (), consumed by the frame piece,
 * returned to _gdk_wayland_buffers_commit_done (). */
typedef struct
{
  struct wl_buffer *wl_buffer;
  guint             slot;
  gint              buffer_width, buffer_height;     /* device pixels */
  gint              logical_width, logical_height;
  guint             scale120;
  gboolean          full;                /* the frame damage was the whole buffer */
  gint              n_rects;
  GdkRectangle      rects[GDKW_MAX_DAMAGE_RECTS];   /* device pixels, frame damage */
  gsize             bytes_copied;        /* the stale copy of this slot */
} GDKWCommitInfo;

/* ---- defined by the integrator, declared here so the pieces compile ----- */

/* gdkwindow.c (I3b).  Identical redeclaration of gdkinternals.h is legal. */
void _gdk_window_detach_cairo_surfaces       (GdkWindow *window);

/* gdkcursor-wayland.c (I8d). */
void _gdk_wayland_cursor_scale_changed       (GdkDisplay *display);

/* ---- owner "buffers": gdkbuffer-wayland.c ------------------------------ */

/* The record of @impl, created on demand when @create is TRUE. */
GDKWBuffers *_gdk_wayland_buffers_get        (GDKWWindowImplWayland *impl,
                                              gboolean               create);

/* Makes the store match (impl->width, impl->height, scale120 of the scale
 * piece).  TRUE if a store exists afterwards.  When it replaces an existing
 * store it retires the slots, repoints impl->backing, impl->shm_data, stride
 * and shm_size, marks FULL damage, and calls _gdk_window_detach_cairo_surfaces
 * on the wrapper if it is alive.  FALSE without wl_shm, with a zero size, or
 * when allocation fails. */
gboolean     _gdk_wayland_buffers_ensure     (GDKWWindowImplWayland *impl);

/* WRITER entry used by the gdkw_* vfuncs (D5c).  New reference, device scale
 * set, NULL on failure.  Sets unknown_dirty when called outside an expose
 * scope (asks _gdk_wayland_frame_in_expose ()). */
cairo_surface_t *
             _gdk_wayland_buffers_ref_backing    (GDKWWindowImplWayland *impl);

/* READER entry: same surface, never marks damage. */
cairo_surface_t *
             _gdk_wayland_buffers_ref_backing_ro (GDKWWindowImplWayland *impl);

/* The core cache entry (D3): a fresh image surface over the store with device
 * scale and no offset.  @width and @height are what the core asked for; the
 * store is made to match the impl first, a different request is ignored. */
cairo_surface_t *
             _gdk_wayland_buffers_create_wrapper (GDKWWindowImplWayland *impl,
                                                  gint width, gint height);

/* Paints impl->background over the whole store (CAIRO_OPERATOR_SOURCE) and
 * marks FULL damage.  No-op without a store. */
void         _gdk_wayland_buffers_paint_background (GDKWWindowImplWayland *impl);

/* Damage in LOGICAL window coordinates; converted to device pixels (D2) and
 * unioned into the frame damage and the stale region of every slot. */
void         _gdk_wayland_buffers_damage_region (GDKWWindowImplWayland *impl,
                                                 const GdkRegion       *region);
void         _gdk_wayland_buffers_damage_all    (GDKWWindowImplWayland *impl);

/* TRUE if frame damage is pending. */
gboolean     _gdk_wayland_buffers_has_damage    (GDKWWindowImplWayland *impl);

/* The unknown_dirty flag of D5c: set by ref_backing outside an expose scope.
 * Returns the value; @clear TRUE resets it.  The frame piece clears it when it
 * turns the flag into FULL damage (present_all). */
gboolean     _gdk_wayland_buffers_unknown_dirty (GDKWWindowImplWayland *impl,
                                                 gboolean               clear);

/* Picks a FREE slot (R1), copies its stale region from the store, moves it to
 * COPYING, and fills @info.  Writes nothing when the result is not OK. */
GDKWPrepareResult
             _gdk_wayland_buffers_prepare    (GDKWWindowImplWayland *impl,
                                               GDKWCommitInfo        *info);

/* After the frame piece attached and committed @info: slot -> ATTACHED, the
 * previous ATTACHED -> SUPERSEDED, frame damage cleared, impl->wl_buffer set,
 * times_attached++. */
void         _gdk_wayland_buffers_commit_done (GDKWWindowImplWayland *impl,
                                               const GDKWCommitInfo  *info);

/* Drops the store (the cairo surfaces keep it alive), retires every slot
 * (D4), clears impl->backing, shm_data, stride, shm_size.  The impl size is
 * not touched.  Safe when the wl_surface is gone.  Detaches core surfaces. */
void         _gdk_wayland_buffers_reset      (GDKWWindowImplWayland *impl);

/* Teardown: destroys every wl_buffer and the record.  Idempotent. */
void         _gdk_wayland_buffers_free       (GDKWWindowImplWayland *impl);

/* Pixel at LOGICAL (x, y) read at device pixel (x * scale120 / 120,
 * y * scale120 / 120).  FALSE without a store or out of range. */
gboolean     _gdk_wayland_buffers_pixel      (GDKWWindowImplWayland *impl,
                                              gint x, gint y, guint32 *argb);

/* The check behind _gdk_windowing_set_cairo_surface_size (): TRUE if @surface
 * is not ours or its logical extent covers (@width, @height); FALSE counts a
 * size mismatch. */
gboolean     _gdk_wayland_buffers_surface_extent_ok (cairo_surface_t *surface,
                                                     gint width, gint height);

/* ---- owner "frame": gdkframe-wayland.c --------------------------------- */

/* The one entry for "this window should show its store".  See D6, D7.  Safe
 * for any impl (surfaceless, unmapped, not configured). */
void     _gdk_wayland_frame_commit_request   (GDKWWindowImplWayland *impl,
                                              GDKWCommitReason       reason);

/* TRUE while a process_updates scope is open for @impl (D5c). */
gboolean _gdk_wayland_frame_in_expose        (GDKWWindowImplWayland *impl);

/* From xdg_surface_handle_configure for a toplevel, after the ack (I5c).
 * First configure since the map (ever_committed FALSE): FULL damage and a
 * commit NOW (the state is UNMAPPED or IDLE, so it is immediate).  Later
 * configures: no commit; arm the settle idle only if the size or scale changed
 * since the last commit. */
void     _gdk_wayland_frame_configured       (GDKWWindowImplWayland *impl);

/* D9.  Compositor resize.  TRUE if the size changed. */
gboolean _gdk_wayland_frame_apply_configure_size (GDKWWindowImplWayland *impl,
                                                  gint width, gint height);

/* D9.  Client resize, called by move_resize after impl->width / height were
 * stored.  Does nothing if the buffers already match. */
void     _gdk_wayland_frame_client_resize    (GDKWWindowImplWayland *impl,
                                              gint width, gint height);

/* D10 S1 to S7.  Called by the scale piece only. */
void     _gdk_wayland_frame_set_scale        (GDKWWindowImplWayland *impl,
                                              guint scale120);

/* D8.  Toplevel hide. */
void     _gdk_wayland_frame_unmap            (GDKWWindowImplWayland *impl);

/* From the buffers release listener. */
void     _gdk_wayland_frame_buffer_freed     (GDKWWindowImplWayland *impl);

/* Destroys callback, timers, idles, the frame record, then
 * _gdk_wayland_buffers_free (impl).  Idempotent.  Runs before the wl_surface
 * is destroyed. */
void     _gdk_wayland_frame_teardown         (GDKWWindowImplWayland *impl);

/* Moved here from gdkwindowimpl-wayland.c and gdkdisplay-wayland.c (I4f,
 * I5a).  Signatures as declared in gdkprivate-wayland.h:
 *   void _gdk_wayland_window_impl_present (GDKWWindowImplWayland *impl);
 *       = FULL damage + commit request.
 *   void _gdk_wayland_window_impl_resize_buffer (impl, gint width, gint height);
 *       = M9 prototype: discard store and slots, set impl size, FULL damage,
 *         detach core surfaces; valid without a wl_surface.
 *   void _gdk_wayland_display_present_all (GDKWDisplayWayland *display);
 *       = D7 item 2, first statement stays _gdk_wayland_popup_commit_pending.
 *   void _gdk_wayland_windowing_window_process_updates_recurse (window, region);
 *       = D7 item 1.
 *   void _gdk_wayland_windowing_before_process_all_updates (void);
 *   void _gdk_wayland_windowing_after_process_all_updates (void);
 *   void _gdk_wayland_windowing_set_cairo_surface_size (surface, w, h);
 *       = calls _gdk_wayland_buffers_surface_extent_ok (), D3.
 */

/* ---- owner "scale": gdkscale-wayland.c --------------------------------- */

/* Registry (I4c, I4d).  bind_output binds MIN (version, 4), adds the listener,
 * records the output, mirrors the first one into GDKWOutputInfo,
 * display->wl_output, output_width and output_height. */
void     _gdk_wayland_scale_bind_output      (GDKWDisplayWayland *display,
                                              struct wl_registry *registry,
                                              guint32 name, guint32 version);

/* wp_viewporter and wp_fractional_scale_manager_v1.  TRUE if consumed. */
gboolean _gdk_wayland_scale_bind_global      (GDKWDisplayWayland *display,
                                              struct wl_registry *registry,
                                              guint32 name,
                                              const char *interface,
                                              guint32 version);

/* TRUE if @name was an output or one of the two globals. */
gboolean _gdk_wayland_scale_global_remove    (GDKWDisplayWayland *display,
                                              guint32 name);

/* Destroys outputs (wl_output_release at version >= 3), viewporter, manager.
 * Idempotent.  After all windows are torn down. */
void     _gdk_wayland_scale_display_finalize (GdkDisplay *display);

/* Per surface (I5e, I6).  created is idempotent: adds the wl_surface
 * listener, the fractional object if the manager exists, the viewport object
 * if the viewporter exists.  destroy releases them; idempotent. */
void     _gdk_wayland_scale_surface_created  (GDKWWindowImplWayland *impl);
void     _gdk_wayland_scale_surface_destroy  (GDKWWindowImplWayland *impl);

/* The effective scale120 of @impl (D10); also valid when @impl has no
 * surface state (returns the HINT).  Range GDKW_SCALE_MIN..MAX. */
guint    _gdk_wayland_scale_surface_scale120 (GDKWWindowImplWayland *impl);

/* Called by the frame piece inside a commit, after the copy and before the
 * attach, when the scale state is dirty or the buffer geometry changed.  It
 * decides integer or fractional path (D11) from @scale120 and the logical
 * size, makes only the requests whose value changed, and records the last
 * pair sent.  A no-op for an impl without a wl_surface. */
void     _gdk_wayland_scale_send_state       (GDKWWindowImplWayland *impl,
                                              guint scale120,
                                              gint logical_width,
                                              gint logical_height);

/* Screen and monitors in LOGICAL units (D12, I4e, I7).  Monitors without an
 * output answer like one 1024 x 768 monitor. */
void     _gdk_wayland_scale_screen_size      (GDKWDisplayWayland *display,
                                              gint *width, gint *height);
gint     _gdk_wayland_scale_n_monitors       (GdkDisplay *display);
void     _gdk_wayland_scale_monitor_geometry (GdkDisplay *display, gint monitor,
                                              GdkRectangle *dest);
/* mm of monitor @monitor; the physical size of the output, else computed at
 * 96 dpi from the logical size.  -1 for a bad monitor. */
gint     _gdk_wayland_scale_monitor_width_mm  (GdkDisplay *display, gint monitor);
gint     _gdk_wayland_scale_monitor_height_mm (GdkDisplay *display, gint monitor);
gchar   *_gdk_wayland_scale_monitor_plug_name (GdkDisplay *display, gint monitor);
/* mm of the whole screen (bounding box). */
gint     _gdk_wayland_scale_screen_width_mm   (GdkDisplay *display);
gint     _gdk_wayland_scale_screen_height_mm  (GdkDisplay *display);

/* Cursor (D13, I8).  Integer scale of the pointer focus window, else the
 * largest output scale, at least 1. */
gint     _gdk_wayland_scale_cursor_scale     (GdkDisplay *display);
void     _gdk_wayland_scale_note_cursor      (GdkDisplay *display,
                                              gint buffer_scale,
                                              gint buffer_width,
                                              gint buffer_height,
                                              gint hot_x_sent, gint hot_y_sent,
                                              gint theme_px);

G_END_DECLS

#endif /* GDK_COMPILATION */

#endif /* __YDK_GDKM10_WAYLAND_H__ */
