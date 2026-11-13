#!/usr/bin/env python3
import json, pathlib, re

job = pathlib.Path('/root/jobs/fx/cx/v21/s033/lad-node-m2')
root = job / 'raw/profile/4351'
results_root = job / 'raw/profile-bench/4351'

def target_profile(path):
    data = json.loads(path.read_text())
    nodes = {n['id']: n for n in data.get('nodes', [])}
    if not any('zlink-framework-server/main.js' in n.get('callFrame', {}).get('url', '') for n in nodes.values()):
        return None
    parents = {n['id']: n.get('parent') for n in nodes.values()}
    totals = {}
    categories = {'busy': 0, 'recv': 0, 'program': 0, 'gc': 0, 'permit': 0}
    for sid, delta in zip(data.get('samples', []), data.get('timeDeltas', [])):
        delta = int(delta)
        categories['busy'] += delta
        stack=[]; cur=sid
        while cur in nodes:
            n=nodes[cur]; stack.append(n.get('callFrame', {})); cur=parents.get(cur)
        text=' '.join(f"{f.get('functionName','')} {f.get('url','')}" for f in stack).lower()
        if 'socket_operations.js' in text or re.search(r'\brecv\b', text): categories['recv'] += delta
        if '(program)' in text: categories['program'] += delta
        if re.search(r'\b(gc|minor gc|major gc|scavenger)\b|v8::internal::heap', text): categories['gc'] += delta
        if 'application-job-queue.js' in text or 'applicationjobqueue.acquire' in text or 'releasebeforehandler' in text or 'releaseunqueued' in text:
            categories['permit'] += delta
        leaf=nodes[sid].get('callFrame',{})
        url=leaf.get('url','')
        if 'application-job-queue.js' in url:
            key=(leaf.get('functionName',''),url,leaf.get('lineNumber',-1)+1)
            totals[key]=totals.get(key,0)+delta
    result_file=results_root / path.relative_to(root).parent / 'run1' / 'zlink-framework-node-request-serial-PLACEHOLDER/results.json'
    parts=path.relative_to(root).parts
    reason,variant=parts[0],parts[1]
    pattern='request-serial'; payload=int(reason.rsplit('-',1)[1])
    result_file=results_root/reason/variant/'run1'/f'zlink-framework-node-{pattern}-{payload}'/'results.json'
    c=json.loads(result_file.read_text())['cells'][0]
    count=c['target_stats']['received']
    per_msg={k:v/1000/count for k,v in categories.items()}
    leaves=[{'function':f,'url':u,'line':line,'ms_msg':v/1000/count} for (f,u,line),v in sorted(totals.items(), key=lambda x:x[1], reverse=True)]
    return {'reason':reason,'variant':variant,'profile':path.relative_to(job).as_posix(),'result':result_file.relative_to(job).as_posix(),
            'received':count,'per_message_ms':per_msg,'queue_leaf_frames':leaves}

profiles=[]
for path in sorted(root.glob('*/*/*.cpuprofile')):
    item=target_profile(path)
    if item: profiles.append(item)
out=job/'raw/aggregates/profile-4351.json'
out.write_text(json.dumps(profiles,indent=2)+'\n')
for p in profiles:
    print(p['reason'],p['variant'],p['received'],p['per_message_ms'])
    print('  permit frames:',p['queue_leaf_frames'][:8])
print(out)
