# C++ gate-less drain second pass — #1639

Status: acceptance UNMET; changes remain uncommitted on perf/1639-cpp-multi-drain, based on HEAD 5a585edc93. No commit or push. The working tree was clean at job start.

## Acceptance criteria

| Criterion | Result | Main median | Branch median | Required branch value | Change / ratio |
|---|---|---:|---:|---:|---:|
| request-backpressure | UNMET | 48,414.703 | 47,660.156 | ≥ 47,930.556 | -1.56% |
| send-saturation | UNMET | 176,977.062 | 154,844.152 | ≥ 175,207.291 | -12.51% |
| PM hot throughput | MET | 9,530.248 | 61,566.428 | ≥ 38,120.993 | 6.460× |
| PM Spot p99 (µs) | UNMET | 489.922 | 7,538.543 | ≤ 979.844 | 15.387× |

Cheap-cell and hot-cell values are Framework msg/s; Spot values are the median of the three per-run nearest-rank p99 values. Cheap acceptance compares Framework medians, not Framework/binding ratios. All cells retain the same scripts, condition B and three-round ordering as the previous job.

Spec references: S/02 = `01-execution/02-handler-turn-and-execution-gate.ko.md`; S/04 = `01-execution/04-application-job-queue-and-backpressure.ko.md`; S/08 = `01-execution/08-messaging-hot-path.ko.md`; API = `00-foundation/06-framework-api.ko.md`, all under `framework/doc/framework/common/spec/server/`.

## Decision ownership before implementation

The supervisor approved the supplied backlog-growth and helper-rotation design (classification A, contract adaptation). Alternatives considered: retain unconditional take-time wake (measured CPU and latency regressions), or the approved W5 backlog comparison (selected). No other scheduling rule is introduced.

| Decision | Before (runtime-relative path) | Intended owner after | Spec authority |
|---|---|---|---|
| Record ownership | mesh/service_mailbox.cpp:464 | same mailbox atomic take; return exact remaining count | S/02 §2; S/08 W1 |
| Ready membership | mesh/service_mailbox.cpp:79,464 | unchanged mailbox | S/04 ready-owner set |
| Idle reservation / wake eligibility | dispatch/offload_executor.cpp:207; unconditional call at :461 | same executor reservation; gate-less extra wake decided at W5 from remaining-count comparison | S/04 ready-owner set |
| Worker rotation | dispatch/offload_executor.cpp:425–434 | same executor queue at W5, budget or multiple serving workers | S/02 §9; API §11; S/08 W5 |
| Permit boundary | dispatch/application_job_queue.hpp:1281 | unchanged handler entry | S/04 step 5; S/08 W3 |
| Gated execution | existing gated drain/gate route | unchanged | S/02 §2 |

Decision-location count from the previous report: 13 → 6; this pass starts at 6 and must not increase it. The existing active-cancellation registry will become the active-work registry, preserving cancellation ownership and supplying current synchronous drain-worker identity without a second registry, flag, queue or counter. The remembered take count is worker-local. The mailbox remains the sole record-count owner.

Cross-language read-only comparison inherited from the previous report: .NET pool owns readiness/running counts (ZLinkProcessExecutionPool.cs:443–455,993–1010); Java runtime uses serial namespace dispatch (ZLinkMeshApplicationDispatcher.java:995–998; ZLinkHandlerDispatch.java:16–44), Kotlin reuses Java; Node.js mailbox uses event-loop owner claims (service-mailbox.ts:68,134–159,177–189). This explicitly C++ job leaves these implementations unchanged.


## Decision locations after implementation

Counts use the same accepted hosted gate-less-drain scope as the previous report. Paths below are runtime-relative. Standalone/manual/rejection-only paths, gated execution and shutdown broadcasts remain outside this count.

