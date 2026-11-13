#!/usr/bin/env python3
"""Append checked numbers immediately after each hold; plan only required followups."""
import datetime,hashlib,json,statistics,sys
from pathlib import Path
JOB=Path('/root/jobs/fx/cx/v21/s033/l3868-m1'); RAW=JOB/'raw'
PATTERNS=('send-saturation','request-serial','request-backpressure')
def write(path,value):
 path.parent.mkdir(parents=True,exist_ok=True);path.write_text(json.dumps(value,indent=2)+'\n')
def records(variant,pattern,size):
 rows=[]
 for path in sorted((RAW/'matrix'/variant/f'{pattern}-{size}').glob('run*/*/results.json')):
  if not (path.parent/'validated.json').exists(): continue
  v=json.loads(path.read_text());c=v['cells'][0];marker=json.loads((path.parent/'validated.json').read_text())
  assert hashlib.sha256(path.read_bytes()).hexdigest()==marker['result_sha256']
  assert c['errors']==c['target_stats']['errors']==0
  assert c['submitted']==c['completed']==c['target_stats']['received']
  rows.append({'path':str(path),'run':int(path.parent.parent.name[3:]),'msg_s':c['throughput_per_second']})
 return sorted(rows,key=lambda r:r['run'])
def stats(rows):
 if not rows:return None
 values=[r['msg_s'] for r in rows];m=statistics.median(values)
 return {'runs':rows,'median':m,'g5':None if len(rows)==1 else max(abs(v-m)/m for v in values)<=.10,
  'spread':None if len(rows)==1 else max(abs(v-m)/m for v in values)}
def link(value,path):return f'[{value}]({path})'
def linked(s):
 if not s:return '미완료'
 return ' / '.join(link(f'{r["msg_s"]:,.0f}',r['path']) for r in s['runs'])
def reference():
 p=RAW/'binding-daily-reference.json'
 if not p.exists():return None
 value=json.loads(p.read_text())
 value.setdefault('date_utc',value.get('utc_date'))
 for c in value['cells'].values():
  c.setdefault('msg_s',c.get('msg_per_second'))
  c.setdefault('raw',(c.get('raw_paths') or [None])[0])
  assert c['msg_s'] and c['raw']
 return value
