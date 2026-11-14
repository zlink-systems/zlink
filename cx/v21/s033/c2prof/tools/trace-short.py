from pathlib import Path
import subprocess,signal
import profile as p
class FutexTrace:
 def __init__(self,args,path):
  self.path=Path(str(path)+'.futex.txt');self.log=open(str(path)+'.log','w');self.pid=args[args.index('-p')+1];self.p=None
 def control(self,command):
  if command=='enable':
   self.p=subprocess.Popen(['perf','trace','-s','-e','futex','-p',self.pid,'-o',str(self.path)],stdout=self.log,stderr=self.log)
  elif command=='disable' and self.p and self.p.poll() is None:self.p.send_signal(signal.SIGINT)
  return 'ack\n'
 def stop(self):
  if self.p:
   if self.p.poll() is None:self.p.send_signal(signal.SIGINT)
   self.p.wait(timeout=4)
  self.log.close()
p.Perf=FutexTrace
for pattern in ['request-backpressure','send-saturation']:
 for level in [1,2]:p.run(level,pattern,2,'trace')
