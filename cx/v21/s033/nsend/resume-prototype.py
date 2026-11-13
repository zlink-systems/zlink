import subprocess,pathlib,sys,json,shutil,time,os,hashlib
J=pathlib.Path(__file__).parent
ROOT=pathlib.Path('/root/worktree/zlink-nsend')
REL=pathlib.Path('framework/languages/node/packages/framework/dist/runtime/backend/node')
baseline_sha=hashlib.sha256((J/'baseline-code'/REL/'node-raw-binding-port.js').read_bytes()).hexdigest()
candidate_sha=hashlib.sha256((ROOT/REL/'node-raw-binding-port.js').read_bytes()).hexdigest()
assert baseline_sha!=candidate_sha, 'candidate runtime was not built'
(J/'variant-manifest.json').write_text(json.dumps({'baseline_js_sha256':baseline_sha,'candidate_js_sha256':candidate_sha},indent=2)+'\n')
variant=J/'prototype-code';variant.mkdir(exist_ok=True)
source_rel=pathlib.Path('framework/languages/node/packages/framework/src/runtime/backend/node/node-raw-binding-port.ts')
source_dest=variant/source_rel;source_dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(ROOT/source_rel,source_dest)

for f in (ROOT/REL).glob('node-raw-binding-port.*'):
 q=variant/REL/f.name;q.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(f,q)
def select(code):
 for f in (code/REL).glob('node-raw-binding-port.*'):shutil.copyfile(f,ROOT/REL/f.name)
def run(name,extra):
 print('PHASE',name,time.strftime('%FT%T%z'),flush=True)
 with open(J/(name+'.log'),'w') as log:
  p=subprocess.Popen([sys.executable,str(J/'run.py'),'--out',name]+extra,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
  for line in p.stdout:log.write(line);log.flush();print(line,end='',flush=True)
  code=p.wait()
  if code:raise SystemExit(code)
def summary(name):
 with open(J/(name+'-table.txt'),'w') as log:subprocess.run([sys.executable,str(J/'summarize.py'),name],stdout=log,check=True)
try:
 select(J/'baseline-code')
 run('matched-rate-baseline',['--runs','3','--patterns','send-saturation','--sizes','4096','--match-framework-rate']);summary('matched-rate-baseline')
finally:select(variant)
run('prototype',['--runs','5']);summary('prototype')
os.environ['NSEND_CAPTURE_AFTER_MESSAGES']='205000'
for name,mode in [('prototype-cpu','cpu'),('prototype-diag','diag')]:
 opts=['--runs','1','--patterns','send-saturation','--sizes','4096','--capture',mode,'--duration','30']
 if mode=='diag':opts+=['--preload','diag.cjs']
 run(name,opts);summary(name)
subprocess.run([sys.executable,str(J/'profile-summary.py'),'prototype-cpu'],stdout=open(J/'prototype-cpu-analysis.txt','w'),check=True)
subprocess.run([sys.executable,str(J/'diag-summary.py'),'prototype-diag'],stdout=open(J/'prototype-diag-analysis.txt','w'),check=True)
subprocess.run([sys.executable,str(J/'same-rate-analysis.py'),'matched-rate-baseline'],stdout=open(J/'matched-rate-baseline-cpu-table.txt','w'),check=True)
subprocess.run([sys.executable,str(J/'artifact-check.py')],check=True)
print('PROTOTYPE CAPTURES COMPLETE',flush=True)
