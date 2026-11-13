import concurrent.futures,hashlib,json,os,pathlib,re,signal,statistics,struct,subprocess,sys,time,traceback,urllib.request
P=pathlib.Path('/root/jobs/fx/cx/v21/s033/xlang');W=pathlib.Path('/root/worktree/zlink-xlang');G=W/'framework/bench/grpc';T=P/'tools'
children=[];mem_min=10**12

def save(p,x):
 p=pathlib.Path(p);p.parent.mkdir(parents=True,exist_ok=True);p.write_text(json.dumps(x,indent=2,ensure_ascii=False))
def mem():
 global mem_min
 n=int(next(x.split()[1] for x in pathlib.Path('/proc/meminfo').read_text().splitlines() if x.startswith('MemAvailable:')));mem_min=min(mem_min,n)
 if n<12582912:raise RuntimeError('MemAvailable below 12 GiB: '+str(n))
 return n

def http(u,data=None):
 r=urllib.request.Request(u,None if data is None else json.dumps(data).encode(),headers={'content-type':'application/json'})
 with urllib.request.urlopen(r,timeout=330 if data is not None else 10) as f:return json.load(f)
def stats(u):return http(u+'/bench/stats')
def wait(u,pred,seconds=45):
 end=time.monotonic()+seconds
 while time.monotonic()<end:
  mem()
  for c in children:
   if c.poll() is not None:raise RuntimeError(('child exited',c.pid,c.returncode,c.args))
  try:
   s=stats(u)
   if s.get('phase')=='failed':raise RuntimeError(s)
   if pred(s):return s
  except (OSError,ValueError):pass
  time.sleep(.12)
 raise TimeoutError(u)

def settle(su,tu,counter):
 end=time.monotonic()+30;last=None;stable=0
 while time.monotonic()<end:
  mem();s=stats(su);t=stats(tu);key=(s.get('completed',0),s.get('currentInFlight',s.get('inFlight',0)),t[counter],t.get('errors',0),t.get('rejected'))
  stable=stable+1 if key==last else 0
  if stable>=2 and key[1]==0:return s,t
  last=key;time.sleep(.1)
 raise TimeoutError('settle bound hit: '+str(last))

def launch(cmd,out,name,env):
 save(out/(name+'.command.json'),cmd)
 h=open(out/(name+'.log'),'w');c=subprocess.Popen(cmd,stdout=h,stderr=h,env=env,start_new_session=True);h.close();children.append(c);save(out/(name+'.pid.json'),{'pid':c.pid});return c

def terminate(c,sig=signal.SIGTERM):
 if c.poll() is None:os.killpg(c.pid,sig)
 try:c.wait(timeout=20)
 except subprocess.TimeoutExpired:
  os.killpg(c.pid,signal.SIGINT);c.wait(timeout=20)

def cleanup():
 outcomes=[];stuck=[]
 for c in reversed(children):
  try:terminate(c);outcomes.append({'pid':c.pid,'code':c.returncode})
  except Exception as e:stuck.append(c);outcomes.append({'pid':c.pid,'error':str(e)})
 children[:]=stuck
 if stuck:raise RuntimeError('processes did not stop with SIGTERM/SIGINT: '+str(outcomes))
 return outcomes

