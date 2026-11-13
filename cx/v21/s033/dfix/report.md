# .NET request path cost — issue #3869

Worktree: `/root/worktree/zlink-3869`; branch: `perf/3869-dotnet-request`; starting commit: `910d1fc41f`. No push is authorized or performed. Comparison A is the existing, prebuilt `/root/worktree/zlink-ratio040` at `ee1139025782688e33109455041d8a0222cc8fe3`; B is this worktree. Final commit and A/B results are recorded below.

**Result:** isolated reply decode cost fell about 24%, but the four throughput ratios remain **0.503 / 0.493 / 0.514 / 0.498**, all below 0.6. All final tests, format checks and 32 benchmark runs completed.

## Findings and changes

### Deadline management

The suspected per-request management visit was not reproduced. The .NET implementation is an intrusive single-table index, expressly permitted by spec `01-execution/01-submit-and-completion` §10.1. At the reserved visit it must inspect retained completed entries to release their references. Registration does not walk pending entries; there are no slot-round scans. The index already owns its reserved visit time and preserves the cursor when its 1 ms management budget expires.

The avoidable work is the second clock read per examined entry: `ZLinkDeadlineIndex.Visit` called the owner's deadline clock before the terminal callback, and its budget predicate read the same clock again afterwards. Completed requests also incurred both reads. The fix uses the visit's monotonic timestamp for timeout candidacy, as C++ and Java already do, and retains the per-entry budget read. It removes the deadline-clock callback and its two adapters. An entry is never timed out before its absolute deadline: the visit timestamp is at or after any deadline it expires. An entry whose deadline passes during the scan can reserve an immediately due subsequent visit. Timeout and reply continue to compete by removing the same terminal-table entry.

Focused `request-backpressure / 1024` captures used 5 s warmup and 5 s active time, Framework only, condition B CPU affinity. The temporary per-visit diagnostic emitted the monotonic visit timestamp, prior reservation, entries examined, and next reservation. It was removed before final builds. Counts below include setup, warmup, active and drain, and divide by the first-to-last visit span; these are diagnostic counts, not the final throughput A/B.

| Measurement | Before | After |
|---|---:|---:|
| First-to-last visit span (s) | 10.8545 | 10.8100 |
| Management visits | 81 | 57 |
| Visits/s | 7.462 | 5.273 |
| Entries examined | 228,602 | 242,772 |
| Median entries/visit | 1,893 | 3,723 |
| Maximum entries/visit | 7,016 | 7,936 |
| Visits retaining an already-due budget remainder | 40 | 16 |
| Active completed requests | 138,280 | 145,511 |

The after-index diagnostic was collected with the initial decoder candidate; its deadline index is identical to the final index, but the later-rejected field-name comparison was still present in that diagnostic. These are management-work counts, not a final-build throughput claim.

The source worker therefore had periodic cleanup plus budget continuations, rather than one management visit per request. The smaller number of budget continuations after eliminating redundant clock reads is consistent with doing more cleanup per 1 ms turn. This does not establish that the profiler's entire 6.9 µs inclusive estimate is removable; the original sampled attribution should not be treated as exact instruction timing.

Evidence: `raw/deadline-counts.json`, `raw/deadline-{before,after}/run1/zlink-framework-dotnet-request-backpressure-1024/{source.log,results.json}`, original and final diagnostic source snapshots under `raw/`.

### Reply decode

The benchmark uses Protobuf `BenchPayload { bytes body = 1; }`. The focused microbenchmark uses generated `BytesValue`, with the same single bytes-field wire shape and a 1,024-byte value. It includes the normal Response envelope and compares standalone native parts and the packed multipart view used by the request path. Each stage has 20,000 warmup calls and five rounds of 100,000 calls. Both before/after runs disable tiered compilation for comparable, fully compiled method timing; this setting is confined to the diagnostic test process, and is absent from the throughput A/B.

