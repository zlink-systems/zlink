# Java Multi REQREP·DD 비용 지도

## 결론

- 고정 Core `0.17.3-alpha`를 같은 티켓에서 C와 Java에 적용해 다시 잰 결과, Java/C는
  REQREP 64~4096 B에서 52.71~61.20%, DD 64 B에서 71.33%였다.
- REQREP 64 B에서 독립적으로 분리한 가장 큰 후보는 completion worker handoff와 poller
  settlement wait의 합계 299.505 ns/op이며, C 대비 gap 2,304.106 ns/op의 13.00%다.
- 이 값은 Pass 1 진입 기준 20%에 못 미친다. handoff와 분리된 GC pause까지 뺀 86.39%는
  allocation 실행 비용, FFM/native 전이, reply materialization·복사와 동기화가 섞인 미분리
  묶음이다. 이를 하나의 dominant item으로 간주하지 않았다.
- 따라서 추측 최적화나 우회 구현을 하지 않았다. binding·runner 변경과 after 측정은 없으며,
  이 문서만 추가한다.

## 측정 경계

- Core source: `release`
- Core package prefix: `/home/hep7/.cache/zlink/core-pinned/0.17.3-alpha`
- 실제 C·Java runtime: `/home/hep7/.cache/zlink/core-pinned/0.17.3-alpha/lib/libzlink.so.0.17.2`
- 조건: TCP, 100 clients, 5 s, 1 run, 64/256/1024/4096 B, auto-HWM, 기존 timeout과
  상한을 그대로 사용했다.
- paired before는 티켓 하나에서 C를 먼저 실행하고 Java를 이어서 실행했다. Java는 지시대로
  첫 실행에서 `--reuse-build` 없이 빌드했다.
- 모든 benchmark·JFR·microbenchmark는 priority 2, owner `codex` perf ticket으로 실행했다.
- 표의 `ns/op`은 `10^9 / aggregate throughput`이다. 개별 요청 latency가 아니라 C와 Java의
  처리량 차이를 한 연산당 비용으로 정규화한 지표다.
- 단일 5초 run이므로 절대 성능 판정이 아니라 비용 후보의 크기와 다음 조사 순서를 정하는
  지도다. 기존 0.17.2 계획서 수치와 비교하지 않았다.

## Paired before

| pattern | bytes | C ops/s | Java ops/s | Java/C | C ns/op | Java ns/op | gap ns/op |
|---|---:|---:|---:|---:|---:|---:|---:|
| DD | 64 | 1,729,004.8 | 1,233,343.2 | 71.33% | 578.367 | 810.804 | 232.437 |
| DD | 256 | 1,226,578.0 | 985,517.2 | 80.35% | 815.276 | 1,014.696 | 199.419 |
| DD | 1024 | 1,005,775.2 | 1,002,056.4 | 99.63% | 994.258 | 997.948 | 3.690 |
| DD | 4096 | 654,686.4 | 643,319.4 | 98.26% | 1,527.449 | 1,554.438 | 26.989 |
| DEALER-ROUTER REQREP | 64 | 383,160.8 | 203,501.2 | 53.11% | 2,609.870 | 4,913.976 | 2,304.106 |
| DEALER-ROUTER REQREP | 256 | 348,081.4 | 200,980.0 | 57.74% | 2,872.891 | 4,975.619 | 2,102.728 |
| DEALER-ROUTER REQREP | 1024 | 363,182.8 | 195,976.0 | 53.96% | 2,753.434 | 5,102.666 | 2,349.231 |
| DEALER-ROUTER REQREP | 4096 | 330,437.4 | 178,501.2 | 54.02% | 3,026.292 | 5,602.203 | 2,575.911 |
| ROUTER-ROUTER REQREP | 64 | 365,738.4 | 192,791.6 | 52.71% | 2,734.195 | 5,186.948 | 2,452.753 |
| ROUTER-ROUTER REQREP | 256 | 336,620.0 | 192,929.0 | 57.31% | 2,970.709 | 5,183.254 | 2,212.545 |
| ROUTER-ROUTER REQREP | 1024 | 312,420.6 | 191,207.0 | 61.20% | 3,200.813 | 5,229.934 | 2,029.121 |
| ROUTER-ROUTER REQREP | 4096 | 299,147.2 | 176,600.0 | 59.03% | 3,342.836 | 5,662.514 | 2,319.678 |

원본은 `/tmp/zlink-java-reqrep-cost-map-alpha/before-c.txt`와
`/tmp/zlink-java-reqrep-cost-map-alpha/before-java.txt`다. 영속적인 ticket log는
`.artifacts/perf-queue/log/2-1788840206-27029-codex-java_reqrep_cost_map_alpha_paired_baseli.log`다.

## REQREP 64 B 비용 지도

DEALER-ROUTER 64 B를 대표 경로로 사용했다. C 2,609.870 ns/op, Java 4,913.976 ns/op,
gap 2,304.106 ns/op이다.