def main():
 phase=sys.argv[1] if len(sys.argv)>1 else 'partial';ref=reference();cells=[];plan=[];profiles=[]
 for pattern in PATTERNS:
  for size in (1024,4096):
   variants={v:records(v,pattern,size) for v in ('binding','L0','L1')}
   cell={'pattern':pattern,'size':size,'variants':{v:stats(r) for v,r in variants.items()}}
   daily=ref['cells'].get(f'{pattern}-{size}') if ref else None
   cell['daily']=daily
   l0,l1=variants['L0'],variants['L1']
   if daily and l0:
    cell['L0_initial_daily_ratio']=l0[0]['msg_s']/daily['msg_s']
    cell['L0_needs_confirmation']=cell['L0_initial_daily_ratio']<.95
    confirm=cell['L0_needs_confirmation']
    if confirm:
     for v in ('binding','L0'):
      for n in (1,2,3):
       if n not in [r['run'] for r in variants[v]]:plan.append(dict(variant=v,pattern=pattern,size=size,run=n))
    bs,ls=cell['variants']['binding'],cell['variants']['L0']
    if confirm and bs and ls and len(bs['runs'])==len(ls['runs'])==3:
     cell['L0_confirm_ratio']=ls['median']/bs['median']
     cell['L0_verdict']='판정 불가' if not(bs['g5'] and ls['g5']) else '통과' if cell['L0_confirm_ratio']>=.95 else '미달'
    else:cell['L0_verdict']='확인 대기' if confirm else '통과'
   if l0 and l1:
    initial_loss=1-l1[0]['msg_s']/l0[0]['msg_s'];cell['initial_step_loss']=initial_loss
    boundary=.05<=initial_loss<=.09;cell['step_needs_confirmation']=boundary
    if boundary:
     for v in ('L0','L1'):
      for n in (2,3):
       if n not in [r['run'] for r in variants[v]]:plan.append(dict(variant=v,pattern=pattern,size=size,run=n))
    pair_count=3 if boundary else 1
    pair_ready=all(len(variants[v])>=pair_count for v in ('L0','L1'))
    if pair_ready:
     a,b=stats(l0[:pair_count]),stats(l1[:pair_count])
     cell['step_previous']=a;cell['step_new']=b;cell['step_loss']=1-b['median']/a['median']
     valid=pair_count==1 or (a['g5'] and b['g5'])
     cell['L1_verdict']='판정 불가' if not valid else '통과' if cell['step_loss']<=.07 else '미달'
     if cell['step_loss']>.07:
      for v in ('L0','L1'):
       profiles.append(dict(variant=v,pattern=pattern,size=size,run=1))
    else:cell['L1_verdict']='확인 대기'
    if daily:cell['L1_initial_daily_ratio']=l1[0]['msg_s']/daily['msg_s']
   cells.append(cell)
 summary={'updated_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'phase':phase,'cells':cells}
 write(RAW/'summary.json',summary)
 # Deduplicate shared L0 runs and interleave the previous/new steps within each repeat.
 unique={(p['pattern'],p['size'],p['run'],p['variant']):p for p in plan}
 plan=sorted(unique.values(),key=lambda p:(PATTERNS.index(p['pattern']),p['size'],p['run'],('binding','L0','L1').index(p['variant'])))
 write(RAW/'confirm-plan.json',plan);write(RAW/'profile-plan.json',profiles)
 report=JOB/'report.md'
 if not report.exists():
  report.write_text('# #4350 C++ L0 · #4351 L1 permit 빠른 측정\n\n소스 `608513f260`, 실험 브랜치 `exp/3868-cpp-ladder`. [call-shape](call-shape.md)에서 공통 source submit, blocking recv, 새 envelope/body reply를 대조했다. L1만 실제 application job queue permit을 recv 전에 확보하고 handler 직전에 반환한다.\n\n조건: warmup 3초, active 5초, source CPU 0–7, target CPU 8–15. 기본 셀당 1회. loss 5–9% 셀만 L0/L1 3회, L0÷daily binding<0.95 셀만 binding/L0 3회. G5는 3회 셀에서만 max(|run−median|)/median≤10%를 확인한다. [측정 규칙](/root/jobs/fx/cx/v21/s033/common-rules.md) · [binary provenance](raw/build-provenance.json).\n')
 lines=[f'\n{summary["updated_utc"]} `{phase}` hold 직후 결과(append). 단위 msg/s. 원본은 모두 보존한다.']
 if ref:lines+=['',f'Binding 기준: [cpp-{ref["date_utc"]}.json](/root/jobs/fx/cx/v21/s033/binding-daily/cpp-{ref["date_utc"]}.json). daily ratio는 이 파일의 값을 사용한다.']
 lines+=['','| 셀 | daily binding | L0 첫 실행 | L1 첫 실행 | L0÷daily | L1÷daily | 첫 step loss |','|---|---:|---:|---:|---:|---:|---:|']
 for c in cells:
  v=c['variants'];d=c['daily'];row=f'{c["pattern"]} {c["size"]//1024}K'
  b=link(f'{d["msg_s"]:,.0f}',d['raw']) if d else linked(v['binding'])
  a=linked(stats(v['L0']['runs'][:1])) if v['L0'] else '미완료'
  n=linked(stats(v['L1']['runs'][:1])) if v['L1'] else '미완료'
  ratio=lambda k:f'{c[k]:.4f}' if k in c else '—'
  loss=f'{100*c["initial_step_loss"]:.2f}%' if 'initial_step_loss' in c else '—'
  lines.append(f'| {row} | {b} | {a} | {n} | {ratio("L0_initial_daily_ratio")} | {ratio("L1_initial_daily_ratio")} | {loss} |')
 lines+=['','| 셀 | L0 판정 | L1 판정 | 적용 step loss | 적용 횟수 |','|---|---|---|---:|---:|']
 for c in cells:
  loss=f'{100*c["step_loss"]:.2f}%' if 'step_loss' in c else '—'
  n=len(c['step_previous']['runs']) if 'step_previous' in c else '—'
  lines.append(f'| {c["pattern"]} {c["size"]//1024}K | {c.get("L0_verdict","미완료")} | {c.get("L1_verdict","미완료")} | {loss} | {n} |')
 if any(v and len(v['runs'])==3 for c in cells for v in c['variants'].values()):
  lines+=['','| 3회 확인 셀 | variant | 실행값(msg/s) | median | median÷daily | G5 spread |','|---|---|---|---:|---:|---:|']
  for c in cells:
   for name,v in c['variants'].items():
    if v and len(v['runs'])==3:
     lines.append(f'| {c["pattern"]} {c["size"]//1024}K | {name} | {linked(v)} | {v["median"]:,.0f} | {v["median"]/c["daily"]["msg_s"]:.4f} | {100*v["spread"]:.2f}% ({"통과" if v["g5"] else "실패"}) |')
  for c in cells:
   if 'L0_confirm_ratio' in c:lines.append(f'\n{c["pattern"]} {c["size"]//1024}K L0 3회 확인 ratio(이번 hold pair median): {c["L0_confirm_ratio"]:.4f}. Daily ratio와 구분하며 L0 확인 판정에만 사용한다.')
 lines+=['',f'[hold 시작](raw/lock-windows/{phase}-before.txt) · [hold 종료](raw/lock-windows/{phase}-after.txt) · [계산 입력·실행별 원본](raw/summary.json).']
 with report.open('a') as out:out.write('\n'.join(lines)+'\n')
 print(json.dumps({'phase':phase,'confirmed_pairs':len([c for c in cells if 'step_loss' in c]),'confirmation_runs':len(plan),'profile_runs':len(profiles)}))
if __name__=='__main__':main()
