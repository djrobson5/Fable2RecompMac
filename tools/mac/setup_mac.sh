#!/bin/bash
# setup_mac.sh -- build Fable II for macOS (Apple silicon, Metal) from your own disc. The install entry point a
# launcher (Wayfarer) runs in a clone of this repository.
#
#   tools/mac/setup_mac.sh --game <.iso | folder with default.xex> --update <title update file | folder to search> \
#                          --assets <folder> [--toolchains <folder>] [--reuse-sdk <dir>] [--reuse-dxc <dir>]
#   tools/mac/setup_mac.sh --check [--json]
#
#   --game <path>       your disc image (.iso), or a folder holding the unpacked disc (default.xex)
#   --update <path>     title update 1: the STFS package (a file), or a folder to look in (a few levels deep); the
#                       one made for the disc's media ID is used
#   --assets <folder>   where the game files go: <assets>/game (the disc, with default.xexp beside default.xex) and
#                       <assets>/update. A game folder that is <assets>/game, or inside <assets>, is used where it
#                       is; another folder is copied (cloned on the same volume: no extra space)
#   --toolchains <dir>  where the ReXGlue SDK and DXC are built (default: out/toolchains in this clone)
#   --reuse-sdk <dir>   an existing ReXGlue SDK source tree, built and installed (out/install/mac-arm64), at the
#   --reuse-dxc <dir>   pinned commit; an existing DirectXShaderCompiler tree at v1.8.2407, built in out/build.
#                       Used only when it matches the pin; never changed.
#   --check [--json]    what the build needs (Apple silicon, Xcode tools, Homebrew's cmake and ninja, git, python3,
#                       the two toolchains, the downloads): one line each, or the JSON a launcher reads (cpu,
#                       compiler, toolchains, downloads). Changes nothing, installs nothing, builds nothing.
#   --progress json     also print `@fable2 {json}` lines for a launcher (stage / progress / error / result;
#                       tools/mac/mac_common.sh has the shapes), or FABLE2_PROGRESS=json
#   --stop-after <id>   stop after that stage (for tests)
#
# The build installs nothing: a missing tool is `missing_packages`, naming it (brew install cmake ninja python).
# The settings of a run are kept in out/install.env, so a later `tools/mac/setup_mac.sh` repeats them.
#
# Stages (each announced, also when it has nothing to do); the stage ids of --progress json:
#   toolchains  tools/mac/build_toolchains.sh: the SDK at bd833a2a and DXC at v1.8.2407, built from source (or
#               reused when they match)
#   extract     the extractor (tools/sdk_tools, built against the SDK); the disc unpacked, copied or used in place;
#               the title update for the disc's media ID unpacked so default.xexp sits beside default.xex;
#               assets/game and assets/update in this clone link to them
#   check       the game program with its update must give the image hash the build was recompiled from
#               (cmake/launcher.cmake), else `wrong_game`
#   codegen     rexglue codegen Fable2_manifest.toml (skipped when the same inputs were generated already)
#   configure   submodules (plume; XenosRecomp with smol-v, zstd, xxHash, fmt; never its dxc-bin), cmake
#   compile     ninja, out/build/mac-arm64-native, target Fable2 (ninja's [n/N] is the progress)
#
#   JOBS=n         parallel compiles (default: the number of CPUs)
#
# Error codes: missing_packages, no_clang, no_brew, no_disc, disc_not_found, wrong_game, no_update, extract_failed,
# toolchain_failed, codegen_failed, configure_failed, build_failed, download_failed, disk_full.
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
source "$ROOT/tools/mac/mac_common.sh"
CLEANUP_HOOK=remove_temporaries
KILL_WAIT_TICKS=60      # longer than build_toolchains.sh's own wait, so its cleanup finishes first
install_traps

STAGES="toolchains extract check codegen configure compile"
BUILD="$ROOT/out/build/mac-arm64-native"
LOGS="$ROOT/out/logs"
SETTINGS="$ROOT/out/install.env"
EXTRACT_BUILD="$ROOT/out/sdk_tools"
EXTRACT="$EXTRACT_BUILD/extract"
JOBS=${JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || echo 4)}

# The image hash the build was recompiled from.
IMAGE_HASH=$(sed -n 's/^set(FABLE2_LAUNCHER_IMAGE_HASH "\([0-9a-f]*\)")$/\1/p' "$ROOT/cmake/launcher.cmake" 2>/dev/null)
IMAGE_LABEL=$(sed -n 's/^set(FABLE2_LAUNCHER_IMAGE_LABEL "\(.*\)")$/\1/p' "$ROOT/cmake/launcher.cmake" 2>/dev/null)
[ -n "$IMAGE_HASH" ] || die "cmake/launcher.cmake has no FABLE2_LAUNCHER_IMAGE_HASH"
FABLE2_TITLE_ID=4d5307f1

# Temporaries (staging folders) of this run: removed on a cancel and on every exit.
CLEANUP_PATHS=
add_cleanup() { CLEANUP_PATHS="$CLEANUP_PATHS$1"$'\n'; }
drop_cleanup() {   # the path was moved into place
    local keep= p
    while IFS= read -r p; do [ -z "$p" ] || [ "$p" = "$1" ] || keep="$keep$p"$'\n'; done <<EOF
$CLEANUP_PATHS
EOF
    CLEANUP_PATHS=$keep
}
remove_temporaries() {
    local p
    while IFS= read -r p; do
        [ -n "$p" ] && [ -e "$p" ] || continue
        chmod -R u+w "$p" 2>/dev/null || true
        rm -rf "$p"
    done <<EOF
$CLEANUP_PATHS
EOF
    CLEANUP_PATHS=
}

# ---- what the build needs (read only) --------------------------------------------------------------------------
cpu_ok=0 cpu_text=
clang_ok=0 clang_text=
brew_ok=0 brew_missing=""
py_path= py_ok=0
git_ok=0

check_cpu() {
    local name; name=$(/usr/sbin/sysctl -n machdep.cpu.brand_string 2>/dev/null) || name=
    # hw.optional.arm64 is 1 on Apple silicon even for a shell running under Rosetta
    if [ "$(/usr/sbin/sysctl -n hw.optional.arm64 2>/dev/null)" = 1 ]; then
        cpu_ok=1; cpu_text="${name:-Apple silicon}: native arm64 build"
    else
        cpu_ok=0; cpu_text="${name:-this processor} is not Apple silicon: the port is built for arm64"
    fi
}

