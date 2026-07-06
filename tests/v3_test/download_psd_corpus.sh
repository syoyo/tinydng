#!/bin/sh
# Download a small real-world PSD/PSB corpus (psd-tools test fixtures, MIT)
# into tests/v3_test/data/psd-corpus/ (gitignored). Usage:
#   ./download_psd_corpus.sh [destdir]
# Then: ./test_v3_psd corpus data/psd-corpus/*.ps[db]
set -eu

DEST="${1:-$(dirname "$0")/data/psd-corpus}"
BASE="https://raw.githubusercontent.com/psd-tools/psd-tools/main/tests/psd_files"
mkdir -p "$DEST"

FILES="
1layer.psd
2layers.psd
2layers.psb
16bit5x5.psd
16bit5x5.psb
32bit.psd
32bit5x5.psd
0layers.psd
hidden-groups.psd
empty-group.psd
layer-name-emoji.psd
placedLayer.psd
placedLayer.psb
transparentbg.psd
transparentbg-gimp.psd
mask.psd
clipping-mask.psd
broken-groups.psd
colormodes/4x4_8bit_cmyk.psd
colormodes/4x4_8bit_index_color.psd
colormodes/4x4_1bit_bitmap.psd
colormodes/4x4_16bit_rgb.psd
colormodes/4x4_16bit_lab.psd
colormodes/4x4_32bit_rgb.psd
colormodes/4x4_8bit_duotone.psd
"

for f in $FILES; do
  out="$DEST/$(echo "$f" | tr '/' '_')"
  if [ -s "$out" ]; then
    echo "have  $out"
  else
    echo "fetch $f"
    curl -fsSL -m 60 -o "$out" "$BASE/$f" || echo "  (failed: $f)"
  fi
done
echo "corpus in $DEST"
