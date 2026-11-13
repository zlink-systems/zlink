# C++ bottom-up request layer ladder — 0.33.0 / #1639

**The first large costs appear in envelope processing and then the Framework task/admission/completion boundary, before the logical registry is added.** At window 100, throughput is 223,019 → 156,798 → 84,284 msg/s, with caller demand 4.404 → 6.178 → 11.357 µs/request. The full API reaches 45,051 msg/s (F/B 0.2020). The unchanged unlimited-window comparison is **49,558.2/246,721.5 = 0.2009**, reproducing the published Framework cell within 0.09%.

Source: `/root/worktree/zlink-3867s`, HEAD `c8869b63b19c8504a727459c766bf3ccee995708`. This HEAD has the same file tree as `e86f55886a08f4870913bf781b4f515c3239283f`, used in the prior report. Product files are unchanged. All new sources, builds, runners and data are under this directory. No sub-agents, other models, docs synchronization, repository mutation, GitHub access, pull or fetch was used.

## Measurement and interpretation

Source CPUs 0–7; target CPUs 8–15. Each reported rung has three independent process pairs, 30 s warm-up and 20 s active time. Throughput is the median of the complete active phases. Per-thread CPU is the median of the 100 ms `/proc/<pid>/task/<tid>/schedstat` timelines over a 12 s interior window, divided by completed requests in that same window. “msg/s” counts completed logical requests, not both physical directions or multipart frames. Send CPU is normalized by same-window target receives. CPU time is not latency. Same-name threads are ranked by CPU within each run before taking medians; all TIDs and each run are in `threads.csv` and `summary.json`. Independently taken medians need not sum exactly. A thread that retires between snapshots can lose its last sampling interval.

Builds use the shared measure lock and `cpp-build-slot.sh`, with one compiler per build (at most two build slots; below the requested parallelism ceiling of three). Measurements and perf use the exclusive `flock -o /root/jobs/fx/cx/measure.lock`. The collector checks MemAvailable >= 12 GiB at every snapshot. Endpoint and perf cleanup uses SIGTERM/SIGINT only.

The existing `cthin/run-B.sh`, `runner-common.sh` and `collect.py` were copied locally. Rung 0 removes Framework from the executable link closure and uses a small POSIX HTTP listener for control; the binding request/echo pump, Protobuf payload, 29-byte body validation, metrics, phases and count checks are copied from the repository benchmark. `nm` checks there are no Framework runtime symbols in this executable. Rungs 1–8 use the same local control listener; rung 9 runs the real public benchmark and its real host. Production libraries were copied from the current `csubmit/build`; all 13 recorded source hashes match current sources (`manifest.json`). The benchmark metadata still labels Framework 0.32.0; HEAD and source hashes, not that inherited string, identify this 0.33.0 investigation.

## Baseline check: the requested window differs from the published cell

`framework/bench/grpc/cpp/client/bench_cpp_client.cpp:1012` returns an unlimited window for `request-backpressure`; window 100 belongs to `request-window`. This ladder explicitly forces 100 without changing the public request path. `LADDER_UNBOUNDED=1` enables the separate comparison. The published 246,826.1 binding and 49,602.0 Framework values are unlimited-window, 5 s active references; these new active phases last 20 s. A 5% reproduction claim must state which of these conditions it compares.

| Case | Runs | msg/s, median | Difference from published same implementation |
|---|---:|---:|---:|
| Unchanged existing binding benchmark, unlimited | 3 | 246,721.5 | -0.04% |
| Standalone binding, window 100 | 3 | 223,019.2 | -9.65% |
| Standalone binding, unlimited | 3 | 273,358.9 | +10.75% |
| Existing benchmark binding path, window 100 | 3 | 218,388.6 | -11.52% |
| Full public Framework, unlimited | 3 | 49,558.2 | -0.09% |

**The requested window-100 reproduction passes:** standalone 223,019.2 versus existing-benchmark binding 218,388.6 msg/s, +2.12%. The unchanged unlimited binding benchmark also reproduces the published cell (246,721.5, -0.04%). The standalone unlimited case is +10.80% faster than that benchmark, so it is **not** a 5%-matched replacement for the published unlimited denominator. The changed control host/link closure and request window are disclosed rather than treating these two experiments as interchangeable.

## Cumulative rungs

