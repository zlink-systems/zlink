# C++ request completion receive-role handoff — read-only diagnosis

**C++ does not implement S08 §4.4 handoff at HEAD `5ecae5de8b`.** The measured source completed **241,991 requests in 195,683 receive rotations: 0 attempts, 0 successes, 0 successes/rotation**. This is absence of an assignment operation, not unsuccessful resource assignment. Recommend alternative A below for the supervisor's design review. Its counts do **not** establish the 0.6 throughput target: handing reception to another worker also requires notification.

Worktree `/root/worktree/zlink-3867h`, branch `perf/3867-cpp-receive-handoff`; clean before and after. No product/spec changes, GitHub access, synchronization, Core C header inspection, sub-agents, commit or branch operations. Only counters and scratch harnesses were implemented. Artifacts below are relative to this report.

## 1. Current decision owners

Path abbreviations: `S01`, `S02`, `S08` are respectively `01-submit-and-completion.ko.md`, `02-handler-turn-and-execution-gate.ko.md`, `08-messaging-hot-path.ko.md` under `framework/doc/framework/common/spec/server/01-execution/`. C++ implementation paths below start at `framework/languages/cpp/framework/`. `Asio` is `core/external/boost/boost/asio/detail/impl/scheduler.ipp` (third-party C++, not Core C headers).

“Sites” counts places making the specified decision, not every caller or synchronization operation. Physical binding terminal, C1 logical terminal and coroutine wait delivery have different identities; they must not become competing terminal authorities.

| Decision | Spec owner, file:line | Current C++ file:line | Sites |
|---|---|---|---:|
| Assign initial receive resource | S08:306–316,326 | `src/runtime/dispatch/offload_executor.cpp:172` accepts a role; `:360` exclusively dequeues it. Host requests initial placement at `src/runtime/mesh/mesh_node_host_service.cpp:3381`. No reassignment decision exists. | 1 placement owner; 0 handoff sites |
| Drive receive rotations / poller | S08:307–314; `bindings/doc/spec/async-execution-model.ko.md:67–75` | `src/runtime/mesh/mesh_node_host_service.cpp:2088–2093` retains the role until stop; `:2097–2330` rotates. `src/runtime/backend/raw_route_port.cpp:404–451` serializes and drives the binding poller. | 1 rotation driver; 1 poll entry |
| Take physical completion | Binding async-execution model:67–75,104 | `bindings/cpp/src/Runtime/Eventing/poller.cpp:404` delegates to `Runtime/Messaging/completion_owner.cpp:668` (paths under `bindings/cpp/src/`). Framework observer: `src/runtime/backend/raw_binding_adapter.hpp:146–198`. | 1 binding owner |
| Take logical terminal | S01:445–463,487 | `src/runtime/foundation/operation_registry.cpp:123–128`; common take lookup/lock `:513–524`; normal complete `:532–552`. Timeout/cancel/shutdown use the same reserved completion slot. | 1 C1 authority |
| Create independent completion turn | S01:490–519; S08:302–316 | `src/runtime/foundation/operation_registry.cpp:250–275` passes `inline_allowed=false` and a non-null owner to `src/runtime/dispatch/provider_task_context.cpp:174–191`. Scope is installed inside the posted work, never by the receive role. | 1 C1 turn-placement policy |
| Select a completion worker | S08:324,326 preserves worker policy | `src/runtime/dispatch/provider_task_context.cpp:56–64,74–79` delegates to Asio; `Asio:449–490` takes an independent ready operation. | 1 scheduler owner |
| Wake completion workers | S08:293,324,328–331 | `Asio:363–366,648–659`; worker-side propagation at `:483–484`. These are producer notification and worker propagation within one scheduler, not two placement authorities. | 2 notification sites; 1 owner |
| Resume caller continuation / enter user code | S01:508–519; S02:76–135,490–513 | `include/zlink/framework/contracts/dispatch/task.hpp:462–479` selects the captured scheduler or dispatcher; `:509–527` claims the wait and restores an owned turn. Coroutine entry is `:383` / `:408`; callback entry `:751–753`. Native await routing is `:323–362`. | 1 wait-claim authority; 2 coroutine entry sites plus 1 callback branch |
| Drain accepted completions at shutdown | S01:539–544; S08:313–314 | `src/runtime/foundation/operation_registry.cpp:162–178,201–205,794–799` owns accepted reservation lifetime. `src/runtime/dispatch/provider_task_context.cpp:67–71,107–131` owns process dispatcher close/join. Offload role lifetime is separate: `src/runtime/dispatch/offload_executor.cpp:223–260`. | 3 distinct lifetime scopes, 1 owner each |

