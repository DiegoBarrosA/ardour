/*
 * gdkm8-wayland.h
 *
 * ydk-wayland fork, M8 (selection, clipboard, drag and drop): the one shared
 * contract for the four M8 pieces, the decisions behind them, the function
 * ownership split, the call sites the integrator wires, and the headless test
 * hooks the demo drives.
 *
 * Layout of this header
 *
 *   1. OWNERSHIP, INTEGRATOR CALL SITES, DECISIONS (this comment).
 *   2. PUBLIC section (no GDK_COMPILATION needed).  Enums, the GDKWTest*Info
 *      structs and the test hooks.  Depends on <ydk/gdk.h> alone, so the demo
 *      includes it as <ydk/gdkm8-wayland.h> once the integrator installs it
 *      (flake.nix postInstall).
 *   3. INTERNAL section (#ifdef GDK_COMPILATION).  The structs that cross
 *      piece boundaries (contract) and the prototypes of every function the
 *      pieces add, each tagged with its owner.
 *
 * Include order inside the library: gdkprivate-wayland.h, gdkseat-wayland.h,
 * this header, then (last of all) gdkalias.h, like the other wayland sources.
 * The primary selection protocol header ("primary-selection-unstable-v1-
 * client.h") is NOT included here: only the datadevice piece includes it.
 *
 * ---------------------------------------------------------------------------
 * OWNERSHIP.  No file is touched by two pieces.  Pieces never edit this
 * header and never edit a shared file.  If a contract here looks wrong, the
 * piece implements the contract anyway and reports the problem.
 *
 *   piece "property"    ydk/wayland/gdkproperty-wayland.c
 *   piece "selection"   ydk/wayland/gdkselection-wayland.c
 *   piece "datadevice"  ydk/wayland/gdkdatadevice-wayland.c   (NEW file)
 *   piece "dnd"         ydk/wayland/gdkdnd-wayland.c          (context type,
 *                                                              destination)
 *                       ydk/wayland/gdkdnd-source-wayland.c   (NEW file, source)
 *
 * Shared files, wired by the integrator only: ydk/gdkbackend.c,
 * ydk/x11/ydk/gdkwindowing-x11.h, ydk/wayland/ydk/gdkprivate-wayland.h,
 * ydk/wayland/ydk/gdkseat-wayland.h, wscript, flake.nix, demo/, scripts/,
 * PLANNING.md, gdkevents-wayland.c, gdkdisplay-wayland.c,
 * gdkwindowimpl-wayland.c, gdkwindow-hints-wayland.c, x11/gdkwindow-x11.c.
 * M8 needs NO new dispatcher in gdkbackend.c and NO new rename in
 * gdkwindowing-x11.h: every public name it touches (gdk_selection_*,
 * gdk_drag_*, gdk_drop_*, gdk_window_register_dnd, gdk_property_*,
 * gdk_atom_*) is dispatched already.
 *
 * The piece that "talks protocol" is datadevice.  No other piece calls a
 * wl_data_* or zwp_primary_selection_* request.  That one rule is what makes
 * every protocol request observable by the test hooks.
 *
 * ---------------------------------------------------------------------------
 * INTEGRATOR CALL SITES (the callee is named; the piece that owns the callee is
 * in brackets)
 *
 *  I1 gdkdisplay-wayland.c wl_registry_global ():
 *       "wl_data_device_manager":
 *           _gdk_wayland_dd_bind_manager (display, registry, name, version);
 *       "zwp_primary_selection_device_manager_v1":
 *           _gdk_wayland_dd_bind_primary_manager (display, registry, name,
 *                                                 version);
 *       Both are guarded by "not yet bound" like the other globals.  [datadevice]
 *  I2 gdkevents-wayland.c _gdk_wayland_seat_bind (), last statement:
 *           _gdk_wayland_dd_seat_ready (display);                  [datadevice]
 *  I3 gdkdisplay-wayland.c gdkw_display_wayland_dispose () and
 *       gdkw_display_wayland_finalize (), immediately before the existing
 *       _gdk_wayland_cursor_display_finalize () call (which is before
 *       _gdk_wayland_events_finalize ()):
 *           _gdk_wayland_dd_display_finalize (GDK_DISPLAY_OBJECT (display));
 *       Idempotent.  Destroys every proxy before wl_seat and wl_display go.
 *                                                                   [datadevice]
 *  I4 gdkwindowimpl-wayland.c gdk_window_wayland_destroy () (the GdkWindowImpl
 *       destroy vfunc, where @window is still a valid GdkWindow), inside the
 *       existing "impl->screen != NULL && display != NULL" test and before
 *       _gdk_wayland_window_impl_teardown ():
 *           _gdk_wayland_selection_window_destroyed (window);
 *       NOT in _gdk_wayland_window_impl_teardown (): that one also runs from
 *       display finalize, when the wrapper may already be gone.  Idempotent.
 *       That one function also calls _gdk_wayland_property_window_destroyed ()
 *       and _gdk_wayland_dnd_window_destroyed ().  Every record of the pieces
 *       that names a GdkWindow holds it through g_object_add_weak_pointer (),
 *       so a window finalized without this hook (display teardown) is
 *       safe.  Display teardown itself is I3.                         [selection]
 *  I5 gdkwindow-hints-wayland.c, the three existing NULL stubs:
 *       _gdk_wayland_window_foreign_new_for_display (display, anid)
 *           -> w = _gdk_wayland_native_id_lookup (display, anid);
 *              return w ? g_object_ref (w) : NULL;
 *       _gdk_wayland_window_lookup_for_display (display, anid)
 *           -> return _gdk_wayland_native_id_lookup (display, anid);
 *       _gdk_wayland_window_lookup (anid)
 *           -> same with gdk_display_get_default ().                  [selection]
 *  I6 x11/gdkwindow-x11.c, the public gdk_x11_window_foreign_new_for_display ():
 *       the existing "if (!GDK_IS_DISPLAY_X11 (display)) return NULL;" guard
 *       becomes "return gdk_window_foreign_new_for_display (display, window);"
 *       (the dispatcher, so it reaches I5).  Add the same guard to
 *       gdk_x11_window_lookup_for_display (), forwarding to
 *       gdk_window_lookup_for_display ().  ytk's _gtk_selection_request ()
 *       calls the X11 name to wrap the requestor of a SelectionRequest, so
 *       without this an external paste from our process cannot work.
 *  I7 wscript: add to libydk_wayland_sources
 *           ydk/wayland/gdkdatadevice-wayland.c
 *           ydk/wayland/gdkdnd-source-wayland.c
 *           ydk/wayland/primary-selection-unstable-v1-protocol.c
 *       The last file is generated and committed, like xdg-shell-protocol.c.
 *       Generate it ONCE from the dev shell (wayland-protocols and
 *       wayland-scanner are already in devShells.default; the XML is
 *       $WAYLAND_PROTOCOLS/unstable/primary-selection/
 *       primary-selection-unstable-v1.xml, checked present in
 *       wayland-protocols 1.49 and scanner 1.26.0 generates it cleanly):
 *           wayland-scanner client-header  <xml> ydk/wayland/primary-selection-unstable-v1-client.h
 *           wayland-scanner private-code   <xml> ydk/wayland/primary-selection-unstable-v1-protocol.c
 *       No flake input change, no wscript scanner step: the build never needs
 *       the XML.  This step must land BEFORE the datadevice piece compiles.
 *  I8 flake.nix postInstall: install this header next to gdkm7-wayland.h:
 *           cp ydk/wayland/ydk/gdkm8-wayland.h "$out/include/ydk/"
 *  I9 demo/ydk_demo.c: update the three M4 checks that assert stub behaviour
 *       (see DECISIONS: DEMO), add m8_exercise () and its call in main (),
 *       print "ydk_demo: M8: PASS".  scripts/smoke-test.sh and
 *       scripts/valgrind-smoke.sh: require "ydk_demo: M8: PASS" (the
 *       valgrind loop is "for m in M4 M5 M5C M5V M6 M6B M7": add M8).
 *       scripts/audit-x11-symbols.sh: re-run only; no name list changes.
 *  I10 PLANNING.md: M8 section rewritten from the DECISIONS below; the
 *       "Stubbed and reporting a safe value" list loses "selection
 *       notification" only for the parts made real (it stays FALSE, see
 *       DECISIONS: NOT DONE).
 *
 * ---------------------------------------------------------------------------
 * DECISIONS: THE MODEL
 *
 * GDK (X11 semantics, which GTK2 relies on) is property based.
 *   paste:  gdk_selection_convert (requestor, selection, target, time)
 *           -> owner stores the data in property GDK_SELECTION of @requestor
 *           -> GDK_SELECTION_NOTIFY on @requestor (property = GDK_SELECTION,
 *              or GDK_NONE for a refusal)
 *           -> gdk_selection_property_get (requestor, &data, &type, &format)
 *              -> gdk_property_delete (requestor, GDK_SELECTION).
 *   copy:   gdk_selection_owner_set_for_display (display, window, selection)
 *           -> later GDK_SELECTION_REQUEST {window = owner, requestor,
 *              selection, target, property, time}
 *           -> owner does gdk_property_change (requestor window, property,
 *              type, format, ...) and gdk_selection_send_notify_for_display
 *              (display, requestor, selection, target, property or GDK_NONE)
 *           -> GDK_SELECTION_CLEAR on the old owner only when ANOTHER CLIENT
 *              takes the selection.  An owner change inside the process sends
 *              no CLEAR from GDK: GTK sends it itself (see gtkselection.c
 *              gtk_selection_owner_set_for_display).
 * Wayland is fd based and asynchronous: wl_data_offer.receive (mime, fd) on the
 * paste side, wl_data_source.send (mime, fd) on the copy side, and
 * wl_data_device.set_selection / start_drag need the serial of a recent input
 * event.  The mapping rule is: EVERY transfer is expressed in GDK terms, a
 * (window, property) pair plus a requestor token, and the backend never gets
 * the data of its own owner by any other way than the GDK request protocol.
 * So a copy in window A and a paste in window B of the same client is the X11
 * sequence above with no compositor involved (the "loopback"), and it works
 * with no wl_seat, no wl_data_device and no wl_data_device_manager.
 *
 * DECISIONS: PROPERTY TABLE (piece property)
 *
 * The property functions become real for every GdkWindow of a Wayland display
 * (toplevel, child, TEMP, root; the window needs no wl_surface and need not be
 * mapped: GtkInvisible, the ipc and clipboard widgets, are unmapped TEMP
 * windows and must work).  Storage: a GHashTable atom -> {type, format,
 * nelements, bytes} kept as g_object_set_data_full () qdata
 * GDKW_PROPERTY_DATA_KEY on the GdkWindow, freed with the object or by
 * _gdk_wayland_property_window_destroyed ().
 *   - Item size: format 8 -> 1, 16 -> 2, 32 -> sizeof (long).  Type ATOM and
 *     ATOM_PAIR hold GdkAtom values (pointer sized) verbatim; GdkAtom is
 *     process local so there is NO conversion to or from "X atoms".
 *   - change: REPLACE, PREPEND, APPEND.  PREPEND or APPEND with another type or
 *     format than the stored one is ignored (X returns BadMatch).  A NULL
 *     @window means the default screen root window.  Destroyed or non Wayland
 *     windows are ignored.  nelements < 0 is ignored; nelements 0 with data
 *     NULL is allowed (empty property).  A change that would make one window
 *     hold more than GDKW_SELECTION_MAX_BYTES in total is ignored (g_debug).
 *   - get: X11 gdk_property_get semantics.  @offset is in 32 bit units,
 *     @length in bytes rounded up to 4 and 0 / G_MAXULONG are accepted; no
 *     g_warning for length 0 (return FALSE).  Not found: FALSE with the out
 *     parameters untouched.  @type != GDK_NONE and different from the stored
 *     type: FALSE, actual_property_type and actual_format_type filled, NO
 *     g_warning.  Found: TRUE, *actual_length in BYTES, *data a g_malloc'ed
 *     copy WITHOUT a trailing NUL (X11 gdk_property_get does not add one;
 *     empty property gives *data NULL).  @pdelete deletes the property when
 *     the whole value was returned.
 *   - delete: removes it.  No events.
 *   - No GDK_PROPERTY_NOTIFY is ever generated.  GTK uses it only for INCR,
 *     which this backend neither sends nor accepts (see SERVING).
 *   - The atom table (intern, name) is unchanged.
 *
 * DECISIONS: NATIVE IDS AND REQUESTOR TOKENS (piece selection)
 *
 * GdkEventSelection.requestor and gdk_selection_send_notify () take a
 * GdkNativeWindow (guint32).  Wayland has none, so the selection piece keeps a
 * per display table  id -> entry  with ids from a counter starting at
 * 0x00010000 (never 0, wrap skips 0 and live ids).  Entry kinds:
 *   WINDOW    one per GdkWindow, assigned lazily by _gdk_wayland_native_id_for_
 *             window () and kept until the window is destroyed.  It names a
 *             REQUESTOR in a loopback paste.
 *   SEND      one per outbound transfer (compositor asked our source for a
 *             mime).  Resolves to the OWNER window; the property used is a
 *             private atom "YDK_WL_SEND_<slot>" (pool of GDKW_MAX_SEND_SLOTS,
 *             so the atom table does not grow without bound).
 *   PROBE     one per TARGETS probe (see OWNERSHIP).  Resolves to the owner
 *             window; property atom "YDK_WL_PROBE_<kind>".
 *   DISCARD   one per synthesized request whose answer is thrown away (the
 *             DnD "DELETE" request).  Resolves to the owner window; property
 *             "YDK_WL_DISCARD".
 * _gdk_wayland_native_id_lookup () returns the GdkWindow an id resolves to
 * (borrowed), NULL for unknown, finished or destroyed entries.  The integrator
 * wires it into gdk_window_foreign_new_for_display / lookup (I5) and ytk's
 * existing _gtk_selection_request () then works: it wraps event->requestor,
 * does gdk_property_change (wrapper, event->property, ...) and
 * gdk_selection_send_notify_for_display (display, event->requestor, ...).
 * The window pointer inside an entry is a weak reference.
 *
 * DECISIONS: OWNERSHIP (piece selection, with datadevice for the protocol)
 *
 * Selection kinds (GDKWSelKind): CLIPBOARD, PRIMARY, and the pseudo selection
 * "XdndSelection" (DND), which is what gdk_drag_get_selection () returns for
 * every Wayland drag context.  Any other selection atom (SECONDARY, a private
 * one) is a LOCAL selection: owner table and loopback only, no protocol.
 *
 * gdk_selection_owner_set_for_display (display, owner, selection, time, send):
 *   1. Returns FALSE for a destroyed or non Wayland @owner, and for a @time
 *      older than the stored owner time (X rule: @time != 0, stored time != 0
 *      and (stored - time) < 0x80000000 and time != stored).  @send_event is
 *      ignored (X11 ignores it too).
 *   2. @owner NULL: removes the owner record.  Replacing or removing an owner
 *      NEVER queues a SELECTION_CLEAR (GTK does it).  If a wl_data_source
 *      for the old record exists it is destroyed (the compositor then clears the
 *      selection) and its pending outbound transfers are aborted (fds closed).
 *   3. Else records {selection, owner window (weak), time, generation + 1} and
 *      returns TRUE at once.  This is true with or without a data device.
 *   4. CLIPBOARD and PRIMARY with a usable device (_gdk_wayland_dd_usable ()):
 *      the compositor must learn the mime types BEFORE set_selection, and GDK
 *      does not pass targets.  So the selection piece asks the owner, as an X11
 *      client would: it queues a GDK_SELECTION_REQUEST {window = owner,
 *      requestor = PROBE token, selection, target = TARGETS, property = probe
 *      property, time}.  The answer arrives through the normal reply calls
 *      (gdk_property_change on the owner window, then send_notify with the
 *      PROBE token).  Reading property type ATOM / format 32 gives the target
 *      atoms.  The probe CANNOT be answered synchronously: ytk adds the owner
 *      to its current_selections list only AFTER gdk_selection_owner_set_
 *      for_display returns TRUE, so a direct call into the owner would be
 *      refused.  This asynchrony is a trap; do not shortcut it.
 *   5. On the probe answer (generation still current): build the source map
 *      (MIME MAP), create the wl source with the mapped mimes, and call
 *      _gdk_wayland_dd_set_selection (dd, kind, source, serial) with
 *      serial = GDKWSeat.last_serial at that moment (the newest serial of any
 *      input event; compositors check it is not older than the one of the
 *      previous selection, and a copy comes from a key or button event).  If
 *      the mapped mime list is empty, no source is created and the compositor
 *      selection is cleared with set_selection (NULL).  Probe timeout
 *      GDKW_PROBE_TIMEOUT_MS: the ownership stays local only.  A newer
 *      owner_set makes the older probe answer stale (generation check) and it
 *      is dropped.  PRIMARY is probed on every change; coalescing is the
 *      generation check.
 *   6. DND: no probe, no set_selection.  The targets come from the drag
 *      context's source map; the compositor learns the mimes from the drag
 *      source created by gdk_drag_begin ().
 *   7. No wl_seat serial yet (GDKWSeat.last_serial == 0): set_selection is
 *      still sent with serial 0; the compositor may ignore it, loopback is
 *      unaffected.
 * gdk_selection_owner_get_for_display (): the LOCAL owner window if the record
 *   exists and its window is alive, else NULL.  An external owner is never
 *   reported (Wayland cannot discover it).  GTK relies on this to shortcut
 *   in-process pastes and drags (gtk_selection_convert, gtk_drag_get_data).
 * wl_data_source.cancelled / zwp_primary_selection_source_v1.cancelled (another
 *   client took the selection): the record is removed, the source freed and
 *   a GDK_SELECTION_CLEAR {window = owner, selection, time = GDKWSeat.last_time}
 *   is queued, once.  A cancelled for a source that was already replaced or
 *   destroyed by us is ignored (handlers are detached on destroy).
 * _gdk_wayland_selection_window_destroyed (window): drops the ownership
 *   records of @window with no CLEAR, destroys their sources, aborts pending
 *   transfers that name @window, forgets its native id, then calls
 *   _gdk_wayland_property_window_destroyed (window) and
 *   _gdk_wayland_dnd_window_destroyed (window).
 *
 * DECISIONS: CONVERT (paste, piece selection)
 *
 * gdk_selection_convert (requestor, selection, target, time).  Returns quietly
 * (no event) for a destroyed or non Wayland @requestor, like X11.  Otherwise
 * exactly one GDK_SELECTION_NOTIFY is delivered to @requestor LATER, from the
 * main loop, never from inside this call.  Event: {window = requestor,
 * selection, target, property = GDK_SELECTION or GDK_NONE, time,
 * requestor = 0}.  Cases in this order:
 *   a. LOCAL owner exists (loopback): queue GDK_SELECTION_REQUEST {window =
 *      owner, selection, target, property = GDK_SELECTION, time, requestor =
 *      the WINDOW id of @requestor}.  Record the pending request {requestor,
 *      selection, target}.  The owner's reply is gdk_property_change on the
 *      requestor window plus gdk_selection_send_notify_for_display with that
 *      id: the selection piece turns the notify into the GDK_SELECTION_NOTIFY
 *      above (property copied from the notify).  No reply within
 *      GDKW_LOCAL_REQUEST_TIMEOUT_MS: NOTIFY with GDK_NONE.  Several pending
 *      requests for one requestor window clobber each other's property exactly
 *      as on X11; matching is by {requestor, selection, target}.
 *   b. else an EXTERNAL offer is current for the selection kind
 *      (dd->selection_offer[kind], or dd->dnd.dest_offer for DND):
 *        target TARGETS     answered at once from the offer's mimes: property
 *                           GDK_SELECTION type ATOM format 32, GdkAtom array
 *                           (see MIME MAP, inbound).  NOTIFY queued.
 *        target DELETE      (DND only) answered as success: property type
 *                           "NULL" format 8, zero bytes.  (The source deletes
 *                           its own data on dnd_finished.)
 *        TIMESTAMP, MULTIPLE, SAVE_TARGETS, INCR, ATOM_PAIR: refused.
 *        any other target   mapped to a mime (MIME MAP); no mime: refused.
 *                           Else _gdk_wayland_dd_offer_receive () reads the
 *                           data asynchronously; on success the bytes are
 *                           converted (GDKWConv), stored with
 *                           gdk_property_change semantics as property
 *                           GDK_SELECTION on @requestor {type, format 8},
 *                           then NOTIFY; on timeout, error or size limit:
 *                           NOTIFY with GDK_NONE and no property left behind.
 *      The result type is: UTF8_STRING for the UTF-8 mimes, STRING for target
 *      STRING, the target atom itself for everything else.
 *   c. else (no owner, no offer, unknown selection): NOTIFY with GDK_NONE.
 * gdk_selection_property_get (requestor, ...): X11 semantics on property
 *   GDK_SELECTION of @requestor, NOT deleting it.  Found: returns the byte
 *   length (0 for an empty value), *data a g_malloc'ed copy with ONE extra
 *   trailing NUL byte (non NULL even for length 0, because ytk treats a NULL
 *   buffer as failure), *ret_type, *ret_format.  ATOM or ATOM_PAIR: length is
 *   n * sizeof (GdkAtom) and *ret_format is 32.  Not found: returns 0, *data
 *   NULL, type GDK_NONE, format 0.  No g_critical for a destroyed window.
 *
 * DECISIONS: SERVING (copy, piece selection)
 *
 * The compositor asks for data with wl_data_source.send (mime, fd).  The
 * handler gets the fd (it owns and must always close it).  _gdk_wayland_
 * selection_serve (source, selection, map, mime, fd) then:
 *   - finds the owner record of @selection with a matching live source
 *     generation; none, or @mime not in the source map: close fd, done;
 *   - takes one of GDKW_MAX_SEND_SLOTS slots (none free: close fd);
 *   - queues GDK_SELECTION_REQUEST {window = owner, selection, target = map
 *     target, property = the slot atom, requestor = the SEND token, time =
 *     GDKWSeat.last_time};
 *   - the owner's reply is gdk_property_change on the owner window (reached
 *     through the SEND token) and send_notify with the SEND token.  On notify
 *     with property GDK_NONE: close fd.  Else read the property from the owner
 *     window with delete, refuse type INCR (close fd, g_debug), apply the
 *     conversion, write the bytes with _gdk_wayland_dd_write_async ()
 *     (non blocking, on the main context), close the fd at the end.
 *   - GDKW_SEND_TIMEOUT_MS without a notify: close fd, drop the slot.
 *   - The backend never asks the owner for TEXT, COMPOUND_TEXT or a locale
 *     charset target: ytk's COMPOUND_TEXT code path calls X11 only functions.
 *   - ytk sends INCR only above GTK_SELECTION_MAX_SIZE.  After the ytk patch
 *     below that limit is G_MAXINT on Wayland, so INCR never happens.
 *   - SIGPIPE: the first write path installs signal (SIGPIPE, SIG_IGN) if and
 *     only if the current disposition is SIG_DFL; EPIPE ends the transfer.
 *
 * DECISIONS: MIME MAP (pure functions, selection piece)
 *
 * Outbound, GDK target atoms the owner declared  ->  wl mimes offered.  Text
 * is one group.  The "serving target" of the group is the first present of
 * UTF8_STRING, STRING, "text/plain;charset=utf-8", "text/plain" (TEXT,
 * COMPOUND_TEXT and "text/plain;charset=<locale>" are ignored).  If the group
 * has a serving target, the mimes "text/plain;charset=utf-8", "text/plain" and
 * "UTF8_STRING" are offered, in this order, and every one of them is served by
 * asking the owner for the serving target, with conversion:
 *     UTF8_STRING                 GDKW_CONV_RAW
 *     STRING                      GDKW_CONV_LATIN1_TO_UTF8
 *     "text/plain;charset=utf-8"  GDKW_CONV_CRLF_TO_LF   (GTK2 writes CRLF)
 *     "text/plain"                GDKW_CONV_CRLF_TO_LF
 * "text/uri-list" -> the mime "text/uri-list", RAW.  The pseudo targets TARGETS,
 * MULTIPLE, TIMESTAMP, SAVE_TARGETS, DELETE, ATOM_PAIR, INCR, NULL are never
 * offered.  EVERY OTHER atom is offered under its own name as the mime (an
 * application type "x-ardour/region" or "image/png" survives unchanged) and
 * served RAW with the atom itself as target.  Duplicates are dropped; order
 * follows the owner's list, with the text group at the position of its first
 * member.
 * Inbound, wl mimes of an offer  ->  GDK targets advertised (TARGETS reply and
 * the DnD context target list):
 *     any of "text/plain;charset=utf-8", "UTF8_STRING", "text/plain"
 *         -> UTF8_STRING, then STRING.  UTF8_STRING fetches the first present
 *            mime of {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain"}
 *            and returns it RAW; if the bytes are not valid UTF-8 they are read
 *            as ISO-8859-1.  STRING fetches the same mime and converts UTF-8
 *            to ISO-8859-1 with "?" for characters that do not fit
 *            (GDKW_CONV_UTF8_TO_LATIN1).
 *     "text/uri-list" -> "text/uri-list".
 *     the pseudo names above, and the empty string -> ignored.
 *     everything else -> the atom with the mime's name, RAW.
 * "text/plain" and "text/plain;charset=utf-8" are deliberately NOT advertised
 * inbound as targets: ytk reads those types as ISO-8859-1 / with CRLF rules.
 * Order is the offer order with the text group at the position of its first
 * mime.
 *
 * DECISIONS: PRIMARY (piece datadevice, with selection)
 *
 * PRIMARY is real when the compositor has zwp_primary_selection_device_manager_v1
 * (XML verified present in wayland-protocols 1.49, vendored as generated C, I7).
 * It is the same state machine as CLIPBOARD behind GDKWSelKind: device events
 * data_offer / selection, offer events offer, offer request receive, source
 * request offer and events send / cancelled, device request set_selection.  Only
 * wire names differ, and only the datadevice piece knows them.  Without the
 * global, PRIMARY is a LOCAL selection (loopback only, nothing in the
 * compositor); middle click paste inside the application still works.
 *
 * DECISIONS: PIPES, TIMEOUTS, LIMITS (piece datadevice)
 *
 * All fds are O_CLOEXEC; the application side of every pipe is O_NONBLOCK.
 * _gdk_wayland_dd_offer_receive (): pipe (), wl_data_offer_receive (offer,
 * mime, write end), close the write end at once (libwayland dups it),
 * wl_display_flush, then a GSource on the read end (default main context,
 * G_PRIORITY_DEFAULT) reads chunks of GDKW_PIPE_CHUNK until EOF.  Idle timeout
 * GDKW_RECEIVE_TIMEOUT_MS since the last byte, hard cap
 * GDKW_SELECTION_MAX_BYTES (status TOO_BIG).  The completion callback is always
 * called from the main loop.  The writer is the same in reverse (G_IO_OUT,
 * partial writes, idle timeout GDKW_SEND_TIMEOUT_MS).  All timeouts are
 * overridable by _gdk_wayland_test_dd_set_timeouts ().
 * Virtual mode: _gdk_wayland_dd_offer_receive () still makes the real pipe, and
 * instead of the protocol request it hands the write end to the pretend sender
 * of that offer and mime (_gdk_wayland_test_dd_offer_data), which writes with
 * the SAME async writer used for real sends, one 4096 byte chunk per main loop
 * iteration.  So a 100 KB payload crosses the 64 KiB pipe buffer for real and
 * the reader's partial read and EOF handling run for real.
 *
 * DECISIONS: DRAG CONTEXT TYPE (piece dnd)
 *
 * The risk: GdkDragContext is a single GType, defined by G_DEFINE_TYPE in
 * x11/gdkdnd-x11.c.  Its init stores an X11 private block in
 * context->windowing_data and its finalize dereferences that block
 * (window_caches) and, for protocol XDND on a non source context, calls
 * xdnd_manage_source_filter () on the source window, which is Xlib.
 * Decision: Wayland gets its own SUBCLASS, GDKWDragContext (GDKW_TYPE_DRAG_
 * CONTEXT, parent GDK_TYPE_DRAG_CONTEXT), with its state as instance fields.
 *   - windowing_data is NOT touched: it stays the zeroed X11 private block the
 *     parent init made, so the parent finalize is safe.
 *   - context->protocol is always GDK_DRAG_PROTO_LOCAL.  NEVER XDND (finalize
 *     would run Xlib), never MOTIF, never ROOTWIN.  ytk only special cases
 *     MOTIF and ROOTWIN, so LOCAL is neutral.
 *   - The subclass finalize releases its own state, then chains up; the parent
 *     frees context->targets (a plain GList of GdkAtom owned by the context)
 *     and unrefs source_window and dest_window (the piece holds a ref on each).
 *   - Contexts are created with g_object_new (GDKW_TYPE_DRAG_CONTEXT, NULL),
 *     never gdk_drag_context_new ().  Every _gdk_wayland_drag_* entry point
 *     checks GDKW_IS_DRAG_CONTEXT and ignores a foreign context silently.
 *   - Layering: the base type still comes from the X11 object.  When the
 *     Wayland only build (M11) drops X11, GDK_TYPE_DRAG_CONTEXT moves to the
 *     core gdkdnd.c; the dnd piece needs no change.
 *
 * DECISIONS: DRAG SOURCE (piece dnd, source half)
 *
 * ytk drives a source with these calls: gdk_pointer_grab (ipc window, motion and
 * release masks), gdk_drag_begin (ipc window, targets), then from an idle
 * gdk_drag_find_window_for_screen (ctx, icon window, ...) and gdk_drag_motion
 * (ctx, dest, protocol, x, y, suggested, possible, time) which report the
 * possible actions; gdk_drag_get_selection () and, if it is not GDK_NONE,
 * gtk_selection_owner_set_for_display (ipc, XdndSelection); and it ends the drag
 * on GDK_DROP_FINISHED (always as success; the event handler ignores the
 * context result).  On X11 a pointer grab keeps feeding motion and button
 * release to ytk.  On Wayland the COMPOSITOR owns the pointer after start_drag,
 * so ytk never sees the release.  Therefore:
 *   gdk_drag_begin (window, targets):
 *     - ALWAYS returns a non NULL context (ytk dereferences it at once).
 *       is_source TRUE, protocol LOCAL, source_window = ref (@window) (the ipc
 *       window, NOT the toplevel: DRAG_STATUS and DROP_FINISHED events go to
 *       it, and gtk_drag_get_source_widget compares it with the ipc window),
 *       targets = copy of the list, actions 0, start_time = GDKWSeat.last_time.
 *     - Replaces a previous unfinished source context (aborted silently).
 *     - Creates the drag wl_data_source (kind DND) with the mimes of the source
 *       map.  Does NOT start the drag yet.
 *     - It cannot start when: no data device (neither real nor virtual), or
 *       GDKWSeat.button_mask == 0, or no pointer focus impl (pointer_focus,
 *       else last_pointer_impl) with a wl_surface, or GDKWSeat.button_serial ==
 *       0.  Then the context is FAILED and a GDK_DROP_FINISHED event for it is
 *       queued (to source_window).  ytk then ends its drag through its normal
 *       path.  (This replaces the M4 stub that returned NULL.)
 *   The start is DEFERRED to the first gdk_drag_motion () or
 *     gdk_drag_find_window_for_screen () call, because wl_data_source.set_actions
 *     must precede start_drag and only motion carries the possible actions.  If
 *     neither call comes within GDKW_DRAG_START_FALLBACK_MS (ytk calls them only
 *     when it has an event), start with actions COPY|MOVE.  Start = recheck
 *     button_mask != 0 (else FAILED as above), set_actions (mapped possible
 *     actions), then _gdk_wayland_dd_start_drag (dd, source, origin, serial)
 *     with origin = the toplevel wrapper of the pointer focus impl and serial =
 *     GDKWSeat.button_serial.  The icon is NOT sent in M8 (see NOT DONE).
 *   gdk_drag_find_window_for_screen: remembers @drag_window in
 *     context->icon_window (weak, unused in M8); *dest_window = NULL,
 *     *protocol = GDK_DRAG_PROTO_NONE.  gdk_drag_get_protocol_for_display:
 *     unchanged (0, NONE).
 *   gdk_drag_motion: stores suggested_action and possible_actions in the context,
 *     starts the drag if not started, returns FALSE.
 *   gdk_drag_drop: records the request, nothing else (the compositor decides).
 *   gdk_drag_abort: not started or FAILED: finish locally.  Started: destroy the
 *     source (the compositor cancels the drag).  No event is queued (ytk ends the
 *     drag itself after abort).  The context ends DONE with no protocol object.
 *   gdk_drag_get_selection: the atom "XdndSelection" for every Wayland context.
 *   gdk_drag_drop_succeeded: TRUE only after dnd_finished.
 *   Source events (handlers installed on the drag source):
 *     target (mime)       first call, or mime NULL vs non NULL changed, and the
 *                         compositor sends no action events (manager < 3):
 *                         context->action = mime ? COPY : 0, queue GDK_DRAG_STATUS.
 *     action (wl_action)  context->action = from_wl (wl_action); queue
 *                         GDK_DRAG_STATUS when it changed (and on the first one).
 *     dnd_drop_performed  record only.
 *     dnd_finished        succeeded = TRUE.  If the final action is MOVE and no
 *                         enter ever reached one of OUR windows during this drag
 *                         (context->local_dest FALSE): call _gdk_wayland_
 *                         selection_discard_request (display, XdndSelection,
 *                         "DELETE") so ytk emits drag-data-delete (the same
 *                         request an X11 destination sends).  Then queue
 *                         GDK_DROP_FINISHED.  Then the context is DONE and the
 *                         source freed.  (With a local destination ytk deletes
 *                         through its own in process shortcut: no second delete.)
 *     cancelled           succeeded = FALSE, action = 0, queue GDK_DROP_FINISHED,
 *                         DONE, source freed.
 *     send (mime, fd)     _gdk_wayland_selection_serve (source, XdndSelection,
 *                         context->source_map, mime, fd).
 *   Events are queued with gdk_event_put (): {type, window = source_window,
 *   send_event FALSE, context (the event takes a ref), time = last seat time}.
 *   Compositors normally send wl_pointer.leave for the origin surface after
 *   start_drag; the M6 pointer code already clears button state on leave.  A
 *   drag leaves no state:
 *   after finished, cancelled, aborted or failed the context holds no
 *   protocol object and dd->dnd.source is NULL.
 *
 * DECISIONS: DRAG DESTINATION (piece dnd, destination half)
 *
 * Only a toplevel that was passed to gdk_window_register_dnd () (or whose child
 * was: the flag is set on the toplevel too) receives events.  ytk registers
 * the toplevel of every drag dest widget.  data_device.enter on any other
 * surface: accept (serial, NULL), set_actions (0, 0) if v3, remember the offer,
 * no context, no events.
 * data_device.enter (serial, surface, x, y, offer) on a registered toplevel W:
 *   - drop leftovers of a previous destination (destroy its offer, unref its
 *     context; no LEAVE if it already got LEAVE or DROP_START);
 *   - context: is_source FALSE, protocol LOCAL, dest_window = ref (W),
 *     source_window = ref of the active source context's source_window if one
 *     of OUR drags is active (so gtk_drag_get_source_widget finds the source
 *     widget: tree view reorder needs it), ELSE ref of the screen root window.
 *     Never NULL: gtk_drag_finish () calls gdk_window_get_screen (source_window)
 *     on a destination context.  If a local source is active also set its
 *     local_dest TRUE.  targets = _gdk_wayland_selection_targets_for_offer ().
 *     actions = from_wl (offer.source_actions) if the event was seen, else
 *     COPY|MOVE.  suggested_action = from_wl (offer.action) if seen, else COPY.
 *     start_time = last seat time.
 *   - queue GDK_DRAG_ENTER then GDK_DRAG_MOTION for W with the position.
 * data_device.motion (time, x, y): update the position, queue GDK_DRAG_MOTION.
 * data_device.leave: queue GDK_DRAG_LEAVE, destroy the offer, release the
 *   context.  A leave that arrives AFTER a drop is ignored completely (no
 *   event, the offer stays): compositors differ on whether they send one, and
 *   the offer is still needed for receive and finish.
 * data_device.drop: queue GDK_DROP_START (position of the last motion).  The
 *   offer and context stay until finish, a failed reply or the next enter.
 * Event coordinates: x_root = (gshort) x + wrapper->x and y_root = (gshort) y +
 *   wrapper->y, where wrapper->x / y is what gdk_window_get_position (W)
 *   returns.  ytk computes the widget position as x_root - position, so this is
 *   what makes it right.  NOTE this is NOT the pointer event convention of M6
 *   (x_root == x, root origin 0): the two agree only for a toplevel stored at
 *   0, 0.  The test reads gdk_window_get_position () instead of assuming 0.
 * gdk_drag_status (ctx, action, time) -> wl_data_offer.accept (enter serial,
 *   mime) and set_actions (v3), deduplicated against the last pair sent:
 *     action 0: accept (serial, NULL), set_actions (0, 0).
 *     else:     accept (serial, FIRST mime of the offer), set_actions (mask,
 *               preferred) with mask = to_wl (action), preferred = same single
 *               action (COPY, MOVE or ASK).
 *   context->action = action (ytk reads it with gdk_drag_context_get_selected_
 *   action when it decides about MOVE).  Offer version < 3: accept only.
 * offer.source_actions / offer.action events update context->actions /
 *   suggested_action; after the drop an action event also sets context->action
 *   (the final one).
 * gdk_drop_reply (ctx, ok, time): ok TRUE: nothing more.  FALSE: accept (serial,
 *   NULL), set_actions (0, 0), destroy the offer (the source sees cancelled),
 *   release the context.
 * gdk_drop_finish (ctx, success, time): success and (action != 0): finish ()
 *   (v3), then destroy the offer.  Not success: destroy the offer WITHOUT
 *   finish (the source sees cancelled).  Then release the context.  ytk calls
 *   this itself after the data arrived (gtk_drag_finish).
 * Data of a destination: ytk calls gtk_selection_convert (ipc, gdk_drag_get_
 *   selection (ctx) = XdndSelection, target, time).  With a local owner that is
 *   the in process shortcut; otherwise gdk_selection_convert () case b with
 *   dd->dnd.dest_offer.  The offer must stay alive until finish.
 * gdk_window_register_dnd: sets GDKW_DND_REGISTERED_KEY on the toplevel of
 *   @window.  Harmless on a destroyed window.
 *
 * DECISIONS: ACTIONS
 *
 * to_wl:   COPY, LINK, DEFAULT, PRIVATE -> wl COPY (1);  MOVE -> wl MOVE (2);
 *          ASK -> wl ASK (4).  LINK has no wl equivalent: it degrades to copy.
 * from_wl: 1 -> GDK_ACTION_COPY, 2 -> GDK_ACTION_MOVE, 4 -> GDK_ACTION_ASK;
 *          0 -> 0.  A mask maps bit by bit.
 * The helpers are static inline in the internal section.
 *
 * DECISIONS: DEMO (integrator, I9)
 *
 * Three M4 checks assert the old stubs and must change:
 *   m4_selection: it owns CLIPBOARD with main_window, then expects a refused
 *     convert.  Now an owner means a SELECTION_REQUEST nobody answers.  Release
 *     the ownership (owner_set NULL) before the convert; the refused NOTIFY is
 *     then the "no owner, no offer" case (c).
 *   m4_drag: "gdk_drag_begin returns NULL" becomes "returns a context, and a
 *     GDK_DROP_FINISHED for it arrives" (no data device on headless Weston).
 *   m4_property: "properties are safe no-ops" becomes a real round trip
 *     (change, get, delete).
 * Headless Weston has no seat, so no wl_data_device exists there: every M8
 * check runs on the VIRTUAL data device (_gdk_wayland_test_dd_init ()).  If a
 * real device exists the hook returns FALSE and m8_exercise prints SKIP for the
 * protocol parts and still runs the loopback and property parts.
 *
 * DECISIONS: YTK PATCHES (M11, out of scope for M8, listed so nobody is
 * surprised; M8 contracts assume them)
 *
 *   gtkselection.c  GTK_SELECTION_MAX_SIZE (display) calls XExtendedMaxRequestSize
 *     (GDK_DISPLAY_XDISPLAY (display)) when GDK_WINDOWING_X11 is defined, which
 *     is always in the dual backend build: a crash on any display that is not
 *     X11 the first time something asks us for a selection.  Replace the macro
 *     by a function returning G_MAXINT unless GDK_IS_DISPLAY_X11 (display).
 *   gtkdnd.c  grab_dnd_keys (), ungrab_dnd_keys () and root_key_filter () use
 *     XGrabKey on GDK_WINDOW_XDISPLAY / GDK_WINDOW_XID: same crash when a drag
 *     starts.  Make them return at once unless GDK_IS_DISPLAY_X11 (display).
 *   gtkselection.c  line 2287 uses gdk_x11_window_foreign_new_for_display ():
 *     works through I6, no change needed.
 *
 * DECISIONS: NOT DONE (known gaps, to put in PLANNING.md)
 *
 *   - Drag icon.  The icon window ytk builds is a TEMP window that already has
 *     (or would get) an xdg role; a wl_surface used as drag icon must have none.
 *     start_drag is sent with icon NULL.  Follow up: snapshot @drag_window (the
 *     argument of the first gdk_drag_find_window_for_screen) into an own ARGB
 *     shm surface.
 *   - gdk_display_supports_selection_notification stays FALSE and no
 *     GDK_OWNER_CHANGE event is generated.  ytk then does not cache TARGETS
 *     and asks every time, which is correct; a wrong cache would not be.
 *   - Clipboard persistence (gdk_display_supports_clipboard_persistence,
 *     store_clipboard) stays FALSE / no-op: there is no clipboard manager
 *     protocol in use, so the clipboard content is lost when the process exits.
 *   - INCR is neither sent nor accepted.  MULTIPLE and TIMESTAMP are refused.
 *   - Programmatic selections of other clients are not discoverable:
 *     gdk_selection_owner_get returns NULL for them.
 *   - Only one seat (the first), one data device, one drag at a time.
 *   - LINK drag action degrades to copy.
 *   - Verified only against the virtual device and the hooks; no real
 *     compositor has seen wl_data_device.set_selection / start_drag from this
 *     code.
 *
 * ---------------------------------------------------------------------------
 * HOOK OWNERS (every hook returns FALSE, never warns, never g_critical, when its
 * precondition fails: not a Wayland display, no virtual device, unknown id)
 *
 *   datadevice  _gdk_wayland_test_dd_init, _dd_fini, _dd_info, _dd_reset_requests,
 *               _dd_set_timeouts, _dd_seat_serial, _dd_offer_new, _dd_offer_data,
 *               _dd_selection, _dd_dnd_enter, _dd_dnd_motion, _dd_dnd_leave,
 *               _dd_dnd_drop, _dd_offer_source_actions, _dd_offer_action,
 *               _dd_source_send, _dd_source_event
 *   selection   _gdk_wayland_selection_mimes_for_targets,
 *               _gdk_wayland_selection_targets_for_mimes,
 *               _gdk_wayland_test_selection_info, _gdk_wayland_test_native_id
 *   property    _gdk_wayland_test_property_count, _gdk_wayland_test_property_bytes
 *   dnd         _gdk_wayland_test_dnd_dest_info, _gdk_wayland_test_dnd_source_info,
 *               _gdk_wayland_test_drag_context_info
 * ---------------------------------------------------------------------------
 */

