; NaOS user program entry: stack is already set (iretq lands here).
; Call main, then exit(0). Ring 3: only syscalls from here on.

bits 64
global _ustart
extern main

section .text
_ustart:
    and rsp, -16
    call main
    mov eax, 0          ; SYS_EXIT
    int 0x80
.hang:
    hlt                 ; unreachable (exit never returns); faults if reached
    jmp .hang
