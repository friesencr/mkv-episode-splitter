#!/bin/bash
set -euo pipefail
PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
APPLICATIONS_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
DESKTOP_FILE="$APPLICATIONS_DIR/mkvsplit.desktop"
mkdir -p -- "$APPLICATIONS_DIR"

# Escape literal characters for a quoted Desktop Entry Exec argument.
launcher="$PROJECT_DIR/mkvsplit-launcher.sh"
launcher="${launcher//\\/\\\\}"
launcher="${launcher//\$/\\\$}"
launcher="${launcher//\`/\\\`}"
launcher="${launcher//\"/\\\"}"
launcher="${launcher//%/%%}"
cat > "$DESKTOP_FILE" <<EOF
[Desktop Entry]
Type=Application
Name=MKV Splitter (Interactive)
Comment=Split multi-episode MKV files with auto-detection
Exec=bash "$launcher" %f
Icon=video-x-generic
Terminal=true
Categories=AudioVideo;Video;Utility;
MimeType=video/x-matroska;
EOF
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$APPLICATIONS_DIR"
fi
printf 'Installed %s\n' "$DESKTOP_FILE"
