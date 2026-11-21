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

    mov ebx, 0x1000
    mov ecx, 2
    mov dword [0x1010], 0x55AA
    mov eax, [ebx+ecx*4+8]
    cmp eax, 0x55AA
    jne fail

    call sub32
    cmp eax, 0x77
    jne fail

    int 0x30
    cmp edx, 0x1234
    jne fail

    mov al, 'P'
    out 0xE9, al
    mov al, 'M'
    out 0xE9, al
    mov al, 10
    out 0xE9, al
    mov al, 0
    mov dx, 0x501
    out dx, al
    hlt

fail:
    mov al, 'F'
    out 0xE9, al
    mov al, 1
    mov dx, 0x501
    out dx, al
    hlt

sub32:
    mov eax, 0x77
    ret

handler30:
    mov edx, 0x1234
    iretd

align 8
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

align 8
idt:
    times 0x30*8 db 0
    dw handler30
    dw 0x0008
    db 0
    db 0x8E
    dw 0
idt_end:
idtr:
    dw idt_end - idt - 1
    dd idt
