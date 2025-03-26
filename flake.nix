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
      in rec {
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

        packages.wayfire = (pkgs.wayfire.override { stdenv = clangStdenv; }).overrideAttrs (old: {
          src = ./.;

          buildInputs =
            old.buildInputs
            ++ (with pkgs; [
              llvmPackages.openmp
            ]);

          mesonFlags = with pkgs; [
            "--sysconfdir /etc"
            "-Duse_system_wlroots=enabled"
            "-Duse_system_wfconfig=enabled"
            (lib.mesonEnable "wf-touch:tests" (stdenv.buildPlatform.canExecute stdenv.hostPlatform))
          ];
        });
        packages.default = packages.wayfire;
      }
    );
}
