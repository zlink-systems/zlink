from pathlib import Path
import json
P=Path('/root/jobs/fx/cx/v21/s033/creply2');S=json.loads((P/'summary.json').read_text());T=json.loads((P/'tables.json').read_text())
label={'main':'main','direct':'변경본','binding':'binding'}
print('| Pattern | Payload | 구현 | RUNS=5 중앙값 req/s | 최솟값–최댓값 req/s | F÷B | µs/request | over budget by µs/request |')
print('|---|---:|---|---:|---:|---:|---:|---:|')
for pattern,size in [('request-backpressure',1024),('request-backpressure',4096),('request-serial',1024)]:
 for variant in ('binding','main','direct'):
  s=S['throughput'][f'{variant}/{pattern}/{size}'];ratio='—' if variant=='binding' else f"{s['ratio']:.4f}"
  cost=1e6/s['rate'];over='—' if variant=='binding' else f"{s['over_budget_us']:+.3f}"
  print(f"| {pattern} | {size//1024}KiB | {label[variant]} | {s['rate']:,.1f} | {s['min']:,.1f}–{s['max']:,.1f} | {ratio} | {cost:.3f} | {over} |")
print('\n| 40k paced 실행 | 실제 perf 구간 req/s | Reply data segments/request | 평균 data segment B | Retransmit delta |')
print('|---|---:|---:|---:|---:|')
for index in (1,2):
 for variant in ('main','direct','binding'):
  r=T['same_rate'][f'{variant}-perf-{index}'];d=r['reply_connection']['delta']
  print(f"| {label[variant]} {index} | {r['rate']:,.1f} | {r['packets_per_request']:.6f} | {r['average_data_segment_bytes']:.1f} | {d['total_retrans']} |")
print('\n| 40k paced mutex 실행 | 요청 수 | Reply lock 호출 수 | 경합 호출 수 | 누적 wait ns | Wait µs/request |')
print('|---|---:|---:|---:|---:|---:|')
for variant in ('main','direct'):
 r=S['mutex'][variant]
 print(f"| {label[variant]} | {r['requests']:,} | {r['calls']:,} | {r['contended']:,} | {r['wait_ns']:,} | {r['wait_us_per_request']:.6f} |")
