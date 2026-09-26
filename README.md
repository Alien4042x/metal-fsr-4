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
- `src/vendor/`: pinned AMD SDK and Streamline API headers. Their notices are in
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

The intended public controls are **enable the Metal upscaler** and, for games
requesting FSR 3.1, **opt in to the 4.0.2 upgrade**. The source currently names
these switches `Enabled_Metal_FSR4` and `METAL_FSR4_UPGRADE31`. Wine DLL
selection, original-provider delegation and the native-library path are
integration details. Merely setting either switch does not load the provider.
There is no supported one-line game installer or launcher in this repository
yet.

Frame generation is a separate experiment. The optional bridge can forward a
game's AMD FG calls and has created an AMD **3.1.6** context in KCD2. A KCD2
run generated frames but also showed artifacts on moving objects and lower
performance. This is not AMD FG 4.0.1 support, and RIFE/MetalFX experiments are
not qualified alternatives. The experimental DXGI proxy must not replace a
Wine runtime's original `dxgi.dll` globally.

## License

The project's own code is GPL-3.0; see `LICENSE`. Third-party sources retain
their upstream notices in `licenses/THIRD-PARTY-NOTICES.txt`. No AMD binaries,
game files or model assets are distributed here.
