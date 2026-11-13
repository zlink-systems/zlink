# C++ request receiver — 0.33.0 #1639 / #3867

Source: `/root/worktree/zlink-3867r`, branch `perf/3867-cpp-request-receiver`, baseline `c8869b63b19c8504a727459c766bf3ccee995708`. This continuation includes the five inherited uncommitted files. It writes only C++ implementation/tests and `/root/jobs/fx/cx/v21/s033/crecv2/`. No commit, branch change, synchronization, GitHub operation, spec edit, sub-agent, or Core C header inspection was performed.

## Open question: two terminal deliveries were possible

**Confirmed defect.** The inherited implementation sent an owning receiver to `compose_node_request_submission` while putting a borrowed receiver in the registry callback. Those paths could both invoke the same single-use projection. Against the inherited benchmark library, the real-mesh regression detects **two conversions instead of one** for deadline expiry before native start and pending shutdown. Immediate rejection before registration and a normal reply are positive controls. See `repro-before-four-cases.log`, `repro-before-deadline.log`, `repro-before.py`, and `repro-commands.json`. This is a reproduction of the inherited incomplete patch, not a claim that c8869b63b1 already had released optional captures.

All source references below are relative to `framework/languages/cpp/framework/`, in the final worktree unless explicitly identified as the inherited version.

| Case | Original delivery/order and second path | Final delivery and what the other path touches |
|---|---|---|
| Immediate rejection **before C1 registration** | Missing target/port returns a failed submission without a pending entry (`src/runtime/mesh/raw_mesh_node_owner.cpp:1523`, `:1528`, `:1579`); invalid timeout and encode/registration exceptions also return a failed task. Inherited `compose_node_request_submission` (`mesh_node_runtime.cpp:4338`, immediate branch `:4346`) completed admission and the receiver. No registry terminal exists. | Ownership remains in `completion`; the submission observer (`mesh_node_runtime.cpp:4442–4454`) publishes admission, then invokes that retained callback once with the original failure. It releases the unused decoder captures through the same callback. No C1 entry can call it. |
| Immediate **native** submit rejection after registration | `raw_route_port.cpp:360–405` and `raw_binding_adapter.hpp:170–196` publish admission failure before reporting physical failure to `observe_request`. That reports to C1 (`raw_mesh_node_owner.cpp:1375–1401`, `:252–277`). Inherited compose's ready-failure branch can therefore deliver first; the registry callback is also eligible. A different dispatcher worker may deliver the already-posted registry terminal before submission returns, so the spec does not impose a universal physical first path. Both can touch the same projector. | C1 consumes the retained callback at successful publication (`operation_registry.cpp:434–438`) before native start. Submission can complete admission/public failure, but has no delivery callback. C1's callback performs physical conversion once in its dispatcher turn. |
| Deadline before native start | The readiness loop expires through C1 (`raw_mesh_node_owner.cpp:321–328`); an external deadline visit takes the same completion (`operation_registry.cpp:698–747`). Its callback first publishes admission failure (`operation_registry.cpp:23–35`), waking the pending submission observer, then calls the receiver. Inherited pending compose uses `observe_task_completion` (`mesh_node_runtime.cpp:4376`), so its failure delivery and the registry conversion can race on separate dispatcher turns. Neither is guaranteed to convert first. Both conversions were reproduced. | C1 takes and delivers once. The submission observer can run before or after conversion, but only touches the admission source. The readiness wait observes that same admission failure and exits. With no physical receiver supplied for Node requests, it cannot deliver another projection (`raw_mesh_node_owner.cpp:354–371`). |
| Normal reply | Compose handles successful admission (`mesh_node_runtime.cpp:4369–4370`) without delivering a reply. `observe_request` completes C1 through `deliver_request_terminal`; C1's callback alone converts the reply. | Same single C1 delivery. Later timeout, cancellation, shutdown, or native failure cannot take an empty completion slot (`operation_registry.cpp:124–145`, `:513–525`). They do not touch the receiver. |
| Shutdown with a request pending | C1 shutdown takes every pending completion (`operation_registry.cpp:765–799`), then callback admission publication can awaken inherited compose exactly as in the deadline case. If admission was still pending, two projection invocations can race; both were reproduced after dropping the public handle. If admission had already succeeded, compose does not deliver and only the registry callback does. | C1 alone delivers in both cases. Admission observation has no complete receiver after registration. Registry shutdown waits until accepted callbacks and their captures are destroyed. A late raw result only attempts C1 take and releases its payload; this Node path supplies no physical receiver to `deliver_request_terminal`. |