#ifndef __YDK_GDKM8_WAYLAND_H__
#define __YDK_GDKM8_WAYLAND_H__

#include <glib.h>
#include <glib-object.h>

#include <ydk/gdk.h>

G_BEGIN_DECLS

/* =========================================================================
 * 2. PUBLIC: enums, info structs and headless test hooks
 *
 * The injection hooks push ONE synthetic protocol event through the REAL
 * static listener function (wl_data_device, wl_data_offer, wl_data_source,
 * and the zwp_primary_selection_* equivalents), with fake proxies, so the
 * whole translation layer runs.  Every protocol REQUEST the code would send is
 * recorded in GDKWTestDDInfo instead (the "virtual device").  Event delivery to
 * the application still happens from the main loop: the demo runs
 * g_main_context_iteration (NULL, FALSE) until it returns FALSE, with its own
 * gdk_event_handler_set () recorder in place, as in M6.
 * ========================================================================= */

#define GDKW_TEST_MAX_MIMES 16
#define GDKW_TEST_MIME_LEN  96

/* Which selection a hook or a record is about.  CLIPBOARD and PRIMARY are the
 * two protocol selections, DND is the drag and drop pseudo selection
 * "XdndSelection". */
typedef enum
{
  GDKW_SEL_NONE = -1,
  GDKW_SEL_CLIPBOARD = 0,
  GDKW_SEL_PRIMARY = 1,
  GDKW_SEL_DND = 2,
  GDKW_SEL_N_KINDS = 3
} GDKWSelKind;

