#!/usr/bin/env bash
set -euo pipefail
tag=${1:?expected a release tag}
bash packaging/release-tag.sh "$tag" >/dev/null
dest=$(realpath "${2:?expected the artifact directory}")
test -f "$dest/mkvsplit-cli-linux-x86_64"
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
sha256sum mkvsplit-cli-linux-x86_64 "$root-source.tar.gz" LICENSES.txt > SHA256SUMS