The message path remains binding request/reply. The additions are actual Framework functions, linked from the copied production archive and instantiated from repository headers. The job code supplies glue and configurations; it does not reimplement the codec, registry, lane, mailbox, permit, executor or handler invocation. Intermediate rungs are diagnostic component compositions, not alternate supported Framework APIs or production optimizations.

Rung 1 also emits the actual canonical Framework header fields and real deadline/correlation values rather than the shorter fixed raw literal; its delta includes those byte/validation differences. Rung 2 changes to the real Framework benchmark’s task/admission observation loop and the real raw port, adding a dedicated source completion poller. Rung 3 separates the logical registry cost from that task bridge. Callback publication is measured with registry terminal completion rather than with unregister-only dummy work. Rung 4 adds the real scheduled owner; the ownerless rung 3 drives its real deadline management from the poller at `next_deadline()`. This keeps completed index references bounded.

The service-wire wrapper is added after the leaf operations so its packing cost is separately visible. In the real path it encloses the envelope and registry submission; it is not a second transport. Rung 9 restores the full composition, including peer/liveness checks, public result projections, real ingress/management turns and dispatch/reply adapters. The explicit `request_to_node` RID does not use the ChannelName selector; adding a generic route-selection algorithm would measure another API. The integration residual at rung 9 is disclosed separately below.

F/B below divides each rung by the standalone window-100 binding median. Δ CPU shows the change in total CPU demand at each endpoint; it is not a latency delta. Δ wall is the change in inverse throughput (µs/message).

| Rung | Added real work | Runs | msg/s | F/B | Rate Δ | Δ wall µs | Source CPU µs/msg | Target CPU µs/msg | Δ CPU S/T µs |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | C++ binding echo | 3 | 223,019.2 (220,794–225,433) | 1.0000 | — | — | 10.927 | 13.116 | — |
| 1 | Real envelope creation, encode and decode | 3 | 156,797.8 (152,407–164,643) | 0.7031 | -29.7% | +1.894 | 16.258 | 18.242 | +5.331/+5.125 |
| 2 | Real raw port + public task/admission/completion bridge | 3 | 84,283.5 (83,699–86,090) | 0.3779 | -46.2% | +5.487 | 51.824 | 34.406 | +35.565/+16.165 |
| 3 | Pending entry + deadline index + registry completion dispatcher | 3 | 76,901.9 (75,848–77,009) | 0.3448 | -8.8% | +1.139 | 59.285 | 36.588 | +7.461/+2.182 |
| 4 | Scheduled operation owner lane | 3 | 76,624.7 (76,360–76,974) | 0.3436 | -0.4% | +0.047 | 60.073 | 36.981 | +0.788/+0.393 |
| 5 | Target mailbox and ingress permit | 3 | 66,491.2 (66,225–67,105) | 0.2981 | -13.2% | +1.989 | 64.493 | 41.388 | +4.420/+4.408 |
| 6 | Application executor | 3 | 72,579.3 (72,059–72,891) | 0.3254 | +9.2% | -1.262 | 61.833 | 48.678 | -2.660/+7.289 |
| 7 | Typed serializer, DI, handler scope and filter invocation | 3 | 62,501.3 (62,469–63,383) | 0.2803 | -13.9% | +2.222 | 67.985 | 55.105 | +6.152/+6.428 |
| 8 | Canonical service-wire application and request/reply wrappers | 3 | 55,663.3 (55,661–56,279) | 0.2496 | -10.9% | +1.965 | 73.941 | 61.810 | +5.957/+6.705 |
| 9 | Full public API and production RouteMesh runtime | 3 | 45,050.7 (44,181–45,658) | 0.2020 | -19.1% | +4.232 | 91.766 | 72.469 | +17.825/+10.660 |

### Per-thread CPU µs/message

Every value >= 0.05 µs/message is shown. Comma-separated values are distinct threads sharing the displayed name, not a summed pool. The complete unrounded rows, including idle threads, are in `threads.csv`.

