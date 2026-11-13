import json, os, pathlib, subprocess, time, urllib.request
ROOT=pathlib.Path('/root/worktree/zlink-v21-s033/i3560')
OUT=pathlib.Path('/root/jobs/fx/cx/v21/s033/i3560')
LIB='/root/jobs/fx/cx/v21/s033/core124/prefix-1.24/lib/libzlink.so.1.23.0'
BUILD=ROOT/'framework/bench/grpc/cpp/build'
RAW=OUT/'raw-diag'

def stats(port):
    with urllib.request.urlopen(f'http://127.0.0.1:{port}/bench/stats',timeout=.5) as r: return json.load(r)
def pid(kind,port):
    for p in pathlib.Path('/proc').glob('[0-9]*/cmdline'):
        try:
            c=p.read_bytes().replace(b'\0',b' ').decode()
            if kind=='source' and c.startswith(str(BUILD/'bench_cpp_client')+' ') and f'--stats-port {port}' in c: return int(p.parent.name)
            if kind=='target' and 'bench_cpp_framework_server ' in c and f'--stats-port {port}' in c: return int(p.parent.name)
        except (OSError,UnicodeError): pass
    return None

def snapshot(p):
    out={}
    for t in pathlib.Path(f'/proc/{p}/task').glob('[0-9]*'):
        try: out[t.name]={'comm':(t/'comm').read_text().strip(),'cpu_ns':int((t/'schedstat').read_text().split()[0])}
        except (OSError,IndexError): pass
    return out

src='''BEGIN { @gate=0; @sec=0; }
uprobe:LIB:_ZN5zlink13socket_base_t21process_async_mailboxEv /pid == SRC || pid == TGT/ { if (@gate) { @async[pid,tid]=count(); @in_async[tid]=1; } }
uretprobe:LIB:_ZN5zlink13socket_base_t21process_async_mailboxEv /pid == SRC || pid == TGT/ { delete(@in_async[tid]); }
uprobe:LIB:_ZN5zlink8object_t15process_commandERKNS_9command_tE /pid == SRC || pid == TGT/ { if (@gate) { @command[pid,*(uint32*)(arg1+8)]=count(); if (@in_async[tid]) { @executor_command[pid,*(uint32*)(arg1+8)]=count(); } } }
uprobe:LIB:_ZN5zlink9mailbox_t4sendERKNS_9command_tE /pid == SRC || pid == TGT/ { if (@gate) { @send[pid,tid,*(uint32*)(arg1+8)]=count(); } }
uprobe:LIB:zlink_request /pid == SRC && @gate/ { @request[tid]=count(); }
uretprobe:LIB:zlink_request /pid == SRC && @gate && (int32)retval == 0/ { @request_ok[tid]=count(); }
interval:s:1 { @sec=@sec+1; if (@sec==1) { @gate=1; } if (@sec==3) { @gate=0; } if (@sec==4) { exit(); } }
'''.replace('LIB',LIB).replace('SRC','SRC_PID').replace('TGT','TGT_PID')
RAW.mkdir(exist_ok=True)
env=dict(os.environ,IMPLEMENTATIONS='zlink-framework-cpp',PATTERNS='request-backpressure',PAYLOAD_SIZES='1024',RUNS='1',WARMUP_SECONDS='5',DURATION_SECONDS='8',SKIP_BUILD='1',RUN_STAMP='i3560-core124-diag',OUTPUT_DIR=str(RAW))
log=open(RAW/'runner.log','w')
runner=subprocess.Popen(['bash','framework/bench/grpc/cpp/run_local.sh'],cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT)
try:
    captured=False
    while runner.poll() is None:
        try:
            st=stats(5293); sp=pid('source',5293); tp=pid('target',5295)
            if st.get('phase')!='active' or not sp or not tp or captured: time.sleep(.1); continue
            captured=True
            d=RAW/'capture'; d.mkdir()
            (d/'pids.txt').write_text(f'source={sp}\ntarget={tp}\n')
            before={'source':snapshot(sp),'target':snapshot(tp)}
            (d/'threads-before.json').write_text(json.dumps(before,indent=2)+'\n')
            (d/'stats-before.json').write_text(json.dumps(st,indent=2)+'\n')
            script=d/'commands.bt'; script.write_text(src.replace('SRC_PID',str(sp)).replace('TGT_PID',str(tp)))
            with open(d/'command-counts.txt','w') as f: rc=subprocess.run(['bpftrace','-q',str(script)],stdout=f,stderr=subprocess.STDOUT,timeout=8).returncode
            (d/'bpftrace-exit.txt').write_text(str(rc)+'\n')
            time.sleep(.1)
            aftst=stats(5293); after={'source':snapshot(sp),'target':snapshot(tp)}
            (d/'threads-after.json').write_text(json.dumps(after,indent=2)+'\n')
            (d/'stats-after.json').write_text(json.dumps(aftst,indent=2)+'\n')
        except (OSError,ValueError,KeyError,subprocess.TimeoutExpired,urllib.error.URLError) as e:
            (RAW/'errors.log').open('a').write(f'{time.time()} {type(e).__name__}: {e}\n')
            time.sleep(.1)
    rc=runner.wait()
finally:
    if runner.poll() is None: runner.terminate(); runner.wait(timeout=10)
    log.close()
(OUT/'diagnostic-summary.json').write_text(json.dumps({'runner_exit':rc,'captured':captured,'raw':str(RAW/'capture')},indent=2)+'\n')
if rc: raise SystemExit(rc)
print(json.dumps({'runner_exit':rc,'captured':captured,'summary':str(OUT/'diagnostic-summary.json')}))
