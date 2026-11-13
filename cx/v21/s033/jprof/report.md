# Java Framework vs binding: current 1 KiB stage profile

## Scope and result

- Worktree `/root/worktree/zlink-ratio040`, `main` at `ee1139025782688e33109455041d8a0222cc8fe3`; Java 25.0.4.1. The required preflight passed (`dirty=4`; the four existing pinned `run_local.sh` changes were retained). The bench was not rebuilt. No tracked files were changed.
- Every benchmark, JFR, and `strace` capture was serialized through `flock -o /root/jobs/fx/cx/measure.lock`. `MemAvailable` stayed above 24 GiB. No raw capture exceeded 1 GiB.
- Current one-run, unprofiled results are the comparison point for captures. They are one observation, not a repeatability estimate. The 10 s run differs from the supplied ratio040 5 s run and happened amid other serialized profiling jobs.

| Pattern, 1024 B | ratio040 Framework / binding (given) | Current unprofiled Framework / binding | Current ratio | JFR rate F / B | `strace` rate F / B |
|---|---:|---:|---:|---:|---:|
| send-saturation | 114,094 / 311,460 | 108,085 / 308,449 | 0.350 | 94,520 / 282,803 | 11,841 / 36,990 |
| request-backpressure | 57,145 / 124,104 | 49,914 / 122,744 | 0.407 | 47,633 / 114,368 | 18,487 / 4,253 |
| request-serial | 8,651 / 14,862 | 6,689 / 14,344 | 0.466 | 5,464 / 12,353 | 975 / 1,606 |

JFR changed current unprofiled throughput by −12.6%/−8.3% for send, −4.6%/−6.8% for backpressure, and −18.3%/−13.9% for serial (Framework/binding). `strace` rates are severely perturbed and are used only to normalize syscall counts. The serial run is especially sensitive: low throughput leaves compiler activity high per completed message. Do not interpret its JFR CPU-per-message totals as serial request cost.

## Per-thread CPU and limiting resource

CPU/message is Linux thread CPU divided by completed messages during the active capture; core share is CPU divided by active elapsed time. JFR and `/proc` are separate measures: Java stack samples identify stages while `/proc` captures native thread CPU. The complete top-thread lists are in the detailed capture output below; these rows show the request-serving threads and important worker pools.

| Pattern | Process | Critical/busiest thread and role | CPU µs/msg | Core share | Other high-use threads (CPU µs/msg; core share) |
|---|---|---|---:|---:|---|
| send | Framework source | `ZLINKbg/IO/0`, Core submit/completion I/O; busiest required resource | 10.146 | 0.959 | eight `bench-stream-*` submitters 1.465–1.508 each, 0.139–0.142 each |
| send | binding source | `ZLINKbg/IO/0`, Core submit/completion I/O; busiest required resource | 3.456 | 0.978 | eight `bench-stream-*` submitters 0.808–0.819 each, 0.229–0.232 each |
| send | Framework target | `ZLINKbg/IO/0`, Core receive I/O | 8.342 | 0.788 | Java ingress pump 6.940, 0.656; FJP workers 2.164–2.534, 0.204–0.239 |
| send | binding target | `ZLINKbg/IO/0`, Core receive I/O | 2.907 | 0.822 | `bench-raw-command` 2.351, 0.665 |
| backpressure | Framework source | `bench-phase-active`, submit/await driver | 15.222 | 0.725 | Core IO/0 14.456, 0.689; IO/2 14.005, 0.667; Java pump 13.428, 0.640; common-pool workers 3.263–3.378, 0.155–0.161 |
| backpressure | binding source | `bench-phase-active`, submit/await driver | 8.293 | 0.948 | Core IO/0 8.114, 0.928; IO/2 6.654, 0.761 |
| backpressure | Framework target | `ZLINKbg/IO/1`, Core receive I/O | 15.191 | 0.724 | Java ingress pump 13.974, 0.666; FJP workers 4.742–6.662, 0.226–0.317 |
| backpressure | binding target | `ZLINKbg/IO/1`, Core receive I/O | 7.659 | 0.876 | Core IO/0 7.498, 0.857; `bench-raw-request` 6.816, 0.779 |
| serial | Framework source | No single saturated request-serving thread; one-outstanding round trip is critical. Highest CPU was C2 compiler at 37.611, 0.205, a profiling/JIT artifact. | — | — | `ZLINKbg/IO/2` 29.741, 0.162; Java pump 25.989, 0.142; IO/0 25.714, 0.140; `bench-stream-0` 24.708, 0.135; common-pool worker 22.146, 0.121 |
| serial | binding source | `bench-stream-0`, serial submit/waiter; low-load request path | 18.214 | 0.225 | Core IO/0 15.704, 0.194; IO/2 15.380, 0.190 |
| serial | Framework target | No saturated target thread; round-trip wait is critical. Highest CPU was C2 compiler at 36.970, 0.202, a profiling/JIT artifact. | — | — | Java pump 28.368, 0.155; Core IO/1 23.335, 0.127; IO/0 14.001, 0.076; FJP worker 17.387, 0.095 |
| serial | binding target | Core IO/1 | 15.340 | 0.189 | Core IO/0 15.057, 0.186; `bench-raw-request` 12.062, 0.149 |

For send, the Framework source Core IO thread is near one full core and consumes about 6.69 µs/message more CPU than binding's corresponding thread; the target also shows a large Core IO and ingress-pump delta. For request-backpressure, caller, IO, ingress, and completion workers all contribute, so there is no one-thread explanation. For serial, no thread is saturated; CPU is far below a core and the measured Framework mean round trip is 0.149 ms versus 0.0696 ms for binding in the unprofiled run. The request-serial 4K-stage latency split could not be measured from the supplied counters.

### Request-serial latency breakdown

The required timestamps (submit→wire send, network/peer, receive→completion, completion→next submit) are **not emitted by the bench or runtime counters**. They are therefore not measured; JFR CPU samples cannot substitute for elapsed transition timestamps. Available total means: current unprofiled Framework 0.1490 ms and binding 0.06956 ms; ratio040 JSON Framework 0.11221 ms. The detailed report must not infer per-stage latency from CPU time.

## Stage accounting

Functions were assigned using JFR sampled stack self-time and scaled to measured per-thread CPU. Allocation includes sampled allocation/GC frames; hand-off includes `LockSupport`, ForkJoin, execution queue, lane, and monitor/futex work; task/future includes `CompletableFuture` and completion frames; codec includes protobuf, envelope, JSON, and M6A service-wire frames; binding/Core includes FFM/downcall, native `ZLINKbg/IO`, and kernel socket frames; Framework bookkeeping includes remaining `systems.zlink.framework` runtime work; benchmark driver includes `systems.zlink.bench`; unrecognized/JDK/JIT work remains `other`. Unsampled native IO is attributed by thread name. Inclusive function values overlap and are ceilings, not additive savings.

| Pattern / µs per completed msg | F source | F target | B source | B target |
|---|---:|---:|---:|---:|
| send: allocation | 1.315 | 0.827 | 0.069 | 0.322 |
| send: hand-off/wake | 0.670 | 4.452 | 0.380 | 0.000 |
| send: task/future | 0.195 | 0.256 | 0.000 | 0.000 |
| send: codec | 0.702 | 0.652 | 1.019 | 0.000 |
| send: binding/Core | 11.820 | 9.879 | 3.877 | 3.216 |
| send: Framework bookkeeping | 0.767 | 1.025 | 0.000 | 0.000 |
| send: benchmark driver | 1.324 | 0.004 | 0.573 | 0.245 |
| send: other | 6.778 | 7.154 | 4.432 | 1.884 |
| backpressure: allocation | 2.200 | 1.076 | 0.214 | 0.689 |
| backpressure: hand-off/wake | 6.333 | 10.405 | 0.153 | 0.000 |
| backpressure: task/future | 3.621 | 0.018 | 0.400 | 0.000 |
| backpressure: codec | 9.851 | 4.756 | 1.549 | 0.904 |
| backpressure: binding/Core | 36.596 | 30.157 | 15.926 | 16.525 |
| backpressure: Framework bookkeeping | 3.295 | 1.051 | 0.000 | 0.000 |
| backpressure: benchmark driver | 1.085 | 0.024 | 0.649 | 0.173 |
| backpressure: other | 35.444 | 21.323 | 5.255 | 4.587 |
| serial: allocation | 4.759 | 4.301 | 0.607 | 0.324 |
| serial: hand-off/wake | 17.894 | 32.116 | 0.000 | 0.000 |
| serial: task/future | 2.678 | 0.000 | 1.656 | 0.000 |
| serial: codec | 5.508 | 10.157 | 1.656 | 1.206 |
| serial: binding/Core | 64.578 | 42.164 | 36.149 | 32.809 |
| serial: Framework bookkeeping | 11.677 | 2.101 | 0.000 | 0.000 |
| serial: benchmark driver | 0.768 | 0.000 | 3.312 | 2.443 |
| serial: other | 133.269 | 108.471 | 12.557 | 9.966 |

