#!/usr/bin/env python3
"""Foreground bench supervisor; no shell background jobs and SIGTERM only."""
from pathlib import Path
import argparse, datetime, json, os, signal, subprocess, time
def terminate_supervisor(signum, frame):
 raise KeyboardInterrupt(f'signal {signum}')
signal.signal(signal.SIGTERM, terminate_supervisor)
P=Path('/root/jobs/fx/cx/v21/s033/p0')
R=Path('/root/worktree/zlink-v21-s033/p0')
G=R/'framework/bench/grpc'
parser=argparse.ArgumentParser();parser.add_argument('mode',choices=['on','off']);parser.add_argument('--runs',type=int,default=3);parser.add_argument('--patterns',default='send-saturation,request-backpressure,request-serial');parser.add_argument('--sizes',default='1024,4096');parser.add_argument('--stamp',default='main');args=parser.parse_args()
assert Path('/root/jobs/fx/cx/v21/s033/core124b/verdict.md').is_file()
B=P/f'build-{args.mode}'
env=os.environ.copy();env.update(DURATION_SECONDS='5',WARMUP_SECONDS='30',RUNS=str(args.runs),SKIP_BUILD='1')
def helper(code, *values, log):
 cmd=['bash','-c', 'set -euo pipefail\nsource "$1/runner_common.sh"\nshift\n'+code, 'p0',str(G),*map(str,values)]
 subprocess.run(cmd,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
def available():
 return int(next(x.split()[1] for x in Path('/proc/meminfo').read_text().splitlines() if x.startswith('MemAvailable:')))*1024
root=P/'raw'/args.mode/args.stamp;root.mkdir(parents=True,exist_ok=True)
with (root/'supervisor.log').open('w',buffering=1) as log:
 for run in range(1,args.runs+1):
  for pattern in args.patterns.split(','):
   for size in map(int,args.sizes.split(',')):
    d=root/f'run{run}'/f'zlink-framework-cpp-{pattern}-{size}';d.mkdir(parents=True,exist_ok=False)
    log.write(f'{datetime.datetime.now().isoformat()} START {d}\n')
    mem=available();assert mem >= 12*1024**3, f'MemAvailable only {mem}'
    (d/'environment.json').write_text(json.dumps({'time':datetime.datetime.now().isoformat(),'MemAvailable':mem,'source_cpus':'0-7','target_cpus':'8-15','uptime':subprocess.check_output(['uptime'],text=True)},indent=2)+'\n')
    helper('check_ports_free 5280 5299',log=log)
    children=[];files=[]
    try:
     for role,cpus,binary,opts,port,ready in [
       ('target','8-15','bench_cpp_framework_server',['--endpoint','tcp://127.0.0.1:5294','--stats-port','5295'],5295,0),
       ('source','0-7','bench_cpp_client',['--implementation','zlink-framework-cpp','--pattern',pattern,'--payload-size',str(size),'--trigger-port','5292','--stats-port','5293','--target-stats-port','5295','--endpoint','tcp://127.0.0.1:5294','--output-file',str(d/'results.json'),'--warmup-seconds','30','--warmup-segments','10','--request-timeout-ms','30000','--drain-bound-ms','30000'],5293,1)]:
      f=(d/f'{role}.log').open('w');files.append(f)
      ce=env.copy();ce['ZLINK_PATH_COUNTER_FILE']=str(d/f'{role}-counters')
      child=subprocess.Popen(['taskset','-c',cpus,str(B/binary),*opts],stdout=f,stderr=subprocess.STDOUT,env=ce,start_new_session=True)
      children.append(child)
      helper('wait_for_stats "$1" "$2"',f'http://127.0.0.1:{port}',ready,log=log)
     helper('bench_run_cell "$@"','http://127.0.0.1:5292','http://127.0.0.1:5293','http://127.0.0.1:5295',f'p0-{args.mode}-{args.stamp}-{run}',f'{pattern}-{size}','zlink-framework-cpp',pattern,size,30000,d/'results.json',d/'target-stats.json','http://127.0.0.1:5295',log=log)
    finally:
     for child in children:
      if child.poll() is None: os.killpg(child.pid,signal.SIGTERM)
     for child in children:
      try: rc=child.wait(timeout=45)
      except subprocess.TimeoutExpired:
       log.write(f'TERM timeout pid={child.pid}; no forced kill\n');raise
      log.write(f'EXIT pid={child.pid} status={rc}\n')
     for f in files:f.close()
    helper('wait_for_ports_free 5280 5299',log=log)
    log.write(f'{datetime.datetime.now().isoformat()} END {d}\n')