The owning callback returned by `request_callback_t::capture` aliases the request block. The registry item holds that callback through delivery (`operation_registry.cpp:188–216`), so dropping the compose/public owner does **not**, by itself, free the request block. The unsafe object was the **released callable inside that block**: `request_task.hpp:53–61` resets each stored capture after invocation, and `:602–615` resets the final projector. A second borrowed invocation can dereference an empty optional even though the memory block remains allocated. Adding a completed flag or checking the optional would create a second terminal authority; neither is used.

The fix transfers existing callback ownership **at C1 publication**, before deadline reservation or cancellation can throw or deliver. Host/raw/registry parameters are rvalue references; `with_admission` returns the same rvalue reference. C1 reserves its completion item without consuming the callback, preserves the caller's callback on rejected/rolled-back publication, and moves it into the item only at successful publication (`operation_registry.cpp:434–438`). A post-publication deadline-reservation exception is therefore distinguishable by consumed callback ownership even though registration did not return. The raw owner catches that exception and calls C1 `fail`; it never delivers independently. The mesh submission observer holds admission plus the possibly unconsumed callback, never a second owning receiver. No registration flag, terminal marker, queue, lock, or counter authority was added.

The exception case was reproduced separately: an injected `std::bad_alloc` from the existing deadline-change callback leaves one published C1 entry and an undelivered callback in the earlier raw copy/return sequence; fallback plus shutdown delivers **twice** (`registration-fault-before.log`, exit 1; reproduction source and exact build commands are adjacent). Final tests require rejection to retain the undelivered callback, but a post-publication exception to leave only C1 delivery ownership.

Two alternatives were considered: make repeated invocation harmless with another completion marker, or transfer delivery ownership at existing C1 registration. The latter removes an independent delivery decision and retains the existing terminal authority. The user's continuation explicitly authorizes this correction when either competing path is possible.

### Both channel projections

`src/runtime/channels/channel_runtime.cpp:1895–1915` stores synchronous decoder projections in the request block. The mesh arm receives exactly the selected mesh receiver's physical terminal. The route arm uses `route_channel_runtime.cpp:417–483`: a submission exception invokes `failed` before a physical observer is installed; successful installation observes the backend's physical task once, and the successful submission observer returns without delivery. `request_task.hpp:667–676` bridges admission separately from physical completion. An early admission failure only completes the public result, never a decoder capture; a later physical result must still decode and release payloads.

For immediate submission failure, only the failed-submission path converts; for normal reply, backend completion converts; for early admission/deadline/shutdown failure followed by a physical terminal, admission completes the public result and the physical observer converts once. Repeated backend task completion is rejected by its existing `task_shared_state_t::complete` (`task.hpp:622–660`) before notifying observers. The mesh arm now benefits from the C1 ownership correction above. There is no additional channel terminal selector.

A separate lifetime defect existed in both inherited projections: they passed `context->project.target` into the submit call, while an inline or concurrent reply could destroy that decoder before submit returned. Keeping only a submit-local copy was insufficient: the existing transport/native-count contract tests require the borrowed target to survive pending admission and early public failure until physical decode. The final implementation stores the **one target value in request capture storage**, retains an alias on the submit stack, and gives the stored decoder a borrowed alias. Ordinary standalone callbacks use the existing separate-allocation fallback. The target therefore survives both submit and physical conversion without a cycle or an extra allocation on prepared requests. Existing target-lifetime assertions are unchanged; the allocation test's original pointer/co-location assertions were restored. An added inline-completion test reads the target before and after decoder release. The decoder and other heavy captures are still retired at conversion; the inline routing ID remains part of the result block until its final owner releases it.

## Decision ownership and rule counts

S01 = `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md`; BP = `bindings/doc/spec/async-coroutine-policy.ko.md`.

