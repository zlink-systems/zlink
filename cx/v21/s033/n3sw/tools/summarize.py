#!/usr/bin/env python3
import collections, json, pathlib, sys
job=pathlib.Path('/root/jobs/fx/cx/v21/s033/n3sw')
rows=[]
for label in sys.argv[1:]:
    directory=job/'raw'/label
    events=json.loads((directory/'events.json').read_text())
    start=next(e['monotonic_us'] for e in events if e['event']=='active-start')
    end=next(e['monotonic_us'] for e in events if e['event']=='active-end')
    for role in ['source','target']:
        p=json.loads((directory/f'{role}.cpuprofile').read_text())
        nodes={n['id']:n for n in p['nodes']}
        parent={c:n['id'] for n in p['nodes'] for c in n.get('children',[])}
        def name(n):
            f=n['callFrame']; return f"{f['functionName'] or '(anonymous)'} {f['url']}:{f['lineNumber']+1}"
        self_counts=collections.Counter(); inclusive=collections.Counter(); self_us=collections.Counter()
        categories=collections.Counter(); total=0; timestamp=p['startTime']
        for node,delta in zip(p['samples'],p['timeDeltas']):
            timestamp+=delta
            if not start<=timestamp<=end: continue
            total+=1; self_counts[name(nodes[node])]+=1; self_us[name(nodes[node])]+=delta
            stack=[]; cur=node
            while cur in nodes:
                stack.append(name(nodes[cur])); cur=parent.get(cur)
            for function in set(stack): inclusive[function]+=1
            for category,term in [('service_wire','service-wire-m6a-codec'),('channel_envelope','channel-envelope'),
                ('slab','payload-slab'),('binding','node_modules/@zlink-systems/zlink/'),('protobuf','protobufjs'),
                ('gc','(garbage collector)'),('idle','(idle)'),('poll',' poll ' )]:
                if any(term in (' '+f+' ') for f in stack): categories[category]+=1
        h=json.loads((directory/f'{role}.heapprofile').read_text())
        allocation=collections.Counter()
        def walk(n):
            allocation[name(n)]+=n.get('selfSize',0)
            for c in n.get('children',[]): walk(c)
        walk(h['head'])
        result=dict(label=label,role=role,active_samples=total,active_window_us=[start,end],profile_window_us=[p['startTime'],p['endTime']],
            categories={k:dict(samples=v,percent=round(100*v/max(1,total),2)) for k,v in categories.items()},
            self_samples=self_counts.most_common(35),inclusive_samples=inclusive.most_common(45),
            service_inclusive=[(k,v) for k,v in inclusive.most_common() if 'service-wire-m6a-codec' in k],
            self_elapsed_us=self_us.most_common(25),allocations=allocation.most_common(35),
            service_allocations=[(k,v) for k,v in allocation.most_common() if 'service-wire-m6a-codec' in k])
        (directory/f'{role}-summary.json').write_text(json.dumps(result,indent=2)+'\n')
        rows.append({k:result[k] for k in ['label','role','active_samples','categories','service_inclusive','service_allocations']})
print(json.dumps(rows,indent=2))