check_tools() {   # xcrun and /usr/bin/clang are shims that start an installer when there are no developer tools,
    # so they only run once xcode-select -p says there are some
    local dir clang
    clang_ok=0; clang_text="not installed (xcode-select --install)"
    dir=$(xcode-select -p 2>/dev/null) || dir=
    if [ -n "$dir" ] && [ -d "$dir" ]; then
        clang=$(xcrun --find clang++ 2>/dev/null) || clang=
        if [ -n "$clang" ] && [ -x "$clang" ]; then
            clang_ok=1
            clang_text=$("$(dirname "$clang")/clang" --version 2>/dev/null | sed -n '1s/ version \([0-9][0-9]*\)\..*/ \1/p')
            clang_text="${clang_text:-clang} ($dir)"
        fi
    fi
    git_ok=0
    if [ "$clang_ok" = 1 ] && git --version >/dev/null 2>&1; then git_ok=1; fi
    brew_ok=0
    if command -v brew >/dev/null 2>&1; then brew_ok=1; fi
    brew_missing=""
    local pkg
    for pkg in cmake ninja; do
        command -v "$pkg" >/dev/null 2>&1 || brew_missing="$brew_missing $pkg"
    done
    # package_app.sh reads the SDK's achievement list with tomllib (python 3.11 and up)
    py_path=
    local c p
    for c in python3.14 python3.13 python3.12 python3.11 python3; do
        p=$(command -v "$c" 2>/dev/null) || continue
        [ "$p" = /usr/bin/python3 ] && [ "$clang_ok" = 0 ] && continue
        if "$p" -I -c 'import tomllib' >/dev/null 2>&1; then py_path=$p; break; fi
    done
    if [ -n "$py_path" ]; then py_ok=1; else py_ok=0; brew_missing="$brew_missing python"; fi
    brew_missing=${brew_missing# }
}

# the toolchains' state, from build_toolchains.sh --check
tc_json=
tc_text=
check_toolchains() {   # (git is only run once the developer tools are there: without them /usr/bin/git is an installer shim)
    local args=()
    tc_json= tc_text=
    [ "$clang_ok" = 1 ] || return 0
    [ -z "$REUSE_SDK" ] || args+=(--reuse-sdk "$REUSE_SDK")
    [ -z "$REUSE_DXC" ] || args+=(--reuse-dxc "$REUSE_DXC")
    tc_json=$("$ROOT/tools/mac/build_toolchains.sh" --into "$TOOLCHAINS" --check --json ${args[@]+"${args[@]}"} 2>/dev/null) || tc_json=
    tc_text=$("$ROOT/tools/mac/build_toolchains.sh" --into "$TOOLCHAINS" --check ${args[@]+"${args[@]}"} 2>/dev/null) || tc_text=
}
tc_needs_build() { case "$tc_json" in *'"state": "build"'*) return 0 ;; esac; return 1; }

submodules_missing() {   # 0 when a submodule the build needs is not checked out
    local s
    for s in thirdparty/plume thirdparty/XenosRecomp thirdparty/XenosRecomp/thirdparty/smol-v \
             thirdparty/XenosRecomp/thirdparty/zstd thirdparty/XenosRecomp/thirdparty/xxHash \
             thirdparty/XenosRecomp/thirdparty/fmt; do
        [ -e "$ROOT/$s/.git" ] || return 0
    done
    return 1
}

run_check() {   # [json]
    PROGRESS_JSON=0
    check_cpu; check_tools; check_toolchains
    local missing="" missing_names="" packages="" compiler_ok=1 compiler_text name
    add_missing() { missing="$missing${missing:+, }$1"; missing_names="$missing_names$1"$'\n'; compiler_ok=0; }
    [ "$clang_ok" = 1 ] || add_missing "clang (Xcode command-line tools)"
    [ "$git_ok" = 1 ] || add_missing git
    [ "$brew_ok" = 1 ] || [ -z "$brew_missing" ] || add_missing Homebrew
    if [ -n "$brew_missing" ]; then
        for name in $brew_missing; do add_missing "$name"; done
        packages=$brew_missing
    fi
    if [ "$compiler_ok" = 1 ]; then
        compiler_text="$clang_text, git, Homebrew, cmake, ninja and python3 ($py_path)"
    else
        compiler_text="missing: $missing"
    fi
    local tc_ok=1 tc_message
    [ -n "$tc_json" ] || tc_ok=0
    tc_message=$(printf '%s\n' "$tc_text" | sed 's/  */ /g' | awk 'NR == 1 {sub(/^[^ ]+ [^ ]+ /, ""); a = $0} NR == 2 {sub(/^[^ ]+ [^ ]+ /, ""); b = $0} END {if (a != "") printf "ReXGlue SDK: %s; DXC: %s", a, b}')
    [ -n "$tc_message" ] || tc_message="ReXGlue SDK and DXC: could not be checked"
    # downloads: needed when a toolchain is built or a submodule is missing; then GitHub must answer
    local dl_ok=1 dl_text code needs=0
    if tc_needs_build || submodules_missing; then needs=1; fi
    if [ "$needs" = 0 ]; then
        dl_text="nothing to download: the toolchains and the build's libraries are there"
    else
        code=$(curl -sI --max-time 6 -o /dev/null -w '%{http_code}' https://github.com 2>/dev/null) || code=000
        case "$code" in
        2*|3*) dl_text="GitHub answers: the first build downloads the toolchains' and the libraries' source" ;;
        *) dl_ok=0; dl_text="GitHub is not reachable: the first build downloads the toolchains' and the libraries' source" ;;
        esac
    fi
    if [ "$1" = json ]; then
        local tcj=${tc_json:-null}
        # the toolchain row: build_toolchains' two parts under one ok and message
        printf '{"cpu": {"ok": %s, "message": %s}, "compiler": {"ok": %s, "message": %s, "missing": %s, "packages": %s, "xcode_tools": %s}, "toolchains": {"ok": %s, "message": %s, "detail": %s}, "downloads": {"ok": %s, "message": %s}}\n' \
            "$(json_bool $cpu_ok)" "$(json_str "$cpu_text")" \
            "$(json_bool $compiler_ok)" "$(json_str "$compiler_text")" \
            "$(json_list_lines "$missing_names")" "$(json_list_words "$packages")" "$(json_bool $((1 - clang_ok)))" \
            "$(json_bool $tc_ok)" "$(json_str "$tc_message")" "$tcj" \
            "$(json_bool $dl_ok)" "$(json_str "$dl_text")"
        return 0
    fi
    row() { printf '%-8s %-14s %s\n' "$1" "$2:" "$3"; }
    row "$([ "$cpu_ok" = 1 ] && echo ok || echo MISSING)" "Apple silicon" "$cpu_text"
    row "$([ "$clang_ok" = 1 ] && echo ok || echo MISSING)" "clang" "$clang_text"
    row "$([ "$git_ok" = 1 ] && echo ok || echo MISSING)" "git" "$(command -v git 2>/dev/null || echo 'needs the Xcode command-line tools')"
    if [ "$brew_ok" = 1 ]; then row ok "Homebrew" "$(command -v brew)"; elif [ -n "$brew_missing" ]; then row MISSING "Homebrew" "https://brew.sh"; else row ok "Homebrew" "not installed, and not needed"; fi
    for name in cmake ninja; do
        case " $brew_missing " in *" $name "*) row MISSING "$name" "brew install $name" ;; *) row ok "$name" "$(command -v $name)" ;; esac
    done
    if [ "$py_ok" = 1 ]; then row ok "python3" "$py_path (3.11 or newer)"; else row MISSING "python3" "brew install python (3.11 or newer)"; fi
    printf '%-8s %-14s %s\n' ok "ReXGlue SDK:" "$(printf '%s' "$tc_text" | sed -n '1p' | sed 's/  */ /g' | cut -d' ' -f3-)"
    printf '%-8s %-14s %s\n' ok "DXC:" "$(printf '%s' "$tc_text" | sed -n '2p' | sed 's/  */ /g' | cut -d' ' -f3-)"
    row "$([ "$dl_ok" = 1 ] && echo ok || echo MISSING)" "downloads" "$dl_text"
    [ "$cpu_ok" = 1 ] && [ "$compiler_ok" = 1 ] && [ "$dl_ok" = 1 ]
}
json_list_words() { set -- $1; json_list "$@"; }
json_list_lines() {
    local out= line
    while IFS= read -r line; do [ -n "$line" ] && out="$out${out:+, }$(json_str "$line")"; done <<EOF
$1
EOF
    printf '[%s]' "$out"
}

