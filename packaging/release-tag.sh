#!/usr/bin/env bash
# Validate the tag and print the value for gh release's --prerelease flag.
set -euo pipefail
tag=${1:?expected a release tag}
number='(0|[1-9][0-9]*)'
version=$(sed -nE 's/^project\(mkv-splitter VERSION ([0-9]+\.[0-9]+\.[0-9]+) LANGUAGES .*$/\1/p' CMakeLists.txt)
if [[ ! $tag =~ ^v${number}\.${number}\.${number}(-(alpha|beta|rc)\.[1-9][0-9]*)?$ || ${tag%%-*} != "v$version" ]]; then
    echo "Expected v$version with an optional -alpha.N, -beta.N, or -rc.N suffix, got $tag" >&2
    exit 1
fi
if [[ $tag == *-* ]]; then echo true; else echo false; fi
