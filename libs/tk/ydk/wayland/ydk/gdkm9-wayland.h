/*
 * gdkm9-wayland.h
 *
 * ydk-wayland fork, M9 (popups, input-only windows, reparenting): the one
 * shared contract for the three M9 pieces, the decisions behind them, the
 * function ownership split, the call sites the integrator wires, and the
 * headless test hooks the demo drives.
 *
 * Layout of this header
 *
 *   1. MEASURED FACTS, OWNERSHIP, DECISIONS, INTEGRATOR CALL SITES, TESTS
 *      (this comment).
 *   2. PUBLIC section (no GDK_COMPILATION needed).  Enums, the GDKWTest*Info
 *      structs and the test hooks.  Depends on <ydk/gdk.h> alone, so the demo
 *      includes it as <ydk/gdkm9-wayland.h> once the integrator installs it
 *      (flake.nix postInstall).
 *   3. INTERNAL section (#ifdef GDK_COMPILATION).  The types that cross
 *      piece boundaries and the prototype of every function the pieces add
 *      or the integrator exports, each tagged with its owner.
 *
 * Include order inside the library: gdkprivate-wayland.h, gdkseat-wayland.h,
 * gdkm7-wayland.h, this header, then (last of all) gdkalias.h.
 *
 * ---------------------------------------------------------------------------
 * 1. MEASURED FACTS (probe against weston 16.0.0 headless, 2026-10-29)
 *
 *  F1 A TEMP window is an xdg_toplevel today.  gdk_window_show () on a
 *     GDK_WINDOW_TEMP window at (50, 60) sends get_toplevel, the position is
 *     ignored, and gdk_window_get_origin () answers (0, 0) instead of
 *     (50, 60) because get_root_coords () is the identity for every window.
 *  F2 A toplevel GDK_INPUT_ONLY window crashes gdk_window_new () on Wayland.
 *     The M8 guard reads "GDK_IS_DISPLAY_X11 (GDK_WINDOW_DISPLAY (parent))",
 *     but GDK_WINDOW_DISPLAY is the X11 macro (gdkprivate-x11.h): it casts
 *     the Wayland impl to GdkDrawableImplX11 and reads garbage
 *     (valgrind: Invalid read of size 8 at gdkwindow.c:1374, address 0x330).
 *     ytk makes exactly these windows: the GtkMenu grab transfer window
 *     (gtkmenu.c menu_grab_transfer_window_get) and every GtkInvisible
 *     (gtkinvisible.c, clipboard and drag source windows).
 *  F3 Crossings for CLIENT SIDE input-only children already work.  The core
 *     (gdkwindow.c proxy_pointer_event, _gdk_synthesize_crossing_events)
 *     turns the toplevel ENTER / MOTION that gdkevents-wayland.c queues into
 *     ENTER / LEAVE on children, input-only ones included.  Measured with the
 *     test hooks: pointer into an input-only child at (100,100) gives
 *     LEAVE(main, INFERIOR) then ENTER(child, ANCESTOR, x=50, y=50,
 *     x_root=150, y_root=150); out again gives the mirror pair.  M9 adds
 *     tests for this, not an implementation.  What M9 must fix around it is
 *     in D13.
 *  F4 xdg_wm_base is bound at version 1 (gdkdisplay-wayland.c).  Weston 16
 *     offers 5.  Everything popups need beyond the basics (reposition,
 *     repositioned, set_reactive, set_parent_size) is since 3.  The generated
 *     xdg-shell-client.h / xdg-shell-protocol.c (scanner 1.26.0) already
 *     contain all of it, so NO regeneration is needed; only the bind
 *     version changes (I4).
 *  F5 Headless weston: no wl_seat, so xdg_popup.grab cannot be sent.  It
 *     answers get_popup with xdg_popup.configure (x, y, w, h) equal to the
 *     request, then xdg_surface.configure.  It does NOT slide a popup that
 *     lies outside the output (headless output has no constraint), so a
 *     "compositor moved the popup" case needs the injection hooks.  Nested
 *     popups, reposition + repositioned(token), and destroying popups
 *     top-down all work.
 *  F6 GTK menu order (gtkmenu.c gtk_menu_popup, gtkcombobox.c):
 *       a. gdk_pointer_grab / gdk_keyboard_grab on the input-only TEMP
 *          transfer window at (-100, -100) 10x10, BEFORE the menu exists on
 *          screen (or on the parent menu shell's window for a submenu, or on
 *          a window of the base toplevel for a menu bar);
 *       b. gtk_window_set_transient_for (menu toplevel, parent toplevel);
 *       c. the menu toplevel (GDK_WINDOW_TEMP, hint POPUP_MENU) is moved to
 *          its ROOT position, then gdk_window_show ();
 *       d. only AFTER the show: gdk_pointer_grab on the menu's own window.
 *     xdg_popup.grab must precede the first commit, so the first commit
 *     cannot happen inside gdk_window_show () (see D5).
 *  F7 _gdk_wayland_windowing_pointer_grab () and _gdk_wayland_keyboard_grab ()
 *     return GDK_GRAB_NOT_VIEWABLE for a native window with no wl_surface.
 *     Once input-only toplevels stop owning a surface (D1), step (a) of F6
 *     would fail and gtk_menu_popup () would give up.  Fix in D15.
 *  F8 ytk handles a broken grab: GtkMenuShell::grab-broken-event with
 *     event->grab_window == NULL deactivates the menu (gtkmenushell.c
 *     gtk_menu_shell_grab_broken).  The core already queues GDK_GRAB_BROKEN
 *     when a grab is ended with implicit == TRUE (gdkdisplay.c
 *     _gdk_display_pointer_grab_update).
 *
 * ---------------------------------------------------------------------------
 * 2. OWNERSHIP.  No file is touched by two pieces.  Pieces never edit this
 * header and never edit a shared file.  If a contract here looks wrong, the
 * piece implements the contract anyway and reports the problem.
 *
 *   piece "popup"      ydk/wayland/gdkpopup-wayland.c      (NEW)
 *                        role choice, xdg_popup lifecycle, positioner,
 *                        grab policy, popup_done, popup tree, root origin,
 *                        object ledger and the popup / role / seat hooks.
 *   piece "inputonly"  ydk/wayland/gdkinputonly-wayland.c  (NEW)
 *                        surfaceless window rules, grab viewability,
 *                        pointer position in any window, root coordinates
 *                        for events, and the pointer movement hooks.
 *   piece "reparent"   ydk/wayland/gdkreparent-wayland.c   (NEW)
 *                        promotion (child to toplevel), toplevel move,
 *                        demotion (toplevel to child), grab breaking on
 *                        reparent, and the reparent counter hook.
 *
 * "integrator" (NOT a drafter piece): every edit in an existing file.
 * Shared files: ydk/gdkwindow.c, ydk/ydk/gdkwindowimpl.h,
 * ydk/wayland/gdkwindowimpl-wayland.c, gdkdisplay-wayland.c,
 * gdkevents-wayland.c, gdkwindow-hints-wayland.c, ydk/wayland/ydk/
 * gdkprivate-wayland.h, wscript, flake.nix, scripts/, demo/, PLANNING.md.
 * M9 needs NO new dispatcher in gdkbackend.c, NO new rename in
 * gdkwindowing-x11.h and NO edit in x11/gdkwindow-x11.c: every public name it
 * touches is core or already dispatched, and the one new GdkWindowImplIface
 * field is NULL for X11 and for the offscreen impl.
 *
 * The piece that talks xdg_popup / xdg_positioner is "popup".  No other piece
 * calls an xdg_popup_* or xdg_positioner_* request.  That one rule is what
 * makes every positioner and grab request observable by the test hooks.
 *
 * ---------------------------------------------------------------------------
 * 3. DECISIONS
 *
 * D1  ROLES.  Every native Wayland window gets exactly one role when it is
 *     shown, decided by _gdk_wayland_window_choose_role () (popup piece).
 *     First match wins:
 *       1. input_only                        -> NONE (reason INPUT_ONLY)
 *       2. GDK_WINDOW_CHILD (native child)   -> NONE (reason NATIVE_CHILD)
 *       3. type hint DND                     -> NONE (reason DND)
 *       4. GDK_WINDOW_TEMP, or a toplevel whose override_redirect is TRUE
 *                                            -> POPUP candidate
 *       5. everything else                   -> TOPLEVEL
 *     NONE means "mapped in the GDK sense only": no wl_surface, no xdg
 *     object, nothing sent.  Reasons: an input-only toplevel is the GTK grab
 *     transfer window or a GtkInvisible, it must never be visible.  A native
 *     CHILD exists after gdk_window_ensure_native () on a child (or in the
 *     middle of a reparent to the root); there are no wl_subsurfaces in M9,
 *     so it is not drawn (known gap).  A DND icon window gets no surface
 *     either (the icon is not drawn; wiring it to wl_data_device.start_drag
 *     is a DnD follow up).  All TEMP input-output windows are POPUP
 *     candidates whatever their type hint, because TEMP means override
 *     redirect: it positions itself in root coordinates.  A candidate with no
 *     usable parent becomes a plain xdg_toplevel (D3), which is the fallback
 *     the spec asks for: no decoration request, set_parent when a
 *     transient_for exists.
 *
 * D2  ROOT COORDINATES.  GDK expects one coordinate space where menus are
 *     positioned (gdk_window_get_origin, gdk_display_get_pointer, x_root).
 *     Wayland has none.  Decision: GDK root space is the surface space of the
 *     BASE toplevel of the popup family the user is working in; every base
 *     toplevel sits at (0, 0) (unchanged from M6).  A popup P has root
 *     position (P.x, P.y) = (private->x, private->y) of its GdkWindow.  It
 *     is absolute in root space, so a nested popup needs no extra sum.  The
 *     root origin of a window impl is therefore
 *       O (impl) = (0, 0)                      for a toplevel or fallback
 *       O (impl) = (impl->x, impl->y)          for a popup
 *     and every root conversion adds O (impl) of the NATIVE window.  This
 *     fixes F1 and makes all of these consistent:
 *       get_root_coords, get_deskrelative_origin (impl file): + O
 *       gdk_display_get_pointer (display file): px + O (last_pointer_impl)
 *       x_root / y_root of motion, button, scroll, crossing (events file):
 *         x + O (event impl)   via _gdk_wayland_event_root_coords ()
 *       gdk_window_get_root_origin / frame extents (hints file): O
 *     Several base toplevels share the root space only by convention (GTK
 *     computes positions from the toplevel of the attach widget): known gap.
 *
 * D3  PARENT CHOICE.  Done at COMMIT time (D5) so late state counts.  Order
 *     of candidates, first usable wins:
 *       a. transient_for of the window (new hints accessor, I9)
 *       b. seat->pointer_focus
 *       c. seat->keyboard_focus
 *       d. seat->last_pointer_impl
 *       e. the newest impl in display->windows that is an xdg_toplevel and
 *          is configured
 *     Usable = alive, not the window itself, same display, xdg_surface set,
 *     configured, and if it is a popup then COMMITTED or MAPPED and not being
 *     released.  The chosen candidate is then normalised:
 *       - candidate came from (a): keep it; every popup above it in its
 *         family stack is dismissed first (D9);
 *       - candidate came from (b) to (e): replace it by the TOPMOST live
 *         popup of the same family (so a tooltip nests on top of an open
 *         menu chain instead of closing it).
 *     No usable candidate: fallback toplevel (D1): the wl_surface made at show
 *     time gets _gdk_wayland_window_impl_create_toplevel () and
 *     impl->popup is freed.  The decision is final for this show; a hide and
 *     a new show decides again.
 *
 * D4  POSITIONER.  Let W be the popup window, (wx, wy) = (private->x,
 *     private->y), (w, h) = (impl->width, impl->height) clamped to >= 1,
 *     P the chosen parent and (ox, oy) = O (P).  The parent window geometry
 *     origin is its surface origin (the xdg_surface_listener sets geometry
 *     (0, 0, w, h)).  Requests, in this order, on a fresh positioner:
 *       xdg_positioner_set_size (w, h)
 *       xdg_positioner_set_anchor_rect (wx - ox, wy - oy, 1, 1)
 *       xdg_positioner_set_anchor (TOP_LEFT)
 *       xdg_positioner_set_gravity (BOTTOM_RIGHT)
 *       xdg_positioner_set_offset (0, 0)
 *       xdg_positioner_set_constraint_adjustment (SLIDE_X | SLIDE_Y)
 *     The positioner is destroyed right after get_popup / reposition.  No
 *     flip (GTK already flipped against the monitor, a second flip would
 *     mirror it), no resize, no reactive, no parent_size.  The compositor may
 *     still move the popup: the answer is xdg_popup.configure (x, y, w, h)
 *     relative to the parent geometry (D6).  The size is always set by the
 *     client through the positioner, never by a configure.
 *
 * D5  LIFECYCLE AND DEFERRED COMMIT.  show () of a POPUP candidate creates
 *     the wl_surface only, registers the impl in display->windows (so
 *     pointer and keyboard enter find it, and the test hooks work), creates
 *     the GDKWPopup record in state PENDING and arms ONE display idle source
 *     (priority GDKW_POPUP_COMMIT_PRIORITY, G_PRIORITY_HIGH).  The first
 *     commit is NOT made in show (F6: the grab comes after the show).
 *     _gdk_wayland_popup_commit_pending () runs from that idle, and from the
 *     start of _gdk_wayland_display_present_all () so gdk_flush () and
 *     display sync commit too.  For each PENDING popup, in creation order:
 *       1. wrapper destroyed or not mapped any more: drop the record;
 *       2. choose the parent (D3); none: fallback toplevel;
 *       3. dismiss popups above the parent where D3 says so;
 *       4. xdg_wm_base_get_xdg_surface on the existing wl_surface (stored in
 *          impl->xdg_surface, so the shared teardown and present code just
 *          work), attach the impl's xdg_surface listener, then
 *          xdg_surface_get_popup with the positioner of D4, add the popup
 *          listener;
 *       5. grab policy (D7): xdg_popup_grab;
 *       6. input region (D11);
 *       7. wl_surface_commit (no buffer; the buffer goes out after the
 *          configure is acked, through the normal present path);
 *       8. state COMMITTED, push on the family stack.
 *     hide () destroys everything: xdg_popup, xdg_surface, wl_surface (a
 *     wl_surface must not change role, so a new show makes a new one), drops
 *     the shm buffer, removes the impl from display->windows and frees the
 *     record.  A PENDING popup that is hidden just frees the record and the
 *     wl_surface.
 *
 * D6  CONFIGURE, ACK, POSITION FEEDBACK.  xdg_popup.configure (x, y, w, h)
 *     is stored (have_pending).  The xdg_surface.configure that follows calls
 *     _gdk_wayland_popup_surface_configure (impl, xdg_surface, serial):
 *       1. serial != 0: set_window_geometry (0, 0, w, h) and ack_configure.
 *          serial == 0 is an INJECTED configure (test hooks): do everything
 *          else but send neither request (a fake ack kills the connection);
 *       2. impl->configured = TRUE, state MAPPED;
 *       3. size differs from impl: _gdk_wayland_window_impl_resize_buffer ()
 *          and update private->width / height;
 *       4. new root position = O (parent) + (x, y).  If it or the size
 *          differs from what the GdkWindow holds: update private->x / y and
 *          impl->x / y, then queue GDK_CONFIGURE (heap event, gdk_event_put)
 *          with the root position and size, only when the window's event
 *          mask has GDK_STRUCTURE_MASK.  If nothing changed there is NO
 *          GDK_CONFIGURE;
 *       5. every descendant popup gets root = new O + its own parent-relative
 *          position and the same GDK_CONFIGURE rule.
 *     The caller then runs _gdk_wayland_window_impl_present ().  The
 *     repositioned (token) event only records the token (the configure pair
 *     that follows does the work).
 *
 * D7  GRAB POLICY.  Evaluated at commit time.  The popup asks for
 *     xdg_popup_grab (wl_seat, serial) iff ALL hold:
 *       - type hint is POPUP_MENU, DROPDOWN_MENU or COMBO (a tooltip and any
 *         other hint NEVER grabs);
 *       - the display has a wl_seat with pointer or keyboard capability, or
 *         the virtual seat of the tests is on;
 *       - an EXPLICIT GDK pointer grab exists (last entry of
 *         display->pointer_grabs with implicit == FALSE) or a GDK keyboard
 *         grab exists.  Whose window holds it does not matter: it may be the
 *         input-only transfer window, the popup, a parent popup, or a window
 *         of the base toplevel (menu bar);
 *       - a serial exists.  Serial rule: if a button is held
 *         (seat->buttons_down != 0) use seat->button_serial; else the newer
 *         (wraparound compare) of seat->key_serial and seat->button_serial.
 *         Zero means none.
 *     The decision, the serial and whether the request was real or virtual are
 *     recorded for the hooks.  If the compositor dismisses a grabbing popup
 *     BEFORE its first configure (it refused the serial, typical for a menu
 *     opened on button release), the popup is re-created ONCE without grab
 *     (retried_without_grab), and the GDK grabs are left alone.  The GDK
 *     grab keeps routing events inside the application either way.
 *     Parent rule (found in review, xdg-shell protocol error otherwise): the
 *     parent of a grabbing popup is an xdg_toplevel or another popup that
 *     itself sent a grab.  A popup that wants a grab therefore never hangs from
 *     a tooltip or from a popup retried without grab: the parent choice (D3)
 *     skips non grabbing popups for it, and if the chosen parent still cannot
 *     hold a grab the popup is made without one.
 *
 * D8  POPUP_DONE.  The compositor dismissed popup P (typically a click
 *     outside).  Real listener xdg_popup_handle_popup_done () does, in order:
 *       1. before P ever configured and grab_sent: retry (D7), stop;
 *       2. chain = P plus every popup above P in its family stack;
 *       3. GDK grabs: if the CHAIN held the grab (some member sent a grab and
 *          its parent is not a live grabbing popup outside the chain; looking
 *          only at P misses a tooltip below a grabbing popup), end the
 *          current pointer grab of the
 *          display whatever window holds it, and the keyboard grab, with
 *          implicit == TRUE so the core queues GDK_GRAB_BROKEN with
 *          grab_window NULL (F8):
 *            serial = _gdk_windowing_window_get_next_serial (display);
 *            _gdk_display_end_pointer_grab (display, serial, NULL, TRUE);
 *            _gdk_display_pointer_grab_update (display, serial);
 *            _gdk_wayland_seat_grab_clear (seat);
 *            if (display->keyboard_grab.window != NULL)
 *              _gdk_display_unset_has_keyboard_grab (display, TRUE);
 *          This happens BEFORE the windows are hidden so ytk deactivates the
 *          menu from the broken-grab event;
 *       4. for each chain member, TOPMOST first: gdk_window_hide (wrapper)
 *          (this runs the hide vfunc: the role is destroyed top-down, D9),
 *          then queue GDK_UNMAP on it when its event mask has
 *          GDK_STRUCTURE_MASK.  A destroyed wrapper is released silently.
 *     The GDK window state is hidden afterwards, so a later show by GTK makes
 *     a new popup.
 *
 * D9  ORDER OF DESTRUCTION.  xdg-shell: destroying a popup that is not the
 *     topmost is a protocol error, and a parent must outlive its popups.
 *     Rules: (a) each family (base toplevel plus its popups) keeps a stack in
 *     commit order, display wide in one list; (b) releasing popup P first
 *     releases every popup above P in the same family, topmost first, each
 *     with xdg_popup_destroy, xdg_surface_destroy, wl_surface_destroy;
 *     (c) any hide, destroy or teardown of a native window first releases the
 *     popups whose family contains it (hide_windows TRUE from hide and
 *     destroy: GDK windows are hidden; FALSE from teardown and display
 *     finalize: silent, wrappers may be gone); (d) creating a popup whose
 *     parent has popups above it dismisses them (D3); (e) after the role of
 *     an impl is released the seat must stop pointing at it:
 *     _gdk_wayland_seat_surface_gone () (I10) queues the LEAVE the compositor
 *     will not send.  Z-order is the stack order: later popups are above;
 *     raise, lower and restack are no-ops for popups.
 *
 * D10 SIZE AND MOVE.  A mapped popup's size or position changes through
 *     _gdk_wayland_popup_move_resize (): if the GDK values differ from the
 *     record and the xdg_popup proxy version is >= 3, send a new positioner
 *     with xdg_popup_reposition (token), token = a per display counter that
 *     starts at 1.  The size becomes real when the configure pair arrives
 *     (D6).  Version < 3 (an old compositor, or the force hook): release the
 *     role, dismiss descendants, go back to PENDING so the next commit makes
 *     a new popup.  A PENDING popup needs nothing: the commit reads the
 *     current values.  A changed transient_for on a live popup compares the
 *     new D3 parent with the current one and re-creates the role the same
 *     way (an xdg parent cannot change).
 *
 * D11 INPUT REGIONS.  Shapes from M7 apply as before
 *     (_gdk_wayland_shape_surface_created () after get_popup).  A popup with
 *     type hint TOOLTIP or NOTIFICATION and no stored input shape gets an
 *     EMPTY input region (wl_compositor_create_region, set_input_region,
 *     region destroy) so a tooltip under the pointer does not steal enter and
 *     leave from the widget below it.  M7 never resets the input region at
 *     present time, so this survives.
 *
 * D12 KEYBOARD FOCUS (tier 2).  A grabbing popup takes wl_keyboard focus,
 *     which would send GDK_FOCUS_CHANGE out on the base toplevel and make
 *     ytk think the window lost focus while a menu is open.  Rule: the GDK
 *     keyboard focus of a family is its BASE toplevel.  wl_keyboard.enter on
 *     a popup surface sets seat->keyboard_focus to the family root impl.
 *     wl_keyboard.leave on a surface of a family that has live popups defers
 *     the FOCUS_CHANGE (out) by one idle (G_PRIORITY_HIGH); an enter in the
 *     same family before it runs cancels it and sends nothing.  Key events
 *     still reach the menu through display->keyboard_grab redirection
 *     (emit_key).  Implemented by the integrator (I10) with
 *     _gdk_wayland_popup_family_root () and
 *     _gdk_wayland_popup_family_active ().
 *
 * D13 INPUT-ONLY WINDOWS.  What exists: client side input-only children are
 *     handled by the core (F3).  What M9 adds:
 *       - the crash fix (I2) and the surfaceless rule (D1);
 *       - grab acceptance for surfaceless natives (D15);
 *       - the pointer position model: "where is the pointer in window X",
 *         for any window including input-only and popups, and root
 *         coordinates for events (D2).  gdk_window_get_pointer () for a
 *         native window the pointer is not in (a menu grab window while the
 *         pointer is over the base) must report the position converted
 *         through root space instead of (0, 0);
 *       - tests with the shape and mask interplay from M7 and the core:
 *           input shape or bounding shape: point_in_window (core) already
 *           includes both, so a pointer in a hole produces no ENTER;
 *           event mask: an input-only window with only ENTER / LEAVE gets no
 *           MOTION; the toplevel mask already carries what proxying needs
 *           (get_native_event_mask);
 *           overlapping siblings: the topmost mapped sibling in the core's
 *           children order wins, raising an input-only sibling under a
 *           stationary pointer produces the crossings from the core's idle
 *           (_gdk_synthesize_crossing_events_for_geometry_change).
 *     No crossing code is written in the backend; if a test shows the core
 *     result is wrong the piece reports it.
 *
 * D14 REPARENT.  Three transitions, all through gdk_window_reparent ():
 *       child -> child (same or other toplevel): the core only (change_impl).
 *           The backend does nothing.  This is what ytk does (GtkHandleBox,
 *           gtk_widget_reparent).
 *       child -> toplevel (new parent is the root): the core calls
 *           gdk_window_ensure_native () which makes a NEW impl through
 *           _gdk_window_impl_new (role NONE while the type is still CHILD),
 *           then iface->reparent () -> _gdk_wayland_reparent (): break the
 *           grabs of the window subtree, store x / y as the new root
 *           position, return was_mapped.  TRUE makes the core call
 *           gdk_window_show_unraised () AFTER the type became TOPLEVEL, so
 *           the show vfunc now picks TOPLEVEL (or POPUP for TEMP) and the
 *           xdg objects appear.  The toplevel is empty until the core
 *           invalidates it (show_internal does).
 *       toplevel / popup -> child of another window: the core cannot demote a
 *           native window (X11 keeps a native child).  New core branch (I3)
 *           calls iface->demote_to_child () -> _gdk_wayland_reparent_demote ()
 *           when the window has an impl and the new parent is not the root:
 *           grabs of the subtree are broken (GRAB_BROKEN, grab_window NULL),
 *           popups of the family are released, and the role objects are
 *           destroyed (wl_surface, xdg_surface, xdg_toplevel or xdg_popup).
 *           Returning TRUE tells the core to hide the window, change_impl ()
 *           to the parent's impl, and continue as for a client side child.
 *           The old impl is then unreferenced and finalizes; its teardown
 *           removes it from display->windows and from the seat.
 *     State tied to the window: selection ownership, DnD registration and
 *     properties live on the GdkWindow (survive).  Cursor and shape caches
 *     live on the impl (rebuilt: the core's update_cursor and apply_shape
 *     run again; the reparent piece calls _gdk_wayland_cursor_update ()).
 *     Hints (title, app id, size hints) live on the impl and are LOST by a
 *     demote followed by a promote (known gap).  A pointer grab that
 *     belongs to the subtree is always broken on promote and on demote
 *     (conservative).  gdk_window_reparent () of a mapped popup to the root
 *     at (x, y) is a move: _gdk_wayland_popup_move_resize ().
 *
 * D15 GRABS AND SURFACELESS WINDOWS.  The grab validation of
 *     _gdk_wayland_windowing_pointer_grab () and _gdk_wayland_keyboard_grab ()
 *     becomes: reject if destroyed or not mapped; reject if the impl has no
 *     wl_surface AND _gdk_wayland_inputonly_grab_viewable (native) is FALSE.
 *     A PENDING popup has its wl_surface from show, so it passes.  Cursor
 *     code reached by a grab on a surfaceless native must not touch
 *     impl->wl_surface (check _gdk_wayland_cursor_update).
 *
 * D16 NOT DONE (deliberate).  Native child windows are not drawn (no
 *     wl_subsurface).  DND icon windows are not drawn.  No xdg_popup
 *     reactive mode, no parent_size, no flip.  Touch.  A menu opened on button
 *     release may lose the compositor grab (retry without grab only).
 *     Popups of a second base toplevel use that toplevel as their own root
 *     space.  The core computes x_root of crossings it synthesizes as
 *     toplevel_x + toplevel->x (private->x): right for popups, wrong for a
 *     base toplevel whose private->x is not 0 (existing behaviour).
 *
 * ---------------------------------------------------------------------------
 * 4. INTEGRATOR CALL SITES.  The callee is named; its owner is in brackets.
 * Tier 1 is everything except I11.  I0 to I3 must land BEFORE the pieces
 * compile.
 *
 *  I0  ydk/wayland/ydk/gdkprivate-wayland.h: forward declaration
 *        "struct _GDKWPopup;" and ONE new field at the end of
 *        struct _GDKWWindowImplWayland:  struct _GDKWPopup *popup;
 *      (NULL unless the impl is a PENDING / live popup).  Nothing else in
 *      that header changes.  flake.nix postInstall: install this header next
 *      to gdkm8-wayland.h:
 *        cp ydk/wayland/ydk/gdkm9-wayland.h "$out/include/ydk/"
 *  I1  ydk/ydk/gdkwindowimpl.h, struct _GdkWindowImplIface, after
 *      supports_native_bg add
 *        gboolean (* demote_to_child) (GdkWindow *window,
 *                                      GdkWindow *new_parent);
 *      X11 and offscreen leave it NULL (zero filled), so their behaviour is
 *      unchanged.
 *  I2  ydk/gdkwindow.c gdk_window_new (), the Xorg workaround (F2): both
 *      occurrences of GDK_WINDOW_DISPLAY (parent) become
 *      gdk_drawable_get_display (GDK_DRAWABLE (parent)):
 *        GDK_IS_DISPLAY_X11 (gdk_drawable_get_display (GDK_DRAWABLE (parent)))
 *        && !G_LIKELY (GDK_DISPLAY_X11 (gdk_drawable_get_display (
 *                        GDK_DRAWABLE (parent)))->trusted_client)
 *  I3  ydk/gdkwindow.c gdk_window_reparent (), replace
 *        "if (gdk_window_has_impl (private)) { ... show = impl_iface->
 *         reparent (...); } else { client side branch }"
 *      by a three way branch.  The first test is new, the other two are the
 *      existing bodies unchanged:
 *        if (gdk_window_has_impl (private) &&
 *            new_parent_private->window_type != GDK_WINDOW_ROOT &&
 *            new_parent_private->window_type != GDK_WINDOW_FOREIGN &&
 *            impl_iface->demote_to_child != NULL &&
 *            impl_iface->demote_to_child (window, new_parent))
 *          {
 *            show = was_mapped;
 *            gdk_window_hide (window);
 *            do_reparent_to_impl = TRUE;
 *            change_impl (private, new_parent_private->impl_window,
 *                         new_parent_private->impl);
 *          }
 *        else if (gdk_window_has_impl (private)) { existing native branch }
 *        else { existing client side branch }
 *      Keep "old_native_event_mask" and the later has_impl tests as they are:
 *      after the demotion gdk_window_has_impl () is FALSE.
 *  I4  gdkdisplay-wayland.c wl_registry_global (), "xdg_wm_base": bind
 *      version MIN (version, GDKW_XDG_WM_BASE_MAX_VERSION) instead of 1.  No
 *      listener struct change (the toplevel and popup events used are v1).
 *  I5  gdkdisplay-wayland.c _gdk_wayland_display_present_all (), first
 *      statement:  _gdk_wayland_popup_commit_pending (display);        [popup]
 *      (gdk_flush and display sync both reach it.)
 *  I6  gdkdisplay-wayland.c gdkw_display_wayland_dispose () and _finalize (),
 *      before the loop that tears windows down and before xdg_wm_base is
 *      destroyed:  _gdk_wayland_popup_display_finalize (GDK_DISPLAY_OBJECT
 *      (display));  Idempotent.                                       [popup]
 *  I7  gdkdisplay-wayland.c grabs and pointer:
 *      a. _gdk_wayland_windowing_pointer_grab (): the early test becomes
 *           impl == NULL || destroyed || !mapped ||
 *           (impl->wl_surface == NULL &&
 *            !_gdk_wayland_inputonly_grab_viewable (native))
 *         -> GDK_GRAB_NOT_VIEWABLE.                                [inputonly]
 *      b. _gdk_wayland_keyboard_grab (): same change for its
 *         "!mapped || impl->wl_surface == NULL" test.              [inputonly]
 *      c. _gdk_wayland_windowing_get_pointer (): after reading px / py call
 *           _gdk_wayland_pointer_root_position (display, x, y)
 *         instead of the raw cast.                                 [inputonly]
 *      _gdk_wayland_windowing_window_at_pointer () is unchanged (native
 *      wrapper plus coordinates relative to it; the core descends).
 *  I8  gdkwindowimpl-wayland.c, in this order of work:
 *      a. EXPORT four helpers (definitions, not static) with the prototypes of
 *         the INTERNAL section:
 *           _gdk_wayland_window_impl_create_toplevel (impl)  = the part of
 *             gdk_window_wayland_show () after wl_surface exists: xdg_surface,
 *             xdg_toplevel, both listeners, hints_apply, shape_surface_created,
 *             display->windows registration (skip it if already in the list),
 *             first buffer-less commit;
 *           _gdk_wayland_window_impl_resize_buffer (impl, w, h) = resize_buffer;
 *           _gdk_wayland_window_impl_grab_check_unmap (window) = grab_check_unmap;
 *           _gdk_wayland_window_impl_xdg_surface_listener () = &xdg_surface_listener.
 *      b. gdk_window_wayland_show (): after the display / compositor checks and
 *         BEFORE the "if (impl->wl_surface) return" test:
 *           if (_gdk_wayland_popup_show (window)) return;           [popup]
 *         then the old body, now calling _create_toplevel ().
 *      c. gdk_window_wayland_hide (): order: grab_check_unmap (existing);
 *           _gdk_wayland_popup_release_children (impl, TRUE);       [popup]
 *           if (_gdk_wayland_popup_hide (window)) return;           [popup]
 *         then the existing resize_buffer and commit.
 *      d. gdk_window_wayland_move_resize (): after storing x, y, width, height:
 *           _gdk_wayland_popup_move_resize (window, with_move, x, y,
 *                                           width, height);         [popup]
 *         (no-op unless impl->popup).  Store x / y only when with_move.
 *      e. xdg_surface_handle_configure (): first thing:
 *           if (impl->popup != NULL)
 *             {
 *               _gdk_wayland_popup_surface_configure (impl, xdg_surface,
 *                                                     serial);      [popup]
 *               _gdk_wayland_window_impl_present (impl);
 *               return;
 *             }
 *      f. gdk_window_wayland_reparent (): body becomes
 *           return _gdk_wayland_reparent (window, new_parent, x, y);
 *                                                                   [reparent]
 *         gdk_window_impl_iface_init (): add
 *           iface->demote_to_child = _gdk_wayland_reparent_demote;  [reparent]
 *      g. gdk_window_wayland_get_root_coords () and
 *         gdk_window_wayland_get_deskrelative_origin (): add
 *         _gdk_wayland_popup_root_origin (impl, &ox, &oy) to x, y.  [popup]
 *      h. gdk_window_wayland_get_pointer (): replace the body after the mask by
 *           inside = _gdk_wayland_pointer_in_window (display, window, &fx, &fy);
 *         write *x, *y from it, and return TRUE only if impl == pointer_focus
 *         (the core's contract).                                   [inputonly]
 *      i. gdk_window_wayland_destroy (): inside the existing display test and
 *         before _gdk_wayland_window_impl_teardown ():
 *           _gdk_wayland_popup_release_children (impl, TRUE);       [popup]
 *      j. _gdk_wayland_window_impl_teardown (): immediately before
 *         "if (impl->xdg_toplevel)":  _gdk_wayland_popup_teardown (impl);
 *         (destroys xdg_popup, frees the record, silent children release; the
 *         existing code then destroys xdg_surface and wl_surface.)  [popup]
 *      k. _gdk_wayland_window_impl_new (): after wmclass handling, if
 *         (attributes_mask & GDK_WA_NOREDIR) && attributes->override_redirect
 *         call _gdk_wayland_window_set_override_redirect (window, TRUE).
 *  I9  gdkwindow-hints-wayland.c:
 *      a. new accessors (definitions in the INTERNAL section):
 *           GdkWindow *_gdk_wayland_window_get_transient_for (window)
 *           gboolean _gdk_wayland_window_get_override_redirect (window)
 *      b. _gdk_wayland_window_set_transient_for (): at the end of every path
 *         that stores or clears the weak pointer call
 *           _gdk_wayland_popup_transient_changed (window);          [popup]
 *      c. _gdk_wayland_window_get_root_origin (): use
 *         _gdk_wayland_popup_root_origin on the toplevel impl, else 0, 0.
 *         _gdk_wayland_window_get_frame_extents (): rect->x / y the same.
 *  I10 gdkevents-wayland.c:
 *      a. x_root / y_root: queue_crossing (), wl_pointer_motion (),
 *         wl_pointer_button (), emit_scroll () set them with
 *           _gdk_wayland_event_root_coords (impl, px, py, &xr, &yr);
 *                                                                  [inputonly]
 *      b. define and export (declared in the INTERNAL section)
 *           void _gdk_wayland_seat_surface_gone (GDKWDisplayWayland *wd,
 *                                                GDKWWindowImplWayland *impl);
 *         = if seat->pointer_focus == impl run pointer_focus_lost (wd); if
 *         seat->keyboard_focus == impl clear it and cancel key repeat.  The
 *         popup piece calls it whenever it releases the role of an impl that
 *         stays alive.
 *      c. wl_pointer_enter (): unchanged logic; the surface of a popup is in
 *         display->windows, so _gdk_wayland_impl_from_surface finds it.
 *  I11 (tier 2) gdkevents-wayland.c wl_keyboard_enter () and
 *      wl_keyboard_leave (): D12, using _gdk_wayland_popup_family_root () and
 *      _gdk_wayland_popup_family_active ().  The pending out is one static
 *      idle per display; cancel it in release_keyboard () and on display
 *      finalize.
 *  I12 wscript, libydk_wayland_sources: add
 *           ydk/wayland/gdkpopup-wayland.c
 *           ydk/wayland/gdkinputonly-wayland.c
 *           ydk/wayland/gdkreparent-wayland.c
 *      NO scanner step and NO xdg-shell regeneration (F4).  flake.nix: the
 *      header install of I0.  scripts/smoke-test.sh: require
 *      "ydk_demo: M9: PASS" and extend the header comment;
 *      scripts/valgrind-smoke.sh: the loop becomes
 *      "for m in M4 M5 M5C M5V M6 M6B M7 M8 M9".
 *      scripts/audit-x11-symbols.sh: re-run only, no name list changes.
 *  I13 demo/ydk_demo.c: add m9_exercise () (TESTS below) and its call in
 *      main (), print "ydk_demo: M9: PASS".  Existing checks that stay valid:
 *      "root origin is (0,0)" (a plain toplevel), m8_temp () windows (never
 *      shown).  PLANNING.md: rewrite the M9 section from the decisions
 *      here, add the D16 list to the known gaps, and correct the M8 line
 *      "the Xorg workaround is now guarded with GDK_IS_DISPLAY_X11" with a
 *      [!warning] callout (F2: the guard itself crashed).
 *
 * ---------------------------------------------------------------------------
 * 5. TESTS.  m9_exercise () runs after the main window is configured and the
 * M6 recorder style is installed (gdk_event_handler_set).  "Real" means the
 * assertion follows a round trip with the headless weston (gdk_flush ()
 * delivers the configure pair); "hook" means an injected event through a REAL
 * listener function.  A virtual seat (hook) lets the grab request be recorded
 * without a wl_seat.  Values below are asserted exactly.
 *
 *  T1  popup at a known position (real).  Main toplevel configured at root
 *      (0, 0).  TEMP window 80x40 moved to root (50, 60), hint POPUP_MENU,
 *      transient_for main, shown.  Before gdk_flush (): role_info.role NONE,
 *      has_wl_surface TRUE, has_xdg_popup FALSE; popup_info.state PENDING.
 *      After gdk_flush (): role POPUP; popup_info: parent == main, depth 0,
 *      pos 80x40, anchor rect (50, 60, 1, 1), anchor TOP_LEFT (5), gravity
 *      BOTTOM_RIGHT (8), offset (0, 0), constraint SLIDE_X | SLIDE_Y (3),
 *      get_popup_requests 1, conf (50, 60, 80, 40) from weston, state
 *      MAPPED, root (50, 60).  No GDK_CONFIGURE event.
 *      gdk_window_get_origin (popup) == (50, 60) and a client side child at
 *      (3, 4) of the popup has origin (53, 64).
 *  T2  nested chain (real).  Submenu TEMP 60x30 at root (130, 70),
 *      transient_for the first popup.  popup_info: parent == first popup,
 *      depth 1, anchor rect (80, 10, 1, 1), conf (80, 10, 60, 30).  The popup
 *      stack from the stack hook is [first, second].
 *  T3  compositor correction (hook).  popup_configure (first, 40, 55, 80, 40):
 *      private x, y == (40, 55); one GDK_CONFIGURE for it with x 40, y 55,
 *      width 80, height 40; the submenu gets root (120, 65) and its own
 *      GDK_CONFIGURE.  popup_configure (first, 40, 55, 90, 40) changes the
 *      width: gdk_window_get_width == 90.  A configure equal to the present
 *      state sends NO GDK_CONFIGURE.  ack count does not move (serial 0).
 *  T4  grab sequence of gtk_menu_popup (hook, virtual seat on).  A button
 *      press hook gives serial S (seat_serials).  Create the input-only TEMP
 *      transfer window at (-100, -100) 10x10 with override_redirect, show it:
 *      role NONE, reason INPUT_ONLY, no wl_surface, gdk_pointer_grab ==
 *      GDK_GRAB_SUCCESS and gdk_keyboard_grab == GDK_GRAB_SUCCESS (both were
 *      NOT_VIEWABLE).  Create the menu TEMP (hint POPUP_MENU, transient_for
 *      main), show it, then gdk_pointer_grab on a child of the menu: SUCCESS
 *      while still PENDING.  gdk_flush (): popup_info.grab_wanted TRUE,
 *      grab_sent TRUE, grab_virtual TRUE, grab_serial == S, get_popup
 *      requests 1.
 *  T5  tooltip never grabs (hook).  Same grabs active, TEMP with hint TOOLTIP:
 *      grab_wanted FALSE, grab_sent FALSE, input_region_empty TRUE.  COMBO and
 *      DROPDOWN_MENU behave like POPUP_MENU.
 *  T6  popup_done ends the chain and the grab (hook).  Chain main > M1 (grab) >
 *      S1.  popup_done (M1): S1 hidden then M1 hidden (GDK_UNMAP order S1,
 *      M1); gdk_pointer_is_grabbed () FALSE; keyboard grab gone;
 *      GDK_GRAB_BROKEN events: pointer (keyboard FALSE, grab_window NULL) and
 *      keyboard (keyboard TRUE); role_info for both: no xdg_popup, no
 *      wl_surface; object_counts back to the value before M1 was shown.
 *  T7  popup_done on the top popup only (hook).  popup_done (S1) while M1
 *      still grabs: S1 hidden, M1 MAPPED, gdk_pointer_is_grabbed () TRUE, no
 *      GRAB_BROKEN.
 *  T8  no usable parent (real).  No transient_for, main window hidden so no
 *      focus and no configured toplevel: show a TEMP window: after flush
 *      role TOPLEVEL, fallback_toplevel TRUE, has_xdg_toplevel TRUE,
 *      has_xdg_popup FALSE, get_popup_requests 0.  Hide it: counts back.  Then
 *      a transient_for that is not configured yet: same fallback.
 *  T9  tooltip nests on the open chain (real).  With M1 and S1 live, a TOOLTIP
 *      TEMP without transient_for and pointer focus on main: its parent is S1
 *      (depth 2), M1 and S1 stay mapped.  A second POPUP_MENU with
 *      transient_for main while M1 is up dismisses M1 and S1 first.
 *  T10 reposition (real, then hook).  gdk_window_move_resize (first, 60, 70,
 *      90, 50): reposition_requests 1, reposition_token 1, anchor rect
 *      (60, 70, 1, 1), size 90x50; weston answers repositioned and configure;
 *      width 90.  With force_version (2): the same call re-creates the popup
 *      (get_popup_requests +1, reposition_requests unchanged) and descendants
 *      are hidden.
 *  T11 retry without grab (hook, virtual seat).  popup_done injected on a
 *      grabbing popup right after commit and before any configure:
 *      retried_without_grab TRUE, get_popup_requests 2, the second without
 *      grab, GDK pointer grab still held, no GRAB_BROKEN.
 *  T12 transient change (real).  set_transient_for on a mapped popup from main
 *      to a second toplevel: popup_info.parent == second, get_popup_requests
 *      +1.  gdk_window_reparent (popup, root, 20, 30): reparent counts +1 move,
 *      root (20, 30).
 *  T13 input-only crossings (hook, no protocol).  Main toplevel with A
 *      (input-output child at 20, 20, 200x200) and B (input-only child of A at
 *      50, 50, 100x100, mask ENTER | LEAVE | POINTER_MOTION).  move_to_window
 *      (main, 5, 5) then (B, 10, 10): LEAVE on main with detail INFERIOR, then
 *      ENTER on B with x 10, y 10, x_root 80, y_root 80, detail ANCESTOR,
 *      mode NORMAL (measured shape: F3).  Then move_to_window (main, 300,
 *      250): LEAVE on B (detail ANCESTOR, x 230, y 180 relative to B) and
 *      ENTER on main (detail INFERIOR).  Exact values, in this order.
 *  T14 input-only under an overlapping sibling.  C (input-output) overlaps B
 *      and is above it: pointer in the overlap gives ENTER on C and none on B;
 *      gdk_window_raise (B) with a stationary pointer gives LEAVE C and ENTER
 *      B from the core idle (run the main loop).  Input shape on B with a
 *      hole: pointer in the hole, no ENTER on B.  B with only ENTER mask:
 *      motion inside B produces no MOTION on B.  Show of B under a stationary
 *      pointer produces ENTER.
 *  T15 pointer in a popup (hook).  pointer_enter (popup, 5, 6) on a popup at
 *      root (50, 60): ENTER has x 5, y 6, x_root 55, y_root 66;
 *      gdk_display_get_pointer == (55, 66); gdk_window_get_pointer (main, &x,
 *      &y, NULL) returns NULL (the pointer is not in main) and still fills
 *      x = 55, y = 66 (root space converted to main's coordinates).
 *  T16 toplevel input-only and DND (real).  The creation that crashed now
 *      returns a window; show: role NONE, reason INPUT_ONLY, no wl_surface;
 *      destroy: no leak.  A TEMP with hint DND: reason DND.
 *  T17 reparent toplevel <-> child (real).  Window W (TOPLEVEL, 160x120)
 *      mapped with xdg objects; object_counts C0.  gdk_window_reparent (W,
 *      main, 30, 40): type CHILD, parent main, visible, gdk_window_get_position
 *      (30, 40), role_info own_impl FALSE, counts: wl_surfaces, xdg_surfaces,
 *      xdg_toplevels each C0 - 1.  move_to_window (W, 10, 10) gives ENTER on W
 *      with x 10, y 10.  Reparent back to the root at (0, 0): type TOPLEVEL,
 *      visible, after flush role TOPLEVEL and counts == C0.  A pointer grab
 *      on W before each step: GRAB_BROKEN, seat_info focus fields NULL after.
 *      A popup reparented under main (demote): hidden role, counts -1.
 *  T18 child to toplevel with a ready child (real).  Client side child K of
 *      main at (7, 8) 50x50, mapped, reparented to the root: native, own
 *      impl, role TOPLEVEL after the show, visible, type TOPLEVEL.
 *  T19 version (real).  object_counts.xdg_wm_base_version == MIN (advertised,
 *      3) and >= 3 on weston 16.
 *  T20 cycles and valgrind.  50 times: show menu chain of 3 plus a tooltip,
 *      popup_done or hide, destroy.  object_counts return to the baseline
 *      every cycle (positioners_created == positioners_destroyed, no pending
 *      idle).  scripts/valgrind-smoke.sh must be clean (0 errors, no
 *      suppressions).
 *  T21 (tier 2) keyboard focus coalescing (hook).  keyboard_enter (main);
 *      keyboard_leave (main); keyboard_enter (popup of main); run the main
 *      loop: no FOCUS_CHANGE out for main.  keyboard_leave (popup): one
 *      FOCUS_CHANGE out for main after the loop.
 *
 * ---------------------------------------------------------------------------
 */

