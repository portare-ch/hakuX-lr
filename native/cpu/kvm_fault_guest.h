/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* KVM guest fault handlers assembled with GNU as (--32, Intel syntax). */
#ifndef NATIVE_CPU_KVM_FAULT_GUEST_H
#define NATIVE_CPU_KVM_FAULT_GUEST_H

#include <stdint.h>

/* Ring-0 #PF entry at 0x7400. Save CR2 at 0x7800 and jump to the vector 14
 * stub at 0x40E0 with the original stack. SS provides a flat ring-0 segment
 * even if the title has changed DS.
 *
 *  push eax
 *  mov eax, cr2
 *  mov ss:[0x7800], eax
 *  pop eax
 *  jmp STUB14
 */
static const uint8_t fault_guest_pf[16] = {
    0x50, 0x0f, 0x20, 0xd0, 0x36, 0xa3, 0x00, 0x78, 0x00, 0x00, 0x58, 0xe9, 0xd0, 0xcc, 0xff, 0xff,
};

/* Ring-0 NMI handler at 0x7440. The list at 0x7804 holds a count followed
 * by linear addresses. Invalidate each page and clear the count before
 * resuming the title.
 *
 *  push eax
 *  push ecx
 *  mov ecx, ss:[0x7804]
 *  jecxz done
 * next:
 *  mov eax, ss:[ecx*4 + 0x7804]
 *  invlpg ss:[eax]
 *  dec ecx
 *  jnz next
 *  mov ss:[0x7804], ecx
 * done:
 *  pop ecx
 *  pop eax
 *  iret
 */
static const uint8_t fault_guest_nmi[36] = {
    0x50, 0x51, 0x36, 0x8b, 0x0d, 0x04, 0x78, 0x00, 0x00, 0xe3, 0x16, 0x36, 0x8b, 0x04, 0x8d, 0x04, 0x78, 0x00,
    0x00, 0x36, 0x0f, 0x01, 0x38, 0x49, 0x75, 0xf1, 0x36, 0x89, 0x0d, 0x04, 0x78, 0x00, 0x00, 0x59, 0x58, 0xcf,
};

#endif
