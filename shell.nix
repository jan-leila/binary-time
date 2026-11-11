let
  sources = import ./npins;
  pebble = import sources."pebble.nix";
  pkgs = import sources.nixpkgs { };
in
pebble.pebbleEnv {
  emulatorTarget = "basalt";
  # sdl3: pebble-qemu needs it to start the emulator.
  # gcc: waf's configure step wants a host compiler (the ARM one is found separately).
  packages = [ pkgs.sdl3 pkgs.gcc ];
  # Newer SDKs need pebble-tool >= 5.1, which the pinned pebble.nix doesn't have yet.
  shellHook = ''
    unset LD_LIBRARY_PATH
    # the gcc wrapper exports CC/CXX, which would override the ARM cross compiler
    unset CC CXX
    [ -e ~/.local/share/pebble-sdk/SDKs/4.9.169 ] \
      || echo "Hint: run 'pebble sdk install 4.9.169' (newer SDKs need pebble-tool >= 5.1)"
  '';
}
