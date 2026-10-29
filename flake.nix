{
  description = "ydk-wayland: Ardour's GDK2 fork (YDK) as a standalone library with a native Wayland backend";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };
        lib = pkgs.lib;

        # Runtime libraries (for LD_LIBRARY_PATH / test harness).
        runtimeDeps = with pkgs; [
          glib gdk-pixbuf pango cairo libpng libjpeg
          wayland libxkbcommon
        ] ++ (with pkgs.xorg; [
          libX11 libXext libXi libXrandr libXinerama libXfixes
        ]);

        # Build/header inputs: the .dev outputs carry headers and pkg-config
        # files plus unversioned .so symlinks, so one list serves both
        # compilation and linking.
        devDeps = with pkgs; [
          glib.dev gdk-pixbuf.dev pango.dev cairo.dev
          libpng.dev libjpeg.dev wayland.dev libxkbcommon.dev
        ] ++ (with pkgs.xorg; [
          libX11.dev libXext.dev libXi.dev libXrandr.dev
          libXinerama.dev libXfixes.dev
        ]);

        buildInputs = devDeps;

        ydk-wayland = pkgs.stdenv.mkDerivation {
          pname = "ydk-wayland";
          version = "0.1.0";
          src = self;

          nativeBuildInputs = [ pkgs.python3 pkgs.pkg-config ];
          inherit buildInputs;

          preBuild = ''
            export HOME=$TMPDIR
          '';

          buildPhase = ''
            python3 waf configure --prefix=$out --with-x11 --with-wayland
            python3 waf build
          '';

          installPhase = ''
            python3 waf install || true
            runHook postInstall
          '';

          postInstall = ''
            mkdir -p $out/include
            cp -r ydk/ydk "$out/include/ydk"
            cp -r ydk-pixbuf/ydk-pixbuf "$out/include/ydk-pixbuf"
            cp ydk/gdkconfig.h "$out/include/gdkconfig.h"
            cp ydk/gdkconfig.h "$out/include/ydk/gdkconfig.h"
            # M6: the demo drives the headless input test hooks declared in the
            # public section of this header
            cp ydk/wayland/ydk/gdkseat-wayland.h "$out/include/ydk/"
            # M7: cursor, shape and font test hooks (public section)
            cp ydk/wayland/ydk/gdkm7-wayland.h "$out/include/ydk/"
            cp ydk/wayland/ydk/gdkm8-wayland.h "$out/include/ydk/"
            cp ydk/wayland/ydk/gdkm9-wayland.h "$out/include/ydk/"

            # M6b: the dispatch audit (scripts/audit-x11-symbols.sh) reads the
            # per backend object files and the shared library with nm
            mkdir -p $out/share/ydk-audit/build
            cp -r build/ydk $out/share/ydk-audit/build/ydk
            cp -L build/libydk.so $out/share/ydk-audit/build/libydk.so

            mkdir -p $out/lib/pkgconfig
            cat > $out/lib/pkgconfig/ydk.pc <<'PC'
            prefix=@PREFIX@
            includedir=@PREFIX@/include
            libdir=@PREFIX@/lib

            Name: ydk
            Description: YDK 2.24.23 (Ardour's GDK2 fork) glue with Wayland/X11 backends
            Version: 2.24.23
            Requires: glib-2.0 gobject-2.0 gio-unix-2.0 pango cairo gdk-pixbuf-2.0
            Libs: -L@LIBDIR@ -lydk
            Cflags: -I@INCLUDEDIR@
            PC
            sed -i -e "s|@PREFIX@|$out|" \
                    -e "s|@LIBDIR@|$out/lib|" \
                    -e "s|@INCLUDEDIR@|$out/include|" \
                    $out/lib/pkgconfig/ydk.pc

            cat > $out/lib/pkgconfig/ydk-pixbuf.pc <<'PC'
            prefix=@PREFIX@
            includedir=@PREFIX@/include
            libdir=@PREFIX@/lib

            Name: ydk-pixbuf
            Description: gdk-pixbuf 2.31.1 fork used by YDK
            Version: 2.31.1
            Requires: glib-2.0 gobject-2.0 gdk-pixbuf-2.0
            Libs: -L@LIBDIR@ -lydk-pixbuf
            Cflags: -I@INCLUDEDIR@
            PC
            sed -i -e "s|@PREFIX@|$out|" \
                    -e "s|@LIBDIR@|$out/lib|" \
                    -e "s|@INCLUDEDIR@|$out/include|" \
                    $out/lib/pkgconfig/ydk-pixbuf.pc
          '';
        };

        ydk-demo = pkgs.stdenv.mkDerivation {
          pname = "ydk-demo";
          version = "0.1.0";
          src = self;

          nativeBuildInputs = [ pkgs.pkg-config ];
          buildInputs = [ ydk-wayland ] ++ buildInputs;

          PKG_CONFIG_PATH = "${ydk-wayland}/lib/pkgconfig";

          buildPhase = ''
            mkdir -p $out/bin
            $CC -o $out/bin/ydk-demo demo/ydk_demo.c \
                $(pkg-config --cflags --libs ydk)
          '';

          installPhase = "true";
        };

        # Test payload dir: gives the scripts a stable way to reference the demo.
        test-env = pkgs.buildEnv {
          name = "ydk-test-env";
          paths = [ ydk-demo ];
        };

        smoke-test = pkgs.writeShellScript "ydk-smoke-test" ''
          export PATH=${lib.makeBinPath [ pkgs.xorg.xorgserver pkgs.weston pkgs.dbus pkgs.imagemagick pkgs.binutils pkgs.coreutils pkgs.gnugrep pkgs.gawk pkgs.gnused ydk-demo ]}:$PATH
          export LD_LIBRARY_PATH=${lib.makeLibraryPath [ ydk-wayland ]}:$LD_LIBRARY_PATH
          export FONTCONFIG_FILE=${pkgs.makeFontsConf { fontDirectories = [ pkgs.dejavu_fonts ]; }}
          export YDK_AUDIT_SCRIPT=${./scripts/audit-x11-symbols.sh}
          export YDK_AUDIT_BUILD=${ydk-wayland}/share/ydk-audit/build
          ${pkgs.bash}/bin/bash ${./scripts/smoke-test.sh}
        '';

        # M6b: the Wayland demo under valgrind.  Separate from the smoke test
        # because it is 20 to 50 times slower.  Run it with
        #   nix run .#valgrind-demo
        valgrind-demo = pkgs.writeShellScript "ydk-valgrind-demo" ''
          export PATH=${lib.makeBinPath [ pkgs.valgrind pkgs.weston pkgs.dbus pkgs.coreutils pkgs.gnugrep pkgs.procps ydk-demo ]}:$PATH
          export LD_LIBRARY_PATH=${lib.makeLibraryPath [ ydk-wayland ]}:$LD_LIBRARY_PATH
          export FONTCONFIG_FILE=${pkgs.makeFontsConf { fontDirectories = [ pkgs.dejavu_fonts ]; }}
          export YDK_VALGRIND_SUPP=${./scripts/valgrind.supp}
          ${pkgs.bash}/bin/bash ${./scripts/valgrind-smoke.sh}
        '';

        baseline-diff = pkgs.writeShellScript "ydk-baseline-diff" ''
          export PATH=${lib.makeBinPath [ pkgs.xorg.xorgserver pkgs.weston pkgs.dbus pkgs.imagemagick pkgs.coreutils pkgs.util-linux ydk-demo ]}:$PATH
          export LD_LIBRARY_PATH=${lib.makeLibraryPath [ ydk-wayland ]}:$LD_LIBRARY_PATH
          export FONTCONFIG_FILE=${pkgs.makeFontsConf { fontDirectories = [ pkgs.dejavu_fonts ]; }}
          ${pkgs.bash}/bin/bash ${./scripts/baseline-diff.sh}
        '';

        python-with-python-symlink = pkgs.symlinkJoin {
          name = "python";
          paths = [ pkgs.python3 ];
        };
      in
      {
        packages = {
          inherit ydk-wayland ydk-demo smoke-test baseline-diff valgrind-demo;
          default = ydk-wayland;
        };

        apps = {
          smoke-test = { type = "app"; program = "${smoke-test}"; };
          baseline-diff = { type = "app"; program = "${baseline-diff}"; };
          valgrind-demo = { type = "app"; program = "${valgrind-demo}"; };
        };

        checks = {
          smoke-test = pkgs.runCommand "smoke-test-check" { } ''
            ${smoke-test}
            echo "smoke test passed" > $out
          '';
        };

        devShells.default = pkgs.mkShell {
          name = "ydk-wayland-dev";
          packages = with pkgs; [
            python-with-python-symlink
            pkg-config
            gcc
          ] ++ devDeps ++ [
            wayland-protocols wayland-scanner
            mesa
            weston grim imagemagick
            valgrind binutils
            xorg.xorgserver
            dbus libei
          ];
          shellHook = ''
            export LD_LIBRARY_PATH=${lib.makeLibraryPath runtimeDeps}
          '';
        };
      });
}