| 항목 | Java 빈도·비용 | C 대응 | gap 기여 | 판정 |
|---|---:|---:|---:|---|
| C와 공통인 Core·socket 처리 기준선 | 2,609.870 ns/op | 2,609.870 ns/op | gap 외 | paired C 기준선 |
| `Pending.awaitSettlement()`의 실제 monitor wait | 0.010594 wait/op, 186.315 ns/op | 없음 | 8.09% | JFR에서 분리 |
| `CompletionLane` queue·worker schedule | 1 logical task/op, 113.190 ns/task | 없음 | 4.91% | 동일 구현 microbenchmark 중앙값 |
| GC pause | client+server 14.26 ms, 14.163 ns/op | C binding 관리 heap 없음 | 0.61% | JFR에서 분리 |
| allocation 실행, FFM/native 전이, materialization·복사, 나머지 동기화 | 1,990.437 ns/op | 미분리 | 86.39% | 여러 원인이 섞인 잔여값 |

위 표에서 handoff 선별값은 두 관측을 보수적으로 더한 299.505 ns/op, gap의 13.00%다. JFR의
blocked time과 microbenchmark 중앙값은 같은 실행에서 동시에 계측한 값이 아니며 서로 겹칠 수도
있다. 따라서 이 합을 정밀 attribution으로 사용하지 않는다. 반대로 실제 callback body의 계약상
필수 작업도 microbenchmark에서 제외했으므로 제거 가능 비용의 상한이나 하한으로 단정하지 않는다.
확인된 dominant item이 아니라 다음 후보를 거르는 선별값으로만 사용한다.

### Completion handoff의 실제 모양

Java public poller는 completion을 drain한 뒤 settlement를 실행한다
(`NativePoller.java:247-281`). `CompletionOwner`는 결과를 capture한 뒤 settlement wait를
등록하고(`CompletionOwner.java:612-669`), terminal future completion을 socket별 lane에
dispatch한 뒤 필요할 때 기다린다(`CompletionOwner.java:1150-1181`). `CompletionLane`은
FIFO queue에서 한 task씩 worker에 다시 schedule한다
(`CompletionDispatcher.java:129-180`). C runner는 같은 poller thread에서 completion을
읽고 결과를 즉시 기록한다(`perf_multi_socket_reqrep.hpp:352-409`).

- Java logical executor task: 정확히 1/op, C: 0/op.
- Java completion worker의 실제 `ThreadPark`: 231,006 / 978,489 = 0.23608/op, C: 0/op.
- Java poller의 실제 `awaitSettlement` wait: 10,367 / 978,489 = 0.010594/op, C: 0/op.
- 따라서 “요청마다 두 번 park/unpark” 가설은 관측으로 기각됐다. queue에 일이 남아 worker가
  이어서 실행하는 경우가 많다.
- 100 lanes, 16 workers, 재사용 Runnable을 사용한 1,000,000-task microbenchmark 세 번은
  129.041, 113.190, 100.639 ns/task였고 중앙값 113.190 ns/task를 사용했다.

Handoff JFR ticket log는
`.artifacts/perf-queue/log/2-1788841020-78116-codex-java_reqrep_exact_handoff_JFR_DR_64_alph.log`,
lane microbenchmark log는
`.artifacts/perf-queue/log/2-1788843060-44332-codex-java_completion_lane_micro_correct_prefi.log`다.

## Allocation과 GC

JFR `ThreadAllocationStatistics`의 benchmark 구간 application-thread delta를 profile 실행의
처리 건수로 나눴다. 아래 byte 수는 Java managed allocation이다. Core 내부 native allocation은
양쪽에서 제외했다. JFR allocation sample은 object 수 전수 계측이 아니므로, object count는
source에서 성공·무재시도 hot path를 따라 센 보수적 하한이다.

| 경로 | client B/op | server B/op | 합계 B/op | source object 하한 | GC pause 정규화 |
|---|---:|---:|---:|---:|---:|
| DR REQREP 64 B | 1,018.262 | 524.242 | 1,542.504 | 25개/op 이상 | 14.163 ns/op |
| DD 64 B | 348.419 | 28.439 | 376.858 | 8개/msg 이상 | 2.938 ns/msg |
| C binding hot loop | 0 | 0 | 0 | 0 | 해당 없음 |

C의 0은 binding-owned application heap 기준이다. Core가 양쪽 공통으로 수행하는 내부 할당을
0이라고 주장하는 값이 아니다. Java object 하한도 FFM이 생성하는 `MemorySegment` wrapper와
배열 등 일부 runtime 객체를 제외하므로 실제 개수보다 작다.

REQREP client allocation sample 상위에는 `Message`, `NativeMemorySegmentImpl`, `Long`,
`Object[]`, `UniWhenComplete`, `CompletableFuture`, `Pending`, queue node와 completion lambda가
있었다. server에는 `Message`, `RoutingId`, reply storage, `Optional`, `ReplyToken`, `ArrayList`,
native routing-id materialization이 있었다. DD에서는 completion handoff가 없지만 376.858 B/msg가
남으므로 completion lane 하나로 Java 공통 비용을 설명할 수 없다.

