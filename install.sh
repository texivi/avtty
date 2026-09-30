#!/bin/sh

cc avtty.c -o avtty || exit 1
chmod +x avtty

if [ -w /usr/local/bin ]; then
    cp avtty /usr/local/bin/avtty
elif command -v sudo >/dev/null 2>&1; then
    sudo cp avtty /usr/local/bin/avtty
else
    mkdir -p "$HOME/.local/bin"
    cp avtty "$HOME/.local/bin/avtty"
fi

echo "[+] AVTTY installed. Type 'avtty --help for more'"