| Rung | Source threads | Target threads |
|---:|---|---|
| 0 | `ZLINKbg/IO/0` 3.66; `ZLINKbg/IO/2` 2.84; `caller` 4.40 | `ZLINKbg/IO/0` 3.09; `ZLINKbg/IO/1` 3.97; `ladder-http` 2.21; `receive` 3.82 |
| 1 | `ZLINKbg/IO/0` 5.39; `ZLINKbg/IO/2` 4.68; `caller` 6.18 | `ZLINKbg/IO/0` 4.38; `ZLINKbg/IO/1` 5.52; `ladder-http` 2.71; `receive` 5.44 |
| 2 | `ZLINKbg/IO/0` 11.30; `ZLINKbg/IO/2` 10.10; `bench_cpp_clien` 0.98,0.98,0.98,0.98,0.97,0.97,0.97,0.97; `caller` 11.36; `ladder-poll` 11.15 | `ZLINKbg/IO/0` 9.00; `ZLINKbg/IO/1` 10.89; `ladder-http` 3.56; `receive` 10.95 |
| 3 | `ZLINKbg/IO/0` 12.20; `ZLINKbg/IO/2` 9.87; `bench_cpp_clien` 1.69,1.68,1.68,1.68,1.68,1.68,1.68,1.67; `caller` 12.61; `ladder-poll` 11.30 | `ZLINKbg/IO/0` 9.62; `ZLINKbg/IO/1` 11.63; `ladder-http` 3.66; `receive` 11.77 |
| 4 | `ZLINKbg/IO/0` 12.22; `ZLINKbg/IO/2` 10.11; `bench_cpp_clien` 1.69,1.69,1.69,1.69,1.69,1.68,1.68,1.68; `caller` 12.70; `ladder-poll` 11.22 | `ZLINKbg/IO/0` 9.82; `ZLINKbg/IO/1` 11.64; `ladder-http` 3.68; `receive` 11.81 |
| 5 | `ZLINKbg/IO/0` 13.58; `ZLINKbg/IO/2` 11.50; `bench_cpp_clien` 1.59,1.58,1.58,1.58,1.58,1.58,1.57,1.57; `caller` 14.01; `ladder-poll` 12.36 | `ZLINKbg/IO/0` 10.59; `ZLINKbg/IO/1` 13.18; `ladder-http` 3.82; `receive` 13.79 |
| 6 | `ZLINKbg/IO/0` 12.73; `ZLINKbg/IO/2` 9.74; `bench_cpp_clien` 1.79,1.79,1.79,1.79,1.79,1.78,1.78,1.78; `caller` 13.25; `ladder-poll` 11.40 | `ZLINKbg/IO/0` 10.23; `ZLINKbg/IO/1` 11.74; `bench_cpp_zlink` 1.97,1.75,1.66,1.56,1.50,1.49,1.38,1.35; `ladder-http` 3.89; `receive` 10.13 |
| 7 | `ZLINKbg/IO/0` 14.12; `ZLINKbg/IO/2` 12.76; `bench_cpp_clien` 1.70,1.70,1.69,1.69,1.69,1.69,1.69,1.68; `caller` 14.58; `ladder-poll` 12.84 | `ZLINKbg/IO/0` 11.75; `ZLINKbg/IO/1` 13.64; `bench_cpp_zlink` 2.41,2.32,1.92,1.83,1.70,1.65,1.46,1.30; `ladder-http` 3.98; `receive` 10.71 |
| 8 | `ZLINKbg/IO/0` 15.79; `ZLINKbg/IO/2` 14.53; `bench_cpp_clien` 1.58,1.58,1.57,1.57,1.57,1.57,1.57,1.56; `caller` 16.35; `ladder-poll` 14.60 | `ZLINKbg/IO/0` 13.78; `ZLINKbg/IO/1` 15.31; `bench_cpp_zlink` 2.66,2.39,2.37,2.21,2.16,2.11,1.65,1.38; `ladder-http` 4.05; `receive` 11.64 |
| 9 | `ZLINKbg/IO/0` 18.23; `ZLINKbg/IO/2` 16.94; `bench_cpp_clien` 18.39,2.20,2.20,2.20,2.20,2.20,2.20,2.19,2.19,0.15,0.15; `caller` 20.15 | `ZLINKbg/IO/0` 13.67; `ZLINKbg/IO/1` 17.87; `app-worker` 8.13,5.47,5.15,1.38; `bench_cpp_frame` 0.55,0.53,0.52,0.51,0.50,0.47,0.47,0.45,0.16; `receive` 15.89 |

### Where each incremental CPU demand appears

Each row is the difference between three-run role medians, in µs/message. The full source receive role is assigned to the hottest unnamed worker by the independent full perf stack; the remaining unnamed source workers are mostly the completion dispatcher. Lower target worker aggregates use their inherited benchmark name. Small control/main-thread CPU remains in the other columns. Rungs 5 and 6 change only the target: changes in their source CPU are effects of timing/batching, not newly added source code. These conditional deltas must not be read as independent function self-costs.

