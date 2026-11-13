import argparse, json, os, pathlib, subprocess, time, urllib.request, signal, struct, select
p=argparse.ArgumentParser()
for a in ('mode','source','target','source-url','target-url','out'): p.add_argument('--'+a,required=True)
a=p.parse_args(); out=pathlib.Path(a.out); out.mkdir(parents=True,exist_ok=True)
pids={'source':int(a.source),'target':int(a.target)}
def stats(url):
 with urllib.request.urlopen(url+'/bench/stats',timeout=3) as r: return json.load(r)
def snapshot():
 d={'monotonic':time.monotonic(),'stats':{'source':stats(a.source_url),'target':stats(a.target_url)},'processes':{}}
 for role,pid in pids.items():
  threads={}
  for t in pathlib.Path(f'/proc/{pid}/task').iterdir():
   try:
    s=(t/'stat').read_text(); fields=s[s.rindex(')')+2:].split(); status=dict(x.split(':',1) for x in (t/'status').read_text().splitlines() if ':' in x)
    threads[t.name]={'comm':(t/'comm').read_text().strip(),'runtime_ns':int((t/'schedstat').read_text().split()[0]),'utime_ticks':int(fields[11]),'stime_ticks':int(fields[12]),'voluntary':int(status.get('voluntary_ctxt_switches',0)),'involuntary':int(status.get('nonvoluntary_ctxt_switches',0))}
   except FileNotFoundError: pass
  d['processes'][role]={'pid':pid,'threads':threads,'affinity':sorted(os.sched_getaffinity(pid))}
 if a.mode=='fastcounts':
  for role,pid in pids.items():
   data=(out.parents[2]/'alloc'/f'alloc-{pid}.bin').read_bytes()
   magic,version,nslots=struct.unpack_from('QQQ',data,0)
   assert magic==0x435050414c4c4f43 and nslots<=4096,(magic,nslots)
   d['processes'][role]['alloc']={}
   for i in range(nslots):
    tid,m,c,r,f=struct.unpack_from('QQQQQ',data,4096+i*64)
    d['processes'][role]['alloc'][str(tid)]={'malloc':m,'calloc':c,'realloc':r,'free':f}
 d['end_monotonic']=time.monotonic(); return d
assert stats(a.source_url)['phase']=='active'
time.sleep(5)
steady_messages=int(os.environ.get('NSEND_CAPTURE_AFTER_MESSAGES','0'))
if steady_messages:
 deadline=time.monotonic()+12
 while stats(a.target_url)['received']<steady_messages:
  if time.monotonic()>deadline:raise RuntimeError('latency sample buffer did not fill before capture')
  time.sleep(.1)
mem=int(next(l.split()[1] for l in pathlib.Path('/proc/meminfo').read_text().splitlines() if l.startswith('MemAvailable:')))
assert mem>=12582912,mem
(out/'meta.json').write_text(json.dumps({'mode':a.mode,'pids':pids,'memavailable_kb':mem,'clock_ticks':os.sysconf('SC_CLK_TCK'),'steady_messages_threshold':steady_messages,'received_before_capture':stats(a.target_url)['received']},indent=2))
children=[]; handles=[]
if a.mode=='profile':
 for role,pid in pids.items():
  h=open(out/f'{role}.perf.stderr','w'); handles.append(h)
  cmd=['perf','record','-e','cpu-clock','-F','199','--call-graph','dwarf,16384','--sample-cpu','-p',str(pid),'-o',str(out/f'{role}.perf.data')]
  (out/f'{role}.command.json').write_text(json.dumps(cmd))
  children.append(subprocess.Popen(cmd,stderr=h,stdout=h))
 time.sleep(.2)
 before=snapshot(); time.sleep(8); after=snapshot()
 for ch in children: ch.send_signal(signal.SIGINT)
 for ch in children: ch.wait(timeout=30)
 if any(ch.returncode not in (0,130,-2) for ch in children): raise RuntimeError('perf failed')
elif a.mode=='fastcounts':
 controls=[]
 events='task-clock,context-switches,raw_syscalls:sys_enter,syscalls:sys_enter_futex,syscalls:sys_enter_sendto,syscalls:sys_enter_recvfrom,syscalls:sys_enter_read,syscalls:sys_enter_write,syscalls:sys_enter_epoll_wait'
 for role,pid in pids.items():
  cr,cw=os.pipe();ar,aw=os.pipe()
  h=open(out/f'{role}.stat.stderr','w');handles.append(h)
  cmd=['perf','stat','--per-thread','--delay=-1',f'--control=fd:{cr},{aw}','-x,','--no-big-num','-e',events,'-p',str(pid),'-o',str(out/f'{role}.stat.csv')]
  (out/f'{role}.stat.command.json').write_text(json.dumps(cmd))
  ch=subprocess.Popen(cmd,pass_fds=(cr,aw),stdout=h,stderr=h);children.append(ch);os.close(cr);os.close(aw);controls.append((cw,ar))
 time.sleep(.2)
 def control(cmd):
  for cw,ar in controls:os.write(cw,(cmd+'\n').encode())
  for cw,ar in controls:
   assert select.select([ar],[],[],5)[0],f'perf control timeout: {cmd}'
   assert os.read(ar,128).startswith(b'ack'),f'bad perf ack: {cmd}'
 before=snapshot();control('enable');time.sleep(8);control('disable');after=snapshot()
 for ch in children:ch.send_signal(signal.SIGINT)
 for ch in children:
  ch.wait(timeout=15)
  if ch.returncode not in (0,130,-2):raise RuntimeError('perf stat failed')
 for cw,ar in controls:os.close(cw);os.close(ar)
