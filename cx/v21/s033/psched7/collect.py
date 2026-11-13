import argparse,json,pathlib,time,urllib.request,os,threading
p=argparse.ArgumentParser()
for key in ('source','target','source-url','target-url','out'):p.add_argument('--'+key,required=True)
a=p.parse_args(); out=pathlib.Path(a.out);out.mkdir(parents=True,exist_ok=True)
pids={'source':int(a.source),'target':int(a.target)}
def stats(url):
 with urllib.request.urlopen(url+'/bench/stats',timeout=5) as r:return json.load(r)
def snap():
 d={'time':time.monotonic(),'stats':{r:stats(getattr(a,r+'_url')) for r in pids},'processes':{}}
 for role,pid in pids.items():
  ts={}
  for t in pathlib.Path(f'/proc/{pid}/task').iterdir():
   try:
    status=dict(l.split(':',1) for l in (t/'status').read_text().splitlines() if ':' in l)
    ts[t.name]={'comm':(t/'comm').read_text().strip(),'runtime_ns':int((t/'schedstat').read_text().split()[0]),'voluntary':int(status['voluntary_ctxt_switches']),'involuntary':int(status['nonvoluntary_ctxt_switches'])}
   except (FileNotFoundError,ProcessLookupError):pass
  d['processes'][role]={'pid':pid,'threads':ts}
 return d
pm=os.getenv('PM','0')=='1'
assert stats(a.source_url)['phase']=='active'
before=snap(); samples=[before];latencies=[];pm_samples=[]
until=time.monotonic()+(25 if pm else 3)
probe_errors=[]
def probe_stream():
 next_probe=time.monotonic()
 try:
  while time.monotonic()<until:
   pause=next_probe-time.monotonic()
   if pause>0:time.sleep(pause)
   if time.monotonic()>=until:break
   sent=time.monotonic()
   with urllib.request.urlopen(a.target_url+'/pm/request',timeout=5) as r:probe=json.load(r)
   probe['sentAt']=sent;probe['completedAt']=time.monotonic()
   latencies.append(probe['spotLatencyUs']);pm_samples.append(probe)
   next_probe=max(next_probe+.1,time.monotonic())
 except Exception as error:
  probe_errors.append(repr(error))
probe_thread=threading.Thread(target=probe_stream) if pm else None
if probe_thread:probe_thread.start()
while time.monotonic()<until:
 time.sleep(.1)
 samples.append(snap())
if probe_thread:probe_thread.join()
if probe_errors:raise RuntimeError(probe_errors)
after=samples[-1]
assert after['stats']['source']['phase']=='active'
(out/'before.json').write_text(json.dumps(before,indent=2));(out/'after.json').write_text(json.dumps(after,indent=2));(out/'samples.json').write_text(json.dumps(samples));(out/'pm-latencies.json').write_text(json.dumps(latencies));(out/'pm-probes.json').write_text(json.dumps(pm_samples))
print('captured',out,'seconds',after['time']-before['time'],'Spot samples',len(latencies),flush=True)