| Rung | S caller Δ | S poll/receive Δ | S other workers Δ | S Core I/O Δ | T receive Δ | T app/worker Δ | T Core I/O Δ | T control/other Δ |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | +1.774 | +0.000 | +0.000 | +3.572 | +1.616 | +0.000 | +2.964 | +0.491 |
| 2 | +5.180 | +11.153 | +7.804 | +11.332 | +5.515 | -0.001 | +9.870 | +0.853 |
| 3 | +1.254 | +0.147 | +5.636 | +0.590 | +0.818 | +0.000 | +1.258 | +0.105 |
| 4 | +0.094 | -0.084 | +0.104 | +0.390 | +0.036 | +0.000 | +0.338 | +0.015 |
| 5 | +1.311 | +1.147 | -0.846 | +2.857 | +1.983 | +0.000 | +2.287 | +0.142 |
| 6 | -0.764 | -0.964 | +1.659 | -2.631 | -3.660 | +12.525 | -1.745 | +0.068 |
| 7 | +1.328 | +1.438 | -0.755 | +4.336 | +0.579 | +2.244 | +3.536 | +0.088 |
| 8 | +1.769 | +1.767 | -0.963 | +3.382 | +0.932 | +2.324 | +3.607 | +0.076 |
| 9 | +3.798 | +3.789 | +5.396 | +4.855 | +4.251 | +3.800 | +2.375 | +0.162 |

Absolute per-role medians and every underlying thread remain in `summary.json`/`threads.csv`. Across rungs, different queue occupancy and callback placement can move existing work as well as add it.

## Largest throughput losses: functions, required behavior and lower-cost representations

Code paths below are relative to `framework/languages/cpp/framework/`; abbreviated `mesh/`, `backend/` and bare runtime filenames refer to `src/runtime/`. Spec links point to the read-only canonical server documents.

The two largest absolute throughput losses are rungs 1 and 2. Rung 1 loses 66,221 msg/s; rung 2 loses a further 72,514. These are cumulative-composition experiments. They do not prove that removing an isolated library function recovers the entire rate loss: a slower producer/reply producer changes Core batching and kernel work too. Rung 2 also changes the completion pump and benchmark observation loop; its loss is **not** an isolated `raw_route_port_t` measurement.

Perf uses CPU-clock at 199 Hz with 16 KiB DWARF stacks. Physical call stacks are decoded with `--no-inline`; compiler inline subfunctions are not individually resolved. The captured stack weights are scaled to each thread's 12 s `/proc` CPU window, following the earlier collectors. The record starts approximately 0.2 s before that window. Perf and `/proc` timestamps have different clock offsets here, so timestamps are not incorrectly compared as one absolute clock. The small recording margin stays within steady active traffic. Inclusive costs below overlap. Exclusive category totals are separately available in `profiles.json`.

### Rung 1 — real envelope work appears immediately on the caller and target receive thread

Unprofiled caller demand grows **4.404 → 6.178 µs/message**. In the profiled caller, header decode encloses **0.809 µs**, header encode **0.600 µs**; the complete codec category is **1.566 µs**. On target receive, decode encloses **1.128 µs**, encode **0.678 µs**. These are real codec calls, not a substituted JSON implementation.

| Responsible file:function | Required work / governing server clause | Concrete lower-cost form | Conditional CPU saving |
|---|---|---|---|
| `src/runtime/messaging/envelope_codec.cpp:envelope_codec_t::decode_header`, `canonical_header_reader_t::string`, `header_sax_t::finish` | [Message model:12](/root/worktree/zlink-3867s/framework/doc/framework/common/spec/server/00-foundation/05-message-model.ko.md:12), [Payload ownership/codec:130](/root/worktree/zlink-3867s/framework/doc/framework/common/spec/server/01-execution/05-payload-ownership-and-codec.ko.md:130), [Wire protocol:45](/root/worktree/zlink-3867s/framework/doc/framework/common/spec/server/02-channel-transport/06-wire-protocol.ko.md:45) require shared bytes, validation and typed codec behavior. **No clause** requires eagerly owning every header string while the existing record already owns the header buffer. | Replace the existing internal decoded-header storage with buffer-backed views for validation/routing, and materialize only strings whose public context must outlive that buffer. Retain the original header through its existing `message_parts_t`/record owner. Preserve malformed-input, metadata, flow-Off and cross-language rules. | **0.2–0.4 µs caller**, **0.3–0.6 µs target receive**, estimated from a 25–40% cut to the measured header decode scope. |
| `src/runtime/messaging/envelope_codec.cpp:envelope_codec_t::encode_header`, `write_header`, `header_writer_t::append` | Same wire and codec clauses. **No clause** prescribes the current owning intermediate values or two visits to escaped dynamic fields. | Within the existing codec owner, reuse already validated dynamic field views and remove repeated temporary ownership before final native output. The stable-prefix plan already exists at `envelope_codec.cpp:324`; “add prefix caching” is not a new proposal. Do not replace the envelope with a fixed benchmark header. | A further **0.1–0.2 µs per encode** is a representation target, not a measured patch gain. |

