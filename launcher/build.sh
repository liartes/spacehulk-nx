#!/bin/sh
# Build spacehulk_nx.nro (the launcher) with the runtime's launcher build
# (devkitPro's 64-bit toolchain container). Build the wrapper first
# (../build.sh): the NRO carries ../spacehulk_nx.nsp and ../spacehulk_nx.build.
HERE="$(cd "$(dirname "$0")" && pwd)"
LAUNCHER_DIR="$HERE" PAYLOAD=spacehulk_nx exec "$HERE/../runtime/launcher/build.sh" "$@"