| Stage, median per call | Before | After |
|---|---:|---:|
| Header decode | 1.558 µs | 1.148 µs |
| Typed body decode | 0.205 µs | 0.182 µs |
| Complete reply decode, parts | 1.942 µs | 1.473 µs |
| Complete reply decode, multipart view | 1.974 µs | 1.503 µs |
| Managed allocation, header | 344 B | 344 B |
| Managed allocation, typed body | 1,112 B | 1,112 B |
| Managed allocation, full decode | 1,456 B | 1,456 B |

The header dominated this isolated decode. Its cache lookup hashed every incoming header byte using a scalar FNV loop, even though correlated responses cannot enter the decoded cache; the cache then still required byte equality. The fix removes the hash and cached hash field, using the existing byte-equality check as the sole identity decision. Insertion deduplication now calls that same lookup instead of repeating its identity predicate. This follows Java's decoded-cache byte comparison. The existing property-name decoding is retained. A candidate using `Utf8JsonReader.ValueTextEquals` was rejected after a new negative test demonstrated that it accepted an unknown property name containing byte `0xFF`, which the existing UTF-16 decoding rejects. The failing test is retained as a regression test, including both native parts and packed views; restoring the original name decoder makes it pass. This is why the final change removes the hash without adding a property-parser shortcut. Escaped names, duplicate detection, unknown-field skipping, Unicode validation and typed deserialization use their existing implementation.

The typed Protobuf body already reads native storage through its span deserializer. There is no intermediate payload array to remove on this benchmark path. The generic decoder's custom serializer selection and error wrapping remain in place. No codec shortcut, new cache, canonical-only parser, runtime option, or alternate decoding path was added.

C++ `envelope_codec.cpp:1094` tries its canonical byte scanner, then the SAX parser, with validation in the same decoder; its body API returns an encoded payload view. Java `ZLinkChannelEnvelope.java:482` reads the native ByteBuffer, uses byte equality for cached headers, and feeds the remaining input to Jackson's ByteBuffer parser (`:692`). Node uses its existing JSON-based channel envelope inspection. These implementations differ in parsing mechanism, not the public envelope contract. Other languages are unchanged in this job.

The original sampled 12.5 µs self time includes inlined work and is much larger than the isolated decode measurement. This change does not claim to recover 12.5 µs of end-to-end latency.

Evidence: `raw/decode-micro-before.txt`, `raw/decode-micro-final.txt`, `raw/decode-micro-source.cs`; rejected candidate evidence: `raw/unknown-name-utf8.log`, recovery: `raw/decode-hash-only.log`. The diagnostic test was removed from the committed unit tests.

### Reply disposal and completion reservations

`ZLinkBackendRouteReceived.Dispose` atomically claims disposal once, disposes its owned native Message parts, and releases the optional payload owner in `finally`. The direct reply contains the service header and the packed envelope; the application multipart view borrows the latter. Disposing either fewer owners or removing the once-only claim would change ownership semantics. No redundant copy or duplicate disposal was identified.

`ZLinkMeshCompletionTable.ReleaseReservation` updates dispatcher drain accounting under the completion table's existing lock, and completes the drain waiter outside it. The pending correlation is removed before dispatch; the outstanding reservation remains until the callback returns. Those are different lifetime facts required by the completion drain contract. No redundant pending owner was identified, and no change was made to either method. Their sampled self costs are not proven to be independently removable.

## Ownership, alternatives, decision count

The chosen deadline change removes an unnecessary clock read while retaining the existing index. A new timing wheel, a second deadline index, or eager completion-to-owner cleanup would add state or owner work without evidence of a scheduling defect. The chosen decoder change removes work from the existing cache while retaining the original parser. Adding a response-specific parser would add a second validation path and was rejected.

