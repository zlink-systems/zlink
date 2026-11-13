# C++ request submit allocations — issue #3867

Commit: `317d4037cc983b85a7ca009bcb0316e51185e825` on `perf/3867-submit-alloc`. Committed locally; not pushed.

The submitter measured **49.466 → 29.262 heap calls/message** (40.84% fewer), against the ≤30 goal. Public and physical reply states remain separate. The measured final source changes representation, not submission/completion policy.

## Allocation census

Fresh reference: `/root/worktree/zlink-ratio040`, main `ee11390257`, prebuilt Release. Changed tree started at `3fb411b4ab`. The user-specified reference and starting base differ in offload_executor.cpp and state_lane.cpp/.hpp; throughput ratios compare those complete builds rather than an isolated same-base patch. Both use Core/binding 1.23.0. Framework only, request-backpressure, 1024 bytes, 10-second warmup and 20-second active phase; an approximately 8-second capture runs inside active. Reused `calloc` alloc_trace.so, collect.py and instrumented runner through `census.sh`. Each run held `flock -o /root/jobs/fx/cx/measure.lock`. The final runner checked that Ninja had no pending benchmark work.

Exact per-thread/process malloc/calloc/realloc counters use the completed-message delta from the same capture window. Site shares are sampled at 1/256 and rescaled to those exact totals. Aligned allocation APIs and direct mmap are outside this existing counter. No allocator replacement or custom allocation policy was added to the Framework.

| Scope | Before calls/message | After calls/message |
|---|---:|---:|
| Source submitter | 49.466 | 29.262 |
| Whole source process | 68.781 | 46.942 |
| Whole target process | 52.423 | 50.349 |
| Source + target processes | 121.205 | 97.291 |

| Capture | Before | After |
|---|---:|---:|
| Completed messages in capture | 327,367 | 361,625 |
| Submitter sampled stacks | 63,260 | 41,407 |

### Per-site submitter attribution

Same first Framework frame convention as the original census. A reported 0.000 means no sampled allocation was attributed to that site; it does not imply that every allocation in an inlined callee disappeared. The independent receiver-copy probe verifies its specific zero-allocation claim directly.

| Allocating site | Before calls/message | After calls/message | Representation change |
|---|---:|---:|---|
| `request_task_access_t::receive` | 6.014 | 4.061 | Admission source and state share one allocation; bind receivers directly to existing task states. |
| `submit_request_reply_message_erased` | 4.948 | 1.975 | Coallocate target/decoder/projection; remove unused default media string and borrow the retained selected serializer media string during synchronous encoding. |
| `request_receiver_t copy/constructor` | 3.007 | 0.000 | Copy shared callable ownership instead of cloning nested std::function captures. |
| `task_shared_state_t completion observer` | 2.995 | 0.994 | Coallocate callback and registration; store terminal callback in its list node. |
| `observe_task_terminal` | 0.992 | 0.000 | Forward the typed callback without a second std::function box; see relocated terminal-node attribution below. |
| `observe_task_completion` | 1.032 | 0.000 | Forward the typed callback into the combined observer allocation. |
| `channel.hpp request_to_node` | 2.037 | 0.000 | Own the request directly in the submit closure; share that closure. |
| `raw_mesh_node_owner request_with_header` | 1.996 | 0.001 | Reuse the existing node-owned outer wire vector capacity; clear all contents at each return/throw. Inline the bounded raw callback. |
| `raw_route_port request` | 2.039 | 2.936 | Borrow target bytes and consume the wire vector synchronously by rvalue reference. See inlining attribution below. |
| `operation_registry register_operation` | 1.991 | 2.025 | Unchanged: pending completion identity and deadline ownership remain in the registry. |
| `classify_node_target_from_topology` | 1.026 | 0.000 | Transparent span lookup avoids a temporary routing-id byte vector. |
| `envelope_codec encode_header` | 0.954 | 1.042 | Keep the final encoded envelope allocation; borrow the selected media type through the existing private codec overload. |
| `client_call_codec create_envelope` | 1.002 | 1.003 | Unchanged envelope representation; avoid the separate media-type copy in the caller. |
| `public_host_runtime request_to_node` | 0.977 | 0.001 | Borrow submit bytes; retain the fixed-size routing_id_t in the pending classifier. |
| `encode_node_request_header` | 2.993 | 1.014 | Reserve the complete 13-byte header once, then write the identical prefix and sequence. |
| `materialize_binding_part (unchanged, attribution control)` | 4.120 | 1.999 | Out of scope and unchanged. Some of its allocations inline into raw_route_port request; compare their combined count. |
| `relocated admission terminal node` | 0.000 | 0.997 | New first-frame attribution for the remaining terminal-observer node, not a new control decision. |

