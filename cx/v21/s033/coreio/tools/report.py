from pathlib import Path
import json,collections,datetime
J=Path('/root/jobs/fx/cx/v21/s033/coreio');R=Path('/root/worktree/zlink-ratio040')
metrics=json.load(open(J/'metrics.json'));models=json.load(open(J/'savings-model.json'));bf=json.load(open(J/'bpf-summary.json'));pf=json.load(open(J/'perf-summary.json'))
M={(r['language'],r['pattern'],r['implementation']):r for r in metrics}
cases=[('cpp','request-backpressure'),('cpp','send-saturation'),('java','send-saturation')]
name=lambda c:('C++' if c[0]=='cpp' else 'Java')+' '+c[1]
def src(label,path,line=None):return f'[{label}]({R/path}'+(f':{line}' if line else '')+')'
def fmt(x,d=3):return f'{x:.{d}f}'
def pair(a,b,d=3):return fmt(a,d)+' / '+fmt(b,d)
def call(r,role,sc,field='calls',g='io'):return r['roles'][role]['calls_per_message'][g].get(sc,{}).get(field,0)
def event(r,role,g,f):return r['roles'][role]['eventfd_per_message'][g].get(f,0)
L=[]
def add(s=''):L.append(s)
add('**Core I/O cost under Framework and raw binding — #3867, #3868, #3870 / #1639**\n')
add('The largest measured difference is **TCP-call amortization**. Framework and binding send **two physical ZMP parts per application record**, use the **same native Core build**, and maintain **two TCP connections per ROUTER peer pair**. Framework sends fewer messages per TCP write. Java makes the mechanism particularly clear: Framework averages about 1.75 messages per write versus 6.41 for binding, while sampled TCP-send CPU per call is about 15.6 versus 15.3 µs.\n')
add('For C++ request-backpressure, the hottest source I/O thread costs 21.83 µs/message under Framework versus 2.88 under binding; total source I/O CPU is 38.59 versus 5.43. About 69% of this aggregate excess is below TCP send/receive calls. Smaller batches explain the dominant measured mechanism. **Whether faster Framework submission alone will produce binding-sized batches is not measured**: no rate-matched or patched counterfactual was run.\n')
add('**Measurement scope and interpretation**\n')
add('- Read-only investigation on detached HEAD `ee1139025782688e33109455041d8a0222cc8fe3`. The four pre-existing runner pinning changes were preserved. No tracked files, branches, commits, packages, or builds were changed. All new scripts and outputs are in this directory. No agents or other models were invoked.')
add('- Every benchmark and attached tracer held `flock -o /root/jobs/fx/cx/measure.lock`. Source CPUs were 0–7; target CPUs were 8–15. Job-local runner copies preserve the benchmark behavior and use the existing builds with `SKIP_BUILD=1`, `PAYLOAD_SIZES=1024`, one pattern/implementation, `RUNS=1`, warmup 10 s, and active 30–60 s. Request mailbox follow-ups used 45 s active. Lock waits are recorded in [matrix.log](matrix.log).')
add('- CPU: per-thread `/proc/PID/task/TID/schedstat` deltas over approximately 10 s; samples from `perf record -e cpu-clock -F 149 --call-graph dwarf,16384`. CPU sampling ran first, before syscall or mailbox probes. CPU denominators are completed requests, or target received messages for send. I/O totals sum all `ZLINKbg/IO/*` threads; idle contexts are included. These are CPU time, not syscall elapsed time or end-to-end latency.')
add('- Counters: separate 8 s BPF windows, with a start/stop gate and per-process/thread/FD syscall entry, return, successful byte, and epoll timeout/return aggregates. The main tables use the **latest syscall-only window** for each case. To avoid HTTP/proc snapshot boundary skew, counter denominators are forward-data TCP bytes divided by the decoded fixed record size. This is a wire-message estimate; rare small service-control records and partial boundary records produce a small error. Completed/received snapshots remain in the raw evidence. The two processes can also differ by data already in flight at the boundaries.')
add('- Wire and callers: approximately 1 s `strace -f -k -xx -s 131072 -v -yy`, after the counter window. TCP streams were reassembled and ZMP headers decoded. `strace` changes batching substantially; **its batch sizes and timings are not used as production performance measurements**. Core traffic is separated from benchmark HTTP statistics traffic.')
add('- Mailboxes: later uprobes on the existing native `mailbox_t::send/recv` symbols and whole-record `zlink_request`, `zlink_send_rid`, and `zlink_reply` entry points. There is no `zlink_msg_send` entry point in this build. These probes materially slow the workload; their counts describe the probed run and omit compiler-inlined calls. They identify command types and callers, rather than supplying the main batching numbers.')
add('- All inspected native libraries have build ID `cc6989f03587dd8c267953b75b2f40042efec476`, SHA-256 `6f496304ef6e1287f57234567231bbd2b76ad2392f99e816001417d01215bf01`. Java loads the same binary from its native cache. BPF CPU deltas are deliberately excluded from savings estimates because tracing executes work on the traced threads. `perf` uses the raw monotonic clock here, about 76 s ahead of Python monotonic time; samples are selected by the active-only recorder lifetime, not by comparing these different clocks directly.\n')
add('`F / B` below means Framework / binding. “Message” in request-backpressure means one request operation; its request and reply are both shown. TCP byte counts exclude TCP/IP headers and ACKs.\n')
add('**CPU per message**\n')
add('| Case | Source I/O total, µs F / B | Source hottest, µs F / B | Target I/O total, µs F / B | Target hottest, µs F / B | CPU-window rate, msg/s F / B |')
add('|---|---:|---:|---:|---:|---:|')
for c in cases:
 f=M[*c,'Framework'];b=M[*c,'binding'];p={x['implementation']:next(z for z in pf if z['capture']==x['perf_capture']) for x in (f,b)}
 add('| '+name(c)+' | '+pair(f['roles']['source']['io_cpu_us_per_message'],b['roles']['source']['io_cpu_us_per_message'],2)+' | '+pair(f['roles']['source']['hot_cpu_us_per_message'],b['roles']['source']['hot_cpu_us_per_message'],2)+' | '+pair(f['roles']['target']['io_cpu_us_per_message'],b['roles']['target']['io_cpu_us_per_message'],2)+' | '+pair(f['roles']['target']['hot_cpu_us_per_message'],b['roles']['target']['hot_cpu_us_per_message'],2)+' | '+f"{p['Framework']['rate']:,.0f} / {p['binding']['rate']:,.0f}"+' |')
