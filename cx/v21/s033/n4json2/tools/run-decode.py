import os,subprocess,pathlib,json,time
job=pathlib.Path('/root/jobs/fx/cx/v21/s033/n4json2')
for size in (4096,1024):
 for kind in ('before','after'):
  out=job/'raw'/f'decode-{size}-{kind}';out.mkdir();env=os.environ.copy();env['PROFILE_DIR']=str(out)
  cmd=['taskset','-c','6-7,14-15','node','--cpu-prof',f'--cpu-prof-dir={out}',f'--require={job}/tools/allocation-preload.cjs',str(job/'tools/decode-costs.cjs'),kind,str(size)]
  start=time.monotonic()
  with (out/'run.log').open('w') as log:r=subprocess.run(cmd,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=19)
  (out/'lifetime.json').write_text(json.dumps({'wall_seconds':time.monotonic()-start,'exit_code':r.returncode,'command':cmd}))
  print(out.name,r.returncode,flush=True)
  if r.returncode:raise SystemExit(r.returncode)
