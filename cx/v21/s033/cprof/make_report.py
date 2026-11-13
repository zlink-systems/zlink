from pathlib import Path
from collections import Counter,defaultdict
import json,statistics,re,csv
import analyze
J=analyze.ROOT;repo=Path('/root/worktree/zlink-ratio040');data=json.load(open(J/'analysis.json'));counts=json.load(open(J/'counts-summary.json'));scopes=json.load(open(J/'candidate-scopes.json'));quality=json.load(open(J/'quality.json'))
F='zlink-framework-cpp-request-backpressure-1024';B='zlink-cpp-request-backpressure-1024'
f=data[F];b=data[B];fr=f['roles']['source'];br=b['roles']['source'];ft=fr['threads'][fr['critical_tid']];bt=br['threads'][br['critical_tid']]
runs={}
for mode in ['none','profile','fastcounts']:
 for name in [F,B]:runs[mode,name]=json.load(open(J/f'out-{mode}/run1'/name/'results.json'))['cells'][0]
base={}
for name in [F,B]:
 cs=[json.load(open(p))['cells'][0] for p in (repo/'framework/bench/grpc/log/ratio040/cpp').glob(f'run*/{name}/results.json')]
 base[name]={'rate':statistics.median(c['throughput_per_second'] for c in cs),'cpu':statistics.median(c['submit_thread_cores']*1e6/c['throughput_per_second'] for c in cs),'source':statistics.median(c['client_cores']*1e6/c['throughput_per_second'] for c in cs),'target':statistics.median(c['server_cpu_percent']*c['logical_cores']/100*1e6/c['throughput_per_second'] for c in cs)}
