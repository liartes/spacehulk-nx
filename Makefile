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
PORT_LIBS  := -L$(CURDIR)/portlibs32/lib -lavformat -lavcodec -lavutil
include runtime/runtime.mk

# FFmpeg's headers want int-sized enums (devkitARM's default is short)
$(BUILD)/sh_video.o: $(SOURCES)/sh_video.c $(RENDERER_STAMP) | $(BUILD)
	@echo $(notdir $<)
	@$(CC) -MMD -MP $(CFLAGS) -fno-short-enums -c $< -o $@

# the port's C# (mod/build_mod.sh), carried inside the program
$(BUILD)/mod_blob.o: mod/dcrmod.dll

.PHONY: check
check:
	@echo "run on the host: python3 tools/offsets/verify_offsets.py <lib/armeabi-v7a>"
	@echo "                 python3 runtime/tools/gen_imports.py --check"