The serial table is strongly distorted by JIT/compiler CPU divided by the lower profiled completion count. Do not use it as round-trip stage latency. For send and backpressure, process totals include threads running in parallel and are not wall-time budgets. Critical-thread stage splits, Framework Java function samples, thread-park counts, and allocation samples are listed in the detailed analysis appended below.

### Message counts and contention

`strace -f -c` and `/proc/<tid>/status` were collected in separate, intrusive count runs. Allocation is JFR `ObjectAllocationSample` at 100 events/s: report event samples/message and weighted byte estimates, **not exact object construction counts**. The JFR profile captured no GC event in these windows (0 recorded per 1M messages). The full syscall/futex/context-switch counts are in the appended analysis. The top contended Java monitor classes are in `locks-gc.txt`; most starts came from `ReferenceQueue$Lock` in cleaner/GC support, not a message lock. For example, send target Framework recorded 1,803 monitor-enter starts/M messages, of which 1,792/M were `ReferenceQueue$Lock`; backpressure Framework source recorded 2,456/M, of which 2,222/M were `ReferenceQueue$Lock`. These are JFR monitor contention events, not every synchronized acquisition. `ThreadPark` includes parks of at least 1 ms only.


### `perf stat` cross-check

`task-clock` divided by the 20 s active interval gives each process's CPU core share; context switches are divided by messages completed in that same perf run. The profiler attaches at active and detaches at idle.

| Pattern | Framework source / target core share | Binding source / target core share | Switches/message F source / target | Switches/message B source / target |
|---|---:|---:|---:|---:|
| send-saturation | 2.02 / 2.11 | 2.77 / 1.52 | 1.074 / 0.927 | 0.354 / 0.183 |
| request-backpressure | 4.60 / 3.23 | 2.62 / 2.45 | 2.181 / 1.098 | 0.352 / 0.444 |
| request-serial | 1.22 / 0.92 | 0.61 / 0.52 | 6.061 / 4.037 | 3.007 / 2.994 |

These process-wide values corroborate the `/proc` picture: send uses about two source cores in each implementation, while Framework request-backpressure uses more than four. Context switches include all JVM workers. CPU migrations and raw counts are preserved in each `perf-*.csv`.

## Ranked candidate changes

The ratio ceiling uses the supplied ratio040 reciprocal-throughput gap and assumes every listed inclusive CPU µs becomes end-to-end saving, with no new limiter. It is an intentionally optimistic arithmetic ceiling, not a speedup prediction. Required removed time is send 3.414 µs/message, backpressure 4.070 µs/message, serial 3.451 µs/message.

| Rank / cell | Candidate and stage | Measured maximum attributable cost | Optimistic ratio ceiling | Assessment |
|---|---|---:|---:|---|
| 1 / backpressure | Avoid scanning the same envelope header twice across `ZLinkChannelEnvelope.decodeDispatchHeader:558` → `decodeHeader:493` (codec); preserve strict validation and cache behavior. | 3.540 µs/msg inclusive on the busy Java request path | 0.577 | Most concrete large sampled Java hotspot. The inclusive cost overlaps the decoder below it and is not fully removable. It cannot alone reach 0.6 under the optimistic bound. |
| 1 / serial | Reduce completion hand-off around `ZLinkServiceCompletionDispatcher.post:53` / `ZLinkJavaRawServicePort.deliverTerminal:491` (handoff/completion). | 2.079 µs/msg inclusive across two samples | 0.593 | Profile sample is sparse and CPU time is not elapsed latency. Use only as a first focused prototype after adding/using timestamp evidence for the four transitions. |
| 1 / send | Reduce Framework target dispatch work at `ZLinkJavaRawMeshNode$1Rotation.dispatchClaimed:5587` (dispatch/handoff). | 0.862 µs/msg inclusive on busiest Java ingress pump | 0.406 | The saturated thread is native/Core IO, not this Java pump; current samples do not show this as a causal send limiter. Ceiling stays below 0.6. |
| 2 / send | Simplify `ZLinkJavaRawMeshNode.drainIngressBatch:5617` / envelope and M6A decode (handoff/codec). | 0.675 µs/msg inclusive; measured target codec total 0.652 | 0.402 | A small ceiling, and receive ordering/claim ownership must stay intact. |
| 2 / backpressure | Reduce source header encode work at `ZLinkChannelEnvelope.jsonString:270` (codec). | 0.963 µs/msg self, one sampled caller thread | 0.489 | One of the clearest source-side samples; dynamic correlation/deadline fields prevent treating the full header as cacheable. |
| 2 / serial | Reduce header decode/dispatch at `ZLinkChannelEnvelope.decodeHeader:493` and `decodeDispatchHeader:558` (codec). | 5.253 µs/msg inclusive on Framework target Java pump, across 15 samples | 0.611 | Largest apparent serial Framework Java stack ceiling, but serial JFR CPU/message is compiler-contaminated and decoder work is required. Do not credit this ratio without a clean serial profile and latency markers. |

**First change to evaluate:** the backpressure dispatch-header double scan at `decodeDispatchHeader`/`decodeHeader`, because it is the clearest high-cost, code-local sample in a throughput-critical pattern and can be measured without changing the wire contract. It is not predicted to meet 0.6 alone. The send profile points at the Core/native IO execution resource, so a Framework-only first change is not justified for send by this evidence. Serial needs transition timestamps before a code choice is supportable.

## Artifacts and limits

Raw JFR, count-run snapshots/logs, and run JSON are under `/root/jobs/fx/cx/v21/s033/jprof/`; analysis scripts and summaries are there as well. `analysis.txt` is appended below to preserve all captured top threads, stage splits, sampled Framework functions, and per-message counters. The `perf stat` active-window captures are summarized below; per-thread CPU still comes from `/proc`, and contention from JFR. Exact allocation counts and request-serial transition timestamps were not measured.

---

## Detailed JFR and thread analysis


## send-saturation

