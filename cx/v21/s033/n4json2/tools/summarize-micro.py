import pathlib,json,collections
job=pathlib.Path('/root/jobs/fx/cx/v21/s033/n4json2')
groups={'codec':{'stringifyFrameworkJsonV1','schemaWriter','parseFrameworkJsonV1'},'toJSON':{'toJSON'},'prepare':{'prepareNodeJsonBytes'},'encode':{'encodeNodeBase64','encodeBase64','base64Slice','btoa'},'decode':{'decodeNodeBase64','decodeBase64','base64Write','atob'},'atob':{'atob'},'decodeBase64':{'decodeBase64'},'decodeNodeBase64':{'decodeNodeBase64'},'gc':{'(garbage collector)'}}
rows=[]
for d in sorted(job.joinpath('raw').iterdir()):
 if not d.is_dir() or not (d.name.startswith('candidate-final-') or d.name.startswith('decode-')):continue
 r=json.loads((d/'candidate-result.json').read_text());r['directory']=d.name;r['normalizationMessages']=r['totalMessages']+1
 data=json.loads(next(d.glob('*.cpuprofile')).read_text());nodes={n['id']:n for n in data['nodes']};parents={c:n['id'] for n in data['nodes'] for c in n.get('children',[])};counts=collections.Counter()
 for n in data.get('samples',[]):
  stack=[];index=n
  while index in nodes:
   stack.append(nodes[index]['callFrame']['functionName'])
   if index not in parents:break
   index=parents[index]
  for group,names in groups.items():
   if any(x in stack for x in names):counts[group]+=1
 r['totalSamples']=len(data.get('samples',[]));r['cpu_subtree_pct']={k:round(100*counts[k]/r['totalSamples'],2) for k in groups}
 alloc=json.loads(next(p for p in d.glob('alloc-*.json') if p.name[6].isdigit()).read_text());sizes=collections.Counter()
 def walk(n,stack):
  stack=stack+[n['callFrame']['functionName']];size=n.get('selfSize',0);sizes['all']+=size
  for group,names in groups.items():
   if any(x in stack for x in names):sizes[group]+=size
  for c in n.get('children',[]):walk(c,stack)
 walk(alloc['head'],[])
 r['allocation_bytes_per_message']={k:round(sizes[k]/r['normalizationMessages'],2) for k in ['all',*groups]}
 r['lifetime']=json.loads((d/'lifetime.json').read_text());assert r['lifetime']['wall_seconds']<20;assert r['lifetime']['exit_code']==0
 rows.append(r)
(job/'raw/micro-summary.json').write_text(json.dumps(rows,indent=2))
for r in rows:print(r['directory'],'CPU',r['cpu_subtree_pct'],'B/msg',r['allocation_bytes_per_message'])
