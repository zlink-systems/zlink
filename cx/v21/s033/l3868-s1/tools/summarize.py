import collections, datetime, json, pathlib, statistics, sys
J=pathlib.Path('/root/jobs/fx/cx/v21/s033/l3868-s1'); runs=[json.loads(s) for s in (J/'raw/runs.jsonl').read_text().splitlines()] if (J/'raw/runs.jsonl').exists() else []
cells=[(p,s) for p in ['send-saturation','request-serial','request-backpressure'] for s in [1024,4096]]; levels=['binding','0','1','2','3','4','7']
groups=collections.defaultdict(list)
for r in runs:
    if r['kind'].startswith('counted'): groups[r['level'],r['pattern'],r['size']].append(r)
summary={}; bykey={}
for key,items in groups.items():
    rates=[i['rate'] for i in items]; median=statistics.median(rates); chosen=min(items,key=lambda i:abs(i['rate']-median))
    v={'rate':median,'n':len(items),'g5':max(abs(x-median) for x in rates)/median,'raw':chosen['raw'],'raw_paths':[i['raw'] for i in items],'scheduling':[i['scheduling'] for i in items]}
    summary['|'.join(map(str,key))]=v; bykey[key]=v
(J/'raw/summary.json').write_text(json.dumps(summary,indent=2)+'\n')
comparisons=[]; repeats={}
for prev,cur in zip(levels,levels[1:]):
    for p,s in cells:
        a=bykey.get((prev,p,s)); b=bykey.get((cur,p,s))
        if not a or not b: continue
        loss=1-b['rate']/a['rate']; limit=.05 if cur=='0' else .21 if cur=='7' else .07
        badg5=any(x['n'] not in (1,3) or (x['n']>=3 and x['g5']>.05) for x in [a,b])
        boundary=cur!='0' and ((.19<=loss<=.23) if cur=='7' else (.05<=loss<=.09))
        status='판정 불가' if badg5 or (boundary and any(x['n']<3 for x in [a,b])) else '통과' if loss<=limit else '실패'
        comparisons.append(dict(previous=prev,current=cur,pattern=p,size=s,loss=loss,budget=limit,status=status,previous_raw=a['raw_paths'],current_raw=b['raw_paths'],g5_previous=a['g5'],g5_current=b['g5'],n_previous=a['n'],n_current=b['n'],boundary=boundary))
        if boundary:
            for lev,x in [(prev,a),(cur,b)]:
                if x['n']<3: repeats[lev,p,s]=3-x['n']
(J/'raw/comparisons.json').write_text(json.dumps(comparisons,ensure_ascii=False,indent=2)+'\n')
if '--repeat-plan' in sys.argv:
    path=pathlib.Path(sys.argv[sys.argv.index('--repeat-plan')+1]); path.write_text(json.dumps([[a,b,c,n] for (a,b,c),n in repeats.items()],indent=2)+'\n')
print(json.dumps({'counted':len([r for r in runs if r['kind'].startswith('counted')]),'cells':len(summary),'comparisons':comparisons,'repeat_cells':len(repeats)},ensure_ascii=False,indent=2))
if '--binding' in sys.argv:
    assert all(('binding',p,s) in bykey for p,s in cells)
    dest=pathlib.Path('/root/jobs/fx/cx/v21/s033/binding-daily/cpp-2026-11-13b.json')
    data={'language':'cpp','utc_date':'2026-11-13','measured_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'conditions':'C2·C3 off, SCHED_OTHER, source pin 0-5 / target pin 8-13, guard v8','warmup_seconds':3,'active_seconds':5,'discarded_warm_run_per_cell_change':True,'worktree':'/root/worktree/zlink-v21-s033/l3868-next','commit':'62e766f88dc6d1feefdff76e6132185019db0bb6','affinity_verified':True,'scheduling_verified':True,'status':'valid','validity_note':'All source/target threads verified SCHED_OTHER and pinned 0-5 / 8-13 at start and mid-active; discarded warm run per cell change.','cells':{f'{p}-{s}':{'msg_s':bykey['binding',p,s]['rate'],'raw':bykey['binding',p,s]['raw'],'scheduling':bykey['binding',p,s]['scheduling'],'counted_runs':bykey['binding',p,s]['n']} for p,s in cells}}
    with dest.open('x') as f: json.dump(data,f,ensure_ascii=False,indent=2); f.write('\n')