#ifndef __YDK_GDKM9_WAYLAND_H__
#define __YDK_GDKM9_WAYLAND_H__

#include <glib.h>
#include <glib-object.h>
#include <ydk/gdk.h>

G_BEGIN_DECLS

/* =========================================================================
 * PUBLIC: enums, info structs, headless test hooks
 *
 * Every hook returns TRUE when it did its job and FALSE when a precondition
 * failed (not a Wayland display or window, no popup record, no proxy).  They
 * never emit warnings or criticals.  Event delivery to the application still
 * happens from the main loop, as for the M6 hooks.
 * ========================================================================= */

/* Mirrors of xdg-shell enum values, so the demo needs no wayland headers */
enum
{
  GDKW_TEST_ANCHOR_NONE = 0,
  GDKW_TEST_ANCHOR_TOP_LEFT = 5,
  GDKW_TEST_GRAVITY_NONE = 0,
  GDKW_TEST_GRAVITY_BOTTOM_RIGHT = 8,
  GDKW_TEST_CONSTRAINT_NONE = 0,
  GDKW_TEST_CONSTRAINT_SLIDE_X = 1,
  GDKW_TEST_CONSTRAINT_SLIDE_Y = 2,
  GDKW_TEST_CONSTRAINT_FLIP_X = 4,
  GDKW_TEST_CONSTRAINT_FLIP_Y = 8
};

