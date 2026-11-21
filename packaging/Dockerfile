FROM alpine:3.24@sha256:f59abe656b692d901e184be3445e084786602108599d2a51d229c0f5a48c71bf AS ffmpeg
RUN apk add --no-cache build-base cmake ninja nasm pkgconf aom-dev aom-static zlib-dev zlib-static
COPY third_party/ffmpeg /src/ffmpeg
WORKDIR /build/ffmpeg
ARG JOBS=4
RUN /src/ffmpeg/configure --prefix=/opt/ffmpeg --pkg-config-flags=--static \
        --disable-autodetect --disable-shared --enable-static --disable-doc --disable-debug \
        --disable-programs --disable-network --disable-avdevice --disable-avfilter \
        --disable-swscale --disable-encoders --disable-filters \
        --disable-muxers --enable-muxer=matroska --disable-demuxers --enable-demuxer=matroska \
        --disable-protocols --enable-protocol=file --disable-hwaccels --disable-decoder=av1 \
        --enable-libaom --enable-zlib \
    && make -j"$JOBS" && make install

FROM ffmpeg AS tested
RUN apk add --no-cache bash coreutils diffutils ffmpeg
WORKDIR /src/splitter
COPY CMakeLists.txt ./
COPY include include
COPY src src
COPY tests tests
RUN PKG_CONFIG_PATH=/opt/ffmpeg/lib/pkgconfig cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DMKVSPLIT_STATIC=ON \
    && cmake --build build --parallel "$JOBS" \
    && ctest --test-dir build --output-on-failure \
    && install -D -m 755 build/mkvsplit-cli /dist/mkvsplit-cli-linux-x86_64 \
    && strip /dist/mkvsplit-cli-linux-x86_64 \
    && readelf -lWd /dist/mkvsplit-cli-linux-x86_64 > /tmp/elf.txt \
    && ! grep -Eq 'INTERP|\(NEEDED\)' /tmp/elf.txt

FROM scratch AS smoke
COPY --from=tested /dist/mkvsplit-cli-linux-x86_64 /mkvsplit-cli
COPY --from=tested /src/splitter/build/test_run_dir/long_gop.mkv /input.mkv
RUN ["/mkvsplit-cli", "-i", "/input.mkv", "-m", "range", "-s", "5", "-e", "10", "-o", "/output"]

FROM tested AS verified
COPY --from=smoke /output/split_input_part1.mkv /tmp/smoke.mkv
RUN ffmpeg -v error -xerror -i /tmp/smoke.mkv -f null - \
    && cd /dist && sha256sum mkvsplit-cli-linux-x86_64 > SHA256SUMS

FROM scratch AS artifacts
COPY --from=verified /dist/ /