/* wl_data_device_manager.dnd_action values, mirrored so the demo needs no
 * wayland headers */
enum
{
  GDKW_TEST_WL_ACTION_NONE = 0,
  GDKW_TEST_WL_ACTION_COPY = 1,
  GDKW_TEST_WL_ACTION_MOVE = 2,
  GDKW_TEST_WL_ACTION_ASK = 4
};

/* What the pretend sender does when wl_data_offer.receive (mime, fd) would be
 * sent (see _gdk_wayland_test_dd_offer_data) */
typedef enum
{
  GDKW_TEST_FEED_NORMAL = 0,   /* write the bytes in 4096 byte chunks, one chunk
                                * per main loop iteration, then close the fd */
  GDKW_TEST_FEED_STALL,        /* keep the fd open, write nothing (timeouts) */
  GDKW_TEST_FEED_ERROR         /* close the fd at once, no bytes (empty read) */
} GDKWTestFeed;

/* wl_data_source events a hook can raise */
typedef enum
{
  GDKW_TEST_SRC_TARGET = 0,    /* mime may be NULL */
  GDKW_TEST_SRC_ACTION,        /* wl_action */
  GDKW_TEST_SRC_DROP_PERFORMED,
  GDKW_TEST_SRC_FINISHED,
  GDKW_TEST_SRC_CANCELLED
} GDKWTestSourceEvent;

