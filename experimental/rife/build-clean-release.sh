#!/bin/sh
# Assemble the experimental Wine package without copying anything into a game.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd -P)
local_dir="$root/.local/workbench"
archives="$local_dir/archives"
native_input="$local_dir/inputs/metal-fsr4.dylib"
compiler=${MINGW_CXX:-x86_64-w64-mingw32-clang++}
winebuild_tool=${WINEBUILD:-winebuild}

mkdir -p -- "$local_dir/inputs" "$archives"
if [ ! -f "$native_input" ]; then
    if [ ! -f "$root/release/lib/metal-fsr4.dylib" ]; then
        printf '%s\n' 'Missing native Metal dylib. Set up a verified native input first.' >&2
        exit 1
    fi
    cp -p -- "$root/release/lib/metal-fsr4.dylib" "$native_input"
fi
command -v "$compiler" >/dev/null 2>&1 || {
    printf 'Missing MinGW compiler: %s\n' "$compiler" >&2
    exit 1
}
command -v "$winebuild_tool" >/dev/null 2>&1 || {
    printf 'Missing Wine build tool: %s\n' "$winebuild_tool" >&2
    exit 1
}
stage=$(mktemp -d "$local_dir/release-stage-XXXXXX")
trap 'rm -rf -- "$stage"' EXIT HUP INT TERM
mkdir -p -- "$stage/lib/wine/x86_64-windows" "$stage/licenses"
cp -p -- "$native_input" "$stage/lib/metal-fsr4.dylib"
cp -p -- "$root/LICENSE" "$stage/LICENSE"
cp -p -- "$root/licenses/THIRD-PARTY-NOTICES.txt" "$stage/licenses/THIRD-PARTY-NOTICES.txt"

(
    cd "$root"
    "$compiler" -std=c++17 -O2 -Wall -Wextra -Werror -shared \
        -DFSR4_PROVIDER_INLINE_HELPER=1 \
        -static -static-libstdc++ -static-libgcc \
        -I src/runtime -I src/bridge \
        src/runtime/fsr4_ffx_provider.cpp src/bridge/context_native_dll.cpp \
        src/runtime/fsr4_full_metal_dll.cpp \
        -o "$stage/lib/wine/x86_64-windows/amd_fidelityfx_loader_dx12.dll" \
        -ld3d12 -ldxgi -ldxguid
)
# WineForge-Internal: fsr4/process-scoped-provider-builtin-v1.
# WINEDLLPATH accepts this PE only after Wine marks it as a builtin.
"$winebuild_tool" --builtin "$stage/lib/wine/x86_64-windows/amd_fidelityfx_loader_dx12.dll"
# Both observed FidelityFX basenames resolve to the same single PE payload.
ln -s amd_fidelityfx_loader_dx12.dll \
    "$stage/lib/wine/x86_64-windows/amd_fidelityfx_upscaler_dx12.dll"

# WineForge-Internal: fsr4/process-scoped-fg-observer-builtin-v1.
# The FG entry forwards all calls to the game's untouched AMD provider.
(
    cd "$root"
    "$compiler" -std=c++17 -O2 -Wall -Wextra -Werror -shared \
        -static -static-libstdc++ -static-libgcc \
        experimental/rife/game_probe.cpp \
        -o "$stage/lib/wine/x86_64-windows/amd_fidelityfx_framegeneration_dx12.dll" \
        -ld3d12 -ldxgi -ldxguid
)
"$winebuild_tool" --builtin "$stage/lib/wine/x86_64-windows/amd_fidelityfx_framegeneration_dx12.dll"

# WineForge-Internal: fsr4/process-scoped-native-dxgi-v1.
# A bottle system32 native DLL can be selected by dxgi=n,b for one process.
# Do not mark this PE as Wine builtin: that would select Wine's DXGI instead.
(
    cd "$root"
    "$compiler" -std=c++17 -O2 -Wall -Wextra -Werror -shared \
        -static -static-libstdc++ -static-libgcc -DUNICODE -D_UNICODE \
        src/dxgi/proxy.cpp src/dxgi/exports.def \
        -o "$stage/lib/wine/x86_64-windows/dxgi.dll" \
        -ld3d12 -ldxgi -ldxguid -ld3dcompiler
)

backup=''
if [ -e "$root/release" ]; then
    backup=$(mktemp -d "$archives/release-XXXXXX")
    mv -- "$root/release" "$backup/prior-release"
fi
if ! mv -- "$stage" "$root/release"; then
    if [ -n "$backup" ]; then mv -- "$backup/prior-release" "$root/release"; fi
    exit 1
fi
trap - EXIT HUP INT TERM
printf 'Experimental package: %s\n' "$root/release"
if [ -n "$backup" ]; then printf 'Prior package archived: %s\n' "$backup/prior-release"; fi
printf '%s\n' 'No game, Wine runtime, or prefix files were changed.'
