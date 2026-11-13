import json, os, pathlib, signal, subprocess, sys, time, urllib.request
repo=pathlib.Path('/root/worktree/zlink-dseg')
raw=pathlib.Path('/root/jobs/fx/cx/v21/s033/dseg2')
out=raw/'out'
out.mkdir(parents=True,exist_ok=True)
env=os.environ.copy()
env.update(ZLINK_DSEG_TRACE=str(raw/'trace'), IMPLEMENTATIONS='zlink-framework-dotnet', PATTERNS='request-serial', PAYLOAD_SIZES='1024', RUNS='1', WARMUP_SECONDS='20', DURATION_SECONDS='15', SKIP_BUILD='1', RUN_STAMP='dseg2', OUTPUT_DIR=str(out))
cmd=['flock','-o','/root/jobs/fx/cx/measure.lock','env']+[f'{k}={env[k]}' for k in ('ZLINK_DSEG_TRACE','IMPLEMENTATIONS','PATTERNS','PAYLOAD_SIZES','RUNS','WARMUP_SECONDS','DURATION_SECONDS','SKIP_BUILD','RUN_STAMP','OUTPUT_DIR')]+['bash','dotnet/run_local.sh']
def stats(port):
    try:
        with urllib.request.urlopen(f'http://127.0.0.1:{port}/bench/stats',timeout=.5) as f:return json.load(f)
    except Exception:return {}
def pid(port):
    return int(subprocess.check_output(['lsof','-nP','-t',f'-iTCP:{port}','-sTCP:LISTEN'],text=True).splitlines()[-1])
with (raw/'runner.log').open('w') as log:
    proc=subprocess.Popen(cmd,cwd=repo/'framework/bench/grpc',env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
    deadline=time.monotonic()+3600
    while time.monotonic()<deadline:
        s=stats(5213)
        if s.get('phase')=='active':break
        if proc.poll() is not None:raise RuntimeError(f'runner exited before active: {proc.returncode}')
        time.sleep(.02)
    else:raise RuntimeError('source never active')
    time.sleep(.3)
    pids={'source':pid(5213),'target':pid(5215)}
    start=time.time()
    perf=subprocess.run(['perf','record','-q','-k','mono','-o',str(raw/'perf.data'),'-e','raw_syscalls:sys_enter,raw_syscalls:sys_exit,sched:sched_switch,sched:sched_wakeup','-p',f"{pids['source']},{pids['target']}",'--','sleep','3'],stdout=open(raw/'perf.log','w'),stderr=subprocess.STDOUT)
    end=time.time()
    proc.wait()
    (raw/'capture.json').write_text(json.dumps({'pids':pids,'active_start_wall':start,'active_end_wall':end,'perf_exit':perf.returncode,'runner_exit':proc.returncode,'source_stats_after':stats(5213),'target_stats_after':stats(5215)},indent=2)+'\n')
    subprocess.run(['perf','script','-i',str(raw/'perf.data'),'--ns','-F','comm,pid,tid,cpu,time,event,trace'],stdout=open(raw/'perf.script','w'),stderr=subprocess.STDOUT)
    print(f'DONE pids={pids} perf={perf.returncode} runner={proc.returncode}')
