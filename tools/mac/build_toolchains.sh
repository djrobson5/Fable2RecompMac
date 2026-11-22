#!/bin/bash
# build_toolchains.sh -- builds, from source, the two toolchains the Fable II build needs, at pinned versions:
#
#   the ReXGlue SDK   github.com/rexglue/rexglue-sdk  at commit bd833a2a (tag v0.10.1)
#   DXC               github.com/microsoft/DirectXShaderCompiler  at v1.8.2407, with one edit to its
#                     CMakeLists.txt (cmake_policy(SET CMP0051 OLD) commented out: CMake 4 removed OLD)
#
# Nothing prebuilt is downloaded or run: both are cloned as source and compiled here. In particular DXC is
# built, never taken from the XenosRecomp submodule's dxc-bin.
#
#   tools/mac/build_toolchains.sh --into <dir> [--reuse-sdk <dir>] [--reuse-dxc <dir>] [--progress json]
#   tools/mac/build_toolchains.sh --into <dir> --check [--json]
#
#   --into <dir>       where the toolchains this script builds go (<dir>/rexglue-sdk, <dir>/DirectXShaderCompiler),
#                      and where it writes toolchains.env, the paths the other scripts read
#   --reuse-sdk <dir>  an existing ReXGlue SDK source tree, built and installed in out/install/mac-arm64
#   --reuse-dxc <dir>  an existing DirectXShaderCompiler source tree, built in out/build
#                      A reused tree is used only when it is at the pin and its outputs are there; otherwise this
#                      script says why and builds its own. A reused tree is never changed (only read with git's
#                      --no-optional-locks commands).
#   --check [--json]   changes nothing: says what is found at its pin and what would be built
#   --progress json    `@fable2 {json}` progress lines (or FABLE2_PROGRESS=json); no stage events: the caller
#                      (setup_mac.sh) announces its `toolchains` stage
#   JOBS=n             parallel compiles (default: the number of CPUs)
#
# Run again with everything built, it does nothing. A cancelled build is resumed by running it again (a toolchain
# counts as built only when its stamp, written last, is there).
#
# It prints, at the end, where the SDK install folder (REXSDK), the rexglue CLI, dxc, libdxcompiler and DXC's
# include folder are, and keeps them in <dir>/toolchains.env.
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/tools/mac/mac_common.sh"
install_traps

SDK_URL=https://github.com/rexglue/rexglue-sdk.git
SDK_PIN=bd833a2ab69d305f99ddf57418fc38ffec8af93d
SDK_TAG=v0.10.1
DXC_URL=https://github.com/microsoft/DirectXShaderCompiler.git
DXC_PIN=416fab6b5c4ba956a320d9131102304da995edfc
DXC_TAG=v1.8.2407
# the submodules DXC's build uses (googletest is for its tests only)
DXC_SUBMODULES="external/DirectX-Headers external/SPIRV-Headers external/SPIRV-Tools"
# Bump when the build recipe below changes, so an older build is made again.
RECIPE=2

INTO=
REUSE_SDK=
REUSE_DXC=
CHECK=0
WANT_JSON=0
EMBEDDED=0
JOBS=${JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || echo 4)}

while [ $# -gt 0 ]; do
    case "$1" in
    --into) INTO="${2:?--into takes a folder}"; shift 2 ;;
    --reuse-sdk) REUSE_SDK="${2:?--reuse-sdk takes a folder}"; shift 2 ;;
    --reuse-dxc) REUSE_DXC="${2:?--reuse-dxc takes a folder}"; shift 2 ;;
    --check) CHECK=1; shift ;;
    --json) WANT_JSON=1; shift ;;
    --embedded) EMBEDDED=1; shift ;;       # called by setup_mac.sh: no result line
    --progress) set_progress_mode "${2:?--progress takes text or json}"; shift 2 ;;
    --progress=*) set_progress_mode "${1#--progress=}"; shift ;;
    -h|--help) sed -n '2,/^set -e/p' "$0" | sed '$d;s/^# \{0,1\}//'; exit 0 ;;
    *) die "unknown argument: $1 (see tools/mac/build_toolchains.sh --help)" ;;
    esac
