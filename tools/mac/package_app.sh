#!/bin/bash
# Builds "Fable 2.app" (bundle org.rexglue.fable2) from the build tree: the game itself,
# not a launcher, so a launcher such as Wayfarer can start it with no arguments.
#
#   tools/mac/package_app.sh [--out <folder> | --out <path>.app | --out <path>.app.partial]
#                            [--seed-cache <folder> | --no-seed] [--no-data]
#                            [--toolchains <dir>] [--progress json]
#                                                  default out: ~/Games/Fable 2
#
# --out <folder>: the app goes to <folder>/Fable 2.app, and the game files are expected
# beside it in <folder>/assets/{game,update}.
# --out <path ending .app or .app.partial>: the app is made at exactly that path (a launcher
# builds a .partial beside the installed app and swaps it in after the game has quit).
# --seed-cache: where the app's translated shaders live (default ~/Library/Caches/Fable2,
# only when --out isn't given): the build folder's shader_runtime/ is copied there once,
# if that folder has none yet, so the app's first play doesn't translate every shader
# again. --no-seed skips it. --no-data touches no data or cache folder at all (it implies
# --no-seed); the app is the only thing written, and only at the --out path.
# --toolchains <dir>: the folder tools/mac/build_toolchains.sh wrote toolchains.env in; it
# names the rexglue CLI and libdxcompiler (REXGLUE_CLI and FABLE2_DXC_LIB in the environment
# win over it; without either, the dev tree's folders in ~/Projects are used).
# --progress json (or FABLE2_PROGRESS=json): `@fable2 {json}` lines for a launcher: a
# `package` stage, an error {error, message} on a failure (code `game_running` when the app
# at the target path is running; `package_failed` otherwise) and a last
#   {"result": "ok", "tool": "package", "app": path, "commit": short, "dirty": bool,
#    "upstream_base": short}                 (upstream_base only when there is one)
#
# Needs the build in out/build/mac-arm64-native (tools/mac/setup_mac.sh makes it), the
# rexglue CLI (to take the achievement icons from the player's own default.xex), and a
# DirectX Shader Compiler built from source (libdxcompiler).
#
# Info.plist stamps: Fable2SourceCommit is the short commit, plus "+dirty" when tracked files
# differ from it (the submodules' configure-time patch series does not count);
# Fable2UpstreamBase is the merge base with the remote `upstream`'s main when there is one.
#
# Contents/Resources/settings.json (the player settings a launcher draws as a page) comes
# from the built game: `Fable2 --describe-settings`.
#
# Nothing is written inside the app at run time: settings, saves and logs go to
# ~/Library/Application Support/Fable2, caches to ~/Library/Caches/Fable2.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/tools/mac/mac_common.sh"
DEFAULT_ERROR=package_failed
CLEANUP_HOOK=cleanup
install_traps
BUILD="${FABLE2_BUILD_DIR:-$ROOT/out/build/mac-arm64-native}"
TOOLCHAINS="${FABLE2_TOOLCHAINS:-}"
XEX="${FABLE2_XEX:-$ROOT/assets/game/default.xex}"
OUT="$HOME/Games/Fable 2"
EXACT=             # --out names the app itself (.app or .app.partial)
NO_DATA=
SEED="$HOME/Library/Caches/Fable2"
SEED_SET=
LSREGISTER=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister

while [ $# -gt 0 ]; do
    case "$1" in
        --out) OUT="${2:?--out needs a folder or an app path}"; OUT_SET=1; shift 2 ;;
        --seed-cache) SEED="${2:?--seed-cache needs a folder}"; SEED_SET=1; shift 2 ;;
        --no-seed) SEED=; SEED_SET=1; shift ;;
        --no-data) NO_DATA=1; SEED=; SEED_SET=1; shift ;;
        --toolchains) TOOLCHAINS="${2:?--toolchains needs the toolchains folder}"; shift 2 ;;
        --progress) set_progress_mode "${2:?--progress takes text or json}"; shift 2 ;;
        --progress=*) set_progress_mode "${1#--progress=}"; shift ;;
        -h|--help) sed -n '2,/^set -e/p' "$0" | sed '$d;s/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

die() { LAST_DIE="$*"; echo "package_app: $*" >&2; exit 1; }
need_file() { [ -f "$1" ] || die "missing $1 ($2)"; }

