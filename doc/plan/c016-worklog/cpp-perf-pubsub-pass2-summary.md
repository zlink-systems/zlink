# C++ binding Multi PUBSUB read-only review — pass 2

## 결론

추천은 **binding 변경 no-go**다. 공개 시그니처, message ownership, 실패 시 output 보존을
유지하면서 처리량을 5% 이상 높일 후보를 찾지 못했다. 확인된 C++ 고정 비용은
`socket_t::subscribe_part()`가 두 part 각각에 대해 C API를 감싸고 topic과 output ownership을
C++ 객체로 옮기는 비용이다. 64B callgrind에서 Core 호출을 뺀 wrapper 비용은
`5,741,586 Ir / 26,715 calls = 214.9 Ir/call`, 즉 약 `429.8 Ir/message`다. 이 비용을 전부
없애야 client 함수 기준 약 5.7%인데, 전부 없애려면 공개 output 의미를 줄여야 한다.
4096B profile에서도 wrapper 순비용은 약 206 Ir/call로 같은 고정비 형태다.

다만 3-run의 C 대비 aggregate 81.5%를 모두 binding 비용으로 귀속할 수는 없다. 두 runner의
active drain 구조는 대체로 같지만 server auto-HWM 계산 시점과 process lifecycle이 다르며,
실제 paired report의 server HWM도 다르다. 기본 PUBSUB은 lossy이므로 이 차이가 subscriber의
keep-up과 drop 수를 직접 바꾼다. 따라서 library 판정은 no-go로 닫되, **runner parity 수정과
재측정은 library 개선과 별도 작업으로 처리**하는 것이 타당하다.

## 3-run 결과와 프로파일이 말하는 범위

- 3-run median 비율은 64B 80.3%, 256B 61.9%, 1024B 80.4%, 4096B 91.4%, 65536B
  93.4%, 산술평균 81.5%다. 평균 latency 비율은 1.08x다.
- pass 1의 교차 실행에서 C++ publisher + C subscriber는 C/C의 99.7%(64B), 101.0%(4096B)였고,
  C publisher + C++ subscriber는 94.7%, 94.4%였다. 반복해서 보이는 library 차이는 publisher가
  아니라 subscriber의 약 5~6% 고정 비용이다.
- 64B subscriber callgrind에서 C는 12,986 messages에 140,562,158 Ir, C++는 13,346 messages에
  170,110,651 Ir였다. 단순 정규화하면 10,824 대 12,746 Ir/message로 C++가 17.8% 더 쓴다.
  setup과 서로 다른 실행 시점의 Core 비용까지 포함하므로 이 값 자체를 wrapper 효과로 볼 수는
  없지만, CPU-bound subscriber의 keep-up 차이가 81.5% 방향과 맞는다는 근거다.
- 같은 profile에서 C++ poller wrapper 전체는 217,001 Ir, 프로그램의 0.13%뿐이다. poller
  wrapper가 18.5%p 차이의 주원인이라는 가설은 기각한다.

사용한 subscriber profile은
`profiles/pubsub-pass1/c64.2362408.callgrind`와
`profiles/pubsub-pass1/cpp64.2361024.callgrind`다.
3-run 원본은 `perf_c_multi_linux_20260905_055350_p1cpp-pubsub-r3.txt`와
`perf_cpp_multi_linux_20260905_055517_p1cpp-pubsub-r3.txt`다.

## 수신 호출과 Core 경계 대조

기본 측정은 payload와 빈 tail의 2-part message다. C와 C++ 모두 ready socket 하나를 선택하면
`EAGAIN`까지 계속 읽는다. 따라서 성공 message 하나에 `zlink_subscribe_part()`가 2회 호출되고,
각 drain batch 끝에서 실패 호출이 1회 더 발생한다.

| 64B profile 항목 | C | C++ | 판단 |
|---|---:|---:|---|
| 완료 message | 12,986 | 13,346 | Core `buffer_recv_parts` 호출 수로 계산 |
| `zlink_subscribe_part` | 26,013, 2.003/message | 26,715, 2.002/message | 동일: part당 1회 + batch 끝의 소수 `EAGAIN` |
| Core part-count 조회 | 0 | 0 | `has_more`로 진행한다. runner의 `measurement_part_count()`만 각 message에서 1회 실행한다 |
| runner/binding에서 시작한 `zlink_msg_size` | 38,958, 3.000/message | 53,407, 4.002/message | C++ 순증은 약 1회/message |
| Core 내부까지 포함한 `zlink_msg_size` | 51,964, 4.002/message | 66,773, 5.003/message | 위 순증과 일치 |
| runner/binding이 소유한 `zlink_msg_close` | 26,013, 2.003/message | 26,715, 2.002/message | 동일: part당 정확히 1회, 실패 호출 포함 |
| Core 내부까지 포함한 `zlink_msg_close` | 90,994, 7.007/message | 93,488, 7.005/message | 정규화하면 동일 |

