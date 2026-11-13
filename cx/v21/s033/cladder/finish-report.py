from pathlib import Path
import json,statistics,subprocess,math
J=Path(__file__).parent
subprocess.run(['python3',str(J/'write-report.py')],check=True)
s=json.loads((J/'summary.json').read_text());p=json.loads((J/'profiles.json').read_text());text=(J/'report.md').read_text()
base=s['rung0']['throughput'];ref=s['binding-reference-window100']['throughput']
conclusion=f'''**The requested window-100 reproduction passes:** standalone {base:,.1f} versus existing-benchmark binding {ref:,.1f} msg/s, {100*(base/ref-1):+.2f}%. The unchanged unlimited binding benchmark also reproduces the published cell ({s['binding-reference']['throughput']:,.1f}, {100*(s['binding-reference']['throughput']/246826.1-1):+.2f}%). The standalone unlimited case is {100*(s['rung0-unbounded']['throughput']/s['binding-reference']['throughput']-1):+.2f}% faster than that benchmark, so it is **not** a 5%-matched replacement for the published unlimited denominator. The changed control host/link closure and request window are disclosed rather than treating these two experiments as interchangeable.'''
text=text.replace('<!-- BASELINE_CONCLUSION -->',conclusion)
S='/root/worktree/zlink-3867s/framework/doc/framework/common/spec/server/'
C='/root/worktree/zlink-3867s/framework/languages/cpp/framework/'
analysis='''## Largest throughput losses: functions, required behavior and lower-cost representations

The two largest absolute throughput losses are rungs 1 and 2. Rung 1 loses 66,221 msg/s; rung 2 loses a further 72,514. These are cumulative-composition experiments. They do not prove that removing an isolated library function recovers the entire rate loss: a slower producer/reply producer changes Core batching and kernel work too. Rung 2 also changes the completion pump and benchmark observation loop; its loss is **not** an isolated `raw_route_port_t` measurement.

Perf uses CPU-clock at 199 Hz with 16 KiB DWARF stacks. Physical call stacks are decoded with `--no-inline`; compiler inline subfunctions are not individually resolved. The captured stack weights are scaled to each thread's 12 s `/proc` CPU window, following the earlier collectors. The record starts approximately 0.2 s before that window. Perf and `/proc` timestamps have different clock offsets here, so timestamps are not incorrectly compared as one absolute clock. The small recording margin stays within steady active traffic. Inclusive costs below overlap. Exclusive category totals are separately available in `profiles.json`.

### Rung 1 — real envelope work appears immediately on the caller and target receive thread

Unprofiled caller demand grows **4.404 → 6.178 µs/message**. In the profiled caller, header decode encloses **0.809 µs**, header encode **0.600 µs**; the complete codec category is **1.566 µs**. On target receive, decode encloses **1.128 µs**, encode **0.678 µs**. These are real codec calls, not a substituted JSON implementation.

| Responsible file:function | Required work / governing server clause | Concrete lower-cost form | Conditional CPU saving |
|---|---|---|---|
| `src/runtime/messaging/envelope_codec.cpp:envelope_codec_t::decode_header`, `canonical_header_reader_t::string`, `header_sax_t::finish` | [Message model:12](SPEC00-foundation/05-message-model.ko.md:12), [Payload ownership/codec:130](SPEC01-execution/05-payload-ownership-and-codec.ko.md:130), [Wire protocol:45](SPEC02-channel-transport/06-wire-protocol.ko.md:45) require shared bytes, validation and typed codec behavior. **No clause** requires eagerly owning every header string while the existing record already owns the header buffer. | Replace the existing internal decoded-header storage with buffer-backed views for validation/routing, and materialize only strings whose public context must outlive that buffer. Retain the original header through its existing `message_parts_t`/record owner. Preserve malformed-input, metadata, flow-Off and cross-language rules. | **0.2–0.4 µs caller**, **0.3–0.6 µs target receive**, estimated from a 25–40% cut to the measured header decode scope. |
| `src/runtime/messaging/envelope_codec.cpp:envelope_codec_t::encode_header`, `write_header`, `header_writer_t::append` | Same wire and codec clauses. **No clause** prescribes the current owning intermediate values or two visits to escaped dynamic fields. | Within the existing codec owner, reuse already validated dynamic field views and remove repeated temporary ownership before final native output. The stable-prefix plan already exists at `envelope_codec.cpp:324`; “add prefix caching” is not a new proposal. Do not replace the envelope with a fixed benchmark header. | A further **0.1–0.2 µs per encode** is a representation target, not a measured patch gain. |

The combined conservative envelope target is about **0.3–0.6 µs on the source caller** and **0.4–0.7 µs on target receive**; the two rows are not independently additive beyond that range. No saving is promised for unchanged Core I/O work. A shorter producer path could improve batching, which must be measured separately.

### Rung 2 — public task/admission delivery and the raw-port boundary add more than the registry

Before any registry entry is registered, caller demand grows **6.178 → 11.357 µs/message**. A new source poller consumes **11.153 µs/message**, and completion workers consume **7.805 µs/message in aggregate** in the unprofiled median. The poller, caller and hotter Core I/O thread are already serial demands above the 0.6 budget. A new logical pending entry is not the cause of this first large loss.

In the profiled caller, allocation accounts for **3.125 µs**, handoff/synchronization **1.294 µs**, task/future internals **0.851 µs**, and benchmark bookkeeping **0.665 µs**. `request_task_access_t::receive` encloses **9.427 µs** and `raw_route_port_t::request_impl` **5.264 µs**, including the binding submit; those inclusive numbers cannot be added. On the poller, `raw_route_port_t::poll` encloses **11.177 µs**, the binding completion drain **9.741 µs**, and the real `observe_request_completion` coroutine about **7.280 µs**. This shows where Framework continuation/receiver work executes inside the native completion-drain stack; it is not all transport polling.

| Responsible file:function | Required work / governing server clause | Concrete lower-cost form | Conditional CPU saving |
|---|---|---|---|
| `include/zlink/framework/contracts/dispatch/request_task.hpp:request_task_access_t::receive`, `request_task_t::receiver`; `task.hpp:task_shared_state_t::on_completed` | [Submit/completion:61](SPEC01-execution/01-submit-and-completion.ko.md:61), [single receiver/pending:445](SPEC01-execution/01-submit-and-completion.ko.md:445), [new callback turn:490](SPEC01-execution/01-submit-and-completion.ko.md:490). Admission, physical lifetime, caller abandonment and logical terminal are distinct facts. **No clause** requires a separate shared allocation for every wrapper/callable that carries them. | Co-allocate the existing admission/public/physical receiver ownership in one request-owned block, removing the corresponding standalone allocations and shared-owner copies. Keep the distinct facts and one terminal authority. Keep the existing binding await bridge and independent callbacks; do not add a binding callback API or bypass its completion drain. | **1.0–1.8 µs caller** as a first representation target; bounded by the measured allocation increase and task scope, not by deleting the whole bridge. |
| `src/runtime/backend/raw_binding_adapter.hpp:observe_request_completion`; `raw_route_port.cpp:raw_route_port_t::request_impl`; `provider_task_context.cpp:dispatch_completion` | [Submit/completion:490](SPEC01-execution/01-submit-and-completion.ko.md:490) and [execution placement:300](SPEC01-execution/08-messaging-hot-path.ko.md:300). **No clause** requires repeatedly boxing the same receiver or allocating an Asio handler owner at every projection. Core/binding retain native route and drain decisions. | Keep one stable receiver capture in the existing observer/request lifetime and project results directly into it. Preserve the required new turn through the existing dispatcher slot; remove intermediate callable owners rather than adding a second queue, generation or poller. | **0.6–1.2 µs on the source poller**, plus a separately measured opportunity in aggregate dispatcher CPU. Do not count dispatcher aggregate as one serial-thread saving. |

These estimates are unimplemented hypotheses. The rung-2 glue includes one job-local `terminal_t` allocation and the Framework benchmark's completion-notification loop; neither may be silently called product registry cost. Perf also perturbs completion-worker CPU materially: rung-2 profiled worker demand is about 12 µs/message versus 7.8 unprofiled. Caller estimates therefore stay below its **5.179 µs** measured incremental demand rather than claiming the whole profiled synchronization category as recoverable.

### What the middle rungs establish

The registry adds approximately **1.254 µs caller** (rung 2 → 3); the scheduled owner adds approximately **0.094 µs caller** (3 → 4) and only **0.36%** throughput loss. That owner already executes idle-lane turns on the caller; adding another “inline owner” mechanism would duplicate existing behavior. Registry completion/deadline behavior remains required by [Submit/completion:445](SPEC01-execution/01-submit-and-completion.ko.md:445); state-lane exclusivity and C1/C2 boundaries remain required by [State ownership:90](SPEC01-execution/06-state-ownership-and-lanes.ko.md:90).

Mailbox/permit and executor work are owned by the actual mailbox/queue/executor code. Permit reservation precedes binding receive, the real mailbox-ready callback claims the FIFO prefix, and permit return occurs at the actual handler-entry callback once typed invocation is enabled. The executor rung improves throughput while increasing target total CPU. That is a batching/resource-placement effect, not negative CPU work. See [Ingress permit sequence:116](SPEC01-execution/04-application-job-queue-and-backpressure.ko.md:116), [receive batch:287](SPEC01-execution/04-application-job-queue-and-backpressure.ko.md:287) and [handler placement:300](SPEC01-execution/08-messaging-hot-path.ko.md:300).

The target statistics listener is a separate measured consumer: in rung 1 it uses about 3.0 profiled µs/message, mostly `server_metrics_t::snapshot_json` percentile `std::__introsort_loop` (2.56 µs). It is benchmark observation work, **not Framework request dispatch**, and is shown separately in the per-thread data.
'''.replace('SPEC',S)
text=text.replace('<!-- TOP_RUNG_ANALYSIS -->',analysis)
# Full composition / residual: no telescoping assertion is used as proof.
r8=s['rung8'];r9=s['rung9'];r0=s['rung0'];gap=100*(r8['throughput']/r9['throughput']-1)
recon=f'''## Reconciliation with the full Framework cell

Rungs 0–8 finish at **{r8['throughput']:,.1f} msg/s**; the complete public API runs at **{r9['throughput']:,.1f}**. The intermediate composition is **{gap:.2f}% faster** than the full cell, so the difference exceeds 10% and is not treated as closed.

| Demand | Rung 0 + measured changes through rung 8 | Full rung 9 | Integration residual |
|---|---:|---:|---:|
| Source total CPU µs/message | {r8['endpoint_us']['source']:.3f} | {r9['endpoint_us']['source']:.3f} | {r9['endpoint_us']['source']-r8['endpoint_us']['source']:+.3f} |
| Target total CPU µs/message | {r8['endpoint_us']['target']:.3f} | {r9['endpoint_us']['target']:.3f} | {r9['endpoint_us']['target']-r8['endpoint_us']['target']:+.3f} |
| Source caller CPU µs/message | {r8['groups']['source'].get('caller',0):.3f} | {r9['groups']['source'].get('caller',0):.3f} | {r9['groups']['source'].get('caller',0)-r8['groups']['source'].get('caller',0):+.3f} |
| Inverse throughput µs/message | {1e6/r8['throughput']:.3f} | {1e6/r9['throughput']:.3f} | {1e6/r9['throughput']-1e6/r8['throughput']:+.3f} |

Adding adjacent throughput deltas would telescope by construction; that is not a validation that costs are independent. The real check is the independently run full endpoint and the residual shown here. Canonical service-wire bytes alone do not restore public facade/result projections, real peer/liveness/source identity checks, the production receive/management loop, dispatch preparation, terminal/scope ownership and reply adapters. The component harness also has simpler control plumbing and mailbox scheduling. Those are restored together only at rung 9, so this report does not falsely assign their aggregate to one function.

<!-- FULL_PROFILE_DETAILS -->

Using the matched standalone window-100 denominator, 0.6 means {0.6*base:,.1f} msg/s and **{1e6/(.6*base):.3f} µs** per serial thread. Using the published unlimited binding denominator gives the original **6.752 µs** budget. Rung 1's caller is already near either budget, and rung 2 exceeds both before the logical registry is added. The measured ladder does not demonstrate a 0.6 implementation, and the conditional savings estimates are not additive promises to reach it.
'''
text=text.replace('<!-- RECONCILIATION -->',recon)
limits='''- No product runtime, public API or spec was changed; no commit, push, branch mutation or GitHub operation was performed.
- The target mailbox/executor intermediate harness uses the real owner-FIFO readiness/claim functions but not the whole production ingress/management driver until rung 9. The one-component-at-a-time diagnostic path is not claimed to satisfy every public runtime contract at its early rungs.
- Peer/liveness checks, public projections and production dispatch integration were not split into separate intermediate rungs. They are a measured integration residual, with enclosing function costs from the full perf capture. No ChannelName selector is measured for this explicit-RID request.
- CPU savings are estimates from sampled enclosing work and actual caller deltas; no candidate was implemented or benchmarked. Some functions are inlined and only their physical enclosing frame is visible.
- The first full perf attempt timed out during post-record cache processing and lacks complete counter captures; it is preserved under `failed/perf-rung9-cache/` and excluded. The corrected collector disables build-ID cache updates and saves `/proc` captures before recorder shutdown. The initial job-specific default-cache entries were relocated to `perf-default-cache/`, and their four global build-ID links were removed (`perf-cache-correction.json`). Subsequent perf cache paths are local to this job.
- Send-saturation is measured for the unchanged binding benchmark and full public API endpoints only; a separate cumulative send ladder was not built. Request-registry/deadline rungs do not automatically represent the send path.

리팩토링 점검: 성능 3건, POSDDD 0건, 불필요 코드 0건 발견. 고친 항목: 없음. 넘긴 항목: envelope 문자열 처리, task·completion 객체와 전달 비용, 전체 경로의 추가 통합 비용. 제품 코드 수정 전 설계·소유권 검토와 검증이 필요하다.'''
text=text.replace('<!-- LIMITS -->',limits)
(J/'report.md').write_text(text)
print('filled report analyses; full-profile detail placeholder awaits explicit reviewed evidence')
