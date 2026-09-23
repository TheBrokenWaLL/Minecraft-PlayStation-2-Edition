#!/usr/bin/env bash
# Local equivalent of the platform configure/build steps in .github/workflows/build.yml.
set -Eeuo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
usage() {
    cat <<'EOF'
Usage: bash build_linux.sh [gcc|wii|ps2] [options] [-- -DCMAKE_OPTION=value ...]
  -debug, --debug    Debug instead of Release
  -release          Explicit Release configuration
  --jobs N           Parallel jobs (default JOBS or 4)
  --configure-only   Check dependencies and configure, without compiling/staging
  --no-assets        Compile only (useful for CI or a source-only checkout)
  --dry-run          Print commands without changing files or requiring SDKs
  -h, --help         Show help

No target: interactive menu. gcc means native Linux GCC, NOT Windows MinGW.
Run inside your toolchain environment (for example ArchLinux Distrobox).
No packages are installed, SDKs downloaded, or source files patched automatically.
EOF
}
fail() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
requested_config=
target= config=Release dry=0 configure_only=0 assets=1
parallel="${JOBS:-4}"
extra=()
while (($#)); do
    case "$1" in
        gcc|wii|ps2) [[ -z "$target" ]] || fail 'Specify only one target'; target="$1" ;;
        -debug|--debug|-release|--release)
            next_config=Release
            [[ "$1" != *debug ]] || next_config=Debug
            [[ -z "$requested_config" || "$requested_config" == "$next_config" ]] || fail 'Choose debug or release, not both'
            requested_config="$next_config"; config="$next_config" ;;
        -jobs|--jobs) (($# >= 2)) || fail '--jobs requires a positive integer'; parallel="$2"; shift ;;
        -configure-only|--configure-only) configure_only=1 ;;
        -no-assets|--no-assets) assets=0 ;;
        -dry-run|--dry-run) dry=1 ;;
        -h|-help|--help) usage; exit 0 ;;
        --) shift; extra=("$@"); break ;;
        *) fail "Unknown option: $1 (see --help)" ;;
    esac
    shift
done
[[ "$parallel" =~ ^[1-9][0-9]*$ ]] || fail 'Jobs must be a positive integer'
if [[ -z "$target" ]]; then
    [[ -t 0 ]] || fail 'Specify gcc, wii, or ps2 in non-interactive use'
    printf '1) Linux GCC\n2) Nintendo Wii (full game)\n3) PlayStation 2\n'
    read -r -p 'Build target: ' selection
    case "$selection" in 1) target=gcc ;; 2) target=wii ;; 3) target=ps2 ;; *) fail 'Invalid target' ;; esac
fi
run() {
    printf '+'; printf ' %q' "$@"; printf '\n'
    if (( ! dry )); then "$@"; fi
}
need() { (( dry )) || command -v "$1" >/dev/null 2>&1 || fail "Missing $1. Install the required build dependencies."; }
file_needed() { (( dry )) || [[ -f "$1" ]] || fail "Missing $1. Install the required dependencies and provide the game assets."; }
need cmake
need ninja
build="$ROOT/build/$target-${config,,}"
args=(-S "$ROOT" -B "$build" -G Ninja -DCMAKE_MAKE_PROGRAM=ninja -DCMAKE_BUILD_TYPE="$config")
# Separate trees avoid inheriting Windows preset paths or another SDK's cache.
if command -v ccache >/dev/null 2>&1; then
    args+=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
