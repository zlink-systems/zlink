import json,pathlib,statistics,sys
J=pathlib.Path(__file__).parent;name=sys.argv[1] if len(sys.argv)>1 else 'matched-rate-baseline';rows=[]
print('| 회차 | 구현 | target msg/s | source user | source kernel | target user | target kernel | 합계 µs/msg | source main | target main |')
print('|---|---|---:|---:|---:|---:|---:|---:|---:|---:|')
for run in range(1,4):
 for impl in ['zlink-framework-node','zlink-node']:
  p=J/name/f'run{run}'/f'{impl}-send-saturation-4096';b=json.loads((p/'before.json').read_text());a=json.loads((p/'after.json').read_text());n=a['stats']['target']['received']-b['stats']['target']['received'];r={'run':run,'impl':impl,'messages':n,'rate':n/(a['monotonic']-b['monotonic'])}
  for role in ['source','target']:
   bt=b['processes'][role]['threads'];at=a['processes'][role]['threads'];r[role]={}
   for label,key in [('user','utime_ticks'),('kernel','stime_ticks')]:r[role][label]=sum((v[key]-bt[k][key])*10000 for k,v in at.items() if k in bt)/n
   pid=str(a['processes'][role]['pid']);r[role]['main_us']=(at[pid]['runtime_ns']-bt[pid]['runtime_ns'])/1000/n
  r['total_us']=sum(r[role][key] for role in ['source','target'] for key in ['user','kernel']);rows.append(r)
  print(f'| {run} | {"F" if "framework" in impl else "B"} | {r["rate"]:,.1f} | '+ ' | '.join(f'{r[role][key]:.3f}' for role in ['source','target'] for key in ['user','kernel'])+f' | {r["total_us"]:.3f} | {r["source"]["main_us"]:.3f} | {r["target"]["main_us"]:.3f} |')
for run in range(1,4):
 f,b=[r for r in rows if r['run']==run];print('PAIR',run,'rate_difference_msgs',round(b['rate']-f['rate'],2),'cpu_difference_us',round(f['total_us']-b['total_us'],3))
for impl in ['zlink-framework-node','zlink-node']:
 v=[r['total_us'] for r in rows if r['impl']==impl];print(impl,'median/range',statistics.median(v),min(v),max(v))
(J/(name+'-cpu-summary.json')).write_text(json.dumps(rows,indent=2)+'\n')
