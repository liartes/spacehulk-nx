mono-opt/libmono.so -- Space Hulk for Switch (spacehulk_nx)

This is the Mono runtime the game itself ships (Unity 5.3.4f1, libmono.so in
the APK), rebuilt from the same source with optimization: Unity built it
without any -O flag, so unoptimized.

Source:   https://github.com/Unity-Technologies/mono
          branch unity-5.3, commit 28bc0dcd7b (2016-02-19)
          (an -O0 build of it matches the game's library: every exported
          function the same size, the same data and bss)
Changes:  none to the code; mono_arch_flush_icache is kept out of line
          (__attribute__((noinline))) so the port can catch its Linux
          cacheflush call
Build:    tools/mono/build_libmono.sh <checkout> <out> "-O2 -fno-strict-aliasing"
          in github.com/liartes/spacehulk-nx: Android NDK r10e, GCC 4.8,
          android-9, armv7-a softfp VFP -- Unity's own recipe
          (external/buildscripts/build_runtime_android.sh), then stripped
          with --strip-unneeded
License:  the Mono runtime is under the GNU LGPL version 2 (COPYING.LIB here);
          its garbage collector (libgc) under the Boehm-Demers-Weiser license,
          eglib under the MIT license. The source above is the corresponding
          source; the build script and the one-line change are the port's.

Without this file, or with [performance] optimized_mono = false in
config.ini, the port runs the game's own libmono.so.
