bits 16
org 0x7C00
start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    mov word [0x21*4], handler21
    mov word [0x21*4+2], 0

    mov ax, 1234
    add ax, 4321
    cmp ax, 5555
    jne fail1

    mov bx, 0
    mov cx, 10
.l1:
    add bx, cx
    loop .l1
    cmp bx, 55
    jne fail2

    call sub1
    cmp ax, 0x42
    jne fail3

    mov al, 7
    mov dl, 6
    mul dl
    cmp ax, 42
    jne fail4
    mov dx, 0
    mov ax, 1000
    mov cx, 7
    div cx
    cmp ax, 142
    jne fail4
    cmp dx, 6
    jne fail4

    mov si, src
    mov di, 0x8000
    mov cx, 8
    rep movsb
    cmp byte [0x8007], 'Z'
    jne fail5

    mov di, 0x8100
    mov al, 'A'
    mov cx, 3
    rep stosb
    cmp byte [0x8102], 'A'
    jne fail5
    cmp byte [0x8103], 0
    jne fail5

    mov ax, 0x8001
    shl ax, 1
    jnc fail6
    cmp ax, 2
    jne fail6

    mov eax, 0x12345678
    add eax, 0x11111111
    cmp eax, 0x23456789
    jne fail7

    int 0x21
    cmp bx, 0xBEEF
    jne fail8

    mov si, src
    mov di, src
    mov cx, 3
    repe cmpsb
    jne fail9

    mov ax, -5
    cmp ax, 3
    jge fail10

    mov si, pass
    call puts
    mov al, 0
    mov dx, 0x501
    out dx, al
    hlt

fail1: mov al, 1
       jmp report
fail2: mov al, 2
       jmp report
fail3: mov al, 3
       jmp report
fail4: mov al, 4
       jmp report
fail5: mov al, 5
       jmp report
fail6: mov al, 6
       jmp report
fail7: mov al, 7
       jmp report
fail8: mov al, 8
       jmp report
fail9: mov al, 9
       jmp report
fail10: mov al, 10
       jmp report

report:
    push ax
    mov si, failmsg
    call puts
    pop ax
    mov dx, 0x501
    out dx, al
    hlt

puts:
    lodsb
    test al, al
    jz .done
    out 0xE9, al
    jmp puts
.done:
    ret

sub1:
    mov ax, 0x42
    ret

handler21:
    mov bx, 0xBEEF
    iret

src:     db 'abcdefgZ'
pass:    db 'REAL PASS', 13, 10, 0
failmsg: db 'REAL FAIL', 13, 10, 0
