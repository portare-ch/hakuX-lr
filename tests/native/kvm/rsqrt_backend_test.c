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
          nc_rd32(guest + 8) == ((LIN_RSQ_RTN + (64 + 1) * RSQ_RTN_SIZE) | 4u << 16) &&
          nc_rd32(guest + 12) == CODE + 0x40 &&
          nc_rd32(guest + 16) == ((LIN_RSQ_RTN + (2 * 8 + 1) * RSQ_RTN_SIZE) | 3u << 16),
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
    check(cpu->rsqrt_sites[0].armed && nc_rd32(guest) == 0, "a memory form stays out of the guest's table");
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

    /* RCPSS/RCPPS: an opt-in (NC_KVM_RCP), register forms only. */
    cpu->nhooks = 0;
    const uint8_t rcpss[] = {0xF3, 0x0F, 0x53, 0xC1}, rcpps[] = {0x0F, 0x53, 0xD2}; /* xmm0, xmm1; xmm2, xmm2 */
    const uint32_t rcp_expected[] = {0x3F7FF000, 0x3E7FF000, 0x407FF000, 0x3D7FF000};
    memcpy(vm.phys, rcpss, sizeof(rcpss));
    memcpy(vm.phys + 0x40, rcpps, sizeof(rcpps));
    rsqrt_patch(cpu, CODE, NC_PAGE);
    check(vm.phys[0] == 0xF3 && vm.phys[0x40] == 0x0F, "RCP sites stay the host CPU's without the opt-in");
    cpu->rcp_on = true;
    rsqrt_patch(cpu, CODE, NC_PAGE);
    check(vm.phys[0] == 0xCC && vm.phys[0x40] == 0xCC && cpu->rcp_armed == 2 && nc_rd32(guest) == 2 &&
          nc_rd32(guest + 8) == ((LIN_RSQ_RTN + (64 + 1) * RSQ_RTN_SIZE) | 4u << 16 | 1u << 24) &&
          nc_rd32(guest + 16) == ((LIN_RSQ_RTN + (2 * 8 + 2) * RSQ_RTN_SIZE) | 3u << 16 | 1u << 24),
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
    /* the table has no room: RSQRT sites keep the exit, RCP sites aren't armed */
    memcpy(vm.phys + 0x40, packed, sizeof(packed));
    rsqrt_patch(cpu, CODE + 0x40, sizeof(packed));
    check(cpu->rcp_armed == 1 && nc_rd32(guest) == 2, "an RSQRT site replaces the RCP site at its address");
    cpu->rcp_armed = RSQ_GUEST_MAX;
    cpu->rsqrt_guest_stale = true;
    rsqrt_guest_sync(cpu);
    check(nc_rd32(guest) == 1 && nc_rd32(guest + 4) == CODE && cpu->rsqrt_sites[1].armed,
          "with the table full of RCP sites an RSQRT site keeps the host exit");
    cpu->rcp_armed = 1;
    rsqrt_restore(cpu);
    check(vm.phys[0] == 0xF3 && cpu->rcp_armed == 0, "RCP patches are restored");
    cpu->rcp_armed = RSQ_GUEST_MAX;
    rsqrt_patch(cpu, CODE, sizeof(rcpss));
    check(vm.phys[0] == 0xF3 && cpu->rcp_left_full == 1, "an RCP site past the table's room stays the host CPU's");
    cpu->rcp_armed = 0;
    rsqrt_patch(cpu, CODE, sizeof(rcpss));
    check(vm.phys[0] == 0xCC && cpu->rcp_armed == 1, "and is armed once there is room");
    rsqrt_restore(cpu);
    const uint8_t rcp_memory[] = {0xF3, 0x0F, 0x53, 0x05, 0, 0, 2, 0};
    memcpy(vm.phys, rcp_memory, sizeof(rcp_memory));
    rsqrt_patch(cpu, CODE, sizeof(rcp_memory));
    rsqrt_patch(cpu, CODE, sizeof(rcp_memory));
    check(vm.phys[0] == 0xF3 && !cpu->rsqrt_sites[0].armed && cpu->rcp_left_mem == 1 && nc_rd32(guest) == 0,
          "an RCP memory form stays the host CPU's, counted once");
    rsqrt_restore(cpu);

    free(cpu->rsqrt_sites);
    free(cpu->sys);
    free(vm.phys);
    free(vm.vprot);
    free(vm.pte);
    free(cpu);
    return failures ? 1 : 0;
}