typedef enum
{
  GDKW_TEST_ROLE_NONE = 0,       /* no xdg role: surfaceless or client side */
  GDKW_TEST_ROLE_TOPLEVEL,
  GDKW_TEST_ROLE_POPUP
} GDKWTestRole;

typedef enum
{
  GDKW_TEST_NOROLE_HAS_ROLE = 0, /* the window has a role */
  GDKW_TEST_NOROLE_NOT_SHOWN,    /* native window not shown yet, or pending */
  GDKW_TEST_NOROLE_INPUT_ONLY,
  GDKW_TEST_NOROLE_DND,
  GDKW_TEST_NOROLE_NATIVE_CHILD,
  GDKW_TEST_NOROLE_CLIENT_SIDE   /* shares the impl of an ancestor */
} GDKWTestNoRole;

typedef struct
{
  GDKWTestRole   role;
  GDKWTestNoRole norole_reason;
  gboolean       own_impl;         /* the window is its own impl window */
  gboolean       has_wl_surface;
  gboolean       has_xdg_surface;
  gboolean       has_xdg_toplevel;
  gboolean       has_xdg_popup;
  gboolean       configured;
  gboolean       fallback_toplevel; /* TEMP window that became an xdg_toplevel */
} GDKWTestRoleInfo;

typedef enum
{
  GDKW_TEST_POPUP_NONE = 0,      /* no popup record */
  GDKW_TEST_POPUP_PENDING,       /* shown, first commit not made */
  GDKW_TEST_POPUP_COMMITTED,     /* committed, no configure acked yet */
  GDKW_TEST_POPUP_MAPPED         /* configured at least once */
} GDKWTestPopupState;

