from pathlib import Path
import importlib.util,json,re,collections,csv
ROOT=Path('/root/jobs/fx/cx/v21/s033/cthin')
spec=importlib.util.spec_from_file_location('base',ROOT/'analyze.py');base=importlib.util.module_from_spec(spec);spec.loader.exec_module(base)
# Leaf-first semantic scope attribution. Allocation and synchronization stay with
# the enclosing operation here; analyze.py separately reports exclusive costs.
SCOPES=[
 ('binding builder',re.compile(r'^zlink::(?:detail::)?(?:router_socket_t::request|request_operation_t::message|request_submit_operation_t::message|operation_state_t::|append_send_part|make_routed_operation)')),
 ('binding submission',re.compile(r'zlink::(?:detail::)?(?:request_submit_operation_t::async|completion_owner_t::start_async_request|completion_entry_t::(?:start_request|submit_request_attempt))|submit_raw_request_state|zlink_request')),
 ('binding send',re.compile(r'zlink::send_submit_operation_t::async|zlink_send_rid|submit_raw_send')),
 ('service frame construction',re.compile(r'encode_application_payload_(?:into|message)|encode_node_request_header_message|encode_node_send_header_message|native_wire_writer_t::')),
 ('logical registration',re.compile(r'operation_registry_state_t::(?:register_operation|index_deadline|next_deadline)|operation_completion_item_t::operation_completion_item_t')),
 ('binding await bridge',re.compile(r'observe_request_completion|submit_request_once|request_submission_stages_t')),
 ('source envelope and codec',re.compile(r'client_call_codec_t::|envelope_codec_t::encode|format_utc_deadline|serialize_protobuf|serializer_registry_t::|serialize_with_content_type|encode_node_send_parts')),
 ('source public receiver',re.compile(r'request_task_access_t::receive|request_task_t.*(?:receiver|request_task_t)|channel_request_call_t::(?:start|async)|route_client_t::request_to_node')),
 ('source owner and lane',re.compile(r'raw_mesh_node_owner_t::(?:request_with_header|start_send|send_with_header_result|submit_owned_send_result)|state_lane_t::(?:run_sync|schedule_drain|drain_loop)')),
 ('source channel/mesh adaptation',re.compile(r'route_client_t::submit_(?:request_reply_message_erased|send_erased)|mesh_node_runtime_t::(?:submit_node_request_reply|request_node_reply)|compose_node_request_submission|await_request_admission|project_task_terminal|resolve_node_transport')),
 ('benchmark completion observation',re.compile(r'completion_signal_t::|observe_task_completion')),
]
INCLUSIVE={
 'public receive':r'request_task_access_t::receive',
 'channel submit':r'route_client_t::submit_request_reply_message_erased|route_client_t::submit_send_erased',
 'raw owner turn':r'raw_mesh_node_owner_t::request_with_header.*lambda|raw_mesh_node_owner_t::start_send_on_lane',
 'registry registration':r'operation_registry_state_t::register_operation',
 'binding request async':r'zlink::request_submit_operation_t::async',
 'binding send async':r'zlink::send_submit_operation_t::async',
 'raw port request':r'raw_route_port_t::request_impl',
 'service frame':r'encode_application_payload_into|encode_application_payload_message',
 'envelope create':r'client_call_codec_t::create_envelope',
 'envelope encode':r'client_call_codec_t::encode_payload_parts|envelope_codec_t::encode_header',
 'typed serializer':r'serialize_protobuf|BenchPayload.*_InternalSerialize|google::protobuf::MessageLite::Serialize',
 'binding observer':r'observe_request_completion',
 'logical terminal':r'operation_registry_state_t::(?:complete|take)|deliver_request_terminal',
 'dispatcher publication':r'post_completions|dispatch_completion\(',
 'target owner drain':r'drain_application_owner',
 'target dispatch':r'mesh_node_host_service_t::dispatch_application',
 'target handler/filter':r'route_handler_invoker_t::invoke|invoke_filters_async|echo_handler_t::handle|command_handler_t::handle',
 'target reply':r'raw_route_port_t::reply|raw_mesh_node_owner_t::reply',
 'poller':r'raw_route_port_t::poll',
 'target receive':r'raw_route_port_t::receive_if_ready',
 'TCP send':r'__sys_sendto|tcp_sendmsg',
 'TCP recv':r'__sys_recvfrom|tcp_recvmsg',
 'Core request API':r'^zlink_request$',
 'Core send API':r'^zlink_send_rid$',
 'Core reply API':r'^zlink_reply$',
 'native message allocation':r'zlink_msg_init_size|message_t::allocate',
}
inc_re={k:re.compile(v) for k,v in INCLUSIVE.items()}
analysis=json.loads((ROOT/'analysis.json').read_text());out={}
for cell,r in analysis.items():
 out[cell]={}
 cell_dir=Path(r['directory'])
 for role,rr in r['roles'].items():
  samples=json.loads((cell_dir/'capture'/f'{role}.samples.json').read_text());per=collections.defaultdict(list)
  for x in samples:per[x['tid']].append(x)
  roles={};rd={}
  log=(cell_dir/f'{role}.log').read_text()
  caller_marks=set(re.findall(r'ROLE source-submitter tid=(\d+)',log))
  app_marks=set(re.findall(r'ROLE app-worker tid=(\d+)',log))
  for tid,t in rr['threads'].items():
   sm=per[tid];wt=sum(x['period'] for x in sm);factor=t['cpu_us']/wt if wt else 0
   scopes=collections.Counter();incs=collections.Counter();unresolved=0
   for x in sm:
    frames=x['frames'];label=None
    for sym,dso in frames:
     for name,rx in SCOPES:
      if rx.search(sym):label=name;break
     if label:break
    scopes[label or 'other enclosing work']+=x['period']
    for name,rx in inc_re.items():
     if any(rx.search(s) for s,d in frames):incs[name]+=x['period']
    if not frames or frames[0][0]=='[unknown]':unresolved+=1
   role_name='auxiliary'
   if t['comm'].startswith('ZLINKbg/IO/'):role_name='Core I/O'
   elif role=='source' and tid in caller_marks:role_name='caller'
   elif any(any('node_receive_role_t::run' in s or 'node_receive_role_t::rotate' in s for s,d in x['frames']) for x in sm):role_name='receive role'
   elif any(any('completion_dispatcher_t' in s for s,d in x['frames']) for x in sm):role_name='completion worker'
   elif role=='target' and (tid in app_marks or any(any('drain_application_owner' in s or 'mesh_node_host_service_t::dispatch_application' in s for s,d in x['frames']) for x in sm)):role_name='application worker'
   elif role=='source' and t['cpu_us']>0.1 and any(any('raw_driver_t::' in s or 'binding_driver_t::' in s or 'zlink_driver_t::' in s or 'ready_queue_t::run' in s for s,d in x['frames']) for x in sm):role_name='caller'
   elif role=='target' and any(any(re.search(r'\b(request_loop|command_loop)\b',s) for s,d in x['frames']) for x in sm):role_name='raw receive/handler'
   rd[tid]={'role':role_name,'comm':t['comm'],'cpu_us':t['cpu_us'],'cores':t['cores'],'samples':t['samples'],'unknown_leaves':unresolved,'exclusive_scopes_us':{k:v*factor for k,v in scopes.items()},'inclusive_scopes_us':{k:v*factor for k,v in incs.items()}}
  totals=collections.defaultdict(lambda:dict(cpu_us=0,cores=0,tids=[],stage_us=collections.Counter(),inclusive_scopes_us=collections.Counter()))
  for tid,t in rd.items():
   g=totals[t['role']];g['cpu_us']+=t['cpu_us'];g['cores']+=t['cores'];g['tids'].append(tid)
   g['stage_us'].update(rr['threads'][tid]['stage_us']);g['inclusive_scopes_us'].update(t['inclusive_scopes_us'])
  out[cell][role]={'groups':dict(totals),'threads':rd}
(ROOT/'scopes.json').write_text(json.dumps(out,indent=2))
for cell,r in out.items():
 print(cell)
 for role,rr in r.items():
  print(role,{k:round(v['cpu_us'],3) for k,v in rr['groups'].items()})
  for tid,t in rr['threads'].items():
   if t['role']=='caller':print('caller',tid,round(t['cpu_us'],3),'scopes', {k:round(v,3) for k,v in t['exclusive_scopes_us'].items()},'inclusive',{k:round(v,3) for k,v in t['inclusive_scopes_us'].items()})
