#!/bin/sh
# Project-only menu for the experimental process-scoped Wine package.
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
release_dir="$project_dir/release"

check_binary() {
    path="$release_dir/$1"
    [ -f "$path" ] || { printf 'Missing file: %s\n' "$path" >&2; return 1; }
    actual=$(od -An -tx1 -N "$2" "$path" | tr -d ' \n')
    [ "$actual" = "$3" ] || {
        printf 'Unexpected binary format: %s\n' "$path" >&2
        return 1
    }
}

verify_release() {
    check_binary lib/metal-fsr4.dylib 4 cffaedfe || return 1
    check_binary lib/wine/x86_64-windows/amd_fidelityfx_loader_dx12.dll 2 4d5a || return 1
    check_binary lib/wine/x86_64-windows/amd_fidelityfx_upscaler_dx12.dll 2 4d5a || return 1
    check_binary lib/wine/x86_64-windows/amd_fidelityfx_framegeneration_dx12.dll 2 4d5a || return 1
    check_binary lib/wine/x86_64-windows/dxgi.dll 2 4d5a || return 1
    builtin_signature=$(od -An -tx1 -j 64 -N 17 \
        "$release_dir/lib/wine/x86_64-windows/amd_fidelityfx_loader_dx12.dll" | tr -d ' \n')
    [ "$builtin_signature" = 57696e65206275696c74696e20444c4c00 ] || {
        printf '%s\n' 'The FidelityFX PE is missing its Wine builtin signature.' >&2
        return 1
    }
    builtin_signature=$(od -An -tx1 -j 64 -N 17 \
        "$release_dir/lib/wine/x86_64-windows/amd_fidelityfx_framegeneration_dx12.dll" | tr -d ' \n')
    [ "$builtin_signature" = 57696e65206275696c74696e20444c4c00 ] || {
        printf '%s\n' 'The frame-generation PE is missing its Wine builtin signature.' >&2
        return 1
    }
    dxgi_signature=$(od -An -tx1 -j 64 -N 17 \
        "$release_dir/lib/wine/x86_64-windows/dxgi.dll" | tr -d ' \n')
    [ "$dxgi_signature" != 57696e65206275696c74696e20444c4c00 ] || {
        printf '%s\n' 'The DXGI PE must be native for a bottle system32 override.' >&2
        return 1
    }
    [ "$(readlink "$release_dir/lib/wine/x86_64-windows/amd_fidelityfx_upscaler_dx12.dll")" = \
      amd_fidelityfx_loader_dx12.dll ] || {
        printf '%s\n' 'The upscaler basename must link to the single provider PE.' >&2
        return 1
    }
    [ -f "$release_dir/LICENSE" ] || return 1
    [ -f "$release_dir/licenses/THIRD-PARTY-NOTICES.txt" ] || return 1
    if [ -e "$release_dir/game" ] || [ -e "$release_dir/optional-frame-generation" ] ||
       [ -e "$release_dir/lib/wine/x86_64-windows/metal-fsr4.dll" ] ||
       [ -e "$release_dir/lib/wine/x86_64-windows/d3d12.dll" ]; then
        printf '%s\n' 'Unexpected legacy or experimental files in release.' >&2
        return 1
    fi
    printf '%s\n' 'Package layout verified. Wine loading and game behavior are not qualified.'
}

show_status() {
    printf '%s\n' \
        'This is an experimental process-scoped Wine upscaler package.' \
        'One Wine builtin FidelityFX PE owns the native bridge entry.' \
        'The alternate FidelityFX basename is a symlink to that same PE.' \
        'The Metal dylib is a separate runtime input.' \
        'A separate FG PE observes and forwards calls to the original AMD provider.' \
        'An opt-in native DXGI proxy supports games without native FG calls.' \
        'Build and verify do not install into Wine or any game.' \
        'See README.md and TESTING.md.'
}

run_choice() {
    choice=$1
    shift
    if [ "$#" -ne 0 ]; then
        printf 'Unexpected arguments for: %s\n' "$choice" >&2
        return 2
    fi
    case "$choice" in
        1|build)
            sh "$project_dir/experimental/rife/build-clean-release.sh"
            ;;
        2|verify) verify_release ;;
        3|status) show_status ;;
        4|exit|quit) exit 0 ;;
        help|--help|-h)
            printf '%s\n' 'Usage: ./build.sh [build|verify|status|exit]' \
                'Without arguments: open the interactive menu.'
            ;;
        *)
            printf 'Unknown or retired option: %s\n' "$choice" >&2
            return 2
            ;;
    esac
}

if [ "$#" -gt 0 ]; then
    choice=$1
    shift
    run_choice "$choice" "$@"
    exit $?
fi
while :; do
    printf '\n%s\n' '1. Build candidate' '2. Verify package' '3. Show status' '4. Exit'
    printf 'Choose a number or command: '
    if ! IFS= read -r choice; then printf '\n'; exit 0; fi
    if run_choice "$choice"; then :; else
        printf 'Command failed (%s).\n' "$?" >&2
    fi
done
