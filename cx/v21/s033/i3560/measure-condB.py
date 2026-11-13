import json, os, pathlib, subprocess, sys, time, urllib.request

ROOT = pathlib.Path('/root/worktree/zlink-v21-s033/i3560')
OUT = pathlib.Path('/root/jobs/fx/cx/v21/s033/i3560')
LIB = '/root/jobs/fx/cx/v21/s033/core124/prefix-1.24/lib/libzlink.so.1.23.0'
STAMP = 'i3560-core124-condB'
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
    pids = f'(pid == {source} || pid == {target})'
    send = '_ZN5zlink10signaler_t4sendEv'
    recv = '_ZN5zlink10signaler_t13recv_failableEv'
    s = ['BEGIN { @gate=0; @sec=0; }']
    s += [f'uprobe:{LIB}:{send} /{pids} && @gate/ {{ @send[pid,tid]=count(); @inside_send[tid]=1; }}',
          f'uretprobe:{LIB}:{send} /{pids}/ {{ delete(@inside_send[tid]); }}',
          f'uprobe:{LIB}:{recv} /{pids} && @gate/ {{ @recv_attempt[pid,tid]=count(); @inside_recv[tid]=1; }}',
          f'uretprobe:{LIB}:{recv} /{pids}/ {{ delete(@inside_recv[tid]); }}',
          f'tracepoint:syscalls:sys_enter_write /({pids}) && @gate && @inside_send[tid]/ {{ @signal_write[pid,tid]=count(); }}',
          f'tracepoint:syscalls:sys_enter_read /({pids}) && @gate && @inside_recv[tid]/ {{ @signal_read[pid,tid]=count(); }}',
          f'uprobe:{LIB}:_ZN5zlink13socket_base_t21process_async_mailboxEv /({pids}) && @gate/ {{ @async[pid,tid]=count(); @in_async[tid]=1; }}',
          f'uretprobe:{LIB}:_ZN5zlink13socket_base_t21process_async_mailboxEv /{pids}/ {{ delete(@in_async[tid]); }}',
          f'uprobe:{LIB}:_ZN5zlink8object_t15process_commandERKNS_9command_tE /{pids} && @gate/ {{ @command[pid,*(uint32*)(arg1+8)]=count(); if(@in_async[tid]) {{ @executor_command[pid,*(uint32*)(arg1+8)]=count(); }} }}',
          f'uprobe:{LIB}:zlink_request /pid == {source} && @gate/ {{ @requests[tid]=count(); }}',
          f'uretprobe:{LIB}:zlink_request /pid == {source} && @gate && (int32)retval == 0/ {{ @request_ok[tid]=count(); }}',
          'interval:s:1 { @sec=@sec+1; if (@sec == 1) { @gate=1; } if (@sec == 3) { @gate=0; } if (@sec == 4) { exit(); } }']
    return '\n'.join(s) + '\n'

run_dir = OUT / 'raw-condB'
run_dir.mkdir(exist_ok=True)
env = dict(os.environ, IMPLEMENTATIONS='zlink-cpp,zlink-framework-cpp',
           PATTERNS='request-backpressure', PAYLOAD_SIZES='1024', RUNS='2',
           WARMUP_SECONDS='5', DURATION_SECONDS='8', SKIP_BUILD='1',
           RUN_STAMP=STAMP, OUTPUT_DIR=str(run_dir))
log = open(run_dir/'runner.log', 'w')
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
                script=cell/'wake.bt'; script.write_text(bpf_script(src,tgt))
                with open(cell/'wake-counts.txt','w') as f:
                    probe=subprocess.run(['bpftrace','-q',str(script)],stdout=f,stderr=subprocess.STDOUT,timeout=8)
                (cell/'bpftrace-exit.txt').write_text(str(probe.returncode)+'\n')
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
(OUT/'capture-summary.json').write_text(json.dumps({'runner_exit':rc,'captures':rows},indent=2)+'\n')
if rc != 0: raise SystemExit(rc)
print(json.dumps({'runner_exit':rc,'captures':len(rows),'summary':str(OUT/'capture-summary.json')}))
