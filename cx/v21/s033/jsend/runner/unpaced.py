import subprocess,pathlib
p=pathlib.Path(__file__).parent
for i in range(1,4):
 for case in ('main','prototype','binding'):
  print('UNPACED',case,i,flush=True)
  subprocess.run(['python3',str(p/'run.py'),f'{case}-unpaced-{i}'],check=True)
