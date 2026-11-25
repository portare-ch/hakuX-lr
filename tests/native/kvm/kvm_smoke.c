/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Exercise the KVM backend with small guest programs and a system-operation recorder.
 * Requires an x86-64 Linux host with /dev/kvm.
 *
 * - Check CPL 3, IOPL 3 and IF, and compare SIDT, SGDT and SMSW handling with FEX.
 * - Resume from INT, INT3, UD2, WBINVD, RDMSR, CR0 reads, port I/O, MMIO, page faults and
 * HLT.
 * - Switch threads inside a trapped kernel call and check per-thread registers and SSE state.
 * - Deliver an interrupt with IF set, or defer it from CLI until STI.
 * - Check the 733 MHz RDTSC and continuity across a guest-clock pause.
 * - Check hook dispatch and execution of the following guest instruction.
 * - Verify split 16-byte MMIO stores, x87 state round trips and restoration of the kick
 * signal handler.
 * - Check the real OHCI register page under KVM: reads without exits, trapped writes,
 *   and an interrupt's status visible to guest code before it acknowledges the IRQ.
 */
#include "native/core/core.h"
#include "native/core/mmio.h"
#include "native/core/irq.h"
#include "native/dev/usb.h"
#include "native/hle/hle_native.h"
#include "native/cpu/kvm_rsqrt.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define CODE  0x10000u
#define DATA  0x20000u
#define FLAG  (DATA + 0x200)
#define LATE  0x30000u /* unmapped until the guest touches it */
#define IDT   0x8001F000u
#define MMIO  0xFE000000u
#define OHCI  0xFED00000u
#define OHCI_IRQ 1
#define OHCI_RHSC (1u << 6)
#define OHCI_MIE (1u << 31)
#define HOOK  (CODE + 0x805)
#define WP    0x40000u
#define WPFLAG (DATA + 0x204)

static NcCore core;
static NcCpu *cpu;
static unsigned failures;

static uint32_t reports[16];
static unsigned nreports;
static struct {
    enum NcSysOp op;
    unsigned size;
    uint32_t eip;
} ops[64];
static unsigned nops;
static NcCpuContext ctx[2];
static int cur;
static int irqs, hooks, late_faults;
static uint32_t irq_eip, irq_ecx;
static uint16_t out_port;
static uint32_t out_value;
static uint64_t mmio_reg;
static struct {
    uint32_t off;
    uint64_t value;
    unsigned size;
} mmio_writes[8];
static unsigned nmmio_writes;
static int (*core_fault)(void *, NcFrame *, uint32_t, bool, const char **);

static void check(bool ok, const char *what)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    failures += !ok;
}

static bool system_op(void *opaque, NcFrame *f, enum NcSysOp op, unsigned size)
{
    const uint32_t eip = nc_frame_eip(f);
    if (nops < 64) {
        ops[nops].op = op;
        ops[nops].size = size;
        ops[nops].eip = eip;
        nops++;
    }
    switch (op) {
    case NC_SYSOP_INT: {
        const uint8_t v = *(const uint8_t *)nc_vm_host(&core.vm, eip + 1);
        if (v == 0x41) {
            /* a kernel call that switches threads: this one resumes past it */
            nc_frame_set_eip(f, eip + size);
            nc_frame_save(f, &ctx[cur]);
            cur ^= 1;
            nc_frame_load(f, &ctx[cur]);
            return true;
        }
        if (v == 0x42 && nreports < 16) {
            reports[nreports++] = nc_frame_gpr(f, NC_EAX);
        } else if (v == 0x43) {
            /* 100 ms the guest's TSC leaves out, as a pause's */
            const uint64_t t0 = nc_cpu_host_cycles();
            usleep(100000);
            nc_cpu_cycle_offset_add(cpu, nc_cpu_host_cycles() - t0);
        } else if (v == 0x46) {
            nc_vm_view_prot(&core.vm, WP, NC_PAGE, PROT_READ);  /* protect between vCPU entries */
        } else if (v == 0x44) {
            usleep(50000); /* 50 ms it counts */
        } else if (v == 0x45) {
            /* the thread through a savestate's export and import */
            NcCpuContext saved, back;
            NcCpuState st;
            nc_frame_set_eip(f, eip + size);
            nc_frame_save(f, &saved);
            nc_context_export(&saved, &st);
            nc_context_import(&back, &st);
            nc_frame_load(f, &back);
            return true;
        }
        nc_frame_set_eip(f, eip + size);
        return true;
    }
    case NC_SYSOP_RDMSR:
        nc_frame_set_gpr(f, NC_EAX, 0x5A5A);
        nc_frame_set_gpr(f, NC_EDX, 0);
        nc_frame_set_eip(f, eip + size);
        return true;
    case NC_SYSOP_MOVCR:
        nc_frame_set_gpr(f, NC_EAX, 0x8001003B);
        nc_frame_set_eip(f, eip + size);
        return true;
    case NC_SYSOP_INT3:
    case NC_SYSOP_UD2:
    case NC_SYSOP_WBINVD:
        nc_frame_set_eip(f, eip + size);
        return true;
    case NC_SYSOP_INTERRUPT:
        irqs++;
        irq_eip = eip;
        irq_ecx = nc_frame_gpr(f, NC_ECX);
        nc_wr32(nc_vm_host(&core.vm, FLAG), 1);
        nc_frame_clear_interrupt(f);
        return true;
    case NC_SYSOP_HOOK:
        hooks++;
        nc_frame_bypass_hook(f);
        return true;
    default:
        return false;
    }
}

