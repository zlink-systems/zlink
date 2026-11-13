import pathlib,subprocess
p=pathlib.Path(__file__).parent
for case,mode in [('nagle-50k-combined','combined')]+[(f'nagle-unpaced-{i}','none') for i in range(1,4)]:
 cmd=['python3',str(p/('run.py' if mode=='none' else 'run-instrument.py')),case]
 if mode!='none':cmd+=['--rate','50000','--collect',mode]
 print('NAGLE MATRIX',case,flush=True);subprocess.run(cmd,check=True)
