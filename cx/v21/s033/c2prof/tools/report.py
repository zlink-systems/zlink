from pathlib import Path
import json
J=Path('/root/jobs/fx/cx/v21/s033/c2prof');R=J/'raw';c=json.loads((R/'comparison.json').read_text());profiles=json.loads((R/'profiles-summary.json').read_text())
items={}
for pattern,ss in c.items():
 entries={}
 for label,stage in [('decode','header decode (inclusive children)'),('encode','header encode (inclusive children)'),('Core residual','binding/Core'),('creation','request header creation'),('parts','envelope adapters/parts'),('reply header','reply header/parts')]:
  entries[label]={'cpu_delta_us':sum(ss[side]['2']['stages'].get(stage,0)-ss[side]['1']['stages'].get(stage,0) for side in ['source','target']),'shares':{side:{level:ss[side][level]['stage_sample_share'].get(stage,0) for level in ['1','2']} for side in ['source','target']},'stage':stage}
 # This clone is outside create_envelope (arguments evaluated by ladder encoder).
 selected=[r for r in profiles.values() if r['pattern']==pattern]
 us={};shares={}
 for level in [1,2]:
  rs=[r for r in selected if r['level']==level];us[level]=sum(sum(v['inclusive_us'] for sym,v in rr['roles']['source']['symbols'].items() if '_M_construct<char*>' in sym) for rr in rs)/len(rs)
  n=d=0
  for run in rs:
   a=json.loads((Path(run['path'])/'source.perf.aggregates-v2.json').read_text())
   for g in a['groups'].values():
    d+=g['weight'];n+=sum(v for sym,v in g['inclusive'].items() if '_M_construct<char*>' in sym)
  shares[level]=n/d*100
 entries['sender content_type copy']={'cpu_delta_us':us[2]-us[1],'source_share':shares,'symbol_pattern':'_M_construct<char*>','allocation_stack_evidence':'alloc-stacks-v2-*/source.alloc-collapsed-v3.txt: operator new -> _M_construct<char*> -> ladder::encoder_t::encode'}
 entries['creation+copy']={'cpu_delta_us':entries['creation']['cpu_delta_us']+entries['sender content_type copy']['cpu_delta_us'],'source_share':{l:entries['creation']['shares']['source'][str(l)]+shares[l] for l in [1,2]}}
 items[pattern]=entries
(R/'cost-items.json').write_text(json.dumps(items,indent=2))
rows=[]
def get(pattern,side,level,name,metric):return c[pattern][side][str(level)]['by_name'].get(name,{}).get(metric,0)
def arrow(pattern,side,name,metric,digits):return f"{get(pattern,side,1,name,metric):.{digits}f} → {get(pattern,side,2,name,metric):.{digits}f}"
for side,names in [('source',[f'bench-submit-{i}' for i in range(8)]+['ladder-binding','ZLINKbg/IO/0','ZLINKbg/IO/2']),('target',['bench_cpp_frame','ZLINKbg/IO/0','ZLINKbg/IO/1'])]:
 for name in names:
  rows.append('|'+side+' · `'+name+'`|'+ '|'.join(arrow(p,side,name,k,n) for p in ['request-backpressure','send-saturation'] for k,n in [('cpu_us_per_message',3),('context_switches_per_message',4)])+'|')
for side in ['source','target']:
 vals=[]
 for p in ['request-backpressure','send-saturation']:
  for k,n in [('cpu_us_per_message',3),('context_switches_per_message',4)]:vals.append(f"{c[p][side]['1'][k]:.{n}f} → {c[p][side]['2'][k]:.{n}f}")
 rows.append('|'+side+' 전체|'+ '|'.join(vals)+'|')
