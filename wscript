#!/usr/bin/env python
# -*- coding: utf-8 -*-
#
# Root build script for the standalone ydk-wayland project.
#
# Replaces the Ardour-child wscript layout: this file alone drives the
# tree.  It builds:
#   libydk          -- ydk (core) + x11 + wayland backends, one shared lib
#   libydk-pixbuf   -- the vendored gdk-pixbuf fork
#
# Compiles each backend as its own object group so compile flags can be
# kept separate.  Most importantly, the X11 object is built with the
# backend rename map force-included (-include ydk/x11/ydk/gdkwindowing-x11.h)
# which stamps its `_gdk_windowing_*` symbols `_gdk_x11_*` so they cannot
# collide with the Wayland backend's `_gdk_wayland_*` implementation; the
# dispatcher (ydk/gdkbackend.c, part of the core object) owns the canonical
# names.  See DESIGN.md for the full rationale.
#
# The gdkaliasdef.c object is compiled separately with every per-file
# alias marker macro that the compiled sources define (they are detected by
# scanning the sources), because the public symbol aliases (gdk_display_*,
# gdk_window_*, ...) are emitted there.

import re

import waflib

VERSION = '2.24.23'
PIXBUF_VERSION = '2.31.1'

libydk_sources = [
    'ydk/gdk.c',
    'ydk/gdkapplaunchcontext.c',
    'ydk/gdkbackend.c',
    'ydk/gdkcairo.c',
    'ydk/gdkcolor.c',
    'ydk/gdkcursor.c',
    'ydk/gdkdisplay.c',
    'ydk/gdkdisplaymanager.c',
    'ydk/gdkdnd.c',
    'ydk/gdkdraw.c',
    'ydk/gdkenumtypes.c',
    'ydk/gdkevents.c',
    'ydk/gdkfont.c',
    'ydk/gdkgc.c',
    'ydk/gdkglobals.c',
    'ydk/gdkimage.c',
    'ydk/gdkkeys.c',
    'ydk/gdkkeyuni.c',
    'ydk/gdkmarshalers.c',
    'ydk/gdkoffscreenwindow.c',
    'ydk/gdkpango.c',
    'ydk/gdkpixbuf-drawable.c',
    'ydk/gdkpixbuf-render.c',
    'ydk/gdkpixmap.c',
    'ydk/gdkpolyreg-generic.c',
    'ydk/gdkrectangle.c',
    'ydk/gdkregion-generic.c',
    'ydk/gdkrgb.c',
    'ydk/gdkscreen.c',
    'ydk/gdkselection.c',
    'ydk/gdkvisual.c',
    'ydk/gdkwindow.c',
    'ydk/gdkwindowimpl.c',
]

libydk_wayland_sources = [
    'ydk/wayland/gdkdisplay-wayland.c',
    'ydk/wayland/gdkevents-wayland.c',
    'ydk/wayland/gdkkeys-wayland.c',
    'ydk/wayland/gdkmain-wayland.c',
    'ydk/wayland/gdkscreen-wayland.c',
    'ydk/wayland/gdkwindowimpl-wayland.c',
    'ydk/wayland/xdg-shell-protocol.c',
]

libydk_x11_sources = [
    'ydk/x11/gdkapplaunchcontext-x11.c',
    'ydk/x11/gdkasync.c',
    'ydk/x11/gdkcolor-x11.c',
    'ydk/x11/gdkcursor-x11.c',
    'ydk/x11/gdkdisplay-x11.c',
    'ydk/x11/gdkdnd-x11.c',
    'ydk/x11/gdkdrawable-x11.c',
    'ydk/x11/gdkevents-x11.c',
    'ydk/x11/gdkfont-x11.c',
    'ydk/x11/gdkgc-x11.c',
    'ydk/x11/gdkgeometry-x11.c',
    'ydk/x11/gdkglobals-x11.c',
    'ydk/x11/gdkim-x11.c',
    'ydk/x11/gdkimage-x11.c',
    'ydk/x11/gdkinput-none.c',
    'ydk/x11/gdkinput.c',
    'ydk/x11/gdkkeys-x11.c',
    'ydk/x11/gdkmain-x11.c',
    'ydk/x11/gdkpixmap-x11.c',
    'ydk/x11/gdkproperty-x11.c',
    'ydk/x11/gdkscreen-x11.c',
    'ydk/x11/gdkselection-x11.c',
    'ydk/x11/gdkspawn-x11.c',
    'ydk/x11/gdkvisual-x11.c',
    'ydk/x11/gdkwindow-x11.c',
    'ydk/x11/gdkxftdefaults.c',
    'ydk/x11/gdkxid.c',
    'ydk/x11/xsettings-client.c',
    'ydk/x11/xsettings-common.c',
]

