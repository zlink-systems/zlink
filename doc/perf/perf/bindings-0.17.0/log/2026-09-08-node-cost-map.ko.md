# Node Multi 비용 지도와 DD raw receive 객체 제거

## 결론

고정 Core `core/v0.17.3-alpha`에서 C와 Node를 같은 시각에 다시 재어 비교했다. 64 B
기준 Node의 메시지당 시간은 DD가 1,753.3 ns(C 583.5 ns), DR REQREP가
7,192.5 ns/op(C 2,616.7 ns/op)이었다. DD receiver의 `recv`가 CPU sample의
75.12%를 차지했다. C의 전체 583.5 ns를 이 구간의 공통 Core 비용으로 모두 빼도
733.7 ns, 곧 DD 격차의 62.7%가 남는다. 이번 조사에서 가장 큰 단일 병목이다.

DD의 2-part receive는 N-API 안에서 `envelope -> part snapshot 2개`를 만들고
TypeScript에서 다시 `Message[]`로 바꾸고 있었다. 공개 결과에 필요 없는 이 중간 객체
3개를 없애고 N-API가 `[Buffer, Buffer]`를 바로 넘기게 했다. 공개 `Received.parts`와
`Message` ownership은 그대로다. 1-run after에서 DD의 C 대비 기하평균 비율은
41.04%에서 44.94%로 3.90%p 올랐고, 다섯 크기의 Node throughput은 모두 증가했다.

DR에는 이 fast path가 적용되지 않는다. DR의 after 변화는 크기별 -6.45%~+11.18%로
엇갈렸고 C 대비 기하평균 비율은 31.37%에서 31.96%였다. 이를 대조군 변동으로 본다.

## 측정 범위와 기준

- source commit: `7cddf82627`
- Core source: `release`
- Core package prefix: `/home/hep7/.cache/zlink/core-pinned/0.17.3-alpha`
- Core revision: `d6432ec4fa8c82787a0b35b4295b32e516af7440`
- transport `tcp`, client 100, duration 5초, run 1회
- message size: 64, 256, 1024, 4096, 65536 B
- pattern: `MULTI_DEALER_DEALER`, `MULTI_DEALER_ROUTER_REQREP`
- application part 수: 2

계획서의 0.17.2 측정값은 쓰지 않았다. before와 after 모두 각 실행 안에서 C를 먼저
재고 이어서 Node를 쟀다. `ns/message = 10^9 / throughput`으로 계산했다. REQREP의
한 operation은 request와 reply가 끝난 왕복 1회다.

before ticket은
`2-1788840211-27214-codex-Node_cost_map_before_paired_C_then_Node_`, after ticket은
`2-1788841320-90201-codex-Node_cost_map_after_paired_C_then_Node_D`이며 둘 다 rc=0이다.

## before 처리량 지도

### DD

| 크기 | C (ns/msg) | Node (ns/msg) | 격차 (ns/msg) | Node/C |
|---:|---:|---:|---:|---:|
| 64 | 583.5 | 1,753.3 | 1,169.8 | 33.28% |
| 256 | 696.2 | 1,966.7 | 1,270.5 | 35.40% |
| 1,024 | 849.8 | 2,142.5 | 1,292.7 | 39.66% |
| 4,096 | 1,485.3 | 2,936.9 | 1,451.6 | 50.57% |
| 65,536 | 6,105.1 | 12,385.5 | 6,280.4 | 49.29% |

64~4,096 B에서 고정 격차가 1.17~1.45 us/msg다. 크기보다 호출과 객체 수에 더
가까운 모양이다. 다섯 크기의 Node/C 산술평균은 41.64%, 기하평균은 41.04%다.

### DR REQREP

| 크기 | C (ns/op) | Node (ns/op) | 격차 (ns/op) | Node/C |
|---:|---:|---:|---:|---:|
| 64 | 2,616.7 | 7,192.5 | 4,575.8 | 36.38% |
| 256 | 2,676.8 | 7,975.8 | 5,299.0 | 33.56% |
| 1,024 | 2,888.8 | 9,235.3 | 6,346.5 | 31.28% |
| 4,096 | 3,038.8 | 10,888.2 | 7,849.4 | 27.91% |
| 65,536 | 16,213.4 | 56,891.2 | 40,677.8 | 28.50% |

다섯 크기의 Node/C 산술평균은 31.53%, 기하평균은 31.37%다.

원본 report:

- C before: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260908_130802_node-cost-map-before-c.txt`
- Node before: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260908_131018_node-cost-map-before-node.txt`

## 64 B CPU 비용 지도

