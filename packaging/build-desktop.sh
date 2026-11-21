#!/usr/bin/env bash
# Build on Apple Silicon for macOS, or on Ubuntu with the x86_64 MinGW-w64 toolchain for Windows.
set -euo pipefail
target=${1:?expected macos-arm64 or windows-x86_64}
root=$(cd "$(dirname "$0")/.." && pwd)
work="$root/build/standalone-$target"
prefix="$work/prefix"
asset="$root/dist/mkvsplit-cli-$target"
aom_version=3.15.3
archive="libaom-$aom_version.tar.gz"
mkdir -p "$work/ffmpeg" "$root/dist"
cd "$work"

cmake_args=()
ffmpeg_args=()
export PKG_CONFIG_PATH="$prefix/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$PKG_CONFIG_PATH"
case "$target" in
    macos-arm64)
        test "$(uname -sm)" = 'Darwin arm64'
        export MACOSX_DEPLOYMENT_TARGET=15.0
        export MAKE=gmake
        cmake_args+=(-DCMAKE_OSX_ARCHITECTURES=arm64)
        ffmpeg_args+=(--arch=aarch64)
        ;;
    windows-x86_64)
        triple=x86_64-w64-mingw32
        sysroot="/usr/$triple"
        export PKG_CONFIG_LIBDIR="$PKG_CONFIG_PATH:$sysroot/lib/pkgconfig"
        cmake_args+=(-DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR=x86_64
            "-DCMAKE_C_COMPILER=$triple-gcc-posix" "-DCMAKE_CXX_COMPILER=$triple-g++-posix"
            "-DCMAKE_FIND_ROOT_PATH=$prefix;$sysroot" -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY
            -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER)
        ffmpeg_args+=(--enable-cross-compile --target-os=mingw32 --arch=x86_64
            "--cross-prefix=$triple-" "--cc=$triple-gcc-posix" "--cxx=$triple-g++-posix"
            --pkg-config=pkg-config --extra-ldflags=-static)
        asset+=.exe
        ;;
    *) echo "Unknown desktop target: $target" >&2; exit 1 ;;
esac

curl -fLsS --retry 3 "https://storage.googleapis.com/aom-releases/$archive" -o "$archive"
echo "12c01dc731ed7b5a999e28120b45bf58a002ba3435a4ba2cfbc1e084a27c9c04  $archive" | shasum -a 256 -c -
tar -xzf "$archive"
cmake -S "libaom-$aom_version" -B aom -G Ninja "${cmake_args[@]}" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib \
    -DBUILD_SHARED_LIBS=OFF -DCONFIG_AV1_ENCODER=0 -DENABLE_DOCS=OFF -DENABLE_TESTS=OFF \
    -DENABLE_EXAMPLES=OFF -DENABLE_TOOLS=OFF
cmake --build aom --parallel "${JOBS:-4}"
cmake --install aom
(cd ffmpeg && sh "$root/packaging/build-ffmpeg.sh" "$root/third_party/ffmpeg" "$prefix" "${ffmpeg_args[@]}")
cmake -S "$root" -B splitter -G Ninja "${cmake_args[@]}" -DCMAKE_BUILD_TYPE=Release -DMKVSPLIT_STATIC=ON
cmake --build splitter --parallel "${JOBS:-4}"

if [[ $target == macos-arm64 ]]; then
    ctest --test-dir splitter --output-on-failure
    cp splitter/mkvsplit-cli "$asset"
    strip "$asset"
    codesign --force --sign - "$asset"
    codesign --verify "$asset"
    lipo -verify_arch arm64 "$asset"
    otool -L "$asset" | awk 'NR > 1 && $1 !~ /^\/(usr\/lib|System\/Library)\// {print "Unexpected dependency:", $1; bad=1} END {exit bad}'
else
    "$triple-strip" -o "$asset" splitter/mkvsplit-cli.exe
    "$triple-objdump" -p "$asset" | awk '/DLL Name:/ {print; dll=tolower($3);
        if (dll !~ /^(advapi32|bcrypt|kernel32|msvcrt|ole32|shell32|user32|ws2_32)\.dll$/) bad=1}
        END {exit bad}'
fi
