#!/usr/bin/env bash
set -eu

cli=$1
ffmpeg=$2
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
cd "$work"
"$ffmpeg" -v error -f lavfi -i testsrc2=size=320x240:rate=30 -t 6 -c:v libx264 -g 30 input.mkv

# A failed rerun must preserve a previous episode and stop before the next one.
"$cli" -i input.mkv -m duration -d 3 -o previous > /dev/null
first=split_input_part1.mkv
mkdir regular linked targets
cp "previous/$first" "regular/$first"
cp "previous/$first" targets/episode.mkv
ln -s ../targets/episode.mkv "linked/$first"

for output in regular linked; do
    if (trap '' XFSZ; ulimit -f 16; exec "$cli" -i input.mkv -m duration -d 3 -o "$output") > /dev/null 2> error.log; then
        echo "Expected the split to fail under the file size limit" >&2
        exit 1
    fi
    grep -F "$first" error.log
    cmp "previous/$first" "$output/$first"
    test "$(ls -A "$output")" = "$first"
done
test -L "linked/$first"
test "$(ls -A targets)" = "episode.mkv"

# A successful retry still writes through the link.
printf 'old target' > targets/episode.mkv
"$cli" -i input.mkv -m duration -d 3 -o linked > /dev/null
test -L "linked/$first"
test "$(wc -c < targets/episode.mkv)" -gt 16384
