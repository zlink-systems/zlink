# Binding 성능 최적화 참고 자료

이 문서는 binding library의 hot path를 개선할 때 언어별로 어떤 최적화를 적용했는지
확인하는 유지보수자용 참고 자료다. 공개 API나 동작 계약을 정하는 spec이 아니다. 수치가
있는 항목은 해당 report의 측정 결과이고, 수치가 없는 항목은 구조 또는 소유권 판단이다.

## 판정 기준과 측정 범위

최적화는 public API, message ownership, callback lifetime을 유지해야 한다. pool은 외부에서
객체 identity를 관찰할 수 없고 종료 시점이 분명한 내부 상태에만 적용한다. 다른 thread에서
반환될 수 있으면 반환 경로를 분리하고 bounded shared pool을 사용한다. public future,
callback userdata, 사용자가 보유할 수 있는 wrapper는 generation을 구분할 수 없으므로
재사용하지 않는다.

표의 “적용됨”은 현재 source에서 확인한 내용이다. “구조 개선”은 ownership·lifetime을
단순하게 했지만 독립적인 처리량 이득을 이 문서에서 확정하지 않은 경우다. “측정 대기”는
contract와 source 검증은 끝났지만 이 변경에 대한 최종 benchmark 수치가 없는 경우다.
기호만으로 benchmark 결과를 추측하지 않는다.

기존 report의 비교 조건은 Core 0.14.6, Release/LTO, TCP/WSS, 1024B, clients 100,
I/O threads 4였다. 이 조건에서 나온 역사적 수치는 그대로 보존한다. REQ/REP binding
변경은 Core 0.15.0, TCP, 1024B, CCU100, auto-HWM `balanced`로 검증했고, C
`MULTI_STREAM` async 제출은 Core 0.15.1에서 별도로 측정했다. 아래 값은 기능 smoke와
병목 분리를 위한 표본이며 언어 순위를 정하는 baseline이 아니다.

## 언어별 적용 matrix

| 최적화 | C | C++ | .NET | Java | Node | Python | Go | Rust |
|---|---|---|---|---|---|---|---|---|
| native message header·wrapper 재사용 | native baseline | 적용됨 | 적용됨 | 적용됨 | 적용됨 | owner-aware clone | clone 경로 | clone 경로 |
| 2-part staging heap 제거 | native 경로 | 적용됨 | stack·ArrayPool | scratch 재사용 | inline storage | 미적용 | 미적용 | 미적용 |
| 수신 payload 재복사 제거 | direct reply | 적용됨 | 적용됨 | 적용됨 | 적용됨 | native clone | native clone | native clone |
| 내부 completion 지연 생성·재사용 | Core 소유 | pending retain | lazy·ArrayPool | pool·lazy | payload 축소 | completion batch | 별도 최적화 없음 | 별도 최적화 없음 |
| 기본 DEALER request submit | `zlink_dealer_request_part` | 표준 API | 표준 API | 표준 API | 표준 API | 표준 API | 표준 API | 표준 API |
| Router routed submit | Core route | exact pair | exact pair | exact pair | exact pair | exact pair | exact pair | exact pair |
| STREAM packet echo 제출 | `zlink_send_async` 1회·Core 자동 처리 | 별도 최적화 없음 | 별도 최적화 없음 | 별도 최적화 없음 | 별도 최적화 없음 | 별도 최적화 없음 | 별도 최적화 없음 | 별도 최적화 없음 |

## Core 0.15.0 CCU100 REQ/REP 검증

공식 2-part `[payload, empty]` wire shape로 두 REQ/REP 패턴을 실행했다. C, C++, .NET,
Java, Node, Go, Rust는 1초 R1 smoke이며 Python 최종값은 completion 병목 수정 뒤의
3초 R3 중앙값이다. Python은 binding 기본값인 I/O thread 1, 나머지 smoke는 I/O thread
4를 사용했으므로 이 표에서 언어 간 처리량 비율을 성능 순위로 해석하지 않는다.

