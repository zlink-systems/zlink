# C++ source completion path — 0.33.0 #1639 / #3867

**Completion workers and publication allocations improve; the requested whole receive-role CPU reduction is not demonstrated.** The selected implementation publishes the preallocated C1 reservation directly as an independent Asio operation, collecting poll publications without serializing callbacks. Completion-worker CPU is 17.711 → 15.617 µs/reply across the eight ranked worker medians (-11.8%). Source receive CPU is 15.800 → 15.824 µs/reply (+0.2%, effectively unchanged), and source total is 82.152 → 79.833 µs/reply (−2.8%). The ≤6.75 µs receive budget is **not met**. Request throughput is −0.5%, send throughput +1.5%; these are historical three-run comparisons, not evidence of an end-to-end request throughput improvement.

Worktree `/root/worktree/zlink-3867c`; branch `perf/3867-cpp-completion-batch`; HEAD/base `5ecae5de8b31c0633a6f7e8c1cb897b26c5ac87a`. No commit, push, branch mutation, GitHub operation, docs synchronization, sub-agent or other model was used. Only `framework/languages/cpp/**` and this scratch directory were written. No Core C header was read.

## Profile before and choice of representation

A fresh before profile uses the unchanged crecv3 binaries: condition B, request-backpressure 1024 B, 30 s warmup / 20 s active, source CPUs 0–7 and target CPUs 8–15, exclusive `flock -o /root/jobs/fx/cx/measure.lock`. cpu-clock 199 Hz / DWARF capture covers 12.04 s and 553,490 replies. Per-thread CPU comes from matched `/proc` snapshots, not sampled percentages. Receive-role identity is confirmed by `raw_route_port_t::poll` / `node_receive_role_t` stacks.

Before receive CPU is **13.047 µs/reply** in this instrumented capture. Inclusive costs: raw poll **12.686**, C1 complete **5.250**, post_completions **4.538**, dispatch_completion **4.456**, pthread_cond_signal **4.074**. These costs overlap and are not additive. Completion workers consume **3.044–3.066 µs/reply each**. Raw data: `profile-before/`, `profile-before.json`, `profile-before-summary.txt`. Instrumented demand differs from the crecv3 unprofiled baseline and is not substituted for it.

Alternatives were evaluated, preserving independent callback turns throughout:

| Variant | Receive µs/reply | Completion workers, µs/reply each | Source total µs/reply | Reservation bytes | Decision |
|---|---:|---:|---:|---:|---|
| crecv3 / HEAD baseline | 15.800 | 2.191–2.235 | 82.152 | 368 | Required before cohort |
| Poll collection, intermediate erased work hook (`round1/`) | 15.947 | 1.923–1.962 | 79.442 | 384 | Remove redundant per-item owner/hook storage |
| Poll collection, direct C1 Asio hook (`round2/`) | 15.824 | 1.941–1.966 | 79.833 | 368 | **Selected**: consistent worker reduction, no reservation growth |
| Direct C1 hook without poll collection (`round3/`) | 15.987 | 2.202–2.243 | 84.328 | 368 | No observed receive/worker benefit |

A single posted lambda that drains callbacks serially violates S08 §4.4 and is retained only as a negative control. The selected runtime exactly matches the measured `round2` runtime sources and CMake file: `selected-runtime-equivalence.json`. The final additional tests do not enter the benchmark build (`TEST_PROBES=OFF`, `BUILD_TESTS=OFF`). The selected cohort is reused by source identity, not mixed with faster individual runs from another variant. All three complete after cohorts remain available.

The selected after profile (`round2/profile-after/`, `profile-after.json`) also fails to show a whole receive-role reduction: **15.885 µs/reply**, workers **2.566–2.596 each**, versus before **13.047** and **3.044–3.066**. This reinforces the limitation: cheaper publication and workers did not establish a cheaper complete receive role. No optimized receive minimum or spec-imposed CPU floor is claimed.

## Changes with source locations

Paths below are relative to `framework/languages/cpp/`.