| Decision | Original main locations | HEAD 5a585edc93 locations | Current locations | Current authority location |
|---|---:|---:|---:|---|
| Record ownership | 3 | 1 | 1 | `mesh/service_mailbox.cpp:464` |
| Ready membership insertion/removal | 4 | 2 | 2 | `mesh/service_mailbox.cpp:79,464` |
| Extra wake eligibility and idle reservation | 3 | 1 | 1 | `dispatch/offload_executor.cpp:209` (backlog, ready and idle checks together) |
| Worker budget / helper rotation | 2 | 1 | 1 | `dispatch/offload_executor.cpp:442` |
| Permit release boundary | 1 | 1 | 1 | unchanged `dispatch/application_job_queue.hpp:1279` |
| Total | **13** | **6** | **6** | |

수정 전/후 규칙 수: **6 → 6** (이전 작업 포함: **13 → 6 → 6**). The remembered count is local; the active-work registry replaces the active-cancellation registry. No second authoritative owner-running state is maintained.

## Implementation

`take_application_record` returns the exact untaken count while holding the existing mailbox lock. Mesh and Channel drain adapters forward that count. The worker retains only its previous post-take count. At its next W5 selection, the next take and comparison occur under the existing executor scheduling lock. A decreasing static batch therefore wakes only its first worker; a maintained or growing backlog can reserve one more idle worker. Comparing before the next take would mistake an unchanged static queue for growth, so the counted boundary includes the worker's next take.

The existing active-cancellation registry is replaced by the active-work registry. Its entries borrow current work-item addresses, so cancellation and synchronous drain-worker identity have one registry. No owner-running counter, flag, queue, lock or timer is added. A W5 scan checks whether another worker is serving the same group/owner. If so, or the sole worker has exhausted the existing budget, the existing executor queue rotates to the first different waiting owner. Asynchronous terminal credits do not count as workers. Ordinary callable captures settle before publishing quiescence; their existing variant alternative remains stable while registered workers inspect drain identity.

The old unconditional worker-loop wake and take-time capacity path are removed. Ordinary jobs, receive roles and newly ready owners reserve wakes at publication; a gate-less continuation requests more capacity through the backlog comparison. Owner identity comparison is shared by budget continuity and rotation.

The shutdown test publishes its second record after observing the first record's last take. It retains all taken/running, permit and shutdown assertions, while testing the first-ready publication path independently of helper scale-out.

## Verification

The first focused pass exposed a capture-lifetime regression in the registry refactor: `application_drain_waits_for_work_capture_destruction` failed (`test-focused.log`). Callable targets now clear before quiescence while their variant tag stays stable for registry readers. The existing assertion passes unchanged in the corrected focused run. No additional lock or persistent state was added.


- Negative reproduction: `test-negative.log`, old runtime plus strengthened cheap-batch assertion: `cheap batch wake count=4`; assertion `notifications == 1` failed. Build log: `build-negative.log`.
- Focused build/test: **8/8 passed**, 3.18 seconds (`build-focused-fix.log`, `test-focused-fix.log`, `test-focused-detail.log`). Cheap batch output: `cheap batch wake count=1`; the maintained-backlog and growing-backlog cases each assert exactly one additional wake, and the second ready owner executes before the next hot record.
- Full verification: `verify-final.sh`; **309/309 non-install tests passed**, zero failed/skipped, 264.21 seconds, the same set as the previous job. The two install-consumer tests are excluded as requested. Logs: `build-all.log`, `test-all.log`, `test-all.xml`; compiler-invoking CTest commands use `cxx-slot.sh` and `cmake-slot.sh`.
- Format: `flock -s /root/jobs/fx/cx/measure.lock bash scripts/format/format.sh --check cpp` passed (`format.log`, exit 0). `git diff --check` passed (`diff-check.log`, exit 0).
- The installed build-slot wrapper accepts a command directly, not a leading `--parallel` option; as in the prior job, builds use `/root/jobs/fx/cx/cpp-build-slot.sh cmake --build ... --parallel 3`, inside the required shared lock. Measurements use `flock -o` (exclusive) on the same lock.
- Existing object files were copied into this job and validated by Ninja's command hashes/dependency graph; the previous job remains unchanged. Configure, build, test and measurement outputs are confined to this job.
- Baseline evidence: `verify-baseline.log`, `versions.json`: all 256 baseline runtime blobs match frozen main `3f83745bed140d2e50df747f5e157fb2ee02293e`; the original Core checksum is unchanged. Frozen main benchmark executable checksums were saved.