add('\nThe binding C++ request source’s hottest thread receives replies. Its sending I/O thread costs 2.54 µs/message; Framework’s sending thread costs 21.83. Thus the hottest-thread comparison is not a claim that the thread roles are identical. Full TIDs, core shares, sample counts and stacks are in [perf-summary.json](perf-summary.json).\n')
add('| Case / side | TCP send CPU, µs F / B | TCP receive CPU, µs F / B | epoll CPU, µs F / B | write CPU, µs F / B | Other I/O CPU, µs F / B |')
add('|---|---:|---:|---:|---:|---:|')
for c in cases:
 f=M[*c,'Framework'];b=M[*c,'binding']
 for role in ('source','target'):
  fs=f['roles'][role]['cpu_stages'];bs=b['roles'][role]['cpu_stages'];stages=('send','recv','epoll_wait','write')
  otherf=f['roles'][role]['io_cpu_us_per_message']-sum(fs.get(s,0) for s in stages);otherb=b['roles'][role]['io_cpu_us_per_message']-sum(bs.get(s,0) for s in stages)
  add('| '+name(c)+' / '+role+' | '+' | '.join(pair(fs.get(s,0),bs.get(s,0),2) for s in stages)+' | '+pair(otherf,otherb,2)+' |')
add('\nStages are mutually exclusive sampled stack groups, scaled to each thread’s measured CPU. TCP groups include kernel networking and loopback processing below the libc calls, including TCP and netfilter paths. “Other” includes Core/Asio userspace, other syscalls, and small unsampled idle-thread totals. The finer division of that remainder between per-frame, per-batch, allocation, locking and cache costs is **not measured**.\n')
add('**Wire parts, bytes and batching**\n')
add('| Record | Physical parts F / B | Part body sizes F | Part body sizes B | ZMP bytes/record F / B |')
add('|---|---:|---|---|---:|')
for c,direction,label in [(cases[0],'send','C++ request'),(cases[0],'recv','C++ reply'),(cases[1],'send','C++ one-way send'),(cases[2],'send','Java one-way send')]:
 f=M[*c,'Framework']['wire'][direction];b=M[*c,'binding']['wire'][direction]
 add('| '+label+' | '+f"{len(f['part_sizes'])} / {len(b['part_sizes'])}"+' | '+', '.join(map(str,f['part_sizes']))+' | '+', '.join(map(str,b['part_sizes']))+' | '+f"{f['wire_bytes']} / {b['wire_bytes']}"+' |')