/* Everything the datadevice piece recorded.  Counters count since the last
 * _gdk_wayland_test_dd_reset_requests () (the "alive" and "pending" fields are
 * live gauges, never reset).  Strings are copies, NUL terminated, cut at
 * GDKW_TEST_MIME_LEN. */
typedef struct
{
  gboolean virtual_device;      /* the virtual device is active */
  gboolean have_device;         /* a real or virtual wl_data_device exists */
  gboolean have_primary;        /* a real or virtual primary selection device */
  guint    manager_version;     /* bound wl_data_device_manager version, 3 virtual */
  guint32  seat_serial;         /* GDKWSeat.last_serial now */

  /* set_selection requests, index GDKW_SEL_CLIPBOARD / GDKW_SEL_PRIMARY */
  struct
  {
    guint    count;             /* requests, set_selection (NULL) included */
    guint32  serial;            /* serial of the last one */
    gboolean cleared;           /* the last one carried a NULL source */
    guint    n_mimes;           /* mimes offered by the last non NULL source */
    gchar    mimes[GDKW_TEST_MAX_MIMES][GDKW_TEST_MIME_LEN];
  } set_selection[2];

  /* wl_data_offer.receive / zwp offer receive */
  guint        receive_count;
  GDKWSelKind  receive_kind;
  gchar        receive_mime[GDKW_TEST_MIME_LEN];

  /* destination requests */
  guint    accept_count;
  guint32  accept_serial;
  gboolean accept_null;         /* the last accept carried a NULL mime */
  gchar    accept_mime[GDKW_TEST_MIME_LEN];
  guint    set_actions_count;   /* wl_data_offer.set_actions */
  guint32  offer_actions_mask;
  guint32  offer_actions_preferred;
  guint    finish_count;        /* wl_data_offer.finish */
  guint    offer_destroy_count; /* offer proxies destroyed */
  guint32  last_enter_serial;

  /* source requests */
  guint     start_drag_count;
  guint32   start_drag_serial;
  GdkWindow *start_drag_origin; /* borrowed toplevel wrapper */
  gboolean  start_drag_icon;    /* an icon surface was passed (always FALSE in M8) */
  guint     start_drag_n_mimes;
  gchar     start_drag_mimes[GDKW_TEST_MAX_MIMES][GDKW_TEST_MIME_LEN];
  gboolean  source_actions_set; /* wl_data_source.set_actions was sent */
  guint32   source_actions_mask;
  guint     source_destroy_count;

  /* gauges, for leak checks */
  guint    offers_alive;        /* GDKWOffer records not freed */
  guint    sources_alive;       /* GDKWSource records not freed */
  guint    pending_reads;       /* receive pipes in flight */
  guint    pending_writes;      /* send pipes in flight */
} GDKWTestDDInfo;

