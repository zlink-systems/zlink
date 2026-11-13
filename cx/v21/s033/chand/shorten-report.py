from pathlib import Path
p=Path('/root/jobs/fx/cx/v21/s033/chand/report.md');s=p.read_text().replace('mesh_node_host_service.cpp:3383','mesh_node_host_service.cpp:3381')
a=s.index('The job-local client copies');b=s.index('### Full source path',a)
s=s[:a]+'''The job-local client instruments copies of the current receive/dispatcher sources and reuses unchanged `crecv3` objects. `library-identity.json` verifies the seven changed Framework source hashes against that build's recorded state; all other Framework files have no tree difference. Generated source identities: `source-manifest.json`.

`run.py`: public TCP request-backpressure, condition B, 1024 B, the benchmark's unlimited application window, source CPUs 0–7 / target 8–15, 30 s warmup / 5 s active. Counters bracket the complete active phase and drain. Exclusive measure lock; MemAvailable >= 12 GiB; SIGTERM cleanup. Builds used the requested shared-lock/build-slot/CMake preset command, parallelism 3, from job-local `framework/languages/cpp`. No perf profile was repeated.

Source/target agree: **241,991 completions, zero errors/in-flight; 195,683 `rotate()` entries and returns; 241,991 receive-side completion posts**. Handoff attempts/successes are structurally zero: no assignment function exists to instrument. Only their rotation denominator is measured. Raw evidence: `counts-raw.json`, `counts-summary.json`, `quality.json`, `results.json`.

'''+s[b:]
a=s.index('Heap means intercepted');b=s.index('### Small counting/contract harness',a)
s=s[:a]+'''Heap calls include malloc/calloc/realloc/aligned_alloc/posix_memalign and C++ new through those allocators. Mutex counts include successful pthread lock/try-lock calls across Framework/binding/native libraries, excluding atomics and implicit condition-wait reacquisition. They do **not** measure lock-wait duration. Signals/broadcasts are wake requests; wait returns are wake observations, **not kernel scheduling switches or one-to-one signal matches**. Whole-phase counts include management/control work. Worker wake propagation can exceed one per completion.

'''+s[b:]
a=s.index('`harness.cpp` uses');b=s.index('| Scope | Caller',a)
s=s[:a]+'''`harness.cpp` uses the current real C1 registry/dispatcher with 64 empty-payload operations, excluding startup; this scope is smaller than a network request. `harness-direct.log`:

'''+s[b:]
a=s.index('Worker wake counts depend');b=s.index('## 3. Design alternatives',a)
s=s[:a]+'''Calibration reports exactly **1 malloc / 1 aligned allocation / 1 mutex acquisition / 1 signal**. `check-counts.py` accepts the real capture and rejects a copy missing one post. Burst wake counts depend on scheduling and do not replace the benchmark counts.

C1 checks pass: 64 deliveries once and 64 rejected duplicate terminals; one terminal each for timeout/cancel/shutdown; one delivery for preparation and callback exceptions; unrelated callback finishes while the first blocks. These are focused baseline checks, not candidate/socket-level handoff validation.

'''+s[b:]
a=s.index('Replace receive placement inside');b=s.index('### B —',a)
s=s[:a]+'''Keep placement in the **existing offload executor**. Once per completed rotation, attempt without waiting to reserve a particular currently idle, already-created worker exclusively for the next receive rotation. Failure keeps reception and dispatches independently as today. Replace queued-role/idle-count membership with worker-owned idle/assignment membership; no second role queue, placement broker, new thread, raised limit or start-confirmation wait.

Hold the existing reserved completion items through the rotation. After poll/drain, receive turn, completion-publication stack and lane scopes have returned, publish siblings independently and execute **at most one** item locally with the existing dispatcher identity. Never serially drain callbacks on the former receiver. Role-owned socket/poller/cursor/permits/management state transfers as one role. The same reserved item uses one replacement placement algorithm and its mandatory fallback; no old/new pipeline option.

**Estimated completion-path counts:** posts and publication allocations **1 → 1−h/request**, if sibling Asio publication is unchanged; add **h successor notifications/request**, and at most **0.8086 non-waiting assignment attempts/request** in this capture. Small-harness receive mutex acquisitions: **3 → 1 + 2(1−h)**, plus successful rotation-level assignment acquisition. Assignment/internal settlement must add **no blocking or lock wait**. Worker queue takes fall by h; the former receiver still executes the callback's task work. Net wake savings are unproven: completion wake may merely become receive wake. These are estimates, not measured whole-request savings.

Files under `src/runtime/`: `dispatch/offload_executor.{hpp,cpp}`, `mesh/mesh_node_host_service.cpp`, `dispatch/completion_turn.hpp`, `dispatch/provider_task_context.cpp`, `foundation/operation_registry.cpp`; pool/registry/real-poll tests. No binding/Core/public API change.

Risks: S01:508 premature same-stack execution; S02 §2–3 bypassing captured gate/Yield scheduling; §13 entering application before relinquishing reception; shutdown losing an assigned role or unpublished reservation. Retain C1's sole terminal slot, captured application context, per-item exception isolation and accepted-work drain through callback-capture destruction, including local execution. Do not flush callbacks from a poll/rotate destructor.

'''+s[b:]
a=s.index('Keep existing receive placement.');b=s.index('## 4. Contract acceptance',a)
s=s[:a]+'''Retain reception. Turn the reserved C1 item into the dispatcher operation and publish independent items in bulk at the poll boundary. This is `cpoll`'s representation: its report found no throughput improvement. Public callbacks/coroutines stay on workers. Do not move locking/callback-running `task_shared_state_t::complete` onto reception; only existing internal result classification/settlement may remain there.

**Estimated completion-path counts:** publication allocations **1 → 0/request**; independent executions stay **1/request**; producer posts/signals approximately **1/b** for b completions per publication batch. Worker queue takes, wake propagation and task work remain. Small-harness receive mutex budget approximately **1 + 1/b/request** when accepted reservation lifetime replaces the stopped check. This retains existing dispatcher synchronization and introduces no internal-settlement wait. It does **not** prove no contention in the existing fallback; literal zero lock-wait on all receive work is not established by either baseline counts or this representation.

Files: `dispatch/completion_turn.hpp`, `dispatch/provider_task_context.cpp`, `foundation/operation_registry.cpp`, `backend/raw_route_port.cpp`, allocation/lifetime/poll tests. Owners and worker policy stay unchanged. S01 §11 risk: accepted work disappearing before deferred publication during close; account before deferral. S02 §1–3/§13 remain unchanged. A single posted callback-draining lambda is invalid.

**Recommend A for the next design review**, with independent reserved-item fallback. Require counters for successor wakes, assignment successes and residual publications before accepting a performance claim. B is smaller but has no evidence of reaching 7.5 µs/message. Neither has measured time savings here; expected count reductions do not establish recovery of the roughly 14.7 µs/message gap. The PM no-wait condition must be verified at the internal settlement/assignment boundary, not inferred from the existence of an idle waiter or from these lock-call counts.

'''+s[b:]
a=s.index('For both alternatives, retain');b=s.index('Classification:',a)
s=s[:a]+'''For either replacement, retain the C1 slot and reservation lifetime. Race reply against timeout/cancel/shutdown: **one delivery and one winning terminal**. Inject preparation/callback exceptions: **no accepted operation gets zero deliveries**, and siblings still progress. Inject acceptance/publication/assignment failure and close while an item is unpublished/assigned: retain the same item; no catch-and-drop path. Existing focused checks are the baseline, not future implementation proof.

Cross-language source check: .NET `framework/languages/dotnet/src/Zlink.Framework/Runtime/Backend/DotNet/ZLinkCompletionDispatcher.cs:13–17,27–43` uses independent pool turns; Java/Kotlin share `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceCompletionDispatcher.java:51–81,101–125`, independently submitting items; Node `framework/languages/node/packages/framework/src/runtime/backend/node/node-raw-mesh-backend.ts:1871–1884` uses microtasks and has no successor on its single event loop. Public terminal/gate contracts stay identical; no cross-language behavior/spec change is proposed.

'''+s[b:]
a=s.index('Classification:');s=s[:a]+'''Classification: **A — existing-contract adaptation**, pending supervisor selection; absent optional handoff is not a resource-assignment bug. Decision sites before/after: **placement 1→1, C1 terminal 1→1, completion turn policy 1→1, dispatcher scheduler 1→1**. A replaces placement membership; B replaces publication storage. Neither owner was changed in this job.

Validation: isolated build, counter calibration, positive/negative count checks and focused C1 harness passed. No remaining test failure. No full suite or candidate/socket handoff tests: runtime design was not implemented. Baseline counts are measured; proposed savings are estimates; no after-performance result exists.

리팩토링 점검: 성능 2건, POSDDD 0건, 불필요 코드 0건 발견. 고친 항목: 없음. 넘긴 항목: 완료 게시 할당, 완료 게시·깨우기와 수신 자원 배치 비용.
'''
p.write_text(s);print(len(s.encode()),len(s.split()))