F40=base[F]['rate'];B40=base[B]['rate'];C40=base[F]['cpu'];interval=1e6/F40;goal=1e6/(.6*B40);gap=interval-goal;scale=C40/ft['cpu_us']
newc=runs['none',F];newb=runs['none',B];newC=newc['submit_thread_cores']*1e6/newc['throughput_per_second'];newB=newb['throughput_per_second']
calc={'ratio040':base,'ratio040_interval_us':interval,'ratio040_goal_interval_us':goal,'ratio040_gap_us':gap,'profile_to_ratio040_cpu_scale':scale,'new_unprofiled_submit_cpu_us':newC,'candidate_bounds':{},'stage_gap_fractions':{}}
def table(head,rows):return ['| '+' | '.join(head)+' |','|'+'|'.join(['---']*len(head))+'|']+['| '+' | '.join(str(v).replace('|','\\|') for v in row)+' |' for row in rows]+['']
def us(v):return f'{v:.3f}'
def rate(v):return f'{v:,.0f}'
def cpu40(d):return 1e6/(B40*(C40-d*scale))
def newbound(d):return 1e6/(newB*(newC-d*newC/ft['cpu_us']))
lines=['# Current C++ Framework vs binding stage profile — issue #3867','',
'Cell: **request-backpressure, 1024 bytes**, `ee1139025782688e33109455041d8a0222cc8fe3`, 2026-11-05 KST. The Framework application submitter is the critical CPU resource. Its largest exclusive stage is allocation: **7.559 µs/message**, followed by Framework bookkeeping **5.057** and binding/Core calls **5.004**. Whole-source CPU is **103.093 µs/message**, versus binding **9.124**. These are current measurements; no earlier profile timings were reused.','',
'**Candidate R from the previous static analysis is already implemented in #3600 (`c8274434c0`).** It cannot be credited again. Of the remaining named candidates, W has the largest observed critical-thread enclosing budget, **2.395 µs/message**; even deleting that entire budget gives only an optimistic ratio040 submit-CPU ceiling of about **0.193**, holding other resource costs fixed.','',
'## Measurement and evidence','',
'- [Preflight](preflight.txt) passed before the first benchmark. HEAD is detached at the supplied main commit. The four existing `run_local.sh` pinning changes were retained; no repository tracked file or existing binary was modified. Source CPUs 0–7; target CPUs 8–15.',
'- Every benchmark/profile/check run held `flock -o /root/jobs/fx/cx/measure.lock`. The runs were sequential and waited for other jobs. Existing Release binaries were used with `SKIP_BUILD=1`. Only the small allocation counter and its validation executable were built, wholly in this directory, through `cpp-build-slot.sh`, under a shared measure lock, with `cmake --build ... --parallel 3`.',
'- Job-local copies of the current runner used `PATTERNS=request-backpressure`, `PAYLOAD_SIZES=1024`, the two `IMPLEMENTATIONS`, `RUNS=1`, warmup 10 s, active 20 s, and job-local output/stamps. See [run.sh](run.sh), [instrumented runner](instrumented/run_local.sh), and [collector](collect.py). Warmup, payload, admission, reply validation, and target/source count checks remain the repository runner\'s behavior.',
'- `perf record -e cpu-clock -F 199 --call-graph dwarf,16384 --sample-cpu -p PID` attached separately to source and target only within active. Capture CPU/message snapshots span about 8 s. Recorder startup makes effective sample spans 6.93–7.51 s; each thread\'s stage mix is normalized to its CPU delta across the enclosing capture window. No off-CPU latency is being reported as CPU.',
'- Thread CPU/message = `/proc/PID/task/TID/schedstat` runtime delta / source completed-reply delta. Core share = thread runtime / capture elapsed time. Sequential HTTP/proc snapshots and schedstat updates cause boundary skew: binding submitter shows 1.016 cores, meaning approximately one saturated core, not capacity above one core. Full-phase benchmark CPU counters provide an independent comparison below.',
'- Every sample enters one exclusive stage. Stage µs = thread CPU/message × sampled-period fraction on that thread; whole-process values sum threads. Inclusive function/scope values overlap. Generic leaf functions inherit the nearest recognized caller. Exact removable cost is not measured by an enclosing scope.',
'- [Profile quality](quality.json): no empty or wholly unresolved stacks; unknown leaves are 1.01–1.89% of sampled weight and use resolved caller attribution. Perf stderr reports no lost samples. The Framework submitter has '+str(ft['samples'])+' samples; one self sample is approximately '+us(ft['cpu_us']/ft['samples'])+' µs/message. Small function timings have sampling uncertainty. Binaries have symbols/unwind data but no complete source-line/inline timing; file locations below come from source matching.',
'- Heap calls use a job-local LD_PRELOAD interposer on external libc `malloc/calloc/realloc`, with per-thread counters and active-window deltas; `new` is counted when it reaches these APIs. `realloc` counts as one allocation call. Internal libc allocation entry points, aligned allocation APIs, direct mmap and custom pools are excluded. Free counts are separate and need not balance these intercepted allocation calls. The [positive/negative validation](counter-check/validation.json) observed exactly 1000 malloc, 1000 calloc, 1000 realloc and 2000 free calls and rejected a deliberately incorrect expectation.',
'- Futex/syscall/context-switch counts use per-thread `perf stat`, enabled/disabled with its control pipe during active, in separate counter runs. No malloc uprobes were used. Counter/snapshot windows have millisecond boundary skew. Call counts are not stack-attributed allocation counts. Perf emitted `<not counted>` for idle/auxiliary threads with zero counter-running time; those auxiliary call counts are **not measured**, rather than proven zero. Their enclosing proc-window CPU totals are 0.33/33.53 ms for Framework source/target and 0.37/156.30 ms for binding source/target; the nontrivial target entries are snapshot/stats auxiliary threads. All workload submit/receive/I/O/dispatch/completion workers have valid counters. Whole-process call tables sum valid thread counters and are lower bounds by these auxiliary omissions.',
'- All six final benchmark cells passed count validation with zero errors, abandoned requests, header-validation failures, or contamination. MemAvailable was above 12 GiB at every capture; raw profile files are all below 1 GB. No frame-pointer rebuild, implementation change, sub-agent, other model, CI, or unrelated test run was used.','',
'## Throughput and profiling overhead','']
rows=[['ratio040, three-run medians',rate(F40),rate(B40),f'{F40/B40:.3f}']]
for mode,label in [('none','New unprofiled, whole 20 s'),('profile','CPU profile, whole 20 s'),('fastcounts','Counters, whole 20 s')]:
 ff=runs[mode,F]['throughput_per_second'];bb=runs[mode,B]['throughput_per_second'];rows.append([label,rate(ff),rate(bb),f'{ff/bb:.3f}'])