## Measurements

Release / `-O3 -DNDEBUG`, identical Core/binding packages and dependency versions, test creation probes disabled and install enabled for both builds. Condition B pins source 0–7 and target 8–15. Each cell uses 30 s warm-up, followed by 5 s active time (PM: 30 s). Three runs per cell and variant. An exclusive measure.lock excludes builds/tests. TIDs are marked once on first handler/receive entry; no per-record logging is used. CPU and switches come from `/proc/<pid>/task/*/{schedstat,status}`, sampled every ~100 ms during a 3 s window (PM: 25 s). CPU µs/msg and switches/msg use target messages received in that same window as denominator. A retiring thread may have at most one sampling interval of unobserved final counters. Unique TIDs across a window can exceed the concurrent worker bound because idle workers retire and are replaced.

| Cell | Run | Revision | Binding msg/s | Framework msg/s | Framework / binding |
|---|---:|---|---:|---:|---:|
| request-backpressure 1K | 1 | main | 251,127.8 | 48,414.7 | 0.1928 |
| request-backpressure 1K | 1 | branch | 248,209.5 | 46,265.3 | 0.1864 |
| request-backpressure 1K | 2 | main | 249,453.3 | 49,378.7 | 0.1979 |
| request-backpressure 1K | 2 | branch | 250,488.3 | 47,660.2 | 0.1903 |
| request-backpressure 1K | 3 | main | 251,218.4 | 47,022.3 | 0.1872 |
| request-backpressure 1K | 3 | branch | 243,823.3 | 48,388.1 | 0.1985 |
| send-saturation 1K | 1 | main | 631,031.9 | 176,977.1 | 0.2805 |
| send-saturation 1K | 1 | branch | 624,563.7 | 155,997.3 | 0.2498 |
| send-saturation 1K | 2 | main | 623,156.3 | 176,201.4 | 0.2828 |
| send-saturation 1K | 2 | branch | 621,643.4 | 154,844.2 | 0.2491 |
| send-saturation 1K | 3 | main | 609,675.7 | 180,486.5 | 0.2960 |
| send-saturation 1K | 3 | branch | 608,582.7 | 153,643.1 | 0.2525 |

| Cell | Revision | Binding 3-run median | Framework 3-run median | Ratio of medians |
|---|---|---:|---:|---:|
| request-backpressure 1K | main | 251,127.8 | 48,414.7 | 0.1928 |
| request-backpressure 1K | branch | 248,209.5 | 47,660.2 | 0.1920 |
| send-saturation 1K | main | 623,156.3 | 176,977.1 | 0.2840 |
| send-saturation 1K | branch | 621,643.4 | 154,844.2 | 0.2491 |

Target per-thread CPU (each entry is `TID: µs/msg`). Receive-role and worker TIDs are identified by the first-entry role markers. Context switches include voluntary and involuntary switches for all threads in the corresponding process.

