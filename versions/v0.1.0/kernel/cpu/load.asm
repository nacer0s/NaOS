; NaOS table loaders: gdt_flush (reloads CS via far return) + idt_load.

bits 64
global gdt_flush
global idt_load

section .text

; void gdt_flush(uint64_t gdtr_addr) — RDI = &gdtr
gdt_flush:
    lgdt [rdi]
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    pop rdx              ; return address (stack still valid: same base 0)
    push 0x08
    push rdx
    retfq                ; reload CS = 0x08

; void idt_load(uint64_t idtr_addr) — RDI = &idtr
idt_load:
    lidt [rdi]
    ret
