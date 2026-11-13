import json,os,re,collections
J='/root/jobs/fx/cx/v21/s033/csite2/'
data=json.load(open(J+'analysis-final.json'))
fw=json.load(open(J+'framework-final-counts.json')); bd=json.load(open(J+'binding-role-counts.json'))

def metric(raw,side,role,kind):
 d=next(x for x in data if x['label']==raw and x['side']==side)
 return d['calls'].get(f'{role}:{kind}',0)/d['requests']

def shortfn(fn):
 if fn.startswith('UNATTRIBUTED'): return 'Unattributed — no Framework/binding frame in 20 frames'
 fn=fn.replace('(anonymous namespace)::','')
 head=fn.split('(',1)[0].strip().split()
 sym=head[-1] if head else fn
 sym=sym.split('<',1)[0]
 parts=[x for x in sym.split('::') if x]
 if len(parts)>=2: return '::'.join(parts[-2:])
 return sym

def classify(loc,fn,run):
 if 'UNATTRIBUTED' in fn or loc.startswith('?:'):
  return 'Unknown — no Framework/binding frame in captured stack', 'No Framework decision can be identified from the sampled 20-frame stack.'
 if run.lower().startswith('binding'):
  return 'Yes',decision(loc,fn)
 if 'bindings/cpp/' in loc:
  return 'Yes',decision(loc,fn)
 if 'framework/bench/grpc/cpp/' in loc:
  if 'source_t::run_phase' in fn: return 'Yes','Benchmark phase trigger and result collection; no runtime decision.'
  return 'No','Framework benchmark input, task, or result handling; no runtime decision.'
 return 'No',decision(loc,fn)

def decision(loc,fn):
 f=loc.lower(); n=fn.lower()
 if 'raw_mesh_node_owner' in f or 'start_admitted_request' in n or 'await_request_admission' in n: return 'Mesh peer/admission selection and request/reply submission.'
 if 'operation_registry' in f or 'register_operation' in n: return 'Logical request operation registration and terminal lifetime.'
 if 'completion_owner' in f or 'completion_entry' in n or 'submit_raw_request_state' in n or 'request_reply.cpp' in f: return 'Binding request submission, completion registration, drain, or reply publication.'
 if 'message.cpp' in f or 'message.hpp' in f or 'message_t::message_t' in n: return 'Binding message buffer construction and ownership.'
 if 'poller' in f or 'poller_t' in n: return 'Socket poll readiness and event wait.'
 if 'service_topology_registry' in f: return 'Request peer lookup and peer observation.'
 if 'mesh_node_host_service' in f or 'public_host_runtime' in f or 'dispatch_ready' in n: return 'Receive readiness and application dispatch.'
 if 'service_mailbox' in f: return 'Application dispatch mailbox acceptance and readiness.'
 if 'route_handler' in f or 'route_packet' in f or 'channel_' in f: return 'Route packet classification, handler invocation, or reply construction.'
 if 'codec' in f or 'wire' in f or 'serialize' in f or 'payload' in n: return 'Request header/payload encoding, decoding, or correlation.'
 if 'state_lane' in f or 'coroutine_executor' in f or 'task_wait' in n or 'dispatch_completion' in n: return 'Completion turn scheduling and task completion delivery.'
 if 'completion' in f or 'completion' in n: return 'Completion state capture and terminal delivery.'
 if 'application_supply_slot' in n: return 'Receive-side application admission waiter state.'
 if 'socket.cpp' in f or 'native_receive' in f or 'reply_operations' in f: return 'Binding socket receive or raw reply submission.'
 if 'UNATTRIBUTED' in fn: return 'No Framework decision can be identified from the sampled 20-frame stack.'
 return 'Call site in the Framework request, receive, or completion path.'

# Side-by-side per-request counts, retaining csite's exact Framework values.
count_rows=[
 ('Source / caller','17.905','36.429',metric('binding-role','source',1,'heap'),metric('binding-role','source',1,'lock')),
 ('Source / receive role','9.125','44.018',None,None),
 ('Source / completion workers','7.001','6.002',metric('binding-role','source',3,'heap'),metric('binding-role','source',3,'lock')),
 ('Source / other threads','2.240','9.329',metric('binding-role','source',0,'heap'),metric('binding-role','source',0,'lock')),
 ('Source / total','36.271','95.778',sum(metric('binding-role','source',r,'heap') for r in (0,1,3)),sum(metric('binding-role','source',r,'lock') for r in (0,1,3))),
 ('Target / all threads','42.747','58.072',metric('binding-role','target',0,'heap'),metric('binding-role','target',0,'lock'))]
md=[]
coverage_lines=[]
for x in data:
 groups=collections.defaultdict(list)
 for row in x['sites']: groups[row['role']].append(row)
 for role,rows in groups.items():
  last={kind:max((r['coverage_after'] for r in rows if r['kind']==kind),default=0) for kind in ('lock','heap')}
  label='Framework' if x['label']=='framework-final' else 'Binding'
  role_label='all target threads' if x['side']=='target' else role
  coverage_lines.append(f"{label} {x['side']} {role_label}: {last['lock']*100:.1f}% / {last['heap']*100:.1f}%")
