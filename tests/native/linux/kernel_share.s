/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Share access of hard disk files (kernel_share.py): an open that asks
 * for what another doesn't share, or doesn't share what another holds, is
 * refused; an open for attributes takes no part; and the opens of a file
 * follow it through a rename.
 * Code: 0x11000; thunks: 0x10f00, in IMPORTS order. Print and halt. */
.intel_syntax noprefix
.set THUNK, 0x10f00
.set DbgPrint,             THUNK + 4*0
.set NtCreateFile,         THUNK + 4*1
.set NtClose,              THUNK + 4*2
.set NtSetInformationFile, THUNK + 4*3

.set READ,        0x00100001    /* FILE_READ_DATA | SYNCHRONIZE */
.set WRITE,       0x00100002    /* FILE_WRITE_DATA | SYNCHRONIZE */
.set READ_WRITE,  0x00100003
.set READ_DELETE, 0x00110001    /* FILE_READ_DATA | DELETE | SYNCHRONIZE */
.set ATTRIBUTES,  0x00100080    /* FILE_READ_ATTRIBUTES | SYNCHRONIZE */
.set OPEN_IF,     3
.set SHARING_VIOLATION, 0xC0000043

/* push imm32 (LLVM's Intel syntax makes "push offset sym" a 16-bit push) */
.macro PUSHI sym
    .byte 0x68
    .long \sym
.endm

.macro STEP n
    mov dword ptr [step], \n
.endm

/* NtCreateFile(&handle, access, attrs, &iosb, 0, NORMAL, share, FILE_OPEN_IF,
 *   SYNCHRONOUS_IO_NONALERT | NON_DIRECTORY_FILE) -> eax */
.macro OPEN handle, attrs, access, share
    push 0x60
    push OPEN_IF
    push \share
    push 0x80
    push 0
    PUSHI iosb
    PUSHI \attrs
    .byte 0x68
    .long \access
    PUSHI \handle
    call dword ptr [NtCreateFile]
.endm

.macro CLOSE handle
    push dword ptr [\handle]
    call dword ptr [NtClose]
.endm

.text
.globl _start
_start:
    /* 1. Held for writing without sharing: a reader is refused. */
    STEP 1
    OPEN ha, tmp_attrs, READ_WRITE, 0
    test eax, eax
    jnz fail
    STEP 2
    OPEN hb, tmp_attrs, READ, 7
    cmp eax, SHARING_VIOLATION
    jne fail
    /* 2. An open for attributes takes no part. */
    STEP 3
    OPEN hb, tmp_attrs, ATTRIBUTES, 0
    test eax, eax
    jnz fail
    CLOSE hb
    CLOSE ha

    /* 3. Two readers, one of them sharing no writing: a writer is refused. */
    STEP 4
    OPEN ha, tmp_attrs, READ_DELETE, 7
    test eax, eax
    jnz fail
    STEP 5
    OPEN hb, tmp_attrs, READ, 5
    test eax, eax
    jnz fail
    STEP 6
    OPEN hc, tmp_attrs, WRITE, 7
    cmp eax, SHARING_VIOLATION
    jne fail

    /* 4. Renamed through the first handle: the second is still an open of
     * that file, under its new name, and not of a new file of the old one. */
    STEP 7
    push 10                             /* FileRenameInformation */
    push 16
    PUSHI rename
    PUSHI iosb
    push dword ptr [ha]
    call dword ptr [NtSetInformationFile]
    test eax, eax
    jnz fail
    STEP 8
    OPEN hc, dat_attrs, WRITE, 7
    cmp eax, SHARING_VIOLATION
    jne fail
    STEP 9
    OPEN hc, tmp_attrs, WRITE, 7
    test eax, eax
    jnz fail

    PUSHI passed
    call dword ptr [DbgPrint]
    add esp, 4
    hlt
fail:
    push eax
    push dword ptr [step]
    PUSHI failed
    call dword ptr [DbgPrint]
    add esp, 12
    hlt

.align 4
step: .long 0
ha: .long 0
hb: .long 0
hc: .long 0
iosb: .long 0, 0
tmp_attrs: .long 0, tmp_name, 0x40      /* OBJ_CASE_INSENSITIVE */
dat_attrs: .long 0, dat_name, 0x40
tmp_name: .short tmp_end - tmp_str - 1, tmp_end - tmp_str
    .long tmp_str
dat_name: .short dat_end - dat_str - 1, dat_end - dat_str
    .long dat_str
rename: .long 1, 0                      /* ReplaceIfExists, RootDirectory, FileName */
    .short dat_end - dat_str - 1, dat_end - dat_str
    .long dat_str
tmp_str: .asciz "\\Device\\Harddisk0\\Partition1\\share.tmp"
tmp_end:
dat_str: .asciz "\\Device\\Harddisk0\\Partition1\\share.dat"
dat_end:
passed: .asciz "kshare: all passed"
failed: .asciz "kshare: failed at step %d (%08x)"
