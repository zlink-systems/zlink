# C++ source submit path (#3867) — partial implementation

The complete job is **not finished**: Part A.1/A.2/A.4 and the native close-on-lane subitem require the scope/design decisions below. Changes remain uncommitted.

| Done criterion | Result | Evidence |
|---|---|---|
| Request 1K source submitter CPU decreases ≥30% | **NOT MET** | Main 19.292 → branch 18.326 CPU µs/completed message; reduction **5.01%** (three-run medians). |
| Both 1K Framework/binding throughput medians rise | **NOT MET**, ratio of throughput medians; **NOT MET**, median of paired run ratios | Both definitions and raw throughput medians are reported below. |
| Full C++ non-install tests and format | **PASS** | 309/309 CTest entries; C++ format and diff checks pass. |

Target release: 0.33.0, milestone #1639.

Baseline: `origin/main` and worktree HEAD `6f10186f979e74b75a4b982205857b94c078cdef`. Worktree: `/root/worktree/zlink-3867s`, branch `perf/3867-cpp-submit-path`. Changes remain uncommitted. No sub-agents, alternate models, GitHub operations or spec changes.

## Ownership diagnosis and authorized scope

The supplied Part A is the approved design. The proposed extracted-node representation was implemented and passed focused tests and Valgrind, then rejected in standards review: retained pre-extraction index references cannot be used to access an element while a node handle owns it. The final source retains the original independently allocated completion and C2 index storage. The shared completion sink, lane-path port cleanup, send storage and integer formatter are retained. No extra runtime reference counter, opaque node storage or queue remains.

| Decision | Spec owner | Baseline C++ locations | .NET / Java / Kotlin / Node comparison | Final owner |
|---|---|---|---|---|
| Terminal authority | Submit §10.1 C1 | `foundation/operation_registry.cpp:108–119,489–501` | .NET `Runtime/Service/ZLinkManagedMeshNode.cs:12554–12582` compares identity then removes; Java `runtime/internal/service/ZLinkServiceOperationRegistry.java:508–516` removes identity; Kotlin shares Java; Node `runtime/foundation/operation-registry.ts:434–442` removes from entries | Existing registry completion slot, taken at notification |
| Deadline index and cursor | Submit §10.1 C2 | `foundation/operation_registry.cpp:437–485,710–763` | Managed implementations retain independent index references or visit the same index on their owner; no public contract change | Existing operation-owner lane |
| Admission ends before reply on failure | Submit §2/§9 | `mesh/raw_mesh_node_owner.cpp:224–246`; public state in `contracts/dispatch/request_task.hpp:378–387` | Language-specific result storage; externally the admitted/reply phases stay separate | Pending completion sink retains admitted source; public phase remains public request state |
| Port during synchronous submit | State ownership §4 / Hot path E3–E4 | `mesh/raw_mesh_node_owner.cpp:1552–1556,1648–1656` | Resource representation differs; no transport admission policy changes | Raw mesh lane, with socket operation gate retained in the port |
| Typed decode and physical lifetime | Submit §9 | `channels/channel_runtime.cpp:1774–1915`, `mesh/mesh_node_runtime.cpp:4399–4439`, `mesh/raw_mesh_node_owner.cpp:1393`, `backend/raw_route_port.cpp:359` | Public physical/admission separation is retained in all languages | Full Part A conversion collapse remains blocked below |

Classification: **A**, representation/ownership adaptation to existing Submit §9/§10.1, Hot path §3 and State ownership §4; **B**, #3888 borrowed-source address-stability hardening demonstrated by the negative control, with the scoped limitations below. C++ resource storage changes do not alter public wire bytes, ordering, admission results or terminal classifications. No other language is modified under the explicit C++ scope.

## Scope blockers

