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
    if (!cpu->rsqrt.index && !nc_rsqrt_init(&cpu->rsqrt)) {
        return 1;
    }
    /* Exercise the handler on Intel CI as well, without adding any
     * production override to the AMD vendor gate. */
    cpu->nrsqrt = 2;
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
    /* Memory form, including a source page fault and retry with no
     * destination state committed before the memory becomes readable. */
    const uint8_t memory[] = {0xF3, 0x0F, 0x52, 0x05, 0, 0, 2, 0};
    memcpy(vm.phys, memory, sizeof(memory));
    memcpy(vm.phys + NC_PAGE, inputs, 16);
    vm.pte[DATA / NC_PAGE] = 0;
    vm.vprot[DATA / NC_PAGE] = 0;
    before = cpu->frame.fpu;
    rsqrt_patch(cpu, CODE, sizeof(memory));
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

    nc_rsqrt_destroy(&cpu->rsqrt);
    free(vm.phys);
    free(vm.vprot);
    free(vm.pte);
    free(cpu);
    return failures ? 1 : 0;
}