for mode,label in [('none','Unprofiled capture window'),('profile','CPU-profile capture window'),('fastcounts','Counter capture window')]:
 vals=[]
 for name in [F,B]:
  if mode=='profile':v=data[name]['messages']/data[name]['seconds']
  elif mode=='fastcounts':v=counts[name]['rate']
  else:
   cap=J/'out-none/run1'/name/'capture';aa=json.load(open(cap/'after.json'));zz=json.load(open(cap/'before.json'));v=(aa['stats']['source']['completed']-zz['stats']['source']['completed'])/(aa['monotonic']-zz['monotonic'])
  vals.append(v)
 rows.append([label]+[rate(v) for v in vals]+[f'{vals[0]/vals[1]:.3f}'])
lines+=table(['Measurement','Framework msg/s','Binding msg/s','F/B'],rows)
lines += [f"New unprofiled rates differ from ratio040 by **{newc['throughput_per_second']/F40-1:+.1%} Framework / {newB/B40-1:+.1%} binding**. These runs use longer warmup/active windows than ratio040. One new unprofiled run per implementation was measured; run-to-run variance and a causal explanation for the baseline change are **not measured**. New results do not replace the supplied ratio040 values.",'']
rows=[]
for label,ff,bb in [('ratio040 medians',base[F],base[B])]:rows.append([label]+[us(v) for v in [ff['cpu'],bb['cpu'],ff['source'],bb['source'],ff['target'],bb['target']]])
rows.append(['New unprofiled full-phase counters']+[us(v) for v in [newC,newb['submit_thread_cores']*1e6/newB,newc['client_cores']*1e6/newc['throughput_per_second'],newb['client_cores']*1e6/newB,newc['server_cpu_percent']*newc['logical_cores']/100*1e6/newc['throughput_per_second'],newb['server_cpu_percent']*newb['logical_cores']/100*1e6/newB]])
lines+=table(['CPU µs/message','F submit','B submit','F source','B source','F target','B target'],rows)
lines += ['CPU-profile capture-window rates are about 5.0% lower for Framework and 5.2% lower for binding than their unprofiled capture windows; counter-window rates are about 7.0% / 9.6% lower. These differences include instrumentation and unmeasured run variation; they are not isolated overhead estimates.','']
lines+=['## Per-thread CPU','',f"**Critical thread: Framework source application submitter TID {fr['critical_tid']}**, `framework_driver_t::run_tasks<BenchPayload>`, {us(ft['cpu_us'])} µs/message and {ft['cores']:.3f} cores. Binding is also limited by its application submitter, TID {br['critical_tid']}, {us(bt['cpu_us'])} µs/message, approximately one core. The Framework target\'s busiest thread is Core IO/1; no target thread reaches one core.",'','Every thread is listed below. Numbered workers are paired by role and TID order, not by a cross-run thread identity. Auxiliary/idle means its specific role was not resolved from sampled stacks. TID and comm, CPU deltas, context switches, and stage values are also preserved in each capture\'s `*.threads.csv`.','']
def thread_roles(rr):
 groups=defaultdict(list)
 for tid,t in rr['threads'].items():
  syms='\n'.join(v['symbol'] for v in t['functions'])
  if 'ZLINKbg/IO/' in t['comm']:key='Core '+t['comm'].replace('ZLINKbg/','')
  elif 'node_receive_role_t::run' in syms:key='Framework receive role'
  elif 'completion_dispatcher_t::completion_dispatcher_t' in syms:key='completion worker'
  elif 'framework_driver_t::run_tasks' in syms or 'zlink_raw_driver_t::run' in syms:key='application submitter'
  elif 'request_loop(' in syms:key='request handler loop'
  elif 'drain_application_owner' in syms:key='application dispatch worker'
  elif 'stats_http_server' in syms:key='stats HTTP worker'
  elif 'offload_executor_t::worker_loop' in syms:key='offload worker'
  else:key='auxiliary/idle'
  groups[key].append((tid,t))
 out={}
 for key,values in groups.items():
  for i,v in enumerate(sorted(values,key=lambda v:int(v[0])),1):out[key+(f' {i:02}' if len(values)>1 else '')]=v
 return out
