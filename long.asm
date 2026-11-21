; Step 3 test: RIP-relative addressing and a 64-bit interrupt through a 16-byte IDT gate.
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

    ; 64-bit IDT at 0x6000; gate 0x30 points at handler30, selector 0x18, type 0xE (interrupt gate)
    mov edi, 0x6000 + 0x30 * 16
    mov eax, handler30
    mov word [edi], ax
    mov word [edi + 2], 0x18
    mov byte [edi + 4], 0
    mov byte [edi + 5], 0x8E
    shr eax, 16
    mov word [edi + 6], ax
    mov dword [edi + 8], 0
    mov dword [edi + 12], 0
    lidt [idtr]

    ; identity-map the first 2 MB with 2 MB pages
    mov edi, 0x1000
    mov ecx, 0x1000
    xor eax, eax
    rep stosd
    mov dword [0x1000], 0x2003
    mov dword [0x2000], 0x3003
    mov dword [0x3000], 0x83
    mov eax, 0x1000
    mov cr3, eax
    mov eax, cr4
    or eax, 0x20
    mov cr4, eax
    mov ecx, 0xC0000080
    rdmsr
    or eax, 0x100
    wrmsr
    mov eax, cr0
    or eax, 0x80000000
    mov cr0, eax
    jmp dword 0x18:lm

bits 64
lm:
    ; RIP-relative load, and LEA of the same address
    mov rax, [rel value]
    mov rcx, 0x1122334455667788
    cmp rax, rcx
    jne fail
    lea rbx, [rel value]
    mov rax, [rbx]
    cmp rax, rcx
    jne fail

    ; RIP-relative store with an immediate: the displacement counts from the end of the instruction
    mov qword [rel out], 0x42
    mov rax, [rel out]
    cmp rax, 0x42
    jne fail

    ; 64-bit interrupt through the 16-byte gate, then IRETQ back here
    mov rbx, 1
    int 0x30
    cmp rbx, 2
    jne fail

    mov rsi, msg
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

fail:
    mov al, 'F'
    out 0xE9, al
    mov dx, 0x501
    mov al, 1
    out dx, al
    hlt

handler30:
    mov rbx, 2
    iretq

value: dq 0x1122334455667788
out:   dq 0
msg:   db 'LONG OK', 13, 10, 0

idtr:
    dw 0x31 * 16 - 1
    dd 0x6000

gdt:
    dq 0
    dw 0xFFFF, 0x0000
    db 0x00, 0x9A, 0xCF, 0x00      ; 0x08: 32-bit code
    dw 0xFFFF, 0x0000
    db 0x00, 0x92, 0xCF, 0x00      ; 0x10: data
    dw 0xFFFF, 0x0000
    db 0x00, 0x9A, 0xAF, 0x00      ; 0x18: 64-bit code (L=1)
gdt_end:
gdtr:
    dw gdt_end - gdt - 1
    dd gdt
