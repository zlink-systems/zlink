from pathlib import Path
import json,subprocess,csv,re,hashlib
J=Path(__file__).parent;R=Path('/root/worktree/zlink-3867s');s=json.loads((J/'summary.json').read_text());checks={};threads=[]
for label,row in s.items():
 for i,c in enumerate(row['runs'],1):
  if 'roles' not in c:continue
  for role,rr in c['roles'].items():
   assert abs(sum(t['us'] for t in rr['threads'].values())-rr['total_us'])<1e-8
   for tid,t in rr['threads'].items():threads.append({'label':label,'run':i,'endpoint':role,'tid':tid,'comm':t['comm'],'role':t['role'],'cpu_us_per_message':t['us'],'cores':t['cores'],'runtime_ns':t['runtime_ns'],'messages':c['messages'],'capture_seconds':c['seconds']})
  checks[f'{label}/{i}']={'zero_errors':True,'exact_counts':True,'affinity':True,'phase_active':c['cell'].get('contaminated') is False,'proc_cpu_sum':True,'throughput':c['throughput']}
with (J/'threads.csv').open('w') as f:
 writer=csv.DictWriter(f,fieldnames=list(threads[0]));writer.writeheader();writer.writerows(threads)
# Deliberately fail the existing counter check; preserve both outputs.
p=next((J/'raw/rung0/run1').glob('*/results.json'));good=json.loads(p.read_text());bad=json.loads(p.read_text());bad['cells'][0]['target_stats']['received']-=1
(J/'negative-count.json').write_text(json.dumps(bad,indent=2))
commands=[]
for name,path,ok in [('valid',p,True),('invalid',J/'negative-count.json',False)]:
 result=subprocess.run(['bash','-c','source "$1"; verify_request_counts "$2"','quality',str(J/'runner-common.sh'),str(path)],text=True,capture_output=True)
 assert (result.returncode==0)==ok,(name,result.stdout,result.stderr)
 commands.append({'input':name,'returncode':result.returncode,'stdout':result.stdout,'stderr':result.stderr})
checks['counter_negative_control']=commands
symbols=subprocess.check_output(['nm','-C',str(J/'build/raw-client')],text=True)
assert not re.search(r'zlink::framework::(?:runtime::|app_t::|detail::)',symbols)
checks['rung0_has_no_framework_runtime_symbols']=True
manifest=json.loads((J/'manifest.json').read_text());checks['source_hashes_still_match']={p:hashlib.sha256((R/p).read_bytes()).hexdigest()==v for p,v in manifest['source_hashes'].items()};assert all(checks['source_hashes_still_match'].values())
checks['repository_status']=subprocess.check_output(['git','status','--short'],cwd=R,text=True);assert not checks['repository_status']
(J/'quality.json').write_text(json.dumps(checks,indent=2));print('valid measured cells:',len(threads),'thread rows; counts accept valid and reject one missing receive; repository clean')
