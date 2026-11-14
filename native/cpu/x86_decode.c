/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "native/cpu/x86_decode.h"

#include <string.h>

/* what follows the opcode */
#define M   0x01u /* ModRM */
#define I8  0x02u /* imm8 */
#define IZ  0x04u /* imm16/32 by operand size */
#define I16 0x08u /* imm16 */
#define J8  0x10u /* rel8 */
#define JZ  0x20u /* rel16/32 */
#define PFX 0x40u /* a prefix */
#define BAD 0x80u /* not an instruction */

static const uint8_t g_map1[256] = {
    /* 00 */ M, M, M, M, I8, IZ, 0, 0, M, M, M, M, I8, IZ, 0, 0,
    /* 10 */ M, M, M, M, I8, IZ, 0, 0, M, M, M, M, I8, IZ, 0, 0,
    /* 20 */ M, M, M, M, I8, IZ, PFX, 0, M, M, M, M, I8, IZ, PFX, 0,
    /* 30 */ M, M, M, M, I8, IZ, PFX, 0, M, M, M, M, I8, IZ, PFX, 0,
    /* 40 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 50 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 60 */ 0, 0, M, M, PFX, PFX, PFX, PFX, IZ, M | IZ, I8, M | I8, 0, 0, 0, 0,
    /* 70 */ J8, J8, J8, J8, J8, J8, J8, J8, J8, J8, J8, J8, J8, J8, J8, J8,
    /* 80 */ M | I8, M | IZ, M | I8, M | I8, M, M, M, M, M, M, M, M, M, M, M, M,
    /* 90 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, IZ | I16, 0, 0, 0, 0, 0,
    /* A0 */ 0, 0, 0, 0, 0, 0, 0, 0, I8, IZ, 0, 0, 0, 0, 0, 0, /* A0-A3: an address, below */
    /* B0 */ I8, I8, I8, I8, I8, I8, I8, I8, IZ, IZ, IZ, IZ, IZ, IZ, IZ, IZ,
    /* C0 */ M | I8, M | I8, I16, 0, M, M, M | I8, M | IZ, I16 | I8, 0, I16, 0, 0, I8, 0, 0,
    /* D0 */ M, M, M, M, I8, I8, 0, 0, M, M, M, M, M, M, M, M,
    /* E0 */ J8, J8, J8, J8, I8, I8, I8, I8, JZ, JZ, IZ | I16, J8, 0, 0, 0, 0,
    /* F0 */ PFX, 0, PFX, PFX, 0, 0, M, M, 0, 0, 0, 0, 0, 0, M, M, /* F6/F7 /0,/1: an immediate, below */
};

/* 0F xx, through SSE and 3DNow! (a title's libraries may carry paths for
 * other CPUs); SSE2's share the first SSE's opcodes */
static const uint8_t g_map2[256] = {
    /* 00 */ M, M, M, M, BAD, BAD, 0, BAD, 0, 0, BAD, 0, BAD, M, 0, M | I8,
    /* 10 */ M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M,
    /* 20 */ M, M, M, M, BAD, BAD, BAD, BAD, M, M, M, M, M, M, M, M,
    /* 30 */ 0, 0, 0, 0, 0, 0, BAD, BAD, BAD, BAD, BAD, BAD, BAD, BAD, BAD, BAD,
    /* 40 */ M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M,
    /* 50 */ M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M,
    /* 60 */ M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M,
    /* 70 */ M | I8, M | I8, M | I8, M | I8, M, M, M, 0, BAD, BAD, BAD, BAD, BAD, BAD, M, M,
    /* 80 */ JZ, JZ, JZ, JZ, JZ, JZ, JZ, JZ, JZ, JZ, JZ, JZ, JZ, JZ, JZ, JZ,
    /* 90 */ M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M,
    /* A0 */ 0, 0, 0, M, M | I8, M, BAD, BAD, 0, 0, 0, M, M | I8, M, M, M,
    /* B0 */ M, M, M, M, M, M, M, M, BAD, BAD, M | I8, M, M, M, M, M,
    /* C0 */ M, M, M | I8, M, M | I8, M | I8, M | I8, M, 0, 0, 0, 0, 0, 0, 0, 0,
    /* D0 */ BAD, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M,
    /* E0 */ M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M,
    /* F0 */ BAD, M, M, M, M, M, M, M, M, M, M, M, M, M, M, BAD,
};

