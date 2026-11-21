#!/bin/bash
set -euo pipefail
PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BINARY="$PROJECT_DIR/build/mkvsplit-cli"
if [[ ! -x "$BINARY" ]]; then
    printf 'Build the CLI first: cmake -S "%s" -B "%s/build" && cmake --build "%s/build"\n' "$PROJECT_DIR" "$PROJECT_DIR" "$PROJECT_DIR" >&2
    exit 1
fi
if [[ $# -gt 0 ]]; then
    exec "$BINARY" -I "$@"
fi
exec "$BINARY"
