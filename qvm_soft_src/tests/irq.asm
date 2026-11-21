bits 16
org 0x7C00
start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7000
    lgdt [gdtr]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp dword 0x08:pm

bits 32
pm:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, 0x7000
    lidt [idtr]
    mov dword [0x3000], 0

    ; master PIC: vectors 0x20-0x27, slave on IRQ2, ICW4 in 8086 mode
    mov al, 0x11
    out 0x20, al
    mov al, 0x20
    out 0x21, al
    mov al, 0x04
    out 0x21, al
    mov al, 0x01
    out 0x21, al
    ; slave PIC: vectors 0x28-0x2F, all masked
    mov al, 0x11
    out 0xA0, al
    mov al, 0x28
    out 0xA1, al
    mov al, 0x02
    out 0xA1, al
    mov al, 0x01
    out 0xA1, al
    mov al, 0xFF
    out 0xA1, al
    ; unmask IRQ0 only
    mov al, 0xFE
    out 0x21, al

    ; PIT channel 0: low/high byte, mode 2 (rate generator), divisor 50
    mov al, 0x34
    out 0x43, al
    mov al, 50
    out 0x40, al
    mov al, 0
    out 0x40, al

    sti
.wait:
    hlt                              ; sleeps until the timer interrupts
    cmp dword [0x3000], 3
    jl .wait
    cli

    mov esi, msg
.p:
    lodsb
    test al, al
    jz .done
    out 0xE9, al
    jmp .p
.done:
    mov dx, 0x501
    mov al, 0
    out dx, al
    hlt

handler20:
    inc dword [0x3000]
    mov al, 0x20
    out 0x20, al                     ; end of interrupt
    iretd

msg: db 'IRQ OK', 13, 10, 0

gdt:
    dq 0
    dw 0xFFFF, 0x0000
    db 0x00, 0x9A, 0xCF, 0x00
    dw 0xFFFF, 0x0000
    db 0x00, 0x92, 0xCF, 0x00
gdt_end:
gdtr:
    dw gdt_end - gdt - 1
    dd gdt

idt:
    times 0x20*8 db 0
    dw handler20
    dw 0x0008
    db 0
    db 0x8E
    dw 0
idt_end:
idtr:
    dw idt_end - idt - 1
    dd idt
