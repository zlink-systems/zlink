import json, os, pathlib, subprocess, sys, time, urllib.request

ROOT = pathlib.Path('/root/worktree/zlink-v21-s033/i3560b')
OUT = pathlib.Path('/root/jobs/fx/cx/v21/s033/i3560b')
LIB = '/root/jobs/fx/cx/v21/s033/core124/prefix-1.24/lib/libzlink.so.1.23.0'
MODE=os.environ.get('PROBE_MODE','stacks')
LABEL=os.environ.get('RUN_LABEL','')
STAMP = 'i3560b-'+MODE+LABEL
BUILD = ROOT / 'framework/bench/grpc/cpp/build'

def stats(port):
    with urllib.request.urlopen(f'http://127.0.0.1:{port}/bench/stats', timeout=0.5) as r:
        return json.load(r)

def find_pid(kind, port):
    for p in pathlib.Path('/proc').glob('[0-9]*/cmdline'):
        try:
            cmd = p.read_bytes().replace(b'\0', b' ').decode()
            if kind == 'source' and cmd.startswith(str(BUILD / 'bench_cpp_client') + ' '):
                if f'--stats-port {port}' in cmd: return int(p.parent.name)
            if kind == 'target' and ('bench_cpp_zlink_server ' in cmd or 'bench_cpp_framework_server ' in cmd):
                if f'--stats-port {port}' in cmd: return int(p.parent.name)
        except (OSError, UnicodeError):
            pass
    return None

def snapshot(pid):
    found = {}
    for t in pathlib.Path(f'/proc/{pid}/task').glob('[0-9]*'):
        try:
            tid = t.name
            found[tid] = {'comm': (t/'comm').read_text().strip(),
                          'cpu_ns': int((t/'schedstat').read_text().split()[0])}
        except (OSError, IndexError):
            pass
    return found

