; Step 2/3 test: 64-bit operands, REX prefixes and register extension.
; Sets up identity paging (2 MB pages), PAE, EFER.LME and CR0.PG, then enters 64-bit code.
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

    ; identity-map the first 2 MB: PML4 at 0x1000, PDPT at 0x2000, PD at 0x3000
    mov edi, 0x1000
    mov ecx, 0x1000          ; 16 KB of zeroed tables
    xor eax, eax
    rep stosd
    mov dword [0x1000], 0x2003
    mov dword [0x2000], 0x3003
    mov dword [0x3000], 0x83 ; 2 MB page, present, writable
    mov eax, 0x1000
    mov cr3, eax
    mov eax, cr4
    or eax, 0x20             ; PAE
    mov cr4, eax
    mov ecx, 0xC0000080      ; IA32_EFER
    rdmsr
    or eax, 0x100            ; LME
    wrmsr
    mov eax, cr0
    or eax, 0x80000000       ; PG: long mode becomes active
    mov cr0, eax
    jmp dword 0x18:lm

bits 64
lm:
    ; 64-bit add with wraparound: 0x1122334455667788 + (-1) = 0x1122334455667787
    mov rax, 0x1122334455667788
    mov rbx, -1
    add rax, rbx
    mov rcx, 0x1122334455667787
    cmp rax, rcx
    jne fail

    ; a 32-bit write clears the upper half: mov edx, 5 after rdx = -1
    mov rdx, -1
    mov edx, 5
    cmp rdx, 5
    jne fail

    ; REX.B and REX.R: r12 and r13 are real registers
    mov r12, 7
    add r12, 35
    cmp r12, 42
    jne fail

    ; push and pop use the 64-bit stack pointer
    push r12
    pop r13
    cmp r13, 42
    jne fail

    ; memory through extended registers: [rdi] with r12 stored and r13 loaded
    mov rdi, 0x5000
    mov [rdi], r12
    mov r13, [rdi]
    cmp r13, 42
    jne fail

    ; 8-bit registers with REX: sil is not dh
    mov rsi, 0x1122334455667788
    mov sil, 0x99
    mov rax, rsi
    mov rcx, 0x1122334455667799
    cmp rax, rcx
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

msg: db 'REX OK', 13, 10, 0

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