| Decision | Spec owner | Before locations | Final owner/locations | Before → after |
|---|---|---|---|---|
| Registered mesh reply delivery | S01:445–488, Framework C1 | Registry callback plus admission-failure compose (`mesh_node_runtime.cpp:4338–4376`, `:4399`) | Registry callback only; submission callback is consumed on registration (`operation_registry.cpp:434–438`, `mesh_node_runtime.cpp:4442`) | **2 → 1** |
| Rejection before registration | S01:61–77, source submission | Submission failure forwarding | Same submission outcome, delivered through its unconsumed callback | 1 → 1 |
| Logical terminal winner: reply/timeout/cancellation/shutdown | S01:445–488 | `operation_registry.cpp:124–145`, C1 take | Same C1 completion slot | 1 → 1 |
| Registration, deadline indexing and close | S01:453–488 | Raw owner turn and C2 index | Same owner and index | 1 → 1 |
| Independent admission / first public result | S01:61–77,331–340 | Binding admission and `task_shared_state_t::complete` | Same authorities; co-allocated request stages and admission publication hook | 1 → 1 each |
| Physical native operation, retry and completion drain | BP:160,189 | Public binding `.async()`, existing poller/await producer | Unchanged binding/Core owner | 1 → 1 |
| Per-layer codec conversion | S01:331–340 and message contract | Public typed codec, channel envelope decoder, mesh service-wire decoder | Same codecs, one capture per layer in request storage | 1 → 1 each |
| Application delivery turn and shutdown drain | S01:490–544 | Existing completion dispatcher and wait-registration owner | Unchanged; no projection-specific Asio post | 1 → 1 |
| Borrowed Node target lifetime | S01:331–340, Framework physical producer | `channel_runtime.cpp:1843,1893–1913`, projection context | Request-owned value at `request_task.hpp:161`, retained/borrowed aliases at `channel_runtime.cpp:1843–1849` | 1 → 1 |
| Callable/stage allocation representation | C++ private representation | Separate state and callable ownership blocks | One request block with synchronous projections | 0 → 0 |

**Rule count:** registered delivery decision sites **2 → 1**; all other decision-location counts stay unchanged. The per-request capture resource is storage, not a cache, registry, execution queue, or terminal flag. Its upstream allocation preserves support for large custom captures. It replaces independent shared callable allocations and the forwarding admission observer; unused receiver/preparation overloads and the now-unused capture-address output were removed.

Ownership: Framework owns logical terminal selection, typed conversion, admission/public projection and callback placement; binding/Core owns native admission and physical progress. Spec: S01 §§2,9–11; BP:160,189. Cross-language comparison: .NET `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:5823–5832` takes through `_operations.TryRemove`, with registration before native submit at `:11039`; Java `ZLinkServiceOperationRegistry.java:343–369` registers/submits on its owner and `:508–516` removes conditionally; Kotlin uses Java runtime; Node `operation-registry.ts:434–441` takes/deletes once. The correction restores this shared single-terminal behavior without a language contract difference. Classification: **A** for allocation/receiver representation, **B** for the inherited competing delivery and submit-argument lifetime defects. No C/D change or spec edit is required.

## Changed files

All paths below are under `framework/languages/cpp/`.

- `framework/include/zlink/framework/contracts/dispatch/request_task.hpp`: co-allocate admission, public/physical stages and final projector; retain borrowed submit values and synchronous codec captures in the same block, releasing heavy captures at conversion; remove unused preparation/receiver overloads and capture-address output.
- `framework/include/zlink/framework/contracts/dispatch/task.hpp`: allow embedded stages to alias request ownership; publish admission's early public failure without a forwarding allocation; reuse the selected task owner for observers.
- `framework/src/runtime/channels/channel_runtime.cpp`: compose directly into the stored receiver; retain the target in the request block independently of terminal decoder destruction.
- `framework/src/runtime/mesh/mesh_node_runtime.cpp`: share one receiver block with the C1 callback and make admission observation incapable of delivering an already-registered receiver.
- `framework/src/runtime/mesh/raw_mesh_node_owner.cpp` and `.hpp`: carry the callback reference to C1; route post-publication and native-start exceptions through C1.
- `framework/src/runtime/stateful/public_host_runtime.cpp` and `.hpp`: pass callback ownership through to the actual synchronous registration boundary.
- `framework/src/runtime/foundation/operation_registry.cpp` and `.hpp`: consume callbacks exactly at accepted C1 publication, preserve them on rejection, and keep the existing terminal take as the sole authority.
- `tests/Zlink.Framework.UnitTests/test_cpp_framework_request_call_allocations.cpp`: one-block allocation/co-location, several observers, independent early/physical completion, const-capture release, pending shutdown, inline target lifetime, and direct route projection coverage.
- `tests/Zlink.Framework.UnitTests/test_cpp_framework_request_transport_projection.cpp`: real mesh pre-registration rejection, native rejection, pre-native deadline, normal reply, and pending-shutdown regression tests; count conversions and drop the public handle before shutdown.

## Validation

