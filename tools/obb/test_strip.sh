#!/bin/sh
# test_strip.sh -- run source/sh_obbstrip.c (the console's OBB optimisation)
# on the PC: test_strip.sh <your OBB> <out.obb>. Downloads miniz 3.0.2 into
# tools/obb/.miniz (not in git) the first time.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
M="$HERE/.miniz"
if [ ! -f "$M/miniz.c" ]; then
  mkdir -p "$M"
  curl -sL -o "$M/miniz.zip" https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip
  (cd "$M" && unzip -q -o miniz.zip)
fi
gcc -O2 -Wall -pthread -DSH_OBBSTRIP_HOST -D_FILE_OFFSET_BITS=64 -I"$M" -I"$HERE/../../source" \
  "$HERE/test_strip.c" "$HERE/../../source/sh_obbstrip.c" "$M/miniz.c" -o "$M/test_strip"
exec "$M/test_strip" "$@"