add('\nEach request/reply has 24 ZMP header bytes: two 8-byte headers and an 8-byte sequence extension on the first frame. One-way records have 16 header bytes. Neither the routing ID nor the request sequence becomes an additional multipart payload frame in these captures. Core I/O uses scalar `sendto` buffers here: **one contiguous buffer per call; zero Core-I/O `sendmsg` calls** in all six selected counter windows. A contiguous buffer can contain several records and many ZMP parts. HTTP `sendmsg` calls in whole-process strace are excluded.\n')
add('Framework carries the JSON application envelope and protobuf body **inside one binary `ZLinkFrameworkMultipart` payload part**, preceded by the service header. C++ request overhead versus binding is 175 bytes: 13 service-header bytes, 75 wrapper/inner-length bytes, and 87 additional JSON-header bytes. Reply overhead is 165 bytes. One-way overhead is 145 bytes in C++ and 140 in Java. These increases are about 11–14%, not additional physical parts. See '+src('C++ application packing','framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp',2688)+', '+src('C++ service writer','framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp',2821)+', and '+src('Java multipart writer','framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceM6AWireCodec.java',356)+'.\n')
add('| Data direction | Messages/successful send F / B | Messages/successful receive F / B | Mean TCP bytes/successful send F / B |')
add('|---|---:|---:|---:|')
for c,direction,label,sender,receiver in [(cases[0],'send','C++ request: source → target','source','target'),(cases[0],'recv','C++ reply: target → source','target','source'),(cases[1],'send','C++ one-way: source → target','source','target'),(cases[2],'send','Java one-way: source → target','source','target')]:
 vals=[]
 for impl in ('Framework','binding'):
  r=M[*c,impl];w=r['wire'][direction]['wire_bytes'];tx=call(r,sender,'sendto','positive_return_sum');rx=call(r,receiver,'recvfrom','positive_return_sum');sp=call(r,sender,'sendto','positive');rp=call(r,receiver,'recvfrom','positive');vals.append((tx/w/sp,rx/w/rp,tx/sp))
 add('| '+label+' | '+pair(vals[0][0],vals[1][0],2)+' | '+pair(vals[0][1],vals[1][1],2)+' | '+pair(vals[0][2],vals[1][2],0)+' |')
add('\nThese batching values use returned TCP bytes and successful call counts, so they do not depend on HTTP message-counter timing. Rare small control records are negligible in the byte ratios. A successful receive is not necessarily a whole record; the byte ratio is an average over the stream. [Wire decoder results](wire-summary.json) retain the decoded record counts, frame kinds, prefixes, control records and partial-boundary information.\n')
add('| Case / side | sendto calls/msg F / B | recvfrom calls/msg F / B | recvfrom EAGAIN/msg F / B | TCP TX bytes/msg F / B | TCP RX bytes/msg F / B |')
add('|---|---:|---:|---:|---:|---:|')
for c in cases:
 f=M[*c,'Framework'];b=M[*c,'binding']
 for role in ('source','target'):
  add('| '+name(c)+' / '+role+' | '+pair(call(f,role,'sendto'),call(b,role,'sendto'))+' | '+pair(call(f,role,'recvfrom'),call(b,role,'recvfrom'))+' | '+pair(call(f,role,'recvfrom','ret_-11'),call(b,role,'recvfrom','ret_-11'))+' | '+pair(call(f,role,'sendto','positive_return_sum'),call(b,role,'sendto','positive_return_sum'),1)+' | '+pair(call(f,role,'recvfrom','positive_return_sum'),call(b,role,'recvfrom','positive_return_sum'),1)+' |')
