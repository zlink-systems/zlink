#!/usr/bin/env python3
import os,sys,time,glob,signal
pid,out=sys.argv[1],sys.argv[2]
keep=True
def stop(*_):
 global keep; keep=False
signal.signal(signal.SIGINT,stop); signal.signal(signal.SIGTERM,stop)
hz=os.sysconf(os.sysconf_names['SC_CLK_TCK']); prev={}; totals={}; names={}
def snapshot():
 found={}
 for f in glob.glob(f'/proc/{pid}/task/*/stat'):
  try:
   s=open(f).read(); q=s.rfind(')'); name=s[s.index('(')+1:q]; a=s[q+2:].split(); tid=int(os.path.basename(os.path.dirname(f))); ticks=int(a[11])+int(a[12]); found[tid]=(ticks,name)
  except (OSError,ValueError,IndexError): pass
 return found
prev=snapshot(); start=time.monotonic_ns()
while keep:
 time.sleep(.05); now=snapshot()
 for tid,(ticks,name) in now.items():
  if tid in prev: totals[tid]=totals.get(tid,0)+max(0,ticks-prev[tid][0])
  else: totals[tid]=0
  names[tid]=name
 prev=now
end=time.monotonic_ns()
with open(out,'w') as f:
 f.write(f'# hz={hz} elapsed_ns={end-start}\n')
 for tid,ticks in sorted(totals.items(),key=lambda x:x[1],reverse=True): f.write(f'{tid}\t{ticks}\t{names[tid]}\n')
