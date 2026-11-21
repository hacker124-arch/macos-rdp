; Step 4 test: the SSE2 subset, the integer instructions added for firmware, NOP forms, x87 control
; instructions, and the board devices (CMOS, PCI config space with no device, COM1 status).
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

    ; rotate through carry
    stc
    mov al, 0x81
    rcl al, 1                ; al = 0x03, CF = 1
    jnc fail
    cmp al, 0x03
    jne fail
    clc
    mov al, 0x01
    rcr al, 1                ; al = 0x00, CF = 1
    jnc fail
    test al, al
    jnz fail

    ; double-precision shifts
    mov eax, 0x12345678
    mov ebx, 0x9ABCDEF0
    shld eax, ebx, 8         ; 0x3456789A
    cmp eax, 0x3456789A
    jne fail
    mov eax, 0x12345678
    shrd eax, ebx, 4         ; 0x01234567
    cmp eax, 0x01234567
    jne fail

    ; xadd and cmpxchg
    mov eax, 5
    mov ebx, 7
    xadd ebx, eax            ; ebx = 12, eax = 7
    cmp ebx, 12
    jne fail
    cmp eax, 7
    jne fail
    mov eax, 12
    mov ecx, 99
    cmpxchg ebx, ecx         ; equal: ebx = 99, ZF = 1
    jne fail
    cmp ebx, 99
    jne fail
    mov eax, 1
    cmpxchg ebx, ecx         ; not equal: eax = 99, ZF = 0
    je fail
    cmp eax, 99
    jne fail

    ; bit test family
    mov eax, 0
    bts eax, 5
    cmp eax, 0x20
    jne fail
    bt eax, 5
    jnc fail
    btr eax, 5
    test eax, eax
    jnz fail
    mov ebx, 0
    btc ebx, 3
    cmp ebx, 8
    jne fail
    bt ebx, 3
    jnc fail

    ; multi-byte NOPs
    db 0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00
    db 0x0F, 0x1F, 0x40, 0x00

    ; x87 control words
    fninit
    fnstcw [cw]
    cmp word [cw], 0x037F
    jne fail

    ; SSE2
    movdqu xmm0, [v1]
    movdqu xmm1, [v2]
    paddd xmm0, xmm1         ; 11, 22, 33, 44
    movdqu [buf], xmm0
    cmp dword [buf], 11
    jne fail
    cmp dword [buf + 8], 33
    jne fail

    pxor xmm2, xmm2
    movdqu [buf], xmm2
    cmp dword [buf], 0
    jne fail
    cmp dword [buf + 12], 0
    jne fail

    movdqu xmm3, [v1]
    movdqu xmm4, [v1]
    pcmpeqb xmm3, xmm4
    pmovmskb eax, xmm3
    cmp eax, 0xFFFF
    jne fail

    pshufd xmm5, xmm1, 0x1B  ; reversed: 40, 30, 20, 10
    movdqu [buf], xmm5
    cmp dword [buf], 40
    jne fail
    cmp dword [buf + 12], 10
    jne fail
    psrldq xmm5, 8           ; 20, 10, 0, 0
    movdqu [buf], xmm5
    cmp dword [buf], 20
    jne fail
    cmp dword [buf + 4], 10
    jne fail
    cmp dword [buf + 8], 0
    jne fail

    movq xmm6, [v1]          ; low 64 bits: 1, 2
    movq [buf], xmm6
    cmp dword [buf], 1
    jne fail
    cmp dword [buf + 4], 2
    jne fail

    mov eax, 0x55
    movd xmm7, eax
    movd ebx, xmm7
    cmp ebx, 0x55
    jne fail

    ; board devices
    mov al, 0x0B             ; CMOS status B: BCD, 24-hour
    out 0x70, al
    in al, 0x71
    cmp al, 0x02
    jne fail
    mov eax, 0x80000000      ; PCI config address
    mov dx, 0x0CF8
    out dx, eax
    mov dx, 0x0CFC
    in eax, dx               ; empty bus: all ones
    cmp eax, 0xFFFFFFFF
    jne fail
    mov dx, 0x03FD           ; COM1 line status: transmitter empty
    in al, dx
    test al, 0x20
    jz fail

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

msg: db 'OPS OK', 13, 10, 0
align 16
v1:  dd 1, 2, 3, 4
v2:  dd 10, 20, 30, 40
buf: times 16 db 0
cw:  dw 0

gdt:
    dq 0
    dw 0xFFFF, 0x0000
    db 0x00, 0x9A, 0xCF, 0x00      ; 0x08: 32-bit code
    dw 0xFFFF, 0x0000
    db 0x00, 0x92, 0xCF, 0x00      ; 0x10: data
gdt_end:
gdtr:
    dw gdt_end - gdt - 1
    dd gdt