| 언어 | I/O threads | 반복 | DEALER→ROUTER | ROUTER→ROUTER | 결과 |
|---|---:|---:|---:|---:|---|
| C | 4 | R1 | 182.106 Kops/s | 153.332 Kops/s | 2/2 성공 |
| C++ | 4 | R1 | 125.285 Kops/s | 130.626 Kops/s | 2/2 성공 |
| .NET | 4 | R1 | 60.287 Kops/s | 60.953 Kops/s | 2/2 성공 |
| Java | 4 | R1 | 48.811 Kops/s | 39.548 Kops/s | 2/2 성공 |
| Node | 4 | R1 | 40.400 Kops/s | 41.464 Kops/s | 2/2 성공 |
| Python | 1 | R3 | 20.800 Kops/s | 18.133 Kops/s | 6/6 성공 |
| Go | 4 | R1 | 73.929 Kops/s | 65.710 Kops/s | 2/2 성공 |
| Rust | 4 | R1 | 111.615 Kops/s | 103.009 Kops/s | 2/2 성공 |

실행 당시 report는 `/tmp/zlink-final-ccu100-{c,cpp,dotnet,java,node,go,rust}/multi/report/`와
`/tmp/zlink-python-reqrep-completion-r3-part2/multi/report/`에 생성됐다. 모든 binding이
같은 Core 0.15.0 binary SHA-256
`068a57e43cf8ef6bb262d4de0d3d4c5a8eb883f5de8263eca2c78eb0f4ee5ca6`을 사용했다.

## 소유권 경계

직접 reply 또는 native clone은 submit 결과와 binding별 owner 규칙을 함께 따라야 한다. C++,
.NET, Java, Node는 수신 native part를 reply 경로에 전달하고, Python·Go·Rust는 수신 part를
`zlink_msg_copy` 계열로 clone해 submit용 native part를 만든다. clone을 사용하는 binding은
submit 전까지 원본 수신 객체의 lifetime을 보존하고, submit 실패 시 clone을 정리한다. public
`Message`, `Future`, `Task`, `CompletionStage`, callback userdata는 호출자가 수명을 연장할 수
있으므로 내부 pool의 대상이 아니다.

## C

C binding은 별도 VM wrapper가 없는 native baseline이다. request/reply server는 받은 native
message를 직접 reply 경로에 넘기며, binding 층에서 `Message → bytes → Message` 변환을
추가하지 않는다. 따라서 C는 “최적화를 새로 적용하지 않은 기준 경로”이면서 direct reply가
이미 적용된 상태다.

- **적용됨·측정 baseline:** Core message header와 part lifetime을 C API가 직접 유지한다.
- **구조 경계:** C caller가 `zlink_msg_t`를 보유하는 동안 Core에 소유권을 넘기지 않으며,
  submit 뒤에는 Core의 ownership 규칙을 따른다. 이 경계를 깨는 wrapper pool은 C baseline의
  비교 의미를 바꾸므로 추가하지 않는다.
- **검증 위치:** `bindings/c/include/zlink/message/api.h`, `bindings/c/perf/multi/` 및
  `bindings/c/perf/single/`의 request/reply server/client. 현재 Core 0.15.0 CCU100
  smoke 결과는 위 표에 기록했다.

### `MULTI_STREAM` packet echo

C `MULTI_STREAM` server는 `zlink_stream_packet_handler()`가 전달한 packet마다 public
`zlink_send_async()`를 한 번 호출해 같은 `source_rid`로 echo한다. server는 먼저
`DONTWAIT`로 제출하거나 `POLLOUT`을 관찰해 같은 packet을 다시 보내지 않는다.