- Part A.1 asks to remove the binding observer coroutine. The deployed binding's `async_result_t` exposes only move-only coroutine await: `bindings/cpp/include/zlink/Contracts/Messaging/operation_contracts.hpp:60–156`. A non-coroutine completion callback is not in that public interface. Accessing its private `detail` state would violate `doc/principal/dev/zlink-system-design-principles.ko.md:63–81` (Framework consumes deployed public binding contracts). Binding edits are outside the authorized write scope. The current adapter is retained; a callback API is not emulated.
- Part A.2 asks the selected serializer to size and write directly into the final service frame. `serializer_t<T>` only exposes `serialize(const T&) -> encoded_payload_t` at `framework/include/zlink/framework/contracts/codecs/serializer.hpp:216–245`. The deployed protobuf extension allocates its own message and calls `SerializeToArray` at `extensions/framework-codec-protobuf/include/zlink/codecs/protobuf.hpp:25–36`, outside the authorized write scope. Detecting protobuf in the route caller and bypassing the selected serializer would violate codec ownership and custom serializer selection. The direct serializer write is stopped; the UTC formatter subitem is implementable independently.

- Part A.4 is also stopped. The candidate's C1 take at `rejected-extract-operation_registry.cpp:247` transfers map ownership while the C2 links/cursor established before extraction continue to point into that element. The [C++ draft, unordered-container requirement 244](https://eel.is/c++draft/unord.req.general#244) does not permit access through pre-extraction element references while the node handle owns the element. Rebinding the deadline links and cursor at notification would change C2 state outside its owner lane, contrary to Submit §10.1. No exception for these retained references is assumed. The proposed code and diff are retained only as `rejected-extract-operation_registry.cpp` / `rejected-extract.patch`; that implementation is removed from the worktree. A separately allocated index/receiver would change the approved one-node design, so it is not substituted. The normal original pending/index implementation remains. The candidate's Valgrind pass is not used as a standards-conformance verdict.

- Part A.3 native close-on-lane subitem is stopped. The full suite rejected it at `tests/Zlink.Framework.UnitTests/test_cpp_framework_close_failure.cpp:805` (`mesh-close-lane`, `assert(!closed_on_lane)`); the hook at lines 86–91 observes `zlink_close` under a state lane. `raw_route_port_t::close` acquires the poller/socket protocol gates. State ownership §3 and §4 forbid waiting for an external operation/gate while holding the state lane and permit closing a resource after exclusive ownership is moved out. The original detach-on-lane / native-close-outside / settle-on-lane implementation is restored. The assertion is unchanged; no extra close lane turn or disposal flag is added. Failure evidence: `rejected-close-on-lane-test-all.log` / XML. The lane-path `_port` read-lock removals are independent and retained.

Question sent to the user about whether to retain the current scope or authorize binding and protobuf extension work. No response is assumed.

## Part A implementation status

1. **Partial.** The registry now stores `completion_sink_t`, which reuses the existing shared callable representation. Admission failure and the terminal receiver share that value; the allocating `request_admission_completion` wrapper is removed. The other channel/mesh projections and binding coroutine remain. This is not a claim that the final typed sink collapse is complete.
2. **Partial.** UTC deadlines use integer calendar fields and fixed-width decimal writes. The regression compares the original `gmtime`/`strftime` bytes at month/day boundaries, leap centuries and negative epochs in the platform's supported range. Existing wire golden tests are unchanged. Direct serialization into the final frame is blocked as stated above.
3. **Implemented for existing owner-lane paths.** Request submission and the four reply/relocation lane snapshots no longer take `_lifecycle_mutex` to read `_port`; the send-backpressure sequence lock is retained because it also protects the reply-route sequence used by mutex-only control paths. The attempted native-close move is withdrawn after the close-lane contract failure above; original native disposal remains outside the state lane. Non-lane receive/wait snapshots retain their existing synchronization; no extra receive-lane round trip or cached port state is introduced.
4. **Stopped and withdrawn.** The original completion allocation, nullable completion slot, multimap/index ownership and observation count remain. The extract candidate conflicts with the retained-reference/C2 requirements above. No replacement index state, reference counter or opaque node storage remains in the final runtime.
5. **Implemented for the actual send state.** The single-use flag is inline in the callable's shared owner; node payload storage also moves into that owner. Copies retain the same one-shot authority without a separate `make_shared<submit_once_t>`. The current `channel_request_call_t` has no equivalent shared `submit_once_t` to remove; no new single-use request restriction was added.