C runner는 첫 part의 stop-token 검사와 header decode에서 size를 각각 조회하고 tail에서 한 번
조회한다(`bindings/c/perf/multi/src/perf_multi_pubsub_client.cpp:73-93`,
`bindings/c/perf/common/perf_zlink_part_helpers.hpp:246-256`). C++ runner는 첫 part의 size를 한 번
저장해 두고 tail에서 한 번 조회하지만(`bindings/cpp/perf/multi/src/perf_pubsub_client.cpp:69-96`),
wrapper가 매 part의 기존 output이 비었는지 `zlink_msg_size()`로 먼저 확인한다
(`bindings/cpp/src/Runtime/Sockets/socket.cpp:327-335`). 그러므로 wrapper에서 2회가 늘고 runner에서
1회가 줄어 C++의 순증은 1회/message다.

`message_t` 생성자와 소멸자는 각각 native init과 close를 한 번 수행한다
(`bindings/cpp/src/Runtime/Messaging/message.cpp:11-31,216-223`). C도 각 raw part에서 같은 init/close를
직접 수행하므로 Core 호출 수 차이는 없다. C++ 64B profile의 생성자 801,450 Ir, 소멸자
2,245,889 Ir은 추가 ownership 횟수가 아니라 같은 native 수명을 RAII로 감싼 비용이다.

수신 completion, `std::function`, binding lock은 이 경로에 없다. `socket_t::subscribe_part()`는
동기식으로 Core의 `zlink_subscribe_part()`를 직접 호출한다. Core가 multipart를 buffer에 넣고
`part_helper` mutex를 잡는 비용(`core/src/api/socket/socket_message_api.cpp:377-389,453-565`)은 C와
C++가 함께 지불하므로 binding 차이의 후보가 아니다.

## 원인 가설과 확인 방법

| 우선순위 | 가설 | 코드·측정 근거 | 확인 방법 |
|---|---|---|---|
| 1 | 공개 wrapper의 고정 비용이 subscriber 처리 한계를 낮춘다 | wrapper 순비용 214.9 Ir/call, 2 calls/message. topic SSO 복사는 997,139 Ir / 13,346 messages = 74.7 Ir/message다. pass 1 교차 실행도 C++ subscriber만 약 5~6% 낮았다 | 같은 C publisher에 대해 동일한 C++ poll/metric loop에서 raw C API와 public C++ wrapper만 바꾸는 진단 binary를 A/B한다. instructions/message, calls/message, sequence gap을 함께 비교한다 |
| 2 | lossy drop이 작은 CPU 차이를 throughput 차이로 키운다 | 공식 기본은 `NODROP=0`; Core distributor는 HWM에 막힌 pipe를 drop한다. 한 app thread가 100개 SUB socket을 ready socket별로 EAGAIN까지 drain하므로 wrapper가 느려질수록 나머지 queue가 HWM에 머무는 시간이 늘어난다. 256B 61.9%와 4096/65536B 91~93%처럼 size별 비율이 비선형이다 | header `seq`를 socket별로 기록해 gap/drop, drain batch 길이, OK/EAGAIN 수를 센다. `NODROP=1` 또는 송신률 제한은 원인 분리용 진단으로만 사용하고 공식 결과에는 쓰지 않는다 |
| 3 | server auto-HWM과 lifecycle 불일치가 3-run 비율에 섞였다 | C server는 client START 뒤 size마다 HWM을 적용하고 context를 재계산한다(`perf_multi_pubsub_server.cpp:299-325`). C++ server는 bind/connect 전에 한 번 재계산한다(`perf_pubsub_server.cpp:128-144`). report에서 C server는 64/1024/65536B가 1,048,576, C++ server는 전 size 4,096,000이다. C는 한 process에서 여러 size를 처리하고 C++는 size마다 새 process다 | C++도 연결 준비 뒤 같은 시점에 재계산하고, 양쪽 `SNDHWM/RCVHWM` snapshot과 process lifecycle을 맞춘 runner-only A/B를 한다. 결과는 library 개선과 합산하지 않는다 |
| 4 | deadline/turn 차이가 phase 경계와 socket fairness를 바꾼다 | C는 매 receive 전에 deadline을 확인하고 `min(100ms, remaining)`으로 기다린다(`perf_multi_pubsub_client.cpp:168-205`). C++는 성공 receive 뒤에만 확인하고 남은 전체 시간을 기다리며, stop을 받아도 inner drain에서 `continue`한다(`perf_pubsub_client.cpp:241-291`). active 구간의 기본 drain-to-EAGAIN 구조와 Core 호출 빈도는 같다 | poll 횟수, event 수, socket별 batch 길이, deadline 뒤 receive 수를 계수한다. C++에 100ms cap·drain 상단 deadline·stop break를 각각 적용한 runner-only A/B로 throughput 영향이 5%인지 확인한다. 기존 profile의 poller 비용 0.13%라 주원인 가능성은 낮다 |
| 5 | 측정 순서와 host 상태가 lossy 변동을 키웠다 | 같은 코드의 기존 1-run aggregate가 93.3%와 120.7%로 크게 달랐다. 이번 report 시작 load average도 C 0.18, C++ 4.17이고 C 자체 run 범위도 64B 772~956 Kmsg/s, 256B 744~1050 Kmsg/s다 | host가 비어 있을 때 size별 C/C++를 ABBA 순서로 배치하고 HWM·sequence gap·CPU time을 함께 기록한다. 값을 고르기 위한 반복이 아니라 환경/runner 귀속 확인에만 쓴다 |

