from pathlib import Path
import subprocess,signal,os,time,json
P=Path('/root/jobs/fx/cx/v21/s033/creply2');B=Path('/root/worktree/zlink-3867-creply2/framework/bench/grpc/cpp/build')
env=os.environ.copy();env['LD_PRELOAD']=str(B/'framework/libtest_cpp_framework_reply_close_clock.so');env['LD_LIBRARY_PATH']='/root/.cache/zlink/core/1.23.0/linux-x64/lib:'+env.get('LD_LIBRARY_PATH','')
command=['taskset','-c','0-15','stress-ng','--cpu','16','--cpu-load','90','--metrics-brief','--timeout','300s']
start=time.time();stress=None
with (P/'logs/stress-ng.log').open('w') as out:
 try:
  stress=subprocess.Popen(command,stdout=out,stderr=subprocess.STDOUT,start_new_session=True)
  time.sleep(1)
  assert stress.poll() is None
  with (P/'logs/stress-race-1000.log').open('w') as log:
   race=subprocess.run(['taskset','-c','0-15',str(B/'framework/test_cpp_framework_reply_close'),'race','1000'],env=env,stdout=log,stderr=subprocess.STDOUT)
  result=race.returncode
  assert result==0, result
  assert stress.poll() is None,'stress ended before race completed'
 finally:
  if stress is not None and stress.poll() is None:os.killpg(stress.pid,signal.SIGTERM)
  if stress is not None:stress.wait(timeout=30)
  (P/'stress-status.json').write_text(json.dumps({'command':command,'stress_exit':None if stress is None else stress.returncode,'race_exit':locals().get('result'),'started':start,'ended':time.time()},indent=2))
print('1000 race iterations under 16 CPU stress passed; stress stopped with SIGTERM')
