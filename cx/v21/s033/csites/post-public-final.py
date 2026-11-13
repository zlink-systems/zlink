import os,pathlib as P,subprocess as S,time,sys
j=P.Path(__file__).parent;started=time.time();gate=j/'test-public-full-final.log'
os.environ['TMPDIR']=str(j/'tmp')
while True:
 if gate.exists() and gate.stat().st_mtime>started:
  text=gate.read_text()
  if 'Total Test time' in text:
   if '100% tests passed, 0 tests failed out of 297' not in text:
    print('gate failed; measurements not started',flush=True);raise SystemExit(1)
   break
 time.sleep(3)
print('public registration gate passed; build final benchmarks',flush=True)
for stage in ('after-final','after-final-perf'):
 assert not (j/stage).exists(),stage+' artifacts already exist'
 with (j/f'build-{stage}.log').open('w') as f:
  S.run(['flock','/root/jobs/fx/cx/cpp-build.2.lock','flock','-s','/root/jobs/fx/cx/measure.lock','/root/jobs/fx/cx/cpp-build-slot.sh',sys.executable,str(j/'build-bench.py'),stage],stdout=f,stderr=f,check=True)
 print('benchmark built',stage,flush=True)
with (j/'after-final-counts.log').open('w') as f:S.run(['flock','-o','/root/jobs/fx/cx/measure.lock',sys.executable,str(j/'measure.py'),'after-final','counts'],stdout=f,stderr=f,check=True)
print('final count done',flush=True)
with (j/'after-final-throughput.log').open('w') as f:S.run(['flock','-o','/root/jobs/fx/cx/measure.lock',sys.executable,str(j/'throughput.py'),'after-final'],stdout=f,stderr=f,check=True)
print('final throughput 12 cells and checks done',flush=True)