def bpf_script(source, target):
    pids=f'(pid == {source} || pid == {target})'
    s=['BEGIN { @gate=0; @sec=0; }']
    def probe(kind, sym, body, gate=True):
        s.append(f'{kind}:{LIB}:{sym} /{pids}'+(' && @gate' if gate else '')+f'/ {{ {body} }}')
    probe('uprobe','zlink_request','@requests[pid,tid,arg0,arg4]=count();')
    probe('uretprobe','zlink_request','if ((int32)retval == 0) { @request_ok[pid,tid]=count(); }')
    if MODE == 'light':
        probe('uprobe','_ZN5zlink10signaler_t4sendEv','@send[pid,tid,usym(*(uint64*)reg("sp")),arg0]=count();')
    elif MODE == 'stacks':
        probe('uprobe','_ZN5zlink9mailbox_t4sendERKNS_9command_tE','@mail[tid]=*(uint32*)(arg1+8)+1; @mailbox[tid]=arg0; @mailcommand[pid,tid,arg0,*(uint32*)(arg1+8)]=count(); @mail_origin[pid,tid,*(uint32*)(arg1+8),usym(*(uint64*)reg("sp"))]=count();')
        probe('uretprobe','_ZN5zlink9mailbox_t4sendERKNS_9command_tE','delete(@mail[tid]); delete(@mailbox[tid]);',False)
        probe('uprobe','_ZN5zlink13socket_base_t25notify_request_completionEv','@notify[tid]=arg0; @notify_socket[pid,tid,arg0]=count(); @notify_origin[pid,tid,usym(*(uint64*)reg("sp"))]=count();')
        probe('uretprobe','_ZN5zlink13socket_base_t25notify_request_completionEv','delete(@notify[tid]);',False)
        probe('uprobe','_ZN5zlink10signaler_t4sendEv','@send[pid,tid,@mail[tid],@notify[tid],usym(*(uint64*)reg("sp")),arg0]=count(); @stacks[pid,tid,ustack(perf,16)]=count(); @sendptr[tid]=arg0;')
        probe('uretprobe','_ZN5zlink10signaler_t4sendEv','delete(@sendptr[tid]);',False)
        probe('uprobe','_ZN5zlink10signaler_t13recv_failableEv','@recv[pid,tid,usym(*(uint64*)reg("sp")),arg0]=count(); @recvptr[tid]=arg0;')
        probe('uretprobe','_ZN5zlink10signaler_t13recv_failableEv','delete(@recvptr[tid]);',False)
        s.append(f'tracepoint:syscalls:sys_enter_write /{pids} && @gate && @sendptr[tid]/ {{ @writes[pid,tid,@sendptr[tid]]=count(); }}')
        s.append(f'tracepoint:syscalls:sys_enter_read /{pids} && @gate && @recvptr[tid]/ {{ @reads[pid,tid,@recvptr[tid]]=count(); }}')
    elif MODE == 'syscalls':
        for name in ('read','write','recvfrom','sendto','recvmsg','sendmsg'):
            s.append(f'tracepoint:syscalls:sys_enter_{name} /{pids} && @gate/ {{ @fd[tid]=args.fd; @sys_enter[pid,tid,probe,args.fd]=count(); }}')
            s.append(f'tracepoint:syscalls:sys_exit_{name} /{pids} && @gate/ {{ @sys_exit[pid,tid,probe,@fd[tid],args.ret < 0 ? args.ret : 0]=count(); if(args.ret > 0) {{ @sys_bytes[pid,tid,probe,@fd[tid]]=sum(args.ret); }} delete(@fd[tid]); }}')
    elif MODE == 'api':
        probe('uprobe','zlink_poller_modify','@modify[pid,tid,arg0,arg1,arg2]=count();')
        probe('uprobe','zlink_completion_recv','@comp_handle[tid]=arg0; @compptr[tid]=arg1; @comp_flags[tid]=arg2; @comp_enter[pid,tid,arg0,arg2]=count();')
        probe('uretprobe','zlink_completion_recv','@comp_result[pid,tid,@comp_handle[tid],@comp_flags[tid],(int32)retval]=count(); if((int32)retval == 0) { @comp_record[pid,tid,@comp_handle[tid],*(uint32*)(@compptr[tid]+(uint64)4)]=count(); } delete(@compptr[tid]); delete(@comp_handle[tid]); delete(@comp_flags[tid]);')
        probe('uprobe','zlink_poller_wait','@poller[tid]=arg0; @waitbuf[tid]=arg1; @wait_enter[pid,tid,arg0,(int64)arg3]=count();')
        probe('uretprobe','zlink_poller_wait','@wait_result[pid,tid,@poller[tid],(int32)retval]=count(); $i=0; unroll(4) { if($i < (int32)retval) { @event_mask[pid,tid,@poller[tid],*(uint16*)(@waitbuf[tid]+(uint64)48*(uint64)$i+(uint64)40)]=count(); } $i=$i+1; } delete(@poller[tid]); delete(@waitbuf[tid]);')
        probe('uprobe','zlink_router_recv','@router_handle[tid]=arg0; @router_enter[pid,tid,arg0,*(uint64*)(reg("sp")+8)]=count();')
        probe('uretprobe','zlink_router_recv','@router_result[pid,tid,@router_handle[tid],(int32)retval]=count(); delete(@router_handle[tid]);')
        probe('uprobe','zlink_reply','@reply[pid,tid,arg0]=count();')
        probe('uprobe','_ZN5zlink8object_t15process_commandERKNS_9command_tE','@command[pid,tid,*(uint32*)(arg1+8)]=count();')
    s.append('interval:s:1 { @sec=@sec+1; if (@sec==1) { @gate=1; } if (@sec==3) { @gate=0; } if (@sec==4) { exit(); } }')
    return '\n'.join(s)+'\n'

run_dir = OUT / ('raw-'+MODE+LABEL)
run_dir.mkdir(exist_ok=True)
env = dict(os.environ, IMPLEMENTATIONS=os.environ.get('IMPLEMENTATIONS','zlink-cpp,zlink-framework-cpp'),
           PATTERNS='request-backpressure', PAYLOAD_SIZES='1024', RUNS=os.environ.get('MEASURE_RUNS','1'),
           WARMUP_SECONDS='5', DURATION_SECONDS='8', SKIP_BUILD='1',
           RUN_STAMP=STAMP, OUTPUT_DIR=str(run_dir), SOURCE_CPUS='0-7', TARGET_CPUS='8-15')