- `framework/src/runtime/dispatch/completion_turn.hpp:13,27,42`: reuse one completion scope; the C1 reservation directly implements the existing Asio operation. No extra per-item owner identity or invocation pointer is stored beyond Asio's own hook and the existing dispatcher borrow. A stack-local publication chain has no worker-facing ready membership.
- `framework/src/runtime/dispatch/provider_task_context.cpp:38,83,100–149`: the existing process dispatcher bulk-publishes independent operations through its existing io_context implementation. Asio work accounting begins when an item enters the unpublished chain, so last-owner close cannot retire workers before publication. Existing worker count, work guard, mutex, ready queue and wake propagation remain in Asio. One producer publication/wake occurs per nonempty same-dispatcher poll batch; existing worker wake propagation may wake further workers.
- `framework/src/runtime/foundation/operation_registry.cpp:189,214,264`: the reserved item is the queued item and deletes itself after its own callback. The old separately allocated Asio function operation and C1 std::function publication wrapper are removed. The C1 unique_ptr chain link is replaced by the same intrusive link used by Asio. Cancellation subscriptions still stop outside C1's mutex; exceptions still release the reservation and do not block unrelated callbacks.
- `framework/src/runtime/backend/raw_route_port.cpp:408`: gather logical publications during the existing binding poll and publish after releasing the poller mutex. The binding's physical drain, socket, poller and cursor remain unchanged.
- `tests/Zlink.Framework.ContractTests/test_cpp_framework_raw_route_port_contract.cpp:1053`: 32 real inproc router requests/replies complete in one raw-port poll. Every callback checks a dispatcher turn, absence of a serial turn and a different thread from the poll producer. One callback blocks while the other 31 complete; one consumer throws; duplicates are rejected and every callback runs once.
- `tests/Zlink.Framework.UnitTests/test_cpp_framework_operation_registry.cpp:92,166,212,260`: zero publication allocations; a 64-item terminal chain with an independently blocked first callback; shutdown of an already queued batch with all workers occupied; last-owner shutdown while a 64-item poll-publication chain is still unpublished. Both shutdown cases deliver each item once.
- `CMakeLists.txt:1314–1320` and unit-test `:32`: Linux aligned_alloc wrapping extends the existing allocation counter. Asio's node allocation bypasses C++ new, so new-only counting cannot prove its removal.

## Decision-owner table

S01 = `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md`; S08 = `08-messaging-hot-path.ko.md` in the same directory. Counts mean decision locations, not storage fields or runtime item counts. Before locations refer to HEAD.

| Decision | Spec owner | Before location(s) | After owner/location | Before → after |
|---|---|---|---|---:|
| Logical terminal take / exactly once | S01 §9–10 | `foundation/operation_registry.cpp:118–124,510–523` | Same C1 take_completion / take | 1 → 1 |
| Completion execution scope and identity | S01 §11:490–544; S08 §4.4:306–326 | `dispatch/provider_task_context.cpp:174–194` | Same process dispatcher; shared scope at `provider_task_context.cpp:38`, C1 hook at `operation_registry.cpp:214` | 1 → 1 |
| Ready membership, wake and worker selection | S01 §11; S08 §4.4 | `provider_task_context.cpp:74–86` delegates to Asio | Same Asio scheduler via `provider_task_context.cpp:83–97` | 1 → 1 |
| Accepted logical callback drain | S01 §11 | `foundation/operation_registry.cpp:134–156,793–799` | Same reservation drain_state; no new count | 1 → 1 |
| Physical completion drain / receive role | S08 §4.4; Framework Guidelines | `backend/raw_route_port.cpp:404–456` calls binding poller | Same binding poller; Framework groups only logical callback publication | 1 → 1 |

Decision locations / rules **5 → 5**. Handoff representations **2 → 1**: reserved item + allocated function operation become one reserved operation. The existing per-item chain link is removed. The new stack-local chain and TLS nesting pointer only group publication; no worker reads them, and they make no acceptance, ready-selection, capacity or wake decision. They replace the old handoff representation rather than adding a broker or second runnable queue. Asio's existing work counter remains its shutdown authority; C1's existing drain counter remains its logical-reservation authority.

Ownership: C1 terminal/drain and existing Asio dispatcher scheduling. Classification **A — internal representation adaptation to the existing contract**, with no spec change, contract relaxation or upper-layer compensation for Core/binding behavior.

Cross-language comparison before implementation: .NET `Runtime/Backend/DotNet/ZLinkCompletionDispatcher.cs:12–17,27–43` posts a reserved WorkItem to ThreadPool; Java/Kotlin share `ZLinkServiceCompletionDispatcher.java:61–92,119–141`, which posts independent registered WorkItems to the common pool. Node `runtime/backend/node/node-raw-mesh-backend.ts:1871–1884` defers native results with queueMicrotask and Promise continuations. Node's single-event-loop blocking behavior remains the language policy explicitly preserved by S08 §4.4. No observable contract or worker-policy difference is introduced in any language; no four-language spec gap is created.