# the rexglue CLI and libdxcompiler: the environment, else the toolchains folder, else the dev tree's folders
if [ -n "$TOOLCHAINS" ]; then
    [ -f "$TOOLCHAINS/toolchains.env" ] || die "no toolchains.env in $TOOLCHAINS (tools/mac/build_toolchains.sh writes it)"
    _cli="${REXGLUE_CLI:-}"; _lib="${FABLE2_DXC_LIB:-}"
    # shellcheck disable=SC1091
    . "$TOOLCHAINS/toolchains.env"
    [ -z "$_cli" ] || REXGLUE_CLI=$_cli
    [ -z "$_lib" ] || FABLE2_DXC_LIB=$_lib
fi
REXGLUE="${REXGLUE_CLI:-$HOME/Projects/rexglue-sdk/out/install/mac-arm64/bin/rexglue}"
DXC_LIB="${FABLE2_DXC_LIB:-$HOME/Projects/DirectXShaderCompiler/out/build/lib/libdxcompiler.dylib}"

# where the app goes
case "$OUT" in
    *.app|*.app.partial) EXACT=1; FINAL="$OUT" ;;
    *) FINAL="$OUT/Fable 2.app" ;;
esac
if [ -n "$EXACT" ]; then OUTDIR="$(dirname "$FINAL")"; else OUTDIR="$OUT"; fi
mkdir -p "$OUTDIR"
OUTDIR="$(cd "$OUTDIR" && pwd)"
FINAL="$OUTDIR/$(basename "$FINAL")"
[ -n "$EXACT" ] || FINAL="$OUTDIR/Fable 2.app"
stage_event package "Packaging the app ($(basename "$FINAL"))"
STAGE=
SWAPPING=          # 1 between moving the old app aside and moving the new one in
cleanup() {
    if [ -n "$STAGE" ]; then
        [ -z "${APP:-}" ] || "$LSREGISTER" -u "$APP" >/dev/null 2>&1 || true
        # a stop between the two moves of the swap puts the old app back (it is deleted only after the swap worked)
        if [ "$SWAPPING" = 1 ] && [ -e "$STAGE/Fable 2.app.old" ]; then
            rm -rf "$FINAL"
            mv "$STAGE/Fable 2.app.old" "$FINAL" || echo "package_app: could not put the old app back: it is in $STAGE" >&2
            [ -e "$FINAL" ] || return 0     # keep the staging folder: the old app is in it
        fi
        rm -rf "$STAGE"
    fi
}

# only the app at the target path matters: if that one is running, stop
_procs="$(ps -axo command= 2>/dev/null || true)"
case "$_procs" in
    *"$FINAL/Contents/MacOS/"*)
        error_event game_running "the app at $FINAL is running: quit it first"
        die "the app at $FINAL is running: quit it first" ;;
esac
unset _procs

need_file "$BUILD/Fable2" "build the game first: cmake --build out/build/mac-arm64-native"
need_file "$BUILD/librexruntime.dylib" "the SDK runtime, staged by the build"
need_file "$BUILD/vulkan/lib/libvulkan.1.dylib" "the Vulkan loader, staged by the build"
need_file "$BUILD/vulkan/lib/libMoltenVK.dylib" "MoltenVK, staged by the build"
need_file "$BUILD/vulkan/share/vulkan/icd.d/MoltenVK_icd.json" "the MoltenVK ICD file, staged by the build"
need_file "$BUILD/XenosRecomp/XenosRecomp/XenosRecomp" "the shader translator (the build makes it)"
need_file "$ROOT/thirdparty/XenosRecomp/XenosRecomp/shader_common.h" "shader_common.h"
need_file "$DXC_LIB" "libdxcompiler, built from source (tools/mac/build_toolchains.sh; --toolchains or FABLE2_DXC_LIB)"
need_file "$ROOT/tools/launcher/AppIcon.icns" "the app icon"
[ -x "$REXGLUE" ] || die "the rexglue CLI is missing: $REXGLUE (tools/mac/build_toolchains.sh; --toolchains or REXGLUE_CLI)"
# the achievement list is read with tomllib (python 3.11 and up); /usr/bin/python3 is older
PYTHON=
for _c in python3.14 python3.13 python3.12 python3.11 python3; do
    _p=$(command -v "$_c" 2>/dev/null) || continue
    if "$_p" -I -c 'import tomllib' >/dev/null 2>&1; then PYTHON=$_p; break; fi