add('\n**Notifications and epoll returns**\n')
add('| Case / side | I/O eventfd reads/msg F / B | I/O eventfd writes/msg F / B | Other-thread eventfd reads/msg F / B | Other-thread eventfd writes/msg F / B |')
add('|---|---:|---:|---:|---:|')
for c in cases:
 f=M[*c,'Framework'];b=M[*c,'binding']
 for role in ('source','target'):
  add('| '+name(c)+' / '+role+' | '+' | '.join(pair(event(f,role,g,sc),event(b,role,g,sc)) for g,sc in [('io','read'),('io','write'),('other','read'),('other','write')])+' |')
add('\nThe measured transport notification FDs are eventfds, not pipes. Core I/O eventfd reads are zero. The large C++ process-wide read count belongs primarily to the Framework receive owner, through Core’s `socket_poller_t` and `signaler_t::recv_failable`; many reads return EAGAIN. Successful notification reads/writes are 8 bytes. `recv_failable` can also write back a surplus counter, which makes the receive owner a writer as well. Thus process-wide read/write totals cannot be charged wholesale to an I/O thread. Per-FD and per-TID counts, including success/EAGAIN splits, are in [metrics.json](metrics.json) and [bpf-summary.json](bpf-summary.json).\n')
add('| Case / side | epoll_wait calls/msg F / B | Nonempty returns/msg F / B | Nonempty, blocking permitted F / B | Nonempty, timeout=0 F / B | Other-thread Core reactor epoll_ctl/msg F / B |')
add('|---|---:|---:|---:|---:|---:|')
for c in cases:
 f=M[*c,'Framework'];b=M[*c,'binding']
 for role in ('source','target'):
  vals=[pair(call(f,role,'epoll_wait',field),call(b,role,'epoll_wait',field),4) for field in ('calls','positive','blocking_nonempty','zero_timeout_nonempty')]
  add('| '+name(c)+' / '+role+' | '+' | '.join(vals)+' | '+pair(f['roles'][role]['core_reactor_ctl_other'],b['roles'][role]['core_reactor_ctl_other'],4)+' |')
