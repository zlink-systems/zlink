import subprocess,pathlib
p=pathlib.Path(__file__).parent
for case in ('prototype','binding'):
 subprocess.run(['python3',str(p/'run-instrument.py'),case+'-50k-handoff','--rate','50000','--collect','handoff'],check=True)
