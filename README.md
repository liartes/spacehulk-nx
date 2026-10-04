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

```bash
git submodule update --init
python3 tools/make_icon.py <your APK>   # launcher/icon.jpg (the game's icon, not in git)
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