add('\nA nonempty epoll return is not necessarily a sleeping thread waking. “Blocking permitted” means the input timeout was nonzero; a ready descriptor can still return immediately. Exact `sched_wakeup` counts are **not measured**. In send-saturation, source I/O spends almost all epoll calls polling with no returned event while continuing its write-completion work. Calling all of those “wakeups” would misstate the mechanism. External `epoll_ctl` calls above target the Core I/O reactor FDs, excluding the benchmark HTTP reactor. They arise from Core/Asio mailbox posts, rather than evidence of a Framework socket re-registration per message.\n')
add('**Caller and ownership attribution**\n')
add('| Extra work | Observed call path and owner | Classification | Implication |')
add('|---|---|---|---|')
add('| More TCP sends and smaller writes | Framework `raw_mesh_node_owner_t::request_with_header` / `start_send_on_lane` → `raw_route_port_t::request` / `submit_send` → binding operation → Core `asio_engine_t::prepare_output_buffer` / `start_async_write` → `tcp_transport_t::async_write_some` → TCP/kernel | **Batching consequence**, executed by Core | The Core encoder already pulls queued frames until its output target or lack of data stops it. Neither path adds a syscall per part. Faster submission, reply production, or completion handling may change queue occupancy; their causal contributions are not isolated. |')
add('| More TCP receives and EAGAIN calls | Core `asio_engine_t::start_async_read` / `on_read_complete` → `tcp_transport_t::async_read_some` and Asio reactor receive operations | **Core/binding behavior triggered by traffic cadence** | Smaller arriving byte batches amortize receive and read-arm checks over fewer messages. These are native receive/arming calls, not a Framework retry loop. |')
add('| I/O-thread eventfd writes | Core `asio_engine_t::process_input` → `pipe_t::flush_unlocked` → `object_t::send_activate_read` / `send_pipe_command` → `mailbox_t::send` → `signaler_t::send` | **Core/binding behavior**, sensitive to receiver quiescence and batching | Wakes the pipe reader after an inactive-to-pending edge. Framework’s public poller requires descriptor notification; raw target’s blocking receive uses a different wait path. The write bucket is only 0.66–0.75 µs/request across each endpoint’s I/O threads in C++. |')
add('| Receive-owner reads and self-notifications | Framework `node_receive_role_t::rotate` / `raw_route_port_t::poll` → binding `poller_t::wait` → Core `socket_poller_t::check_socket_events` / `collect_socket_event`; Core reply drain calls `notify_request_completion` → mailbox → eventfd | **Framework polling architecture triggers Core/binding behavior** | The native poller owns the descriptor reads. Reply-completion notification can be produced while that same receive owner drains replies. This work is primarily outside I/O threads. |')
add('| Reactor interrupts and handler dispatch | Core `mailbox_t::schedule_if_needed_unlocked` → `boost::asio::post` / `scheduler::post_immediate_completion` → `epoll_reactor::interrupt` (`epoll_ctl`) | **Core/binding behavior**, amortized by batching | The mailbox has an existing scheduled/pending decision. More returns to idle produce more posts and readiness processing. Remaining Core/Asio userspace CPU is measured as a group; its exact per-call/per-frame split is not measured. |')
add('| Additional service/application wrapper bytes | Framework `public_host_runtime_t::encode_application`, `application_payload_t::from_parts`, `encode_application_payload`; Java `ZLinkJavaRawMeshNode.encodeApplicationFrames` / `ZLinkServiceM6AWireCodec.encodeFrameworkMultipartMessage` | **Framework wire-format choice, currently a shared contract** | Adds approximately 11–14% of bytes. The optional metadata part is absent in these application records. It does not multiply the physical frame count. A format change requires contract review; changing benchmark codecs would not establish a runtime fix. |')
add('| Separate Completion connection | Core transport-pair creation in `socket_base_endpoint.cpp`; reply routing in `socket_request_reply_runtime_io.cpp` | **Core/binding behavior common to both** | Two established TCP connections in every case: request/data on the Application lane, replies on the Completion lane. One-way uses the data lane; the second lane is idle or carries control. No extra application copy or Framework-only connection multiplier is observed. |')
add('| Request completion entry | Binding `request_submit_operation_t::async`, `request_completion_bundle_t`, `completion_owner_t::start_async_request`; Core `notify_request_completion` | **Common binding entry, Core notification coalescing** | Both C++ paths use the same async request implementation and native two-part request API. One entry does not force one I/O-thread eventfd write per reply. Pending notification coalescing and drain cadence determine the observed commands. |\n')
add('Source anchors: '+', '.join([
 src('Framework C++ request construction','framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp',1526),
 src('Framework C++ binding request','framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp',313),
 src('Framework C++ poll','framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp',365),
 src('binding async request','bindings/cpp/src/Runtime/Messaging/request_reply.cpp',102),
 src('Core output buffer','core/src/runtime/engine/asio/asio_engine.cpp',1314),
 src('Core TCP write','core/src/runtime/transports/tcp/tcp_transport.cpp',519),
 src('Core pipe flush','core/src/runtime/core/pipe_write.cpp',1430),
 src('Core mailbox','core/src/runtime/core/mailbox.cpp',72),
 src('Core poller','core/src/runtime/core/socket_poller.cpp',543),
 src('Core completion notification','core/src/runtime/sockets/common/socket_base_dispatch.cpp',325),
 src('Core Completion lane creation','core/src/runtime/sockets/common/socket_base_endpoint.cpp',872),
 src('Java Framework send','framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawServicePort.java',202),
 src('Java binding native calls','bindings/java/src/main/java/systems/zlink/runtime/nativeapi/Native.java',119)
])+'.\n')
add('The C++ request strace directly shows the source receive owner writing its primary eventfd through `complete_reply_from_transport → notify_request_completion`; target I/O writes through `process_input → pipe_t::flush_unlocked`. The C++ send strace also resolves `epoll_ctl` through Core’s scheduled mailbox post, and target flow-control publication through `zlink_socket_set_receive_flow_state`. Full native stack chains are retained beside each `.strace` as `.wire.json`.\n')
add('**Observed mailbox command calls**\n')
add('The following counts come from the latest **uprobe window**, normalized by its forward wire-message estimate. They are diagnostic symbol-call counts, not the syscall-only window above; compiler-inlined calls are not counted. `activate_read` is the normal pipe activation command (enum 5), `activate_write` is cumulative credit return (6), and `request_completion` is the coalesced socket-completion notification (9).\n')
add('| Case / command sender | activate_read/msg F / B | activate_write/msg F / B | request_completion/msg F / B |')
add('|---|---:|---:|---:|')
up={}
for x in bf:
 if x.get('mode')!='uprobe':continue
 cell=Path(x['capture']).parent.name;k=('java' if '-java-' in cell else 'cpp','request-backpressure' if 'request-backpressure' in cell else 'send-saturation','Framework' if cell.startswith('zlink-framework-') else 'binding');stamp=json.load(open(Path(x['capture'])/'after.json'))['monotonic']
 if k not in up or stamp>up[k][0]:up[k]=stamp,x
