#!/bin/sh
# Run from an out-of-tree build directory with the target dependencies on PKG_CONFIG_PATH.
set -eu
source_dir=$1
prefix=$2
shift 2
"$source_dir/configure" --prefix="$prefix" --pkg-config-flags=--static \
    --disable-autodetect --disable-shared --enable-static --disable-doc --disable-debug \
    --disable-programs --disable-network --disable-avdevice --disable-avfilter \
    --disable-swscale --disable-encoders --disable-filters \
    --disable-muxers --enable-muxer=matroska --disable-demuxers --enable-demuxer=matroska \
    --disable-protocols --enable-protocol=file --disable-hwaccels --disable-decoder=av1 \
    --enable-libaom --enable-zlib "$@"
"${MAKE:-make}" -j"${JOBS:-4}"
"${MAKE:-make}" install
