import subprocess,socket,time,json,urllib.request,pathlib
job=pathlib.Path('/root/jobs/fx/cx/v21/s033/cdrain')
s=socket.socket();s.bind(('127.0.0.1',0));port=s.getsockname()[1];s.close()
with open(job/'pm-smoke-target.log','w') as log:
 p=subprocess.Popen(['taskset','-c','8-15',str(job/'main-build/bench_cpp_pm_server'),'--endpoint','tcp://127.0.0.1:0','--stats-port',str(port)],stdout=log,stderr=log)
 try:
  until=time.monotonic()+20
  while True:
   assert p.poll() is None,p.returncode
   try:
    with urllib.request.urlopen(f'http://127.0.0.1:{port}/ready',timeout=1) as r:ready=json.load(r)
    break
   except OSError:
    assert time.monotonic()<until
    time.sleep(.1)
  with urllib.request.urlopen(f'http://127.0.0.1:{port}/pm/request',timeout=10) as r:reply=json.load(r)
  assert reply['spotLatencyUs']>0,reply
  print('PM public Instance Spot request smoke passed',flush=True)
 finally:
  p.terminate()
  try:p.wait(timeout=20)
  except subprocess.TimeoutExpired:p.kill();p.wait()
  assert p.returncode==0,p.returncode
