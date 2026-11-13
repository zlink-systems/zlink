# C++ gate-less multi-worker drain — #1639

Status: C++ implementation committed locally as `5a585edc93`; 309 non-install tests and the official stream install consumer passed. All 30 final measurement cells completed with errors=0, abandoned=0 and no contaminated/drain-bound cells. No push. The full test completion criterion remains unmet because the existing Framework install-consumer fixture fix has not been authorized. Performance acceptance floors also remain unmet, as detailed below.

Baseline: origin/main `3f83745bed140d2e50df747f5e157fb2ee02293e` (the implementation base of spec PR #3891). Local `main` is older and is not the comparison baseline. Branch starts at `6e52a5c144`.

## Observed result

One gate-less hot Node reached **8 concurrent handlers** on the branch versus **1** on main in every PM run. Median hot throughput increased from **9,534.1 to 61,443.0 msg/s** (6.44×). The requested concurrency, FIFO take, wake, permit, scope and shutdown checks passed.

Cheap-message throughput regressed: Framework request-backpressure decreased **1.85%**, and send-saturation decreased **11.44%**. Framework/binding ratios of 3-run medians changed **0.1973 → 0.1898** and **0.3006 → 0.2572**, respectively.

Spot latency increased under the achieved hot load: median-run p50 **0.300 → 1.355 ms**, p99 **0.502 → 7.586 ms**. The branch runs eight hot handlers concurrently while main runs one; these are the same offered workloads with different achieved CPU concurrency. This change demonstrates parallel progress and budget fairness, and is not a throughput or latency improvement for every workload.

## Decision ownership

Classification A: adapt C++ to the explicit multi-worker contract in PR #3891. This job explicitly authorizes C++ implementation; protected specification files remain unchanged.

Reference key: S/02 = [Handler turn and execution gate](/root/worktree/zlink-1639c/framework/doc/framework/common/spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md); S/04 = [Application job queue](/root/worktree/zlink-1639c/framework/doc/framework/common/spec/server/01-execution/04-application-job-queue-and-backpressure.ko.md); S/08 = [Messaging hot path](/root/worktree/zlink-1639c/framework/doc/framework/common/spec/server/01-execution/08-messaging-hot-path.ko.md); API = [Framework API](/root/worktree/zlink-1639c/framework/doc/framework/common/spec/server/00-foundation/06-framework-api.ko.md).

Paths below are relative to `framework/languages/cpp/framework/src/runtime/`. “Before” refers to the frozen main revision; “after” refers to this worktree.

| Decision | Before | After | Authority |
|---|---|---|---|
| Gate-less execution ownership | `mesh/mesh_node_host_service.cpp:3785` calls owner-wide begin; mailbox guards at `mesh/service_mailbox.cpp:453,158,181` exclude concurrent drains/claims | `mesh/service_mailbox.cpp:464` atomically detaches exactly one queue head; `dispatch/offload_executor.cpp:440` retains the source when more remain | S/02 §2; S/08 §4.3 W1 |
| Ready-set insertion, last-take removal, re-registration | `mesh/service_mailbox.cpp:79,505,402,463`: enqueue, notification extraction, terminal settlement and drain-end re-registration mutate membership | `mesh/service_mailbox.cpp:79,464` insert/remove under the existing mailbox mutex; `:574` marks the existing phase and copies only notification keys, leaving untaken owners ready | S/04 §3, prepared-owner set |
| Idle enrollment, wake reservation and coalescing | `dispatch/offload_executor.cpp:107,168,180`: separate unconditional notifications in internal/public submission and receive-role registration | `dispatch/offload_executor.cpp:207` removes one idle registry entry to reserve its wake; `:389` enrolls, rechecks work and then waits under the existing mutex; notification occurs after unlock | S/04 §3, wait registration and wake conditions |
| Worker budget | `mesh/mesh_node_host_service.cpp:3787`, `channels/channel_host_service.cpp:390`: a fresh deadline per owner invocation | `dispatch/offload_executor.cpp:425` worker-local group/owner identity and deadline; same-owner reselection keeps the deadline and rotates after expiry when another entry is ready | S/02 §9; S/08 W5; API §11 |
| Permit release | `dispatch/application_job_queue.hpp:1281` | unchanged `handler_entry_t::operator()` at the same location; take retains the original permit | S/04 §3 step 5; S/08 W3 |
| Gated Spot/Actor/timer/session execution | existing begin/end drain and execution gate | existing gated route retained; the new record source is selected only for Node/ChannelName (`mesh/mesh_node_host_service.cpp:3333`) | S/02 §2 |

Alternative rejected: submit one executor item per record. S/08 §4.3 forbids per-record dispatch tasks. Selected: persistent owner drain entry in the existing executor queue, preparing one record under scheduler state and running its pipeline outside scheduler state.

State replacements: the existing executor queue exposes its underlying deque to rotate its existing entries. Registered idle worker condition variables replace the scalar idle count and shared condition variable; registry membership is the wake reservation, without another scheduled flag. The existing record list node moves from its owner queue into the claim as single-record storage, permitting independent terminal credit settlement without copying the record or enlarging gated claims by a whole record.

Decision-location count before/after: **13 → 6**, scoped to accepted hosted gate-less drains and the five decisions above: ownership 3→1, ready membership mutations 4→2, wake eligibility 3→1, budget 2→1, permit boundary 1→1. Gated, standalone/manual and rejection-only legacy paths, and shutdown broadcasts, are outside this count. The executor still uses its original work queue and mutex; the mailbox still uses its original owner queues, ready set and mutex. No second record queue, lock or wake flag was added.

Code/refactoring audit of the changed modules and call paths: **6 findings, 5 addressed, 1 pending scope approval**. Addressed: three obsolete control groups (owner-wide gate-less exclusion, duplicated wake decisions and owner-invocation budgets), the gated claim allocation regression, and the manual scheduler adapter. The remaining item is the baseline package DTO fixture; its proposed correction passes in a temporary copy. No other runtime code finding was deferred.

## Verification

Release and full C++ builds passed. After the storage correction, all 309 non-install CTest entries passed in 266.38 seconds, with zero failures or skips (`test-all-storage.log`, `test-all-storage.xml`). Godot and Axmol engine integration tests are included. Compiler-invoking checks use the build-slot wrappers; package consumer builds use the same wrappers. `scripts/format/format.sh --check cpp` and `git diff --check` passed. The inline detached-record holder had increased all claim allocations, including gated Spot claims, to 3,216 bytes against the unchanged 3,072-byte published-record bound. The fix transfers the original queue list node into single-record claim storage instead. The allocation test and its bound remain unchanged. After the node-storage correction, all eight focused tests passed (`test-storage-focused.log`, 3.11 s). The unchanged Spot allocation test now observes a maximum relevant allocation of 2,264 bytes, below its original 3,072-byte bound (`test-storage-focused-detail.log`); the former 3,216-byte claim allocation is absent.

| Test / coverage | Result or purpose |
|---|---|
| Updated mailbox ingress concurrency test against frozen main | **Fails as expected** while A is held synchronously: B cannot drain (`negative-baseline.log`). Same barrier passes with the branch implementation. |
| `test_cpp_framework_gate_less_drain` | Four-worker batch with bounded notifications; A held synchronously while B finishes; FIFO takes/exactly once across 1,000 enqueue/last-take races; 300 lone-record publications alternating immediate and observed waiter-enrollment boundaries; budget fairness; gated Actor serial negative control; shutdown with taken-before-entry and running records. |
| `test_cpp_framework_mailbox_owner_dispatch` | Native ingress multiworker progress and exactly-once delivery; three independent handler scopes complete in order 2,0,1 and are each disposed once. |
| `test_cpp_framework_handler_entry_permit` | Detached record remains queued while preparation is paused; first handler instruction releases the permit; terminal-before-handler returns it once. |
| `test_cpp_framework_manual_dispatch_owner` | Manual ready owner coalescing, suspended filters, scope creation failures, reply failures and physical-terminal retention; held scheduler now handles the persistent drain source through its existing queue. |
| `zlink_cpp_framework_mesh_node_vertical_test` | Public local Node submissions run concurrently and return queue capacity at handler entry; gated actor cases retain their original controls. |
| `test_cpp_framework_offload_executor` | Worker bounds, idle timeout, role reservations, wake recovery, cancellation and shutdown using the waiter registry. |
| `test_cpp_framework_mailbox_fifo_cost` | Retained-membership reference allocation calibration. |
| `test_cpp_framework_local_enqueue_allocations` | Original bound unchanged; gated Spot record-continuation allocation regression corrected with original-node transfer. |
| Install consumers | Official stream consumer passed again against the final library. The official Framework consumer has a baseline typed-JSON DTO fixture error. A temporary copy with `package-fixture-proposal.patch` passed the complete install/build/run/symbol checks against the final library; repository fixture approval is pending (`package-final-validation.json`, `package-final-validation.log`). |

The package fixture and public JSON headers are identical to frozen main. The proposed fixture-only migration to the existing typed JSON DTO contract passed in a temporary copy against the final library. The repository fixture remains unchanged. The initial CTest syntax-only probes invoked the compiler directly under the shared measurement lock. The final 309-test run routes them through the build-slot wrapper. Package builds/configures and compiler-invoking checks use `cmake-slot.sh` / `cxx-slot.sh`, which forward to `cpp-build-slot.sh`; compile parallelism is at most 3. The installed build-slot wrapper takes the build command directly, so CMake builds use `cpp-build-slot.sh cmake --build … --parallel 3`. All tests/builds held measure.lock shared; measurements held it exclusively with `flock -o`.

## Measurements

Release / `-O3 -DNDEBUG`, identical Core/binding packages and dependency versions, test creation probes disabled and install enabled for both builds. Condition B pins source 0–7 and target 8–15. Each cell uses 30 s warm-up, followed by 5 s active time (PM: 30 s). Three runs per cell and variant. An exclusive measure.lock excludes builds/tests. TIDs are marked once on first handler/receive entry; no per-record logging is used. CPU and switches come from `/proc/<pid>/task/*/{schedstat,status}`, sampled every ~100 ms during a 3 s window (PM: 25 s). CPU µs/msg and switches/msg use target messages received in that same window as denominator. A retiring thread may have at most one sampling interval of unobserved final counters. Unique TIDs across a window can exceed the concurrent worker bound because idle workers retire and are replaced.

| Cell | Run | Revision | Binding msg/s | Framework msg/s | Framework / binding |
|---|---:|---|---:|---:|---:|
| request-backpressure 1K | 1 | main | 244,729.5 | 47,514.6 | 0.1942 |
| request-backpressure 1K | 1 | branch | 254,480.3 | 48,339.9 | 0.1900 |
| request-backpressure 1K | 2 | main | 253,560.9 | 49,141.1 | 0.1938 |
| request-backpressure 1K | 2 | branch | 245,791.0 | 47,094.2 | 0.1916 |
| request-backpressure 1K | 3 | main | 247,236.5 | 48,771.7 | 0.1973 |
| request-backpressure 1K | 3 | branch | 252,215.0 | 47,870.7 | 0.1898 |
| send-saturation 1K | 1 | main | 626,745.5 | 175,407.7 | 0.2799 |
| send-saturation 1K | 1 | branch | 603,894.9 | 155,867.6 | 0.2581 |
| send-saturation 1K | 2 | main | 562,094.2 | 175,711.0 | 0.3126 |
| send-saturation 1K | 2 | branch | 578,971.4 | 155,334.2 | 0.2683 |
| send-saturation 1K | 3 | main | 583,572.4 | 174,661.7 | 0.2993 |
| send-saturation 1K | 3 | branch | 624,930.7 | 155,097.4 | 0.2482 |

| Cell | Revision | Binding 3-run median | Framework 3-run median | Ratio of medians |
|---|---|---:|---:|---:|
| request-backpressure 1K | main | 247,236.5 | 48,771.7 | 0.1973 |
| request-backpressure 1K | branch | 252,215.0 | 47,870.7 | 0.1898 |
| send-saturation 1K | main | 583,572.4 | 175,407.7 | 0.3006 |
| send-saturation 1K | branch | 603,894.9 | 155,334.2 | 0.2572 |

Target per-thread CPU (each entry is `TID: µs/msg`). Receive-role and worker TIDs are identified by the first-entry role markers. Context switches include voluntary and involuntary switches for all threads in the corresponding process.

| Cell | Run | Revision | Target application workers | Target receive role | Target switches/msg | Source switches/msg |
|---|---:|---|---|---|---:|---:|
| request-backpressure | 1 | main | 3850173: 10.627; 3850498: 5.193; 3850525: 4.037 | 3848912: 14.602 | 1.3945 | 2.5065 |
| request-backpressure | 1 | branch | 3852633: 6.533; 3854002: 6.392; 3854003: 6.452; 3854004: 6.443; 3854006: 6.216; 3854007: 6.488; 3854008: 6.346; 3854009: 6.362 | 3852548: 14.537 | 2.5713 | 2.5673 |
| request-backpressure | 2 | main | 3870491: 9.255; 3870701: 9.319 | 3869172: 14.176 | 1.4394 | 2.4064 |
| request-backpressure | 2 | branch | 3872794: 6.116; 3874340: 6.137; 3874341: 6.218; 3874342: 6.195; 3874344: 6.130; 3874345: 6.317; 3874346: 6.178; 3874347: 6.162 | 3872795: 14.770 | 2.7089 | 2.5495 |
| request-backpressure | 3 | main | 3895979: 9.902; 3896356: 9.018 | 3894822: 14.391 | 1.3179 | 2.5850 |
| request-backpressure | 3 | branch | 3898502: 6.366; 3899920: 6.048; 3899921: 6.166; 3899922: 6.440; 3899923: 6.230; 3899925: 6.109; 3899926: 6.426; 3899927: 6.075 | 3898426: 14.885 | 2.5972 | 2.4745 |
| send-saturation | 1 | main | 3856169: 2.722; 3857670: 2.837 | 3856086: 4.194 | 0.2536 | 0.0410 |
| send-saturation | 1 | branch | 3859869: 3.087; 3861281: 3.121; 3861282: 3.112; 3861283: 3.087; 3861285: 3.095; 3861286: 3.095; 3861287: 3.099; 3861288: 3.114 | 3859789: 5.287 | 1.3209 | 0.0362 |
| send-saturation | 2 | main | 3879474: 2.901; 3881058: 2.689 | 3879475: 4.005 | 0.2955 | 0.0427 |
| send-saturation | 2 | branch | 3885753: 3.117; 3887189: 3.136; 3887190: 3.102; 3887191: 3.103; 3887192: 3.107; 3887194: 3.100; 3887195: 3.086; 3887196: 3.094 | 3885673: 5.290 | 1.3690 | 0.0351 |
| send-saturation | 3 | main | 3903320: 2.741; 3903873: 2.667 | 3902182: 4.468 | 0.2213 | 0.0346 |
| send-saturation | 3 | branch | 3907093: 3.126; 3908606: 3.123; 3908607: 3.115; 3908608: 3.110; 3908609: 3.111; 3908611: 3.095; 3908612: 3.094; 3908613: 3.100 | 3907094: 5.322 | 1.3862 | 0.0360 |
| pm | 1 | main | 3876433: 2.255; 3876482: 0.335; 3876490: 0.955; 3876513: 1.727; 3876516: 0.670; 3876520: 0.891; 3876521: 1.731; 3876544: 1.118; 3876547: 0.659; 3876552: 1.615; 3876558: 3.408; 3876587: 0.502; 3876588: 1.006; 3876594: 1.232; 3876612: 0.672; 3876617: 12.062; 3876683: 0.336; 3876720: 0.673; 3876735: 2.567; 3876742: 1.963; 3876770: 0.671; 3876772: 0.641; 3876773: 6.372; 3876803: 0.224; 3876805: 0.224; 3876815: 1.292; 3876836: 5.900; 3876883: 0.168; 3876884: 4.225; 3876916: 0.224; 3876917: 0.617; 3876922: 4.859; 3876947: 0.335; 3876955: 3.740; 3876956: 0.223; 3876984: 1.397; 3876988: 0.614; 3876989: 6.425; 3877009: 0.224; 3877041: 0.225; 3877046: 4.797; 3877063: 0.169; 3877084: 6.089; 3877119: 1.567; 3877145: 0.725; 3877146: 2.961; 3877152: 0.225; 3877156: 2.183; 3877178: 1.118; 3877183: 2.072; 3877209: 1.117; 3877211: 3.861; 3877242: 1.730; 3877251: 0.725; 3877252: 0.000 | 3874556: 4.035 | 0.3751 | 0.3640 |
| pm | 1 | branch | 3881412: 14.135; 3882894: 14.140; 3882895: 14.196; 3882896: 14.227; 3882897: 14.192; 3882898: 14.217; 3882899: 14.201; 3882900: 14.176 | 3881307: 4.140 | 0.2550 | 0.0612 |
| pm | 2 | main | 3889234: 1.368; 3889348: 7.309; 3889398: 0.450; 3889404: 0.953; 3889406: 5.735; 3889461: 2.523; 3889468: 0.184; 3889471: 2.190; 3889497: 7.576; 3889537: 2.754; 3889567: 0.785; 3889569: 2.412; 3889585: 1.412; 3889599: 2.916; 3889610: 8.194; 3889629: 0.190; 3889674: 1.121; 3889686: 6.836; 3889703: 0.225; 3889750: 1.850; 3889755: 2.589; 3889783: 6.062; 3889812: 0.164; 3889822: 6.361; 3889853: 0.169; 3889860: 0.169; 3889881: 3.196; 3889891: 2.640; 3889915: 3.260; 3889946: 3.308; 3889957: 0.676; 3889961: 7.303; 3890015: 0.191; 3890021: 1.573; 3890022: 0.225; 3890023: 1.103; 3890044: 0.799; 3890047: 1.694; 3890054: 5.354; 3890084: 0.338; 3890096: 0.000 | 3887431: 3.966 | 0.3821 | 0.3647 |
| pm | 2 | branch | 3890562: 14.243; 3892072: 14.121; 3892073: 14.216; 3892074: 14.138; 3892076: 14.130; 3892077: 14.214; 3892078: 14.178; 3892079: 14.243 | 3890475: 4.144 | 0.2514 | 0.0621 |
| pm | 3 | main | 3910753: 1.184; 3910817: 3.137; 3910827: 0.955; 3910849: 1.119; 3910851: 7.956; 3910914: 2.857; 3910925: 0.671; 3910926: 3.814; 3910959: 2.467; 3910983: 2.279; 3910989: 0.224; 3910990: 2.466; 3911016: 0.896; 3911017: 2.015; 3911025: 0.619; 3911034: 10.527; 3911113: 0.391; 3911116: 1.623; 3911119: 4.759; 3911156: 4.424; 3911159: 0.396; 3911187: 4.263; 3911219: 0.841; 3911220: 2.021; 3911243: 11.371; 3911318: 0.224; 3911320: 1.963; 3911321: 0.225; 3911345: 3.680; 3911362: 4.772; 3911397: 0.785; 3911410: 3.303; 3911420: 1.126; 3911443: 2.128; 3911448: 2.369; 3911475: 1.232; 3911477: 0.672; 3911478: 1.458; 3911486: 0.505; 3911490: 1.461; 3911508: 3.566; 3911544: 1.502 | 3908864: 4.128 | 0.3788 | 0.3628 |
| pm | 3 | branch | 3912012: 14.175; 3913499: 14.154; 3913501: 14.210; 3913502: 14.194; 3913503: 14.145; 3913504: 14.143; 3913505: 14.133; 3913506: 14.101 | 3911930: 4.181 | 0.2574 | 0.0608 |

Aggregate Framework CPU and context switches (3-run medians; application CPU is the sum over its observed worker TIDs in each run). The per-thread tables above and `summary.json` retain individual values.

| Cell | Revision | Application µs/msg | Receive µs/msg | Target switches/msg | Source switches/msg |
|---|---|---:|---:|---:|---:|
| request-backpressure | main | 18.920 | 14.391 | 1.3945 | 2.5065 |
| request-backpressure | branch | 49.860 | 14.770 | 2.5972 | 2.5495 |
| send-saturation | main | 5.559 | 4.194 | 0.2536 | 0.0410 |
| send-saturation | branch | 24.846 | 5.290 | 1.3690 | 0.0360 |
| pm | main | 104.247 | 4.035 | 0.3788 | 0.3640 |
| pm | branch | 113.482 | 4.144 | 0.2550 | 0.0612 |

Binding comparison: the receive role executes its handler inline; there is no separate application-worker stage.

| Cell | Run | Revision | Binding receive + handler TID: µs/msg | Target switches/msg | Source switches/msg |
|---|---:|---|---|---:|---:|
| request-backpressure | 1 | main | 3847061: 3.516; 3847062: 0.000 | 0.1349 | 0.2031 |
| request-backpressure | 1 | branch | 3850726: 3.409; 3850727: 0.000 | 0.1299 | 0.2275 |
| request-backpressure | 2 | main | 3867359: 3.351; 3867360: 0.000 | 0.1258 | 0.2067 |
| request-backpressure | 2 | branch | 3870946: 3.561; 3870947: 0.000 | 0.1328 | 0.2148 |
| request-backpressure | 3 | main | 3893009: 3.649; 3893010: 0.000 | 0.1287 | 0.2169 |
| request-backpressure | 3 | branch | 3896613: 3.250; 3896614: 0.000 | 0.1335 | 0.2137 |
| send-saturation | 1 | main | 3854239: 0.000; 3854240: 1.306 | 0.1022 | 0.0355 |
| send-saturation | 1 | branch | 3857937: 0.000; 3857938: 1.334 | 0.1029 | 0.0369 |
| send-saturation | 2 | main | 3877633: 0.000; 3877634: 1.347 | 0.1178 | 0.0284 |
| send-saturation | 2 | branch | 3883830: 0.000; 3883831: 1.321 | 0.1062 | 0.0283 |
| send-saturation | 3 | main | 3900147: 0.000; 3900148: 1.373 | 0.1132 | 0.0317 |
| send-saturation | 3 | branch | 3905240: 0.000; 3905241: 1.322 | 0.0954 | 0.0338 |

PM cell: one hot Node owner runs 100 µs synchronous CPU handlers; a public HTTP trigger issues an independent typed request to a gated Instance Spot on an independent 100 ms cadence while the main collector samples /proc. Per-run p50/p99 use nearest-rank percentiles; the summary takes the median of the three per-run values. The reported latency measures only the public `request_to_spot(...).instance_spot(...).async<...>()` interval inside the trigger, excluding HTTP transport time. Cold activation completes before hot traffic starts. The same target hosts the hot Node and Spot. The HTTP-triggered public Spot call uses same-host local enqueue through the ordinary application mailbox/executor, gate and permit paths (public_host_runtime.cpp:9615,9345,8992; mesh_node_host_service.cpp:3331). Its latency includes local admission, scheduling, execution and completion; it excludes HTTP and remote network transport. The hot traffic source is pinned to 0–7 and the target including the Spot to 8–15. In-memory Stores are setup dependencies.

| Run | Revision | Hot msg/s | Spot samples | Spot p50 µs | Spot p99 µs | Peak concurrent hot handlers | Median hot handlers at probe completion |
|---:|---|---:|---:|---:|---:|---:|---:|
| 1 | main | 9,534.1 | 250 | 316.172 | 487.527 | 1 | 1.0 |
| 1 | branch | 61,443.0 | 250 | 1355.003 | 7622.277 | 8 | 5.0 |
| 2 | main | 9,529.0 | 250 | 292.268 | 539.703 | 1 | 1.0 |
| 2 | branch | 61,390.9 | 250 | 1377.946 | 7585.620 | 8 | 5.0 |
| 3 | main | 9,538.9 | 250 | 299.932 | 501.754 | 1 | 1.0 |
| 3 | branch | 61,537.4 | 250 | 1209.375 | 7264.328 | 8 | 5.0 |

| Revision | Median run p50 µs | Median run p99 µs |
|---|---:|---:|
| main | 299.932 | 501.754 |
| branch | 1355.003 | 7585.620 |

Raw cells: `/root/jobs/fx/cx/v21/s033/cdrain/measure/`; per-thread counters and summaries: `/root/jobs/fx/cx/v21/s033/cdrain/summary.json`. Reproduction scripts: `configure.sh`, `configure-main.sh`, `measure.sh`, `run-B.sh`, `runner-common.sh`, `collect.py`, and `bench/` in the same job directory.

## Cross-language comparison (read-only)

| Language | Current decision location | Finding against new contract |
|---|---|---|
| .NET | `Runtime/Execution/ZLinkProcessExecutionPool.cs:443–455,993–1010`; `Runtime/Backend/DotNet/ZLinkMeshDispatchPump.cs:118–127`; `Runtime/Execution/ZLinkSerialExecutionQueue.cs:1415–1429` | Readiness and running count are centralized in the pool. Mesh and shared queue drains explicitly request ApplicationWorkerCount concurrent source turns. ClientServer Channel dispatch uses a single-reader source (`ZLinkChannelApplicationDispatchQueue.cs:15,113`). |
| Java | `runtime/channels/ZLinkMeshApplicationDispatcher.java:995–998`; `ZLinkHandlerDispatch.java:16–44` | Node/Channel namespaces use the existing serial execution queue. |
| Kotlin | Uses the Java framework runtime and the same namespace queue | No independent Kotlin scheduler implementation. |
| Node.js | `runtime/foundation/service-mailbox.ts:68,134–159,177–189` | A claimed owner leaves ready state until release; JS application instructions use the language event loop. |

These are implementation differences/gaps under the now-explicit PR #3891 contract, not a specification gap. This job's explicit C++ scope takes precedence over the generic instruction to change all languages together. No other language files are modified.

## Remaining failures and unmet clauses

- **Official install-consumer test:** [install_consumer.cmake:215](/root/worktree/zlink-1639c/framework/languages/cpp/tests/Zlink.Framework.PackageTests/install_consumer.cmake:215) still defines generated DTOs with legacy nlohmann converters instead of the current `zlink::json::dto_traits` contract. This is unchanged from frozen main. The concrete proposal is [package-fixture-proposal.patch](/root/jobs/fx/cx/v21/s033/cdrain/package-fixture-proposal.patch); its temporary copy passes all package checks against the final library. Applying it to repository source awaits the scope decision because [AGENTS.md:111](/root/worktree/zlink-1639c/AGENTS.md:111) says “unrelated failure를 임의로 고치거나 expectation을 낮추지 않는다.” The 309-test result excludes the two separately handled install tests; it is not an all-311-pass claim.
- **S/08 §7(c), throughput:** send-saturation 1K remains below the 0.90 Framework/binding floor: main 0.3006, branch 0.2572. The common bench §7.2 request-backpressure 0.80 floor is also unmet at 1K: main 0.1973, branch 0.1898. These gaps already exist on main and the branch regresses the measured ratios. No timeout, retry, HWM or budget was increased to change the verdict. Other payloads/patterns are outside this job's requested measurement set.
- **PM interpretation:** p99 increased as the hot owner used all eight application workers. The reported Spot interval is a same-host public request through the real application queue and gate; it does not measure remote Spot network latency. No numerical PM latency ceiling was specified, so the p50/p99 observations are reported without an invented pass threshold.
- No exception to the requested gate-less take, ready membership, waiter/wake, worker budget, permit, gated serialization, per-dispatch lifetime or shutdown clauses was required by this implementation. The listed C++ tests cover those behaviors. Cross-language implementations remain outside this explicitly C++ job.

Owner layer: C++ Framework mailbox and application executor.  
Spec authority: S/02 §2/§9; S/04 §3 step 5 and ready-owner conditions; S/08 I4/W1–W5/§7(f); API §11.  
Cross-language result: read-only comparison above; existing implementation differences remain outside the C++ scope.  
Change classification: **A — adaptation to the explicit contract in spec PR #3891**.

Changed repository files: scheduler/mailbox (`offload_executor.*`, `service_mailbox.*`), mesh and ClientServer drain integration (`mesh_node_host_service.*`, `channel_host_service.cpp`, `public_host_runtime.*`), CMake test registration, seven C++ test files and `tests/support/manual_internal_scheduler.hpp`. The working tree is clean. Commit: `5a585edc93`.

Validation artifacts: `build-production-node-storage.log`, `build-storage-focused.log`, `test-storage-focused.log`, `test-storage-focused-detail.log`, `build-storage-all.log`, `test-all-storage.log`, `test-all-storage.xml`, `format-final.log`, `build-post-commit.log`, `test-layout-post-commit.log`, `package-final-validation.log`, `versions.json`. Earlier diagnostic measurements are preserved in `measure-before-node-storage/` and `pm-before-independent-probes/`; none are included in the final tables.
