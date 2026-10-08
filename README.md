<div align="center">

# spacehulk_nx

**Space Hulk (Hoplite Research) on Nintendo Switch**

An unofficial Switch port of the 32-bit Android version of **Space Hulk**,
running the game's own Unity 5.3 engine and Mono scripts.

By **Liartes**, on the [android32](https://github.com/aks796/android32) runtime.

</div>

---

## About

`spacehulk_nx` loads the Android game's own libraries (`libunity.so`,
`libmono.so`, `libmain.so`) and the game's C# assemblies, and provides what
they expect from Android: the C library, a Java VM without Java (JNI),
OpenGL ES through Mesa, audio, controllers and files. The program runs in
32-bit (AArch32) mode, like the game.

**No game code or data is included.** You need your own copy of the game: its
APK and its expansion file (OBB).

### What works

* The whole game: menus, campaign, missions, saves, with the controller.
* 720p handheld, 900p docked (configurable), about 50-60 fps in missions.
* Intro movies (with sound), the game's music and sounds at 48 kHz.
* Fast loading: a mission loads in about 15 seconds.

### What was changed for the Switch

* Textures are drawn at half resolution: the game was made for the NVIDIA
  Shield (3 GB of memory) and a 32-bit Switch program has 1 GB.
* At the first start, the OBB is rewritten for the Switch, once (a few
  minutes): its textures start at the size the port draws them, so it is
  about 40% smaller and loads faster. The picture is the same.
* Unity's multithreaded rendering is enabled, and the heaviest settings of
  the top quality level are toned down (dynamic lights, shadow distance, level
  of detail). All of it can be changed in `config.ini`.
* The video options the Switch cannot honour (resolution, full screen, v-sync)
  are hidden from the game's menu.
* The doors' leaves are drawn where their animation puts them: the missions
  were built with them merged into the static scenery, so on the Switch the
  doors opened (line of sight included) but looked shut.
* Online features (Google Play Games, multiplayer servers) are not available.

---

## Installing