#3888 follow-ups: the observer asserts that its callback is still present before delivery; observer and combined-source storage cannot move. Moving into or out of the combined source's borrowed wrapper terminates, including after observer registration. Ordinary owning sources retain their published move contract. The observer regression invokes the same scheduled delivery twice and observes exactly one callback.

## Decision locations (Fable §3.6 scope)

Counts identify independent decision sites or explicitly named protection mechanisms, not allocations or method calls. The five removed lock sites are counted separately from the two competing protection mechanisms. Rows overlap and are not summed. In particular the planned whole-design `9 → 6` is not asserted for this partial Part A implementation.

| Decision | Before | After | Result |
|---|---:|---:|---|
| Physical reply → typed result conversion | 4 | 4 | Raw port, raw owner, mesh runtime and channel decoder remain; incomplete Part A.1. |
| Redundant lifecycle-mutex `_port` read sites within an existing lane | 5 | 0 | One request submit and four reply/relocation snapshots; the existing lane remains the owner. |
| `_port` protection mechanisms within an already-owned submit turn | 2 | 1 | The lane is sufficient; request no longer adds the lifecycle mutex. The send fast path already used the lane alone; its separate pending-sequence guard remains unchanged. |
| Dispatcher worker wake rule | 1 | 1 | Existing per-item Asio posting; Part B unchanged. |
| Polls per receive rotation | 2 | 2 | Receive rotation unchanged. |
| Reply port acquisition protection | 2 | 1 | Existing lane snapshot, with redundant lifecycle lock removed. No Part C batch-held reference claim. |
| C1 terminal authority | 1 | 1 | Original nullable completion slot and one atomic take are retained. |
| Admission failure adaptation before terminal receiver | 1 | 1 | Allocating wrapper → by-value sink. |
| Send single-use decision | 1 | 1 | Separate shared flag → inline flag in callable owner. |

수정 전/후 규칙 수: typed 변환 위치 4→4, lane 내부의 중복 port mutex 판정 위치 5→0, submit turn의 port 보호 수단 2→1, completion wake 1→1, rotation poll 2→2, reply port 보호 2→1, terminal 권한 1→1, admission 실패 전달 1→1, send 단일 제출 1→1.

## Verification

Job-script commands below use `/root/jobs/fx/cx/v21/s033/csubmit/` as their working directory. Repository commands run in `/root/worktree/zlink-3867s`. Production build/configure commands and their output are retained in `configure-main.sh`, `configure-branch.sh`, `configure-tests.sh`, their `.log` files and `build-branch-final.log`. The #3884 (`910d1fc41f`) and #3888 (`3f83745bed`) diffs were reviewed before implementation.

| Check | Command | Final result and logs |
|---|---|---|
| Relevant regression suite | `bash verify-focused.sh` | 15/15 passed in 6.49 s after withdrawing extract: `test-focused.log` / XML. |
| Restored sequence guard | Build relevant targets through build-slot wrapper; shared-lock CTest regex in `test-sequence-guard.log` | 5/5 passed in 6.54 s: `build-sequence-guard.log`, `test-sequence-guard.log`. |
| Registry callback/index lifetime | `flock -s /root/jobs/fx/cx/measure.lock valgrind test-build/test_cpp_framework_operation_registry` | 37,204 allocations / 37,204 frees; zero bytes at exit; `ERROR SUMMARY: 0`: `registry-valgrind.log`, `registry-valgrind.stdout.log`. Registry code is unchanged since this check. |
| Registered borrowed-source move, negative control | Identical `bench/source-move.cpp`, built through build-slot; execute `main-build/csubmit_completion_source_move` and `build/csubmit_completion_source_move` under shared lock with core dumps disabled | Main accepts invalid move (exit 0); branch rejects it (SIGABRT, -6). `source-move-negative.json`, `source-move-main.log`, `source-move-branch.log`. Normal completion and duplicate delivery pass in the positive suite. |
| Full C++ non-install tests | `bash verify-all.sh` | **309/309 passed, 0 failed, 0 skipped**, 107.56 s after sequence-guard restoration. `build-all.log`, `test-all.log`, `test-all.xml`. The two install-consumer entries are excluded. |
| Format | `bash scripts/format/format.sh --check cpp` via `verify-all.sh` | `format.log`: `== cpp`, exit 0. |
| Whitespace | `git diff --check` | Exit 0. |

