#!/bin/bash
# Builds "Fable 2.app" (bundle org.rexglue.fable2) from the build tree: the game itself,
# not a launcher, so a launcher such as Wayfarer can start it with no arguments.
#
#   tools/mac/package_app.sh [--out <folder>]      default: ~/Games/Fable 2
#
# The app goes to <folder>/Fable 2.app, and the game files are expected beside it in
# <folder>/assets/{game,update}. Needs the build in out/build/mac-arm64-native, the
# locally built rexglue CLI (to take the achievement icons from the player's own
# default.xex), and a DirectX Shader Compiler built from source (libdxcompiler).
#
# Nothing is written inside the app at run time: settings, saves and logs go to
# ~/Library/Application Support/Fable2, caches to ~/Library/Caches/Fable2.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="${FABLE2_BUILD_DIR:-$ROOT/out/build/mac-arm64-native}"
REXGLUE="${REXGLUE_CLI:-$HOME/Projects/rexglue-sdk/out/install/mac-arm64/bin/rexglue}"
DXC_LIB="${FABLE2_DXC_LIB:-$HOME/Projects/DirectXShaderCompiler/out/build/lib/libdxcompiler.dylib}"
XEX="${FABLE2_XEX:-$ROOT/assets/game/default.xex}"
OUT="$HOME/Games/Fable 2"
LSREGISTER=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister

while [ $# -gt 0 ]; do
    case "$1" in
        --out) OUT="${2:?--out needs a folder}"; shift 2 ;;
        -h|--help) sed -n 2,12p "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

die() { echo "package_app: $*" >&2; exit 1; }
need_file() { [ -f "$1" ] || die "missing $1 ($2)"; }

need_file "$BUILD/Fable2" "build the game first: cmake --build out/build/mac-arm64-native"
need_file "$BUILD/librexruntime.dylib" "the SDK runtime, staged by the build"
need_file "$BUILD/vulkan/lib/libvulkan.1.dylib" "the Vulkan loader, staged by the build"
need_file "$BUILD/vulkan/lib/libMoltenVK.dylib" "MoltenVK, staged by the build"
need_file "$BUILD/vulkan/share/vulkan/icd.d/MoltenVK_icd.json" "the MoltenVK ICD file, staged by the build"
need_file "$BUILD/XenosRecomp/XenosRecomp/XenosRecomp" "the shader translator (the build makes it)"
need_file "$ROOT/thirdparty/XenosRecomp/XenosRecomp/shader_common.h" "shader_common.h"
need_file "$DXC_LIB" "libdxcompiler, built from source (cmake/local/dxc_from_source.cmake)"
need_file "$ROOT/tools/launcher/AppIcon.icns" "the app icon"
[ -x "$REXGLUE" ] || die "the rexglue CLI is missing: $REXGLUE (build the SDK, or set REXGLUE_CLI)"
need_file "$XEX" "the game's default.xex, for the achievement icons (set FABLE2_XEX or link assets/game)"

mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
STAGE="$(mktemp -d "$OUT/.package-XXXXXX")"
APP="$STAGE/Fable 2.app"
C="$APP/Contents"
cleanup() {
    "$LSREGISTER" -u "$APP" >/dev/null 2>&1 || true
    rm -rf "$STAGE"
}
trap cleanup EXIT

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

# ---- achievement icons, renamed from image id to achievement id ----
WORK="$STAGE/achievements-work"
mkdir -p "$WORK/cwd" "$WORK/out"
( cd "$WORK/cwd" && "$REXGLUE" init --project-name fable2 --xex-path "$XEX" achievements "$XEX" "$WORK/out" ) \
    > "$WORK/rexglue.log" 2>&1 || { cat "$WORK/rexglue.log" >&2; die "rexglue could not read the achievements from $XEX"; }
python3 -I - "$WORK/out" "$C/Resources/achievements" <<'PY' || die "achievement icons"
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

# ---- rpaths: nothing may point outside the bundle ----
fix_rpaths() {  # drop every absolute rpath, then add the given ones
    local f="$1"; shift
    local p
    while IFS= read -r p; do
        install_name_tool -delete_rpath "$p" "$f"
    done < <(otool -l "$f" | awk '/cmd LC_RPATH/{r=1} r&&/ path /{print $2; r=0}' | grep '^/' || true)
    for p in "$@"; do
        otool -l "$f" | awk '/cmd LC_RPATH/{r=1} r&&/ path /{print $2; r=0}' | grep -qxF "$p" \
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
COMMIT="$(git -C "$ROOT" describe --always --dirty=+dirty)"
BASE="$(git -C "$ROOT" merge-base HEAD origin/main)" || die "no merge-base with origin/main (origin is upstream: itar3/Fable2RecompMac)"
COUNT="$(git -C "$ROOT" rev-list --count HEAD)"
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
	<key>Fable2UpstreamBase</key>
	<string>$BASE</string>
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
    done < <({ otool -L "$f" | tail -n +2 | awk '{print $1}'
               otool -D "$f" | tail -n +2
               otool -l "$f" | awk '/cmd LC_RPATH/{r=1} r&&/ path /{print $2; r=0}'; } | sort -u)
    # a dependency named @rpath/x must be in the bundle
    while IFS= read -r dep; do
        name="${dep#@rpath/}"
        [ -e "$C/Frameworks/$name" ] || [ -e "$C/Resources/$name" ] || [ -e "$C/MacOS/$name" ] \
            || [ "@rpath/$name" = "$(otool -D "$f" | tail -n +2)" ] || { echo "audit: $rel needs $dep, which is not in the bundle" >&2; BAD=1; }
    done < <(otool -L "$f" | tail -n +2 | awk '{print $1}' | grep '^@rpath/' || true)
done < <(macho_files)
[ "$BAD" = 0 ] || die "the bundle still refers to paths outside itself"

# ---- sign: libraries first, then the app ----
for f in "$C/Frameworks/"*.dylib "$C/Resources/libdxcompiler.dylib" "$C/Resources/XenosRecomp"; do
    codesign --force --sign - "$f"
done
codesign --force --sign - "$APP"
codesign --verify --deep --strict "$APP" || die "codesign --verify failed"

# ---- swap in ----
rm -rf "$STAGE/achievements-work"
"$LSREGISTER" -u "$APP" >/dev/null 2>&1 || true
FINAL="$OUT/Fable 2.app"
if [ -e "$FINAL" ]; then
    mv "$FINAL" "$STAGE/Fable 2.app.old"
fi
mv "$APP" "$FINAL"
echo "Fable 2.app: $FINAL"
echo "  Fable2SourceCommit $COMMIT"
echo "  Fable2UpstreamBase $BASE"
echo "  LSMinimumSystemVersion $MINOS"