| Decision | Before | After | Specification / owner |
|---|---|---|---|
| Registration reserves / advances the visit | `ZLinkRequestDeadlineIndex.cs:27` | Same index registration, `:27` | Framework operation owner, spec 01 §10.1 |
| Management visit computes the next visit and cursor continuation | `ZLinkRequestDeadlineIndex.cs:66` | Same index visit, `:66` | Framework operation owner, spec 01 §10.1 |
| Mesh timeout terminal claim | `ZLinkManagedMeshNode.cs:12554` | Same atomic terminal table | Framework operation owner, spec 01 §10.1 |
| ClientServer timeout terminal claim | `ZLinkRequestDeadlineIndex.cs:238` | Same pending table, `:234` | Framework operation owner, spec 01 §10.1 |
| Envelope protocol validation | `ZLinkEnvelopeCodec.cs:905` (`ValidateProtocolHeader`) | Same validator, `:904` | Framework messaging, spec 01-execution/05-payload-ownership-and-codec §§6–7 and 02-channel-transport/06-wire-protocol typed payload profile |
| Header-field identity | `ZLinkEnvelopeCodec.cs:597` (`ReadHeaderField`) | Same original UTF-16 decoding and mapping, `:596` | Framework envelope parser |
| Decoded-cache byte identity | `ZLinkEnvelopeCodec.cs:1364,1381` (lookup and insertion each compare) | `FindDecodedHeaderCacheEntry` comparison at `:1359`; insertion calls it | Framework envelope codec cache |

Decision locations in this inventory: **8 before → 7 after** (cache identity 2 → 1; the other six decision locations are unchanged; index registration and management are counted separately). No new persistent state, flag, queue, lock, mode or fallback path is added; the cached hash field and two clock adapters are removed. The visit timestamp is already an input, not a second stored deadline.

Cross-language deadline comparison: C++ `operation_registry.cpp:679–713` checks the reservation and scans with the visit timestamp while reading its elapsed budget separately. Java `ZLinkServiceOperationRegistry.java:584–639` takes one `nowTick`, uses a cursor and absolute deadline comparisons, and reads elapsed budget separately. Node `operation-registry.ts:318,398` derives the visit from its same table/cursor and uses one `nowMs` for timeout checks. The .NET change aligns the clock-use mechanism without changing the common rule or any other language.

Classification: **B — existing implementation cost**, within the current contracts. No A/C/D design or specification change is introduced.

Owner: Framework operation owners and messaging codec.

Specification: submit/completion §10.1; payload ownership/codec §§6–7; service-wire multipart application profile.

Cross-language: C++/Java/Node use a visit timestamp for timeout checks; Java already compares decoded-cache bytes without the removed hash. Other languages are unchanged.

Classification: B — existing implementation cost. The user's task explicitly authorizes this scoped implementation, .NET-only comparison, tests, measurement and commit. Protected specification documents are unchanged.

## Changed files and call-path audit

- `framework/languages/dotnet/src/Zlink.Framework/Runtime/Messaging/ZLinkRequestDeadlineIndex.cs`: reuse the management visit timestamp; remove the per-entry deadline-clock callback and ClientServer adapter.
- `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs`: remove the mesh deadline-clock adapter and obsolete Visit argument.
- `framework/languages/dotnet/src/Zlink.Framework/Runtime/Messaging/ZLinkEnvelopeCodec.cs`: remove the full-header hash and cached hash state; share byte-identity lookup with insertion deduplication.
- `framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/MeshNodeShutdownSealTests.cs`: add the management-visit count contract and update the internal Visit call sites.
- `framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/ProtobufNativePayloadTests.cs`: add parts/view decode equivalence, malformed-input rejection and invalid-property-name UTF-8 regression coverage.

Touched-call-path audit: **3 baseline findings fixed** (duplicate deadline clock work, redundant header hash state/work, duplicate cache identity predicate), **0 deferred**; no new state owner, gate bypass, lock or queue. The candidate parser regression was removed and its reproducer retained. The temporary .NET benchmark affinity edit was restored before commit and is excluded from the committed files.

## Validation

The normal project commands completed under `flock -s -o /root/jobs/fx/cx/measure.lock` (initial diagnostic tests also used the requested shared lock). Final build/test commands set `MSBUILDDISABLENODEREUSE=1` and `-p:UseSharedCompilation=false` to avoid retained build-server locks. There are no benchmark runtime-setting changes beyond the requested CPU pinning.