static uint32_t port_in(uint16_t port, unsigned bytes)
{
    return port == 0x1234 && bytes == 4 ? 0xCAFEF00Du : 0xFFFFFFFFu;
}

static void port_out(uint16_t port, unsigned bytes, uint32_t value)
{
    out_port = port;
    out_value = value;
}

static uint64_t mmio_read(void *opaque, uint32_t off, unsigned size)
{
    return mmio_reg;
}

static void mmio_write(void *opaque, uint32_t off, uint64_t value, unsigned size)
{
    mmio_reg = value;
    if (nmmio_writes < 8) {
        mmio_writes[nmmio_writes].off = off;
        mmio_writes[nmmio_writes].value = value;
        mmio_writes[nmmio_writes].size = size;
        nmmio_writes++;
    }
}

/* A device whose page the guest reads directly (nc_mmio_direct): its read
 * handler must never run, its write handler only for the register that
 * isn't quiet. */
#define MMIO_DIRECT 0xFE100000u
static uint8_t *direct_page;
static unsigned direct_reads, direct_writes;

static uint64_t direct_read(void *opaque, uint32_t off, unsigned size)
{
    direct_reads++;
    return 0xBAD0BAD0u;
}

static void direct_write(void *opaque, uint32_t off, uint64_t value, unsigned size)
{
    direct_writes++;
    memcpy(direct_page + off, &value, size);
}

/* Make WP read-only while the guest writes to it. Sample the counter twice
 * before the fault handler restores write access to check for stale stores. */
static int wp_faults;
static volatile int wp_sampled;
static uint32_t wp_v0, wp_v1;

static void *protect_later(void *arg)
{
    usleep(20000);
    nc_vm_view_prot(&core.vm, WP, NC_PAGE, PROT_READ);
    wp_v0 = nc_vm_rd32(&core.vm, WP);
    usleep(5000);
    wp_v1 = nc_vm_rd32(&core.vm, WP);
    __atomic_store_n(&wp_sampled, 1, __ATOMIC_RELEASE);
    usleep(200000);
    nc_wr32(nc_vm_host(&core.vm, WPFLAG), 1);   /* let the guest exit if it never faults */
    return arg;
}

/* A page the guest touches first maps on its fault; the rest is the core's. */
static int fault(void *opaque, NcFrame *f, uint32_t va, bool write, const char **why)
{
    if (va - WP < NC_PAGE && write) {
        if (wp_faults++ == 1) {
            while (!__atomic_load_n(&wp_sampled, __ATOMIC_ACQUIRE)) {
                usleep(100);
            }
            nc_wr32(nc_vm_host(&core.vm, WPFLAG), 1);
        }
        return nc_vm_view_prot(&core.vm, WP, NC_PAGE, PROT_READ | PROT_WRITE) == 0 ? 1 : 0;
    }
    if (va - LATE < NC_PAGE) {
        late_faults++;
        return nc_vm_alloc_at(&core.vm, LATE, NC_PAGE, NULL) ? 1 : 0;
    }
    return core_fault(opaque, f, va, write, why);
}

static unsigned hook_list(void *opaque, uint32_t *eips, unsigned max)
{
    eips[0] = HOOK;
    return max ? 1 : 0;
}

static void *interrupt_later(void *arg)
{
    usleep((useconds_t)(uintptr_t)arg);
    nc_cpu_request_interrupt(cpu);
    return NULL;
}

/* Where an instruction's bytes first are in a program (its offset). */
static uint32_t offset_of(const uint8_t *code, size_t n, const uint8_t *insn, size_t m)
{
    for (size_t i = 0; i + m <= n; i++) {
        if (!memcmp(code + i, insn, m)) {
            return (uint32_t)i;
        }
    }
    return UINT32_MAX;
}

static void put(uint32_t at, const uint8_t *code, size_t n)
{
    memcpy(nc_vm_host(&core.vm, at), code, n);
}

static void run_at(uint32_t eip)
{
    NcCpuContext c;
    nc_context_init(cpu, &c, eip, DATA + 0xF00);
    nc_cpu_run(cpu, &c);
}

