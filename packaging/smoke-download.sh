#!/usr/bin/env bash
# Exercise an already downloaded binary in a new scratch container.
set -euo pipefail
binary=$(realpath "${1:?expected the downloaded binary}")
engine=${CONTAINER_ENGINE:-docker}
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
install -m 755 "$binary" "$work/mkvsplit-cli"
ffmpeg -v error -f lavfi -i testsrc2=size=160x90:rate=24:duration=6 \
    -c:v libx264 -g 24 -sc_threshold 0 "$work/input.mkv"
cat > "$work/Dockerfile" <<'DOCKER'
FROM scratch AS smoke
COPY mkvsplit-cli input.mkv /
RUN ["/mkvsplit-cli", "-i", "/input.mkv", "-m", "duration", "-d", "3", "-o", "/output"]
FROM scratch
COPY --from=smoke /output/ /
DOCKER
"$engine" build --no-cache --platform linux/amd64 --output "type=local,dest=$work/result" "$work"
for part in 1 2; do
    ffmpeg -v error -xerror -i "$work/result/split_input_part$part.mkv" -f null -
done
