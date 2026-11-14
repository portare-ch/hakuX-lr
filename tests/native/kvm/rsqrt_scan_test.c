/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The RSQRT site scan on a synthetic XBE: the instructions control flow
 * reaches are found, and bytes that only look like one are not. */
#include "native/cpu/kvm_rsqrt_scan.h"
#include "native/cpu/x86_decode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures;

static void check(bool ok, const char *what)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    failures += !ok;
}

static void wr32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

#define BASE  0x10000u
#define TEXT  0x11000u
#define RDATA 0x12000u
#define TEXT_SIZE  0x200u
#define RDATA_SIZE 0x40u
#define RDATA_RAW  0x18u /* the rest of .rdata is zeros not in the file */

static void put(uint8_t *text, uint32_t va, const char *hex)
{
    for (uint8_t *p = text + (va - TEXT); *hex; hex += 2) {
        unsigned b;
        sscanf(hex, "%2x", &b);
        *p++ = (uint8_t)b;
    }
}

static void decoder(void)
{
    static const struct {
        const char *what;
        uint8_t code[8];
        unsigned size, len, flow, rsqrt;
        uint32_t imm; /* imm[0], a branch's target from 0x1000 */
    } t[] = {
        {"rsqrtss xmm0, xmm1", {0xF3, 0x0F, 0x52, 0xC1}, 4, 4, NC_X86_NEXT, 2, 0},
        {"rsqrtps xmm0, [eax+ecx*4+disp32]", {0x0F, 0x52, 0x84, 0x88, 1, 2, 3, 4}, 8, 8, NC_X86_NEXT, 1, 0},
        {"66 0F 52 is no instruction", {0x66, 0x0F, 0x52, 0xC1}, 4, 0, 0, 0, 0},
        {"call rel32", {0xE8, 0x10, 0, 0, 0}, 5, 5, NC_X86_CALL, 0, 0x1015},
        {"jmp rel8 backwards", {0xEB, 0xFE}, 2, 2, NC_X86_JMP, 0, 0x1000},
        {"jne rel32", {0x0F, 0x85, 0, 1, 0, 0}, 6, 6, NC_X86_JCC, 0, 0x1106},
        {"jmp [eax*4+table]", {0xFF, 0x24, 0x85, 0x44, 0x10, 0x01, 0x00}, 7, 7, NC_X86_JMP_TABLE, 0, 0},
        {"jmp [eax+4]", {0xFF, 0x60, 0x04}, 3, 3, NC_X86_JMP_OTHER, 0, 0},
        {"ret imm16", {0xC2, 0x08, 0x00}, 3, 3, NC_X86_END, 0, 0},
        {"ud2", {0x0F, 0x0B}, 2, 2, NC_X86_END, 0, 0},
        {"mov eax, imm32", {0xB8, 0x78, 0x56, 0x34, 0x12}, 5, 5, NC_X86_NEXT, 0, 0x12345678},
        {"mov ax, imm16", {0x66, 0xB8, 0x34, 0x12}, 4, 4, NC_X86_NEXT, 0, 0},
        {"lock add [eax], ecx", {0xF0, 0x01, 0x08}, 3, 3, NC_X86_NEXT, 0, 0},
        {"lock add eax, ecx is no instruction", {0xF0, 0x01, 0xC8}, 3, 0, 0, 0, 0},
        {"lea eax, eax is no instruction", {0x8D, 0xC0}, 2, 0, 0, 0, 0},
        {"fld dword [eax]", {0xD9, 0x00}, 2, 2, NC_X86_NEXT, 0, 0},
        {"D9 /1 is no instruction", {0xD9, 0x08}, 2, 0, 0, 0, 0},
        {"a call cut short", {0xE8, 0x10, 0}, 3, 0, 0, 0, 0},
    };
    for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        NcX86Insn in;
        const bool ok = nc_x86_decode(t[i].code, t[i].size, 0x1000, &in);
        check(t[i].len ? ok && in.len == t[i].len && in.flow == t[i].flow && in.rsqrt == t[i].rsqrt &&
                         (!t[i].imm || (in.nimm == 1 && in.imm[0] == t[i].imm))
                       : !ok,
              t[i].what);
    }
    NcX86Insn in;
    const uint8_t cmp[] = {0x83, 0xF9, 0x07}, far_jmp[] = {0xFF, 0x2D, 0, 0x20, 0x01, 0};
    check(nc_x86_decode(cmp, 3, 0x1000, &in) && in.cmp_reg == 2 && in.cmp_imm == 7, "cmp ecx, 7 bounds a switch");
    check(nc_x86_decode(far_jmp, 6, 0x1000, &in) && in.has_abs && in.abs == 0x12000 && !in.nimm,
          "an absolute operand is reported apart from immediates");
    const uint8_t pop_ds[] = {0x1F}, int21[] = {0xCD, 0x21};
    check(nc_x86_decode(pop_ds, 1, 0x1000, &in) && in.odd && nc_x86_decode(int21, 2, 0x1000, &in) && in.odd,
          "instructions compiled code doesn't have are told apart");
}