for role in ['source','target']:
 fm=thread_roles(f['roles'][role]);bm=thread_roles(b['roles'][role]);order=['application submitter','Framework receive role','Core IO/0','Core IO/1','Core IO/2','Core IO/3','request handler loop','application dispatch worker','completion worker','offload worker','stats HTTP worker','auxiliary/idle']
 def key(x):return next((i for i,v in enumerate(order) if x.startswith(v)),99),x
 rows=[]
 for name in sorted(set(fm)|set(bm),key=key):
  vals=[]
  for m in [fm,bm]:
   if name in m:
    tid,t=m[name];vals += [tid,us(t['cpu_us']),f"{t['cores']:.3f}"]
   else:vals += ['—']*3
  rows.append([name]+vals)
 lines+=['### '+role.capitalize(),'']+table(['Thread role','F TID','F µs/msg','F cores','B TID','B µs/msg','B cores'],rows)
lines+=['## Exclusive stage CPU','',
'Allocation and synchronization have precedence over caller ownership: allocator locks stay in allocation; other futex/mutex/CV/scheduler work stays in handoff/wake. Thus the Core column excludes native allocation and lock work already charged elsewhere. This is a global cost taxonomy, not a claim that every lock is owned by Framework.','']
lines+=table(['Stage','Function-to-stage mapping'],[
['Allocation','libc malloc/free/calloc/realloc, tcache, `_int_malloc/_int_free`, new/delete, allocator arena/kernel descendants. C++ has no GC here.'],
['Lane/worker handoff and wake','futex, pthread mutex/condition-variable and scheduler paths; `state_lane_t`, serial execution queues, offload queues, completion dispatcher scheduling. Native locks are included here except allocator locks.'],
['Task/coroutine/result state','Framework task/shared-state/completion-source/request receiver/access, observe/await/result and coroutine lifetime work; standard promise/future machinery. Allocation and locks excluded.'],
['Codec','protobuf/generated BenchPayload serialization; envelope/client-call codec and deadline text formatting; serializer lookup; service-wire encode/decode and byte/container operations. Allocation excluded.'],
['Binding/Core','C++ binding, libzlink, native transport/Asio and kernel send/recv/read/write/epoll descendants. Framework completion-pool Asio is handoff, not transport.'],
['Framework bookkeeping','Remaining registry/admission/deadline registration, transport selection, host dispatch/scope/context, references/callback lifetime, and lifecycle work.'],
['Benchmark driver','Source submit/validate/latency loop, ready/completion-signal non-lock work, stats/metrics handlers.'],
['Other','No usable recognized owner, or unsampled auxiliary CPU. No fabricated stage attribution.']])
rows=[]
for stage in analyze.STAGES:rows.append([stage]+[us(x['stage_us'][stage]) for x in [ft,bt,fr,br,f['roles']['target'],b['roles']['target']]])
rows.append(['**Total**']+[us(x['cpu_us']) for x in [ft,bt,fr,br,f['roles']['target'],b['roles']['target']]])
lines+=table(['µs/message','F critical','B critical','F source','B source','F target','B target'],rows)
lines += [f"The Framework source spends **{fr['cpu_us']-ft['cpu_us']:.3f} µs/message outside the submitter**. Its eight completion workers together use {sum(t['cpu_us'] for tid,t in fr['threads'].items() if 'completion_dispatcher_t::completion_dispatcher_t' in '\n'.join(v['symbol'] for v in t['functions'])):.3f} µs/message. These costs cannot be added to submitter CPU as wall-time savings. The pool is already sized from process-available CPUs in `C/dispatch/provider_task_context.cpp:56` / #3557; the obsolete 16-worker observation is not current evidence.",'']
lines+=['## Counts per completed message','',
'Separate counter runs, using same-window completed replies as denominator. Heap allocations = intercepted malloc + calloc + realloc. Syscalls include futex; columns are not additive. Critical source TIDs differ from the CPU-profile run.','']
rows=[]
for role,scope,label in [('source','critical','critical source'),('source','total','whole source'),('target','total','whole target'),('both','total','both processes')]:
 vals=[]
 for name in [F,B]:
  z=counts[name];v=Counter()
  if role=='both':
   for rr in z['roles'].values():v.update(rr['total'])
  elif scope=='critical':v.update(z['roles'][role]['threads'][z['roles'][role]['critical_tid']])
  else:v.update(z['roles'][role]['total'])
  n=z['messages'];vals += [f"{v.get('context-switches',0)/n:.4f}",f"{v.get('syscalls:sys_enter_futex',0)/n:.4f}",f"{sum(v.get(k,0) for k in ['malloc','calloc','realloc'])/n:.3f}",f"{v.get('raw_syscalls:sys_enter',0)/n:.4f}"]
 rows.append([label]+vals)
