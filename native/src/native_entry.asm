; Entry point for Java native methods implemented in guest (ARM64) code.
; Per-method stubs load r10 with the method's MethodInfo* and jump here. We spill
; the Win64 argument registers so NativeDispatch sees all positional arguments as
; one array ([rbp+16] = rcx, rdx, r8, r9, then the caller's stack arguments) and
; the first four xmm registers separately.

EXTERN NativeDispatch:PROC

.code

RefractNativeEntry PROC FRAME
    mov     [rsp+8], rcx
    mov     [rsp+16], rdx
    mov     [rsp+24], r8
    mov     [rsp+32], r9
    push    rbp
    .pushreg rbp
    mov     rbp, rsp
    .setframe rbp, 0
    sub     rsp, 96
    .allocstack 96
    .endprolog
    movdqu  [rsp+32], xmm0
    movdqu  [rsp+48], xmm1
    movdqu  [rsp+64], xmm2
    movdqu  [rsp+80], xmm3
    mov     rcx, r10            ; MethodInfo*
    lea     rdx, [rbp+16]       ; positional integer slots
    lea     r8, [rsp+32]        ; saved xmm0-3 (16 bytes each)
    call    NativeDispatch
    movq    xmm0, rax           ; float/double results are returned as raw bits
    lea     rsp, [rbp]
    pop     rbp
    ret
RefractNativeEntry ENDP

END
