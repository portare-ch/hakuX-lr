/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Guest EEPROM test for kernel_smbus.py.
 * MODE 0: read/write; 1: read frontend settings; 2: unchanged write;
 * 3: encrypted-section write followed by an unrelated setting save.
 * Code starts at 0x11000; import thunks at 0x10f00. */
.intel_syntax noprefix
.set THUNK, 0x10f00
.set DbgPrint,                  THUNK + 4*0
.set ExQueryNonVolatileSetting, THUNK + 4*1
.set HalReadSMBusValue,         THUNK + 4*2
.set HalWriteSMBusValue,        THUNK + 4*3
.set ExSaveNonVolatileSetting,  THUNK + 4*4

.set EEPROM,           0xA8
.set XC_ETHERNET_ADDR, 0x101

/* push imm32 (LLVM's Intel syntax makes "push offset sym" a 16-bit push) */
.macro PUSHI sym
    .byte 0x68
    .long \sym
.endm

.macro STEP n
    mov dword ptr [step], \n
.endm

/* HalReadSMBusValue(EEPROM, offset, word, &value), value preset to all ones */
.macro READ offset, word
    mov dword ptr [value], 0xFFFFFFFF
    PUSHI value
    push \word
    push \offset
    push EEPROM
    call dword ptr [HalReadSMBusValue]
    test eax, eax
    jnz fail
.endm

/* HalWriteSMBusValue(EEPROM, offset, word, v) */
.macro WRITE offset, word, v
    push \v
    push \word
    push \offset
    push EEPROM
    call dword ptr [HalWriteSMBusValue]
    test eax, eax
    jnz fail
.endm

.text
.globl _start
_start:
.if MODE == 1
    READ 0x90, 1                        /* Language */
    mov eax, dword ptr [value]
    mov dword ptr [lang], eax
    READ 0x94, 1                        /* Video flags */
    mov eax, dword ptr [value]
    mov dword ptr [video], eax
    READ 0x96, 1
    mov eax, dword ptr [value]
    shl eax, 16
    or dword ptr [video], eax
    READ 0x64, 1                        /* Time zone bias */
    mov eax, dword ptr [value]
    mov dword ptr [bias], eax
    READ 0x66, 1
    mov eax, dword ptr [value]
    shl eax, 16
    or dword ptr [bias], eax
    push dword ptr [bias]
    push dword ptr [video]
    push dword ptr [lang]
    PUSHI shown
    call dword ptr [DbgPrint]
    add esp, 16
    jmp done
.endif
.if MODE == 2
    STEP 1
    READ 0x90, 0
    push dword ptr [value]              /* Write back the language byte. */
    push 0
    push 0x90
    push EEPROM
    call dword ptr [HalWriteSMBusValue]
    test eax, eax
    jnz fail
    jmp done
.endif
.if MODE == 3
    STEP 1
    WRITE 0x1C, 0, 0xA5                 /* Byte in the encrypted HDD key */
    STEP 2
    mov dword ptr [lang], 5             /* XC_LANGUAGE (7), REG_DWORD (4): Spanish */
    push 4
    PUSHI lang
    push 4
    push 7
    call dword ptr [ExSaveNonVolatileSetting]
    test eax, eax
    jnz fail
    jmp done
.endif
    /* 1. Query the MAC address through the kernel API. */
    STEP 1
    PUSHI len
    push 6
    PUSHI mac
    PUSHI type
    push XC_ETHERNET_ADDR
    call dword ptr [ExQueryNonVolatileSetting]
    test eax, eax
    jnz fail
    cmp byte ptr [mac + 1], 0x50        /* Xbox MAC prefix (00:50:F2) */
    jne fail

    /* 2. Read the same MAC address one byte at a time over SMBus. */
    STEP 2
    xor esi, esi
mac_byte:
    mov dword ptr [value], 0xFFFFFFFF
    PUSHI value
    push 0
    lea eax, [esi + 0x40]
    push eax
    push EEPROM
    call dword ptr [HalReadSMBusValue]
    test eax, eax
    jnz fail
    movzx ecx, byte ptr [mac + esi]
    cmp dword ptr [value], ecx
    jne fail
    inc esi
    cmp esi, 6
    jb mac_byte

    /* 3. Read a little-endian word. */
    STEP 3
    READ 0x40, 1
    movzx ecx, word ptr [mac]
    cmp dword ptr [value], ecx
    jne fail

    /* 4. Write and read back a byte. */
    STEP 4
    WRITE 0xF0, 0, 0x5A
    READ 0xF0, 0
    cmp dword ptr [value], 0x5A
    jne fail

    /* 5. Write and read back a word. */
    STEP 5
    WRITE 0xF2, 1, 0x1234
    READ 0xF2, 1
    cmp dword ptr [value], 0x1234
    jne fail
    READ 0xF3, 0
    cmp dword ptr [value], 0x12
    jne fail

done:
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

.align 4
step: .long 0
value: .long 0
type: .long 0
len: .long 0
mac: .space 8
lang: .long 0
video: .long 0
bias: .long 0
passed: .asciz "ksmbus: all passed"
failed: .asciz "ksmbus: failed at step %d"
shown: .asciz "ksmbus: language %u, video %08x, bias %d"
