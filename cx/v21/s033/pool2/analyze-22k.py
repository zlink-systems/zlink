#!/usr/bin/env python3
"""Extract window-A per-thread and role metrics from one round's 22k captures."""
import csv,json,pathlib,re,sys
root=pathlib.Path(sys.argv[1]); rows=[]
for capture in sorted(root.glob('*/runner/run1/*/capture')):
    cell=capture.parent
    implementation=cell.name.split('-request-backpressure-')[0]
    variant=capture.parents[3].name.split('-',1)[0]
    round_no=int(capture.parents[5].name)
    win=json.loads((capture/'windows.json').read_text()); n=win['A_messages']
    before={};after={}
    for role in ('target','source'):
        before[role]={}
        after[role]={}
        for label, dest in (('before',before),('after',after)):
            for line in (capture/f'A-{role}-{label}.tsv').read_text().splitlines():
                tid,comm,ns,vol,inv=line.split('\t')
                dest[role][tid]={'comm':comm,'runtime_ns':int(ns),'voluntary':int(vol),'involuntary':int(inv)}
    perf={}
    for r in csv.reader((capture/'A-perfstat.csv').read_text().splitlines()):
        if len(r)<4 or not r[0] or r[0].startswith('#'): continue
        try: val=float(r[1])
        except ValueError: continue
        tid=r[0].rsplit('-',1)[-1]
        perf.setdefault(tid,{})[r[3]]=val
    threads={}
    wakes={}
    only_before={}
    for role in ('target','source'):
        only_before[role]=len(set(before[role])-set(after[role]))
        wakes[role]=sum(max(0,after[role].get(tid,{}).get('voluntary',0)-before[role].get(tid,{}).get('voluntary',0)) for tid in set(before[role])|set(after[role]))/n
        for tid in set(before[role])|set(after[role]):
            b=before[role].get(tid,{'comm':after[role][tid]['comm'],'runtime_ns':0,'voluntary':0,'involuntary':0})
            a=after[role].get(tid)
            if not a: continue
            counters=perf.get(tid,{})
            instructions=counters.get('instructions',0); cycles=counters.get('cycles',0)
            ap=counters.get('msr/aperf/',0); mp=counters.get('msr/mperf/',0)
            threads[tid]={'role':role,'comm':b['comm'],'us_per_msg':max(0,a['runtime_ns']-b['runtime_ns'])/1000/n,
              'instructions_per_msg':instructions/n,'ipc':instructions/cycles if cycles else None,
              'effective_ghz':ap/mp*2.0 if mp else None,'sendto_per_msg':counters.get('syscalls:sys_enter_sendto',0)/n}
    io={}
    for role in ('target','source'):
        io_threads=[(tid,v) for tid,v in threads.items() if v['role']==role and '/IO/' in v['comm']]
        if not io_threads: io[role]=None; continue
        tid,v=max(io_threads,key=lambda item:item[1]['sendto_per_msg'])
        io[role]={'tid':tid,**v}
    target_roles={}
    if implementation=='zlink-framework-cpp':
        for kind,tid in re.findall(r'ROLE (app-worker|receive) tid=(\d+)',(cell/'target.log').read_text(errors='replace')):
            target_roles.setdefault(kind,[]).append(tid)
        for kind,tids in target_roles.items():
            target_roles[kind]=sum(threads[t]['us_per_msg'] for t in set(tids) if t in threads)
    rows.append({'round':round_no,'variant':variant,'implementation':implementation,'messages':n,'rate':win['A_rate'],'only_before_tids':only_before,
      'io':io,'wakes_per_message':wakes,'threads':threads,'framework_target_role_us_per_message':target_roles,
      'raw':str(capture)})
out=root/'summary-22k.json';out.write_text(json.dumps(rows,indent=2)+'\n');print(out)