# ---- arguments -----------------------------------------------------------------------------------------------------
GAME= UPDATE= ASSETS= TOOLCHAINS= REUSE_SDK= REUSE_DXC= STOP_AFTER=
CHECK=0 WANT_JSON=0
while [ $# -gt 0 ]; do
    case "$1" in
    --game) GAME="${2:?--game takes the disc image or its folder}"; shift 2 ;;
    --update) UPDATE="${2:?--update takes the title update or a folder}"; shift 2 ;;
    --assets) ASSETS="${2:?--assets takes a folder}"; shift 2 ;;
    --toolchains) TOOLCHAINS="${2:?--toolchains takes a folder}"; shift 2 ;;
    --reuse-sdk) REUSE_SDK="${2:?--reuse-sdk takes a folder}"; shift 2 ;;
    --reuse-dxc) REUSE_DXC="${2:?--reuse-dxc takes a folder}"; shift 2 ;;
    --stop-after) STOP_AFTER="${2:?--stop-after takes a stage id}"; shift 2 ;;
    --check) CHECK=1; shift ;;
    --json) WANT_JSON=1; shift ;;
    --progress) set_progress_mode "${2:?--progress takes text or json}"; shift 2 ;;
    --progress=*) set_progress_mode "${1#--progress=}"; shift ;;
    -h|--help) sed -n '2,/^set -e/p' "$0" | sed '$d;s/^# \{0,1\}//'; exit 0 ;;
    *) die "unknown argument: $1 (see tools/mac/setup_mac.sh --help)" ;;
    esac
done
case " $STAGES " in *" $STOP_AFTER "*) ;; *) [ -z "$STOP_AFTER" ] || die "--stop-after: not a stage: $STOP_AFTER" ;; esac
abs() {   # an absolute, symlink-free path of an existing or yet to be made path
    local p=$1 d b
    if [ -d "$p" ]; then ( cd "$p" && pwd -P ); return; fi
    d=$(dirname "$p"); b=$(basename "$p")
    if [ -d "$d" ]; then printf '%s/%s\n' "$(cd "$d" && pwd -P)" "$b"; else printf '%s\n' "$p"; fi
}

GAME_CLI=$GAME UPDATE_CLI=$UPDATE      # (a saved game or update that is gone is not an error: see the run's inputs)
# saved settings (out/install.env): used for what the command line did not give
saved_game= saved_update= saved_assets= saved_toolchains= saved_reuse_sdk= saved_reuse_dxc=
if [ -f "$SETTINGS" ]; then
    # shellcheck disable=SC1090
    . "$SETTINGS"
    saved_game=${FABLE2_GAME:-} saved_update=${FABLE2_UPDATE:-} saved_assets=${FABLE2_ASSETS:-}
    saved_toolchains=${FABLE2_TOOLCHAINS:-} saved_reuse_sdk=${FABLE2_REUSE_SDK:-} saved_reuse_dxc=${FABLE2_REUSE_DXC:-}
fi
[ -n "$GAME" ] || GAME=$saved_game
[ -n "$UPDATE" ] || UPDATE=$saved_update
[ -n "$ASSETS" ] || ASSETS=$saved_assets
[ -n "$TOOLCHAINS" ] || TOOLCHAINS=$saved_toolchains
[ -n "$REUSE_SDK" ] || REUSE_SDK=$saved_reuse_sdk
[ -n "$REUSE_DXC" ] || REUSE_DXC=$saved_reuse_dxc
[ -n "$TOOLCHAINS" ] || TOOLCHAINS="$ROOT/out/toolchains"
TOOLCHAINS=$(abs "$TOOLCHAINS")

if [ "$CHECK" = 1 ]; then
    trap - EXIT TERM INT HUP    # a report, not a build: no error event, and never an @fable2 line
    if [ "$WANT_JSON" = 1 ]; then run_check json || true; exit 0; fi
    run_check
    exit
fi
[ "$WANT_JSON" = 0 ] || die "--json goes with --check"

# ---- the run's inputs --------------------------------------------------------------------------------------------------
[ -n "$ASSETS" ] || fail no_disc "no folder for the game files given: --assets <folder>"
mkdir -p "$ASSETS" "$ROOT/out" "$LOGS"
ASSETS=$(abs "$ASSETS")
# A game or an update remembered from an earlier run (not given on this command line) that is gone counts as absent.
if [ -n "$GAME" ] && [ ! -e "$GAME" ]; then
    [ -z "$GAME_CLI" ] || fail disc_not_found "not found: $GAME"
    GAME=
fi
if [ -z "$GAME" ]; then
    if [ -f "$ASSETS/game/default.xex" ]; then
        GAME="$ASSETS/game"     # the game files are there already: used where they are
    else
        fail no_disc "no game given: --game <your Fable II disc image (.iso) or the folder holding default.xex>"
    fi
fi
if [ -n "$UPDATE" ] && [ ! -e "$UPDATE" ]; then
    [ -z "$UPDATE_CLI" ] || fail no_update "not found: $UPDATE (the title update's file or the folder it is in)"
    UPDATE=       # a remembered one that is gone: asked for only when it is needed
fi
GAME=$(abs "$GAME")
[ -z "$UPDATE" ] || UPDATE=$(abs "$UPDATE")
[ -z "$REUSE_SDK" ] || REUSE_SDK=$(abs "$REUSE_SDK")
[ -z "$REUSE_DXC" ] || REUSE_DXC=$(abs "$REUSE_DXC")
cd "$ROOT"