The combined conservative envelope target is about **0.3–0.6 µs on the source caller** and **0.4–0.7 µs on target receive**; the two rows are not independently additive beyond that range. No saving is promised for unchanged Core I/O work. A shorter producer path could improve batching, which must be measured separately.

### Rung 2 — public task/admission delivery and the raw-port boundary add more than the registry

Before any registry entry is registered, caller demand grows **6.178 → 11.357 µs/message**. A new source poller consumes **11.153 µs/message**, and completion workers consume **7.805 µs/message in aggregate** in the unprofiled median. The poller, caller and hotter Core I/O thread are already serial demands above the 0.6 budget. A new logical pending entry is not the cause of this first large loss.

In the profiled caller, allocation accounts for **3.125 µs**, handoff/synchronization **1.294 µs**, task/future internals **0.851 µs**, and benchmark bookkeeping **0.665 µs**. `request_task_access_t::receive` encloses **9.427 µs** and `raw_route_port_t::request_impl` **5.264 µs**, including the binding submit; those inclusive numbers cannot be added. On the poller, `raw_route_port_t::poll` encloses **11.177 µs**, the binding completion drain **9.741 µs**, and the real `observe_request_completion` coroutine about **7.280 µs**. This shows where Framework continuation/receiver work executes inside the native completion-drain stack; it is not all transport polling.

| Responsible file:function | Required work / governing server clause | Concrete lower-cost form | Conditional CPU saving |
|---|---|---|---|
| `include/zlink/framework/contracts/dispatch/request_task.hpp:request_task_access_t::receive`, `request_task_t::receiver`; `task.hpp:task_shared_state_t::on_completed` | [Submit/completion:61](/root/worktree/zlink-3867s/framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:61), [single receiver/pending:445](/root/worktree/zlink-3867s/framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:445), [new callback turn:490](/root/worktree/zlink-3867s/framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:490). Admission, physical lifetime, caller abandonment and logical terminal are distinct facts. **No clause** requires a separate shared allocation for every wrapper/callable that carries them. | Co-allocate the existing admission/public/physical receiver ownership in one request-owned block, removing the corresponding standalone allocations and shared-owner copies. Keep the distinct facts and one terminal authority. Keep the existing binding await bridge and independent callbacks; do not add a binding callback API or bypass its completion drain. | **1.0–1.8 µs caller** as a first representation target; bounded by the measured allocation increase and task scope, not by deleting the whole bridge. |
| `src/runtime/backend/raw_binding_adapter.hpp:observe_request_completion`; `raw_route_port.cpp:raw_route_port_t::request_impl`; `provider_task_context.cpp:dispatch_completion` | [Submit/completion:490](/root/worktree/zlink-3867s/framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:490) and [execution placement:300](/root/worktree/zlink-3867s/framework/doc/framework/common/spec/server/01-execution/08-messaging-hot-path.ko.md:300). **No clause** requires repeatedly boxing the same receiver or allocating an Asio handler owner at every projection. Core/binding retain native route and drain decisions. | Keep one stable receiver capture in the existing observer/request lifetime and project results directly into it. Preserve the required new turn through the existing dispatcher slot; remove intermediate callable owners rather than adding a second queue, generation or poller. | **0.6–1.2 µs on the source poller**, plus a separately measured opportunity in aggregate dispatcher CPU. Do not count dispatcher aggregate as one serial-thread saving. |

These estimates are unimplemented hypotheses. The rung-2 glue includes one job-local `terminal_t` allocation and the Framework benchmark's completion-notification loop; neither may be silently called product registry cost. Perf also perturbs completion-worker CPU materially: rung-2 profiled worker demand is about 12 µs/message versus 7.8 unprofiled. Caller estimates therefore stay below its **5.179 µs** measured incremental demand rather than claiming the whole profiled synchronization category as recoverable.

