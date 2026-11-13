#!/usr/bin/env python3
import json,pathlib,statistics
job=pathlib.Path('/root/jobs/fx/cx/v21/s033/lad-node-m1')
base=job/'raw/session'; data=json.load(open(job/'raw/aggregates/session.json'))
impl={'L0':'zlink-framework-node','L0R':'zlink-framework-node','L1':'zlink-framework-node'}
def samples(v,pattern,payload):
  paths=[base/v/f'run1/{impl[v]}-{pattern}-{payload}/results.json']
  paths += [base/'confirm'/v/f'run{i}/{impl[v]}-{pattern}-{payload}/results.json' for i in (1,2)]
  values=[]
  for p in paths:
    c=json.load(open(p))['cells'][0]
    if c['errors'] or c['server_errors'] or c['target_stats']['errors'] or c['submitted']!=c['completed'] or c['submitted']!=c['target_stats']['received']: raise SystemExit(f'broken result: {p}')
    values.append((c['throughput_per_second'],p.relative_to(job).as_posix()))
  med=statistics.median(x[0] for x in values)
  spread=max(abs(x[0]-med) for x in values)/med
  return values,med,spread
lines=['\n## 경계 셀 3회 확인\n','기본 hold의 1회 값은 위 ladder 비교표에 그대로 둔다. 아래 비교만 기본 표본과 추가 2회를 합친 3회 중앙값으로 판정한다. G5는 최대 편차/중앙값이며 10% 초과는 판정 불가다.\n','| 셀 | 비교 | 이전 중앙값 | 새 중앙값 | 손실 | G5 이전/새 | 판정 | 세 raw 경로 |','|---|---|---:|---:|---:|---|---|---|']
for line in open(job/'raw/confirm-pairs.txt'):
  pattern,payload,pair=line.split(); payload=int(payload); left,right=pair.split('-',1)
  lv,lm,ls=samples(left,pattern,payload); rv,rm,rs=samples(right,pattern,payload)
  loss=1-rm/lm
  g5l='통과' if ls<=.10 else '판정 불가'; g5r='통과' if rs<=.10 else '판정 불가'
  if right=='L1': verdict='통과' if loss<=.07 and g5l==g5r=='통과' else ('미달' if loss>.07 else '판정 불가')
  else: verdict='모듈 load 비용 확인'
  paths=', '.join(f"{v}: `{p}`" for v,arr in ((left,lv),(right,rv)) for _,p in arr)
  lines.append(f'| {pattern}-{payload} | {left}→{right} | {lm:.1f} | {rm:.1f} | {loss:.2%} | {g5l}/{g5r} | {verdict} | {paths} |')
lines.append('\n## Request-serial 4K L0/L0R profiling\n')
lines.append('요청 시 조건(L0R 대 L0 module-load loss 9.84% > 2%)에 따라 한 쌍을 수집했다. Target 종료 시 메모리: L0 RSS 78.7 MiB, heapTotal 15.1 MiB, heapUsed 11.6 MiB; L0R RSS 94.4 MiB, heapTotal 29.6 MiB, heapUsed 12.1 MiB. 원시 메모리 기록은 `raw/profile-bench/module-L0/run1/zlink-framework-node-request-serial-4096/target.log` 및 `raw/profile-bench/module-L0R/run1/zlink-framework-node-request-serial-4096/target.log`다.\n')
lines.append('Target CPU sample 비율은 L0→L0R에서 `recv` `framework/languages/node/node_modules/@zlink-systems/zlink/dist/zlink/runtime/sockets/socket_operations.js:156` 약 7.0%→19.1%, L0 pump `framework/bench/grpc/node/zlink-framework-server/main.js:93` 약 0.5%→1.5%, GC 0.6%→1.7%, `(program)` 5.9%→14.6%였다. profile raw는 `raw/profile/module-load/L0/CPU.20261114.005724.1092573.0.001.cpuprofile` 및 `raw/profile/module-load/L0R/CPU.20261114.005754.1099754.0.001.cpuprofile`이다.\n')
with open(job/'report.md','a') as f: f.write('\n'.join(lines))
print(job/'report.md')
