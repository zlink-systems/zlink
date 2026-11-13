# Node Framework vs binding: current 4096-byte stage profile (#3872)

## Result and measurement window

Preflight passed on `ee1139025782688e33109455041d8a0222cc8fe3` (2026-11-05). The four pre-existing `run_local.sh` pinning edits were preserved. No tracked repository files were changed. All runs held `/root/jobs/fx/cx/measure.lock`; `SKIP_BUILD=1`; `MemAvailable` stayed above 25 GiB. The job-local runner launched one implementation and one pattern at a time.

Each unprofiled baseline used one 20-second active run after 10 seconds warmup. Active profiling used an 8-second window inside a 20-second active run: `--perf-basic-prof` with `perf record` for native/JIT stacks, Inspector CPU profiles for JS self/inclusive timing, Inspector heap allocation sampling at 32 KiB sampling interval, and a separate active-window `strace -c -f` run for syscall counts. Heap and CPU sampling were separate runs; syscall counts are from their own trace run. Profile timings are estimates, under profiler overhead, with one capture per cell; use them to rank code paths, not as standalone throughput results. The sole unprofiled run per cell is also reference-only.

| Pattern / 4096 B | Ratio040 supplied F / B | Ratio040 ratio | Current unprofiled F / B msg/s | Current ratio |
|---|---:|---:|---:|---:|
| send-saturation | 54,892 / 91,921 | 0.597 | 55,077 / 85,408 | 0.645 |
| request-serial | not supplied / not supplied | 0.601 | 5,068 / 8,499 | 0.596 |

The current request-serial baseline also measured mean end-to-end round trip of 193.54 µs (p95 252.09 µs) for Framework and 114.63 µs (p95 149.31 µs) for binding. Current baseline figures do not reuse older-run numbers. The 1024-byte request-serial and request-backpressure cells were **not measured** in this task.

## Per-thread CPU and saturation

CPU is `/proc/<pid>/task/*/schedstat` runtime delta divided by that process's completed-message delta. Core share is thread runtime divided by the 8.0-second capture interval. Source main thread also runs the benchmark driver; it is not a separate thread. `ZLINKbg/IO/*` is binding native I/O. Remaining `node` threads combine libuv and V8 helpers; Linux reports them under the same `node` comm name, so GC helpers cannot be isolated. Values use the perf window, not the unprofiled baseline.

| Pattern | Impl / process | Main event loop (µs/msg; cores) | Node worker / V8 helpers (µs/msg; cores) | Binding native I/O (µs/msg; cores) | Other native | Whole process CPU (µs/msg) |
|---|---|---:|---:|---:|---:|---:|
| send-saturation | Framework source | 16.98; 0.978 | 2.46; 0.142 | 9.61; 0.554 | 0.00 | 29.05 |
| send-saturation | Framework target | 17.45; 1.005 | 2.46; 0.142 | 3.88; 0.223 | 0.00 | 23.78 |
| send-saturation | binding source | 11.82; 0.982 | 1.70; 0.142 | 9.72; 0.808 | 0.00 | 23.24 |
| send-saturation | binding target | 12.10; 1.006 | 0.78; 0.065 | 7.58; 0.630 | 0.00 | 20.46 |
| request-serial | Framework source | 115.83; 0.484 | 1.72; 0.007 | 57.56; 0.241 | 0.04 | 175.15 |
| request-serial | Framework target | 109.05; 0.456 | 38.00; 0.159 | 43.25; 0.181 | 0.05 | 190.34 |
| request-serial | binding source | 68.52; 0.387 | 1.55; 0.009 | 51.77; 0.293 | 0.03 | 121.88 |
| request-serial | binding target | 176.88; 1.000 | 2.26; 0.013 | 47.76; 0.270 | 0.04 | 226.93 |

The send-saturation critical resource is the Framework target main event loop at about one core; the source main loop is also nearly saturated. Binding source and target main loops are both at one core. Request-serial is latency-bound for Framework: neither main loop reaches one core, and the critical submit/completion path spans the source loop, native I/O, peer, and target loop. Binding's target main loop is saturated. A core share slightly above 1.0 is capture-boundary skew, not more than one core of execution.

For request-serial, the requested per-round-trip breakdown is **not measured** by the public benchmark counters:

| Interval | Framework | Binding |
|---|---:|---:|
| submit to wire send | not measured | not measured |
| network and peer | not measured | not measured |
| receive to completion | not measured | not measured |
| completion to next submit | not measured | not measured |
| Whole round trip, mean | 193.54 µs | 114.63 µs |

