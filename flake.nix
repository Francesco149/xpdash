{
  description = "xpdash — ultra-low latency streaming and remote control for real Windows XP hardware";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-25.05";
    flake-utils.url = "github:numtide/flake-utils";
    rust-overlay = {
      url = "github:oxalica/rust-overlay";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs = { self, nixpkgs, flake-utils, rust-overlay }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        overlays = [ (import rust-overlay) ];
        pkgs = import nixpkgs { inherit system overlays; };
        rustToolchain = pkgs.rust-bin.stable.latest.default.override {
          targets = [ "x86_64-unknown-linux-gnu" "x86_64-pc-windows-gnu" ];
        };
        mingw32 = pkgs.pkgsCross.mingw32.buildPackages;
        mingwW64 = pkgs.pkgsCross.mingwW64.buildPackages;
        mingwW64_pthreads = pkgs.pkgsCross.mingwW64.windows.pthreads;
        mingwW64_crt = pkgs.pkgsCross.mingwW64.windows.mingw_w64;
      in
      {
        devShells.default = pkgs.mkShell {
          name = "xpdash-dev";
          packages = [
            # i686 Windows XP cross toolchain
            mingw32.gcc
            mingw32.binutils

            # x86_64 Windows 10/11 cross toolchain
            mingwW64.gcc
            mingwW64.binutils

            # Modern Rust toolchain (1.88+) with Linux & Windows targets
            rustToolchain

            # Native build tools
            pkgs.gnumake
            pkgs.pkg-config
            pkgs.nasm
            pkgs.cmake
            pkgs.zip
            pkgs.p7zip

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
            export MINGW32_CC=i686-w64-mingw32-gcc
            export MINGW32_OBJDUMP=i686-w64-mingw32-objdump
            export MINGW64_CC=x86_64-w64-mingw32-gcc
            export CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER=x86_64-w64-mingw32-gcc
            export CARGO_TARGET_X86_64_PC_WINDOWS_GNU_RUSTFLAGS="-L ${mingwW64_pthreads}/lib -L ${mingwW64_crt}/lib"
            echo "xpdash development shell loaded"
            echo "  mingw32 cc : $(command -v $MINGW32_CC || echo MISSING)"
            echo "  mingw64 cc : $(command -v $MINGW64_CC || echo MISSING)"
            echo "  rustc      : $(rustc --version)"
            echo "  cargo      : $(cargo --version)"
          '';
        };

        formatter = pkgs.nixfmt-rfc-style;
      });
}
