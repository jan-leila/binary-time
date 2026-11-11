# Binary Time

A Pebble binary (BCD) watchface, rebuilt from scratch from a closed-source watchface that has no
published source.

## Build

```
nix-shell                       # pebble.nix pinned with npins (see shell.nix)
pebble sdk install 4.9.169      # once; newer SDKs need pebble-tool >= 5.1
pebble build                    # -> build/binary-watch.pbw (aplite basalt chalk diorite emery)
pebble install --emulator basalt
```