### Framework: plain=108084.8/s profiled=94519.8/s counts=11840.5/s profile_msgs=1,890,395
 source: proc CPU=23.572 us/msg; ExecutionSample=146; allocation samples=1911 (0.001011/msg), weighted bytes=2521.4 B/msg; primary=ZLINKbg/IO/0
  Thread | CPU µs/message | share of core | role
  ZLINKbg/IO/0 | 10.146 | 0.959 | native/core IO
  bench-stream-3 | 1.508 | 0.142 | JVM
  bench-stream-5 | 1.486 | 0.141 | JVM
  bench-stream-0 | 1.481 | 0.140 | JVM
  bench-stream-4 | 1.481 | 0.140 | JVM
  bench-stream-7 | 1.481 | 0.140 | JVM
  bench-stream-1 | 1.471 | 0.139 | JVM
  bench-stream-2 | 1.465 | 0.139 | JVM
  bench-stream-6 | 1.465 | 0.139 | JVM
  C2 CompilerThre | 0.587 | 0.056 | JVM
  C2 CompilerThre | 0.180 | 0.017 | JVM
  ForkJoinPool-1-worker-3 | 0.164 | 0.015 | JVM
  C1 CompilerThre | 0.138 | 0.013 | JVM
  C2 CompilerThre | 0.127 | 0.012 | JVM
  C2 CompilerThre | 0.079 | 0.008 | JVM
  HTTP-Dispatcher | 0.063 | 0.006 | JVM
  Process stage µs/message (self-sample classification × measured per-thread CPU; native unsampled CPU mapped by thread name)
  binding/core | 11.820 | 50.1%
  other | 6.778 | 28.8%
  benchmark driver | 1.324 | 5.6%
  allocation | 1.315 | 5.6%
  framework bookkeeping | 0.767 | 3.3%
  codec | 0.702 | 3.0%
  handoff | 0.670 | 2.8%
  task/future | 0.195 | 0.8%
  Critical-thread stage µs/message (ZLINKbg/IO/0; no JFR samples means native CPU assigned to binding/core)
  binding/core | 10.146 | 100.0%
  Framework-only functions on critical thread, self/inclusive µs/message (inclusive rows overlap)
  None sampled: critical thread is native/core IO and has no Java stack frames.
  Busiest sampled Java Framework thread supplement: bench-stream-3 (1.508 µs/msg)
  systems.zlink.framework.runtime.internal.configuration.ZLinkCodecRegistration.serializerForContentType:324 | self=0.000 | inclusive=0.137 | selfSamples=0 inclusiveSamples=1
  systems.zlink.framework.codecs.protobuf.ZLinkProtobufMessageSerializer.serializeOwned:35 | self=0.000 | inclusive=0.137 | selfSamples=0 inclusiveSamples=1
  systems.zlink.framework.runtime.messaging.ZLinkPayloadEncoding.encodeSelected:57 | self=0.000 | inclusive=0.137 | selfSamples=0 inclusiveSamples=1
  systems.zlink.framework.runtime.messaging.ZLinkPayloadEncoding.encodeMessage:48 | self=0.000 | inclusive=0.137 | selfSamples=0 inclusiveSamples=1
  systems.zlink.framework.runtime.channels.ZLinkChannelRuntime.sendToNode:1306 | self=0.000 | inclusive=0.137 | selfSamples=0 inclusiveSamples=1
  systems.zlink.framework.runtime.internal.execution.ZLinkStateLane.current:101 | self=0.000 | inclusive=0.137 | selfSamples=0 inclusiveSamples=1
  systems.zlink.framework.runtime.internal.calls.ZLinkBlockingCalls.isRuntimeExecutionContext:65 | self=0.000 | inclusive=0.137 | selfSamples=0 inclusiveSamples=1
  systems.zlink.framework.runtime.internal.service.ZLinkFrameworkMultipartView.close:197 | self=0.000 | inclusive=0.137 | selfSamples=0 inclusiveSamples=1
  systems.zlink.framework.runtime.internal.service.ZLinkFrameworkMultipartView.closeOwner:205 | self=0.000 | inclusive=0.137 | selfSamples=0 inclusiveSamples=1
  systems.zlink.framework.runtime.internal.service.ZLinkFrameworkMultipartView.close:198 | self=0.000 | inclusive=0.137 | selfSamples=0 inclusiveSamples=1
  systems.zlink.framework.runtime.internal.service.ZLinkFrameworkMultipartView.closeAll:138 | self=0.000 | inclusive=0.137 | selfSamples=0 inclusiveSamples=1
  systems.zlink.framework.runtime.channels.ZLinkChannelSocketRegistry.lambda$submitToNode$0:524 | self=0.000 | inclusive=0.137 | selfSamples=0 inclusiveSamples=1
  systems.zlink.framework.runtime.channels.ZLinkChannelSocketRegistry$$Lambda.0x00000000164c0450.get:-1 | self=0.000 | inclusive=0.137 | selfSamples=0 inclusiveSamples=1
  systems.zlink.framework.runtime.channels.ZLinkChannelSocketRegistry.lambda$prepareAndSubmit$0:648 | self=0.000 | inclusive=0.137 | selfSamples=0 inclusiveSamples=1
  systems.zlink.framework.runtime.channels.ZLinkChannelSocketRegistry$$Lambda.0x00000000164c08a0.get:-1 | self=0.000 | inclusive=0.137 | selfSamples=0 inclusiveSamples=1
  jdk.ThreadPark: 1063 (0.000562/msg)
  jdk.JavaMonitorWait: 3 (0.000002/msg)
  jdk.JavaMonitorEnter: 1 (0.000001/msg)
 target: proc CPU=24.249 us/msg; ExecutionSample=695; allocation samples=1841 (0.000974/msg), weighted bytes=5570.9 B/msg; primary=ZLINKbg/IO/0
  Thread | CPU µs/message | share of core | role
  ZLINKbg/IO/0 | 8.342 | 0.788 | native/core IO
  zlink-jvm-raw-mesh-bench | 6.940 | 0.656 | native/core IO
  ForkJoinPool-1-worker-5 | 2.534 | 0.239 | JVM
  ForkJoinPool-1-worker-4 | 2.164 | 0.204 | JVM
  ForkJoinPool-1-worker-2 | 1.106 | 0.104 | JVM
  C2 CompilerThre | 0.682 | 0.065 | JVM
  ForkJoinPool-1-worker-1 | 0.629 | 0.059 | JVM
  C2 CompilerThre | 0.212 | 0.020 | JVM
  C2 CompilerThre | 0.143 | 0.013 | JVM
  C2 CompilerThre | 0.122 | 0.011 | JVM
  bench-stats | 0.116 | 0.011 | JVM
  C2 CompilerThre | 0.116 | 0.011 | JVM
  Common-Cleaner | 0.095 | 0.009 | JVM
  GC Thread#4 | 0.090 | 0.009 | JVM
  GC Thread#1 | 0.085 | 0.008 | JVM
  GC Thread#2 | 0.085 | 0.008 | JVM
  Process stage µs/message (self-sample classification × measured per-thread CPU; native unsampled CPU mapped by thread name)
  binding/core | 9.879 | 40.7%
  other | 7.154 | 29.5%
  handoff | 4.452 | 18.4%
  framework bookkeeping | 1.025 | 4.2%
  allocation | 0.827 | 3.4%
  codec | 0.652 | 2.7%
  task/future | 0.256 | 1.1%
  benchmark driver | 0.004 | 0.0%
  Critical-thread stage µs/message (ZLINKbg/IO/0; no JFR samples means native CPU assigned to binding/core)
  binding/core | 8.342 | 100.0%
  Framework-only functions on critical thread, self/inclusive µs/message (inclusive rows overlap)
  None sampled: critical thread is native/core IO and has no Java stack frames.
  Busiest sampled Java Framework thread supplement: zlink-jvm-raw-mesh-bench (6.940 µs/msg)
  systems.zlink.framework.runtime.binding.ZLinkJavaRawMeshNode$1Rotation.dispatchClaimed:5587 | self=0.023 | inclusive=0.862 | selfSamples=1 inclusiveSamples=37
  systems.zlink.framework.runtime.binding.ZLinkJavaRawMeshNode.drainIngressBatch:5617 | self=0.023 | inclusive=0.675 | selfSamples=1 inclusiveSamples=29
  systems.zlink.framework.runtime.binding.ZLinkJavaRawMeshNode.lambda$startPump$0:5396 | self=0.000 | inclusive=0.652 | selfSamples=0 inclusiveSamples=28
  systems.zlink.framework.runtime.binding.ZLinkJavaRawMeshNode.pumpTurn:5430 | self=0.000 | inclusive=0.582 | selfSamples=0 inclusiveSamples=25
  systems.zlink.framework.runtime.channels.ZLinkMeshApplicationDispatcher.accept:65 | self=0.070 | inclusive=0.489 | selfSamples=3 inclusiveSamples=21
  systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec.decodeFrameworkMultipartFrameView:504 | self=0.210 | inclusive=0.443 | selfSamples=9 inclusiveSamples=19
  systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec.decodeFrameworkMultipartFrameView:465 | self=0.000 | inclusive=0.443 | selfSamples=0 inclusiveSamples=19
  systems.zlink.framework.runtime.binding.ZLinkJavaRawMeshNode.dispatch:5973 | self=0.000 | inclusive=0.443 | selfSamples=0 inclusiveSamples=19
  systems.zlink.framework.runtime.internal.execution.ZLinkStateLane.runSynchronousTurn:142 | self=0.000 | inclusive=0.419 | selfSamples=0 inclusiveSamples=18
  systems.zlink.framework.runtime.internal.execution.ZLinkStateLane.callWithCurrent:424 | self=0.093 | inclusive=0.396 | selfSamples=4 inclusiveSamples=17
  systems.zlink.framework.runtime.binding.ZLinkJavaRawMeshNode.dispatch:6058 | self=0.000 | inclusive=0.326 | selfSamples=0 inclusiveSamples=14
  systems.zlink.framework.runtime.binding.ZLinkJavaRawMeshNode$1Rotation.get:5469 | self=0.000 | inclusive=0.303 | selfSamples=0 inclusiveSamples=13
  systems.zlink.framework.runtime.internal.execution.ZLinkStateLane.awaitTurn:129 | self=0.000 | inclusive=0.279 | selfSamples=0 inclusiveSamples=12
  systems.zlink.framework.runtime.binding.ZLinkJavaRawMeshNode$$Lambda.0x000000000939d178.run:-1 | self=0.000 | inclusive=0.279 | selfSamples=0 inclusiveSamples=12
  systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.decodeHeader:487 | self=0.210 | inclusive=0.256 | selfSamples=9 inclusiveSamples=11
  systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.decodeDispatchHeader:558 | self=0.000 | inclusive=0.256 | selfSamples=0 inclusiveSamples=11
  systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec.decodeMultipartView:519 | self=0.233 | inclusive=0.233 | selfSamples=10 inclusiveSamples=10
  systems.zlink.framework.runtime.channels.ZLinkMeshApplicationDispatcher.accept:199 | self=0.000 | inclusive=0.210 | selfSamples=0 inclusiveSamples=9
  systems.zlink.framework.runtime.binding.ZLinkJavaRawServicePort.inStateLane:728 | self=0.000 | inclusive=0.210 | selfSamples=0 inclusiveSamples=9
  systems.zlink.framework.runtime.binding.ZLinkJavaRawMeshNode$1Rotation.get:5548 | self=0.186 | inclusive=0.186 | selfSamples=8 inclusiveSamples=8
  jdk.ThreadPark: 3559 (0.001883/msg)
  jdk.JavaMonitorWait: 30 (0.000016/msg)
  jdk.JavaMonitorEnter: 3409 (0.001803/msg)
 counts (strace and /proc, separate intrusive run)
  source: syscalls=2239634 (9.457/msg), futex=689602 (2.912/msg), context switches=878486 (3.710/msg)
  target: syscalls=2364518 (9.985/msg), futex=567746 (2.397/msg), context switches=2508163 (10.591/msg)