/* State of one selection in the selection piece. */
typedef struct
{
  gboolean   has_owner;         /* a LOCAL owner record exists */
  GdkWindow *owner;             /* borrowed, NULL when none */
  guint32    owner_time;
  guint      generation;        /* bumped by every owner_set that records an owner */
  gboolean   source_alive;      /* a wl source exists for this owner */
  gboolean   probing;           /* a TARGETS probe is outstanding */
  guint      probes_started;
  guint      probes_completed;  /* answered and acted on (not stale) */
  guint      local_pending;     /* loopback requests waiting for the owner */
  guint      send_pending;      /* outbound transfers waiting for the owner */
  gboolean   external_offer;    /* an external offer is current for this kind */
  guint      external_offer_id;
  guint      clear_events;      /* SELECTION_CLEAR events queued since the start */
} GDKWTestSelectionInfo;

/* Destination side of DnD as the dnd piece sees it. */
typedef struct
{
  gboolean        active;       /* a destination context exists */
  gboolean        ignored;      /* last enter was on an unregistered surface */
  gboolean        dropped;      /* DROP_START queued, offer kept */
  GdkDragContext *context;      /* borrowed, NULL when none */
  gint            x_root, y_root;   /* last position sent in an event */
  GdkWindow      *dest_window;  /* borrowed */
  guint           events_queued;/* DND events queued for destination contexts */
} GDKWTestDndDestInfo;

/* Source side of DnD as the dnd piece sees it. */
typedef struct
{
  gboolean        active;       /* a source context exists and is not DONE */
  GdkDragContext *context;      /* borrowed, NULL when none */
  gboolean        started;      /* start_drag was sent */
  gboolean        failed;       /* could not start, DROP_FINISHED queued */
  gboolean        finished;     /* dnd_finished seen */
  gboolean        cancelled;    /* cancelled seen or aborted */
  gboolean        succeeded;    /* what gdk_drag_drop_succeeded () returns */
  gboolean        local_dest;   /* an enter reached one of our windows */
  gboolean        delete_requested; /* the DELETE request was synthesized */
  guint           events_queued;
} GDKWTestDndSourceInfo;

/* Facts about one drag context, for a context of any backend. */
typedef struct
{
  gboolean        is_wayland;   /* G_TYPE_CHECK_INSTANCE_TYPE GDKW_TYPE_DRAG_CONTEXT */
  GdkDragProtocol protocol;
  gboolean        is_source;
  guint           n_targets;
} GDKWTestDragContextInfo;

/* ---- pure helpers (selection piece; need only the atom table) ------------
 *
 * _gdk_wayland_selection_mimes_for_targets (): the mimes the MIME MAP rules
 * offer for the owner targets @targets[0..@n_targets), a NULL terminated
 * vector in offer order, free with g_strfreev (); never NULL (empty vector).
 *
 * _gdk_wayland_selection_targets_for_mimes (): the GDK targets advertised for
 * an offer carrying @mimes (NULL terminated), a g_malloc'ed array of
 * *@n_targets atoms in advertised order, free with g_free (); NULL and
 * *@n_targets 0 when there are none. */
gchar    **_gdk_wayland_selection_mimes_for_targets (const GdkAtom      *targets,
                                                     gint                n_targets);
GdkAtom   *_gdk_wayland_selection_targets_for_mimes (const gchar * const *mimes,
                                                     gint               *n_targets);

/* ---- datadevice hooks ---------------------------------------------------- */

/* Owner: datadevice.  Creates the VIRTUAL data device: dd->virtual = TRUE, no
 * proxies, both managers "present" at version 3, requests recorded instead of
 * sent, receive satisfied by the pretend sender.  FALSE if @display is not
 * Wayland or a real wl_data_device already exists.  Idempotent. */
gboolean _gdk_wayland_test_dd_init           (GdkDisplay *display);

/* Owner: datadevice.  Destroys the virtual device and everything attached:
 * offers, sources, pipe operations (fds closed, callbacks NOT called), then
 * clears the record.  After it the display behaves as with no data device. */
gboolean _gdk_wayland_test_dd_fini           (GdkDisplay *display);

/* Owner: datadevice.  Copies the record.  Works with or without the virtual
 * device (zeros for what does not exist). */
gboolean _gdk_wayland_test_dd_info           (GdkDisplay *display,
                                              GDKWTestDDInfo *info);

/* Owner: datadevice.  Zeroes the request counters, keeps objects and gauges. */
gboolean _gdk_wayland_test_dd_reset_requests (GdkDisplay *display);

/* Owner: datadevice (with selection reading the values).  Sets the timeouts in
 * milliseconds; 0 keeps the current one.  Defaults: GDKW_RECEIVE_TIMEOUT_MS,
 * GDKW_SEND_TIMEOUT_MS, GDKW_PROBE_TIMEOUT_MS, GDKW_LOCAL_REQUEST_TIMEOUT_MS. */
gboolean _gdk_wayland_test_dd_set_timeouts   (GdkDisplay *display,
                                              guint       receive_ms,
                                              guint       send_ms,
                                              guint       probe_ms,
                                              guint       local_ms);

/* Owner: datadevice.  GDKWSeat.last_serial now (0 for a non Wayland display). */
guint32  _gdk_wayland_test_dd_seat_serial    (GdkDisplay *display);

/* Owner: datadevice.  Creates an offer as the compositor would: calls the REAL
 * data_device.data_offer listener (CLIPBOARD and DND) or primary_device.
 * data_offer listener (PRIMARY) with a fake offer proxy, then the real
 * offer.offer listener once per mime in @mimes (NULL terminated).  @version is
 * the offer version, 1 to 3 (3 makes source_actions / action events legal).
 * Returns the offer id (> 0, unique per display), 0 on failure.  The offer is
 * "pending" until _dd_selection () or _dd_dnd_enter () introduces it. */
guint    _gdk_wayland_test_dd_offer_new      (GdkDisplay         *display,
                                              GDKWSelKind         kind,
                                              guint               version,
                                              const gchar * const *mimes);

/* Owner: datadevice.  Programs the pretend sender of @offer_id for @mime: when
 * the code later issues receive (@mime, fd) for that offer, the sender writes
 * @data (copied, @length bytes) into the fd according to @feed.  May be set per
 * mime and replaced.  A receive for a mime with no program behaves as
 * GDKW_TEST_FEED_ERROR. */
gboolean _gdk_wayland_test_dd_offer_data     (GdkDisplay    *display,
                                              guint          offer_id,
                                              const gchar   *mime,
                                              const guint8  *data,
                                              gsize          length,
                                              GDKWTestFeed   feed);

/* Owner: datadevice.  Calls the REAL data_device.selection listener (CLIPBOARD)
 * or primary_device.selection listener (PRIMARY) with offer @offer_id, or with
 * NULL for @offer_id 0.  @kind DND is not valid here (FALSE). */
gboolean _gdk_wayland_test_dd_selection      (GdkDisplay *display,
                                              GDKWSelKind kind,
                                              guint       offer_id);

/* Owner: datadevice.  Raise offer events through the REAL wl_data_offer
 * listener: source_actions (a WL_ACTION mask) and action (one value). */
gboolean _gdk_wayland_test_dd_offer_source_actions (GdkDisplay *display,
                                                    guint       offer_id,
                                                    guint32     wl_actions);
