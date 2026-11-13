import json,pathlib,subprocess,statistics
j=pathlib.Path('/root/jobs/fx/cx/v21/s033/wire');tree='/root/worktree/zlink-3867w'
sha=subprocess.check_output(['git','-C',tree,'rev-parse','HEAD'],text=True).strip()
summary=json.loads((j/'measure-summary.json').read_text());old=json.loads((j/'probe-main.json').read_text());new=json.loads((j/'probe-W.json').read_text())
minimum=json.loads((j/'memory-total-min.json').read_text())['minimum_kb']
rounds=summary['rounds'];med=summary['medians']
report=f'''# Candidate W — final native service wire, issue #3867

Commit: **`{sha}`**, branch `perf/3867-wire-writer`; committed locally, **not pushed**. Base: `3fb411b4abf520d71d23bdd8e522e6af5643f0b3`. Worktree: `{tree}`. No sub-agent, other model or AI tool was used.

The common application request, reply and send paths write the service header and aggregated application envelope directly into their final native messages. Moved multipart storage is owned inline, and the dispatcher passes reply ownership through `take_items()`. Wire bytes, routing, admission, completion and public serializer paths are preserved.

## Changed files

Paths below are under `framework/languages/cpp/`.

| Files | Purpose |
|---|---|
| `framework/src/runtime/protocol/service_wire_codec.{{hpp,cpp}}` | One sizing/validation/encoding implementation per command supports vector and native output. Native output covers node/channel/Spot/Actor request/send, bound Session send, generic reply and application payload. Multipart values replace the shared box; explicit copies retain binding storage through `message_t::copy()`. |
| `framework/src/runtime/backend/raw_binding_adapter.hpp`, `framework/src/runtime/backend/raw_route_port.{{hpp,cpp}}` | Already materialized native messages move into the existing binding submit implementation. Raw and native overloads share one physical admission/completion implementation. |
| `framework/src/runtime/mesh/raw_mesh_node_owner.{{hpp,cpp}}` | Carry final native messages through the existing request, readiness-wait, send and reply paths. Adopt already encoded owned metadata without an added byte copy. |
| `framework/src/runtime/channels/route_packet_dispatcher.{{hpp,cpp}}`, `framework/src/runtime/mesh/mesh_record_dispatcher.cpp` | Submit the reply as an rvalue and consume its multipart list through the existing `take_items()` seam. |
| `framework/src/runtime/stateful/public_host_runtime.{{hpp,cpp}}` | Forward consuming replies into `from_parts(std::move(...))`; shared implementations retain one local/remote reply decision. |
| `tests/Zlink.Framework.UnitTests/test_cpp_framework_service_wire_codec.cpp` | Frozen request/reply/send bytes; native header and flow-v2 parity; inline-buffer coverage; invalid input and deliberate corruption controls; moved list/buffer identity, refcount and retained lifetime. |
| `tests/Zlink.Framework.UnitTests/test_cpp_framework_m6a_runtime.cpp` | Reference the actual native encoder when accounting for synchronous wire work; preserve the no-extra-lane-allocation assertions. |
| `tests/Zlink.Framework.UnitTests/test_cpp_framework_actor_late_result.cpp` | Adapt the existing private request-submission seam to the native header return type. Its assertions and late-terminal behavior are unchanged. |

The borrowed const-list reply API remains for callers that retain their input. Packed infrastructure/recovery records, generated logical-multicast/control/activation headers and fixtures retain their existing byte-vector APIs. Application vector/native encoders share one implementation; a native encoding failure never selects a vector fallback. Owned pre-encoded optional metadata keeps its existing external-message adoption, while borrowed multicast metadata retains its existing single ownership copy directly into native storage.

## Ownership and rule count

- Owner layer: Framework codec for E1 storage; binding/Core retain physical admission, transport retry and completion decisions.
- Spec clauses: `08-messaging-hot-path.ko.md` §3 E1 and `05-payload-ownership-and-codec.ko.md` §2/§4.
- Cross-language result: .NET and Java/Kotlin keep the same wire/ownership contract; C++ changes its internal native storage representation only.
- Classification: **A — adaptation to the existing contract**; no spec or public API change.

Before implementation, the ownership comparison identified these locations (`C = framework/languages/cpp/framework/src/runtime/`):

| Decision | Before locations | Final owner |
|---|---|---|
| Multipart adoption | `C/protocol/service_wire_codec.cpp:2688,2703`, `C/stateful/public_host_runtime.cpp:8549,8594` | `application_payload_t`, moved multipart value |
| Final wire storage | `C/protocol/service_wire_codec.cpp:2822`, `C/backend/raw_binding_adapter.hpp:211` | Codec native allocation |
| Physical admission/completion | `C/backend/raw_route_port.cpp:144,313` | Existing port/binding implementation |
| Reply routing/lifetime | `C/stateful/public_host_runtime.cpp:8549`, `C/mesh/raw_mesh_node_owner.cpp:2020` | Existing reply token/transport owner |

Cross-language comparison: .NET `ZLinkServiceWireCodec.cs:172,390` and `ZLinkApplicationPayloadEnvelopeCodec.cs`; Java `ZLinkServiceWireCodec.java:24` (also used by Kotlin). They retain the same wire/ownership contract; managed/native storage representations differ by binding. C++ changes internal storage only. Common golden/contract tests continue checking wire parity.

For the common application request/reply/send paths, counting Framework decision locations: final wire storage **2 → 1**, multipart adoption **1 → 1**, physical admission **1 → 1**, terminal completion **1 → 1**, reply routing **1 → 1**; total **6 → 5**. Cold infrastructure materialization is unchanged. The writer constructs its sized native message directly, avoiding an unused empty-message init/close. Its transient pointer/offset replace vector position/capacity; no persistent flag, counter, mode, queue, lock, timer or fallback was added.

Refactoring audit: four related ownership/representation findings fixed (multipart box, header holder, payload holder, borrowed dispatcher reply list); none deferred. Execution-gate and transport ownership remain unchanged.

## Validation

- Original-runtime frozen byte test: **PASS**, `baseline-test.log`. The initial new literal mistakenly used ZL; it was corrected to schema magic ZM before the baseline pass and implementation.
- Original-runtime ownership negative control: **expected failure**, `ownership-before.log`, at `retained.parts() != payload.parts()`. The existing representation assertion is updated to require independent lists, retaining byte equality and adding large-buffer identity checks.
- New focused codec test: **PASS**, `focused-test.log`.
- All **153 executable targets** covering the selected unit/contract labels compiled through `cpp-build-slot.sh`, `--parallel 3 -- -k 0`: see `unit-contract-final-build.log`. The existing `ZLINK_FRAMEWORK_CPP_TEST_PROBES=ON` / `INSTALL_FRAMEWORK=OFF` configuration is required by allocation tests. The first unit configuration omitted it and was superseded; benchmark and allocation-probe libraries remain production Release with probes off.
- Focused codec/runtime/native-call checks: **3/3 PASS**, `focused-contract-test.log`. Existing native init/close assertions are unchanged and pass after removing unused message initialization.
- C++ unit and contract labels: **287/287 PASS, 0 failures**, 131.25 seconds, `unit-contract-test.log`. Command: `measure-shared.sh ctest --test-dir {j}/build -L 'framework-unit|framework-contract' --parallel 3 --output-on-failure`.
- C++ bench: built from this worktree by `framework/bench/grpc/cpp/build.sh` under the build slot. A job-local `cmake` wrapper enforces `--parallel 3 -- -k 0`; see `bench-verified-build.log` and `bin/cmake`.
- Format: `bash scripts/format/format.sh --check cpp` from the worktree root, final log `format-final.log`. `git diff --check` also passes.

## Allocations per message

Isolated production-library probe: 10,000 messages per path, two input native parts (128/1024 bytes) prepared before counting. Task/completion/transport and input allocation are excluded. Request/send use moved input; reply compares the previous const-list/materialization path with the consuming path. Counters are the supplied `cprof/alloc_counter.so`; main-thread malloc/calloc/realloc deltas are read from its mapped file. Sources: `allocation-{{main,W}}.cpp`, `probe/CMakeLists.txt`; outputs: `probe-{{main,W}}.json`.

| Node path | Old static | W static | Old measured | W measured | Measured removed |
|---|---:|---:|---:|---:|---:|
'''
for path,before in [('request',12),('reply',12),('send',10)]:report+=f'| {path} | {before} | 4 | {old[path]:.4f} | {new[path]:.4f} | {old[path]-new[path]:.4f} |\n'
report+='''
Request removes the multipart box, three header-vector allocation/growth calls, two vector holders, the envelope vector allocation and one net native content allocation. Reply additionally removes its const-list and raw-to-native output-list allocations, with one header-vector allocation instead of three. Send has one header-vector allocation. New node output needs two packet/content strings, one native multipart list and one native envelope allocation (four calls); headers fit Core's existing 29-byte inline storage. The required aggregation write remains; no full-body memcpy removal is claimed. Other header sizes can change native inline/allocation counts.

## Required A/B measurement

A = `/root/worktree/zlink-ratio040`, main `ee1139025782688e33109455041d8a0222cc8fe3`, existing binaries. B = W. Framework only, payload 1024, four rounds per pattern; each round runs B then A for request-backpressure, then B then A for send-saturation. `WARMUP_SECONDS=30`, `DURATION_SECONDS=5`, one fresh process pair per cell. Source CPUs 0–7, target CPUs 8–15. Every run holds `flock -o /root/jobs/fx/cx/measure.lock`. The requested pinning patch was applied only to this worktree's C++ runner and reversed before commit; baseline pinning was preserved.

All rounds use the same allocation interposer. Submit CPU is the benchmark's full active-phase thread CPU divided by completed-at-close throughput. Heap calls use a 3-second inner active window, starting after 0.3 seconds, normalized by same-window source completions. The active submitter is selected among new source threads at phase start (`bench_cpp_client.cpp:1169`) by unique allocation dominance (more than 10 times the next candidate); transient HTTP/stats threads can also appear. Its inner-window CPU agrees with the benchmark's actual submitter CPU within 1.32% across all 16 cells. Candidate TIDs and deltas are preserved in each capture. Whole-process heap totals retain counters from exited threads as well. Snapshot boundaries have small HTTP/proc skew. The counter excludes aligned allocation, direct mmap and libc-internal allocation entry points. Counter overhead and run variation are not separated.

'''+(j/'measure-table.md').read_text()
for pattern in ['request-backpressure','send-saturation']:
 a=next(x for x in med if x['pattern']==pattern and x['build']=='main');b=next(x for x in med if x['pattern']==pattern and x['build']=='W')
 report+=f"\n{pattern}: median throughput **{(b['throughput']/a['throughput']-1)*100:+.2f}%**; submit CPU **{b['submit_cpu_us']-a['submit_cpu_us']:+.3f} µs/message**; submit heap calls **{b['submit_heap']-a['submit_heap']:+.3f}/message**.\n"
