import datetime,hashlib,json,pathlib,subprocess
J=pathlib.Path('/root/jobs/fx/cx/v21/s033/l3868-s1'); R=pathlib.Path('/root/worktree/zlink-v21-s033/l3868-next')
S=json.loads((J/'raw/summary.json').read_text()); C=json.loads((J/'raw/comparisons.json').read_text()); runs=[json.loads(s) for s in (J/'raw/runs.jsonl').read_text().splitlines()]
assert len(S)==42 and len(C)==36
assert all(x['n'] in [1,3] for x in S.values())
for c in C:
    if c['boundary']: assert c['n_previous']==c['n_current']==3,c
    if c['g5_previous']>.05 and c['n_previous']==3: assert c['status']=='판정 불가'
    if c['g5_current']>.05 and c['n_current']==3: assert c['status']=='판정 불가'
last=None; checked=0
for r in runs:
    key=r['hold'],r['level'],r['pattern'],r['size']
    if r['kind']=='warm': last=key; continue
    assert last==key,('missing warm cell',r)
    result=json.loads(pathlib.Path(r['raw']).read_text()); c=result['cells'][0]; text=pathlib.Path(r['scheduling']).read_text()
    assert c['submitted']==c['completed']==c['target_stats']['received']
    assert c['errors']==c['target_stats']['errors']==0
    assert not c.get('contaminated')
    for label,cpus in [('source_start','0-5'),('target_start','8-13'),('mid_active_source','0-5'),('mid_active_target','8-13')]:
        tids=[x for x in text.splitlines() if x.startswith(label+'_tid=')]
        affin=[x for x in text.splitlines() if x.startswith(label+'_thread_affinity ')]
        assert tids and len(tids)==len(affin) and all(x.endswith('cpus='+cpus) for x in affin)
    policies=[x for x in text.splitlines() if 'current scheduling policy:' in x]
    assert policies and all(x.endswith('SCHED_OTHER') for x in policies) and 'scheduling_invalid=' not in text
    checked+=1
holds=[]
for p in (J/'raw/holds').glob('*/duration.json'):
    d=json.loads(p.read_text()); assert d['seconds']<1800; holds.append(d['seconds'])
    for name in ['before','after']:
        e=json.loads(p.with_name(name+'.json').read_text()); assert set(e['cstates'].values())=={'1'} and set(e['governor'].values())=={'performance'}
assert subprocess.check_output(['git','branch','--show-current'],cwd=R,text=True).strip()=='exp/3868-cpp-ladder-next'
assert subprocess.check_output(['git','rev-parse','HEAD'],cwd=R,text=True).strip()=='62e766f88dc6d1feefdff76e6132185019db0bb6'
assert not subprocess.check_output(['git','status','--porcelain'],cwd=R,text=True).strip()
fresh=json.loads((J/'raw/freshness.json').read_text())
for a in fresh['artifacts']:
    p=pathlib.Path(a['binary']); assert p.exists() and hashlib.sha256(p.read_bytes()).hexdigest()==a['sha256']
base=json.loads(pathlib.Path('/root/jobs/fx/cx/v21/s033/binding-daily/cpp-2026-11-13b.json').read_text()); assert len(base['cells'])==6
out={'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'summary_cells':len(S),'comparisons':len(C),'counted_runs':checked,'hold_seconds':holds,'artifact_count':len(fresh['artifacts']),'git_clean':True,'all_checks_passed':True}
(J/'raw/final-audit.json').write_text(json.dumps(out,indent=2)+'\n'); print(json.dumps(out,indent=2))
