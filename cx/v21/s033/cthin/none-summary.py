from pathlib import Path
import json,re
job=Path('/root/jobs/fx/cx/v21/s033/cthin');rows={}
for cell in sorted((job/'none/run1').iterdir()):
 cap=cell/'capture';ss=json.loads((cap/'samples.json').read_text());b,a=ss[0],ss[-1];secs=a['monotonic']-b['monotonic'];n=(a['stats']['target']['received']-b['stats']['target']['received']) if 'send-saturation' in cell.name else a['stats']['source']['completed']-b['stats']['source']['completed']
 r={'directory':str(cell),'messages':n,'seconds':secs,'capture_rate':n/secs,'result':json.loads((cell/'results.json').read_text())['cells'][0],'roles':{}}
 c=r['result'];assert c['errors']==c['abandoned']==c['header_validation_failures']==0 and c['completed']==c['target_stats']['received'] and c['target_stats']['errors']==0
 for role in ('source','target'):
  prev=b['processes'][role]['threads'].copy();tt={};log=(cell/f'{role}.log').read_text();marks=set(re.findall(r'ROLE source-submitter tid=(\d+)',log));app=set(re.findall(r'ROLE app-worker tid=(\d+)',log));recv=set(re.findall(r'ROLE receive tid=(\d+)',log))
  for snap in ss[1:]:
   for tid,v in snap['processes'][role]['threads'].items():
    old=prev.get(tid,dict(runtime_ns=0,voluntary=0,involuntary=0));t=tt.setdefault(tid,{'comm':v['comm'],'runtime_ns':0,'switches':0})
    t['runtime_ns']+=max(0,v['runtime_ns']-old['runtime_ns']);t['switches']+=sum(max(0,v[k]-old[k]) for k in ('voluntary','involuntary'));prev[tid]=v
  for tid,t in tt.items():
   t['cpu_us']=t['runtime_ns']/1000/n;t['cores']=t['runtime_ns']/1e9/secs;t['role']='Core I/O' if t['comm'].startswith('ZLINKbg/IO/') else 'caller' if tid in marks else 'application worker' if tid in app else 'receive' if tid in recv else 'auxiliary'
  if role=='source' and cell.name.startswith('zlink-cpp-'):
   hot=max((tid for tid,t in tt.items() if t['role']!='Core I/O'),key=lambda tid:tt[tid]['cpu_us']);tt[hot]['role']='caller'
  groups={}
  for tid,t in tt.items():groups[t['role']]=groups.get(t['role'],0)+t['cpu_us']
  r['roles'][role]={'cpu_us':sum(v['cpu_us'] for v in tt.values()),'groups_us':groups,'threads':tt,'hottest_cpu_us':max(v['cpu_us'] for v in tt.values())}
 rows[cell.name]=r
(job/'none-summary.json').write_text(json.dumps(rows,indent=2))
for k,r in rows.items():
 print(k,round(r['result']['throughput_per_second'],1),'capture',round(r['capture_rate'],1))
 for role,v in r['roles'].items():print(role,round(v['cpu_us'],3),{k:round(z,3) for k,z in v['groups_us'].items()})