md += ['# C++ binding request counts and sampled call sites — 0.33.0 #1639 / #3867','',
       'Worktree: `/root/worktree/zlink-3867h`, branch `perf/3867-cpp-receive-handoff`, HEAD `5ecae5de8b31c0633a6f7e8c1cb897b26c5ac87a`. The worktree was not modified. All harnesses, counters, binaries, logs, and this report are under `csite2/`.','',
       '## 1. Per-request counts','',
       'Same `request-backpressure`, 1 KiB payload, source CPUs 0–7, target CPUs 8–15, 30 s warmup and 5 s active phase. Binding run completed 807,232 requests; source/target counts agree, errors 0, in-flight 0. Current-HEAD Framework stack run completed 174,962 requests with the same checks. MemAvailable was 27.6 GiB and 27.8 GiB at the respective run checks.','',
       'Heap counts are `malloc`, `calloc`, `realloc`, and aligned allocation calls. Explicit mutex counts are `pthread_mutex_lock` plus successful `pthread_mutex_trylock`; condition-wait implicit reacquisition is excluded. These are calls per completed request, not wait time or contention. Framework comparison values are from `csite/report.md`; binding values are this run.','',
       '| Process / role | Framework heap/request | Framework mutex/request | zlink-cpp heap/request | zlink-cpp mutex/request |','|---|---:|---:|---:|---:|']
for role,fh,fl,bh,bl in count_rows:
 md.append(f'| {role} | {fh} | {fl} | {bh:.3f} | {bl:.3f} |' if bh is not None else f'| {role} | {fh} | {fl} | — | — |')
md += ['',
       'Binding source role 3 had no counted events (0.000/0.000); its idle threads are included for completeness. Binding source totals are 12.463 heap and 27.035 mutex calls/request. The Framework values in this comparison are the prior csite capture; the current-HEAD sampled-stack run used for the site table measured source caller 17.933/36.465, receive 9.607/46.661, completion workers 7.003/6.003, other 2.822/10.461, and target 44.820/67.120 (heap/mutex). This difference is retained rather than mixing the two captures.','',
       '## 2. Lock and heap call-site samples','',
       'The copied interceptor recorded one stack per 64th mutex/heap call per thread, up to 20 frames, and symbolized executable frames. A 524,288-entry ring retained the complete active sample windows: Framework source 374,401 samples, target 306,014; binding source 487,581, target 228,623. Each site’s calls/request is its sample share multiplied by that role’s exact interceptor count/request. Site sample shares are shown; selected top sites per role/category reach at least 80%, with cumulative coverage listed below.','',
       'Framework role markers are caller, receive role, completion workers, and other source threads. The binding benchmark copy marks its raw-driver run thread as caller; all remaining source threads are other. Target rows aggregate all target threads. `Unknown` means no Framework/binding frame was present in the captured 20 frames.','',
       'Selected top-site cumulative sample coverage (lock / heap): '+ '; '.join(coverage_lines)+'.','',
       '| Site (function, file:line) | Role | Locks/request | Heap/request | Site sample share (lock/heap) | In binding run? | Framework decision served |','|---|---|---:|---:|---:|---|---|']
# Gather each selected top-site row, merge lock and heap values for each site/role/run.
merged={}
for x in data:
 run='Framework' if x['label']=='framework-final' else 'Binding'
 side='source' if x['side']=='source' else 'target'
 for r in x['sites']:
  role=f"{run} {side} / {'all target threads' if side=='target' else r['role'].lower()}"
  key=(run,role,r['site'],r['loc'])
  z=merged.setdefault(key,{'lock':None,'heap':None,'lc':None,'hc':None,'ls':None,'hs':None})
  z[r['kind']]=r['calls_per_request']; z['lc' if r['kind']=='lock' else 'hc']=r['coverage_after']; z['ls' if r['kind']=='lock' else 'hs']=r['sample_fraction']
for (run,role,fn,loc),v in sorted(merged.items(),key=lambda kv:(kv[0][0],kv[0][1],kv[0][3],kv[0][2])):
 disp=shortfn(fn); loc2=loc
 # Correct the few inline-symbol source mappings using the matching implementation definition in this HEAD.
 if 'node_receive_role_t::rotate' in fn: loc2='framework/languages/cpp/framework/src/runtime/mesh/mesh_node_host_service.cpp:2097'
 elif 'service_topology_registry_t::peer(' in fn: loc2='framework/languages/cpp/framework/src/runtime/mesh/service_topology_registry.cpp:515'
 elif 'poller_t::impl::wait' in fn: loc2='bindings/cpp/src/Runtime/Eventing/poller.cpp:345'
 elif 'raw_route_port_t::request_impl' in fn: loc2='framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp:351'
 elif 'raw_route_port_t::request(' in fn: loc2='framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp:311'
 elif 'external_message_t::from' in fn: loc2='bindings/cpp/src/Runtime/Messaging/message.cpp:273'
 b,why=classify(loc2,fn,run)
 site=f'{disp}, `{loc2}`' if not fn.startswith('UNATTRIBUTED') else f'{disp}, —'
 lk='—' if v['lock'] is None else f"{v['lock']:.3f}"
 hp='—' if v['heap'] is None else f"{v['heap']:.3f}"
 lc='—' if v['ls'] is None else format(v['ls']*100,'.2f')+'%'
 hc='—' if v['hs'] is None else format(v['hs']*100,'.2f')+'%'
 cov=lc+' / '+hc
 md.append(f'| {site} | {role} | {lk} | {hp} | {cov} | {b} | {why} |')
md += ['',
       '## Measurement limits','',
       'Sampled stacks identify the first symbolized Framework/binding frame available within 20 frames. Rows with no such frame are kept as unknown/native coverage rather than assigned to a Framework function. Per-site values are estimates from sampled stack proportions; per-role totals above are exact interceptor counts. No product or worktree changes were made.','',
       '리팩토링 점검: 성능 0건, POSDDD 0건, 불필요 코드 0건 발견. 고친 항목 없음, 넘긴 항목 없음']
open(J+'report.md','w').write('\n'.join(md)+'\n')
print('report rows',len(merged),'bytes',os.path.getsize(J+'report.md'))
