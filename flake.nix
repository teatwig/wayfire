{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    utils.url = "github:numtide/flake-utils";
  };
  outputs = { self, nixpkgs, utils }: utils.lib.eachDefaultSystem (system:
    let
      pkgs = nixpkgs.legacyPackages.${system};
    in
    {
      devShell = pkgs.mkShell {
        buildInputs = with pkgs; [
        ];
      };

      packages.default = pkgs.stdenv.mkDerivation {
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
