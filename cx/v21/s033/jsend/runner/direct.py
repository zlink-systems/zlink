import pathlib,subprocess
p=pathlib.Path(__file__).parent
for case,mode in [('direct-50k-combined','combined')]+[(f'direct-unpaced-{i}','none') for i in range(1,4)]+[('direct-50k-handoff','handoff'),('original-50k-repeat','perf'),('binding-50k-repeat','perf')]:
 cmd=['python3',str(p/('run.py' if mode=='none' else 'run-instrument.py')),case]
 if mode!='none':cmd+=['--rate','50000','--collect',mode]
 print('DIRECT MATRIX',case,flush=True);subprocess.run(cmd,check=True)
