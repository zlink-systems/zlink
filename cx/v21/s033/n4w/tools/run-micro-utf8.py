import subprocess,pathlib,os,json,time,hashlib
job=pathlib.Path('/root/jobs/fx/cx/v21/s033/n4w')
for shape in ('0','1024','4096','nested','dto'):
 for kind in ('before','writer'):
  for phase in ('warm','counted'):
   out=job/'raw'/f'micro-utf8-{shape}-{kind}-{phase}';out.mkdir()
   env=os.environ.copy();env['PROFILE_DIR']=str(out)
   cmd=['taskset','-c','6-7,14-15','node','--cpu-prof',f'--cpu-prof-dir={out}',f'--require={job}/tools/allocation-preload.cjs',str(job/'tools/micro-utf8.cjs'),kind,shape]
   provenance={'head':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),'dirty':bool(subprocess.check_output(['git','status','--porcelain'],text=True).strip()),'base':subprocess.check_output(['git','merge-base','HEAD','origin/main'],text=True).strip(),'shape':shape,'kind':kind,'phase':phase,'writer_sha256':hashlib.sha256((job/'tools/writer.cjs').read_bytes()).hexdigest()}
   (out/'provenance.json').write_text(json.dumps(provenance,indent=2))
   with (job/'progress.md').open('a') as f:f.write('\nMicro '+json.dumps(provenance)+'; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.\n')
   assert (job/'raw/baseline-source-integrity.json').is_file()
   start=time.monotonic()
   with (out/'run.log').open('w') as log:r=subprocess.run(cmd,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=19)
   (out/'lifetime.json').write_text(json.dumps({'wall_seconds':time.monotonic()-start,'exit_code':r.returncode,'command':cmd}))
   print(out.name,r.returncode,flush=True)
   if r.returncode:raise SystemExit(r.returncode)
