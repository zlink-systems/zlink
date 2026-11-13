import json,pathlib,statistics,sys,collections
root=pathlib.Path(__file__).parent
name=sys.argv[1] if len(sys.argv)>1 else 'baseline'
rows=collections.defaultdict(dict)
def cpu(before,after,role):
 b=before['processes'][role];a=after['processes'][role];threads={};delta=after['monotonic']-before['monotonic'];key='completed' if role=='source' else 'received';n=after['stats'][role][key]-before['stats'][role][key]
 for tid,x in a['threads'].items():
  y=b['threads'].get(tid)
  if y is None:continue
  us=(x['runtime_ns']-y['runtime_ns'])/1000
  threads[tid]={'comm':x['comm'],'cpu_us':us,'us_per_msg':us/n,'cores':us/1e6/delta,'user_us':(x['utime_ticks']-y['utime_ticks'])*10000,'kernel_us':(x['stime_ticks']-y['stime_ticks'])*10000}
 main=threads[str(a['pid'])];return {'messages':n,'seconds':delta,'rate':n/delta,'main':main,'threads':threads,'total_us_per_msg':sum(x['cpu_us'] for x in threads.values())/n,'user_us_per_msg':sum(x['user_us'] for x in threads.values())/n,'kernel_us_per_msg':sum(x['kernel_us'] for x in threads.values())/n}
for f in sorted((root/name).glob('run*/*/results.json')):
 data=json.loads(f.read_text());c=data['cells'][0];key=(c['pattern'],c['payload_size']);impl='F' if 'framework' in c['implementation'] else 'B';rows[key].setdefault(impl,[]).append(c['throughput_per_second'])
 cap=f.parent/'capture';cap=cap if (cap/'before.json').exists() else f.parent
 if (cap/'before.json').exists():
  b=json.loads((cap/'before.json').read_text());a=json.loads((cap/'after.json').read_text());out={r:cpu(b,a,r) for r in ('source','target')};(cap/'cpu.json').write_text(json.dumps(out,indent=2)+'\n')
print('| 셀 | F msg/s 중앙값 [최소–최대] | B msg/s 중앙값 [최소–최대] | F÷B | B µs/msg | F 예산 µs/msg | F µs/msg | 예산 초과 또는 여유 | 판정 |')
print('|---|---:|---:|---:|---:|---:|---:|---|---|')
summary=[]
for (pat,size),d in sorted(rows.items(),key=lambda item:(['send-saturation','request-backpressure','request-serial'].index(item[0][0]),item[0][1])):
 if set(d)!=set(('F','B')):continue
 f=statistics.median(d['F']);b=statistics.median(d['B']);budget=1e6/b/.6;ft=1e6/f;ratio=f/b;diff=ft-budget
 def fmt(x):return f'{statistics.median(x):,.1f} [{min(x):,.1f}–{max(x):,.1f}]'
 tags=[]
 if ratio<.6:tags.append('0.6 미달')
 if .58<=ratio<=.62:tags.append('±0.02 경계')
 print(f'| {pat} {size} | {fmt(d["F"])} | {fmt(d["B"])} | {ratio:.4f} | {1e6/b:.3f} | {budget:.3f} | {ft:.3f} | {"over budget by" if diff>0 else "여유"} {abs(diff):.3f} µs/msg | {", ".join(tags) or "0.62 초과"} |')
 summary.append({'pattern':pat,'size':size,'F':d['F'],'B':d['B'],'F_median':f,'B_median':b,'ratio':ratio,'budget_us':budget,'F_us':ft,'over_budget_us':diff})
(root/(name+'-summary.json')).write_text(json.dumps(summary,indent=2)+'\n')