lines+=table(['Scope','F switches','F futex','F heap calls','F syscalls','B switches','B futex','B heap calls','B syscalls'],rows)
rows=[]
for ev in ['sendto','recvfrom','read','write','epoll_wait']:
 rows.append([ev]+[f"{counts[name]['roles'][role]['per_message'].get('syscalls:sys_enter_'+ev,0):.4f}" for role in ['source','target'] for name in [F,B]])
lines+=table(['Syscall/message','F source','B source','F target','B target'],rows)
lines+=['The binding benchmark also hosts its stats HTTP service with Framework `app_t::run`; its tiny off-path bookkeeping samples come from that host, not request processing. Raw counters and per-thread totals: [counts-summary.json](counts-summary.json). Calls through uninstrumented allocation APIs and per-allocation-site counts are **not measured**. These counts are observations under counter overhead, not literal construction counts inferred from source.','',
'## Top 20 Framework functions on the critical thread','',
'Ordered by self CPU, then inclusive CPU. Inclusive times overlap and must not be summed. Generated protobuf and benchmark functions, libstdc++ and allocator functions are excluded from this Framework-only list. Full symbols, including lambdas/templates, are in [top20.json](top20.json) and the source critical-functions CSV. C = `framework/languages/cpp/framework/src/runtime/`; P = `framework/languages/cpp/framework/include/zlink/framework/contracts/`.','']
locs=[
('C/messaging/envelope_codec.cpp','header_writer_t::append'),('C/channels/channel_runtime.cpp','route_client_t::submit_request_reply_message_erased'),('C/mesh/raw_mesh_node_owner.cpp','raw_mesh_node_owner_t::request_with_header · lane lambda #1'),('C/messaging/envelope_codec.cpp','header_writer_t::string'),('C/codecs/serializer.cpp','serializer_registry_t::cached_serializer'),('C/foundation/operation_registry.cpp','operation_registry_state_t::register_operation'),('C/mesh/raw_mesh_node_owner.cpp','raw_mesh_node_owner_t::request_with_header'),('C/backend/raw_route_port.cpp','raw_route_port_t::request · callback overload'),('P/channels/channel.hpp','route_client_t::request_to_node<BenchPayload>'),('P/dispatch/request_task.hpp','request_task_access_t::receive<BenchPayload>'),('P/dispatch/task.hpp','task_completion_source_t<void>::complete · isra clone'),('P/dispatch/task.hpp','task_wait_registration_t::prepare_context'),('P/dispatch/task.hpp','ambient_context_scope_t::~ambient_context_scope_t'),('C/dispatch/coroutine_executor.cpp','capture_native_scheduler'),('C/channels/channel_runtime.cpp','route_client_t::submit_request_reply_message_erased · encoded-result projection lambda'),('C/protocol/service_wire_codec.cpp','append_text8'),('C/messaging/envelope_codec.cpp','envelope_codec_t::encode_header'),('P/dispatch/task.hpp','task_shared_state_t<BenchPayload>::on_completed'),('C/mesh/service_topology_registry.cpp','service_topology_registry_t::peer'),('C/protocol/service_wire_codec.cpp','append_u32')]
fs=json.load(open(J/'top20.json'));assert len(fs)==len(locs)==20
rows=[]
for i,(v,(path,func)) in enumerate(zip(fs,locs),1):rows.append([i,f'`{path}:{func}`',us(v['self_us']),us(v['inclusive_us'])])
lines+=table(['#','File:function','Self µs/msg','Inclusive µs/msg'],rows)
lines+=['## Re-ranked R / W / T / M / H','',
'R\'s old intermediate raw-result task/producer/holder and host-result conversion path were removed from this benchmark\'s hot path by #3600. Current `C/mesh/raw_mesh_node_owner.cpp:248` calls the callback overload at `C/backend/raw_route_port.cpp:313`; `C/mesh/mesh_node_runtime.cpp:4396` submits directly into the prepared receiver. The task-returning overload and `compose_node_request_reply` still exist for other paths; their existence is not evidence that the benchmark pays for them. Required public reply/admission state remains.','',
f"Ratio040: F={F40:.3f}, B={B40:.3f}; submit CPU C={C40:.3f} µs/message. Profile critical CPU is {ft['cpu_us']:.3f}. For comparison only, a measured profile scope d is scaled by **C/profileCPU={scale:.4f}**. The ratio ceiling is **U = 10^6 / [B × (C − scaled d)]**, holding other resource costs fixed. This is a submit-CPU capacity bound, not predicted throughput; at d=0 it is {cpu40(0):.3f}, already above observed F/B={F40/B40:.3f} because the submitter is not on CPU for the entire interval. The new-run column uses the new unprofiled C and B with the same stage-mix assumption. Exact candidate savings and post-change rates are **not measured**.",'',
'W/H/T/M selectors are enclosing budgets and retain necessary codec, public-state, invocation or reply work. Thus each is at most the reported budget, often less. Zero critical µs for off-thread work is structural for these candidates; indirect contention benefits are not measured. Candidates overlap and cannot be added.','']
cs=scopes[F];critical=cs['source']['busiest_thread'];wholeS=cs['source']['whole_process'];wholeT=cs['target']['whole_process']
descriptions={'W':'Service-wire aggregation/holder/multipart ownership, native materialization and destruction; required serialization/storage included in ceiling.','H':'Driver task observation, context capture, signal/queue/locks; includes required reply notification, so ready-admission-only saving is smaller.','T':'Target invocation coroutine/result/terminal lifetime; required handler/filter/transport work excluded by selector.','M':'Outer receive-activity coroutine/result/lifetime, not management/deadline work.','R':'Already implemented by #3600; original removable intermediate states/holder are absent from the measured hot path.'}
rows=[]
for rank,tag in enumerate(['W','H','T','M','R'],1):
 d=sum(critical.get(tag,{}).values()) if tag in ['W','H'] else 0.;sp=sum(wholeS.get(tag,{}).values()) if tag!='R' else 0.;tp=sum(wholeT.get(tag,{}).values()) if tag!='R' else 0.
 calc['candidate_bounds'][tag]={'profile_critical_scope_us':d,'ratio040_scaled_scope_us':d*scale,'ratio040_cpu_bound':cpu40(d),'new_run_cpu_bound':newbound(d),'whole_source_scope_us':sp,'whole_target_scope_us':tp}
 rows.append([f'{rank}. {tag}',descriptions[tag],us(d),us(d*scale),f'{cpu40(d):.3f}',f'{newbound(d):.3f}',f'{sp:.3f} / {tp:.3f}'])