### Binding: plain=308449.4/s profiled=282802.5/s counts=36989.8/s profile_msgs=5,656,049
 source: proc CPU=10.350 us/msg; ExecutionSample=423; allocation samples=1940 (0.000343/msg), weighted bytes=3889.8 B/msg; primary=ZLINKbg/IO/0
  Thread | CPU µs/message | share of core | role
  ZLINKbg/IO/0 | 3.456 | 0.978 | native/core IO
  bench-stream-5 | 0.819 | 0.232 | JVM
  bench-stream-1 | 0.812 | 0.230 | JVM
  bench-stream-2 | 0.812 | 0.230 | JVM
  bench-stream-3 | 0.812 | 0.230 | JVM
  bench-stream-4 | 0.812 | 0.230 | JVM
  bench-stream-7 | 0.812 | 0.230 | JVM
  bench-stream-6 | 0.810 | 0.229 | JVM
  bench-stream-0 | 0.808 | 0.229 | JVM
  C2 CompilerThre | 0.083 | 0.024 | JVM
  bench-phase-active | 0.065 | 0.018 | JVM
  ForkJoinPool-1-worker-1 | 0.064 | 0.018 | JVM
  C1 CompilerThre | 0.046 | 0.013 | JVM
  HTTP-Dispatcher | 0.025 | 0.007 | JVM
  Attach Listener | 0.018 | 0.005 | JVM
  GC Thread#3 | 0.011 | 0.003 | JVM
  Process stage µs/message (self-sample classification × measured per-thread CPU; native unsampled CPU mapped by thread name)
  other | 4.432 | 42.8%
  binding/core | 3.877 | 37.5%
  codec | 1.019 | 9.8%
  benchmark driver | 0.573 | 5.5%
  handoff | 0.380 | 3.7%
  allocation | 0.069 | 0.7%
  Critical-thread stage µs/message (ZLINKbg/IO/0; no JFR samples means native CPU assigned to binding/core)
  binding/core | 3.456 | 100.0%
  Framework-only functions on critical thread, self/inclusive µs/message (inclusive rows overlap)
  None sampled: critical thread is native/core IO and has no Java stack frames.
  Busiest sampled Java Framework thread supplement: bench-stream-5 (0.819 µs/msg)
  jdk.ThreadPark: 1359 (0.000240/msg)
  jdk.JavaMonitorWait: 2 (0.000000/msg)
  jdk.JavaMonitorEnter: 1 (0.000000/msg)
 target: proc CPU=5.666 us/msg; ExecutionSample=219; allocation samples=1840 (0.000325/msg), weighted bytes=2019.2 B/msg; primary=ZLINKbg/IO/0
  Thread | CPU µs/message | share of core | role
  ZLINKbg/IO/0 | 2.907 | 0.822 | native/core IO
  bench-raw-command | 2.351 | 0.665 | JVM
  C2 CompilerThre | 0.092 | 0.026 | JVM
  bench-stats | 0.046 | 0.013 | JVM
  GC Thread#3 | 0.023 | 0.007 | JVM
  C1 CompilerThre | 0.021 | 0.006 | JVM
  GC Thread#6 | 0.021 | 0.006 | JVM
  Attach Listener | 0.021 | 0.006 | JVM
  GC Thread#0 | 0.019 | 0.005 | JVM
  GC Thread#1 | 0.019 | 0.005 | JVM
  GC Thread#2 | 0.019 | 0.005 | JVM
  GC Thread#4 | 0.019 | 0.005 | JVM
  GC Thread#5 | 0.019 | 0.005 | JVM
  GC Thread#7 | 0.019 | 0.005 | JVM
  bench-stats | 0.016 | 0.004 | JVM
  C2 CompilerThre | 0.012 | 0.004 | JVM
  Process stage µs/message (self-sample classification × measured per-thread CPU; native unsampled CPU mapped by thread name)
  binding/core | 3.216 | 56.7%
  other | 1.884 | 33.2%
  allocation | 0.322 | 5.7%
  benchmark driver | 0.245 | 4.3%
  Critical-thread stage µs/message (ZLINKbg/IO/0; no JFR samples means native CPU assigned to binding/core)
  binding/core | 2.907 | 100.0%
  Framework-only functions on critical thread, self/inclusive µs/message (inclusive rows overlap)
  None sampled: critical thread is native/core IO and has no Java stack frames.
  Busiest sampled Java Framework thread supplement: bench-raw-command (2.351 µs/msg)
  jdk.ThreadPark: 4 (0.000001/msg)
  jdk.JavaMonitorWait: 1 (0.000000/msg)
  jdk.JavaMonitorEnter: 1 (0.000000/msg)
 counts (strace and /proc, separate intrusive run)
  source: syscalls=662434 (0.891/msg), futex=186118 (0.250/msg), context switches=309371 (0.416/msg)
  target: syscalls=2309692 (3.107/msg), futex=825157 (1.110/msg), context switches=2420306 (3.256/msg)

## request-backpressure