int main(void)
{
    decoder();

    const uint32_t file_size = 0x1000 + TEXT_SIZE + RDATA_RAW;
    uint8_t *xbe = calloc(1, file_size), *text = xbe + 0x1000, *rdata = text + TEXT_SIZE;
    if (!xbe) {
        return 1;
    }
    memcpy(xbe, "XBEH", 4);
    wr32(xbe + 0x104, BASE);
    wr32(xbe + 0x11C, 2);
    wr32(xbe + 0x120, BASE + 0x200);
    wr32(xbe + 0x128, TEXT ^ 0xA8FC57ABu); /* the entry point, under the retail key */
    uint8_t *sec = xbe + 0x200;
    wr32(sec + 0x00, 0x06); /* preload, executable */
    wr32(sec + 0x04, TEXT);
    wr32(sec + 0x08, TEXT_SIZE);
    wr32(sec + 0x0C, 0x1000);
    wr32(sec + 0x10, TEXT_SIZE);
    wr32(sec + 0x14, BASE + 0x300);
    memcpy(xbe + 0x300, ".text", 6);
    sec += 0x38;
    wr32(sec + 0x00, 0x02);
    wr32(sec + 0x04, RDATA);
    wr32(sec + 0x08, RDATA_SIZE);
    wr32(sec + 0x0C, 0x1000 + TEXT_SIZE);
    wr32(sec + 0x10, RDATA_RAW);
    wr32(sec + 0x14, BASE + 0x310);
    memcpy(xbe + 0x310, ".rdata", 7);

    memset(text, 0xCC, TEXT_SIZE);
    /* the entry: a direct call, an immediate holding 0F 52 C1 (at 11007),
     * and main as XDK startup reaches it, through a pointer */
    put(text, 0x11000, "E81B000000" "B8900F52C1" "FF1500200100" "C3");
    put(text, 0x11020, "F30F52C1" "C3");             /* called: rsqrtss xmm0, xmm1 */
    /* through the pointer: rsqrtps xmm2, xmm1 and a two-case switch, its
     * table among the code (11044) */
    put(text, 0x11030, "0F52D1" "83F801" "7708" "FF248544100100");
    put(text, 0x11040, "C3");
    wr32(text + 0x44, 0x11050);
    wr32(text + 0x48, 0x11060);
    put(text, 0x11050, "F30F520510200100" "C3");     /* case 0: rsqrtss xmm0, [12010] */
    put(text, 0x11060, "0F52C0" "C3");               /* case 1: rsqrtps xmm0, xmm0 */
    put(text, 0x11070, "0F52C9" "C3");               /* nothing refers to this function */
    put(text, 0x11080, "F30F52C2" "C3");             /* a callback, only in data */
    /* mov eax, C1520FC3: its immediate reads as ret; rsqrtps xmm0, xmm1,
     * and a dword in the data happens to be that address (11092) */
    put(text, 0x11090, "B8C30F52C1" "C3");
    put(text, 0x110A0, "E8EBFFFFFF" "C3");           /* another callback, which calls 11090 */
    wr32(rdata + 0x00, 0x11030);
    wr32(rdata + 0x04, 0x11081); /* the rsqrtss at 11080 past its F3: an rsqrtps that isn't there */
    wr32(rdata + 0x08, 0x11080);
    wr32(rdata + 0x0C, 0x110A0);
    wr32(rdata + 0x10, 0x3F800000);
    wr32(rdata + 0x14, 0x11092); /* tried before 110A0: later seeds first */

    NcRsqrtScan scan;
    static const uint32_t expect[] = {0x11020, 0x11030, 0x11050, 0x11060, 0x11080};
    check(nc_rsqrt_scan_xbe(xbe, file_size, &scan), "the XBE is scanned");
    for (unsigned i = 0; i < scan.nsites; i++) {
        printf("  site %08x\n", scan.sites[i]);
    }
    check(scan.nsites == 5 && !memcmp(scan.sites, expect, sizeof(expect)),
          "sites: called, through a pointer, both switch cases, a callback in data");
    check(scan.unreached == 1, "the unreferenced function's RSQRT is left alone");
    check(scan.evicted_straddled == 1, "the seed inside an instruction gives way to the function called over it");
    check(scan.disputed == 0, "the linear sweep agrees with every site");

    /* the entry under the debug key, and a section table past the file */
    wr32(xbe + 0x128, TEXT ^ 0x94859D4Bu);
    NcRsqrtScan debug;
    check(nc_rsqrt_scan_xbe(xbe, file_size, &debug) && debug.nsites == 5, "a debug XBE's entry is decoded");
    nc_rsqrt_scan_free(&debug);
    wr32(xbe + 0x11C, 2000);
    check(!nc_rsqrt_scan_xbe(xbe, file_size, &debug), "a broken section table is refused");
    check(!nc_rsqrt_scan_xbe(xbe, 0x100, &debug), "a truncated file is refused");

    nc_rsqrt_scan_free(&scan);
    free(xbe);
    return failures ? 1 : 0;
}