done
[ -n "$PYTHON" ] || die "python 3.11 or newer is needed (brew install python)"
need_file "$XEX" "the game's default.xex, for the achievement icons (set FABLE2_XEX or link assets/game)"

STAGE="$(mktemp -d "$OUTDIR/.package-XXXXXX")"
APP="$STAGE/Fable 2.app"
C="$APP/Contents"

mkdir -p "$C/MacOS" "$C/Frameworks" "$C/Resources/vulkan/icd.d" "$C/Resources/achievements"

# ---- files ----
cp "$BUILD/Fable2" "$C/MacOS/Fable2"
cp "$BUILD/librexruntime.dylib" "$C/Frameworks/"
cp "$BUILD/vulkan/lib/libvulkan.1.dylib" "$BUILD/vulkan/lib/libMoltenVK.dylib" "$C/Frameworks/"
# The ICD file's library path is relative to the file: Resources/vulkan/icd.d -> Frameworks.
sed 's|"library_path": *"[^"]*"|"library_path": "../../../Frameworks/libMoltenVK.dylib"|' \
    "$BUILD/vulkan/share/vulkan/icd.d/MoltenVK_icd.json" > "$C/Resources/vulkan/icd.d/MoltenVK_icd.json"
grep -q '"../../../Frameworks/libMoltenVK.dylib"' "$C/Resources/vulkan/icd.d/MoltenVK_icd.json" \
    || die "could not point the ICD file at libMoltenVK"
cp "$BUILD/XenosRecomp/XenosRecomp/XenosRecomp" "$C/Resources/XenosRecomp"
cp "$DXC_LIB" "$C/Resources/libdxcompiler.dylib"
cp "$ROOT/thirdparty/XenosRecomp/XenosRecomp/shader_common.h" "$C/Resources/"
cp "$ROOT/tools/launcher/AppIcon.icns" "$C/Resources/AppIcon.icns"
chmod 755 "$C/MacOS/Fable2" "$C/Resources/XenosRecomp"
chmod 644 "$C/Frameworks/"*.dylib "$C/Resources/libdxcompiler.dylib"

# ---- settings.json: the player settings, for a launcher (Wayfarer) to draw as a page ----
# The game prints it and exits before any window (src/app/settings_describe.cpp). A build from before that flag
# would start the game instead, so look for the flag in the binary first.
grep -aqF -- '--describe-settings' "$BUILD/Fable2" \
    || die "$BUILD/Fable2 has no --describe-settings: rebuild the game first (cmake --build out/build/mac-arm64-native)"
SETTINGS_JSON="$C/Resources/settings.json"
"$BUILD/Fable2" --describe-settings > "$SETTINGS_JSON" 2> "$STAGE/describe-settings.err" \
    || { cat "$STAGE/describe-settings.err" >&2; die "Fable2 --describe-settings failed"; }
"$PYTHON" -I -m json.tool "$SETTINGS_JSON" > /dev/null || die "settings.json is not valid JSON (Fable2 --describe-settings)"
"$PYTHON" -I - "$SETTINGS_JSON" <<'PY' || die "settings.json has no usable rows"
import json, sys
d = json.load(open(sys.argv[1]))
rows = d.get("settings")
if not isinstance(rows, list) or not rows:
    sys.exit("no settings rows")
keys = [r.get("key") for r in rows]
if len(set(keys)) != len(keys) or not all(keys):
    sys.exit("a setting key is missing or listed twice")
start = d.get("start")
if not isinstance(start, dict) or not start or not set(start) <= set(keys):
    sys.exit("start is missing, or names a setting that is not a row")
print(f"settings.json: {len(rows)} settings, {len(start)} start values")
PY

# ---- achievement icons, renamed from image id to achievement id ----
WORK="$STAGE/achievements-work"
mkdir -p "$WORK/cwd" "$WORK/out"
( cd "$WORK/cwd" && "$REXGLUE" init --project-name fable2 --xex-path "$XEX" achievements "$XEX" "$WORK/out" ) \
    > "$WORK/rexglue.log" 2>&1 || { cat "$WORK/rexglue.log" >&2; die "rexglue could not read the achievements from $XEX"; }
"$PYTHON" -I - "$WORK/out" "$C/Resources/achievements" <<'PY' || die "achievement icons"
import shutil, sys, tomllib
src, dst = sys.argv[1], sys.argv[2]
with open(f"{src}/achievements.toml", "rb") as f:
    items = tomllib.load(f).get("achievements", [])
