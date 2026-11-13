import subprocess, pathlib, os, json, time
job=pathlib.Path('/root/jobs/fx/cx/v21/s033/n4json2')
for size in (4096,1024):
 for typed in ('untyped','typed'):
  for kind in ('before','projection','schema'):
   out=job/'raw'/f'candidate-{size}-{typed}-{kind}';out.mkdir()
   env=os.environ.copy();env['PROFILE_DIR']=str(out)
   cmd=['taskset','-c','6-7,14-15','node','--cpu-prof',f'--cpu-prof-dir={out}',f'--require={job}/tools/allocation-preload.cjs',str(job/'tools/candidate-costs.cjs'),kind,typed,str(size)]
   start=time.monotonic()
   with (out/'run.log').open('w') as log:
    result=subprocess.run(cmd,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=19)
   (out/'lifetime.json').write_text(json.dumps({'wall_seconds':time.monotonic()-start,'exit_code':result.returncode,'command':cmd}))
   print(out.name,result.returncode,flush=True)
   if result.returncode:raise SystemExit(result.returncode)