/* What the popup piece last SENT and last RECEIVED for one popup window.
 * Pointers are borrowed. */
typedef struct
{
  GDKWTestPopupState state;
  GdkWindow *parent;             /* window whose xdg_surface is the parent */
  guint      depth;              /* 0 = the parent is an xdg_toplevel */
  gint       root_x, root_y;     /* GDK root position the backend holds */

  /* last positioner (xdg_surface.get_popup or xdg_popup.reposition) */
  gint       pos_width, pos_height;
  gint       anchor_x, anchor_y, anchor_w, anchor_h;
  guint      anchor, gravity, constraint_adjustment;
  gint       offset_x, offset_y;
  gboolean   reactive;
  guint      get_popup_requests;  /* since the window was shown */
  guint      reposition_requests;
  guint32    reposition_token;    /* last token sent */
  guint32    repositioned_token;  /* last token received */

  /* grab */
  gboolean   grab_wanted;         /* policy said yes at commit */
  gboolean   grab_sent;           /* request made (real or virtual) */
  gboolean   grab_virtual;        /* recorded only, no wl_seat */
  guint32    grab_serial;
  gboolean   retried_without_grab;

  /* last xdg_popup.configure, relative to the parent geometry */
  gint       conf_x, conf_y, conf_w, conf_h;
  guint      configure_events;
  guint      acked_configures;    /* real acks only (serial != 0) */
  guint      popup_done_events;
  gboolean   input_region_empty;  /* the tooltip rule of D11 was applied */
} GDKWTestPopupInfo;