for c in cases:
 values={}
 for impl in ('Framework','binding'):
  r=up[*c,impl][1];w=M[*c,impl]['wire']['send']['wire_bytes'];io=r['roles']['source']['totals']['io'];n=io['sendto']['counts']['positive_return_sum']/w
  for role in ('source','target'):
   ct=collections.Counter()
   for key,v in r['roles'][role]['commands']['@cmdsend'].items():ct[int(key.split(',')[-1])]+=v
   values[impl,role]={cmd:ct[cmd]/n for cmd in (5,6,9)}
 for role in ('source','target'):add('| '+name(c)+' / '+role+' | '+' | '.join(pair(values['Framework',role][cmd],values['binding',role][cmd]) for cmd in (5,6,9))+' |')
add('\nThe request uprobes observe two-part `zlink_request` and two-part `zlink_reply` calls in both implementations. Occasional one-part `zlink_send_rid` calls are service-control records. Framework’s request-completion command is close to one per operation in this heavily probed run; binding coalesces many replies per notification. The registered entry is common; the notification cadence differs. The count alone does not imply that a separate Framework entry generates an I/O-thread wake.\n')
add('**Connections and notification writers**\n')
add('| Case / implementation | Source → target TCP endpoints | Application/completion usage | Largest notification writers |')
add('|---|---|---|---|')
for c in cases:
 for impl in ('Framework','binding'):
  r=M[*c,impl];con=r['roles']['source']['connections'];ends='; '.join(f"{v['local']} → {v['remote']}" for v in con.values())
  writers=[]
  for role in ('source','target'):
   ws=[x for x in r['roles'][role]['eventfd_threads'] if x['syscall']=='write'][:3]
   writers.append(role+': '+(', '.join(f"TID {x['tid']} ({'Core I/O' if x['io'] else 'non-I/O'}), fd {x['fd']}, {x['per_message']:.3f}/msg" for x in ws) or 'no eventfd write observed'))
  usage='requests on Application, replies on Completion' if c[1]=='request-backpressure' else 'data on Application; Completion idle/control only'
  add('| '+name(c)+' / '+impl+' | '+ends+' | '+usage+' | '+'; '.join(writers)+' |')
