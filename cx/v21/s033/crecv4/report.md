# PR #3919 second review: physical reply delivery

## Fixes

1. **Local completion-sink construction** — `framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp:8674-8759`. The physical receiver stays in `operation.receiver` while the completion callback is constructed. It is cleared only after the callback has been built and transferred. The submission owner settles a constructed callback on registration rejection; when callback construction itself throws, the caller still owns the receiver and settles the physical failure.
2. **Node admission observer installation** — `framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp:4462-4490`. The unregistered completion remains reachable by the catch while the observer is installed. The test-only failure is injected in `task.hpp:1431-1437`, immediately before ambient-context capture, after observer construction and before installation completes.

Both injected failures are guarded by `ZLINK_FRAMEWORK_TEST_CREATION_PROBE`. The hooks are absent from production builds. No delivered/engaged flag was added.

## RED / GREEN evidence

RED runs used baseline `5ecae5de8b31c0633a6f7e8c1cb897b26c5ac87a` with each exact-point test hook enabled:

- Local callback construction RED: `red-local.log` — failed; public request failed but physical reply did not become ready and delivery delta was 0.
- Node observer snapshot RED: `red-node.log` — failed; physical reply did not become ready, delivery delta and conversion count were 0.
- Local callback construction GREEN: `green-local.log` — passed; public and physical results failed as expected, delivery delta was exactly 1.
- Node observer snapshot GREEN: `green-node.log` — passed; public and physical results failed as expected, delivery delta, conversion count, and public result count were each exactly 1.

The previously passing `PublicationExceptionDeliversPhysicalFailureOnce`, `RejectionBeforeRegistrationDeliversOnce`, and `RejectedLocalRegistrationDeliversPhysicalFailureOnce` were left unchanged and are not counted as regressions.

## Verification

- Full Release build: **passed, 420/420 targets**. The first build attempt found a test-hook linkage error; making the test-only task hook inline fixed it, and the full build was rerun successfully. Log: `full-build-final.log`.
- Unit + contract CTest: **passed, 297/297 tests**, 231.50 sec. Log: `test-unit-contract.log`.
- Format check: **passed**. Log: `format-final.log`.
- `MemAvailable` at completion: **26.59 GiB** (required minimum: 12 GiB).
- No benchmark remeasurement was run; both changes are on failure-only paths.

Commands run, from `framework/languages/cpp`:

```sh
flock -s /root/jobs/fx/cx/measure.lock /root/jobs/fx/cx/cpp-build-slot.sh \
  cmake --build --preset linux-ninja-release --parallel 3 -- -k 0
flock -s /root/jobs/fx/cx/measure.lock \
  ctest --preset linux-ninja-release -L '^(unit|contract)$'
```

From the repository root:

```sh
scripts/format/format.sh --check cpp
```

## Refactoring inspection

Performance: 0 findings. POSDDD: 2 findings, both physical-reply ownership gaps fixed here. Unnecessary code: 0 findings. Deferred: none. The existing completion/registration owners remain the only delivery decision owners; no new state or retry path was added.

## Runtime ownership record

- Owner: the C++ Framework submission owner retains the physical receiver until C1 callback/observer transfer; after transfer, the registered C1 completion owns reply delivery.
- Spec: `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:331,333,447` requires one physical reply delivery per request.
- Cross-language comparison: .NET `ZLinkRequestAdmission.Run` turns a synchronous submission exception into the request's failed reply; Java uses its completion bridge to couple admission/reply failures; Node's request flow uses promise completion. These paths do not construct the C++ `request_callback_t` via `std::make_shared` or capture inline ambient context in `observe_task_terminal`. The failure is specific to C++ callback ownership during construction/observer installation; no behavior difference in the shared one-delivery contract was found, so no other language changed.
- Classification: **B, existing C++ Framework defect**.
- Physical-delivery decision sites: 2 before → 2 after (same two submission/callback boundaries; no second delivery flag or new policy site).

Exact test locations: local test `test_cpp_framework_actor_late_result.cpp:649`, with injection immediately before callback-to-sink transfer at `public_host_runtime.cpp:8737-8743`; Node test `test_cpp_framework_request_transport_projection.cpp:294`, with injection armed at `mesh_node_runtime.cpp:4476-4480` and thrown from `task.hpp:1433-1437` during ambient snapshot capture.
