#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

echo "==== CLI Tray build ===="
echo "Icon selection:"
echo "  [Enter]  generate the default icon"
echo "  [k]      keep the existing src/icon.ico (if any)"
echo "  [path]   use an external .ico file"
read -rp "Choice: " ICON_CHOICE

case "$ICON_CHOICE" in
    ""|g|generate)
        python3 make_icon.py
        ;;
    k|keep)
        if [ ! -f src/icon.ico ]; then
            echo "ERROR: src/icon.ico does not exist, nothing to keep."
            exit 1
        fi
        echo "Keeping existing src/icon.ico"
        ;;
    *)
        if [ ! -f "$ICON_CHOICE" ]; then
            echo "ERROR: icon file not found: $ICON_CHOICE"
            exit 1
        fi
        cp "$ICON_CHOICE" src/icon.ico
        echo "Using external icon: $ICON_CHOICE"
        ;;
esac

mkdir -p dist

x86_64-w64-mingw32-windres src/resource.rc -O coff -o src/resource.o

x86_64-w64-mingw32-gcc -O2 -municode -mwindows -static \
    -o dist/CLITray.exe \
    src/main.c src/resource.o \
    -luser32 -lshell32 -lshlwapi -lcomctl32 -lgdi32

cp clitray.ini dist/clitray.ini

ls -la dist/
echo "Build OK: dist/CLITray.exe"