### What the middle rungs establish

The registry adds approximately **1.254 µs caller** (rung 2 → 3); the scheduled owner adds approximately **0.094 µs caller** (3 → 4) and only **0.36%** throughput loss. That owner already executes idle-lane turns on the caller; adding another “inline owner” mechanism would duplicate existing behavior. Registry completion/deadline behavior remains required by [Submit/completion:445](/root/worktree/zlink-3867s/framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:445); state-lane exclusivity and C1/C2 boundaries remain required by [State ownership:90](/root/worktree/zlink-3867s/framework/doc/framework/common/spec/server/01-execution/06-state-ownership-and-lanes.ko.md:90).

Mailbox/permit and executor work are owned by the actual mailbox/queue/executor code. Permit reservation precedes binding receive, the real mailbox-ready callback claims the FIFO prefix, and permit return occurs at the actual handler-entry callback once typed invocation is enabled. The executor rung improves throughput while increasing target total CPU. That is a batching/resource-placement effect, not negative CPU work. See [Ingress permit sequence:116](/root/worktree/zlink-3867s/framework/doc/framework/common/spec/server/01-execution/04-application-job-queue-and-backpressure.ko.md:116), [receive batch:287](/root/worktree/zlink-3867s/framework/doc/framework/common/spec/server/01-execution/04-application-job-queue-and-backpressure.ko.md:287) and [handler placement:300](/root/worktree/zlink-3867s/framework/doc/framework/common/spec/server/01-execution/08-messaging-hot-path.ko.md:300).

The target statistics listener is a separate measured consumer: in rung 1 it uses about 3.0 profiled µs/message, mostly `server_metrics_t::snapshot_json` percentile `std::__introsort_loop` (2.56 µs). It is benchmark observation work, **not Framework request dispatch**, and is shown separately in the per-thread data.


## Reconciliation with the full Framework cell

Rungs 0–8 finish at **55,663.3 msg/s**; the complete public API runs at **45,050.7**. The intermediate composition is **23.56% faster** than the full cell, so the difference exceeds 10% and is not treated as closed.

| Demand | Rung 0 + measured changes through rung 8 | Full rung 9 | Integration residual |
|---|---:|---:|---:|
| Source total CPU µs/message | 73.941 | 91.766 | +17.825 |
| Target total CPU µs/message | 61.810 | 72.469 | +10.660 |
| Source caller CPU µs/message | 16.351 | 20.148 | +3.798 |
| Inverse throughput µs/message | 17.965 | 22.197 | +4.232 |

Adding adjacent throughput deltas would telescope by construction; that is not a validation that costs are independent. The real check is the independently run full endpoint and the residual shown here. Canonical service-wire bytes alone do not restore public facade/result projections, real peer/liveness/source identity checks, the production receive/management loop, dispatch preparation, terminal/scope ownership and reply adapters. The component harness also has simpler control plumbing and mailbox scheduling. Those are restored together only at rung 9, so this report does not falsely assign their aggregate to one function.

### Full-path function evidence and the formerly unattributed source role

The full perf capture resolves the extra source execution role: **`mesh_node_host_service_t::node_receive_role_t::run/rotate`**, in `src/runtime/mesh/mesh_node_host_service.cpp:2073`. It uses **17.333 profiled µs/message**, with `raw_route_port_t::poll` enclosing **14.273 µs**. It is the Framework receive/completion role hosted by an offload worker, separate from the caller and both active Core I/O threads. This identifies the role corresponding to the earlier unexplained hot source thread; it does not assert that a TID from another run is the same thread.

