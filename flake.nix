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
          version = "0.11.0";

          src = ./.;

          # wayfire doesn't declare `drm` as a dependency for all plugins, which is why some files can't be resolved
          postPatch = ''
            substituteInPlace plugins/ipc-rules/meson.build \
              --replace \
              "all_deps = [wlroots, pixman, wfconfig, wftouch, json, plugin_pch_dep]" \
              "all_deps = [wlroots, pixman, drm, wfconfig, wftouch, json, plugin_pch_dep]"
          '';

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
            xcbutilwm

            libxml2
            yyjson
            vulkan-headers

            glm # for wf-config
            # wlroots deps
            glslang
            hwdata
            lcms
            libdisplay-info
            libgbm
            libliftoff
            libxcb-errors
            libxcb-render-util
            seatd
            xwayland
          ];

          propagatedBuildInputs = with pkgs; [
            # wf-config
            # wlroots_0_20
            wayland
            cairo
            pango
            vulkan-loader # for vulkan-effects
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
            "-Dvulkan_effects=true"
            "-Duse_system_wlroots=disabled"
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
        packages.wcm = (pkgs.wayfirePlugins.wcm.override {wayfire = packages.wayfire;}).overrideAttrs (old: rec {
          version = "0.11.0-unstable-2026-04-27";

          src = pkgs.fetchFromGitHub {
            owner = "WayfireWM";
            repo = "wcm";
            rev = "84063a07ccfc5be2a96b98d934271761a1730c2b";
            fetchSubmodules = true;
            hash = "sha256-WL4hXbiCDAKTkeB2zTUlMetS199a+fpnHqGuBTHRVDA=";
          };

          # remove wf-shell since we don't use it
          buildInputs = with pkgs;
            (old.buildInputs |> lib.remove wayfirePlugins.wf-shell)
            ++ [fmt glm];
          mesonFlags = ["-Dwf_shell=disabled"];
        });
        packages.default = packages.wayfire;

        formatter = pkgs.alejandra;
      }
    );
}
