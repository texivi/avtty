#!/bin/sh
set -e

cd "$(dirname "$0")"

ICON_URL="https://raw.githubusercontent.com/texivi/avtty/main/Logo.png"

if [ ! -f avtty.c ]; then
    echo "[!] avtty.c not found next to install.sh" >&2
    exit 1
fi

CC_BIN=${CC:-cc}
if ! command -v "$CC_BIN" >/dev/null 2>&1; then
    echo "[!] No C compiler found. Install gcc or clang and try again." >&2
    exit 1
fi

TMP_ICON=""
trap 'rm -f avtty "$TMP_ICON"' EXIT

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
    APP_DIR="$DATA_DIR/applications"
    ICON_DIR="$DATA_DIR/icons/hicolor/256x256/apps"
    ICON_FILE="$ICON_DIR/avtty.png"
    mkdir -p "$APP_DIR" "$ICON_DIR"

    HAVE_ICON=0
    if [ -f Logo.png ]; then
        cp Logo.png "$ICON_FILE" && HAVE_ICON=1
    else
        TMP_ICON="$(mktemp)"
        echo "[*] Downloading the app icon from GitHub..."
        if command -v curl >/dev/null 2>&1 && curl -fsSL "$ICON_URL" -o "$TMP_ICON" && [ -s "$TMP_ICON" ]; then
            cp "$TMP_ICON" "$ICON_FILE" && HAVE_ICON=1
        elif command -v wget >/dev/null 2>&1 && wget -qO "$TMP_ICON" "$ICON_URL" && [ -s "$TMP_ICON" ]; then
            cp "$TMP_ICON" "$ICON_FILE" && HAVE_ICON=1
        fi
    fi

    if [ "$HAVE_ICON" -eq 1 ]; then
        ICON_REF="$ICON_FILE"
    else
        echo "[!] Could not get the logo, using a built-in icon." >&2
        ICON_REF="$DATA_DIR/icons/hicolor/scalable/apps/avtty.svg"
        mkdir -p "$(dirname "$ICON_REF")"
        cat > "$ICON_REF" <<'EOF'
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 128 128">
<defs><linearGradient id="g" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#7aa2f7"/><stop offset="1" stop-color="#bb9af7"/></linearGradient></defs>
<rect width="128" height="128" rx="26" fill="#1a1b26"/>
<rect x="8" y="8" width="112" height="112" rx="20" fill="none" stroke="#3b4261" stroke-width="2"/>
<polyline points="30,92 64,28 98,92" fill="none" stroke="url(#g)" stroke-width="13" stroke-linecap="round" stroke-linejoin="round"/>
<line x1="46" y1="72" x2="82" y2="72" stroke="url(#g)" stroke-width="11" stroke-linecap="round"/>
<rect x="42" y="106" width="44" height="8" rx="3" fill="#9ece6a"/>
</svg>
EOF
    fi

    cat > "$APP_DIR/avtty.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=avtty
GenericName=Text Editor
Comment=A very tiny text yard
Exec="$BIN_DIR/avtty" %f
Icon=$ICON_REF
Terminal=true
Categories=Utility;TextEditor;
MimeType=text/plain;
Keywords=text;editor;
EOF
    chmod 755 "$APP_DIR/avtty.desktop"

    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database "$APP_DIR" >/dev/null 2>&1 || true
    fi
    echo "[+] Added avtty to your app menu"
fi

case ":$PATH:" in
    *":$BIN_DIR:"*) ;;
    *) echo "[!] $BIN_DIR is not on your PATH. Add this to your shell profile:"
       echo "    export PATH=\"$BIN_DIR:\$PATH\"" ;;
esac

echo "[+] AVTTY installed. Run 'avtty' for the start screen, or 'avtty -h' for help."
