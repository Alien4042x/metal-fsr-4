#!/bin/sh
# Launch one Wine command with the experimental FidelityFX provider override.
set -eu

usage() {
    printf '%s\n' \
        'Usage: WINEPREFIX=/path/to/prefix ./fsr-hook.command --loader loader|upscaler' \
        '       --original /absolute/path/to/game/amd_fidelityfx_*.dll [--upgrade31]' \
        '       [--fg-observe --fg-original /absolute/path/to/game/amd_fidelityfx_framegeneration_dx12.dll]' \
        '       [--fg-present [--dxgi-original /absolute/path/to/runtime/dxgi.dll]]' \
        '       -- wine /path/to/game.exe' >&2
    exit 2
}

loader=''
original=''
upgrade31=0
fg_observe=0
fg_original=''
fg_present=0
dxgi_original=''
while [ "$#" -gt 0 ]; do
    case "$1" in
        --loader) [ "$#" -ge 2 ] || usage; loader=$2; shift 2 ;;
        --original) [ "$#" -ge 2 ] || usage; original=$2; shift 2 ;;
        --upgrade31) upgrade31=1; shift ;;
        --fg-observe) fg_observe=1; shift ;;
        --fg-original) [ "$#" -ge 2 ] || usage; fg_original=$2; shift 2 ;;
        --fg-present) fg_present=1; shift ;;
        --dxgi-original) [ "$#" -ge 2 ] || usage; dxgi_original=$2; shift 2 ;;
        --) shift; break ;;
        *) usage ;;
    esac
done
[ "$#" -gt 0 ] && [ -n "${WINEPREFIX:-}" ] && [ -n "$original" ] || usage
if [ "$fg_observe" -eq 1 ]; then
    [ -n "$fg_original" ] || usage
else
    [ -z "$fg_original" ] || usage
fi
[ "$fg_present" -eq 0 ] || [ "$fg_observe" -eq 1 ] || usage
[ "$fg_present" -eq 1 ] || [ -z "$dxgi_original" ] || usage

case "$loader" in
    loader) loader=amd_fidelityfx_loader_dx12.dll ;;
    upscaler) loader=amd_fidelityfx_upscaler_dx12.dll ;;
    *) usage ;;