GC pause만 환산하면 REQREP gap의 0.61%, DD 64 B gap의 1.26%다. 이는 allocation의 객체 생성,
초기화와 참조 관리 비용까지 0.61% 또는 1.26%라는 뜻이 아니다. 그 CPU 비용은 잔여 묶음에 남겼다.

Allocation JFR ticket log:

- REQREP: `.artifacts/perf-queue/log/2-1788842384-62915-codex-java_reqrep_allocation_only_JFR_DR_64_al.log`
- DD: `.artifacts/perf-queue/log/2-1788842738-1060-codex-java_DD_alloc_JFR_retry2.log`

## DD 64 B 비용 지도

DD는 C 578.367 ns/msg, Java 810.804 ns/msg, gap 232.437 ns/msg다.

| 항목 | Java 비용 | C 대응 | gap 기여 | 판정 |
|---|---:|---:|---:|---|
| C와 공통인 Core·socket 처리 기준선 | 578.367 ns/msg | 578.367 ns/msg | gap 외 | paired C 기준선 |
| Completion handoff | 0 | 0 | 0% | 성공 즉시 반환 경로에는 없음 |
| GC pause | 2.938 ns/msg | 해당 없음 | 1.26% | JFR에서 분리 |
| allocation 실행, FFM/native 전이와 나머지 runtime 비용 | 229.499 ns/msg | 미분리 | 98.74% | 여러 원인이 섞인 잔여값 |

DD 256 B 이상에서 격차가 급감하고 1024 B에서는 3.690 ns/msg뿐이다. 64 B 결과 하나를 모든
크기에 적용하는 고정비라고 단정할 수 없으며, 추가 attribution 없이 pass를 만들 근거가 없다.

## Native 경계 호출 수

성공하고 retry가 없는 2-part 경로를 source로 따라가며 poller wait와 마지막 `NO_DATA` probe를
제외한 application-to-Core ABI 호출 수의 보수적 하한을 셌다.

| 경로 | Java 하한 | C 하한 | Java/C | 해석 |
|---|---:|---:|---:|---|
| DR REQREP round trip | 40회/op 이상 | 25회/op 이상 | 1.60x | extra transition 후보, 시간 미분리 |
| DD one-way message | 20회/msg 이상 | 15회/msg 이상 | 1.33x | extra transition 후보, 시간 미분리 |

이 수에는 submit용 native 구조체 준비, part set, completion receive/close, message와 routing-id
materialization/close가 포함된다. 호출 수만으로 ns 비용을 추정하지 않았고 잔여 runtime 후보로만
분류했다.

## 계약 경계와 다음 조사 순서

| 분류 | 항목 | 처리 |
|---|---|---|
| public contract | request `CompletionStage`, owned reply `List<Message>`, 예외 전달 | 유지해야 함. 비용 제거 대상으로 간주하지 않음 |
| internal runtime policy | `Pending`, socket lane, worker dispatch, poller settlement wait | public signature는 아니지만 측정 합계가 13.00%라 이번 pass 대상 아님 |
| internal runtime | FFM wrapper, native 구조체 변환, routing-id/reply materialization | 계약 외 후보. 항목별 시간을 아직 분리하지 못함 |
| unconfirmed | allocation 생성·초기화 CPU, native downcall 비용, copy, lock 경쟁 | 잔여 86.39%를 구성하지만 개별 지배 항목은 확인되지 않음 |
| runner | timestamp·payload template과 집계 | 조건 유지. 변경하지 않음 |

다음 조사는 REQREP client/server를 나눠 allocation site별 실행 비용과 native downcall 누적 시간을
분리해야 한다. 그중 하나가 동일 paired 조건에서 gap의 20% 이상임을 확인한 뒤에만 Pass 1을
선정한다. timeout, sleep, client 수, 메시지 크기, HWM, 상한, in-flight 수는 바꾸지 않는다.

## Pass 1과 검증

- dominant item: 없음. 가장 큰 독립 측정 후보는 handoff 13.00%다.
- Pass 1 diff: 없음.
- before: 위 paired ticket 결과.
- after: 구현하지 않았으므로 없음.
- 수정 전/후 규칙 수: 변경 없음. completion worker handoff 1개와 조건부 settlement wait 1개를
  그대로 유지했다.
- 테스트: `bindings/java/gradlew --no-daemon -p bindings/java test :perf-multi:test` 성공
  (`BUILD SUCCESSFUL`, 18 tasks: 3 executed, 15 up-to-date).
- 높은 overhead 때문에 benchmark timeout이 난 CPU/allocation 혼합 JFR 실행은 timeout을 늘리지
  않고 폐기했다. 위 표에는 완료된 low-overhead 실행만 사용했다.