| Full-path location | Function evidence, profiled µs/message | Required behavior / server clause | Lower-cost representation to test |
|---|---|---|---|
| `src/runtime/channels/channel_runtime.cpp:route_client_t::submit_request_reply_message_erased`; `mesh/mesh_node_runtime.cpp:mesh_node_runtime_t::submit_node_request_reply`; `mesh/raw_mesh_node_owner.cpp:request_with_header` | Caller inclusive scopes **16.355**, **11.826**, **10.629**, respectively. Caller exclusive allocation **4.369**, Framework bookkeeping **4.412**. `state_lane_t::schedule_drain` encloses 10.416 because it encloses the request body; the isolated lane rung costs about 0.094 caller µs, so 10.416 is not “lane overhead.” | [Source E1–E3:66](/root/worktree/zlink-3867s/framework/doc/framework/common/spec/server/01-execution/08-messaging-hot-path.ko.md:66), [single pending:445](/root/worktree/zlink-3867s/framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:445), [new callback turn:490](/root/worktree/zlink-3867s/framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:490). **No clause** requires the present number of erased callable/receiver owners and projections. | Carry the existing final receiver through the existing raw operation entry and co-allocate its capture rather than reconstructing intermediate result/callable owners at each facade. A **1.0–2.0 µs caller** target is within the 3.797 µs observed rung-8→9 caller residual. This overlaps the rung-2 receiver proposal; do not add the estimates. |
| `src/runtime/mesh/mesh_node_host_service.cpp:node_receive_role_t::rotate`; `backend/raw_route_port.cpp:raw_route_port_t::poll`; `mesh_node_runtime.cpp:submit_node_request_reply` completion lambda | Receive role **17.333**; poll **14.273**. Source completion dispatcher workers are separate (roughly 3.2 µs each in this profile). On one representative dispatcher worker, the `submit_node_request_reply` callback encloses **1.626 µs**, its next receiver projection **1.252**, and header decode **0.458**. These overlap. | [Receive progress/management:143](/root/worktree/zlink-3867s/framework/doc/framework/common/spec/server/01-execution/08-messaging-hot-path.ko.md:143), [callback independence:490](/root/worktree/zlink-3867s/framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:490), [receive-role placement:300](/root/worktree/zlink-3867s/framework/doc/framework/common/spec/server/01-execution/08-messaging-hot-path.ko.md:300). **No clause** requires repeated internal result boxing/repacking. Native polling and route decisions remain Core/binding-owned. | Preserve the already owned native result/header views through the current receiver instead of constructing another boxed carrier and decoding it again. A **1–2 µs aggregate dispatcher** target and **0.5–1.0 µs receive-role** target require a measured implementation; neither is a claim that the poll or completion drain can be removed. No direct Core-I/O saving is assumed. |
| `src/runtime/stateful/public_host_runtime.cpp:dispatch_ready/dispatch_application_claim`; `mesh/mesh_record_dispatcher.cpp:dispatch`; `mesh_node_host_service.cpp:dispatch_application` | Target receive **16.372**, `public_host_runtime_t::dispatch_ready` inclusive **12.405**, including `raw_mesh_node_owner_t::receive_next` **8.585**. Two marked app workers use **10.772 + 10.366** profiled µs/message; `mesh_record_dispatcher_t::dispatch` encloses **7.676/7.356** on those workers. | [Permit order:116](/root/worktree/zlink-3867s/framework/doc/framework/common/spec/server/01-execution/04-application-job-queue-and-backpressure.ko.md:116), [owner execution gate:76](/root/worktree/zlink-3867s/framework/doc/framework/common/spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md:76), [payload ownership:58](/root/worktree/zlink-3867s/framework/doc/framework/common/spec/server/01-execution/05-payload-ownership-and-codec.ko.md:58). **No clause** requires duplicate retained metadata/callable carriers between preparation and invocation. | Keep the one existing record/permit/terminal owner through preparation and invocation, moving its view instead of reconstructing another carrier. Initial representation targets are **0.4–0.8 µs target receive** and **0.5–1.0 µs aggregate app-worker CPU**. Preserve handler-entry permit timing, FIFO claims and independent progress. |

The full-profile categories are source Core/binding **45.009**, allocation **11.041**, codec **7.356**, handoff/synchronization **17.108** and Framework bookkeeping **13.081 µs/message**; target Core/binding is **45.057**. These are normalized profiled totals, not the unprofiled medians or differences against a profiled rung 8. In particular, packet cadence changes Core/socket/kernel work. The extra wrapper/dispatcher/receive-preparation scopes locate the integration work; they do not uniquely apportion every µs of the 23.56% throughput residual. Another sub-ladder would be needed to isolate each remaining facade and control-plane check. The reported gap remains explicit.


Using the matched standalone window-100 denominator, 0.6 means 133,811.5 msg/s and **7.473 µs** per serial thread. Using the published unlimited binding denominator gives the original **6.752 µs** budget. Rung 1's caller is already near either budget, and rung 2 exceeds both before the logical registry is added. The measured ladder does not demonstrate a 0.6 implementation, and the conditional savings estimates are not additive promises to reach it.


## Send-saturation 1K endpoint check

