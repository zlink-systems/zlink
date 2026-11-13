from pathlib import Path
import json,hashlib,subprocess,re,os
J=Path('/root/jobs/fx/cx/v21/s033/coreio');R=Path('/root/worktree/zlink-ratio040');checks=[]
initial=json.load(open(J/'initial-state.json'))
assert subprocess.check_output(['git','rev-parse','HEAD'],cwd=R,text=True).strip()==initial['head']
files=subprocess.check_output(['git','ls-files','-m','-o','--exclude-standard'],cwd=R,text=True).splitlines()
assert set(files)==set(initial['files'])
assert {f:hashlib.sha256((R/f).read_bytes()).hexdigest() for f in files}==initial['files']
checks.append('HEAD and exact pre-existing changed/untracked file set and contents unchanged')
for p,d in json.load(open(J/'builds.json')).items():assert hashlib.sha256(Path(p).read_bytes()).hexdigest()==d['sha256']
checks.append('existing C++ executables and Core library hashes unchanged')
m=json.load(open(J/'metrics.json'));assert len(m)==6
for r in m:
 assert r['paired_epoll']
 for role in ('source','target'):
  a=r['roles'][role];assert len(a['connections'])==2,(r['language'],r['implementation'],role,a['connections'])
  assert a['eventfd_per_message']['io'].get('read',0)==0
  assert a['calls_per_message']['io'].get('sendmsg',{}).get('calls',0)==0
  assert abs(sum(a['cpu_stages'].values())-a['io_cpu_us_per_message'])<1e-8
  assert a['cpu_stages'].get('send',0)+a['cpu_stages'].get('recv',0)>0
 for w in r['wire'].values():assert len(w['part_sizes'])==2
 cap=Path(r['counter_capture']);b=json.load(open(cap/'before.json'));a=json.load(open(cap/'after.json'))
 assert b['stats']['source']['phase']==a['stats']['source']['phase']=='active'
 for snap in (b,a):
  assert snap['stats']['source']['errors']==0 and snap['stats']['target']['errors']==0
checks.append('six cells: two peer TCP connections, two application wire parts, no I/O sendmsg/eventfd reads, paired epoll, CPU stage sums, active counter windows, no workload errors')
# Each requested cell has at least one clean full run with exact final count agreement.
clean=set()
for p in J.glob('out/*/run1/*/results.json'):
 c=json.load(open(p))['cells'][0];t=c.get('target_stats')
 if not t:continue
 assert c.get('errors',0)==t.get('errors',0)==0
 assert not c.get('abandoned',0) and not c.get('drain_bound_hit',False) and not c.get('contaminated',False)
 assert c['completed']==t['received']
 clean.add((c['implementation'],c['pattern']))
assert len(clean)==6
checks.append('all six requested implementation/pattern cells have clean completed=received full runs, no errors/abandonment/drain contamination')
builds=set();minimum=None;alive=set();sizes=[]
for p in J.glob('out/*/run1/*/capture*/meta.json'):
 d=json.load(open(p));minimum=d['memavailable_kb'] if minimum is None else min(minimum,d['memavailable_kb'])
 for n in d.get('native_builds',{}).values():builds.add(n['build_id'])
 for pid in d.get('pids',{}).values():
  try:os.kill(pid,0);alive.add(pid)
  except ProcessLookupError:pass
assert minimum>=12582912
assert builds=={'cc6989f03587dd8c267953b75b2f40042efec476'}
assert not alive,alive
for p in J.glob('out/**/*'):
 if p.is_file() and (p.name.endswith('.perf.data') or p.name.endswith('.strace')):sizes.append((p.stat().st_size,str(p)))
assert sizes and max(n for n,p in sizes)<1_000_000_000
checks.append('all observed native build IDs identical; recorded memory above 12 GiB; all benchmark PIDs terminated; no raw trace exceeds 1 GB')
# Catch bad links and absent evidence files, including source line targets.
report=(J/'report.md').read_text();broken=[]
for target in re.findall(r'\]\(([^)]+)\)',report):
 if target.startswith(('http:','https:')):continue
 path=target.split('#')[0];path=re.sub(r':\d+$','',path);p=Path(path) if path.startswith('/') else J/path
 if not p.exists():broken.append(target)
assert not broken,broken
checks.append('all report artifact/source links resolve')
result={'checks':checks,'minimum_capture_memavailable_kb':minimum,'largest_raw_trace_bytes':max(n for n,p in sizes),'largest_raw_trace':max(sizes)[1],'raw_trace_count':len(sizes),'native_build_ids':sorted(builds),'clean_cells':sorted(clean),'report_words':len(report.split()),'status':'pass'}
(J/'verification.json').write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))
