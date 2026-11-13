from pathlib import Path
from statistics import median
import json,re,hashlib,subprocess,math,csv
J=Path(__file__).parent;R=Path('/root/worktree/zlink-3867s');s=json.loads((J/'summary.json').read_text());p=json.loads((J/'profiles.json').read_text());t=(J/'report.md').read_text()
t=re.sub(r'### Where each incremental CPU demand appears\n.*?\n(?=## Largest throughput losses:)', '', t, flags=re.S)
# Where the conditional delta lands. Full source receive is the hottest unnamed
# non-I/O worker, as independently identified by its perf stack.
def demands(row,full=False):
 values=[]
 for c in row['runs']:
  a={}
  for side in ['source','target']:
   ts=list(c['roles'][side]['threads'].values())
   io=sum(x['us'] for x in ts if 'bg/IO/' in x['comm'])
   if side=='source':
    caller=sum(x['us'] for x in ts if x['role']=='caller')
    receive=sum(x['us'] for x in ts if x['comm']=='ladder-poll')
    other=sum(x['us'] for x in ts if x['comm']=='bench_cpp_clien' and x['role']!='caller')
    if full:
     receive=max((x['us'] for x in ts if x['comm']=='bench_cpp_clien' and x['role']!='caller'),default=0)
     other-=receive
    a.update(S_call=caller,S_receive=receive,S_other=other,S_io=io)
   else:
    receive=sum(x['us'] for x in ts if x['role']=='receive')
    workers=sum(x['us'] for x in ts if x['role']=='app-worker') if full else sum(x['us'] for x in ts if x['comm']=='bench_cpp_zlink' and x['role']!='receive')
    a.update(T_receive=receive,T_workers=workers,T_io=io,T_other=c['roles'][side]['total_us']-receive-workers-io)
  values.append(a)
 return {k:median(x[k] for x in values) for k in values[0]}
D={n:demands(s[f'rung{n}'],n==9) for n in range(10)}
lines=['### Where each incremental CPU demand appears','', 'Each row is the difference between three-run role medians, in µs/message. The full source receive role is assigned to the hottest unnamed worker by the independent full perf stack; the remaining unnamed source workers are mostly the completion dispatcher. Lower target worker aggregates use their inherited benchmark name. Small control/main-thread CPU remains in the other columns. Rungs 5 and 6 change only the target: changes in their source CPU are effects of timing/batching, not newly added source code. These conditional deltas must not be read as independent function self-costs.','', '| Rung | S caller Δ | S poll/receive Δ | S other workers Δ | S Core I/O Δ | T receive Δ | T app/worker Δ | T Core I/O Δ | T control/other Δ |','|---:|---:|---:|---:|---:|---:|---:|---:|---:|']
keys=['S_call','S_receive','S_other','S_io','T_receive','T_workers','T_io','T_other']
for n in range(1,10):lines.append('| '+str(n)+' | '+' | '.join(f'{D[n][k]-D[n-1][k]:+.3f}' for k in keys)+' |')
lines+=['', 'Absolute per-role medians and every underlying thread remain in `summary.json`/`threads.csv`. Across rungs, different queue occupancy and callback placement can move existing work as well as add it.','']
insert='\n'.join(lines)
t=t.replace('## Largest throughput losses:',insert+'\n## Largest throughput losses:',1)
t=t.replace('Source raw and Framework unlimited request endpoints reproduce','The existing binding benchmark and full Framework unlimited request endpoints reproduce')
if 'Code paths below are relative to' not in t:
 t=t.replace('## Largest throughput losses: functions, required behavior and lower-cost representations\n\n','## Largest throughput losses: functions, required behavior and lower-cost representations\n\nCode paths below are relative to `framework/languages/cpp/framework/`; abbreviated `mesh/`, `backend/` and bare runtime filenames refer to `src/runtime/`. Spec links point to the read-only canonical server documents.\n\n',1)
checks={}
for n in range(10):assert len(s[f'rung{n}']['runs'])==3
assert abs(s['rung0']['throughput']/s['binding-reference-window100']['throughput']-1)<=.05
assert abs(s['binding-reference']['throughput']/246826.1-1)<=.05
assert abs(s['rung9-unbounded']['throughput']/49602-1)<=.05
checks['all_requested_request_rungs_have_three_runs']=True;checks['finite_binding_and_published_unlimited_baselines_match_within_five_percent']=True
checks['valid_cells']=sum(len(x['runs']) for x in s.values());assert checks['valid_cells']==51
checks['profiles']={}
for name,rr in p.items():
 checks['profiles'][name]={}
 for side,r in rr['roles'].items():
  assert r['sample_count']>1000
  assert r['unknown_leaf_count']/r['sample_count']<.03
  expected=s[name]['runs'][0]['roles'][side]['total_us']
  assert math.isclose(sum(r['stage_us'].values()),expected,abs_tol=1e-6)
  stderr=(Path(rr['path'])/'capture'/f'{side}.perf.stderr').read_text()
  assert not re.search(r'\b(?:lost|losing)\b',stderr,re.I)
  checks['profiles'][name][side]={'samples':r['sample_count'],'unknown_leaf_fraction':r['unknown_leaf_count']/r['sample_count'],'cpu_sum_valid':True,'no_lost_sample_warning':True}
m=json.loads((J/'manifest.json').read_text());checks['library_hashes_still_match']={f:hashlib.sha256(Path(f).read_bytes()).hexdigest()==h for f,h in m['libraries'].items()};assert all(checks['library_hashes_still_match'].values())
checks['head']=subprocess.check_output(['git','rev-parse','HEAD'],cwd=R,text=True).strip();assert checks['head']==m['head'];assert checks['head'] in t
checks['branch']=subprocess.check_output(['git','branch','--show-current'],cwd=R,text=True).strip()
checks['status']=subprocess.check_output(['git','status','--short'],cwd=R,text=True);assert not checks['status']
links=re.findall(r'\]\((/[^)]+)\)',t)
for link in links:
 path,sep,line=link.rpartition(':');file=Path(path if sep and line.isdigit() else link)
 assert file.exists(),link
 if sep and line.isdigit():assert 0<int(line)<=len(file.read_text().splitlines()),link
checks['local_links_valid']=len(links)
assert '<!--' not in t and 'TODO' not in t
assert not Path('/root/.debug/root/jobs/fx/cx/v21/s033/cladder').exists()
checks['misplaced_job_perf_cache_removed']=True
(J/'report.md').write_text(t);(J/'final-check.json').write_text(json.dumps(checks,indent=2));print(json.dumps({k:v for k,v in checks.items() if k not in ['profiles','library_hashes_still_match']},indent=2))
