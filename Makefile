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
include runtime/runtime.mk

# the port's C# (mod/build_mod.sh), carried inside the program
$(BUILD)/mod_blob.o: mod/dcrmod.dll

.PHONY: check
check:
	@echo "run on the host: python3 tools/offsets/verify_offsets.py <lib/armeabi-v7a>"
	@echo "                 python3 runtime/tools/gen_imports.py --check"
