#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 1 ]; then
  echo "usage: $0 <dng_or_raw_ljpeg_file> [iterations]" >&2
  exit 1
fi

FILE="$1"
ITERS="${2:-100}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

run_one() {
  local bin="$1"
  local out
  if ! out=$("$bin" "$FILE" "$ITERS" 2>&1); then
    printf "%s\n" "$out" >&2
    exit 2
  fi
  local thr
  thr=$(printf "%s\n" "$out" | awk -F'throughput_mpix_s=' '/throughput_mpix_s=/{print $2}' | awk '{print $1}')
  if [ -z "$thr" ]; then
    echo "failed to parse throughput from $bin" >&2
    printf "%s\n" "$out" >&2
    exit 2
  fi
  echo "$thr"
}

SCALAR=$(run_one "$SCRIPT_DIR/bench-scalar")
SSE2=$(run_one "$SCRIPT_DIR/bench-sse2")
AVX2=$(run_one "$SCRIPT_DIR/bench-avx2")

awk -v s="$SCALAR" -v x="$SSE2" -v a="$AVX2" 'BEGIN {
  printf("scalar: %s MPix/s\n", s);
  printf("sse2:   %s MPix/s (x%.3f)\n", x, x/s);
  printf("avx2:   %s MPix/s (x%.3f)\n", a, a/s);
}'
