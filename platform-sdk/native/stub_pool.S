// Pool of distinct entry points for ovr_* functions Refract does not implement by hand.
// Stub i loads its index and calls refract_ovr_stub_dispatch(i), which logs the name on the
// first call and returns a harmless value for that function's kind (0, "", an empty array
// or a request id). d0 is cleared too, so float/double getters read 0.
#if !defined(__aarch64__)
#error "The Platform SDK stand-in is loaded into arm64 games only"
#endif

#define REFRACT_OVR_STUB_COUNT 4096

    .text
    .p2align 3
    .globl refract_ovr_stub_pool
    .hidden refract_ovr_stub_pool
    .type refract_ovr_stub_pool, %function
refract_ovr_stub_pool:
    .set stub_index, 0
    .rept REFRACT_OVR_STUB_COUNT
    mov x9, #stub_index
    b refract_ovr_stub_common
    .set stub_index, stub_index + 1
    .endr
    .size refract_ovr_stub_pool, . - refract_ovr_stub_pool

    .p2align 2
    .type refract_ovr_stub_common, %function
refract_ovr_stub_common:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    mov w0, w9
    bl refract_ovr_stub_dispatch
    movi d0, #0
    ldp x29, x30, [sp], #16
    ret
    .size refract_ovr_stub_common, . - refract_ovr_stub_common

    .section .note.GNU-stack, "", %progbits