fi
file_needed "$ROOT/external/zlib/CMakeLists.txt"
file_needed "$ROOT/external/stb/stb_image.h"
case "$target" in
    gcc)
        need gcc; need g++
        for file in SDL2/CMakeLists.txt SDL_net/CMakeLists.txt gzip-hpp/include/gzip/compress.hpp miniaudio/miniaudio.h; do
            file_needed "$ROOT/external/$file"
        done
        args+=(-DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DMC_LOG_LEVEL=2 -DSDL_PIPEWIRE=OFF
            "-DCMAKE_CXX_FLAGS=-I\"$ROOT/src/net/minecraft/src\"")
        ;;
    wii)
        export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
        export DEVKITPPC="${DEVKITPPC:-$DEVKITPRO/devkitPPC}"
        export PATH="$DEVKITPPC/bin:$DEVKITPRO/tools/bin:$PATH"
        need powerpc-eabi-g++
        need elf2dol
        file_needed "$DEVKITPRO/libogc/include/gccore.h"
        args+=("-DCMAKE_TOOLCHAIN_FILE=$ROOT/cmake/wii_toolchain.cmake" -DPLATFORM=WII
            "-DDEVKITPRO=$DEVKITPRO" "-DDEVKITPPC=$DEVKITPPC"
            -DWII_BRINGUP=OFF -DWII_ENABLE_SOUND=ON -DWII_ENABLE_NETWORK=ON
            "-DCMAKE_CXX_FLAGS=-DHW_RVL -mrvl -mcpu=750 -meabi -mhard-float -I\"$ROOT/src/net/minecraft/src\"")
        ;;
    ps2)
        export PS2DEV="${PS2DEV:-/usr/local/ps2dev}"
        export PS2SDK="${PS2SDK:-$PS2DEV/ps2sdk}"
        export GSKIT="${GSKIT:-$PS2DEV/gsKit}"
        export PATH="$PS2DEV/ee/bin:$PS2DEV/iop/bin:$PS2DEV/dvp/bin:$PS2DEV/bin:$PS2SDK/bin:$PATH"
        need mips64r5900el-ps2-elf-g++
        file_needed "$PS2SDK/ee/include/kernel.h"
        args+=("-DCMAKE_TOOLCHAIN_FILE=$ROOT/cmake/ps2_toolchain.cmake" -DPLATFORM=PS2
            -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY -DPS2_ENABLE_NETWORK=ON
            -DPS2_ENABLE_SOUND=ON -DPS2_ENABLE_PERSPECTIVE_TEXTURES=ON
            -DPS2_RENDER_STATS=OFF -DPS2_ENABLE_VU1_TERRAIN=ON -DMC_LOG_LEVEL=0)
        ;;
esac
if (( assets && ! configure_only )); then
    if (( ! dry )); then
        [[ -d "$ROOT/data/assets" ]] || fail 'Game assets missing; restore data/ or use --no-assets to compile only.'
        if [[ "$target" == ps2 ]]; then
            [[ -d "$ROOT/data/startup" && -d "$ROOT/data/resources_ps2" ]] || fail 'PS2 staging requires data/startup and data/resources_ps2.'
        elif [[ "$target" == wii ]]; then
            [[ -d "$ROOT/data/resources" ]] || fail 'Wii staging requires data/resources.'
        fi
    fi
    if [[ "$target" == ps2 ]]; then need python3; file_needed "$ROOT/scripts/make_pak.py"; fi
fi
build_steps() {
    run cmake "${args[@]}" "${extra[@]}"
    (( configure_only )) && return 0
    run cmake --build "$build" --target OptiCraft --parallel "$parallel"
    if (( assets )); then
        case "$target" in
            ps2)
                run cmake --build "$build" --target ps2-data --parallel "$parallel"
                run python3 "$ROOT/scripts/make_pak.py" "$ROOT/bin/ps2/usb/MCBETA/data" "$ROOT/bin/ps2/usb/MCBETA/assets.pak"
                ;;
            wii) run cmake --build "$build" --target wii-data --parallel "$parallel" ;;
            gcc)
                run cmake -E copy_directory "$ROOT/data/assets" "$ROOT/bin/$config/assets"
                if [[ -d "$ROOT/data/resources" ]]; then
                    run cmake -E copy_directory "$ROOT/data/resources" "$ROOT/bin/$config/resources"
                else
                    printf 'WARNING: data/resources is missing; desktop audio is not staged (resources_ps2 is not a substitute).\n' >&2
                fi
                if [[ -f "$ROOT/bin/$config/assets.pak" ]]; then
                    printf 'WARNING: existing assets.pak takes priority over staged loose files; update it if it contains older assets.\n' >&2
                fi
                ;;
        esac
    fi
}
if (( dry )); then
    build_steps
else
    mkdir -p "$ROOT/build/logs"
    log="$ROOT/build/logs/$target-${config,,}.log"
    printf 'Build log: %s\n' "$log"
    # pipefail preserves configure, compile and staging failures through tee.
    build_steps 2>&1 | tee "$log"
fi
if (( ! configure_only )); then
    case "$target" in
        gcc) printf 'Executable: %s/bin/%s/OptiCraft\n' "$ROOT" "$config" ;;
        wii) printf 'Homebrew Channel directory: %s/bin/wii/sd/apps/OptiCraft\n' "$ROOT" ;;
        ps2) printf 'USB application directory: %s/bin/ps2/usb/MCBETA\n' "$ROOT" ;;
    esac
fi