def commands(lang,fw,pattern,out,env):
 impl='zlink-'+('framework-' if fw else '')+lang
 base={'cpp':5292 if fw else 5285,'java':5252 if fw else 5245,'dotnet':5212 if fw else 5205,'node':5232 if fw else 5225}[lang]
 trigger=f'http://127.0.0.1:{base}';su=f'http://127.0.0.1:{base+1}';tu=f'http://127.0.0.1:{base+3 if fw else base+4}';ep=f'tcp://127.0.0.1:{base+2}'
 target=['--endpoint',ep,'--metrics-url',tu]
 if not fw:target+=['--command-endpoint',f'tcp://127.0.0.1:{base+3}']
 source=['--implementation',impl,'--scenario',pattern,'--payload-size','1024','--request-window','100','--send-concurrency','8','--latency-sample-limit','200000','--warmup-seconds',env.get('XLANG_WARMUP','5'),'--drain-bound-ms','30000','--request-timeout-ms','30000','--route-ready-ms','30000','--trigger-url',trigger,'--stats-url',su,'--target-endpoint',ep,'--target-stats-url',tu,'--raw-socket','router','--output',str(out),'--report-file','report.txt']
 if not fw:source+=['--target-command-endpoint',f'tcp://127.0.0.1:{base+3}']
 if lang=='cpp':
  target=[str(G/'cpp/build'/('bench_cpp_framework_server' if fw else 'bench_cpp_zlink_server')),'--endpoint',ep,'--stats-port',str(base+3 if fw else base+4)]
  if not fw:target+=['--command-endpoint',f'tcp://127.0.0.1:{base+3}']
  source=[str(G/'cpp/build/bench_cpp_client'),'--implementation',impl,'--pattern',pattern,'--payload-size','1024','--trigger-port',str(base),'--stats-port',str(base+1),'--target-stats-port',str(base+3 if fw else base+4),'--endpoint',ep,'--output-file',str(out/'results.json'),'--warmup-seconds',env.get('XLANG_WARMUP','5'),'--warmup-segments','10','--request-timeout-ms','30000','--drain-bound-ms','30000']
  if not fw:source+=['--command-endpoint',f'tcp://127.0.0.1:{base+3}']
 elif lang=='java':
  module='zlink-framework-server' if fw else 'zlink-raw-server';name='bench-'+module
  target=[str(G/'java'/module/'build/install'/name/'bin'/name)]+target
  source=[str(G/'java/client/build/install/bench-client/bin/bench-client')]+source+['--run-id','xlang','--cell-id',impl+'-'+pattern+'-1024']
 elif lang=='dotnet':
  module='ZLinkServer' if fw else 'ZLinkRawServer'
  target=['dotnet',str(G/'dotnet'/module/'bin/Release/net8.0'/('WithGrpcBench.'+module+'.dll'))]+target
  source=['dotnet',str(G/'dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll')]+source+['--configuration','Release','--timeout-seconds','300']
 elif lang=='node':
  target=['node',str(G/'node'/('zlink-framework-server' if fw else 'zlink-raw-server')/'main.js')]+target
  source=['node',str(G/'node/client/main.js')]+source
 return impl,base,trigger,su,tu,['taskset','-c','0-7']+source,['taskset','-c','8-15']+target

def data_ports(pids,base,fw,out):
 text=subprocess.check_output(['ss','-ltnp'],text=True);(out/'listeners.txt').write_text(text)
 excluded={base,base+1,base+3 if fw else base+4,base+5,base+6};ports={side:[] for side in pids}
 for line in text.splitlines():
  f=line.split()
  if len(f)<5:continue
  m=re.search(r':(\d+)$',f[3])
  if not m:continue
  port=int(m[1])
  if port in excluded:continue
  for side,pid in pids.items():
   if f'pid={pid},' in line:ports[side].append(port)
 for side in ports:ports[side]=sorted(set(ports[side]))
 assert ports['target'],ports
 save(out/'data-ports.json',ports);return ports

def procshot(pids,su,tu):
 d={'realtime':time.time(),'time':time.monotonic(),'stats':{'source':stats(su),'target':stats(tu)},'processes':{}}
 for side,pid in pids.items():
  threads={}
  for t in pathlib.Path(f'/proc/{pid}/task').iterdir():
   try:
    s=(t/'stat').read_text();fields=s[s.rindex(')')+2:].split();status=(t/'status').read_text()
    allowed=sorted(os.sched_getaffinity(int(t.name)));expected=set(range(0,8) if side=='source' else range(8,16));assert set(allowed)<=expected,(pid,t.name,allowed)
    threads[t.name]={'cpus_allowed':allowed,'comm':(t/'comm').read_text().strip(),'runtime_ns':int((t/'schedstat').read_text().split()[0]),'user_ticks':int(fields[11]),'kernel_ticks':int(fields[12]),'switches':sum(int(x.split(':')[1]) for x in status.splitlines() if x.startswith(('voluntary_ctxt_switches:','nonvoluntary_ctxt_switches:')))}
   except (FileNotFoundError,ProcessLookupError):pass
  d['processes'][side]={'pid':pid,'threads':threads,'stat':pathlib.Path(f'/proc/{pid}/stat').read_text(),'maps':pathlib.Path(f'/proc/{pid}/maps').read_text()}
  ap=pathlib.Path(os.environ.get('XLANG_ALLOC_DIR','/none'))/f'alloc-{pid}.bin'
  # The caller reads allocation mmap separately with its per-cell path.
 d['end_time']=time.monotonic();return d

