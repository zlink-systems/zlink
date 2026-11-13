import json, os, pathlib, signal, subprocess, time
job=pathlib.Path('/root/jobs/fx/cx/v21/s033/dcount')
runner=job/'overlay3/framework/bench/grpc/dotnet/run_local.sh'
limit=12*1024*1024
records=[]
def mem():
 return int(next(x.split()[1] for x in pathlib.Path('/proc/meminfo').read_text().splitlines() if x.startswith('MemAvailable:')))
for r in range(1,4):
 for impl in ('zlink-framework-dotnet','zlink-dotnet'):
  tag=f'base-r{r}-{impl}'
  out=job/'raw'/tag
  env=os.environ.copy(); env.update(SKIP_BUILD='1',RUNS='1',IMPLEMENTATIONS=impl,PATTERNS='request-backpressure,request-serial,send-saturation',PAYLOAD_SIZES='1024,4096',WARMUP_SECONDS='30',DURATION_SECONDS='5',RUN_STAMP=tag,OUTPUT_DIR=str(out))
  m=mem()
  if m<limit: raise RuntimeError(f'MemAvailable below 12 GiB before {tag}: {m}')
  cmd=['flock','-o','/root/jobs/fx/cx/measure.lock','env']+[f'{k}={env[k]}' for k in ('SKIP_BUILD','RUNS','IMPLEMENTATIONS','PATTERNS','PAYLOAD_SIZES','WARMUP_SECONDS','DURATION_SECONDS','RUN_STAMP','OUTPUT_DIR')]+['bash',str(runner)]
  log=(job/'raw'/f'{tag}.log');log.parent.mkdir(parents=True,exist_ok=True)
  started=time.time(); minmem=m
  with log.open('w') as f:
   p=subprocess.Popen(cmd,cwd='/root/worktree/zlink-1639d',env=env,stdout=f,stderr=subprocess.STDOUT,start_new_session=True)
   while p.poll() is None:
    minmem=min(minmem,mem())
    if minmem<limit:
     os.killpg(p.pid,signal.SIGTERM);p.wait();raise RuntimeError(f'MemAvailable below limit during {tag}: {minmem}')
    time.sleep(1)
  if p.returncode: raise RuntimeError(f'{tag} failed {p.returncode}; see {log}')
  for pattern in ('request-backpressure','request-serial','send-saturation'):
   for size in (1024,4096):
    f=out/'run1'/f'{impl}-{pattern}-{size}'/'results.json'
    d=json.loads(f.read_text())['cells'][0]
    records.append({'round':r,'implementation':impl,'pattern':pattern,'payload':size,'mps':d['throughput_per_second'],'completed':d['completed'],'client_alloc_bytes':d.get('client_allocated_bytes'),'server_alloc_bytes':d.get('server_allocated_bytes'),'client_work_items':d.get('client_threadpool_work_items'),'server_work_items':d.get('server_threadpool_work_items'),'mem_min_gib':minmem/1024/1024,'result':str(f)})
  (job/'raw'/'baseline-results.json').write_text(json.dumps(records,indent=2)+'\n')
  print(f'DONE {tag}: {len(records)} cells, elapsed={time.time()-started:.1f}s, min_mem={minmem/1024/1024:.2f} GiB',flush=True)
