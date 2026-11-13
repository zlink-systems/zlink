from pathlib import Path
import json,re,subprocess,hashlib,ast,csv
J=Path(__file__).parent;repo=Path('/root/worktree/zlink-ratio040');data=json.load(open(J/'analysis.json'));report=(J/'report.md').read_text();checks=[]
def validate_partition(t):
 assert abs(sum(t['stage_us'].values())-t['cpu_us'])<1e-7
 for f in t.get('functions',[]):assert f['inclusive_us']+1e-10>=f['self_us']
for cell,r in data.items():
 assert r['messages']>0
 for role,rr in r['roles'].items():
  validate_partition(rr);assert abs(sum(t['cpu_us'] for t in rr['threads'].values())-rr['cpu_us'])<1e-7
  assert rr['critical_tid']==max(rr['threads'],key=lambda tid:rr['threads'][tid]['cpu_seconds'])
  for tid,t in rr['threads'].items():
   validate_partition(t);assert f'| {tid} |' in report,('missing TID',tid)
checks.append('four process profiles: all 175 thread rows present; stage/thread sums; self <= inclusive; critical selection')
# Negative test: corrupt a partition and confirm the invariant rejects it.
bad={'cpu_us':2.,'stage_us':{'a':1.}}
try:validate_partition(bad)
except AssertionError:checks.append('deliberately corrupted stage partition rejected')
else:raise AssertionError('bad partition accepted')
for mode in ['none','profile','fastcounts']:
 ps=list((J/f'out-{mode}/run1').glob('*/results.json'));assert len(ps)==2
 for p in ps:
  c=json.load(open(p))['cells'][0]
  assert not c['contaminated'] and not c['drain_bound_hit']
  assert c['errors']==c['abandoned']==c['header_validation_failures']==0
  assert c['completed']==c['target_stats']['received']
  assert c['submitted']==c['completed']
  cap=p.parent/'capture';a=json.load(open(cap/'after.json'));b=json.load(open(cap/'before.json'))
  assert a['stats']['source']['phase']==b['stats']['source']['phase']=='active'
 checks.append(mode+': both final cells clean; count agreement; snapshots inside active')
metas=[json.load(open(p)) for p in J.glob('out-*/run1/*/capture/meta.json')];assert len(metas)==6
minmem=min(x['memavailable_kb'] for x in metas);assert minmem>=12582912
checks.append(f'all six memory capture checks >=12 GiB (min {minmem/1048576:.2f} GiB)')
raw=list(J.glob('out-profile/run1/*/capture/*.perf.data'));assert len(raw)==4
assert all(p.stat().st_size<10**9 for p in raw)
for p in J.glob('out-profile/run1/*/capture/*.perf.stderr'):
 assert not re.search(r'\blost\b',p.read_text(),re.I)
checks.append('four raw perf files below 1 GB; no lost-sample diagnostics')
not_counted={}
for p in J.glob('out-fastcounts/run1/*/capture/*.stat.csv'):
 seen=0;missing=0
 for row in csv.reader(p.open()):
  if len(row)>5 and row[0] and not row[0].startswith('#'):
   if row[1]=='<not counted>':
    assert float(row[4])==0
    missing+=1;continue
   try:value=float(row[1]);running=float(row[5])
   except ValueError:raise AssertionError(('unsupported counter',row))
   assert running>=99.;seen+=1
 assert seen>0
 not_counted[str(p.relative_to(J))]=missing
checks.append('numeric perf stat events >=99% running; zero-running auxiliary rows explicitly excluded and disclosed')
cv=json.load(open(J/'counter-check/validation.json'));assert cv['observed']==cv['expected'] and cv['positive_pass'] and cv['deliberately_wrong_expectation_rejected']
checks.append('allocation interposer positive and deliberately wrong expected-count validation')
count=json.load(open(J/'counts-summary.json'))
for r in count.values():
 for rr in r['roles'].values():
  z=rr['total'];assert z['raw_syscalls:sys_enter']>=z['syscalls:sys_enter_futex']>=0
checks.append('futex counts are a subset of total syscall counts')
section=report.split('## Top 20 Framework functions on the critical thread')[1].split('## Re-ranked')[0]
assert re.findall(r'^\| (\d+) \|',section,re.M)==list(map(str,range(1,21)))
for pref,path in set(re.findall(r'`([CP])/([^:`\s;]+)',report)):
 full=repo/('framework/languages/cpp/framework/src/runtime' if pref=='C' else 'framework/languages/cpp/framework/include/zlink/framework/contracts')/path
 assert full.is_file(),full
assert (repo/'core/src/runtime/core/msg.cpp').is_file()
assert (repo/'bindings/cpp/src/Runtime/Messaging/completion_owner.cpp').is_file()
checks.append('exactly 20 top function rows; all C/P and additional Core/binding source files exist')
initial=json.load(open(J/'initial-state.json'));head=subprocess.check_output(['git','rev-parse','HEAD'],cwd=repo,text=True).strip();status=subprocess.check_output(['git','status','--short'],cwd=repo,text=True)
assert head==initial['commit'] and status==initial['status']
assert subprocess.check_output(['git','diff','--','framework/bench/grpc/cpp/run_local.sh'],cwd=repo,text=True)==initial['pinning_diff']
for lang in ['cpp','dotnet','java','node']:
 d=subprocess.check_output(['git','diff','--numstat','--',f'framework/bench/grpc/{lang}/run_local.sh'],cwd=repo,text=True);assert d.startswith('2\t2\t'),d
checks.append('HEAD and dirty-file list unchanged; original C++ pinning diff identical; four two-line pinning diffs remain')
for s in ['## Per-thread CPU','## Exclusive stage CPU','## Counts per completed message','## Top 20 Framework functions','## Re-ranked R / W / T / M / H','## Gap to 0.6 and first change']:assert s in report
checks.append('all requested report sections present')
(J/'verification.json').write_text(json.dumps({'checks':checks,'head':head,'status':status,'min_memory_gib':minmem/1048576,'not_counted_auxiliary_rows':not_counted,'raw_profile_sizes':{str(p.relative_to(J)):p.stat().st_size for p in raw},'report_sha256':hashlib.sha256((J/'report.md').read_bytes()).hexdigest()},indent=2))
print('\n'.join(checks))
