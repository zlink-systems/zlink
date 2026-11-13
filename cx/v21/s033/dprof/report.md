# .NET Framework vs binding: request-path profile (issue #3869)

## Result

The current profile does **not** reproduce the old `ZLinkProcessExecutionPool._gate` contention rate. The code changed in `bbb734e1b3` (#3413): it replaced the process-wide `Monitor.PulseAll/Wait` worker parking path with per-worker wake handles and an idle-worker list. In the captured Framework target, `_gate` contention was 0.00508 starts/message for request-backpressure and 0.000062 for request-serial; `RunWorker` itself was 0.00119 and 0.000062 starts/message. The older 7.793 and 0.604 rates therefore describe the pre-#3413 implementation.

The remaining visible Framework work differs by pattern. In backpressure, the hottest Framework managed worker sampled `ZLinkManagedMeshNode.ExpireOperationsUnderOwner` and `ZLinkDeadlineIndex.Visit`; the busiest source and target threads overall were Core I/O workers. In serial, the hottest source ThreadPool worker sampled `ZLinkEnvelopeReplyDecoder.Decode` (12.51 self µs/message) and `ZLinkDurableRequest.RequestAsync` (4.26 self µs/message). No single thread exceeded one core in either Framework profile. The single best first investigation is optimizing the reply envelope decoder while preserving the envelope contract; its measured cost alone is below the saving needed to reach 0.6 in either serial cell.

## Scope and measurement quality

- Worktree was `main`, `ee11390257`; preflight passed. The four existing `run_local.sh` pinning edits were retained. No tracked file was changed and no build was run.
- The two requested 1024-byte patterns were measured once per implementation unprofiled (10 s active) and profiled (20 s active). Every run held `/root/jobs/fx/cx/measure.lock`. The runner enforced `MemAvailable >= 12 GiB`; no run aborted. Other cells, including 4096-byte profiles, are **not measured**.
- Profiles attached `dotnet-trace` CPU sampling plus `Microsoft-Windows-DotNETRuntime` contention events and `dotnet-counters` System.Runtime counters to both source and target. `perf stat` collected task-clock, context switches, raw syscall entries, and futex syscall entries. Per-thread CPU came from `/proc/<pid>/task/<tid>/stat` sampled during the active phase. Perf maps were retained under this job's `raw/perf-maps/`.
- CPU stage estimates use EventPipe managed stack samples scaled to process task-clock. Thread CPU uses scheduler ticks scaled to the active window. EventPipe did not resolve native Core frames, so `ZLINKbg/IO/*` thread CPU is assigned to binding/Core by thread role; internal native/Core sub-stages are unresolved. The `other` column also contains unresolved runtime, kernel, and inlined work.
- The profile reduced throughput relative to the paired unprofiled run by 10.0–36.0%; profiled rates are diagnostic only. Allocation bytes are exact `GC.GetTotalAllocatedBytes(precise: true)` benchmark deltas divided by completed messages. Object allocation counts were not captured. GC counts below are estimated by summing 1 s System.Runtime counter rate samples over the 20 s profile; they include attach/drain edges. Syscall and context-switch rates likewise include profiler attach/drain edges.

## Throughput

| Pattern | ratio040 Framework / binding (msg/s; ratio) | Current unprofiled Framework / binding (msg/s; ratio) | Profiled Framework / binding (msg/s; ratio) |
|---|---:|---:|---:|
| request-backpressure 1024 | 29,571 / 58,990 (0.501) | 25,978 / 53,208 (0.488) | 23,380 / 43,653 (0.535) |
| request-serial 1024 | 5,825 / 11,303 (0.515) | 5,087 / 8,975 (0.567) | 4,062 / 5,756 (0.706) |

The current one-run baseline is slower than ratio040 in both backpressure implementations and both serial implementations. These measurements are single-run drift checks, not a new median. The serial profile ratio is especially distorted because profiling reduced binding throughput more than Framework throughput.

## Per-thread CPU

Each entry is `role / TID: CPU µs per completed message; share of one core`. Shares above are individual thread CPU divided by the observed active interval. `Core I/O` means `ZLINKbg/IO/*`; `pool worker` means `.NET Long Runni`; `ThreadPool` means `.NET TP Worker`. These are the top three CPU threads per process; the full tick samples and thread names are retained in each raw directory's `thread-cpu.csv`.

### request-backpressure 1024

| Impl / process | Highest CPU threads | Whole process CPU |
|---|---|---:|
| Framework source | Core I/O/2248974: 22.2; 0.52 · Core I/O/2248977: 14.7; 0.34 · pool worker/2248988: 13.2; 0.31 | 207.9 µs/msg |
| Framework target | Core I/O/2248815: 23.6; 0.55 · Core I/O/2248814: 18.8; 0.44 · pool worker/2248829: 17.2; 0.40 | 106.6 µs/msg |
| Binding source | ThreadPool/2255533: 19.6; 0.86 · Core I/O/2255472: 17.5; 0.76 · Core I/O/2255474: 12.0; 0.52 | 96.4 µs/msg |
| Binding target | Core I/O/2255355: 15.0; 0.65 · Core I/O/2255356: 13.0; 0.57 · ThreadPool/2255359: 12.0; 0.52 | 41.4 µs/msg |

The source ThreadPool worker is the closest to saturation (binding, 0.86 core). Framework's busiest threads are Core I/O workers (0.52 source, 0.55 target); none is saturated.

### request-serial 1024

| Impl / process | Highest CPU threads | Whole process CPU |
|---|---|---:|
| Framework source | ThreadPool/2262020: 58.6; 0.24 · ThreadPool/2262118: 45.5; 0.18 · ThreadPool/2262119: 42.2; 0.17 | 416.8 µs/msg |
| Framework target | pool worker/2261901: 44.3; 0.18 · pool worker/2261902: 43.9; 0.18 · Core I/O/2261880: 39.3; 0.16 | 165.9 µs/msg |
| Binding source | ThreadPool/2272731: 54.1; 0.31 · ThreadPool/2272776: 48.5; 0.28 · ThreadPool/2272775: 43.1; 0.25 | 331.5 µs/msg |
| Binding target | Core I/O/2272563: 27.2; 0.16 · Core I/O/2272564: 27.2; 0.16 · ThreadPool/2272567: 24.9; 0.14 | 89.9 µs/msg |

There is no saturated thread. The bench's unprofiled end-to-end mean / p95 latency was 193.7 / 254 µs Framework and 109.1 / 148 µs binding. A phase breakdown cannot be recovered from these counters: **submit to wire send, network/peer, receive to completion, and completion to next submit are not measured**. The existing result JSON records only end-to-end latency; no per-request phase timestamps or correlation spans are emitted.

## CPU by stage

Values are µs of process CPU per completed message. Critical-thread values use the hottest thread in that process. For the Framework backpressure cells and binding target backpressure/serial cells, the hottest thread is a Core I/O thread; its total CPU is shown as binding/Core, while the managed/native split is unresolved. Stages follow the profiler's sampled stacks: GC runtime frames → worker/lane/queue and waits → task/continuation frames → protobuf/envelope codec → binding/Core interop → Framework methods → benchmark driver → unresolved/other. A method such as `RunWorker` maps to worker hand-off; `ZLinkEnvelopeReplyDecoder.Decode` maps to codec; `ZLinkManagedMeshNode.ExpireOperationsUnderOwner` maps to Framework bookkeeping.

| Pattern / impl / process | Critical thread stage µs/msg | Whole-process stages µs/msg (allocation/GC; hand-off/wake; task/async; codec; binding/Core; Framework; bench; other) |
|---|---|---|
| Backpressure Framework source | Core I/O thread: binding/Core 22.2; native detail unresolved | 20.7; 26.5; 18.4; 8.5; 6.3; 38.5; 1.5; 87.5 (total 207.9) |
| Backpressure Framework target | Core I/O thread: binding/Core 23.6; native detail unresolved | 7.2; 8.4; 13.9; 0.4; 3.4; 2.9; 13.1; 57.4 (total 106.6) |
| Backpressure binding source | ThreadPool: alloc 0.2; hand-off 14.4; task 0.4; codec 0.0; binding 2.7; Framework 0.0; bench 0.2; other 1.8 | 0.2; 49.5; 1.3; 0.0; 3.0; 0.0; 0.4; 41.9 (total 96.4) |
| Backpressure binding target | Core I/O thread: binding/Core 15.0; native detail unresolved | 1.1; 0.0; 0.0; 0.0; 10.0; 0.0; 0.5; 29.7 (total 41.4) |
| Serial Framework source | ThreadPool: alloc 5.3; hand-off 17.8; task 8.8; codec 13.8; binding 2.4; Framework 7.7; bench 0.0; other 2.7 | 25.1; 104.9; 37.9; 64.6; 8.8; 53.8; 2.2; 119.4 (total 416.8) |
| Serial Framework target | pool worker: alloc 3.8; hand-off 5.1; task 17.7; codec 0.1; binding 3.3; Framework 2.6; bench 8.0; other 3.7 | 8.4; 10.1; 33.1; 0.1; 6.3; 5.9; 16.1; 86.0 (total 165.9) |
| Serial binding source | ThreadPool: alloc 2.7; hand-off 35.3; task 0.5; codec 1.6; binding 3.7; Framework 0.0; bench 2.1; other 8.0 | 17.9; 172.3; 4.0; 1.6; 17.9; 0.0; 16.4; 101.3 (total 331.5) |
| Serial binding target | Core I/O thread: binding/Core 27.2; native detail unresolved | 2.9; 0.0; 0.0; 0.0; 14.0; 0.0; 7.8; 65.2 (total 89.9) |

The sampling classifier is directional, not an exact instruction-cost attribution: inlined allocation and kernel/native Core work can land in the sampled caller or `other`. Process CPU totals are not latency; multiple workers execute concurrently.

## Per-message counts

### Kernel counters

Each cell below is `context switches / futex syscalls / all syscall entries per message`, in source and target process order. These came from `perf stat` during the profile window.

| Pattern | Impl | Source | Target |
|---|---|---:|---:|
| Backpressure | Framework | 2.83 / 2.37 / 42.6 | 1.68 / 1.34 / 11.8 |
| Backpressure | Binding | 1.05 / 1.57 / 28.3 | 1.08 / 1.41 / 5.5 |
| Serial | Framework | 8.30 / 8.68 / 166.5 | 5.53 / 3.35 / 26.4 |
| Serial | Binding | 5.22 / 6.00 / 146.5 | 3.17 / 3.13 / 11.5 |

### Lock contention, allocation, and GC

Lock numbers are Contention/Start events with stack aggregation from the target process. The `_gate` and `_socketGate` identities are mapped by their caller stacks to their declarations in `ZLinkProcessExecutionPool` and `ZLinkManagedMeshNode`; remaining IDs are other locks.

| Pattern | Impl | `_gate` starts/msg | `_socketGate` starts/msg | Other starts/msg | Allocation bytes/msg source / target | Estimated GCs per million messages, Gen0/Gen1/Gen2 source / target |
|---|---|---:|---:|---:|---:|---|
| Backpressure | Framework | 0.005075 | 0.000169 | 0.000034 | 10,155 / 4,846 | 1,216 / 135 / 24 · 58 / 0 / 0 |
| Backpressure | Binding | 0 | 0 | 0.000005 | 3,108 / 1,458 | 365 / 11 / 0 · 17 / 0 / 0 |
| Serial | Framework | 0.000062 | 0.000025 | 0.000049 | 10,125 / 5,760 | 1,206 / 1,206 / 0 · 74 / 25 / 0 |
| Serial | Binding | 0 | 0 | 0.000035 | 3,400 / 1,468 | 391 / 391 / 0 · 17 / 0 / 0 |

Framework backpressure `_gate` call-site starts were 742 `ReceiveUntilHandoff`, 605 `ReceiveSource.WaitAsync`, 555 `RunWorker`, 372 `ApplicationSource.Complete`, 71 `ReceiveSource.OnCompleted`, 21 `ApplicationSource.Signal`, and 7 other, over 467,627 completed messages. Framework serial `_gate` had five starts, all attributed to `RunWorker`, over 81,248 messages. Binding target traces contained four contention starts per pattern, none attributed to a Framework frame. Exact object allocation counts were **not measured**; allocation bytes use the benchmark's precise process allocation deltas. `Monitor Lock Contention Count` counters agreed in scale with the event counts.

## Framework-only methods on the hot managed thread

For backpressure, the hottest overall thread is a Core I/O worker and has no resolved Framework stack. This table therefore shows the hottest **managed Framework pool worker** in the source process (`.NET Long Runni`, TID 2248988), the only sampled Framework worker with a useful managed hot stack. Serial shows the busiest source ThreadPool worker (TID 2262020); there is no single saturated thread. Self and inclusive values are sampled CPU µs/message on that thread, scaled by its observed thread CPU. Fewer than 20 distinct Framework methods had self samples on either selected thread.

| Pattern | Self µs/msg | Inclusive µs/msg | Framework method |
|---|---:|---:|---|
| Backpressure | 4.415 | 6.892 | `ZLinkManagedMeshNode.ExpireOperationsUnderOwner` |
| Backpressure | 1.615 | 2.477 | `ZLinkDeadlineIndex<T>.Visit` |
| Backpressure | 0.538 | 0.538 | `ZLinkManagedMeshNode.DeadlineBudgetSpent` |
| Backpressure | 0.323 | 0.323 | `ZLinkManagedMeshNode.DeadlineNow` |
| Backpressure | 0.215 | 0.215 | `ZLinkManagedMeshNode.CollectInfrastructureUnderLock` |
| Backpressure | 0.108 | 13.246 | `ZLinkProcessExecutionPool.ReceiveUntilHandoff` |
| Backpressure | 0.108 | 7.539 | `ZLinkManagedMeshNode.ReceiveLoopAsync.MoveNext` |
| Backpressure | 0.108 | 7.000 | `ZLinkManagedMeshNode.ExpireOperations` |
| Backpressure | 0.108 | 0.108 | `ZLinkServiceAdmissionGuard.ImmutableFieldsMatch` |
| Backpressure | 0.108 | 0.108 | `ZLinkProcessExecutionPool.CollectReadySourcesUnderLock` |
| Serial | 12.512 | 13.045 | `ZLinkEnvelopeReplyDecoder.Decode<TReply>` |
| Serial | 4.259 | 25.823 | `ZLinkDurableRequest.RequestAsync<TReply>.MoveNext` |
| Serial | 2.928 | 2.928 | `ZLinkBackendRouteReceived.Dispose` |
| Serial | 2.130 | 12.512 | `ZLinkManagedMeshNode.RunDeadlineRequestAsync.RegisterAndStartAsync.MoveNext` |
| Serial | 2.130 | 2.130 | `ZLinkMeshCompletionTable.ReleaseReservation` |
| Serial | 1.331 | 1.331 | `ZLinkManagedMeshNode.DecodeDirectApplicationReply` |
| Serial | 0.799 | 0.799 | `ZLinkManagedMeshNode.CreateOperationAsync` callback |
| Serial | 0.532 | 16.505 | `ZLinkRouteRequestCall<T>.ExecuteAsync` |
| Serial | 0.532 | 0.532 | `ZLinkSpotNodeRuntime.RequestToNodeAsync` completion callback |
| Serial | 0.532 | 0.532 | `ZLinkDeadlineClock.GetUnixTimeMilliseconds` callback |
| Serial | 0.266 | 18.103 | `ZLinkManagedMeshNode.RequestNativeDurableWireAsync<T>.MoveNext` |
| Serial | 0.266 | 17.570 | `ZLinkManagedMeshNode.ObserveDirectWireReplyAsync.MoveNext` |
| Serial | 0.266 | 1.597 | `ZLinkManagedMeshNode.CreateOperationAsync` |

## Ranked candidate changes and optimistic upper bounds

The upper-bound calculation is `F′ = 1 / (1/F − s)`, then `F′ / binding`, where `F` and binding are the supplied ratio040 rates and `s` is the measured candidate self CPU in seconds/message. It assumes every measured CPU µs is removable and lies on the throughput-critical chain. The machine had multiple active worker threads and no saturated Framework thread, so these are optimistic ceilings, not throughput predictions. For 4096 cells, the 1024 measured cost is applied unchanged as an explicit extrapolation.

| Rank | Candidate / stage removed | Measured maximum self cost to target | Upper-bound ratio at 1024; at 4096 | Evidence and limit |
|---:|---|---:|---:|---|
| 1 | Optimize `ZLinkEnvelopeReplyDecoder.Decode<TReply>` in `ZLinkClientCallCodec.cs:105` — codec | 12.51 µs/msg | Serial: 0.556; 0.528 | Largest single hot Framework method on the serial source worker. This only recovers part of the required 24.4 µs (1024) / 32.9 µs (4096); preserve envelope validation and typed decode behavior. |
| 2 | Reduce operation deadline traversal cost in `ZLinkManagedMeshNode.ExpireOperationsUnderOwner` (`ZLinkManagedMeshNode.cs:12616`) and `ZLinkDeadlineIndex<T>.Visit` (`ZLinkRequestDeadlineIndex.cs:66`) — Framework bookkeeping | 6.03 µs/msg | Backpressure: 0.610; 0.566 | Sum of two sampled self costs on one managed Framework worker. It reaches 0.6 only in the 1024 optimistic estimate; the 4096 cell needs about 8.1 µs saved. Deadline correctness must remain unchanged. |
| 3 | Reduce `ZLinkDurableRequest.RequestAsync<TReply>.MoveNext` self work (`ZLinkDurableRequest.cs:8`) — async state machine / route bookkeeping | 4.26 µs/msg | Serial: 0.528; 0.503 | Inclusive 25.82 µs overlaps child methods, so only self CPU is used. It is smaller than the decoder and cannot bridge either serial target alone. |
| 4 | Reduce reply disposal/operation completion work (`ZLinkBackendRouteReceived.Dispose`, `ZLinkMeshCompletionTable.ReleaseReservation`) — Framework completion bookkeeping | 5.06 µs/msg combined self | Serial: 0.531; 0.505 | Sampled self costs 2.93 + 2.13 µs on the same hot worker; independently owned operations make combined removal an aggressive bound. |

The old worker wake/broadcast fix is already present and verified by current source history and captures. Its measured lock starts are too rare to rank as a worthwhile next change. I would start with candidate 1 because it is the largest clean single hot method on the serial path. It cannot meet the target alone; the profile does not support claiming a single currently observed method can raise all four cells to 0.6. The next design review should examine codec plus request-completion costs as one serial path while retaining the public envelope contract.

## Evidence files

All scripts, per-run logs, result JSON, nettraces, contention stacks, profile summaries, and perf maps are under `/root/jobs/fx/cx/v21/s033/dprof/`. The analyzer summary is `profile-summary.json`; lock details are `raw/dprof-profile-*/target.contention-stacks.csv`. Raw traces are below 19 MB each; no trace over 1 GB was produced.