def allocshot(out,pids,prefix="alloc"):
 d={}
 for side,pid in pids.items():
  f=out/f'{prefix}-{pid}.bin'
  if not f.exists():continue
  b=f.read_bytes();n=min(struct.unpack_from('Q',b,8)[0],4096)
  d[side]=[struct.unpack_from('4Q',b,4096+i*64) for i in range(n)]
 return d

def phase(trigger,name,duration,impl,pattern):
 return http(trigger+'/bench/start',{'runId':'xlang','cellId':impl+'-'+pattern+'-1024','pattern':pattern,'payloadBytes':1024,'phase':name,'durationMs':duration*1000,'requestWindow':100,'sendConcurrency':8})

def collect_start(mode,out,pids,ports,env):
 cs=[]
 if mode=='profile':
  c=launch(['perf','record','--clockid','mono','-e','cpu-clock','-F','199','--call-graph','dwarf,8192','-p',','.join(str(x) for x in pids.values()),'-o',str(out/'perf.data')],out,'perf',env);cs.append(c)
 if mode in ('paced','profile','handoff'):
  expr='tcp and ('+' or '.join('port '+str(p) for p in sorted(set(ports['source']+ports['target'])))+')'
  if mode!='handoff':
   c=launch(['tcpdump','-i','lo','-Q','in','-nn','-s','96','-B','4096','-w',str(out/'tcp.pcap'),expr],out,'tcpdump',env);cs.append(c)
  # Directed scheduler wakeups are distinct from context switches.
  defs=''.join(f'@owned[{tid}]={pid};' for pid in pids.values() for tid in [int(t.name) for t in pathlib.Path(f'/proc/{pid}/task').iterdir()])
  cond=' || '.join(f'pid=={pid}' for pid in pids.values())
  program=f'''BEGIN {{{defs} @on=0; printf("READY\\n");}}
tracepoint:syscalls:sys_enter_getpgid /pid=={os.getpid()}/ {{if(args->pid=={pids['source']}){{@on=1;}} if(args->pid=={pids['target']}){{@on=0;}}}}
tracepoint:sched:sched_waking /@on && ({cond}) && @owned[args->pid]/ {{@wakes[pid,tid,args->pid]=count();}}
tracepoint:sched:sched_process_fork /@owned[args->parent_pid]/ {{@owned[args->child_pid]=@owned[args->parent_pid];}}
tracepoint:syscalls:sys_enter_sendto /@on && ({cond})/ {{@send[pid,tid]=count();}}
tracepoint:syscalls:sys_enter_recvfrom /@on && ({cond})/ {{@recv[pid,tid]=count();}}
END {{clear(@owned);clear(@on);}}
'''
  (out/'wake.bt').write_text(program)
  c=launch(['bpftrace','-q','-f','json','-B','line',str(out/'wake.bt')],out,'wake',env);cs.append(c)
  end=time.monotonic()+20
  while 'READY' not in (out/'wake.log').read_text():
   if c.poll() is not None:raise RuntimeError('bpftrace failed: '+(out/'wake.log').read_text())
   if time.monotonic()>end:raise TimeoutError('bpftrace startup')
   time.sleep(.1)
 return cs

