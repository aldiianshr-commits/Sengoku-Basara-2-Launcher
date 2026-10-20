#!/usr/bin/env bash
# Assemble the Windows distribution from a cross build (Linux host):
#   dist/SB2HeroesRecompiled/            launcher, tools, SDK, bundled compiler
#   dist/SB2HeroesRecompiled-<ver>-win64-portable.zip
#
# usage: packaging/package_windows.sh BUILD_DIR LLVM_MINGW_WINDOWS_ZIP [VERSION]
set -euo pipefail
BUILD=$1
ZIP=$2
VERSION=${3:-0.2.0}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DIST=$ROOT/dist
APP=$DIST/SB2HeroesRecompiled
rm -rf "$APP"
mkdir -p "$APP"
cmake --install "$BUILD" --prefix "$APP"

# Bundled compiler: llvm-mingw (UCRT, x86_64 only).
TMP=$(mktemp -d)
python3 -c "import zipfile,sys; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" "$ZIP" "$TMP"
TC=$(find "$TMP" -maxdepth 1 -mindepth 1 -type d | head -1)
# Keep only what is needed to compile and link x86_64 Windows executables.
for arch in i686 aarch64 armv7 arm64ec; do
  rm -rf "$TC/$arch-w64-mingw32" "$TC/$arch-w64-windows-gnu"
  rm -f "$TC"/bin/$arch-w64-*
done
rm -f "$TC"/bin/*uwp*
rm -rf "$TC/python" "$TC/lib/libear" "$TC/lib/libscanbuild" "$TC/share/man" "$TC/share/doc" "$TC/include/lldb" 2>/dev/null || true
for f in lldb lldb-argdumper lldb-dap lldb-instr lldb-server liblldb libpython3 clangd clang-tidy clang-format clang-scan-deps \
         analyze-build intercept-build scan-build scan-view llvm-pdbutil libclang_rt.asan libomp llvm-lto llvm-lto2; do
  rm -f "$TC"/bin/$f*
done
find "$TC/lib/clang" -type f -path '*windows*' ! -name 'libclang_rt.builtins-x86_64.a' -delete 2>/dev/null || true
find "$TC/x86_64-w64-mingw32/lib" -maxdepth 1 -name '*.dll.a' -size +5M -print 2>/dev/null | head -0
mkdir -p "$APP/toolchain"
cp -a "$TC"/. "$APP/toolchain/"
rm -rf "$TMP"

mkdir -p "$APP/licenses"
cp "$ROOT/LICENSE" "$APP/licenses/SB2H-Recompiled-LICENSE.txt" 2>/dev/null || true
[ -f "$APP/toolchain/LICENSE.TXT" ] && cp "$APP/toolchain/LICENSE.TXT" "$APP/licenses/llvm-mingw-LICENSE.txt"
cat > "$APP/licenses/THIRD_PARTY.txt" <<'TXT'
Third-party components bundled with Sengoku Basara 2 Heroes Recompiled:
- SDL3 (zlib license) - https://libsdl.org
- Dear ImGui (MIT license) - https://github.com/ocornut/imgui
- llvm-mingw: LLVM/Clang/LLD (Apache License 2.0 with LLVM exceptions), mingw-w64 (ZPL / public domain),
  libc++/libunwind (Apache 2.0 with LLVM exceptions) - https://github.com/mstorsjo/llvm-mingw
No game data, BIOS or copyrighted assets of Capcom or Sony are included.
TXT

du -sh "$APP" "$APP/toolchain"
cd "$DIST"
rm -f "SB2HeroesRecompiled-$VERSION-win64-portable.zip"
# Portable flavour keeps user data next to the executable.
touch SB2HeroesRecompiled/portable.txt
python3 - "$VERSION" <<'PY'
import os, sys, zipfile
ver = sys.argv[1]
out = f"SB2HeroesRecompiled-{ver}-win64-portable.zip"
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
    for root, _, files in os.walk("SB2HeroesRecompiled"):
        for f in files:
            p = os.path.join(root, f)
            z.write(p, p)
print(out, os.path.getsize(out) // (1 << 20), "MB")
PY
rm -f SB2HeroesRecompiled/portable.txt
