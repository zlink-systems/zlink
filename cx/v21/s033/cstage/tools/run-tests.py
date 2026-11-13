from pathlib import Path
import hashlib,json,os,subprocess,sys,time
j=Path('/root/jobs/fx/cx/v21/s033/cstage')
rows=[]
for name,args,preload,expected in [('prototype-test-final',[],False,0),('cancel-budget-before-512',['cancel-budget'],True,2),('cancel-budget-after-512',['cancel-budget'],False,0)]:
 env=os.environ.copy();env.pop('LD_PRELOAD',None)
 if preload:env['LD_PRELOAD']=str(j/'core-before-cancel-fix.so')
 start=time.time()
 with (j/(name+'.log')).open('w') as f:
  p=subprocess.run([str(j/'prototype_test')]+args,env=env,stdout=f,stderr=subprocess.STDOUT)
 rows.append({'name':name,'exit_code':p.returncode,'expected':expected,'seconds':time.time()-start})
 print(rows[-1],flush=True)
 if p.returncode!=expected:print((j/(name+'.log')).read_text(),flush=True)
status={'passed':all(x['exit_code']==x['expected'] for x in rows),'tests':rows,'sha256':{str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in [j/'prototype_test',j/'core-before-cancel-fix.so',j/'package-staged/core/lib/libzlink.so']}}
(j/'prototype-test-status.json').write_text(json.dumps(status,indent=2))
sys.exit(0 if status['passed'] else 1)
