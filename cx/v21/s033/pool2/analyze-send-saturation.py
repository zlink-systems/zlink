#!/usr/bin/env python3
"""Summarize raw send-saturation window snapshots using the union of TIDs."""
import csv,json,pathlib,sys
root=pathlib.Path(sys.argv[1]); rows=[]
def read_tsv(p):
    d={}
    for line in p.read_text().splitlines():
        tid,comm,ns,vol,inv=line.split('\t')
        d[tid]={'comm':comm,'runtime_ns':int(ns),'voluntary':int(vol),'involuntary':int(inv)}
    return d
def percentile(hist,pct):
    total=sum(hist.values())
    if total==0: return None
    threshold=max(1,int((total*pct+99)//100)); cum=0
    for depth,count in sorted(hist.items()):
        cum+=count
        if cum>=threshold: return depth
    return 0
for capture in sorted(root.glob('*/**/capture')):
    win=json.loads((capture/'windows.json').read_text()); n=win['A_messages']; seconds=win['A_seconds']
    perf={}
    for r in csv.reader((capture/'A-perfstat.csv').read_text().splitlines()):
        if len(r)<4 or not r[0] or r[0].startswith('#'): continue
        try: val=float(r[1])
        except ValueError: continue
        tid=r[0].rsplit('-',1)[-1]; perf.setdefault(tid,{})[r[3]]=val
    metrics={}; only_before={}
    for role in ('source','target'):
        before=read_tsv(capture/f'A-{role}-before.tsv'); after=read_tsv(capture/f'A-{role}-after.tsv')
        union=set(before)|set(after); only_before[role]=len(set(before)-set(after))
        rows_thread=[]; cpu_ns=0; voluntary=0
        for tid in union:
            b=before.get(tid,{'comm':after.get(tid,{}).get('comm','?'),'runtime_ns':0,'voluntary':0})
            a=after.get(tid)
            if not a: continue
            ns=max(0,a['runtime_ns']-b['runtime_ns']); wakes=max(0,a['voluntary']-b['voluntary'])
            c=perf.get(tid,{}); ins=c.get('instructions',0); cycles=c.get('cycles',0); ap=c.get('msr/aperf/',0); mp=c.get('msr/mperf/',0)
            row={'tid':tid,'comm':a['comm'],'cpu_percent':ns/(seconds*1e7),'us_per_msg':ns/1000/n,'wakes_per_msg':wakes/n,
                 'effective_ghz':ap/mp*2.0 if mp else None,'sendto_per_msg':c.get('syscalls:sys_enter_sendto',0)/n,
                 'instructions_per_msg':ins/n,'ipc':ins/cycles if cycles else None}
            rows_thread.append(row); cpu_ns+=ns; voluntary+=wakes
        metrics[role]={'process_us_per_msg':cpu_ns/1000/n,'process_wakes_per_msg':voluntary/n,
          'completion_threads':[t for t in rows_thread if t['comm'].startswith('zl-cmpl-')],
          'io_threads':[t for t in rows_thread if '/IO/' in t['comm']]}
    queues={}
    for role in ('source','target'):
        p=capture.parents[3]/'queue'/f'{role}-queue.tsv'
        if not p.exists(): queues[role]=None; continue
        hist={}; maximum=0
        for line in p.read_text().splitlines():
            first,second=line.split('\t')
            if first=='depth': continue
            if first=='max': maximum=int(second); continue
            hist[int(first)]=int(second)
        queues[role]={'p50':percentile(hist,50),'p99':percentile(hist,99),'max':maximum,'observations':sum(hist.values()),'histogram':hist}
    cell=capture.parent
    variant=cell.parents[2].name
    round_no=int(cell.parents[3].name)
    results=json.loads((cell/'results.json').read_text())
    rows.append({'round':round_no,'variant':variant,'msg_per_s':(results.get('cells') or [{}])[0].get('throughput_per_second'),
                 'window_msg_per_s':win['A_rate'],'messages':n,'seconds':seconds,'only_before_tids':only_before,
                 'roles':metrics,'queue_depth':queues,'raw':str(capture)})
out=root/'summary-send-saturation.json';out.write_text(json.dumps(rows,indent=2)+'\n');print(out)