`--cpu-prof`는 ticket
`2-1788840718-56959-codex-Node_cost_map_corrected_CPU_profiles_DD_`로 실행했다.
profile 자체의 부담이 있으므로 absolute time은 unprofiled before의 ns/message를 쓰고,
각 항목의 비율만 CPU self sample에서 가져왔다. 따라서 다음 ns는 attribution 추정치다.

DD는 throughput을 세는 receiver profile을 사용했다. C 전체 시간을 `recv` 구간의 공통
Core 비용으로 먼저 빼는 가장 보수적인 계산을 했다. 이 계산으로도 raw receive 구간이
격차의 62.7%다.

| DD receiver 항목 | Node 시간 (ns/msg) | C를 뺀 격차 기여 | 격차 비율 | 판정 |
|---|---:|---:|---:|---|
| N-API `recv` + raw 객체 생성 | 1,317.2 | 733.7 | 62.7% | **병목**, binding 내부 |
| idle | 205.6 | 205.6 | 17.6% | 대기 sample, 실행 비용 아님 |
| timestamp/metric 기록 | 77.4 | 77.4 | 6.6% | benchmark 계측 |
| JS materialize/replace/close | 44.6 | 44.6 | 3.8% | binding 내부 |
| GC | 20.3 | 20.3 | 1.7% | allocation 결과 |
| 나머지 | 88.3 | 88.3 | 7.5% | 개별 항목 1% 미만 |
| **합계** | **1,753.3** | **1,169.8** | **100.0%** | C 583.5 ns 제외 |

DR client는 request 완료를 세는 쪽이다. `submitRequest`, completion `drain`과 entry
정리를 묶은 구간이 3,736.0 ns/op이다. 여기서도 C 전체 2,616.7 ns/op을 빼면
1,119.3 ns/op, 격차의 24.5%가 남는다.

| DR client 항목 | Node 시간 (ns/op) | C를 뺀 격차 기여 | 격차 비율 | 판정 |
|---|---:|---:|---:|---|
| request submit + Promise completion | 3,736.0 | 1,119.3 | 24.5% | **병목**, Promise terminal 계약 포함 |
| idle + completion poll wait | 2,232.3 | 2,232.3 | 48.8% | peer 응답 대기 포함, 실행 비용 아님 |
| timestamp/metric 기록 | 357.2 | 357.2 | 7.8% | benchmark 계측 |
| GC | 200.8 | 200.8 | 4.4% | allocation 결과 |
| 나머지 | 666.2 | 666.2 | 14.6% | 여러 작은 JS 경로 |
| **합계** | **7,192.5** | **4,575.8** | **100.0%** | C 2,616.7 ns 제외 |

동시에 도는 DR server profile도 `recv + replyDirect`가 61.54%였다. 같은 보수 계산을
따로 적용하면 4,426.4 - 2,616.7 = 1,809.7 ns/op, 격차의 39.6%다. client와 server는
동시에 실행되므로 두 수를 더하지 않는다. 이는 routed receive/reply가 별도 병목이라는
교차 확인이다.

## allocation, 경계 호출과 thread 전환

allocation sampling profile은 ticket
`2-1788840718-56971-codex-Node_cost_map_corrected_allocation_profi`로 얻었다. GC가 끝난
뒤 살아 있는 sample만 남으므로 allocation rate의 총량으로 쓰지 않았다. 대신 실제 N-API
shape와 같은 V8 객체 50만 개를 보존하고 전후 heap 차이를 재는 모델을 사용했다. 이 값은
public wrapper, builder, closure를 제외한 **raw transfer 하한**이다.

| 64 B 항목 | C 언어층 | Node 횟수 | Node retained B | 비고 |
|---|---:|---:|---:|---|
| DD raw receive before | 0 | 6 objects/msg | 608.0 B/msg | envelope 1 + array 1 + snapshots 2 + Buffers 2 |
| DD raw receive after | 0 | 3 objects/msg | 512.0 B/msg | array 1 + Buffers 2 |
| DD pass 차이 | 0 | **-3 objects/msg** | **-96.0 B/msg** | payload 64 B copy는 유지 |
| DD settled Promise 하한 | 0 | 3.02 objects/msg | 145.0 B/msg | 48.0 B/Promise 모델 |
| DR server routed receive | 0 | 9 objects/op | 735.2 B/op | 2-part, shared properties, identity string, token 포함 |
| DR client completion receive | 0 | 5 objects/op | 456.0 B/op | completion envelope, ids, parts array, Buffer 포함 |
| DR raw 왕복 합계 | 0 | **14 objects/op** | **1,191.3 B/op** | Promise/public wrapper 제외 |
| DR settled Promise 하한 | 0 | 8.04 objects/op | 385.9 B/op | 48.0 B/Promise 모델 |