lines+=table(['Rank / candidate','Stage / scope','Profile critical max µs','Scaled to ratio040 µs','U ratio040','U new run','Whole source / target scope µs'],rows)
residual=sum(critical.get('R',{}).values());lines += [f"The legacy script\'s broad R selector still tags **{residual:.3f} critical µs** ({sum(wholeS.get('R',{}).values()):.3f} whole-source) of remaining public tasks, callback storage and raw-request parent work. That is **not** the old removable R design. Even treating all of this unrelated residual scope as removable gives only a {cpu40(residual):.3f} ratio040 CPU ceiling; no new representation proposal or achievable saving was measured. See [candidate-scopes.json](candidate-scopes.json) and selector comments in [candidates.py](candidates.py).",'',
'T has more whole-target scope than M, but both have zero direct submitter saving. Their order is secondary to W/H for this cell. M\'s nonzero whole-process measurement is not a per-message management-start count; its invocation frequency is **not measured**.','',
'## Costs outside the earlier candidate list','']
if (J/'extras.json').exists():
 extras=json.load(open(J/'extras.json'));ex=extras[F]['source'];rows=[]
 for key,label,file in [('native message buffer allocation','Native message-buffer allocation (subset of allocation)','core/src/runtime/core/msg.cpp:msg_t::init_size; framework/languages/cpp/extensions/framework-codec-protobuf/include/zlink/codecs/protobuf.hpp:serialize_protobuf'),('epoll_ctl inclusive','Core Asio mailbox/reactor wake: epoll_ctl path (inclusive, overlaps Core/handoff)','core/src/runtime/core/mailbox.cpp:mailbox_t::schedule_if_needed_unlocked → Asio → libc/kernel epoll_ctl'),('continuation context inclusive','Continuation context/scheduler capture (inclusive, overlaps tasks/bookkeeping/allocation)','C/dispatch/coroutine_executor.cpp:capture_native_scheduler; P/dispatch/task.hpp:task_wait_registration_t::prepare_context'),('allocator synchronization','Allocator lock/wake paths (subset of allocation; overlaps other allocation scopes)','libc malloc/free → futex/scheduler'),('strftime self','UTC deadline formatting, libc strftime self only','C/messaging/client_call_codec.cpp:format_utc_deadline')]:
  rows.append([label,f'`{file}`',us(ex['critical'].get(key,0)),us(ex['whole'].get(key,0))])
 lines+=table(['Cost / scope','File:function','Critical µs/msg','Whole source µs/msg'],rows)