| Cell | Run | Revision | Target application workers | Target receive role | Target switches/msg | Source switches/msg |
|---|---:|---|---|---|---:|---:|
| request-backpressure | 1 | main | 4175092: 10.875; 4175789: 8.888 | 4174264: 14.741 | 1.3808 | 2.6451 |
| request-backpressure | 1 | branch | 4181598: 4.173; 4183001: 4.801; 4183002: 4.573; 4183003: 4.658; 4183004: 5.354; 4183006: 4.485; 4183007: 2.736; 4183008: 4.662; 4183035: 2.215 | 4181498: 16.389 | 2.3408 | 2.4889 |
| request-backpressure | 2 | main | 22392: 4.475; 23979: 10.514; 24008: 3.725 | 22391: 14.221 | 1.3249 | 2.5849 |
| request-backpressure | 2 | branch | 29812: 1.080; 31196: 4.884; 31197: 5.007; 31199: 4.614; 31200: 5.414; 31201: 4.762; 31202: 5.116; 31203: 4.305; 31227: 3.387 | 29721: 15.222 | 2.2932 | 2.5971 |
| request-backpressure | 3 | main | 59770: 5.929; 60286: 10.251; 60338: 3.864 | 58759: 15.410 | 1.4008 | 2.4651 |
| request-backpressure | 3 | branch | 63802: 4.702; 65204: 4.866; 65206: 4.883; 65207: 4.821; 65208: 4.777; 65209: 4.934; 65210: 5.252; 65211: 4.786 | 63722: 15.468 | 2.2017 | 2.4698 |
| send-saturation | 1 | main | 4189900: 2.687; 4190411: 2.911 | 4188821: 4.267 | 0.2709 | 0.0383 |
| send-saturation | 1 | branch | 2178: 3.108; 3712: 3.093; 3713: 3.123; 3715: 3.076; 3716: 3.083; 3717: 3.099; 3718: 3.083; 3719: 3.101 | 2179: 5.284 | 1.3585 | 0.0354 |
| send-saturation | 2 | main | 36913: 2.830; 38525: 2.692 | 36914: 4.259 | 0.2456 | 0.0393 |
| send-saturation | 2 | branch | 44416: 3.085; 45894: 3.091; 45895: 3.091; 45896: 3.081; 45897: 3.063; 45899: 3.073; 45900: 3.088; 45901: 3.059 | 44335: 5.327 | 1.3664 | 0.0346 |
| send-saturation | 3 | main | 67399: 2.615; 68888: 2.761 | 67302: 4.087 | 0.2386 | 0.0371 |
| send-saturation | 3 | branch | 71087: 3.133; 72519: 3.106; 72520: 3.137; 72522: 3.120; 72523: 3.125; 72524: 3.122; 72525: 3.120; 72526: 3.123 | 71007: 5.298 | 1.4367 | 0.0355 |
| pm | 1 | main | 10003: 1.184; 10014: 1.739; 10028: 0.674; 10033: 2.410; 10058: 0.673; 10059: 1.236; 10064: 3.482; 10092: 2.858; 10101: 3.149; 10129: 1.514; 10137: 0.617; 10141: 1.460; 10163: 0.842; 10164: 4.100; 10193: 0.226; 10195: 1.123; 10199: 3.305; 9353: 3.139; 9482: 1.297; 9489: 0.846; 9507: 0.900; 9508: 1.008; 9510: 2.025; 9535: 2.475; 9550: 4.092; 9581: 3.083; 9606: 2.521; 9618: 2.892; 9646: 2.798; 9658: 2.692; 9682: 2.803; 9693: 2.523; 9720: 2.689; 9735: 2.744; 9752: 3.141; 9788: 5.882; 9813: 0.394; 9825: 12.045; 9923: 2.411; 9931: 1.124; 9940: 5.120; 9992: 3.223 | 7528: 3.923 | 0.3762 | 0.3645 |
| pm | 1 | branch | 14420: 14.139; 15911: 14.154; 15913: 14.127; 15914: 14.127; 15915: 14.141; 15916: 14.176; 15917: 14.115; 15918: 14.097 | 14329: 4.165 | 0.2504 | 0.0607 |
| pm | 2 | main | 51800: 0.914; 51806: 3.315; 51837: 1.852; 51842: 4.996; 51879: 5.014; 51913: 1.296; 51936: 2.860; 51944: 3.375; 51975: 6.567; 52015: 0.788; 52019: 0.676; 52035: 1.908; 52043: 5.167; 52079: 1.009; 52080: 2.415; 52107: 1.796; 52113: 0.728; 52114: 2.758; 52124: 0.225; 52140: 4.381; 52180: 2.245; 52185: 1.802; 52211: 6.121; 52235: 0.170; 52249: 1.291; 52254: 4.156; 52277: 0.336; 52283: 3.482; 52311: 1.912; 52336: 1.458; 52340: 1.010; 52344: 1.010; 52349: 5.277; 52384: 1.279; 52405: 4.643; 52445: 2.861; 52455: 8.979; 52519: 0.170; 52523: 1.692; 52550: 1.682; 52553: 0.562; 52554: 0.223 | 49866: 4.029 | 0.3680 | 0.3653 |
| pm | 2 | branch | 53019: 14.242; 54570: 14.173; 54571: 14.134; 54573: 14.153; 54574: 14.137; 54575: 14.209; 54576: 14.178; 54577: 14.061 | 52933: 4.172 | 0.2493 | 0.0618 |
| pm | 3 | main | 74594: 0.291; 74718: 0.785; 74720: 6.390; 74779: 0.841; 74780: 1.119; 74783: 0.563; 74784: 0.783; 74785: 3.539; 74816: 0.842; 74817: 4.210; 74849: 6.055; 74889: 0.225; 74908: 4.768; 74938: 1.457; 74947: 4.770; 74980: 5.496; 74989: 0.333; 75019: 6.426; 75022: 0.169; 75066: 0.565; 75084: 0.730; 75088: 3.423; 75092: 0.393; 75116: 3.534; 75127: 2.418; 75153: 5.327; 75187: 5.969; 75214: 0.225; 75225: 9.760; 75247: 0.225; 75280: 0.225; 75296: 4.132; 75330: 0.847; 75340: 3.445; 75372: 2.957; 75406: 2.464; 75411: 5.055; 75463: 3.223; 75480: 0.225 | 72759: 4.078 | 0.3674 | 0.3661 |
| pm | 3 | branch | 75963: 14.102; 77467: 14.156; 77468: 14.130; 77470: 14.199; 77471: 14.153; 77472: 14.167; 77473: 14.125; 77474: 14.098 | 75860: 4.164 | 0.2491 | 0.0616 |

