/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Failed-commit regression test, run by kernel_mm.py.
 * Repeat ten times: reject 256 MiB, then commit, write, and release 16 MiB.
 * Leaked pages block the smaller commit; leaked reservations exhaust VA space.
 * Then a kernel call on a reserved page: its fault goes to the thread's SEH
 * handler, which commits the page, and the call runs again; an overlapping
 * RtlMoveMemory into such a page moves each byte once.
 * Code is at 0x11000; thunks at 0x10f00 follow kernel_mm.py's IMPORTS order.
 * Print the result and halt. */
.intel_syntax noprefix
.set THUNK, 0x10f00
.set DbgPrint,                THUNK + 4*0
.set NtAllocateVirtualMemory, THUNK + 4*1
.set NtFreeVirtualMemory,     THUNK + 4*2
.set RtlInitializeCriticalSection, THUNK + 4*3
.set RtlMoveMemory,           THUNK + 4*4

.set MEM_COMMIT,     0x1000
.set MEM_RESERVE,    0x2000
.set MEM_RELEASE,    0x8000
.set PAGE_READWRITE, 0x04

/* push imm32 (LLVM's Intel syntax makes "push offset sym" a 16-bit push) */
.macro PUSHI sym
    .byte 0x68
    .long \sym
.endm

.macro STEP n
    mov dword ptr [step], \n
.endm

/* NtAllocateVirtualMemory(&base = 0, 0, &size = bytes, MEM_COMMIT, PAGE_READWRITE) */
.macro COMMIT bytes
    mov dword ptr [base], 0
    mov dword ptr [size], \bytes
    push PAGE_READWRITE
    push MEM_COMMIT
    PUSHI size
    push 0
    PUSHI base
    call dword ptr [NtAllocateVirtualMemory]
.endm

.text
.globl _start
_start:
    mov dword ptr [round], 0
again:
    /* 1. Reject a commit larger than physical RAM. */
    STEP 1
    COMMIT 0x10000000
    cmp eax, 0xC0000017                 /* STATUS_NO_MEMORY */
    jne fail

    /* 2. A smaller commit must succeed. */
    STEP 2
    COMMIT 0x1000000
    test eax, eax
    jnz fail
    mov edi, dword ptr [base]
    test edi, edi
    jz fail
    cmp dword ptr [size], 0x1000000
    jne fail

    /* 3. Verify writes to the first and last pages. */
    STEP 3
    mov dword ptr [edi], 0x12345678
    mov dword ptr [edi + 0xfffffc], 0x9abcdef0
    cmp dword ptr [edi], 0x12345678
    jne fail
    cmp dword ptr [edi + 0xfffffc], 0x9abcdef0
    jne fail

    /* 4. Release the entire reservation. */
    STEP 4
    mov dword ptr [size], 0
    push MEM_RELEASE
    PUSHI size
    PUSHI base
    call dword ptr [NtFreeVirtualMemory]
    test eax, eax
    jnz fail

    inc dword ptr [round]
    cmp dword ptr [round], 10
    jb again

    /* 5. RtlInitializeCriticalSection on a reserved page: the fault goes
     * to the SEH handler, which commits the page and continues, and the
     * call runs again (Rainbow Six: Lockdown commits its heap so). The
     * fault is a write: its pager commits a page read-only on a read. */
    STEP 5
    mov dword ptr [base], 0
    mov dword ptr [size], 0x10000
    push PAGE_READWRITE
    push MEM_RESERVE
    PUSHI size
    push 0
    PUSHI base
    call dword ptr [NtAllocateVirtualMemory]
    test eax, eax
    jnz fail
    STEP 6
    PUSHI seh_commit
    push dword ptr fs:[0]
    mov dword ptr fs:[0], esp
    mov eax, dword ptr [base]
    add eax, 0x3000
    mov dword ptr [cs_va], eax
    push eax
    call dword ptr [RtlInitializeCriticalSection]
    pop eax
    mov dword ptr fs:[0], eax
    add esp, 4
    STEP 7
    cmp dword ptr [seh_count], 1
    jne fail
    mov eax, dword ptr [cs_va]
    cmp dword ptr [eax + 0x10], 0xFFFFFFFF  /* LockCount */
    jne fail
    cmp dword ptr [seh_write], 1            /* the call's store faulted as a write */
    jne fail

    /* 8. RtlMoveMemory down by 8 over a reserved page: pages 0 and 2 of
     * a new region committed, page 1 reserved. The move writes page 0
     * before it meets page 1 (the copy loads its source's ends first), then
     * runs again once the handler commits page 1, from the source as it
     * was: each byte moves once. */
    STEP 8
    mov dword ptr [base], 0
    mov dword ptr [size], 0x3000
    push PAGE_READWRITE
    push MEM_RESERVE
    PUSHI size
    push 0
    PUSHI base
    call dword ptr [NtAllocateVirtualMemory]
    test eax, eax
    jnz fail
    mov eax, dword ptr [base]
    mov dword ptr [mbase], eax
    mov dword ptr [size], 0x1000
    push PAGE_READWRITE
    push MEM_COMMIT
    PUSHI size
    push 0
    PUSHI base
    call dword ptr [NtAllocateVirtualMemory]
    test eax, eax
    jnz fail
    mov eax, dword ptr [mbase]
    add eax, 0x2000
    mov dword ptr [base], eax
    mov dword ptr [size], 0x1000
    push PAGE_READWRITE
    push MEM_COMMIT
    PUSHI size
    push 0
    PUSHI base
    call dword ptr [NtAllocateVirtualMemory]
    test eax, eax
    jnz fail
    mov edi, dword ptr [mbase]
    xor ecx, ecx