log = open(run_dir/'runner.log', 'w')
registration=None
if MODE=='api':
    reg=run_dir/'registration.bt'
    reg.write_text(f'uprobe:{LIB}:zlink_socket {{ @type[tid]=arg1; }}\nuretprobe:{LIB}:zlink_socket {{ printf("socket pid=%d tid=%d type=%d socket=%lx\\n",pid,tid,@type[tid],retval); delete(@type[tid]); }}\nuretprobe:{LIB}:zlink_poller_new {{ printf("poller pid=%d tid=%d poller=%lx\\n",pid,tid,retval); }}\nuprobe:{LIB}:zlink_poller_add {{ printf("add pid=%d tid=%d poller=%lx socket=%lx mask=%d\\n",pid,tid,arg0,arg1,arg3); }}\nuprobe:{LIB}:zlink_connect {{ printf("connect pid=%d tid=%d socket=%lx endpoint=%s\\n",pid,tid,arg0,str(arg1)); }}\nuprobe:{LIB}:zlink_bind {{ printf("bind pid=%d tid=%d socket=%lx endpoint=%s\\n",pid,tid,arg0,str(arg1)); }}\n')
    with reg.open('a') as f:
        f.write(f'uprobe:{LIB}:zlink_poller_remove {{ printf("remove pid=%d tid=%d poller=%lx socket=%lx\\n",pid,tid,arg0,arg1); }}\nuprobe:{LIB}:zlink_poller_destroy {{ printf("destroy pid=%d tid=%d poller=%lx\\n",pid,tid,*(uint64*)arg0); }}\n')
    reglog=open(run_dir/'registration.txt','w')
    registration=subprocess.Popen(['bpftrace','-q',str(reg)],stdout=reglog,stderr=subprocess.STDOUT)
    time.sleep(2)
runner = subprocess.Popen(['bash', 'framework/bench/grpc/cpp/run_local.sh'], cwd=ROOT,
                          env=env, stdout=log, stderr=subprocess.STDOUT)
rows=[]
seen=set()
pinned=set()
try:
    while runner.poll() is None:
        for impl, sport, tport in [('zlink-cpp',5286,5289),
                                   ('zlink-framework-cpp',5293,5295)]:
            try:
                # Condition B: pin source and target as soon as each process appears,
                # before the runner starts its measured active window.
                for role, port, cpus in [('source',sport,'0-7'),('target',tport,'8-15')]:
                    process=find_pid(role,port)
                    if process and process not in pinned:
                        q=subprocess.run(['taskset','-cp',cpus,str(process)],capture_output=True,text=True,check=True)
                        with (run_dir/'pinning.log').open('a') as pf: pf.write(q.stdout.strip()+'\n')
                        pinned.add(process)
                ss = stats(sport)
                src = find_pid('source', sport)
                tgt = find_pid('target', tport)
                if ss.get('phase') != 'active' or not src or not tgt:
                    continue
                key=(impl, ss.get('run_id'), src)
                if key in seen: continue
                seen.add(key)
                cell=run_dir/f'{impl}-run-{len(seen):02d}'
                cell.mkdir()
                (cell/'pids.txt').write_text(f'source={src}\ntarget={tgt}\n')
                (cell/'core-maps-source.txt').write_text('\n'.join(x for x in pathlib.Path(f'/proc/{src}/maps').read_text().splitlines() if 'libzlink' in x)+'\n')
                (cell/'core-maps-target.txt').write_text('\n'.join(x for x in pathlib.Path(f'/proc/{tgt}/maps').read_text().splitlines() if 'libzlink' in x)+'\n')
                before_stats=stats(sport)
                before={'source':snapshot(src),'target':snapshot(tgt)}
                (cell/'stats-before.json').write_text(json.dumps(before_stats,indent=2)+'\n')
                (cell/'threads-before.json').write_text(json.dumps(before,indent=2)+'\n')
                if MODE in ('cpu','wakeperf'):
                    perf_log=open(cell/'perf-record.log','w')
                    perf_args=['-e','cpu-clock','-F','499'] if MODE=='cpu' else ['-e','i3560b:wake','-c','101']
                    perf=subprocess.Popen(['perf','record']+perf_args+['--call-graph','dwarf,16384','-p',f'{src},{tgt}','-o',str(cell/'perf.data'),'--','sleep','4'],stdout=perf_log,stderr=subprocess.STDOUT)
                script=cell/'wake.bt'; script.write_text(bpf_script(src,tgt))
                for role, pid_value in [('source',src),('target',tgt)]:
                    fd_links={}
                    for fd in pathlib.Path(f'/proc/{pid_value}/fd').glob('*'):
                        try: fd_links[fd.name]=os.readlink(fd)
                        except OSError: pass
                    (cell/f'fds-{role}.json').write_text(json.dumps(fd_links,indent=2)+'\n')
                with open(cell/'connections.txt','w') as f:
                    subprocess.run(['ss','-tnp'],stdout=f,stderr=subprocess.STDOUT)
                if MODE in ('cpu','wakeperf'):
                    time.sleep(4)
                    probe=subprocess.CompletedProcess([],0)
                else:
                    with open(cell/'wake-counts.txt','w') as f:
                        probe=subprocess.run(['bpftrace','-q',str(script)],stdout=f,stderr=subprocess.STDOUT,timeout=8)
                (cell/'bpftrace-exit.txt').write_text(str(probe.returncode)+'\n')
                if MODE in ('cpu','wakeperf'):
                    perf.wait(timeout=8); perf_log.close()
                    if perf.returncode: raise RuntimeError("perf failed: "+str(cell))
                time.sleep(0.15)
                after_stats=stats(sport)
                after={'source':snapshot(src),'target':snapshot(tgt)}
                (cell/'stats-after.json').write_text(json.dumps(after_stats,indent=2)+'\n')
                (cell/'threads-after.json').write_text(json.dumps(after,indent=2)+'\n')
                for role in ('source','target'):
                    union=set(before[role])|set(after[role])
                    io={tid:{'comm':(after[role].get(tid) or before[role][tid])['comm'],
                             'cpu_us':max(0,(after[role].get(tid) or {}).get('cpu_ns',0)-before[role].get(tid,{}).get('cpu_ns',0))/1000}
                         for tid in union
                         if '/IO/' in (after[role].get(tid) or before[role][tid])['comm']}
                    (cell/f'{role}-io-cpu.json').write_text(json.dumps(io,indent=2)+'\n')
                rows.append({'implementation':impl,'source_pid':src,'target_pid':tgt,
                             'source_completed_delta':after_stats.get('completed',0)-before_stats.get('completed',0),
                             'raw':str(cell)})
            except (OSError, ValueError, KeyError, subprocess.TimeoutExpired, urllib.error.URLError) as e:
                (run_dir/'capture-errors.log').open('a').write(f'{time.time()} {impl}: {type(e).__name__}: {e}\n')
        time.sleep(0.01)
    rc=runner.wait()
