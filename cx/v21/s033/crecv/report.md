# C++ request receiver — #1639 / #3867

Work in progress. Source: `/root/worktree/zlink-3867r`, branch `perf/3867-cpp-request-receiver`, baseline `c8869b63b19c8504a727459c766bf3ccee995708`. Only C++ files and this scratch directory are written. No repository synchronization, GitHub action, branch mutation, commit, push, or sub-agent is used.

## Diagnosis and decision owners

Classification: **A — representation adaptation preserving the existing contract**, within the implementation scope explicitly authorized by the job. Baseline evidence: `../cladder/report.md` rung 2 and `../cthin/report.md` §2 R1/R2/R5/R7 and §3. The allocation/lifetime representation is owned by C++ Framework task/receiver internals; it does not transfer protocol or execution authority.

| Decision | Spec | Before location(s), relative to `framework/languages/cpp/framework/` at c8869b63b1 | After owner | Decision location count before → after |
|---|---|---|---|---|
| Admission result, independent for each submission | S01:61–77; binding policy:160 | `src/runtime/backend/raw_route_port.cpp:351–393` consumes binding `.async()`; `raw_binding_adapter.hpp:177–205` publishes its admission | Same binding/Core authority; same bridge | 1 → 1 |
| First public task result, including admission failure | S01:331–340 | `include/zlink/framework/contracts/dispatch/task.hpp:600` first-result mutex/optional, used by `request_task.hpp:402` admission observer and physical projection | Same `task_shared_state_t::complete`; embedded stages alias one request allocation | 1 → 1 |
| Physical completion and conversion after early public completion | S01:331–340 | `request_task.hpp:311–327`, `channel_runtime.cpp:1770–1843`, `mesh_node_runtime.cpp:4399–4438` | Same codec owners and binding physical terminal; captures share request storage | 1 → 1 for each codec and physical producer |
| Logical terminal competition | S01:445–488 | `src/runtime/foundation/operation_registry.cpp` C1 take/unregister | Existing operation registry, unchanged | 1 → 1 |
| Registration, close, deadline visits | S01:453–488 | `src/runtime/mesh/raw_mesh_node_owner.cpp:1519`, existing operation owner and C2 index | Unchanged operation owner and deadline index | 1 → 1 |
| Callback execution placement and shutdown drain | S01:490–544 | `src/runtime/dispatch/provider_task_context.cpp:171–190`, existing dispatcher; task wait registrations | Same dispatcher and wait owner; no inline-on-notification exception | 1 → 1 |
| Native retry, completion drain, native lifetime | binding policy:160,189 | Public binding `.async()`/await and existing poller | Binding/Core, unchanged | 1 → 1 |
| Forwarding callable storage | S01:331 | Separate shared callable allocations in receiver, channel and mesh adapters | One request capture storage, synchronous codec projections | 0 → 0 (no decision authority) |

The alternative of independent `shared_ptr` owners per adapter preserves behavior but retains the profiled heap/refcount work. The chosen representation puts the three observable stages, source wrapper and final projector in one allocation. Admission failure is published by the admission state into the public state before notifying its observers; it no longer allocates a forwarding observer. Synchronous adapter captures use that same block's storage. Capture storage is per-request memory, not a cache, execution queue, pending registry, retry mechanism or completion flag. It has an upstream allocator for unusually large captures; no API size limit is introduced. No new logical queue, lock, cache, flag or counter authority is added.

Cross-language baseline: .NET `Runtime/Service/ZLinkManagedMeshNode.cs:11039` registers before native submit and uses `TryRemoveOperation` for terminal selection; Java `ZLinkServiceOperationRegistry.java:343–369` registers on its owner and `:508–516` removes once; Kotlin uses Java's runtime; Node `runtime/foundation/operation-registry.ts:434–441` takes/deletes once. Their physical binding results and asynchronous delivery remain distinct from logical terminal ownership, as in `cthin` §3. This patch changes C++ allocation representation only and introduces no cross-language behavior difference.

## Implementation, verification and measurements

Initial bidirectional allocation check: the same standalone probe built with baseline headers rejects the one-allocation budget (**7 allocations**, exit 1), while the changed headers pass (**1 allocation**, exit 0); both decode the same result, 41. Raw: `allocation-positive-negative.json`, `receiver-allocation-probe.cpp`, `receiver-before-build.log`, `receiver-after-build.log`. Whole-program allocation census remains pending.

`format.sh --check cpp` passed (`format.log`). Focused verification passes: task and operation registry (2 CTest targets) and all 33 request allocation/lifetime tests (`test-focused-task-registry.log`, `test-focused-allocations-corrected.log`). Final all-target build and unit/contract gate, rates/CPU and source-format recheck remain pending.

Changed files (relative to `framework/languages/cpp/`):

- `framework/include/zlink/framework/contracts/dispatch/request_task.hpp`: co-allocate the request stages and projector; store synchronous codec captures under that ownership; retire capture objects at physical processing; remove four unused preparation/receiver overloads.
- `framework/include/zlink/framework/contracts/dispatch/task.hpp`: let embedded stages alias their request lifetime; publish admission's public failure without a forwarding heap object; reuse the stable state owner for task observers.
- `framework/src/runtime/channels/channel_runtime.cpp`: put decoder captures in the request block and retain their address during submission and conversion.
- `framework/src/runtime/mesh/mesh_node_runtime.cpp`: register a callback sharing the same receiver block instead of boxing a new mesh receiver owner.
- `tests/Zlink.Framework.UnitTests/test_cpp_framework_request_call_allocations.cpp`: verify co-location while preserving distinct stage addresses; add one-block, several-observer, early-public/const-capture-lifetime and pending-shutdown regression coverage.

The raw binding adapter, raw route port/poller and provider dispatcher were inspected. Their existing raw callback uses inline storage in the coroutine, and the poller consumes the public binding result; no extra per-projection post exists there to delete. Keeping those components unchanged preserves their single physical producer and new-turn placement. This patch removes ownership allocations at the receiver/projection boundary; it does not claim to remove the one required registry dispatcher post or per-independent-observer registrations.

Build invocation detail: the supplied `cpp-build-slot.sh --parallel 3 cmake ...` fails with exit 127 because that wrapper treats its first argument as the command. `build-requested.log` preserves the failure. The equivalent supported invocation puts `--parallel 3` on `cmake --build`, under the same shared measure lock and build-slot wrapper.


Added/extended test details:

- `PreparedReceiverAndCodecCapturesUseOneOwnershipBlock`: one allocation including two projections, three distinct stage result locations in that allocation, correct result and complete block release after all handles are dropped.
- `PhysicalProjectionReleasesConstCapturesWithTaskHandlesRetained`: admission failure ends public reply first; const captures remain through physical conversion and are released even while public/physical handles remain.
- `PreparedReceiverKeepsSeveralIndependentObservers`: two public handles to one state, two independently registered observers, identical final value; explicitly waits for delivery because the test process owns another completion owner.
- `ShutdownDrainsPendingPreparedReceiversAndTheirObservers`: 16 pending registry requests settle at shutdown, with physical/admission stages ready and all observers off the notification thread.
- `NodeReplyConstructsOnlyFinalPublicAndPhysicalResultStates`: replace standalone-allocation-size inference with the existing creation probe; three stage objects, no conversion state; an injected fourth state must be rejected. Existing physical/public distinct result addresses, target address stability and three terminal notifications are retained.
- Existing channel tests retain separate projection-allocation checks; prepared node and Spot receivers explicitly require co-location. The baseline early-physical-conversion/error/payload-release and wait-owner cancellation cases remain unchanged and pass.