### Framework: plain=49914.4/s profiled=47633.3/s counts=18486.9/s profile_msgs=953,216
 source: proc CPU=98.425 us/msg; ExecutionSample=911; allocation samples=1833 (0.001923/msg), weighted bytes=8795.2 B/msg; primary=bench-phase-active
  Thread | CPU µs/message | share of core | role
  bench-phase-active | 15.222 | 0.725 | JVM
  ZLINKbg/IO/0 | 14.456 | 0.689 | native/core IO
  ZLINKbg/IO/2 | 14.005 | 0.667 | native/core IO
  zlink-jvm-raw-mesh-bench | 13.428 | 0.640 | native/core IO
  C2 CompilerThre | 5.497 | 0.262 | JVM
  C2 CompilerThre | 4.868 | 0.232 | JVM
  C2 CompilerThre | 4.857 | 0.231 | JVM
  ForkJoinPool.commonPool-worker-4 | 3.378 | 0.161 | JVM
  ForkJoinPool.commonPool-worker-3 | 3.357 | 0.160 | JVM
  ForkJoinPool.commonPool-worker-1 | 3.336 | 0.159 | JVM
  ForkJoinPool.commonPool-worker-5 | 3.336 | 0.159 | JVM
  ForkJoinPool.commonPool-worker-6 | 3.336 | 0.159 | JVM
  ForkJoinPool.commonPool-worker-7 | 3.315 | 0.158 | JVM
  ForkJoinPool.commonPool-worker-2 | 3.263 | 0.155 | JVM
  C1 CompilerThre | 0.378 | 0.018 | JVM
  ForkJoinPool-1- | 0.294 | 0.014 | JVM
  Process stage µs/message (self-sample classification × measured per-thread CPU; native unsampled CPU mapped by thread name)
  binding/core | 36.596 | 37.2%
  other | 35.444 | 36.0%
  codec | 9.851 | 10.0%
  handoff | 6.333 | 6.4%
  task/future | 3.621 | 3.7%
  framework bookkeeping | 3.295 | 3.3%
  allocation | 2.200 | 2.2%
  benchmark driver | 1.085 | 1.1%
  Critical-thread stage µs/message (bench-phase-active; no JFR samples means native CPU assigned to binding/core)
  other | 7.201 | 47.3%
  codec | 2.139 | 14.1%
  binding/core | 1.676 | 11.0%
  framework bookkeeping | 1.212 | 8.0%
  allocation | 1.069 | 7.0%
  benchmark driver | 0.963 | 6.3%
  task/future | 0.642 | 4.2%
  handoff | 0.321 | 2.1%
  Framework-only functions on critical thread, self/inclusive µs/message (inclusive rows overlap)
  systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.field:356 | self=0.107 | inclusive=1.533 | selfSamples=3 inclusiveSamples=43
  systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.encodeHeaderBytesInternal:400 | self=0.000 | inclusive=1.355 | selfSamples=0 inclusiveSamples=38
  systems.zlink.framework.runtime.binding.ZLinkJavaRawSpotNode.lambda$requestToNode$0:345 | self=0.000 | inclusive=1.212 | selfSamples=0 inclusiveSamples=34
  systems.zlink.framework.runtime.binding.ZLinkJavaRawMeshNode.requestNode:2389 | self=0.000 | inclusive=1.176 | selfSamples=0 inclusiveSamples=33
  systems.zlink.framework.runtime.binding.ZLinkJavaRawMeshNode.encodeApplicationFrames:5212 | self=0.000 | inclusive=1.069 | selfSamples=0 inclusiveSamples=30
  systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.jsonString:270 | self=0.963 | inclusive=0.963 | selfSamples=27 inclusiveSamples=27
  systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.encodeHeaderBytesUncached:442 | self=0.000 | inclusive=0.891 | selfSamples=0 inclusiveSamples=25
  systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.encodeHeaderBytesUncached:446 | self=0.071 | inclusive=0.820 | selfSamples=2 inclusiveSamples=23
  systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.writeHeader:373 | self=0.000 | inclusive=0.784 | selfSamples=0 inclusiveSamples=22
  systems.zlink.framework.runtime.internal.execution.ZLinkStateLane.isOnLane:111 | self=0.000 | inclusive=0.713 | selfSamples=0 inclusiveSamples=20
  systems.zlink.framework.runtime.binding.ZLinkJavaRawMeshNode.observePeerRoute:8694 | self=0.000 | inclusive=0.606 | selfSamples=0 inclusiveSamples=17
  systems.zlink.framework.runtime.channels.RouteRequestCall.submit:360 | self=0.036 | inclusive=0.606 | selfSamples=1 inclusiveSamples=17
  systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec.encodeFrameworkMultipartMessage:358 | self=0.000 | inclusive=0.606 | selfSamples=0 inclusiveSamples=17
  systems.zlink.framework.runtime.binding.ZLinkJavaRawMeshNode.lambda$admitRequest$2:8871 | self=0.000 | inclusive=0.463 | selfSamples=0 inclusiveSamples=13
  systems.zlink.framework.runtime.messaging.ZLinkPayloadEncoding.encode:28 | self=0.000 | inclusive=0.463 | selfSamples=0 inclusiveSamples=13
  systems.zlink.framework.runtime.internal.service.ZLinkServiceLivenessRegistry.connectionFromOwner:83 | self=0.000 | inclusive=0.463 | selfSamples=0 inclusiveSamples=13
  systems.zlink.framework.runtime.binding.ZLinkJavaRawMeshNode$$Lambda.0x000000008b3fe940.apply:-1 | self=0.000 | inclusive=0.463 | selfSamples=0 inclusiveSamples=13
  systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec.encodeFrameworkMultipartMessage:361 | self=0.000 | inclusive=0.428 | selfSamples=0 inclusiveSamples=12
  systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationRegistry.registerEntry:733 | self=0.000 | inclusive=0.428 | selfSamples=0 inclusiveSamples=12
  systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationRegistry.lambda$submitTerminal$0:362 | self=0.000 | inclusive=0.428 | selfSamples=0 inclusiveSamples=12
  jdk.ThreadPark: 13356 (0.014012/msg)
  jdk.JavaMonitorWait: 38 (0.000040/msg)
  jdk.JavaMonitorEnter: 2341 (0.002456/msg)
 target: proc CPU=68.809 us/msg; ExecutionSample=1637; allocation samples=1811 (0.001900/msg), weighted bytes=8201.8 B/msg; primary=ZLINKbg/IO/1
  Thread | CPU µs/message | share of core | role
  ZLINKbg/IO/1 | 15.191 | 0.724 | native/core IO
  zlink-jvm-raw-mesh-bench | 13.974 | 0.666 | native/core IO
  ZLINKbg/IO/0 | 10.973 | 0.523 | native/core IO
  ForkJoinPool-1-worker-3 | 6.662 | 0.317 | JVM
  ForkJoinPool-1-worker-1 | 6.641 | 0.316 | JVM
  ForkJoinPool-1-worker-2 | 4.742 | 0.226 | JVM
  C2 CompilerThre | 3.399 | 0.162 | JVM
  C2 CompilerThre | 2.717 | 0.129 | JVM
  C2 CompilerThre | 2.675 | 0.127 | JVM
  C1 CompilerThre | 0.262 | 0.012 | JVM
  bench-stats | 0.168 | 0.008 | JVM
  Attach Listener | 0.147 | 0.007 | JVM
  GC Thread#0 | 0.136 | 0.006 | JVM
  GC Thread#1 | 0.126 | 0.006 | JVM
  GC Thread#5 | 0.126 | 0.006 | JVM
  GC Thread#7 | 0.126 | 0.006 | JVM
  Process stage µs/message (self-sample classification × measured per-thread CPU; native unsampled CPU mapped by thread name)
  binding/core | 30.157 | 43.8%
  other | 21.323 | 31.0%
  handoff | 10.405 | 15.1%
  codec | 4.756 | 6.9%
  allocation | 1.076 | 1.6%
  framework bookkeeping | 1.051 | 1.5%
  benchmark driver | 0.024 | 0.0%
  task/future | 0.018 | 0.0%
  Critical-thread stage µs/message (ZLINKbg/IO/1; no JFR samples means native CPU assigned to binding/core)
  binding/core | 15.191 | 100.0%
  Framework-only functions on critical thread, self/inclusive µs/message (inclusive rows overlap)
  None sampled: critical thread is native/core IO and has no Java stack frames.
  Busiest sampled Java Framework thread supplement: zlink-jvm-raw-mesh-bench (13.974 µs/msg)
  systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.decodeHeader:493 | self=0.184 | inclusive=4.148 | selfSamples=10 inclusiveSamples=225
  systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.decodeDispatchHeader:558 | self=0.000 | inclusive=3.540 | selfSamples=0 inclusiveSamples=192
  systems.zlink.framework.runtime.channels.ZLinkMeshApplicationDispatcher.accept:199 | self=0.000 | inclusive=2.489 | selfSamples=0 inclusiveSamples=135
  systems.zlink.framework.runtime.channels.ZLinkMeshApplicationDispatcher.accept:65 | self=0.037 | inclusive=1.641 | selfSamples=2 inclusiveSamples=89
  systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.decodeHeader:740 | self=0.018 | inclusive=1.180 | selfSamples=1 inclusiveSamples=64
  systems.zlink.framework.runtime.binding.ZLinkJavaRawMeshNode$1Rotation.dispatchClaimed:5587 | self=0.000 | inclusive=1.051 | selfSamples=0 inclusiveSamples=57
  systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.decodeHeader:885 | self=0.203 | inclusive=0.811 | selfSamples=11 inclusiveSamples=44
  systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.decodeHeader:745 | self=0.092 | inclusive=0.793 | selfSamples=5 inclusiveSamples=43
  systems.zlink.framework.runtime.binding.ZLinkJavaRawMeshNode.drainIngressBatch:5617 | self=0.000 | inclusive=0.719 | selfSamples=0 inclusiveSamples=39
  systems.zlink.framework.runtime.binding.ZLinkJavaRawMeshNode.dispatch:6058 | self=0.000 | inclusive=0.498 | selfSamples=0 inclusiveSamples=27
  systems.zlink.framework.runtime.binding.ZLinkJavaRawMeshNode.pumpTurn:5430 | self=0.000 | inclusive=0.479 | selfSamples=0 inclusiveSamples=26
  systems.zlink.framework.runtime.binding.ZLinkJavaRawMeshNode.lambda$startPump$0:5396 | self=0.000 | inclusive=0.442 | selfSamples=0 inclusiveSamples=24
  systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.decodeHeader:892 | self=0.350 | inclusive=0.387 | selfSamples=19 inclusiveSamples=21
  systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec.decodeFrameworkMultipartFrameView:465 | self=0.000 | inclusive=0.369 | selfSamples=0 inclusiveSamples=20
  systems.zlink.framework.runtime.binding.ZLinkJavaRawMeshNode.dispatch:5973 | self=0.000 | inclusive=0.369 | selfSamples=0 inclusiveSamples=20
  systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec.decodeFrameworkMultipartFrameView:504 | self=0.055 | inclusive=0.258 | selfSamples=3 inclusiveSamples=14
  systems.zlink.framework.runtime.internal.execution.ZLinkStateLane.runSynchronousTurn:142 | self=0.018 | inclusive=0.258 | selfSamples=1 inclusiveSamples=14
  systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.decodeHeader:951 | self=0.000 | inclusive=0.258 | selfSamples=0 inclusiveSamples=14
  systems.zlink.framework.runtime.channels.ZLinkMeshApplicationDispatcher.accept:299 | self=0.258 | inclusive=0.258 | selfSamples=14 inclusiveSamples=14
  systems.zlink.framework.runtime.internal.execution.ZLinkStateLane.callWithCurrent:424 | self=0.037 | inclusive=0.240 | selfSamples=2 inclusiveSamples=13
  jdk.ThreadPark: 5041 (0.005288/msg)
  jdk.JavaMonitorWait: 32 (0.000034/msg)
  jdk.JavaMonitorEnter: 24 (0.000025/msg)
 counts (strace and /proc, separate intrusive run)
  source: syscalls=2706664 (7.283/msg), futex=920385 (2.477/msg), context switches=2866216 (7.712/msg)
  target: syscalls=1820196 (4.898/msg), futex=389984 (1.049/msg), context switches=1945546 (5.235/msg)

