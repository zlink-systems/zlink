import json, pathlib, re
out=pathlib.Path('/root/jobs/fx/cx/v21/s033/i3560')
s=json.loads((out/'capture-summary.json').read_text())
rows=[]
for rec in s['captures']:
    d=pathlib.Path(rec['raw']); impl=rec['implementation']; src=rec['source_pid']; tgt=rec['target_pid']; den=rec['source_completed_delta']
    cpu={}
    for role in ('source','target'):
        entries=json.loads((d/f'{role}-io-cpu.json').read_text())
        cpu[role]={}
        for tid,v in entries.items():
            m=re.search(r'/IO/(\d+)',v['comm'])
            if m: cpu[role].setdefault('IO/'+m.group(1),[]).append({'tid':int(tid),'us':v['cpu_us'],'comm':v['comm']})
    txt=(d/'wake-counts.txt').read_text()
    maps={}
    for kind,key in [('send',r'@send\[(\d+),\s*(\d+)\]:\s*(\d+)'),('read',r'@signal_read\[(\d+),\s*(\d+)\]:\s*(\d+)'),('write',r'@signal_write\[(\d+),\s*(\d+)\]:\s*(\d+)'),('reqok',r'@request_ok\[(\d+)\]:\s*(\d+)')]:
        data={}
        for m in re.finditer(key,txt):
            if kind=='reqok':
                tid,n=map(int,m.groups()); data[tid]=n
            else:
                pid,tid,n=map(int,m.groups()); data[(pid,tid)]=n
        maps[kind]=data
    req=sum(maps['reqok'].values())
    wake={}
    for kind in ('send','read','write'):
        wake[kind]=sum(n for (pid,tid),n in maps[kind].items() if pid==src)/req if req else None
    rows.append({'implementation':impl,'pid_source':src,'pid_target':tgt,'completed_delta':den,'source_api_success_bpf':req,'bpf_source_req_per_sec':req/2,
                 'wake_per_request':wake,'cpu_per_request_us':{role:{k:sum(x['us'] for x in vals)/den for k,vals in groups.items()} for role,groups in cpu.items()},
                 'cpu_threads':cpu,'raw':str(d)})
# Aggregate CPU by role/index weighted over each implementation's two completed windows.
agg={}
for impl in sorted({r['implementation'] for r in rows}):
    chosen=[r for r in rows if r['implementation']==impl]
    totalden=sum(r['completed_delta'] for r in chosen)
    agg[impl]={}
    for role in ('source','target'):
        n={}
        for r in chosen:
            for idx, vals in r['cpu_threads'][role].items(): n[idx]=n.get(idx,0)+sum(x['us'] for x in vals)
        agg[impl][role]={idx:us/totalden for idx,us in sorted(n.items())}
summary={'runner_exit':s['runner_exit'],'captures':rows,'aggregate_io_cpu_us_per_request':agg}
(out/'measurement-summary.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps({'aggregates':agg,'captures':[{'implementation':r['implementation'],'cpu':r['cpu_per_request_us'],'wake':r['wake_per_request'],'req_bpf':r['source_api_success_bpf']} for r in rows]},indent=2))