add('\nTCP endpoints are the selected counter window’s established peer connections, not statistics listeners. There are two peer TCP connections in all six cases. The raw C++ server additionally creates an unused logical ROUTER/listener for the other benchmark pattern; that is not a second active peer workload. Listener, eventfd, epoll FD and TID inventories remain in each snapshot.\n')
add('**Framework changes and conditional I/O savings**\n')
add('The measurements identify a batching mechanism, not a validated Framework patch. The primary Framework proposal is to reduce work before Core admission and before reply production, preserving the same typed serializer, message bytes, admission/deadline semantics, execution gate and completion context. A concrete compatible scope is to write the service-wire application payload into its final native message buffer, replacing the intermediate byte vector plus `materialize_binding_part` copy in the existing owner. This reduces Framework construction work; **direct I/O-thread savings with unchanged traffic are approximately zero**. Its possible I/O benefit is the change in arrival cadence and batch occupancy.\n')
add('For C++ requests, also examine the existing `framework_driver_t::run_tasks` admitted-task/completion bookkeeping and the runtime request/operation path, while retaining the public API contract. Java’s corresponding scope is `ZLinkJavaRawMeshNode.encodeApplicationFrames`, `ZLinkJavaRawServicePort.sendMessages`, and their existing state-lane turn. **Caller execution when the lane is idle already exists** in both '+src('C++ `state_lane_t::run_sync`','framework/languages/cpp/framework/src/runtime/execution/state_lane.hpp',289)+' and '+src('Java `ZLinkStateLane.awaitTurn`','framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/execution/ZLinkStateLane.java',123)+'. Adding that existing behavior is not a new optimization. No isolated construction/admission patch was implemented or timed here.\n')
add('The frequency-scaling model below estimates the Core I/O opportunity **if Framework reaches binding’s call amortization**. For send/receive stages, it adjusts the binding call rate by the Framework/binding record-byte ratio; for epoll/write it uses the observed call-rate ratio. For each stage: `saving = Framework stage CPU × max(0, 1 − adjusted binding calls/msg ÷ Framework calls/msg)`. This is optimistic: some CPU is per-byte or per-frame, raw target uses a different receive wait form, and profiled call costs can change with batch size. Other Core userspace CPU is not removed by this model. The estimates are not measured patch gains.\n')
add('| Case | Source currently hottest I/O thread, conditional µs saved/msg | Target currently hottest I/O thread, conditional µs saved/msg | Source / target aggregate I/O estimate, µs/msg |')
add('|---|---:|---:|---:|')
for m in models:
 add('| '+name((m['language'],m['pattern']))+' | '+fmt(m['roles']['source']['existing_hottest_thread_model_us'],1)+' | '+fmt(m['roles']['target']['existing_hottest_thread_model_us'],1)+' | '+pair(m['roles']['source']['aggregate_model_us'],m['roles']['target']['aggregate_model_us'],1)+' |')
