from pathlib import Path
import subprocess,os,json,sys,time
r=Path('/root/jobs/fx/cx/v21/s033/cbclose2')
i=int(sys.argv[1]); pattern=sys.argv[2]; attempt=int(sys.argv[3]); stamp=f'{pattern}-{i}-attempt{attempt}'
uptime=subprocess.check_output(['uptime'],text=True).strip();load=os.getloadavg()[0]
record={'pair':i,'pattern':pattern,'attempt':attempt,'uptime':uptime,'load1':load,'runs':[]}
if load>4:
 record['deferred']=True;(r/(stamp+'.json')).write_text(json.dumps(record,indent=2));print('defer',stamp,uptime,flush=True);sys.exit(75)
order=['main','after'] if i%2 else ['after','main']
for label in order:
 mem=int(next(x.split()[1] for x in Path('/proc/meminfo').read_text().splitlines() if x.startswith('MemAvailable:')))
 if mem<12*1024*1024:raise RuntimeError('MemAvailable <12 GiB')
 tree=r/'main-tree' if label=='main' else Path('/root/worktree/zlink-4060-cpp-binding')
 if pattern=='REQUEST_SERIAL':cmd=[str(r/('serial-'+label))]
 else:cmd=[str(tree/'bindings/cpp/perf/single/build'/('cpp_perf_'+pattern.lower())),'cpp','inproc','1024']
 cmd=['taskset','-c','8-15']+cmd
 env=dict(os.environ,PERF_SINGLE_DURATION_SECONDS='5',PERF_PART_COUNT='1')
 start=time.time();p=subprocess.run(cmd,env=env,capture_output=True,text=True,timeout=90)
 log=r/(stamp+'-'+label+'.log');log.write_text(p.stdout+p.stderr)
 record['runs'].append({'label':label,'command':cmd,'exit':p.returncode,'start':start,'end':time.time(),'memory_kB':mem,'log':log.name})
 if p.returncode:break
(r/(stamp+'.json')).write_text(json.dumps(record,indent=2))
print(stamp,[(x['label'],x['exit']) for x in record['runs']],flush=True)
sys.exit(0 if len(record['runs'])==2 and all(x['exit']==0 for x in record['runs']) else 1)