### Binding: plain=122744.0/s profiled=114368.0/s counts=4252.9/s profile_msgs=2,287,368
 source: proc CPU=24.146 us/msg; ExecutionSample=190; allocation samples=1928 (0.000843/msg), weighted bytes=5985.1 B/msg; primary=bench-phase-active
  Thread | CPU µs/message | share of core | role
  bench-phase-active | 8.293 | 0.948 | JVM
  ZLINKbg/IO/0 | 8.114 | 0.928 | native/core IO
  ZLINKbg/IO/2 | 6.654 | 0.761 | native/core IO
  C2 CompilerThre | 0.210 | 0.024 | JVM
  ForkJoinPool-1-worker-1 | 0.153 | 0.017 | JVM
  C2 CompilerThre | 0.144 | 0.016 | JVM
  C2 CompilerThre | 0.118 | 0.013 | JVM
  C1 CompilerThre | 0.114 | 0.013 | JVM
  HTTP-Dispatcher | 0.061 | 0.007 | JVM
  Attach Listener | 0.044 | 0.005 | JVM
  GC Thread#1 | 0.031 | 0.003 | JVM
  GC Thread#7 | 0.031 | 0.003 | JVM
  GC Thread#0 | 0.026 | 0.003 | JVM
  GC Thread#3 | 0.026 | 0.003 | JVM
  GC Thread#4 | 0.026 | 0.003 | JVM
  GC Thread#5 | 0.026 | 0.003 | JVM
  Process stage µs/message (self-sample classification × measured per-thread CPU; native unsampled CPU mapped by thread name)
  binding/core | 15.926 | 66.0%
  other | 5.255 | 21.8%
  codec | 1.549 | 6.4%
  benchmark driver | 0.649 | 2.7%
  task/future | 0.400 | 1.7%
  allocation | 0.214 | 0.9%
  handoff | 0.153 | 0.6%
  Critical-thread stage µs/message (bench-phase-active; no JFR samples means native CPU assigned to binding/core)
  other | 4.546 | 54.8%
  codec | 1.549 | 18.7%
  binding/core | 1.149 | 13.9%
  benchmark driver | 0.649 | 7.8%
  task/future | 0.400 | 4.8%
  Framework-only functions on critical thread, self/inclusive µs/message (inclusive rows overlap)
  jdk.ThreadPark: 365 (0.000160/msg)
  jdk.JavaMonitorWait: 2 (0.000001/msg)
  jdk.JavaMonitorEnter: 1 (0.000000/msg)
 target: proc CPU=22.878 us/msg; ExecutionSample=339; allocation samples=1861 (0.000814/msg), weighted bytes=4680.3 B/msg; primary=ZLINKbg/IO/1
  Thread | CPU µs/message | share of core | role
  ZLINKbg/IO/1 | 7.659 | 0.876 | native/core IO
  ZLINKbg/IO/0 | 7.498 | 0.857 | native/core IO
  bench-raw-request | 6.816 | 0.779 | JVM
  C2 CompilerThre | 0.149 | 0.017 | JVM
  bench-stats | 0.109 | 0.012 | JVM
  GC Thread#3 | 0.057 | 0.006 | JVM
  GC Thread#7 | 0.057 | 0.006 | JVM
  GC Thread#0 | 0.052 | 0.006 | JVM
  C1 CompilerThre | 0.052 | 0.006 | JVM
  GC Thread#2 | 0.052 | 0.006 | JVM
  GC Thread#5 | 0.052 | 0.006 | JVM
  Attach Listener | 0.052 | 0.006 | JVM
  GC Thread#4 | 0.048 | 0.005 | JVM
  GC Thread#6 | 0.048 | 0.005 | JVM
  GC Thread#1 | 0.044 | 0.005 | JVM
  bench-stats | 0.039 | 0.004 | JVM
  Process stage µs/message (self-sample classification × measured per-thread CPU; native unsampled CPU mapped by thread name)
  binding/core | 16.525 | 72.2%
  other | 4.587 | 20.0%
  codec | 0.904 | 4.0%
  allocation | 0.689 | 3.0%
  benchmark driver | 0.173 | 0.8%
  Critical-thread stage µs/message (ZLINKbg/IO/1; no JFR samples means native CPU assigned to binding/core)
  binding/core | 7.659 | 100.0%
  Framework-only functions on critical thread, self/inclusive µs/message (inclusive rows overlap)
  None sampled: critical thread is native/core IO and has no Java stack frames.
  Busiest sampled Java Framework thread supplement: bench-raw-request (6.816 µs/msg)
  jdk.ThreadPark: 4 (0.000002/msg)
  jdk.JavaMonitorWait: 1 (0.000000/msg)
  jdk.JavaMonitorEnter: 1 (0.000000/msg)
 counts (strace and /proc, separate intrusive run)
  source: syscalls=2366536 (27.822/msg), futex=167220 (1.966/msg), context switches=1399181 (16.449/msg)
  target: syscalls=2209722 (25.978/msg), futex=539797 (6.346/msg), context switches=2382348 (28.008/msg)

## request-serial