Binding comparison: the receive role executes its handler inline; there is no separate application-worker stage.

| Cell | Run | Revision | Binding receive + handler TID: µs/msg | Target switches/msg | Source switches/msg |
|---|---:|---|---|---:|---:|
| request-backpressure | 1 | main | 4172461: 3.431; 4172462: 0.000 | 0.1421 | 0.2133 |
| request-backpressure | 1 | branch | 4179699: 3.372; 4179700: 0.000 | 0.1407 | 0.2317 |
| request-backpressure | 2 | main | 20564: 3.443; 20565: 0.000 | 0.1219 | 0.1996 |
| request-backpressure | 2 | branch | 27808: 3.212; 27809: 0.000 | 0.1504 | 0.2296 |
| request-backpressure | 3 | main | 56872: 3.631; 56873: 0.000 | 0.1144 | 0.2055 |
| request-backpressure | 3 | branch | 61912: 3.536; 61914: 0.000 | 0.1298 | 0.2244 |
| send-saturation | 1 | main | 4186922: 0.000; 4186923: 1.301 | 0.0931 | 0.0381 |
| send-saturation | 1 | branch | 302: 0.000; 303: 1.252 | 0.1140 | 0.0308 |
| send-saturation | 2 | main | 35048: 0.000; 35049: 1.327 | 0.0929 | 0.0392 |
| send-saturation | 2 | branch | 42477: 0.000; 42478: 1.316 | 0.0993 | 0.0340 |
| send-saturation | 3 | main | 65458: 0.000; 65459: 1.350 | 0.1031 | 0.0300 |
| send-saturation | 3 | branch | 69145: 0.000; 69146: 1.320 | 0.0976 | 0.0399 |

PM cell: one hot Node owner runs 100 µs synchronous CPU handlers; a public HTTP trigger issues an independent typed request to a gated Instance Spot on an independent 100 ms cadence while the main collector samples /proc. The reported latency measures only the public `request_to_spot(...).instance_spot(...).async<...>()` interval inside the trigger, excluding HTTP transport time. Cold activation completes before hot traffic starts. The same target hosts the hot Node and Spot. The HTTP-triggered public Spot call uses same-host local enqueue through the ordinary application mailbox/executor, gate and permit paths (public_host_runtime.cpp:9615,9345,8992; mesh_node_host_service.cpp:3331). Its latency includes local admission, scheduling, execution and completion; it excludes HTTP and remote network transport. The hot traffic source is pinned to 0–7 and the target including the Spot to 8–15. In-memory Stores are setup dependencies.

