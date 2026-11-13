import subprocess as S,pathlib as P,sys
j=P.Path('/root/jobs/fx/cx/v21/s033/csites');stage=sys.argv[1]
for round in range(1,4):
 for pattern in ['request-backpressure','send-saturation']:
  for impl in ['framework','binding']:
   log=j/f'{stage}-perf-{impl}-{pattern}-{round}.log'
   with log.open('w') as f:S.run([sys.executable,str(j/'measure.py'),stage,'throughput',impl,pattern,str(round)],stdout=f,stderr=f,check=True)
   print(stage,round,pattern,impl,'done',flush=True)
if stage=='after-final':
 for script,args,log in [('analyze.py',['before'],'before-analysis.log'),('analyze.py',['after-final'],'after-final-analysis.log'),('validate.py',[],'validation.log'),('provenance.py',[],'provenance.log')]:
  if script=='analyze.py' and args==['after-final'] and (j/'after-final-analysis.json').exists():
   print('using completed final count analysis',flush=True)
   continue
  with (j/log).open('w') as f:S.run([sys.executable,str(j/script)]+args,stdout=f,stderr=f,check=True)
  print(script,*args,'PASS',flush=True)
 import os
 env=dict(os.environ,CSITES_OUTPUT=str(j/'calibration'),LD_PRELOAD=str(j/'redirect.so')+':'+str(j/'before/libchand.so'))
 with (j/'calibration.log').open('w') as f:S.run([str(j/'calibrate')],env=env,stdout=f,stderr=f,check=True)
 S.run([sys.executable,str(j/'report-build.py')],check=True)
 print('report rendered',flush=True)
