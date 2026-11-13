from pathlib import Path
import json,re
j=Path(__file__).parent
root=Path('/root/worktree/zlink-3867f/framework/languages/cpp')
f=root/'framework'
paths={
 'req':'include/zlink/framework/contracts/dispatch/request_task.hpp',
 'task':'include/zlink/framework/contracts/dispatch/task.hpp',
 'call':'include/zlink/framework/contracts/channels/call.hpp',
 'channel':'src/runtime/channels/channel_runtime.cpp',
 'route':'src/runtime/channels/route_channel_runtime.cpp',
 'mesh':'src/runtime/mesh/mesh_node_runtime.cpp',
 'raw':'src/runtime/mesh/raw_mesh_node_owner.cpp',
 'host':'src/runtime/stateful/public_host_runtime.cpp',
 'actor':'src/runtime/actors/actor_client.cpp',
 'registry':'src/runtime/foundation/operation_registry.cpp',
 'app':'src/runtime/host/app.cpp'}
def ref(k,needle):
 p=paths[k];lines=(f/p).read_text().splitlines();n=next(i+1 for i,s in enumerate(lines) if needle in s)
 return f'`{Path(p).name}:{n}`'
def testref(name,needle):
 p=Path('tests/Zlink.Framework.UnitTests')/name;lines=(root/p).read_text().splitlines();n=next(i+1 for i,s in enumerate(lines) if needle in s)
 return f'`{p}:{n}`'
