/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NATIVE_CPU_X86_DECODE_H
#define NATIVE_CPU_X86_DECODE_H

#include <stdbool.h>
#include <stdint.h>

/* What an instruction does to control flow, as far as a static walk of a
 * title's code follows it. */
enum NcX86Flow {
    NC_X86_NEXT,      /* the next instruction (calls included) */
    NC_X86_END,       /* ret, iret, int3, hlt, ud2 */
    NC_X86_JMP,       /* jmp rel: imm[0] only */
    NC_X86_JCC,       /* jcc, jecxz rel: imm[0] and the next instruction */
    NC_X86_CALL,      /* call rel: imm[0] is the callee */
    NC_X86_JMP_TABLE, /* jmp [index*4 + abs] */
    NC_X86_JMP_OTHER, /* any other indirect jmp */
};

typedef struct NcX86Insn {
    uint8_t len;
    uint8_t flow;      /* an NcX86Flow */
    bool odd;          /* not in compiled title code: segment loads, BCD, far transfers... */
    bool mem;          /* a ModRM memory operand */
    bool has_abs;      /* a memory operand that is an address alone (abs), no register */
    uint8_t rsqrt;     /* 1 rsqrtps, 2 rsqrtss */
    uint8_t index;     /* NC_X86_JMP_TABLE: the index register (abs is the table) */
    uint8_t cmp_reg;   /* cmp r32, imm: the register + 1, and cmp_imm */
    uint8_t nimm;
    uint32_t abs;
    uint32_t cmp_imm;
    uint32_t imm[2];   /* 32-bit immediates, a relative branch's target, a far pointer's offset */
} NcX86Insn;

/* One 32-bit protected mode instruction of a Pentium III's set (the Xbox's
 * CPU) at code[0..size), which runs at va: its length and the operands a
 * code walk needs, no more. false: not an instruction, or cut short. */
bool nc_x86_decode(const uint8_t *code, unsigned size, uint32_t va, NcX86Insn *out);

#endif