gboolean _gdk_wayland_test_dd_offer_action   (GdkDisplay *display,
                                              guint       offer_id,
                                              guint32     wl_action);

/* Owner: datadevice.  Real wl_data_device.enter / motion / leave / drop
 * listeners.  enter uses the wl_surface of @window's impl (FALSE when it has
 * none), a private incrementing serial (returned in *@serial when not NULL,
 * also GDKWTestDDInfo.last_enter_serial) and the surface coordinates @x, @y.
 * motion uses @time (ms) and @x, @y. */
gboolean _gdk_wayland_test_dd_dnd_enter      (GdkDisplay *display,
                                              GdkWindow  *window,
                                              guint       offer_id,
                                              gdouble     x,
                                              gdouble     y,
                                              guint32    *serial);
gboolean _gdk_wayland_test_dd_dnd_motion     (GdkDisplay *display,
                                              guint32     time,
                                              gdouble     x,
                                              gdouble     y);
gboolean _gdk_wayland_test_dd_dnd_leave      (GdkDisplay *display);
gboolean _gdk_wayland_test_dd_dnd_drop       (GdkDisplay *display);

/* Owner: datadevice.  What the compositor would receive from our current
 * source of @kind when it asks for @mime.  Makes a pipe, calls the REAL source
 * send listener with the write end, then pumps the default main context
 * (g_main_context_iteration) while reading the read end, until EOF or 3 s.  The
 * pumping lets the application answer the SELECTION_REQUEST the call queues:
 * the demo's event handler must be installed and must answer it (the hook does
 * not).  Returns TRUE on EOF with the bytes in *@data (g_malloc'ed, one extra
 * NUL not counted in *@length; free with g_free) and *@length.  A REFUSAL (mime
 * not offered, no owner, owner answered GDK_NONE) closes the fd without a byte
 * and therefore returns TRUE with *@length 0: the hook cannot tell a refusal
 * from an empty selection, and neither can a real compositor.  FALSE (and
 * *@data NULL, *@length 0) only on timeout or when there is no live source of
 * @kind. */
gboolean _gdk_wayland_test_dd_source_send    (GdkDisplay *display,
                                              GDKWSelKind kind,
                                              const gchar *mime,
                                              guint8    **data,
                                              gsize      *length);

/* Owner: datadevice.  Raises a wl_data_source event through the REAL listener of
 * the current source of @kind (CLIPBOARD, PRIMARY: only CANCELLED is valid; DND:
 * all).  @mime is used by TARGET (NULL allowed), @wl_action by ACTION. */
gboolean _gdk_wayland_test_dd_source_event   (GdkDisplay         *display,
                                              GDKWSelKind         kind,
                                              GDKWTestSourceEvent event,
                                              const gchar        *mime,
                                              guint32             wl_action);

/* ---- selection hooks ----------------------------------------------------- */

/* Owner: selection.  FALSE if @display is not Wayland.  @selection may be any
 * atom; a never used one reports "no owner". */
gboolean _gdk_wayland_test_selection_info    (GdkDisplay            *display,
                                              GdkAtom                selection,
                                              GDKWTestSelectionInfo *info);

/* Owner: selection.  The native id (the value that appears in
 * GdkEventSelection.requestor for a loopback request) of @window, assigning one
 * if needed; 0 for a NULL or non Wayland window. */
GdkNativeWindow _gdk_wayland_test_native_id  (GdkWindow *window);

/* ---- property hooks ------------------------------------------------------ */

/* Owner: property.  Number of properties stored on @window; -1 if it is not a
 * Wayland window.  Used to prove that a completed transfer leaves none. */
gint     _gdk_wayland_test_property_count    (GdkWindow *window);

/* Owner: property.  Total bytes stored on @window; -1 if not a Wayland window. */
gint     _gdk_wayland_test_property_bytes    (GdkWindow *window);

/* ---- dnd hooks ----------------------------------------------------------- */

/* Owner: dnd.  Facts about @context of any backend; FALSE for NULL. */
gboolean _gdk_wayland_test_drag_context_info (GdkDragContext          *context,
                                              GDKWTestDragContextInfo *info);

/* Owner: dnd.  Number of GDKWDragContext instances not yet finalized (a gauge;
 * the leak check: 0 after every drag ended and every event was freed). */
guint    _gdk_wayland_test_dnd_contexts_alive (void);

/* Owner: dnd. */
gboolean _gdk_wayland_test_dnd_dest_info     (GdkDisplay            *display,
                                              GDKWTestDndDestInfo   *info);
gboolean _gdk_wayland_test_dnd_source_info   (GdkDisplay            *display,
                                              GDKWTestDndSourceInfo *info);

G_END_DECLS

/* =========================================================================
 * 3. INTERNAL
 * ========================================================================= */

#ifdef GDK_COMPILATION

#include <wayland-client.h>

#include "gdkprivate-wayland.h"
#include "gdkseat-wayland.h"

G_BEGIN_DECLS

/* The primary selection protocol types are used only through pointers here; the
 * datadevice piece includes the generated client header. */
struct zwp_primary_selection_device_manager_v1;
struct zwp_primary_selection_device_v1;
struct zwp_primary_selection_offer_v1;
struct zwp_primary_selection_source_v1;

/* ---- constants ------------------------------------------------------------ */

#define GDKW_PROPERTY_DATA_KEY       "ydk-wayland-properties"
#define GDKW_DATADEVICE_DATA_KEY     "ydk-wayland-datadevice"
#define GDKW_SELECTION_DATA_KEY      "ydk-wayland-selection"
#define GDKW_DND_REGISTERED_KEY      "ydk-wayland-dnd-registered"

#define GDKW_SELECTION_MAX_BYTES     (64 * 1024 * 1024)  /* one transfer, one window */
#define GDKW_PIPE_CHUNK              65536
#define GDKW_RECEIVE_TIMEOUT_MS      5000   /* idle, since the last byte */
#define GDKW_SEND_TIMEOUT_MS         10000  /* owner reply, then writer idle */
#define GDKW_PROBE_TIMEOUT_MS        2000
#define GDKW_LOCAL_REQUEST_TIMEOUT_MS 5000
#define GDKW_DRAG_START_FALLBACK_MS  100
#define GDKW_MAX_SEND_SLOTS          16
#define GDKW_NATIVE_ID_FIRST         0x00010000u

/* The wl_data_device_manager version bound (the highest the code uses). */
#define GDKW_DD_MANAGER_VERSION      3

/* ---- shared types --------------------------------------------------------- */

typedef struct _GDKWDataDevice GDKWDataDevice;
typedef struct _GDKWOffer      GDKWOffer;
typedef struct _GDKWSource     GDKWSource;
typedef struct _GDKWDragContext GDKWDragContext;
typedef struct _GDKWPipeOp     GDKWPipeOp;

typedef enum
{
  GDKW_CONV_RAW = 0,            /* bytes as the other side gave them */
  GDKW_CONV_LATIN1_TO_UTF8,     /* outbound: owner target STRING, wire is UTF-8 */
  GDKW_CONV_UTF8_TO_LATIN1,     /* inbound: wire is UTF-8, GDK target STRING */
  GDKW_CONV_CRLF_TO_LF          /* outbound: owner target text/plain*, GTK2 adds CRLF */
} GDKWConv;

/* One row of a source map: wl mime offered, the GDK target the owner is asked
 * for, the type tag expected in the owner's reply (informational), the
 * conversion applied to the owner's bytes.  The strings are owned by the array
 * (g_free in its clear function). */
typedef struct
{
  gchar    *mime;
  GdkAtom   target;
  GdkAtom   type;
  GDKWConv  conv;
} GDKWMimeMap;

typedef enum
{
  GDKW_READ_OK = 0,             /* data valid, may be empty */
  GDKW_READ_TIMEOUT,
  GDKW_READ_TOO_BIG,
  GDKW_READ_ERROR               /* read (2) failed, or the pipe could not be made.
                                 * An immediate EOF is NOT an error: it is an OK
                                 * read of zero bytes (this is also what the
                                 * pretend sender's ERROR feed produces) */
} GDKWReadStatus;

/* @data is g_malloc'ed with ONE extra NUL byte not counted in @length, and
 * ownership passes to the callee; it is NULL unless @status is OK.  Always
 * called from the main loop, never from inside the call that created the op,
 * and never after _gdk_wayland_dd_pipe_cancel (). */
typedef void (*GDKWReadDone)  (GDKWReadStatus status,
                               guchar        *data,
                               gsize          length,
                               gpointer       user_data);
typedef void (*GDKWWriteDone) (gboolean       ok,      /* all bytes written */
                               gpointer       user_data);

/* Handlers a source's creator provides.  A NULL member is "ignore".  They are
 * called from the real wl_data_source / zwp source listeners.  @send takes
 * ownership of @fd.  After @cancelled the source is dead: the creator frees it
 * with _gdk_wayland_dd_source_free (), which may be done from inside the
 * handler (the listener touches @src no more afterwards). */
typedef struct
{
  void (*target)             (GDKWSource *src, const gchar *mime);  /* NULL = none */
  void (*send)               (GDKWSource *src, const gchar *mime, int fd);
  void (*cancelled)          (GDKWSource *src);
  void (*dnd_drop_performed) (GDKWSource *src);
  void (*dnd_finished)       (GDKWSource *src);
  void (*action)             (GDKWSource *src, guint32 wl_action);
} GDKWSourceHandlers;

/* An offer from the compositor.  Created by the datadevice listeners, shared by
 * reference count.  @mimes holds g_malloc'ed strings in arrival order (NOT NULL
 * terminated). */
struct _GDKWOffer
{
  GDKWDataDevice *dd;
  guint           id;               /* unique per display, > 0; the hook handle */
  gint            ref_count;
  GDKWSelKind     kind;             /* GDKW_SEL_NONE until a selection or enter event
                                     * introduces it (data_offer alone does not say);
                                     * the virtual hook sets it provisionally */
  struct wl_data_offer *wl_offer;   /* NULL in virtual mode and for PRIMARY offers */
  struct zwp_primary_selection_offer_v1 *zwp_offer;  /* PRIMARY, else NULL */
  guint32         version;          /* 1..3; virtual: as given */
  GPtrArray      *mimes;
  guint32         source_actions;   /* wl mask, valid when got_source_actions */
  guint32         action;           /* last wl action, valid when got_action */
  gboolean        got_source_actions;
  gboolean        got_action;
  gboolean        introduced;       /* a selection or enter event named it */
  gboolean        dead;             /* proxy destroyed; receives still draining */
  gboolean        maybe_self;       /* arrived while we owned this selection locally */
  guint32         enter_serial;     /* DND: serial of the enter */
};

/* A wl_data_source or zwp source we created.  @map is the selection piece's
 * source map (NULL for none). */
struct _GDKWSource
{
  GDKWDataDevice *dd;
  guint           id;
  GDKWSelKind     kind;
  struct wl_data_source *wl_source;               /* CLIPBOARD, DND; NULL if virtual */
  struct zwp_primary_selection_source_v1 *zwp_source; /* PRIMARY */
  GPtrArray      *mimes;            /* offered, g_malloc'ed strings, NOT NULL terminated */
  const GDKWSourceHandlers *handlers;
  gpointer        user_data;        /* creator's record */
  guint32         actions_mask;     /* set_actions value, 0 if never set */
  gboolean        actions_set;
  gboolean        cancelled;        /* cancelled seen: no more handler calls */
  gboolean        freed;            /* proxy destroyed */
};

