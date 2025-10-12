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
        inherit (pkgs) lib;
      in rec {
        devShell = (pkgs.mkShell.override {stdenv = pkgs.clangStdenv;}) {
          inputsFrom = [
            (self.packages.${system}.default.overrideAttrs (old: {
              # otherwise clang-tidy won't find the stdenv headers for cpp
              nativeBuildInputs =
                old.nativeBuildInputs
                ++ (with pkgs; [
                  clang-tools
                  llvmPackages.openmp
                ]);
            }))
          ];
          buildInputs = with pkgs; [
          ];
        };

        packages.wayfire = pkgs.stdenv.mkDerivation {
          pname = "wayfire-tea";
          version = "0.10.0";

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

            libxml2
            yyjson
            vulkan-headers

            glm # for wf-config
          ];

          propagatedBuildInputs = with pkgs; [
            # wf-config
            wlroots_0_19
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
            # "-Duse_system_wfconfig=enabled"
            "-Duse_system_wfconfig=disabled"
            (lib.mesonEnable "wf-touch:tests" (stdenv.buildPlatform.canExecute stdenv.hostPlatform))
          ];

          passthru.providedSessions = ["wayfire"];

          meta = {
            homepage = "https://wayfire.org/";
            description = "3D Wayland compositor";
            license = lib.licenses.mit;
            platforms = lib.platforms.unix;
            mainProgram = "wayfire";
          };
        };
        packages.wcm = (pkgs.wayfirePlugins.wcm.override {wayfire = packages.wayfire;}) .overrideAttrs (old: rec {
          version = "0.10.0";

          src = pkgs.fetchFromGitHub {
            owner = "WayfireWM";
            repo = "wcm";
            rev = "v${version}";
            fetchSubmodules = true;
            hash = "sha256-O4BYwb+GOMZIn3I2B/WMJ5tUZlaegvwBuyNK9l/gxvQ=";
          };

          # remove wf-shell since we don't use it
          buildInputs = (old.buildInputs ++ [pkgs.glm]) |> lib.remove pkgs.wayfirePlugins.wf-shell;
          mesonFlags = ["-Dwf_shell=disabled"];
        });
        packages.default = packages.wayfire;

        formatter = pkgs.alejandra;
      }
    );
}
