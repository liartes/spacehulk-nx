#!/bin/sh
# build_libmono.sh -- Unity 5.3's libmono.so for armeabi-v7a, rebuilt from
# Unity's Mono fork with optimization.
#
#   tools/mono/build_libmono.sh <unity-mono checkout> <out dir> [OPT flags]
#
# The game's libmono.so (Unity 5.3.4f1, built 2016-03-11) is Mono 2.6.5 from
# github.com/Unity-Technologies/mono, branch unity-5.3, built by its
# external/buildscripts/build_runtime_android.sh: NDK r10e, GCC 4.8,
# android-9, armv7-a softfp VFP -- and CFLAGS without any -O, so GCC's -O0:
# 17-29% of its instructions only reload or spill variables to the stack. This
# is that recipe for armv7a alone (revision 28bc0dcd7b, 2016-02-19: its 910
# exports match the game's to the byte; same --prefix, whose paths it embeds), in a container (ubuntu:16.04 for an
# autotools of the time), with OPT appended to CFLAGS (empty: Unity's build).
# The NDK (dl.google.com) and Unity's krait-signal-handler are fetched into
# <out dir>/deps the first time. MIT.
set -eu
SRC="$(cd "$1" && pwd)"
OUT="$(mkdir -p "$2" && cd "$2" && pwd)"
OPT="${3:-}"
docker run --rm -v "$SRC:/src" -v "$OUT:/out" -e OPT="$OPT" -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
  ubuntu:16.04 bash -c '
set -e
export DEBIAN_FRONTEND=noninteractive
apt-get -qq update >/dev/null
apt-get -qq install -y build-essential autoconf automake libtool gettext pkg-config bison \
  perl git unzip curl ca-certificates python >/dev/null
git config --global --add safe.directory "*"
mkdir -p /out/deps && cd /out/deps
[ -d android-ndk-r10e ] || { curl -sSLo ndk.zip https://dl.google.com/android/repository/android-ndk-r10e-linux-x86_64.zip && unzip -q ndk.zip && rm ndk.zip; }
NDK=/out/deps/android-ndk-r10e
# the handler as it was when this libmono was built (its master moved to r16b/clang)
[ -d krait-signal-handler ] || { git clone -q https://github.com/Unity-Technologies/krait-signal-handler.git &&
  git -C krait-signal-handler checkout -q "$(git -C krait-signal-handler rev-list -1 --before=2016-03-11 HEAD)"; }
# one C file: built with the toolchain below (its ndk-build setup wants a clang r10e lacks)
TC0=$NDK/toolchains/arm-linux-androideabi-4.8/prebuilt/linux-x86_64/bin/arm-linux-androideabi-
KRAIT=/out/deps/krait-lib
mkdir -p $KRAIT
${TC0}gcc --sysroot=$NDK/platforms/android-9/arch-arm -march=armv7-a -mfloat-abi=softfp -mfpu=vfp -fpic -O2 -Wall \
  -c /out/deps/krait-signal-handler/jni/krait_signal_handler.c -o $KRAIT/krait.o
rm -f $KRAIT/libkrait-signal-handler.a && ${TC0}ar rcs $KRAIT/libkrait-signal-handler.a $KRAIT/krait.o
echo "krait: $(git -C /out/deps/krait-signal-handler log -1 --format="%h %ad" --date=short 2>/dev/null)"
# build in a copy: the checkout stays clean
rm -rf /build && cp -a /src /build && cd /build
# mono_arch_flush_icache issues Linux'"'"'s cacheflush syscall, which the port hooks
# at its entry (source/mono_rt.c); -O2 inlined it into five callers, each with
# a raw `svc 0` the Switch rejects. Kept a function of its own:
perl -0pi -e '"'"'s/^void\nmono_arch_flush_icache \(guint8/__attribute__((noinline)) void\nmono_arch_flush_icache (guint8/m'"'"' mono/mini/mini-arm.c
grep -q "noinline)) void" mono/mini/mini-arm.c || { echo "mini-arm.c: noinline not applied"; exit 1; }
PLATFORM_ROOT=$NDK/platforms/android-9/arch-arm
TC=$NDK/toolchains/arm-linux-androideabi-4.8/prebuilt/linux-x86_64
P=$TC/bin/arm-linux-androideabi-
export PATH="$TC/bin:$PATH"
CFLAGS="-DANDROID -DPLATFORM_ANDROID -DLINUX -D__linux__ -DHAVE_USR_INCLUDE_MALLOC_H -DPAGE_SIZE=0x1000 \
 -D_POSIX_PATH_MAX=256 -DS_IWRITE=S_IWUSR -DHAVE_PTHREAD_MUTEX_TIMEDLOCK -fpic -g -funwind-tables \
 -ffunction-sections -fdata-sections -DARM_FPU_VFP=1 -march=armv7-a -mfloat-abi=softfp -mfpu=vfp -DHAVE_ARMV6=1 $OPT"
LDFLAGS="-Wl,--wrap,sigaction -L$KRAIT -lkrait-signal-handler -Wl,--no-undefined \
 -Wl,--gc-sections -Wl,-rpath-link=$PLATFORM_ROOT/usr/lib -ldl -lm -llog -lc -Wl,--fix-cortex-a8"
(cd eglib && autoreconf -i >/dev/null 2>&1)
autoreconf -i >/dev/null 2>&1
./configure --prefix=/Users/builduser/buildslave/mono/build/builds/android --cache-file=android_cross.cache --host=arm-eabi-linux --disable-mcs-build \
  --disable-parallel-mark --disable-shared-handles --with-sigaltstack=no --with-tls=pthread --with-glib=embedded \
  --enable-nls=no mono_cv_uscore=yes \
  CC="${P}gcc --sysroot=$PLATFORM_ROOT" CXX="${P}g++ --sysroot=$PLATFORM_ROOT" CPP="${P}cpp" CXXCPP="${P}cpp" \
  CFLAGS="$CFLAGS" CPPFLAGS="$CFLAGS" CXXFLAGS="$CFLAGS" LDFLAGS="$LDFLAGS" \
  LD=${P}ld AR=${P}ar AS=${P}as RANLIB=${P}ranlib STRIP=${P}strip CPATH="$PLATFORM_ROOT/usr/include" > /out/configure.log 2>&1 \
  || { tail -30 /out/configure.log; exit 1; }
# parallel first; its Makefiles race (libgc: mach_dep.lo), so a serial pass finishes
make -j"$(nproc)" > /out/make.log 2>&1 || make >> /out/make.log 2>&1 || { grep -n -m5 -E "error|Error" /out/make.log; tail -20 /out/make.log; exit 1; }
cp mono/mini/.libs/libmono.so /out/libmono.so
chown -R $HOST_UID:$HOST_GID /out
echo "built /out/libmono.so (OPT=$OPT)"
'