/* DnD state kept in the data device; the dnd piece manages every field. */
typedef struct
{
  GDKWDragContext *dest;            /* destination context, ref held; NULL if none */
  GDKWOffer       *dest_offer;      /* offer of the last enter, ref held; kept while
                                     * ignored and after a drop until released */
  guint32          dest_serial;
  gboolean         dest_ignored;
  gboolean         dest_dropped;
  GDKWDragContext *source;          /* active source context, ref held; NULL if none */
} GDKWDndState;

/* The data device of a display, qdata GDKW_DATADEVICE_DATA_KEY, created zeroed on
 * first use by _gdk_wayland_display_get_data_device () and freed with the
 * display object by _gdk_wayland_dd_free (). */
struct _GDKWDataDevice
{
  GDKWDisplayWayland *display;      /* back pointer, no ref */

  /* globals and proxies (datadevice piece) */
  struct wl_data_device_manager *manager;
  guint32  manager_version;
  struct wl_data_device *device;    /* NULL until manager and wl_seat both exist */
  struct zwp_primary_selection_device_manager_v1 *primary_manager;
  struct zwp_primary_selection_device_v1 *primary_device;
  gboolean virtual_device;          /* the test hook created a virtual device */

  /* objects (datadevice piece) */
  GList   *offers;                  /* every live GDKWOffer */
  guint    next_offer_id;
  GList   *sources;                 /* every live GDKWSource */
  guint    next_source_id;
  GDKWOffer  *selection_offer[2];   /* current external offers: CLIPBOARD, PRIMARY */
  GDKWSource *source[GDKW_SEL_N_KINDS];  /* newest source per kind */

  /* timeouts in ms (datadevice sets, selection reads) */
  guint    receive_timeout_ms;
  guint    send_timeout_ms;
  guint    probe_timeout_ms;
  guint    local_timeout_ms;

  /* drag and drop (dnd piece) */
  GDKWDndState dnd;

  /* test record (datadevice piece) */
  GDKWTestDDInfo rec;
  guint32  test_serial;             /* private serial counter of the hooks */
  GHashTable *feeds;                /* (offer id, mime) -> pretend sender program */

  /* added by the datadevice piece (additive, end of struct) */
  GList   *ops;                     /* every live GDKWPipeOp */
  gboolean finalized;               /* _gdk_wayland_dd_display_finalize () ran */
};

/* The Wayland drag context.  Parent first.  See DECISIONS: DRAG CONTEXT TYPE. */
typedef enum
{
  GDKW_DRAG_NEW = 0,       /* source: begun, start_drag not sent / destination: entered */
  GDKW_DRAG_STARTED,       /* source: start_drag sent */
  GDKW_DRAG_DROPPED,       /* source: dnd_drop_performed seen / destination: dropped */
  GDKW_DRAG_DONE           /* finished, cancelled, aborted, failed, left: inert */
} GDKWDragState;

struct _GDKWDragContext
{
  GdkDragContext parent_instance;   /* windowing_data stays the X11 block, untouched */

  GdkDisplay    *display;           /* no ref; the display outlives live contexts or
                                     * _gdk_wayland_dnd_display_finalize () clears it */
  GDKWDragState  state;

  /* destination side */
  GDKWOffer     *offer;             /* ref held */
  guint32        enter_serial;
  gint           x_root, y_root;    /* last position put in an event */
  guint32        last_time;
  gboolean       sent_valid;        /* accept / set_actions sent at least once */
  GdkDragAction  sent_action;       /* last action passed to them (dedupe) */
  gboolean       dropped;

  /* source side */
  GDKWSource    *source;            /* owned */
  GArray        *source_map;        /* GDKWMimeMap rows, owned */
  GdkWindow     *icon_window;       /* weak, unused in M8 */
  guint          start_timeout;     /* GSource id of the start fallback, 0 none */
  gboolean       started;
  gboolean       failed;
  gboolean       local_dest;
  gboolean       drop_performed;
  gboolean       succeeded;
  gboolean       got_action;        /* an action event was seen */
  gboolean       drop_requested;    /* gdk_drag_drop () was called */
  gboolean       delete_requested;
};

/* ---- GType ---------------------------------------------------------------- */

GType gdkw_drag_context_get_type (void) G_GNUC_CONST;

#define GDKW_TYPE_DRAG_CONTEXT         (gdkw_drag_context_get_type ())
#define GDKW_DRAG_CONTEXT(object)      (G_TYPE_CHECK_INSTANCE_CAST ((object), GDKW_TYPE_DRAG_CONTEXT, GDKWDragContext))
#define GDKW_IS_DRAG_CONTEXT(object)   (G_TYPE_CHECK_INSTANCE_TYPE ((object), GDKW_TYPE_DRAG_CONTEXT))

/* ---- accessors and helpers: static inline, so a piece compiles and links
 * without another piece's object file ---------------------------------------- */

/* Free function for the qdata slot (GDestroyNotify).  Defined by the datadevice
 * piece; it does what _gdk_wayland_dd_display_finalize () does and frees the
 * record.  Declared here so the inline accessor below can install it. */
void _gdk_wayland_dd_free (gpointer data);

/* The data device record of a Wayland @display, created zeroed (timeouts at the
 * defaults) on first use.  Never returns NULL for a Wayland display. */
static inline GDKWDataDevice *
_gdk_wayland_display_get_data_device (GdkDisplay *display)
{
  GDKWDataDevice *dd = g_object_get_data (G_OBJECT (display),
                                          GDKW_DATADEVICE_DATA_KEY);

  if (G_UNLIKELY (dd == NULL))
    {
      dd = g_new0 (GDKWDataDevice, 1);
      dd->display = (GDKWDisplayWayland *) display;
      dd->next_offer_id = 1;
      dd->next_source_id = 1;
      dd->receive_timeout_ms = GDKW_RECEIVE_TIMEOUT_MS;
      dd->send_timeout_ms = GDKW_SEND_TIMEOUT_MS;
      dd->probe_timeout_ms = GDKW_PROBE_TIMEOUT_MS;
      dd->local_timeout_ms = GDKW_LOCAL_REQUEST_TIMEOUT_MS;
      g_object_set_data_full (G_OBJECT (display), GDKW_DATADEVICE_DATA_KEY, dd,
                              _gdk_wayland_dd_free);
    }

  return dd;
}

/* TRUE when @kind can reach a compositor: CLIPBOARD and DND need the data device
 * (real or virtual), PRIMARY needs the primary selection device (real or
 * virtual).  FALSE for GDKW_SEL_NONE. */
static inline gboolean
_gdk_wayland_dd_usable (const GDKWDataDevice *dd, GDKWSelKind kind)
{
  if (dd == NULL || kind == GDKW_SEL_NONE)
    return FALSE;
  if (dd->virtual_device)
    return TRUE;
  if (kind == GDKW_SEL_PRIMARY)
    return dd->primary_device != NULL;
  return dd->device != NULL;
}

/* GDK drag action <-> wl_data_device_manager.dnd_action, see DECISIONS: ACTIONS */
static inline guint32
_gdk_wayland_dnd_actions_to_wl (GdkDragAction actions)
{
  guint32 mask = 0;

  if (actions & (GDK_ACTION_COPY | GDK_ACTION_LINK | GDK_ACTION_DEFAULT |
                 GDK_ACTION_PRIVATE))
    mask |= WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
  if (actions & GDK_ACTION_MOVE)
    mask |= WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;
  if (actions & GDK_ACTION_ASK)
    mask |= WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK;

  return mask;
}

static inline GdkDragAction
_gdk_wayland_dnd_actions_from_wl (guint32 mask)
{
  GdkDragAction actions = 0;

  if (mask & WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY)
    actions |= GDK_ACTION_COPY;
  if (mask & WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE)
    actions |= GDK_ACTION_MOVE;
  if (mask & WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK)
    actions |= GDK_ACTION_ASK;

  return actions;
}

/* The single wl action for the one GDK action a destination chose: MOVE and ASK
 * map to themselves, everything else to COPY, 0 to 0. */
static inline guint32
_gdk_wayland_dnd_preferred_to_wl (GdkDragAction action)
{
  if (action == 0)
    return 0;
  if (action & GDK_ACTION_MOVE)
    return WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;
  if (action & GDK_ACTION_ASK)
    return WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK;
  return WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
}

/* ---------------------------------------------------------------------------
 * Prototypes and owners
 * ------------------------------------------------------------------------ */

/* ---- owner "property": gdkproperty-wayland.c -----------------------------
 *
 * The three existing functions (_gdk_wayland_property_get / _change / _delete,
 * prototypes in gdkprivate-wayland.h) get the real bodies of DECISIONS:
 * PROPERTY TABLE.  The atom functions stay as they are.  New: */

/* Idempotent.  Frees the property table of @window.  Called from
 * _gdk_wayland_selection_window_destroyed () (the integrator never calls it). */
void  _gdk_wayland_property_window_destroyed (GdkWindow *window);

/* ---- owner "selection": gdkselection-wayland.c ---------------------------
 *
 * The existing public-facing functions get the real bodies of DECISIONS:
 * OWNERSHIP, CONVERT, SERVING:
 *   _gdk_wayland_selection_owner_set_for_display ()
 *   _gdk_wayland_selection_owner_get_for_display ()
 *   _gdk_wayland_selection_convert ()
 *   _gdk_wayland_selection_property_get ()
 *   _gdk_wayland_selection_send_notify_for_display ()
 * The text helpers (text_property_to_*, string_to_compound_text, free_*) are
 * unchanged.  New: */

/* The atoms "GDK_SELECTION" and "XdndSelection" (interned once). */
GdkAtom      _gdk_wayland_selection_property_atom (void);
GdkAtom      _gdk_wayland_selection_dnd_atom      (void);

/* CLIPBOARD -> GDKW_SEL_CLIPBOARD, PRIMARY -> GDKW_SEL_PRIMARY, XdndSelection ->
 * GDKW_SEL_DND, everything else GDKW_SEL_NONE (a local selection). */
GDKWSelKind  _gdk_wayland_selection_kind          (GdkAtom selection);

/* Integrator call site I4.  Idempotent. */
void         _gdk_wayland_selection_window_destroyed (GdkWindow *window);

/* Native ids (DECISIONS: NATIVE IDS).  _id_for_window assigns one lazily and
 * returns 0 for NULL or a window that is not a live Wayland window.  _lookup
 * returns the borrowed window an id resolves to, NULL if unknown. */
GdkNativeWindow _gdk_wayland_native_id_for_window (GdkWindow *window);
GdkWindow      *_gdk_wayland_native_id_lookup     (GdkDisplay *display,
                                                   GdkNativeWindow id);

/* Source map (DECISIONS: MIME MAP, outbound).  _new builds the rows for the
 * owner targets (g_array with GDKWMimeMap elements and a clear function that
 * frees mime), _mimes returns the NULL terminated vector of the mimes in row
 * order (g_strfreev), _free frees the array.  _new returns an empty array, not
 * NULL, when nothing is offerable. */
GArray      *_gdk_wayland_selection_source_map_new   (const GdkAtom *targets,
                                                      gint           n_targets);
gchar      **_gdk_wayland_selection_source_map_mimes (const GArray *map);
void         _gdk_wayland_selection_source_map_free  (GArray *map);

