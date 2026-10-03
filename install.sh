#!/bin/sh
set -e

cc -O2 avtty.c -o avtty

if [ -w /usr/local/bin ]; then
    install -m 755 avtty /usr/local/bin/avtty
elif command -v sudo >/dev/null 2>&1; then
    sudo install -m 755 avtty /usr/local/bin/avtty
else
    mkdir -p "$HOME/.local/bin"
    install -m 755 avtty "$HOME/.local/bin/avtty"
fi

rm -f avtty

echo "[+] AVTTY installed. Run 'avtty -h' for help."