Core는 packet callback의 현재 pipe에서 같은 target의 FIFO를 보존할 수 있으면 즉시
admission을 수행하고, 그렇지 않으면 pending operation에 넣어 HWM backpressure와 전송
순서를 처리한다. 이 선택은 C perf server가 보유하는 정책이 아니다. operation id `0`은
callback 없이 즉시 완료되고, nonzero id만 정확히 한 번 completion callback을 받는다.
C server는 completion으로 terminal 결과만 집계하며 재제출하지 않는다.

- **검증 위치:** `bindings/c/perf/multi/common/perf_multi_stream_session.hpp`,
  `bindings/c/perf/multi/src/perf_multi_stream_server.cpp`, 그리고 Core STREAM
  packet-handler contract test.

#### Core 0.15.1 성능 확인

위의 Core 0.15.0 REQ/REP smoke와 구분해 C `MULTI_STREAM` packet echo만 측정했다. 조건은
Core 0.15.1 Release, TCP, 1024B, CCU100, server와 client I/O thread 각각 4,
auto-HWM `balanced`, 3초 R3다.

| 반복 | 처리량 | 평균 지연 | P95 | P99 |
|---|---:|---:|---:|---:|
| R1 | 292.572 Kops/s | 0.171 ms | 0.301 ms | 0.365 ms |
| R2 | 300.574 Kops/s | 0.166 ms | 0.294 ms | 0.356 ms |
| R3 | 305.621 Kops/s | 0.163 ms | 0.290 ms | 0.352 ms |
| 중앙값 | 300.574 Kops/s | 0.166 ms | 0.294 ms | 0.356 ms |

최종 중앙값을 같은 주요 조건에서 측정한 이전 경로와 비교하면 다음과 같다. 초기 안전
all-async 경로보다는 처리량이 3.074배로 증가했지만, packet callback에서 직접 송신 여부를
판단한 수동 경로와는 13.16~15.90% 차이가 남아 있다. 따라서 이 결과는 Core가 자동으로
backpressure를 처리하는 경로의 개선을 확인한 값이며, 수동 경로와의 성능 차이가 해소됐다는
근거는 아니다.

| 비교 경로 | 처리량 | Core 0.15.1 최종 중앙값의 변화 |
|---|---:|---:|
| 초기 안전 all-async, R1 | 97.784 Kops/s | +207.4% (3.074배) |
| 직전 per-target all-async, R3 중앙값 | 288.755 Kops/s | +4.09% |
| 인접 수동 hybrid, R3 중앙값 | 346.130 Kops/s | -13.16% |
| direct baseline, R1 | 357.398 Kops/s | -15.90% |

최종 측정은
`bindings/c/perf/results/multi/report/perf_c_multi_linux_20260901_140443_core-0.15.1-final-r3.txt`에
있다. 비교 report는 같은 디렉터리의
`perf_c_multi_linux_20260901_124119_async-admission-after.txt`,
`perf_c_multi_linux_20260901_134357_per_target_all_async_no_notify_r3.txt`,
`perf_c_multi_linux_20260901_133738_per_target_hybrid_adjacent_r3.txt`,
`perf_c_multi_linux_20260901_123321_async-admission-baseline.txt`다. R1과 R3 및 코드 snapshot이
섞인 비교이므로 이 표는 최종 구현의 상대 위치를 보여 주는 참고값이며 통계적 회귀 판정값이
아니다.

## C++

### 적용됨

- `message_t` constructor는 곧 덮어쓸 native storage를 먼저 초기화하지 않는다. Core 0.10.1
  DR REQ/REP TLS 비교에서 C 대비 비율이 84.56%에서 95.50%로 개선됐다. 구현은
  `bindings/cpp/src/Runtime/Messaging/message.cpp`에 있다.
- subscription single-part 결과는 기존 native output storage에 직접 받고 in-place로 교체한다.
- receive message는 payload 존재 여부를 캐시해 native header 조회를 줄인다. DD TLS 평균
  비율은 89.24%에서 93.23%로 개선됐다. 구현은 `message_access.hpp`와 `Sockets/detail.hpp`다.
