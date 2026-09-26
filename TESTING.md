# Testing the local Metal FSR 4 candidate

This describes a development candidate, not a game-qualified release. The
upscaler is the Metal FSR 4.0.2 network. The optional game FG bridge currently
uses AMD's 3.1.6 provider; it is not AMD FG 4.0.1. The latest KCD2 run loaded
both paths and generated frames, but the player saw artifacts on moving 3D
objects and lower FPS. Do not report a successful hook as a successful image
or performance test.

## First comparison: upscaling only

Build and verify the local package, then run the existing game command through
`fsr-hook.command`. Keep the game files untouched. The Wine prefix, Wine
executable, original FidelityFX DLL and game executable must refer to the
same installation. Example for a game that loads the `loader` DLL:

```sh
./build.sh build
./build.sh verify
export WINEPREFIX=/absolute/path/to/bottle
./fsr-hook.command \
  --loader loader \
  --original "$WINEPREFIX/drive_c/path/to/game/amd_fidelityfx_loader_dx12.dll" \
  --upgrade31 \
  -- /absolute/path/to/wine 'C:\path\to\game.exe' >metal-fsr4-test.log 2>&1
```

Use `--loader upscaler` and the matching original DLL if that is what the
game loads. Omit `--upgrade31` when the game already requests FSR 4. The
launcher sets the required environment variables in the game process;
manually exporting `Enabled_Metal_FSR4` is not a substitute for selecting the
wrapper. A build currently retains a pinned local native dylib; it does not
rebuild the latest native source. Record its SHA-256 when comparing results.

Test one scene at the same output resolution, graphics settings and upscaling
profile with the stock game and the wrapped game. Keep the game's FG setting
off for this first comparison. Note FPS and frame pacing, but also inspect
moving models, distant thin edges, text and the menu. A profile change should
be reported separately from a startup result.

Useful evidence in the log:

| Line | What it establishes |
| --- | --- |
| `FSR4_ADMISSION` | The wrapper received a FidelityFX context request. |
| `DLL_NATIVE_READY` | The Metal transport initialized. |
| `NATIVE_MODEL_PRESET` | The model variant and render/output size chosen for a frame. |
| `DLL_NATIVE_FRAME` | A native Metal frame completed. |
| `FG_GAME_CONTEXT provider=3.1.6` | The optional DXGI bridge created AMD FG 3.1.6, not 4.0.1. |
| `FG_HUDLESS_CAPTURE` | The bridge captured a scene image before a UI write. |
| `FG_CALLBACK_DISPATCH` or a positive `generated_callback_delta` | FG generated callbacks; neither proves correct image quality or displayed FPS. |

The `--fg-observe` option only observes and forwards a game's existing AMD FG
calls. The `--fg-present` option requires a separate bottle-level DXGI proxy
installation and is still under image qualification. It should remain an
internal comparison, not a default tester instruction. RIFE and MetalFX FG
experiments are also not qualified tester modes.

When sharing a result, include the game and Wine runtime, package/dylib hash,
output and render resolution, profile, FG on/off, one short description of the
scene and motion, baseline/candidate FPS, and whether the problem appears in
the real frame or only in a generated frame. Attach the relevant short log
excerpt rather than an unfiltered trace; inspect it for personal filesystem
paths before posting it publicly.