The current `start_role()` only enqueues and signals. `_idle_workers` plus a queued role is not an exclusive assignment: worker selection happens later under the shared pool mutex (`offload_executor.cpp:330–370`). Therefore neither existing function is a §4.4 success primitive.

The binding observer's coroutine at `raw_binding_adapter.hpp:186` is Framework internal. It may classify and hand a terminal to C1. This does **not** authorize a public coroutine resume: `task.hpp:383,408` executes user code. `task_shared_state_t::complete` also invokes callbacks and registrations (`task.hpp:622–658`); moving that entire function onto reception would cross the PM boundary and acquire its mutex.

## 2. Counts, including the first handoff audit

The job-local client instruments copies of the current receive/dispatcher sources and reuses unchanged `crecv3` objects. `library-identity.json` verifies the seven changed Framework source hashes against that build's recorded state; all other Framework files have no tree difference. Generated source identities: `source-manifest.json`.

`run.py`: public TCP request-backpressure, condition B, 1024 B, the benchmark's unlimited application window, source CPUs 0–7 / target 8–15, 30 s warmup / 5 s active. Counters bracket the complete active phase and drain. Exclusive measure lock; MemAvailable >= 12 GiB; SIGTERM cleanup. Builds used the requested shared-lock/build-slot/CMake preset command, parallelism 3, from job-local `framework/languages/cpp`. No perf profile was repeated.

Source/target agree: **241,991 completions, zero errors/in-flight; 195,683 `rotate()` entries and returns; 241,991 receive-side completion posts**. Handoff attempts/successes are structurally zero: no assignment function exists to instrument. Only their rotation denominator is measured. Raw evidence: `counts-raw.json`, `counts-summary.json`, `quality.json`, `results.json`.

### Full source path, calls per completed request

| Executing resource | Heap calls | Completion posts | `cond_signal` requests | `cond_broadcast` requests | Successful condition-wait returns | Explicit mutex acquisitions |
|---|---:|---:|---:|---:|---:|---:|
| Caller | 17.911 | 0.000087 | 0.999 | 1.999 | 0 | 36.550 |
| Receive role | 9.174 | **1.000000** | **1.001** | 4.848 | **0** | **44.303** |
| Completion workers, aggregate | 7.002 | 0 | 1.202 | 2.000 | 1.201 | 6.002 |
| Other source threads, including binding/Core I/O and management | 2.237 | 0.000017 | 0.002 | 0.002 | 0.002 | 9.352 |

Heap calls include malloc/calloc/realloc/aligned_alloc/posix_memalign and C++ new through those allocators. Mutex counts include successful pthread lock/try-lock calls across Framework/binding/native libraries, excluding atomics and implicit condition-wait reacquisition. They do **not** measure lock-wait duration. Signals/broadcasts are wake requests; wait returns are wake observations, **not kernel scheduling switches or one-to-one signal matches**. Whole-phase counts include management/control work. Worker wake propagation can exceed one per completion.

### Small counting/contract harness

`harness.cpp` uses the current real C1 registry/dispatcher with 64 empty-payload operations, excluding startup; this scope is smaller than a network request. `harness-direct.log`:

| Scope | Caller | Receive resource | Completion workers |
|---|---|---|---|
| Register 64 | 196 heap calls (**3.0625/request**, including map growth); 128 mutex acquisitions (**2/request**) | — | — |
| Complete 64 + final drain | 1 shutdown mutex acquisition total | **64 aligned allocations, 64 posts, 64 signals, 192 mutex acquisitions**: **1 / 1 / 1 / 3 per request** | **0 allocations, 64 executions, 64 mutex acquisitions**, 15 signals, 16 wait returns |

Calibration reports exactly **1 malloc / 1 aligned allocation / 1 mutex acquisition / 1 signal**. `check-counts.py` accepts the real capture and rejects a copy missing one post. Burst wake counts depend on scheduling and do not replace the benchmark counts.