if not items:
    sys.exit("achievements.toml lists no achievements")
for a in items:
    icon = f"{src}/icons/{a['image_id']}.png"
    try:
        shutil.copyfile(icon, f"{dst}/{a['id']}.png")
    except OSError as e:
        sys.exit(f"missing icon for achievement {a['id']}: {e}")
print(f"{len(items)} achievement icons")
PY

# otool's lines carry the whole path, which may hold spaces (a clone or a toolchains folder under "Application Support")
rpaths_of() { otool -l "$1" | awk '/cmd LC_RPATH/{r=1} r&&/ path /{sub(/^ *path /, ""); sub(/ \(offset [0-9]+\)$/, ""); print; r=0}'; }
deps_of() { otool -L "$1" | tail -n +2 | sed -e 's/^[[:space:]]*//' -e 's/ (compatibility version .*$//'; }

# ---- rpaths: nothing may point outside the bundle ----
fix_rpaths() {  # drop every absolute rpath, then add the given ones
    local f="$1"; shift
    local p
    while IFS= read -r p; do
        install_name_tool -delete_rpath "$p" "$f"
    done < <(rpaths_of "$f" | grep '^/' || true)
    for p in "$@"; do
        rpaths_of "$f" | grep -qxF "$p" \
            || install_name_tool -add_rpath "$p" "$f"
    done
}
fix_rpaths "$C/MacOS/Fable2" "@executable_path/../Frameworks"
fix_rpaths "$C/Resources/XenosRecomp" "@loader_path"
fix_rpaths "$C/Resources/libdxcompiler.dylib" "@loader_path"

# ---- Info.plist ----
macho_files() { find "$C" -type f \( -perm -u+x -o -name '*.dylib' \) -exec sh -c 'file -b "$1" | grep -q "Mach-O" && echo "$1"' _ {} \; ; }
MINOS="$(macho_files | while IFS= read -r f; do otool -l "$f" | awk '/ minos /{print $2}'; done | sort -V | tail -1)"
[ -n "$MINOS" ] || die "could not read the binaries' minimum macOS version"
git_stamp "$ROOT"
COMMIT="$STAMP_COMMIT"
[ "$STAMP_DIRTY" = 0 ] || COMMIT="$COMMIT+dirty"
BASE="$STAMP_BASE"
COUNT="$(git -C "$ROOT" rev-list --count HEAD)"
BASE_KEY=
if [ -n "$BASE" ]; then
    BASE_KEY="	<key>Fable2UpstreamBase</key>
	<string>$BASE</string>"
fi
cat > "$C/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleIdentifier</key>
	<string>org.rexglue.fable2</string>
	<key>CFBundleExecutable</key>
	<string>Fable2</string>
	<key>CFBundleName</key>
	<string>Fable II</string>
	<key>CFBundleDisplayName</key>
	<string>Fable II</string>
	<key>CFBundlePackageType</key>
	<string>APPL</string>
	<key>CFBundleIconFile</key>
	<string>AppIcon</string>
	<key>CFBundleShortVersionString</key>
	<string>1.0</string>
	<key>CFBundleVersion</key>
	<string>$COUNT</string>
	<key>LSMinimumSystemVersion</key>
	<string>$MINOS</string>
	<key>NSHighResolutionCapable</key>
	<true/>
	<key>LSApplicationCategoryType</key>
	<string>public.app-category.role-playing-games</string>
	<key>Fable2SourceCommit</key>
	<string>$COMMIT</string>
$BASE_KEY
</dict>
</plist>
EOF
plutil -lint "$C/Info.plist" >/dev/null || die "Info.plist is not valid"

