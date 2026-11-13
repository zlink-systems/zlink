import analyze,json,re
from collections import Counter,defaultdict
R=analyze.ROOT;data=json.load(open(R/'analysis.json'))
# Enclosing-symbol ceilings deliberately retain public state/required codec work.
# They are a superset of each proposal, not isolated savings or disjoint costs.
W_RE=re.compile(r'protocol::(?:encode_application_payload|application_payload_t::(?:from_parts|~application_payload_t))|materialize_binding_part')
R_RE=re.compile(r'raw_request_completion_t|host::operation_completion_t|start_admitted_request|request_admission_completion|request_task_access_t|request_task_t|task_completion_source_t<void>|compose_node_request_reply|observe_request_failure|raw_mesh_node_owner_t::observe_request')
M_RE=re.compile(r'next_receive_activity_async')
T_RE=re.compile(r'route_handler_invoker_t::invoke|mesh_node_builder_t::add_route_request_handler|mesh_node_builder_t::add_route_send_handler')
P_RE=re.compile(r'observe_send_completion|send_completion_state_t|task_completion_source_t<zlink::submit_result_t>|task_shared_state_t<zlink::submit_result_t>|application_task_shared_state_t<zlink::submit_result_t>')
def first_owner(fr):
 for s,d in fr:
  if analyze.ALLOC.search(s):continue
  if ('zlink::framework::' in s or '/libzlink.so' in d or 'framework_driver_t::' in s or 'binding_driver_t::' in s or 'raw_driver_t::' in s):return s
 return ''
allc={}
for cell in ('zlink-framework-cpp-request-backpressure-1024',):
 res={};bp='backpressure' in cell
 for role in ('source','target'):
  rr=data[cell]['roles'][role];samps=json.load(open(R/'out-profile/run1'/cell/'capture'/f'{role}.samples.json'));bytid=defaultdict(list)
  for s in samps:bytid[s['tid']].append(s)
  totals=defaultdict(Counter);crit=defaultdict(Counter);symtags=defaultdict(Counter);threadtags=defaultdict(Counter)
  for tid,t in rr['threads'].items():
   ss=bytid.get(tid,[]);wt=sum(x['period'] for x in ss);factor=t['cpu_us']/wt if wt else 0
   for x in ss:
    fr=x['frames'];cat=analyze.classify(fr);owner=first_owner(fr);p=x['period']*factor;tags=[];syms=[s for s,d in fr]
    if any(W_RE.search(s) for s in syms):tags.append('W')
    # R ceiling also includes anonymous bookkeeping directly inside raw request and host submission.
    if bp and role=='source' and (cat=='task/coroutine/future' or ((cat=='allocation' or cat=='Framework bookkeeping' or cat=='handoff/wake') and (R_RE.search(owner) or 'raw_route_port_t::request(' in owner))):tags.append('R')
    # H covers all task observation/notification issued directly by the benchmark.
    # Restrict to driver-owned observers: no request submit/body caller in between.
    if bp:
     sig=any('completion_signal_t::' in s for s in syms)
     direct_obs=any(('observe_task_completion<void' in s or 'task_shared_state_t<void>::on_completed' in s) for s in syms)
     no_submit=not any('submit_request_reply_message_erased' in s or 'request_with_header' in s or 'compose_node_request_reply' in s for s in syms)
     if no_submit and (sig or direct_obs):tags.append('H')
    # A: allocation/lifetime instructions of the host conversion's result<void> coroutine,
    # plus its own symbol instructions; child routing/transport/codec work remains excluded.
    if not bp and role=='source':
     aobj=('coroutine_task_shared_state_t<zlink::framework::result_t<void>' in owner or 'task_shared_state_t<zlink::framework::result_t<void>' in owner)
     aown='app_t::_apply_zlink_framework()' in owner
     if (cat in ('allocation','task/coroutine/future','Framework bookkeeping','handoff/wake')) and (aobj or aown):tags.append('A')
     if cat in ('allocation','task/coroutine/future','Framework bookkeeping','handoff/wake') and P_RE.search(owner):tags.append('P-visible')
     # Conservative unresolved-symbol ceiling: all task machinery and all local allocations
     # in submit_send/start_send_on_lane, including required immediate-path pieces.
     if cat=='task/coroutine/future' or (cat in ('allocation','Framework bookkeeping','handoff/wake') and (P_RE.search(owner) or 'raw_route_port_t::submit_send(' in owner or 'raw_mesh_node_owner_t::start_send_on_lane(' in owner)):tags.append('P')
    if (M_RE.search(owner) and cat in ('Framework bookkeeping','handoff/wake')) or (cat in ('allocation','task/coroutine/future') and any(M_RE.search(s) for s in syms)):tags.append('M')
    # T ceiling: all direct invocation lifetime/task work and all nested allocations;
    # nested codec, handler, reply transport and gate work are not proposed for deletion.
    if role=='target' and any(T_RE.search(s) for s in syms) and cat in ('allocation','task/coroutine/future'):tags.append('T')
    if role=='target' and T_RE.search(owner) and cat in ('Framework bookkeeping','handoff/wake'):tags.append('T')
    if role=='target' and cat in ('allocation','task/coroutine/future') and ('application_dispatch_terminal_owner_t' in owner or 'mesh_node_host_service_t::dispatch_application(' in owner):tags.append('T')
    if role=='source' and set(tags)&({'R','W','H'} if bp else {'A','W','P'}):tags.append('union')
    for tag in set(tags):
     totals[tag][cat]+=p;threadtags[tid][tag]+=p
     if tid==rr['critical_tid']:crit[tag][cat]+=p
     symtags[tag][owner]+=p
  res[role]={'whole_process':{k:dict(v) for k,v in totals.items()},'busiest_thread':{k:dict(v) for k,v in crit.items()},'threads':{k:dict(v) for k,v in threadtags.items()},'owners':{k:dict(v) for k,v in symtags.items()}}
 allc[cell]=res
(R/'candidate-scopes.json').write_text(json.dumps(allc,indent=2))
for cell,res in allc.items():
 print(cell)
 for role,z in res.items():
  print(role,'whole',{k:round(sum(v.values()),4) for k,v in z['whole_process'].items()},'critical',{k:round(sum(v.values()),4) for k,v in z['busiest_thread'].items()})
  if role=='source':
   for tag,owners in z['owners'].items():
    print('OWNERS',tag)
    for own,v in sorted(owners.items(),key=lambda it:-it[1])[:10]:print(round(v,4),own[:200])