| Validation | Result | Evidence |
|---|---|---|
| Focused .NET unit tests, `dotnet test .../Zlink.Framework.UnitTests.csproj -c Release --filter ...` | 352 passed, 0 failed | `raw/focused-final.log`; exact filter in `validate.sh` |
| Entire .NET contract project, `dotnet test .../Zlink.Framework.ContractTests.csproj -c Release` | 98 passed, 0 failed | `raw/contract-final.log` |
| Entire .NET unit project, `dotnet test .../Zlink.Framework.UnitTests.csproj -c Release` | 4,304 passed, 0 failed, 0 skipped; 8 m 22 s | `raw/unit-final.log` |
| Bench, `bash framework/bench/grpc/dotnet/build.sh` | Passed; copied Release DLLs matched | `raw/bench-final-build.log` |
| `bash scripts/format/format.sh --check dotnet` | Passed all 1,260 files | `raw/format-final.log` |
| `git diff --check` | Passed | Final worktree check |

The first focused run exposed an error in the newly written visit-count test's observation interval: it ended at tick 1,024, before the fourth cleanup reservation at tick 1,027. The test now observes the already-specified final reservation while retaining its exact four-visit and all-references-cleaned assertions. No production timeout, budget or fixture expectation was weakened. The initial format check identified only the four edited codec/index/test files; those were formatted with the repository-pinned CSharpier.

The new request-count regression uses the production generic deadline index with virtual time: 1,024 and 65,536 completed registrations must both require four management visits, examine every retained reference, and stop when the index is empty. New decode equivalence tests compare byte-identical native parts and packed views at 1 KiB and 4 KiB, including malformed JSON, marker, duplicate field, missing correlation, wrong kind, truncated Protobuf and invalid UTF-8 in an unknown property name. The original decoder passes the first set of decode equivalence tests as well (`raw/equivalence-original.log`: 69 selected tests passed).

## A/B throughput

The preliminary A/B pair used the later-rejected name parser and is excluded from the final table; its artifacts are retained in `raw/excluded-utf8-map/`. All four final rounds used the same final source.

Completed: **32/32 final runs**, four rounds × two patterns × two sizes × two Framework builds. Every run completed with zero client/server errors and matching completed/received counts. WARMUP_SECONDS=30, DURATION_SECONDS=5; each active window was 5,000 ms. A then B for each cell, each run separately under `flock -o /root/jobs/fx/cx/measure.lock`. A was not rebuilt. B was built by `framework/bench/grpc/dotnet/build.sh`, which checked copied Framework DLLs against the Release outputs. Temporary target CPUs 8–15 / source CPUs 0–7 affinity matched ratio040. The .NET run script has been restored byte-for-byte before commit. Measurement and shared build/test locks caused substantial queue waits between cells; those waits are outside the active windows.

The copied linux-x64 `libzlink.so` binaries in A and B have identical SHA-256 (`6f496304ef6e1287f57234567231bbd2b76ad2392f99e816001417d01215bf01`); see `raw/native-comparison.sha256` and the matching final verification in `raw/native-comparison-final.sha256`.

Ratios use the fixed ratio040 binding medians: backpressure 1024 = 58,990; backpressure 4096 = 49,454; serial 1024 = 11,303; serial 4096 = 11,320 msg/s. The ≥0.6 thresholds are 35,394; 29,672.4; 6,781.8; 6,792 msg/s respectively.

