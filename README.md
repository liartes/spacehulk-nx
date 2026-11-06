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
* 720p handheld, 900p docked (configurable), a steady 30 fps (or up to 60,
  configurable).
* Intro movies (with sound), the game's music and sounds at 48 kHz.
* Fast loading: a mission loads in about 15 seconds.

### What was changed for the Switch

* Textures are drawn at half resolution: the game was made for the NVIDIA
  Shield (3 GB of memory) and a 32-bit Switch program has 1 GB.
* At the first start, the OBB is rewritten for the Switch, once (a few
  minutes): its textures start at the size the port draws them, so it is
  about 40% smaller and loads faster. The picture is the same.
* Unity's multithreaded rendering is enabled, and the heaviest settings of
  the top quality level are toned down: 2 dynamic lights drawn per pixel
  instead of 4 (a mission has ~130 of them, each drawn per pixel draws what
  it touches again), shorter shadow distance, less level of detail. All of
  it can be changed in `config.ini`.
* The undo button takes 0.2 s instead of 2.4 s: after the model is restored,
  the game waited a frame for each unit of the mission while rebuilding the
  board, even for the units already there.
* The game's code and the C# code Mono compiles are mapped in 2 MB blocks
  instead of 4 KB pages: the CPU finds their addresses in its translation
  cache (TLB) instead of looking them up in memory, which cost 30-80 ns each
  time (measured on the console). `[performance] large_pages` turns it off.
* Smoother missions: the game asked Unity to unload its unused assets nearly
  twice a second (every kill, every sound, a leftover from the iOS version),
  and each time froze ~90 ms while Unity walked every object -- the hitches
  when animations start or end. One a minute is let through
  (`[performance] unload_assets_every_s`).
* Mono, the runtime of the game's C#, runs from `mono-opt/libmono.so`:
  Unity's own Mono 2.6 for this game, from the same source, rebuilt with
  optimization (Unity shipped it unoptimized). Its garbage collections freeze
  the game about half as long. `[performance] optimized_mono = false` goes
  back to the game's.
* Mesa, the graphics driver, is built with a profile of the game
  (profile-guided optimization): ~20% less time per frame on the render
  thread, in missions at 60 fps.
* The program binaries Unity caches for its shaders are dropped when the
  graphics driver changes: one made by another driver build was refused, and
  the one Unity linked again in its place drew some textures flat grey
  (weapons). A port update with a new driver recompiles the shaders once (a
  longer first mission load).
* The video options the Switch cannot honour (resolution, full screen, v-sync)
  are hidden from the game's menu.
* The doors' leaves are drawn where their animation puts them: the missions
  were built with them merged into the static scenery, so on the Switch the
  doors opened (line of sight included) but looked shut.
* Online features (Google Play Games, multiplayer servers) are not available.
* Optional: `[game] unlock_dlc_campaigns` opens the campaigns this Android
  build carries but keeps locked -- Sword of Halcyon, Defilement of Honour,
  Fangs of Fenris (DLC on PC) and Messenger of Purgatory (sold in-app on
  mobile). It is meant for players who own them; off by default. Harbinger
  of Torment is not in the Android build. Known issue in Fangs of Fenris:
  some marine icons of the tactical view (the axe-armed ones) show as plain
  magenta squares -- their asset is missing from the Android build (the same
  in v0.1.4).

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
sd:/switch/spacehulk_nx/mono-opt/libmono.so      (from the release zip)
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
| `[display] frame_rate` | `30` | `60`: more fluid, 40-60 fps in big missions, twice the CPU and GPU work (heat, battery) |
| `[graphics] texture_resolution` | `half` | `full` does not fit in memory in missions |
| `[graphics] pixel_lights` | `2` | `0`: coarser lighting, ~40% fewer draws (for `frame_rate 60`); `game`: the top level's 4 |
| `[graphics] shadow_distance`, `lod_bias` | `100`, `3` | `game` for the game's own top-level values |
| `[graphics] antialiasing` | `0` | `2`, `4` or `game` |
| `[performance] loading_priority` | `high` | how fast Unity takes in what it loads |
| `[setup] optimize_obb` | `true` | the OBB optimization of the first start |
| `[performance] optimized_mono` | `true` | Mono from `mono-opt/libmono.so`; `false`: the game's |
| `[performance] unload_assets_every_s` | `60` | how often the game may unload unused assets; `0`: whenever it asks (hitches) |
| `[game] unlock_dlc_campaigns` | `false` | the DLC campaigns in the build, for players who own them |

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
  own shader cache works; a larger buffer map in libdrm_nouveau; and hooks
  for the GPU buffers' memory, which `sh_gpumem.c` keeps apart), its
  `lib/` and `include/` in `portlibs32/`. The stock mesa32 release works too,
  without those three.
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
| `sh_gpumem.c` | textures and meshes in memory of their own, so the heap does not fragment |
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
* `mono-opt/libmono.so`: Unity Technologies' Mono 2.6 fork
  ([github.com/Unity-Technologies/mono](https://github.com/Unity-Technologies/mono),
  branch `unity-5.3`, commit `28bc0dcd7b`), the runtime under the LGPL 2
  (`mono-opt/COPYING.LIB`), rebuilt by `tools/mono/build_libmono.sh` (see its
  `mono-opt/README.txt`).
* Space Hulk is © Hoplite Research / Full Control; Warhammer 40,000 and Space
  Hulk are trademarks of Games Workshop. This project is not affiliated with
  them.

## License

MIT, see [LICENSE](LICENSE). Third-party parts keep their own licenses.