Inlining moves some unchanged materialization allocations from `materialize_binding_part` into `raw_route_port_t::request`. Read those two rows together; the sum drops by approximately one callback allocation. The receiver/submit closure and terminal-node rows also move attribution when allocations are combined or inlined. The exact total, rather than a sum of assumed object counts, determines goal attainment. `encode_application_payload`, `application_payload_t`, `message_parts_t` and `raw_binding_adapter.hpp` were not modified. Only `encode_node_request_header` changed in service_wire_codec.cpp.

Raw evidence: `before.json`, `after-final.json`, `census-before/`, `census-final/`, their runner logs, and the reused `analyze.py`. Intermediate captures (35.308, 31.303 and 29.240 before comparator consolidation) preceded the final source and are not the final result. The interrupted 20-cell preliminary A/B is under ab-before-comparator/ and is excluded from the final table.

## Throughput A/B

Framework only, 1024 bytes, WARMUP_SECONDS=30, DURATION_SECONDS=5, RUNS=1 per cell. Four rounds; each round runs modified then reference for request-backpressure, request-serial and send-saturation. Each individual cell holds the exclusive measurement lock. The C++ condition-B pin patch fixes source to CPUs 0–7 and target to 8–15 on both trees. The temporary patch was removed from this worktree before commit. No payload/wire optimization from the separate job is included.

| Pattern | Round | Reference msg/s | Modified msg/s | Change |
|---|---:|---:|---:|---:|
| request-backpressure | 1 | 41,967.50 | 44,833.78 | +6.83% |
| request-backpressure | 2 | 42,609.14 | 45,396.07 | +6.54% |
| request-backpressure | 3 | 42,569.17 | 46,257.14 | +8.66% |
| request-backpressure | 4 | 42,116.18 | 46,025.72 | +9.28% |
| request-backpressure | **Median** | **42,342.67** | **45,710.89** | **+7.95%** |
| request-serial | 1 | 7,867.78 | 7,352.32 | -6.55% |
| request-serial | 2 | 8,322.12 | 7,820.51 | -6.03% |
| request-serial | 3 | 7,626.11 | 7,854.98 | +3.00% |
| request-serial | 4 | 7,780.11 | 7,889.91 | +1.41% |
| request-serial | **Median** | **7,823.95** | **7,837.74** | **+0.18%** |
| send-saturation | 1 | 181,345.27 | 182,644.06 | +0.72% |
| send-saturation | 2 | 177,347.06 | 182,687.34 | +3.01% |
| send-saturation | 3 | 175,472.70 | 184,892.08 | +5.37% |
| send-saturation | 4 | 168,044.26 | 182,386.20 | +8.53% |
| send-saturation | **Median** | **176,409.88** | **182,665.70** | **+3.55%** |

All 24 throughput cells passed result validation: zero errors, header failures, rejected messages, abandoned messages and completed/final-received mismatches after drain; no contamination or drain-bound hit. Per-round inputs, source/target logs and results are under `ab/`; command order is in `ab.log` and `ab.sh`.

## Build, tests and formatting

