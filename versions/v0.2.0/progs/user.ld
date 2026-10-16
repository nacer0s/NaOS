/* NaOS user program link script: fixed base 0x40100000 (inside the task
 * PT window [0x40100000, 0x40200000)), entry _ustart from crt0.asm. */
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_ustart)

BASE = 0x40100000;

/* Explicit single segment: some linkers otherwise align the LOAD down to
 * a 2M boundary (vaddr 0x40000000), which task_spawn_elf() must refuse
 * (outside the [PROG_BASE, PROG_END) window). */
PHDRS
{
    prog PT_LOAD;
}

SECTIONS
{
    . = BASE;
    .text ALIGN(4K) : {
        *(.text*)
    } :prog
    .rodata ALIGN(4K) : {
        *(.rodata*)
    } :prog
    .data ALIGN(4K) : {
        *(.data*)
    } :prog
    .bss ALIGN(4K) : {
        *(.bss*)
        *(COMMON)
    } :prog
    /DISCARD/ : {
        *(.eh_frame*)
        *(.comment*)
        *(.note*)
        *(.note.*)
    }
}