- **Full all-target Release build passed**, all 323 remaining steps after the final ownership corrections, including samples and cross-language targets (`build-all.log`).
- **Unit/contract CTest: 297/297 passed**, 231.48 seconds (`test-unit-contract.log`).
- **C++ format check passed**: `scripts/format/format.sh --check cpp` (`format-final.log`).
- **Focused allocation/receiver suite: 42/42 passed** (`test-focused-final.log`); all four focused CTest targets passed (`test-focused-four.log`). The two existing target-lifetime suites pass with their original assertions (`test-target-lifetimes.log`).
- **Negative terminal controls:** the inherited implementation passes pre-registration rejection and normal reply, but fails native rejection, pre-native deadline and pending shutdown solely because conversions are **2 instead of 1** (`repro-before-final.log`, exit 1). The final implementation passes all five.
- **Negative publication-exception control:** the earlier copy/return sequence leaves `pending=1`, `retained_callback=1` and delivers twice after the existing deadline-change hook throws (`registration-fault-before.log`, exit 1). Final tests require rejection to preserve the callback and successful C1 publication to consume it before a later exception.
- **Bidirectional allocation controls:** baseline headers produce **7 ownership allocations**, reject the one-allocation budget and return value 41 (`../crecv/allocation-positive-negative.json`, `../crecv/receiver-allocation-probe.cpp`). Final code produces **1 ownership allocation** for the same two-projection operation and returns 41. Its distinct admission/public/physical stages and the retained target are in the same allocation. The additional-state guard rejects an injected fourth conversion state.

Coverage includes several independent observers, early public failure followed by physical conversion and payload release, const-capture release while task handles remain, 16 pending shutdown deliveries on independent turns, five real mesh outcomes, registration rejection/publication failure, target use after inline completion, and early public failure on the direct route projection. The Release gate does not execute the existing AddressSanitizer-only resolver-destruction branch; no new skip or relaxed assertion was introduced.

Exact final build and test commands from `framework/languages/cpp`:

```sh
flock -s /root/jobs/fx/cx/measure.lock /root/jobs/fx/cx/cpp-build-slot.sh \
  cmake --build --preset linux-ninja-release --parallel 3 -- -k 0
flock -s /root/jobs/fx/cx/measure.lock \
  ctest --preset linux-ninja-release -L '^(unit|contract)$'
```

The installed build-slot wrapper takes the command as its first argument, so `--parallel 3` is placed on `cmake --build`. The inherited literal placement before the command failed with exit 127 (`../crecv/build-requested.log`); the supported form preserves the same lock, build slot, CPU affinity, parallelism and `-k 0`. Initial failed target-lifetime assertions are preserved as negative evidence in `test-unit-contract-initial.log`; the final gate above has no remaining failure.

## Measurement conditions and integrity

Runners are copied from the inherited crecv preparation with output/build-root substitutions; workload and CPU collectors match the before cohort (`runner-equivalence.json`). The only csubmit helper difference is the required SIGTERM cleanup fallback replacing SIGKILL. csubmit uses **condition B**, request-backpressure/send-saturation, **1 KiB**, **three runs**, **30 s warmup / 5 s active**. Cladder rung 9 and unlimited Framework use **30 s warmup / 20 s active**, three runs. Source CPUs are 0–7 and target CPUs 8–15. Each campaign holds `flock -o /root/jobs/fx/cx/measure.lock`; campaigns run sequentially. Collectors enforce MemAvailable ≥12 GiB at capture points. `measure.sh`, `measure.log` and `verify.sh` preserve the commands and outcomes.

**All 19 after cells passed integrity checks**: 18 unprofiled cells plus one allocation census; completed count equals target received, with zero errors, abandoned requests, header-validation failures and target rejections, and no contamination or drain-bound flag. The same count validator accepts a real cell and rejects a copy whose target received count is increased by one (`quality.json`). The 12 changed source-file hashes and HEAD match the final tested state (`source-state.json`, `final-state.json`); measured binaries and deployed 1.23.0 binding/Core hashes are recorded in `artifact-hashes.json`. No Core or binding package was changed by this job.

## Final before/after measurements

Median of three runs; throughput in messages/s, CPU in µs/completed message. csubmit uses condition B and paired binding medians. Ladder denominators are its unchanged before references (rung 0: 223,019.24; unlimited binding: 246,721.55 messages/s), so ladder F/B is not a newly paired binding measurement. Full per-run values, ranges, and actual thread IDs are in `measurements.json` and `threads.csv`.

