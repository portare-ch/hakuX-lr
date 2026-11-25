/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exercise the real patch/trap code against synthetic guest RAM, without
 * /dev/kvm. Unused KVM entry points are discarded by --gc-sections. */
#include "native/cpu/cpu_kvm.c"

void (*nc_vm_write_hook)(uint32_t va, uint32_t len);
void (*nc_vm_read_hook)(uint32_t va, uint32_t len);

void nc_log(const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

#define CODE 0x10000u
#define DATA 0x20000u
static unsigned failures, faults;

static void check(bool ok, const char *what)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    failures += !ok;
}

static int fault(void *opaque, NcFrame *f, uint32_t va, bool write, const char **why)
{
    NcVm *vm = opaque;
    faults++;
    vm->pte[va / NC_PAGE] = NC_PAGE | NC_PTE_VALID;
    vm->vprot[va / NC_PAGE] = PROT_READ;
    return 1;
}

int main(void)
{
    NcCpu *cpu = calloc(1, sizeof(*cpu));
    NcVm vm = {.ram_size = 4 * NC_PAGE};
    vm.pte = calloc(1u << 20, sizeof(*vm.pte));
    vm.vprot = calloc(1u << 20, 1);
    vm.phys = calloc(1, vm.ram_size);
    if (!cpu || !vm.pte || !vm.vprot || !vm.phys) {
        return 1;
    }
    cpu->vm = &vm;
    cpu->cfg.opaque = &vm;
    cpu->cfg.fault = fault;
    cpu->frame.cpu = cpu;
    vm.pte[CODE / NC_PAGE] = NC_PTE_VALID;
    vm.vprot[CODE / NC_PAGE] = PROT_READ | PROT_WRITE;
    vm.pte[DATA / NC_PAGE] = NC_PAGE | NC_PTE_VALID;
    vm.vprot[DATA / NC_PAGE] = PROT_READ;

    setenv("NC_KVM_RSQRT_SITES", "10000,10040", 1);
    check(rsqrt_config(cpu) && cpu->nrsqrt == (nc_rsqrt_host_amd() ? 2u : 0u),
          "environment selection applies only on actual AMD hosts");
    unsetenv("NC_KVM_RSQRT_SITES");
    cpu->rsqrt_on = true;
    /* Exercise the handler on Intel CI as well, without adding any
     * production override to the AMD vendor gate. */
    if (!cpu->rsqrt_sites) {
        cpu->rsqrt_sites = calloc(2, sizeof(*cpu->rsqrt_sites));
    }
    cpu->nrsqrt = 2;
    /* the guest's site table, as its #BP handler searches it */
    cpu->sys = calloc(1, SYS_SIZE);
    if (!cpu->sys) {
        return 1;
    }
    cpu->rsqrt_inguest = true;
    const uint8_t *guest = lin(cpu, LIN_RSQ_SITES);
    cpu->rsqrt_sites[0].eip = CODE;
    cpu->rsqrt_sites[1].eip = CODE + 0x40;
    cpu->frame.fpu_valid = true;
    cpu->frame.fpu.mxcsr = 0x5F95;
    cpu->frame.eflags = 0x246;
    memset(cpu->frame.fpu.xmm, 0xA5, sizeof(cpu->frame.fpu.xmm));
    const uint32_t inputs[] = {0x3F800000, 0x40800000, 0x3E800000, 0x41800000};
    const uint32_t expected[] = {0x3F7FF000, 0x3EFFF000, 0x3FFFF000, 0x3E7FF000};
    memcpy(cpu->frame.fpu.xmm[1], inputs, 16);
    const uint8_t scalar[] = {0xF3, 0x0F, 0x52, 0xC1};
    memcpy(vm.phys, scalar, sizeof(scalar));
    memcpy(vm.phys + 0x20, scalar, sizeof(scalar)); /* unselected copy */
    const uint8_t packed[] = {0x0F, 0x52, 0xD1};
    memcpy(vm.phys + 0x40, packed, sizeof(packed));
    struct kvm_fpu before = cpu->frame.fpu;
    rsqrt_patch(cpu, CODE, NC_PAGE);
    check(vm.phys[0] == 0xCC && vm.phys[0x40] == 0xCC && vm.phys[0x20] == 0xF3,
          "only explicitly selected instruction addresses patched");
    check(nc_rd32(guest) == 2 && nc_rd32(guest + 4) == CODE &&
          nc_rd32(guest + 8) == (rsqrt_rtn(true, 0, 1) | 4u << 16) &&
          nc_rd32(guest + 12) == CODE + 0x40 &&
          nc_rd32(guest + 16) == (rsqrt_rtn(false, 2, 1) | 3u << 16),
          "the guest's table holds both register forms, ascending, each with its form's routine and length");
    check(rsqrt_trap(cpu, CODE) && cpu->frame.rip == CODE + sizeof(scalar),
          "scalar trap advances past the original instruction");
    memcpy(before.xmm[0], expected, 4);
    check(!memcmp(&before, &cpu->frame.fpu, sizeof(before)) && cpu->frame.eflags == 0x246,
          "scalar correction preserves upper lanes, other registers, MXCSR and EFLAGS");
    check(rsqrt_trap(cpu, CODE + 0x40), "packed register trap handled");
    memcpy(before.xmm[2], expected, 16);
    check(!memcmp(&before, &cpu->frame.fpu, sizeof(before)), "all four packed lanes corrected");
    check(!rsqrt_trap(cpu, CODE + 0x20), "unselected INT3 is not consumed");

    rsqrt_restore(cpu);
    check(!memcmp(vm.phys, scalar, sizeof(scalar)) && !memcmp(vm.phys + 0x40, packed, sizeof(packed)),
          "original instruction bytes restored");
    check(nc_rd32(guest) == 0, "restored sites leave the guest's table");
    /* Memory form, including a source page fault and retry with no
     * destination state committed before the memory becomes readable. */
    const uint8_t memory[] = {0xF3, 0x0F, 0x52, 0x05, 0, 0, 2, 0};
    memcpy(vm.phys, memory, sizeof(memory));
    memcpy(vm.phys + NC_PAGE, inputs, 16);
    vm.pte[DATA / NC_PAGE] = 0;
    vm.vprot[DATA / NC_PAGE] = 0;
    before = cpu->frame.fpu;
    rsqrt_patch(cpu, CODE, sizeof(memory));
    const uint8_t *stub = lin(cpu, LIN_END - RSQ_STUB);
    check(cpu->rsqrt_sites[0].armed && nc_rd32(guest) == 1 && nc_rd32(guest + 4) == CODE &&
          nc_rd32(guest + 8) == ((LIN_END - RSQ_STUB) | 8u << 16) && stub[0] == 0xB8 && nc_rd32(stub + 1) == DATA &&
          stub[5] == 0xB9 && nc_rd32(stub + 6) == (0x88u | 1u << 13) && stub[10] == 0xE9 &&
          LIN_END - 1 + nc_rd32(stub + 11) == LIN_RSQ_MEM,
          "a memory form is in the guest's table with a stub: its displacement, modes and the jump");
    /* the operands the guest takes, and those it leaves to the exit */
    static const struct { uint8_t len, bytes[9]; uint32_t disp, modes; } ops[] = {
        {3, {0x0F, 0x52, 0x00}, 0, 0x80},                                              /* xmm0, [eax] */
        {5, {0xF3, 0x0F, 0x52, 0x4B, 0x10}, 0x10, 0x83 | 1 << 10 | 1 << 13},           /* xmm1, [ebx+16] */
        {5, {0x0F, 0x52, 0x54, 0x84, 0xF8}, 0xFFFFFFF8u, 0x04 | 2 << 8 | 2 << 10},     /* xmm2, [esp+eax*4-8] */
        {4, {0x0F, 0x53, 0x7D, 0x00}, 0, 0x85 | 7 << 10},                              /* xmm7, [ebp] */
        {8, {0x0F, 0x52, 0x04, 0xF5, 0x78, 0x56, 0x34, 0x12}, 0x12345678, 0x68 | 3 << 8}, /* xmm0, [esi*8+disp] */
        {8, {0xF3, 0x0F, 0x53, 0x9C, 0x24, 0x00, 0x01, 0x00, 0x00}, 0, 0},             /* 9 bytes: below */
        {8, {0x0F, 0x52, 0xAC, 0x39, 0x00, 0x00, 0x01, 0x00}, 0x10000, 0x71 | 5 << 10}, /* xmm5, [ecx+edi+0x10000] */
        {4, {0x64, 0x0F, 0x52, 0x00}, 0, 0},                                           /* fs: the exit's */
        {4, {0x67, 0x0F, 0x52, 0x04}, 0, 0},                                           /* 16-bit address: the exit's */
    };
    bool operands = true;
    for (unsigned i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
        RsqrtSite t = {.mem = true};
        NcRsqrtInsn in;
        const uint32_t regs[8] = {0};
        t.len = i == 5 ? 9 : ops[i].len;
        memcpy(t.bytes, ops[i].bytes, 9);
        if (!nc_rsqrt_decode(t.bytes, t.len, regs, 0, &in) || !in.mem || in.len != t.len) {
            operands = false;
            continue;
        }
        t.dst = (uint8_t)in.dst;
        t.scalar = in.scalar;
        uint32_t disp = 0, modes = 0;
        const bool taken = rsqrt_mem_operand(&t, &disp, &modes);
        if (i == 5) {
            operands &= taken && disp == 0x100 && modes == (0x84u | 3 << 10 | 1 << 13); /* xmm3, [esp+256] */
        } else if (i >= 7) {
            operands &= !taken;
        } else {
            operands &= taken && disp == ops[i].disp && modes == ops[i].modes;
        }
    }
    check(operands, "memory operands: base, index, scale and displacement, none for a prefixed address");
    check(rsqrt_trap(cpu, CODE) && faults == 1 && cpu->frame.rip == CODE &&
          !memcmp(&before, &cpu->frame.fpu, sizeof(before)), "unmapped source faults without committing SSE state");
    check(rsqrt_trap(cpu, CODE) && cpu->frame.rip == CODE + sizeof(memory), "memory source retried after mapping");
    check(nc_rd32(cpu->frame.fpu.xmm[0]) == expected[0], "scalar memory source uses corrected result");

    /* Replace the opcode tail while our first-byte trap is installed. */
    vm.phys[2] = 0x51;
    rsqrt_patch(cpu, CODE + 2, 1);
    check(vm.phys[0] == 0xF3 && vm.phys[2] == 0x51 && !cpu->rsqrt_sites[0].armed,
          "code invalidation removes a stale patch without overwriting changed bytes");
    memcpy(vm.phys, scalar, sizeof(scalar));
    rsqrt_patch(cpu, CODE, sizeof(scalar));
    check(cpu->rsqrt_sites[0].armed, "replacement RSQRT can be patched again");
    rsqrt_restore(cpu);

    /* The contiguous allocator relocates a patched page, copying it: the
     * copy's INT3 is still the site's, the old copy loses it. */
    rsqrt_patch(cpu, CODE, sizeof(scalar));
    memcpy(vm.phys + 2 * NC_PAGE, vm.phys, NC_PAGE);
    vm.pte[CODE / NC_PAGE] = 2 * NC_PAGE | NC_PTE_VALID;
    check(rsqrt_trap(cpu, CODE) && cpu->frame.rip == CODE + sizeof(scalar),
          "a relocated page's INT3 is handled as the site's");
    check(vm.phys[0] == 0xF3, "the relocated page's old copy loses the INT3");
    rsqrt_restore(cpu);
    check(vm.phys[2 * NC_PAGE] == 0xF3, "the relocated patch is restored on teardown");
    vm.pte[CODE / NC_PAGE] = NC_PTE_VALID;

    /* An unmap removes the mapping before the invalidation reaches the
     * backend: the patch is removed at its physical page, and the page
     * mapped back is patched again. */
    rsqrt_patch(cpu, CODE, sizeof(scalar));
    vm.pte[CODE / NC_PAGE] = 0;
    vm.vprot[CODE / NC_PAGE] = 0;
    rsqrt_patch(cpu, CODE, NC_PAGE);
    check(vm.phys[0] == 0xF3 && !cpu->rsqrt_sites[0].armed, "an unmapped site's INT3 is removed at its physical page");
    vm.pte[CODE / NC_PAGE] = NC_PTE_VALID;
    vm.vprot[CODE / NC_PAGE] = PROT_READ | PROT_WRITE;
    rsqrt_patch(cpu, CODE, NC_PAGE);
    check(cpu->rsqrt_sites[0].armed && vm.phys[0] == 0xCC, "the page mapped back is patched again");
    check(rsqrt_trap(cpu, CODE) && cpu->frame.rip == CODE + sizeof(scalar), "and its trap is handled");
    rsqrt_restore(cpu);
    cpu->nhooks = 1;
    cpu->hooks[0].eip = CODE;
    hooks_patch(cpu, CODE, 1);
    rsqrt_patch(cpu, CODE, sizeof(scalar));
    check(vm.phys[0] == 0xCC && !cpu->rsqrt_sites[0].armed, "existing HLE hook retains ownership of its address");

    /* RCPSS/RCPPS: register forms only. */
    cpu->nhooks = 0;
    const uint8_t rcpss[] = {0xF3, 0x0F, 0x53, 0xC1}, rcpps[] = {0x0F, 0x53, 0xD2}; /* xmm0, xmm1; xmm2, xmm2 */
    const uint32_t rcp_expected[] = {0x3F7FF000, 0x3E7FF000, 0x407FF000, 0x3D7FF000};
    memcpy(vm.phys, rcpss, sizeof(rcpss));
    memcpy(vm.phys + 0x40, rcpps, sizeof(rcpps));
    rsqrt_patch(cpu, CODE, NC_PAGE);
    check(vm.phys[0] == 0xCC && vm.phys[0x40] == 0xCC && cpu->reg_armed[1] == 2 && nc_rd32(guest) == 2 &&
          nc_rd32(guest + 8) == (rsqrt_rtn(true, 0, 1) | 4u << 16 | 1u << 24) &&
          nc_rd32(guest + 16) == (rsqrt_rtn(false, 2, 2) | 3u << 16 | 1u << 24),
          "RCP register forms are in the guest's table as family 1");
    memset(cpu->frame.fpu.xmm, 0xA5, sizeof(cpu->frame.fpu.xmm));
    memcpy(cpu->frame.fpu.xmm[1], inputs, 16);
    memcpy(cpu->frame.fpu.xmm[2], inputs, 16);
    before = cpu->frame.fpu;
    check(rsqrt_trap(cpu, CODE) && cpu->frame.rip == CODE + sizeof(rcpss), "RCPSS trap advances past the instruction");
    memcpy(before.xmm[0], rcp_expected, 4);
    check(!memcmp(&before, &cpu->frame.fpu, sizeof(before)) && cpu->frame.eflags == 0x246,
          "RCPSS preserves upper lanes, other registers, MXCSR and EFLAGS");
    check(rsqrt_trap(cpu, CODE + 0x40), "RCPPS trap handled");
    memcpy(before.xmm[2], rcp_expected, 16);
    check(!memcmp(&before, &cpu->frame.fpu, sizeof(before)) && cpu->rcp_hits == 2,
          "all four lanes of an aliased RCPPS corrected");
    memcpy(vm.phys + 0x40, packed, sizeof(packed));
    rsqrt_patch(cpu, CODE + 0x40, sizeof(packed));
    check(cpu->reg_armed[1] == 1 && cpu->reg_armed[0] == 1 && nc_rd32(guest) == 2,
          "an RSQRT site replaces the RCP site at its address");
    rsqrt_restore(cpu);
    check(vm.phys[0] == 0xF3 && cpu->reg_armed[1] == 0 && cpu->reg_armed[0] == 0, "RCP patches are restored");
    const uint8_t rcp_memory[] = {0xF3, 0x0F, 0x53, 0x05, 0, 0, 2, 0};
    memcpy(vm.phys, rcp_memory, sizeof(rcp_memory));
    rsqrt_patch(cpu, CODE, sizeof(rcp_memory));
    rsqrt_patch(cpu, CODE, sizeof(rcp_memory));
    check(vm.phys[0] == 0xCC && cpu->rsqrt_sites[0].armed && nc_rd32(guest) == 1 &&
          nc_rd32(guest + 8) == ((LIN_END - RSQ_STUB) | 8u << 16 | 1u << 24),
          "an RCP memory form is in the guest's table as family 1");
    memset(cpu->frame.fpu.xmm, 0xA5, sizeof(cpu->frame.fpu.xmm));
    check(rsqrt_trap(cpu, CODE) && cpu->frame.rip == CODE + sizeof(rcp_memory) &&
          nc_rd32(cpu->frame.fpu.xmm[0]) == rcp_expected[0] && nc_rd32(cpu->frame.fpu.xmm[0] + 4) == 0xA5A5A5A5u,
          "and corrected through the host as well");
    /* A fault on the source inside the guest's routine: the title's state
     * at the site back from the ring-0 stack, the stub's frame the trap's. */
    const uint32_t ring0[] = {14, 0, LIN_RSQ_MRTN + 8 * 64 + 7, SEL_CODE0, 0x3006, 1, 2, 3, 4, LIN_RSQ_MEM + 67,
                              0, 0x77, 0x66, 0x55, DATA + 0x900, 0x33, 0x22, 0x11, 0x00C0FFEE,
                              CODE + sizeof(rcp_memory), SEL_CODE3, 0x3247, DATA + 0x900, SEL_DATA3};
    memcpy(lin(cpu, LIN_STACK - sizeof(ring0)), ring0, sizeof(ring0));
    for (int r = 0; r < 8; r++) {
        nc_frame_set_gpr(&cpu->frame, r, 0xDEAD0000u + (unsigned)r);
    }
    cpu->stub_esp = LIN_STACK - sizeof(ring0);
    cpu->frame.rip = ring0[2];
    cpu->frame.eflags = ring0[4];
    nc_wr32(lin(cpu, LIN_RSQ_HITS + 8), 5);
    rsqrt_mem_fault(cpu);
    check(cpu->frame.rip == CODE && cpu->frame.eflags == 0x3247 && cpu->stub_esp == LIN_STACK - 28 &&
          nc_frame_gpr(&cpu->frame, NC_EAX) == 0x00C0FFEE && nc_frame_gpr(&cpu->frame, NC_ECX) == 0x11 &&
          nc_frame_gpr(&cpu->frame, NC_EDX) == 0x22 && nc_frame_gpr(&cpu->frame, NC_EBX) == 0x33 &&
          nc_frame_gpr(&cpu->frame, NC_ESP) == DATA + 0x900 && nc_frame_gpr(&cpu->frame, NC_EBP) == 0x55 &&
          nc_frame_gpr(&cpu->frame, NC_ESI) == 0x66 && nc_frame_gpr(&cpu->frame, NC_EDI) == 0x77 &&
          nc_rd32(lin(cpu, LIN_RSQ_HITS + 8)) == 4,
          "a fault in the guest's memory routine: the title's state at the site, its count taken back");
    cpu->frame.eflags = 0x246;
    rsqrt_restore(cpu);

    /* The guest's table at its limit: a site every four bytes of three pages,
     * RSQRT but for the RCP at either end. */
    enum { FULL = RSQ_GUEST_MAX, LAST = FULL + 1 };
    free(cpu->rsqrt_sites);
    cpu->rsqrt_sites = calloc(LAST + 1, sizeof(*cpu->rsqrt_sites));
    if (!cpu->rsqrt_sites || (LAST + 1) * 4 > 3 * NC_PAGE) {
        return 1;
    }
    cpu->nrsqrt = LAST + 1;
    for (unsigned i = 1; i <= 2; i++) {
        vm.pte[CODE / NC_PAGE + i] = (i + 1) * NC_PAGE | NC_PTE_VALID;
        vm.vprot[CODE / NC_PAGE + i] = PROT_READ | PROT_WRITE;
    }
#define SITE(i) (vm.phys + ((i) * 4u < NC_PAGE ? (i) * 4u : NC_PAGE + (i) * 4u))
    for (unsigned i = 0; i <= LAST; i++) {
        cpu->rsqrt_sites[i].eip = CODE + i * 4;
        memcpy(SITE(i), i == LAST ? rcpss : scalar, 4);
    }
    /* RSQRT fills the table, the RCP after it finds no room */
    memset(SITE(0), 0x90, 4);
    rsqrt_patch(cpu, CODE + 4, FULL * 4);
    rsqrt_patch(cpu, CODE + LAST * 4, 4);
    bool all_rsqrt = nc_rd32(guest) == FULL;
    for (unsigned i = 0; i < FULL; i++) {
        all_rsqrt &= nc_rd32(guest + 4 + 8 * i) == CODE + (i + 1) * 4 && !(nc_rd32(guest + 8 + 8 * i) >> 24);
    }
    check(all_rsqrt && cpu->reg_armed[0] == FULL && cpu->reg_armed[1] == 0 && *SITE(LAST) == 0xF3 &&
          cpu->rcp_left_full == 1, "a table full of RSQRT sites keeps them all: the RCP site is left, and counted");
    rsqrt_patch(cpu, CODE + LAST * 4, 4);
    check(cpu->rcp_left_full == 1, "once");
    rsqrt_restore(cpu);
    /* an RCP armed while there was room gives its place to the last RSQRT */
    memcpy(SITE(0), rcpss, 4);
    rsqrt_patch(cpu, CODE, FULL * 4);
    check(*SITE(0) == 0xCC && cpu->reg_armed[1] == 1 && cpu->reg_armed[0] == FULL - 1 && nc_rd32(guest) == FULL &&
          nc_rd32(guest + 4) == CODE && nc_rd32(guest + 8) >> 24 == 1, "an RCP site takes the room RSQRT leaves");
    rsqrt_patch(cpu, CODE + FULL * 4, 4);
    all_rsqrt = nc_rd32(guest) == FULL;
    for (unsigned i = 0; i < FULL; i++) {
        all_rsqrt &= nc_rd32(guest + 4 + 8 * i) == CODE + (i + 1) * 4 && !(nc_rd32(guest + 8 + 8 * i) >> 24);
    }
    check(all_rsqrt && *SITE(0) == 0xF3 && !cpu->rsqrt_sites[0].armed && cpu->reg_armed[1] == 0 &&
          cpu->rcp_left_full == 2, "and gives it up to an RSQRT site armed later, counted");
    /* past the table RSQRT keeps the exit, as before */
    memcpy(SITE(LAST), scalar, 4);
    rsqrt_patch(cpu, CODE + LAST * 4, 4);
    check(*SITE(LAST) == 0xCC && cpu->rsqrt_sites[LAST].armed && nc_rd32(guest) == FULL &&
          cpu->reg_armed[0] == FULL + 1, "an RSQRT site past the table keeps the host exit");
    rsqrt_restore(cpu);
    rsqrt_patch(cpu, CODE, 4);
    check(*SITE(0) == 0xCC && cpu->reg_armed[1] == 1 && nc_rd32(guest) == 1, "an RCP site left is armed once there is room");
    rsqrt_restore(cpu);
    /* memory forms take the room register forms leave, an entry and a stub each */
    const uint8_t mem4[] = {0xF3, 0x0F, 0x52, 0x00}; /* xmm0, [eax] */
    memcpy(SITE(0), scalar, 4);
    memcpy(SITE(LAST), mem4, 4);
    rsqrt_patch(cpu, CODE, FULL * 4);
    rsqrt_patch(cpu, CODE + LAST * 4, 4);
    check(nc_rd32(guest) == FULL && cpu->rsqrt_sites[LAST].armed && *SITE(LAST) == 0xCC,
          "a memory form keeps the exit when register forms fill the table");
    rsqrt_restore(cpu);
    for (unsigned i = 0; i <= LAST; i++) {
        memcpy(SITE(i), mem4, 4);
    }
    rsqrt_patch(cpu, CODE, (LAST + 1) * 4);
    const uint32_t held = nc_rd32(guest);
    check(held == FULL / 3 && LIN_RSQ_SITES + 4 + 8 * held <= LIN_END - RSQ_STUB * held &&
          (nc_rd32(guest + 8) & 0xFFFF) == LIN_END - RSQ_STUB &&
          (nc_rd32(guest + 8 * held) & 0xFFFF) == LIN_END - RSQ_STUB * held && cpu->rsqrt_sites[LAST].armed,
          "memory forms fill the table and their stubs meet it; the rest keep the exit");
    rsqrt_restore(cpu);
#undef SITE

    free(cpu->rsqrt_sites);
    free(cpu->sys);
    free(vm.phys);
    free(vm.vprot);
    free(vm.pte);
    free(cpu);
    return failures ? 1 : 0;
}
