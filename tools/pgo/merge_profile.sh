#!/bin/sh
# merge_profile.sh -- a training run's mesa-pgo.gcov (source/sh_pgo.c) into
# the .gcda files of mesa32/pgo-profile/, for MESA_PGO=use.
#
#   tools/pgo/merge_profile.sh <mesa-pgo.gcov> [<mesa32 folder>]
#
# The stream names each file as the instrumented build saw it,
# /work/pgo-profile/<object>.gcda (mesa32 mounted at /work, as its build.sh
# does), so gcov-tool runs in the same container with the same mount. A
# second stream merges into the same files (counts add up); delete
# pgo-profile/ to start over. fix_summary.py then gives every file the summary
# of one normal run. MIT.
set -eu
STREAM="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
MESA="$(cd "${2:-$(dirname "$0")/../../../mesa32}" && pwd)"
IMAGE="${TOOLCHAIN_IMAGE:-ghcr.io/vita2hos/devcontainer/vita2hos:latest}"
mkdir -p "$MESA/pgo-profile"
docker run --rm --user "$(id -u):$(id -g)" -v "$MESA:/work" -v "$STREAM:/stream.gcov:ro" -w /work "$IMAGE" \
  bash -lc '$DEVKITPRO/devkitARM/bin/arm-none-eabi-gcov-tool merge-stream /stream.gcov'
# runs=1 and the program-wide sum_max, as libgcov's exit would have written
python3 -I "$(dirname "$0")/fix_summary.py" "$MESA/pgo-profile"
echo "$(ls "$MESA/pgo-profile" | wc -l) profile files in $MESA/pgo-profile"