| Run | Revision | Hot msg/s | Spot samples | Spot p50 µs | Spot p99 µs | Peak concurrent hot handlers | Median hot handlers at probe completion |
|---:|---|---:|---:|---:|---:|---:|---:|
| 1 | main | 9,530.2 | 250 | 299.862 | 489.922 | 1 | 1.0 |
| 1 | branch | 61,566.4 | 250 | 1547.979 | 7635.611 | 8 | 5.0 |
| 2 | main | 9,532.5 | 250 | 307.676 | 465.046 | 1 | 1.0 |
| 2 | branch | 61,551.6 | 250 | 1465.307 | 6753.458 | 8 | 5.0 |
| 3 | main | 9,528.9 | 250 | 293.920 | 514.738 | 1 | 1.0 |
| 3 | branch | 61,628.0 | 250 | 1121.735 | 7538.543 | 8 | 5.0 |

| Revision | Median run p50 µs | Median run p99 µs |
|---|---:|---:|
| main | 299.862 | 489.922 |
| branch | 1465.307 | 7538.543 |

Raw cells: `/root/jobs/fx/cx/v21/s033/cdrain2/measure/`; per-thread counters and summaries: `/root/jobs/fx/cx/v21/s033/cdrain2/summary.json`. Reproduction scripts: `configure.sh`, `configure-main.sh`, `measure.sh`, `run-B.sh`, `runner-common.sh`, `collect.py`, and `bench/` in the same job directory.

## Cross-language comparison

The previous read-only comparison was checked at the cited .NET, Java and Node.js locations before implementation. .NET keeps ready/running state in its process pool; Java uses namespace serial queues and Kotlin shares that runtime; Node.js uses claimed-owner state on its event loop. The supervisor's explicitly C++ scope applies.


## Profile medians

| Cell | Revision | Application µs/msg | Receive µs/msg | Target switches/msg | Participating application TIDs/run |
|---|---|---:|---:|---:|---|
| request-backpressure | main | 19.763 | 14.741 | 1.3808 | 2, 3, 3 |
| request-backpressure | branch | 38.569 | 15.468 | 2.2932 | 9, 9, 8 |
| send-saturation | main | 5.522 | 4.259 | 0.2456 | 2, 2, 2 |
| send-saturation | branch | 24.764 | 5.298 | 1.3664 | 8, 8, 8 |
| pm | main | 104.401 | 4.029 | 0.3680 | 42, 42, 39 |
| pm | branch | 113.130 | 4.165 | 0.2493 | 8, 8, 8 |

TID counts show participation within the sampled window, not instantaneous handler concurrency. Raw per-thread CPU and switches remain in `summary.json` and each cell’s `capture/`.

## Changed files and purpose

Paths are relative to `framework/languages/cpp/`.

| Files | Purpose |
|---|---|
| `framework/src/runtime/dispatch/offload_executor.cpp`, `.hpp` | Centralize extra-wake eligibility in the existing reservation method; compare post-take counts at W5; rotate helpers using the existing queue and active-work registry; preserve capture destruction before quiescence and the original pre-take budget start. |
| `framework/src/runtime/mesh/service_mailbox.cpp`, `.hpp` | Return the exact untaken count from the atomic record take, replacing the boolean `more`. |
| `framework/src/runtime/mesh/mesh_node_host_service.cpp` | Forward the remaining count through the Node drain source; retain the rejection route. |
| `framework/src/runtime/channels/channel_host_service.cpp` | Forward the same count for ChannelName drains. |
| `framework/src/runtime/stateful/public_host_runtime.cpp`, `.hpp` | Carry the count through the existing internal claim-batch bridge. |
| `tests/Zlink.Framework.UnitTests/test_cpp_framework_gate_less_drain.cpp` | Require one cheap-batch wake; test maintained and growing backlog, helper rotation, and deterministic last-take publication in the unchanged shutdown assertions. |
| `tests/support/manual_internal_scheduler.hpp` | Adapt the existing manual queue to record presence and remaining count. |

