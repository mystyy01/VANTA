#!/bin/bash
set -e

echo "Building PHOBOS apps..."

CFLAGS="-ffreestanding -mno-red-zone -fno-pic -mcmodel=large -fno-builtin"

# Resolve the directory of this script
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GLOBAL_LD="$SCRIPT_DIR/linker.ld"

# Iterate over all subfolders
for appdir in "$SCRIPT_DIR"/*/; do
    appname="$(basename "$appdir")"

    # Check if <folder>/<folder>.c exists
    SRC_FILE="$appdir/$appname.c"
    if [[ -f "$SRC_FILE" ]]; then
        echo "Building ${appname}..."

        # Compile
        x86_64-elf-gcc $CFLAGS -c "$SRC_FILE" -o "$appdir/$appname.o"

        # Determine which linker script to use
        LD_SCRIPT="$appdir/linker.ld"
        if [[ ! -f "$LD_SCRIPT" ]]; then
            LD_SCRIPT="$GLOBAL_LD"
        fi

        # Link
        x86_64-elf-ld -T "$LD_SCRIPT" -o "$appdir/$appname" "$appdir/$appname.o"

        echo "Built: ${appname}"
    else
        echo "Skipping ${appname}, no $appname.c found"
    fi
done