libydkpixbuf_sources = [
    'ydk-pixbuf/gdk-pixbuf-animation.c',
    'ydk-pixbuf/gdk-pixbuf-data.c',
    'ydk-pixbuf/gdk-pixbuf-enum-types.c',
    'ydk-pixbuf/gdk-pixbuf-io.c',
    'ydk-pixbuf/gdk-pixbuf-loader.c',
    'ydk-pixbuf/gdk-pixbuf-scale.c',
    'ydk-pixbuf/gdk-pixbuf-scaled-anim.c',
    'ydk-pixbuf/gdk-pixbuf-simple-anim.c',
    'ydk-pixbuf/gdk-pixbuf-util.c',
    'ydk-pixbuf/gdk-pixbuf.c',
    'ydk-pixbuf/gdk-pixdata.c',
    'ydk-pixbuf/io-pixdata.c',
    'ydk-pixbuf/io-jpeg.c',
    'ydk-pixbuf/io-png.c',
    'ydk-pixbuf/io-xbm.c',
    'ydk-pixbuf/io-xpm.c',
    'ydk-pixbuf/pixops/pixops.c',
]

with_x11 = True
with_wayland = True

def options(opt):
    opt.load('compiler_c')
    opt.add_option('--prefix', type='string', default='',
                   dest='prefix', help='installation prefix')
    opt.add_option('--with-x11', action='store_true', dest='with_x11', default=True,
                   help='build the X11 backend (default)')
    opt.add_option('--without-x11', action='store_false', dest='with_x11',
                   help='do not build the X11 backend')
    opt.add_option('--with-wayland', action='store_true', dest='with_wayland', default=True,
                   help='build the Wayland backend (default)')
    opt.add_option('--without-wayland', action='store_false', dest='with_wayland',
                   help='do not build the Wayland backend')

def configure(conf):
    global with_x11, with_wayland

    with_x11 = getattr(conf.options, 'with_x11', True)
    with_wayland = getattr(conf.options, 'with_wayland', True)

    if not with_x11 and not with_wayland:
        conf.fatal('at least one backend is required (--with-x11 / --with-wayland)')

    autowaf = None
    conf.load('compiler_c')

    def check_pkg(package, uselib_store, minimum='0.0.0', mandatory=True):
        conf.check_cfg(package=package, uselib_store=uselib_store,
                       args='--cflags --libs', atleast_version=minimum,
                       mandatory=mandatory)

    check_pkg('glib-2.0', 'GLIB', '2.28')
    check_pkg('gio-2.0', 'GIO', '2.2')
    check_pkg('gobject-2.0', 'GOBJECT')
    check_pkg('pango', 'PANGO', '1.20')
    check_pkg('cairo', 'CAIRO', '1.12')
    check_pkg('pangocairo', 'PANGOCAIRO')
    check_pkg('gio-unix-2.0', 'GIO-UNIX')
    check_pkg('libpng', 'LIBPNG')

    conf.check_cc(header_name='jpeglib.h', lib='jpeg',
                  uselib_store='LIBJPEG', mandatory=False,
                  execute=False)

    if with_x11:
        check_pkg('x11', 'X11')
        check_pkg('xext', 'XEXT')
        check_pkg('xrandr', 'RANDR', '1.2.99', False)
        check_pkg('xi', 'XI', '1.7.10', False)
        conf.check_cc(lib='dl', uselib_store='DL', mandatory=False)
        conf.check_cc(header_name='X11/extensions/Xinerama.h', lib='Xinerama',
                      uselib_store='XINERAMA', mandatory=False)

    if with_wayland:
        check_pkg('wayland-client', 'WAYLAND')
        check_pkg('xkbcommon', 'XKBCOMMON')

