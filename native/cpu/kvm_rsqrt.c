/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "native/cpu/kvm_rsqrt.h"

#include <cpuid.h>
#include <emmintrin.h>
#include <stdlib.h>
#include <string.h>

#include "native/cpu/intel_rcp_table.inc"
#include "native/cpu/intel_rsqrt_table.inc"

bool nc_rsqrt_decode(const uint8_t *code, unsigned size, const uint32_t regs[8],
                     uint32_t fs_base, NcRsqrtInsn *out)
{
    unsigned n = 0;
    bool scalar = false, addr16 = false, fs = false;
    if (size > 15) {
        size = 15;
    }
    while (n < size) {
        const uint8_t b = code[n];
        if (b == 0xF3) {
            scalar = true;
        } else if (b == 0x67) {
            addr16 = true;
        } else if (b == 0x64 || b == 0x65 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26) {
            fs = b == 0x64; /* last segment override wins; the other segments are flat */
        } else {
            break;
        }
        n++;
    }
    if (size - n < 3 || code[n] != 0x0F || (code[n + 1] != 0x52 && code[n + 1] != 0x53)) {
        return false;
    }
    const bool rcp = code[n + 1] == 0x53;
    n += 2;
    const uint8_t m = code[n++];
    const unsigned mod = m >> 6, rm = m & 7;
    *out = (NcRsqrtInsn){.dst = (m >> 3) & 7, .src = rm, .scalar = scalar, .mem = mod != 3, .rcp = rcp};
    uint32_t address = 0;
    unsigned disp = 0;
    if (mod != 3 && addr16) {
        /* BX+SI, BX+DI, BP+SI, BP+DI, SI, DI, BP, BX */
        static const uint8_t base[] = {3, 3, 5, 5, 6, 7, 5, 3};
        address = regs[base[rm]];
        if (rm < 4) {
            address += regs[6 + (rm & 1)];
        }
        if (mod == 0 && rm == 6) {
            address = 0;
            disp = 2;
        } else {
            disp = mod == 1 ? 1 : mod == 2 ? 2 : 0;
        }
    } else if (mod != 3) {
        unsigned base = rm;
        if (rm == 4) {
            if (n == size) {
                return false;
            }
            const uint8_t sib = code[n++];
            const unsigned index = (sib >> 3) & 7;
            if (index != 4) {
                address = regs[index] << (sib >> 6);
            }
            base = sib & 7;
        }
        if (mod == 0 && base == 5) {
            disp = 4;
        } else {
            address += regs[base];
            disp = mod == 1 ? 1 : mod == 2 ? 4 : 0;
        }
    }
    if (disp > size - n) {
        return false;
    }
    if (disp == 1) {
        address += (uint32_t)(int32_t)(int8_t)code[n];
    } else if (disp) {
        uint32_t d = code[n] | (uint32_t)code[n + 1] << 8;
        if (disp == 4) {
            d |= (uint32_t)code[n + 2] << 16 | (uint32_t)code[n + 3] << 24;
        }
        address += d;
    }
    if (addr16) {
        address &= 0xFFFFu;
    }
    out->address = address + (fs ? fs_base : 0);
    out->len = n + disp;
    return true;
}

bool nc_rsqrt_host_amd(void)
{
    unsigned a, b, c, d;
    return __get_cpuid(0, &a, &b, &c, &d) && b == 0x68747541u &&
           d == 0x69746E65u && c == 0x444D4163u; /* AuthenticAMD */
}

uint32_t nc_rsqrt_native(uint32_t bits)
{
    const __m128 x = _mm_castsi128_ps(_mm_cvtsi32_si128((int)bits));
    return (uint32_t)_mm_cvtsi128_si32(_mm_castps_si128(_mm_rsqrt_ss(x)));
}

uint32_t nc_rsqrt_intel(uint32_t bits)
{
    const uint32_t magnitude = bits & 0x7FFFFFFFu;
    if (magnitude > 0x7F800000u) {
        return bits | 0x00400000u; /* quiet NaN, payload and sign retained */
    }
    if (magnitude < 0x00800000u) {
        return (bits & 0x80000000u) | 0x7F800000u; /* signed zero/subnormal */
    }
    if (bits & 0x80000000u) {
        return 0xFFC00000u; /* negative normal or -infinity */
    }
    if (magnitude == 0x7F800000u) {
        return 0;
    }
    const uint32_t exponent = (380u - (bits >> 23)) >> 1;
    return (exponent << 23) | ((uint32_t)intel_rsqrt_mantissa[(bits >> 13) & 2047u] << 11);
}

const uint16_t *nc_rsqrt_intel_mantissa(void)
{
    return intel_rsqrt_mantissa;
}

uint32_t nc_rcp_native(uint32_t bits)
{
    const __m128 x = _mm_castsi128_ps(_mm_cvtsi32_si128((int)bits));
    return (uint32_t)_mm_cvtsi128_si32(_mm_castps_si128(_mm_rcp_ss(x)));
}

uint32_t nc_rcp_intel(uint32_t bits)
{
    const uint32_t sign = bits & 0x80000000u, exponent = (bits >> 23) & 0xFFu;
    if (exponent == 0xFFu) {
        return bits & 0x007FFFFFu ? bits | 0x00400000u : sign; /* quiet NaN; 1/infinity */
    }
    if (!exponent) {
        return sign | 0x7F800000u; /* signed zero/subnormal */
    }
    if (exponent >= 253u) {
        return sign; /* underflow */
    }
    return sign | (253u - exponent) << 23 | (uint32_t)intel_rcp_mantissa[(bits >> 12) & 2047u] << 8;
}

const uint16_t *nc_rcp_intel_mantissa(void)
{
    return intel_rcp_mantissa;
}
