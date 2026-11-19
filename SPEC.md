# PortareOS port: day-one specification

Status: complete; integrated in PR #3 (`8f2937308e`), issue #2 closed.
Historical scope for the headless milestone; later work is recorded in
[PORTAREOS_HANDOFF.md](PORTAREOS_HANDOFF.md).
Written: 2026-10-24.
Scope updated: 2026-10-24; Android regression validation waived by the owner.
Time budget: one developer, one working day (approximately eight hours).
Target: Retroid Pocket Nova (SM8550), native AArch64 Linux on PortareOS.
Repository: [portare-ch/hakuX-lr](https://github.com/portare-ch/hakuX-lr).

## Objective

Prove that hakuX's existing FEX embedding, guest memory and replacement Xbox
kernel can build and execute on native ARM64 Linux. Produce a reproducible
headless build, runtime evidence and a Vulkan capability report from the Nova.
These establish whether the next step can be a native SDL3/Vulkan frontend.

A playable game is a stretch goal for later work. A successful compile alone
does not complete this milestone, and this work does not establish game
compatibility, frame rate or an advantage over xemu.

## Starting point

The fork's `master` was at `254507d7083a2437725b09c881ebe105c829125c`
when this specification was written. The source review also inspected the
following upstream commit, `98b935311184a11b80b6c2a2993536475ca4c0d3`,
which adds `external/fex` as a submodule. That submodule addition is absent
from the fork's starting revision. Reconcile this explicitly before building;
do not silently update the rest of upstream.

Use hakuX's FEX fork, with the reviewed revision
`52018bf46a8279b22bce738548ab3f40db467064`. PortareOS's existing system FEX
package provides a Clang cross-build example; it does not supply hakuX's
custom embedding interface. Dependencies and their documented revisions are
listed in [android/README.md](android/README.md).

Relevant boundaries already present in the code:

| Area | Existing code | Day-one implication |
| --- | --- | --- |
| Build | [native/CMakeLists.txt](native/CMakeLists.txt), [native/cmake](native/cmake) | Android libraries and platform definitions are linked unconditionally in several targets. |
| Headless runner | [native/run/xboxrun.c](native/run/xboxrun.c) | Reuse its boot path, bounded execution and fault diagnostics. |
| CPU and signals | [native/cpu/cpu.cpp](native/cpu/cpu.cpp), [native/core/mmio.c](native/core/mmio.c) | Verify the Linux/glibc signal context and generated ARM64 code by execution. |
| Guest RAM | [native/core/vm.c](native/core/vm.c), [hw/xbox/hle/uma/uma.c](hw/xbox/hle/uma/uma.c) | A memfd fallback exists; preserve alias mappings and write tracking. |
| Audio output | [native/dev/apu/out.c](native/dev/apu/out.c) | AAudio needs a separate Linux diagnostic backend, including pacing. |
| Graphics | [hw/xbox/hle/vk/vkdev.c](hw/xbox/hle/vk/vkdev.c), [hw/xbox/hle/vk/guest_ram.c](hw/xbox/hle/vk/guest_ram.c), [hw/xbox/hle/vk/swapchain.c](hw/xbox/hle/vk/swapchain.c) | AHB is currently required at device initialization, and presentation uses Android surfaces. |

## Scope and implementation constraints

Keep the existing CPU, kernel, graphics translation and APU implementations.
Add platform boundaries where needed for PortareOS. Android regression
validation and ongoing Android compatibility are outside this fork's
acceptance criteria. Linux builds must not require the Android SDK, NDK,
JVM, adrenotools or Android runtime libraries.

The day-one executable is the existing `xboxrun`, built for AArch64 Linux.
The graphical app target may be disabled explicitly for this configuration.
Guard or separate Android-only window and AHB code so the shared core can
compile. Do not make the linker pass by adding fake Android/Vulkan functions
or by changing guest kernel semantics.

Use memfd-backed guest RAM for the initial Linux path. Preserve the existing
copy fallback for future rendering; Linux zero-copy sharing is a later task.
Make Android-only Vulkan extensions conditional without weakening the other
renderer requirements. A headless run may explicitly disable the renderer;
it must report that mode rather than imply that Vulkan presentation works.

Provide an explicitly selected diagnostic audio sink that drains the APU's
output at the correct rate and respects pause. Keep the emulated APU running.
Discarding samples without pacing, or bypassing the APU to make a title
advance, is not an acceptable substitute. Audible output is outside this day.

Put Android property access behind a small platform interface or conditional
compilation. Preserve defaults and distinguish startup configuration from
live debug controls; environment variables alone do not provide live updates.

Use an ARM64 Linux container on the macOS development host. Verify the
resulting executable against PortareOS's runtime libraries before treating
a Debian container build as deployable. Use PortareOS's sysroot if its ABI
requires it. Build outputs and downloaded dependencies stay out of Git.

## Work sequence

The time allocations are planning limits. If a stage blocks, preserve the
exact failure and remaining work rather than expanding the day into a full
frontend port.

| Time | Work | Deliverable |
| --- | --- | --- |
| Hour 1 | Reconcile the fork baseline; pin FEX and required submodules and dependencies; record the compiler, container and target ABI. | Reproducible source/dependency manifest and build entry point. |
| Hours 2–4 | Separate Android build dependencies, properties, audio output and graphics platform code sufficiently to link the Linux headless runner. | An AArch64 Linux `xboxrun` with a documented configuration. |
| Hours 5–6 | Exercise the actual FEX embedding and guest-memory fault paths, first in ARM64 Linux and then on the Nova. | Small runtime smoke harness, explicit assertions and logs. |
| Hour 7 | Query the Nova's Vulkan driver and compare it with hakuX's requirements. | Capability report with each missing requirement named. |
| Hour 8 | Repeat the documented Linux/Nova build/checks; write the handoff and next task. | Reviewable changes, results and outstanding blockers. |

If time remains after the required checks, attempt a bounded headless boot
of one user-supplied game known to boot in the Android version. Record the
title ID, build, options and furthest observed execution point. Loading an
XBE or reaching a timeout is not evidence that the game is playable.

## Required runtime checks

Use the real `NcCpu`/FEX embedding and `NcVm` implementation. Add only the
small tests needed for the platform boundary; reuse the existing
[FEX runner](tests/native/fexrun) and [host tests](tests/hle) where useful.

1. **Guest execution:** run a small x86 program with known arithmetic,
   branch, memory and register results. Exercise a system-operation exit
   through hakuX's callback and an expected stop. Assert the results.
2. **Memory aliases:** write through one guest mapping and read through
   another mapping of the same physical page. Verify the shared bytes and
   the actual host page size; do not assume it is 4 KiB.
3. **Fault handling:** make a tracked guest page read-only, perform a write
   from translated guest code and verify fault handling resumes the write
   and updates tracking. Exercise an MMIO access through the core's handler
   and verify its read/write result. An unexpected host fault must fail.
4. **Runner failures:** verify a missing disc produces a clear error and
   nonzero exit. Verify an intentionally nonterminating guest is stopped
   by the timeout and reported as a timeout, never a passing boot.
5. **Audio pacing:** verify the diagnostic sink consumes samples at the
   configured rate, wakes a waiting producer and stops consuming during
   pause. A full ring must not deadlock the smoke run.

The positive smoke checks must exit successfully, and an intentionally
incorrect expected result must make the harness fail. Capture commands,
exit codes and logs. Run the relevant existing tests after the changes.

## Nova Vulkan investigation

Record the GPU/driver identification, Vulkan API version, graphics/compute
queue support and the complete required feature/extension checks in
`vkdev.c`. Include Vulkan 1.3, descriptor indexing, buffer device addresses,
timeline semaphores, synchronization2, dynamic rendering and the required
dynamic-state, format and blending features.

Identify the instance extensions SDL3 KMSDRM needs for a Vulkan display
surface on the installed PortareOS build. Report Android surface/AHB
extensions separately from portable renderer requirements. Record available
Linux external-memory mechanisms as information for later work; their
presence alone does not prove that guest RAM can be imported correctly.

This step queries capabilities. Surface creation, presentation, cache
coherency, shared-memory correctness and frame pacing need subsequent runtime
tests. A software Vulkan device in a container cannot substitute for the
Nova's Turnip driver.

## Acceptance and evidence

Day one is complete when all of the following are recorded:

- A clean checkout plus the pinned dependencies can reproduce the Linux
  build using documented commands. The executable is AArch64 ELF and its
  dynamic dependencies resolve on PortareOS without Android libraries.
- The required positive and negative runtime checks pass on the Nova.
  Container results are recorded separately. If the device is unavailable,
  label the milestone incomplete and retain the build and test artifacts.
- The Vulkan report names supported and missing requirements and identifies
  the next graphics task. A missing portable feature is an explicit design
  blocker, not a reason to silently relax the renderer's requirements.
- Android regression validation is not required. It was waived by the owner
  on 2026-10-24 and was not performed; this is not a passing Android result.
- The handoff identifies the exact source/dependency revisions, build and
  run commands, test results, device/OS/driver versions and remaining work.

Do not commit disc images, guest software, credentials, personal saves or
large generated logs. Use a dedicated temporary data directory for any
device run so diagnostics cannot overwrite the user's existing hakuX or
xemu data. Preserve the upstream license and attribution files.

## Deferred work

The following belong to subsequent specifications:

- SDL3/Vulkan window presentation and replacement of Choreographer timing.
- Real Linux audio output, controller/rumble integration and game lifecycle.
- CHD reads through a disc abstraction, including compressed-image tests.
  The owner changed this on 2026-10-24: the Xbox library accepts XISO
  (`.iso`) images now, and CHD comes last, once everything else works.
- PortareOS package, launcher selection, persistent settings and save paths.
- Save-state UI, pause menu, suspend/resume and reliable title relaunch.
- Linux zero-copy GPU memory, performance tuning and game compatibility work.

Choose the next day's scope from the measured blockers. If the CPU/memory
checks pass and the Nova meets the portable Vulkan requirements, the next
milestone is presenting the first game frame through SDL3 KMSDRM.
