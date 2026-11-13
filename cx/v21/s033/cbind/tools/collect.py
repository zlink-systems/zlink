import argparse,json,os,pathlib,signal,subprocess,time,urllib.request,hashlib
p=argparse.ArgumentParser()
for a in ('mode','source','target','source-url','target-url','out'):p.add_argument('--'+a,required=True)
p.add_argument('--wait-active',action='store_true')
a=p.parse_args();out=pathlib.Path(a.out);out.mkdir(parents=True,exist_ok=True)
pids={'source':int(a.source),'target':int(a.target)}
def stats(url):
 with urllib.request.urlopen(url+'/bench/stats',timeout=5) as r:return json.load(r)
def snapshot():
 d={'monotonic':time.monotonic(),'realtime':time.time(),'stats':{'source':stats(a.source_url),'target':stats(a.target_url)},'processes':{}}
 for role,pid in pids.items():
  threads={}
  for t in pathlib.Path(f'/proc/{pid}/task').iterdir():
   try:threads[t.name]={'comm':(t/'comm').read_text().strip(),'runtime_ns':int((t/'schedstat').read_text().split()[0]),'stat':(t/'stat').read_text(),'status':(t/'status').read_text()}
   except FileNotFoundError:pass
  fds={}
  for f in pathlib.Path(f'/proc/{pid}/fd').iterdir():
   try:fds[f.name]={'link':os.readlink(f),'info':pathlib.Path(f'/proc/{pid}/fdinfo/{f.name}').read_text()}
   except FileNotFoundError:pass
  d['processes'][role]={'pid':pid,'threads':threads,'fds':fds,'maps':pathlib.Path(f'/proc/{pid}/maps').read_text(),'cmdline':pathlib.Path(f'/proc/{pid}/cmdline').read_text().replace('\0',' ')}
 d['tcp']=pathlib.Path('/proc/net/tcp').read_text();d['tcp6']=pathlib.Path('/proc/net/tcp6').read_text()
 d['end_monotonic']=time.monotonic();return d
if a.wait_active:
 deadline=time.monotonic()+90
 while time.monotonic()<deadline:
  try:
   if stats(a.source_url)['phase']=='active':break
  except (OSError,ValueError):pass
  time.sleep(.03)
 else:raise RuntimeError('active phase not observed')
assert stats(a.source_url)['phase']=='active' 
time.sleep(1)
mem=lambda:int(next(l.split()[1] for l in pathlib.Path('/proc/meminfo').read_text().splitlines() if l.startswith('MemAvailable:')))
assert mem()>=12582912
if a.mode=='uprobe' and os.environ.get('RECORD_BPF_FIRST')=='1':
 subprocess.run(['python3',str(pathlib.Path(__file__)), '--mode','bpf','--source',a.source,'--target',a.target,'--source-url',a.source_url,'--target-url',a.target_url,'--out',str(out.parent/'capture-bpf')],check=True)
 time.sleep(1)
children=[];handles=[]
def start(cmd,name):
 (out/(name+'.command.json')).write_text(json.dumps(cmd));h=open(out/(name+'.stderr'),'w');handles.append(h)
 ch=subprocess.Popen(cmd,stdout=h,stderr=h);children.append(ch);return ch
