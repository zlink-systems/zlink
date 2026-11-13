from pathlib import Path
import subprocess,os,json
p=Path('/root/jobs/fx/cx/v21/s033/creply2/tools');b=p/'creply_mutex_test'
row=next(l.split() for l in subprocess.check_output(['nm','-S',str(b)],text=True).splitlines() if l.endswith(' marked'))
start,size=int(row[0],16),int(row[1],16);env=os.environ.copy();env['LD_PRELOAD']=str(p/'creply_mutex.so');results={}
for case,args,span in [('uncontended',[],f'{start:x}-{start+size:x}'),('contended',['hold'],f'{start:x}-{start+size:x}'),('excluded',[],'1-2')]:
 env.update(CREPLY_LOCK_RANGE=span,CREPLY_LOCK_OUTPUT=str(p/(case+'.json')));subprocess.run([str(b)]+args,env=env,check=True)
 results[case]=json.loads((p/(case+'.json')).read_text())
assert results['uncontended']['calls']==100 and results['uncontended']['contended']==0
assert results['contended']['calls']==101 and results['contended']['contended']>=1 and results['contended']['wait_ns']>1000000
assert results['excluded']['calls']==0
(p/'mutex-validation.json').write_text(json.dumps(results,indent=2));print(json.dumps(results))