### Framework: plain=6688.7/s profiled=5463.8/s counts=975.2/s profile_msgs=109,277
 source: proc CPU=241.130 us/msg; ExecutionSample=179; allocation samples=1901 (0.017396/msg), weighted bytes=9513.1 B/msg; primary=C2 CompilerThre
  Thread | CPU µs/message | share of core | role
  C2 CompilerThre | 37.611 | 0.205 | JVM
  ZLINKbg/IO/2 | 29.741 | 0.162 | native/core IO
  C2 CompilerThre | 28.734 | 0.157 | JVM
  C2 CompilerThre | 28.277 | 0.154 | JVM
  zlink-jvm-raw-mesh-bench | 25.989 | 0.142 | native/core IO
  ZLINKbg/IO/0 | 25.714 | 0.140 | native/core IO
  bench-stream-0 | 24.708 | 0.135 | JVM
  ForkJoinPool.commonPool-worker-1 | 22.146 | 0.121 | JVM
  C2 CompilerThre | 2.471 | 0.013 | JVM
  C1 CompilerThre | 2.288 | 0.012 | JVM
  C2 CompilerThre | 1.922 | 0.010 | JVM
  C2 CompilerThre | 1.647 | 0.009 | JVM
  ForkJoinPool-1- | 1.190 | 0.006 | JVM
  ForkJoinPool-1- | 1.190 | 0.006 | JVM
  HTTP-Dispatcher | 1.098 | 0.006 | JVM
  ForkJoinPool-1- | 0.915 | 0.005 | JVM
  Process stage µs/message (self-sample classification × measured per-thread CPU; native unsampled CPU mapped by thread name)
  other | 133.269 | 55.3%
  binding/core | 64.578 | 26.8%
  handoff | 17.894 | 7.4%
  framework bookkeeping | 11.677 | 4.8%
  codec | 5.508 | 2.3%
  allocation | 4.759 | 2.0%
  task/future | 2.678 | 1.1%
  benchmark driver | 0.768 | 0.3%
  Critical-thread stage µs/message (C2 CompilerThre; no JFR samples means native CPU assigned to binding/core)
  binding/core | 37.611 | 100.0%
  Framework-only functions on critical thread, self/inclusive µs/message (inclusive rows overlap)
  None sampled: critical thread is native/core IO and has no Java stack frames.
  Busiest sampled Java Framework thread supplement: zlink-jvm-raw-mesh-bench (25.989 µs/msg)
  systems.zlink.framework.runtime.binding.ZLinkJavaRawMeshNode.lambda$startPump$0:5396 | self=0.000 | inclusive=7.277 | selfSamples=0 inclusiveSamples=7
  systems.zlink.framework.runtime.binding.ZLinkJavaRawMeshNode$$Lambda.0x0000000096424230.run:-1 | self=0.000 | inclusive=5.198 | selfSamples=0 inclusiveSamples=5
  systems.zlink.framework.runtime.binding.ZLinkJavaRawMeshNode.lambda$request$2:5178 | self=1.040 | inclusive=3.119 | selfSamples=1 inclusiveSamples=3
  systems.zlink.framework.runtime.binding.ZLinkJavaRawMeshNode$$Lambda.0x00000000964b0d48.apply:-1 | self=0.000 | inclusive=3.119 | selfSamples=0 inclusiveSamples=3
  systems.zlink.framework.runtime.binding.ZLinkJavaSocketReceivePoller.waitForOwnerReady:150 | self=0.000 | inclusive=3.119 | selfSamples=0 inclusiveSamples=3
  systems.zlink.framework.runtime.binding.ZLinkJavaRawMeshNode.pumpTurn:5416 | self=1.040 | inclusive=3.119 | selfSamples=1 inclusiveSamples=3
  systems.zlink.framework.runtime.internal.service.ZLinkServiceCompletionDispatcher.post:53 | self=0.000 | inclusive=2.079 | selfSamples=0 inclusiveSamples=2
  systems.zlink.framework.runtime.binding.ZLinkJavaRawServicePort.deliverTerminal:491 | self=0.000 | inclusive=2.079 | selfSamples=0 inclusiveSamples=2
  systems.zlink.framework.runtime.binding.ZLinkJavaRawServicePort$$Lambda.0x00000000964b3740.accept:-1 | self=0.000 | inclusive=2.079 | selfSamples=0 inclusiveSamples=2
  systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec.decodeFrameworkMultipartFrameView:474 | self=1.040 | inclusive=2.079 | selfSamples=1 inclusiveSamples=2
  systems.zlink.framework.runtime.binding.ZLinkJavaRawMeshNode.decodeRequestReply:5260 | self=0.000 | inclusive=2.079 | selfSamples=0 inclusiveSamples=2
  systems.zlink.framework.runtime.binding.ZLinkJavaSocketSupport$$Lambda.0x00000000964b3980.accept:-1 | self=1.040 | inclusive=2.079 | selfSamples=1 inclusiveSamples=2
  systems.zlink.framework.runtime.binding.ZLinkJavaRawServicePort.waitForManagementReady:589 | self=0.000 | inclusive=2.079 | selfSamples=0 inclusiveSamples=2
  systems.zlink.framework.runtime.binding.ZLinkJavaRawMeshNode.pumpTurn:5432 | self=1.040 | inclusive=2.079 | selfSamples=1 inclusiveSamples=2
  systems.zlink.framework.runtime.binding.ZLinkJavaRawMeshNode.refreshAdmissionPending:8535 | self=0.000 | inclusive=2.079 | selfSamples=0 inclusiveSamples=2
  systems.zlink.framework.runtime.binding.ZLinkJavaRawMeshNode.tickLivenessOnLane:8560 | self=0.000 | inclusive=2.079 | selfSamples=0 inclusiveSamples=2
  systems.zlink.framework.runtime.binding.ZLinkJavaRawMeshNode.pumpTurn:5412 | self=2.079 | inclusive=2.079 | selfSamples=2 inclusiveSamples=2
  systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationRegistry.complete:407 | self=0.000 | inclusive=1.040 | selfSamples=0 inclusiveSamples=1
  systems.zlink.framework.runtime.binding.ZLinkJavaRawServicePort.lambda$requestMessagesTo$1:459 | self=0.000 | inclusive=1.040 | selfSamples=0 inclusiveSamples=1
  systems.zlink.framework.runtime.binding.ZLinkJavaRawMeshNode.decodeRequestReply:5250 | self=0.000 | inclusive=1.040 | selfSamples=0 inclusiveSamples=1
  jdk.ThreadPark: 1168 (0.010688/msg)
  jdk.JavaMonitorWait: 28 (0.000256/msg)
  jdk.JavaMonitorEnter: 389 (0.003560/msg)
 target: proc CPU=199.310 us/msg; ExecutionSample=182; allocation samples=1855 (0.016975/msg), weighted bytes=10414.7 B/msg; primary=C2 CompilerThre
  Thread | CPU µs/message | share of core | role
  C2 CompilerThre | 36.970 | 0.202 | JVM
  zlink-jvm-raw-mesh-bench | 28.368 | 0.155 | native/core IO
  C2 CompilerThre | 27.728 | 0.151 | JVM
  C2 CompilerThre | 25.623 | 0.140 | JVM
  ZLINKbg/IO/1 | 23.335 | 0.127 | native/core IO
  ForkJoinPool-1-worker-1 | 17.387 | 0.095 | JVM
  ForkJoinPool-1-worker-2 | 14.733 | 0.080 | JVM
  ZLINKbg/IO/0 | 14.001 | 0.076 | native/core IO
  C1 CompilerThre | 1.373 | 0.007 | JVM
  C2 CompilerThre | 1.373 | 0.007 | JVM
  Common-Cleaner | 0.915 | 0.005 | JVM
  Attach Listener | 0.824 | 0.004 | JVM
  bench-stats | 0.732 | 0.004 | JVM
  GC Thread#2 | 0.641 | 0.003 | JVM
  GC Thread#3 | 0.641 | 0.003 | JVM
  GC Thread#5 | 0.549 | 0.003 | JVM
  Process stage µs/message (self-sample classification × measured per-thread CPU; native unsampled CPU mapped by thread name)
  other | 108.471 | 54.4%
  binding/core | 42.164 | 21.2%
  handoff | 32.116 | 16.1%
  codec | 10.157 | 5.1%
  allocation | 4.301 | 2.2%
  framework bookkeeping | 2.101 | 1.1%
  Critical-thread stage µs/message (C2 CompilerThre; no JFR samples means native CPU assigned to binding/core)
  binding/core | 36.970 | 100.0%
  Framework-only functions on critical thread, self/inclusive µs/message (inclusive rows overlap)
  None sampled: critical thread is native/core IO and has no Java stack frames.
  Busiest sampled Java Framework thread supplement: zlink-jvm-raw-mesh-bench (28.368 µs/msg)
  systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.decodeHeader:493 | self=0.350 | inclusive=5.253 | selfSamples=1 inclusiveSamples=15
  systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.decodeDispatchHeader:558 | self=0.000 | inclusive=4.903 | selfSamples=0 inclusiveSamples=14
  systems.zlink.framework.runtime.channels.ZLinkMeshApplicationDispatcher.accept:65 | self=0.000 | inclusive=3.502 | selfSamples=0 inclusiveSamples=10
  systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.decodeHeader:740 | self=0.000 | inclusive=3.152 | selfSamples=0 inclusiveSamples=9
  systems.zlink.framework.runtime.binding.ZLinkJavaRawMeshNode.lambda$startPump$0:5396 | self=0.000 | inclusive=2.802 | selfSamples=0 inclusiveSamples=8
  systems.zlink.framework.runtime.channels.ZLinkMeshApplicationDispatcher.accept:199 | self=0.000 | inclusive=2.802 | selfSamples=0 inclusiveSamples=8
  systems.zlink.framework.runtime.binding.ZLinkJavaRawServicePort.lambda$receiveNow$0:618 | self=0.000 | inclusive=1.401 | selfSamples=0 inclusiveSamples=4
  systems.zlink.framework.runtime.binding.ZLinkJavaRawServicePort.inStateLane:728 | self=0.350 | inclusive=1.401 | selfSamples=1 inclusiveSamples=4
  systems.zlink.framework.runtime.binding.ZLinkJavaRawMeshNode$$Lambda.0x000000008f39cf50.run:-1 | self=0.000 | inclusive=1.401 | selfSamples=0 inclusiveSamples=4
  systems.zlink.framework.runtime.binding.ZLinkJavaRawMeshNode.pumpTurn:5416 | self=0.000 | inclusive=1.051 | selfSamples=0 inclusiveSamples=3
  systems.zlink.framework.runtime.channels.ZLinkMeshApplicationDispatcher.accept:288 | self=0.000 | inclusive=1.051 | selfSamples=0 inclusiveSamples=3
  systems.zlink.framework.runtime.binding.ZLinkJavaRawMeshNode.pumpTurn:5430 | self=0.000 | inclusive=1.051 | selfSamples=0 inclusiveSamples=3
  systems.zlink.framework.runtime.binding.ZLinkJavaRawServicePort$$Lambda.0x000000008f3d4a78.get:-1 | self=0.000 | inclusive=1.051 | selfSamples=0 inclusiveSamples=3
  systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext.enter:87 | self=0.000 | inclusive=1.051 | selfSamples=0 inclusiveSamples=3
  systems.zlink.framework.runtime.binding.ZLinkJavaRawMeshNode.pumpTurn:5412 | self=1.051 | inclusive=1.051 | selfSamples=3 inclusiveSamples=3
  systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.decodeHeader:704 | self=0.000 | inclusive=1.051 | selfSamples=0 inclusiveSamples=3
  systems.zlink.framework.runtime.binding.ZLinkJavaRawServicePort.waitForManagementReady:589 | self=0.000 | inclusive=0.700 | selfSamples=0 inclusiveSamples=2
  systems.zlink.framework.runtime.binding.ZLinkJavaRawServicePort.receiveOnLane:652 | self=0.000 | inclusive=0.700 | selfSamples=0 inclusiveSamples=2
  systems.zlink.framework.runtime.channels.ZLinkMeshApplicationDispatcher.dispatchRequest:602 | self=0.000 | inclusive=0.700 | selfSamples=0 inclusiveSamples=2
  systems.zlink.framework.execution.ZLinkSerialExecutionQueue.requestDrainLocked:1868 | self=0.350 | inclusive=0.700 | selfSamples=1 inclusiveSamples=2
  jdk.ThreadPark: 397 (0.003633/msg)
  jdk.JavaMonitorWait: 30 (0.000275/msg)
  jdk.JavaMonitorEnter: 225 (0.002059/msg)
 counts (strace and /proc, separate intrusive run)
  source: syscalls=1255216 (64.350/msg), futex=131704 (6.752/msg), context switches=1234373 (63.282/msg)
  target: syscalls=656682 (33.666/msg), futex=77012 (3.948/msg), context switches=733860 (37.622/msg)