## Tests and negative controls

- Full Release build with `--parallel 3 -- -k 0`: **passed**, final all-target command (182 updated build/link steps), `build-all.log`.
- Unit + contract CTest: **297/297 passed**, 231.73 s (`test-unit-contract.log`). This includes the final additional terminal-batch and both queued/unpublished shutdown cases.
- Required format command: **passed** (`format-final.log`). `git diff --check`: passed.
- Focused registry/dispatcher/raw-route checks: **6/6 passed** before the final all-target validation; the final full suite validates the selected runtime with all added tests.
- No remaining required build, test or format failure. The receive CPU performance goal remains unmet as stated at the start.

- Real poll batch: 32 items, one poll, independent dispatcher turns, each once (`test-poll-details.log`; final focused/full test logs).
- Existing terminal batch: 64 items; whichever callback starts first blocks while the other 63 finish (`--terminal-batch`).
- Shutdown: 64 queued items with all workers occupied, and 64 unpublished items while last-owner close begins. Close cannot return early and each is delivered once.
- Allocation negative control links the exact HEAD registry and dispatcher snapshots with the same assertion: **32 publication allocations / 32 items**, then fails the zero-allocation assertion. Final implementation: **0 / 32**, passes (`calibration.json`, `calibration-*.log`). Linux aligned_alloc and C++ new are both covered.
- Policy negative controls are scratch-only implementations. Serial delivery is tested against the real 64-item terminal batch; inline delivery against the real 32-item raw-port poll. Both must fail the independence/new-turn assertion (`negative-policies.json`, `negative-serial.log`, `negative-inline.log`). No invalid serial drain is retained in product code.

Required commands, from `framework/languages/cpp`:

```sh
flock -s /root/jobs/fx/cx/measure.lock /root/jobs/fx/cx/cpp-build-slot.sh \
  cmake --build --preset linux-ninja-release --parallel 3 -- -k 0
flock -s /root/jobs/fx/cx/measure.lock \
  ctest --preset linux-ninja-release -L '^(unit|contract)$'
```

From repository root: `scripts/format/format.sh --check cpp`.

## Allocation and retained storage

Linux x86-64 / libstdc++, same 32 registrations and independent deliveries, Release unit-test flags. Source snapshots and commands: `build-calibration.sh`, `before-registry.cpp`, `before-provider.cpp`, `before-header/`. sizeof compiler checks are in `size-*.log`. These counts cover this calibration, not a complete network request or allocator metadata.

| Scope | Before | Selected after |
|---|---:|---:|
| C1 reservation | 368 B | 368 B |
| Calls for 32 registrations, including callback/map storage | 99 | 99 |
| Requested registration bytes | 17,960 B | 17,960 B |
| Publication allocation calls for 32 items | 32 | 0 |
| Requested publication bytes | 3,072 B (96 B/item) | 0 B |
| Combined scoped calls / requested bytes | 131 / 21,032 B | 99 / 17,960 B |

The reservation itself still allocates before submission. The selected representation removes one later allocation/free/cache path per completion without growing the pending reservation. Combined figures are lifetime allocation totals, not peak retained memory.

## csubmit comparison

Before is exactly the crecv3 after cohort at `5ecae5de8b`: `crecv3/measure/run{1,2,3}/`. Selected after: `round2/measure/run{1,2,3}/`. Both patterns use 1024 B, three fresh source/target process pairs, 30 s warmup / 5 s active, source affinity 0–7 and target 8–15, with paired zlink-cpp controls. The unchanged runner and CPU collector differ only in output/build-root paths (`source-state.json`, `runner_equivalence`). The complete after campaign runs under exclusive `flock -o`; builds use the required shared lock and build slot. MemAvailable starts at **27,907,088 KiB**, and the CPU collector asserts ≥12 GiB at every snapshot. Endpoint cleanup uses SIGTERM; perf cleanup uses SIGINT/SIGTERM only.

Every final cell validates completed = target received, zero errors, abandoned requests, header-validation failures, target rejects and drain contamination. Throughput is the median of full active phases. Thread CPU is normalized to logical completions in the same approximately 3 s interior snapshot window (target receives for send), including every sampled thread. Same-name threads are ranked within each run before medians; ranks are not stable TIDs. Medians need not sum exactly. Retiring threads can lose their final interval. CPU time is service demand, not latency. All TIDs and runs are in `threads.csv` and `measurements.json`.