add('\nThese numbers are a shared batching opportunity for producer, reply-producer and receiver improvements; **they must not be added once for each proposed change**. For example, C++ request source’s measured TCP send/receive excess is about 23.0 µs/message across I/O threads, while its eventfd-write bucket is only 0.66. A notification-only proposal cannot recover the TCP difference. [savings-model.json](savings-model.json) contains the per-stage and per-thread arithmetic.\n')
add('| Framework-side cause / proposed scope | Direct Core I/O saving with unchanged traffic | Conditional or unresolved saving |')
add('|---|---|---|')
add('| Construction/admission/reply-production work: remove duplicate materialization in the existing service writer and simplify already-completed admission bookkeeping where the existing contract permits | Approximately 0 µs/msg; this work executes on Framework/caller resources | The batching model above is the conditional opportunity. The amount recovered by this specific change is **not measured**. Validate both bytes and call amortization after any approved patch. |')
add('| Polling/receive architecture: retain the existing multiplexed owner and consolidate necessary bookkeeping at its existing turn boundary | No independently demonstrated Framework-only I/O saving | Reduced idle-to-active edges may lower Core writes/posts. In C++ requests, removing the entire I/O write bucket would save only about 0.66 µs/source and 0.75 µs/target aggregate. Removing those notifications safely is a Core/poller ownership question; substituting a second owner or skipping required progress is not justified by this evidence. |')
add('| Nested envelope bytes: a more compact common typed-message carrier would be a shared wire-contract design change; the compatible immediate proposal is construction into final storage with the existing bytes | Final-storage construction: approximately 0 µs/msg on I/O directly | Wire-only savings are **not measured**. Even a linear-byte sensitivity of the C++ request hottest-source CPU gives only `21.83 × 175/1427 ≈ 2.7 µs/msg`; fixed per-call TCP costs make that an aggressive sensitivity, not a prediction. No compatible removal of all 175 bytes has been established. |')
add('| Extra physical parts, Framework-only Completion connection, or an extra binding completion entry | 0 attributable saving: these differences were not observed | Two physical parts and two peer connections are common; both C++ request paths use the same binding entry owner. |\n')
add('The receive/completion and gate contracts remain owned by '+src('Core polling','core/doc/spec/core/05-polling.ko.md')+', '+src('Core ROUTER','core/doc/spec/core/socket/07-router.ko.md')+', and Framework '+src('submit/completion','framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md')+' / '+src('messaging hot path','framework/doc/framework/common/spec/server/01-execution/08-messaging-hot-path.ko.md')+'. This investigation proposes no Core decision reimplementation in Framework and makes no runtime or spec change.\n')
add('**Validation, limitations and evidence**\n')
add('- All six cases have CPU samples, successful-byte/syscall counters, decoded wire records, native stacks and mailbox/API probe evidence. Final benchmark captures drained with matching completed/received totals and zero workload errors. The original Java trace attempt missed active because its trigger endpoint is synchronous; it was excluded and the job-local collector was corrected. The first C++ Framework request mailbox script failed compilation due to a duplicate `END` block; its prior CPU/counter/wire windows remain valid, and a later clean request run supplies mailbox evidence. These were collection failures, not evidence of a runtime failure.')
add('- Exact TCP packet counts, exact scheduler wakeups, isolated byte-copy CPU cost, finer Core/Asio userspace attribution, rate-matched raw controls, and savings from an implemented Framework change are **not measured**. .NET was not reprofiled; the supplied .NET result is not sufficient to assign the same numerical breakdown to .NET.')
add('- The wire decoder was checked with valid multipart records, a truncated record that must not count as complete, and a control frame that must not count as an application record. [wire-parser-check.json](wire-parser-check.json) records both acceptance and rejection checks. Trace perturbation is material: the straced Framework request source can accumulate many messages per write, demonstrating that the existing Core path can batch these same Framework records; those distorted rates are excluded from the main tables.')
add('- Primary artifacts: [metrics.json](metrics.json), [CPU stages](perf-summary.json), [BPF counters](bpf-summary.json), [wire results](wire-summary.json), [savings arithmetic](savings-model.json), [build identities](builds.json), [initial repository state](initial-state.json), [collection and analysis tools](tools/), and per-run raw files under [out/](out/). Each capture retains before/after statistics, FD/TID inventories, commands, exit codes and native build identities.\n')
v=json.load(open(J/'verification.json'))
add(f"Verification: **pass** ([verification.json](verification.json)). The lowest capture-end MemAvailable was {v['minimum_capture_memavailable_kb']/1048576:.1f} GiB; the largest raw trace was {v['largest_raw_trace_bytes']/1000000:.1f} MB. No raw trace exceeded 1 GB. All benchmark PIDs have terminated; the initial tracked-file and executable hashes are unchanged.\n")
add('Repository/runtime rules before → after: unchanged; no state, queue, flag or lock added. Refactoring audit: not performed in this evidence-only task. No implementation, commit or push was performed.\n')
(J/'report.md').write_text('\n'.join(L))
print('wrote',J/'report.md','lines',len((J/'report.md').read_text().splitlines()),'words',len((J/'report.md').read_text().split()))
