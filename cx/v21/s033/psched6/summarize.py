#!/usr/bin/env python3
"""Build the Korean base/branch comparison report from preserved raw cells."""
import json,math,pathlib,statistics
job=pathlib.Path('/root/jobs/fx/cx/v21/s033/psched5'); records=[]
def cell(run,pattern,variant,impl):
    return job/'measure'/str(run)/pattern/variant/'run1'/f'{impl}-{pattern}-1024'
def readbench(run,pattern,variant,impl):
    d=cell(run,pattern,variant,impl); c=json.loads((d/'results.json').read_text())['cells'][0]
    if c.get('contaminated') or c.get('errors') or c.get('abandoned') or c.get('drain_bound_hit'): raise RuntimeError(f'invalid cell {d}: {c}')
    return c['throughput_per_second'],d
for pattern in ('send-saturation','request-backpressure'):
 for variant in ('base','narrowed'):
  for run in (1,2,3):
   f,d=readbench(run,pattern,variant,'zlink-framework-cpp')
   records.append({'pattern':pattern,'variant':variant,'round':run,'throughput':f,'raw':str(d/'results.json')})
pm=[]
for variant in ('base','narrowed'):
 for run in (1,2,3):
  d=job/'measure'/str(run)/'pm'/variant/'run1'/'zlink-framework-cpp-send-saturation-1024'
  lat=json.loads((d/'capture/pm-latencies.json').read_text()); lat.sort()
  if len(lat)<150: raise RuntimeError(f'insufficient Spot probes in {d}: {len(lat)}')
  p50=lat[math.ceil(.50*len(lat))-1]; p99=lat[math.ceil(.99*len(lat))-1]
  pm.append({'variant':variant,'round':run,'p50_us':p50,'p99_us':p99,'raw':str(d/'capture/pm-latencies.json')})
source=job/'measure'
for round_no in (1,2,3):
    import subprocess
    subprocess.run(['python3',str(job/'analyze-22k.py'),str(source/str(round_no)/'22k')],check=True)
rows22=[]
for round_no in (1,2,3): rows22.extend(json.loads((source/str(round_no)/'22k/summary-22k.json').read_text()))
# report data and raw location per metric family/variant/round
lines=['# Scheduler PR #1639 — 측정 결과','', '## 조건','', 'Base는 `54ee69b8c0`에 동일한 bench-pacer 두 커밋을 적용했고, branch는 `37f78cba87`에 같은 두 커밋을 적용했다. 두 빌드는 Release `-O3 -DNDEBUG`, Core/binding prefix, 의존성, harness, CPU 고정 조건을 공유한다. 조건 전체는 [measurement-conditions.json](measurement-conditions.json)에 있다. 각 round 안에서 base와 branch를 짝지어 실행했다.','', '| 측정 | Base round 1 / 2 / 3 | Base median | Narrowed round 1 / 2 / 3 | Narrowed median | 원자료 경로 (round 1 / 2 / 3) |','|---|---:|---:|---:|---:|---|']
for pattern,label in [('send-saturation','Framework send-saturation 1 KiB msg/s'),('request-backpressure','Framework request-backpressure 1 KiB msg/s')]:
 vals={v:[r['throughput'] for r in records if r['pattern']==pattern and r['variant']==v] for v in ('base','narrowed')}
 paths={v:[r['raw'] for r in records if r['pattern']==pattern and r['variant']==v] for v in ('base','narrowed')}
 lines.append(f"| {label} | {', '.join(f'{x:,.1f}' for x in vals['base'])} | {statistics.median(vals['base']):,.1f} | {', '.join(f'{x:,.1f}' for x in vals['narrowed'])} | {statistics.median(vals['narrowed']):,.1f} | base: `{'; '.join(paths['base'])}`<br>branch: `{'; '.join(paths['narrowed'])}` |")
for metric,label in [('p50_us','Spot p50 µs'),('p99_us','Spot p99 µs')]:
 vals={v:[r[metric] for r in pm if r['variant']==v] for v in ('base','narrowed')}
 paths={v:[r['raw'] for r in pm if r['variant']==v] for v in ('base','narrowed')}
 lines.append(f"| PM Spot {label} | {', '.join(f'{x:.3f}' for x in vals['base'])} | {statistics.median(vals['base']):.3f} | {', '.join(f'{x:.3f}' for x in vals['narrowed'])} | {statistics.median(vals['narrowed']):.3f} | base: `{'; '.join(paths['base'])}`<br>branch: `{'; '.join(paths['narrowed'])}` |")
lines+=['','## 22k request-backpressure window A','', '각 값은 5초 window의 per-thread 값을 message 수로 나눈 값이다. I/O thread는 comm의 `/IO/` 이름 중 sendto/message가 가장 큰 thread다. 유효 주파수는 aperf/mperf × 2.0 GHz다. Framework target role 시간은 해당 role thread들의 schedstat µs/message 합이다. 자발적 context switch는 프로세스의 모든 thread를 합산했다.','', '| Binding / Framework | Variant | Round | Target I/O sendto/msg | Source I/O sendto/msg | Target / Source I/O GHz | Framework target µs/msg (receive / app-worker) | Target / Source voluntary wakes/msg | Window-A msg/s | 원자료 경로 |','|---|---|---:|---:|---:|---:|---:|---:|---:|---|']
for impl in ('zlink-cpp','zlink-framework-cpp'):
 for variant in ('base','narrowed'):
  for r in sorted((x for x in rows22 if x['implementation']==impl and x['variant']==variant),key=lambda x:x['round']):
   t,s=r['io']['target'],r['io']['source']; roles=r['framework_target_role_us_per_message']; role_text='— / —' if impl=='zlink-cpp' else f"{roles.get('receive',0):.3f} / {roles.get('app-worker',0):.3f}"
   lines.append(f"| {impl} | {variant} | {r['round']} | {t['sendto_per_msg']:.3f} ({t['comm']}) | {s['sendto_per_msg']:.3f} ({s['comm']}) | {t['effective_ghz']:.2f} / {s['effective_ghz']:.2f} | {role_text} | {r['wakes_per_message']['target']:.3f} / {r['wakes_per_message']['source']:.3f} | {r['rate']:,.0f} | `{r['raw']}`<br>`{pathlib.Path(r['raw'])/'A-perfstat.csv'}` |")