Download the latest release from
[github.com/liartes/spacehulk-nx/releases](https://github.com/liartes/spacehulk-nx/releases).

You need a Switch with **Atmosphère** and **sphaira**, and your own copy of
**Space Hulk for Android** (`com.hoplite.spacehulk`, versionCode 7, the
armeabi-v7a build signed by Hoplite Research):

* the APK,
* its OBB, `main.7.com.hoplite.spacehulk.obb` (about 1.5 GB), from
  `Android/obb/com.hoplite.spacehulk/` on the phone.

Copy them to the SD card like this:

```
sd:/switch/spacehulk_nx/spacehulk_nx.nro
sd:/switch/spacehulk_nx/<your APK, any name>.apk
sd:/switch/spacehulk_nx/obb/main.7.com.hoplite.spacehulk.obb
```

Then, in **sphaira**: *Homebrew* › *spacehulk_nx* › *Install Forwarder*, and
start the game from its new icon on the HOME menu.

**The first start takes a few minutes**: the libraries are unpacked from the
APK, then the OBB is optimized for the Switch (a progress bar shows it). This
needs about 1 GB free on the SD card while it runs; the optimized OBB then
replaces the original one (keep your original elsewhere if you want it). The
following starts go straight to the game.

`title_id.txt` in the game folder tells which `atmosphere/contents/` folder
belongs to the port, if you want to remove it.

---

## Controls

| Switch | Game |
| --- | --- |
| **A** | Select / confirm (Cross) |
| **B** | Back / cancel / undo (Circle) |
| **X** / **Y** | Square / Triangle |
| **L** / **R** | Previous / next squad member |
| **ZL** / **ZR** | Rotate the camera |
| **Left stick / D-pad** | Move the selection |
| **Right stick** | Camera |
| **Left stick click** | Reroll |
| **Right stick click** | Reveal numbers |
| **+** | Menu (pause) |
| **−** | Strategic view |

Any button skips the intro movies.

---

## Settings

`sd:/switch/spacehulk_nx/config.ini` is written at the first start, each
option explained above it. Among them:

| Option | Default | |
| --- | --- | --- |
| `[display] resolution` | `auto` | 720 handheld, 900 docked; or `720`, `900`, `1080` |
| `[graphics] texture_resolution` | `half` | `full` does not fit in memory in missions |
| `[graphics] pixel_lights`, `shadow_distance`, `lod_bias` | `2`, `100`, `3` | `game` for the game's own top-level values |
| `[graphics] antialiasing` | `0` | `2`, `4` or `game` |
| `[performance] loading_priority` | `high` | how fast Unity takes in what it loads |
| `[setup] optimize_obb` | `true` | the OBB optimization of the first start |

---

## When something goes wrong

Everything is in `sd:/switch/spacehulk_nx/`: `debug.log`, and `crash.log`
after a crash (and Atmosphère's report in `sd:/atmosphere/crash_reports/`).
Please attach them to a report. Never share the game's files.

---

## Building

Requirements:

* Docker, the toolchain image `ghcr.io/vita2hos/devcontainer/vita2hos` and
  `devkitpro/devkita64` (the launcher),
* [libnx32](https://github.com/aks796/libnx32) built next to this folder
  (`../libnx32/prefix`),
* [mesa32](https://github.com/aks796/mesa32), built with
  `tools/mesa32-switch.patch` applied (`git am`: program binaries, so Unity's
  own shader cache works, and a larger buffer map in libdrm_nouveau), its
  `lib/` and `include/` in `portlibs32/`. The stock mesa32 release works too,
  without those two.
* [ffmpeg32](https://github.com/aks796/ffmpeg32) with H.264, its `lib/` and
  `include/` in `portlibs32/`:
  `FFMPEG_COMPONENTS="--enable-demuxer=mov --enable-decoder=h264 --enable-parser=h264 --enable-decoder=aac --enable-parser=aac" ./build.sh`
* the .NET SDK image `mcr.microsoft.com/dotnet/sdk:8.0` (the port's C#),
* Python 3 with Pillow (the icon), and `pyelftools` + `capstone` for the
  offset tools.

From your own APK:

```bash
git submodule update --init
python3 tools/make_icon.py <your APK>                              # launcher/icon.jpg
unzip -j <your APK> 'assets/bin/Data/Managed/*.dll' -d mod/refs   # the game's assemblies
mod/build_mod.sh      # mod/dcrmod.dll: the port's C# (mod/src)
./build.sh            # spacehulk_nx.nsp
launcher/build.sh     # launcher/spacehulk_nx.nro (carries the NSP)
```

None of the game's files end up in the repository or in the NRO.

Host tools:

```bash
python3 tools/offsets/derive_offsets.py <dir with libunity.so libmono.so>   # source/dcr_offsets.h
python3 runtime/tools/gen_imports.py --libs <dir with the three libraries> # source/imports.c
tools/obb/test_strip.sh <your OBB> out.obb       # the first start's OBB optimization, on the PC
python3 tools/prof_self.py prof_self.txt --elf spacehulk_nx.elf --libs <dir>  # [debug] profile_continuous
```

### How it works

The port started from [dcr_sea_nx](https://github.com/aks796/dcr_sea_nx)
(Disney Crossy Road, Unity 5.6 / Mono): its Mono JIT and Unity layers are
kept, and [NOTES.md](NOTES.md) is that port's technical notes. The Space Hulk
parts are the `sh_*.c` files in `source/`:

| File | What it does |
| --- | --- |
| `sh_quality.c` | texture limit, MSAA, lights, shadows, LOD, loading priority, held against the game's own quality levels |
| `sh_threaded.c` | Unity's multithreaded rendering, left off on Android by the game |
| `sh_io.c` | the OBB opened once and read with read-ahead; `stat()` below the OBB answered |
| `sh_obbindex.c` | the OBB's zip headers, read once and kept |
| `sh_obbstrip.c` | the first start's OBB optimization |
| `sh_video.c` | `Handheld.PlayFullScreenMovie` through FFmpeg |
| `sh_sched.c` | the render thread on a core of its own |
| `sh_padprobe.c` | `[debug] log_buttons`: which Unity button each Switch button becomes |
| `sh_imports_extra.c`, `sh_egl.c` | imports Unity 5.3 needs, EGL details |
| `mod/src/` | C# loaded into the game: the video menu's options for the Switch, the doors' animation |

---

## Credits

* **Liartes**: the Space Hulk port.
* **aks796**: the android32 runtime, libnx32, mesa32, ffmpeg32 and the
  dcr_sea_nx port this one started from.
* The Switch and Vita `.so` loader work of Andy Nguyen (TheOfficialFloW) and
  fgsfds; [vita2hos](https://github.com/xerpi/vita2hos) by xerpi; libnx by the
  switchbrew authors; Mesa; FFmpeg (LGPL); miniz.
* Space Hulk is © Hoplite Research / Full Control; Warhammer 40,000 and Space
  Hulk are trademarks of Games Workshop. This project is not affiliated with
  them.

## License

MIT, see [LICENSE](LICENSE). Third-party parts keep their own licenses.
