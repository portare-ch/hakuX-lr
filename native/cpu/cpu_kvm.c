/*
 * hakuX native core — the guest CPU on x86-64 Linux through KVM (#56).
 *
 * Not written yet: the x86-64 build has this placeholder, which refuses to
 * create a CPU, so everything that doesn't run guest code builds and is
 * tested on x86-64 before the backend exists.
 */
#include "native/cpu/cpu.h"

NcCpu *nc_cpu_create(const NcCpuConfig *cfg, uint32_t fs_base)
{
    nc_log("hakuX-native", "cpu: the KVM backend isn't there yet (#56): no guest CPU on x86-64");
    return NULL;
}

void nc_cpu_destroy(NcCpu *cpu) {}
void nc_cpu_thread_done(void) {}

void nc_context_init(NcCpu *cpu, NcCpuContext *c, uint32_t eip, uint32_t esp) {}
void nc_context_set_gpr(NcCpuContext *c, int reg, uint32_t v) {}
uint32_t nc_context_gpr(const NcCpuContext *c, int reg) { return 0; }
uint32_t nc_context_eip(const NcCpuContext *c) { return 0; }
void nc_context_set_eip(NcCpuContext *c, uint32_t eip) {}
void nc_context_export(const NcCpuContext *c, NcCpuState *s) {}
void nc_context_import(NcCpuContext *c, const NcCpuState *s) {}

void nc_cpu_run(NcCpu *cpu, const NcCpuContext *c) {}
void nc_cpu_invalidate(NcCpu *cpu, uint32_t va, uint32_t len) {}
bool nc_cpu_x87_single_check(NcCpu *cpu) { return false; }
void nc_cpu_request_interrupt(NcCpu *cpu) {}
void nc_cpu_cycle_offset_add(NcCpu *cpu, uint64_t ticks) {}
uint64_t nc_cpu_host_cycles(void) { return 0; }
uint64_t nc_cpu_guest_cycles(NcCpu *cpu) { return 0; }
void nc_cpu_guest_cycles_set(NcCpu *cpu, uint64_t ticks) {}
bool nc_cpu_x87_single(NcCpu *cpu) { return false; }
void nc_cpu_x87_single_restore(NcCpu *cpu) {}

void nc_frame_save(const NcFrame *f, NcCpuContext *c) {}
void nc_frame_load(NcFrame *f, const NcCpuContext *c) {}
uint32_t nc_frame_gpr(const NcFrame *f, int reg) { return 0; }
void nc_frame_set_gpr(NcFrame *f, int reg, uint32_t v) {}
uint32_t nc_frame_eip(const NcFrame *f) { return 0; }
void nc_frame_set_eip(NcFrame *f, uint32_t eip) {}
bool nc_frame_if(const NcFrame *f) { return false; }
void nc_frame_set_if(NcFrame *f, bool on) {}
void nc_frame_clear_interrupt(NcFrame *f) {}
void nc_frame_bypass_hook(NcFrame *f) {}
uint16_t nc_frame_cs(const NcFrame *f) { return 0; }
uint32_t nc_frame_eflags(NcFrame *f) { return 0; }
void nc_frame_set_eflags(NcFrame *f, uint32_t eflags) {}
uint64_t *nc_frame_regs(NcFrame *f) { return NULL; }
uint64_t *nc_frame_rip(NcFrame *f) { return NULL; }

uint32_t nc_cpu_guest_eip_at(NcCpu *cpu, uintptr_t host_pc) { return 0; }
bool nc_cpu_sample_pc(NcCpu *cpu, uintptr_t host_pc, uint32_t *eip)
{
    *eip = 0;
    return false;
}
NcFrame *nc_cpu_fault_frame(NcCpu *cpu, void *ucontext) { return NULL; }
void nc_cpu_fault_resume(NcCpu *cpu, void *ucontext) {}
bool nc_cpu_fault_is_write(const void *ucontext) { return false; }
bool nc_cpu_unaligned_fault(NcCpu *cpu, int code, void *ucontext) { return false; }
bool nc_cpu_callret_fault(NcCpu *cpu, uintptr_t fault_addr, void *ucontext) { return false; }
