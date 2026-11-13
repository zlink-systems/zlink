# Issue #3867 review fixes

Base: `013a924289aab5390f7f11a25e0c57adfd20869f`  
Branch: `perf/3867-wire-writer`  
New commit: `a45e00386d8934d4d8e3706b4b59f39bdb530276`.

## Changes

- `native_wire_writer_t` now checks `push_back`, `insert`, and `take()` in every build configuration. Overflow is checked before copying/writing; the writer throws `service_wire_error_t` with a message naming the native wire writer. `take()` rejects both short and long output. Removed the unused `<cassert>` include.
- Added `native_wire_writer_bounds_test_access()` as a headerless internal test seam in `service_wire_codec.cpp`. The writer is private to that translation unit and existing public codec seams cannot produce underwrite/overflow on demand. The unit test forward-declares only this function and exercises push overflow, insert overflow, and take underwrite. The test translation unit rejects `NDEBUG`; the library was built as Release with `NDEBUG`, so it verifies the production writer checks are unconditional.
- Audited lvalue copies of `application_payload_t` and payload-bearing records. Moved values after their last use, avoided copies when a retained frozen record could be referenced, made the frozen-record encoders consume by-value inputs, and removed duplicate relocation closure captures. No shared holder was reintroduced.

## Copy-site audit

`C` means `framework/languages/cpp/framework/src/runtime/`. “Source used later” refers to the source owner after the copy/move expression. Relocation rows are cold per-record work, not ordinary request/send fanout.

### Lvalue copies removed or changed to moves/references

| Site | Path | Source used later? | Change |
|---|---|---:|---|
| `C/fanout/fanout_location_runtime.cpp:813` | Channel publish/send | No | Move local `encoded` into `publish`. |
| `C/stateful/raw_stateful_dispatch.cpp:740,748` | Per-message actor/Spot request or send admission | No | Move `payload` into the accepted-record builder. |
| `C/stateful/raw_stateful_dispatch.cpp:491-492` | Per-message stateful forwarding | The enclosing turn is read for its sequence; these payload fields are not | Move the optional application/frozen record into the forwarding value. |
| `C/protocol/service_wire_codec.cpp:2394-2397` | Frozen-record decode | No, after validation | Move decoded body application into the result. |
| `C/protocol/service_wire_codec.cpp:2498,2506` | Frozen-record summary/encode | No, after validation | Move the selected actor/Spot body application; removed the unused application copy from validation input. |
| `C/stateful/stateful_object_runtime.cpp:1673-1675` | Cold relocation snapshot | No | `encode_frozen_application_record` now takes by value; move the saved application record into it, then clear the optional. |
| `C/stateful/public_host_runtime.cpp:4263,4269-4277` | Cold relocation restore | The staging snapshot remains for retry/cleanup; the mutable local copy is not used after extraction | Move each local frozen record into saved relocation data instead of copying it. |
| `C/stateful/public_host_runtime.cpp:6740-6746,6772` and `C/mesh/mesh_node_runtime.cpp:828-835` | Per-message ActorJoin request | No | Move the decoded payload to the target callback; the by-value callback parameter moves it into the async capture. |
| `C/stateful/maintenance_runtime.cpp:549-558` | Cold relocation terminal registration | Yes | Reference `turn.frozen_record` directly instead of copying its payload-bearing record. Decode/summary results use a local owner only when needed. |
| `C/spots/spot_runtime.cpp:8042-8047` | Cold actor-join recovery | Yes | Reference an existing frozen record instead of copying it; decoded fallback remains locally owned. |
| `C/stateful/public_host_runtime.cpp:5026-5035` | Cold relocation terminal callback | Borrowed `data` remains caller-owned | Removed the local `frozen_record` copy and capture of all `data`; the callback keeps one necessary frozen identity copy plus scalar fields. |

### Lvalue copies retained because two owners are needed

