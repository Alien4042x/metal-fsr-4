# Experiments

`cube/` contains the development scene and supporting AMD SDK files.
Other directories contain isolated compute variants; their presence does not mean
they are enabled in the game runtime.

Build the cube from the project root with `./build.sh dev build`.
Private AMD 4.1.1 binary experiments remain under
`.local/workbench/experiments/4.1.1/`. Binary inputs for older builders are stored
in `.local/binary-inputs/`; compatibility links are excluded from source sharing.