done
[ -n "$INTO" ] || die "--into <dir> is required"
[ "$WANT_JSON" = 0 ] || [ "$CHECK" = 1 ] || die "--json goes with --check"
abspath() { ( cd "$1" 2>/dev/null && pwd -P ) || printf '%s' "$1"; }
if [ "$CHECK" = 0 ]; then mkdir -p "$INTO"; fi
INTO="$(abspath "$INTO")"
[ -z "$REUSE_SDK" ] || REUSE_SDK="$(abspath "$REUSE_SDK")"
[ -z "$REUSE_DXC" ] || REUSE_DXC="$(abspath "$REUSE_DXC")"

for _r in "$REUSE_SDK" "$REUSE_DXC"; do
    [ -z "$_r" ] || case "$_r/" in "$INTO"/*) fail toolchain_failed "a --reuse path ($_r) is the --into folder or lies inside it: $INTO" ;; esac
done
unset _r
SDK_OWN="$INTO/rexglue-sdk"
DXC_OWN="$INTO/DirectXShaderCompiler"
STAMPS="$INTO/stamps"
LOGS="$INTO/logs"
ENV_FILE="$INTO/toolchains.env"

# ---- what is where (read only) ------------------------------------------------------------------------------
# Read-only git on a tree that is not ours (a reused one). `git diff` and `git status` may rewrite .git/index when its
# stat data is stale, and --no-optional-locks does not stop that, so every read goes through a private copy of the
# index. Called as: git_ro -C <tree> <git args...>
git_ro() {
    local tree=$2 idx tmp rc=0
    idx=$(git --no-optional-locks -C "$tree" rev-parse --git-path index 2>/dev/null) || idx=
    case "$idx" in /*) ;; *) idx="$tree/$idx" ;; esac
    tmp=$(mktemp "${TMPDIR:-/tmp}/fable2-ro-index.XXXXXX") || return 1
    cp "$idx" "$tmp" 2>/dev/null || true
    GIT_INDEX_FILE=$tmp git --no-optional-locks "$@" || rc=$?
    rm -f "$tmp"
    return $rc
}
has_arm64() { lipo -archs "$1" 2>/dev/null | tr ' ' '\n' | grep -qx arm64; }

sdk_install_of() { printf '%s/out/install/mac-arm64' "$1"; }
sdk_outputs_missing() {   # <source tree>: prints the first missing output, if any
    local inst f; inst=$(sdk_install_of "$1")
    for f in bin/rexglue lib/cmake/rexglue/rexglueConfig.cmake lib/librexruntime.dylib lib/libvulkan.1.dylib lib/libMoltenVK.dylib; do
        [ -e "$inst/$f" ] || { printf '%s' "$inst/$f"; return 0; }
    done
    for f in bin/rexglue lib/librexruntime.dylib; do
        has_arm64 "$inst/$f" || { printf 'arm64 code in %s' "$inst/$f"; return 0; }
    done
}
dxc_outputs_missing() {   # <source tree>
    local f
    for f in out/build/bin/dxc out/build/lib/libdxcompiler.dylib include/dxc/dxcapi.h; do
        [ -e "$1/$f" ] || { printf '%s' "$1/$f"; return 0; }
    done
    has_arm64 "$1/out/build/lib/libdxcompiler.dylib" || printf 'arm64 code in %s' "$1/out/build/lib/libdxcompiler.dylib"
}
# submodule_problem <tree> <depth> <required top-level paths, or * for all>: prints the first submodule that is not
# checked out at the commit the tree records (read only: the recorded commit comes from `git ls-tree HEAD`).
# At the top level the required ones must be checked out; nested ones are only compared when they are.
submodule_problem() {
    local tree=$1 depth=$2 required=$3 path want have r needed
    [ -f "$tree/.gitmodules" ] || return 0
    while IFS= read -r path; do
        [ -n "$path" ] || continue
        want=$(git_ro -C "$tree" ls-tree HEAD -- "$path" 2>/dev/null | awk '$2 == "commit" {print $3}')
        [ -n "$want" ] || continue
        if [ ! -e "$tree/$path/.git" ]; then
            needed=0
            if [ "$depth" = 0 ]; then
                if [ "$required" = '*' ]; then needed=1; else for r in $required; do [ "$r" = "$path" ] && needed=1; done; fi
            fi
            [ "$needed" = 0 ] || { printf '%s is not checked out' "$path"; return 0; }
            continue
        fi
        have=$(git_ro -C "$tree/$path" rev-parse HEAD 2>/dev/null) || have=
        [ "$have" = "$want" ] || { printf '%s is at %s, not %s' "$path" "${have:0:8}" "${want:0:8}"; return 0; }
        have=$(submodule_problem "$tree/$path" $((depth + 1)) '')
        [ -z "$have" ] || { printf '%s' "$have"; return 0; }
    done <<EOF
$(git config -f "$tree/.gitmodules" --get-regexp '^submodule\..*\.path$' 2>/dev/null | awk '{print $2}')
EOF
}
head_of() { git_ro -C "$1" rev-parse HEAD 2>/dev/null || true; }
tracked_dirt() {   # <tree>: tracked files that differ from HEAD (submodules not looked at)
    git_ro -C "$1" status --porcelain --untracked-files=no --ignore-submodules=all 2>/dev/null || true
}

# why_not_sdk <tree>: nothing printed when it is at the pin and built; else why not
why_not_sdk() {
    local tree=$1 head dirt missing
    [ -d "$tree" ] || { printf 'not a folder'; return; }
    head=$(head_of "$tree")
    [ -n "$head" ] || { printf 'not a git checkout'; return; }
    [ "$head" = "$SDK_PIN" ] || { printf 'it is at %s, not %s' "${head:0:8}" "${SDK_PIN:0:8}"; return; }
    dirt=$(tracked_dirt "$tree")
    [ -z "$dirt" ] || { printf 'its tracked files are changed'; return; }
    missing=$(submodule_problem "$tree" 0 '*')
    [ -z "$missing" ] || { printf 'a submodule is off the recorded commit: %s' "$missing"; return; }
    missing=$(sdk_outputs_missing "$tree")
    [ -z "$missing" ] || printf 'not built and installed (missing %s)' "$missing"
}

# DXC's one allowed local change: the CMP0051 line commented out in CMakeLists.txt.
dxc_dirt_is_the_edit() {
    local tree=$1 names bad
    names=$(git_ro -C "$tree" status --porcelain --untracked-files=no --ignore-submodules=all 2>/dev/null | sed 's/^...//')
    [ -z "$names" ] && return 0
    [ "$names" = CMakeLists.txt ] || return 1
    bad=$(git_ro -C "$tree" diff -U0 HEAD -- CMakeLists.txt | grep -E '^[-+]' | grep -vE '^(---|\+\+\+)' \
          | grep -vE '^-[[:space:]]*cmake_policy\(SET CMP0051 OLD\)$' | grep -vE '^\+[[:space:]]*#[[:space:]]*cmake_policy\(SET CMP0051 OLD\)' || true)
    [ -z "$bad" ]
}
why_not_dxc() {
    local tree=$1 head missing
    [ -d "$tree" ] || { printf 'not a folder'; return; }
    head=$(head_of "$tree")
    [ -n "$head" ] || { printf 'not a git checkout'; return; }
    if [ "$head" != "$DXC_PIN" ]; then
        printf 'it is at %s, not %s (%s)' "${head:0:8}" "${DXC_PIN:0:8}" "$DXC_TAG"; return
    fi
    dxc_dirt_is_the_edit "$tree" || { printf 'its tracked files are changed beyond the CMP0051 edit'; return; }
    missing=$(submodule_problem "$tree" 0 "$DXC_SUBMODULES")
    [ -z "$missing" ] || { printf 'a submodule is off the recorded commit: %s' "$missing"; return; }
    missing=$(dxc_outputs_missing "$tree")
    [ -z "$missing" ] || printf 'not built (missing %s)' "$missing"
}

stamp_ok() {   # <name> <expected text>
    [ "$(cat "$STAMPS/$1" 2>/dev/null)" = "$2" ]
}
SDK_STAMP="sdk $SDK_PIN recipe $RECIPE"
DXC_STAMP="dxc $DXC_PIN recipe $RECIPE"
sdk_own_ok() { stamp_ok sdk.ok "$SDK_STAMP" && [ -z "$(sdk_outputs_missing "$SDK_OWN")" ]; }
dxc_own_ok() { stamp_ok dxc.ok "$DXC_STAMP" && [ -z "$(dxc_outputs_missing "$DXC_OWN")" ]; }

# Resolve: SDK_STATE / DXC_STATE = reused | built | build; SDK_TREE / DXC_TREE; SDK_NOTE / DXC_NOTE.
SDK_STATE= DXC_STATE= SDK_TREE= DXC_TREE= SDK_NOTE= DXC_NOTE=
resolve() {
    local why
    if [ -n "$REUSE_SDK" ]; then
        why=$(why_not_sdk "$REUSE_SDK")
        if [ -z "$why" ]; then
            SDK_STATE=reused; SDK_TREE=$REUSE_SDK; SDK_NOTE="reusing $REUSE_SDK (at ${SDK_PIN:0:8}, built)"
        else
            SDK_NOTE="not reusing $REUSE_SDK: $why; "
        fi
    fi
    if [ -z "$SDK_STATE" ]; then
        SDK_TREE=$SDK_OWN
        if sdk_own_ok; then SDK_STATE=built; SDK_NOTE="${SDK_NOTE}built here (${SDK_PIN:0:8})"
        else SDK_STATE=build; SDK_NOTE="${SDK_NOTE}will be built at ${SDK_PIN:0:8}"; fi
    fi
    if [ -n "$REUSE_DXC" ]; then
        why=$(why_not_dxc "$REUSE_DXC")
        if [ -z "$why" ]; then
            DXC_STATE=reused; DXC_TREE=$REUSE_DXC; DXC_NOTE="reusing $REUSE_DXC ($DXC_TAG, built)"
        else
            DXC_NOTE="not reusing $REUSE_DXC: $why; "
        fi
    fi
    if [ -z "$DXC_STATE" ]; then
        DXC_TREE=$DXC_OWN
        if dxc_own_ok; then DXC_STATE=built; DXC_NOTE="${DXC_NOTE}built here ($DXC_TAG)"
        else DXC_STATE=build; DXC_NOTE="${DXC_NOTE}will be built at $DXC_TAG"; fi
    fi
}

resolve_dev_tools() {   # /usr/bin/clang and /usr/bin/git are shims that start an installer when there are no developer tools
    local dir; dir=$(xcode-select -p 2>/dev/null) || dir=
    [ -n "$dir" ] && [ -d "$dir" ] && xcrun --find clang >/dev/null 2>&1
}
if ! resolve_dev_tools; then fail no_clang "Apple's compiler is missing: install the Xcode command-line tools (xcode-select --install)"; fi
resolve

if [ "$CHECK" = 1 ]; then
    trap - EXIT
    sdk_ok=0; dxc_ok=0
    [ "$SDK_STATE" = build ] || sdk_ok=1
    [ "$DXC_STATE" = build ] || dxc_ok=1
    if [ "$WANT_JSON" = 1 ]; then
        printf '{"ok": true, "message": %s, "sdk": {"ok": %s, "state": %s, "message": %s, "dir": %s}, "dxc": {"ok": %s, "state": %s, "message": %s, "dir": %s}}\n' \
            "$(json_str "ReXGlue SDK: $SDK_NOTE; DXC: $DXC_NOTE")" \
            "$(json_bool $sdk_ok)" "$(json_str "$SDK_STATE")" "$(json_str "$SDK_NOTE")" "$(json_str "$SDK_TREE")" \
            "$(json_bool $dxc_ok)" "$(json_str "$DXC_STATE")" "$(json_str "$DXC_NOTE")" "$(json_str "$DXC_TREE")"
    else
        printf '%-6s %-12s %s\n' ReXGlue "$SDK_STATE" "$SDK_NOTE"
        printf '%-6s %-12s %s\n' DXC "$DXC_STATE" "$DXC_NOTE"
    fi
    exit 0
fi

# ---- building (only what resolve said to build) ------------------------------------------------------------------
DEFAULT_ERROR=toolchain_failed
mkdir -p "$STAMPS" "$LOGS"
SCALE=1000
PROGRESS_SCALE=$SCALE

need_tool() { command -v "$1" >/dev/null 2>&1 || fail missing_packages "$1 is needed to build the toolchains ($2)"; }
need_tool git "Xcode command-line tools"
need_tool cmake "brew install cmake"
need_tool ninja "brew install ninja"

phase() {   # phase <note>: a progress event at the bar's current place
    step "$1"
    progress_event "$PROGRESS_BASE" "$SCALE" "" "$1"
}
# in_logged <log> <command...>: the command in its own process group, output to the log
in_logged() {
    local log=$1; shift
    run_child "$@" >>"$log" 2>&1
}
# ninja_logged <log> <command...>: the command, output to the log and through the progress filter
ninja_logged_cmd() {
    local log=$1; shift
    "$@" 2>&1 | tee -a "$log" | ninja_progress
    return "${PIPESTATUS[0]}"
}
run_ninja() { run_child ninja_logged_cmd "$@"; }

# A tree this script makes carries a marker file. It never removes, or builds into, a folder that exists and is not
# empty unless that marker is in it: a folder of the user's own at the same place is left alone.
MARKER=.fable2-toolchain
own_dir_ok() {   # <dir>: 0 when it is absent, empty or marked
    [ -e "$1" ] || return 0
    [ -f "$1/$MARKER" ] && return 0
    [ -d "$1" ] && [ -z "$(ls -A "$1" 2>/dev/null)" ]
}
safe_rm() {   # <dir>: only a marked tree inside --into
    case "$1" in "$INTO"/*) ;; *) die "refusing to remove $1: it is not inside $INTO" ;; esac
    [ -f "$1/$MARKER" ] || die "refusing to remove $1: it has no $MARKER (not made by this script)"
    rm -rf "$1"
}

# clone_pinned <url> <dir> <sha> <tag> <log>: a shallow checkout of exactly <sha> (fetched by hash, else by tag)
clone_pinned() {
    local url=$1 dir=$2 sha=$3 tag=$4 log=$5 mark
    own_dir_ok "$dir" || fail toolchain_failed "$dir exists and was not made by this script (no $MARKER in it): move it away, or use another --into folder"
    if [ -f "$dir/$MARKER" ] && [ "$(head_of "$dir")" = "$sha" ]; then fetch_tag "$url" "$dir" "$sha" "$tag" "$log"; return 0; fi
    [ ! -f "$dir/$MARKER" ] || safe_rm "$dir"
    mkdir -p "$dir"
    printf 'made by tools/mac/build_toolchains.sh\n' > "$dir/$MARKER"
    git -C "$dir" init -q
    git -C "$dir" remote add origin "$url"
    mark=$(log_size "$log")
    if ! in_logged "$log" git -C "$dir" fetch --depth 1 origin "$sha"; then
        if ! in_logged "$log" git -C "$dir" fetch --depth 1 origin "refs/tags/$tag:refs/tags/$tag"; then
            if net_error_since "$log" "$mark"; then fail download_failed "could not fetch $url at $tag (needs the internet)" "$log"; fi
            fail toolchain_failed "could not fetch $url at $tag" "$log"
        fi
        sha=$(git -C "$dir" rev-parse "refs/tags/$tag^{commit}")
        [ "$sha" = "$3" ] || fail toolchain_failed "$url tag $tag is at ${sha:0:8}, not the pinned ${3:0:8}" "$log"
        git -C "$dir" checkout -q "$sha"
    else
        git -C "$dir" checkout -q FETCH_HEAD
    fi
    [ "$(head_of "$dir")" = "$3" ] || fail toolchain_failed "the checkout of $url is not at ${3:0:8}" "$log"
    fetch_tag "$url" "$dir" "$sha" "$tag" "$log"
}
# The pin's release tag must be in the checkout: the SDK takes its version from `git describe` (v0.10.1 at the pin;
# without the tag it calls itself 0.10.0.0-dev.unknown, and codegen then rewrites the project's manifest).
fetch_tag() {   # <url> <dir> <sha> <tag> <log>
    local url=$1 dir=$2 sha=$3 tag=$4 log=$5 got
    got=$(git -C "$dir" rev-parse -q --verify "refs/tags/$tag^{commit}" 2>/dev/null) || got=
    if [ -z "$got" ]; then
        in_logged "$log" git -C "$dir" fetch --depth 1 origin "refs/tags/$tag:refs/tags/$tag" \
            || fail toolchain_failed "could not fetch the tag $tag of $url" "$log"
        got=$(git -C "$dir" rev-parse -q --verify "refs/tags/$tag^{commit}" 2>/dev/null) || got=
    fi
    [ "$got" = "$sha" ] || fail toolchain_failed "the tag $tag of $url is at ${got:0:8}, not the pinned ${sha:0:8}" "$log"
}

# guard_gitmodules <dir> <log>: refuse a submodule that is a repository of prebuilt binaries (by its url or path)
guard_gitmodules() {
    [ -f "$1/.gitmodules" ] || return 0
    if git config -f "$1/.gitmodules" --get-regexp '^submodule\..*\.(url|path)$' | grep -Eqi '(-bin|prebuilt|binaries)(\.git)?$'; then
        fail toolchain_failed "$1 lists a prebuilt-binary submodule: refusing to fetch it" "$2"
    fi
}
# update_submodules <dir> <log> [paths...]: one level at a time, so each level's .gitmodules is checked before its
# submodules are fetched; the checked-out ones are then looked into the same way (nested submodules)
update_submodules() {
    local dir=$1 log=$2 mark p; shift 2
    guard_gitmodules "$dir" "$log"
    mark=$(log_size "$log")
    in_logged "$log" git -C "$dir" submodule update --init --depth 1 --jobs 4 "$@" \
        || in_logged "$log" git -C "$dir" submodule update --init --jobs 4 "$@" \
        || {
            if net_error_since "$log" "$mark"; then fail download_failed "could not fetch the libraries of $dir (needs the internet)" "$log"; fi
            fail toolchain_failed "could not fetch the libraries of $dir" "$log"
        }
    [ -f "$dir/.gitmodules" ] || return 0
    while IFS= read -r p; do
        [ -n "$p" ] || continue
        if [ $# -gt 0 ]; then case " $* " in *" $p "*) ;; *) continue ;; esac; fi
        if [ -e "$dir/$p/.git" ] && [ -f "$dir/$p/.gitmodules" ]; then update_submodules "$dir/$p" "$log"; fi
    done <<EOF
$(git config -f "$dir/.gitmodules" --get-regexp '^submodule\..*\.path$' | awk '{print $2}')
EOF
}

build_sdk() {
    local log="$LOGS/sdk.log" install_dir
    : > "$log"
    PROGRESS_LABEL="ReXGlue SDK: "
    rm -f "$STAMPS/sdk.ok"
    phase "ReXGlue SDK: fetching the source (${SDK_PIN:0:8})"
    clone_pinned "$SDK_URL" "$SDK_OWN" "$SDK_PIN" "$SDK_TAG" "$log"
    # every submodule is a source repository (none holds prebuilt binaries); each level is checked before it is fetched
    phase "ReXGlue SDK: fetching its libraries (submodules)"
    update_submodules "$SDK_OWN" "$log"
    phase "ReXGlue SDK: setting up the build (cmake; the first run downloads source of MoltenVK's libraries)"
    # CI's flags: Apple's compiler by path (a Homebrew llvm first on PATH would be picked otherwise) and the
    # deployment target the release builds use.
    in_logged "$log" cmake -S "$SDK_OWN" --preset mac-arm64 -DCMAKE_C_COMPILER=/usr/bin/clang \
        -DCMAKE_CXX_COMPILER=/usr/bin/clang++ -DCMAKE_OSX_DEPLOYMENT_TARGET=13.3 \
        || fail toolchain_failed "configuring the ReXGlue SDK failed" "$log"
    phase "ReXGlue SDK: compiling"
    run_ninja "$log" cmake --build "$SDK_OWN/out/build/mac-arm64" --config Release --target install --parallel "$JOBS" \
        || fail toolchain_failed "building the ReXGlue SDK failed" "$log"
    install_dir=$(sdk_outputs_missing "$SDK_OWN")
    [ -z "$install_dir" ] || fail toolchain_failed "the ReXGlue SDK build did not make $install_dir" "$log"
    printf '%s\n' "$SDK_STAMP" > "$STAMPS/sdk.ok"
}

DXC_LINK=
remove_dxc_link() { [ -z "$DXC_LINK" ] || rm -f "$DXC_LINK"; }
CLEANUP_HOOK=remove_dxc_link
build_dxc() {
    local log="$LOGS/dxc.log" missing
    : > "$log"
    PROGRESS_LABEL="DXC: "
    rm -f "$STAMPS/dxc.ok"
    phase "DXC: fetching the source ($DXC_TAG)"
    clone_pinned "$DXC_URL" "$DXC_OWN" "$DXC_PIN" "$DXC_TAG" "$log"
    # DXC's own submodules by name (googletest is for its tests only)
    # shellcheck disable=SC2086
    update_submodules "$DXC_OWN" "$log" $DXC_SUBMODULES
    # The one edit: CMake 4 removed the OLD behaviour of policy CMP0051.
    if grep -qE '^[[:space:]]*cmake_policy\(SET CMP0051 OLD\)' "$DXC_OWN/CMakeLists.txt"; then
        sed -i '' -E 's|^([[:space:]]*)cmake_policy\(SET CMP0051 OLD\)|\1# cmake_policy(SET CMP0051 OLD)  # local: CMake 4 removed OLD|' "$DXC_OWN/CMakeLists.txt"
    fi
    grep -qE '^[[:space:]]*# cmake_policy\(SET CMP0051 OLD\)' "$DXC_OWN/CMakeLists.txt" \
        || fail toolchain_failed "could not apply the CMP0051 edit to DXC's CMakeLists.txt" "$log"
    # DXC's own CMake breaks on a space in its path (utils/version's generated-file command is not quoted). A tree
    # under a folder with a space in its name (Application Support) is built through a link without one, so every path
    # DXC records is free of spaces; the built files stay where they are and the link is removed afterwards.
    local root=$DXC_OWN
    case "$DXC_OWN" in
    *[[:space:]]*)
        DXC_LINK="${TMPDIR:-/tmp}"
        case "$DXC_LINK" in *[[:space:]]*) DXC_LINK=/tmp ;; esac
        DXC_LINK="$DXC_LINK/fable2-dxc-$(printf '%s' "$DXC_OWN" | cksum | cut -d' ' -f1)"
        rm -f "$DXC_LINK"
        ln -s "$DXC_OWN" "$DXC_LINK" || fail toolchain_failed "could not make a link without a space in its path for the DXC build" "$log"
        root=$DXC_LINK
        step "DXC: the path has a space: building through $DXC_LINK"
        # a build folder made through another path (an older run) cannot be reused
        [ ! -f "$DXC_OWN/out/build/CMakeCache.txt" ] || grep -qF "CMAKE_HOME_DIRECTORY:INTERNAL=$DXC_LINK" "$DXC_OWN/out/build/CMakeCache.txt" || rm -rf "$DXC_OWN/out/build"
        ;;
    esac
    phase "DXC: setting up the build (cmake)"
    in_logged "$log" cmake -S "$root" -B "$root/out/build" -G Ninja -C "$root/cmake/caches/PredefinedParams.cmake" \
        -DSPIRV_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_CXX_FLAGS=-Wno-invalid-specialization \
        -DCLANG_FORMAT_EXE=CLANG_FORMAT_EXE-NOTFOUND \
        || fail toolchain_failed "configuring DXC failed" "$log"
    phase "DXC: compiling (dxc and libdxcompiler)"
    run_ninja "$log" ninja -C "$root/out/build" -j "$JOBS" dxc dxcompiler \
        || fail toolchain_failed "building DXC failed" "$log"
    missing=$(dxc_outputs_missing "$DXC_OWN")
    [ -z "$missing" ] || fail toolchain_failed "the DXC build did not make $missing" "$log"
    printf '%s\n' "$DXC_STAMP" > "$STAMPS/dxc.ok"
}

# Where the bar goes when both are built: the SDK first (its share is about what the two took on an M5 Pro:
# 85 s and 96 s; ninja's counts fill each part).
if [ "$SDK_STATE" = build ] && [ "$DXC_STATE" = build ]; then
    SDK_SHARE=450; DXC_SHARE=550
else
    SDK_SHARE=$SCALE; DXC_SHARE=$SCALE
fi
if [ "$SDK_STATE" = build ]; then
    SECONDS=0
    PROGRESS_BASE=0; PROGRESS_SPAN=$SDK_SHARE
    build_sdk
    step "ReXGlue SDK built in $((SECONDS / 60)) min $((SECONDS % 60)) s"
    SDK_STATE=built; SDK_NOTE="built here ($((SECONDS / 60)) min $((SECONDS % 60)) s)"
fi
if [ "$DXC_STATE" = build ]; then
    SECONDS=0
    if [ "$SDK_SHARE" != "$SCALE" ]; then PROGRESS_BASE=$SDK_SHARE; PROGRESS_SPAN=$DXC_SHARE; else PROGRESS_BASE=0; PROGRESS_SPAN=$SCALE; fi
    build_dxc
    step "DXC built in $((SECONDS / 60)) min $((SECONDS % 60)) s"
    DXC_STATE=built; DXC_NOTE="built here ($((SECONDS / 60)) min $((SECONDS % 60)) s)"
fi

# ---- the paths, for setup_mac.sh and package_app.sh -------------------------------------------------------------------
REXSDK_DIR=$(sdk_install_of "$SDK_TREE")
tmp=$(mktemp "$INTO/.toolchains.env.XXXXXX")
{
    printf '# written by tools/mac/build_toolchains.sh: where the toolchains are\n'
    printf 'REXSDK=%q\n' "$REXSDK_DIR"
    printf 'REXGLUE_CLI=%q\n' "$REXSDK_DIR/bin/rexglue"
    printf 'REXSDK_SOURCE=%q\n' "$SDK_TREE"
    printf 'REXSDK_PIN=%q\n' "$SDK_PIN"
    printf 'REXSDK_ORIGIN=%q\n' "$SDK_STATE"
    printf 'FABLE2_DXC_SOURCE=%q\n' "$DXC_TREE"
    printf 'FABLE2_DXC_BUILD=%q\n' "$DXC_TREE/out/build"
    printf 'FABLE2_DXC_BIN=%q\n' "$DXC_TREE/out/build/bin/dxc"
    printf 'FABLE2_DXC_LIB=%q\n' "$DXC_TREE/out/build/lib/libdxcompiler.dylib"
    printf 'FABLE2_DXC_INCLUDE=%q\n' "$DXC_TREE/include/dxc"
    printf 'FABLE2_DXC_PIN=%q\n' "$DXC_PIN"
    printf 'FABLE2_DXC_ORIGIN=%q\n' "$DXC_STATE"
} > "$tmp"
if cmp -s "$tmp" "$ENV_FILE" 2>/dev/null; then rm -f "$tmp"; else mv "$tmp" "$ENV_FILE"; fi

step "ReXGlue SDK:  $SDK_NOTE"
step "DXC:          $DXC_NOTE"
printf 'REXSDK          %s\n' "$REXSDK_DIR"
printf 'rexglue CLI     %s\n' "$REXSDK_DIR/bin/rexglue"
printf 'dxc             %s\n' "$DXC_TREE/out/build/bin/dxc"
printf 'libdxcompiler   %s\n' "$DXC_TREE/out/build/lib/libdxcompiler.dylib"
printf 'DXC include     %s\n' "$DXC_TREE/include/dxc"
printf 'paths kept in   %s\n' "$ENV_FILE"
if [ "$EMBEDDED" = 0 ]; then
    event "{\"result\": \"ok\", \"tool\": \"toolchains\", \"rexsdk\": $(json_str "$REXSDK_DIR"), \"rexglue\": $(json_str "$REXSDK_DIR/bin/rexglue"), \"dxc\": $(json_str "$DXC_TREE/out/build/bin/dxc"), \"dxc_lib\": $(json_str "$DXC_TREE/out/build/lib/libdxcompiler.dylib"), \"dxc_include\": $(json_str "$DXC_TREE/include/dxc"), \"env\": $(json_str "$ENV_FILE")}"
fi