### runner 차이 판정

active 중 수신 호출 빈도와 기본 turn은 실질적으로 같다. 두 구현 모두 poller가 알려 준 socket을
EAGAIN까지 drain하므로 C++가 message마다 Core receive를 더 호출하지 않는다. 차이는 phase 경계,
poll timeout, topic 검사와 lifecycle에 있다.

- C는 SUB filter로 빈 문자열을 등록하고 C++는 `bench`를 등록한다
  (`perf_multi_client_helpers.hpp:668-677`, `perf_pubsub_client.cpp:174-181`). 현재 publisher가
  `bench`만 보내므로 받은 payload 의미는 같지만 runner parity는 아니다.
- C는 topic 길이와 NULL source만 검사하고, C++는 topic 문자열과 routing-id size까지 검사한다.
  이는 runner 비용이며 library 효과와 분리해야 한다.
- C++ `poller_t`는 activity mutex와 native event→slot 변환을 거치지만 profile 상한이 0.13%라
  18.5%p를 설명하지 못한다.
- 가장 중요한 차이는 C의 연결 후 per-size server HWM 재계산과 C++의 연결 전 1회 재계산이다.
  lossy 결과를 다시 비교하려면 이 항목부터 맞춰야 한다.

## 공개 contract를 유지하는 후보

| 후보 | contract 보존 방법 | 예상 효과 | 판정 |
|---|---|---:|---|
| `subscribe_part`의 empty-output preflight를 native size 조회 대신 내부 preservation 상태로 판정 | public signature는 그대로 둔다. 성공한 direct receive 뒤 상태를 보수적으로 “보존 필요”로 표시해, 다음 실패에서도 기존 output을 잃지 않게 한다 | preflight의 최대 제거분은 64B profile에서 1,335,750 / 170,110,651 Ir = **0.79%**. 2-part 기준 size 조회 2회/message를 없애도 5% 미만 | 미채택. 작은 이득에 비해 cached state 의미가 복잡해지고 empty received part 재사용은 slow path가 된다 |
| topic assignment의 동일값/SSO fast path | 성공한 각 part에서 독립 `std::string` output을 그대로 보장하고, 같은 값일 때만 byte copy를 건너뛴다 | topic 관련 비용을 전부 없앤 비현실적 상한도 997,139 / 170,110,651 Ir = **0.59%**. 현재 runner는 매 호출 새 string이라 실제 이득은 0에 가깝다 | 미채택 |
| socket-only `poller_t` wait bookkeeping 축소 | 공개 poller 결과와 재진입/close 오류 계약을 유지하는 범위에서 event 변환만 줄인다 | poller 전체를 없앤 상한이 **0.13%** | 미채택 |
| non-empty output rollback 경로 정리 | 성공 전까지 기존 `message_t`를 보존하고 성공 시에만 교체한다 | benchmark는 항상 새 empty output을 넘겨 현재 fast path만 탄다. **0%** | 대상 아님 |

가이드 §4의 public wrapper pool, timeout-0 spin, 인위적 in-flight 상한, 2단계 측정과 송신률 제한은
후보로 올리지 않았다. default `message_t`의 native init을 없애는 변경도 `valid()`와 ownership
계약을 바꾸므로 후보가 아니다. topic/source output 생략이나 raw C API를 perf runner에서 직접
사용하는 방식도 측정 대상인 공개 C++ contract를 바꾸므로 채택할 수 없다.

## 최종 추천

1. C++ binding pass 2는 **no-go**로 기록한다. 5% 기준을 넘는 계약 보존 후보가 없다.
2. 81.5%는 현재 측정 결과로 남기되, 18.5%p 전체를 binding에 귀속하지 않는다. profile과 교차
   실행이 직접 뒷받침하는 subscriber wrapper 비용은 약 5~6%다.
3. 후속 작업을 한다면 library가 아니라 runner parity부터 고친다. 특히 server auto-HWM을 연결
   준비 뒤 같은 시점에 재계산하고 C/C++의 size별 process lifecycle과 100ms poll 규칙을 맞춘다.
   이 변경의 효과는 binding 최적화와 별도 report로 판정한다.

이번 pass에서는 저장소 파일을 수정하지 않았고 build, perf, callgrind 실행도 하지 않았다.
기존 callgrind 파일에는 `callgrind_annotate`만 사용했다.
