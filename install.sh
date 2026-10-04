#!/bin/sh
set -e

cd "$(dirname "$0")"

ICON_URL="https://raw.githubusercontent.com/texivi/avtty/main/Logo.png"
CC_BIN=${CC:-cc}

[ -f avtty.c ] || { echo "[!] avtty.c not found next to install.sh" >&2; exit 1; }
command -v "$CC_BIN" >/dev/null 2>&1 || { echo "[!] No C compiler found. Install gcc or clang first." >&2; exit 1; }

trap 'rm -f avtty' EXIT

echo "[*] Building avtty..."
"$CC_BIN" -O2 avtty.c -o avtty

if [ -w /usr/local/bin ]; then
    BIN_DIR=/usr/local/bin
    install -m 755 avtty "$BIN_DIR/avtty"
elif command -v sudo >/dev/null 2>&1 && sudo -v 2>/dev/null; then
    BIN_DIR=/usr/local/bin
    sudo install -m 755 avtty "$BIN_DIR/avtty"
else
    BIN_DIR="$HOME/.local/bin"
    mkdir -p "$BIN_DIR"
    install -m 755 avtty "$BIN_DIR/avtty"
fi
echo "[+] Installed $BIN_DIR/avtty"

if [ "$(uname -s)" = "Linux" ]; then
    DATA_DIR="${XDG_DATA_HOME:-$HOME/.local/share}"
    ICON_FILE="$DATA_DIR/icons/hicolor/256x256/apps/avtty.png"
    mkdir -p "$DATA_DIR/applications" "$(dirname "$ICON_FILE")"

    if [ -f Logo.png ]; then
        cp Logo.png "$ICON_FILE"
    elif command -v curl >/dev/null 2>&1; then
        curl -fsSL "$ICON_URL" -o "$ICON_FILE" || rm -f "$ICON_FILE"
    elif command -v wget >/dev/null 2>&1; then
        wget -qO "$ICON_FILE" "$ICON_URL" || rm -f "$ICON_FILE"
    fi

    ICON_LINE=""
    [ -s "$ICON_FILE" ] && ICON_LINE="Icon=$ICON_FILE"

    cat > "$DATA_DIR/applications/avtty.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=avtty
GenericName=Text Editor
Comment=A very tiny text yard
Exec="$BIN_DIR/avtty" %f
$ICON_LINE
Terminal=true
Categories=Utility;TextEditor;
MimeType=text/plain;
EOF
    command -v update-desktop-database >/dev/null 2>&1 &&
        update-desktop-database "$DATA_DIR/applications" >/dev/null 2>&1 || true
    echo "[+] Added avtty to your app menu"
fi

case ":$PATH:" in
    *":$BIN_DIR:"*) ;;
    *) echo "[!] $BIN_DIR is not on your PATH. Add this to your shell profile:"
       echo "    export PATH=\"$BIN_DIR:\$PATH\"" ;;
esac

echo "[+] AVTTY installed. Run 'avtty' to start."