| Pattern / bytes | Round | A main msg/s | B fix msg/s | A / binding | B / binding |
|---|---:|---:|---:|---:|---:|
| request-backpressure / 1024 | 1 | 28,836.0 | 29,859.2 | 0.489 | 0.506 |
| request-backpressure / 1024 | 2 | 29,441.4 | 29,608.2 | 0.499 | 0.502 |
| request-backpressure / 1024 | 3 | 29,785.4 | 29,533.2 | 0.505 | 0.501 |
| request-backpressure / 1024 | 4 | 29,319.4 | 29,732.4 | 0.497 | 0.504 |
| **request-backpressure / 1024** | **median** | **29,380.4** | **29,670.3** | **0.498** | **0.503** |
| request-backpressure / 4096 | 1 | 24,202.0 | 24,388.0 | 0.489 | 0.493 |
| request-backpressure / 4096 | 2 | 24,093.4 | 24,346.8 | 0.487 | 0.492 |
| request-backpressure / 4096 | 3 | 23,723.0 | 24,377.4 | 0.480 | 0.493 |
| request-backpressure / 4096 | 4 | 24,171.8 | 20,772.8 | 0.489 | 0.420 |
| **request-backpressure / 4096** | **median** | **24,132.6** | **24,362.1** | **0.488** | **0.493** |
| request-serial / 1024 | 1 | 5,807.6 | 5,815.6 | 0.514 | 0.515 |
| request-serial / 1024 | 2 | 5,665.0 | 5,802.8 | 0.501 | 0.513 |
| request-serial / 1024 | 3 | 6,014.4 | 6,044.8 | 0.532 | 0.535 |
| request-serial / 1024 | 4 | 4,299.6 | 5,529.2 | 0.380 | 0.489 |
| **request-serial / 1024** | **median** | **5,736.3** | **5,809.2** | **0.508** | **0.514** |
| request-serial / 4096 | 1 | 5,713.8 | 5,750.2 | 0.505 | 0.508 |
| request-serial / 4096 | 2 | 5,608.0 | 5,543.4 | 0.495 | 0.490 |
| request-serial / 4096 | 3 | 5,600.8 | 5,735.0 | 0.495 | 0.507 |
| request-serial / 4096 | 4 | 5,438.8 | 5,412.2 | 0.480 | 0.478 |
| **request-serial / 4096** | **median** | **5,604.4** | **5,639.2** | **0.495** | **0.498** |

| Cell | B versus A median | B / binding ≥ 0.6 |
|---|---:|---|
| request-backpressure / 1024 | +0.99% | no |
| request-backpressure / 4096 | +0.95% | no |
| request-serial / 1024 | +1.27% | no |
| request-serial / 4096 | +0.62% | no |

Minimum sampled MemAvailable during the 32 queued/run intervals: 20.07 GiB (limit: 12 GiB; no aborts).

The ≥0.6 target is **not reached in any cell**. The median changes versus A are only +0.62–1.27%, with overlapping run-to-run variation; these runs do not establish a substantial end-to-end throughput improvement. The isolated reply decoder cost reduction is demonstrated independently by the microbenchmark.

Round 4 includes a low backpressure/4096 B result (20,772.8 msg/s), retained without replacement. It had zero errors and completed=received=103,885. Against round 3 B, source CPU fell 71.6→59.525%, target CPU fell 34.30→27.95%, and p99 latency rose 4.23→29.811 ms. The following unmodified A serial/1024 run also fell to 4,299.6 msg/s. These observations do not identify a cause; no host/scheduler/GC explanation is asserted. Normal result JSON records allocations but not GC collection counts. All four rounds, including low values, participate in the medians.

Evidence: `raw/ab-results.json`, `raw/ab-medians.json`, each `raw/ab-r*-*.log` and corresponding result directory; exact orchestration is in `ab.py`.

## Remaining request chain and limits

The following is the remaining normal, ready-peer request path, established from source and the supplied per-thread profile. It identifies actual scheduling/await boundaries; it does not assign unmeasured latency to them.

