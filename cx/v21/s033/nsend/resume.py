import subprocess,pathlib,sys,json,statistics,time
J=pathlib.Path(__file__).parent

def run(name,extra):
 print('PHASE',name,time.strftime('%FT%T%z'),flush=True)
 with open(J/(name+'.log'),'w') as log:
  cmd=[sys.executable,str(J/'run.py'),'--out',name]+extra
  p=subprocess.Popen(cmd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
  for line in p.stdout:log.write(line);log.flush();print(line,end='',flush=True)
  code=p.wait()
  if code:raise SystemExit(code)

def summary(name):
 with open(J/(name+'-table.txt'),'w') as log:subprocess.run([sys.executable,str(J/'summarize.py'),name],stdout=log,check=True)
run('baseline-r3-clean',['--runs','1'])
import shutil
for cell in (J/'baseline-r3-clean'/'run1').iterdir():
 if cell.is_dir():shutil.copytree(cell,J/'baseline'/'run3'/cell.name)
(J/'baseline-replacement.json').write_text(json.dumps({'retained':'baseline/run1 and baseline/run2','excluded':'baseline/excluded/run3-unlocked-build and excluded-cpu-unlocked-build','replacement':'baseline-r3-clean/run1 copied to baseline/run3','reason':'external Java/javac compilation without shared lock; entire round excluded, independent of throughput'},indent=2)+'\n')
summary('baseline')
baseline_rows=json.loads((J/'baseline-summary.json').read_text());assert len(baseline_rows)==6 and all(len(x['F'])==len(x['B'])==3 for x in baseline_rows),baseline_rows
for name,mode in [('cpu','cpu'),('perf','profile'),('heap','heap'),('diag','diag')]:
 opts=['--runs','1','--patterns','send-saturation','--sizes','4096','--capture',mode]
 if name=='diag':opts+=['--preload','diag.cjs']
 run(name,opts);summary(name)
rows=json.loads((J/'baseline-summary.json').read_text());f=next(x for x in rows if x['pattern']=='send-saturation' and x['size']==4096);rate=min(f['F'])*.95
(J/'same-rate-target.json').write_text(json.dumps({'rate':rate,'basis':'0.95 * minimum unprofiled Framework send-saturation 4096 rate','F_baseline':f['F']},indent=2)+'\n')
run('same-rate',['--runs','3','--patterns','send-saturation','--sizes','4096','--rate',str(rate)]);summary('same-rate')
subprocess.run([sys.executable,str(J/'profile-summary.py'),'cpu'],stdout=open(J/'cpu-analysis.txt','w'),check=True)
subprocess.run([sys.executable,str(J/'perf-summary.py'),'perf'],stdout=open(J/'perf-analysis.txt','w'),check=True)
print('ALL BASELINE CAPTURES COMPLETE',flush=True)
