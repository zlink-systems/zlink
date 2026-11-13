import json, pathlib, subprocess, time, urllib.request, csv
job=pathlib.Path('/root/jobs/fx/cx/v21/s033/dcount')
roles={'source':('5213','http://127.0.0.1:5213/bench/stats'),'target':('5215','http://127.0.0.1:5215/bench/stats')}
pids={r:int(subprocess.check_output(['lsof','-nP','-t','-iTCP:'+port,'-sTCP:LISTEN'],text=True).splitlines()[-1]) for r,(port,url) in roles.items()}
def stats(url):
 try:
  with urllib.request.urlopen(url,timeout=1) as x:return json.load(x)
 except Exception:return {}
def snap(pid):
 d={}
 for p in pathlib.Path(f'/proc/{pid}/task').glob('*/stat'):
  try:
   s=p.read_text(); rr=s.rfind(')'); f=s[rr+2:].split(); tid=p.parent.name
   comm=pathlib.Path(f'/proc/{pid}/task/{tid}/comm').read_text().strip()
   d[tid]=(int(f[11])+int(f[12]),comm)
  except (OSError,ValueError,IndexError):pass
 return d
while True:
 st=stats(roles['source'][1])
 if st.get('phase')=='active': break
 time.sleep(.02)
start=time.monotonic(); before={r:snap(p) for r,p in pids.items()}
while stats(roles['source'][1]).get('phase')=='active': time.sleep(.02)
end=time.monotonic(); after={r:snap(p) for r,p in pids.items()}
hz=int(subprocess.check_output(['getconf','CLK_TCK'],text=True).strip())
out={}
for role in roles:
 vals=[]
 for tid,(ticks,name) in before[role].items():
  if tid in after[role]:
   secs=max(0,after[role][tid][0]-ticks)/hz
   vals.append({'tid':tid,'name':name,'cpu_seconds':secs,'cpu_us_per_completed':None,'core_share':secs/(end-start)})
 out[role]=sorted(vals,key=lambda a:a['cpu_seconds'],reverse=True)
resdir=next((job/'raw'/'base-r3-zlink-framework-dotnet'/'run1').glob('zlink-framework-dotnet-request-backpressure-1024'))
cell=json.load((resdir/'results.json').open())['cells'][0]; n=cell['completed']
for role in roles:
 for t in out[role]:t['cpu_us_per_completed']=t['cpu_seconds']*1e6/n
(job/'raw'/'framework-thread-cpu-r3.json').write_text(json.dumps({'elapsed_seconds':end-start,'completed':n,'pids':pids,'threads':out},indent=2)+'\n')
print(json.dumps({'elapsed':end-start,'completed':n,'top':{r:out[r][:5] for r in out}},indent=2))