- async send completion은 Core가 nonzero operation id를 반환한 경우에만 self-reference를
  유지한다. 즉시 완료 경로를 줄인 r3는 직전 후보보다 TCP 8.6%, WSS 4.9% 개선됐다.

### 적용됨·구조 개선

REQ/REP server는 bytes로 다시 materialize하지 않고 받은 `Message`를 direct reply에 넘긴다.
수신 객체가 가진 native part를 reply builder가 Core에 제출하고, 실패할 때만 원래 객체가
part를 닫는다. Core 0.15.0·CCU100 smoke는 두 패턴 모두 성공했다.

검증 위치는 `bindings/cpp/include/zlink/`의 messaging/socket contract, `bindings/cpp/src/`
의 reply 경로, `test_cpp_contract_request_reply`와 `test_cpp_contract_optimization_guard`다.
역사적 후보 report는 `bindings/cpp/perf/results/multi/report/`에 보존한다.

## .NET

### 적용됨

- `Message.Allocate`는 send 뒤 무효화된 내부 wrapper를 thread-local pool에서 재사용한다.
- DEALER receive는 source-generated native import와 `Unsafe.SkipInit`을 사용한다.
- PUB/SUB receive는 private two-slot wrapper와 반환 가능한 wrapper pool을 사용한다. DONT_WAIT
  경로에는 suppress-GC-transition을 적용했다. 과거 평균 C 대비 비율은 68.05%에서 84.44%로
  개선됐다([.NET PUB/SUB TCP 기록](../bindings-0.10.0/log/2026-08-13-dotnet-pubsub-tcp.ko.md)).
- multipart async send는 작은 part를 stack에 두고 큰 배열만 `ArrayPool<ZlinkMsg>`에서 빌린다.
  Core가 pending operation을 만든 경우에만 `TaskCompletionSource`를 만든다.

### 적용됨·구조 개선

REQ/REP server는 받은 native `Message`를 reply에 전달한다. 따라서 server managed allocation과
payload byte copy가 한 번씩 사라진다. `Task`와 public wrapper의 lifetime은 그대로 두고,
Core가 submit을 수락한 뒤에만 native ownership이 이동한다. Core 0.15.0·CCU100 smoke는
두 패턴 모두 성공했다.

public wrapper나 `Task`를 pool에 반환하지 않는다. `GCHandle` 재사용은 늦은 callback이 새
operation을 완료시키는 ABA 위험 때문에 적용하지 않는다. 검증은 `.NET` binding의 request/reply
contract와 `bindings/dotnet/perf/multi/` runner에서 한다. 과거 direct 2-part native submit은
SENDSEND TCP 14~16%, REQ/REP 8~41% 회귀해 기각했다.

## Java

### 적용됨

- 이미 native `msg_t`를 소유한 request part는 임시 `Arena`와 payload copy를 만들지 않고
  Core에 직접 전달한다. 과거 경로별 처리량은 7.2~21.1% 개선됐다([Java request zero-copy 기록](../bindings-0.10.0/log/2026-08-13-java-request-zero-copy.ko.md)).
- request payload template, routed reply scratch, routing identity를 재사용한다. template
  재사용은 DD 비율을 17.646%에서 약 74.3%로, DR REQ/REP WSS를 46.864%에서 51.8%로
  개선했다.
- callback/poller는 reply native message pair만 snapshot하고 public `Message` 생성은
  completion worker가 수행한다. native slot은 같은 thread에서 닫히면 thread-local pool로,
  다른 thread에서 닫히면 bounded shared pool로 돌아간다.
- public `CompletionStage`와 분리된 내부 send terminal `Pending`만 socket-local bounded
  pool에서 재사용한다.

### direct reply와 검증

