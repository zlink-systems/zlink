from pathlib import Path
import json,statistics,re
J=Path(__file__).parent;s=json.loads((J/'summary.json').read_text());p=json.loads((J/'profiles.json').read_text());text=(J/'report.md').read_text()
S='/root/worktree/zlink-3867s/framework/doc/framework/common/spec/server/'
full='''### Full-path function evidence and the formerly unattributed source role

The full perf capture resolves the extra source execution role: **`mesh_node_host_service_t::node_receive_role_t::run/rotate`**, in `src/runtime/mesh/mesh_node_host_service.cpp:2073`. It uses **17.333 profiled µs/message**, with `raw_route_port_t::poll` enclosing **14.273 µs**. It is the Framework receive/completion role hosted by an offload worker, separate from the caller and both active Core I/O threads. This identifies the role corresponding to the earlier unexplained hot source thread; it does not assert that a TID from another run is the same thread.

| Full-path location | Function evidence, profiled µs/message | Required behavior / server clause | Lower-cost representation to test |
|---|---|---|---|
| `src/runtime/channels/channel_runtime.cpp:route_client_t::submit_request_reply_message_erased`; `mesh/mesh_node_runtime.cpp:mesh_node_runtime_t::submit_node_request_reply`; `mesh/raw_mesh_node_owner.cpp:request_with_header` | Caller inclusive scopes **16.355**, **11.826**, **10.629**, respectively. Caller exclusive allocation **4.369**, Framework bookkeeping **4.412**. `state_lane_t::schedule_drain` encloses 10.416 because it encloses the request body; the isolated lane rung costs about 0.094 caller µs, so 10.416 is not “lane overhead.” | [Source E1–E3:66](SPEC01-execution/08-messaging-hot-path.ko.md:66), [single pending:445](SPEC01-execution/01-submit-and-completion.ko.md:445), [new callback turn:490](SPEC01-execution/01-submit-and-completion.ko.md:490). **No clause** requires the present number of erased callable/receiver owners and projections. | Carry the existing final receiver through the existing raw operation entry and co-allocate its capture rather than reconstructing intermediate result/callable owners at each facade. A **1.0–2.0 µs caller** target is within the 3.797 µs observed rung-8→9 caller residual. This overlaps the rung-2 receiver proposal; do not add the estimates. |
| `src/runtime/mesh/mesh_node_host_service.cpp:node_receive_role_t::rotate`; `backend/raw_route_port.cpp:raw_route_port_t::poll`; `mesh_node_runtime.cpp:submit_node_request_reply` completion lambda | Receive role **17.333**; poll **14.273**. Source completion dispatcher workers are separate (roughly 3.2 µs each in this profile). On one representative dispatcher worker, the `submit_node_request_reply` callback encloses **1.626 µs**, its next receiver projection **1.252**, and header decode **0.458**. These overlap. | [Receive progress/management:143](SPEC01-execution/08-messaging-hot-path.ko.md:143), [callback independence:490](SPEC01-execution/01-submit-and-completion.ko.md:490), [receive-role placement:300](SPEC01-execution/08-messaging-hot-path.ko.md:300). **No clause** requires repeated internal result boxing/repacking. Native polling and route decisions remain Core/binding-owned. | Preserve the already owned native result/header views through the current receiver instead of constructing another boxed carrier and decoding it again. A **1–2 µs aggregate dispatcher** target and **0.5–1.0 µs receive-role** target require a measured implementation; neither is a claim that the poll or completion drain can be removed. No direct Core-I/O saving is assumed. |
| `src/runtime/stateful/public_host_runtime.cpp:dispatch_ready/dispatch_application_claim`; `mesh/mesh_record_dispatcher.cpp:dispatch`; `mesh_node_host_service.cpp:dispatch_application` | Target receive **16.372**, `public_host_runtime_t::dispatch_ready` inclusive **12.405**, including `raw_mesh_node_owner_t::receive_next` **8.585**. Two marked app workers use **10.772 + 10.366** profiled µs/message; `mesh_record_dispatcher_t::dispatch` encloses **7.676/7.356** on those workers. | [Permit order:116](SPEC01-execution/04-application-job-queue-and-backpressure.ko.md:116), [owner execution gate:76](SPEC01-execution/02-handler-turn-and-execution-gate.ko.md:76), [payload ownership:58](SPEC01-execution/05-payload-ownership-and-codec.ko.md:58). **No clause** requires duplicate retained metadata/callable carriers between preparation and invocation. | Keep the one existing record/permit/terminal owner through preparation and invocation, moving its view instead of reconstructing another carrier. Initial representation targets are **0.4–0.8 µs target receive** and **0.5–1.0 µs aggregate app-worker CPU**. Preserve handler-entry permit timing, FIFO claims and independent progress. |

The full-profile categories are source Core/binding **45.009**, allocation **11.041**, codec **7.356**, handoff/synchronization **17.108** and Framework bookkeeping **13.081 µs/message**; target Core/binding is **45.057**. These are normalized profiled totals, not the unprofiled medians or differences against a profiled rung 8. In particular, packet cadence changes Core/socket/kernel work. The extra wrapper/dispatcher/receive-preparation scopes locate the integration work; they do not uniquely apportion every µs of the 23.56% throughput residual. Another sub-ladder would be needed to isolate each remaining facade and control-plane check. The reported gap remains explicit.
'''.replace('SPEC',S)
text=text.replace('<!-- FULL_PROFILE_DETAILS -->',full)
b=s['binding-reference']['throughput'];f=s['rung9-unbounded']['throughput']
intro=f'''**The first large costs appear in envelope processing and then the Framework task/admission/completion boundary, before the logical registry is added.** At window 100, throughput is 223,019 → 156,798 → 84,284 msg/s, with caller demand 4.404 → 6.178 → 11.357 µs/request. The full API reaches 45,051 msg/s (F/B 0.2020). The unchanged unlimited-window comparison is **{f:,.1f}/{b:,.1f} = {f/b:.4f}**, reproducing the published Framework cell within {abs(100*(f/49602-1)):.2f}%.

'''
text=text.replace('\n\nSource:', '\n\n'+intro+'Source:',1)
text=text.replace('CPU time is not latency.', '“msg/s” counts completed logical requests, not both physical directions or multipart frames. Send CPU is normalized by same-window target receives. CPU time is not latency.',1)
text=text.replace('Rung 2 changes to', 'Rung 1 also emits the actual canonical Framework header fields and real deadline/correlation values rather than the shorter fixed raw literal; its delta includes those byte/validation differences. Rung 2 changes to',1)
# Optional send endpoint check and resource/validation summary.
sb=s['send-binding-reference'];sf=s['send-full']
send=f'''## Send-saturation 1K endpoint check

The two production benchmark endpoints were also measured with eight send slots, the same CPU sets, 30 s warm-up, three 20 s active phases and the same `/proc` capture method. This is an endpoint comparison; no intermediate send ladder is inferred from request-only registry/deadline rungs.

| Endpoint | Median msg/s | F/B | Source CPU µs/message | Target CPU µs/message | Source caller µs/message |
|---|---:|---:|---:|---:|---:|
| Binding benchmark | {sb['throughput']:,.1f} | 1.0000 | {sb['endpoint_us']['source']:.3f} | {sb['endpoint_us']['target']:.3f} | {sb['groups']['source'].get('caller',0):.3f} |
| Full public Framework | {sf['throughput']:,.1f} | {sf['throughput']/sb['throughput']:.4f} | {sf['endpoint_us']['source']:.3f} | {sf['endpoint_us']['target']:.3f} | {sf['groups']['source'].get('caller',0):.3f} |

Relative to the published 616,974/173,204 msg/s references, these endpoints differ by {100*(sb['throughput']/616974-1):+.2f}%/{100*(sf['throughput']/173204-1):+.2f}%. All sends settle with zero errors and zero abandoned messages; server rejection accounting is checked by the original runner. For the current binding median, 0.6 is {0.6*sb['throughput']:,.1f} msg/s and a {1e6/(.6*sb['throughput']):.3f} µs serial-thread budget. The original denominator gives 2.701 µs.

'''
text=text.replace('## Validation and raw data',send+'## Validation and raw data',1)
valid=sum(len(r['runs']) for r in s.values());mem=[]
for r in s.values():
 for c in r['runs']:
  path=Path(c['path'])/'capture/meta.json'
  if path.exists():mem.append(json.loads(path.read_text())['memavailable_kb']/1048576)
validated=f'''**Final checks:** {valid} valid measured cells (30 primary request-rung runs, 12 request baseline comparisons, three perf captures and six send endpoint runs); all count/payload/error/abandon/drain and affinity checks pass. The counter check accepts valid input and rejects a deliberately missing receive. Minimum recorded capture-start MemAvailable is **{min(mem):.2f} GiB**; every CPU snapshot additionally enforced the 12 GiB floor. Product source hashes still match and `git status --short` is empty. Source raw and Framework unlimited request endpoints reproduce the published cells by -0.04% and {100*(f/49602-1):+.2f}%, respectively.

'''
text=text.replace('## Validation and raw data\n\n','## Validation and raw data\n\n'+validated,1)
text=text.replace('- `raw/binding-reference*`, `raw/rung0-unbounded`, `raw/rung9-unbounded`:', '- `raw/send-binding-reference`, `raw/send-full`: send endpoint results and per-thread captures.\n- `raw/binding-reference*`, `raw/rung0-unbounded`, `raw/rung9-unbounded`:')
assert '<!--' not in text
(J/'report.md').write_text(text)
print('final sections complete;',valid,'valid cells; min memory',min(mem))
