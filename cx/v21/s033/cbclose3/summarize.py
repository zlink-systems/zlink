from pathlib import Path
import json,re,statistics,math
r=Path(__file__).parent
records=[];deferred=[]
for p in r.glob('*-attempt*.json'):
 d=json.loads(p.read_text())
 if d.get('deferred'):deferred.append(d);continue
 if len(d.get('runs',[]))!=2:raise RuntimeError(f'incomplete {p.name}')
 v={}
 for a in d['runs']:
  if a['exit']!=0:raise RuntimeError(a)
  m=re.search(r'RESULT,cpp,[^\n]+,throughput,([0-9.]+)',(r/a['log']).read_text())
  if not m:raise RuntimeError(a)
  v[a['label']]=float(m[1])
 d.update(values=v,ratio=v['after']/v['main'],json=p.name)
 records.append(d)
out=[];stats={}
for pattern in ['PAIR','PUBSUB','REQUEST_SERIAL']:
 rows=sorted([d for d in records if d['pattern']==pattern],key=lambda d:d['pair'])
 for d in rows:
  order='→'.join(x['label'] for x in d['runs'])
  logs=', '.join(x['log'] for x in d['runs'])
  out.append(f"{pattern} {d['pair']}회({order}): main {d['values']['main']:.3f}, PR {d['values']['after']:.3f}, PR/main {d['ratio']:.6f}. 부하 원문: {d['uptime']}. 로그: {logs}. JSON: {d['json']}.")
 if not rows:continue
 ratios=[d['ratio'] for d in rows]
 stats[pattern]={'pairs':len(rows),'median':statistics.median(ratios),'geomean':math.exp(statistics.mean(math.log(x) for x in ratios)),'min':min(ratios),'max':max(ratios)}
 s=stats[pattern]
 out.append(f"{pattern}: {s['pairs']}쌍, 중앙값 {s['median']:.6f}, 기하평균 {s['geomean']:.6f}, 범위 {s['min']:.6f}–{s['max']:.6f}.")
for d in deferred:out.append(f"측정 연기: {d['pattern']} {d['pair']}회 attempt {d['attempt']}. {d['uptime']}. Benchmark 실행 없음.")
(r/'benchmark-summary.txt').write_text('\n\n'.join(out)+'\n')
(r/'benchmark-results.json').write_text(json.dumps({'stats':stats,'pairs':records,'deferred':deferred},indent=2))
print(json.dumps(stats,indent=2))