save_settings() {
    local tmp; tmp=$(mktemp "$ROOT/out/.install.env.XXXXXX")
    {
        printf 'FABLE2_GAME=%q\n' "$GAME"
        printf 'FABLE2_UPDATE=%q\n' "$UPDATE"
        printf 'FABLE2_ASSETS=%q\n' "$ASSETS"
        printf 'FABLE2_TOOLCHAINS=%q\n' "$TOOLCHAINS"
        printf 'FABLE2_REUSE_SDK=%q\n' "$REUSE_SDK"
        printf 'FABLE2_REUSE_DXC=%q\n' "$REUSE_DXC"
    } > "$tmp"
    mv "$tmp" "$SETTINGS"
}

finish_stage() {   # a --stop-after stage ends the run here
    if [ "$STOP_AFTER" = "$1" ]; then
        step "stopped after the $1 stage (--stop-after)"
        event "{\"result\": \"ok\", \"tool\": \"setup\", \"stopped_after\": $(json_str "$1")}"
        exit 0
    fi
}

# ---- 1. toolchains -----------------------------------------------------------------------------------------------------
DEFAULT_ERROR=toolchain_failed
check_tools
check_toolchains
if [ -z "$tc_json" ]; then
    stage_event toolchains "Checking the build tools and the toolchains"
elif tc_needs_build; then
    stage_event toolchains "Building the toolchains from source (ReXGlue SDK and DXC; the first install takes a while)"
else
    stage_event toolchains "Toolchains: ReXGlue SDK and DXC found at their pins; nothing to build"
fi
[ "$clang_ok" = 1 ] || fail no_clang "Apple's compiler is missing: install the Xcode command-line tools (xcode-select --install)"
if [ -n "$brew_missing" ]; then
    [ "$brew_ok" = 1 ] || fail no_brew "Homebrew is needed for $brew_missing: https://brew.sh"
    fail missing_packages "Homebrew packages the build needs are missing: $brew_missing (brew install $brew_missing)"
fi
step "Toolchains (tools/mac/build_toolchains.sh)"
tc_args=(--into "$TOOLCHAINS" --embedded)
[ -z "$REUSE_SDK" ] || tc_args+=(--reuse-sdk "$REUSE_SDK")
[ -z "$REUSE_DXC" ] || tc_args+=(--reuse-dxc "$REUSE_DXC")
tc_rc=0
tc_flag="$ROOT/out/.toolchain-error.$$"; rm -f "$tc_flag"; add_cleanup "$tc_flag"
export FABLE2_ERROR_FLAG="$tc_flag"      # build_toolchains.sh touches it when it sends an error event
run_child "$ROOT/tools/mac/build_toolchains.sh" "${tc_args[@]}" || tc_rc=$?
unset FABLE2_ERROR_FLAG
if [ "$tc_rc" -ne 0 ]; then
    if [ "$tc_rc" -eq 143 ]; then ERROR_EMITTED=1; exit 143; fi      # a cancel: no error event
    if [ -e "$tc_flag" ]; then ERROR_EMITTED=1; exit "$tc_rc"; fi    # it sent its own, more specific error event
    fail toolchain_failed "build_toolchains.sh stopped (exit status $tc_rc)"
fi
# shellcheck disable=SC1090
. "$TOOLCHAINS/toolchains.env"
[ -x "$REXGLUE_CLI" ] || fail toolchain_failed "the rexglue CLI is missing: $REXGLUE_CLI"
export REXSDK
save_settings
finish_stage toolchains

# ---- helpers for the game files -----------------------------------------------------------------------------------------
hex_at() {   # hex_at <file> <offset> <count>: the bytes as one lowercase hex string
    od -An -v -tx1 -j "$2" -N "$3" "$1" 2>/dev/null | tr -d ' \n'
}
hex_to_dec() { printf '%d' "0x$1"; }

# xex_identity <default.xex>: sets XEX_TITLE, XEX_MEDIA, XEX_VERSION (hex); 1 when it is not an XEX2
xex_identity() {
    local hex n i key at
    XEX_TITLE= XEX_MEDIA= XEX_VERSION=
    hex=$(hex_at "$1" 0 65536)
    [ "${hex:0:8}" = 58455832 ] || return 1
    n=$(hex_to_dec "${hex:40:8}"); [ "$n" -le 64 ] || n=64
    i=0
    while [ "$i" -lt "$n" ]; do
        key=${hex:$(( (24 + 8 * i) * 2 )):8}
        if [ "$key" = 00040006 ]; then
            at=$(hex_to_dec "${hex:$(( (28 + 8 * i) * 2 )):8}")
            [ $(( (at + 16) * 2 )) -le ${#hex} ] || return 1
            XEX_MEDIA=${hex:$(( at * 2 )):8}
            XEX_VERSION=${hex:$(( at * 2 + 8 )):8}
            XEX_TITLE=${hex:$(( at * 2 + 24 )):8}
            return 0
        fi
        i=$((i + 1))
    done
    return 1
}

# stfs_update_info <file>: "<media> <disc version>" (hex) when it is an STFS title update (content type 0xB0000) for
# Fable II; 1 otherwise. The offsets are those of the project's own launcher (tools/launcher/Fable2Launcher.swift).
stfs_update_info() {
    local magic hex size
    size=$(stat -L -f %z "$1" 2>/dev/null) || return 1
    [ "$size" -ge 868 ] || return 1
    magic=$(head -c 4 "$1" 2>/dev/null) || return 1
    case "$magic" in LIVE|PIRS|"CON ") ;; *) return 1 ;; esac
    hex=$(hex_at "$1" $((0x344)) 32)            # 0x344 .. 0x364
    [ "${hex:0:8}" = 000b0000 ] || return 1     # content type: title update
    [ "${hex:56:8}" = "$FABLE2_TITLE_ID" ] || return 1
    printf '%s %s\n' "${hex:32:8}" "${hex:48:8}"   # 0x354 media ID, 0x35C disc version
}

# update_candidates <media> <disc version>: the title update packages under $UPDATE made for the media ID, the one
# made for the disc's own version first
update_candidates() {
    local list f info
    list=$([ -d "$UPDATE" ] && find "$UPDATE" -maxdepth 4 -type f -size +1k -size -200000k ! -name '.*' 2>/dev/null || printf '%s\n' "$UPDATE")
    while IFS= read -r f; do
        [ -n "$f" ] || continue
        info=$(stfs_update_info "$f") || continue
        set -- $info
        [ "$1" = "$XEX_MEDIA" ] || continue
        if [ "$2" = "$XEX_VERSION" ]; then printf '0 %s\n' "$f"; else printf '1 %s\n' "$f"; fi
    done <<EOF
$list
EOF
}

