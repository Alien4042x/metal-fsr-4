# Metal FSR 4

An experimental Metal implementation of the FSR 4.0.2 neural upscaler, connected
to D3D12 applications through a FidelityFX wrapper. The neural network executes
with Metal shaders and model weights. This path does not use MetalFX as its upscaler.

Frame generation uses AMD's original 3.1.6 provider and the 3.1.7 interpolation
swapchain from FidelityFX SDK 2.3.0. The wrapper supplies frame resources and
coordinates presentation. An experimental HUD-less capture path protects UI
from interpolation and uses two buffers for asynchronous frame generation.

## Project layout

| Directory | Contents |
| --- | --- |
| `src/runtime/` | FidelityFX provider, Metal inference and temporal reconstruction |
| `src/bridge/` | D3D12 resource transfers, command forwarding and synchronization |
| `src/dxgi/` | DXGI forwarding, AMD frame generation and HUD-less capture |
| `src/shaders/` | Metal shaders copied from the last tested game candidate |
| `src/vendor/` | Pinned AMD SDK headers and dependency hashes |
| `experimental/` | Development cube and isolated compute experiments |
| `licenses/` | Consolidated third-party license notices |
| `tools/` | Entry point for local build and test tools |

Private dependencies, game DLLs, model data, results and working notes are stored
under `.local/`. They are not part of the source material intended for sharing.

## Building

The current build uses macOS, Xcode's command-line tools, Python 3 and LLVM-MinGW.
Toolchain locations and test inputs still depend on the local development setup;
this is not yet a standalone build from a fresh checkout.

Run from the project directory:

```sh
./build.sh build
```

This builds the runtime without the development cube into the ignored local
`release/` directory:

```text
lib/
├── metal-fsr4.dylib
└── wine/x86_64-windows/
    └── metal-fsr4.dll
game/
└── amd_fidelityfx_loader_dx12.dll
optional-frame-generation/
├── amd_fidelityfx_framegeneration_dx12.dll
├── dxgi.dll
└── README.txt
LICENSE
licenses/
```

The `optional-frame-generation/` directory is a separate per-application integration.
Never overwrite Wine's original `dxgi.dll` with this proxy. It needs application
DLL overrides and an explicit path to the original DXGI backend; copying the
whole release into Wine does not configure FG. Leave `Enabled_Metal_FG=0` until
that integration is configured.

The package contains no Python scripts or installer. Copy the main `lib/` libraries into the
corresponding Wine runtime locations as appropriate for your integration. PE
placement, DLL overrides and the path used to load the native library still need
to match that runtime; Wine does not automatically load an arbitrary dylib merely
because it is in `lib/`. Building does not install files into Wine or a game.

A new build is an unqualified candidate. The current full builder still takes
shader and weight inputs from its pinned private baseline; it does not yet build
directly from `src/shaders/`. Do not assume a fresh build reproduces the last tested
game candidate or its performance.

## Game wrapper

`game/amd_fidelityfx_loader_dx12.dll` replaces the game's FidelityFX loader;
it is not a Wine system DLL. The current wrapper also requires the game's own
original loader beside it as `wf_original_fidelityfx_loader_dx12.dll`. That file
comes from the game installation and is deliberately not distributed. Do not
replace the game loader unless its original is retained under the required name.
The AMD frame generation DLL is a separate provider, not that original loader.

`lib/wine/x86_64-windows/metal-fsr4.dll` is the PE bridge to the native dylib.
Neither it nor the dylib is automatically enabled by copying it into Wine.

Run `./build.sh` in the project for the English shell menu. Enter `1`–`4` or
`build`, `verify`, `commands`, `exit`. Commands also work directly:

```sh
./build.sh build
./build.sh verify
./build.sh commands
```

The menu itself uses POSIX shell, not Python, and is not included in release
packages. The existing build tooling still requires Python 3.

## Manual installation (upscaling only)