/* Counts of live protocol objects of one display (ledger). */
typedef struct
{
  guint wl_surfaces;              /* impls with a wl_surface */
  guint xdg_surfaces;
  guint xdg_toplevels;
  guint xdg_popups;
  guint popups_pending;
  guint popup_stack;              /* committed popups, all families */
  guint positioners_created;
  guint positioners_destroyed;
  guint commit_sources;           /* armed deferred commit idles, 0 or 1 */
  guint registered_windows;       /* length of display->windows */
  guint xdg_wm_base_version;      /* proxy version, 0 without xdg_wm_base */

  /* M9 audit: a ledger of every xdg_surface and xdg_toplevel proxy ever
   * created and destroyed by the backend, counted at the request sites, not
   * by walking display->windows.  created - destroyed is the live number, so
   * a proxy that leaked from a window that left the list still shows. */
  guint xdg_surfaces_created, xdg_surfaces_destroyed;
  guint xdg_toplevels_created, xdg_toplevels_destroyed;
} GDKWTestObjectCounts;

typedef struct
{
  guint promotions;               /* child -> toplevel completed */
  guint demotions;                /* toplevel / popup -> child completed */
  guint moves;                    /* toplevel or popup moved by reparent */
  guint grabs_broken;             /* grab breaks done by reparent */
} GDKWTestReparentCounts;

