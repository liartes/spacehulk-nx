#!/bin/sh
# build_mod.sh -- mod/dcrmod.dll from mod/src/*.cs, compiled against YOUR copy
# of the game's assemblies (mod/refs/, not in git:
# unzip -j <apk> 'assets/bin/Data/Managed/*.dll' -d mod/refs), with the
# .NET SDK image's Roslyn. The game runs Mono 2.x (.NET 3.5 profile): the
# mod references the game's own mscorlib, nothing newer.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
[ -f "$HERE/refs/Assembly-CSharp.dll" ] || { echo "mod/refs/: the game's Managed/*.dll first" >&2; exit 1; }
exec docker run --rm -v "$HERE:/mod" -w /mod mcr.microsoft.com/dotnet/sdk:8.0 sh -c '
  CSC=$(ls /usr/share/dotnet/sdk/*/Roslyn/bincore/csc.dll | head -1)
  dotnet "$CSC" -nologo -noconfig -nostdlib -target:library -optimize -langversion:7.3 \
    -r:refs/mscorlib.dll -r:refs/System.dll -r:refs/System.Core.dll -r:refs/UnityEngine.dll \
    -r:refs/Assembly-CSharp.dll -out:dcrmod.dll src/*.cs && chmod a+rw dcrmod.dll'