allocation shape ticket은 DD
`2-1788841499-1007-codex-Node_DD_raw_receive_shape_retained_alloc`, DR
`2-1788841830-19891-codex-Node_DR_REQREP_raw_native_receive_comple`이며 모두 rc=0이다.
작은 Buffer backing store는 이 V8 버전에서 `heapUsed`에 잡혔다. 따라서 표의 B는
`heapUsed + external` 차이다.
settled Promise 200만 개를 따로 보존한 하한 모델 ticket
`2-1788842142-40458-codex-Node_retained_bytes_per_settled_Promise_`도 rc=0이다. Promise
reaction, closure와 public `Message` wrapper는 여전히 표에 포함되지 않는다.

`async_hooks`와 native 함수 wrapper로 호출 횟수를 셌다. before 계수 ticket은
`2-1788840786-66446-codex-Node_cost_map_exact_event_counters_retry`, OS context switch를
더한 확인 ticket은
`2-1788841937-26834-codex-Node_cost_map_exact_NAPI_Promise_event-l`이다. 계수 자체가
throughput을 낮추므로 횟수만 사용한다.

| 64 B 항목 | C 언어층 | Node DD (/msg) | Node DR (/op) |
|---|---:|---:|---:|
| JS -> N-API 호출 | 0 | 2.000 | 8.025 |
| `PROMISE` async resource init | 0 | 3.02 | 8.04 |
| binding이 별도 worker로 보내는 명시적 handoff | 0 | 0 | 0 |
| `setImmediate` event-loop 재개 | 해당 없음 | 0.0100 | client 0.0100 + server 0.00536 |
| voluntary OS context switch | 미측정 | 0.307 | 1.123 |
| involuntary OS context switch | 미측정 | 0.000205 | 0.000295 |

OS context switch는 `process.resourceUsage()`의 process 전체 값이라 Core I/O thread와
시작·종료를 포함한다. binding의 논리 handoff와 같다고 해석하지 않는다. C도 같은 방식으로
분리할 수 있는 `perf`가 설치되어 있지 않았다. `perf stat` ticket
`2-1788840305-28751-codex-Node_cost_map_C_and_Node_perf_stat_conte`는 `perf: command not
found`로 rc=127이어서 판정에 쓰지 않았다.

DR의 8.025 N-API 호출/op은 client 5.993회와 server 2.032회의 합이다. client 쪽에는
request submit 1회, completion receive 1.797회, poll wait 0.010회와 poll event field
조회 3.186회가 들어간다. server 쪽에는 receive 1.005회, reply 1회, poll wait와 event
조회 0.027회가 들어간다. C는 같은 Core 작업을 하지만 JS/N-API 경계는 없다.

DD sender는 한 event-loop turn에 약 100건을 제출한다. receiver는 cap 없이 EAGAIN까지
비우며 before 계수 run에서는 successful poll wake 1회당 약 317,700건을 받았다. DR
client는 outer turn당 99.97개 completion을 받았고 server는 wake당 약 188.1개 request를
비웠다. 따라서 client receive를 1건씩 제한한 고정 cap은 관찰되지 않았다.

## 계약 경계와 pass 1

Node 계약은 receive가 호출자가 준 `Received`를 채우고, multipart를 `Message[]`로
노출하도록 정한다(`bindings/doc/spec/node/README.ko.md:499-525`). 또한 send terminal은
`Promise<void>`, request terminal은 `Promise<Message[]>`다
(`bindings/doc/spec/async-coroutine-policy.ko.md:87-95`). 다음 항목은 계약으로 표시하고
이번 변경에서 건드리지 않았다.

- `Received.parts: Message[]`와 part ownership/lifetime
- receive payload를 JS-owned Buffer로 복사하는 규칙
- send/request Promise terminal과 request completion
- routed identity, `ReplyToken`, message properties

변경한 것은 공개 객체가 아니라 N-API와 TypeScript materializer 사이의 private raw
shape뿐이다.

- `bindings/node/native/src/addon_core.cc:659`: routing ID가 없는 multipart receive는
  envelope와 part snapshot을 만들지 않고 Buffer array를 반환한다.
- `bindings/node/src/zlink/runtime/messaging/message_materializer.ts:45`: private raw union에
  Buffer array를 추가한다.
- `bindings/node/src/zlink/runtime/messaging/message_materializer.ts:137`: Buffer array를
  기존 `messageFromOwnedBuffer`로 감싸 공개 `Message[]`를 만든다.