complete=(j/'stage').read_text().strip()=='complete' if (j/'stage').exists() else False
unit=(j/'test-unit-contract.log').read_text() if (j/'test-unit-contract.log').exists() else ''
m=re.search(r'100% tests passed, 0 tests failed out of (\d+)',unit)
unit_status=f"**{m[1]}/{m[1]} passed**" if m else 'Pending.'
full_status='**Passed**, all targets with `--parallel 3 -- -k 0` (527 remaining steps).' if m or complete or (j/'stage').read_text().strip() in ['unit-contract','format','measurements'] else 'In progress; see `build-all.log`.'
fmt='**Passed** (`format-final.log`).' if (j/'format-final.log').exists() and ((j/'stage').read_text().strip() in ['measurements','complete']) else 'Pending final check.'
measure=(j/'measurement-tables.md').read_text() if (j/'measurement-tables.md').exists() else 'Measurement campaign pending.'
text=f'''# PR #3919 review corrections — C++ request receiver

Worktree `/root/worktree/zlink-3867f`, detached at authorized HEAD `247b16c4be`. Status: {'complete' if complete else 'validation in progress'}. Scope is C++ implementation/tests plus this scratch directory. No commit, push, branch mutation, synchronization, GitHub access, Core C header inspection or sub-agent was used.

## Result and ownership

P1 fixes the registered Actor double delivery and an additional asynchronous Node-classification double delivery found by the required audit. P2 separates callback capture lifetime from task-result lifetime without another heap allocation on the measured paths. P3 reduces inline storage from **1,024 to 640 bytes**, preserving the common request's one ownership allocation.

Ownership: Framework C1 selects a logical terminal; its existing physical producer delivers the physical result and each codec layer converts once. Binding/Core retains native admission, progress and drain ownership.

Spec: S01 = `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md` §§2,9,10.1 (especially 445–488); native operation ownership remains the binding async/coroutine policy.

Cross-language comparison: .NET `ZLinkManagedMeshNode.cs:11085–11121` conditionally takes with `TryRemoveOperation` before native terminal dispatch; Java `ZLinkServiceOperationRegistry.java:343–369,508–516` registers on its owner and conditionally removes one entry; Kotlin shares the Java runtime; Node `operation-registry.ts:434–442` takes/deletes one entry. These implementations have the same single-terminal behavior. References were checked before implementation and are saved in `cross-language-owners.txt`.

Classification: **B** for competing physical delivery and unused-capture retention; **A** for private storage representation/sizing. No C/D change, public API change or spec edit is needed. The user's job explicitly authorizes these corrections.

| Decision | Spec owner | Before decision sites | Final owner | Decision sites before → after |
|---|---|---|---|---:|
| Registered Actor physical delivery | S01 §§9,10.1; physical producer under C1 | Physical receiver plus `actor_client.cpp:896–900` submission observer | Physical producer, {ref('raw','void deliver_request_terminal')}; observer has only admission, {ref('actor','[admitted = receiver.admitted]')} | **2 → 1** |
| Registered Node delivery after asynchronous classification | S01 §§9,10.1; C1 | C1 callback `mesh_node_runtime.cpp:4421` plus classification catch `:4502` when admission-observer installation throws | Stored callback ownership is transferred in {ref('mesh','receiver.complete =')}; the submission boundary catches post-registration observer failure without physical delivery | **2 → 1** |
| Pre-registration rejection | S01 §2; submitting source | Source fallback | Same unconsumed callback/source | 1 → 1 |
| Reply / timeout / cancellation / shutdown winner | S01 §10.1; C1 | Existing C1 take | {ref('registry','bool operation_registry_state_t::take')} and its completion slot | 1 → 1 |
| Uninvoked callable-capture retirement | S01 §9; physical callback lifetime | Request task block's final owner | In-block callback ownership, {ref('req','~callback_lifetime_t ()')} | 1 → 1 |
| Native admission / progress / drain | Binding async policy | Binding/Core | Unchanged | 1 → 1 |

Registered physical-delivery decision sites total **4 → 2**. No completed flag, projector-engagement check, terminal counter, timer, retry, lock, queue or registry was added. The new standard shared ownership control block replaces task-handle ownership as the retirement owner of unused callable captures; it is allocated inside existing request storage. Per-layer conversion ownership remains one site per codec.

## Exhaustive receiver and delivery audit

All paths in this section are relative to `framework/languages/cpp/framework/`. The exact exhaustive searches are `receiver-inventory-before.txt`, `receiver-inventory-final.txt` and `delivery-sites-before.txt`. The production receive callers are `call.hpp:206,257`, `actor_client.cpp:873`, `route_channel_runtime.cpp:409,506`; the delegating receive helpers in `request_task.hpp` add no producer.

The two physical authorities used below are:

- **Node C1 callback:** {ref('mesh','void mesh_node_runtime_t::submit_node_request_reply')}, registered by moving the callback at {ref('registry','entry->completion->callback = std::move (callback)')}. C1's take at {ref('registry','bool operation_registry_state_t::take')} consumes one completion slot. Expiry (`operation_registry.cpp:698–747`) and shutdown (`:765–799`) take that same slot; both cannot deliver it.
- **Actor physical producer:** native completion at {ref('raw','void raw_mesh_node_owner_t::observe_request')} → {ref('raw','void deliver_request_terminal')} → `public_host_runtime.cpp:3192–3201` → {ref('mesh','void mesh_node_runtime_t::prepare_actor_request_reply')}. Before native start, the readiness coroutine at {ref('raw','task_t<void> run_request_admission')} owns physical delivery. Once native start succeeds (`raw_mesh_node_owner.cpp:317–320`), it returns and leaves the binding callback as the sole producer. If C1 already ended, `pending_operation_t::receive` (`public_host_runtime.hpp:217–231`) waits for that same public terminal, then delivers one late physical result.

| Path / conversion entry points | Rejection before registration | Native admission rejection after registration | Deadline before native start | Normal reply | Shutdown pending |
|---|---|---|---|---|---|
| Typed Node; `call.hpp:206`; channel decoder/target at `channel_runtime.cpp:1843,1898`; C1 wire decoder at {ref('mesh','receiver.complete.template capture<runtime::foundation::operation_terminal_t,')} | Target classification at {ref('mesh','void mesh_node_runtime_t::request_node_reply')}, or the unconsumed submission callback. They are mutually exclusive branches. | Node C1 callback once. Submission observer has no consumed callback. | Node C1 callback once; raw readiness has no independent Node physical receiver. | Node C1 callback once; later takes fail on the emptied slot. | Node C1 callback once, including after the public handle drops. |
| Node after pending target classification; {ref('mesh','void mesh_node_runtime_t::request_unknown_node_reply')} | Classification failure/rejection invokes the classification fallback once. | Same Node C1 callback. A post-registration exception while installing admission observation now publishes admission failure only. | Same C1 callback; an observer-installation failure cannot deliver again. | Same C1 callback. | Same C1 callback; observer-installation failure cannot deliver again. |
| Remote Actor; {ref('actor','submit_request (resolved_actor_t actor,')}; projection at {ref('actor','prepare_request_receiver (resolved_actor_t actor,')} | {ref('actor','if (operation.receiver)')} uses the retained receiver only before transfer. Host/raw callbacks are consumed synchronously at C1 publication; host then clears the source receiver at {ref('host','if (!complete_public)')}. | Actor native physical producer once. The work observer captures only `admitted`, not a final receiver. | Actor readiness producer once; registry callback publishes public failure only. | Native physical producer once; C1 public callback does not project. | Before native start: readiness producer once. After native start: binding's one late physical callback; old late-result tests cover this independently. |
| Local Actor; {ref('host','std::chrono::steady_clock::time_point public_host_runtime_t::register_local_completion')} | Registration rejection leaves `complete` unconsumed; that same callback delivers physical failure once before rethrow. Earlier lookup/preparation failure stays with the Actor source fallback. | No native binding operation in this path. | Local C1 callback once. | `complete_local_request` takes C1, then its callback owns the receiver. Reply tokens have no receiver after registration. | Local C1 callback once. Source observer has admission only. |
| Node over a direct route channel; channel projection `channel_runtime.cpp:1908` | Route `failed` callback at `route_channel_runtime.cpp:424–430`, before backend physical observation is installed. | Backend admission forwards public failure only; backend physical task delivers once through `:467`. | Before backend start, route submission fails and `failed` runs once. After start, backend physical task delivers once. | Backend physical task once. | Backend physical task once; if the lane rejected before start, only `failed` exists. |
| Raw route parts / route Spot wrapper; `route_channel_runtime.cpp:409,412,506` | Same route `failed` callback. The internal/public wrappers only compose types. | Same backend physical task; no admission decoder. | Same route submission/backend split. | Same backend physical task. | Same backend physical task or pre-start `failed`, exclusively. |
| Direct mesh Spot decoder; `channel_runtime.cpp:2270` | Before child observation, the submitting boundary settles the error once. | `request_receiver_t::observe` forwards admission separately; only child physical reply projects. | Before child start, the boundary error wins. After start, the child's one physical reply projects. | One child physical reply. | One child physical reply, retained through early public failure. |
| Direct route Spot decoder; `channel_runtime.cpp:2285` | Same direct-route `failed` callback. | Same route backend physical task. | Same route submission/backend split. | Backend physical task once. | Backend physical task once or exclusive pre-start failure. |
| Resolved Spot / Instance Spot; extra projection `channel_runtime.cpp:2392`; success `:2434`; outer failure `:2471` | Resolve/configuration failure invokes outer failure once, with no child physical observer. | Selected child admission changes public state only. The coroutine awaits its physical task; final error reaches outer failure once. | Deadline before a child starts throws to outer failure once. A started child has one physical terminal. | Success invokes receiver then immediately returns. The authority-follow loop does not deliver intermediate refusals. | Selected child physical failure throws to the one outer failure callback. There is no final-receiver submission observer. |
| Raw encoded-to-message wrapper; `call.hpp:257,260` | Same selected source as above. | Same selected source. | Same selected source. | Synchronous one-to-one conversion; no extra producer. | Same selected source. |
| ChannelName, client/server and mesh; `channel_runtime.cpp:1977` / `:2000–2090`; app factory `app.cpp:999–1021` | Existing selected task completes once and the physical-task projections each run once. | Admission ends public reply only; selected physical task remains the sole projection trigger. | Existing selected C1/task terminal once. | Existing physical-task terminal once. | Existing selected C1/task terminal once. These paths use no prepared inline receiver block. |
| Streams / connectors / HTTP | No independent `receive_public`, `receive_internal` or `request_receiver_t::project` producer exists in these modules. Stream Actor routing delegates at `app.cpp:2049,2794` to the Actor/relay task path. | Same delegated physical producer. | Same delegated physical producer. | Same delegated physical producer. | Same delegated physical producer. |

Every `request_receiver_t::project` user is covered: Actor (`actor_client.cpp:812`), route wrapper (`route_channel_runtime.cpp:412`), raw encoded wrapper (`call.hpp:260`), direct mesh/route Spot (`channel_runtime.cpp:2270,2285`) and resolved Spot (`:2392`). The two manual Node channel captures (`:1898,1908`) and C1 capture are covered as well. `request_task.hpp`'s generic `project` only stores a synchronous conversion and a borrowed downstream callback; it creates no terminal authority.

A synchronous throw before any producer is retained is the P2 abandonment case: {ref('req','const auto failure = current_exception_result<T> ();')} settles the admission/public/physical stages directly once, and the unused projector is never called. Its last callback owner retires captures. An early public/admission failure is never used as a physical conversion trigger. `request_receiver_t::observe` bridges child admission and physical reply separately; backend task duplicate results are rejected by `task_shared_state_t::complete` (`task.hpp:622–660`). Observer installation allocates before attaching (`task.hpp:791–805`), with no throwing step after a physical observer is attached.

## Fixes with source references

- **Actor:** {ref('actor','static task_t<void> submit_prepared_request')} handles a pre-registration failure through the still-owned projected receiver, and publishes admission failure for registered requests. {ref('actor','[admitted = receiver.admitted]')} makes the submission-failure observer structurally incapable of physical delivery. {ref('host','if (!complete_public)')} relinquishes the source receiver when C1 consumes the callback. {ref('raw','task_t<bool> raw_mesh_node_owner_t::request_to_actor')} is now the same synchronous registration forwarding boundary as Node, removing an unnecessary coroutine allocation.
- **Registration failures:** {ref('raw','// An empty callback was consumed at C1 publication')} routes a post-publication exception through C1 plus its physical receiver. Local registration at {ref('host','std::chrono::steady_clock::time_point public_host_runtime_t::register_local_completion')} preserves the unconsumed callback on rejection and uses C1 when publication already consumed it. This preserves physical completion after removing the old observer fallback.
- **Additional Node defect:** {ref('mesh','void mesh_node_runtime_t::submit_node_request_reply')} now owns the exception boundary as well as registration. {ref('mesh','receiver.complete =')} transfers physical ownership into the stored callback. A retained callback handles rejection before registration; a consumed callback leaves only admission publication in the catch. The asynchronous classification catch therefore cannot become a second registered deliverer. The observer-installation fault hook is test-only and is compiled out of production builds.
- **Capture lifetime:** {ref('req','~callback_lifetime_t ()')} retires stored callable captures and the final projector when the last callback owner disappears. {ref('req','auto callbacks = request_capture_storage_t::own_callbacks (storage);')} gives physical callbacks this independent ownership while task stages retain the result block. Allocator ownership keeps that block alive through control-block destruction/deallocation. The existing borrowed downstream callbacks avoid cycles. The retained submit target keeps its existing value lifetime and all original inline/pending target-lifetime assertions remain unchanged.
- **Storage:** {ref('req','std::array<std::byte, 640>')} fits the measured maximum common-path inline use, including the callback control block, without an upstream allocation. Larger captures retain the existing upstream fallback.

Changed files: `framework/include/zlink/framework/contracts/dispatch/request_task.hpp`; `framework/include/zlink/framework/detail/test_creation_probe.hpp`; `framework/src/runtime/actors/actor_client.cpp`; `framework/src/runtime/mesh/{{mesh_node_runtime.cpp,raw_mesh_node_owner.cpp,raw_mesh_node_owner.hpp}}`; `framework/src/runtime/stateful/public_host_runtime.cpp`; `tests/Zlink.Framework.UnitTests/{{test_cpp_framework_actor_late_result.cpp,test_cpp_framework_request_call_allocations.cpp,test_cpp_framework_request_transport_projection.cpp}}`.

## Verification and negative controls

- Actor negative control (`repro-actor-before.log`): real public Actor request → `actor_client_t::submit_request()` → prepared submission → real host/raw registration. Native admission rejection, pending timeout and pending shutdown each delivered **2 instead of 1**. Pre-registration rejection passed. The final same three failures, plus pre-registration rejection, publication exception and local registration rejection, pass; physical receiver and public result counts are each **1**. These tests do not bypass prepared submission.
- Additional Node negative control (`repro-node-observer-before.log`): a held real Location Store classification completes, raw C1 registers the request, then admission-observer installation throws. Native rejection, pending timeout and shutdown each delivered **2 instead of 1**. All three final tests pass with one physical conversion and one public result. The test-only fault point is exactly between real submission and admission-observer installation.
- P2 negative control (`repro-captures-before.log`): both pre-registration throw and last-callback abandonment retained their weak-marker resources while completed public handles remained. Final tests pass, including a marker in the final projector and a composed projector; completed public/physical handles are retained. The escaped physical producer positive control proves that a later delivery still has its projection captures.
- Regression locations: Actor tests {testref('test_cpp_framework_actor_late_result.cpp','class submitted_actor_receiver_t')}; additional Node tests {testref('test_cpp_framework_request_transport_projection.cpp','void unknown_node_observer_failure')}; P2 tests {testref('test_cpp_framework_request_call_allocations.cpp','ThrowBeforeRegistrationReleasesUnusedProjectionWithHandlesRetained')}.
- **Focused CTest: 5/5 passed**, 1.20 s (`test-focused.log`). This includes **49/49** allocation/receiver tests and **28/28** Actor late-result/production-path tests, plus registry, direct-route and native-count suites. Details, measured sizes and one-allocation assertions are preserved in `test-focused-details.log`.
- Full Release build: {full_status}
- Unit + contract CTest: {unit_status} (`test-unit-contract.log`).
- Format: {fmt}

Final commands, from `framework/languages/cpp`:

```sh
flock -s /root/jobs/fx/cx/measure.lock /root/jobs/fx/cx/cpp-build-slot.sh \\
  cmake --build --preset linux-ninja-release --parallel 3 -- -k 0
flock -s /root/jobs/fx/cx/measure.lock \\
  ctest --preset linux-ninja-release -L '^(unit|contract)$'
```

From repository root: `scripts/format/format.sh --check cpp`. The preset builds all targets, including samples and C++ cross-language executables. Core/binding packages remain the same deployed 1.23.0 packages used by crecv2. This job does not use CI to discover local defects.

## Bytes per request and retention

Linux x86-64 / libstdc++ measurements. Stored values exclude node metadata; stored capture bytes include metadata and `optional` representation. The final typed projector remains in the request block and is 8 bytes on Node; Actor's identity projector is 1 byte. Its lifetime is covered by the P2 root-projector marker test.

| Path | Stored value bytes before → after | Stored captures before → after | Callback ownership bytes before → after | Inline bytes used before → after | Inline capacity before → after |
|---|---:|---:|---:|---:|---:|
| Node | 504 → 504 | 576 → 600 | 0 → 40 | **576 → 640** | **1,024 → 640** |
| Actor | 448 → 448 | 472 → 480 | 0 → 40 | **472 → 520** | **1,024 → 640** |
| ChannelName | 0 → 0 | 0 → 0 | 0 → 0 | 0 → 0 | No prepared receiver block |

Node's 504 value bytes are the retained routing ID (256), channel decoder/projection (152), and C1 callback (96). The extra 8 metadata bytes per stored entry provide destruction of uninvoked callables independently of retained submit values. All measured inline allocations have 8-byte alignment and fit exactly in 640 bytes on this platform.

| Scope / retained completed-handle memory | Before | After | Ownership allocations before → after |
|---|---:|---:|---:|
| Benchmarked Node `BenchPayload` request block | 1,792 B | 1,408 B | 1 → 1 |
| Same Node primary heap allocation, including its control block | **1,808 B** | **1,424 B** | **1 → 1** |
| Actor request block | 2,032 B | 1,664 B | 1 → 1 |
| Actor primary heap allocation, including its control block | 2,048 B | 1,680 B | 1 → 1 |
| Isolated prepared receiver with two projections | Same single owner | Same single owner | **1 → 1** |
| ChannelName | Existing ordinary task stages | Unchanged ordinary task stages | 0 → 0 prepared-block allocations; ordinary stages unchanged |

The Node block uses the benchmark's real `BenchPayload` type (32 B) and the same one-pointer final projector layout. `block-size.json` records exact compiler commands and the libstdc++ control-block size; test-build probes record actual production Node/Actor captures in `test-focused-details.log`. The Node byte probe's small test result has a 1,344 B final block; this is not substituted for the 1,408 B benchmark block above.

The table counts requested primary allocation bytes; allocator metadata/rounding are excluded. Dynamic decoded reply payloads, native buffers and registry allocations are outside these byte figures; their representation is unchanged. A completed task handle still retains its primary block, including inline capacity. It retains **384 fewer bytes per benchmarked Node request** and **368 fewer bytes per Actor request**. Completed callable captures release at physical conversion as before. Never-invoked callable captures now release when their last callback owner disappears; they no longer retain the marker/client/decoder resources until the task handle dies. The independently retained routing ID value and capture-storage memory remain with the primary block. No claim is made that the inline buffer is freed separately from the task allocation.

The selected common-path size costs no additional allocation. The large-capture tradeoff is measured separately in `storage-options.json`:

| Storage-only calibration | 1,024 B inline, before | 640 B inline, final |
|---|---:|---:|
| Node-shaped 256/152/96 B stored values | 1 allocation, 0 upstream bytes | 1 allocation, 0 upstream bytes |
| One 800 B custom stored value | 1 allocation, 0 upstream bytes | **2 allocations, 1,024 upstream bytes** |

The custom case's capacity is retained until the primary block dies, even after callable resources are retired. This is the explicit smaller-storage tradeoff; it is not part of the benchmarked Node/ChannelName/Actor capture set. The normal-path ownership count and whole-process census have different scopes; neither is a claim of one heap call for an entire network request.

## csubmit before/after

Same condition B runner, collectors and benchmark source as crecv2, with only output/build-root substitutions. Both patterns use 1 KiB, three runs, 30 s warmup / 5 s active, CPU affinity 0–7 / 8–15 and paired binding controls. The campaign is under exclusive `flock -o /root/jobs/fx/cx/measure.lock`; MemAvailable is checked to be at least 12 GiB after acquiring it (`measure-memory.log`) and by the unchanged CPU collector on every snapshot. Runner/source equivalence is recorded in `runner-equivalence.json`. Endpoint cleanup uses SIGTERM only.

Before cohort: `/root/jobs/fx/cx/v21/s033/crecv2/measure/run{{1,2,3}}/{{zlink-cpp,zlink-framework-cpp}}-{{request-backpressure,send-saturation}}-1024`. After cohort: the same layout under this directory's `measure/`. Each cell contains `results.json`, endpoint logs and `capture/` CPU snapshots. Full per-run rates/ranges and actual thread IDs are in `measurements.json` and `threads.csv`.

{measure}

Framework request throughput is **50,895.7 msg/s (−2.0%)**, while its paired binding median is −3.0%; request F/B changes from **0.207 to 0.209**. Framework send throughput is **173,869.5 msg/s (−1.2%)**, with send F/B **0.287 to 0.282**. Caller CPU rises by 0.302 µs/request and 0.087 µs/send. These measurements do **not** demonstrate a throughput improvement; the demonstrated benefit is smaller retained request storage with the same calibrated one-block ownership allocation.

The complete caller census changes from **17.251161 to 17.261617 heap calls/request** (+0.010456, about +0.061%). The isolated storage calibration remains **1 → 1**, without a normal-path upstream allocation. The whole-process census is reported as measured, rather than described as an exact allocation reduction.

The allocation census uses the same interposer and eight-second counter window as crecv2; raw before is `crecv2/census/after/run1/zlink-framework-cpp-request-backpressure-1024`, raw after is `crecv3/census/after/run1/zlink-framework-cpp-request-backpressure-1024`. Census throughput/CPU is excluded from unprofiled comparisons. Rates are historical three-run medians, not an interleaved A/A significance study; per-thread CPU uses the same sampled `/proc` accounting, with the same retiring-thread-tail limitation.

## Refactoring inspection and remaining work

Performance **1**: oversize inline request storage. POSDDD **3**: competing Actor delivery, competing asynchronous Node delivery, and callback captures owned by completed task handles. Unnecessary code **1**: the raw Actor forwarding coroutine. All **5 fixed**, none deferred. Registered physical-delivery decision sites **4 → 2**. Existing gate, native-progress and codec decisions remain with their original owners; no gate-bypassing synchronization was added.

{'No required build/test/format/measurement failure remains. No benchmark endpoint process remains; final artifacts and source hashes are recorded in final-state.json.' if complete else 'Final all-target build, unit/contract gate and measurement campaign are still in progress; this report will be updated when they finish.'}
'''
(j/'report.md').write_text(text)