esac
case "$original" in /*) ;; *) usage ;; esac
[ -f "$original" ] || { printf 'Original DLL is missing: %s\n' "$original" >&2; exit 1; }
[ "$(basename "$original")" = "$loader" ] || {
    printf 'Original DLL must be named %s\n' "$loader" >&2
    exit 1
}

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
prefix_dir=$(CDPATH= cd -- "$WINEPREFIX" && pwd -P)
original_dir=$(CDPATH= cd -- "$(dirname -- "$original")" && pwd -P)
original_path="$original_dir/$loader"
case "$original_path" in
    "$prefix_dir"/drive_c/*) ;;
    *) printf '%s\n' 'Original DLL must be inside this prefix drive_c for the C:/Z: override pair.' >&2; exit 1 ;;
esac
if [ "$fg_observe" -eq 1 ]; then
    case "$fg_original" in /*) ;; *) usage ;; esac
    fg_name=amd_fidelityfx_framegeneration_dx12.dll
    [ -f "$fg_original" ] && [ "$(basename "$fg_original")" = "$fg_name" ] || {
        printf 'Original FG DLL is missing or not named %s\n' "$fg_name" >&2
        exit 1
    }
    fg_original_dir=$(CDPATH= cd -- "$(dirname -- "$fg_original")" && pwd -P)
    fg_original_path="$fg_original_dir/$fg_name"
    case "$fg_original_path" in
        "$prefix_dir"/drive_c/*) ;;
        *) printf '%s\n' 'Original FG DLL must be inside this prefix drive_c.' >&2; exit 1 ;;
    esac
fi
if [ "$fg_present" -eq 1 ]; then
    dxgi_package="$project_dir/release/lib/wine/x86_64-windows/dxgi.dll"
    dxgi_installed="$prefix_dir/drive_c/windows/system32/dxgi.dll"
    [ -f "$dxgi_package" ] || {
        printf '%s\n' 'Missing native DXGI proxy. Build the local package first.' >&2
        exit 1
    }
    if ! cmp -s "$dxgi_package" "$dxgi_installed"; then
        printf 'Install %s in this bottle as %s before using --fg-present.\n' \
            "$dxgi_package" "$dxgi_installed" >&2
        exit 1
    fi
    if [ -z "$dxgi_original" ]; then
        [ -n "${D3DMETAL_RUNTIME_DIR:-}" ] || {
            printf '%s\n' 'Set D3DMETAL_RUNTIME_DIR or pass --dxgi-original.' >&2
            exit 1
        }
        dxgi_original="$D3DMETAL_RUNTIME_DIR/wine/x86_64-windows/dxgi.dll"
    fi
    case "$dxgi_original" in /*) ;; *) usage ;; esac
    [ -f "$dxgi_original" ] && ! cmp -s "$dxgi_package" "$dxgi_original" || {
        printf '%s\n' 'The original DXGI backend is missing or points back to the proxy.' >&2
        exit 1
    }
    case ";${WINEDLLOVERRIDES:-};" in
        *';dxgi=n,b;'*) dxgi_override_needed=0 ;;
        *dxgi*) printf '%s\n' 'Use dxgi=n,b for this process when --fg-present is enabled.' >&2; exit 1 ;;
        *) dxgi_override_needed=1 ;;
    esac
fi

wine_dll_dir="$project_dir/release/lib/wine/x86_64-windows"
native_dylib="$project_dir/release/lib/metal-fsr4.dylib"
[ -f "$wine_dll_dir/$loader" ] && [ -f "$native_dylib" ] || {
    printf '%s\n' 'Build and verify the local package first: ./build.sh build && ./build.sh verify' >&2
    exit 1
}
if [ "$fg_observe" -eq 1 ] && [ ! -f "$wine_dll_dir/$fg_name" ]; then
    printf '%s\n' 'Missing forward-only FG observer. Build the local package first.' >&2
    exit 1
fi

to_z_path() {
    printf 'Z:%s' "$(printf '%s' "$1" | sed 's,/,\\,g')"
}
clone_original() {
    if ! cp -c "$1" "$2" 2>/dev/null; then cp -p "$1" "$2"; fi
}
# WineForge-Internal: fsr4/distinct-original-file-identity-v1.
# Wine associates a selected builtin with the game's original file identity.
# A distinct temporary copy is required for native delegation in this process.
cache_parent=${TMPDIR:-/private/tmp}
cache_parent=${cache_parent%/}
cache_dir=$(mktemp -d "$cache_parent/metal-fsr4-original-XXXXXXXX")
trap 'rm -rf -- "$cache_dir"' EXIT HUP INT TERM
clone_original "$original_path" "$cache_dir/$loader"
if [ "$loader" = amd_fidelityfx_loader_dx12.dll ] &&
   [ -f "$original_dir/amd_fidelityfx_upscaler_dx12.dll" ]; then
    clone_original "$original_dir/amd_fidelityfx_upscaler_dx12.dll" \
        "$cache_dir/amd_fidelityfx_upscaler_dx12.dll"
fi
if [ "$fg_observe" -eq 1 ]; then
    clone_original "$fg_original_path" "$cache_dir/$fg_name"
fi
if [ "$fg_present" -eq 1 ]; then
    clone_original "$dxgi_original" "$cache_dir/dxgi.dll"
fi

original_win=$(to_z_path "$cache_dir/$loader")
dylib_win=$(to_z_path "$native_dylib")
original_override=${original_win%.dll}
loader_override=${loader%.dll}
case "$original_override" in *';'*|*'='*) usage ;; esac
case ";${WINEDLLOVERRIDES:-};" in
    *";$loader_override="*)
        printf 'Remove the existing %s override before launching.\n' "$loader_override" >&2
        exit 1 ;;
esac
if [ "$fg_observe" -eq 1 ]; then
    case ";${WINEDLLOVERRIDES:-};" in
        *';amd_fidelityfx_framegeneration_dx12='*)
            printf '%s\n' 'Remove the existing frame-generation override before launching.' >&2
            exit 1 ;;
    esac
    fg_original_win=$(to_z_path "$cache_dir/$fg_name")
    fg_original_override=${fg_original_win%.dll}
    case "$fg_original_override" in *';'*|*'='*) usage ;; esac
fi
if [ "$fg_present" -eq 1 ]; then
    dxgi_original_win=$(to_z_path "$cache_dir/dxgi.dll")
    dxgi_original_override=${dxgi_original_win%.dll}
    case "$dxgi_original_override" in *';'*|*'='*) usage ;; esac
fi

# WineForge-Internal: fsr4/process-scoped-provider-activation-v1.
# C: basename selects our builtin PE; the explicit Z: path selects the
# distinct native copy for forwarding other FidelityFX calls.
WINEDLLPATH="$project_dir/release/lib/wine${WINEDLLPATH:+:$WINEDLLPATH}"
requested_overrides="$loader_override=b,n;$original_override=n,b"
if [ "$fg_observe" -eq 1 ]; then
    requested_overrides="$requested_overrides;amd_fidelityfx_framegeneration_dx12=b,n;$fg_original_override=n,b"
    METAL_FSR4_ORIGINAL_FG=$fg_original_win
    export METAL_FSR4_ORIGINAL_FG
else
    unset METAL_FSR4_ORIGINAL_FG
fi
if [ "$fg_present" -eq 1 ]; then
    [ "$dxgi_override_needed" -eq 0 ] || requested_overrides="$requested_overrides;dxgi=n,b"
    requested_overrides="$requested_overrides;$dxgi_original_override=n,b"
    METAL_FG_DXGI_BACKEND=$dxgi_original_win
    METAL_FSR4_FG=amd
    Enabled_Metal_FG=1
    METAL_FG_CAPTURE_HUDLESS=1
    METAL_FG_REQUIRE_EARLY_HUDLESS=1
    export METAL_FG_DXGI_BACKEND METAL_FSR4_FG Enabled_Metal_FG
    export METAL_FG_CAPTURE_HUDLESS METAL_FG_REQUIRE_EARLY_HUDLESS
else
    unset METAL_FG_DXGI_BACKEND Enabled_Metal_FG
    unset METAL_FG_CAPTURE_HUDLESS METAL_FG_REQUIRE_EARLY_HUDLESS
fi
WINEDLLOVERRIDES="$requested_overrides${WINEDLLOVERRIDES:+;$WINEDLLOVERRIDES}"
METAL_FSR4_ORIGINAL_LOADER=$original_win
METAL_FSR4_LIBRARY="\\??\\$dylib_win"
METAL_FSR4_UPGRADE31=$upgrade31
Enabled_Metal_FSR4=1
export WINEDLLPATH WINEDLLOVERRIDES METAL_FSR4_ORIGINAL_LOADER
export METAL_FSR4_LIBRARY METAL_FSR4_UPGRADE31 Enabled_Metal_FSR4
unset METAL_FSR4_HELPER METAL_FSR4_WRAPPER METAL_FSR4_D3D12_BACKEND
"$@"