# ---- audit: no Mach-O may name a path outside the bundle ----
BAD=0
while IFS= read -r f; do
    rel="${f#$APP/}"
    # install names, dependencies and rpaths
    while IFS= read -r ref; do
        case "$ref" in
            /System/*|/usr/lib/*|@rpath/*|@executable_path|@executable_path/*|@loader_path|@loader_path/*) ;;
            *) echo "audit: $rel refers to $ref" >&2; BAD=1 ;;
        esac
    done < <({ deps_of "$f"
               otool -D "$f" | tail -n +2
               rpaths_of "$f"; } | sort -u)
    # a dependency named @rpath/x must be in the bundle
    while IFS= read -r dep; do
        name="${dep#@rpath/}"
        [ -e "$C/Frameworks/$name" ] || [ -e "$C/Resources/$name" ] || [ -e "$C/MacOS/$name" ] \
            || [ "@rpath/$name" = "$(otool -D "$f" | tail -n +2)" ] || { echo "audit: $rel needs $dep, which is not in the bundle" >&2; BAD=1; }
    done < <(deps_of "$f" | grep '^@rpath/' || true)
done < <(macho_files)
[ "$BAD" = 0 ] || die "the bundle still refers to paths outside itself"

# ---- sign: libraries first, then the app ----
for f in "$C/Frameworks/"*.dylib "$C/Resources/libdxcompiler.dylib" "$C/Resources/XenosRecomp"; do
    codesign --force --sign - "$f"
done
codesign --force --sign - "$APP"
codesign --verify --deep --strict "$APP" || die "codesign --verify failed"
# (The translated-shader cache is stamped with the contents of shader_common.h, the translator and
# libdxcompiler, as signed here: runtime_stamp.h. Their dates do not matter.)

# ---- seed the translated shaders, once ----
# An app packaged elsewhere (--out, e.g. a test) seeds nothing unless --seed-cache says where.
if [ -n "${OUT_SET:-}" ] && [ -z "$SEED_SET" ]; then SEED=; fi
if [ -n "$SEED" ] && [ -d "$BUILD/shader_runtime" ] && [ ! -e "$SEED/shader_runtime" ]; then
    mkdir -p "$SEED"
    rm -rf "$SEED/shader_runtime.copying"   # a leftover from an interrupted run would nest the copy
    cp -R "$BUILD/shader_runtime" "$SEED/shader_runtime.copying"
    # Those translations came from this build's translator, the program the app holds (re-signed, so its bytes, and
    # the stamp by contents, differ). When the folder's own stamp says they fit this build's shader_common.h and
    # translator, rewrite it in the form that keeps them once (runtime_stamp.h): the hash of shader_common.h and a
    # date field of 0. A folder that is stale against the build keeps its stamp and is dropped at the first run.
    "$PYTHON" -I - "$ROOT/thirdparty/XenosRecomp/XenosRecomp/shader_common.h" "$BUILD/XenosRecomp/XenosRecomp/XenosRecomp" \
        "$SEED/shader_runtime.copying/stamp.txt" <<'PY' || true
import os, sys
def fnv(path):
    h = 1469598103934665603
    for b in open(path, "rb").read():
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return f"{h:016x}"
common, translator, stamp = sys.argv[1:4]
try:
    fields = open(stamp).read().split(" ")
except OSError:
    sys.exit(0)
want = fnv(common)
# the old form "<common> <date>", or the form by contents "<common> <translator> <compiler>" (the build tree's
# compiler is not beside its translator, so that field is not compared)
if (len(fields) == 2 and fields[0] == want) or (len(fields) == 3 and fields[0] == want and fields[1] == fnv(translator)):
    tmp = stamp + ".tmp"
    with open(tmp, "w") as f:
        f.write(f"{want} 0")
    os.replace(tmp, stamp)
PY
    mv "$SEED/shader_runtime.copying" "$SEED/shader_runtime"
    echo "seeded $SEED/shader_runtime ($(ls "$SEED/shader_runtime" | grep -c '\.f2sc$') files)"
fi

# ---- swap in ----
rm -rf "$STAGE/achievements-work"
"$LSREGISTER" -u "$APP" >/dev/null 2>&1 || true
SWAPPING=1
if [ -e "$FINAL" ]; then
    mv "$FINAL" "$STAGE/Fable 2.app.old"
fi
mv "$APP" "$FINAL"
SWAPPING=       # done: the old app in $STAGE goes with the staging folder
echo "Fable 2.app: $FINAL"
echo "  Fable2SourceCommit $COMMIT"
echo "  Fable2UpstreamBase ${BASE:-(none: no remote named upstream)}"
echo "  LSMinimumSystemVersion $MINOS"
result="{\"result\": \"ok\", \"tool\": \"package\", \"app\": $(json_str "$FINAL"), \"commit\": $(json_str "$COMMIT"), \"dirty\": $(json_bool "$STAMP_DIRTY")"
[ -z "$BASE" ] || result="$result, \"upstream_base\": $(json_str "$BASE")"
event "$result}"
