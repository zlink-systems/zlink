from pathlib import Path
import json,math,statistics,sys
r=Path('/root/jobs/fx/cx/v21/s033/cbclose2');valid={};deferred=[]
for p in r.glob('*-attempt*.json'):
 x=json.loads(p.read_text())
 if x.get('deferred'):deferred.append(x);continue
 if len(x['runs'])!=2 or any(y['exit'] for y in x['runs']):continue
 key=(x['pattern'],x['pair']);assert key not in valid,key
 rates={}
 for y in x['runs']:
  lines=(r/y['log']).read_text().splitlines()
  hits=[z for z in lines if z.startswith('RESULT,') and ',throughput,' in z]
  assert len(hits)==1,(p,hits)
  rates[y['label']]=float(hits[0].split(',')[-1]);assert rates[y['label']]>0
 x['rates']=rates;x['ratio']=rates['after']/rates['main'];valid[key]=x
rows=sorted(valid.values(),key=lambda x:(['PAIR','PUBSUB','REQUEST_SERIAL'].index(x['pattern']),x['pair']))
(r/'benchmark-summary.json').write_text(json.dumps({'pairs':rows,'deferred':deferred},indent=2))
out=[]
for x in rows:
 order='→'.join(y['label'] for y in x['runs'])
 out.append(f"{x['pattern']} {x['pair']}회({order}): main {x['rates']['main']:.3f} msg/s, PR {x['rates']['after']:.3f} msg/s, PR/main {x['ratio']:.6f}. 부하 원문: {x['uptime']}. 로그: "+', '.join(y['log'] for y in x['runs'])+'.')
for pattern in ['PAIR','PUBSUB','REQUEST_SERIAL']:
 xs=[x['ratio'] for x in rows if x['pattern']==pattern]
 if xs:out.append(f'{pattern}: {len(xs)}쌍, 비율 기하평균 {math.exp(statistics.mean(map(math.log,xs))):.6f}, 중앙값 {statistics.median(xs):.6f}, 범위 {min(xs):.6f}–{max(xs):.6f}.')
(r/'benchmark-summary.txt').write_text('\n\n'.join(out)+'\n')
print('complete pairs',len(rows),'deferred',len(deferred))
if '--final' in sys.argv:assert len(rows)==18,len(rows)
