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

    ; page directory at 0x10000, PT0 at 0x11000 (identity map of the first 4 MB), PT1 at 0x12000
    mov edi, 0x11000
    mov eax, 0x00000003
    mov ecx, 1024
.fill:
    mov [edi], eax
    add eax, 0x1000
    add edi, 4
    loop .fill
    mov dword [0x12000], 0x00200003  ; linear 0x00400000 -> physical 0x00200000
    mov dword [0x10000], 0x00011003  ; PDE 0 -> PT0
    mov dword [0x10004], 0x00012003  ; PDE 1 -> PT1
    mov eax, 0x10000
    mov cr3, eax
    mov eax, cr0
    or eax, 0x80000000
    mov cr0, eax

    ; a remapped page: write through linear 0x400000, read it back at the physical address
    mov dword [0x400000], 0xCAFE
    cmp dword [0x200000], 0xCAFE
    jne fail

    ; PDE 2 is empty: this write must fault (#PF, vector 14), and the handler resumes at recover
    mov dword [0x800000], 1
recover:
    cmp dword [0x3000], 0x800000
    jne fail
    cmp dword [0x3004], 2            ; error code: not present, write access
    jne fail

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

fail:
    mov al, 'F'
    out 0xE9, al
    mov dx, 0x501
    mov al, 1
    out dx, al
    hlt

handler14:
    mov eax, cr2
    mov [0x3000], eax
    mov eax, [esp]                   ; error code
    mov [0x3004], eax
    add esp, 16                      ; error code plus the saved eip, cs and eflags
    jmp recover

msg: db 'PG OK', 13, 10, 0

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
    times 0x0E*8 db 0
    dw handler14
    dw 0x0008
    db 0
    db 0x8E
    dw 0
idt_end:
idtr:
    dw idt_end - idt - 1
    dd idt