The benchmark reports only whole-request latency. Adding interval probes would require another instrumented run and is not inferred from CPU time.

## CPU per stage

These are sampled self-CPU shares scaled by the per-process `schedstat` CPU/message total. The critical-thread column is Framework target main for send-saturation and Framework source main for request-serial; the binding critical main is target in both captures. The process columns are source and target separately. The classifier uses function symbols and source paths: allocator/V8 allocation and GC symbols → allocation/GC; Promise hooks, async builtins → Promise/async; callback, queue, receive dispatch and wait symbols → handoff/wake; protobuf, envelope, header, M6A and multipart symbols → codec; N-API, ZLink/Core, kernel send/receive → binding/Core; `framework/dist` symbols outside codec → Framework bookkeeping; `bench/grpc/node` → benchmark driver; remaining symbols → other. Whole-process stage numbers are per-message CPU estimates; categories are exclusive in this classifier. Tiny sampled functions and native work that lacks symbols can shift between rows.

**send-saturation, µs/message**

| Stage | F critical thread | F source process | F target process | B critical thread | B source process | B target process |
|---|---:|---:|---:|---:|---:|---:|
| Allocation and GC | 0.66 | 1.74 | 1.85 | 0.57 | 1.72 | 1.57 |
| Handoff and wake | 0.63 | 0.76 | 1.11 | 0.17 | 0.99 | 0.54 |
| Promise / async | 0.94 | 1.71 | 1.40 | 0.00 | 0.36 | 0.25 |
| Codec | 1.18 | 0.88 | 1.79 | 0.18 | 0.24 | 0.48 |
| Binding / Core call | 0.78 | 3.51 | 1.51 | 0.92 | 2.13 | 2.56 |
| Framework bookkeeping | 0.44 | 0.87 | 0.64 | 0.00 | 0.00 | 0.00 |
| Benchmark driver | 0.07 | 0.44 | 0.10 | 0.04 | 0.57 | 0.08 |
| Other | 7.29 | 19.00 | 15.28 | 4.20 | 17.32 | 14.87 |

**request-serial, µs/message**

| Stage | F critical thread | F source process | F target process | B critical thread | B source process | B target process |
|---|---:|---:|---:|---:|---:|---:|
| Allocation and GC | 2.70 | 5.32 | 4.78 | 3.06 | 4.38 | 4.72 |
| Handoff and wake | 3.60 | 7.20 | 10.60 | 8.63 | 7.82 | 12.64 |
| Promise / async | 3.27 | 7.57 | 7.50 | 8.33 | 2.67 | 12.53 |
| Codec | 4.22 | 7.25 | 8.20 | 1.49 | 1.47 | 2.72 |
| Binding / Core call | 7.77 | 15.08 | 19.00 | 23.81 | 13.76 | 37.24 |
| Framework bookkeeping | 4.19 | 6.34 | 5.65 | 0.00 | 0.00 | 0.00 |
| Benchmark driver | 0.46 | 0.70 | 0.86 | 3.87 | 1.34 | 5.24 |
| Other | 54.14 | 124.23 | 134.82 | 86.30 | 91.10 | 153.27 |

The Inspector CPU profile for the Framework critical thread reports inclusive JS budgets separately below. Inclusive values overlap; do not add them to the self-stage table. `other` is large because V8 builtins, allocator internals, scheduling/runtime code, and unresolved native frames do not map cleanly to one Framework stage.

## Counts per message

Context switches are process task switch deltas from `/proc` during the 8-second perf capture, divided by that process's message delta. Heap figures are Inspector allocation-sampling estimates with a 32 KiB sampling interval; “samples/message” is the number of sampled allocation records, not an object-allocation count. Bytes are estimated V8 heap allocation and exclude a reliable count of external Buffer backing-store bytes.

| Pattern | Impl | Source / target context switches per msg | Both processes | V8 sampled bytes/msg source / target | Sample records/msg source / target |
|---|---|---:|---:|---:|---:|
| send-saturation | Framework | 1.098 / 0.211 | 1.309 | 3,882 / 7,909 B | 0.118 / 0.240 |
| send-saturation | binding | 0.319 / 0.262 | 0.582 | 2,556 / 1,659 B | 0.078 / 0.050 |
| request-serial | Framework | 5.050 / 3.038 | 8.088 | 14,792 / 15,430 B | 0.450 / 0.469 |
| request-serial | binding | 3.052 / 2.131 | 5.183 | 7,691 / 19,789 B | 0.233 / 0.601 |

