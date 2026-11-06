#---------------------------------------------------------------------------------
# Space Hulk -- Nintendo Switch wrapper (32-bit / AArch32, Unity 5.3 Mono)
#
# Ships NO game code and NO game assets: the game's own APK (the player's
# copy, any name) is read at run time, and its libraries are unpacked from it
# on the first launch.
#
# The build is the android32 runtime's (runtime/runtime.mk: devkitARM +
# libnx32 + mesa32 from portlibs32/); ./build.sh runs it in the toolchain
# container. Output: spacehulk_nx.nsp, which the launcher NRO carries (launcher/).
#---------------------------------------------------------------------------------
TARGET               := spacehulk_nx
PORT_NPDM_PROGRAM_ID := 0x0100000000005348
# FFmpeg (the intro movies, sh_video.c): ffmpeg32 with the H.264 decoder
# (FFMPEG_COMPONENTS="--enable-demuxer=mov --enable-decoder=h264
# --enable-parser=h264 --enable-decoder=aac --enable-parser=aac"), its lib/
# and include/ in portlibs32/
# make SH_PGO=gen: the training run of a profile-guided Mesa (source/sh_pgo.c,
# mesa32's build_mesa32.sh MESA_PGO): the instrumented Mesa of
# portlibs32-pgo-gen/ (its prefix-pgo-gen/ plus the FFmpeg above), objects in
# build-pgo-gen/
ifeq ($(SH_PGO),gen)
PORTLIBS     := $(CURDIR)/portlibs32-pgo-gen
BUILD        := build-pgo-gen
PORT_CFLAGS  += -DSH_PGO_GEN=1
PORT_LDFLAGS += -fprofile-generate
endif
PORTLIBS   ?= $(CURDIR)/portlibs32
PORT_LIBS  := -L$(PORTLIBS)/lib -lavformat -lavcodec -lavutil
# 2 MB blocks for the game's code and the JIT arena (source/sh_bigpages.c):
# the runtime's loader and libnx's jitCreate get aligned addresses and staging
# buffers through these two, without changing the runtime
PORT_LDFLAGS += -Wl,--wrap=virtmemFindCodeMemory -Wl,--wrap=__libnx_aligned_alloc
include runtime/runtime.mk

# FFmpeg's headers want int-sized enums (devkitARM's default is short)
$(BUILD)/sh_video.o: $(SOURCES)/sh_video.c $(RENDERER_STAMP) | $(BUILD)
	@echo $(notdir $<)
	@$(CC) -MMD -MP $(CFLAGS) -fno-short-enums -c $< -o $@

# Every variant links spacehulk_nx.elf here: .link-variant changes only when
# the variant or PORTLIBS does, so switching relinks (else a normal build would
# keep the instrumented program, its objects being older); so does a library
# replaced in PORTLIBS (another Mesa)
LINK_VARIANT := $(if $(SH_PGO),pgo-$(SH_PGO),normal) $(PORTLIBS)
.link-variant: FORCE
	@[ "$$(cat $@ 2>/dev/null)" = "$(LINK_VARIANT)" ] || echo "$(LINK_VARIANT)" > $@
$(TARGET).elf: .link-variant $(wildcard $(PORTLIBS)/lib/*.a)

# The Mesa linked in, by its libraries' checksum: Unity's shader cache is
# emptied when it changes (source/sh_shadercache.c)
SH_MESA_ID := $(shell cat $(sort $(wildcard $(PORTLIBS)/lib/libEGL.a $(PORTLIBS)/lib/libGLESv2.a $(PORTLIBS)/lib/libglapi.a $(PORTLIBS)/lib/libdrm_nouveau.a)) | md5sum | cut -c1-16)
$(BUILD)/sh_shadercache.o: CFLAGS += -DSH_MESA_ID=\"$(SH_MESA_ID)\"
$(BUILD)/sh_shadercache.o: .link-variant $(wildcard $(PORTLIBS)/lib/*.a)

# the port's C# (mod/build_mod.sh), carried inside the program
$(BUILD)/mod_blob.o: mod/dcrmod.dll

.PHONY: check
check:
	@echo "run on the host: python3 tools/offsets/verify_offsets.py <lib/armeabi-v7a>"
	@echo "                 python3 runtime/tools/gen_imports.py --check"
