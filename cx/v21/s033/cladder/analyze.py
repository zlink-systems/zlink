from pathlib import Path
from collections import defaultdict
import json,statistics,re,sys
J=Path(__file__).parent
MED=statistics.median
NAMES=['binding only','real envelope create/encode/decode','raw port + Framework task/admission/completion bridge','operation registry + deadline index + callback dispatcher','scheduled owner lane','target mailbox + permit','application executor','typed codec + DI + handler scope/filter invocation','canonical service-wire packing','full public RouteMesh API and runtime']

def analyze(p):
 cell=json.loads((p/'results.json').read_text())['cells'][0]
 assert cell['errors']==0 and cell['abandoned']==0 and not cell.get('contaminated') and not cell.get('drain_bound_hit'),str(p)
 assert cell['header_validation_failures']==0 and cell['completed']==cell['target_stats']['received'] and cell['target_stats']['errors']==0,str(p)
 cap=p/'capture'
 if not (cap/'samples.json').exists():return {'throughput':cell['throughput_per_second'],'cell':cell,'path':str(p)}
 samples=json.loads((cap/'samples.json').read_text());b=samples[0];a=samples[-1];count=(a['stats']['target']['received']-b['stats']['target']['received']) if cell['pattern']=='send-saturation' else (a['stats']['source']['completed']-b['stats']['source']['completed']);elapsed=a['monotonic']-b['monotonic'];assert count>0
 roles={}
 for role in ['source','target']:
  prev=b['processes'][role]['threads'].copy();threads={}
  for snap in samples[1:]:
   for tid,current in snap['processes'][role]['threads'].items():
    allowed=set(range(8)) if role=='source' else set(range(8,16))
    assert set(current['affinity'])<=allowed,(p,role,tid,current['affinity'])
    old=prev.get(tid,{'runtime_ns':0,'voluntary':0,'involuntary':0});t=threads.setdefault(tid,{'comm':current['comm'],'runtime_ns':0,'voluntary':0,'involuntary':0})
    for f in ['runtime_ns','voluntary','involuntary']:t[f]+=max(0,current[f]-old[f])
    prev[tid]=current
  for tid,t in threads.items():t['us']=t['runtime_ns']/1000/count;t['cores']=t['runtime_ns']/1e9/elapsed
  log=(p/(role+'.log')).read_text();marked={tid:label for label,tid in re.findall(r'ROLE ([\w-]+) tid=(\d+)',log)}
  # Raw benchmark phase worker inherits process name; its hot non-I/O thread is the caller.
  if role=='source':
   candidates=[(tid,t) for tid,t in threads.items() if not re.search(r'ZMQ|IO/|zlink-io|ladder-poll|zlink-(offload|complet)',t['comm']) and tid!=str(a['processes'][role]['pid'])]
   if candidates and not any(t['comm']=='ladder-caller' for t in threads.values()) and 'source-submitter' not in marked.values():
    tid,t=max(candidates,key=lambda kv:kv[1]['us']);marked.setdefault(tid,'caller')
  groups=defaultdict(float)
  for tid,t in threads.items():
   name=marked.get(tid,t['comm'])
   if 'source-submitter'==name:name='caller'
   if t['comm']=='ladder-caller':name='caller'
   if role=='target' and t['comm']=='ladder-receive':name='receive'
   t['role']=name;groups[name]+=t['us']
  roles[role]={'total_us':sum(t['us'] for t in threads.values()),'max_us':max(t['us'] for t in threads.values()),'groups':dict(groups),'threads':threads}
 return {'throughput':cell['throughput_per_second'],'window_rate':count/elapsed,'messages':count,'seconds':elapsed,'roles':roles,'path':str(p),'cell':cell}

out={}
for group in sorted((J/'raw').iterdir()):
 cells=[]
 for p in sorted(group.glob('run*/*/results.json')):
  try:cells.append(analyze(p.parent))
  except Exception as e:print('INVALID',p,e,file=sys.stderr);raise
 if not cells:continue
 row={'runs':cells,'throughput':MED(c['throughput'] for c in cells),'groups':{},'endpoint_us':{},'max_us':{}}
 for role in ['source','target']:
  valid=[c for c in cells if 'roles' in c]
  if not valid:continue
  row['endpoint_us'][role]=MED(c['roles'][role]['total_us'] for c in valid)
  row['max_us'][role]=MED(c['roles'][role]['max_us'] for c in valid)
  keys=set().union(*(c['roles'][role]['groups'] for c in valid))
  row['groups'][role]={k:MED(c['roles'][role]['groups'].get(k,0) for c in valid) for k in keys}
 out[group.name]=row
(J/'summary.json').write_text(json.dumps(out,indent=2))
print('label | msg/s | relative to rung0 | source CPU | target CPU | source caller | hottest source / target')
for name,row in out.items():
 ratio=row['throughput']/out['rung0']['throughput']
 print(f"{name} | {row['throughput']:.1f} | {ratio:.4f} | {row['endpoint_us'].get('source',0):.3f} | {row['endpoint_us'].get('target',0):.3f} | {row['groups'].get('source',{}).get('caller',0):.3f} | {row['max_us'].get('source',0):.3f} / {row['max_us'].get('target',0):.3f}")
