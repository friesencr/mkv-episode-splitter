#!/bin/bash
PROJECT_DIR="/home/chris/dev/mkv-splitter"
DESKTOP_FILE="$HOME/.local/share/applications/mkvsplit.desktop"

mkdir -p "$HOME/.local/share/applications"

cat <<EOF > "$PROJECT_DIR/mkvsplit-launcher.sh"
#!/bin/bash
if [ -n "\$1" ]; then
    "$PROJECT_DIR/build/mkvsplit-cli" -I "\$1"
else
    "$PROJECT_DIR/build/mkvsplit-cli"
fi
EOF

chmod +x "$PROJECT_DIR/mkvsplit-launcher.sh"

cat <<EOF > "$DESKTOP_FILE"
[Desktop Entry]
Type=Application
Name=MKV Splitter (Interactive)
Comment=Split multi-episode MKV files with auto-detection
Exec=$PROJECT_DIR/mkvsplit-launcher.sh %f
Icon=video-x-generic
Terminal=true
Categories=AudioVideo;Video;Utility;
MimeType=video/x-matroska;
EOF

chmod +x "$DESKTOP_FILE"
update-desktop-database "$HOME/.local/share/applications" 2>/dev/null || true
