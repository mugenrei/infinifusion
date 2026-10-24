option casemap:none

EXTERN g_winmm_exports:QWORD
EXTERN proxy_resolve_export:PROC

.code

proxy_dispatch PROC
    sub rsp, 136

    mov QWORD PTR [rsp + 32], rcx
    mov QWORD PTR [rsp + 40], rdx
    mov QWORD PTR [rsp + 48], r8
    mov QWORD PTR [rsp + 56], r9

    movdqu XMMWORD PTR [rsp + 64], xmm0
    movdqu XMMWORD PTR [rsp + 80], xmm1
    movdqu XMMWORD PTR [rsp + 96], xmm2
    movdqu XMMWORD PTR [rsp + 112], xmm3

    mov ecx, DWORD PTR [rsp + 136]
    mov rdx, QWORD PTR [rsp + 144]
    call proxy_resolve_export
    mov r11, rax

    mov rcx, QWORD PTR [rsp + 32]
    mov rdx, QWORD PTR [rsp + 40]
    mov r8, QWORD PTR [rsp + 48]
    mov r9, QWORD PTR [rsp + 56]

    movdqu xmm0, XMMWORD PTR [rsp + 64]
    movdqu xmm1, XMMWORD PTR [rsp + 80]
    movdqu xmm2, XMMWORD PTR [rsp + 96]
    movdqu xmm3, XMMWORD PTR [rsp + 112]

    add rsp, 136
    add rsp, 16
    jmp r11
proxy_dispatch ENDP

PROXY_STUB MACRO export_name:REQ, export_index:REQ, export_text:REQ
    .const
    proxy_name_&export_name BYTE export_text, 0

    .code
    ALIGN 16
    PUBLIC proxy_&export_name
proxy_&export_name PROC
    mov rax, QWORD PTR [g_winmm_exports + (export_index * 8)]
    test rax, rax
    jz SHORT @F
    jmp rax
@@:
    lea r10, proxy_name_&export_name
    push r10
    push export_index
    jmp proxy_dispatch
proxy_&export_name ENDP
ENDM

INCLUDE proxy_exports.inc

END