static uint32_t rd(const uint8_t *p, unsigned bytes)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < bytes; i++) {
        v |= (uint32_t)p[i] << (8 * i);
    }
    return v;
}

/* The x87 encodings that are instructions: by ModRM - C0 for a register
 * form, by /reg for a memory form (D8..DF). */
static const uint64_t g_x87_reg[8] = {
    0xFFFFFFFFFFFFFFFFu, 0xFFFF7F33FF01FFFFu, 0x00000200FFFFFFFFu, 0x00FFFF1FFFFFFFFFu,
    0xFFFFFFFF0000FFFFu, 0x0000FFFFFFFF00FFu, 0xFFFFFFFF0200FFFFu, 0x00FFFF01000000FFu,
};
static const uint8_t g_x87_mem[8] = {0xFF, 0xFD, 0xFF, 0xAF, 0xFF, 0xDF, 0xFF, 0xFF};

/* 3DNow!'s operations, the byte after the operands */
static const uint8_t g_3dnow[] = {0x0C, 0x0D, 0x1C, 0x1D, 0x8A, 0x8E, 0x90, 0x94, 0x96, 0x97, 0x9A, 0x9E,
                                  0xA0, 0xA4, 0xA6, 0xA7, 0xAA, 0xAE, 0xB0, 0xB4, 0xB6, 0xB7, 0xBB, 0xBF};

/* An SSE opcode's operands are packed, or scalar after F3 (F2: SSE2's
 * doubles); before the opcodes with no scalar form F3 and F2 make no
 * instruction. */
static bool sse_opcode(unsigned op)
{
    return (op >= 0x10 && op <= 0x17) || (op >= 0x28 && op <= 0x2F) || (op >= 0x50 && op <= 0x7F && op != 0x77) ||
           (op >= 0xC2 && op <= 0xC6) || (op >= 0xD1 && op <= 0xFE);
}

static bool sse_scalar(unsigned op, bool f3)
{
    switch (op) {
    case 0x10: case 0x11: case 0x12: case 0x2A: case 0x2C: case 0x2D: case 0x51: case 0x58: case 0x59:
    case 0x5A: case 0x5C: case 0x5D: case 0x5E: case 0x5F: case 0x70: case 0xC2: case 0xD6: case 0xE6:
        return true;
    case 0x16: case 0x52: case 0x53: case 0x5B: case 0x6F: case 0x7E: case 0x7F:
        return f3;
    }
    return false;
}

/* LOCK is an invalid opcode but before these, storing to memory. */
static bool lockable(bool two, unsigned op, unsigned reg)
{
    if (two) {
        return op == 0xAB || op == 0xB3 || op == 0xBB || op == 0xB0 || op == 0xB1 || op == 0xC0 || op == 0xC1 ||
               (op == 0xBA && reg >= 5) || (op == 0xC7 && reg == 1);
    }
    if (op < 0x38) {
        return (op & 7) < 2; /* add, or, adc, sbb, and, sub, xor to r/m */
    }
    switch (op) {
    case 0x80: case 0x81: case 0x82: case 0x83:
        return reg != 7;
    case 0x86: case 0x87:
        return true;
    case 0xF6: case 0xF7:
        return reg == 2 || reg == 3;
    case 0xFE: case 0xFF:
        return reg < 2;
    }
    return false;
}

