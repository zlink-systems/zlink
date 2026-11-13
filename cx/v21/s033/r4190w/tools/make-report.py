import json, statistics, os
from pathlib import Path
JOB=Path('/root/jobs/fx/cx/v21/s033/r4190w')
RAW=JOB/'raw'
langs=['cpp','dotnet','java','node']
patterns=['request-serial','request-backpressure','send-saturation']
sizes=[1024,4096]
labels={'cpp':'C++','dotnet':'.NET','java':'Java','node':'Node'}
impls={'cpp':('grpc-cpp','zlink-cpp','zlink-framework-cpp'),'dotnet':('grpc-dotnet','zlink-dotnet','zlink-framework-dotnet'),'java':('grpc-java','zlink-java','zlink-framework-java'),'node':('grpc-node','zlink-node','zlink-framework-node')}
agg={l:json.load(open(RAW/'aggregates'/f'{l}.json')) for l in langs}
old={l:json.load(open(Path('/root/jobs/fx/cx/v21/s033/r4190m/raw/aggregates')/f'{l}.json')) for l in langs}
cpu={l:json.load(open(RAW/'bench'/l/'cpu-per-cell.json')) for l in langs+['c','kotlin']}

def row(d,impl,p,s):
 return next(x for x in d['rows'] if x['implementation']==impl and x['pattern']==p and x['payload_size']==s)
def optrow(d,impl,p,s):
 return next((x for x in d['rows'] if x['implementation']==impl and x['pattern']==p and x['payload_size']==s),None)
def medcpu(lang,impl,p,s,side='source'):
 prefix={'cpp':'','dotnet':'','java':'','node':''}.get(lang,'')
 key=f'{impl}-{p}-{s}'
 vals=[x[f'{side}_us_per_msg'] for x in cpu[lang] if x['cell']==key]
 return statistics.median(vals) if vals else None
def judgement(d,formula,p,s):
 return next((x for x in d['judgements'] if x['formula']==formula and x['pattern']==p and x['payload_size']==s),None)
def fmt(x,dp=2): return '—' if x is None else f'{x:.{dp}f}'
def g5(x):return x.get('g5_status','?')
def g6(x):return x.get('g6_status','?')

def resultpaths(lang,impl,p,s):
 folder={'cpp':'cpp','dotnet':'dotnet','java':'java','node':'node'}[lang]
 return ', '.join(f'raw/bench/{folder}/run{i}/{impl}-{p}-{s}/results.json' for i in (1,2,3))