# image_hash <default.xex> [default.xexp]: the guest image's hash, read through a scratch folder of links so the
# extractor's runtime never writes beside the game's files
image_hash() {
    local view out
    view=$(mktemp -d "$ROOT/out/hashview.XXXXXX")
    add_cleanup "$view"
    ln -s "$1" "$view/default.xex"
    [ -z "${2:-}" ] || ln -s "$2" "$view/default.xexp"
    out=$("$EXTRACT" --image-hash "$view" 2>"$LOGS/image-hash.log") || out=
    rm -rf "$view"; drop_cleanup "$view"
    printf '%s' "$out" | sed -n 's/^image \([0-9a-f]*\)$/\1/p' | tail -1
}

free_kb() { df -k "$1" | awk 'NR==2 {print $4}'; }

# unpack_progress: the extractor's `progress <done> <total>` lines (bytes) as events; other lines to the log
unpack_progress() {
    local line done_b total_b started=$SECONDS lastsec=-1 eta
    while IFS= read -r line; do
        if [[ $line =~ ^progress\ ([0-9]+)\ ([0-9]+)$ ]]; then
            done_b=${BASH_REMATCH[1]}; total_b=${BASH_REMATCH[2]}
            [ "$total_b" -gt 0 ] || continue
            if [ "$SECONDS" -ne "$lastsec" ] || [ "$done_b" -ge "$total_b" ]; then
                lastsec=$SECONDS; eta=
                if [ "$done_b" -gt 0 ] && [ "$done_b" -lt "$total_b" ] && [ "$SECONDS" -gt "$started" ]; then
                    eta=$(( (SECONDS - started) * (total_b - done_b) / done_b ))
                fi
                progress_event "$done_b" "$total_b" "$eta" "$PROGRESS_NOTE"
                [ "$PROGRESS_JSON" = 1 ] || printf 'unpacked %s of %s MB\n' "$((done_b / 1048576))" "$((total_b / 1048576))"
            fi
        else
            printf '%s\n' "$line"
        fi
    done
    return 0
}
PROGRESS_NOTE=
run_unpack() {   # run_unpack <note> <log> <extract args...>
    PROGRESS_NOTE=$1; local log=$2; shift 2
    run_child unpack_pipeline "$log" "$@"
}
unpack_pipeline() {
    local log=$1; shift
    "$EXTRACT" --progress "$@" 2>&1 | tee -a "$log" | unpack_progress
    return "${PIPESTATUS[0]}"
}

# ---- 2. extract ------------------------------------------------------------------------------------------------------
DEFAULT_ERROR=extract_failed
GAME_DIR=            # the folder holding default.xex that the build uses
UPDATE_DIR="$ASSETS/update"
mode=copy
case "$GAME" in
    *.iso|*.ISO) [ -f "$GAME" ] && mode=iso ;;