def run(job):
 lang=job['lang'];fw=job['fw'];pattern=job['pattern'];mode=job.get('mode','unpaced');rate=job.get('rate',0)
 label=f"{lang}-{'F' if fw else 'B'}-{pattern}";out=P/'runs'/mode/(label+'-'+str(job.get('run',1))+('-a'+str(job['attempt']) if job.get('attempt',1)>1 else ''));out.mkdir(parents=True,exist_ok=True)
 env=os.environ.copy();env['PATH']=str(P/'bin')+':'+env['PATH'];env['JAVA_HOME']='/usr/lib/jvm/java-25-openjdk-amd64';env['PATH']=env['JAVA_HOME']+'/bin:'+env['PATH'];env['NODE_PATH']=str(W/'framework/languages/node/node_modules');env['XLANG_RATE']=str(rate);env['DOTNET_PerfMapEnabled']='1'
 env['JAVA_OPTS']='-Xms256m -Xmx2048m -XX:MaxDirectMemorySize=1024m';env['DOTNET_GCHeapHardLimit']='80000000';env['NODE_OPTIONS']='--max-old-space-size=2048'
 env['XLANG_ALLOC_DIR']=str(out);env['XLANG_WARMUP']=str(job.get('warmup',5))
 if mode=='alloc':env['LD_PRELOAD']=str(T/'alloc.so')
 if mode=='alloc' and lang=='node':
  env['NODE_OPTIONS']+=' --require='+str(T/'node-alloc.cjs');env['LD_PRELOAD']=str(T/'alloc.so')
 if mode=='alloc' and lang=='java':env['JAVA_TOOL_OPTIONS']='-agentpath:'+str(T/'jvm-alloc.so')
 if mode=='profile' and lang=='java':env['JAVA_TOOL_OPTIONS']='-XX:+PreserveFramePointer -XX:+UnlockDiagnosticVMOptions -XX:+DebugNonSafepoints -XX:+DumpPerfMapAtExit'
 if mode=='profile' and lang=='node':env['NODE_OPTIONS']+=' --perf-basic-prof-only-functions --require='+str(T/'node-alloc.cjs')
 impl,base,trigger,su,tu,source_cmd,target_cmd=commands(lang,fw,pattern,out,env)
 print(time.strftime('%H:%M:%S'),'START',mode,label,'rate',rate,flush=True)
 meta={'job':job,'start':time.time(),'out':str(out),'environment':{k:env[k] for k in ['JAVA_OPTS','JAVA_TOOL_OPTIONS','DOTNET_GCHeapHardLimit','NODE_OPTIONS','XLANG_RATE','LD_PRELOAD'] if k in env}};save(out/'meta.json',meta)
 collectors=[]
 try:
  mem();tenv=env.copy();senv=env.copy()
  if mode in ('alloc','profile') and lang=='node':tenv['XLANG_NODE_ALLOC']=str(base+6);senv['XLANG_NODE_ALLOC']=str(base+5)
  target=launch(target_cmd,out,'target',tenv);wait(tu,lambda s:True)
  source=launch(source_cmd,out,'source',senv);wait(su,lambda s:s.get('ready') is True)
  pids={'source':source.pid,'target':target.pid};save(out/'pids.json',pids)
  pool=concurrent.futures.ThreadPoolExecutor(max_workers=2)
  f=pool.submit(phase,trigger,'warmup',job.get('warmup',5),impl,pattern)
  while not f.done():mem();time.sleep(.2)
  save(out/'warmup.json',f.result());s=wait(su,lambda s:s.get('phase')=='idle',330)
  assert not s.get('currentInFlight',s.get('inFlight',0)),s
  warm_source,warm_target=settle(su,tu,'allPhaseMessages');save(out/'warmup-source.json',warm_source);save(out/'warmup-target.json',warm_target)
  assert warm_source.get('errors',0)==warm_target.get('errors',0)==0,(warm_source,warm_target)
  if lang=='cpp':http(tu+'/bench/reset',{})
  duration=job.get('duration',10 if mode=='unpaced' else 30)
  ports=data_ports(pids,base,fw,out);ss_filter='( '+' or '.join(f'sport = :{p} or dport = :{p}' for p in sorted(set(ports['source']+ports['target'])))+' )'
  collectors=collect_start(mode,out,pids,ports,env)
  if lang=='java' and mode in ('profile','alloc'):
   for side,pid in pids.items():
    cmd=[env['JAVA_HOME']+'/bin/jcmd',str(pid),'JFR.start','name=xlang','settings='+str(T/('cpu.jfc' if mode=='profile' else 'alloc.jfc')),'filename='+str(out/(side+'.jfr'))]
    subprocess.run(cmd,stdout=open(out/(side+'.jfr.log'),'w'),stderr=subprocess.STDOUT,check=True,env={k:v for k,v in env.items() if k not in ('JAVA_TOOL_OPTIONS','LD_PRELOAD','XLANG_ALLOC_DIR')})
  if mode in ('alloc','profile') and lang=='dotnet':
   trace='/root/jobs/fx/cx/v21/p032net/tools/dotnet-trace8/dotnet-trace'
   for side,pid in pids.items():collectors.append(launch([trace,'collect','-p',str(pid)]+(['--profile','cpu-sampling'] if mode=='profile' else ['--providers','Microsoft-Windows-DotNETRuntime:0x1:5'])+['--duration','00:00:00:'+str(duration+10),'-o',str(out/(side+'.nettrace'))],out,side+'-trace',{k:v for k,v in env.items() if k not in ('DOTNET_PerfMapEnabled','LD_PRELOAD','XLANG_ALLOC_DIR')}))
  if mode=='alloc' and lang=='node':
   http(f'http://127.0.0.1:{base+5}/start');http(f'http://127.0.0.1:{base+6}/start')
  if mode=='profile' and lang=='node':
   http(f'http://127.0.0.1:{base+5}/cpu/start');http(f'http://127.0.0.1:{base+6}/cpu/start')
  f=pool.submit(phase,trigger,'active',duration,impl,pattern)
  wait(su,lambda s:s.get('phase')=='active',10)
  time.sleep(.5)
  before=procshot(pids,su,tu);os.getpgid(pids['source']);save(out/'before.json',before);save(out/'alloc-before.json',allocshot(out,pids));save(out/'jvm-before.json',allocshot(out,pids,'jvm-alloc'))
  ss=subprocess.run(['ss','-tinp',ss_filter],capture_output=True,text=True);(out/'ss-before.txt').write_text(ss.stdout)
  end=time.monotonic()+duration-2
  while time.monotonic()<end:
   mem();time.sleep(.25)
   if f.done() and f.exception() is not None:f.result()
  os.getpgid(pids['target']);after=procshot(pids,su,tu);save(out/'after.json',after);save(out/'alloc-after.json',allocshot(out,pids));save(out/'jvm-after.json',allocshot(out,pids,'jvm-alloc'))
  ss=subprocess.run(['ss','-tinp',ss_filter],capture_output=True,text=True);(out/'ss-after.txt').write_text(ss.stdout)
  for c in collectors:
   terminate(c,signal.SIGINT)
   if c in children:children.remove(c)
  collectors=[]
  if mode in ('paced','profile'):assert (out/'tcp.pcap').stat().st_size>24,'tcpdump captured no packets'
  if lang=='java' and mode in ('profile','alloc'):
   for side,pid in pids.items():subprocess.run([env['JAVA_HOME']+'/bin/jcmd',str(pid),'JFR.stop','name=xlang'],env={k:v for k,v in env.items() if k not in ('JAVA_TOOL_OPTIONS','LD_PRELOAD','XLANG_ALLOC_DIR')},stdout=open(out/(side+'.jfr-stop.log'),'w'),stderr=subprocess.STDOUT,check=True)
  if mode=='alloc' and lang=='java':save(out/'jfr-capture-count.json',{'completed':stats(su)['completed']})
  if mode=='alloc' and lang=='node':
   http(f'http://127.0.0.1:{base+5}/stop');http(f'http://127.0.0.1:{base+6}/stop');save(out/'heap-capture-count.json',{'completed':stats(su)['completed']})
  if mode=='profile' and lang=='node':
   http(f'http://127.0.0.1:{base+5}/cpu/stop');http(f'http://127.0.0.1:{base+6}/cpu/stop')
  while not f.done():mem();time.sleep(.2)
  save(out/'active.json',f.result());pool.shutdown();s=wait(su,lambda s:s.get('phase')=='idle',330)
  s,t=settle(su,tu,'received');save(out/'source-stats.json',s);save(out/'target-stats.json',t)
  r=json.load(open(out/'results.json'));cell=r['cells'][0];cell['target_stats']=t
  checks={'source_errors':cell.get('errors',0),'abandoned':cell.get('abandoned',0),'target_errors':t.get('errors',0),'submitted':cell.get('submitted'),'completed':cell.get('completed'),'received':t.get('received'),'rejected':t.get('rejected',0)}
  save(out/'checks.json',checks)
  assert checks['source_errors']==checks['abandoned']==checks['target_errors']==0,checks
  if pattern!='send-saturation':assert cell['submitted']==cell['completed']==t['received'],checks
  else:assert cell['completed']==t['received']+(t.get('rejected') or 0),checks
  meta.update({'ok':True,'throughput':cell['throughput_per_second'],'checks':checks})
  print(time.strftime('%H:%M:%S'),'DONE',mode,label,cell['throughput_per_second'],flush=True)
 except Exception:
  meta.update({'ok':False,'error':traceback.format_exc()});print('FAIL',label,meta['error'],flush=True)
 finally:
  for c in collectors:
   try:terminate(c,signal.SIGINT)
   except Exception:pass
  try:meta['cleanup']=cleanup()
  except Exception:meta.update({'ok':False,'error':traceback.format_exc()})
  meta.update({'end':time.time(),'min_mem_kb':mem_min});save(out/'meta.json',meta)
 return meta

