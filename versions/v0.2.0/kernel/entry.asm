; NaOS kernel entry (NASM, 64-bit). UEFI already put us in long mode.
; ABI bridge: the bootloader is built with mingw (Microsoft x64 ABI), so it
; passes *naos_bootinfo_t in RCX. The kernel uses SysV ABI (RDI). Shim it.
; Then switch to our own 64K stack and call kmain. Never returns.

bits 64
global _start
global stack_top
extern kmain

section .bss
align 16
stack_bottom:
    resb 65536
stack_top:

section .text
_start:
    mov rdi, rcx          ; MS ABI arg1 -> SysV arg1 (bootinfo*)
    mov rsp, stack_top
    and rsp, -16
    cld
    call kmain
    cli
.hang:
    hlt
    jmp .hang
