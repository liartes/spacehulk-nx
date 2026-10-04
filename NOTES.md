# dcr_sea_nx: notes

How the port works, what the 32-bit libraries need, and what was learned
running a 32-bit Android game on the Switch. The player-facing page is
[README.md](README.md).

## Layout

| Part | What it is |
| --- | --- |
| `source/` | The wrapper: a 32-bit (AArch32) Horizon program. It loads the game's `libmain.so`, `libunity.so` and `libmono.so` and provides bionic libc, the Android NDK, JNI, EGL/GLES, audio, input and the file system. |
| `mod/src/` | C# compiled into `mod/dcrmod.dll`. It is embedded in the program and loaded into the game's Mono once Assembly-CSharp is open (`source/dcr_mod.c`). |
| `launcher/` | A 64-bit NRO that carries the 32-bit program in its romfs and installs it for a sphaira forwarder. |
| `tools/` | Host scripts: offsets, imports, the SD setup test and mod data generators. |

A 32-bit program cannot be an NRO, because hbloader is 64-bit. The launcher
therefore runs inside a sphaira forwarder title. It writes
`atmosphere/contents/<forwarder title id>/exefs.nsp`, which is the 32-bit
program with its `main.npdm` retargeted to that title id
(`source/dcr_exefs.h`), and restarts the title. From then on Atmosphère starts
the 32-bit program for that icon. The program installs newer builds from the
NRO in its folder by itself (`dcr_setup_update_from_nro`).

`dcrsea_nx.json` (the NPDM) sets `"is_64_bit": false` and
`"address_space_type": 0`, which is the 32-bit address space.

## The 32-bit libraries: what they need