C1 checks pass: 64 deliveries once and 64 rejected duplicate terminals; one terminal each for timeout/cancel/shutdown; one delivery for preparation and callback exceptions; unrelated callback finishes while the first blocks. These are focused baseline checks, not candidate/socket-level handoff validation.

## 3. Design alternatives for supervisor selection

Let `h` be successful handoffs with one locally executed independent completion divided by requests. Current data gives only **0 <= h <= 195683/241991 = 0.8086**; empty rotations and unavailable workers make the real upper bound lower. Mean completions/all rotations is **1.2366**. The capture does not identify how many currently idle workers could be reserved; a zero-attempt baseline cannot measure future success probability.

### A — exclusive successor assignment, then one local completion turn (recommended candidate)

Keep placement in the **existing offload executor**. Once per completed rotation, attempt without waiting to reserve a particular currently idle, already-created worker exclusively for the next receive rotation. Failure keeps reception and dispatches independently as today. Replace queued-role/idle-count membership with worker-owned idle/assignment membership; no second role queue, placement broker, new thread, raised limit or start-confirmation wait.

Hold the existing reserved completion items through the rotation. After poll/drain, receive turn, completion-publication stack and lane scopes have returned, publish siblings independently and execute **at most one** item locally with the existing dispatcher identity. Never serially drain callbacks on the former receiver. Role-owned socket/poller/cursor/permits/management state transfers as one role. The same reserved item uses one replacement placement algorithm and its mandatory fallback; no old/new pipeline option.

**Estimated completion-path counts:** posts and publication allocations **1 → 1−h/request**, if sibling Asio publication is unchanged; add **h successor notifications/request**, and at most **0.8086 non-waiting assignment attempts/request** in this capture. Small-harness receive mutex acquisitions: **3 → 1 + 2(1−h)**, plus successful rotation-level assignment acquisition. Assignment/internal settlement must add **no blocking or lock wait**. Worker queue takes fall by h; the former receiver still executes the callback's task work. Net wake savings are unproven: completion wake may merely become receive wake. These are estimates, not measured whole-request savings.

Files under `src/runtime/`: `dispatch/offload_executor.{hpp,cpp}`, `mesh/mesh_node_host_service.cpp`, `dispatch/completion_turn.hpp`, `dispatch/provider_task_context.cpp`, `foundation/operation_registry.cpp`; pool/registry/real-poll tests. No binding/Core/public API change.

Risks: S01:508 premature same-stack execution; S02 §2–3 bypassing captured gate/Yield scheduling; §13 entering application before relinquishing reception; shutdown losing an assigned role or unpublished reservation. Retain C1's sole terminal slot, captured application context, per-item exception isolation and accepted-work drain through callback-capture destruction, including local execution. Do not flush callbacks from a poll/rotate destructor.

### B — retain reception; replace publication representation with reserved independent operations

Retain reception. Turn the reserved C1 item into the dispatcher operation and publish independent items in bulk at the poll boundary. This is `cpoll`'s representation: its report found no throughput improvement. Public callbacks/coroutines stay on workers. Do not move locking/callback-running `task_shared_state_t::complete` onto reception; only existing internal result classification/settlement may remain there.

**Estimated completion-path counts:** publication allocations **1 → 0/request**; independent executions stay **1/request**; producer posts/signals approximately **1/b** for b completions per publication batch. Worker queue takes, wake propagation and task work remain. Small-harness receive mutex budget approximately **1 + 1/b/request** when accepted reservation lifetime replaces the stopped check. This retains existing dispatcher synchronization and introduces no internal-settlement wait. It does **not** prove no contention in the existing fallback; literal zero lock-wait on all receive work is not established by either baseline counts or this representation.

Files: `dispatch/completion_turn.hpp`, `dispatch/provider_task_context.cpp`, `foundation/operation_registry.cpp`, `backend/raw_route_port.cpp`, allocation/lifetime/poll tests. Owners and worker policy stay unchanged. S01 §11 risk: accepted work disappearing before deferred publication during close; account before deferral. S02 §1–3/§13 remain unchanged. A single posted callback-draining lambda is invalid.

Estimated per-request budgets using the measured whole-source path as a fixed-work reference (logical roles, not permanent thread labels):

