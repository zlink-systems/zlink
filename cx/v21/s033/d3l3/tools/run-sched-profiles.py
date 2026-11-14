import pathlib,os,time,subprocess,json
job=pathlib.Path('/root/jobs/fx/cx/v21/s033/d3l3');inode=os.stat('/root/jobs/fx/cx/measure.lock').st_ino
# Read-only observation; never opens/acquires the measurement lock.
def held():return any('FLOCK' in l and 'WRITE' in l and '->' not in l and any(v.endswith(':'+str(inode)) for v in l.split()) for l in pathlib.Path('/proc/locks').read_text().splitlines())
for label,level,pattern,size in [('sched-L2-send1K','L2','send-saturation',1024),('sched-L3-send1K','L3','send-saturation',1024),('sched-L2-bp4K','L2','request-backpressure',4096),('sched-L3-bp4K','L3','request-backpressure',4096)]:
 while held():time.sleep(2)
 print(label,flush=True)
 r=subprocess.run(['taskset','-c','6-7,14-15','python3',str(job/'tools/profile.py'),label,level,pattern,str(size)])
 print('rc',r.returncode,flush=True)
