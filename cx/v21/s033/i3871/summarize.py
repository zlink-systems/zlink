#!/usr/bin/env python3
import json,pathlib
root=pathlib.Path('/root/jobs/fx/cx/v21/s033/i3871/raw')
for result in sorted(root.glob('round*/*/results.json')):
    cell=result.parent; cell_data=json.loads(result.read_text())['cells'][0]
    before=json.loads((cell/'before.json').read_text()); after=json.loads((cell/'after.json').read_text())
    rows=[]; n=int(cell_data['completed'])
    for role,root_index in (('source',0),('target',1)):
        b={int(tid):v for tid,v in before['threads'].items() if v['root_index']==root_index}
        a={int(tid):v for tid,v in after['threads'].items() if v['root_index']==root_index}
        union=set(b)|set(a); alive_union=0; cpu=vol=inv=0; per=[]
        for tid in union:
            av=a.get(tid)
            if av is None: continue
            bv=b.get(tid,{'runtime_ns':0,'voluntary_ctxt_switches':0,'nonvoluntary_ctxt_switches':0,'comm':av['comm']})
            alive_union+=1
            runtime=max(0,av['runtime_ns']-bv['runtime_ns'])
            voluntary=max(0,av.get('voluntary_ctxt_switches',0)-bv.get('voluntary_ctxt_switches',0))
            involuntary=max(0,av.get('nonvoluntary_ctxt_switches',0)-bv.get('nonvoluntary_ctxt_switches',0))
            cpu+=runtime; vol+=voluntary; inv+=involuntary
            per.append({'tid':tid,'comm':av['comm'],'cpu_us_per_req':runtime/1000/n,'voluntary_cs_per_req':voluntary/n,'involuntary_cs_per_req':involuntary/n,'cpus_allowed_list':av.get('cpus_allowed_list')})
        rows.append({'role':role,'root_pid':before['roots'][root_index],'tid_union':len(union),'threads_alive_after':alive_union,'only_before_tids':len(set(b)-set(a)),'cpu_us_per_req':cpu/1000/n,'context_switches_per_req':(vol+inv)/n,'voluntary_cs_per_req':vol/n,'involuntary_cs_per_req':inv/n,'threads':sorted(per,key=lambda x:x['cpu_us_per_req'],reverse=True)})
    summary={'round':cell.parent.name,'cell':cell.name,'throughput_per_second':cell_data['throughput_per_second'],'latency_mean_ms':cell_data['latency_mean_ms'],'latency_p50_ms':cell_data.get('latency_p50_ms'),'latency_p99_ms':cell_data['latency_p99_ms'],'target_stats':json.loads((cell/'target-stats.json').read_text()),'completed':n,'snapshot_before_ns':before['monotonic_ns'],'snapshot_after_ns':after['monotonic_ns'],'snapshot_elapsed_ns':after['monotonic_ns']-before['monotonic_ns'],'processes':rows}
    print(json.dumps(summary,ensure_ascii=False))