/* ---- owner "popup": gdkpopup-wayland.c -------------------------------- */

/* Role of any window.  FALSE only for a non Wayland window. */
gboolean _gdk_wayland_test_role_info        (GdkWindow         *window,
                                             GDKWTestRoleInfo  *info);

/* TRUE for any live Wayland window.  info->state is GDKW_TEST_POPUP_NONE when
 * the window has no popup record, so tests can assert "not a popup".  FALSE
 * only for a bad or non Wayland window. */
gboolean _gdk_wayland_test_popup_info       (GdkWindow         *window,
                                             GDKWTestPopupInfo *info);

gboolean _gdk_wayland_test_object_counts    (GdkDisplay        *display,
                                             GDKWTestObjectCounts *counts);

/* Committed popups of all families, bottom to top.  Fills @out with at most
 * @max borrowed windows and returns the number of committed popups. */
guint    _gdk_wayland_test_popup_stack      (GdkDisplay        *display,
                                             GdkWindow        **out,
                                             guint              max);

/* Calls the REAL xdg_popup configure listener with (x, y, w, h) relative to
 * the parent geometry, then _gdk_wayland_popup_surface_configure () with
 * serial 0 (no set_window_geometry, no ack).  FALSE if the window has no
 * xdg_popup. */
gboolean _gdk_wayland_test_popup_configure  (GdkWindow *window,
                                             gint x, gint y,
                                             gint width, gint height);