out=[]
out += ['# #4190 30초 warmup 재측정', '', '측정 commit `d3fd955131`. 기존 r4190m과 같은 셀 집합·배치·3회 반복·5초 활성 측정·stream 설정으로 warmup만 30초로 재측정했다. 언어 결과는 48개씩, Kotlin 보조 셀 6개, C 기준 18개다. 전체 result 파일 warmup은 C++ 48/48, .NET 48/48, Java 48/48, Kotlin 6/6, Node 48/48, C 18/18이 모두 30초였고 `errors=0`, `abandoned=0`, `drain_bound_hit=false`였다.', '', '## 조건·warmup 확인', '', '- 배치 B: source CPU 0–7, target CPU 8–15. 측정 그룹은 `/root/jobs/fx/cx/measure.lock` exclusive lock 안에서 실행했고 CPU 0–15 governor performance, 종료 시 powersave 복구했다. 셀별 uptime/load, free memory, `scaling_cur_freq` 스냅샷은 `raw/bench/<언어>/run*/<cell>/conditions-{before,after}.txt`에 있다. 그룹 단위 lock 전후 상태는 `raw/lock-windows/`에 있다.', '- C++ warmup 근거: `raw/bench/cpp/run1/zlink-framework-cpp-request-backpressure-4096/results.json:91` (`warmupSeconds: 30.000000`).', '- .NET: `raw/bench/dotnet/run1/zlink-dotnet-request-serial-4096/results.json:14` (`warmup: 30`).', '- Java: `raw/bench/java/run1/grpc-java-send-saturation-4096/results.json:17` (`warmupSeconds: 30.0`).', '- Kotlin: `raw/bench/kotlin/run1/zlink-framework-kotlin-request-serial-1024/results.json:17` (`warmupSeconds: 30.0`).', '- Node: `raw/bench/node/run1/zlink-framework-node-send-saturation-4096/results.json:18` (`warmup: 30`).', '- C 기준: `raw/bench/c/run1/zlink-c-send-saturation-4096/results.json:8` (`warmupSeconds: 30`).', '- 집계는 main `bench_aggregate.py` (commit `f649eef0ec`), `--min-runs 3`, baseline `zlink-c`로 수행했다. 결과 `raw/aggregates/{cpp,dotnet,java,node}.json` 및 `.md`; 모든 집계 행의 warmup_seconds는 `[30,30,30]`이다. run별 원본은 아래 표의 `run1–3 results.json` 경로다.', '', '## 전체 처리량·CPU 및 판정', '', '처리량은 aggregator의 3회 중앙값(K msg/s), CPU는 `aggregate_cpu.py`/`aggregate_cpu_c.py`가 TID union의 `/proc` CPU 시간을 집계한 source/target µs/msg 중앙값이다. `FW/bind` pass 기준은 0.60, backpressure의 `bind/C` pass 기준은 0.80이다. 판정의 상태·verdict·reason은 main aggregator의 judgement 그대로이며 G5/G6는 각 gRPC·binding·Framework 결과 행이다. “실행 모델 상한 도달”은 aggregator G6 상태 표기다.', '', '| 패턴 | 크기 | 언어 | gRPC / binding / Framework (K msg/s) | FW/bind | bind/C (BP) | G5 (gRPC/bind/FW) · G6 (bind/FW) | FW/bind 판정: 상태 · verdict · reason | bind/C 판정: 상태 · verdict · reason | CPU µs/msg source/target (gRPC · binding · FW · C 기준) | 원시 경로 |', '|---|---:|---|---:|---:|---:|---|---|---|---:|---|']
for p in patterns:
 for s in sizes:
  for l in langs:
   d=agg[l]; gi,bi,fi=impls[l]
   gr,br,fr=[optrow(d,i,p,s) for i in (gi,bi,fi)]
   gt,bt,ft=[x['median']['throughput_per_second']/1000 if x else None for x in (gr,br,fr)]
   rat=ft/bt if bt else None
   j=judgement(d,f'{fi} / {bi}',p,s)
   jc=judgement(d,f'{bi} / zlink-c',p,s) if p=='request-backpressure' else None
   jtxt='—' if j is None else f"{j['status']} · {j.get('verdict') or '—'} · {j['reason']}"
   jctxt='—' if jc is None else f"{jc['status']} · {jc.get('verdict') or '—'} · {jc['reason']}"
   cpus=[]
   for i in (gi,bi,fi):
    cpus.append(f"{fmt(medcpu(l,i,p,s))}/{fmt(medcpu(l,i,p,s,'target'))}" if optrow(d,i,p,s) else '—/—')
   cpus.append(f"{fmt(medcpu('c','zlink-c',p,s))}/{fmt(medcpu('c','zlink-c',p,s,'target'))}")
   g5s='/'.join(g5(x) if x else 'n/a' for x in (gr,br,fr)); g6s='/'.join(g6(x) if x else 'n/a' for x in (br,fr))
   src='; '.join(resultpaths(l,i,p,s) for i,x in zip((gi,bi,fi),(gr,br,fr)) if x) + f'; raw/bench/c/run{{1,2,3}}/zlink-c-{p}-{s}/results.json; raw/aggregates/{l}.json; raw/bench/{l}/cpu-per-cell.json; raw/bench/c/cpu-per-cell.json'
   cval=next((x['median']['throughput_per_second']/1000 for x in agg[l]['rows'] if x['implementation']=='zlink-c' and x['pattern']==p and x['payload_size']==s),None)
   out.append(f'| {p} | {s} | {labels[l]} | {fmt(gt)}/{fmt(bt)}/{fmt(ft)} | {fmt(rat,3)} | {fmt(bt/cval,3) if p=="request-backpressure" and cval else "—"} | {g5s} · {g6s} | {jtxt} | {jctxt} | {" · ".join(cpus)} | `{src}` |')
out += ['', '## 언어별 aggregator verdict', '', '| 언어 | status | reason | 원시 경로 |', '|---|---|---|---|']
for l in langs:
 verdict=agg[l]['language_verdict']
 out.append(f"| {labels[l]} | {verdict['status']} | {verdict['reason']} | `raw/aggregates/{l}.json` |")
out += ['', '## r4190m (2초) → 30초', '', '처리량 K msg/s. 각 항목은 `binding 2초→30초; Framework 2초→30초`이며 양쪽 값의 원본은 구버전·신버전 aggregate JSON과 각 3회 result 파일이다.', '', '| 패턴 | 크기 | 언어 | binding 2초→30초 | Framework 2초→30초 | 원시 경로 |', '|---|---:|---|---:|---:|---|']
for p in patterns:
 for s in sizes:
  for l in langs:
   gi,bi,fi=impls[l]
   oB=row(old[l],bi,p,s)['median']['throughput_per_second']/1000; nB=row(agg[l],bi,p,s)['median']['throughput_per_second']/1000
   oF=row(old[l],fi,p,s)['median']['throughput_per_second']/1000; nF=row(agg[l],fi,p,s)['median']['throughput_per_second']/1000
   src=f'/root/jobs/fx/cx/v21/s033/r4190m/raw/aggregates/{l}.json; raw/aggregates/{l}.json; /root/jobs/fx/cx/v21/s033/r4190m/raw/bench/{l}/run{{1,2,3}}/{bi}-{p}-{s}/results.json; raw/bench/{l}/run{{1,2,3}}/{bi}-{p}-{s}/results.json; /root/jobs/fx/cx/v21/s033/r4190m/raw/bench/{l}/run{{1,2,3}}/{fi}-{p}-{s}/results.json; raw/bench/{l}/run{{1,2,3}}/{fi}-{p}-{s}/results.json'
   out.append(f'| {p} | {s} | {labels[l]} | {fmt(oB)}→{fmt(nB)} | {fmt(oF)}→{fmt(nF)} | `{src}` |')