| Site | Path | Source used later? | Why both owners are needed |
|---|---|---:|---|
| `C/stateful/raw_stateful_dispatch.cpp:812` | Per-message accepted request/send | Yes; `frozen` moves into the accepted turn at 837 | The pending delivery keeps a summary while the accepted turn keeps its frozen application. |
| `C/stateful/raw_stateful_dispatch.cpp:921-923` | Per-message stateful delivery | Yes; `turn` moves into the delivery at 926 | The delivery needs both its turn and its frozen delivery identity. |
| `C/stateful/raw_stateful_dispatch.cpp:935` | Per-message terminal completion | Yes; pending entry remains registered | Delivery receives a summary while the pending entry retains terminal identity for later completion/retry. |
| `C/stateful/raw_stateful_dispatch.cpp:962-963` | Per-message local accepted request/send | Yes; accepted owner retains the moved turn | Pending delivery needs its summary while the accepted owner retains the original application record. |
| `C/stateful/raw_stateful_dispatch.cpp:1232` | Per-message accepted ingress | Yes; local `frozen` moves into pending delivery | The turn and pending reply identity both retain the frozen payload. |
| `C/stateful/stateful_object_runtime.cpp:1664,1668` | Cold relocation snapshot | Yes; live queue/accepted owner remains | Snapshot copies either an inline pending record or its materialized accepted record while the live owner remains. The later snapshot encoding now moves its copy (see above). |
| `C/stateful/maintenance_runtime.cpp:509` | Cold relocation wire record | Yes; source batch record remains | Relocation output owns the encoded frozen payload while the batch remains available. |
| `C/stateful/maintenance_runtime.cpp:553` | Cold terminal-source registration | Yes; source turn remains in the snapshot | The temporary summary is passed to terminal-identity registration while the snapshot keeps its record. |
| `C/stateful/maintenance_runtime.cpp:1619,1622` | Cold relocation envelope | Yes; frozen snapshot remains | Envelope construction needs its own canonical record while the source snapshot remains reusable. |
| `C/stateful/public_host_runtime.cpp:4263` | Cold relocation restore | Yes; immutable staging source remains | A mutable restore snapshot is needed while staging remains available for retry/cleanup. Its extracted record is moved afterward. |
| `C/stateful/public_host_runtime.cpp:5031,5042` | Per-relay relocation terminal registration | Yes; `data` is borrowed and callback outlives the call | The callback retains frozen identity; registration owns a reply copy while the callback’s reply source remains live. |
| `C/stateful/raw_stateful_dispatch.cpp:1878-1880` | Per-retry relocation reply | Yes; durable registration remains for future retries | Each pending send owns its reply while the registered terminal retains it for another retry. |
| `C/mesh/mesh_node_host_service.cpp:2543,2557-2559` | Per-request ActorCreate reply | Yes; the local reply is also sent to publication | Completion result and publication each retain the reply. |
| `C/stateful/public_host_runtime.cpp:6822-6824` | Per-request User Spot Create replay | Yes; cache entry remains for subsequent retries | The request gets a replay copy while the durable cache retains its terminal. |
| `C/stateful/public_host_runtime.cpp:6875-6884` | Per-request User Spot Create reply | Yes; source is moved to transport after caching | Replay cache owns one copy and the current reply owns the other. |

`C/stateful/public_host_runtime.cpp:7125` also copies a User Spot Close terminal record, but that record has no application payload (`std::nullopt`), so it does not copy an `application_payload_t`. No additional per-target multicast/channel-fanout payload copies were found; the channel publish source is moved at `fanout_location_runtime.cpp:813`.

Audit result: 15 avoidable deep-copy operations removed or replaced with references; 19 lvalue-copy expressions retained where simultaneous owners are required; zero avoidable copies deferred. The resulting implementation has no new payload state, flags, counters, or shared ownership layer.

## Design and scope checks

- Owner layer: C++ Framework service-wire codec owns allocation sizing and write bounds.
- Spec clauses: `08-messaging-hot-path.ko.md` §3 E1 and `05-payload-ownership-and-codec.ko.md` §§2, 4. Wire representation and public request/reply/send behavior are unchanged.
- Cross-language comparison: C# `ZLinkServiceWireCodec.cs` and `ZLinkApplicationPayloadEnvelopeCodec.cs`; Java `ZLinkServiceWireCodec.java` (also used by Kotlin) retain the same wire/ownership contract. This change is C++ internal bounds enforcement and ownership transfer only; no managed runtime behavior diverges.
- Classification: **B — existing Release-build safety defect** in writer bounds. The copy changes are behavior-preserving representation/ownership fixes.
- Writer bounds decision locations: 3 (`push_back`, `insert`, `take`) before → 3 after; assertions became unconditional checks. No new runtime rule location or state was added.
- Refactoring audit: 15 avoidable deep-copy operations removed, 19 deep-copy sites retained because simultaneous owners are required, 0 deferred. No additional gate synchronization or ownership abstraction was introduced.

## Validation

- Build: `cpp-build-slot.sh cmake --build /root/jobs/fx/cx/v21/s033/wire/build --parallel 3 -- -k 0` — **PASS**, 249/249 Ninja steps. Build directory retained `ZLINK_FRAMEWORK_CPP_TEST_PROBES=ON`; Release library used `-DNDEBUG`.
- Tests: `/root/jobs/fx/cx/measure-shared.sh ctest --test-dir /root/jobs/fx/cx/v21/s033/wire/build -L 'framework-unit|framework-contract' --parallel 3 --output-on-failure` — **PASS**, 287/287 tests, 0 failures, 100.51 seconds.
- Format: `bash scripts/format/format.sh --check cpp` — **PASS**.
- Whitespace: `git diff --check` — **PASS**.
- MemAvailable samples during build and tests remained at least 14.85 GiB; requested 12 GiB floor was maintained in sampled checks.
- Non-fatal GCC warnings appeared in unrelated standard-library inlining from `public_host_runtime.cpp`, `actor_gateway_runtime.cpp`, and one unit-test translation unit; no warning was introduced by the writer changes.
- No A/B measurement or push was performed.

## Commit and remaining work

New commit SHA: `a45e00386d8934d4d8e3706b4b59f39bdb530276`.  
Unfinished: none.