def stop_on_term(*args):raise KeyboardInterrupt()
signal.signal(signal.SIGTERM,stop_on_term)
before=None;after=None
try:
 if a.mode=='perf':
  for role,pid in pids.items():start(['perf','record','-e','cpu-clock','-F','149','--call-graph','dwarf,16384','-p',str(pid),'-o',str(out/(role+'.perf.data'))],role)
  time.sleep(.3);before=snapshot();time.sleep(10);after=snapshot()
 elif a.mode in ('bpf','uprobe'):
  cond=' || '.join(f'pid == {pid}' for pid in pids.values());cp=os.getpid()
  program=f'''BEGIN {{ @on = 0; printf("READY\\n"); }}
tracepoint:raw_syscalls:sys_enter /pid == {cp} && args->id == 62 && args->args[1] == 0/ {{
 if (args->args[0] == {pids['source']}) {{ @on = 1; printf("START %llu\\n", nsecs); }}
 if (args->args[0] == {pids['target']}) {{ @on = 0; printf("STOP %llu\\n", nsecs); }}
}}
tracepoint:raw_syscalls:sys_enter /@on && ({cond})/ {{
 $id = args->id;
 if ($id == 0 || $id == 1 || $id == 44 || $id == 45 || $id == 46 || $id == 47 || $id == 19 || $id == 20 || $id == 232 || $id == 233 || $id == 281) {{
  $fd=args->args[0]; if ($id==232) {{ @et[tid]=(int64)args->args[3]; @epolltimeout[pid,tid,$fd,(int64)args->args[3]]=count(); }} @call[pid,tid,$fd,$id]=count(); @live[tid]=1; @id[tid]=$id; @fd[tid]=$fd;
  if ($id == 0 || $id == 1 || $id == 44 || $id == 45) {{ @requested[pid,tid,$fd,$id]=sum(args->args[2]); }}
  @seen[tid,$fd,$id]=@seen[tid,$fd,$id]+1;
  if (@seen[tid,$fd,$id] == 4) {{ @stack[pid,tid,$fd,$id,ustack(30)]=count(); }}
 }}
}}
tracepoint:raw_syscalls:sys_exit /@live[tid]/ {{
 $fd=@fd[tid];$id=@id[tid];if ($id==232) {{ @epollret[pid,tid,$fd,@et[tid],args->ret>0 ? 1 : args->ret]=count();delete(@et[tid]); }} @returns[pid,tid,$fd,$id,args->ret < 0 ? args->ret : 0]=count();
 if (args->ret > 0) {{ @bytes[pid,tid,$fd,$id]=sum(args->ret); @positive[pid,tid,$fd,$id]=count(); @sizes[pid,tid,$fd,$id]=hist(args->ret); }}
 delete(@live[tid]);delete(@fd[tid]);delete(@id[tid]);
}}
END {{ clear(@live);clear(@et);clear(@fd);clear(@id);clear(@seen);clear(@on); }}
'''
  if a.mode=='uprobe':
   from uprobe_program import build
   program=program.replace('clear(@seen);clear(@on);','clear(@seen);clear(@on);clear(@cmdseen);clear(@recvptr);clear(@recvbox);')+build(pids,cond)
  (out/'counts.bt').write_text(program)
  ch=start(['bpftrace','-f','json','-B','line',str(out/'counts.bt')],'bpf')
  deadline=time.monotonic()+15
  while time.monotonic()<deadline:
   if 'READY' in (out/'bpf.stderr').read_text():break
   assert ch.poll() is None,'bpftrace failed'
   time.sleep(.05)
  else:raise RuntimeError('bpf not ready')
  before=snapshot()
  counter_before={'start':time.monotonic(),'stats':{'source':stats(a.source_url),'target':stats(a.target_url)},'end':time.monotonic()}
  os.kill(pids['source'],0);time.sleep(8);os.kill(pids['target'],0)
  counter_after={'start':time.monotonic(),'stats':{'source':stats(a.source_url),'target':stats(a.target_url)},'end':time.monotonic()}
  after=snapshot()
  before['snapshot_stats']=before['stats'];before['stats']=counter_before['stats'];before['counter_stats']=counter_before
  after['snapshot_stats']=after['stats'];after['stats']=counter_after['stats'];after['counter_stats']=counter_after
 elif a.mode=='strace':
  before=snapshot()
  for role,pid in pids.items():start(['strace','-f','-k','-ttt','-T','-xx','-s','131072','-v','-yy','-e','trace=sendto,recvfrom,sendmsg,recvmsg,read,write,readv,writev,epoll_wait,epoll_ctl','-p',str(pid),'-o',str(out/(role+'.strace'))],role)
  time.sleep(1.0);after=snapshot()
 else:raise ValueError(a.mode)
finally:
 for ch in children:
  if ch.poll() is None:ch.send_signal(signal.SIGINT)
 for ch in children:
  try:ch.wait(timeout=20)
  except subprocess.TimeoutExpired:ch.kill();ch.wait()
 for h in handles:h.close()
 if before:(out/'before.json').write_text(json.dumps(before,indent=2))
 if after:(out/'after.json').write_text(json.dumps(after,indent=2))
 native_builds={}
 if before:
  for role in pids:
   for l in before['processes'][role]['maps'].splitlines():
    if '/libzlink' in l:
     path=l.split()[-1]
     if path not in native_builds:
      try:
       bid=subprocess.check_output(['readelf','-n',path],text=True,stderr=subprocess.DEVNULL).split('Build ID: ')[-1].splitlines()[0]
       native_builds[path]={'sha256':hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest(),'build_id':bid}
      except (OSError,subprocess.CalledProcessError):pass
 (out/'meta.json').write_text(json.dumps({'mode':a.mode,'pids':pids,'memavailable_kb':mem(),'native_builds':native_builds,'exit_codes':[ch.returncode for ch in children]},indent=2))
assert after['stats']['source']['phase']=='active','capture extended past active'
assert all(ch.returncode in (0,130,-2) for ch in children),'tracer failed'
print('capture',a.mode,'seconds',after['monotonic']-before['monotonic'],'source delta',after['stats']['source'].get('completed',0)-before['stats']['source'].get('completed',0))

if a.mode=='perf':
 prior_bpf=out.parents[3]/out.parents[2].name.replace('-perf-','-bpf-')/'run1'/out.parent.name/'capture'/'after.json'
 modes=('bpf',)
 for mode in modes:
  if mode=='uprobe':time.sleep(2)
  subprocess.run(['python3',str(pathlib.Path(__file__)), '--mode',mode,'--source',a.source,'--target',a.target,'--source-url',a.source_url,'--target-url',a.target_url,'--out',str(out.parent/('capture-'+mode))],check=True)
