# Binding 성능 최적화 참고 자료

이 문서는 binding library의 hot path를 개선할 때 어떤 최적화가 실제로 효과가 있었고,
현재 구현에 어떻게 적용되어 있는지 확인하려는 유지보수자를 위한 참고 자료다. 공개 API나
동작 계약을 정하는 spec은 아니다. 이 문서는 채택 근거와 적용 경계만 다루며, 원시 수치는
각 항목에 적은 benchmark report와 작업 log에서 확인한다.

## 판정 기준

최적화는 public API, message ownership, callback lifetime을 유지해야 한다. 처리량 변화는 C
Multi와 같은 조건에서 비교하며 현재 확인 작업은 Core 0.14.6, Release/LTO, TCP/WSS, 1024B,
clients 100, I/O threads 4를 사용한다. 후보는 다음 상태로 구분한다.

| 상태 | 의미 |
|---|---|
| 효과 확인 | A/B 또는 채택 전후 측정에서 반복 가능한 개선이 확인됐다. |
| 적용됨 | 이전 측정에서 채택됐고 현재 source에도 같은 책임과 경계가 유지된다. |
| 구조 개선 | ownership과 lifetime을 단순화하지만 독립적인 성능 이득은 아직 확정하지 않았다. |
| 기각 | 처리량·지연 회귀 또는 ownership 위험 때문에 적용하지 않는다. |
| 측정 대기 | contract test는 통과했지만 지정한 Multi 조건의 성능 확인이 남았다. |

pool은 외부에서 객체 identity를 관찰할 수 없고 종료 시점이 분명한 내부 상태에만 적용한다.
다른 thread에서 반환될 수 있으면 반환 경로를 분리하고 bounded shared pool을 사용한다. public
future, callback userdata, 사용자가 보유할 수 있는 wrapper는 generation을 구분할 수 없으므로
재사용하지 않는다.

## 언어별 적용 현황

| 최적화 | C++ | .NET | Java | Node |
|---|---|---|---|---|
| native message header·wrapper 재사용 | 적용됨 | 적용됨 | 적용됨 | 적용됨 |
| 2-part 임시 저장소의 heap 할당 제거 | 적용됨 | 적용됨 | 적용됨 | dealer/router reply와 router request에 적용됨 |
| VM과 native 사이 payload 재복사 제거 | 해당 경로 적용 | 해당 경로 적용 | 적용됨 | reply 경로 적용됨 |
| 내부 completion 상태의 지연 생성·재사용 | pending operation만 retain | lazy 생성, r3 완료 | terminal 상태 pool 효과 확인 | TSFN payload 축소 적용 |
| STREAM 전용 callback·queue 최적화 | 별도 검토 | 별도 검토 | 목표 처리량 통과 | 추가 조사 필요 |

## C++

### 효과 확인 항목

- `message_t` constructor가 곧 덮어쓸 native storage를 먼저 초기화하지 않는다. Core 0.10.1
  DR REQ/REP TLS 비교에서 C 대비 비율이 84.56%에서 95.50%로 개선됐다. 현재 구현은
  `bindings/cpp/src/Runtime/Messaging/message.cpp`에 유지된다. 근거는
  [0.10 처리 기록](../bindings-0.10.0/log/2026-08-09-process-round-log.ko.md)이다.
- subscription single-part 결과는 기존 객체의 native output storage에 직접 받고 in-place로
  교체한다. 현재 구현은 `subscription_reader.hpp`와 `socket.cpp`에 유지된다.
- receive message는 payload 존재 여부를 캐시에 두어 같은 native header 조회를 반복하지 않는다.
  DD TLS 평균 비율이 89.24%에서 93.23%로 개선됐으며 현재 `message_access.hpp`와
  `Sockets/detail.hpp`에 유지된다. 근거는
  [C++ DD TLS parity 기록](../bindings-0.10.0/log/2026-08-13-cpp-dealer-dealer-tls-parity.ko.md)이다.
- async send completion은 Core가 nonzero operation id를 반환한 경우에만 self-reference를
  유지한다. 즉시 완료 경로의 상태 mutex 왕복을 생략한 r3는 직전 후보보다 TCP 8.6%, WSS
  4.9% 개선됐다.

### 검증 근거

- 후보 report: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260830_090317_cpp-retain-pending-only-r3.txt`
- 비교 report: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260830_085016_cpp-send-anchor-r3.txt`
- contract: `test_cpp_contract_request_reply`, `test_cpp_contract_optimization_guard`
- 남은 확인: delayed retain 후보의 DD TCP/WSS 최종 r5

## .NET

### 적용 항목

- `Message.Allocate`가 send 뒤 무효화된 내부 wrapper를 thread-local pool에서 재사용한다. native
  frame 초기화와 Core ownership 이전은 그대로 유지한다.
- DEALER receive는 source-generated native import와 `Unsafe.SkipInit`을 사용한다.
- PUB/SUB receive는 private two-slot wrapper, 반환 가능한 wrapper pool, DONT_WAIT 전용
  suppress-GC-transition 경로를 사용한다. 해당 묶음의 과거 평균 C 대비 비율은 68.05%에서
  84.44%로 개선됐다. 근거는
  [.NET PUB/SUB TCP 기록](../bindings-0.10.0/log/2026-08-13-dotnet-pubsub-tcp.ko.md)이다.
- multipart async send는 작은 part 수를 stack에 두고 큰 배열만 `ArrayPool<ZlinkMsg>`에서
  빌린다. Core가 실제 pending operation을 만든 경우에만 `TaskCompletionSource`를 생성한다.

### 경계와 검증

- public wrapper나 `Task`는 pool에 반환하지 않는다. `GCHandle`을 재사용하는 후보는 늦은
  callback이 새 operation을 완료시키는 ABA 위험 때문에 적용하지 않는다.