The two production benchmark endpoints were also measured with eight send slots, the same CPU sets, 30 s warm-up, three 20 s active phases and the same `/proc` capture method. This is an endpoint comparison; no intermediate send ladder is inferred from request-only registry/deadline rungs.

| Endpoint | Median msg/s | F/B | Source CPU µs/message | Target CPU µs/message | Source caller µs/message |
|---|---:|---:|---:|---:|---:|
| Binding benchmark | 595,181.3 | 1.0000 | 3.011 | 3.313 | 1.379 |
| Full public Framework | 175,520.4 | 0.2949 | 10.082 | 16.177 | 4.817 |

Relative to the published 616,974/173,204 msg/s references, these endpoints differ by -3.53%/+1.34%. All sends settle with zero errors and zero abandoned messages; server rejection accounting is checked by the original runner. For the current binding median, 0.6 is 357,108.8 msg/s and a 2.800 µs serial-thread budget. The original denominator gives 2.701 µs.

## Validation and raw data

**Final checks:** 51 valid measured cells (30 primary request-rung runs, 12 request baseline comparisons, three perf captures and six send endpoint runs); all count/payload/error/abandon/drain and affinity checks pass. The counter check accepts valid input and rejects a deliberately missing receive. Minimum recorded capture-start MemAvailable is **25.98 GiB**; every CPU snapshot additionally enforced the 12 GiB floor. Product source hashes still match and `git status --short` is empty. The existing binding benchmark and full Framework unlimited request endpoints reproduce the published cells by -0.04% and -0.09%, respectively.

- `src/`: standalone and component harnesses; `build.py` and `build-config.json`: exact compile/link dependency closure; `lib/`: copied production libraries.
- `raw/rung{0..9}/run{1..3}/<implementation>-request-backpressure-1024/`: result, endpoint logs, target snapshot and `capture/{before,after,samples,meta}.json`.
- `raw/send-binding-reference`, `raw/send-full`: send endpoint results and per-thread captures.
- `raw/binding-reference*`, `raw/rung0-unbounded`, `raw/rung9-unbounded`: independent baseline comparisons.
- `raw/perf-rung*/run1/.../capture/`: perf data, commands, stderr and decoded stack caches. `profiles.json`: CPU-normalized per-thread exclusive categories and inclusive/self function estimates. Inclusive scopes overlap and must not be summed.
- `summary.json`, `threads.csv`, `quality.json`, `manifest.json`: run-level rates, per-thread CPU, counter/affinity checks and source/library identity.
- `negative-count.json` and the positive/negative results in `quality.json`: the existing counter checker accepts the valid capture and rejects a deliberately missing target receive.
- `smoke/`, `smoke.log`, `smoke-final.log`: setup checks, including the first harness-only executor/mailbox claim failure and the corrected real mailbox-ready/settlement callback path. This was not a product-code failure. Failed setup data is excluded from all medians.

## Limits

- No product runtime, public API or spec was changed; no commit, push, branch mutation or GitHub operation was performed.
- The target mailbox/executor intermediate harness uses the real owner-FIFO readiness/claim functions but not the whole production ingress/management driver until rung 9. The one-component-at-a-time diagnostic path is not claimed to satisfy every public runtime contract at its early rungs.
- Peer/liveness checks, public projections and production dispatch integration were not split into separate intermediate rungs. They are a measured integration residual, with enclosing function costs from the full perf capture. No ChannelName selector is measured for this explicit-RID request.
- CPU savings are estimates from sampled enclosing work and actual caller deltas; no candidate was implemented or benchmarked. Some functions are inlined and only their physical enclosing frame is visible.
- The first full perf attempt timed out during post-record cache processing and lacks complete counter captures; it is preserved under `failed/perf-rung9-cache/` and excluded. The corrected collector disables build-ID cache updates and saves `/proc` captures before recorder shutdown. The initial job-specific default-cache entries were relocated to `perf-default-cache/`, and their four global build-ID links were removed (`perf-cache-correction.json`). Subsequent perf cache paths are local to this job.
- Send-saturation is measured for the unchanged binding benchmark and full public API endpoints only; a separate cumulative send ladder was not built. Request-registry/deadline rungs do not automatically represent the send path.

리팩토링 점검: 성능 3건, POSDDD 0건, 불필요 코드 0건 발견. 고친 항목: 없음. 넘긴 항목: envelope 문자열 처리, task·completion 객체와 전달 비용, 전체 경로의 추가 통합 비용. 제품 코드 수정 전 설계·소유권 검토와 검증이 필요하다.