| File | Placement and purpose |
| --- | --- |
| `release/lib/metal-fsr4.dylib` | Native Metal implementation with embedded shaders and weights. Keep here or copy to `<wine-runtime>/lib/`; `METAL_FSR4_LIBRARY` must point to its actual location. |
| `release/lib/wine/x86_64-windows/metal-fsr4.dll` | Windows-to-native bridge. Keep here or copy to `<wine-runtime>/lib/wine/x86_64-windows/`; `METAL_FSR4_HELPER` must point to its actual location. |
| `release/game/amd_fidelityfx_loader_dx12.dll` | Our game wrapper. Copy beside the game's original loader after the rename described below. Do not install as a global Wine loader. |
| `wf_original_fidelityfx_loader_dx12.dll` | The game's own original loader after renaming; keep beside our wrapper. Not supplied in release. |
| `release/optional-frame-generation/` | Separate experimental FG integration. Not required for upscaling alone; do not merge its DXGI proxy into Wine's libraries. |

`<wine-runtime>` means the Wine engine installation, not a game's Wine prefix or
its `drive_c/windows/system32` directory. Runtime layouts can differ: the explicit
library paths determine what is loaded. Copying files does not enable them.


Building creates `release/`; it does not install into a game, rename the original
loader or configure Wine. For a game using `amd_fidelityfx_loader_dx12.dll`:

1. Close the game. Find its existing `amd_fidelityfx_loader_dx12.dll` in the game
   installation, not in the Wine runtime.
2. On the first installation, rename that original game file to
   `wf_original_fidelityfx_loader_dx12.dll` in the same directory. It is both the
   preserved original and an active dependency: the wrapper loads it at runtime.
   Do not use the AMD frame generation DLL in its place.
3. Copy `release/game/amd_fidelityfx_loader_dx12.dll` into that game directory,
   alongside the renamed original.
4. Keep `lib/metal-fsr4.dylib` and
   `lib/wine/x86_64-windows/metal-fsr4.dll` in the release directory, or place them
   in your chosen Wine runtime. Set the paths below to their actual locations.
5. Apply the environment settings in the same shell or launcher that starts the
   game. Keep FG disabled for this installation. Use your existing working
   Wine/GPTK game launch command and prefix.

For an update, replace only the previously installed wrapper with the new wrapper.
Never rename our wrapper to `wf_original_fidelityfx_loader_dx12.dll` or overwrite
a retained original with it. If the original is missing or its identity is unclear,
restore the game's original loader from a verified game installation first. After
a game update, ensure the retained original matches that game version.

Example paths below assume the release is at `/path/to/release` and Wine's `Z:`
drive maps the macOS filesystem. Replace those paths before use:

```sh
export Enabled_Metal_FSR4=1
export Enabled_Metal_FG=0
export METAL_FSR4_UPGRADE31=1
export METAL_FSR4_LIBRARY='\??\Z:\path\to\release\lib\metal-fsr4.dylib'
export METAL_FSR4_HELPER='Z:\path\to\release\lib\wine\x86_64-windows\metal-fsr4.dll'
```

Then run your usual game launch command from that shell. The project menu's
**Show launch commands** option prints paths for the current local release.
`METAL_FSR4_LIBRARY` identifies the native dylib, using the `\??\Z:` path
form expected by the bridge. `METAL_FSR4_HELPER` identifies the PE bridge DLL,
using a `Z:` Windows path. The menu derives these from the project location;
they are not hard-coded to a particular account. Once copied into your launch
settings, they are absolute paths: update them if you move either library.
These settings do not install the wrapper or select a Wine prefix themselves.
If your launcher requires DLL overrides to load the game-local wrapper, configure
those for this application only, preserving its existing graphics backend settings.

Do not copy `optional-frame-generation/dxgi.dll` over Wine's original DXGI.
FG needs separate presentation integration; setting `Enabled_Metal_FG=1` alone
does not configure it. This manual procedure enables only the upscaling path and
does not establish game compatibility or qualify a newly built release.

To remove the game wrapper, close the game, remove our installed
`amd_fidelityfx_loader_dx12.dll`, and rename the retained original back to
`amd_fidelityfx_loader_dx12.dll`. Remove the launch settings as well.

## Runtime switches

```sh
export Enabled_Metal_FSR4=1
export Enabled_Metal_FG=0
```

These switches enable the corresponding features in the installed wrapper.
They do not install or locate the required DLLs by themselves. Local test launchers
supply the library paths and backend configuration.