All builds use `/root/jobs/fx/cx/cpp-build-slot.sh cmake --build … --parallel 3` under shared `measure.lock`. The installed wrapper does not parse a leading `--parallel 3`, so the option is passed to CMake. Compiler-invoking tests use copied `cxx-slot.sh` / `cmake-slot.sh` through `instrument-test-commands.py`. The unchanged engine .NET fixture runs from the frozen main copy inside this job directory; generated bin/obj/run files stay outside the worktree and fixture assertions are unchanged.

The requested `/root/jobs/fx/cx/pre-pr.sh` does not exist. The full non-install CTest run and repository C++ format check above replace that absent entry point. Test/build scripts and all logs live in this job directory.

The rejected extract candidate's focused/Valgrind logs remain under `extract-candidate-*`. Its runtime success does not establish standards conformance. Native close-on-lane failed `mesh-close-lane` in the initial full run; `rejected-close-on-lane-test-all.log` / XML preserve that failure. The code was restored to the original ownership boundary, then passed both its focused check and the complete final suite without changing assertions.

## Changed files and purpose

Paths below are relative to `framework/languages/cpp/`.

| File | Purpose |
|---|---|
| `framework/include/zlink/framework/contracts/channels/channel.hpp` | Coallocate send callable, one-shot flag and node payload; keep copy identity. |
| `framework/include/zlink/framework/contracts/dispatch/request_task.hpp` | Reuse the existing shared request callback for registry sinks; preserve empty-callable validation. |
| `framework/include/zlink/framework/contracts/dispatch/task.hpp` | Assert observer single delivery; keep combined source/observer addresses stable and reject borrowed-source moves. |
| `framework/src/runtime/channels/channel_runtime.cpp` | Invoke the shared send callable and its inline one-shot authority. |
| `framework/src/runtime/foundation/operation_registry.hpp` | Share terminal callback storage in `completion_sink_t` and carry admission without a second callable allocation. |
| `framework/src/runtime/foundation/operation_registry.cpp` | Use the shared completion sink and its admission phase; retain the original pending/index storage after rejecting the extract candidate. |
| `framework/src/runtime/mesh/raw_mesh_node_owner.cpp` | Remove admission wrapper and redundant lane-path lifecycle locks; preserve the original native-close ownership boundary. |
| `framework/src/runtime/messaging/client_call_codec.hpp` | Declare the internal UTC formatter for codec use and regression coverage; remove unused stream header. |
| `framework/src/runtime/messaging/client_call_codec.cpp` | Replace `gmtime`/`strftime` with integer UTC formatting with identical valid deadline bytes. |
| `tests/Zlink.Framework.UnitTests/test_cpp_framework_envelope_allocations.cpp` | Compare original and integer deadline formats at calendar boundaries. |
| `tests/Zlink.Framework.UnitTests/test_cpp_framework_operation_registry.cpp` | Exercise callback lifetime through index cleanup, a blocked callback and ID reuse. |
| `tests/Zlink.Framework.UnitTests/test_cpp_framework_request_transport_projection.cpp` | Exercise duplicate scheduled delivery and forbidden registered combined-source moves. |
| `tests/Zlink.Framework.UnitTests/test_cpp_framework_send_allocations.cpp` | Verify copied calls share one submission claim and introduce no callable-copy allocation. |