/* The advertised GDK targets of an offer (DECISIONS: MIME MAP, inbound), a new
 * GList of GdkAtom as pointers, free with g_list_free (); NULL when none. */
GList       *_gdk_wayland_selection_targets_for_offer (const GDKWOffer *offer);

/* Handlers for CLIPBOARD and PRIMARY sources.  The user_data of such a source is
 * a selection piece record. */
const GDKWSourceHandlers *_gdk_wayland_selection_source_handlers (void);

/* TRUE when a LOCAL owner record exists for @kind.  Called by the datadevice
 * listeners to set GDKWOffer.maybe_self. */
gboolean     _gdk_wayland_selection_has_local_owner (GdkDisplay *display,
                                                     GDKWSelKind kind);

/* Called by the datadevice piece when it drops the compositor side of a kind
 * for good (display finalize, test fini): forgets sources and aborts probes and
 * outbound transfers of that kind without events. */
void         _gdk_wayland_selection_display_finalize (GdkDisplay *display);

/* Serve one compositor request (DECISIONS: SERVING).  Used by the selection
 * handlers and by the dnd source handlers.  @map is the source map the source
 * was built from.  Takes ownership of @fd (always closed, immediately when the
 * request cannot be served).  The row
 * needed (target, conversion) is copied during the call: neither @src nor @map is
 * used afterwards, so a transfer in flight survives the source being freed and
 * the map being freed (a drag that ends while a send is still writing). */
void         _gdk_wayland_selection_serve (GDKWSource *src,
                                           GdkAtom     selection,
                                           const GArray *map,
                                           const gchar *mime,
                                           int          fd);

/* Queues GDK_SELECTION_REQUEST {window = local owner of @selection, selection,
 * target, requestor = a DISCARD token, property = "YDK_WL_DISCARD"}; the owner's
 * answer is read and thrown away.  No-op without a local owner.  Used by the dnd
 * piece for target DELETE. */
void         _gdk_wayland_selection_discard_request (GdkDisplay *display,
                                                     GdkAtom     selection,
                                                     GdkAtom     target);

/* ---- owner "datadevice": gdkdatadevice-wayland.c --------------------------- */

/* Integrator call sites I1 to I3.  _seat_ready is idempotent and creates the
 * wl_data_device and the primary selection device for every manager that is
 * bound once both the manager and display->wl_seat exist; it is also called by
 * the bind functions themselves.  _display_finalize destroys every proxy, offer
 * and source, cancels pipe operations (callbacks not called), calls
 * _gdk_wayland_selection_display_finalize () and _gdk_wayland_dnd_display_
 * finalize (), and is idempotent. */
void  _gdk_wayland_dd_bind_manager         (GDKWDisplayWayland *display,
                                           struct wl_registry *registry,
                                           uint32_t            name,
                                           uint32_t            version);
void  _gdk_wayland_dd_bind_primary_manager (GDKWDisplayWayland *display,
                                           struct wl_registry *registry,
                                           uint32_t            name,
                                           uint32_t            version);
void  _gdk_wayland_dd_seat_ready           (GDKWDisplayWayland *display);
void  _gdk_wayland_dd_display_finalize     (GdkDisplay *display);

/* Offers.  _ref / _unref count GDKWOffer.ref_count; the last unref frees the
 * record.  The device holds one reference while the offer is in dd->offers. */
GDKWOffer *_gdk_wayland_dd_offer_ref   (GDKWOffer *offer);
void       _gdk_wayland_dd_offer_unref (GDKWOffer *offer);

/* Offer requests.  Every one is a no-op on a dead offer or an offer version that
 * lacks the request (finish and set_actions need v3).  In virtual mode they are
 * recorded in dd->rec instead of sent. */
void  _gdk_wayland_dd_offer_accept      (GDKWOffer *offer,
                                         guint32    serial,
                                         const gchar *mime /* NULL = reject */);
void  _gdk_wayland_dd_offer_set_actions (GDKWOffer *offer,
                                         guint32    wl_mask,
                                         guint32    wl_preferred);
void  _gdk_wayland_dd_offer_finish      (GDKWOffer *offer);
/* Destroys the proxy and marks the offer dead.  Receives already in flight keep
 * draining.  If the offer is dd->selection_offer[] or dd->dnd.dest_offer it is
 * cleared there too.  Idempotent. */
void  _gdk_wayland_dd_offer_destroy     (GDKWOffer *offer);

/* Asynchronous receive (DECISIONS: PIPES).  NULL when the request cannot be
 * issued (offer dead, no usable pipe); @done is then NOT called.  In virtual
 * mode the pretend sender programmed by _gdk_wayland_test_dd_offer_data ()
 * answers.  The returned op is owned by the datadevice piece and freed after
 * @done ran or by _gdk_wayland_dd_pipe_cancel (). */
GDKWPipeOp *_gdk_wayland_dd_offer_receive (GDKWOffer    *offer,
                                           const gchar  *mime,
                                           GDKWReadDone  done,
                                           gpointer      user_data);

/* Asynchronous write of @bytes to @fd (takes ownership of @fd, closes it when
 * done).  @done may be NULL.  Returns the op (never NULL; on an unusable fd @done
 * is called from the main loop with ok FALSE). */
GDKWPipeOp *_gdk_wayland_dd_write_async   (int            fd,
                                           GBytes        *bytes,
                                           guint          timeout_ms,
                                           GDKWWriteDone  done,
                                           gpointer       user_data);

/* Cancels an op that has not completed: closes its fds and frees it, @done is
 * not called.  Must not be called on an op whose @done already ran. */
void        _gdk_wayland_dd_pipe_cancel   (GDKWPipeOp *op);

/* Sources.  _new creates the source (virtual: record only), offers every mime in
 * @mimes (NULL terminated) and installs the one listener table of the datadevice
 * piece, which calls @handlers with @user_data.  NULL when the kind is not
 * usable.  _set_actions needs manager version 3 and must precede start_drag
 * (ignored otherwise).  _free destroys the proxy (if not yet) and the record,
 * removes it from dd->sources and dd->source[], and detaches the handlers.  Call
 * it exactly once per source and drop the pointer. */
GDKWSource *_gdk_wayland_dd_source_new         (GDKWDataDevice *dd,
                                                GDKWSelKind     kind,
                                                const gchar * const *mimes,
                                                const GDKWSourceHandlers *handlers,
                                                gpointer        user_data);
void        _gdk_wayland_dd_source_set_actions (GDKWSource *src,
                                                guint32     wl_mask);
void        _gdk_wayland_dd_source_free        (GDKWSource *src);

/* wl_data_device.set_selection / zwp_primary_selection_device_v1.set_selection
 * with @src (NULL clears).  @kind CLIPBOARD or PRIMARY. */
void        _gdk_wayland_dd_set_selection      (GDKWDataDevice *dd,
                                                GDKWSelKind     kind,
                                                GDKWSource     *src,
                                                guint32         serial);

/* wl_data_device.start_drag (@src, origin surface of @origin_toplevel, icon NULL,
 * @serial).  Returns FALSE (nothing sent) without a usable device, or when
 * @origin_toplevel has no wl_surface. */
gboolean    _gdk_wayland_dd_start_drag         (GDKWDataDevice *dd,
                                                GDKWSource     *src,
                                                GdkWindow      *origin_toplevel,
                                                guint32         serial);

/* Called by the selection piece when its local owner for @kind goes away: frees
 * the offer in dd->selection_offer[kind] if it is flagged maybe_self. */
void        _gdk_wayland_dd_forget_self_offer  (GDKWDataDevice *dd,
                                                GDKWSelKind     kind);

/* The device listeners forward DnD events to the dnd piece (below).  The offer
 * listeners source_actions and action call _gdk_wayland_dnd_offer_changed (). */

/* ---- owner "dnd": gdkdnd-wayland.c (destination, type) --------------------
 *
 * Existing functions that get real bodies here:
 *   _gdk_wayland_drag_context_get_display ()   (context->display, else windows)
 *   _gdk_wayland_drag_status ()
 *   _gdk_wayland_drag_get_selection ()         (the atom XdndSelection)
 *   _gdk_wayland_drop_reply ()
 *   _gdk_wayland_drop_finish ()
 *   _gdk_wayland_window_register_dnd ()
 * New, called by the datadevice listeners with the live GDKWDataDevice: */

void  _gdk_wayland_dnd_dest_enter     (GDKWDataDevice *dd,
                                       guint32         serial,
                                       GDKWWindowImplWayland *impl,  /* may be NULL */
                                       GDKWOffer      *offer,        /* may be NULL */
                                       gdouble         x,
                                       gdouble         y);
void  _gdk_wayland_dnd_dest_motion    (GDKWDataDevice *dd,
                                       guint32         time,
                                       gdouble         x,
                                       gdouble         y);
void  _gdk_wayland_dnd_dest_leave     (GDKWDataDevice *dd);
void  _gdk_wayland_dnd_dest_drop      (GDKWDataDevice *dd);
/* After offer.source_actions or offer.action changed the stored values. */
void  _gdk_wayland_dnd_offer_changed  (GDKWDataDevice *dd,
                                       GDKWOffer      *offer);

/* Idempotent.  Drops the destination and source state that refers to @window
 * (called by _gdk_wayland_selection_window_destroyed ()). */
void  _gdk_wayland_dnd_window_destroyed (GdkWindow *window);

/* Idempotent.  Releases every context the dnd piece holds, destroys the drag
 * source, and sets GDKWDragContext.display to NULL for contexts that live on. */
void  _gdk_wayland_dnd_display_finalize (GdkDisplay *display);

/* Queues a DND event with gdk_event_put () (dnd piece internal helper, shared by
 * its two files): type is one of GDK_DRAG_ENTER, _LEAVE, _MOTION, _STATUS,
 * GDK_DROP_START, _FINISHED.  The event takes a reference on @window and
 * @context. */
void  _gdk_wayland_dnd_put_event      (GdkEventType     type,
                                       GdkWindow       *window,
                                       GDKWDragContext *context,
                                       guint32          time,
                                       gint             x_root,
                                       gint             y_root);

/* ---- owner "dnd": gdkdnd-source-wayland.c (source) ------------------------
 *
 * Existing functions that get real bodies here:
 *   _gdk_wayland_drag_begin ()
 *   _gdk_wayland_drag_get_protocol_for_display ()  (unchanged: 0, NONE)
 *   _gdk_wayland_drag_find_window_for_screen ()
 *   _gdk_wayland_drag_motion ()
 *   _gdk_wayland_drag_drop ()
 *   _gdk_wayland_drag_abort ()
 *   _gdk_wayland_drag_drop_succeeded ()
 * New: */

/* The active (not DONE) source context of @display, borrowed, or NULL. */
GDKWDragContext *_gdk_wayland_dnd_active_source (GdkDisplay *display);

/* Handlers for the drag source; user_data is the GDKWDragContext. */
const GDKWSourceHandlers *_gdk_wayland_dnd_source_handlers (void);

/* Ends a source context with no event: removes the fallback timer, frees the
 * drag source and clears dd->dnd.source.  Defined in gdkdnd-wayland.c, used by
 * gdkdnd-source-wayland.c. */
void _gdk_wayland_dnd_source_end (GDKWDragContext *context);

G_END_DECLS

#endif /* GDK_COMPILATION */

#endif /* __YDK_GDKM8_WAYLAND_H__ */