lines+=['- **Transport and completion work is a large separate CPU constraint.** Source Core IO/0 and IO/2 together cost '+us(sum(t['cpu_us'] for t in fr['threads'].values() if t['comm'] in ['ZLINKbg/IO/0','ZLINKbg/IO/2']))+' µs/message. Target IO/0 and IO/1 cost '+us(sum(t['cpu_us'] for t in f['roles']['target']['threads'].values() if t['comm'] in ['ZLINKbg/IO/0','ZLINKbg/IO/1']))+' µs/message. Paths include `bindings/cpp/src/Runtime/Messaging/completion_owner.cpp:completion_owner_t::start_async_request / completion_entry_t::submit_request_attempt` and Core transport. The much higher sendto/recvfrom/read/epoll activity is measured; whether faster submission would improve batching is **not measured**. Core decisions remain in Core/binding.',
'- **Completion pool work remains after its affinity sizing fix.** Eight workers at `C/dispatch/provider_task_context.cpp:56` each consume about 3.24–3.26 µs/message. This is required-turn delivery plus callback/allocator/scheduler work, not proof that deleting a fresh completion turn or changing worker count is valid.',
'- **Public projection and ambient-context work remains after R.** See `P/dispatch/request_task.hpp:receive/receiver`, `P/dispatch/task.hpp:on_completed/prepare_context`, and `C/dispatch/coroutine_executor.cpp:capture_native_scheduler`. The source still intercepts about '+f"{counts[F]['roles']['source']['critical_per_message']['malloc']:.1f}"+' allocation calls/message on the submitter. Removing historical internal result states does not remove these public/lifetime/codec/native allocations.',
'- **Deadline formatting is visible independently of result ownership.** `C/messaging/client_call_codec.cpp:format_utc_deadline` calls `gmtime_r/strftime`; `__strftime_internal` alone accounts for about 0.337 critical µs/message. This does not establish that the required wire deadline may be omitted.','',
'## Limits and verification','',
'Exact savings, an implementation A/B experiment, site-level allocation counts, admission ready/pending fraction, correlated per-message hop counts, off-CPU waits, and a full source-line/inline profile are **not measured**. The 4096-byte cell is outside this task and was **not measured**. Snapshot/sampling uncertainty and the baseline-rate difference limit the precision of all scaled ceilings.',
'',
'Repository changes: none. Runtime rule/decision count before → after: unchanged. No new state/queue/lock was added. Implementation/refactoring audit: not performed in this evidence-only task. Final workload failures: none. ','',
'## Gap to 0.6 and first change','',
f"At ratio040, reaching 0.6 requires {0.6*B40:,.0f} Framework msg/s: effective spacing must fall from **{interval:.3f} to {goal:.3f} µs/message**, a gap of **{gap:.3f} µs/message**. The table scales the measured critical-thread mix to ratio040\'s submit CPU and expresses each entire stage budget as a fraction of that gap. These are maximum cost budgets, not fully removable overhead; they include work also required by binding.",'']
lines += [f"There is also an aggregate CPU requirement: with eight CPUs per endpoint, the ratio040 goal allows **{8e6/(.6*B40):.3f} CPU µs/message per process**. Ratio040 source/target totals are {base[F]['source']:.3f}/{base[F]['target']:.3f}; they exceed that budget by {base[F]['source']-8e6/(.6*B40):.3f}/{base[F]['target']-8e6/(.6*B40):.3f} µs/message. Improving submission alone therefore leaves whole-process work to reduce. This is a separate CPU-capacity constraint, not extra critical-path latency.",'']
rows=[]
for stage,v in sorted(ft['stage_us'].items(),key=lambda kv:-kv[1]):
 if not v:continue
 value=v*scale;calc['stage_gap_fractions'][stage]={'profile_us':v,'ratio040_scaled_us':value,'fraction_of_gap':value/gap};rows.append([stage,us(v),us(value),f'{value/gap:.1%}'])