/* Calls the REAL xdg_popup popup_done listener. */
gboolean _gdk_wayland_test_popup_done       (GdkWindow *window);

/* Calls the REAL xdg_popup repositioned listener. */
gboolean _gdk_wayland_test_popup_repositioned (GdkWindow *window,
                                               guint32    token);

/* On: the display counts as having a wl_seat with a pointer, and
 * xdg_popup.grab is RECORDED (grab_virtual TRUE) instead of sent.  Off:
 * back to the real seat. */
gboolean _gdk_wayland_test_popup_virtual_seat (GdkDisplay *display,
                                               gboolean    enable);

/* Makes the popup code believe xdg_wm_base is at @version (1 or 2 disables
 * reposition).  0 clears the override. */
gboolean _gdk_wayland_test_popup_force_version (GdkDisplay *display,
                                                guint       version);

/* The serials the grab policy reads from the seat. */
gboolean _gdk_wayland_test_seat_serials     (GdkDisplay *display,
                                             guint32    *button_serial,
                                             guint32    *key_serial,
                                             guint32    *last_serial);

/* ---- owner "inputonly": gdkinputonly-wayland.c ------------------------ */

/* Moves the pointer to (@x, @y) in the coordinates of @window.  @window may
 * be any live window of a Wayland display: a toplevel, a popup, a client
 * side child, an input-only child.  The position is converted to the
 * coordinates of the native window (summing private->x / y up to the impl
 * window).  If the seat has no pointer focus or another focus, it sends
 * leave for the old focus and enter for the native window through
 * _gdk_wayland_test_pointer_leave () / _enter (); otherwise one motion
 * through _gdk_wayland_test_pointer_motion ().  FALSE if the native window
 * has no wl_surface. */
gboolean _gdk_wayland_test_pointer_move_to_window (GdkDisplay *display,
                                                   guint32     time,
                                                   GdkWindow  *window,
                                                   gdouble     x,
                                                   gdouble     y);

/* Pointer position in ROOT coordinates (D2) and the native window that has
 * pointer focus (borrowed, NULL for none).  Any out pointer may be NULL. */
gboolean _gdk_wayland_test_pointer_root     (GdkDisplay  *display,
                                             gint        *root_x,
                                             gint        *root_y,
                                             GdkWindow  **focus_native);

/* ---- owner "reparent": gdkreparent-wayland.c -------------------------- */

gboolean _gdk_wayland_test_reparent_counts  (GdkDisplay             *display,
                                             GDKWTestReparentCounts *counts);

G_END_DECLS

/* =========================================================================
 * INTERNAL
 * ========================================================================= */
#ifdef GDK_COMPILATION

#include <wayland-client.h>
#include <ydk/gdkinternals.h>
#include "gdkprivate-wayland.h"

G_BEGIN_DECLS

struct xdg_surface;
struct xdg_surface_listener;

/* xdg_wm_base is bound at MIN (advertised, this).  Reposition and the
 * repositioned event are since 3. */
#define GDKW_XDG_WM_BASE_MAX_VERSION 3

/* Priority of the deferred first commit of a popup (D5). */
#define GDKW_POPUP_COMMIT_PRIORITY   G_PRIORITY_HIGH

/* Safety limit for walks along a parent chain. */
#define GDKW_POPUP_MAX_DEPTH         32

typedef enum
{
  GDKW_ROLE_NONE = 0,
  GDKW_ROLE_TOPLEVEL,
  GDKW_ROLE_POPUP
} GDKWRole;

typedef enum
{
  GDKW_NOROLE_NONE = 0,          /* the window has a role */
  GDKW_NOROLE_INPUT_ONLY,
  GDKW_NOROLE_NATIVE_CHILD,
  GDKW_NOROLE_DND
} GDKWNoRole;

