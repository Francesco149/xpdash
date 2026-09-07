{
  description = "xpdash — ultra-low latency streaming and remote control for real Windows XP hardware";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-25.05";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };
        mingw32 = pkgs.pkgsCross.mingw32.buildPackages;
      in
      {
        devShells.default = pkgs.mkShell {
          name = "xpdash-dev";
          packages = [
            # i686 Windows XP cross toolchain
            mingw32.gcc
            mingw32.binutils

            # Native build tools
            pkgs.gnumake
            pkgs.cargo
            pkgs.rustc
            pkgs.pkg-config
            pkgs.nasm
            pkgs.cmake

            # Linux host client audio/graphics dev libraries
            pkgs.alsa-lib
            pkgs.libxkbcommon
            pkgs.libGL
            pkgs.xorg.libX11
            pkgs.xorg.libXcursor
            pkgs.xorg.libXi
            pkgs.xorg.libXrandr
            pkgs.wayland

            # Deployment and remote ops tools
            pkgs.samba
            pkgs.netexec
          ];

          shellHook = ''
            export MINGW32_CC=i686-w64-mingw32-gcc
            export MINGW32_OBJDUMP=i686-w64-mingw32-objdump
            export LD_LIBRARY_PATH="${pkgs.lib.makeLibraryPath [
              pkgs.alsa-lib
              pkgs.libGL
              pkgs.libxkbcommon
              pkgs.xorg.libX11
              pkgs.xorg.libXcursor
              pkgs.xorg.libXi
              pkgs.xorg.libXrandr
              pkgs.wayland
            ]}:$LD_LIBRARY_PATH"
            echo "xpdash development shell loaded"
            echo "  mingw32 cc : $(command -v $MINGW32_CC || echo MISSING)"
            echo "  rustc      : $(rustc --version)"
            echo "  cargo      : $(cargo --version)"
          '';
        };

        formatter = pkgs.nixfmt-rfc-style;
      });
}
