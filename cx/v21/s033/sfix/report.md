# C++ request submit fixes — issue #3867

Complete: both fixes are locally committed on `perf/3867-submit-alloc`, rebased onto PR #3884. No push. Submitter heap calls/message are **49.341 → 24.283**; all **287 unit/contract tests** and **24 throughput cells** passed.

## Changes and ownership

The branch `perf/3867-submit-alloc` was rebased onto `origin/perf/3867-wire-writer` (`9dce44c43c`, PR #3884, including its bounds fix and main merge). Request wire parts are again a request-local `std::vector<zlink::message_t>`. The node member, clearing guard and associated comment are removed. Each nested request owns its list independently; correctness no longer depends on synchronous request callbacks remaining non-reentrant.

The request receive-source function, setter and node-installed callback borrow `std::span<const std::uint8_t>`. Its call no longer constructs `raw_bytes_t`. The raw-byte and native-message callback overloads borrow the target, consume parts by rvalue reference, and use `raw_request_callback_t`; both delegate to one templated `request_impl`. No runtime flags, counters, modes, fallback paths or shared mutable buffers were introduced.

The alternatives were to prove every synchronous header/registry/port/completion entry non-reentrant or restore local ownership; local ownership removes that invariant. For the receive source, retaining an owning temporary would preserve a needless per-request allocation on the empty-admission branch; borrowing matches the synchronous lookup and existing topology span lookup.

Owner: Framework request representation and codec storage; binding/Core retain physical submission and completion decisions.
Spec: `01-execution/01-submit-and-completion.ko.md` §2 and §9–11; `01-execution/02-handler-turn-and-execution-gate.ko.md`; design principles “제어 결정과 요청 수락의 소유”.
Cross-language comparison: .NET `Runtime/Service/ZLinkRawRouterServicePort.cs:136–165` creates messages per request and submits through the binding; Java `runtime/binding/ZLinkJavaRawServicePort.java:436–458` chooses an existing admission or synchronous receive-source lookup and submits once (Kotlin uses this Java runtime); Node `runtime/backend/node/node-raw-binding-port.ts:335–372` submits the per-call parts and observes the binding reply. Their physical policies are unchanged; these C++ fixes change storage ownership only.
Classification: A — representation adaptation to the existing contract; no behavior/spec gap or public API change. The user explicitly authorized both fixes and the rebase.

Control decision locations: **9 → 9; 0 added**, using the preceding allocation report's counting scope. Request-list storage construction/cleanup locations: **2 → 1**. Removed persistent state: `_request_wire`; no replacement persistent state. Refactoring review covered the changed request path for performance, POSDDD ownership and unused code: two requested findings fixed (shared list ownership and owning callback input); no deferred finding.

## Conflict list and preservation review

Git reported three conflicted files, containing five conflict regions:

1. `mesh/raw_mesh_node_owner.cpp`, `start_admitted_request`: retained W's `Parts` template so raw and native lists share admission logic; retained this branch's target span and inline `raw_request_callback_t`.
2. The same file, `request_with_header` signature: retained the span target and W's `std::function<zlink::message_t(std::uint64_t)>` header result.
3. The same file, wire list: during rebase retained the allocation commit's guard/member with W's native list type; the separate fix commit restores the request-local native list and removes the guard/member.
4. `mesh/raw_mesh_node_owner.hpp`, `request_with_header` declaration: same combined span/native-header signature as the definition; member was adapted to native parts during rebase and then removed by Fix 1.
5. `protocol/service_wire_codec.cpp`, `encode_node_request_header_into`: retained W's `allocate_wire_buffer<Buffer>(prefix_size + 8)` and common vector/native writer. It supersedes the original vector-only reserve optimization. No second vector encoder was added.

`backend/raw_route_port.cpp/.hpp` auto-merged without conflict markers, but required manual semantic integration: applied span, rvalue parts and inline callback to **both** overloads and their shared template. Keeping only Git's automatic result would have retained owning/boxed parameters in W's native path. `mesh_node_runtime.cpp`, `stateful/public_host_runtime.cpp` and `test_cpp_framework_actor_late_result.cpp` also auto-merged and were reviewed. The latter keeps W's native Actor header and this branch's array-to-span input.

`rebase-conflicts.diff` preserves every Git conflict. `range-diff.txt` compares the original allocation commit with its rebased form: the differences are the native-path integration and superseded vector-header reserve. Other allocation-cut changes are retained.

## Fix 2 finding

The benchmark Framework driver configures a real mesh and calls `route_client_t::request_to_node` (`framework/bench/grpc/cpp/client/bench_cpp_client.cpp:805–826,883`). The runtime reaches `public_host_runtime_t::request_to_node`, then `raw_mesh_node_owner_t::request_to_node` → `request_with_header` → `start_admitted_request`.

The **public** call begins without a liveness admission parameter, but the node's ready-peer submission explicitly supplies `peer->liveness` to the port. The node constructs its topology with `&_liveness` (`raw_mesh_node_owner.cpp:459–463`); peer admission constructs/reuses that connection (`service_topology_registry.cpp:417–427`). Readiness-wait submissions use the same `start_admitted_request`. Thus the normal ready-peer benchmark path supplies nonempty admission and skips the receive-source branch. This is a source-path finding, not an instrumented branch-frequency claim. The optional empty-admission port API still had the real heap copy, so it was removed conservatively as requested. No benchmark-only policy was added.

A contract regression explicitly submits empty admission with both raw and native part lists and checks that the source callback sees the original routing-id pointer and bytes.

## Build and verification

The previous unit build directory `/root/jobs/fx/cx/v21/s033/salloc/build` matches this worktree and was reused: Release, test creation probes ON, framework installation OFF, C++ cross-language host ON. The installed Core and C++ binding are both 1.23.0 from the reference package prefix; neither source nor packages were changed. Benchmark test probes remain OFF.

All default C++ targets compiled with `--parallel 3 -- -k 0` through `/root/jobs/fx/cx/cpp-build-slot.sh`; six large test targets were built sequentially before the remaining all-target build. Final-source restoration after the negative control also rebuilt/relinked every affected target with the same wrapper/options. Logs: `build-unit.log`, `borrow-restored-build.log`.

The focused raw-route contract passed with the final borrowed callback (`borrow-positive.log`, 6.48 seconds). Deliberately restoring the old `raw_bytes_t(begin,end)` allocation, with no assertion changes, made it fail at `observed.data () == target.data ()` (`borrow-negative.log`). This expected failure verifies that the regression detects an owning copy. The original source was restored immediately, then all affected targets were rebuilt before the final label run.

Final labels: **287/287 passed, 0 failed**, 100.86 seconds. Command: `/root/jobs/fx/cx/measure-shared.sh ctest --test-dir /root/jobs/fx/cx/v21/s033/salloc/build -L 'framework-unit|framework-contract' --parallel 3 --output-on-failure --output-junit /root/jobs/fx/cx/v21/s033/sfix/unit-contract.xml`. Evidence: `unit-contract.log`, `unit-contract.xml`. No assertion or fixture was relaxed. The only failing test run is the deliberately reintroduced routing-id copy described above.

## Allocation census

Fresh A = prebuilt main `ee1139025782688e33109455041d8a0222cc8fe3` in `/root/worktree/zlink-ratio040`; B = this worktree, stacked onto #3884 with both fixes. Framework only, request-backpressure, 1024 bytes. Same prior-job `salloc/census.sh`, `salloc/analyze.py`, `calloc/collect.py` and `alloc_trace.so`, with 10-second warmup and 20-second active phase. The inner capture lasts approximately eight seconds. Each process pair ran under `flock -o /root/jobs/fx/cx/measure.lock`; source CPUs 0–7 and target CPUs 8–15. The final census runner checked that Ninja had no pending benchmark work.

| Scope | Fresh main calls/message | Stacked fixes calls/message |
|---|---:|---:|
| Submitter | 49.341 | 24.283 |
| Whole source process | 68.594 | 41.956 |
| Whole target process | 52.921 | 41.994 |
| Source + target processes | 121.515 | 83.950 |

Submitter reduction versus fresh main: **50.79%**. The original `317d4037cc` report measured **29.262** submitter calls/message before stacking; the new stacked source measures **24.283**. That historical comparison includes W, these fixes and upstream changes, so it is not an isolated effect of Fix 1 or Fix 2. The ≤30 submitter goal remains satisfied with request-local ownership.

Capture completions: **325,197 before**, **378,861 after**. Submitter samples: **63,096 before**, **36,282 after**. The submitter is the source thread with the highest exact allocation delta, following the unchanged previous analyzer.

The request-with-header attribution is approximately **1.997 → 0.997 calls/message**: the final profile retains the request-local outer list allocation. The old vector node header is approximately **3.010 calls/message** in main; the final path uses W's native header, with no sampled allocation attributed to that old encoder. Raw request/materialization attribution moves into the shared native `request_impl`; it must not be interpreted as a new transport decision.

Exact per-thread/process malloc/calloc/realloc deltas use completed messages from the same capture window. Sampled sites use the unchanged 1/256 sampling and exact-total rescaling. Aligned allocation, direct mmap and libc-internal allocation entry points are outside this existing counter. A zero sampled site is not proof that every inlined callee allocates zero times. The separate borrowed-pointer negative control specifically verifies the receive-source copy removal.

Raw evidence: `before.json`, `after.json`, `census-before/`, `census-after/`, `census.log` and `census-{before,after}-analysis.log`. Both census cells have matching completed/final-received counts and zero errors, rejected/abandoned requests or header-validation failures.

## Benchmark build and measurement protocol

The benchmark was rebuilt in this worktree using `framework/bench/grpc/cpp/build.sh` inside `cpp-build-slot.sh`. The job-local shell wrapper enforces `cmake --build ... --parallel 3 -- -k 0`; configuration uses the existing reference vcpkg/Core/binding packages. All 81 pending benchmark build actions completed. Evidence: `build-bench.sh`, `build-bench.log`.

The throughput comparison uses A = existing main `ee11390257` binaries and B = the freshly built stacked source. Each of four rounds runs B then A for request-backpressure, request-serial and send-saturation, all at 1024 bytes, Framework only. Every cell uses `RUNS=1`, `WARMUP_SECONDS=30`, `DURATION_SECONDS=5` and its own `flock -o /root/jobs/fx/cx/measure.lock`. The supplied condition-B patch is applied only to this worktree's C++ `run_local.sh` during measurement. The reference C++ runner already has the required CPU pinning; its pre-existing changes, including runners for other languages, are untouched.

Throughput runs use the normal runner without the allocation interposer; the separate census uses the unchanged previous instrumentation. Medians are computed independently for A and B, and the median percentage is the ratio of those medians. Raw results/logs are under `ab/`; command order is in `ab.log` and `ab.sh`.

This is the requested whole-build comparison, not an isolated attribution to the two fixes: the reference main predates the required W/upstream base. Four rounds describe these runs and are not a confidence bound.


| Pattern | Round | Main msg/s | Stacked fixes msg/s | Change |
|---|---:|---:|---:|---:|
| request-backpressure | 1 | 42,449.60 | 48,839.84 | +15.05% |
| request-backpressure | 2 | 42,753.26 | 48,694.95 | +13.90% |
| request-backpressure | 3 | 41,678.89 | 49,034.17 | +17.65% |
| request-backpressure | 4 | 41,880.98 | 48,721.14 | +16.33% |
| request-backpressure | **Median** | **42,165.29** | **48,780.49** | **+15.69%** |
| request-serial | 1 | 7,713.91 | 8,047.31 | +4.32% |
| request-serial | 2 | 7,180.60 | 7,836.80 | +9.14% |
| request-serial | 3 | 7,308.98 | 7,897.53 | +8.05% |
| request-serial | 4 | 7,105.79 | 7,699.51 | +8.36% |
| request-serial | **Median** | **7,244.79** | **7,867.16** | **+8.59%** |
| send-saturation | 1 | 180,913.63 | 181,395.89 | +0.27% |
| send-saturation | 2 | 175,736.72 | 182,272.67 | +3.72% |
| send-saturation | 3 | 180,632.71 | 183,022.60 | +1.32% |
| send-saturation | 4 | 183,052.37 | 183,308.25 | +0.14% |
| send-saturation | **Median** | **180,773.17** | **182,647.64** | **+1.04%** |

All **24/24 cells** passed validation: zero errors, header failures, server rejections and abandoned requests; no contamination or drain-bound hit; submitted/completed/final-received counts agree. `summarize.py` also verifies Framework-only selection, 1024 bytes, 30-second warmup and 5-second active duration. Median throughput changes: **request-backpressure +15.69%**, **request-serial +8.59%**, **send-saturation +1.04%**.

Final formatting: **PASS**, `bash scripts/format/format.sh --check cpp` from the worktree root (`format-final.log`). `git diff --check` passed. The temporary C++ CPU-pinning patch was reversed before commit; it is absent from the final diff. `post-build.log` and the successful pipeline exit record completion of tests, benchmark build, measurements and formatting.

## Memory and execution constraints

Minimum sampled MemAvailable: **15.130 GiB** (15,865,376 KiB), sampled once per second across build/test/measurement work (`memory.tsv`). No memory-floor violation was observed. Each compile invocation checked for at least 14 GiB beforehand and waited if necessary; minimum recorded pre-compile value was **26.000 GiB** (`compile-memory.tsv`). Only one build slot was used at a time and compiler parallelism was three. During the largest benchmark compilation, the job-local governor paused the other two compiler processes until memory recovered (`governor.log`). No own builds overlapped; no measurement overlapped a build.

The temporary compiler governor was stopped before measurements. The memory watcher is now stopped, and measurement processes are cleaned up by the runners. No sub-agent, other model or AI tool was invoked. No push, CI invocation, branch switch outside the authorized rebase, or external message was performed.

## Changed files and final commits

Fix commit paths, relative to `framework/languages/cpp/`:

| File | Change |
|---|---|
| `framework/src/runtime/mesh/raw_mesh_node_owner.cpp` | Request-local native wire list; explicitly borrowed receive-source target; format the merged template signature. |
| `framework/src/runtime/mesh/raw_mesh_node_owner.hpp` | Remove node-owned wire member and obsolete clearing comment. |
| `framework/src/runtime/backend/raw_route_port.hpp` | Span input for receive-source function and setter. |
| `framework/src/runtime/backend/raw_route_port.cpp` | Call source directly with borrowed target. |
| `tests/Zlink.Framework.ContractTests/test_cpp_framework_raw_route_port_contract.cpp` | Verify original target pointer/bytes and synchronous missing-route completion for both part types with empty admission. |

The complete changed-file inventory for the two local commits is `final-files.txt`. The original allocation changes are preserved, with W's native writer superseding only the original vector-header reserve; see `range-diff.txt` and the conflict list above. No protected specification or common documentation file was changed.

- Upstream #3884 base: `9dce44c43c46d55408623754ad3139581370ca10`.
- Rebased allocation cut: `0b06476fb08d8a30bd6a37492f04144251fdb6ad`.
- Review fixes / final HEAD: `edeb16b166d51857849ef9bc9869265b6ecbaef3`.
- Reference main: `ee1139025782688e33109455041d8a0222cc8fe3`.

Final branch: `perf/3867-submit-alloc`. The upstream W tip is an ancestor of final HEAD. The worktree is clean, both commits are local, and nothing was pushed.

Unfinished work: **none**. Remaining unexpected build/test/format failures: **none**. The deliberately restored-copy assertion failure is an expected negative control, with the final source restored and the full label suite passing afterward.
