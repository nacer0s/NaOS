/* NaOS kernel linker script. ELF64, load base 1M.
 * User programs (prog_*.o) live in .utext (2M-aligned): the only kernel
 * region mapped user-accessible, so ring-3 tasks can execute it. */
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

BASE = 0x100000;

SECTIONS
{
    . = BASE;
    .text ALIGN(4K) : {
        *(EXCLUDE_FILE (*prog_counter.o *prog_ticker.o *prog_peek.o) .text .text.*)
    }
    .rodata ALIGN(4K) : {
        *(EXCLUDE_FILE (*prog_counter.o *prog_ticker.o *prog_peek.o) .rodata .rodata.*)
    }
    .data ALIGN(4K) : {
        *(.data*)
    }
    .bss ALIGN(4K) : {
        *(COMMON)
        *(.bss*)
    }
    . = ALIGN(0x200000);
    PROVIDE(utext_start = .);
    .utext : {
        *prog_counter.o(.text .text.* .rodata .rodata.*)
        *prog_ticker.o(.text .text.* .rodata .rodata.*)
        *prog_peek.o(.text .text.* .rodata .rodata.*)
    }
    PROVIDE(utext_end = .);
    PROVIDE(kernel_end = .);
    /DISCARD/ : {
        *(.eh_frame*)
        *(.comment*)
        *(.note*)
        *(.note.*)
    }
}