static bool saw(enum NcSysOp op, unsigned size, uint32_t eip)
{
    for (unsigned i = 0; i < nops; i++) {
        if (ops[i].op == op && ops[i].size == size && ops[i].eip == eip) {
            return true;
        }
    }
    return false;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!nc_vm_init(&core.vm, 64u << 20) || !nc_kernel_init(&core.kernel, &core.vm) ||
        !nc_vm_alloc_at(&core.vm, CODE, 4 * NC_PAGE, NULL) || !nc_vm_alloc_at(&core.vm, DATA, NC_PAGE, NULL) ||
        !nc_vm_alloc_at(&core.vm, WP, NC_PAGE, NULL)) {
        printf("FAIL: guest memory\n");
        return 1;
    }
    NcMmioDev dev = {"kvm-smoke", MMIO, NC_PAGE, mmio_read, mmio_write, NULL};
    nc_mmio_register(&dev);
    direct_page = nc_big_alloc(NC_PAGE);
    NcMmioDev ddev = {"kvm-direct", MMIO_DIRECT, NC_PAGE, direct_read, direct_write, NULL};
    nc_mmio_register(&ddev);
    const NcMmioDirect dpage = {MMIO_DIRECT, direct_page, MMIO_DIRECT + 0x10};
    nc_mmio_direct(&dpage);
    /* Register OHCI before creating the vCPU so KVM installs its read-only slot.
     * Stop its timer thread: the test controls state through guest writes and hotplug. */
    nc_dev_ohci_set_pads(1);
    nc_dev_ohci_set_voice(0);
    nc_dev_ohci_init(&core.vm);
    nc_dev_ohci_stop();
    NcCpuConfig cfg = {.membase = core.vm.base, .sysop = system_op, .idt_base = IDT, .idt_limit = 0x7FF};
    nc_core_cpu_backend(&core, &cfg);
    core_fault = cfg.fault;
    cfg.fault = fault;
    cfg.port_in = port_in;
    cfg.port_out = port_out;
    cfg.hook_list = hook_list;
    /* Known instruction boundaries in the synthetic RSQRT program below.
     * Intel must ignore these; AMD alone installs the corrections. */
    setenv("NC_KVM_RSQRT_SITES", "10a0e,10a19,10a4e,10a59", 1);
    cpu = core.cpu = nc_cpu_create(&cfg, core.kernel.kpcr_va);
    unsetenv("NC_KVM_RSQRT_SITES");
    if (!cpu) {
        printf("FAIL: a KVM guest CPU (needs /dev/kvm with TSC scaling and UMIP)\n");
        return 1;
    }
    printf("PASS: a KVM guest CPU (%s)\n", nc_cpu_backend);

    /* Privilege, UMIP and each exit, in one run. */
    const uint8_t exits[] = {
        0x9C, 0x58, 0xCD, 0x42,                         /* pushfd; pop eax; int 42h      (1) */
        0x8C, 0xC8, 0xCD, 0x42,                         /* mov eax, cs; int 42h          (2) */
        0x0F, 0x01, 0x0D, 0x00, 0x01, 0x02, 0x00,       /* sidt [0x20100] */
        0xA1, 0x02, 0x01, 0x02, 0x00, 0xCD, 0x42,       /* mov eax, [0x20102]; int 42h   (3) */
        0x0F, 0x01, 0x05, 0x10, 0x01, 0x02, 0x00,       /* sgdt [0x20110] */
        0xA1, 0x12, 0x01, 0x02, 0x00, 0xCD, 0x42,       /* mov eax, [0x20112]; int 42h   (4) */
        0x0F, 0x01, 0xE0, 0xCD, 0x42,                   /* smsw eax; int 42h             (5) */
        0xCC,                                           /* int3 */
        0x0F, 0x0B,                                     /* ud2 */
        0x0F, 0x09,                                     /* wbinvd */
        0xB9, 0x1B, 0x00, 0x00, 0x00, 0x0F, 0x32,       /* mov ecx, 1Bh; rdmsr */
        0xCD, 0x42,                                     /* int 42h                       (6) */
        0x0F, 0x20, 0xC0, 0xCD, 0x42,                   /* mov eax, cr0; int 42h         (7) */
        0xBA, 0x34, 0x12, 0x00, 0x00, 0xED, 0xCD, 0x42, /* mov edx, 1234h; in eax, dx; int 42h (8) */
        0xB8, 0xEF, 0xBE, 0xAD, 0xDE, 0xEF,             /* mov eax, DEADBEEFh; out dx, eax */
        0xA3, 0x00, 0x00, 0x00, 0xFE,                   /* mov [0xFE000000], eax */
        0x31, 0xC0,                                     /* xor eax, eax */
        0xA1, 0x00, 0x00, 0x00, 0xFE, 0xCD, 0x42,       /* mov eax, [0xFE000000]; int 42h (9) */
        0xA1, 0x00, 0x00, 0x03, 0x00, 0xCD, 0x42,       /* mov eax, [0x30000]; int 42h   (10) */
        0xF4,                                           /* hlt */
    };
    put(CODE, exits, sizeof(exits));
    run_at(CODE);
    check(nreports == 10, "ten reports from the exits run");
    check((reports[0] & 0x3000) == 0x3000 && (reports[0] & 0x200), "EFLAGS: IOPL 3, IF set");
    check((reports[1] & 3) == 3, "CS: CPL 3");
    check(reports[2] == IDT && nc_rd16(nc_vm_host(&core.vm, DATA + 0x100)) == 0x7FF, "SIDT: the kernel's IDT");
    check(reports[3] == 0xFFFE0000u && nc_rd16(nc_vm_host(&core.vm, DATA + 0x110)) == 0, "SGDT: FEX's answer");
    check(reports[4] == 0x80050033u, "SMSW: FEX's CR0");
