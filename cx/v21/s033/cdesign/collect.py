import argparse,json,pathlib,subprocess,time,urllib.request,signal,os
p=argparse.ArgumentParser()
for x in ['source','target','source-url','target-url','out']:p.add_argument('--'+x,required=True)
a=p.parse_args();out=pathlib.Path(a.out);out.mkdir(parents=True,exist_ok=True)
pids={'source':int(a.source),'target':int(a.target)}
def stats(url):
 with urllib.request.urlopen(url+'/bench/stats',timeout=3) as r:return json.load(r)
def snap():
 d={'time':time.monotonic(),'stats':{r:stats(getattr(a,r+'_url')) for r in pids},'processes':{}}
 for role,pid in pids.items():
  ts={}
  for t in pathlib.Path(f'/proc/{pid}/task').iterdir():
   try:
    status=dict(l.split(':',1) for l in (t/'status').read_text().splitlines() if ':' in l)
    ts[t.name]={'comm':(t/'comm').read_text().strip(),'runtime_ns':int((t/'schedstat').read_text().split()[0]),'switches':int(status['voluntary_ctxt_switches'])+int(status['nonvoluntary_ctxt_switches'])}
   except FileNotFoundError:pass
  d['processes'][role]={'pid':pid,'threads':ts}
 return d
children=[];handles=[]
assert stats(a.source_url)['phase']=='active'
for role,pid in pids.items():
 h=open(out/f'{role}.perf.stderr','w');handles.append(h)
 children.append(subprocess.Popen(['perf','record','-e','cpu-clock','-F','199','--call-graph','dwarf,16384','-p',str(pid),'-o',str(out/f'{role}.data')],stdout=h,stderr=h))
h=open(out/'sched.stderr','w');handles.append(h)
children.append(subprocess.Popen(['perf','record','-a','-e','sched:sched_switch,sched:sched_wakeup','-o',str(out/'sched.data')],stdout=h,stderr=h))
time.sleep(.2);before=snap();time.sleep(3);after=snap()
for c in children:c.send_signal(signal.SIGINT)
for c in children:c.wait(timeout=15)
for h in handles:h.close()
(out/'before.json').write_text(json.dumps(before,indent=2));(out/'after.json').write_text(json.dumps(after,indent=2))
assert all(c.returncode in (0,130,-2) for c in children),[c.returncode for c in children]
assert after['stats']['source']['phase']=='active'
print('captured',out,after['time']-before['time'])
