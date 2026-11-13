import pathlib as P,subprocess as S,time,sys
j=P.Path(__file__).parent;started=time.time();gate=j/'test-full-final.log'
while True:
 if gate.exists() and gate.stat().st_mtime>started:
  text=gate.read_text()
  if 'Total Test time' in text:
   if '100% tests passed, 0 tests failed out of 297' not in text:
    print('gate failed; measurements not started',flush=True);raise SystemExit(1)
   break
 time.sleep(3)
print('pre-parser unit/contract gate passed; finalize static parser build',flush=True)
import shutil
cpp=P.Path('/root/worktree/zlink-3867h/framework/languages/cpp')
shutil.copy2(gate,j/'test-pre-parser-gate.log')
with (j/'build-parser-static-final.log').open('w') as f:
 S.run(['flock','/root/jobs/fx/cx/cpp-build.2.lock','flock','-s','/root/jobs/fx/cx/measure.lock','/root/jobs/fx/cx/cpp-build-slot.sh','cmake','--build','--preset','linux-ninja-release','--parallel','3','--','-k','0'],cwd=cpp,stdout=f,stderr=f,check=True)
with gate.open('w') as f:
 S.run(['flock','-s','/root/jobs/fx/cx/measure.lock','ctest','--preset','linux-ninja-release','-L','^(unit|contract)$'],cwd=cpp,stdout=f,stderr=f,check=True)
shutil.copy2(cpp/'build/linux-ninja-release/Testing/Temporary/LastTest.log',j/'test-full-detail.log')
assert '100% tests passed, 0 tests failed out of 297' in gate.read_text()
print('final unit/contract gate passed; build benchmarks',flush=True)
for stage in ('after','after-perf'):
 assert not (j/stage).exists(),stage+' artifacts already exist'
 with (j/f'build-{stage}.log').open('w') as f:
  S.run(['flock','/root/jobs/fx/cx/cpp-build.2.lock','flock','-s','/root/jobs/fx/cx/measure.lock','/root/jobs/fx/cx/cpp-build-slot.sh',sys.executable,str(j/'build-bench.py'),stage],stdout=f,stderr=f,check=True)
 print('benchmark built',stage,flush=True)
with (j/'after-counts.log').open('w') as f:S.run(['flock','-o','/root/jobs/fx/cx/measure.lock',sys.executable,str(j/'measure.py'),'after','counts'],stdout=f,stderr=f,check=True)
print('after count done',flush=True)
with (j/'after-throughput.log').open('w') as f:S.run(['flock','-o','/root/jobs/fx/cx/measure.lock',sys.executable,str(j/'throughput.py'),'after'],stdout=f,stderr=f,check=True)
print('after throughput 12 cells done',flush=True)
