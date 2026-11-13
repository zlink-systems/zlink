import datetime, hashlib, json, os, pathlib, signal, subprocess, sys, time, traceback
J=pathlib.Path('/root/jobs/fx/cx/v21/s033/l3868-s1'); T=J/'tools'
R=pathlib.Path('/root/worktree/zlink-v21-s033/l3868-next'); B=R/'build-l3868-next'
START=time.monotonic(); DEADLINE= datetime.datetime(2026,11,13,23,55,tzinfo=datetime.timezone.utc).timestamp()
plan=json.loads(pathlib.Path(sys.argv[1]).read_text()); stamp=datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
W=J/'raw'/'holds'/f'{stamp}-{os.getpid()}'; W.mkdir(parents=True)
def utc(): return datetime.datetime.now(datetime.timezone.utc).isoformat()
def log(s):
    with (J/'progress.md').open('a') as f: f.write('- '+utc()+' '+s+'\n')
    print(s,flush=True)
def proc(args): return subprocess.check_output(args,cwd=R,text=True).strip()
def snapshot():
    fresh=json.loads((J/'raw/freshness.json').read_text()); assert all(x['fresh'] for x in fresh['artifacts'])
    assert (J/'raw/build-success.json').exists()
    assert proc(['git','branch','--show-current'])=='exp/3868-cpp-ladder-next'
    assert proc(['git','rev-parse','HEAD'])=='62e766f88dc6d1feefdff76e6132185019db0bb6'
    artifacts=[]
    for x in fresh['artifacts']:
        p=pathlib.Path(x['binary'])
        assert p.stat().st_mtime_ns==x['binary_mtime_ns'] and p.stat().st_mtime_ns>x['newest_input_mtime_ns']
        if p.name in ['bench_cpp_client','bench_cpp_zlink_server','bench_cpp_framework_server']:
            sha=hashlib.sha256(p.read_bytes()).hexdigest(); assert sha==x['sha256']; artifacts.append(f'{p}#{sha}')
    return f"HEAD={proc(['git','rev-parse','HEAD'])} dirty={proc(['git','status','--porcelain']) or 'clean'} merge_base={proc(['git','merge-base','HEAD','origin/main'])} artifacts={';'.join(artifacts)}"
def cost(level):
    common='binding receive{B/zlink/bench_zlink_cpp_server.cpp:41 recv(NONE),54 protobuf parse,56 metrics;84 command recv,97 parse,99 metrics} reply{58 fresh envelope,61-65 fresh body copy/protobuf encode/Message,67 two-part submit}'
    if level!='binding' and int(level)<=4:
        common+=' | ladder receive{R/mesh/raw_mesh_node_owner.cpp:530-546 optional permit→recv(NONE)→direct handler;B/ladder/ladder_wire.hpp:253-270 unpack/header parse/payload decode/metrics} reply{B/ladder/ladder_wire.hpp:283-298 fresh body copy/encode/envelope;308/315 two-part submit}'
    elif level=='7': common+=' | L5~L7 receive{B/framework/bench_framework_cpp_server.cpp:91-100 public route→R/mesh/mesh_node_host_service.cpp:2163,2299,3869 owner/mailbox/worker→R/channels/route_handler_invoker.cpp:37 scope} reply{B/framework/bench_framework_cpp_server.cpp:26-29 copy return→R/channels/route_handler_registry.hpp:147 serialize→R/channels/route_packet_dispatcher.cpp:444 response→R/mesh/raw_mesh_node_owner.cpp:2177 submit}'
    return common+'; costs{binding/L0 blocking recv=1,drain=0,new reply body copy=1,encode=1,metrics=same;L1 permit credits load2/add1/sub1;L2 envelope encode/parse and parts allocation;L3 pack byte writes/retained slices/span vector,no payload copy on target/completion;L4 typed JSON/base64;L5~L7 production owner/worker/scope/registry allocation/atomic;Core/native counts unmeasured}; call-shape.md'
def validate(result,sched):
    d=json.loads(result.read_text()); c=d['cells'][0]; s=sched.read_text()
    assert not c.get('contaminated') and not c.get('drain_bound_hit') and not c.get('settle_bound_hit'), 'contaminated/drain'
    assert all(c.get(k,0)==0 for k in ['errors','server_errors','abandoned']), 'source error'
    assert c['target_stats']['errors']==0, 'target error'
    assert c['submitted']==c['completed']==c['target_stats']['received'], 'counts mismatch'
    assert c['throughput_per_second']>0
    assert all(f'cpu{cpu}.state{state}.disable_at_run_start=1' in s for cpu in [0,8] for state in [2,3])
    assert 'scheduling_invalid=' not in s
    for label,expect in [('source_start','0-5'),('target_start','8-13'),('mid_active_source','0-5'),('mid_active_target','8-13')]:
        tids=[x for x in s.splitlines() if x.startswith(label+'_tid=')]
        affin=[x for x in s.splitlines() if x.startswith(label+'_thread_affinity ')]
        assert tids and len(tids)==len(affin), f'missing thread evidence {label}'
        assert all(x.endswith('cpus='+expect) for x in affin), f'wrong affinity {label}'
    policies=[x for x in s.splitlines() if 'current scheduling policy:' in x]
    assert policies and all(x.endswith('SCHED_OTHER') for x in policies), 'wrong scheduler'
    return c