| Cell | Before msg/s | After msg/s | Change | Before F/B | After F/B | Caller CPU before → after | Source CPU before → after | Target CPU before → after |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| cladder rung 9 | 45,050.7 | 46,180.3 | +2.5% | 0.202 | 0.207 | 20.148 → 19.484 | 91.766 → 88.027 | 72.469 → 71.267 |
| cladder unlimited Framework | 49,558.2 | 50,625.2 | +2.2% | 0.201 | 0.205 | 18.906 → 18.029 | 86.043 → 83.073 | 65.632 → 63.973 |
| csubmit request, binding | 246,826.1 | 250,627.3 | +1.5% | — | — | 4.069 → 4.057 | 9.693 → 9.735 | 10.027 → 9.802 |
| csubmit request, Framework | 49,602.0 | 51,940.2 | +4.7% | 0.201 | 0.207 | 18.326 → 17.585 | 84.121 → 81.215 | 62.637 → 59.341 |
| csubmit send, binding | 616,974.0 | 612,012.0 | -0.8% | — | — | 1.448 → 1.416 | 3.061 → 3.024 | 3.437 → 3.360 |
| csubmit send, Framework | 173,204.0 | 175,907.9 | +1.6% | 0.281 | 0.287 | 4.884 → 4.817 | 10.249 → 10.067 | 15.352 → 14.625 |

Raw paths (each directory contains `results.json`, source/target logs, and `capture/` snapshots):

| Cell | Before | After |
|---|---|---|
| cladder rung 9 | `/root/jobs/fx/cx/v21/s033/cladder/raw/rung9/run1/zlink-framework-cpp-request-backpressure-1024`<br>`/root/jobs/fx/cx/v21/s033/cladder/raw/rung9/run2/zlink-framework-cpp-request-backpressure-1024`<br>`/root/jobs/fx/cx/v21/s033/cladder/raw/rung9/run3/zlink-framework-cpp-request-backpressure-1024` | `/root/jobs/fx/cx/v21/s033/crecv2/ladder/raw/rung9/run1/zlink-framework-cpp-request-backpressure-1024`<br>`/root/jobs/fx/cx/v21/s033/crecv2/ladder/raw/rung9/run2/zlink-framework-cpp-request-backpressure-1024`<br>`/root/jobs/fx/cx/v21/s033/crecv2/ladder/raw/rung9/run3/zlink-framework-cpp-request-backpressure-1024` |
| cladder unlimited Framework | `/root/jobs/fx/cx/v21/s033/cladder/raw/rung9-unbounded/run1/zlink-framework-cpp-request-backpressure-1024`<br>`/root/jobs/fx/cx/v21/s033/cladder/raw/rung9-unbounded/run2/zlink-framework-cpp-request-backpressure-1024`<br>`/root/jobs/fx/cx/v21/s033/cladder/raw/rung9-unbounded/run3/zlink-framework-cpp-request-backpressure-1024` | `/root/jobs/fx/cx/v21/s033/crecv2/ladder/raw/rung9-unbounded/run1/zlink-framework-cpp-request-backpressure-1024`<br>`/root/jobs/fx/cx/v21/s033/crecv2/ladder/raw/rung9-unbounded/run2/zlink-framework-cpp-request-backpressure-1024`<br>`/root/jobs/fx/cx/v21/s033/crecv2/ladder/raw/rung9-unbounded/run3/zlink-framework-cpp-request-backpressure-1024` |
| csubmit request, binding | `/root/jobs/fx/cx/v21/s033/csubmit/measure/1/request-backpressure/branch/run1/zlink-cpp-request-backpressure-1024`<br>`/root/jobs/fx/cx/v21/s033/csubmit/measure/2/request-backpressure/branch/run1/zlink-cpp-request-backpressure-1024`<br>`/root/jobs/fx/cx/v21/s033/csubmit/measure/3/request-backpressure/branch/run1/zlink-cpp-request-backpressure-1024` | `/root/jobs/fx/cx/v21/s033/crecv2/measure/run1/zlink-cpp-request-backpressure-1024`<br>`/root/jobs/fx/cx/v21/s033/crecv2/measure/run2/zlink-cpp-request-backpressure-1024`<br>`/root/jobs/fx/cx/v21/s033/crecv2/measure/run3/zlink-cpp-request-backpressure-1024` |
| csubmit request, Framework | `/root/jobs/fx/cx/v21/s033/csubmit/measure/1/request-backpressure/branch/run1/zlink-framework-cpp-request-backpressure-1024`<br>`/root/jobs/fx/cx/v21/s033/csubmit/measure/2/request-backpressure/branch/run1/zlink-framework-cpp-request-backpressure-1024`<br>`/root/jobs/fx/cx/v21/s033/csubmit/measure/3/request-backpressure/branch/run1/zlink-framework-cpp-request-backpressure-1024` | `/root/jobs/fx/cx/v21/s033/crecv2/measure/run1/zlink-framework-cpp-request-backpressure-1024`<br>`/root/jobs/fx/cx/v21/s033/crecv2/measure/run2/zlink-framework-cpp-request-backpressure-1024`<br>`/root/jobs/fx/cx/v21/s033/crecv2/measure/run3/zlink-framework-cpp-request-backpressure-1024` |
| csubmit send, binding | `/root/jobs/fx/cx/v21/s033/csubmit/measure/1/send-saturation/branch/run1/zlink-cpp-send-saturation-1024`<br>`/root/jobs/fx/cx/v21/s033/csubmit/measure/2/send-saturation/branch/run1/zlink-cpp-send-saturation-1024`<br>`/root/jobs/fx/cx/v21/s033/csubmit/measure/3/send-saturation/branch/run1/zlink-cpp-send-saturation-1024` | `/root/jobs/fx/cx/v21/s033/crecv2/measure/run1/zlink-cpp-send-saturation-1024`<br>`/root/jobs/fx/cx/v21/s033/crecv2/measure/run2/zlink-cpp-send-saturation-1024`<br>`/root/jobs/fx/cx/v21/s033/crecv2/measure/run3/zlink-cpp-send-saturation-1024` |
| csubmit send, Framework | `/root/jobs/fx/cx/v21/s033/csubmit/measure/1/send-saturation/branch/run1/zlink-framework-cpp-send-saturation-1024`<br>`/root/jobs/fx/cx/v21/s033/csubmit/measure/2/send-saturation/branch/run1/zlink-framework-cpp-send-saturation-1024`<br>`/root/jobs/fx/cx/v21/s033/csubmit/measure/3/send-saturation/branch/run1/zlink-framework-cpp-send-saturation-1024` | `/root/jobs/fx/cx/v21/s033/crecv2/measure/run1/zlink-framework-cpp-send-saturation-1024`<br>`/root/jobs/fx/cx/v21/s033/crecv2/measure/run2/zlink-framework-cpp-send-saturation-1024`<br>`/root/jobs/fx/cx/v21/s033/crecv2/measure/run3/zlink-framework-cpp-send-saturation-1024` |

