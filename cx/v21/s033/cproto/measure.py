import json, os, pathlib, signal, subprocess, sys, time, urllib.request
R=pathlib.Path('/root/worktree/zlink-cproto')
J=pathlib.Path('/root/jobs/fx/cx/v21/s033/cproto')
side=sys.argv[1]; binary=pathlib.Path(sys.argv[2]) if len(sys.argv)>2 else R/'framework/bench/grpc/cpp/build'
def memory():
    available=int(next(l.split()[1] for l in pathlib.Path('/proc/meminfo').read_text().splitlines() if l.startswith('MemAvailable:')))
    with (J/'memory-samples.jsonl').open('a') as h: h.write(json.dumps({'time':time.time(),'side':side,'MemAvailable_kib':available})+'\n')
    if available < 12*1024*1024: raise RuntimeError('MemAvailable < 12 GiB')
def stats(port, ready=False):
    deadline=time.monotonic()+30
    while time.monotonic()<deadline:
        try:
            with urllib.request.urlopen(f'http://127.0.0.1:{port}/bench/stats',timeout=2) as h: data=json.load(h)
            if not ready or data.get('ready') is True: return
        except (OSError, ValueError): pass
        time.sleep(.1)
    raise RuntimeError(f'not ready {port}')
for run in range(1,4):
  for pattern in os.environ.get('PATTERNS','request-backpressure,request-serial').split(','):
    memory()
    subprocess.run(['bash','-c','source "$1"; check_ports_free 5280 5299','preflight',str(R/'framework/bench/grpc/runner_common.sh')],check=True)
    cell=f'zlink-framework-cpp-{pattern}-1024'; out=J/'log'/side/f'run{run}'/cell; out.mkdir(parents=True,exist_ok=True)
    processes=[]
    try:
      with open(out/'target.log','w') as log:
        p=subprocess.Popen(['taskset','-c','8-15',str(binary/'bench_cpp_framework_server'),'--endpoint','tcp://127.0.0.1:5294','--stats-port','5295'],stdout=log,stderr=subprocess.STDOUT,start_new_session=True); processes.append(p)
      stats(5295)
      args=['taskset','-c','0-7',str(binary/'bench_cpp_client'),'--implementation','zlink-framework-cpp','--pattern',pattern,'--payload-size','1024','--trigger-port','5292','--stats-port','5293','--target-stats-port','5295','--endpoint','tcp://127.0.0.1:5294','--output-file',str(out/'results.json'),'--warmup-seconds','30','--warmup-segments','10','--request-timeout-ms','30000','--drain-bound-ms','30000']
      with open(out/'source.log','w') as log:
        p=subprocess.Popen(args,stdout=log,stderr=subprocess.STDOUT,start_new_session=True); processes.append(p)
      stats(5293,True)
      command=['bash','-c','set -euo pipefail; source "$1"; DURATION_SECONDS=5; bench_run_cell http://127.0.0.1:5292 http://127.0.0.1:5293 http://127.0.0.1:5295 "$2" "$3" zlink-framework-cpp "$4" 1024 30000 "$5/results.json" "$5/target-stats.json" http://127.0.0.1:5295','measure',str(R/'framework/bench/grpc/runner_common.sh'),f'cproto-{side}-run{run}',cell,pattern,str(out)]
      with open(out/'runner.log','w') as log:
        subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True)
      memory()
      data=json.loads((out/'results.json').read_text())['cells'][0]
      print(side,run,pattern,data.get('throughput_per_second'),data.get('submit_thread_cores'),flush=True)
    finally:
      for p in processes:
        if p.poll() is None: os.killpg(p.pid,signal.SIGTERM)
      for p in processes:
        try: p.wait(timeout=30)
        except subprocess.TimeoutExpired: raise RuntimeError(f'child did not exit after SIGTERM: {p.pid}')
      time.sleep(.3)