def signal_stop(*a):raise KeyboardInterrupt()
signal.signal(signal.SIGTERM,signal_stop)
(P/'queue').mkdir(exist_ok=True);(P/'done').mkdir(exist_ok=True)
save(P/'session.json',{'pid':os.getpid(),'start':time.time(),'lock':'flock -o /root/jobs/fx/cx/measure.lock','worktree':str(W)})
print('SESSION ACQUIRED',os.getpid(),time.strftime('%H:%M:%S'),flush=True)
last_session_save=0
try:
 while True:
  mem()
  if time.monotonic()-last_session_save>10:
   session_meta=json.loads((P/'session.json').read_text());session_meta.update({'min_mem_kb':mem_min,'last':time.time()});save(P/'session.json',session_meta);last_session_save=time.monotonic()
  if (P/'BLOCKED').exists():time.sleep(1);continue
  todo=sorted((P/'queue').glob('*.json'))
  if not todo:
   if (P/'STOP').exists():break
   time.sleep(1);continue
  p=todo[0];jobs=json.loads(p.read_text());results=[]
  for j in jobs:
   namespace={}
   source=pathlib.Path(__file__).read_text().split('\ndef signal_stop')[0]
   exec(compile(source,str(pathlib.Path(__file__)), 'exec'),namespace)
   result=namespace['run'](j);results.append(result);mem_min=min(mem_min,result.get('min_mem_kb',mem_min))
   if not result.get('ok'):
    (P/'BLOCKED').write_text(json.dumps({'batch':p.name,'job':j,'error':result.get('error')}));break
  save(P/'done'/p.name,results)
  if results and not results[-1].get('ok'):
   save(p,jobs[len(results)-1:])
  else:
   p.unlink()
   if p.name=='100-unpaced.json':subprocess.run(['python3',str(T/'queue-segments.py')],check=True)
except BaseException:
 traceback.print_exc();cleanup()
finally:
 s=json.loads((P/'session.json').read_text());s.update({'end':time.time(),'min_mem_kb':mem_min});save(P/'session.json',s)
 print('SESSION RELEASED',flush=True)