### Per-thread CPU

Names below are the recorded thread role or Linux comm plus descending CPU rank within that role/comm in each run, then the three-run median. They are not stable thread IDs across processes. Every thread with at least 0.5 µs/message in either cohort is listed; all smaller threads and each actual TID remain in `threads.csv` and `measurements.json`. Unnamed `bench_cpp_*` workers are intentionally not assigned an inferred execution role.

| Cell | Endpoint | Recorded thread/rank | Before µs/msg | After µs/msg |
|---|---|---|---:|---:|
| cladder rung 9 | source | `caller #1` | 20.148 | 19.484 |
| cladder rung 9 | source | `bench_cpp_clien #1` | 18.393 | 17.772 |
| cladder rung 9 | source | `ZLINKbg/IO/0 #1` | 18.233 | 17.783 |
| cladder rung 9 | source | `ZLINKbg/IO/2 #1` | 16.936 | 16.360 |
| cladder rung 9 | source | `bench_cpp_clien #2` | 2.203 | 2.113 |
| cladder rung 9 | source | `bench_cpp_clien #3` | 2.201 | 2.108 |
| cladder rung 9 | source | `bench_cpp_clien #4` | 2.200 | 2.103 |
| cladder rung 9 | source | `bench_cpp_clien #5` | 2.200 | 2.103 |
| cladder rung 9 | source | `bench_cpp_clien #6` | 2.198 | 2.101 |
| cladder rung 9 | source | `bench_cpp_clien #7` | 2.197 | 2.098 |
| cladder rung 9 | source | `bench_cpp_clien #8` | 2.190 | 2.094 |
| cladder rung 9 | source | `bench_cpp_clien #9` | 2.189 | 2.089 |
| cladder rung 9 | target | `ZLINKbg/IO/1 #1` | 17.874 | 17.241 |
| cladder rung 9 | target | `receive #1` | 15.891 | 15.693 |
| cladder rung 9 | target | `ZLINKbg/IO/0 #1` | 13.670 | 13.934 |
| cladder rung 9 | target | `app-worker #1` | 8.130 | 10.273 |
| cladder rung 9 | target | `app-worker #2` | 5.469 | 7.536 |
| cladder rung 9 | target | `app-worker #3` | 5.152 | 2.075 |
| cladder rung 9 | target | `app-worker #4` | 1.381 | 0.000 |
| cladder rung 9 | target | `bench_cpp_frame #1` | 0.548 | 0.552 |
| cladder rung 9 | target | `bench_cpp_frame #2` | 0.526 | 0.533 |
| cladder rung 9 | target | `bench_cpp_frame #3` | 0.516 | 0.507 |
| cladder rung 9 | target | `bench_cpp_frame #4` | 0.508 | 0.505 |
| cladder unlimited Framework | source | `caller #1` | 18.906 | 18.029 |
| cladder unlimited Framework | source | `ZLINKbg/IO/0 #1` | 16.471 | 15.687 |
| cladder unlimited Framework | source | `bench_cpp_clien #1` | 16.012 | 16.240 |
| cladder unlimited Framework | source | `ZLINKbg/IO/2 #1` | 15.248 | 15.338 |
| cladder unlimited Framework | source | `bench_cpp_clien #2` | 2.380 | 2.183 |
| cladder unlimited Framework | source | `bench_cpp_clien #3` | 2.367 | 2.166 |
| cladder unlimited Framework | source | `bench_cpp_clien #4` | 2.367 | 2.164 |
| cladder unlimited Framework | source | `bench_cpp_clien #5` | 2.366 | 2.163 |
| cladder unlimited Framework | source | `bench_cpp_clien #6` | 2.360 | 2.160 |
| cladder unlimited Framework | source | `bench_cpp_clien #7` | 2.358 | 2.160 |
| cladder unlimited Framework | source | `bench_cpp_clien #8` | 2.355 | 2.152 |
| cladder unlimited Framework | source | `bench_cpp_clien #9` | 2.352 | 2.145 |
| cladder unlimited Framework | target | `ZLINKbg/IO/1 #1` | 16.029 | 16.143 |
| cladder unlimited Framework | target | `receive #1` | 14.019 | 13.127 |
| cladder unlimited Framework | target | `ZLINKbg/IO/0 #1` | 12.619 | 12.088 |
| cladder unlimited Framework | target | `app-worker #1` | 6.520 | 6.430 |
| cladder unlimited Framework | target | `app-worker #2` | 4.864 | 4.259 |
| cladder unlimited Framework | target | `app-worker #3` | 2.933 | 2.812 |
| cladder unlimited Framework | target | `app-worker #4` | 2.131 | 1.580 |
| cladder unlimited Framework | target | `app-worker #5` | 0.857 | 1.414 |
| cladder unlimited Framework | target | `app-worker #6` | 0.787 | 0.961 |
| csubmit request, binding | source | `caller #1` | 4.069 | 4.057 |
| csubmit request, binding | source | `ZLINKbg/IO/2 #1` | 2.948 | 2.877 |
| csubmit request, binding | source | `ZLINKbg/IO/0 #1` | 2.641 | 2.674 |
| csubmit request, binding | target | `ZLINKbg/IO/1 #1` | 3.517 | 3.428 |
| csubmit request, binding | target | `receive #1` | 3.408 | 3.300 |
| csubmit request, binding | target | `ZLINKbg/IO/0 #1` | 2.233 | 2.252 |
| csubmit request, Framework | source | `caller #1` | 18.326 | 17.585 |
| csubmit request, Framework | source | `ZLINKbg/IO/0 #1` | 15.957 | 15.363 |
| csubmit request, Framework | source | `bench_cpp_clien #1` | 15.723 | 15.536 |
| csubmit request, Framework | source | `ZLINKbg/IO/2 #1` | 14.884 | 14.834 |
| csubmit request, Framework | source | `bench_cpp_clien #2` | 2.377 | 2.187 |
| csubmit request, Framework | source | `bench_cpp_clien #3` | 2.374 | 2.181 |
| csubmit request, Framework | source | `bench_cpp_clien #4` | 2.366 | 2.179 |
| csubmit request, Framework | source | `bench_cpp_clien #5` | 2.366 | 2.172 |
| csubmit request, Framework | source | `bench_cpp_clien #6` | 2.366 | 2.168 |
| csubmit request, Framework | source | `bench_cpp_clien #7` | 2.366 | 2.164 |
| csubmit request, Framework | source | `bench_cpp_clien #8` | 2.356 | 2.158 |
| csubmit request, Framework | source | `bench_cpp_clien #9` | 2.351 | 2.153 |
| csubmit request, Framework | target | `ZLINKbg/IO/1 #1` | 16.237 | 15.245 |
| csubmit request, Framework | target | `receive #1` | 13.603 | 12.892 |
| csubmit request, Framework | target | `ZLINKbg/IO/0 #1` | 12.658 | 11.993 |
| csubmit request, Framework | target | `app-worker #1` | 9.660 | 9.151 |
| csubmit request, Framework | target | `app-worker #2` | 9.073 | 7.281 |
| csubmit request, Framework | target | `app-worker #3` | 0.000 | 1.944 |
| csubmit send, binding | source | `ZLINKbg/IO/0 #1` | 1.583 | 1.595 |
| csubmit send, binding | source | `caller #1` | 1.448 | 1.416 |
| csubmit send, binding | target | `ZLINKbg/IO/0 #1` | 1.473 | 1.438 |
| csubmit send, binding | target | `receive #1` | 1.341 | 1.331 |
| csubmit send, Framework | source | `caller #1` | 4.884 | 4.817 |
| csubmit send, Framework | source | `ZLINKbg/IO/0 #1` | 4.846 | 4.784 |
| csubmit send, Framework | target | `ZLINKbg/IO/0 #1` | 4.349 | 3.961 |
| csubmit send, Framework | target | `receive #1` | 4.227 | 4.195 |
| csubmit send, Framework | target | `app-worker #1` | 2.923 | 2.803 |
| csubmit send, Framework | target | `app-worker #2` | 2.777 | 2.732 |