typedef enum
{
  GDKW_POPUP_PENDING = 0,
  GDKW_POPUP_COMMITTED,
  GDKW_POPUP_MAPPED
} GDKWPopupState;

/* The popup record.  Hung off GDKWWindowImplWayland.popup (field added by
 * I0).  The full definition and the semantics of every field belong to the
 * popup piece; the other pieces and the integrator treat it as opaque and
 * go through the functions below. */
typedef struct _GDKWPopup GDKWPopup;

/* =========================================================================
 * Prototypes.  Every function is tagged with its owner.
 * ========================================================================= */

/* ---- owner "popup": gdkpopup-wayland.c -------------------------------- */

/* D1.  Pure decision.  @reason may be NULL.  Never touches the protocol. */
GDKWRole _gdk_wayland_window_choose_role   (GdkWindow  *window,
                                            GDKWNoRole *reason);

/* iface->show, first step (I8b).  TRUE: handled (surfaceless, already a
 * popup, or a new PENDING popup).  FALSE: continue as a plain toplevel. */
gboolean _gdk_wayland_popup_show           (GdkWindow *window);

/* iface->hide, after grab_check_unmap () and release_children () (I8c).  TRUE:
 * the window was a popup or surfaceless and everything is released; the
 * caller returns.  FALSE: a plain toplevel (or fallback toplevel), continue. */
gboolean _gdk_wayland_popup_hide           (GdkWindow *window);

/* iface->move_resize (I8d).  No-op unless the impl has a popup record. */
void     _gdk_wayland_popup_move_resize    (GdkWindow *window,
                                            gboolean   with_move,
                                            gint x, gint y,
                                            gint width, gint height);

/* xdg_surface.configure for a popup impl (I8e), see D6.  serial 0 = injected.
 * Sets impl->configured.  The caller presents afterwards. */
void     _gdk_wayland_popup_surface_configure (GDKWWindowImplWayland *impl,
                                               struct xdg_surface    *xdg_surface,
                                               guint32                serial);

/* D5.  Idempotent, runs from the idle, from present_all (I5) and from the
 * hooks.  Removes the idle when nothing is pending. */
void     _gdk_wayland_popup_commit_pending (GDKWDisplayWayland *display);

/* D9.  Releases every popup whose family stack lies above @impl (all popups
 * that have @impl as an ancestor, or, if @impl is a base toplevel, all of its
 * family), topmost first.  @hide_windows TRUE: gdk_window_hide () on live
 * wrappers.  FALSE: silent. */
void     _gdk_wayland_popup_release_children (GDKWWindowImplWayland *impl,
                                              gboolean               hide_windows);

/* I8j.  Destroys xdg_popup and the record of @impl, silent children release,
 * removes @impl from the stacks.  Leaves xdg_surface and wl_surface to the
 * caller.  Idempotent. */
void     _gdk_wayland_popup_teardown       (GDKWWindowImplWayland *impl);

/* M9 audit.  The ledger of xdg_surface / xdg_toplevel proxies.  Called next to
 * every xdg_wm_base_get_xdg_surface, xdg_surface_get_toplevel,
 * xdg_surface_destroy and xdg_toplevel_destroy. */
void     _gdk_wayland_ledger_xdg           (gboolean toplevel, gboolean created);

/* I6.  Releases every popup, destroys the idle, drops the registry.
 * Idempotent. */
void     _gdk_wayland_popup_display_finalize (GdkDisplay *display);

/* I9b.  D10: re-create the role of a live popup when its D3 parent changed. */
void     _gdk_wayland_popup_transient_changed (GdkWindow *window);

/* D2.  TRUE when @impl is a popup (@ox, @oy = its root position); FALSE for
 * everything else, with @ox, @oy = 0.  Either out pointer may be NULL. */
gboolean _gdk_wayland_popup_root_origin    (GDKWWindowImplWayland *impl,
                                            gint *ox, gint *oy);

/* Base toplevel impl of the family of @impl (@impl itself for a toplevel).
 * NULL for an impl that is not registered. */
GDKWWindowImplWayland *
         _gdk_wayland_popup_family_root    (GDKWWindowImplWayland *impl);

/* The window of the family of @focus (mapped popups, topmost first, then
 * the base toplevel) that holds the root point (@rx, @ry), or NULL.  Popups
 * with an empty input region never hold the pointer. */
GDKWWindowImplWayland *
         _gdk_wayland_popup_family_hit     (GDKWWindowImplWayland *focus,
                                            gdouble rx, gdouble ry);

/* TRUE if the family of @impl has at least one committed popup, or @impl is
 * a popup.  Used by the focus rule D12. */
gboolean _gdk_wayland_popup_family_active  (GDKWWindowImplWayland *impl);

/* ---- owner "inputonly": gdkinputonly-wayland.c ------------------------ */

/* D1 rules 1 to 3 as a predicate: TRUE if @window must never own a wl_surface
 * (input-only, native CHILD, DND hint).  FALSE for a non Wayland window. */
gboolean _gdk_wayland_inputonly_surfaceless (GdkWindow *window);

/* D15.  TRUE if the mapped native @native may hold a grab although it has no
 * wl_surface: it is surfaceless by the predicate above. */
gboolean _gdk_wayland_inputonly_grab_viewable (GdkWindow *native);

/* D2.  Pointer position in root coordinates: seat px / py plus the root origin
 * of last_pointer_impl.  The out pointers may be NULL.  (0, 0) for a pointer
 * never seen. */
void     _gdk_wayland_pointer_root_position (GdkDisplay *display,
                                             gint *root_x, gint *root_y);

/* Pointer position in the coordinates of any @window (native or client
 * side), through root space.  Returns TRUE if the point lies inside the
 * bounds of @window (no shape test).  *x and *y are always filled. */
gboolean _gdk_wayland_pointer_in_window    (GdkDisplay *display,
                                            GdkWindow  *window,
                                            gdouble *x, gdouble *y);

/* D2.  Root coordinates of a point (@x, @y) in the surface space of @impl.
 * @impl NULL: identity.  Either out pointer may be NULL. */
void     _gdk_wayland_event_root_coords    (GDKWWindowImplWayland *impl,
                                            gdouble x, gdouble y,
                                            gdouble *x_root, gdouble *y_root);

/* ---- owner "reparent": gdkreparent-wayland.c -------------------------- */

/* iface->reparent (I8f).  Called only for a window with its own impl whose
 * new parent is the root (a promotion in progress or a toplevel / popup move).
 * Steps in D14.  Returns was_mapped, which makes the core show it again. */
gboolean _gdk_wayland_reparent             (GdkWindow *window,
                                            GdkWindow *new_parent,
                                            gint x, gint y);

/* iface->demote_to_child (I1, I3, I8f).  Called for a window with its own
 * impl whose new parent is not the root.  Breaks grabs, releases popups of
 * the family and the roles of the window; returns TRUE (always, on Wayland)
 * so the core makes the window a client side child. */
gboolean _gdk_wayland_reparent_demote      (GdkWindow *window,
                                            GdkWindow *new_parent);

/* Ends the pointer and keyboard grabs whose window is @window or a
 * descendant, GRAB_BROKEN with grab_window NULL (core machinery, same calls as
 * grab_check_unmap).  Counts into the reparent counters. */
void     _gdk_wayland_reparent_break_grabs (GdkWindow *window);

/* ---- exported by the integrator (definitions live in the shared files) -- */

/* gdkwindowimpl-wayland.c (I8a) */
void     _gdk_wayland_window_impl_create_toplevel (GDKWWindowImplWayland *impl);
void     _gdk_wayland_window_impl_resize_buffer   (GDKWWindowImplWayland *impl,
                                                   gint width, gint height);
void     _gdk_wayland_window_impl_grab_check_unmap (GdkWindow *window);
const struct xdg_surface_listener *
         _gdk_wayland_window_impl_xdg_surface_listener (void);

/* gdkwindow-hints-wayland.c (I9a) */
GdkWindow *_gdk_wayland_window_get_transient_for     (GdkWindow *window);
gboolean   _gdk_wayland_window_get_override_redirect (GdkWindow *window);

/* gdkevents-wayland.c (I10b) */
void     _gdk_wayland_seat_surface_gone (GDKWDisplayWayland    *wd,
                                         GDKWWindowImplWayland *impl);

G_END_DECLS

#endif /* GDK_COMPILATION */

#endif /* __YDK_GDKM9_WAYLAND_H__ */