- direct 2-part native submit 후보는 SENDSEND TCP 14~16%, REQ/REP 8~41% 회귀해 기각했다.
- 현재 ArrayPool·lazy completion 후보는 관련 contract test와 전체 Multi TCP/WSS r3 42/42
  case를 통과했다. STREAM은 TCP/WSS에서 C 대비 80.54%/86.65%, PUBSUB WSS는 52.19%를
  기록했다. report는
  `bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260830_090635_effective-optimizations-r3.txt`다.

## Java

### 효과 확인 항목

- 이미 native `msg_t`를 소유한 request part는 임시 `Arena`와 payload copy를 만들지 않고
  Core에 직접 전달한다. 과거 측정에서 경로별 처리량이 7.2~21.1% 개선됐다. 근거는
  [Java request zero-copy 기록](../bindings-0.10.0/log/2026-08-13-java-request-zero-copy.ko.md)이다.
- request payload template, routed reply scratch, routing identity를 재사용한다. template 재사용
  측정은 DD 비율을 17.646%에서 약 74.3%로, DR REQ/REP WSS 비율을 46.864%에서 51.8%로
  개선했다. 근거는
  [0.10 처리 기록](../bindings-0.10.0/log/2026-08-09-process-round-log.ko.md)이다.
- callback/poller는 reply native message pair만 snapshot하고 public `Message` 생성은 completion
  worker가 수행한다. 현재 r3에서 DR REQ/REP는 TCP 43.7 Kops/s, WSS 54.3 Kops/s다.
- 같은 thread에서 닫히는 native message slot은 thread-local pool로 돌아가며, 다른 thread에서
  닫히는 slot만 bounded shared pool로 반환한다.
- public `CompletionStage`와 분리된 내부 send terminal `Pending`만 socket-local bounded pool에서
  재사용한다. RR SENDSEND는 TCP 19.5%, WSS 11.3% 개선됐다.

### 경계와 검증

- public `Message.close()`는 wrapper를 pool에 반환하지 않는다. owner가 part reference를 제거한
  뒤 실행하는 내부 cleanup만 반환할 수 있다.
- completion batch, atomic pending table, socket-local synchronized pending table, worker 수 증가,
  이미 완료된 stage의 `toCompletableFuture()` fast path는 회귀해 기각했다.
- request/reply 종료, result mapping, raw integration, send ownership, optimization guard 관련
  contract test가 통과했다. 전체 Multi 최종 r5도 14/14 case를 완료했다. PUBSUB TCP와 STREAM
  TCP/WSS는 처리량 목표를 통과했고 DR/RR REQ/REP는 WSS에서 모두 통과했다. report는
  `bindings/java/perf/results/multi/report/perf_java_multi_linux_20260830_091609_effective-optimizations-final-r5.txt`다.

## Node

### 효과 확인 및 적용 항목

- routed single-part receive는 `msg_t`를 보관하는 native frame을 JS thread별 bounded pool에서
  재사용한다. 64B는 79.8→111.9 Kmsg/s, 256B는 62.8→108.7 Kmsg/s로 개선됐으며 현재 native
  receive path에 유지된다. 근거는
  [Node routed native frame pool 기록](../bindings-0.10.0/log/2026-08-13-node-routed-native-frame-pool.ko.md)이다.
- routed identity cache와 wrapper 재사용은 현재 source에 유지된다.
- REQ/REP echo는 `Message → Buffer → Message` 왕복 복사 없이 received `Message`를 public reply
  builder에 직접 넘긴다. Multi와 Single worker 모두 같은 ownership 경계를 사용한다.
- native dealer/router reply의 일반 2-part staging은 heap `std::vector` 대신 inline storage를
  사용한다. SENDSEND completion은 JS에서 쓰지 않는 operation·peer metadata를 만들지 않는다.

### 경계와 검증

- Core가 send를 소비한 경우 builder가 `Message` ownership을 넘기고, `Received.close()`는 남은
  수신 상태만 정리한다. 실패 시 원래 wrapper가 다시 닫을 수 있어야 한다.
- TSFN STREAM payload pool은 mutex와 반환 비용 때문에 기각됐다. STREAM은 일반 routed
  최적화와 분리해 callback queue와 JS scheduling을 측정한다.
- direct reply·inline storage는 `dealer_router.test.js`,
  `perf_multi_routed_sendsend_contract.test.js`, `source_layout.test.js`의 관련 contract 13개가
  통과했다. Single worker 변경은 `perf_single_worker_contract.test.js`를 포함한 관련 contract
  15개가 통과했다.
- Multi TCP/WSS 1024B r3에서 DR/RR REQ/REP는 38.5~40.7 Kops/s, STREAM은
  9.7~10.8 Kops/s다. REQ/REP report는
  `perf_node_multi_linux_20260830_085548_direct-reply-small-storage-r3.txt`, 나머지 pattern report는
  `perf_node_multi_linux_20260830_090206_current-remaining-r3.txt`다. 전체 pattern은 목표 미달이며
  추가 library hot path 조사와 최종 r5가 남아 있다.

## 적용 순서

새 후보는 VM/native payload copy, per-message heap allocation, completion scheduling 순서로
확인한다. 각 후보는 관련 contract test를 먼저 통과시킨 뒤 C와 binding을 직렬로 측정한다.
1024B r3에서 방향을 확인하고 채택 후보만 r5로 확정한다. pool은 64B와 큰 payload에서 효과가
달라질 수 있으므로 최종 일반화 전에 경계 size를 별도로 확인한다.

perf runner의 scheduler, fairness, poll drain 변경은 측정 의미를 맞추는 작업이다. 이 문서의
binding library 최적화 결과와 합산하지 않고 성능 계획의 별도 근거로 관리한다.
