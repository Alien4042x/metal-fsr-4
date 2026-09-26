# Metal FSR 4

An experimental Metal implementation of the FSR 4.0.2 neural upscaler with a
FidelityFX/D3D12 bridge for Wine. The neural network executes with Metal shaders
and model weights; MetalFX is not the upscaler in this path.

This repository is a **source workbench**, not a ready-to-install game release.
The current native dylib is a pinned local build, and the native source has
changed since it was made. A fresh checkout cannot yet reproduce the full
package without that separately verified native input. Image quality,
performance and stability still need game-level validation.

## Code layout

- `src/runtime/`: FidelityFX provider, model selection and Metal inference.
- `src/bridge/`: D3D12 resource transfer, command forwarding and synchronization.
- `src/dxgi/`: experimental presentation and frame-generation bridge.
- `src/shaders/`: Metal shader sources.
- `src/vendor/`: AMD SDK 2.3.0 Git submodule, a compatibility link for existing
  include paths, and Streamline API headers. Their notices are in
  `licenses/THIRD-PARTY-NOTICES.txt`.
- `experimental/`: cube and isolated development implementations. Inclusion of
  their transport headers does not make them supported game modes.

Model assets, AMD/game binaries, logs, captured frames and local release packages
are excluded from Git.

## Local build

`./build.sh` opens the shell menu. Its commands are `build`, `verify` and
`status`. Building requires macOS, a verified local native dylib input, an
LLVM-MinGW compiler (`MINGW_CXX`) and Wine's `winebuild` (`WINEBUILD`). It
assembles an ignored `release/` directory; it does not install anything into
Wine, a bottle or a game.

Initialize the pinned SDK submodule after cloning or before building:

```sh
git submodule update --init src/vendor/FidelityFX-SDK
```

The `v2.3.0` commit is fixed by this repository. GitHub displays the SDK as a
linked repository; the original API-header hashes are recorded in
`src/vendor/fsr-sdk-2.3.0-lock.json`. The compatibility link
`src/vendor/fsr-sdk-2.3.0` points into its `Kits/FidelityFX` directory.

```sh
./build.sh build
./build.sh verify
```

The package contains `lib/metal-fsr4.dylib`, Wine PE implementations of the
FidelityFX loader/upscaler names, an FG forwarding PE, and an optional DXGI
proxy. `verify` checks package layout and binary formats, not rendering. A
successfully built PE or a loaded DLL alone does not prove that a game executed
the Metal network or displayed generated frames.

## Configuration boundary

For a game that loads the FidelityFX DLL by its normal basename, point Wine
at the built package and set the upscaler switches for the **game process**:

```sh
package=/absolute/path/to/fsr-workbench/release
env WINEDLLPATH="$package/lib/wine${WINEDLLPATH:+:$WINEDLLPATH}" \
  WINEDLLOVERRIDES="amd_fidelityfx_loader_dx12=b,n;amd_fidelityfx_upscaler_dx12=b,n${WINEDLLOVERRIDES:+;$WINEDLLOVERRIDES}" \
  Enabled_Metal_FSR4=1 METAL_FSR4_UPGRADE31=1 \
  METAL_FSR4_LIBRARY='\??\Z:\absolute\path\to\fsr-workbench\release\lib\metal-fsr4.dylib' \
  METAL_FSR4_ORIGINAL_LOADER='C:\absolute\path\to\the\game\original\amd_fidelityfx_loader_dx12.dll' \
  "$WINE" "$GAME"
```

`Enabled_Metal_FSR4=1` opts into the Metal 4.0.2 upscaler.
`METAL_FSR4_UPGRADE31=1` additionally allows a game that requests FSR 3.1 to
select it; omit this switch when the game already requests 4.0.2. Both default
to off. Set `WINE` and `GAME` to the chosen Wine executable and game path, and
replace the example package and Windows paths with real ones.
`METAL_FSR4_LIBRARY` identifies the native dylib, while
`METAL_FSR4_ORIGINAL_LOADER` identifies the game's untouched AMD DLL for
delegation. The latter must point to the original DLL, not back to this
wrapper. The release provider contains its own native PE entry, so it does not
need `METAL_FSR4_HELPER`.

`WINEDLLPATH` and `WINEDLLOVERRIDES` make Wine prefer this package's builtin
FidelityFX PE for those two DLL names. This selection passed an isolated Wine
loader probe, but a game that loads its DLL by an explicit path or before this
override takes effect still needs its own integration. This repository has no
qualified one-line game installer. `METAL_FSR4_ASSET_ROOT` is unnecessary for
the packaged dylib because it extracts its embedded shaders and weights on
first use. Keep the paths and overrides scoped to the game process; do not
replace the runtime's DXGI globally.

Frame generation is a separate experiment. The optional bridge can forward a
game's AMD FG calls and has created an AMD **3.1.6** context in KCD2. A KCD2
run generated frames but also showed artifacts on moving objects and lower
performance. This is not AMD FG 4.0.1 support, and RIFE/MetalFX experiments are
not qualified alternatives. `Enabled_Metal_FG=1` only opts into that separate
bridge when its DXGI path is installed for the same game process; it does not
make an unsupported game expose FG or select AMD FG 4.0.1. Tracing and capture
variables in the source are internal diagnostic controls, not required launch
settings.

## Upstream references

The SDK submodule points to
[AMD FidelityFX SDK 2.3.0](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v2.3.0).
AMD's [FSR SDK documentation](https://gpuopen.com/manuals/fsr_sdk/) describes
the upstream techniques. For the separate FG work, see the
[Frame Generation API](https://gpuopen.com/manuals/fsr_sdk/techniques/frame-interpolation-api/)
and [swapchain and UI composition](https://gpuopen.com/manuals/fsr_sdk/techniques/frame-interpolation-swap-chain/).
These links document AMD's SDK; they do not imply that this project implements
every technique or provider it contains.

## License

The project's original implementation is GPL-3.0-only; see [LICENSE](LICENSE).
Third-party sources retain their upstream notices in
[THIRD-PARTY-NOTICES.txt](licenses/THIRD-PARTY-NOTICES.txt). No AMD binaries,
game files or model assets are distributed here.
