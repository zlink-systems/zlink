import argparse,json,pathlib,time,urllib.request,os,threading,subprocess,signal
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
perf_log=open(out/'perf-record.log','w')
perf=subprocess.Popen(['perf','record','-k','mono','-e','cpu-clock','-F',os.getenv('PERF_FREQ','499'),'--call-graph','dwarf,16384',*(['-a','-C','8-15'] if os.getenv('PERF_SCOPE')=='cpu' else ['-p',str(pids['target'])]),'-o',str(out/'perf.data')],stdout=perf_log,stderr=perf_log,start_new_session=True) if os.getenv('PERF_ENABLE','1')=='1' else None
sched_log=open(out/'perf-sched-record.log','w')
sched=subprocess.Popen(['perf','record','-k','mono','-a','-C','8-15','-e','sched:sched_switch','-e','sched:sched_wakeup','-e','sched:sched_wakeup_new','-o',str(out/'sched.data')],stdout=sched_log,stderr=sched_log,start_new_session=True) if os.getenv('PERF_SCHED','0')=='1' else None
stat_log=open(out/'perf-stat.log','w')
stat=subprocess.Popen(['perf','stat','-x',',','-e','task-clock,cycles,instructions,cache-references,cache-misses','-p',str(pids['target']),'-o',str(out/'perf-stat.csv')],stdout=stat_log,stderr=stat_log,start_new_session=True) if os.getenv('PERF_STAT','1' if os.getenv('PERF_FREQ')=='99' else '0')=='1' else None
mem_log=open(out/'perf-mem-record.log','w')
mem=subprocess.Popen(['perf','mem','record','-a','-C','8-15','-e','mem-ldst','-c','262144','-o',str(out/'mem.data')],stdout=mem_log,stderr=mem_log,start_new_session=True) if os.getenv('PERF_MEM','0')=='1' else None
time.sleep(.15)
before=snap(); samples=[before];latencies=[];pm_samples=[]
until=time.monotonic()+(25 if pm else float(os.getenv('PROFILE_SECONDS','3')))
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
(out/'before.json').write_text(json.dumps(before,indent=2));(out/'after.json').write_text(json.dumps(after,indent=2));(out/'samples.json').write_text(json.dumps(samples));(out/'pm-latencies.json').write_text(json.dumps(latencies));(out/'pm-probes.json').write_text(json.dumps(pm_samples))
def stop_profiler(process):
 if process.poll() is not None:return
 os.killpg(process.pid,signal.SIGINT)
 try:process.wait(timeout=10)
 except subprocess.TimeoutExpired:
  os.killpg(process.pid,signal.SIGTERM);process.wait(timeout=5)
if mem:
 stop_profiler(mem);mem_log.close()
 (out/'perf-mem-exit.txt').write_text(str(mem.returncode))
if stat:
 stop_profiler(stat);stat_log.close()
if perf:
 stop_profiler(perf);perf_log.close()
 (out/'perf-exit.txt').write_text(str(perf.returncode))
if sched:
 stop_profiler(sched);sched_log.close()
 (out/'perf-sched-exit.txt').write_text(str(sched.returncode))
assert after['stats']['source']['phase']=='active'
(out/'before.json').write_text(json.dumps(before,indent=2));(out/'after.json').write_text(json.dumps(after,indent=2));(out/'samples.json').write_text(json.dumps(samples));(out/'pm-latencies.json').write_text(json.dumps(latencies));(out/'pm-probes.json').write_text(json.dumps(pm_samples))
print('captured',out,'seconds',after['time']-before['time'],'Spot samples',len(latencies),flush=True)

# The runner still owns measure.lock exclusively; the collector is pinned to 0-7.
for profiler_name,data_name,fields in [('perf','perf.data','comm,pid,tid,time,period,event,ip,sym,dso'),('sched','sched.data','comm,tid,time,cpu,event,trace'),('mem','mem.data','comm,pid,tid,time,event,ip,sym,symoff,dso,addr,data_src')]:
 process=globals().get(profiler_name)
 if process is None:continue
 with (out/(profiler_name+'.script')).open('w') as dest,(out/(profiler_name+'-script.log')).open('w') as log:
  subprocess.run(['perf','script','-i',str(out/data_name),'-F',fields],stdout=dest,stderr=log)
 if profiler_name=='mem':
  with (out/'c2c-report.txt').open('w') as dest,(out/'c2c-report.log').open('w') as log:
   subprocess.run(['perf','c2c','report','--stdio','-i',str(out/data_name)],stdout=dest,stderr=log)