| Variable | Purpose |
| --- | --- |
| `Enabled_Metal_FSR4` | Enable the Metal FSR 4 upscaler (`1` or `0`) |
| `Enabled_Metal_FG` | Enable AMD frame generation (`1` or `0`) |
| `METAL_FSR4_UPGRADE31` | Allow the wrapper to replace supported FSR 3.1 calls |
| `METAL_FSR4_ASSET_ROOT` | Absolute directory containing `shaders/` and `weights/` |
| `METAL_FSR4_LIBRARY` | Native library path supplied by the local launcher |
| `METAL_FSR4_HELPER` | Windows helper DLL path supplied by the local launcher |
| `METAL_FG_DXGI_BACKEND` | Original DXGI backend path supplied by the local launcher |
| `METAL_FG_CAPTURE_HUDLESS` | Enable the experimental double-buffered UI protection |
| `METAL_FSR4_TRACE` | Enable provider diagnostics |
| `METAL_FG_UI_TRACE_FRAME` | Record one selected post-upscale command sequence |

Names are case-sensitive. Previous environment names have been removed without
aliases; rebuild the wrapper before using these settings with an older package.

## Where Metal data is stored

The release dylib contains the shader sources and model weights. When the game
first uses the enabled Metal FSR4 path, the native library extracts them into:

```text
~/Library/Application Support/Metal FSR4/shared/<content-hash>/4.0.2/
├── shaders/
└── weights/
```

No installer or Python is needed on the player's machine. Merely starting Wine
without loading the FSR4 wrapper does not trigger extraction. Enable the installed
wrapper with `Enabled_Metal_FSR4=1` in the game's environment.

The directory name is the SHA-256 of the embedded asset payload. Matching files
are reused; missing or changed files are restored atomically on the next library
initialization. Different asset versions use separate directories. These are model
inputs and shader sources, not Apple's compiled shader cache. Unused versions can
be deleted after games using them have closed; the next use recreates them.

`METAL_FSR4_ASSET_ROOT` remains an explicit override for development. It must point
to an existing absolute directory containing `shaders/` and `weights/` and bypasses
automatic extraction. Older builds without embedded assets retain their relative
`../4.0.2/` lookup and must be rebuilt for the standalone release layout.

## Development cube

```sh
./build.sh dev build
./build.sh dev run
```

The cube is a compiled Windows executable run through Wine/GPTK. Python is used
by the local build and launch tools to prepare the test, supply paths/environment,
collect logs and manage the run; it does not execute the neural network. The network
runs in the native Metal library. You invoke these tools through `build.sh`; Python
3 is still a local development dependency, not a file to copy into the game.

The cube is a separate development tool, not part of the runtime distribution.
`dev build` prints a new fixture path; `dev run` uses the selected fixture unless
an explicit `--fixture` is supplied. Building alone does not select a new fixture.
The currently selected cube inputs belong to an older private candidate, not the
current release. Launcher argument handling and selected manifest hashes were
checked after the shell-menu change; a fresh cube execution against the current
release has not been validated. A successful `dev run` must not be treated as a
test of a different release package.
Game compatibility and image quality still require testing in the actual game.

## Current scope

FSR 4.0.2 inference and AMD frame generation have run together in KCD2. The latest
user-tested HUD capture variant produced readable UI at approximately 45 FPS in
that scene. This was a local observation, not a controlled performance benchmark
or a guarantee for other games. FSR 4.1.1 remains a separate private experiment.

Scaling ratios and trained model presets are different settings. Support for all
five game-menu scaling modes is not yet a completed validation target.

## Upstream resources

- [AMD FidelityFX SDK 2.3.0](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v2.3.0)
- [AMD FSR SDK documentation](https://gpuopen.com/manuals/fsr_sdk/)
- [AMD Frame Generation API](https://gpuopen.com/manuals/fsr_sdk/techniques/frame-interpolation-api/)
- [AMD Frame Generation swapchain and UI composition](https://gpuopen.com/manuals/fsr_sdk/techniques/frame-interpolation-swap-chain/)

## License

The project's original implementation is licensed under the
**GNU General Public License, version 3** (`GPL-3.0-only`). See [LICENSE](LICENSE).
The license text matches the file in the
[project repository](https://github.com/Alien4042x/metal-fsr-4/blob/main/LICENSE).

Third-party components retain their respective license terms and copyright
notices. These are collected in
[THIRD-PARTY-NOTICES.txt](licenses/THIRD-PARTY-NOTICES.txt).
The project license does not relicense AMD binaries, model weights or game files.
Links to upstream projects do not replace their license requirements.
