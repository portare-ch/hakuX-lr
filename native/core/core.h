/*
 * hakuX native core — boot and run a title.
 */
#ifndef NATIVE_CORE_CORE_H
#define NATIVE_CORE_CORE_H

#include "native/core/vm.h"
#include "native/core/xbe_load.h"
#include "native/core/xdvdfs.h"
#include "native/cpu/cpu.h"
#include "native/kernel/kernel.h"

#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NcCore {
    NcVm vm;
    NcDisc disc;
    NcKernel kernel;
    NcXbe xbe;
    NcCpu *cpu;
} NcCore;

/* Sets up guest memory and the kernel, loads default.xbe from the disc and
 * prepares the title's main thread. Takes ownership of disc_fd. */
bool nc_core_boot(NcCore *c, int disc_fd);
/* The next boot goes into the savestate at path (native/state): the XBE it
 * was running boots, then the state replaces what the boot made. */
void nc_core_set_loadstate(const char *path);
/* The next boot is a relaunch: it boots the XBE the saved launch data page
 * names (nc_kernel_relaunch_file) instead of default.xbe. */
void nc_core_set_relaunch(bool on);
/* The next boot is the dashboard's (C:\xboxdash.xbe), with no disc: the app's
 * Dashboard, where Xbox Live accounts are made. */
void nc_core_set_dashboard(bool on);

/* Runs the guest until it stops; logs the frontier summary. */
void nc_core_run(NcCore *c);
/* Stopping the core in its process (LIBRETRO_SPEC.md section 6), in order:
 * - nc_core_request_stop, any thread: the guest stops (nc_core_run returns)
 *   at the CPU's next block start, or where it idles or pauses;
 * - nc_core_halt, once nc_core_run has returned (or nc_core_boot failed):
 *   the devices' and the I/O threads stopped and joined (written files
 *   synced, queued reads landed), nothing of the guest's running;
 * - the renderer stopped, by the caller (it reads guest memory and the
 *   devices' registers);
 * - nc_core_destroy: everything the boot made freed (the CPU, the devices,
 *   guest memory, files, the disc) and the core's state reset, so that
 *   nc_core_boot can run again as in a new process. */
void nc_core_request_stop(NcCore *c);
void nc_core_halt(NcCore *c);
void nc_core_destroy(NcCore *c);

/* The core works through faults: device registers (MMIO), the HLE GPU's
 * dirty tracking and the JIT's call-ret stack all trap. A front end's
 * SIGSEGV/SIGBUS handler calls this first; true means handled (return from
 * the handler), false that the fault is someone else's. */
bool nc_core_fault(NcCore *c, int sig, siginfo_t *info, void *ucontext);

/* The backend-specific half of a CPU's configuration (NcCpuConfig: guest
 * memory, the fault path, devices, CPUID, hooks), for the KVM backend. */
void nc_core_cpu_backend(NcCore *c, NcCpuConfig *cfg);

/* A guest memory access that faulted, as a CPU backend reports it. */
typedef struct NcGuestAccess {
    uint32_t va;     /* the guest address it reached */
    bool wrapped;    /* past the 4 GiB window's end: va is the wrapped address */
    bool page_fault; /* a page fault (FEX: SIGSEGV; SIGBUS is not one) */
    bool write;      /* the CPU's write bit (FEX: the abort's WnR; KVM: #PF's W) */
    /* FEX's backend: the fault's signal, for the decoders of dirty tracking
     * and MMIO (core/mmio.c); NULL in a backend without them (KVM's) */
    const siginfo_t *info;
    void *ucontext;
    /* the guest's frame at the faulting instruction, or NULL when the
     * access is host code's (a kernel call reaching guest memory); its EIP
     * for the logs, 0 if unknown. Asked only when needed: FEX finds them
     * from the host PC, too dear for every dirty-tracking fault. */
    NcFrame *(*frame)(void *opaque);
    uint32_t (*eip)(void *opaque);
    void *opaque;
} NcGuestAccess;

enum NcGuestFault {
    NC_GF_DECLINED, /* not the guest's to take: why says why (NULL: nothing to report) */
    NC_GF_RETRY,    /* handled: the guest runs the access again, or past it (MMIO) */
    NC_GF_FRAME,    /* handled in the frame (the title's #PF handler): the guest runs from it */
};

/* A guest access fault, the same for every CPU backend and in this order:
 * the NC_WATCHPA log, dirty tracking, MMIO by decoding (FEX), then for a
 * page the guest doesn't have the title's page tables since their last
 * flush (nc_paging_fault: Fable maps pages lazily), and only then the
 * title's page-fault handler (nc_kernel_page_fault). */
enum NcGuestFault nc_core_guest_fault(NcCore *c, const NcGuestAccess *a, const char **why);

#ifdef __cplusplus
}
#endif

#endif
