import pathlib,subprocess,os,time,json,struct,copy
J=pathlib.Path('/root/jobs/fx/cx/v21/s033/xbp');R=pathlib.Path('/root/worktree/zlink-v21-s033/xbp');core=R/'.artifacts/wsl/install/zlink-core/1.23.0'
def run(name,args):
 print(time.strftime('%H:%M:%S'),name,flush=True)
 with open(J/'raw'/f'{name}.log','w') as f:
  f.write(json.dumps(args)+'\n');f.flush();p=subprocess.run(args,cwd=R,stdout=f,stderr=subprocess.STDOUT)
 if p.returncode:raise SystemExit(f'{name} failed {p.returncode}')
while not (core/'share/zlink/core-package-provenance.json').exists():time.sleep(10)
run('build-counter-main',['/root/jobs/fx/cx/cpp-build-slot.sh','bash',str(J/'build-counter.sh')])
while not (J/'raw/builds-done.json').exists():time.sleep(10)
run('build-check',['/root/jobs/fx/cx/measure-shared.sh','python3',str(J/'check_builds.py')])
while not pathlib.Path('/root/jobs/fx/cx/v21/s033/core124/verdict.md').exists():time.sleep(10)
D=J/'raw/counter-validation';D.mkdir(exist_ok=False);(D/'control.bin').write_bytes(struct.pack('QQ',1,2**64-1))
e=os.environ.copy();e.update(LD_PRELOAD=str(J/'counter.so'),XBP_DIR=str(D),LD_LIBRARY_PATH=str(core/'lib'))
with open(D/'fixture.log','w') as f:
 p=subprocess.run(['flock','-o','/root/jobs/fx/cx/measure.lock','taskset','-c','0-15',str(J/'counter-fixture')],cwd=R,env=e,stdout=f,stderr=subprocess.STDOUT)
assert p.returncode==0,p.returncode
from counter_io import read
# Import validation without running the report scanner.
namespace={};script=(J/'analyze.py').read_text();exec(script[:script.index('rows=[]')],namespace)
validate=namespace['validate']
files=list(D.glob('[0-9]*.bin'));assert len(files)==1,files
data=read(files[0]);(D/'counter.json').write_text(json.dumps(data,indent=2)+'\n')
s=max(data['stats'],key=lambda x:x['requests']);t=max(data['stats'],key=lambda x:x['reply_calls'])
validate(s);assert s['initial']==32 and s['completed']==32 and s['bp']>0 and s['resubmits']>0 and s['wake_progress']>0,s
assert s['sndhwm']==512 and t['rcvhwm']==512,(s['sndhwm'],t['rcvhwm'])
bad=copy.deepcopy(s);bad['wakes']+=1
try:validate(bad)
except AssertionError:rejected=True
else:rejected=False
assert rejected
(D/'validation.json').write_text(json.dumps({'positive':'pass','completed':s['completed'],'backpressured':s['bp'],'wakes':s['wakes'],'resubmits':s['resubmits'],'negative':'rejected deliberately incorrect WRITABLE total','hwmSourceSend':s['sndhwm'],'hwmTargetReceive':t['rcvhwm']},indent=2)+'\n')
print('counter positive and negative validation passed',flush=True)
for lang in ['cpp','dotnet','java']:
 run('pilot-'+lang,['flock','-o','/root/jobs/fx/cx/measure.lock','taskset','-c','16-23','python3',str(J/'run_cell.py'),lang,'framework','1024','count','pilot'])
(J/'raw/pilots-done.json').write_text(json.dumps({'end':time.time()}))
print('pilots done; inspect before full measurement',flush=True)