| Pattern | Impl | Source syscalls/msg | Target syscalls/msg | Both processes syscalls/msg |
|---|---|---:|---:|---:|
| send-saturation | Framework | 1.946 | 2.298 | 4.244 |
| send-saturation | binding | 1.323 | 3.043 | 4.365 |
| request-serial | Framework | 35.302 | 21.267 | 56.570 |
| request-serial | binding | 20.255 | 29.355 | 49.610 |

The syscall denominator is the source completed-message delta from the same trace window. Strace totals cover all attached threads and are not directly assigned to one Framework stage.

Actual allocation object count, GC collections per million messages, event-loop turns per message, and native-to-JS callback count are **not measured**. The CPU profile sampled `(garbage collector)` for about 507 samples per million Framework target messages in send-saturation and 446 samples per million Framework source messages in request-serial; these are CPU sampling hits, not collection counts. Syscalls came from `strace -c -f` attached only inside a separate active 8-second window; tracer overhead makes these count-only evidence. Scheduler switches are not event-loop turns.

## Top 20 Framework-only methods on the critical thread

Self and inclusive CPU are from an 8-second Node Inspector CPU profile attached after the active phase began, divided by messages completed in that profile window. The send-saturation critical thread is the Framework target main loop; request-serial uses the Framework source main loop. `dist/*.js` symbols are mapped to their corresponding `src/*.ts` paths. Inclusive entries overlap.

**send-saturation, Framework target main loop**

| # | File:method | Self µs/msg | Inclusive µs/msg |
|---:|---|---:|---:|
| 1 | `contracts/Common/ZLinkEncodedPayload.ts:data` | 1.375 | 1.375 |
| 2 | `backend/mesh-dispatch-pump.ts:observeRecordTerminal` | 0.272 | 0.422 |
| 3 | `foundation/raw-service-mesh-runtime.ts:finishReceive` | 0.269 | 0.269 |
| 4 | `channels/channel-envelope.ts:tryParseCanonicalChannelHeader` | 0.190 | 0.863 |
| 5 | `foundation/service-mailbox.ts:tryEnqueue` | 0.190 | 0.376 |
| 6 | `channels/channel-dispatch-pipeline.ts:dispatchOneWay` | 0.188 | 3.597 |
| 7 | `foundation/service-mailbox.ts:receivedAtClock` | 0.179 | 0.179 |
| 8 | `channels/channel-dispatchers.ts:receivedAtClock` | 0.144 | 0.144 |
| 9 | `channels/channel-dispatchers.ts:dispatchMesh` | 0.135 | 5.409 |
| 10 | `backend/node/node-raw-binding-port.ts:receiveRecord` | 0.126 | 6.656 |
| 11 | `backend/mesh-dispatch-pump.ts:dispatchClaimRecords` | 0.124 | 6.302 |
| 12 | `channels/channel-dispatch-pipeline.ts:invoke` | 0.124 | 3.190 |
| 13 | `framework-codec-protobuf/framework.cjs:deserialize` | 0.122 | 1.957 |
| 14 | `channels/channel-envelope.ts:consume` | 0.106 | 0.106 |
| 15 | `channels/channel-dispatchers.ts:dispatchMeshCommand` | 0.106 | 4.161 |
| 16 | `backend/node/node-raw-mesh-backend.ts:decodeMultipartRecord` | 0.102 | 0.458 |
| 17 | `channels/channel-dispatchers.ts:createRouteContext` | 0.095 | 0.190 |
| 18 | `foundation/raw-service-mesh-runtime.ts:processApplicationReceived` | 0.089 | 0.179 |
| 19 | `foundation/raw-service-mesh-runtime.ts:receiveOne` | 0.089 | 0.179 |
| 20 | `host/index.ts:admitMeshDispatch` | 0.089 | 0.179 |

**request-serial, Framework source main loop**

