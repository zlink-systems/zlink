import pathlib,subprocess
j=pathlib.Path(__file__).parent;b='/root/worktree/zlink-v21-s033/psmeas-branch/.psmeas-build/bench_cpp_pm_server'
lines=(j/'pm-symbols.txt').read_text().splitlines()
specs=[('publish','mesh_node_host_service_t::make_application_owner_work(', 'owner=+0(+0(%r8)):string'),('start','mesh_node_host_service_t::drain_application_owner(', 'owner=+0(+0(%cx)):string'),('dispatch','public_host_runtime_t::dispatch_application_owner(', 'owner=+0(+0(%dx)):string')]
specs += [('http','pm_http_t::handle(pm_http_t::handle(', 'frame=%di:u64'),('spot','detail::invoke_spot_member(zlink::framework::detail::invoke_spot_member<', 'frame=%di:u64'),('local','public_host_runtime_t::enqueue_local_spot_request(', '')]
with (j/'probes.log').open('w') as log:
 for name,key,args in specs:
  line=next(l for l in lines if key in l and (('[clone .actor]' in l) if name in ('http','spot') else ('[clone' not in l and 'lambda' not in l)))
  addr=line.split()[0]
  subprocess.run(['perf','probe','-x',b,'--add',f'psched5:{name}=0x{addr} {args}'],stdout=log,stderr=log,check=True)