The old benchmark metadata string still says Framework 0.32.0; HEAD/source hashes identify this 0.33.0 job. The same deployed Core/C++ packages are used; hashes match crecv3 (`baseline-artifact-check.json`). No Core/binding update is claimed. Different supply cadence can change Core batching; endpoint deltas are not all attributed to removed C++ instructions.

| Pattern / implementation | Before msg/s | After msg/s | Change | F/B before → after | Source CPU µs/msg before → after | Target CPU µs/msg before → after |
|---|---:|---:|---:|---:|---:|---:|
| request-backpressure / zlink-cpp | 243,153.1 | 248,921.8 | +2.4% | — | 9.662 → 9.579 | 10.491 → 10.025 |
| request-backpressure / zlink-framework-cpp | 50,895.7 | 50,621.5 | -0.5% | 0.2093 → 0.2034 | 82.152 → 79.833 | 61.334 → 61.817 |
| send-saturation / zlink-cpp | 617,595.4 | 613,270.3 | -0.7% | — | 3.005 → 3.060 | 3.414 → 3.390 |
| send-saturation / zlink-framework-cpp | 173,869.5 | 176,444.4 | +1.5% | 0.2815 → 0.2877 | 10.268 → 9.891 | 14.661 → 14.237 |

| Pattern / implementation | Before range, msg/s | After range, msg/s |
|---|---:|---:|
| request-backpressure / zlink-cpp | 243,039.2–246,398.8 | 247,106.4–256,282.3 |
| request-backpressure / zlink-framework-cpp | 50,692.2–52,279.3 | 50,612.6–50,800.0 |
| send-saturation / zlink-cpp | 589,121.3–635,272.3 | 607,922.9–627,275.5 |
| send-saturation / zlink-framework-cpp | 172,263.4–177,685.4 | 174,833.8–180,443.2 |

The receive role is source `bench_cpp_clien #1` in the request table, confirmed independently by both profiles. Ranks #2–#9 are the eight completion workers. Caller CPU changes 17.887 → 17.577 µs/request. The chosen data demonstrate a worker/whole-source CPU benefit, **not a receive-role or request-throughput improvement**. A source-only publication change does not establish the 0.6 end-to-end target.

### Before/after per-thread CPU

All median thread ranks at or above 0.05 µs/message in either cohort are shown. Smaller threads remain in CSV/JSON.