bool nc_x86_decode(const uint8_t *code, unsigned size, uint32_t va, NcX86Insn *out)
{
    memset(out, 0, sizeof(*out));
    if (size > 15) {
        size = 15;
    }
    unsigned n = 0;
    bool op16 = false, addr16 = false, lock = false, f3 = false, f2 = false;
    uint8_t fl = PFX;
    unsigned op = 0;
    while (n < size) {
        op = code[n++];
        fl = g_map1[op];
        if (!(fl & PFX)) {
            break;
        }
        op16 |= op == 0x66;
        addr16 |= op == 0x67;
        lock |= op == 0xF0;
        f3 |= op == 0xF3;
        f2 |= op == 0xF2;
    }
    if (fl & PFX) {
        return false;
    }
    const bool two = op == 0x0F;
    if (two) {
        if (n == size) {
            return false;
        }
        op = code[n++];
        fl = g_map2[op];
    }
    if (fl & BAD) {
        return false;
    }
    unsigned mod = 3, reg = 0, rm = 0;
    bool table = false;
    if (fl & M) {
        if (n == size) {
            return false;
        }
        const uint8_t m = code[n++];
        mod = m >> 6;
        reg = (m >> 3) & 7;
        rm = m & 7;
        if (two && (op & 0xFC) == 0x20) {
            mod = 3; /* mov to/from CRn, DRn: registers whatever mod says */
        }
        if (mod != 3) {
            unsigned disp;
            out->mem = true;
            if (addr16) {
                out->has_abs = mod == 0 && rm == 6;
                disp = out->has_abs || mod == 2 ? 2 : mod;
            } else {
                unsigned base = rm, index = 4, scale = 0;
                if (rm == 4) {
                    if (n == size) {
                        return false;
                    }
                    const uint8_t sib = code[n++];
                    base = sib & 7;
                    index = (sib >> 3) & 7;
                    scale = sib >> 6;
                }
                if (mod == 0 && base == 5) {
                    disp = 4;
                    out->has_abs = index == 4;
                    table = index != 4 && scale == 2;
                    out->index = (uint8_t)index;
                } else {
                    disp = mod == 2 ? 4 : mod;
                }
            }
            if (size - n < disp) {
                return false;
            }
            if (out->has_abs || table) {
                out->abs = rd(code + n, disp);
            }
            n += disp;
        }
    }
    const bool memory = mod != 3;
    if (lock && !(memory && lockable(two, op, reg))) {
        return false;
    }

    /* the encodings that are no instruction, the odd ones and the flow */
    if (!two) {
        switch (op) {
        case 0x06: case 0x07: case 0x0E: case 0x16: case 0x17: case 0x1E: case 0x1F: /* push/pop of a segment */
        case 0x27: case 0x2F: case 0x37: case 0x3F: /* daa, das, aaa, aas */
        case 0x63:                                  /* arpl */
        case 0xCE: case 0xD4: case 0xD5: case 0xD6: case 0xD7: /* into, aam, aad, salc, xlat */
        case 0xF1:                                  /* int1 */
            out->odd = true;
            break;
        case 0x62: case 0xC4: case 0xC5: /* bound, les, lds */
            if (!memory) {
                return false;
            }
            out->odd = true;
            break;
        case 0x8C:
            if (reg > 5) {
                return false;
            }
            break;
        case 0x8D:
            if (!memory) {
                return false;
            }
            break;
        case 0x8E:
            if (reg > 5 || reg == 1) {
                return false;
            }
            break;
        case 0x8F: case 0xC6: case 0xC7:
            if (reg) {
                return false;
            }
            break;
        case 0x9A: case 0xEA: /* far call, jmp: the code walk goes on past either */
            out->odd = true;
            break;
        case 0xA0: case 0xA1: case 0xA2: case 0xA3: {
            const unsigned bytes = addr16 ? 2 : 4;
            if (size - n < bytes) {
                return false;
            }
            out->mem = out->has_abs = true;
            out->abs = rd(code + n, bytes);
            n += bytes;
            break;
        }
        case 0xC2: case 0xC3: case 0xCF:
            out->flow = NC_X86_END;
            break;
        case 0xCA: case 0xCB: /* retf */
            out->flow = NC_X86_END;
            out->odd = true;
            break;
        case 0xCC:
            out->flow = NC_X86_END;
            break;
        case 0xCD:
            out->odd = n < size && code[n] != 3;
            break;
        case 0xE8:
            out->flow = NC_X86_CALL;
            break;
        case 0xE9: case 0xEB:
            out->flow = NC_X86_JMP;
            break;
        case 0xF4:
            out->flow = NC_X86_END;
            out->odd = true;
            break;
        case 0xF6: case 0xF7:
            if (reg < 2) {
                fl |= op == 0xF6 ? I8 : IZ;
            }
            break;
        case 0xFE:
            if (reg > 1) {
                return false;
            }
            break;
        case 0xFF:
            if (reg == 7 || ((reg == 3 || reg == 5) && !memory)) {
                return false;
            }
            if (reg == 4 || reg == 5) {
                out->flow = table ? NC_X86_JMP_TABLE : NC_X86_JMP_OTHER;
            }
            break;
        default:
            if ((op & 0xF0) == 0x70 || op == 0xE3) { /* not loop: compilers don't emit it */
                out->flow = NC_X86_JCC;
            } else if ((op & 0xF8) == 0xD8 &&
                       !(memory ? g_x87_mem[op & 7] >> reg & 1 : g_x87_reg[op & 7] >> (reg << 3 | rm) & 1)) {
                return false;
            }
        }
    } else {
        if (sse_opcode(op) && (((f3 || f2) && !sse_scalar(op, f3)) || (op16 && (op == 0x52 || op == 0x53)) ||
                               (!op16 && (op == 0x6C || op == 0x6D)) || (!op16 && !f3 && !f2 && (op == 0xD6 || op == 0xE6)))) {
            return false;
        }
        switch (op) {
        case 0x00:
            if (reg > 5) {
                return false;
            }
            out->odd = reg == 2 || reg == 3; /* lldt, ltr */
            break;
        case 0x01:
            if (memory ? reg == 5 : reg != 4 && reg != 6) {
                return false;
            }
            out->odd = memory && (reg == 2 || reg == 3); /* lgdt, lidt */
            break;
        case 0x08: /* invd */
            out->odd = true;
            break;
        case 0x0B: /* ud2 */
            out->flow = NC_X86_END;
            break;
        case 0x0F:
            if (n >= size || !memchr(g_3dnow, code[n], sizeof(g_3dnow))) {
                return false;
            }
            break;
        case 0x18:
            if (!memory && reg < 4) {
                return false;
            }
            break;
        case 0x0D: case 0x13: case 0x17: case 0x2B: case 0xB2: case 0xB4: case 0xB5: case 0xC3: case 0xE7:
            if (!memory) { /* prefetch, movlps/movhps/movntps/movnti/movntq stores, lss/lfs/lgs */
                return false;
            }
            break;
        case 0x50: case 0xC5: case 0xD7: case 0xF7: /* movmskps, pextrw, pmovmskb, maskmovq */
            if (memory) {
                return false;
            }
            break;
        case 0x52:
            out->rsqrt = f3 ? 2 : 1;
            break;
        case 0x71: case 0x72: case 0x73: /* shifts of an MMX register by an immediate */
            if (memory || !(reg == 2 || reg == 6 || (reg == 4 && op != 0x73) || ((reg & 3) == 3 && op == 0x73))) {
                return false;
            }
            break;
        case 0xAE:
            if (memory ? reg > 3 && reg != 7 : reg < 5) {
                return false; /* fxsave, fxrstor, ldmxcsr, stmxcsr, clflush; the fences */
            }
            break;
        case 0xBA:
            if (reg < 4) {
                return false;
            }
            break;
        case 0xC7:
            if (reg != 1 || !memory) {
                return false; /* cmpxchg8b */
            }
            break;
        default:
            if ((op & 0xF0) == 0x80) {
                out->flow = NC_X86_JCC;
            }
        }
    }

    const unsigned z = op16 ? 2 : 4;
    uint32_t iz = 0, i8 = 0;
    if (fl & IZ) {
        if (size - n < z) {
            return false;
        }
        iz = rd(code + n, z);
        n += z;
        if (!op16) {
            out->imm[out->nimm++] = iz;
        }
    }
    if (fl & I16) {
        n += 2;
    }
    if (fl & I8) {
        if (n < size) {
            i8 = (uint32_t)(int32_t)(int8_t)code[n];
        }
        n += 1;
    }
    if (fl & (J8 | JZ)) {
        const unsigned bytes = fl & J8 ? 1 : z;
        if (size - n < bytes) {
            return false;
        }
        const uint32_t rel = rd(code + n, bytes);
        n += bytes;
        const uint32_t target = va + n + (bytes == 1 ? (uint32_t)(int32_t)(int8_t)rel :
                                    bytes == 2 ? (uint32_t)(int32_t)(int16_t)rel : rel);
        out->imm[out->nimm++] = target;
    }
    if (n > size) {
        return false;
    }
    if (!two && !op16 && !memory) {
        if (op == 0x3D) {
            out->cmp_reg = 1;
            out->cmp_imm = iz;
        } else if ((op == 0x81 || op == 0x83) && reg == 7 && (fl & M)) {
            out->cmp_reg = (uint8_t)(rm + 1);
            out->cmp_imm = op == 0x81 ? iz : i8;
        }
    }
    out->len = (uint8_t)n;
    return true;
}
