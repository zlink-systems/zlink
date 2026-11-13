from pathlib import Path
import json,statistics,time
P=Path('/root/jobs/fx/cx/v21/s033/creply');f=P/'report.md';s=f.read_text()
t=json.loads((P/'tables.json').read_text());u=t['throughput'];rates=t['same_rate']
expected=[f'{v}/{p}/{n}' for p,n in [('request-backpressure',1024),('request-backpressure',4096),('request-serial',1024)] for v in ['main','direct','binding']]
assert all(k in u and len(u[k]['runs'])==5 for k in expected),'RUNS=5 incomplete'
close=json.loads((P/'close-validation.json').read_text());assert all(close[k]['exit']==0 for k in ['main','direct'])
assert all(f'direct-perf-{i}' in rates for i in (1,2))
lines=['\n프로토타입은 빌드 후 close 경합 검사를 통과했다. 원본에서는 제출 중 close가 대기했고, unlock한 프로토타입에서는 close가 진행된 뒤 native 제출이 거부됐다. Close 이후 port reply도 두 실행에서 false였다. 이 검사는 native API 진입 전 close를 강제로 앞세운 경우이며 가능한 모든 수명 순서를 시험한 것은 아니다. 상세 출력은 close-validation.json에 있다.\n',
'RUNS=5는 이번 세션에서 main, prototype, binding을 같은 payload와 pattern으로 번갈아 실행했다. 각 실행은 warmup 10초와 active 5초다. Source는 같은 main client 바이너리로 고정했고 target만 바꿨다. 요청률 제한은 없다. 처리량은 5회 중앙값과 최소–최대다. Caller는 submit thread CPU/request의 중앙값이다.\n',
'| Pattern·payload | 대상 | req/s 중앙값 (최소–최대) | caller µs/request | F÷B | 0.6×binding 처리량에 대한 초과 µs/message |',
'|---|---|---:|---:|---:|---:|']
budget_notes=[]
for p,n in [('request-backpressure',1024),('request-backpressure',4096),('request-serial',1024)]:
 b=u[f'binding/{p}/{n}']['rate_median'];budget=1e6/(.6*b)
 for v in ['main','direct','binding']:
  row=u[f'{v}/{p}/{n}'];r=row['rate_median'];excess=1e6/r-budget
  lines.append(f"| {p} {n} B | {v} | {r:,.1f} ({row['rate_min']:,.1f}–{row['rate_max']:,.1f}) | {row['caller_us_median']:.3f} | {r/b:.4f} | {'—' if v=='binding' else f'{excess:+.3f}'} |")
 budget_notes.append(f'{p} {n} B의 처리시간 예산은 {budget:.3f} µs/message다.')
lines+=['\n'+' '.join(budget_notes)+' 초과량은 1e6/F−1e6/(0.6×B)이며 caller CPU 시간과 구분한다.\n']
lines+=['\n동일 요청률에서 프로토타입을 다시 계측한 결과는 다음과 같다. 아래 값도 perf 구간 안의 ss와 같은 구간의 완료 수를 사용한다.\n','| 프로토타입 40k 실행 | 실제 req/s | packet/request | 평균 segment B | ACK/request | send cycles/request | cycles/packet |','|---|---:|---:|---:|---:|---:|---:|']
for i in (1,2):
 r=rates[f'direct-perf-{i}'];lines.append(f"| direct-perf-{i} | {r['rate']:,.3f} | {r['packets_per_request']:.6f} | {r['average_data_segment_bytes']:,.3f} | {r['reply_ACKs_per_request']:.6f} | {r['send_cycles_per_request']:,.0f} | {r['send_cycles_per_packet']:,.0f} |")
s=s.replace('현재 프로토타입 빌드와 close 검증을 기다리고 있으며, 아래에 아직 없는 처리량 수치를 추정하지 않았다.','프로토타입 전후의 처리량과 동일 요청률 결과는 아래에 별도로 기록했다.')
s=s.replace('확인한 중복 작업은 다음과 같다. 현재 runtime 프로토타입은 아직 적용되지 않았다.','확인한 중복 작업은 다음과 같다. 프로토타입에서 native submit의 mutex 범위만 제거했다.')
s=s.replace('submit의 Framework lock 범위를 제거할 예정','prototype에서 submit의 Framework lock 범위 제거')
s=s.replace('현재 남은 일은 프로토타입 close 경합 검사, RUNS=5 처리량 비교, 동일 요청률 재계측이다. ','')
s=s.replace('현재 이 diff는 runtime 파일에 적용하지 않았고 빌드·close 검증도 완료하지 않았다. ','측정 사본에 적용해 빌드·close 검증·성능 측정을 수행했다. ')
s=s.replace('실제 close 경합 fixture 결과는 아직 없다. 따라서 close 안전성을 실측 검증했다고 주장하지 않는다.','Native API 진입 직전 close를 앞세운 fixture의 결과는 아래에 기록했다.')
s=s.replace('현재 실제 소스는 원본이므로 적용 후 규칙 수나 성능 개선을 완료 결과로 보고하지 않는다.','프로토타입의 native 제출 직렬화 판정 위치 수는 수정 전 2곳, 수정 후 1곳이다.')
s=s.replace('제안은 제출 직렬화 범위 1항목만 다룬다.','프로토타입은 제출 직렬화 범위 1항목만 제거했다.')
s+='\n'.join(lines)+'\n'
f.write_text(s)
