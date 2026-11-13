from pathlib import Path
import subprocess,shutil,json,hashlib
P=Path('/root/jobs/fx/cx/v21/s033/creply');W=Path('/root/worktree/zlink-creply');b=W/'framework/bench/grpc/cpp/build';dest=P/'bin-main';dest.mkdir(exist_ok=True)
for name in ('bench_cpp_client','bench_cpp_framework_server','bench_cpp_zlink_server'):
 assert (b/name).exists(),name
 shutil.copy2(b/name,dest/name)
shutil.copy2(b/'CMakeCache.txt',P/'CMakeCache-main.txt')
(P/'binary-main.json').write_text(json.dumps({f.name:hashlib.sha256(f.read_bytes()).hexdigest() for f in dest.iterdir() if f.is_file()},indent=2))
def run(case,variant,collect='none',rate=40000,warmup=30,active=60,mutex=False):
 cmd=['python3',str(P/'runner/run.py'),case,'--variant',variant,'--rate',str(rate),'--warmup',str(warmup),'--active',str(active),'--collect',collect]
 if mutex:cmd+=['--mutex-stat']
 print('RUN',case,flush=True);subprocess.run(cmd,check=True)
for v in ('main','binding'):run(v+'-smoke',v,warmup=5,active=5)
rates=[json.loads((P/'runs'/f'{v}-smoke/results.json').read_text())['cells'][0]['throughput_per_second'] for v in ('main','binding')]
rate=40000 if min(rates)>39600 else int(min(rates)*.8/1000)*1000
assert rate>0
(P/'rate.json').write_text(json.dumps({'rate':rate,'calibration_rates':rates},indent=2))
run('main-mutex','main',rate=rate,active=30,mutex=True)
for index in (1,2):
 for v in ('main','binding'):run(f'{v}-perf-{index}',v,'perf',rate=rate,active=40)
for v in ('main','binding'):run(v+'-details',v,'details',rate=rate)
for f in ('summarize.py','align_ss.py','table.py'):
 subprocess.run(['python3',str(P/'tools'/f)],check=True)
