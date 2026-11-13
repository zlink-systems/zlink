import json,os,pathlib,socket,subprocess,sys,threading,time
if sys.argv[1]=='launch':
 s=socket.socket(socket.AF_UNIX);s.connect(os.environ['BENCH_PROCESS_SOCKET']);s.sendall(json.dumps(sys.argv[2:]).encode());s.shutdown(socket.SHUT_WR);print(s.recv(100).decode());sys.exit()
p='/tmp/psched5-process-'+str(os.getpid())+'.sock';s=socket.socket(socket.AF_UNIX);s.bind(p);s.listen();s.settimeout(.1)
env=dict(os.environ,BENCH_PROCESS_SOCKET=p);runner=subprocess.Popen(sys.argv[1:],env=env);children=[]
try:
 while runner.poll() is None:
  try:
   conn,_=s.accept()
   with conn:
    data=b''
    while True:
     part=conn.recv(65536)
     if not part:break
     data+=part
    log,*cmd=json.loads(data)
    with open(log,'wb') as f: child=subprocess.Popen(cmd,stdout=f,stderr=subprocess.STDOUT,start_new_session=True)
    children.append(child);conn.sendall(str(child.pid).encode())
  except socket.timeout: pass
  for child in children: child.poll()
finally:
 import signal
 for child in children:
  if child.poll() is None: os.killpg(child.pid,signal.SIGTERM)
 for child in children: child.wait(timeout=15)
 s.close();pathlib.Path(p).unlink()
sys.exit(runner.returncode)
