# Experiments

The development cube and isolated compute tests live here. Some small FG
transport headers in `rife/`, `metalfx-fg/`, and `streamline/` are also included
by the current DXGI source. That inclusion does not make those interpolation
backends qualified game modes.

The full FidelityFX SDK checkout, AMD binaries, model data, generated packages,
game logs and captured frames remain local inputs outside Git. The repository
contains a pinned API-header snapshot under `src/vendor/fsr-sdk-2.3.0/`.

The current package builder uses a separately verified native dylib and does
not reproduce it from this checkout. See the root README and TESTING.md before
using a candidate in a game.
