/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * cpudiff-ref: run the CPU differential tests on the host's x86 CPU, under
 * KVM, and write each test's output region (tests/native/cpudiff/README.md).
 *
 *   cpudiff-ref <test dir> <out dir>
 *
 * A test is <name>.bin (32-bit code and data at 0x10000, ending in HLT) and
 * <name>.cfg (gen.py). The guest runs in flat 32-bit protected mode without
 * paging, at CPL 0, with the FPU and SSE enabled and every exception
 * unhandled: anything but the HLT fails the test. Output: <out dir>/<name>.out
 * and one "RAN|FAIL <name>" line per test.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define CODE 0x10000u
#define CODE_MAX 0x200000u
#define MAX_REGIONS 8

typedef struct Region {
    uint32_t addr, size;
    uint8_t *host;
} Region;

static int die(const char *what)
{
    fprintf(stderr, "cpudiff-ref: %s: %s\n", what, strerror(errno));
    exit(2);
}

static void flat(struct kvm_segment *s, unsigned selector, unsigned type)
{
    memset(s, 0, sizeof(*s));
    s->selector = (uint16_t)selector;
    s->limit = 0xFFFFFFFFu;
    s->type = (uint8_t)type;
    s->present = 1;
    s->db = 1;
    s->s = 1;
    s->g = 1;
}

static int run_test(int kvm, const char *dir, const char *name, const char *outdir)
{
    char path[1024];
    Region regions[MAX_REGIONS];
    unsigned nregions = 0;
    uint32_t out_addr = 0, out_size = 0;

    snprintf(path, sizeof(path), "%s/%s.cfg", dir, name);
    FILE *cfg = fopen(path, "r");
    if (!cfg) {
        return -1;
    }
    char line[256];
    while (fgets(line, sizeof(line), cfg)) {
        unsigned a, b;
        if (sscanf(line, "region %i %i", &a, &b) == 2 && nregions < MAX_REGIONS - 1) {
            regions[nregions].addr = a;
            regions[nregions].size = b;
            nregions++;
        } else if (sscanf(line, "out %i %i", &a, &b) == 2) {
            out_addr = a;
            out_size = b;
        }
    }
    fclose(cfg);
    regions[nregions].addr = CODE;
    regions[nregions].size = CODE_MAX - CODE;
    nregions++;

    const int vm = ioctl(kvm, KVM_CREATE_VM, 0);
    if (vm < 0) {
        die("KVM_CREATE_VM");
    }
    for (unsigned i = 0; i < nregions; i++) {
        Region *r = &regions[i];
        r->host = mmap(NULL, r->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (r->host == MAP_FAILED) {
            die("mmap");
        }
        struct kvm_userspace_memory_region m = {
            .slot = i, .guest_phys_addr = r->addr, .memory_size = r->size, .userspace_addr = (uintptr_t)r->host,
        };
        if (ioctl(vm, KVM_SET_USER_MEMORY_REGION, &m) < 0) {
            die("KVM_SET_USER_MEMORY_REGION");
        }
    }
    snprintf(path, sizeof(path), "%s/%s.bin", dir, name);
    const int bin = open(path, O_RDONLY);
    Region *code = &regions[nregions - 1];
    if (bin < 0 || read(bin, code->host, code->size) <= 0) {
        die(path);
    }
    close(bin);

    const int vcpu = ioctl(vm, KVM_CREATE_VCPU, 0);
    if (vcpu < 0) {
        die("KVM_CREATE_VCPU");
    }
    struct kvm_cpuid2 *cpuid = calloc(1, sizeof(*cpuid) + 128 * sizeof(struct kvm_cpuid_entry2));
    cpuid->nent = 128;
    if (ioctl(kvm, KVM_GET_SUPPORTED_CPUID, cpuid) < 0 || ioctl(vcpu, KVM_SET_CPUID2, cpuid) < 0) {
        die("CPUID");
    }
    free(cpuid);
    struct kvm_sregs sregs;
    if (ioctl(vcpu, KVM_GET_SREGS, &sregs) < 0) {
        die("KVM_GET_SREGS");
    }
    flat(&sregs.cs, 0x08, 0xB);
    flat(&sregs.ds, 0x10, 0x3);
    sregs.es = sregs.fs = sregs.gs = sregs.ss = sregs.ds;
    sregs.cr0 = (sregs.cr0 | 0x1u | 0x2u | 0x20u) & ~(0x4ull | 0x8ull | 0x80000000ull); /* PE MP NE; no EM TS PG */
    sregs.cr4 |= 0x200u | 0x400u;                                                       /* OSFXSR OSXMMEXCPT */
    if (ioctl(vcpu, KVM_SET_SREGS, &sregs) < 0) {
        die("KVM_SET_SREGS");
    }
    struct kvm_regs regs = { .rip = CODE, .rflags = 2 };
    if (ioctl(vcpu, KVM_SET_REGS, &regs) < 0) {
        die("KVM_SET_REGS");
    }
    const int run_size = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
    struct kvm_run *run = mmap(NULL, (size_t)run_size, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu, 0);
    if (run == MAP_FAILED) {
        die("mmap vcpu");
    }

    int result = 0;
    alarm(20);
    if (ioctl(vcpu, KVM_RUN, 0) < 0 || run->exit_reason != KVM_EXIT_HLT) {
        ioctl(vcpu, KVM_GET_REGS, &regs);
        fprintf(stderr, "cpudiff-ref: %s: exit %u at eip %08llx\n", name, run->exit_reason, (unsigned long long)regs.rip);
        result = -1;
    }
    alarm(0);
    if (!result) {
        result = -1;
        for (unsigned i = 0; i < nregions; i++) {
            const Region *r = &regions[i];
            if (out_addr >= r->addr && out_addr - r->addr + out_size <= r->size) {
                snprintf(path, sizeof(path), "%s/%s.out", outdir, name);
                FILE *out = fopen(path, "wb");
                if (!out || fwrite(r->host + (out_addr - r->addr), 1, out_size, out) != out_size) {
                    die(path);
                }
                fclose(out);
                result = 0;
            }
        }
    }
    munmap(run, (size_t)run_size);
    close(vcpu);
    close(vm);
    for (unsigned i = 0; i < nregions; i++) {
        munmap(regions[i].host, regions[i].size);
    }
    return result;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: cpudiff-ref <test dir> <out dir>\n");
        return 2;
    }
    const int kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (kvm < 0) {
        die("/dev/kvm");
    }
    char path[1024];
    snprintf(path, sizeof(path), "%s/manifest.txt", argv[1]);
    FILE *manifest = fopen(path, "r");
    if (!manifest) {
        die(path);
    }
    char line[512];
    unsigned failed = 0, ran = 0;
    while (fgets(line, sizeof(line), manifest)) {
        char name[256];
        if (sscanf(line, "%255s", name) != 1) {
            continue;
        }
        const int r = run_test(kvm, argv[1], name, argv[2]);
        printf("%s %s\n", r ? "FAIL" : "RAN", name);
        ran++;
        failed += r != 0;
    }
    fclose(manifest);
    printf("summary ran=%u failed=%u\n", ran, failed);
    return failed ? 1 : 0;
}