1:
    mov byte ptr [edi + ecx + 0x800], cl    /* byte 0x800 + i: i & 0xff */
    inc ecx
    cmp ecx, 0x800
    jb 1b
    STEP 9
    PUSHI seh_commit
    push dword ptr fs:[0]
    mov dword ptr fs:[0], esp
    push 0x2000
    mov eax, dword ptr [mbase]
    lea ecx, [eax + 0x808]
    push ecx
    lea ecx, [eax + 0x800]
    push ecx
    call dword ptr [RtlMoveMemory]
    pop eax
    mov dword ptr fs:[0], eax
    add esp, 4
    STEP 10
    cmp dword ptr [seh_count], 2
    jne fail
    mov edi, dword ptr [mbase]
    cmp byte ptr [edi + 0x800], 8           /* each byte moved once: from 8 on */
    jne fail
    cmp byte ptr [edi + 0x900], 8           /* 0x108 & 0xff */
    jne fail
    cmp byte ptr [edi + 0xff7], 0xff        /* 0x7ff, the last of the pattern */
    jne fail
    cmp byte ptr [edi + 0xff8], 0           /* from page 1: zero */
    jne fail

    PUSHI passed
    call dword ptr [DbgPrint]
    add esp, 4
    hlt
fail:
    push dword ptr [step]
    PUSHI failed
    call dword ptr [DbgPrint]
    add esp, 8
    hlt

/* The SEH handler (cdecl: record, frame, context, dispatcher context):
 * an access violation commits the page and continues execution. */
seh_commit:
    mov eax, dword ptr [esp + 4]
    cmp dword ptr [eax], 0xC0000005
    jne 1f
    inc dword ptr [seh_count]
    mov ecx, dword ptr [eax + 0x14]     /* ExceptionInformation[0]: 1 for a write */
    mov dword ptr [seh_write], ecx
    mov ecx, dword ptr [eax + 0x18]     /* ExceptionInformation[1]: the address */
    and ecx, 0xFFFFF000
    mov dword ptr [cbase], ecx
    mov dword ptr [csize], 0x1000
    push PAGE_READWRITE
    push MEM_COMMIT
    PUSHI csize
    push 0
    PUSHI cbase
    call dword ptr [NtAllocateVirtualMemory]
    xor eax, eax                        /* ExceptionContinueExecution */
    ret
1:
    mov eax, 1                          /* ExceptionContinueSearch */
    ret

.align 4
seh_count: .long 0
seh_write: .long 0
cs_va: .long 0
mbase: .long 0
cbase: .long 0
csize: .long 0
step: .long 0
round: .long 0
base: .long 0
size: .long 0
passed: .asciz "kmm: all passed"
failed: .asciz "kmm: failed at step %d"
