from pathlib import Path
import subprocess,time,sys,os
r=Path('/root/jobs/fx/cx/v21/s033/cbclose2')
patterns=sys.argv[1:] or ['PAIR','PUBSUB','REQUEST_SERIAL']
for i in range(1,7):
 for pattern in patterns:
  attempt=1
  while True:
   p=subprocess.run(['flock','-o','/root/jobs/fx/cx/measure.lock','python3',str(r/'bench-pair.py'),str(i),pattern,str(attempt)])
   if p.returncode==0:break
   if p.returncode!=75:sys.exit(p.returncode)
   attempt+=1
   time.sleep(60)