리팩토링 점검: 성능 6건, POSDDD 3건, 불필요 코드 1건 발견. 고친 항목: send 저장 allocation, admission wrapper, UTC formatter, lane의 중복 port 보호, combined source·observer 주소 안정성, 쓰이지 않는 stream include. 넘긴 항목: binding callback 공개 표면, protobuf 최종 frame writer, retained index reference와 양립하는 registry node 설계가 필요한 세 성능 항목(쓰기 범위·계약·설계 결정 대기). Reply-route sequence의 lane/기존 mutex 혼합 보호는 별도 진단이 필요하며, 이번 변경에서는 기존 보호를 유지한다.

Owner layer: Framework submit-call storage, raw mesh owner lane and operation registry; binding/Core admission and retry remain their owners.

Spec authority: Hot path §3 E1–E4, Submit/completion §9/§10.1 and State ownership §4; blocked items are explicitly incomplete rather than bypassing these clauses.

Cross-language result: the read-only comparison above preserves observable admitted/reply/terminal and wire behavior. Kotlin shares Java. No cross-language runtime behavior change or new public codec/binding contract is implemented here.

Change classification: A for representation/ownership adaptations; B for #3888 observer/source stability hardening. Incomplete items remain scope blockers; no C/D workaround is introduced.

The send-backpressure sequence guard is identical to baseline. `raw_mesh_node_owner_t::request_relocation_prepare` allocates reply-route IDs in a mutex-only path, and `raw_mesh_node_owner.hpp:703` stores the plain counter. Its mixed lane/mutex ownership needs diagnosis outside this port-read change; the guard is preserved here. Earlier measurement/census data and full-test results are retained under `before-sequence-guard-*` for audit. They are superseded by the final-code full tests above and the fresh measurements below. No counter ownership redesign is inserted into this job.

Source snapshot: `changes.patch` contains the uncommitted diff; `source-hashes.json` records its 13 source/test file hashes. `final-verification.json` records the unchanged package hashes and final JUnit counts.

## Remaining problems

- Request-backpressure 1K submitter CPU reduction is 5.01%, below the required 30%.
- Send-saturation 1K Framework throughput median and Framework/binding ratio decreased in the final three-run comparison. No causal attribution for that regression is established by these measurements; it is not treated as a passed performance check.
- The final typed sink collapse, direct serializer-to-frame write, extracted registry node and native close-on-lane subitem remain incomplete for the concrete scope/ownership conflicts above.
- The original reply-route counter's mixed lane/mutex ownership is retained, not redesigned in a port-read optimization. Relevant declarations/use: `framework/src/runtime/mesh/raw_mesh_node_owner.hpp:703`, `framework/src/runtime/mesh/raw_mesh_node_owner.cpp:1880` (mutex-only relocation prepare) and `:4276–4284` (counter decision).

## Measurements

Condition B: source CPUs 0–7, target 8–15. Frozen main and branch use the same Core/binding 1.23.0 packages, Release `-O3 -DNDEBUG`, probes off and identical runner options. Each cell has 30 s warm-up and 5 s active time; three runs per variant and pattern. Binding and Framework run back-to-back; main and branch alternate. All cells hold exclusive `flock -o /root/jobs/fx/cx/measure.lock`; builds/tests hold it shared. `measure.sh`, `run-B.sh`, `runner-common.sh` and `collect.py` are copies of the provided cdrain harness, relocated into this job.

The submitter TID is recorded once at its first Framework submit. `/proc/<pid>/task/<tid>/schedstat` is sampled in the same approximately three-second active window as the completed-message counters. CPU figures cover the complete source driver/submitter thread, including payload creation and completion reaping, as in the original profile. The primary denominator is the source completed-message delta; the target-received denominator is retained independently in `summary.json`. No per-message logging is added. All 24 cells have zero errors/abandoned/rejected messages and matching final completed/received counts.