| # | File:method | Self µs/msg | Inclusive µs/msg |
|---:|---|---:|---:|
| 1 | `contracts/Common/ZLinkEncodedPayload.ts:data` | 2.494 | 2.494 |
| 2 | `channels/channel-envelope.ts:encodePayload` | 2.108 | 6.925 |
| 3 | `backend/node/node-raw-mesh-backend.ts:requestSubmission` | 1.164 | 1.164 |
| 4 | `messaging/request-result.ts:createZLinkRequest` | 1.056 | 29.842 |
| 5 | `foundation/raw-service-mesh-runtime.ts:requestToTarget` | 0.667 | 13.709 |
| 6 | `channels/channel-envelope.ts:decodeChannelReply` | 0.608 | 7.040 |
| 7 | `foundation/operation-registry.ts:register` | 0.527 | 0.971 |
| 8 | `channels/channel-envelope.ts:encodeChannelEnvelopeParts` | 0.499 | 8.393 |
| 9 | `backend/node/node-raw-binding-port.ts:retainAndClose` | 0.472 | 0.666 |
| 10 | `channels/channel-envelope.ts:isValidChannelCorrelationId` | 0.471 | 0.471 |
| 11 | `channels/channel-envelope.ts:preparedChannelHeader` | 0.469 | 1.078 |
| 12 | `backend/node/node-raw-binding-port.ts:receiveRecord callback` | 0.443 | 4.766 |
| 13 | `channels/channel-transports.ts:request callback` | 0.417 | 26.707 |
| 14 | `foundation/service-wire-m6a-codec.ts:writeMultipartPayload` | 0.417 | 0.417 |
| 15 | `backend/node/node-raw-mesh-backend.ts:deliverCompletion` | 0.416 | 8.814 |
| 16 | `channels/channel-envelope.ts:validateChannelHeader` | 0.389 | 0.389 |
| 17 | `backend/node/node-raw-mesh-backend.ts:requestToNode` | 0.361 | 17.174 |
| 18 | `framework-codec-protobuf/framework.cjs:serialize` | 0.361 | 2.742 |
| 19 | `foundation/operation-registry.ts:resolveCaller` | 0.334 | 0.334 |
| 20 | `messaging/packet-name.ts:tryConstructorPacketName` | 0.331 | 0.414 |

## Ranked candidates and first implementation

The ratio ceiling uses the one-run unprofiled rates and assumes the measured inclusive budget is removed entirely from the serial or saturated bottleneck while all other costs stay fixed: `F' = 1 / (1/F - saved_us/1e6)`, then `F'/B`. These are deliberately optimistic ceilings, not expected results. Candidate budgets overlap and cannot be added.

| Rank | Pattern / candidate | Stage and location | Maximum observed critical-path budget | Optimistic ratio ceiling |
|---:|---|---|---:|---:|
| 1 | request-serial: reduce redundant result/admission Promise and continuation work while retaining the required operation owner and completion turn | Promise/async + Framework bookkeeping; `node-raw-mesh-backend.ts:requestToNode` | 17.174 µs/msg inclusive | 0.653 |
| 2 | request-serial: reduce request-result wrapper/state creation | Promise/async; `request-result.ts:createZLinkRequest` | 29.842 µs/msg inclusive parent budget; includes required nested work and overlaps rank 1 | 0.703 |
| 3 | request-serial: avoid the encoded-payload accessor copy on the internal decode path if decoded bytes remain independently owned | Allocation/copy; `ZLinkEncodedPayload.ts:data` | 2.494 µs/msg self | 0.604 |
| 4 | send-saturation: reduce the target-side encoded-payload access copy without weakening decoded-value ownership | Allocation/copy; `ZLinkEncodedPayload.ts:data` | 1.375 µs/msg self | 0.698 |
| 5 | send-saturation: reduce canonical channel-header parsing | Codec; `channel-envelope.ts:tryParseCanonicalChannelHeader` | 0.863 µs/msg inclusive | 0.677 |

Rank 1 is the single change I would implement first: simplify request result/admission Promise representation on `requestToNode`, keeping the existing E3/E4 pending-operation registration and E5 completion-turn behavior. Its 0.653 ceiling only clears the 0.65 aim if almost all of the inclusive budget were truly removable; the profile does not show that it is. Rank 2 is a larger enclosing measurement region, not an independent saving. The payload-access candidates are conditional: `data()` explicitly returns a copy, while decoded byte ownership must remain independent, so a view-only replacement is not valid without preserving that contract. No change was implemented in this evidence-only task.

## Artifacts and limits

All job scripts, profile data, heap profiles, and analysis are under `/root/jobs/fx/cx/v21/s033/nprof/`. The largest raw perf files are below 40 MB; no raw trace exceeded 1 GB. No tests were run. The repository still has only the four pre-existing `run_local.sh` pinning modifications.