report+=f'''
The first collector attempt assumed a unique new TID and stopped when transient HTTP/stats threads appeared. Its files are retained under `rejected-collector-first-cell/`; it is excluded from these 16 complete cells. The collector now checks unique allocation dominance and the CPU cross-check described above.

All 16 final cells have zero errors, abandoned requests and header-validation failures, and no contamination or drain-bound hit. Raw per-round results/captures are under `measure/`; parsed data are in `measure-summary.json`. Minimum memory at capture snapshots: **{min(r['minimum_capture_mem_kb'] for r in rounds)/1048576:.3f} GiB**.

Attribution limit: the required W base also contains inherited changes in `dispatch/offload_executor.cpp` and `execution/state_lane.{{hpp,cpp}}` relative to baseline (test/debug guards and lane completion/capture lifetimes). W does not modify these files. Whole-benchmark differences compare the requested builds and cannot be attributed exclusively to W. The isolated allocation probe avoids those lane/dispatch paths. Both builds use the same installed Core/C++ binding package prefix from ratio040 (`1.23.0`). Four rounds describe these runs, not a statistical confidence bound.

## Remaining issues and constraints

The requested memory floor was **not maintained continuously**: sampled MemAvailable reached **{minimum:,} KiB = {minimum/1048576:.3f} GiB** during overlapping shared builds. Compiles were stopped, then both build slots were reserved while keeping compiler parallelism at three. No performance result was collected during compilation. The earlier 11.843 GiB dip and later lower dip are retained in the memory records; see `memory-min.json` and `memory-total-min.json`.

No build, unit/contract test, format or A/B measurement remains unfinished; final failures: none. The continuous memory-floor requirement was not satisfied, as documented above. No push or CI run was performed.
'''
(j/'report.md').write_text(report)