def build(bld):
    core_sources = list(libydk_sources)

    base_defines = [
        'HAVE_CONFIG_H', '_LARGEFILE64_SOURCE', '_REENTRANT',
        'G_LOG_DOMAIN="Gdk"', 'GDK_COMPILATION', '_FILE_OFFSET_BITS=64',
        'GDK_PIXBUF_DISABLE_DEPRECATED', 'G_DISABLE_CAST_CHECKS',
        'G_DISABLE_SINGLE_INCLUDES', 'GDK_PIXBUF_DISABLE_SINGLE_INCLUDES',
    ]

    def new_obj(name, sources, includes, defines, uselib, extra_cflags):
        obj = bld.objects(target=name, source=sources)
        obj.cflags = ['-fPIC', '-w'] + list(extra_cflags)
        obj.includes = includes
        obj.uselib = uselib
        obj.defines = list(base_defines) + defines
        return obj

    def per_source(prefix, sources, includes, defines, uselib, extra_cflags):
        objs = []
        for src in sources:
            name = '%s-%s' % (prefix, src.split('/')[-1].rsplit('.', 1)[0])
            objs.append(new_obj(name, [src], includes, defines, uselib,
                                extra_cflags))
        return objs

    ydk_core_objs = per_source(
        'core', core_sources,
        ['.', 'ydk', 'ydk/ydk', 'ydk/wayland/ydk',
         'ydk/x11', 'ydk/x11/ydk',
         'ydk-pixbuf', 'ydk-pixbuf/ydk-pixbuf'], [],
        ['GLIB', 'GOBJECT', 'PANGO', 'CAIRO', 'PANGOCAIRO',
         'GIO-UNIX', 'WAYLAND'], [])

    ydk_wayland_objs = []
    if with_wayland:
        ydk_wayland_objs = per_source(
            'wayland', libydk_wayland_sources,
            ['.', 'ydk', 'ydk/ydk', 'ydk/wayland/ydk',
             'ydk-pixbuf', 'ydk-pixbuf/ydk-pixbuf'], ['_GNU_SOURCE'],
            ['GLIB', 'GOBJECT', 'WAYLAND', 'XKBCOMMON', 'CAIRO', 'PANGO'], [])

    ydk_x11_objs = []
    if with_x11:
        rename_map = bld.srcnode.find_node('ydk/x11/ydk/gdkwindowing-x11.h')
        ydk_x11_objs = per_source(
            'x11', libydk_x11_sources,
            ['.', 'ydk', 'ydk/ydk', 'ydk/x11', 'ydk/x11/ydk',
             'ydk/ydk/x11',
             'ydk-pixbuf', 'ydk-pixbuf/ydk-pixbuf'],
            ['GDK_WINDOWING_X11', 'GDK_YDK_X11_OBJ', 'HAVE_XSYNC=1'],
            ['GLIB', 'GOBJECT', 'GIO-UNIX', 'CAIRO', 'PANGO',
             'PANGOCAIRO',
             'X11', 'XEXT', 'RANDR', 'XI', 'DL', 'XINERAMA'],
            ['-include', rename_map.abspath()])

    # --- Public-symbol aliases -------------------------------------------
    # gdkaliasdef.c is a checked-in description of the marker -> exported
    # symbol table.  Modern gcc refuses the old "standalone TU" emission
    # (the IA__ target is never defined there), so every alias is emitted in
    # the TU that defines the target: each compiled source gets a per-source
    # snippet force-included into its own compilation unit (so each object is
    # a single source).  tools/gen_aliases.py does the marker bookkeeping.
    import sys
    sys.path.insert(0, bld.srcnode.find_dir('tools').abspath())
    import gen_aliases

    marker_re = re.compile(r'^\s*#define\s+(__GDK[A-Z0-9_]*_C__)\s*$', re.M)

    all_sources = (core_sources
                   + (libydk_x11_sources if with_x11 else [])
                   + (libydk_wayland_sources if with_wayland else []))

    compiled_markers = set()
    for src in all_sources:
        node = bld.srcnode.find_node(src)
        if node:
            compiled_markers |= set(marker_re.findall(node.read()))

    # the per-file markers must be among the defined macros so that
    # `#if IN_FILE(...)` evaluates true for every compiled source.
    defined = {'GDK_WINDOWING_X11', 'GDK_WINDOWING_WAYLAND'} | compiled_markers

    marker_syms = gen_aliases.parse_marker_symbols(
        bld.srcnode.find_node('ydk/gdkaliasdef.c').abspath(), defined)

    decls = bld.srcnode.find_node('ydk/gdkaliasdef-decls.h').abspath()
    x11_decls = bld.srcnode.find_node('ydk/gdkaliasdef-x11-decls.h').abspath()

    # Build a definition index: where does each symbol actually get defined?
    # Export aliases must live in the same TU as the IA__ definition (modern
    # gcc requires it), so the marker table from gdkaliasdef.c is only a
    # fallback for symbols whose definition pattern we cannot detect.  The
    # scan is preprocessor-aware: definitions behind compile-time-inactive
    # guards (e.g. #ifndef HAVE_XCONVERTCASE) do not count, since their IA__
    # target will not exist in this TU either.
    all_symbols = set()
    for glyphs in marker_syms.values():
        all_symbols |= glyphs
    eval_defined = set(defined)
    cfg = bld.srcnode.find_node('ydk/config.h')
    if cfg:
        eval_defined |= gen_aliases.parse_defines(cfg.abspath())
    eval_defined |= {'_REENTRANT', 'GDK_COMPILATION', 'G_LOG_DOMAIN',
                     'HAVE_CONFIG_H', 'G_HAVE_GNUC_VISIBILITY'}
    for d in base_defines:
        eval_defined.add(d.split('=', 1)[0])
    def_owner = {}
    for src in all_sources:
        node = bld.srcnode.find_node(src)
        if not node:
            continue
        defined_here = gen_aliases.active_definitions(node.read(), eval_defined)
        for sym in (defined_here & all_symbols):
            def_owner.setdefault(sym, src)

    def stamp(objs, decl_incs):
        """Append declarations force-includes and each object's own snippet."""
        for obj in objs:
            for d in decl_incs:
                obj.cflags += ['-include', d]
            src = obj.source[0]
            node = bld.srcnode.find_node(src)
            if not node:
                continue
            src_markers = marker_re.findall(node.read())
            glyphs = {s for s in def_owner if def_owner[s] == src}
            others = set()
            for m in src_markers:
                others |= marker_syms.get(m, set())
            for s in others:
                if s not in def_owner:
                    glyphs.add(s)
            if not glyphs:
                continue
            key = node.name.rsplit('.', 1)[0]
            aliases_node = bld.path.find_or_declare('ydk-aliases/%s' % key)
            aliases_node.write(gen_aliases.render_aliases(glyphs))
            obj.cflags += ['-include', aliases_node.abspath()]

    stamp(ydk_core_objs, [decls])
    if with_x11:
        stamp(ydk_x11_objs, [decls, x11_decls])
    if with_wayland:
        stamp(ydk_wayland_objs, [decls])

    use = [o.target for o in (ydk_core_objs + ydk_wayland_objs + ydk_x11_objs)]

    obj = bld.shlib(features='c cshlib')
    obj.cflags = ['-std=gnu89', '-fPIC', '-w']
    obj.includes = ['.', 'ydk', 'ydk/ydk', 'ydk-pixbuf', 'ydk-pixbuf/ydk-pixbuf']
    obj.name = 'libydk'
    obj.target = 'ydk'
    obj.use = use
    obj.use.append('libydk-pixbuf')
    obj.uselib = ['GLIB', 'GIO-UNIX', 'GOBJECT', 'PANGO', 'PANGOCAIRO', 'CAIRO',
                  'X11', 'XEXT', 'DL']
    if with_x11:
        obj.uselib += ['RANDR', 'XINERAMA', 'XI']
    if with_wayland:
        obj.uselib += ['WAYLAND', 'XKBCOMMON']
    obj.vnum = VERSION
    if bld.options.prefix:
        obj.install_path = bld.options.prefix + '/lib'

    pixbuf = bld.shlib(features='c cshlib')
    pixbuf.source = libydkpixbuf_sources
    pixbuf.cflags = ['-std=gnu99', '-fPIC', '-w']
    pixbuf.includes = ['.', 'ydk-pixbuf', 'ydk-pixbuf/ydk-pixbuf']
    pixbuf.name = 'libydk-pixbuf'
    pixbuf.target = 'ydk-pixbuf'
    pixbuf.uselib = ['GLIB', 'GIO', 'GOBJECT', 'LIBPNG', 'LIBJPEG']
    pixbuf.defines = [
        'HAVE_CONFIG_H', '_LARGEFILE64_SOURCE', '_REENTRANT',
        'G_LOG_DOMAIN="GdkPixbuf"', 'GDK_PIXBUF_COMPILATION',
        '_FILE_OFFSET_BITS=64', 'GDK_PIXBUF_ENABLE_BACKEND',
        'G_DISABLE_SINGLE_INCLUDES', '_POSIX_C_SOURCE=200809L',
        'INCLUDE_png', 'INCLUDE_xbm', 'INCLUDE_xpm', 'INCLUDE_jpeg',
        'PACKAGE="libydk-pixbuf"', 'GDK_PIXBUF_LOCALEDIR=""',
    ]
    pixbuf.vnum = PIXBUF_VERSION
    if bld.options.prefix:
        pixbuf.install_path = bld.options.prefix + '/lib'