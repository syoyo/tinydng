#!/usr/bin/env bash
# End-to-end HTJ2K baseline vs OpenJPH on this machine.
#
# For each corpus image, run ojph_compress (lossless reversible 5/3 and
# lossy irreversible 9/7) and ojph_expand, capturing OpenJPH's own
# "Elapsed time" and converting to MPix/s throughput.
#
# Usage: run_bench.sh [OJPH_BIN_DIR]
set -u

OJH_BIN="${1:-$HOME/work/OpenJPH/build/src/apps}"
OJPH_C="$OJH_BIN/ojph_compress/ojph_compress"
OJPH_E="$OJH_BIN/ojph_expand/ojph_expand"

if [ ! -x "$OJPH_C" ] || [ ! -x "$OJPH_E" ]; then
  echo "OpenJPH binaries not found under $OJH_BIN" >&2
  exit 1
fi

HERE="$(cd "$(dirname "$0")" && pwd)"
DATA="$HERE/data"
OUT="$HERE/out"
mkdir -p "$OUT"

# image  out-ext  decomp  qfactor
CORPUS=(
  "test48mp-gray.pgm  pgm  5  90"
  "pia23128-rgb.ppm   ppm  5  90"
  "skin8k-rgb.ppm     ppm  5  90"
  "shirts4k-rgb.ppm   ppm  5  90"
)

mpix() { # file -> megapixels
  local f="$1" w h
  w=$(identify -format '%w' "$f" 2>/dev/null)
  h=$(identify -format '%h' "$f" 2>/dev/null)
  [ -n "$w" ] && [ -n "$h" ] || return 0
  awk -v w="$w" -v h="$h" 'BEGIN{printf "%.3f", w*h/1e6}'
}

run_ojph() { # label img outext rev qf
  local label="$1" img="$2" oext="$3" rev="$4" qf="$5"
  local src="$DATA/$img" base="${img%.*}"
  local j2c="$OUT/${base}.${label}.j2c"
  local out="$OUT/${base}.${label}.${oext}"
  local args=(-i "$src" -o "$j2c" -reversible "$rev" -colour_trans true)
  [ "$rev" = "true" ] || args+=(-qfactor "$qf")

  local tce tde mp
  mp=$(mpix "$src")

  tce=$("$OJPH_C" "${args[@]}" 2>&1 | sed -n 's/Elapsed time = //p')
  tde=$("$OJPH_E" -i "$j2c" -o "$out" 2>&1 | sed -n 's/Elapsed time = //p')

  local comp_mpix dec_mpix
  comp_mpix=$(awk -v mp="$mp" -v t="$tce" 'BEGIN{ if (t>0) printf "%.1f", mp/t; else printf "0" }')
  dec_mpix=$(awk -v mp="$mp" -v t="$tde" 'BEGIN{ if (t>0) printf "%.1f", mp/t; else printf "0" }')
  printf "%-8s %-18s %-8s size=%11s  enc=%6.3fs (%8.1f MPix/s)  dec=%6.3fs (%8.1f MPix/s)\n" \
    "$label" "$base" "$mp MPix" "$(stat -c %s "$j2c")" "$tce" "$comp_mpix" "$tde" "$dec_mpix"
}

echo "== lossless (reversible 5/3, colour transform) =="
for entry in "${CORPUS[@]}"; do
  set -- $entry
  run_ojph "lossless" "$1" "$2" "true" "0"
done

echo "== lossy (irreversible 9/7, qfactor 90, colour transform) =="
for entry in "${CORPUS[@]}"; do
  set -- $entry
  run_ojph "lossy" "$1" "$2" "false" "$4"
done