text='''# C++ L2 잔여 비용 진단 — 70158ead2a

PM의 다음 우선순위는 **canonical header decode와 request/reply header writer**다. 두 항목의 L2 추가 CPU 합계는 request-backpressure 1K에서 **5.064µs/message**, send 1K에서 **1.415µs/message**다. 특히 send target은 추가 문자열 할당 없이 decode에 1.067µs/message를 쓴다. 복사·할당 수만 줄여서는 이 비용이 없어지지 않는다. [계산·sample 원본 연결](raw/cost-items.json)

`C`는 이 worktree의 `framework/languages/cpp/framework/src/runtime/`, `B`는 `framework/bench/grpc/cpp/`, `D`는 .NET `src/Zlink.Framework/Runtime/`이다. 아래 file:line은 **현재 소스의 호출·구현 위치**다. client/server/Core 모두 `.symtab`은 있으나 `.debug_*`가 없다. DWARF unwind는 가능했지만 instruction→source line은 얻지 못했다. [binary hash·section 검사](raw/binary-provenance.json)

## 표 1 — 남은 비용 순위

request-backpressure의 L2−L1 CPU 순이다. CPU는 같은 활성 구간의 **thread별 `perf stat task-clock` ÷ 해당 side 완료 수**를 call-tree sample period로 배분한 뒤 두 반복을 평균했다. S=source, T=target. Sample %는 side 전체 대비 inclusive call-tree 비중이며 항목끼리는 중복을 제거했다. 생성 행은 `create_envelope`와 그 호출 전 content_type 복사 subtree를 합쳤다. [계산](raw/cost-items.json), [반복별 CPU·symbol](raw/profiles-summary.json)

| 순위 · 항목 | Δ CPU µs/msg BP / send | file:line | sample % L1 → L2: BP; send | 추가 malloc/msg | .NET 대응 — d2l2 최종 보고서 기준 |
|---|---:|---|---|---|---|
| 1. canonical parse·UTF-8 검사·Header 구성/이동 | **2.981 / 1.067** | C`messaging/envelope_codec.cpp:702,956,1000,1258`; B`ladder/ladder_wire.hpp:259,340` | BP S 0→4.95, T 0→10.19; send T 0→22.27 | BP S 약1(owning content_type, 31B), T 0; send 0 | D`Messaging/ZLinkEnvelopeCodec.cs:414,637`: UTF8 reader·직접 field mapping. request decode 5 objects/328B, reply 4/280B. C++ target의 무할당 decode도 CPU가 남는다. |
| 2. header 검증·size/write 두 pass | **2.083 / 0.349** | C`messaging/envelope_codec.cpp:241,257,438,477,527,557`; C`channels/channel_reply_writer.cpp:41`; B`ladder/ladder_wire.hpp:152,292` | BP S 0→3.73, T 0→5.99; send S 0→6.42 | 새 native header 1회는 L1에도 있음. 반복 key/cache 할당 증거 없음 | D`Messaging/ZLinkEnvelopeCodec.cs:188,1203`: 준비한 prefix+동적 field, size/write 두 pass. 최종 .NET deadline format은 1회. |
| 3. binding/Core 잔여 증가 — 원인 미확정 | **1.772 / 0.049** | 진입 C`mesh/raw_mesh_node_owner.cpp:540`; B`ladder/ladder_driver.inc:129`, B`ladder/ladder_wire.hpp:308`; 더 긴 header는 C`messaging/envelope_codec.cpp:438` | BP S 56.71→51.70, T 79.52→67.02; send S 65.37→62.50, T 78.56→61.59 | Core pool·in-flight 차이와 섞여 분리 불가 | .NET도 native transport/completion을 사용한다. d2l2에는 이 구간의 대응 µs 수치가 없다. 새 Framework 정책의 증거로 쓰지 않는다. |
| 4. 송신 Header 생성·content_type 소유 | **0.465 / 0.148** | C`messaging/client_call_codec.cpp:16,34,94`; B`ladder/ladder_wire.hpp:146,149` | BP S 0→1.41; send S 0→2.73 | S 약1(23B), T 0. ID는 SSO, deadline은 inline 20B | D`Messaging/ZLinkClientCallCodec.cs:7`: Header+ID, deadline 값. request 생성+encode 총3 objects/216B. .NET은 기존 이름 string을 참조한다. |
| 5. 두 part vector·adapter | **0.167 / 0.075** | C`messaging/envelope_codec.cpp:24,113`; C`messaging/client_call_codec.cpp:65,73`; B`ladder/ladder_wire.hpp:162,295` | BP S 0→0.32, T 0→0.43; send S 0→1.38 | BP S 약1+T 약1; send S 약1. 각160B | .NET `TwoMessageParts` wrapper. reply 생성+encode는 Header+Message+parts 총3 objects/216B. C++은 vector를 만든 뒤 bench array로 옮긴다. |
| 6. reply Header view 조립 | **0.082 / 0** | C`channels/channel_reply_writer.cpp:12`; B`ladder/ladder_wire.hpp:293` | BP T 0→0.58 | 0 | D`Channels/ZLinkChannelReplyWriter.cs:34`: prepared Header clone. C++은 이미 borrowed view를 쓴다. |

1·2·5·6은 production 함수의 비용이다. 1의 **source owning decode 선택**, 4의 **이미 plan이 소유한 content_type을 다시 복사하는 호출**, 5의 **vector→bench array 변환**은 bench 호출 형태와 구분해야 한다. 3은 BP target +1.944µs와 source −0.172µs의 합이다. header 크기·batching·CPU 경합이 함께 바뀌므로 추가 원인이 확정된 항목으로 보지 않는다. 나머지 공통 allocation/sync/driver 비용은 감소한 항목도 있어 위 양의 비용을 총 CPU 차이와 그대로 같게 만들 수 없다.

## 표 2 — thread별 CPU·context switch

각 셀은 L1 → L2, CPU는 µs/message다. 이름이 같은 thread는 합쳤다. 아래에 없는 저활동 thread와 **각 TID의 원본**도 [profiles-summary.json](raw/profiles-summary.json)에 있다. 모든 이름의 self/inclusive **top 25 symbol 비교**는 [표](raw/thread-top25.md)·[CSV](raw/thread-top25.csv)에 있다. Inclusive symbol은 중첩되므로 합산하지 않는다. 개별 TID top25는 [CSV](raw/thread-top25-by-tid.csv), 같은 이름의 생성 순번을 유지한 L1/L2 비교는 [CSV](raw/thread-top25-by-slot.csv), 모든 thread CPU는 [CSV](raw/thread-cpu-by-tid.csv)에 있다.

| side · thread name | BP CPU | BP switches/msg | send CPU | send switches/msg |
|---|---:|---:|---:|---:|
'''+ '\n'.join(rows)+'''

call-stack sampling 없는 별도 thread-stat 두 반복의 범위도 확인했다. BP source L1 27.57–41.26 → L2 44.29–47.89, target 8.73–11.68 → 16.49–17.25µs/message였다. send source 5.23–5.55 → 6.47–6.65, target 3.81–4.37 → 5.73–6.01이었다. BP의 공통 CPU는 변동이 커서 순위는 반복에서 안정적인 **직접 header call-tree 비용**을 우선한다. 이 실행으로 7% 예산이나 throughput 개선률을 재판정하지 않는다. [전체 값](raw/threadstat-summary.json)

## 할당·대기와 PM 수정의 적용 범위

고정 active 4초 LD_PRELOAD 계수의 malloc/message(S / T)는 BP **12.975→16.652 / 7.664→8.897**, send **7.543→9.451 / 2.560→2.558**이다. free/message는 BP **13.034→16.633 / 7.761→9.014**, send **7.589→9.517 / 2.592→2.590**이다. calloc/realloc은 모두0이다. C++ new/delete는 malloc/free를 거치는 것을 별도 probe로 확인했으며 이 수에 포함한다. 별도의 new/delete 횟수로 중복 집계하지 않았다. aligned allocator 전체를 포괄하는 계수기는 아니다. [계수·thread·분모](raw/allocations-summary-v3.json), [계수기 양방향 검사](raw/counter-check.log), [계수기 소스](tools/window-counter.c)

1/256 무작위 allocation backtrace에서 L2 source의 `operator new → _M_construct<char*> → ladder::encoder_t::encode`(23B), `canonical_header_reader_t<false>::string → _M_mutate`(31B), source/target의 `message_parts_t` constructor(160B)를 확인했다. 표의 약1회는 이 sample 추정과 호출 경로를 함께 표시한 값이다. 정확한 process 전체 계수와 다르며 Core pooling·in-flight의 잔차를 임의로 이 세 호출에 배분하지 않았다. 추가로 request observer coroutine은 L1 168B→L2 176B로 커졌으나 allocation 횟수는 같다(B`client/bench_cpp_client.cpp:473` 대 B`ladder/ladder_driver.inc:134`, level capture 추가). native header malloc 요청 크기도 L1 241B→L2 command306B, request326/327B, reply308/309B로 늘었다. 이 수는 wire 길이가 아니라 allocator 요청 크기다. [BP source](raw/alloc-stacks-v2-L2-request-backpressure-r1/source.alloc-collapsed-v3.txt), [BP target](raw/alloc-stacks-v2-L2-request-backpressure-r1/target.alloc-collapsed-v3.txt), [send source](raw/alloc-stacks-v2-L2-send-saturation-r1/source.alloc-collapsed-v3.txt), 각 디렉터리의 `*.alloc-stacks.bin`·`*.maps`

`b5afcf4c55`의 command view 수정은 C`messaging/client_call_codec.hpp:63`, prepared request 이름 생략은 C`channels/channel_runtime.cpp:1900`, completion view 수정은 C`messaging/client_call_codec.hpp:107`·C`channels/channel_runtime.cpp:263,1819,2078,2243`에 있다. **L2 ladder는 이 호출자들을 실행하지 않는다.** 송신은 B`ladder/ladder_wire.hpp:146`의 owning 인수를 가진 `create_envelope`이며, reply completion은 B`:340`의 owning `decode_header`다. 따라서 PM의 “송신 복사12→3, completion 할당4→0” 검사는 이 ladder의 실제 감소 수치가 아니다. 이번에는 수정 전 바이너리를 실행하지 않았으므로 throughput 미개선의 단독 원인까지 확정한 비교는 아니다. 공통 view decoder는 target에서 이미 사용하지만 위의 parse·검증·Header 이동 비용은 유지된다. nlohmann SAX/DOM symbol은 최종 L2 표본에 없었다. JSON body decoder를 다시 고칠 근거도 없다. [PM diff](raw/pm-fix.diff), [call-tree 원본](raw/profiles-summary.json)

새 L2 wait/queue는 확인되지 않았다. 양 level 모두 C`mesh/raw_mesh_node_owner.cpp:535`의 같은 permit 대기·`:540`의 recv, B`ladder/ladder_driver.inc:129`의 같은 binding request와 B`:134`의 inline completion을 쓴다. 실제 `mesh_record_dispatcher.cpp:32` dispatch는 이 ablation 단계에서 호출하지 않는다. L2는 completion callback 안에서 decode를 더 수행한다. binding은 `bindings/cpp/src/Runtime/Messaging/completion_owner.cpp:531`에서 entry lock을 푼 뒤 completion을 호출한다.

`perf trace -s -e futex`의 calls/message는 BP S **1.471→1.473**, T **0.0878→0.0978**, send S **0.0519→0.0377**, T **0.00305→0.00319**였다. 별도 syscall-stat의 sched_yield/message는 BP S **3.637→3.863**, send S **0.3027→0.3049**였다(T는0). CPU migration/message는 표2와 같은 perf window에서 BP S **0.0292→0.0354**, T **0.00613→0.00942**, send S **0.00103→0.00224**, T **0.000515→0.00117**이다. futex 횟수와 CPU migration은 별도 관측치이며 새로운 lock/대기 소유자를 뜻하지 않는다. futex summary의 누적 시간은 여러 thread의 **대기 wall time**으로 CPU 비용에 더하지 않았다. [syscall·futex thread 요약](raw/syscalls-summary.json), [원본 BP source L1](raw/trace-L1-request-backpressure-r2/source.trace.futex.txt)·[L2](raw/trace-L2-request-backpressure-r2/source.trace.futex.txt), [전체 profile CPU](raw/comparison.json)

## 조건·원시 자료·남은 한계

- HEAD `70158ead2a`, branch `exp/4350-cpp-fixes`. 기존 Release 바이너리만 사용했다. 두 process와 모든 관측 thread는 **CPU6–7,14–15를 함께 사용**했다. 이 머신에서 6/7과14/15는 각각 SMT 형제이므로 물리 core 두 개를 공유한다. [CPU topology 원본](raw/cpu-topology.json)
- 각 process는 SIGCONT를 보내지 않는 **20초 hard KILL timer**로 제한했다. warmup1초·active5초·run 간 gap3초다. 최종 perf는 source/target에 각각 별도 recorder를 동시에 붙여 `cpu-clock -F2999 -g --call-graph dwarf,8192 --clockid mono`로 **활성 구간 내부 약4.04–4.21초**, level×pattern×side마다2회 수집했다. CPU stat도 해당 구간에 켰다. 수동 HTTP boundary가 원자적이지 않은 오차와 profiler의 CPU 경쟁이 남는다.
- source 분모는 해당 구간 완료 수, target 분모는 해당 구간 처리 완료=수신 수다. BP는 reply backlog가 있어 중간 source/target 수가 다를 수 있다. **모든 run 종료 후 제출=완료=수신, errors/validation failures/abandoned=0**을 확인했다. 최종 분석32run 전체 audit: [run-audit.json](raw/run-audit.json).
- 최종16개 perf 파일은 lost warning0, unknown leaf0.74–3.72%였다. 기본 perf clock과 monotonic clock이 다른 초기 수집, startup 실패, exec 전에 preload가 taskset에 걸려 계수가 비었던 초기 allocation 실행은 **최종 판정에서 제외하고 모두 보존**했다. 제외 사유는 audit에 있다.
- 모든 수치의 직접 원본: `raw/perf-final-L{1,2}-{request-backpressure,send-saturation}-r{1,2}/`의 `source/target.perf.data`, `*.stat.csv`, `before/after.json`, `validated.json`; 할당은 `raw/alloc-v2-*`, attribution은 `raw/alloc-stacks-v2-*`, 비표본 CPU는 `raw/threadstat-*`, syscall은 `raw/trace-*`. 재현 도구는 [profile.py](tools/profile.py), 분석은 [analyze.py](tools/analyze.py)·[compare.py](tools/compare.py)·[alloc-analyze.py](tools/alloc-analyze.py)다.
- .NET 비교는 [d2l2/report.md](../d2l2/report.md)의 최종 after 표를 사용했다. 그 managed object/B 계수와 이번 native malloc 계수는 범위가 다르므로 직접 비율로 비교하지 않았다. supervisor의 throughput 원본은 [lfx-cpp measurements](../lfx-cpp/raw/measurements.jsonl)이며 이번 profile 수치와 구분한다.

제품 코드·worktree·build 변경0, hash 변경0. push·GitHub·sub-agent·다른 AI 도구·internal-docs·fetch/pull·measure.lock·SIGCONT 사용0. Raw 삭제0. 남은 실패는 위의 보존된 초기 수집 실패이며 최종 workload 실패0이다. PM은 **production decode/writer의 실제 CPU부터 검토하고, ladder의 호출 차이를 별도로 정리**해야 한다. 이 보고서는 수정이나 성능 통과 판정을 포함하지 않는다.
'''
(J/'report.md').write_text(text)
print(J/'report.md')
print(json.dumps(items,indent=2))