| Pattern | Implementation | Endpoint | Thread name / CPU rank | Before µs/msg | After µs/msg | Change |
|---|---|---|---|---:|---:|---:|
| request-backpressure | zlink-framework-cpp | source | `ZLINKbg/IO/0 #1` | 15.941 | 15.401 | -0.540 |
| request-backpressure | zlink-framework-cpp | source | `ZLINKbg/IO/2 #1` | 15.081 | 14.791 | -0.290 |
| request-backpressure | zlink-framework-cpp | source | `bench_cpp_clien #1` | 15.800 | 15.824 | +0.024 |
| request-backpressure | zlink-framework-cpp | source | `bench_cpp_clien #2` | 2.235 | 1.966 | -0.269 |
| request-backpressure | zlink-framework-cpp | source | `bench_cpp_clien #3` | 2.225 | 1.961 | -0.264 |
| request-backpressure | zlink-framework-cpp | source | `bench_cpp_clien #4` | 2.221 | 1.958 | -0.264 |
| request-backpressure | zlink-framework-cpp | source | `bench_cpp_clien #5` | 2.213 | 1.956 | -0.257 |
| request-backpressure | zlink-framework-cpp | source | `bench_cpp_clien #6` | 2.210 | 1.953 | -0.257 |
| request-backpressure | zlink-framework-cpp | source | `bench_cpp_clien #7` | 2.209 | 1.941 | -0.268 |
| request-backpressure | zlink-framework-cpp | source | `bench_cpp_clien #8` | 2.206 | 1.941 | -0.265 |
| request-backpressure | zlink-framework-cpp | source | `bench_cpp_clien #9` | 2.191 | 1.941 | -0.250 |
| request-backpressure | zlink-framework-cpp | source | `bench_cpp_clien #10` | 0.135 | 0.135 | +0.000 |
| request-backpressure | zlink-framework-cpp | source | `bench_cpp_clien #11` | 0.134 | 0.135 | +0.001 |
| request-backpressure | zlink-framework-cpp | source | `caller #1` | 17.887 | 17.577 | -0.311 |
| request-backpressure | zlink-framework-cpp | target | `ZLINKbg/IO/0 #1` | 12.244 | 13.453 | +1.209 |
| request-backpressure | zlink-framework-cpp | target | `ZLINKbg/IO/1 #1` | 15.764 | 15.717 | -0.047 |
| request-backpressure | zlink-framework-cpp | target | `app-worker #1` | 9.175 | 9.836 | +0.662 |
| request-backpressure | zlink-framework-cpp | target | `app-worker #2` | 7.267 | 4.927 | -2.340 |
| request-backpressure | zlink-framework-cpp | target | `app-worker #3` | 1.456 | 3.409 | +1.954 |
| request-backpressure | zlink-framework-cpp | target | `bench_cpp_frame #1` | 0.158 | 0.153 | -0.005 |
| request-backpressure | zlink-framework-cpp | target | `bench_cpp_frame #2` | 0.146 | 0.144 | -0.002 |
| request-backpressure | zlink-framework-cpp | target | `bench_cpp_frame #3` | 0.141 | 0.142 | +0.002 |
| request-backpressure | zlink-framework-cpp | target | `bench_cpp_frame #4` | 0.131 | 0.134 | +0.003 |
| request-backpressure | zlink-framework-cpp | target | `bench_cpp_frame #5` | 0.124 | 0.124 | -0.000 |
| request-backpressure | zlink-framework-cpp | target | `bench_cpp_frame #6` | 0.114 | 0.114 | +0.000 |
| request-backpressure | zlink-framework-cpp | target | `bench_cpp_frame #7` | 0.105 | 0.101 | -0.004 |
| request-backpressure | zlink-framework-cpp | target | `bench_cpp_frame #8` | 0.098 | 0.092 | -0.006 |
| request-backpressure | zlink-framework-cpp | target | `bench_cpp_frame #9` | 0.087 | 0.081 | -0.005 |
| request-backpressure | zlink-framework-cpp | target | `receive #1` | 13.432 | 13.461 | +0.029 |
| request-backpressure | zlink-cpp | source | `ZLINKbg/IO/0 #1` | 2.736 | 2.738 | +0.002 |
| request-backpressure | zlink-cpp | source | `ZLINKbg/IO/2 #1` | 2.860 | 2.880 | +0.019 |
| request-backpressure | zlink-cpp | source | `caller #1` | 4.231 | 4.069 | -0.162 |
| request-backpressure | zlink-cpp | target | `ZLINKbg/IO/0 #1` | 2.372 | 1.964 | -0.408 |
| request-backpressure | zlink-cpp | target | `ZLINKbg/IO/1 #1` | 3.653 | 3.490 | -0.163 |
| request-backpressure | zlink-cpp | target | `bench_cpp_zlink #1` | 0.137 | 0.142 | +0.005 |
| request-backpressure | zlink-cpp | target | `bench_cpp_zlink #2` | 0.125 | 0.129 | +0.004 |
| request-backpressure | zlink-cpp | target | `bench_cpp_zlink #3` | 0.116 | 0.119 | +0.003 |
| request-backpressure | zlink-cpp | target | `bench_cpp_zlink #4` | 0.109 | 0.108 | -0.001 |
| request-backpressure | zlink-cpp | target | `bench_cpp_zlink #5` | 0.099 | 0.101 | +0.002 |
| request-backpressure | zlink-cpp | target | `bench_cpp_zlink #6` | 0.090 | 0.087 | -0.003 |
| request-backpressure | zlink-cpp | target | `bench_cpp_zlink #7` | 0.072 | 0.081 | +0.008 |
| request-backpressure | zlink-cpp | target | `bench_cpp_zlink #8` | 0.068 | 0.068 | +0.000 |
| request-backpressure | zlink-cpp | target | `receive #1` | 3.578 | 3.363 | -0.215 |
| send-saturation | zlink-framework-cpp | source | `ZLINKbg/IO/0 #1` | 4.865 | 4.722 | -0.143 |
| send-saturation | zlink-framework-cpp | source | `ZLINKbg/IO/2 #1` | 0.051 | 0.051 | +0.000 |
| send-saturation | zlink-framework-cpp | source | `bench_cpp_clien #1` | 0.148 | 0.087 | -0.061 |
| send-saturation | zlink-framework-cpp | source | `caller #1` | 4.904 | 4.764 | -0.140 |
| send-saturation | zlink-framework-cpp | target | `ZLINKbg/IO/0 #1` | 4.186 | 3.681 | -0.505 |
| send-saturation | zlink-framework-cpp | target | `ZLINKbg/IO/1 #1` | 0.060 | 0.060 | +0.000 |
| send-saturation | zlink-framework-cpp | target | `app-worker #1` | 2.968 | 2.901 | -0.067 |
| send-saturation | zlink-framework-cpp | target | `app-worker #2` | 2.711 | 2.625 | -0.086 |
| send-saturation | zlink-framework-cpp | target | `bench_cpp_frame #1` | 0.139 | 0.140 | +0.002 |
| send-saturation | zlink-framework-cpp | target | `bench_cpp_frame #2` | 0.127 | 0.127 | -0.001 |
| send-saturation | zlink-framework-cpp | target | `bench_cpp_frame #3` | 0.121 | 0.118 | -0.003 |
| send-saturation | zlink-framework-cpp | target | `bench_cpp_frame #4` | 0.110 | 0.112 | +0.001 |
| send-saturation | zlink-framework-cpp | target | `bench_cpp_frame #5` | 0.101 | 0.102 | +0.001 |
| send-saturation | zlink-framework-cpp | target | `bench_cpp_frame #6` | 0.093 | 0.095 | +0.002 |
| send-saturation | zlink-framework-cpp | target | `bench_cpp_frame #7` | 0.081 | 0.080 | -0.000 |
| send-saturation | zlink-framework-cpp | target | `bench_cpp_frame #8` | 0.073 | 0.072 | -0.001 |
| send-saturation | zlink-framework-cpp | target | `receive #1` | 4.122 | 4.035 | -0.086 |
| send-saturation | zlink-cpp | source | `ZLINKbg/IO/0 #1` | 1.572 | 1.579 | +0.007 |
| send-saturation | zlink-cpp | source | `caller #1` | 1.418 | 1.459 | +0.041 |
| send-saturation | zlink-cpp | target | `ZLINKbg/IO/0 #1` | 1.454 | 1.460 | +0.006 |
| send-saturation | zlink-cpp | target | `bench_cpp_zlink #1` | 0.113 | 0.110 | -0.002 |
| send-saturation | zlink-cpp | target | `bench_cpp_zlink #2` | 0.102 | 0.096 | -0.006 |
| send-saturation | zlink-cpp | target | `bench_cpp_zlink #3` | 0.087 | 0.087 | -0.000 |
| send-saturation | zlink-cpp | target | `bench_cpp_zlink #4` | 0.078 | 0.078 | -0.000 |
| send-saturation | zlink-cpp | target | `bench_cpp_zlink #5` | 0.069 | 0.067 | -0.002 |
| send-saturation | zlink-cpp | target | `bench_cpp_zlink #6` | 0.059 | 0.060 | +0.001 |
| send-saturation | zlink-cpp | target | `bench_cpp_zlink #7` | 0.054 | 0.052 | -0.002 |
| send-saturation | zlink-cpp | target | `receive #1` | 1.331 | 1.319 | -0.012 |

## Capture exclusion and remaining limit

One initial after-profile attempt is excluded: perf's post-recording build-id cache population wrote 2.7 GiB and exceeded the collector's 30 s finalization wait before CPU snapshots were persisted. It is retained as `profile-after-cache-finalization/`. The corrected collector disables cache updates and persists snapshots before finalization; in-window sampling, affinity and durations remain unchanged. No product timeout or retry budget was increased.

The receive-role goal remains open. The data do not justify adding another queue, wake flag, worker-assignment authority, callback serialization or inline execution on the receive stack. Further receive work requires another causal profile/repro of the remaining path; no speculative Core defect or immutable spec CPU floor is asserted. No GitHub issue was opened because this job forbids GitHub operations.

Refactoring inspection: performance 2 findings (per-item publication allocation and boxed handoff); POSDDD 0 ownership/gate violations; unnecessary code 1 C1 publication wrapper. The allocation/boxed wrapper is removed, the existing owners are reused, and no identified cleanup is deferred. The unachieved receive CPU target is reported above rather than counted as a fixed finding.
