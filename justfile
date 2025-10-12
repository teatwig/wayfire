build:
  nix build '.?submodules=1#wayfire'

build-wcm:
  nix build '.?submodules=1#wcm'

compile:
  cd build/ && meson compile
