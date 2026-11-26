# Grafana for hakuX-lr

Can we see a RalliSport Challenge 2 hitch and tell whether it coincided with a burst of KVM exits, Xbox kernel calls, or GPU work? Start with the things the core can count accurately. The first experiment is to replay the same race with RSQRT correction in the guest and forced through the host, then compare the two traces.

## What the dashboard shows

| Panel | What it measures |
| --- | --- |
| FPS | New frames submitted to RetroArch per second, alongside `retro_run` calls per second. A 30 fps game can still have about 60 frontend ticks. |
| Frametime | p50, p95, p99 and recent maximum for elapsed time between successive new-frame submissions. The dashboard plots milliseconds. The capture uses a monotonic clock and nanoseconds. |
| Xbox kernel activity | Calls to emulated Xbox kernel exports and guest system-op callbacks per second. These counters overlap and are plotted as separate series. |
| KVM userspace returns | Returns from the vCPU's main `KVM_RUN` loop by reason, with interrupted calls identified separately. This panel applies to KVM runs. |
| CPU corrections per frame | RCP and RSQRT corrections divided by new frames over the same window. Break down by instruction, register or memory operand, and guest or host path. |
| Dropped snapshots | A counter for telemetry samples discarded under backpressure. |

Grafana can import the companion JSON and prompt for a Prometheus datasource. Filter by instance, game, backend and test variant. The draft dashboard expects the metric names below. [Grafana import guide](https://grafana.com/docs/grafana/latest/visualizations/dashboards/build-dashboards/import-dashboards/)

## Capture and export

1. **Frame timing.** Reuse the `shown` result from `nc_lr_video_run()` in `retro_run()`. Count fresh submissions and frontend ticks separately. Take a monotonic timestamp on fresh submissions and record the interval from the previous one. Restart the interval at boot, reset and save-state load. Repeated frames contribute to neither the frame count nor the interval histogram. Each sample measures delivery to RetroArch. Display scanout would need a frontend or display trace.
2. **CPU counters.** Reuse the kernel-call and system-op accounting in `native/kernel/kernel.c`. Publish cumulative snapshots from the vCPU thread so the sampling path can read them safely and a guest save-state load cannot rewind them. On KVM, classify the existing `KVM_RUN` return counter by reason. The current RCP and RSQRT totals can seed the aggregate view. The instruction and operand breakdown needs additional counters. Read guest-owned totals at a point where the vCPU cannot update them.
3. **Delivery.** Enable capture with a local socket path, for example `HAKUX_LR_METRICS_SOCKET`. The core gathers one snapshot per second and sends it over a nonblocking Unix datagram socket to a companion exporter. Keep network serving and formatting outside the core. Send cumulative counters and histogram buckets so the next successful snapshot recovers counts after a dropped one. Drop a busy send and count it. The exporter serves `/metrics` and stops exposing a run's metrics if snapshots stop for five seconds.
4. **Run labels.** Use bounded values for `game` (Xbox title ID when available), `backend` (`kvm` or `fex`) and `variant` (`guest`, `host` or `native`). Prometheus supplies `instance`. Start a fresh counter set on a new run and omit KVM-only series on FEX. Keep the exporter on loopback for the first experiment. During the timed segment, stay in gameplay. A long frontend pause can look like a stall because the core receives no frames during either one. Mark deliberate pauses in the run notes.

## Metrics contract

All series carry `game`, `backend` and `variant`. Start with a 1-second Prometheus scrape interval and a matching Grafana datasource interval. Grafana uses a wider rolling window for rates and histogram percentiles. [Prometheus query functions](https://prometheus.io/docs/prometheus/latest/querying/functions/)

| Metric | Type | Meaning |
| --- | --- | --- |
| `hakux_presented_frames_total` | Counter | Fresh frames passed to RetroArch |
| `hakux_retro_runs_total` | Counter | Frontend ticks |
| `hakux_frame_interval_seconds` | Classic histogram | Time between fresh submissions, exported as `_bucket`, `_sum` and `_count`. Give the expected 16.7 ms and 33.3 ms intervals nearby bucket boundaries. |
| `hakux_frame_interval_recent_max_seconds` | Gauge | Max interval over the last five 1-second capture windows. Omit when the window contains no intervals. |
| `hakux_kernel_calls_total` | Counter | Calls to Xbox kernel exports |
| `hakux_guest_system_ops_total` | Counter | Guest system-op callbacks, including hooks and interrupt notifications |
| `hakux_kvm_userspace_returns_total` | Counter | Main-loop returns by `reason`. Completion calls elsewhere in the backend are outside this first counter. |
| `hakux_cpu_corrections_total` | Counter | Corrections by `instruction` (`rcpss`, `rcpps`, `rsqrtss`, `rsqrtps`), `operand` (`reg`, `mem`) and `path` (`guest`, `host`) |
| `hakux_telemetry_dropped_snapshots_total` | Counter | Snapshots discarded by the sender |

Use histogram percentiles for long runs. If one hitch needs a frame-by-frame explanation, add an opt-in local trace with a monotonic timestamp and interval for every fresh frame. Prometheus summaries cannot recover the order of frames inside a scrape.

## First runs on the Ryzen 7 9800X3D

Use one build and the same save, route, resolution, core options and pacing. Warm up, then record the same 60-second gameplay segment three times per variant. Compare defaults (`guest`) with `NC_KVM_RSQRT_INGUEST=0` (`host`, still correcting). Use `NC_KVM_RSQRT=0` (`native`) as a separate performance and accuracy reference. Run matching telemetry-on and telemetry-off samples to see whether instrumentation moves the baseline. Check exact frame and correction totals against the harness or end-of-run counters before interpreting the charts.

| Game | What to look for |
| --- | --- |
| **RalliSport Challenge 2** | The earlier test saw roughly 86,000 RSQRT host exits/s and 44–48 fps before in-guest memory correction, then 60 fps in the test build. Does the frametime tail shrink on the same race while the exit spikes disappear? |
| **Project Gotham Racing 2** | About 22,500 RSQRT host exits/s before the change, with 30 fps on both sides. Look for lower CPU work per frame and a tighter tail despite the same FPS. |
| **Halo 2** | The earlier campaign run averaged about 192 register-form RCP corrections per frame. Check how much the rate varies with the scene and how much comes from scalar versus packed instructions. |

[Register correction benchmark and Halo 2 data](https://github.com/portare-ch/hakuX-lr/pull/316) · [Memory correction benchmark](https://github.com/portare-ch/hakuX-lr/pull/317) · [Game results](https://github.com/portare-ch/hakuX-lr/pull/317#issuecomment-7176501872)

Once the instruction mix is available, multiply avoided exits per frame by the matching host-minus-guest microbenchmark costs to estimate CPU nanoseconds saved per frame. Keep that number alongside the observed frametime difference. The latter includes pacing, scheduling and renderer effects.
