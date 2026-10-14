/* NaOS user program link script: fixed base 0x40100000 (inside the task
 * PT window [0x40100000, 0x40200000)), entry _ustart from crt0.asm. */
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_ustart)

BASE = 0x40100000;

SECTIONS
{
    . = BASE;
    .text ALIGN(4K) : {
        *(.text*)
    }
    .rodata ALIGN(4K) : {
        *(.rodata*)
    }
    .data ALIGN(4K) : {
        *(.data*)
    }
    .bss ALIGN(4K) : {
        *(.bss*)
        *(COMMON)
    }
    /DISCARD/ : {
        *(.eh_frame*)
        *(.comment*)
        *(.note*)
        *(.note.*)
    }
}
