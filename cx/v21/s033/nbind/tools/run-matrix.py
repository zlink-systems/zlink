import json,os,pathlib,subprocess,time
job=pathlib.Path('/root/jobs/fx/cx/v21/s033/nbind')
results=[]
for mode in os.environ.get('MATRIX_MODES','audit,cpu,alloc').split(','):
 for repeat in [1,2]:
  for pattern,tag in [('send-saturation','send'),('request-backpressure','bp')]:
   for level in (['L1','L2','L3'] if repeat==1 else ['L3','L2','L1']):
    label=f'main-{mode}-{level}-{tag}-r{repeat}'
    log=job/'raw'/f'{label}-driver.log'
    with log.open('x') as f:
     t=time.monotonic();p=subprocess.run(['taskset','-c','6-7,14-15','python3',str(job/'tools/profile.py'),mode,level,pattern,label],stdout=f,stderr=subprocess.STDOUT)
    results.append(dict(label=label,rc=p.returncode,elapsed_s=time.monotonic()-t))
    (job/'raw/main-matrix-progress.json').write_text(json.dumps(results,indent=2)+'\n')
    print(results[-1],flush=True)
    if p.returncode:print('FAILED; preserving raw; stopping matrix',flush=True);raise SystemExit(p.returncode)