child=None
def terminate(*_):
    if child and child.poll() is None:
        # Runner's EXIT trap cleans the two independent setsid bench groups.
        os.killpg(child.pid, signal.SIGTERM)
        try: child.wait(timeout=8)
        except subprocess.TimeoutExpired: os.killpg(child.pid, signal.SIGKILL)
    raise SystemExit(124)
signal.signal(signal.SIGTERM,terminate); signal.signal(signal.SIGINT,terminate)
def run(level,pattern,size,kind):
    global child
    for attempt in range(1,4):
        if time.monotonic()-START>1380 or time.time()>DEADLINE-130: raise RuntimeError('hold/time cap: stop before new run')
        out=W/f'{level}-{pattern}-{size}'/f'{kind}-attempt{attempt}'; out.mkdir(parents=True)
        impl='zlink-cpp' if level=='binding' else 'zlink-framework-cpp'
        evidence=snapshot(); (out/'build-meta.txt').write_text(evidence+'\n')
        log(f'BEFORE {level} {pattern}/{size} {kind} attempt={attempt}; built{{{evidence}}}; parity{{{cost(level)}}}; raw={out}')
        env=os.environ.copy(); env.update(IMPL=impl,ZLINK_CPP_LADDER_LEVEL='0' if level=='binding' else level)
        begin=time.monotonic()
        with (out/'launcher.log').open('x') as f:
            child=subprocess.Popen(['bash',str(T/'run-one-binding.sh'),pattern,str(size),str(out),out.name+'-'+stamp,'3'],env=env,stdout=f,stderr=subprocess.STDOUT,start_new_session=True)
            try: rc=child.wait(timeout=120)
            except subprocess.TimeoutExpired: terminate()
        result=out/'run1'/f'{impl}-{pattern}-{size}'/'results.json'; sched=result.with_name('scheduling.txt')
        try:
            assert rc==0, f'runner rc={rc}'
            c=validate(result,sched)
        except Exception as e:
            log(f'INVALID {level} {pattern}/{size} {kind} {e}; raw={out}')
            (out/'validation.json').write_text(json.dumps({'valid':False,'reason':str(e)}))
            continue
        record={'utc':utc(),'hold':str(W),'level':level,'pattern':pattern,'size':size,'kind':kind,'rate':c['throughput_per_second'],'raw':str(result),'scheduling':str(sched),'elapsed':time.monotonic()-begin}
        (out/'validation.json').write_text(json.dumps({'valid':True,**record},indent=2)+'\n')
        with (J/'raw/runs.jsonl').open('a') as f: f.write(json.dumps(record)+'\n')
        log(f'RESULT {level} {pattern}/{size} {kind} msg/s={record["rate"]} raw={result} scheduling={sched}')
        return
    raise RuntimeError('three invalid attempts; leave raw and stop hold')

def environment(path):
    values={'utc':utc(),'governor':{},'cstates':{},'plan':plan,'guard':'v8','pid':os.getpid()}
    for n in range(16):
        values['governor'][n]=pathlib.Path(f'/sys/devices/system/cpu/cpu{n}/cpufreq/scaling_governor').read_text().strip()
        for st in [2,3]: values['cstates'][f'{n}/{st}']=pathlib.Path(f'/sys/devices/system/cpu/cpu{n}/cpuidle/state{st}/disable').read_text().strip()
    path.write_text(json.dumps(values,indent=2)+'\n')

try:
    log(f'LOCK acquired hold={W} plan={sys.argv[1]}')
    for n in range(16): pathlib.Path(f'/sys/devices/system/cpu/cpu{n}/cpufreq/scaling_governor').write_text('performance')
    for _ in range(240):
        if all(pathlib.Path(f'/sys/devices/system/cpu/cpu{n}/cpuidle/state{s}/disable').read_text().strip()=='1' for n in range(16) for s in [2,3]): break
        time.sleep(.25)
    else: raise RuntimeError('guard C2/C3 not ready')
    environment(W/'before.json')
    for cell in plan:
        level,pattern,size,count=cell; level=str(level)
        run(level,pattern,size,'warm')
        for k in range(count): run(level,pattern,size,f'counted{k+1}')
    environment(W/'after.json')
finally:
    for n in range(16): pathlib.Path(f'/sys/devices/system/cpu/cpu{n}/cpufreq/scaling_governor').write_text('powersave')
    (W/'duration.json').write_text(json.dumps({'seconds':time.monotonic()-START,'utc':utc()}))
    log(f'LOCK release hold={W} seconds={time.monotonic()-START:.2f}')