The port builds with devkitARM, libnx32, and Mesa and libdrm_nouveau built
for AArch32 (mesa32). libnx32 is vita2hos's libnx port (vita2hos/libnx
`721c977`) with this project's fixes on the `master` branch of
[aks796/libnx32](https://github.com/aks796/libnx32).

**Status (2026-09-30).** The items below were collected from all seven
32-bit ports. Most are now fixed in the libraries themselves:

- **libnx32 `c6c53d20`** (on switchbrew/libnx master as of 2026-09-30, merged
  in `9b4f3b29`): items 1-7 below, and item 9 in part (the error is now
  EBUSY, not EIO; `stat` by name on a file open for writing still fails), plus `envAcquireOwnProcessHandle()`
  (a real process handle), a working AArch32 `armICacheInvalidate`,
  `nwindowGetDefaultDisplay()`, SHA-1/SHA-256/HMAC for AArch32, and a weak
  `timespec_get`.
- **[mesa32](https://github.com/aks796/mesa32)**: Mesa's `thrd_success` check
  (`24aa14fe`), `eglQuerySurface` sizes (`4e41d89f`), ETC2/ASTC on chipset
  0x120 (`2c27955c`), render-to-texture without storage (`dddc69a4`), and
  opt-in glthread (`972de9c1`).

Still open: item 8 (toolchain: devkitARM's target libraries are not
`-fPIC`), item 10 (`__appInit`), and the newlib items (a softfp newlib
build). This port keeps its own proven workarounds (`nx32_virtmem.c`,
`nx_init.c`, `exc32.S`, `crt0_reloc.c`, ...), which override the library's
versions; a new port can use the library's instead.

### libnx32

Already fixed in `cb01ef9f`: devkitARM builds with small enums
(`-fshort-enums`, `Tag_ABI_enum_size: small`), while the system reads 4-byte
enums. The fork fixed the enum-sized IPC data. `hidSetSupportedNpadIdType`
sent one byte per controller id, so wireless controllers could not connect;
the swkbd arguments, several applet structs and raw enum arguments were
fixed too. `serviceDispatchIn/InOut` now refuse enum raw data at compile
time, and `tools32/check_short_enums.sh` compares struct layouts both ways.

Still needed:

1. **`svcSetThreadCoreMask`** (`svc32.s`). The mask is declared `u32`, but the
   AArch32 SVC takes a 64-bit mask in `r2:r3`. `r3` carries whatever the
   caller left there, so the kernel returns InvalidCoreId and every thread
   stays on its creation core. It should take `u64` and load `r2:r3`.
   Workaround: `source/dcr_sched.c` issues the SVC with inline asm.
2. **`svcGetThreadCoreMask`** (`svc32.s`). The stub pushes three words and
   restores two, which unbalances the stack.
3. **`svcWaitForAddress` / `svcSignalToAddress`**. There are no AArch32 stubs.
   Two register layouts exist:
   - Atmosphère 1.8.0+ (64-bit value): `r0` address, `r1` type, `r2:r3` value,
     `r4:r5` timeout.
   - The older int32 value, which Ryujinx 1.1.1098 still uses: `r2` value,
     `r3:r4` timeout.

   `source/bionic_pthread.c` self-tests the layouts at boot. On HOS 21 with
   Atmosphère, and on Ryujinx, a 30 ms wait took 30 ms only with the int32
   layout. A libnx32 stub should be written and checked the same way.
4. **`kernel/virtmem.c`**. It has two 32-bit bugs (workaround:
   `source/nx32_virtmem.c` replaces the whole object):
   - Region ends are computed as `base + size` in `uintptr_t`. The 32-bit
     ASLR region ends at `0x1_0000_0000`, so the end wraps to 0 and every
     containment test at the top of the address space is wrong. Use `u64`.
   - `virtmemFindAslr` and `virtmemFindCodeMemory` search the whole ASLR
     region. For a 32-bit process the kernel accepts Shared, Code,
     AliasCode, SharedCode, GeneratedCode, Transfered and ThreadLocal mappings
     only inside the code region, `[0x200000, 0x40000000)`. Anything else
     fails with InvalidCurrentMemory, first seen in `hidInitialize`. The
     kernel reports that region as the stack region.

   Measured 32-bit layout: alias `0x40000000` (1 GiB), heap `0x80000000`
   (1 GiB), code/stack `0x200000` to `0x40000000`.
5. **`__libnx_initheap`**. It sizes the heap as TotalMemory - UsedMemory, which
   can be about 3 GB on hardware. The 32-bit heap region is 1 GiB, so
   `svcSetHeapSize` fails and libnx aborts before `main()`. Clamp to the heap
   region and leave room for the GPU driver's pools. Workaround:
   `source/nx_init.c`.
6. **audout** (`services/audout.h`). `AudioOutBuffer` holds native pointers
   (`next`, `buffer`), but the service reads the 64-bit layout for every
   client. On AArch32 the fields must be `u64`. Workaround:
   `source/dcr_audio.c` sends AppendAudioOutBuffer and
   GetReleasedAudioOutBuffers with the 64-bit layout itself. Any other IPC
   struct that contains a pointer needs the same audit.
7. **`exception32.s`**. It is a TODO stub, so faults cannot be handled in the
   process. The kernel's 32-bit entry works like this:
   - It enters at the program entry with `r0` = exception type, `r1` =
     `&ExceptionInfo`, and `sp` in the process-local region (under 448
     bytes).
   - It saves only `r0`-`r7`, `sp`, `lr`, `pc`, `pstate`, `esr` and `far`,
     and restores only those on `svcReturnFromException`.

   Workaround: `source/exc32.S` and `source/exc_handler.c`, which also
   emulate stores to the JIT's read-execute view.
8. **crt0 `__nx_dynamic`**. devkitARM's target libraries (newlib, libsysbase,
   libstdc++) are not built with `-fPIC`, so a PIE link has `R_ARM_RELATIVE`
   relocations in `.text` and `.rodata`. `__nx_dynamic` cannot apply them. If
   a Code page is made writable, Mesosphere turns it into CodeData, which can
   never be executable again (a hardware boot died with svcBreak `0xDC03`).
   Either build those libraries `-fPIC`, or apply the relocations through a
   writable alias:
   - Map each memory block with `svcMapProcessMemory`, one block per call; a
     call spanning blocks of different state fails with `0xD401`.
   - Get a real process handle by sending `CUR_PROCESS_HANDLE` over a
     session to yourself.

   Workaround: `source/crt0_reloc.c`, with `dcr32.specs` and `dcr32.ld`
   (`-z notext`).
9. **fsdev**. `stat()` opens the file to read its size. On a file already
   open for writing, that fails (fs result `0xE02`), so size a file through
   its open handle as a fallback. `access()` is unreliable for the same
   reason. Workaround: `source/bionic_io.c`.
10. **`__appInit`** aborts on any service failure. For 32-bit processes under
    Ryujinx, the time service's shared memory fails to map while everything
    else works. Reporting instead of aborting makes emulator testing
    possible. Workaround: `source/nx_init.c`.

### devkitARM newlib

- libm is soft-float: every `double` operation is a libgcc `__aeabi_d*` call.
  Results are correct but slow. A VFP (softfp) build would help. Workaround:
  `source/bionic_math.c` does sqrt, abs, rounding and min/max in VFP.
- `printf("%s", NULL)` crashes in `_svfprintf_r` (bionic prints `(null)`).
  Workaround: `--wrap=_svfprintf_r` and `--wrap=_vfprintf_r`
  (`source/bionic_printf.c`).
- `timespec_get` is declared but missing; Mesa's C11 threads need it
  (`source/host_compat.c`).
- ABI differences a bionic shim must convert: `mbstate_t` (4 bytes in bionic,
  8 in newlib), `off_t`, `timespec`/`timeval`, errno values, open flags.

### mesa32 (Mesa 20.1.0-rc3, libdrm_nouveau 1.0.1)

devkitPro's Switch branch of Mesa, built for AArch32. Small enums break Mesa in
two places, fixed by mesa32 `099a02a3` ("AArch32: don't depend on int-sized
enums"):

- Enum bitfields: `tgsi_opcode_info.opcode:10` becomes plain `unsigned`.
- `mesa_format`: the result of `_mesa_format_from_format_and_type` can be a
  `MESA_ARRAY_FORMAT` (bit 31). A 16-bit enum truncates it to a bogus
  format. It is now kept in a `uint32_t` in `st_format.c`, `glformats.c` and
  `formats.c`.

Code that includes FFmpeg headers needs `-fno-short-enums`
([ffmpeg32](https://github.com/aks796/ffmpeg32) is built that way).

### Toolchain flags

```text
-march=armv8-a+crc+crypto -mtune=cortex-a57 -mfloat-abi=softfp
-mfpu=neon-fp-armv8 -mtp=soft -fPIE -ftls-model=local-exec
```

`softfp` is not a preference. armeabi-v7a passes float and double in core
registers, and so do libnx32 and newlib, so every shim, callback and engine
entry point agrees without per-function annotations.

## Notes for porting other 32-bit games

- **Threads.** Horizon does not time-slice at the priorities games use. A
  runnable thread keeps its core until it blocks. Mesosphere rotates only the
  priority-59 queue of cores 0-2 (and 63 on core 3), every 10 ms. Android
  engines spin-wait for other threads (Unity's AssetBundle decoder does), so
  run guest threads at priority 59 and spread them over cores 0-2 with a
  64-bit core mask (`source/dcr_sched.c`).
- **Condition variables.** libnx's CondVar forgets a signal that arrives with
  no waiter. Unity signals thread start-up without holding the mutex, so
  bionic condvars need a sequence counter on `svcWaitForAddress`
  (`source/bionic_pthread.c`).
- **JIT (Mono).** Horizon never maps a page writable and executable at once,
  and a code page may go RW to RX only once. `jitCreate` (CodeMemory) gives
  the same pages at two addresses: RX, which Mono uses, and RW, where writes
  go. The pieces:
  - bulk copies go to RW through the `memcpy` family;
  - single stores data-abort, and the handler performs them on RW;
  - Mono's Linux `cacheflush` SVC is hooked (`source/jit_arena.c`).
- **Relocations.** 32-bit Android `.so` files use `SHT_REL`: the addend is
  the word already at the target. The import stub is `LDR PC,[PC,#-4]` plus
  the address (`source/so_util.c`).
- **Memory.** 4 GB of address space is carved into 1 GiB regions (see
  virtmem above). Reserve the game modules' region up front and size it from
  the ELF program headers (`source/config.h`).
- **Asset bundles** can be opened in place inside an uncompressed APK
  (`AssetBundle.LoadFromFileAsync(jar:file://...)`) instead of through
  Unity's WWW cache (`mod/src/FastBundles.cs`).
- **CPU boost.** `appletSetCpuBoostMode(FastLoad)` gives 1785 MHz but drops
  the GPU to its minimum. Use it during loads and long frames only
  (`source/dcr_boost.c`).
- **Controllers.** Send `hidSetSupportedNpadIdType` with 32-bit ids, or
  wireless controllers never connect (fixed in the libnx32 fork).
- **Ryujinx 1.1.1098 with 32-bit guests.**
  - It drops JIT translations only on unmap; no A32 cache maintenance reaches
    it.
  - Its A32 decoder lacks VSWP, VADDHN, VSRI, VSLI, VACGT/GE, VPADAL, VSHLL
    #esize and fixed-point VCVT.
  - Emulator-only workarounds are gated on `dcr_is_emulator()`.
- **Diagnostics that paid off.**
  - A watchdog that reports stuck threads with their stacks.
  - A per-long-frame CPU breakdown by thread.
  - A managed-exception tracer (`[exc]`).
  - A test script that drives input and saves frames in the emulator.

## The game

The game is the Android (armeabi-v7a) build of Disney Crossy Road: SEA
(`net.gogame.disney.crossyroad` 1.5.4, the gogame South-East Asia edition,
Unity 5.6.4f1). The port started as the Disney Crossy Road (world-wide,
Unity 2017.4) port and moved to this edition. The table lists what changed.

### What differs from the Disney Crossy Road port

| Area | Unity 2017.4 (DCR) | Unity 5.6.4f1 (SEA) -- what this port does |
| --- | --- | --- |
| Code addressing | pc-relative | GOT-relative (GOT base + offset): `tools/offsets/arm32.py` resolves both (`track_globals`, `gotoff_refs`); the 2017 header still regenerates byte for byte |
| Vsync globals | mutex, cond, counter in a row | one struct: counter +0x14, mutex +0x38, cond +0x3c |
| Mono entry points | `dlsym` of all 216 | 123 imported directly (DT_NEEDED libmono): imports are bound once all three modules are loaded, and cross-module bindings go through the same interposer as `dlsym`, so the IL patches and icall hooks still attach (`so_util.c`, `main.c`) |
| libc imports | -- | also `SHA1Init/Update/Final` (old bionic exported them; `bionic_sha1.c`, checked against `shasum`), `ferror`, `strnlen`, `strcasestr`, `exp2f` |
| Java start-up | initJni, nativeInitWebRequest | also `nativeFile(<apk>)` (the APK path) and `nativeInitWWW(WWW.class)` (`dcr_boot.c`) |
| WWW / asset bundles | native for `jar:file://` | **every** WWW request goes to the Java `com.unity3d.player.WWW` thread class -- including the game's asset bundles at `jar:file://.../base.apk!/assets/...`. `jni_www.c` implements it: stored entries streamed from game.apk, `file://` from the SD card, network URLs fail with `UnknownHostException` like an offline phone. Callback order and progress maths follow the class's own `runSafe()` |
| Game controller | InControl `NVidiaShield2017AndroidProfile` | SEA's older InControl matches `NVidiaShieldAndroidProfile` ("NVIDIA Corporation NVIDIA Controller"): the pad is named so both match; triggers also on L/RTRIGGER |
| Store | `newPlatformBiller` has a platform switch | always `OpenIABWrapper`: the IL patch builds the game's offline `PlatformBiller` instead, and the 7-argument `ReceiptManager.sendReceipt` invokes its `cloudSuccess` callback (the base store completes purchases only through it) |
| Firebase | -- | `GameController.NewGame` calls `FirebaseMessaging.Init`, which P/Invokes into `libApp-4.5.0.so` (not loaded): `DllNotFoundException` aborted the new game half-way (no save loaded; a NullReferenceException every frame). The IL patch makes `Init` return (always on) |
| OpenIAB | -- | its store-name constants (`OpenIabHelper.NAME_*`) are answered; unhandled Java builder setters return the builder (`jni_core.c`), so C# call chains go on |
| FMOD voices / 48 kHz | `AudioManager::InitNormal` at 0x19a4a4/0x19a428 | the same code at 0x81ee74/0x81edf4 (fields moved: `ldr r1,[r4,#184]`, `ldr r7,[r4,#220]`) |
| Not needed | -- | DCR's second-`UnloadUnusedAssets` and theme-switch-sound patches: SEA 1.5.4 has neither problem |

Everything else is the DCR port's: the loader and its crt0 relocation path,
the JIT arena and store emulation, the Boehm GC bridge, threads at priority 59
on cores 0-2, vsync pacing and CPU boost, FMOD audio into audout, touch and
controller input, the APK block cache, PlayerPrefs as `shared_prefs` XML,
config.ini, the launcher/forwarder install and self-update, and the IL patches
for unlocks, max stars, hop on press, the hidden top bar and shorter wipes.

First-launch setup (unpacking the APK, libraries and class list, or installing
a newer build) shows the PvZ Touch port's green progress bar, not the log.

## What this port adds (config.ini switches each off)

Most of it is C# (`mod/src/`), compiled into `mod/dcrmod.dll`, embedded in the
program and loaded into the game's Mono once Assembly-CSharp is open
(`source/dcr_mod.c`): hooks rewrite a method's first two ARM words into a jump
to the replacement, which can call the original through a trampoline.

- **Controller menus** (`MenuPad.cs`, `[controls] menu_controls`): the game's
  own TV-box controller UI (focus highlights, A presses, B goes back), with a
  button focused on every screen, focus that moves on the press and repeats,
  B sent to the back button of the screen on top (a back arrow before a
  close X; the daily missions' prize info turns back to the missions, as its
  arrow does), lists (leaderboards) scrolled with the left stick, and in the
  character select A = play, L/R and ZL/ZR = the next theme (on the theme
  cards, the left stick's own step; from the characters, the strip glides
  there with the focus left in place, so no focus sounds), Y = random,
  X = info.
  The title's event box, which the game drew no differently, darkens while
  it has the focus.
- **Online without a network** (`Online.cs`, `[online] pretend_online`): the
  game's server answered in-process -- no offline popups, weekly events and
  challenges in rotation, the ticket machine, free gifts, the daily login
  reward, leaderboards, rewarded "ads" granted at once.
- **Local multiplayer** (`LocalMP.cs`, `[multiplayer]`): the online mode's
  rooms for up to 4 players on one Switch, one controller each; the game's
  match server runs in-process. Coin Crazy, Snatch and Run (the Crown), and
  the two modes the game named but never made: Last One Standing and Hop
  Non-Stop, knock-outs with no timer (the road speeds up / runs with the
  leader). A mode's button opens the room (no quick play / join / create
  screen). Any theme's world for the match (player 1's L/R or ZL/ZR: the character
  select's own theme cards in a row along the sign's band), each player's
  character from the game's own character select on their controller (Y;
  only its Play button, the player's number big), a profile per player (X),
  more controllers from the room (+), + to pause (B ends the match). The
  multiplayer leaderboard (the mode screen's podium, and after every match)
  is the session's wins, each row with its player's number. The room's
  legend, badges and pause screen are `Ui.cs`, in the game's fonts, with
  button pictures from `tools/mod/make_button_icons.py`.
- **Switch profiles** (`Profiles.cs`, `source/dcr_profile.c`, `[game]
  profiles`): the leaderboard's first opening offers the Switch's profile
  picker (B: none, not asked again; X / Y in the leaderboard change it, on
  one bar along its bottom that says who is playing, the list ending above
  it); runs
  then go on that profile's name and icon, and waiting-room players can pick
  one with X. The leaderboard is this Switch's high scores (no made-up
  players, no rewards), in the game's three tabs: Overall (each player's
  best on any world), By World (L/R browse the worlds) and Challenge (the
  current Weekend Challenge's world while it runs, its challenge runs
  included -- the game keeps those off its own scores; a new challenge starts
  it afresh). Scores in `leaderboard.txt` (version 2: version 1's no-profile
  scores, possibly copied from the screen, are dropped once).
- **DuckTales** (`Characters.cs`, `[game] ducktales`): the world-wide
  edition's 26 DuckTales figurines, their music and sounds, and the DuckTales
  card, in the SEA edition's hidden Duckburg world. Needs **your own Disney
  Crossy Road (world-wide edition, 3.x) APK**: in the game folder under any
  name (it becomes `dcr.apk`), or built into the launcher (`DCR_APK=<apk> launcher/build.sh`); its
  DuckTales bundles, asset table and English logos are copied out at start-up
  (`dcr_setup.c`). `tools/mod/gen_ducktales.py` makes the recipe.
- **APKs under any name** (`dcr_setup_adopt_apks`): each `.apk` in the folder
  is identified by the package name in its AndroidManifest.xml and renamed
  `game.apk` (SEA) or `dcr.apk` (world-wide), replacing the one before.
- **The old folder** (`source/dcr_migrate.h`): builds up to 202609300710 used
  `/switch/disneycrossyroadsea`; the launcher and the wrapper move its
  contents into `/switch/dcr_sea_nx` on the first start.
- **Sorcerer's Apprentice Mickey** (`[game] sorcerer_mickey`): an Epic
  Mickey & Friends figurine from models in the game's files. Like the Genie,
  every few seconds he turns the obstacle ahead into a marching Fantasia
  broom (`Sorcerer.cs`).
- **Figurines from the game files** (`[game] hidden_characters`): Golden
  Camel, Dragon Genie, Elf Pleakley, Santa Jumba, Vampire Stitch, Witch Lilo
  and The Ocean (released only in the world-wide edition); Classic Mickey, the
  Fantasia Broom, Safari Mickey, Max, Oswald, Ortensia, the Scuba Diver and
  Human Cadenza (never released); the hidden Meowana and Hanging Tree. All are
  built from models already in the SEA bundles (`tools/mod/gen_extras.py`)
  and won in the prize machine.
- **Bundles read in place** (`FastBundles.cs`, `[performance]
  bundles_in_place`): asset bundles are opened inside game.apk instead of
  being copied into memory through the WWW class first. Either way, the
  requests that reach a bundle while it is still loading now hold it too
  (the game's manager forgot them, and the first finished asset unloaded the
  bundle under the rest: failed mesh loads, and the title's preload waiting
  out two 5 s timeouts); `Preload.cs` lets a wipe go on once its mesh
  preload has made no progress for 2 s with nothing still loading.
- **Where the CPU goes** (`dcr_boost.c`): once a minute the log gets each busy
  thread's share of a core and the SoC's temperature (`[cpu]`). The boost is
  off while a system screen (the controller screen, the profile picker)
  holds the frame.

## Build details

Needs Docker, the toolchain image `ghcr.io/vita2hos/devcontainer/vita2hos`,
the patched libnx32 (a libnx32 checkout next to this one, built with its
`./build.sh`) and the 32-bit Mesa in `portlibs32/` (mesa32's `prefix/` or its
release tarball).

```bash
python3 tools/mod/publicize.py <game.apk>   # mod/refs/ (your game's assemblies, never shipped)
mod/build_mod.sh      # mod/dcrmod.dll (the .NET SDK image's Roslyn)
./build.sh            # dcrsea_nx.nsp + dcrsea_nx.build
launcher/build.sh     # launcher/dcr_sea_nx.nro (carries the NSP)
DCR_APK=<Disney Crossy Road 3.x APK> launcher/build.sh   # ... and the DuckTales pack
```

A launcher built with `DCR_APK` carries five files of Disney Crossy Road (the
DuckTales bundles, asset table and logos) in its romfs, so the SD card needs
no `dcr.apk`. The release NRO is built this way. They are of no use without
the player's own SEA APK.

Host checks against your own APK:

```bash
python3 tools/offsets/derive_offsets.py <apk>/lib/armeabi-v7a   # source/dcr_offsets.h
python3 tools/gen_imports.py --libs <apk>/lib/armeabi-v7a      # source/imports.c
python3 tools/test_setup.py <game.apk>
```

`tools/test_ilpatch.py` checks the Disney Crossy Road (world-wide) APK's
patches; the SEA ones were checked by decompiling the patched assembly.

## Testing in Ryujinx (1.1.1098)

Run `dcrsea_nx.nsp` directly (title 0x0100000000001013) with the game folder
at `sdcard/switch/dcr_sea_nx/`. It reaches the title screen and plays.
What only the emulator needs is switched on by `dcr_is_emulator()` and never
runs on a Switch:

- `emu_fixups.c`: its A32 decoder lacks VSWP, VADDHN, VSRI, VSLI, VACGT/GE,
  VPADAL, VSHLL #esize and fixed-point VCVT, all used by libunity 5.6 (and
  Mesa): each becomes a branch to a stub of instructions it has. The stubs
  were differential-tested against the originals in Unicorn. Only `.text` is
  scanned (libunity's executable segment also holds `.rodata` tables).
- `jit_arena.c`: it never notices code rewritten in place. The JIT arena is an
  `svcMapMemory` alias, and a remap (unmap + map) is what drops its
  translations. Mono's call-site patches are undone at once and applied
  between frames with the other threads paused. Free arena pages hold calls
  to a stray-jump reporter (translating zero words made one giant no-op block).
- `test_script.txt` in the game folder drives input without hands:
  `<seconds> tap <x> <y>` (thousandths of the screen), `<seconds> press
  <a|b|x|y|l|r|zl|zr|plus|minus|up|down|left|right> [p2..p8]`, `<seconds> cap`,
  `<seconds> quit`. Captures retry all-black read-backs. Flag files start
  the mod's test drivers (`mod/src/TestDriver.cs`): `test_mp` (a local
  multiplayer room; its first word picks the mode: coins, crown, lastone,
  hop), `test_chars` (the character select, DuckTales, then Sorcerer's
  Apprentice Mickey; `pad` in it: only opens it, for the script),
  `test_tasks` (the daily missions turned to their prize info), `test_sorcerer` (his spells at the title; another
  figurine id in it: a run as that one), `test_profile` (the profile
  picker and leaderboard.txt), `test_board` (the title's leaderboard
  button) and `test_modes` (the title's multiplayer button, for the mode
  screen; needs a save past the tutorial). `hold` in
  test_mp keeps the room open until the script presses down for player 2.

## When something goes wrong

Everything is in `sd:/switch/dcr_sea_nx/`: `debug.log`, `crash.log`
(the port's own exception handler) and Atmosphère's report in
`sd:/atmosphere/crash_reports/`. `[exc]` lines name the C# method behind the
first managed exceptions (Unity's release log gives only the message). A hang
gets a `[watchdog]` report after 10 s; when a lock is stuck it prints straight
to the debug output instead.