리팩토링 점검: 성능 2건, POSDDD 2건, 불필요 코드 1건 발견. 고친 항목: 무조건 추가 wake, 다중 worker의 다른 owner 실행 기회, callback 소멸 전 quiescence 통지, take 전 예산 시작 보존, 기존 take 직후 무조건 wake·capacity 경로 제거. 넘긴 항목: 없음.

## Verification commands and artifacts

Job-relative commands run from this job directory; format and Git commands run from the worktree.

- Configure: `bash configure-tests.sh`, `bash configure.sh`; logs `configure-tests.log`, `configure-production.log`.
- Focused build: `flock -s /root/jobs/fx/cx/measure.lock /root/jobs/fx/cx/cpp-build-slot.sh cmake --build test-build --parallel 3 --target test_cpp_framework_gate_less_drain test_cpp_framework_offload_executor test_cpp_framework_mailbox_owner_dispatch test_cpp_framework_handler_entry_permit test_cpp_framework_manual_dispatch_owner test_cpp_framework_mailbox_fifo_cost test_cpp_framework_local_enqueue_allocations zlink_cpp_framework_mesh_node_vertical_test`; log `build-focused-fix.log`.
- Focused CTest: the eight targets above via `ctest --test-dir test-build -R '^(test_cpp_framework_(gate_less_drain|offload_executor|mailbox_owner_dispatch|handler_entry_permit|manual_dispatch_owner|mailbox_fifo_cost|local_enqueue_allocations)|zlink_cpp_framework_mesh_node_vertical_test)$' --output-on-failure --stop-on-failure`, under the shared lock; log `test-focused-fix.log`: `100% tests passed, 0 tests failed out of 8`.
- Final build/test/production build: `bash verify-final.sh`. `instrument-test-commands.py` reconciles the final executor object and dependent links before instrumenting compiler-invoking tests. Logs: `build-all.log`, `build-final-source.log`, `instrument-tests.log`, `test-all.log`, `test-all.xml`, `build-production.log`.
- Full test command: `flock -s /root/jobs/fx/cx/measure.lock ctest --test-dir test-build -E '^test_cpp_(framework|stream_connector)_install_consumer$' --output-on-failure --stop-on-failure --output-junit test-all.xml`.
- Measurements: `bash measure.sh > measure.log 2>&1`. Each cell acquires `flock -o /root/jobs/fx/cx/measure.lock`; raw cells are in `measure/`, summaries in `summary.json`, and matching configuration/harness evidence in `measurement-conditions.json`.
- Engine integration test run files were archived in `engine-test-run/`; the generated `.run` directory was removed from the worktree. Engine source files are unchanged.
- Final format and whitespace checks: `flock -s /root/jobs/fx/cx/measure.lock bash scripts/format/format.sh --check cpp` and `git diff --check`, both exit 0 (`format.log`, `diff-check.log`).

## Remaining problems

All 30 cells completed with source/target count agreement, errors=0, abandoned=0, no contaminated cells and no drain-bound cells. The new production executable contains `wake_one_locked(std::optional<std::pair<unsigned long, unsigned long>>)`; the previous executable contains the old no-argument symbol (`linked-executor-symbols.log`). Thus the failed acceptance numbers above are from the new implementation, not the previous binary.

- request-backpressure does not meet the supplied acceptance criterion; the numbers and threshold are above.
- send-saturation does not meet the supplied acceptance criterion; the numbers and threshold are above.
- PM Spot p99 (µs) does not meet the supplied acceptance criterion; the numbers and threshold are above.
Stopped at these measured results for the supplied design. No additional scheduling rule was introduced after measurement.

Owner layer: C++ Framework mailbox and application executor.  
Spec authority: S/04 §3 ready-owner conditions; S/02 §9; S/08 §4.3 W5; API §11.  
Cross-language result: read-only .NET/Java/Kotlin/Node.js comparison above; implementation remains within the explicitly approved C++ scope.  
Change classification: A — supervisor-approved contract adaptation for backlog growth and helper rotation.
