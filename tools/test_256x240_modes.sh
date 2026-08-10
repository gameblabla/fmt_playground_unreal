#!/usr/bin/env bash
# Build and show the Bubble Bobble-derived 256x240 libfmt test pattern.
# Usage: ./tools/test_256x240_modes.sh 8
#        ./tools/test_256x240_modes.sh 16

set -euo pipefail

case "${1:-8}" in
    8)
        pixel_bpp=8
        ;;
    16)
        # The existing test selector calls RGB555 "15", although VRAM uses
        # 16-bit words.  Keep the public script spelling intuitive.
        pixel_bpp=15
        ;;
    *)
        echo "Usage: $0 {8|16}" >&2
        exit 2
        ;;
esac

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

make -B -j4 VGM_PLAYER=0 GFX_TEST=0 PIXEL_TEST_BPP="$pixel_bpp" TEST_256_MODE=1
./tools/mkcd.sh

echo "Starting 256x240 ${1:-8}bpp test in Tsugaru (close its window to return)."
exec ./Tsugaru_CUI.elf "$root/MARTY_ROM/" -TOWNSTYPE MARTY -CD output.iso \
    -NORMALFD -DONTUSEFPU -AUTOSCALE
