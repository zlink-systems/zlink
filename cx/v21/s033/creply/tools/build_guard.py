from pathlib import Path
import json,os,signal,subprocess,time
p=Path('/root/jobs/fx/cx/v21/s033/creply')
(p/'resume-entered.json').write_text(json.dumps({'time':time.time(),'pid':os.getpid()}))
def mem():return int(next(l.split()[1] for l in Path('/proc/meminfo').read_text().splitlines() if l.startswith('MemAvailable:')))
while True:
 locks=subprocess.check_output(['lslocks','-n','-o','MODE,PATH'],text=True)
 slots=sum(1 for l in locks.splitlines() if 'WRITE' in l and '*' not in l and any(l.strip().endswith(x) for x in ('/cpp-build.lock','/cpp-build.2.lock')))
 if slots<=1 and mem()>=14*1048576:break
 print('waiting for the other C++ compile slot or 14 GiB memory',flush=True);time.sleep(2)
log=open(p/'build-main-resume.log','w');f=open(p/'build-resume-memory.jsonl','w')
child=subprocess.Popen(['cmake','--build','framework/bench/grpc/cpp/build','--target','bench_cpp_client','bench_cpp_framework_server','bench_cpp_zlink_server','-j1'],stdout=log,stderr=log,start_new_session=True)
def stop(*args):
 if child.poll() is None:os.killpg(child.pid,signal.SIGTERM)
 child.wait()
 if args:raise KeyboardInterrupt()
signal.signal(signal.SIGTERM,stop);signal.signal(signal.SIGINT,stop)
try:
 while child.poll() is None:
  kb=mem();f.write(json.dumps({'time':time.time(),'available_kb':kb})+'\n');f.flush()
  if kb<13*1048576:stop();raise RuntimeError('resume stopped at 13 GiB guard')
  time.sleep(.1)
 assert child.returncode==0,child.returncode
finally:
 stop();f.close();log.close()