| Pattern | Variant | Binding median msg/s | Framework median msg/s | Ratio of medians | Median paired ratio | Submitter CPU µs/completed |
|---|---|---:|---:|---:|---:|---:|
| request-backpressure 1K | main | 252,320.3 | 48,321.1 | 0.1915 | 0.1914 | 19.292 |
| request-backpressure 1K | branch | 246,826.1 | 49,602.0 | 0.2010 | 0.2032 | 18.326 |
| send-saturation 1K | main | 609,125.1 | 176,003.6 | 0.2889 | 0.2814 | 4.836 |
| send-saturation 1K | branch | 616,974.0 | 173,204.0 | 0.2807 | 0.2807 | 4.883 |

| Pattern | Run | Variant | Binding msg/s | Framework msg/s | Framework/binding | Submitter TID | CPU µs/completed |
|---|---:|---|---:|---:|---:|---|---:|
| request-backpressure | 1 | main | 246,120.1 | 48,321.1 | 0.1963 | 160465 | 19.259 |
| request-backpressure | 1 | branch | 244,087.1 | 49,602.0 | 0.2032 | 164092 | 18.253 |
| request-backpressure | 2 | main | 255,019.3 | 48,655.7 | 0.1908 | 175750 | 19.292 |
| request-backpressure | 2 | branch | 249,296.7 | 49,338.9 | 0.1979 | 181255 | 18.326 |
| request-backpressure | 3 | main | 252,320.3 | 48,290.9 | 0.1914 | 199764 | 19.551 |
| request-backpressure | 3 | branch | 246,826.1 | 51,291.1 | 0.2078 | 210257 | 18.333 |
| send-saturation | 1 | main | 609,125.1 | 169,574.3 | 0.2784 | 167971 | 4.836 |
| send-saturation | 1 | branch | 609,160.2 | 174,110.0 | 0.2858 | 171858 | 4.883 |
| send-saturation | 2 | main | 586,480.6 | 177,986.5 | 0.3035 | 186856 | 4.791 |
| send-saturation | 2 | branch | 616,974.0 | 173,204.0 | 0.2807 | 192387 | 4.897 |
| send-saturation | 3 | main | 625,502.5 | 176,003.6 | 0.2814 | 216223 | 4.856 |
| send-saturation | 3 | branch | 626,503.2 | 172,930.9 | 0.2760 | 223220 | 4.841 |

Commands and artifacts: `bash measure.sh`, `python3 analyze.py`; `measure.log`, `measurement-summary.log`, `summary.json`, and `measure/<round>/<pattern>/<variant>/run1/<implementation>-<pattern>-1024/{results.json,source.log,target.log,capture/}`. Raw before/after/samples include the per-thread counters and source/target completed counts. Baseline/package hashes are in `versions.json`; final measured targets have no pending Ninja work (`bench-main-noop.log`, `bench-branch-noop.log`).

## Allocation census

The provided `salloc/census.sh`, its collector and `alloc_trace.so` still run. The relocated `run-census.sh` uses condition B, 10 s warm-up and 20 s active time with an eight-second capture, one run per variant. These instrumented throughput values are excluded from the three-run throughput results above. Heap calls count malloc/calloc/realloc; aligned allocation and direct mmap remain outside the provided counter.

| Variant | Submitter TID | Completed in capture | Submitter heap calls/message | Whole-source heap calls/message |
|---|---|---:|---:|---:|
| main | 226643 | 362,926 | 24.280 | 41.555 |
| branch | 231967 | 379,332 | 23.254 | 40.099 |

**NOT MET:** the Part A allocation target is ≤12 heap calls/message; branch measures 23.254. Raw evidence: `census.log`, `census-summary.json`, `census/{main,branch}/run1/zlink-framework-cpp-request-backpressure-1024/capture/` and the corresponding allocator trace/map files. Command: `bash run-census.sh` under exclusive measurement locks.
