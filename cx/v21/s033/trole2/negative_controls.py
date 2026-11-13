from pathlib import Path
import subprocess, shlex, json, os, resource
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
root=Path('/root/worktree/zlink-v21-s033/trole')
job=Path('/root/jobs/fx/cx/v21/s033/trole2')
build=root/'build-trole'
out=job/'negative';out.mkdir(exist_ok=True)
base='framework/languages/cpp/framework/src/runtime/'
exec_source=base+'dispatch/offload_executor.cpp'
mail_source=base+'mesh/service_mailbox.cpp'
serial_source=base+'execution/serial_execution_queue.cpp'
provider_source=base+'dispatch/provider_task_context.cpp'
invoker_source=base+'channels/route_handler_invoker.cpp'
new='test_cpp_framework_receive_role_handoff'
# Each mutation is confined to a job-local source copy and linked before the
# unmodified product archives. The worktree and its build objects stay intact.
cases=[
 ('T01',exec_source,'    std::unique_lock lock (_mutex, std::try_to_lock);','    return nullptr;\n    std::unique_lock lock (_mutex, std::try_to_lock);',new,'*T01*'),
 ('T02',exec_source,'std::unique_lock lock (_mutex, std::try_to_lock);','std::unique_lock lock (_mutex);',new,'*T02*'),
 ('T03',exec_source,'    std::unique_lock lock (_mutex, std::try_to_lock);','    for (unsigned record = 0; record != 64; ++record) { if (_mutex.try_lock ()) _mutex.unlock (); }\n    std::unique_lock lock (_mutex, std::try_to_lock);',new,'*T03*'),
 ('T04',exec_source,'bool offload_executor_t::try_submit_internal (std::function<void ()> work)\n{','bool offload_executor_t::try_submit_internal (std::function<void ()> work)\n{\n    if (publishing_role ()) { work (); return true; }','test_cpp_framework_mailbox_owner_dispatch',''),
 ('T05',mail_source,'queue.records.push_back (std::move (record));','queue.records.push_front (std::move (record));',new,'*T05Other*'),
 ('T05b',exec_source,'    if (_queue.empty ())\n        return nullptr;','    if ((!current || !current->assignment) && _active > _role_active) return nullptr;\n    if (_queue.empty ())\n        return nullptr;',new,'*T05Independent*'),
 ('T06',invoker_source,'auto terminal = std::move (incoming_terminal);','auto terminal = std::move (incoming_terminal);\n    terminal.enter_handler ();','test_cpp_framework_handler_entry_permit','HandlerEntryPermit.RemoteNode*'),
 ('T07',serial_source,'_spot_gate && _spot_gate->_lane_policy.allows_turn_yield () ? _spot_gate.get ()','false ? _spot_gate.get ()',new,'*T07*'),
 ('T08',provider_source,'const auto count = std::max (minimum_worker_count,','const auto count = std::min (1u,',new,'*T08*'),
 ('T09',exec_source,'available->assignment = std::move (receiver->assignment);','available->assignment = std::move (receiver->assignment);\n    --_active; --_role_active;',new,'*T09Reserved*'),
 ('T09b',exec_source,'return _queue.empty () || std::holds_alternative<std::shared_ptr<role_t>> (_queue.back ().work);','return _queue.empty ();',new,'*T09QueuedRoles*'),
 ('T10',exec_source,'queue_worker->assignment.emplace (std::move (_queue.front ()));\n    _queue.pop_front ();','queue_worker->assignment.emplace (std::move (_queue.back ()));\n    _queue.pop_back ();',new,'*T10*'),
 ('T10b',exec_source,'if (successor)\n                    successor->ready.notify_one ();','if (successor) { successor->ready.notify_one (); successor->ready.notify_one (); }','test_cpp_framework_mailbox_owner_dispatch',''),
 ('T11',mail_source,'ready->second (ready->first.front ());\n        ready->first.pop_front ();','auto owner = std::move (ready->first.front ());\n        ready->first.pop_front ();\n        ready->second (owner);',new,'*T11*'),
]
prepared=[]
rules=['builddir = '+str(out)]
for name,source,old,replacement,target,pattern in cases:
    original=root/source
    contents=original.read_text()
    assert contents.count(old)==1,(name,contents.count(old))
    mutant=out/(name+'.cpp')
    updated=contents.replace(old,replacement)
    if not mutant.exists() or mutant.read_text()!=updated: mutant.write_text(updated)
    relative=source.removeprefix('framework/languages/cpp/')
    objdir='zlink_framework_m6a_objects' if name in ('T01','T02','T03','T04','T05','T05b','T09','T09b','T10','T10b','T11') else 'zlink_framework_provider_abstractions' if name=='T08' else 'zlink_framework'
    object_path='CMakeFiles/'+objdir+'.dir/'+relative+'.o'
    commands=subprocess.check_output(['ninja','-t','commands',object_path],cwd=build,text=True).splitlines()
    args=shlex.split(commands[-1]);args[1:1]=['-iquote',str(original.parent)]
    obj=out/(name+'.o')
    args[args.index('-o')+1]=str(obj)
    args[args.index('-c')+1]=str(mutant)
    if '-MF' in args: args[args.index('-MF')+1]=str(out/(name+'.d'))
    if '-MT' in args: args[args.index('-MT')+1]=str(obj)
    rules += ['rule compile_'+name, '  command = '+shlex.join(args).replace('$','$$'),
              '  depfile = '+str(out/(name+'.d')), '  deps = gcc',
              'build '+str(obj)+': compile_'+name+' '+str(mutant)]
    command=subprocess.check_output(['ninja','-t','commands',target],cwd=build,text=True).splitlines()[-1]
    args=shlex.split(command)
    if '&&' in args:
        args=args[args.index('&&')+1:];args=args[:args.index('&&')]
    args.insert(1,str(obj));binary=out/(name+'-'+target)
    args[args.index('-o')+1]=str(binary)
    link_inputs=list(dict.fromkeys(a for a in args[1:] if not a.startswith('-') and (a.endswith(('.o','.a')) or '.so' in Path(a).name)))
    rules += ['rule link_'+name,'  command = '+shlex.join(args).replace('$','$$'),
              'build '+str(binary)+': link_'+name+' '+' '.join(link_inputs)]
    prepared.append((name,source,target,pattern,binary,replacement))
rules += ['default '+' '.join(str(item[4]) for item in prepared)]
(out/'build.ninja').write_text('\n'.join(rules)+'\n')
with (out/'build.log').open('w') as log:
    command=['prlimit','--as=26843545600','--','ninja','-f',str(out/'build.ninja'),'-j3','-k','0']
    if os.environ.get('ZLINK_HANDOFF_BUILD_SLOT')!='1': command.insert(0,'/root/jobs/fx/cx/cpp-build-slot.sh')
    rc=subprocess.call(command,
                       cwd=build,stdout=log,stderr=subprocess.STDOUT)
if rc: raise RuntimeError(('negative controls build',rc))
manifest=[{'id':name,'source':source,'target':target,'pattern':pattern,
           'binary':str(binary),'mutation':replacement}
          for name,source,target,pattern,binary,replacement in prepared]
(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
print('built negative controls:',len(manifest),flush=True)
