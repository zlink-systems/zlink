from pathlib import Path
import json,re,statistics
job=Path('/root/jobs/fx/cx/v21/s033/cthin');root=job.parent/'csubmit';rows=[]
for pattern in ('request-backpressure','send-saturation'):
 for run in (1,2,3):
  for impl in ('zlink-cpp','zlink-framework-cpp'):
   p=root/'measure'/str(run)/pattern/'branch/run1'/f'{impl}-{pattern}-1024'
   samples=json.loads((p/'capture/samples.json').read_text());b,a=samples[0],samples[-1]
   n=(a['stats']['target']['received']-b['stats']['target']['received']) if pattern=='send-saturation' else (a['stats']['source']['completed']-b['stats']['source']['completed'])
   row={'pattern':pattern,'run':run,'implementation':impl,'raw_path':str(p),'messages':n,'seconds':a['time']-b['time'],'throughput':json.loads((p/'results.json').read_text())['cells'][0]['throughput_per_second'],'endpoints':{}}
   for role in ('source','target'):
    log=(p/f'{role}.log').read_text();caller=set(re.findall(r'ROLE source-submitter tid=(\d+)',log));app=set(re.findall(r'ROLE app-worker tid=(\d+)',log));recv=set(re.findall(r'ROLE receive tid=(\d+)',log));last=b['processes'][role]['threads'].copy();t={}
    for sample in samples[1:]:
     for tid,now in sample['processes'][role]['threads'].items():
      prev=last.get(tid,{'runtime_ns':0});v=t.setdefault(tid,{'cpu_ns':0,'comm':now['comm']});v['cpu_ns']+=max(0,now['runtime_ns']-prev['runtime_ns']);last[tid]=now
    for tid,v in t.items():
     v['cpu_us']=v['cpu_ns']/1000/n;v['cores']=v['cpu_ns']/1e9/row['seconds'];v['role']='Core I/O' if v['comm'].startswith('ZLINKbg/IO/') else 'caller' if tid in caller else 'application worker' if tid in app else 'receive' if tid in recv else 'other'
    grouped={}
    for tid,v in t.items():grouped[v['role']]=grouped.get(v['role'],0)+v['cpu_us']
    row['endpoints'][role]={'cpu_us':sum(v['cpu_us'] for v in t.values()),'groups_us':grouped,'threads':t,'hottest':max(t,key=lambda x:t[x]['cpu_ns'])}
   rows.append(row)
summary=[]
for pattern in ('request-backpressure','send-saturation'):
 for impl in ('zlink-cpp','zlink-framework-cpp'):
  r=[v for v in rows if v['pattern']==pattern and v['implementation']==impl]
  g={'pattern':pattern,'implementation':impl,'rate_median':statistics.median(v['throughput'] for v in r),'endpoints':{}}
  for role in ('source','target'):
   g['endpoints'][role]={'cpu_us':statistics.median(v['endpoints'][role]['cpu_us'] for v in r),'groups_us':{k:statistics.median(v['endpoints'][role]['groups_us'].get(k,0) for v in r) for k in ('caller','Core I/O','application worker','receive','other')},'hottest_cpu_us':statistics.median(v['endpoints'][role]['threads'][v['endpoints'][role]['hottest']]['cpu_us'] for v in r)}
  summary.append(g)
(job/'prior-unprofiled.json').write_text(json.dumps({'records':rows,'summary':summary},indent=2));print(json.dumps(summary,indent=2))