lines+=table(['Critical stage','Profile µs/msg','Scaled ratio040 µs/msg','Fraction of gap'],rows)
w=sum(critical['W'].values());h=sum(critical['H'].values());lines += [f"The named candidates\' observed ceilings cover only **{w*scale/gap:.1%} of the gap for W** and **{h*scale/gap:.1%} for H**; T/M contribute no direct submitter time and the original R is already spent. The allocation stage represents **{ft['stage_us']['allocation']*scale/gap:.1%}** of the required gap, bookkeeping **{ft['stage_us']['Framework bookkeeping']*scale/gap:.1%}**, binding/Core **{ft['stage_us']['binding/Core']*scale/gap:.1%}**, and codec **{ft['stage_us']['codec']*scale/gap:.1%}**. No single observed stage establishes a path to 0.6.",'',
'**First change I would implement: W, a service-wire writer that writes into the final native message buffer and consumes moved multipart ownership directly**, in `C/protocol/service_wire_codec.cpp:encode_application_payload`, using `C/backend/raw_binding_adapter.hpp:materialize_binding_part` as the current conversion boundary. It targets the largest remaining named critical-thread scope and allocation/ownership duplication without changing admission, completion, routing or wire semantics. Keep the existing public serializer path. Preserve required bytes and lifetimes; measure the change against the current unprofiled cell before attributing any gain. Its measured scope is an upper bound, and by itself it is far short of 0.6.','']
(J/'report.md').write_text('\n'.join(lines));(J/'report-calculations.json').write_text(json.dumps(calc,indent=2));print(J/'report.md')
