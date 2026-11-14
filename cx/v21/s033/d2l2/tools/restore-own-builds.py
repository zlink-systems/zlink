import os,pathlib,time,json
inode=os.stat('/root/jobs/fx/cx/measure.lock').st_ino
def held():
 return any('FLOCK' in l and 'WRITE' in l and '->' not in l and any(v.endswith(':'+str(inode)) for v in l.split()) for l in pathlib.Path('/proc/locks').read_text().splitlines())
while held():time.sleep(2)
rows=[]
def own(p,root=False):
 try:
  d=pathlib.Path(f'/proc/{p}');cmd=d.joinpath('cmdline').read_bytes().replace(b'\0',b' ').decode()
  if root and 'd2l2' not in cmd:return
  for c in d.joinpath(f'task/{p}/children').read_text().split():own(int(c))
  for t in d.joinpath('task').iterdir():
   if held():return
   tid=int(t.name);old=os.sched_getscheduler(tid)
   if old==os.SCHED_IDLE:
    os.sched_setscheduler(tid,os.SCHED_OTHER,os.sched_param(0));rows.append(dict(pid=p,tid=tid,old=old,new=0,command=cmd[:200]))
 except (FileNotFoundError,ProcessLookupError):pass
for p in (2152442,2180441,2214121):own(p,True)
pathlib.Path('/root/jobs/fx/cx/v21/s033/d2l2/raw/own-build-scheduler-restoration.json').write_text(json.dumps(rows,indent=2))
print('Own threads restored outside a hold:',len(rows))