finally:
    if runner.poll() is None:
        runner.terminate(); runner.wait(timeout=10)
    log.close()
    if registration is not None:
        registration.send_signal(2); registration.wait(timeout=5); reglog.close()
(OUT/('capture-'+MODE+LABEL+'-summary.json')).write_text(json.dumps({'runner_exit':rc,'captures':rows},indent=2)+'\n')
if rc != 0: raise SystemExit(rc)
print(json.dumps({'runner_exit':rc,'captures':len(rows),'summary':str(OUT/('capture-'+MODE+LABEL+'-summary.json'))}),flush=True)

# Keep the authorized diagnostic runs in the same exclusive measurement window.
if len(rows) != len(env['IMPLEMENTATIONS'].split(',')) * int(env['RUNS']):
    raise SystemExit('incomplete capture count')
for row in rows:
    if pathlib.Path(row['raw'],'bpftrace-exit.txt').read_text().strip() != '0':
        raise SystemExit('probe failed: '+row['raw'])
if MODE == 'stacks' and not LABEL and os.environ.get('I3560B_CHILD') != '1':
    for next_mode in ('light','api','cpu','syscalls'):
        child_env=dict(os.environ,PROBE_MODE=next_mode,I3560B_CHILD='1')
        subprocess.run([sys.executable,__file__],env=child_env,check=True)
    subprocess.run(['bash',str(OUT/'run-wakeperf.sh')],env=dict(os.environ,I3560B_CHILD='1'),check=True)
    # Rate control isolates request arrival/coalescing from socket/poller configuration.
    framework_result=OUT/'raw-cpu/run1/zlink-framework-cpp-request-backpressure-1024/results.json'
    rate=json.loads(framework_result.read_text())['cells'][0]['throughput_per_second']
    (OUT/'matched-rate.json').write_text(json.dumps({'framework_cpu_round_rps':rate,'binding_target_rate':round(rate)},indent=2)+'\n')
    for next_mode in ('light','cpu'):
        child_env=dict(os.environ,PROBE_MODE=next_mode,I3560B_CHILD='1',RUN_LABEL='-matched',IMPLEMENTATIONS='zlink-cpp',TARGET_RATE=str(round(rate)))
        subprocess.run([sys.executable,__file__],env=child_env,check=True)
