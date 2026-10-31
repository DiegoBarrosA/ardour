#ifndef __YDK_ALIASDEF_DECLS_H__
#define __YDK_ALIASDEF_DECLS_H__

/* Force-included into every ydk (core/x11/wayland) translation unit, BEFORE
 * the per-source alias snippets, so that:
 *   - the __typeof() targets of the alias declarations are in scope, and
 *   - gdkalias.h is active, remapping every gdk_* reference inside the
 *     library onto its hidden IA__gdk_* name (the exported aliases emitted
 *     per source keep the public ABI).
 * Must come before the source code; include guards keep the later
 * re-inclusion of gdk.h by the source itself a no-op.
 */

/* GDK_ENABLE_BROKEN unlocks a few extra declarations (e.g.
 * gdk_image_new_bitmap) that gdkenumtypes.c and gdkalias.h gate on; defining
 * it up front keeps those headers consistent on their first pass. */
#ifndef GDK_ENABLE_BROKEN
#define GDK_ENABLE_BROKEN
#endif

#include <ydk/gdk.h>

#include "gdkinternals.h"
#include "gdkwindowimpl.h"

/* The gdkalias.h x11 sections are gated on the x11 header markers (IN_HEADER
 * collapses to `defined(...)`), so the x11 IA__gdk_x11_* declarations only
 * become visible once the x11 backend headers are in the TU.  They must be
 * here, BEFORE the first gdkalias.h pass, otherwise the per-source alias
 * snippets and the sources' own re-include of gdkalias.h self-reference an
 * IA__gdk_x11_* name that was never declared.
 * gdkconfig.h defines GDK_WINDOWING_X11 for every TU (all backends ship in
 * one library), so gate on the per-object marker instead. */
#ifdef GDK_YDK_X11_OBJ
#include "gdkx.h"

#include "gdkpixmap-x11.h"
#include "gdkwindow-x11.h"
#elif defined(GDK_YDK_DISPATCH_OBJ)
/* gdkbackend.c: only the public gdk_x11_* declarations, for the M6b
 * dispatchers of the X11 handle accessors and API guards. */
#include "gdkx.h"
#endif

#include "gdkalias.h"

#endif /* __YDK_ALIASDEF_DECLS_H__ */