REQ/REP는 받은 native `Message`를 direct reply 경로에 넘긴다. `Message.close()`와 public
`CompletionStage`는 pool 대상이 아니며, owner가 part reference를 제거한 뒤 실행하는 내부
cleanup만 반환된다. 과거 RR SENDSEND는 TCP 19.5%, WSS 11.3% 개선됐다. Core 0.15.0·CCU100
smoke는 두 패턴 모두 성공했다. request/reply 종료·result mapping·send ownership contract와
`bindings/java/native/` bridge가 source 검증 위치다.

## Node

Node의 routed native frame pool, identity cache, wrapper 재사용, inline 2-part staging은
기존 reference 최적화로 적용됨이다. 64B는 79.8→111.9 Kmsg/s, 256B는 62.8→108.7 Kmsg/s로
개선됐다([Node routed native frame pool 기록](../bindings-0.10.0/log/2026-08-13-node-routed-native-frame-pool.ko.md)).

REQ/REP echo는 `Message → Buffer → Message` 왕복 없이 received `Message`를 public reply
builder에 넘긴다. Core가 소비하면 builder가 ownership을 넘기고, 실패하면 `Received.close()`가
수신 상태를 정리한다. TSFN STREAM payload pool은 mutex와 반환 비용 때문에 기각했고, JS
callback queue와 scheduling은 별도 측정 대상이다.

JS multipart 입력은 한 번의 N-API 호출로 addon에 전달된다. addon은 일반적인 8-part 이하를
inline native storage에 staging하고, 각 `Buffer`는 native message로 만들며 native `Message`는
`zlink_msg_copy`로 복제한다. SENDSEND는 한 번의 `zlink_send_async`로 record를 제출한다.
REQ/REP는 addon 내부 C++ loop가 part별 Core request 함수를 호출하고 final part에만 reply
callback을 건다. 완료 callback은 JS를 직접 실행하지 않고 reply part를 native payload로 옮긴
뒤 TSFN으로 event-loop에 전달한다. 따라서 “Node도 single-thread”라는 사실은 맞지만 part loop와
callback ingress가 JavaScript가 아니라 addon 안에서 실행된다는 차이가 있다.

검증 위치는 `bindings/node/native/src/`, `bindings/node/src/`,
`bindings/node/tests/dealer_router.test.ts`,
`bindings/node/tests/perf_multi_routed_sendsend_contract.test.ts`,
`bindings/node/tests/source_layout.test.ts`다. 역사적 1024B r3
REQ/REP는 38.5~40.7 Kops/s였으며, 현재 Core 0.15.0·CCU100 smoke도 두 패턴 모두
성공했다.

## Python

### 적용됨·구조 개선

internal materializer에는 owner-aware `ReceivedMessage` native clone이 적용되어 있다. 받은 owner의
part를 다시 Python bytes로 만들지 않고 `zlink_msg_copy`로 clone해 reply materializer에 넘긴다.
이 clone은 internal native lifetime 안에서만 유효하고, public `Message`가 소유하는 bytes와
동일한 pool로 재사용하지 않는다.

routed async 경로에서는 nested `Task`와 admission 중 sleep을 제거하고, socket-local retry와
eager task로 스케줄링 단계를 줄인다. retry 대상은 pre-admission `BACKPRESSURED`뿐이며
`NOT_ADMITTED` 같은 접근·인증 거부를 숨기지 않는다. backpressure와 cancellation의 public
결과는 바꾸지 않으며, retry가 끝난 뒤 Core submit이 ownership을 받는다. 변경 위치는
`bindings/python/src/zlink/_runtime/messaging/message_materializer.py`, `native_parts.py`,
`routed_async.py`와 `bindings/python/src/zlink/_native/_zlink_native.c`다.

completion은 event loop별 batch로 묶어 Core callback thread에서 발생하는 self-pipe wake를
줄인다. callback이 이미 event-loop thread에서 실행되면 Future를 직접 완료한다. batch의 한
Future 완료나 message close가 실패해도 다음 completion을 계속 처리하고 예외는 event-loop
exception handler에 보고한다.

### REQ/REP 핵심 병목과 수정

