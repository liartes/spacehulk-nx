/* mod_blob.S -- mod/dcrmod.dll (the port's own C#, mod/build_mod.sh) inside
 * the program: dcr_mod.c opens it from memory. */
  .section .rodata.dcrmod, "a"
  .balign 16
  .global dcrmod_dll
dcrmod_dll:
  .incbin "mod/dcrmod.dll"
dcrmod_dll_end:
  .balign 4
  .global dcrmod_dll_size
dcrmod_dll_size:
  .word dcrmod_dll_end - dcrmod_dll
