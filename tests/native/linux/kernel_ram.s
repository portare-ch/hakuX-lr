/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Console memory test (kernel_ram.py: 64, 128 and 256 MiB, and a title
 * limited to 64 MiB). Reports what the title has and uses it: the totals,
 * the NV2A's memory size, a contiguous block and its view in the GPU's
 * window, and nearly all of the free memory committed and written.
 * Code: 0x11000; thunks: 0x10f00, in IMPORTS order. Print and halt. */
.intel_syntax noprefix
.set THUNK, 0x10f00
.set DbgPrint,                   THUNK + 4*0
.set MmAllocateContiguousMemory, THUNK + 4*1
.set MmQueryStatistics,          THUNK + 4*2
.set NtAllocateVirtualMemory,    THUNK + 4*3

.set NV_PFB_CSTATUS, 0xFD10020C
.set GPU_WINDOW,     0x70000000     /* 0xF0000000 - 0x80000000 */
.set MEM_COMMIT,     0x1000
.set PAGE_READWRITE, 0x04
.set SPARE_PAGES,    1024           /* left free: page tables and the like */

/* push imm32 (LLVM's Intel syntax makes "push offset sym" a 16-bit push) */
.macro PUSHI sym
    .byte 0x68
    .long \sym
.endm

.macro STEP n
    mov dword ptr [step], \n
.endm

.text
.globl _start
_start:
    /* 1. The totals. */
    STEP 1
    mov dword ptr [stats], 0x24
    PUSHI stats
    call dword ptr [MmQueryStatistics]
    test eax, eax
    jnz fail

    /* 2. A contiguous block, the same bytes through the GPU's window. */
    STEP 2
    push 0x100000
    call dword ptr [MmAllocateContiguousMemory]
    test eax, eax
    jz fail
    mov dword ptr [block], eax
    mov dword ptr [eax + 0xFFFFC], 0x4D415247
    cmp dword ptr [eax + GPU_WINDOW + 0xFFFFC], 0x4D415247
    jne fail
    mov dword ptr [eax + GPU_WINDOW], 0x5A5A1234
    cmp dword ptr [eax], 0x5A5A1234
    jne fail

    /* 3. All the free memory but a spare, committed. */
    STEP 3
    mov dword ptr [stats], 0x24
    PUSHI stats
    call dword ptr [MmQueryStatistics]
    mov eax, dword ptr [stats + 8]          /* AvailablePages */
    sub eax, SPARE_PAGES
    jbe fail
    mov dword ptr [pages], eax
    shl eax, 12
    mov dword ptr [size], eax
    mov dword ptr [base], 0
    push PAGE_READWRITE
    push MEM_COMMIT
    PUSHI size
    push 0
    PUSHI base
    call dword ptr [NtAllocateVirtualMemory]
    test eax, eax
    jnz fail

    /* 4. Every page written, then read back. */
    STEP 4
    mov edi, dword ptr [base]
    xor ecx, ecx
write:
    mov eax, ecx
    xor eax, 0xC0FFEE00
    mov dword ptr [edi], eax
    add edi, 0x1000
    inc ecx
    cmp ecx, dword ptr [pages]
    jb write
    STEP 5
    mov edi, dword ptr [base]
    xor ecx, ecx
read:
    mov eax, ecx
    xor eax, 0xC0FFEE00
    cmp dword ptr [edi], eax
    jne fail
    add edi, 0x1000
    inc ecx
    cmp ecx, dword ptr [pages]
    jb read

    /* total pages, the NV2A's memory, the block, pages committed */
    push dword ptr [pages]
    push dword ptr [block]
    mov eax, NV_PFB_CSTATUS
    push dword ptr [eax]
    push dword ptr [stats + 4]
    PUSHI passed
    call dword ptr [DbgPrint]
    add esp, 20
    hlt
fail:
    push dword ptr [step]
    PUSHI failed
    call dword ptr [DbgPrint]
    add esp, 8
    hlt

.align 4
step: .long 0
block: .long 0
base: .long 0
size: .long 0
pages: .long 0
stats: .fill 9, 4, 0
passed: .asciz "kram: total %u pages, gpu %08x, block %08x, committed %u pages"
failed: .asciz "kram: failed at step %d"