| Stage / threads | Work and wait | Source |
|---|---|---|
| Source caller / ThreadPool continuation | `ZLinkRouteClient` creates the typed envelope and service-wire request; `RequestNativeDurableWireAsync` obtains the native-request lifetime on the node lane. | `ZLinkRouteClient.cs:405`, `ZLinkManagedMeshNode.cs:10695,11546` |
| Source operation owner | `RunDeadlineRequestAsync` awaits `CreateOperationAsync`; the operation lane registers the single pending entry, index reference and dispatcher reservation. A free lane executes on the caller; contention waits behind the preceding turn, without an unconditional thread hop. | `ZLinkManagedMeshNode.cs:5586,11204`, `ZLinkStateLane.cs:77` |
| Source binding / native I/O workers → target native I/O workers | Submission calls the public binding request API under the existing socket gate. The producer awaits the binding reply Task. An unready peer waits on `route.Changed`; delayed native admission awaits `Admitted`. Neither readiness nor backpressure waiting is assumed to happen on every ready-peer request. The native I/O workers move the request over TCP. | `ZLinkManagedMeshNode.cs:11300`, `ZLinkDurableRequest.cs:8`, `ZLinkRequestSubmissionOutcome.cs:67` |
| Target Framework pool receive/application turns | The process pool's receive role observes the ready socket and queues application work; an application turn decodes and calls `EchoHandler`. The handler returns an already-completed ValueTask; reply encoding/submission follows the handler. Work can wait in the application ready list / execution gate. Receive and application roles can be transferred across physical `.NET Long Runni` workers. | `ZLinkProcessExecutionPool.cs:332,457,1042`, `ReceiveLoopAsync` / `DrainRawSocket`, bench `ZLinkServer/Program.cs:64` |
| Target native I/O → source binding completion | The native reply returns through I/O. The binding completes its reply Task; `ObserveDirectWireReplyAsync` and `ObserveNativeDeadlineReplyAsync` resume physical reply observation, decode service/envelope/typed reply, and claim the Framework terminal entry. | `ZLinkManagedMeshNode.cs:11124,11335,11409` |
| Source Framework completion dispatcher → caller / next submit | `CompleteManagedOperation` posts `PendingOperation<TReply>` using `ThreadPool.UnsafeQueueUserWorkItem`. This is an explicit new pool turn after physical completion. `Execute` completes the caller Task; eligible caller continuations may run inline on that dispatcher thread. `Completed` releases the drain reservation after callback return. Serial cannot start its next request before this chain finishes; backpressure replenishes its window through the corresponding completion observation. | `ZLinkManagedMeshNode.cs:11111,14125`, `ZLinkCompletionDispatcher.cs:14`, `ZLinkMeshCompletionTable.cs:274`, bench `Client/BenchMeasurement.cs` |

The supplied unmodified Framework profile measured source/target handoff-and-wake CPU of 104.9/10.1 µs per completed serial message and 26.5/8.4 µs for backpressure, versus isolated decode around 2 µs. Those CPU totals overlap work across threads and are not boundary latency measurements. Serial also showed 8.30/5.53 context switches per message (source/target) and no saturated Framework thread. This supports examining the completion/dispatch chain as a next measurement boundary; it does not establish that removing a particular hop is contract-correct or would reach 0.6. No additional scheduler, retry, queue or timeout changes were made on that inference.

The binding benchmark also has a concrete scheduling difference: `RawBenchTransport.DriveAsync` (`Client/Program.cs:465`) actively calls the public `socket.WaitCompletion` API until the phase task finishes. The Framework transport inherits `IBenchTransport.DriveAsync` (`:149`), which returns the pending task, while its receive/infrastructure owners and terminal dispatcher progress the request. `BenchMeasurement.cs:238,554` uses this transport hook. Thus the comparison includes caller-driven binding completion versus Framework-owned receive/completion scheduling, in addition to envelope/service-wire work. The contribution of that difference to the remaining latency is not measured, and the benchmark was not changed to hide it.

Not measured: per-request time spent in lane backlog, binding reply dispatch, target application queue, Framework completion queue, and caller continuation scheduling. No numerical latency split is inferred from CPU samples.

## Commit and unfinished work

Commit: **`aa7dd5e364f01d6ad44aa78bf47926cb357138d3`** on **`perf/3869-dotnet-request`**. Five source/test files committed; **no push**. The worktree is clean and temporary CPU pinning is restored. All requested validation and the final 32-run measurement are complete.

Unfinished performance outcome: all four median ratios remain below 0.6. Per-boundary waiting times and exact disposal/reservation CPU costs were not measured; the source-backed remaining chain and limitations above identify the next investigation without attributing an unmeasured latency to a specific hop. No extra scheduling/timeout/buffering optimization was guessed into this change.
