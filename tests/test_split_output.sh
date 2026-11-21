#!/usr/bin/env bash
set -euo pipefail

cli=$1
ffmpeg=$2
ffprobe=$3
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
cd "$work"

echo ';FFMETADATA1' > chapters.txt
for n in 0 1 2 3 4; do
    printf '[CHAPTER]\nTIMEBASE=1/1000\nSTART=%s\nEND=%s\ntitle=chapter%s\n' \
        "$((3000*n))" "$((3000*(n+1)))" "$((n+1))" >> chapters.txt
done
"$ffmpeg" -v error -f lavfi -i testsrc2=size=160x90:rate=24:duration=15 -i chapters.txt \
    -map 0:v -map_chapters 1 -c:v libx264 -bf 3 -g 24 -sc_threshold 0 disc.mkv

packets() {
    "$ffprobe" -v error -select_streams v:0 -show_packets -show_data_hash sha256 \
        -show_entries "packet=$2" -of csv=p=0 "$1"
}

# Full copies retain reordered video timestamps and compressed packet contents.
"$cli" -i disc.mkv -m range -o full > /dev/null
packets disc.mkv pts,dts,duration,data_hash > original.csv
packets full/split_disc_part1.mkv pts,dts,duration,data_hash > output.csv
diff -u original.csv output.csv

# Explicit selection overrides the automatic grouping. The cut retains the
# same packets with one constant timestamp offset (Matroska millisecond ticks).
"$cli" -i disc.mkv -m chapters -c 2 -o selected > /dev/null
test "$(ls -A selected)" = "split_disc_ch2_chapter2.mkv"
packets disc.mkv pts,data_hash | awk -F, '$1 >= 3000 && $1 < 6000 {print $1-3000 "," $2}' > chapter.csv
packets selected/split_disc_ch2_chapter2.mkv pts,data_hash > output.csv
diff -u chapter.csv output.csv

"$cli" -i disc.mkv -m chapters -g 2 --tail-bumpers 0 --bumpers separate -o grouped > plan.log
grep -F 'Tag: S01E02 | Range: 00:00:06.000 -> 00:00:12.000' plan.log
grep -F 'Tag: disc_outro | Range: 00:00:12.000 -> 00:00:15.000' plan.log
test "$(ls -A grouped | wc -l)" -eq 3
packets disc.mkv pts,data_hash | awk -F, '$1 >= 6000 {print $2}' > expected-tail.csv
{ packets 'grouped/split_disc - S01E02.mkv' data_hash; packets grouped/split_disc_disc_outro.mkv data_hash; } > actual-tail.csv
diff -u expected-tail.csv actual-tail.csv

# Audio scans must report missing audio, including in combined mode.
for detector in silence combined; do
    if "$cli" -i disc.mkv -m gap --gap-type "$detector" -o failed > /dev/null 2> error.log; then
        echo "Expected the $detector scan to fail" >&2
        exit 1
    fi
    grep -F 'No audio stream' error.log
    test ! -e failed
done

# Only the right channel carries sound, with one real gap from 4 to 6 seconds.
# Exercise both planar AAC decoding and packed PCM decoding.
for codec in aac pcm_s16le; do
    "$ffmpeg" -v error -f lavfi -i "aevalsrc=0|sin(2*PI*440*t)*not(between(t\,4\,6)):d=15:s=48000" \
        -c:a "$codec" "$codec.mkv"
    "$cli" -i "$codec.mkv" -m gap --gap-type silence --min-gap 1 --min-segment 1 -o "$codec" > plan.log
    grep -F 'Split Execution Plan (2 segments)' plan.log
    grep -E 'Tag: S01E01 .* -> 00:00:(04\.9|05\.0)' plan.log
done
