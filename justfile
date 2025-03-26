build:
  nix build "git+file://$(pwd)?submodules=1"

compile:
  cd build/ && nix develop .#default -c meson compile
