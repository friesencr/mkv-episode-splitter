FROM alpine:3.22@sha256:49340b346798875b5592e5d420241f2f9cfba5f47e62ebb1446a1b34734e898f AS ffmpeg
RUN apk add --no-cache build-base cmake ninja nasm pkgconf aom-dev aom-static zlib-dev zlib-static
COPY third_party/ffmpeg /src/ffmpeg
WORKDIR /build/ffmpeg
ARG JOBS=4
COPY packaging/build-ffmpeg.sh /src/build-ffmpeg.sh
RUN sh /src/build-ffmpeg.sh /src/ffmpeg /opt/ffmpeg

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
    && install -D -m 755 build/mkvsplit-cli /dist/mkvsplit-cli \
    && strip /dist/mkvsplit-cli \
    && readelf -lWd /dist/mkvsplit-cli > /tmp/elf.txt \
    && ! grep -Eq 'INTERP|\(NEEDED\)' /tmp/elf.txt

FROM scratch AS smoke
COPY --from=tested /dist/mkvsplit-cli /mkvsplit-cli
COPY --from=tested /src/splitter/build/test_run_dir/long_gop.mkv /input.mkv
RUN ["/mkvsplit-cli", "-i", "/input.mkv", "-m", "range", "-s", "5", "-e", "10", "-o", "/output"]

FROM tested AS verified
COPY --from=smoke /output/split_input_part1.mkv /tmp/smoke.mkv
RUN ffmpeg -v error -xerror -i /tmp/smoke.mkv -f null - \
    && case "$(uname -m)" in x86_64) arch=x86_64 ;; aarch64) arch=arm64 ;; *) exit 1 ;; esac \
    && cd /dist && mv mkvsplit-cli "mkvsplit-cli-linux-$arch"

FROM scratch AS artifacts
COPY --from=verified /dist/ /