Python multi REQ/REP는 requester socket을 completion poller에 등록하지 않아 Core I/O
thread가 매 reply마다 `ctypes.CFUNCTYPE` callback으로 Python에 진입했다. 이 callback과
asyncio submit loop가 GIL을 반복해서 경쟁하면서 실제 native 함수보다 훨씬 큰 submit,
materialize, callback 시간이 관측됐다. 모든 requester를 public poller 하나에
`POLLCOMPLETION`으로 등록하고 active·drain turn마다 `wait(..., 0)`을 한 번 호출해 같은 Core
callback의 dispatch 위치만 asyncio thread로 옮겼다. request는 계속 public async terminal로
완료되며 고정 inflight window, 별도 thread, timer, pipe는 추가하지 않았다.

동일한 CCU100·TCP·1024B·I/O thread 1의 true 1-part 사전 A/B에서 DEALER→ROUTER는
7.467→28.166 Kops/s(+277%, 3.77배), 평균 latency는 28.247→2.462ms로 바뀌었다. 최종 R3
중앙값은 다음과 같다.

| wire shape | DEALER→ROUTER | ROUTER→ROUTER |
|---|---:|---:|
| 진단 1-part | 29.934 Kops/s | 23.500 Kops/s |
| 공식 2-part | 20.800 Kops/s | 18.133 Kops/s |

수정 전 공식 2-part smoke의 6.747/5.514 Kops/s와 비교하면 약 3.08배/3.29배다. 같은 I/O
thread 1의 Node 공식 2-part 진단값은 45.936/44.900 Kops/s였다. 따라서 가장 큰 회귀성
병목은 제거됐지만 Python과 Node가 동일 처리량이라는 결론은 아니다.

### native 호출 비용과 남은 차이

단순 native 호출은 Python `ctypes.CDLL` 0.148µs, Node N-API 0.017µs로 차이가 약 0.13µs였다.
반면 Python submit thread와 Core callback thread가 GIL을 경쟁할 때 같은 `CDLL` 호출은
46.57µs까지 늘었다. GIL을 유지하는 `PyDLL` 호출은 단독 0.114µs였지만 request submit에서
Core callback 진행을 막아 교착할 수 있으므로 적용하지 않았다. 즉 raw native-call 고정비가
주원인이 아니라 callback 실행 위치가 그 비용을 증폭한 것이었다.

수정 뒤에도 Python은 part별 ctypes argument 구성, native message materialization, reply
`Message` 생성과 asyncio task scheduling을 Python 쪽에서 수행한다. Node는 같은 part loop와
reply staging 대부분을 addon C++에서 처리한다. 두 런타임이 single event-loop 기반이라는
공통점만으로 이 비용이 같아지지는 않는다. 1-part profile에서 요청당 submit은
69.9→14.2µs, materialize는 32.7→9.1µs, reply callback은 41.2→13.2µs로 줄었지만 이
잔여 비용은 남아 있다.

SENDSEND도 같은 I/O thread 1, CCU100, TCP, 1024B, 2초 R1로 비교했다. 공식 2-part에서
Node는 43.666/40.657 Kops/s, Python의 안정적인 no-flag 경로는 별도 5초 R1에서
23.218/22.143 Kops/s였다. 1-part DEALER→ROUTER는 Node 70.945 Kops/s, Python
28.038 Kops/s였다. 조건별 duration이 달라 절대 baseline으로 합치지는 않지만 multipart만
제거해도 격차가 사라지지 않는다는 진단 근거다. report는 실행 당시
`/tmp/zlink-node-sendsend-{io1,1part}/multi/report/`와
`/tmp/zlink-python-sendsend-1part-unprofiled/multi/report/`에 생성됐다.

### 기각한 Python 후보

- multipart outbound를 한 native 호출로 합치는 실험은 ownership 경계를 깨고 allocator
  corruption을 일으켜 전부 원복했다. public API는 계속 part별 Core 호출을 사용한다.