### Binding: plain=14344.3/s profiled=12353.3/s counts=1606.3/s profile_msgs=247,067
 source: proc CPU=55.936 us/msg; ExecutionSample=43; allocation samples=801 (0.003242/msg), weighted bytes=6126.1 B/msg; primary=bench-stream-0
  Thread | CPU µs/message | share of core | role
  bench-stream-0 | 18.214 | 0.225 | JVM
  ZLINKbg/IO/0 | 15.704 | 0.194 | native/core IO
  ZLINKbg/IO/2 | 15.380 | 0.190 | native/core IO
  C2 CompilerThre | 1.902 | 0.023 | JVM
  ForkJoinPool-1- | 1.295 | 0.016 | JVM
  C1 CompilerThre | 0.890 | 0.011 | JVM
  C2 CompilerThre | 0.850 | 0.010 | JVM
  HTTP-Dispatcher | 0.486 | 0.006 | JVM
  Attach Listener | 0.364 | 0.004 | JVM
  GC Thread#0 | 0.081 | 0.001 | JVM
  GC Thread#1 | 0.081 | 0.001 | JVM
  GC Thread#2 | 0.081 | 0.001 | JVM
  GC Thread#4 | 0.081 | 0.001 | JVM
  GC Thread#5 | 0.081 | 0.001 | JVM
  GC Thread#6 | 0.081 | 0.001 | JVM
  GC Thread#7 | 0.081 | 0.001 | JVM
  Process stage µs/message (self-sample classification × measured per-thread CPU; native unsampled CPU mapped by thread name)
  binding/core | 36.149 | 64.6%
  other | 12.557 | 22.4%
  benchmark driver | 3.312 | 5.9%
  codec | 1.656 | 3.0%
  task/future | 1.656 | 3.0%
  allocation | 0.607 | 1.1%
  Critical-thread stage µs/message (bench-stream-0; no JFR samples means native CPU assigned to binding/core)
  other | 6.623 | 36.4%
  binding/core | 4.967 | 27.3%
  benchmark driver | 3.312 | 18.2%
  codec | 1.656 | 9.1%
  task/future | 1.656 | 9.1%
  Framework-only functions on critical thread, self/inclusive µs/message (inclusive rows overlap)
  jdk.ThreadPark: 432 (0.001749/msg)
  jdk.JavaMonitorWait: 2 (0.000008/msg)
  jdk.JavaMonitorEnter: 2 (0.000008/msg)
 target: proc CPU=46.748 us/msg; ExecutionSample=39; allocation samples=640 (0.002590/msg), weighted bytes=4996.5 B/msg; primary=ZLINKbg/IO/1
  Thread | CPU µs/message | share of core | role
  ZLINKbg/IO/1 | 15.340 | 0.189 | native/core IO
  ZLINKbg/IO/0 | 15.057 | 0.186 | native/core IO
  bench-raw-request | 12.062 | 0.149 | JVM
  C2 CompilerThre | 1.417 | 0.017 | JVM
  bench-stats | 0.648 | 0.008 | JVM
  C2 CompilerThre | 0.526 | 0.006 | JVM
  C1 CompilerThre | 0.405 | 0.005 | JVM
  Attach Listener | 0.324 | 0.004 | JVM
  C2 CompilerThre | 0.324 | 0.004 | JVM
  bench-stats | 0.283 | 0.003 | JVM
  GC Thread#0 | 0.040 | 0.000 | JVM
  GC Thread#1 | 0.040 | 0.000 | JVM
  GC Thread#2 | 0.040 | 0.000 | JVM
  GC Thread#3 | 0.040 | 0.000 | JVM
  GC Thread#4 | 0.040 | 0.000 | JVM
  GC Thread#5 | 0.040 | 0.000 | JVM
  Process stage µs/message (self-sample classification × measured per-thread CPU; native unsampled CPU mapped by thread name)
  binding/core | 32.809 | 70.2%
  other | 9.966 | 21.3%
  benchmark driver | 2.443 | 5.2%
  codec | 1.206 | 2.6%
  allocation | 0.324 | 0.7%
  Critical-thread stage µs/message (ZLINKbg/IO/1; no JFR samples means native CPU assigned to binding/core)
  binding/core | 15.340 | 100.0%
  Framework-only functions on critical thread, self/inclusive µs/message (inclusive rows overlap)
  None sampled: critical thread is native/core IO and has no Java stack frames.
  Busiest sampled Java Framework thread supplement: bench-raw-request (12.062 µs/msg)
  jdk.ThreadPark: 4 (0.000016/msg)
  jdk.JavaMonitorWait: 1 (0.000004/msg)
  jdk.JavaMonitorEnter: 1 (0.000004/msg)
 counts (strace and /proc, separate intrusive run)
  source: syscalls=1288890 (40.119/msg), futex=6370 (0.198/msg), context switches=603976 (18.800/msg)
  target: syscalls=953238 (29.671/msg), futex=225941 (7.033/msg), context switches=1048447 (32.634/msg)


## Lock and GC event breakdown

```text

## send-saturation

 zlink-framework-java msgs 1890395
  source GC 0 per 1M 0.0 monitor enter starts 1 per 1M 0.53
    1 jdk.jfr.internal.PlatformRecorder @ jdk.jfr.internal.PlatformRecorder.periodicTask
  target GC 0 per 1M 0.0 monitor enter starts 3409 per 1M 1803.33
    3389 java.lang.ref.ReferenceQueue$Lock @ java.lang.ref.ReferenceQueue.remove
    16 systems.zlink.framework.execution.ZLinkSerialExecutionQueue @ systems.zlink.framework.execution.ZLinkSerialExecutionQueue.enqueue
    1 jdk.jfr.internal.PlatformRecorder @ jdk.jfr.internal.PlatformRecorder.periodicTask
    1 java.lang.ref.ReferenceQueue$Lock @ java.lang.ref.ReferenceQueue.enqueue
    1 java.lang.Object @ systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue.reserveRotationBudget
    1 systems.zlink.framework.runtime.internal.drain.ZLinkMeshDrainCoordinator$MeshState @ systems.zlink.framework.runtime.internal.drain.ZLinkMeshDrainCoordinator.isSealed

 zlink-java msgs 5656049
  source GC 0 per 1M 0.0 monitor enter starts 1 per 1M 0.18
    1 jdk.jfr.internal.PlatformRecorder @ jdk.jfr.internal.PlatformRecorder.periodicTask
  target GC 0 per 1M 0.0 monitor enter starts 1 per 1M 0.18
    1 jdk.jfr.internal.PlatformRecorder @ jdk.jfr.internal.PlatformRecorder.periodicTask

## request-backpressure

 zlink-framework-java msgs 953216
  source GC 0 per 1M 0.0 monitor enter starts 2341 per 1M 2455.9
    2117 java.lang.ref.ReferenceQueue$Lock @ java.lang.ref.ReferenceQueue.remove
    183 jdk.internal.ref.CleanerImpl$CleanableList @ jdk.internal.ref.CleanerImpl$CleanableList.insert
    14 java.lang.Object @ systems.zlink.runtime.sockets.CompletionOwner.hasPublicOwner
    14 java.lang.Object @ systems.zlink.runtime.sockets.CompletionOwner.capture
    7 jdk.internal.ref.CleanerImpl$CleanableList @ jdk.internal.ref.CleanerImpl$CleanableList.isEmpty
    3 java.lang.Object @ sun.nio.ch.EPollSelectorImpl.clearInterrupt
    1 jdk.internal.ref.CleanerImpl$CleanableList @ jdk.internal.ref.CleanerImpl$CleanableList.remove
    1 java.lang.Object @ systems.zlink.runtime.sockets.CompletionOwner.register
  target GC 0 per 1M 0.0 monitor enter starts 24 per 1M 25.18
    17 systems.zlink.framework.execution.ZLinkSerialExecutionQueue @ systems.zlink.framework.execution.ZLinkSerialExecutionQueue.enqueue
    3 java.lang.Object @ systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue.queued
    1 jdk.jfr.internal.PlatformRecorder @ jdk.jfr.internal.PlatformRecorder.periodicTask
    1 java.lang.Object @ systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue.reserveAvailable
    1 systems.zlink.framework.runtime.internal.drain.ZLinkMeshDrainCoordinator$MeshState @ systems.zlink.framework.runtime.internal.drain.ZLinkMeshDrainCoordinator.isSealed
    1 java.lang.Object @ systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue.reserveRotationBudget

 zlink-java msgs 2287368
  source GC 0 per 1M 0.0 monitor enter starts 1 per 1M 0.44
    1 jdk.jfr.internal.PlatformRecorder @ jdk.jfr.internal.PlatformRecorder.periodicTask
  target GC 0 per 1M 0.0 monitor enter starts 1 per 1M 0.44
    1 jdk.jfr.internal.PlatformRecorder @ jdk.jfr.internal.PlatformRecorder.periodicTask

## request-serial

 zlink-framework-java msgs 109277
  source GC 0 per 1M 0.0 monitor enter starts 389 per 1M 3559.76
    387 java.lang.ref.ReferenceQueue$Lock @ java.lang.ref.ReferenceQueue.remove
    1 jdk.jfr.internal.PlatformRecorder @ jdk.jfr.internal.PlatformRecorder.periodicTask
    1 jdk.internal.ref.CleanerImpl$CleanableList @ jdk.internal.ref.CleanerImpl$CleanableList.isEmpty
  target GC 0 per 1M 0.0 monitor enter starts 225 per 1M 2058.99
    223 java.lang.ref.ReferenceQueue$Lock @ java.lang.ref.ReferenceQueue.remove
    1 jdk.jfr.internal.PlatformRecorder @ jdk.jfr.internal.PlatformRecorder.periodicTask
    1 java.lang.ref.ReferenceQueue$Lock @ java.lang.ref.ReferenceQueue.enqueue

 zlink-java msgs 247067
  source GC 0 per 1M 0.0 monitor enter starts 2 per 1M 8.09
    1 jdk.jfr.internal.PlatformRecorder @ jdk.jfr.internal.PlatformRecorder.periodicTask
    1 java.lang.Object @ sun.nio.ch.EPollSelectorImpl.clearInterrupt
  target GC 0 per 1M 0.0 monitor enter starts 1 per 1M 4.05
    1 jdk.jfr.internal.PlatformRecorder @ jdk.jfr.internal.PlatformRecorder.periodicTask

```