| Alternative | Caller heap / mutex | Receive heap / completion posts / signal requests / mutex | Completion-work heap / mutex |
|---|---|---|---|
| A | 17.911 / 36.550 | 9.174−h / 1−h / approximately 1.001 including successor notices / at least 44.303−2h+a | 7.002 / at least 6.002−h |
| B | 17.911 / 36.550 | 8.174 / approximately 1/b / approximately 1/b plus unrelated notices / approximately 42.303+1/b | 7.002 / approximately 6.002 |

Here a is acquired placement locks/request (at most 0.8086 attempts/request; failed try-locks acquire none). A's lower bounds exclude pool return/reassignment bookkeeping and do not promise those totals; actual counts can be higher. Callback work on a former receive thread belongs to completion work after handoff. B's worker wake propagation remains additional and scheduler-dependent. Changed producer timing can also change binding/management counts, so these arithmetic budgets require validation in the later implementation.

**Recommend A for the next design review**, with independent reserved-item fallback. Require counters for successor wakes, assignment successes and residual publications before accepting a performance claim. B is smaller but has no evidence of reaching 7.5 µs/message. Neither has measured time savings here; expected count reductions do not establish recovery of the roughly 14.7 µs/message gap. The PM no-wait condition must be verified at the internal settlement/assignment boundary, not inferred from the existence of an idle waiter or from these lock-call counts.

## 4. Contract acceptance conditions for either replacement

Every row of S08 §4.4 remains required:

| Preserved contract | Required check |
|---|---|
| Independent receive/completion/management progress | Block the chosen callback; successor receives a real second reply and management advances before release. Without successful assignment, assert zero user callback/coroutine entries on reception. Added internal settlement and assignment must not block or wait for a lock/application queue. Existing path mutex counts above do not prove this property. |
| Queue/gate/claim/permit ordering and normal application entry | Exercise ordinary awaited continuation and Yield; preserve held gate or reacquire via its captured queue scheduler. A resource assignment never grants execution permission. |
| Independent callbacks and runtime worker policy | A blocked callback must not delay unrelated callbacks; preserve existing worker counts/policies and application-selected contexts. Publish siblings independently before any local callback. |
| Once per receive rotation | Count attempts at the rotation boundary, never per item; preserve continuous claim and management budget. |
| No added resources, start confirmation or concurrent receiver | Assert unchanged thread limits, no worker creation for handoff, no start acknowledgement wait, and maximum one simultaneous socket poll/drain. |

For either replacement, retain the C1 slot and reservation lifetime. Race reply against timeout/cancel/shutdown: **one delivery and one winning terminal**. Inject preparation/callback exceptions: **no accepted operation gets zero deliveries**, and siblings still progress. Inject acceptance/publication/assignment failure and close while an item is unpublished/assigned: retain the same item; no catch-and-drop path. Existing focused checks are the baseline, not future implementation proof.

Cross-language source check: .NET `framework/languages/dotnet/src/Zlink.Framework/Runtime/Backend/DotNet/ZLinkCompletionDispatcher.cs:13–17,27–43` uses independent pool turns; Java/Kotlin share `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceCompletionDispatcher.java:51–81,101–125`, independently submitting items; Node `framework/languages/node/packages/framework/src/runtime/backend/node/node-raw-mesh-backend.ts:1871–1884` uses microtasks and has no successor on its single event loop. Public terminal/gate contracts stay identical; no cross-language behavior/spec change is proposed.

Classification: **A — existing-contract adaptation**, pending supervisor selection; absent optional handoff is not a resource-assignment bug. Decision sites before/after: **placement 1→1, C1 terminal 1→1, completion turn policy 1→1, dispatcher scheduler 1→1**. A replaces placement membership; B replaces publication storage. Neither owner was changed in this job.

Validation: isolated build, counter calibration, positive/negative count checks and focused C1 harness passed. No remaining test failure. No full suite or candidate/socket handoff tests: runtime design was not implemented. Baseline counts are measured; proposed savings are estimates; no after-performance result exists.

리팩토링 점검: 성능 2건, POSDDD 0건, 불필요 코드 0건 발견. 고친 항목: 없음. 넘긴 항목: 완료 게시 할당, 완료 게시·깨우기와 수신 자원 배치 비용.
