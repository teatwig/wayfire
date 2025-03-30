build:
  nix build "git+file://$(pwd)?submodules=1"

compile:
  cd build/ && meson compile
