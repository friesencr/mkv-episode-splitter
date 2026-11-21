#!/usr/bin/env bash
set -euo pipefail
tag=${1:?expected a release tag}
bash packaging/release-tag.sh "$tag" >/dev/null
dest=$(realpath "${2:?expected the artifact directory}")
binaries=(mkvsplit-cli-linux-x86_64 mkvsplit-cli-linux-arm64 mkvsplit-cli-macos-arm64 mkvsplit-cli-windows-x86_64.exe)
for binary in "${binaries[@]}"; do test -s "$dest/$binary"; done
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
root="mkv-episode-splitter-$tag"
git archive --prefix="$root/" HEAD | tar -xf - -C "$work"
revision=$(git rev-parse HEAD:third_party/ffmpeg)
git -C third_party/ffmpeg archive --prefix="$root/third_party/ffmpeg/" "$revision" | tar -xf - -C "$work"
printf 'Splitter %s\nFFmpeg %s\n' "$(git rev-parse HEAD)" "$revision" > "$work/$root/REVISIONS"
tar --sort=name --owner=0 --group=0 --numeric-owner --mtime="@$(git show -s --format=%ct HEAD)" \
    -cf - -C "$work" "$root" | gzip -n > "$dest/$root-source.tar.gz"
cat "$work/$root/packaging/NOTICES" "$work/$root/LICENSE" \
    "$work/$root/third_party/ffmpeg/COPYING.LGPLv2.1" "$work/$root"/packaging/licenses/*.txt > "$dest/LICENSES.txt"
cd "$dest"
sha256sum "${binaries[@]}" "$root-source.tar.gz" LICENSES.txt > SHA256SUMS
