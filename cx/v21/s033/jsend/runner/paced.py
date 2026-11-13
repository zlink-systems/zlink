import pathlib,subprocess
p=pathlib.Path(__file__).parent
for case,mode in [('prototype','combined'),('binding','combined'),('main','combined'),('prototype','handoff'),('binding','handoff')]:
 print('PACED',case,mode,flush=True)
 subprocess.run(['python3',str(p/'run-instrument.py'),f'{case}-50k-{mode}','--rate','50000','--collect',mode],check=True)