elif a.mode=='counts':
 libc='/lib/x86_64-linux-gnu/libc.so.6'; cond=' || '.join(f'pid == {pid}' for pid in pids.values())
 collector_pid=os.getpid()
 program=f'''BEGIN {{ @on = 0; printf("READY\\n"); }}
tracepoint:raw_syscalls:sys_enter /pid == {collector_pid} && args->id == 62 && args->args[1] == 0/ {{
 if (args->args[0] == {pids['source']}) {{ @on = 1; printf("START %llu\\n", nsecs); }}
 if (args->args[0] == {pids['target']}) {{ @on = 0; printf("STOP %llu\\n", nsecs); }}
}}
tracepoint:raw_syscalls:sys_enter /@on && ({cond})/ {{ @syscalls[pid,tid,args->id] = count(); }}
uprobe:{libc}:malloc /@on && ({cond})/ {{ @alloc[pid,tid] = count(); }}
uprobe:{libc}:calloc /@on && ({cond})/ {{ @alloc[pid,tid] = count(); }}
uprobe:{libc}:realloc /@on && ({cond})/ {{ @alloc[pid,tid] = count(); }}'''
 (out/'count.bt').write_text(program)
 h=open(out/'bpf.txt','w'); handles.append(h)
 ch=subprocess.Popen(['bpftrace','-B','line',str(out/'count.bt')],stdout=h,stderr=h); children.append(ch)
 deadline=time.monotonic()+12
 while time.monotonic()<deadline:
  if 'READY' in (out/'bpf.txt').read_text(): break
  if ch.poll() is not None: raise RuntimeError('bpftrace failed')
  time.sleep(.01)
 else: raise RuntimeError('bpftrace not ready')
 before=snapshot(); os.kill(pids['source'],0); time.sleep(8); os.kill(pids['target'],0); after=snapshot(); ch.send_signal(signal.SIGINT); ch.wait(timeout=15)
 if ch.returncode: raise RuntimeError('bpftrace failed')
elif a.mode=='trace':
 children=[]; handles=[]
 for role,pid in pids.items():
  h=open(out/f'{role}.strace.txt','w'); handles.append(h)
  children.append(subprocess.Popen(['strace','-qq','-c','-f','-p',str(pid)],stdout=h,stderr=h))
 deadline=time.monotonic()+5
 while time.monotonic()<deadline:
  attached=True
  for pid in pids.values():
   tids=list(pathlib.Path(f'/proc/{pid}/task').glob('*/status'))
   if not any(int(next((l.split()[1] for l in f.read_text().splitlines() if l.startswith('TracerPid:')), '0')) for f in tids if f.exists()):
    attached=False; break
  if attached: break
  time.sleep(.05)
 if not attached: raise RuntimeError('strace did not attach to both processes')
 before=snapshot();time.sleep(8);after=snapshot()
 for ch in children:ch.send_signal(signal.SIGINT)
 for ch in children:
  ch.wait(timeout=15)
  if ch.returncode not in (0,130,-2):raise RuntimeError('strace failed')
 for h in handles:h.close()
elif a.mode=='cpu':
 import re
 urls={}
 for role in ('source','target'):
  log=out.parent/f'{role}.log'
  deadline=time.monotonic()+4
  while time.monotonic()<deadline:
   text=log.read_text(errors='replace') if log.exists() else ''
   m=re.search(r'Debugger listening on (ws://127\.0\.0\.1:\d+/[^\s]+)',text)
   if m: urls[role]=m.group(1); break
   time.sleep(.05)
 if set(urls)!={'source','target'}: raise RuntimeError(f'inspector endpoints missing: {urls}')
 before=snapshot()
 commands=[]
 for role in ('source','target'):
  commands.append(subprocess.Popen(['node',str(pathlib.Path(__file__).with_name('cpu-sample.cjs')),urls[role],str(out/f'{role}.cpuprofile')],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True))
 for ch in commands:
  stdout,stderr=ch.communicate(timeout=20)
  if ch.returncode: raise RuntimeError(f'CPU sampler failed: {stderr}')
 after=snapshot()
elif a.mode in ('heap','diag'):
 import re
 urls={}
 for role in ('source','target'):
  log=out.parent/f'{role}.log'
  deadline=time.monotonic()+4
  while time.monotonic()<deadline:
   text=log.read_text(errors='replace') if log.exists() else ''
   m=re.search(r'Debugger listening on (ws://127\.0\.0\.1:\d+/[^\s]+)',text)
   if m: urls[role]=m.group(1); break
   time.sleep(.05)
 if set(urls)!={'source','target'}: raise RuntimeError(f'inspector endpoints missing: {urls}')
 before=snapshot()
 commands=[]
 for role in ('source','target'):
  commands.append(subprocess.Popen(['node',str(pathlib.Path(__file__).with_name('diag-sample.cjs' if a.mode=='diag' else 'heap-sample.cjs')),urls[role],str(out/f'{role}.diag.json' if a.mode=='diag' else out/f'{role}.heapprofile')],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True))
 for ch in commands:
  stdout,stderr=ch.communicate(timeout=20)
  if ch.returncode: raise RuntimeError(f'heap sampler failed: {stderr}')
 after=snapshot()
elif a.mode=='none':
 before=snapshot();time.sleep(8);after=snapshot()
else: raise ValueError(a.mode)
for h in handles:h.close()
(out/'before.json').write_text(json.dumps(before,indent=2)); (out/'after.json').write_text(json.dumps(after,indent=2))
print('capture',a.mode,'seconds',after['monotonic']-before['monotonic'],'source messages',after['stats']['source']['completed']-before['stats']['source']['completed'])
assert after['stats']['source']['phase']=='active','capture extended past active window'