- Release benchmark built initially with `framework/bench/grpc/cpp/build.sh` inside cpp-build-slot.sh. The final benchmark, copy probe and every default C++ target compiled with `--parallel 3 -- -k 0` under the required build wrapper and shared measurement lock. The final full target build includes the C++ cross-language host. Six affected test executable targets, including the largest files, were built in sequence, each using --parallel 3 and -k 0, followed by the all-target build, to preserve memory headroom. See `full-build-attempt-1.log` and the successful `final-recompile.log`. The first full build collected all initializer-list/span caller errors with -k 0; all were fixed and rebuilt.
- Unit configuration uses creation probes ON, installation OFF, cross-language host ON, and the already installed reference vcpkg/Core/binding packages. Benchmark uses creation probes OFF. No Core/binding source was changed.
- Six focused CTest targets passed through measure-shared.sh; see `focused-final.log` and `focused.xml`.
- All 296 tests selected by unit|contract labels passed through measure-shared.sh, including all 287 framework-unit|framework-contract tests; the selected names match the pre-change list. See `unit-contract-final.log` and `unit-contract.xml`.
- Added regressions cover shared receiver copy allocation/lifetime, separate public failure and late physical decoding, completion identity, terminal registration order during reentry, coallocated source lifetime, callback capture destruction before retained scheduler storage, raw inline callback copy/move/throw/empty semantics, borrowed target lifetime, and serializer replacement during encoding. Four allocation-reference primitives were updated from removed owning copies to borrowed bytes/media and the target held in the projection. All equality and behavior assertions remain unchanged; the references now enforce the lower allocation count.
- The Debug send-allocation target also passed, including its GNU Debug-only shared-state stack classification check, which is conditionally skipped in Release. The Release send reference now borrows the routing id; both count and bytes equalities remain unchanged. See `send-fix.log`, `send-debug.log` and `send-fix-build.log`.
- The standalone before/after receiver-copy probe measured 1 → 0 heap calls. Its deliberately allocating vector-copy control measured 1 → 1. See `copy-probe-final.log` and `probe/`.
- `bash scripts/format/format.sh --check cpp` passed from the worktree root; `git diff --check` passed. See `format-final.log`.

## Contract ownership and review

- Owner: Framework task/receiver lifecycle, codecs and operation registry. Core/binding transport policy remains unchanged.
- Spec: `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:38` (§2), `:329` (§9), `:375` (§10) and `:490` (§11); `02-handler-turn-and-execution-gate.ko.md` owns execution permission. Public and physical reply states remain distinct; first completion, physical conversion, identity, ordering and capture destruction boundaries are retained.
- Cross-language comparison: .NET ZLinkRequestCompletion/ZLinkMeshCompletionTable, Java ZLinkJavaRawServicePort/ZLinkServiceOperationRegistry, and Node request-runtime/operation-registry retain the same single completion claim ownership and late physical conversion semantics. This is source comparison; only C++ tests were run. No behavior/spec gap was found that requires a multi-language runtime change.
- Classification: A — representation adaptation to the existing contract. No spec change.

Audited decision locations: **10 before → 9 after; 0 added**. Eight contract locations remain eight; the duplicate byte-order comparator owners decrease 2 → 1. The contract locations are task first-result selection, public admission-failure publication, physical-to-public projection, registry terminal claim, raw source admission eligibility, node-role classification, normal wait claim and owner-cancellation wait claim. No new execution-permission flag, timer, retry, queue, lock, pool policy or fallback path was added. The optional callback/serializer storage controls object lifetime, not execution permission. The fixed raw callback has compile-time size/alignment/copy/move checks and no heap fallback. The per-node outer wire vector replaces per-request outer-vector allocation, and terminal-list nodes replace the old callback vector plus boxes.

Refactoring review covered every changed module and its request call path for performance, POSDDD ownership and unused code. Performance review found and fixed 13 representation issues: nested callable cloning; separately owned request, admission source/state and target/projection; unused default media; copied selected media; separate completion registration/callback; boxed terminal observers; header growth; topology and host RID vectors; per-call outer wire vector; and raw callback boxing. POSDDD/helper ownership findings: 1 duplicate byte comparator, consolidated into the existing comparator; unused plumbing/include findings: 2, both removed. No duplicate policy owner or execution gate bypass was introduced. Removed unused private request_type plumbing and the unused task.hpp vector include. No finding in the touched path was deferred. Every changed diff line was read before commit.

## Execution constraint violation and unfinished work

The memory-floor requirement was **not maintained throughout**. During overlapping early builds, MemAvailable was observed at 10.568 GiB and then **8.551 GiB**, below the required 12 GiB. Two own compiler processes were terminated and the own Ninja scheduler paused. Memory recovered; both interrupted translation units subsequently compiled successfully with -k 0. Later builds reserved both C++ build slots while retaining the shared measurement lock; a later large compile was briefly paused/resumed proactively without another observed violation. See `memory-incident.txt`, `final-build-memory.tsv`, `final-rebuild-memory.tsv` and `send-rebuild-memory.tsv`. This historical constraint violation cannot be undone.

No remaining implementation, test, formatting, census or requested A/B work. No push was performed.

Changed files: request/task and channel/call contract headers; raw route adapter; channel runtime; mesh owner/topology/runtime; client/envelope codec plumbing; node header encoder; public host request path; three C++ request unit-test files; four existing tests mechanically adapted to span arguments; and the send-allocation reference. The report and measurement artifacts remain outside the repository.
