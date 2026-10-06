# spacehulk_nx

**Space Hulk (Hoplite Research) on Nintendo Switch** — work in progress.

An unofficial Nintendo Switch wrapper for the 32-bit (armeabi-v7a) Android
build of **Space Hulk**: it loads the game's own Unity 5.3.4f1 and Mono
libraries and provides the Android, JNI, libc, audio, input and graphics
services they expect, on the [android32](https://github.com/aks796/android32)
runtime. It started from [dcr_sea_nx](https://github.com/aks796/dcr_sea_nx)
(Disney Crossy Road, Unity 5.6 / Mono), whose Mono JIT and Unity layers it
keeps; [NOTES.md](NOTES.md) is that port's technical notes.

No game code or data is included: you supply your own APK and OBB.

## What you need

* Space Hulk for Android, `com.hoplite.spacehulk`, versionCode 7 (the build
  signed by Hoplite Research), and its expansion file
  `main.7.com.hoplite.spacehulk.obb` (about 1.5 GB, in
  `Android/obb/com.hoplite.spacehulk/` on the phone).
* A Switch with Atmosphère and sphaira.

## Install

```
sd:/switch/spacehulk_nx/spacehulk_nx.nro
sd:/switch/spacehulk_nx/<your APK, any name>.apk
sd:/switch/spacehulk_nx/obb/main.7.com.hoplite.spacehulk.obb
```

In sphaira: Homebrew › spacehulk_nx › Install Forwarder, then start the new
icon. The first start unpacks the libraries from the APK. `debug.log` and
`crash.log` are written in `sd:/switch/spacehulk_nx/`.

## Build

Requirements: Docker, the toolchain image
`ghcr.io/vita2hos/devcontainer/vita2hos`, `devkitpro/devkita64`,
[libnx32](https://github.com/aks796/libnx32) built next to this folder
(`../libnx32/prefix`), [mesa32](https://github.com/aks796/mesa32)'s `lib/` and
`include/` in `portlibs32/`, Python 3 with `pyelftools` and `capstone`.

mesa32 with `tools/mesa32-program-binary.patch` applied (`git am` in a
mesa32 checkout, then its `./build.sh`) offers program binaries, so Unity's
own shader cache (`data/cache/UnityShaderCache/` on the SD card) works and a
second start skips the GLSL compiles. The stock mesa32 release works too,
without that cache.

```bash
git submodule update --init
python3 tools/make_icon.py <your APK>   # launcher/icon.jpg (the game's icon, not in git)
unzip -j <your APK> 'assets/bin/Data/Managed/*.dll' -d mod/refs   # the game's assemblies, not in git
mod/build_mod.sh      # mod/dcrmod.dll: the port's C# (mod/src), e.g. the Switch's video options
./build.sh            # spacehulk_nx.nsp
launcher/build.sh     # launcher/spacehulk_nx.nro (carries the NSP)
```

Host checks against your own APK (`unzip -j <apk> 'lib/armeabi-v7a/lib{main,unity,mono}.so' -d /tmp/libs`):

```bash
python3 tools/offsets/derive_offsets.py /tmp/libs      # source/dcr_offsets.h
python3 runtime/tools/gen_imports.py --libs /tmp/libs  # source/imports.c
```

## What differs from the Unity 5.6 port

| Area | Unity 5.6.4 (dcr_sea_nx) | Unity 5.3.4 (Space Hulk) |
| --- | --- | --- |
| Time icalls | flat name/function tables | one `mono_add_internal_call` per icall: `derive_offsets.py` reads the registration function |
| TimeManager | `time` +0x80, ... | `time` +0x58, `deltaTime` +0x68, `timeScale` +0xbc, ...; no `maximumParticleDeltaTime` |
| Frame pacing | Choreographer VSYNC, pumped | none (eglSwapInterval / targetFrameRate): the pump is off |
| Data | APK only | APK + OBB: `nativeFile(<obb>)` after the APK (`UnityPlayer.j()`) |
| Imports | — | 43 more: NativeActivity input queue, `wcs*`, `swscanf`, `isnan`, ... (`sh_imports_extra.c`, `tools/imports.cfg`) |

## A Switch-sized OBB (optional, prototype)

The port loads textures from their second mip (1 GB of memory cannot hold the
2048x2048 ones), yet Unity reads and inflates the top mip from the OBB first.
`tools/obb/strip_mips.py` writes a copy of YOUR OBB whose mipmapped textures
start at their second mip: the same picture, 1.48 GB -> 0.9 GB, 1.3 GB less
to read and inflate. The port sees its marker entry and skips no further mip.

```bash
pip install UnityPy
python3 tools/obb/strip_mips.py main.7.com.hoplite.spacehulk.obb out/main.7.com.hoplite.spacehulk.obb
```
