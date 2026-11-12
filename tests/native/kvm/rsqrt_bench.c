/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * kvm-rsqrt-bench ss|ps N: a guest loop of N RSQRTSS (or RSQRTPS) under
 * KVM, timed. The environment picks the path: no NC_KVM_RSQRT_SITES, the
 * host's own result; NC_KVM_RSQRT_SITES=1000c, an INT3 over the
 * instruction in the guest's #BP handler; with NC_KVM_RSQRT_INGUEST=0 as
 * well, a KVM exit per instruction. Prints the time per iteration and the
 * last result.
 */
#include "native/core/core.h"
#include "native/hle/hle_native.h"
#include "native/cpu/kvm_rsqrt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CODE 0x10000u
#define DATA 0x20000u

static NcCore core;

static bool system_op(void *opaque, NcFrame *f, enum NcSysOp op, unsigned size)
{
    fprintf(stderr, "unexpected system op %d at %08x\n", (int)op, nc_frame_eip(f));
    return false;
}

static uint32_t port_in(uint16_t port, unsigned bytes)
{
    return 0xFFFFFFFFu;
}

static void port_out(uint16_t port, unsigned bytes, uint32_t value)
{
}

int main(int argc, char **argv)
{
    const bool packed = argc > 1 && !strcmp(argv[1], "ps");
    const uint32_t n = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 1000000u;
    if (!nc_vm_init(&core.vm, 64u << 20) || !nc_kernel_init(&core.kernel, &core.vm) ||
        !nc_vm_alloc_at(&core.vm, CODE, NC_PAGE, NULL) || !nc_vm_alloc_at(&core.vm, DATA, NC_PAGE, NULL)) {
        return 1;
    }
    NcCpuConfig cfg = {.membase = core.vm.base, .sysop = system_op, .idt_base = 0x8001F000u, .idt_limit = 0x7FF};
    nc_core_cpu_backend(&core, &cfg);
    cfg.port_in = port_in;
    cfg.port_out = port_out;
    NcCpu *cpu = core.cpu = nc_cpu_create(&cfg, core.kernel.kpcr_va);
    if (!cpu) {
        return 1;
    }
    /* mov ecx, n; movups xmm1, [DATA+0x340]; loop at CODE+12: the RSQRT,
     * sub ecx, 1, jnz loop; movups [DATA+0x360], xmm0 (xmm2 packed); hlt */
    uint8_t code[64];
    unsigned k = 0;
    code[k++] = 0xB9;
    nc_wr32(code + k, n);
    k += 4;
    const uint8_t load[] = {0x0F, 0x10, 0x0D};
    memcpy(code + k, load, 3);
    k += 3;
    nc_wr32(code + k, DATA + 0x340);
    k += 4;
    const uint8_t ss[] = {0xF3, 0x0F, 0x52, 0xC1}, ps[] = {0x0F, 0x52, 0xD1};
    const unsigned len = packed ? 3 : 4;
    memcpy(code + k, packed ? ps : ss, len);
    k += len;
    const uint8_t tail[] = {0x83, 0xE9, 0x01, 0x75, (uint8_t)-(int8_t)(len + 5)};
    memcpy(code + k, tail, sizeof(tail));
    k += sizeof(tail);
    const uint8_t store[] = {0x0F, 0x11, packed ? 0x15 : 0x05};
    memcpy(code + k, store, 3);
    k += 3;
    nc_wr32(code + k, DATA + 0x360);
    k += 4;
    code[k++] = 0xF4;
    memcpy(nc_vm_host(&core.vm, CODE), code, k);
    const uint32_t inputs[4] = {0x3F800000, 0x40800000, 0x3E800000, 0x41800000};
    memcpy(nc_vm_host(&core.vm, DATA + 0x340), inputs, sizeof(inputs));

    NcCpuContext c;
    nc_context_init(cpu, &c, CODE, DATA + 0xF00);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    nc_cpu_run(cpu, &c);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    const double ns = (double)(t1.tv_sec - t0.tv_sec) * 1e9 + (double)(t1.tv_nsec - t0.tv_nsec);
    printf("%s %u iterations: %.1f ns each, result %08x %s\n", packed ? "rsqrtps" : "rsqrtss", n, ns / n,
           nc_vm_rd32(&core.vm, DATA + 0x360),
           nc_vm_rd32(&core.vm, DATA + 0x360) == 0x3F7FF000u ? "(Intel)" : "(not Intel)");
    nc_cpu_destroy(cpu);
    return 0;
}