out += ['', '## C++ Framework 절대 처리량 비교', '', '각 셀에서 C++ Framework 처리량과 다른 세 언어 중 가장 빠른 Framework를 비교했다 (K msg/s).', '', '| 패턴 | 크기 | C++ Framework | 가장 빠른 타 언어 Framework | C++ 대비 | 원시 경로 |', '|---|---:|---:|---|---:|---|']
for p in patterns:
 for s in sizes:
  cval=row(agg['cpp'],'zlink-framework-cpp',p,s)['median']['throughput_per_second']/1000
  candidates=[(row(agg[l],impls[l][2],p,s)['median']['throughput_per_second']/1000,labels[l],l) for l in ('dotnet','java','node')]
  val,label,l=max(candidates)
  src=f'raw/aggregates/cpp.json; raw/aggregates/{l}.json; raw/bench/cpp/run{{1,2,3}}/zlink-framework-cpp-{p}-{s}/results.json; raw/bench/{l}/run{{1,2,3}}/{impls[l][2]}-{p}-{s}/results.json'
  out.append(f'| {p} | {s} | {fmt(cval)} | {label} {fmt(val)} | {fmt(cval/val,3)}× | `{src}` |')
out += ['', '## Kotlin 보조 셀', '', '요청된 Kotlin 셀은 gRPC Kotlin 및 Framework Kotlin, request-serial 1024B, 3회다 (binding cell 없음).', '', '| 구현 | 처리량 K msg/s (3회 중앙값) | CPU µs/msg source/target (중앙값) | 원시 경로 |', '|---|---:|---:|---|']
for impl in ('grpc-kotlin','zlink-framework-kotlin'):
 files=[RAW/'bench/kotlin'/f'run{i}'/f'{impl}-request-serial-1024'/'results.json' for i in (1,2,3)]
 vals=[]
 for f in files:
  d=json.load(open(f)); vals.append(next(c['throughput_per_second'] for c in d['cells'] if c['implementation']==impl))
 name='grpc-kotlin' if impl=='grpc-kotlin' else 'zlink-framework-kotlin'
 csrc='grpc-kotlin' if impl=='grpc-kotlin' else 'zlink-framework-kotlin'
 cp=statistics.median(x['source_us_per_msg'] for x in cpu['kotlin'] if x['cell']==f'{name}-request-serial-1024')
 ct=statistics.median(x['target_us_per_msg'] for x in cpu['kotlin'] if x['cell']==f'{name}-request-serial-1024')
 out.append(f'| {impl} | {fmt(statistics.median(vals)/1000)} | {fmt(cp)}/{fmt(ct)} | `{", ".join(str(f.relative_to(JOB)) for f in files)}; raw/bench/kotlin/cpu-per-cell.json` |')
out += ['', '## A/A와 latency 참고', '', 'A/A는 r4190n에서 수행됐으나 per-run p50 샘플은 없어 보고할 수 없다. r4190n의 20개 `aa-clean` 표본에서 single-latency 중앙값 0.006109 ms (범위 0.003244–0.047212 ms), p95 중앙값 0.004688 ms (범위 0.003424–0.150528 ms), p99 중앙값 0.059520 ms (범위 0.016896–1.400832 ms)였다. 이 single-latency 중앙값은 p50이 아니다. 원시 근거: `../r4190n/report.md`, `/root/jobs/fx/cx/v21/s033/f4162/raw/aa-clean-01.log:5-7` 등.', '', '이번 셀 집합은 request-serial, request-backpressure, send-saturation이며 PUBSUB latency 셀은 없다. 따라서 catch-up 미도달로 무효인 PUBSUB latency 셀과 in-flight floor 참고값은 없다. 이번 결과에는 `late_sends_gt_1ms` 계측 필드가 없어 그 카운트를 판별할 수 없다. latency 판정에는 reference 값을 사용하지 않았다.', '', '## 요약', '', '- 모든 언어와 C 기준의 result metadata에 warmup 30초가 기록됐다. Warmup 관련 unsupported 판정은 없다.', '- C request-backpressure는 두 크기 모두 3회 judged runs를 제공했다. Main aggregator의 8개 Framework/binding 판정 중 C++ send-saturation 4096B는 C++ binding denominator의 G5 spread 21.9% (10% 한도 초과)로 unsupported 처리됐다. 이는 warmup 문제가 아니다.', '- Node 행 중 G6 결과가 `실행 모델 상한 도달`로 표시된 것은 main aggregator 결과 그대로 기록했다.', '- 보고서 숫자는 표별 원시 경로 열의 aggregate/result/CPU 파일에서 가져왔다. C++·.NET·Java·Node 집계 JSON은 main aggregator가 생성한 `raw/aggregates/` 산출물이다.', '']
text='\n'.join(out)
(JOB/'report.md').write_text(text)
(JOB/'docs-page.md').write_text(text)
