# Metal FSR 4 workbench

This repository contains an experimental Metal execution path for the FSR
4.0.2 upscaling network and a FidelityFX/D3D12 bridge for Wine. The local
package is a test candidate. It has not been qualified in a game with the new
loading method.

## Local package

`./build.sh build` assembles the ignored `release/` directory:

```text
release/
├── lib/metal-fsr4.dylib
├── lib/wine/x86_64-windows/amd_fidelityfx_loader_dx12.dll
├── lib/wine/x86_64-windows/amd_fidelityfx_upscaler_dx12.dll -> amd_fidelityfx_loader_dx12.dll
├── lib/wine/x86_64-windows/amd_fidelityfx_framegeneration_dx12.dll
├── lib/wine/x86_64-windows/dxgi.dll
├── LICENSE
└── licenses/THIRD-PARTY-NOTICES.txt
```

The upscaler PE exports the FidelityFX API and the native Metal bridge entry,
`LabFullFrame`. Its second DLL name is a symlink because games request
different FidelityFX basenames. The separate frame-generation PE observes
FidelityFX calls and forwards them to the game's original AMD FG provider; it
does not generate frames itself. Wine's `winebuild --builtin` marks both PE
payloads so a process can select them through `WINEDLLPATH` and
`WINEDLLOVERRIDES`. The optional DXGI proxy is an ordinary native PE, selected
from bottle `system32` with a process-specific `dxgi=n,b` override for games
that do not call FidelityFX FG themselves. It delegates normal DXGI calls to
the original D3DMetal DLL. No game DLL or `d3d12.dll` is replaced.

The Metal dylib is retained from a verified local input (SHA-256
`1409d0809efb5a283cbfbf2dfe2b1362a25e24b235f2236c34251f53d6811a45`).
The native source has changed since that binary was built. A fresh native
build and game-level image and performance checks are required before a
published release.

```sh
./build.sh build
./build.sh verify
./build.sh status
```

Building archives the preceding local package under `.local/workbench/archives/`.
It does not install into Wine, a bottle, or a game. `verify` checks the package
layout and binary formats; it does not validate rendering.

## Process-scoped loading

`fsr-hook.command` wraps the command that starts a game. It requires
`WINEPREFIX` and the absolute path to that game's untouched original
FidelityFX DLL. For a game that loads `amd_fidelityfx_loader_dx12.dll`, use:

```sh
WINEPREFIX=/path/to/bottle ./fsr-hook.command \
  --loader loader \
  --original '/path/to/bottle/drive_c/path/to/game/amd_fidelityfx_loader_dx12.dll' \
  --upgrade31 \
  -- /path/to/wine 'C:\path\to\game.exe'
```

For a game that loads `amd_fidelityfx_upscaler_dx12.dll`, use
`--loader upscaler` and point `--original` at that DLL. Omit `--upgrade31`
unless the game requests FSR 3.1 and you explicitly want the experimental
4.0.2 upgrade. The wrapped command must pass its environment to the actual
game process. Merely running `fsr-hook.command` separately does not change an
already running game or Steam process.

To observe a game's existing AMD frame generation in the same process, add
`--fg-observe --fg-original /absolute/path/to/amd_fidelityfx_framegeneration_dx12.dll`.
The FG DLL must be the untouched original inside the selected prefix's
`drive_c`. This option does not enable FG in the game, substitute MetalFX or
RIFE, or advertise a newer AMD FG version.

KCD2 did not load any AMD FG DLL when its original upscaler was selected, even
though the upscaler itself ran. For this kind of game, `--fg-present` adds the
DXGI presentation bridge. It requires `--fg-observe` and the original FG path
above. The native `release/lib/wine/x86_64-windows/dxgi.dll` must first be
installed as `<bottle>/drive_c/windows/system32/dxgi.dll`, after saving that
bottle's original DXGI DLL. This is a one-time bottle installation, not a game
file. The launcher checks that the installed copy matches the local package;
it will not overwrite the bottle automatically. With `--fg-present`, the
launcher selects this proxy only for its wrapped Wine process and temporarily
copies the original D3DMetal DXGI DLL for safe delegation. The bottle file is
still shared by other programs that independently choose native-first DXGI.
For non-WineForge runtimes, pass `--dxgi-original` with the full path to their
original DXGI DLL. CrossOver has not been tested with this path.

The launcher sets `WINEDLLPATH` to the package and a builtin-first override
for the game's requested FidelityFX name. It makes a distinct temporary copy
of the untouched original DLL and adds a path-specific native-first override
under `Z:` so other FidelityFX calls can delegate to it. When FG observation
is enabled, its original DLL is copied the same way. The copies are created
with APFS copy-on-write where available and removed when the wrapped command
exits. The launcher does not alter game files; `--fg-present` does require the
separate bottle-level DXGI installation described above. Wine can otherwise
identify a direct `Z:` reference to the game's original as the same module as
our builtin proxy, causing delegation to loop back into the proxy.

The originals must be under this prefix's `drive_c`, and the game must request
the overridden DLL through its `C:` path. Isolated loader probes validated
these conditions; full game behavior still needs qualification. If the game
requests its DLL through `Z:`, the hook may not engage.

The native dylib is addressed through `METAL_FSR4_LIBRARY`, which the launcher
sets to the full Wine `\??\Z:` path. The PE contains its own `LabFullFrame`
entry, so `METAL_FSR4_HELPER`, `METAL_FSR4_WRAPPER`, and a D3D12 preload are no
longer needed in this package.

## Settings for testers

Only `WINEPREFIX` needs to be set manually. Supply the game's original DLL
with `--original`, choose its name with `--loader`, and add `--upgrade31` only
when testing an FSR 3.1 game with the experimental 4.0.2 upscaler. The launcher
sets the internal Wine overrides, native library path, and provider flags in
the game process. Exporting an enable flag alone cannot load the wrapper.

Start with upscaling only. `--fg-observe` and `--fg-present` are separate
development options; neither implements AMD FG 4.0.1. Diagnostic environment
variables and alternate RIFE/MetalFX paths are not part of the tester setup.

See [TESTING.md](TESTING.md) for a minimal comparison and the log lines that
distinguish a loaded DLL from actual upscaling and generated frames.

## Frame generation

This package can observe a game's original frame-generation provider while
forwarding its calls to AMD. Its optional DXGI presentation bridge can create
an AMD FG 3.1.6 context in KCD2 from the upscaler's motion and depth inputs.
The source candidate now captures a pre-UI image in the cube and KCD2, and a
KCD2 run generated frames with that capture. The player still observed image
artifacts and lower performance, so `--fg-present` is not a qualified KCD2 FG
mode. The local package's DXGI binary predates that source candidate.
It neither advertises nor implements AMD FG 4.0.1. RIFE and MetalFX
frame-generation experiments remain under
`experimental/`; their KCD2 results still include flicker, camera alignment,
and motion artifacts. A higher HUD FPS count alone does not qualify them as
working frame generation.

## Source and license

- `src/runtime/`: FidelityFX provider and Metal network entry points.
- `src/bridge/`: D3D12 resource and command transport.
- `src/shaders/`: Metal shaders.
- `experimental/`: cube, FG transport prototypes and diagnostic tools. These
  are development sources, not supported game modes.

This project is GPL-3.0. AMD binaries, game files, and model assets retain
their upstream terms; see `licenses/` for notices.
