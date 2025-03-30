{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    utils.url = "github:numtide/flake-utils";
  };
  outputs = {
    self,
    nixpkgs,
    utils,
  }:
    utils.lib.eachDefaultSystem (
      system: let
        pkgs = nixpkgs.legacyPackages.${system};
        clangStdenv = pkgs.clangStdenv;
      in {
        devShell = (pkgs.mkShell.override { stdenv = clangStdenv; }) {
          inputsFrom = [
            (self.packages.${system}.default.overrideAttrs (old: {
              # otherwise clang-tidy won't find the stdenv headers for cpp
              nativeBuildInputs = old.nativeBuildInputs ++ [ pkgs.clang-tools ];
            }))
          ];
          buildInputs = with pkgs; [
          ];
        };

        packages.default = clangStdenv.mkDerivation {
          name = "wayfire-test";

          src = ./.;

          nativeBuildInputs = with pkgs; [
            meson
            ninja
            pkg-config
            wayland-scanner
          ];

          buildInputs = with pkgs; [
            libGL
            libdrm
            libexecinfo
            libevdev
            libinput
            libjpeg
            libxkbcommon
            wayland-protocols
            xorg.xcbutilwm
            nlohmann_json
            yyjson
            libxml2

            # wf-config
            glm

            # clang
            llvmPackages.openmp
          ];

          propagatedBuildInputs = with pkgs; [
            # wf-config
            wlroots_0_18
            wayland
            cairo
            pango
          ];

          nativeCheckInputs = with pkgs; [
            cmake
            doctest
          ];

          # CMake is just used for finding doctest.
          dontUseCmakeConfigure = true;

          doCheck = true;

          mesonFlags = with pkgs; [
            "--sysconfdir /etc"
            "-Duse_system_wlroots=enabled"
            "-Duse_system_wfconfig=disabled"
            (lib.mesonEnable "wf-touch:tests" (stdenv.buildPlatform.canExecute stdenv.hostPlatform))
          ];
        };
      }
    );
}