#define AT(...) (CODE + offset_of(exits, sizeof(exits), (const uint8_t[]){__VA_ARGS__}, \
                                  sizeof((const uint8_t[]){__VA_ARGS__})))
    check(saw(NC_SYSOP_INT3, 1, AT(0xCC)), "INT3: system op, size 1");
    check(saw(NC_SYSOP_UD2, 2, AT(0x0F, 0x0B)), "UD2: system op, size 2");
    check(saw(NC_SYSOP_WBINVD, 2, AT(0x0F, 0x09)), "WBINVD: system op, size 2");
    check(saw(NC_SYSOP_RDMSR, 2, AT(0x0F, 0x32)) && reports[5] == 0x5A5A, "RDMSR: system op, result in EAX");
    check(saw(NC_SYSOP_MOVCR, 3, AT(0x0F, 0x20, 0xC0)) && reports[6] == 0x8001003B, "MOV EAX, CR0: system op, size 3");
    for (unsigned i = 0; i < nops && failures; i++) {
        printf("  system op %d, size %u, at %08x\n", (int)ops[i].op, ops[i].size, ops[i].eip);
    }
    check(reports[7] == 0xCAFEF00Du, "IN: the port's value");
    check(out_port == 0x1234 && out_value == 0xDEADBEEFu, "OUT: the port and value");
    check(reports[8] == 0xDEADBEEFu && mmio_reg == 0xDEADBEEFu, "MMIO: a write, then a read of it");
    check(reports[9] == 0 && late_faults == 1, "a page fault: mapped by the fault path, the access run again");

    /* Load MMX and SSE registers into a vCPU whose saved FPU state is still initial.
     * KVM_SET_FPU alone used to leave the XSAVE header unchanged, so the next entry read
     * zeros. Burnout 3 lost its XMV alpha mask this way after a thread switch. */
    const uint8_t fpregs[] = {
        0x0F, 0x7E, 0xF8, 0xCD, 0x42,       /* movd eax, mm7; int 42h */
        0x66, 0x0F, 0x7E, 0xC8, 0xCD, 0x42, /* movd eax, xmm1; int 42h */
        0x0F, 0x77, 0xF4,                   /* emms; hlt */
    };
    put(CODE + 0xA00, fpregs, sizeof(fpregs));
    {
        NcCpuContext c;
        NcCpuState st;
        nc_context_init(cpu, &c, CODE + 0xA00, DATA + 0xF00);
        nc_context_export(&c, &st);
        const uint32_t mm7 = 0x77777777u, xmm1 = 0x11223344u;
        memcpy(st.mm[7], &mm7, 4);
        memcpy(st.xmm[1], &xmm1, 4);
        nc_context_import(&c, &st);
        nreports = 0;
        nc_cpu_run(cpu, &c);
    }
    check(nreports == 2 && reports[0] == 0x77777777u && reports[1] == 0x11223344u,
          "MMX and SSE registers set over an initial FPU state: the guest reads them");

    /* A thread switch inside a trapped kernel call (int 41h). */
    const uint8_t thread_a[] = {
        0xBB, 0xAA, 0xAA, 0x00, 0x00,       /* mov ebx, 0AAAAh */
        0xB8, 0x11, 0x11, 0x11, 0x11,       /* mov eax, 11111111h */
        0x66, 0x0F, 0x6E, 0xC0,             /* movd xmm0, eax */
        0xCD, 0x41,                         /* int 41h: to B */
        0x83, 0xC3, 0x01, 0x89, 0xD8,       /* add ebx, 1; mov eax, ebx */
        0xCD, 0x42,                         /* int 42h */
        0x66, 0x0F, 0x7E, 0xC0, 0xCD, 0x42, /* movd eax, xmm0; int 42h */
        0xCD, 0x41,                         /* int 41h: to B */
        0xF4,                               /* hlt */
    };
    uint8_t thread_b[sizeof(thread_a)];
    memcpy(thread_b, thread_a, sizeof(thread_a));
    thread_b[1] = thread_b[2] = 0xBB;
    memset(thread_b + 6, 0x22, 4);
    put(CODE + 0x400, thread_a, sizeof(thread_a));
    put(CODE + 0x480, thread_b, sizeof(thread_b));
    nc_context_init(cpu, &ctx[0], CODE + 0x400, DATA + 0xF00);
    nc_context_init(cpu, &ctx[1], CODE + 0x480, DATA + 0x800);
    nreports = 0;
    cur = 0;
    nc_cpu_run(cpu, &ctx[0]);
    check(nreports == 4 && reports[0] == 0xAAAB && reports[2] == 0xBBBC, "thread switch: each its own EBX and EIP");
    check(reports[1] == 0x11111111u && reports[3] == 0x22222222u, "thread switch: each its own XMM0");

    /* An interrupt at IF=1: the guest spins until the handler sets the flag. */
    const uint8_t spin[] = {
        0xA1, 0x00, 0x02, 0x02, 0x00, /* L: mov eax, [FLAG] */
        0x85, 0xC0, 0x74, 0xF7,       /* test eax, eax; jz L */
        0xCD, 0x42, 0xF4,             /* int 42h; hlt */
    };
    put(CODE + 0x600, spin, sizeof(spin));
    nc_wr32(nc_vm_host(&core.vm, FLAG), 0);
    pthread_t t;
    pthread_create(&t, NULL, interrupt_later, (void *)(uintptr_t)20000);
    irqs = 0;
    run_at(CODE + 0x600);
    pthread_join(t, NULL);
    check(irqs == 1 && irq_eip >= CODE + 0x600 && irq_eip < CODE + 0x609, "interrupt at IF=1: taken in the loop");

    /* At IF=0 it waits for STI: requested 10 ms into a CLI loop of ~100 ms. */
    const uint8_t held[] = {
        0xFA,                         /* cli */
        0xB9, 0x00, 0x00, 0x00, 0x10, /* mov ecx, 10000000h */
        0x49, 0x75, 0xFD,             /* L: dec ecx; jnz L */
        0xFB, 0x90, 0x90,             /* sti; nop; nop */
        0xCD, 0x42, 0xF4,             /* int 42h; hlt */
    };
    put(CODE + 0x680, held, sizeof(held));
    pthread_create(&t, NULL, interrupt_later, (void *)(uintptr_t)10000);
    irqs = 0;
    run_at(CODE + 0x680);
    pthread_join(t, NULL);
    check(irqs == 1 && irq_ecx == 0 && irq_eip > CODE + 0x689, "interrupt at IF=0: held until after STI");

    /* RDTSC: 733 MHz, and a pause left out. */
    const uint8_t tsc[] = {
        0x0F, 0x31, 0x89, 0xC3, 0x89, 0xD6, /* rdtsc; mov ebx, eax; mov esi, edx */
        0xCD, 0x43,                         /* int 43h: a 100 ms pause */
        0x0F, 0x31, 0x29, 0xD8, 0x19, 0xF2, /* rdtsc; sub eax, ebx; sbb edx, esi */
        0xCD, 0x42,                         /* int 42h */
        0x0F, 0x31, 0x89, 0xC3, 0x89, 0xD6,
        0xCD, 0x44,                         /* int 44h: 50 ms counted */
        0x0F, 0x31, 0x29, 0xD8, 0x19, 0xF2,
        0xCD, 0x42, 0xF4,
    };
    put(CODE + 0x700, tsc, sizeof(tsc));
    nreports = 0;
    run_at(CODE + 0x700);
    printf("  TSC: %u ticks across the pause, %u across 50 ms\n", reports[0], reports[1]);
    check(nreports == 2 && reports[0] < 7333333u, "RDTSC: a 100 ms pause left out (under 10 ms of ticks)");
    check(reports[1] > 33000000u && reports[1] < 45000000u, "RDTSC: 733 MHz (50 ms ~ 36.7 M ticks)");

    /* A hooked address, twice through a loop. */
    const uint8_t hooked[] = {
        0xB9, 0x02, 0x00, 0x00, 0x00, /* mov ecx, 2 */
        0xB8, 0x05, 0x00, 0x00, 0x00, /* L: mov eax, 5  (hooked: CODE + 0x805) */
        0x01, 0xC2, 0x49, 0x75, 0xF6, /* add edx, eax; dec ecx; jnz L */
        0x89, 0xD0, 0xCD, 0x42, 0xF4, /* mov eax, edx; int 42h; hlt */
    };
    put(CODE + 0x800, hooked, sizeof(hooked));
    nreports = 0;
    hooks = 0;
    nc_cpu_invalidate(cpu, 0, 0xFFFFF000u); /* the hook list read again, patched */
    nops = 0;
    run_at(CODE + 0x800);
    printf("  hook: %d hits, %u reports, the first %u\n", hooks, nreports, nreports ? reports[0] : 0);
    for (unsigned i = 0; i < nops; i++) {
        printf("  system op %d, size %u, at %08x\n", (int)ops[i].op, ops[i].size, ops[i].eip);
    }
    check(hooks == 2 && nreports == 1 && reports[0] == 10, "hook: its system op each time, the instruction run");

    /* A 16-byte SSE store to a device: KVM hands it over as two 8-byte parts. */
    const uint8_t wide[] = {
        0x0F, 0x10, 0x0D, 0x00, 0x03, 0x02, 0x00, /* movups xmm1, [0x20300] */
        0x0F, 0x11, 0x0D, 0x10, 0x00, 0x00, 0xFE, /* movups [0xFE000010], xmm1 */
        0xCD, 0x42, 0xF4,
    };
    const uint64_t lo = 0x0123456789ABCDEFull, hi = 0xFEDCBA9876543210ull;
    memcpy(nc_vm_host(&core.vm, DATA + 0x300), &lo, 8);
    memcpy(nc_vm_host(&core.vm, DATA + 0x308), &hi, 8);
    put(CODE + 0x900, wide, sizeof(wide));
    nmmio_writes = 0;
    run_at(CODE + 0x900);
    uint64_t got_lo = 0, got_hi = 0;
    for (unsigned i = 0; i < nmmio_writes; i++) {
        printf("  MMIO write +%x: %016llx (%u bytes)\n", mmio_writes[i].off,
               (unsigned long long)mmio_writes[i].value, mmio_writes[i].size);
        if (mmio_writes[i].off == 0x10 && mmio_writes[i].size == 8) {
            got_lo = mmio_writes[i].value;
        } else if (mmio_writes[i].off == 0x18 && mmio_writes[i].size == 8) {
            got_hi = mmio_writes[i].value;
        }
    }
    check(got_lo == lo && got_hi == hi, "16-byte MMIO store: both halves reach the device");

    /* A device page read in the guest: a write reaches the device, the read
     * of it comes from the page without the device's handler, and a write
     * to the page's quiet register is dropped. */
    const uint8_t direct[] = {
        0xC7, 0x05, 0x20, 0x00, 0x10, 0xFE, 0x44, 0x33, 0x22, 0x11, /* mov dword [0xFE100020], 0x11223344 */
        0xA1, 0x20, 0x00, 0x10, 0xFE, 0xCD, 0x42,                   /* mov eax, [0xFE100020]; int 42h */
        0xC7, 0x05, 0x10, 0x00, 0x10, 0xFE, 0x00, 0x00, 0x01, 0x00, /* mov dword [0xFE100010], 0x10000 */
        0xA1, 0x10, 0x00, 0x10, 0xFE, 0xCD, 0x42,                   /* mov eax, [0xFE100010]; int 42h */
        0xF4,
    };
    const uint32_t quiet_was = 0x5A5A0000u;
    memcpy(direct_page + 0x10, &quiet_was, 4);
    put(CODE + 0xF00, direct, sizeof(direct));
    nreports = 0;
    direct_reads = direct_writes = 0;
    run_at(CODE + 0xF00);
    printf("  direct page: %u reports (%08x, %08x), handler reads %u, writes %u\n", nreports,
           nreports > 0 ? reports[0] : 0, nreports > 1 ? reports[1] : 0, direct_reads, direct_writes);
    check(nreports == 2 && reports[0] == 0x11223344u && reports[1] == quiet_was && direct_reads == 0 &&
          direct_writes == 1, "device page read in the guest: writes reach the device, a quiet register's don't");

    /* x87 mid-calculation through a savestate: fld1, then export and import
     * (int 45h), then fld1; faddp; fistp. TOP lost would make it garbage. */
    const uint8_t x87[] = {
        0xD9, 0xE8, 0xCD, 0x45,                   /* fld1; int 45h */
        0xD9, 0xE8, 0xDE, 0xC1,                   /* fld1; faddp st(1), st */
        0xDB, 0x1D, 0x20, 0x03, 0x02, 0x00,       /* fistp dword [0x20320] */
        0xA1, 0x20, 0x03, 0x02, 0x00, 0xCD, 0x42, /* mov eax, [0x20320]; int 42h */
        0xF4,
    };
    put(CODE + 0x980, x87, sizeof(x87));
    nreports = 0;
    run_at(CODE + 0x980);
    check(nreports == 1 && reports[0] == 2, "x87 state, TOP included, through a savestate's export and import");

    /* Scalar upper lanes, packed lanes and MXCSR through actual KVM exits. */
    const uint8_t rsqrt[] = {
        0x0F, 0x10, 0x05, 0x00, 0x03, 0x02, 0x00, /* movups xmm0, [0x20300] */
        0x0F, 0x10, 0x0D, 0x40, 0x03, 0x02, 0x00, /* movups xmm1, [0x20340] */
        0xF3, 0x0F, 0x52, 0xC1,                   /* rsqrtss xmm0, xmm1 */
        0x0F, 0x11, 0x05, 0x60, 0x03, 0x02, 0x00, /* movups [0x20360], xmm0 */
        0x0F, 0x52, 0xD1,                         /* rsqrtps xmm2, xmm1 */
        0x0F, 0x11, 0x15, 0x80, 0x03, 0x02, 0x00, /* movups [0x20380], xmm2 */
        0x0F, 0xAE, 0x1D, 0x90, 0x03, 0x02, 0x00, /* stmxcsr [0x20390] */
        0xF4,
    };
    const uint32_t inputs[] = {0x3F800000, 0x40800000, 0x3E800000, 0x41800000};
    const uint32_t expected[] = {0x3F7FF000, 0x3EFFF000, 0x3FFFF000, 0x3E7FF000};
    uint8_t scalar_expected[16];
    memcpy(scalar_expected, nc_vm_host(&core.vm, DATA + 0x300), 16);
    nc_wr32(scalar_expected, expected[0]);
    memcpy(nc_vm_host(&core.vm, DATA + 0x340), inputs, sizeof(inputs));
    put(CODE + 0xA00, rsqrt, sizeof(rsqrt));
    run_at(CODE + 0xA00);
    check(!memcmp(nc_vm_host(&core.vm, DATA + 0x360), scalar_expected, 16),
          "RSQRTSS: Intel result and unchanged upper lanes");
    check(!memcmp(nc_vm_host(&core.vm, DATA + 0x380), expected, sizeof(expected)),
          "RSQRTPS: all four Intel results");
    check(nc_vm_rd32(&core.vm, DATA + 0x390) == 0x1F80, "RSQRT leaves guest MXCSR unchanged");
    check(*(uint8_t *)nc_vm_host(&core.vm, CODE + 0xA0E) == (nc_rsqrt_host_amd() ? 0xCC : 0xF3),
          "RSQRT patching uses actual AMD host CPUID only");

    /* The same for RCP, its packed form on one register: 1.5 first, where
     * AMD's own result isn't Intel's. */
    const uint8_t rcp[] = {
        0x0F, 0x10, 0x05, 0x00, 0x03, 0x02, 0x00, /* movups xmm0, [0x20300] */
        0x0F, 0x10, 0x0D, 0xE0, 0x03, 0x02, 0x00, /* movups xmm1, [0x203E0] */
        0xF3, 0x0F, 0x53, 0xC1,                   /* rcpss xmm0, xmm1 */
        0x0F, 0x11, 0x05, 0xA0, 0x03, 0x02, 0x00, /* movups [0x203A0], xmm0 */
        0x0F, 0x53, 0xC9,                         /* rcpps xmm1, xmm1 */
        0x0F, 0x11, 0x0D, 0xC0, 0x03, 0x02, 0x00, /* movups [0x203C0], xmm1 */
        0x0F, 0xAE, 0x1D, 0xD0, 0x03, 0x02, 0x00, /* stmxcsr [0x203D0] */
        0xF4,
    };
    const uint32_t rcp_inputs[] = {0x3FC00000, 0x40400000, 0x3C23D70A, 0x41800000};
    uint32_t rcp_expected[4];
    for (unsigned i = 0; i < 4; i++) {
        /* an Intel host keeps its own results */
        rcp_expected[i] = nc_rsqrt_host_amd() ? nc_rcp_intel(rcp_inputs[i]) : nc_rcp_native(rcp_inputs[i]);
    }
    memcpy(nc_vm_host(&core.vm, DATA + 0x3E0), rcp_inputs, sizeof(rcp_inputs));
    check(nc_rcp_intel(rcp_inputs[0]) == 0x3F2AA000, "RCP: the Intel result of 1.5");
    nc_wr32(scalar_expected, rcp_expected[0]);
    put(CODE + 0xA40, rcp, sizeof(rcp));
    run_at(CODE + 0xA40);
    check(!memcmp(nc_vm_host(&core.vm, DATA + 0x3A0), scalar_expected, 16),
          "RCPSS: Intel result and unchanged upper lanes");
    check(!memcmp(nc_vm_host(&core.vm, DATA + 0x3C0), rcp_expected, sizeof(rcp_expected)),
          "RCPPS: all four Intel results with source and destination the same register");
    check(nc_vm_rd32(&core.vm, DATA + 0x3D0) == 0x1F80, "RCP leaves guest MXCSR unchanged");
    check(*(uint8_t *)nc_vm_host(&core.vm, CODE + 0xA4E) == (nc_rsqrt_host_amd() ? 0xCC : 0xF3),
          "RCP patching uses actual AMD host CPUID only");

    /* Check write faults after protection changes between vCPU entries and
     * while the guest runs. Verify that guest INT 2 reaches the system-op handler. */
    const uint8_t wp[] = {
        0xC7, 0x05, 0x00, 0x00, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00, /* mov dword [WP], 1 */
        0xCD, 0x46,                                                 /* int 46h: protect WP */
        0xC7, 0x05, 0x00, 0x00, 0x04, 0x00, 0x02, 0x00, 0x00, 0x00, /* mov dword [WP], 2 */
        0xA1, 0x00, 0x00, 0x04, 0x00, 0xCD, 0x42,                   /* mov eax, [WP]; int 42h */
        0xFF, 0x05, 0x00, 0x00, 0x04, 0x00,                         /* again: inc dword [WP] */
        0x83, 0x3D, 0x04, 0x02, 0x02, 0x00, 0x00,                   /* cmp dword [WPFLAG], 0 */
        0x74, 0xF1,                                                 /* je again */
        0xCD, 0x02,                                                 /* int 2 */
        0xF4,
    };
    nreports = 0;
    nops = 0;
    nc_wr32(nc_vm_host(&core.vm, WPFLAG), 0);
    put(CODE + 0x1000, wp, sizeof(wp));
    pthread_t wp_th;
    pthread_create(&wp_th, NULL, protect_later, NULL);
    run_at(CODE + 0x1000);
    pthread_join(wp_th, NULL);
    check(nreports == 1 && reports[0] == 2 && wp_faults >= 1, "a page protected between entries: the store faults");
    check(wp_faults == 2 && wp_v0 == wp_v1 && nc_vm_rd32(&core.vm, WP) == wp_v0 + 1,
          "a page protected under the running guest: no store after it");
    check(saw(NC_SYSOP_INT, 2, CODE + 0x1000 + sizeof(wp) - 3), "INT 2: the title's, a system op");

    /* Real OHCI, not the synthetic direct device above. The guest must see
     * the controller's registers without invoking an MMIO read handler.
     * HcInterruptEnable writes must still reach the emulated controller. */
    const uint8_t ohci_setup[] = {
        0xA1, 0x0C, 0x00, 0xD0, 0xFE, 0xCD, 0x42, /* read HcInterruptStatus */
        0xA1, 0x10, 0x00, 0xD0, 0xFE, 0xCD, 0x42, /* read HcInterruptEnable */
        0xC7, 0x05, 0x10, 0x00, 0xD0, 0xFE, 0x40, 0x00, 0x00, 0x80, /* enable RHSC + MIE */
        0xA1, 0x10, 0x00, 0xD0, 0xFE, 0xCD, 0x42, /* read the changed enable mask */
        0xF4,
    };
    put(CODE + 0xB00, ohci_setup, sizeof(ohci_setup));
    nreports = 0;
    uint64_t ohci_mmio_before = nc_mmio_total();
    run_at(CODE + 0xB00);
    /* The controller starts with player 1's pad attached: the port's connect
     * change is pending in HcInterruptStatus from the start, not yet enabled. */
    check(nreports == 3 && reports[0] == OHCI_RHSC && reports[1] == OHCI_MIE &&
          reports[2] == (OHCI_MIE | OHCI_RHSC),
          "OHCI: guest reads the initial and written interrupt registers");
    check(nc_mmio_total() - ohci_mmio_before == 1,
          "OHCI: only the register write exits; three guest reads stay in KVM");

    /* Unplug player 1 while the controller's root-hub interrupt is enabled.
     * OHCI publishes the register page before raising IRQ 1. Deliver that
     * interrupt to the vCPU, read status in guest code, and acknowledge it. */
    nc_irq_attach(cpu);
    nc_irq_enable(OHCI_IRQ, true);
    nc_pad_plug(0, false);
    check((nc_irq_lines() & (1u << OHCI_IRQ)) != 0, "OHCI: hotplug asserts IRQ 1");
    const uint8_t ohci_irq[] = {
        0xA1, 0x0C, 0x00, 0xD0, 0xFE, 0xCD, 0x42, /* status: RHSC */
        0xA1, 0x10, 0x00, 0xD0, 0xFE, 0xCD, 0x42, /* enable: RHSC + MIE */
        0xA1, 0x5C, 0x00, 0xD0, 0xFE, 0xCD, 0x42, /* player 1 root port: unplugged */
        0xC7, 0x05, 0x0C, 0x00, 0xD0, 0xFE, 0x40, 0x00, 0x00, 0x00, /* acknowledge RHSC */
        0xA1, 0x0C, 0x00, 0xD0, 0xFE, 0xCD, 0x42, /* status: cleared */
        0xF4,
    };
    put(CODE + 0xB80, ohci_irq, sizeof(ohci_irq));
    nreports = 0;
    irqs = 0;
    ohci_mmio_before = nc_mmio_total();
    run_at(CODE + 0xB80);
    check(irqs == 1 && nreports == 4 && reports[0] == OHCI_RHSC &&
          reports[1] == (OHCI_MIE | OHCI_RHSC) &&
          (reports[2] & (1u << 16)) && !(reports[2] & 1u) && reports[3] == 0 &&
          !(nc_irq_lines() & (1u << OHCI_IRQ)),
          "OHCI: KVM delivers IRQ with current status; guest acknowledgement lowers it");
    check(nc_mmio_total() - ohci_mmio_before == 1,
          "OHCI: interrupt status and port reads stay in KVM; acknowledge exits");

    /* Replug with delivery masked; guest code must still see the new port and
     * interrupt status directly, without another userspace MMIO read. */
    nc_irq_enable(OHCI_IRQ, false);
    nc_pad_plug(0, true);
    const uint8_t ohci_hotplug[] = {
        0xA1, 0x5C, 0x00, 0xD0, 0xFE, 0xCD, 0x42, /* player 1 root port */
        0xA1, 0x0C, 0x00, 0xD0, 0xFE, 0xCD, 0x42, /* RHSC */
        0xF4,
    };
    put(CODE + 0xC00, ohci_hotplug, sizeof(ohci_hotplug));
    nreports = 0;
    ohci_mmio_before = nc_mmio_total();
    run_at(CODE + 0xC00);
    check(nreports == 2 && (reports[0] & 1u) && (reports[0] & (1u << 16)) &&
          reports[1] == OHCI_RHSC,
          "OHCI: a replug updates the guest-visible port and interrupt status");
    check(nc_mmio_total() == ohci_mmio_before, "OHCI: guest hotplug reads cause no MMIO exits");
    nc_irq_reset();
    nc_dev_ohci_free();

    nc_cpu_destroy(cpu);
    check(*(uint8_t *)nc_vm_host(&core.vm, CODE + 0xA0E) == 0xF3 &&
          *(uint8_t *)nc_vm_host(&core.vm, CODE + 0xA19) == 0x0F,
          "RSQRT original bytes restored on CPU destruction");
    check(*(uint8_t *)nc_vm_host(&core.vm, CODE + 0xA4E) == 0xF3 &&
          *(uint8_t *)nc_vm_host(&core.vm, CODE + 0xA59) == 0x0F,
          "RCP original bytes restored on CPU destruction");
    struct sigaction kick;
    sigaction(SIGRTMIN + 3, NULL, &kick);
    check(kick.sa_handler == SIG_DFL, "the kick signal's handler given back on destroy");
    printf("%s: %u failed\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