### Allocations per request

The census uses the same allocation interposer and eight-second counter window as the csubmit before set. Heap calls count malloc/calloc/realloc; they include protocol/native and scheduling work outside the receiver block. The isolated ownership test counts C++ allocations for one prepared request with two projections. These are different scopes, so the one-block result is not a claim of one heap call for an entire network request.

| Scope | Before | After |
|---|---:|---:|
| Receiver ownership block test | 7 | 1 |
| Complete source caller heap calls/request | 23.254213 | 17.251161 |
| Entire source process heap calls/request | 40.098573 | 34.136117 |
| Entire target process heap calls/request | 41.605314 | 41.328927 |

Caller heap calls fall by **6.003/request (25.8%)**. Census raw paths: before `/root/jobs/fx/cx/v21/s033/csubmit/census/branch/run1/zlink-framework-cpp-request-backpressure-1024`; after `/root/jobs/fx/cx/v21/s033/crecv2/census/after/run1/zlink-framework-cpp-request-backpressure-1024`. Full totals and thread-level counters: `allocation-census.json`; raw allocation samples are under each census cell's `capture/` and campaign `alloc/` directories. Instrumented census throughput/CPU is excluded from the unprofiled rate comparison.

The endpoint gains are modest: csubmit request throughput +4.7%, send +1.6%; ladder bounded/unlimited +2.5%/+2.2%. Request caller CPU falls from 18.326 to 17.585 µs/message in csubmit, and from 20.148 to 19.484 in bounded ladder. These are three-run historical before/after medians, without an interleaved A/A significance study. Thread CPU uses sampled /proc schedstat windows; retiring-thread tails between samples have the same accounting limit as the before runner. Ladder F/B retains its original historical binding denominator, as stated above.

## Remaining issues and refactoring audit

No required build, unit/contract, format, measurement, or count check remains failing or incomplete. Final dry-run inspection has no pending compile/link work; the always-run layout manifest is the only remaining phony step. No job benchmark process remains (`final-state.json`). No spec/public API change, commit, push, synchronization or GitHub operation was performed. Cross-language executables were built; a separate four-language E2E or AddressSanitizer campaign was not requested or run.

Refactoring findings: **performance 2** (separate request-stage allocation and per-layer callable ownership), **POSDDD 2** (competing registered delivery including publication exceptions, and borrowed-target lifetime), **unnecessary code 1** (unused receiver/preparation overloads and capture-address output as one API group). All five were fixed; none deferred. Registered delivery decision sites **2 → 1**; all other decision-location counts unchanged. No new terminal flag, counter, lock, queue, cache, registry or binding callback terminal was introduced. The request-local capture resource replaces independently allocated owners, and target views reuse that same resource.