수정 전 규칙은 `native envelope 생성 -> part snapshot 생성 -> snapshot materialize` 세
단계였다. 수정 후 규칙은 routing metadata가 없는 경우 `owned Buffer array -> Message[]`
한 단계이고, routed 경로만 기존 envelope 규칙을 쓴다. 같은 상태나 retry 규칙을 추가하지
않았다.

소유 계층은 Node binding의 N-API transfer와 TypeScript materializer다. C runner는
`zlink_msg_t`를 직접 소비하여 JS 객체를 만들지 않는다. 이 차이는 Core나 protocol 차이가
아니라 Node 표현 계층의 구조적 차이다. 변경 분류는 **B(기존 결함)** 이다. public 계약에
쓰이지 않는 중간 표현을 매 메시지마다 만들던 결함을 제거했다.

## after 결과

| 크기 | DD before Node/C | DD after Node/C | Node throughput 변화 | Node 절약 (ns/msg) |
|---:|---:|---:|---:|---:|
| 64 | 33.28% | 35.16% | +4.64% | 77.7 |
| 256 | 35.40% | 36.52% | +0.43% | 8.4 |
| 1,024 | 39.66% | 43.68% | +4.69% | 96.0 |
| 4,096 | 50.57% | 56.44% | +2.70% | 77.2 |
| 65,536 | 49.29% | 57.88% | +9.62% | 1,086.8 |
| **기하평균** | **41.04%** | **44.94%** |  |  |

after의 paired C도 새로 쟀으므로 Node/C 판정은 각 열 안의 C를 쓴다. 절약 ns는 Node
before와 Node after의 `10^9 / throughput` 차이다. 1-run이므로 작은 차이의 신뢰 구간은
없지만, 대상 DD의 다섯 크기가 모두 같은 방향이고 변경이 닿지 않은 DR은 엇갈렸다.

| 크기 | DR before Node/C | DR after Node/C | Node throughput 변화 |
|---:|---:|---:|---:|
| 64 | 36.38% | 37.21% | +0.64% |
| 256 | 33.56% | 36.45% | +3.20% |
| 1,024 | 31.28% | 33.77% | +11.18% |
| 4,096 | 27.91% | 26.22% | -6.12% |
| 65,536 | 28.50% | 27.75% | -6.45% |
| **기하평균** | **31.37%** | **31.96%** |  |

원본 report:

- C after: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260908_132422_node-cost-map-after-c.txt`
- Node after: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260908_132635_node-cost-map-after-node.txt`

DD mean latency는 64 B에서 1,567.6 ms에서 810.3 ms, 256 B에서 1,159.0 ms에서
746.9 ms로 줄었지만, 각 before C의 0.0716 ms와 1.2233 ms에는 아직 훨씬 못 미친다.
다른 크기도 backlog에 따라 변동이 크다. 이번 pass는 throughput 병목 하나만 대상으로
삼았으므로 latency 전용 handoff, timeout, client 수, HWM, in-flight 수는 바꾸지 않았다.

## 검증과 남은 비용

- `npm run rebuild-native && npm run build:incremental`: 통과
- 관련 11개 test: 10개 통과. `pair` multipart, routed multipart, multipart hot path,
  DD TCP/WSS 계약은 통과했다.
- `npm test`: 앞선 suite는 통과했고 `multipart.test.js`의 알려진 native stress assertion
  `counts.rejected_einval > 0n`에서 중단했다. 요청에 적힌 기존 실패와 같다.
- 고정 Core 환경에서 `multipart.test.js`를 제외한 나머지 30개 test 파일: 모두 통과
- `git diff --check`: 통과

남은 격차는 다음과 같이 분류한다.

| 범주 | 남은 항목 | 상태 |
|---|---|---|
| 계약 | Promise terminal, `Message[]`, JS-owned receive Buffer, routed metadata/token | 유지 |
| Node runtime/binding | DD payload Buffer copy와 public wrapper materialize, DR completion owner와 poll event 조회 | 측정됨, 이번 pass 범위 밖 |
| Core 공통 | 실제 socket send/recv/reply/completion과 Core I/O thread | C에도 존재 |
| 미확정 | C 대비 OS context switch 차이, V8 transient allocation 총량, DD 초 단위 backlog의 독립 인과 | `perf` 부재와 1-run 한계로 미확정 |

DR의 Promise/completion 및 routed receive/reply도 각각 보수 계산에서 20%를 넘지만, DD raw
receive가 더 큰 단일 병목이었고 요청은 한 pass만 허용한다. 그중 Promise와 공개 reply
shape는 계약 경계이기도 하다. 따라서 추가 상태, timer, retry, in-flight cap이나 두 번째
최적화는 넣지 않았다.
