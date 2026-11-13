#!/usr/bin/env python3
import csv,json,pathlib,re,statistics
root=pathlib.Path('/root/jobs/fx/cx/v21/s033/bstage/measure');rows=[]
for capture in sorted(root.glob('*/runner/run1/*/capture')):
    cell=capture.parent; label=capture.parents[3].name
    win=json.loads((capture/'windows.json').read_text()); n=win['A_messages']
    before={};after={}
    for role in ('target','source'):
        before[role]={};after[role]={}
        for name,dest in (('before',before),('after',after)):
            for line in (capture/f'A-{role}-{name}.tsv').read_text().splitlines():
                tid,comm,ns,vol,inv=line.split('\t')
                dest[role][tid]={'comm':comm,'runtime_ns':int(ns),'voluntary':int(vol),'involuntary':int(inv)}
    perf={}
    for row in csv.reader((capture/'A-perfstat.csv').read_text().splitlines()):
        if len(row)<4 or not row[0] or row[0].startswith('#'): continue
        try: val=float(row[1])
        except ValueError: continue
        perf.setdefault(row[0].rsplit('-',1)[-1],{})[row[3]]=val
    threads={};wakes={}
    for role in ('target','source'):
        wakes[role]=sum(max(0,v['voluntary']-before[role].get(tid,{}).get('voluntary',0)) for tid,v in after[role].items())/n
        for tid,b in before[role].items():
            a=after[role].get(tid)
            if not a: continue
            c=perf.get(tid,{});ins=c.get('instructions',0);cycles=c.get('cycles',0);mp=c.get('msr/mperf/',0)
            threads[tid]={'role':role,'comm':b['comm'],'us_per_msg':max(0,a['runtime_ns']-b['runtime_ns'])/1000/n,
                'effective_ghz':c.get('msr/aperf/',0)/mp*2 if mp else None,
                'sendto_per_msg':c.get('syscalls:sys_enter_sendto',0)/n,
                'instructions_per_msg':ins/n,'ipc':ins/cycles if cycles else None}
    roles={}
    for kind,tid in re.findall(r'ROLE (app-worker|receive) tid=(\d+)',(cell/'target.log').read_text(errors='replace')):
        roles.setdefault(kind,[]).append(tid)
    role_time={kind:sum(threads[t]['us_per_msg'] for t in set(tids) if t in threads) for kind,tids in roles.items()}
    io={}
    for role in ('target','source'):
        its=[(tid,v) for tid,v in threads.items() if v['role']==role and '/IO/' in v['comm']]
        tid,v=max(its,key=lambda x:x[1]['sendto_per_msg']) if its else (None,{})
        io[role]={'tid':tid,'sendto_per_msg':v.get('sendto_per_msg'),'effective_ghz':v.get('effective_ghz')}
    rows.append({'run':label,'messages':n,'rate':win['A_rate'],'app_worker_us_per_msg':role_time.get('app-worker'),
        'receive_us_per_msg':role_time.get('receive'),'target_role_sum_us_per_msg':sum(role_time.values()),
        'target_process_all_threads_us_per_msg':sum(v['us_per_msg'] for v in threads.values() if v['role']=='target'),
        'roles':roles,'role_time':role_time,'wakes_per_message':wakes,'io':io,'threads':threads,'raw':str(capture)})
out=pathlib.Path('/root/jobs/fx/cx/v21/s033/bstage/summary-22k.json');out.write_text(json.dumps(rows,indent=2)+'\n');print(out)
for r in rows:
    print(r['run'], 'rate',round(r['rate']), 'app',r['app_worker_us_per_msg'],'recv',r['receive_us_per_msg'],'role_sum',r['target_role_sum_us_per_msg'],'proc_all',r['target_process_all_threads_us_per_msg'],'wakes',r['wakes_per_message'],'io',r['io'])