- SENDSEND의 중앙 round-robin scheduler와 측정 tuple 사전 생성은 처리량을 개선하지 않아
  원복했다.
- SENDSEND direct `Received` pool은 동일 completion 조건에서 독립 효과가 +2.81%로 5%
  미만이었다. `POLLIN | POLLCOMPLETION`도 R3 중앙값이 기존 `POLLIN`보다 낮고 drain
  안정성을 개선하지 못했다. pool, completion flag, 별도 shutdown drain 실험은 모두
  원복했다.

### 적용 범위와 제한

public wrapper와 Future는 pooled object가 아니다. reply callback은 request당 한 번이며
“part마다 callback”이 아니다. 다만 submit은 part별 Core 호출이고 2-part staging은 아직
inline storage나 pool reuse를 적용하지 않았다. 이번 개선을 Python 전체 runtime overhead
제거로 해석하지 않는다.

## Go

Go request/reply는 `Reply().Message(received payload)`를 사용하고, 내부 submit은
`submitMultipartFromClones`와 `zlink_msg_copy`로 Core-owned clone을 만든다. 따라서 payload를
Go bytes로 materialize한 뒤 다시 native message로 복사하는 경로를 피한다. 이 최적화는
적용됨이지만, Go public `Message`와 callback channel은 caller가 수명을 보유할 수 있으므로
pool 대상이 아니다.

검증 위치는 `bindings/go/internal/native/socket_multipart.go`, `socket_routed.go`,
`bindings/go/contract_test.go`, `bindings/go/internal/native/spec_alignment_test.go`다. clone과 submit의
ownership은 Core admission 결과에 따라 정리된다. Core 0.15.0·CCU100 smoke는 두 패턴
모두 성공했다.

## Rust

### 적용됨·구조 개선

REQ/REP reply는 `Message::try_from(payload bytes)`로 새 message를 만들지 않고
`received.parts()[0].try_clone()`을 사용한다. clone은 내부적으로 `zlink_msg_copy`를 거쳐
Core에 제출할 native part를 만들며, submit이 수락되기 전까지 원본 `received`의 lifetime을
보존한다. multipart 경로도 각 part의 ownership을 같은 규칙으로 처리한다.

이 변경은 VM이 없다는 Rust의 runtime 제약상 “VM/native copy 제거”보다 native clone과
materialization 구조를 단순화한 구조 개선으로 분류한다. public `Message`, `Future`, callback
userdata는 재사용하지 않는다. 검증 위치는 `bindings/rust/src/runtime/messaging/message.rs`,
`bindings/rust/src/internal/`, `bindings/rust/tests/ownership_tests.rs`와
`bindings/rust/tests/behavior_tests.rs`다. `MessageParts`의 2-part 저장은 아직 `Vec` 기반이며
inline storage나 pool reuse를 적용하지 않았다. Core 0.15.0·CCU100 smoke는 두 패턴 모두
성공했다.

## 검증과 적용 순서

각 후보는 다음 순서로 확인한다.

1. public API와 Core admission 결과를 바꾸지 않는지 contract·ownership test로 확인한다.
2. 수신 native part의 owner와 close 시점을 source에서 확인한다.
3. Core 0.15.0, CCU100, 동일 payload·transport·client 조건에서 C baseline과 binding을
   측정한다.
4. 반복 측정이 끝난 항목만 “효과 확인”으로 올리고, 그렇지 않으면 “구조 개선” 또는
   “측정 대기”로 남긴다.

historical Core 0.14.6 report와 현재 Core 0.15.0 검증은 서로 다른 비교 집합이다. scheduler,
fairness, poll drain을 바꾼 perf runner의 결과는 binding library 최적화 결과와 합산하지
않는다. 특히 Python completion poller 결과는 callback dispatch context 수정 효과이며 native
clone 또는 scheduler 후보의 효과와 합산하지 않았다.