esac
if [ "$mode" != iso ]; then
    [ -d "$GAME" ] || fail disc_not_found "$GAME is neither a disc image (.iso) nor a folder"
    [ -f "$GAME/default.xex" ] || fail wrong_game "no default.xex in $GAME: give the disc image (.iso) or the folder holding the unpacked disc"
    case "$GAME" in "$ASSETS"|"$ASSETS"/*) mode=inplace ;; esac
fi
case "$mode" in
    iso) stage_event extract "Unpacking the game from the disc image (about 6.5 GB)" ;;
    inplace) stage_event extract "Using the game files where they are ($GAME)" ;;
    *) stage_event extract "Copying the game files into $ASSETS" ;;
esac
step "Game files ($mode): $GAME"

# 2a. the extractor, built standalone against the SDK
step "Extractor (tools/sdk_tools, against the SDK)"
progress_event 0 1 "" "building the extractor"
elog="$LOGS/extractor-build.log"
: > "$elog"
if [ ! -f "$EXTRACT_BUILD/build.ninja" ] || ! grep -qF "$REXSDK" "$EXTRACT_BUILD/CMakeCache.txt" 2>/dev/null; then
    run_child cmake -S "$ROOT/tools/sdk_tools" -B "$EXTRACT_BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++ -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_PREFIX_PATH="$REXSDK" -Drexglue_DIR="$REXSDK/lib/cmake/rexglue" >>"$elog" 2>&1 || fail extract_failed "could not set up the extractor build" "$elog"
fi
run_child ninja -C "$EXTRACT_BUILD" extract >>"$elog" 2>&1 || fail extract_failed "could not build the extractor (tools/sdk_tools/extract.cpp)" "$elog"
[ -x "$EXTRACT" ] || fail extract_failed "the extractor was not built: $EXTRACT" "$elog"

# 2b. the game files
have_good_game() {   # <folder>: it is a game folder that already checks out
    [ -f "$1/default.xex" ] || return 1
    local x= h
    [ -f "$1/default.xexp" ] && x="$1/default.xexp"
    h=$(image_hash "$1/default.xex" "$x")
    [ "$h" = "$IMAGE_HASH" ]
}
# wrong_game_fail <folder> <hash it gave, may be empty>
wrong_game_fail() {
    local what
    xex_identity "$1/default.xex" || true
    what="media ID $(printf '%s' "${XEX_MEDIA:-unknown}" | tr a-f A-F), version $(printf '%s' "${XEX_VERSION:-unknown}" | tr a-f A-F)"
    if [ -z "$2" ]; then
        fail wrong_game "the game program in $1 could not be loaded ($what; see out/logs/image-hash.log): this build needs $IMAGE_LABEL" "$LOGS/image-hash.log"
    fi
    fail wrong_game "the game files give image $2 ($what), not $IMAGE_HASH: this build needs $IMAGE_LABEL"
}
remove_stale() { [ -e "$1" ] || return 0; chmod -R u+w "$1" 2>/dev/null || true; rm -rf "$1"; }
clear_empty_dir() { [ -d "$1" ] && [ -z "$(ls -A "$1" 2>/dev/null)" ] && rmdir "$1"; return 0; }   # an empty folder may be replaced

# require_update: a title update package for the program in XEX_MEDIA must be there to unpack (sets CANDIDATES)
CANDIDATES=
require_update() {   # require_update <default.xex> [<xexp>]: nothing when this program already gives the pinned image
    local h x= media
    [ -z "${2:-}" ] || x=$2
    h=$(image_hash "$1" "$x")
    [ "$h" != "$IMAGE_HASH" ] || return 0
    xex_identity "$1" || fail wrong_game "$1 could not be read"
    [ "$XEX_TITLE" = "$FABLE2_TITLE_ID" ] || fail wrong_game "this is not Fable II (title ID $(printf '%s' "$XEX_TITLE" | tr a-f A-F))"
    media=$(printf '%s' "$XEX_MEDIA" | tr a-f A-F)
    [ -n "$UPDATE" ] || fail no_update "this disc (media ID $media) needs title update 1: give it with --update <the package, or the folder it is in>"
    CANDIDATES=$(update_candidates | sort)
    [ -n "$CANDIDATES" ] || fail no_update "no title update for Fable II media ID $media in $UPDATE"
}

elog="$LOGS/extract.log"
: > "$elog"
need_staging=0
staging=
case "$mode" in
iso)
    GAME_DIR="$ASSETS/game"
    iso_size=$(stat -L -f %z "$GAME")
    # 1. what the disc is (default.xex alone)
    ident="$ROOT/out/ident.$$"; mkdir -p "$ident"; add_cleanup "$ident"
    run_child "$EXTRACT" --only=default.xex "$GAME" "$ident" >>"$elog" 2>&1 \
        || fail extract_failed "could not read the disc image $GAME (is it an Xbox 360 disc image?)" "$elog"
    [ -f "$ident/default.xex" ] || fail wrong_game "the disc image has no default.xex: not an Xbox 360 game disc" "$elog"
    xex_identity "$ident/default.xex" || fail wrong_game "the disc image's default.xex could not be read" "$elog"
    [ "$XEX_TITLE" = "$FABLE2_TITLE_ID" ] || fail wrong_game "this disc is not Fable II (title ID $(printf '%s' "$XEX_TITLE" | tr a-f A-F), media ID $(printf '%s' "$XEX_MEDIA" | tr a-f A-F))"
    clear_empty_dir "$GAME_DIR"
    if [ -d "$GAME_DIR" ] && have_good_game "$GAME_DIR"; then
        step "$GAME_DIR is already this game: not unpacked again"
        progress_event 1 1 "" "game files already unpacked"
    else
        [ ! -e "$GAME_DIR" ] || fail extract_failed "$GAME_DIR exists and is not this game (Fable II $IMAGE_LABEL): move it away, or use another --assets folder"
        # does this disc need a title update? Known from its program alone, before the 6.5 GB are unpacked
        require_update "$ident/default.xex"
        need_kb=$(( iso_size / 1024 + 1048576 ))
        [ "$(free_kb "$ASSETS")" -ge "$need_kb" ] || fail disk_full "not enough free disk space in $ASSETS: about $((need_kb / 1048576)) GB are needed"
        staging="$ASSETS/game.new"
        remove_stale "$staging"
        add_cleanup "$staging"
        step "Unpacking $GAME (this takes a few minutes)"
        run_unpack "unpacking the disc" "$elog" "$GAME" "$staging" || fail extract_failed "unpacking the disc image failed" "$elog"
        [ -f "$staging/default.xex" ] || fail extract_failed "the unpacked disc has no default.xex" "$elog"
        need_staging=1
    fi
    ;;
inplace)
    GAME_DIR=$GAME
    ;;
copy)
    GAME_DIR="$ASSETS/game"
    clear_empty_dir "$GAME_DIR"
    if [ -d "$GAME_DIR" ] && have_good_game "$GAME_DIR"; then
        step "$GAME_DIR is already this game: not copied again"
    else
        [ ! -e "$GAME_DIR" ] || fail extract_failed "$GAME_DIR exists and is not this game (Fable II $IMAGE_LABEL): move it away, or use another --assets folder"
        x=
        [ -f "$GAME/default.xexp" ] && x="$GAME/default.xexp"
        require_update "$GAME/default.xex" "$x"
        src_dev=$(stat -L -f %d "$GAME"); dst_dev=$(stat -L -f %d "$ASSETS")
        if [ "$src_dev" != "$dst_dev" ]; then
            need_kb=$(du -sk "$GAME" | awk '{print $1}')
            [ "$(free_kb "$ASSETS")" -ge $(( need_kb + 1048576 )) ] || fail disk_full "not enough free disk space in $ASSETS: about $((need_kb / 1048576 + 1)) GB are needed"
        fi
        staging="$ASSETS/game.new"
        remove_stale "$staging"
        add_cleanup "$staging"
        step "Copying $GAME to $GAME_DIR (a clone on the same volume: no extra space)"
        progress_event 0 1 "" "copying the game files"
        if ! run_child cp -Rpc "$GAME" "$staging" >>"$elog" 2>&1; then
            # no clone (another volume, or not APFS): a real copy needs the room
            remove_stale "$staging"
            need_kb=$(du -sk "$GAME" | awk '{print $1}')
            [ "$(free_kb "$ASSETS")" -ge $(( need_kb + 1048576 )) ] || fail disk_full "not enough free disk space in $ASSETS: about $((need_kb / 1048576 + 1)) GB are needed"
            run_child cp -Rp "$GAME" "$staging" >>"$elog" 2>&1 || fail extract_failed "copying the game files failed" "$elog"
        fi
        chmod -R u+w "$staging" 2>/dev/null || true     # a read-only source (a mounted disc) gives read-only copies
        need_staging=1
    fi
    ;;
esac
SRC_FOLDER=${staging:-$GAME_DIR}      # where the folder to complete is right now
[ "$need_staging" = 1 ] || SRC_FOLDER=$GAME_DIR

# place_xexp <default.xexp> <folder>: default.xexp beside default.xex (copied through a temporary name); a different
# one that is there is kept aside, not overwritten
place_xexp() {
    local dest="$2/default.xexp"
    if [ -f "$dest" ] && ! cmp -s "$1" "$dest"; then mv "$dest" "$dest.previous-$(date +%Y%m%d-%H%M%S)"; fi
    if ! { cp "$1" "$dest.partial" && mv -f "$dest.partial" "$dest"; }; then
        rm -f "$dest.partial"
        fail extract_failed "could not write $dest (is the disk full?)"
    fi
}

# 2c. the title update. Two things come from it: default.xexp beside default.xex (needed unless the game program
# alone already gives the pinned image) and <assets>/update (data/ with the update's sound bank, and default.xexp:
# the game reads it at run time whenever the update is applied). Both are left alone when they are right.
x=
[ -f "$SRC_FOLDER/default.xexp" ] && x="$SRC_FOLDER/default.xexp"
current_hash=$(image_hash "$SRC_FOLDER/default.xex" "$x")
update_dir_ok() {   # <assets>/update holds this game folder's default.xexp and the update's data/
    [ -f "$SRC_FOLDER/default.xexp" ] && [ -f "$UPDATE_DIR/default.xexp" ] && [ -d "$UPDATE_DIR/data" ] \
        && cmp -s "$UPDATE_DIR/default.xexp" "$SRC_FOLDER/default.xexp"
}
need_xexp=0; need_update_dir=0
[ "$current_hash" = "$IMAGE_HASH" ] || need_xexp=1
if [ "$need_xexp" = 1 ] || { [ -f "$SRC_FOLDER/default.xexp" ] && ! update_dir_ok; }; then need_update_dir=1; fi
if [ "$need_xexp" = 0 ] && [ "$need_update_dir" = 0 ]; then
    step "Title update 1 is already in place ($([ -f "$SRC_FOLDER/default.xexp" ] && echo "default.xexp and $UPDATE_DIR" || echo "not needed: the game program alone gives the pinned image"))"
else
    xex_identity "$SRC_FOLDER/default.xex" || fail wrong_game "$SRC_FOLDER/default.xex could not be read"
    [ "$XEX_TITLE" = "$FABLE2_TITLE_ID" ] || fail wrong_game "this is not Fable II (title ID $(printf '%s' "$XEX_TITLE" | tr a-f A-F))"
    media=$(printf '%s' "$XEX_MEDIA" | tr a-f A-F)
    if [ -z "$UPDATE" ]; then
        if [ "$need_xexp" = 1 ]; then
            fail no_update "this disc (media ID $media) needs title update 1: give it with --update <the package, or the folder it is in>"
        fi
        fail no_update "$UPDATE_DIR is missing or does not match the game files (the update's data/): give the title update with --update <the package, or the folder it is in>"
    fi
    step "Looking for the title update for media ID $media in $UPDATE"
    progress_event 0 1 "" "looking for the title update"
    CANDIDATES=$(update_candidates | sort)
    [ -n "$CANDIDATES" ] || fail no_update "no title update for Fable II media ID $media in $UPDATE"
    picked=; unpacked=; tried_hash=
    tmp_update="$ASSETS/update.new"
    remove_stale "$tmp_update"
    add_cleanup "$tmp_update"
    while IFS= read -r line; do
        pkg=${line#* }
        pkg_kb=$(( $(stat -L -f %z "$pkg" 2>/dev/null || echo 0) / 512 + 65536 ))      # twice its size and some room
        [ "$(free_kb "$ASSETS")" -ge "$pkg_kb" ] || fail disk_full "not enough free disk space in $ASSETS to unpack the title update"
        step "Unpacking the title update $pkg"
        rm -rf "$tmp_update"
        if run_unpack "unpacking the title update" "$elog" "$pkg" "$tmp_update" && [ -f "$tmp_update/default.xexp" ]; then
            unpacked=$pkg
            # the program with this update must give the pinned image before anything is put in place
            tried_hash=$(image_hash "$SRC_FOLDER/default.xex" "$tmp_update/default.xexp")
            if [ "$tried_hash" = "$IMAGE_HASH" ]; then picked=$pkg; break; fi
            step "$pkg does not give the pinned image ($tried_hash): trying the next one"
        fi
    done <<EOF
$CANDIDATES
EOF
    [ -n "$unpacked" ] || fail no_update "the title update for media ID $media could not be unpacked" "$elog"
    [ -n "$picked" ] || wrong_game_fail "$SRC_FOLDER" "$tried_hash"
    if [ "$need_xexp" = 1 ]; then place_xexp "$tmp_update/default.xexp" "$SRC_FOLDER"; fi
    # the whole update in <assets>/update; one that differs is kept aside, never deleted
    if [ -d "$UPDATE_DIR" ] && diff -rq "$UPDATE_DIR" "$tmp_update" >/dev/null 2>&1; then
        rm -rf "$tmp_update"
    else
        [ ! -e "$UPDATE_DIR" ] || mv "$UPDATE_DIR" "$UPDATE_DIR.previous-$(date +%Y%m%d-%H%M%S)"
        mv "$tmp_update" "$UPDATE_DIR"
    fi
    drop_cleanup "$tmp_update"
fi
# the staged game must give the pinned image before it takes its place: a wrong disc is never put in place
if [ "$need_staging" = 1 ]; then
    x=
    [ -f "$staging/default.xexp" ] && x="$staging/default.xexp"
    staged_hash=$(image_hash "$staging/default.xex" "$x")
    [ "$staged_hash" = "$IMAGE_HASH" ] || wrong_game_fail "$staging" "$staged_hash"
    mv "$staging" "$GAME_DIR"
    drop_cleanup "$staging"
fi

# the clone's assets/ links to the game files
mkdir -p "$ROOT/assets"
link_to() {   # link_to <name> <target>
    local link="$ROOT/assets/$1" here
    # --assets may be this clone's own assets/: the link's place already is the target
    here=$(cd "$link" 2>/dev/null && pwd -P) || here=
    if [ -n "$here" ] && [ "$here" = "$(cd "$2" && pwd -P)" ]; then return 0; fi
    if [ -L "$link" ]; then
        rm -f "$link"; ln -s "$2" "$link"
    elif [ -e "$link" ]; then
        fail extract_failed "$link exists and is not a link: move it away"
    else
        ln -s "$2" "$link"
    fi
}
link_to game "$GAME_DIR"
if [ -d "$UPDATE_DIR" ]; then link_to update "$UPDATE_DIR"; fi
step "assets/game -> $GAME_DIR"
progress_event 1 1 "" "game files ready"
finish_stage extract

# ---- 3. check ------------------------------------------------------------------------------------------------------------
DEFAULT_ERROR=wrong_game
stage_event check "Checking the game version (image hash must be $IMAGE_HASH)"
step "Checking $GAME_DIR"
x=
[ -f "$GAME_DIR/default.xexp" ] && x="$GAME_DIR/default.xexp"
got=$(image_hash "$GAME_DIR/default.xex" "$x")
[ "$got" = "$IMAGE_HASH" ] || wrong_game_fail "$GAME_DIR" "$got"
step "image $got: $IMAGE_LABEL"
progress_event 1 1 "" "image hash ok"
finish_stage check

# ---- 4. codegen ----------------------------------------------------------------------------------------------------------
DEFAULT_ERROR=codegen_failed
# The rexglue CLI must be the SDK version the manifest names: codegen rewrites the manifest (and the stamp turns
# "+dirty") when they differ, and the generated cmake asks for the CLI's own version.
want_sdk=$(sed -n 's/^sdk_version *= *"\(.*\)"/\1/p' "$ROOT/Fable2_manifest.toml" | head -1)
cli_sdk=$("$REXGLUE_CLI" --version 2>/dev/null | head -1) || cli_sdk=
if [ -n "$want_sdk" ] && [ -n "$cli_sdk" ]; then
    case "$cli_sdk" in
    "$want_sdk"|"$want_sdk".*) ;;
    *) fail toolchain_failed "the rexglue CLI ($REXGLUE_CLI) is version $cli_sdk, but Fable2_manifest.toml names SDK $want_sdk: use the SDK at the pin (tools/mac/build_toolchains.sh)" ;;
    esac
fi
CODEGEN_KEY="$ROOT/out/codegen.key"
sha_of() { shasum -a 256 "$1" 2>/dev/null | cut -c1-64; }
want_key="image=$IMAGE_HASH manifest=$(sha_of "$ROOT/Fable2_manifest.toml") config=$(sha_of "$ROOT/Fable2_config.toml") sdk=${REXSDK_PIN:-unknown} rexglue=$(sha_of "$REXGLUE_CLI")"
if [ -f "$ROOT/generated/rexglue.cmake" ] && [ -f "$ROOT/generated/sources.cmake" ] && [ "$(cat "$CODEGEN_KEY" 2>/dev/null)" = "$want_key" ]; then
    stage_event codegen "Recompiling the game's code: skipped, the generated code is up to date"
    CODEGEN_RAN=0
else
    stage_event codegen "Recompiling the game's code (rexglue codegen; about half a minute)"
    step "rexglue codegen Fable2_manifest.toml"
    rm -f "$CODEGEN_KEY"     # until it finishes, nothing says generated/ is complete
    progress_event 0 1 "" "generating the code"
    glog="$LOGS/codegen.log"
    run_child "$REXGLUE_CLI" codegen Fable2_manifest.toml >"$glog" 2>&1 \
        || fail codegen_failed "rexglue codegen failed -- see out/logs/codegen.log" "$glog"
    [ -f "$ROOT/generated/rexglue.cmake" ] && [ -f "$ROOT/generated/sources.cmake" ] \
        || fail codegen_failed "rexglue codegen did not write generated/ -- see out/logs/codegen.log" "$glog"
    printf '%s\n' "$want_key" > "$CODEGEN_KEY"
    CODEGEN_RAN=1
    progress_event 1 1 "" "code generated"
fi
finish_stage codegen

# ---- 5. configure --------------------------------------------------------------------------------------------------------
DEFAULT_ERROR=configure_failed
stage_event configure "Setting up the build (the library submodules, cmake)"
slog="$LOGS/submodules.log"
: > "$slog"
step "Submodules: plume (recursive), XenosRecomp with smol-v, zstd, xxHash and fmt (never its dxc-bin)"
progress_event 0 1 "" "fetching the build's libraries"
# a submodule failure is download_failed only when the output shows a network error
submodule_failed() {
    if net_error_since "$slog" 0; then fail download_failed "could not fetch the build's libraries (needs the internet)" "$slog"; fi
    fail configure_failed "updating the submodules failed -- see out/logs/submodules.log" "$slog"
}
# --force when a submodule moved to another commit while it holds the patch series' changes
submodule_update() {   # submodule_update <repo> <args...>
    local repo=$1; shift
    run_child git -C "$repo" submodule update --init "$@" >>"$slog" 2>&1 \
        || run_child git -C "$repo" submodule update --init --force "$@" >>"$slog" 2>&1 \
        || submodule_failed
}
submodule_update "$ROOT" --recursive -- thirdparty/plume
submodule_update "$ROOT" -- thirdparty/XenosRecomp
# (not --recursive: that would fetch dxc-bin, a repository of prebuilt binaries)
submodule_update "$ROOT/thirdparty/XenosRecomp" -- thirdparty/smol-v thirdparty/zstd thirdparty/xxHash thirdparty/fmt
if git -C "$ROOT/thirdparty/XenosRecomp" submodule status -- thirdparty/dxc-bin 2>/dev/null | grep -qv '^-'; then
    step "note: thirdparty/XenosRecomp/thirdparty/dxc-bin is checked out; it is not used (DXC comes from the toolchains)"
else
    step "thirdparty/XenosRecomp/thirdparty/dxc-bin stays empty, as it should"
fi
# An existing build folder keeps the toolchain paths it found (rexglue_DIR, fmt_DIR, SDL3_DIR... and REEOT_DXC, which
# host_shaders.cmake sets only when empty). When the toolchains are not the ones of the last configure, its cache goes.
clog_cfg="$LOGS/configure.log"
: > "$clog_cfg"
toolchain_key="$REXSDK|$FABLE2_DXC_BUILD"
if [ -f "$BUILD/CMakeCache.txt" ] && [ "$(cat "$BUILD/.fable2-toolchains" 2>/dev/null)" != "$toolchain_key" ]; then
    step "The toolchains are not those of the last configure: its cache starts afresh"
    rm -f "$BUILD/CMakeCache.txt"
fi
step "cmake --preset mac-arm64-native (out/build/mac-arm64-native)"
progress_event 0 1 "" "configuring"
run_child cmake --preset mac-arm64-native \
    -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
    -DCMAKE_PROJECT_Fable2_INCLUDE="$ROOT/cmake/dxc_from_source.cmake" \
    -DFABLE2_DXC_SOURCE="$FABLE2_DXC_SOURCE" -DFABLE2_DXC_BUILD="$FABLE2_DXC_BUILD" \
    -Drexglue_DIR="$REXSDK/lib/cmake/rexglue" -UREEOT_DXC >>"$clog_cfg" 2>&1 \
    || {
        # (only this configure's own output: the log was emptied above)
        if net_error_since "$clog_cfg" 0; then fail download_failed "could not fetch a library the build needs (needs the internet)" "$clog_cfg"; fi
        fail configure_failed "CMake failed -- see out/logs/configure.log" "$clog_cfg"
    }
printf '%s\n' "$toolchain_key" > "$BUILD/.fable2-toolchains"
finish_stage configure

# ---- 6. compile ------------------------------------------------------------------------------------------------------------
DEFAULT_ERROR=build_failed
stage_event compile "Compiling the game (ninja, target Fable2)"
step "ninja -C out/build/mac-arm64-native Fable2"
clog="$LOGS/build.log"
: > "$clog"
ninja_pipeline() {
    local log=$1; shift
    "$@" 2>&1 | tee -a "$log" | ninja_progress
    return "${PIPESTATUS[0]}"
}
# Only what packaging needs: the game (it brings XenosRecomp, the extractor and the Vulkan libraries). The
# default target would also build the SwiftUI launcher app, which needs Xcode itself, not just its tools.
run_child ninja_pipeline "$clog" ninja -C "$BUILD" -j "$JOBS" Fable2 \
    || { grep -E "error:|FAILED" "$clog" | head -20 || true; fail build_failed "build failed -- see out/logs/build.log" "$clog"; }
# ninja numbers a step as it starts and prints nothing for the last ones: report the total
total=$(grep -o '^\[[0-9]*/[0-9]*\]' "$clog" | tail -1 | sed 's/.*\///;s/\]//')
progress_event "${total:-1}" "${total:-1}" 0 "done"
[ -x "$BUILD/Fable2" ] || fail build_failed "the build did not make $BUILD/Fable2" "$clog"
finish_stage compile
step "Done. Next: tools/mac/package_app.sh --out <Fable 2.app> --toolchains $TOOLCHAINS"
update_dir_out=
[ -d "$UPDATE_DIR" ] && update_dir_out=$UPDATE_DIR
event "{\"result\": \"ok\", \"tool\": \"setup\", \"build\": $(json_str "$BUILD"), \"assets\": $(json_str "$ASSETS"), \"game_dir\": $(json_str "$GAME_DIR"), \"update_dir\": $(json_str "$update_dir_out"), \"toolchains\": $(json_str "$TOOLCHAINS"), \"rexglue\": $(json_str "$REXGLUE_CLI"), \"dxc_lib\": $(json_str "$FABLE2_DXC_LIB"), \"codegen\": $(json_bool $CODEGEN_RAN)}"
