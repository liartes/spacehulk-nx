/* dcr_sigreturn.S -- the end of a delivered signal (dcr_exc_mono.c).
 *
 * dcr_sigreturn(ucontext): a handler has run on the thread's own stack; this
 * traps back into the fault handler (the runtime's exc_handler.c calls
 * port_exception_hook), which recognises the address and resumes the thread
 * with the (possibly modified) context in *ucontext -- Linux's rt_sigreturn.
 * MIT.
 */
    .syntax unified
    .arm
    .section .text.dcr_sigreturn, "ax", %progbits
    .global dcr_sigreturn
    .type dcr_sigreturn, %function
    .align 2
dcr_sigreturn:
    udf     #0xdc05
    b       .
    .size dcr_sigreturn, . - dcr_sigreturn