# Round medians for all 22k requested role metrics.
lines+=['','### 22k 3회 중앙값','', '| Binding / Framework | Variant | Target / source I/O sendto/msg | Target / source I/O GHz | Framework target µs/msg (receive / app-worker) | Target / source voluntary wakes/msg | Window-A msg/s | Raw paths (round 1 / 2 / 3) |','|---|---|---:|---:|---:|---:|---:|---|']
for impl in ('zlink-cpp','zlink-framework-cpp'):
 for variant in ('base','narrowed'):
  rs=sorted((x for x in rows22 if x['implementation']==impl and x['variant']==variant),key=lambda x:x['round'])
  def med(fn): return statistics.median(fn(x) for x in rs)
  roles=tuple('—' if impl=='zlink-cpp' else f"{statistics.median(x['framework_target_role_us_per_message'].get(role,0) for x in rs):.3f}" for role in ('receive','app-worker'))
  paths='; '.join(x['raw'] for x in rs)
  lines.append(f"| {impl} | {variant} | {med(lambda x:x['io']['target']['sendto_per_msg']):.3f} / {med(lambda x:x['io']['source']['sendto_per_msg']):.3f} | {med(lambda x:x['io']['target']['effective_ghz']):.2f} / {med(lambda x:x['io']['source']['effective_ghz']):.2f} | {roles[0]} / {roles[1]} | {med(lambda x:x['wakes_per_message']['target']):.3f} / {med(lambda x:x['wakes_per_message']['source']):.3f} | {med(lambda x:x['rate']):,.0f} | `{paths}` |")
lines+=['','### 22k per-thread window-A 계측','', '| Variant | Round | Implementation | Process / thread | µs/msg | Instructions/msg | IPC | Effective GHz | sendto/msg | Raw path |','|---|---:|---|---|---:|---:|---:|---:|---:|---|']
for x in sorted(rows22,key=lambda r:(r['implementation'],r['variant'],r['round'])):
 for tid,v in sorted(x['threads'].items(),key=lambda pair:(pair[1]['role'],pair[1]['comm'],pair[0])):
  ghz='—' if v['effective_ghz'] is None else f"{v['effective_ghz']:.2f}"
  ipc='—' if v['ipc'] is None else f"{v['ipc']:.2f}"
  lines.append(f"| {x['variant']} | {x['round']} | {x['implementation']} | {v['role']} / {v['comm']} ({tid}) | {v['us_per_msg']:.3f} | {v['instructions_per_msg']:.0f} | {ipc} | {ghz} | {v['sendto_per_msg']:.3f} | `{x['raw']}/A-perfstat.csv` |")
lines+=['','## Merge 조건 판정','']
for pattern,label in [('send-saturation','Framework send-saturation 처리량'),('request-backpressure','Framework request-backpressure 처리량')]:
 b=[r['throughput'] for r in records if r['pattern']==pattern and r['variant']=='base']; q=[r['throughput'] for r in records if r['pattern']==pattern and r['variant']=='narrowed']
 spread=max(max(b)-min(b),max(q)-min(q)); delta=statistics.median(q)-statistics.median(b)
 lines.append(f"- {label}: branch {'기준 충족' if delta >= -spread else '기준 미충족'} (중앙값 차이 {delta:+.1f} msg/s, 반복 범위 최대 {spread:.1f} msg/s). 원자료: `{'; '.join(r['raw'] for r in records if r['pattern']==pattern)}`.")
b=[r['p99_us'] for r in pm if r['variant']=='base'];q=[r['p99_us'] for r in pm if r['variant']=='narrowed'];spread=max(max(b)-min(b),max(q)-min(q));delta=statistics.median(q)-statistics.median(b)
lines.append(f"- PM Spot p99: branch {'기준 충족' if delta <= spread else '기준 미충족'} (중앙값 차이 {delta:+.3f} µs, 반복 범위 최대 {spread:.3f} µs). 원자료: `{'; '.join(r['raw'] for r in pm)}`.")
for impl in ('zlink-cpp','zlink-framework-cpp'):
 b=[r['io']['target']['sendto_per_msg'] for r in rows22 if r['implementation']==impl and r['variant']=='base']; q=[r['io']['target']['sendto_per_msg'] for r in rows22 if r['implementation']==impl and r['variant']=='narrowed']
 lines.append(f"- {impl} target I/O sendto/msg: branch {'기준 충족' if statistics.median(q)<=statistics.median(b) else '기준 미충족'} (base {statistics.median(b):.3f}, branch {statistics.median(q):.3f}; 낮을수록 좋음). 원자료는 22k 표 경로.")
lines+=['','## 보존된 자료','', f'전체 raw 결과는 `{job}/measure/` 아래 보존했다. summary 파일은 각 round의 22k `summary-22k.json`과 cdrain 형식의 benchmark `results.json`이다.']
(job/'measurement-summary.md').write_text('\n'.join(lines)+'\n')
