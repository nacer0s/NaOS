/* NaOS kernel linker script. ELF64, load base 1M.
 * User programs (prog_*.o) live in .utext (2M-aligned): the only kernel
 * region mapped user-accessible, so ring-3 tasks can execute it. */
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

BASE = 0x100000;

/* Explicit segments: some linkers (e.g. ld 2.28) otherwise emit an extra
 * zero-based LOAD covering the file headers, which elf_load() must refuse
 * (it would clobber low memory). Two LOADs, pinned to real addresses. */
PHDRS
{
    kseg PT_LOAD;
    useg PT_LOAD;
}

SECTIONS
{
    . = BASE;
    .text ALIGN(4K) : {
        *(EXCLUDE_FILE (*prog_counter.o *prog_ticker.o *prog_peek.o) .text .text.*)
    } :kseg
    .rodata ALIGN(4K) : {
        *(EXCLUDE_FILE (*prog_counter.o *prog_ticker.o *prog_peek.o) .rodata .rodata.*)
    } :kseg
    .data ALIGN(4K) : {
        *(.data*)
    } :kseg
    .bss ALIGN(4K) : {
        *(COMMON)
        *(.bss*)
    } :kseg
    . = ALIGN(0x200000);
    PROVIDE(utext_start = .);
    .utext : {
        *prog_counter.o(.text .text.* .rodata .rodata.*)
        *prog_ticker.o(.text .text.* .rodata .rodata.*)
        *prog_peek.o(.text .text.* .rodata .rodata.*)
    } :useg
    PROVIDE(utext_end = .);
    PROVIDE(kernel_end = .);
    /DISCARD/ : {
        *(.eh_frame*)
        *(.comment*)
        *(.note*)
        *(.note.*)
    }
}
