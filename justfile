build:
  nix build '.?submodules=1#wayfire'

compile:
  cd build/ && meson compile
