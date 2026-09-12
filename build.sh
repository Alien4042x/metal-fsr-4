#!/bin/sh
# Project-only shell menu; not included in release packages.
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
release_dir="$project_dir/release"
cd "$project_dir"

check_binary() {
    if [ ! -f "$release_dir/$1" ]; then
        printf 'Missing file: %s\n' "$release_dir/$1" >&2
        return 1
    fi
    actual=$(od -An -tx1 -N "$2" "$release_dir/$1" | tr -d ' \n')
    if [ "$actual" != "$3" ]; then
        printf 'Unexpected binary format: %s\n' "$1" >&2
        return 1
    fi
}

verify_release() {
    check_binary lib/metal-fsr4.dylib 4 cffaedfe || return 1
    check_binary lib/wine/x86_64-windows/metal-fsr4.dll 2 4d5a || return 1
    check_binary game/amd_fidelityfx_loader_dx12.dll 2 4d5a || return 1
    for name in dxgi.dll amd_fidelityfx_loader_dx12.dll; do
        if [ -e "$release_dir/lib/wine/x86_64-windows/$name" ]; then
            printf 'Game wrapper must not be in Wine lib: %s\n' "$name" >&2
            return 1
        fi
    done
    printf '%s\n' 'Release layout and binary formats verified. Game execution is not tested.'
}

shell_quote() {
    printf "'"
    printf '%s' "$1" | sed "s/'/'\\\\''/g"
    printf "'"
}

show_commands() {
    native_path=$(printf '%s' "$release_dir/lib/metal-fsr4.dylib" | tr '/' '\\')
    helper_path=$(printf '%s' "$release_dir/lib/wine/x86_64-windows/metal-fsr4.dll" | tr '/' '\\')
    printf '%s\n' 'For an already configured game wrapper, with FG disabled:' \
        'export Enabled_Metal_FSR4=1' 'export Enabled_Metal_FG=0' 'export METAL_FSR4_UPGRADE31=1'
    printf 'export METAL_FSR4_LIBRARY='
    shell_quote "\??\Z:$native_path"
    printf '\nexport METAL_FSR4_HELPER='
    shell_quote "Z:$helper_path"
    printf '\n%s\n' 'Launch your existing Wine game command from the same shell.' \
        'The game wrapper needs its original loader beside it as wf_original_fidelityfx_loader_dx12.dll.' \
        'These commands do not install files. See README.md for installation steps.'
}

run_choice() {
    selected=$1
    shift
    case "$selected" in
        dev)
            action=${1:-build}
            if [ "$#" -gt 0 ]; then shift; fi
            case "$action" in
                build) python3 "$project_dir/tools/local_workbench.py" build_fg_cube "$@" ;;
                run) python3 "$project_dir/tools/local_workbench.py" run_fg_cube "$@" ;;
                *) printf '%s\n' 'Use: ./build.sh dev [build|run] [options]' >&2; return 2 ;;
            esac
            return $?
            ;;
        --fg-cube)
            python3 "$project_dir/tools/local_workbench.py" build_fg_cube "$@"
            return $?
            ;;
    esac
    if [ "$#" -ne 0 ]; then
        printf 'Unexpected arguments for: %s\n' "$selected" >&2
        return 2
    fi
    case "$selected" in
        1|build) python3 "$project_dir/tools/local_workbench.py" build_runtime ;;
        2|verify) verify_release ;;
        3|commands) show_commands ;;
        4|exit|quit) exit 0 ;;
        help|--help|-h)
            printf '%s\n' 'Usage: ./build.sh [build|verify|commands|exit|1|2|3|4]' \
                'Without arguments: open the interactive menu.' \
                './build.sh dev [build|run] [options]  Development cube.'
            ;;
        *) printf 'Unknown option: %s\n' "$selected" >&2; return 2 ;;
    esac
}

if [ "$#" -gt 0 ]; then
    run_choice "$@"
    exit $?
fi
while :; do
    printf '\n'
    printf '%s\n' '1. Build release       (build)' '2. Verify release      (verify)' \
        '3. Show launch commands (commands)' '4. Exit                (exit)'
    printf 'Choose a number or command: '
    if ! IFS= read -r choice; then
        printf '\n'
        exit 0
    fi
    if run_choice "$choice"; then :; else
        printf 'Command failed (%s).\n' "$?" >&2
    fi
done
