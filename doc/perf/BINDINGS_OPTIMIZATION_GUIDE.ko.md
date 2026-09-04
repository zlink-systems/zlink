# Binding library 성능 최적화 가이드

이 문서는 binding library의 hot path를 개선할 때 어떤 최적화가 실제로 효과가 있었는지, 그
최적화를 언어별로 어떻게 적용하는지 정리한 유지보수자용 참고 자료다. 공개 API나 동작
계약을 정하는 spec이 아니다. 근거 문서는
[bindings-0.14.0/bindings-performance-optimization-reference.ko.md](perf/bindings-0.14.0/bindings-performance-optimization-reference.ko.md)와
2026-09-04 Core 0.17.0 계약(B) 전환 뒤 수행한 언어별 독립 리뷰 결과다. 수치가 있는 항목은
해당 report의 측정 결과이고, 수치가 없는 항목은 구조·소유권 판단이다.

Core 0.17.0부터 DONTWAIT send의 완료 모델이 바뀌었다(거절 시 `BACKPRESSURED` + 대기 토큰,
credit 회복 시 `ZLINK_COMPLETION_WRITABLE`, callback 없음, pull 방식 completion). 0.14.0
참고 자료의 callback·`zlink_send_async` 서술은 그 시점의 구현이며, 이 문서에서는 지금 계약에서도
유효한 기법만 "유효"로 표시하고 폐기된 것은 별도로 적는다. 계약 원문은
[`core/doc/spec/core/socket/README.ko.md`](../../core/doc/spec/core/socket/README.ko.md)의
"Part send" 절이다.

## 1. 판정 기준

- 최적화는 public API, message ownership, Future/Task/CompletionStage의 수명을 바꾸지 않는다.
  pool은 외부에서 객체 identity를 관찰할 수 없고 종료 시점이 분명한 내부 상태에만 적용한다.
- "효과 확인"은 같은 조건(pattern, transport, size, clients, duration, 같은 Core binary)에서
  전/후를 측정해 5% 이상 개선된 항목이다. "구조 개선"은 소유권·수명을 단순하게 했지만 독립적인
  처리량 이득을 확정하지 않은 항목이다. "기각"은 측정으로 효과가 없거나 안전성을 깨서 되돌린
  항목이다.
- binding 최적화는 C baseline 대비 비율로 판정한다. 언어 간 절대값 비교는 순위가 아니라 병목
  분리를 위한 표본이다. 목표 비율과 측정 범위는
  [bindings-library-performance-improvement-plan-core-0.15.0.ko.md](perf/bindings-0.15.0/bindings-library-performance-improvement-plan-core-0.15.0.ko.md)를 따른다.
- 표준 측정 명령은 각 binding의 `perf/run_benchmarks.sh`(single)와
  `perf/run_benchmarks_multi.sh`(multi)이며 규칙은 [PERF_POLICY.md](PERF_POLICY.md)다.

## 2. hot path 단계별 체크리스트

전송 1건마다 실행되는 코드를 아래 단계로 나누어 점검한다. 각 항목의 "확인" 열은 2026-09-04
리뷰에서 실제로 발견되어 고친 결함이다.

### 2.1 즉시 성공하는 send (DONTWAIT → OK, ID 0)

가장 자주 실행되는 경로다. 여기에는 native submit과 message part 구성만 있어야 한다.

| 규칙 | 근거 |
|---|---|
| completion entry, Promise/Future/Task, waiter map 등록을 만들지 않는다. 토큰이 반환된 뒤에만 만든다 | Node: 성공 send마다 entry·Promise·map 2회 등록이 있어 DEALER_ROUTER가 28k msg/s로 떨어졌고 제거 후 145k(포팅 전 153k) |
| native poller·goroutine·thread를 성공 send마다 만들지 않는다 | Go: 즉시 성공 send도 poller와 goroutine을 만들어 81.5k → 190k msg/s 개선. Rust: send마다 native poller 생성/파괴 제거 |
| payload를 바이트로 복사해 보관하지 않는다. 재전송용 snapshot은 거절 시점에만 만들고, 만들 때도 `zlink_msg_copy`(refcount 공유, 64B 초과 본문은 복사 없음)를 쓴다 | C multi helper: 전송당 2회 복사 제거. Python: bytes → zlink_msg 이중 복사 제거(7.9k → 20k msg/s). .NET: snapshot을 거절 시점으로 이동 |
| socket 단위 배타 락을 submit 전체에 걸지 않는다. blocking submit이 park된 동안 다른 sender가 막히면 안 된다 | Rust: submit gate Mutex를 RwLock(shared submit, exclusive close)으로 |
| 2-part staging은 heap 대신 inline/stack/scratch를 쓴다 | C++ 적용, .NET stack + ArrayPool, Java scratch 재사용, Node inline 8-part |

### 2.2 거절된 send (BACKPRESSURED + 대기 토큰)

| 규칙 | 근거 |
|---|---|
| 토큰·context·RID를 함께 키로 entry를 등록하고 O(1)로 찾는다 | Python: 해제마다 live ID map 전체 스캔(O(n), 부하 시 O(n²)) → 단일 key. Node: POLLOUT마다 retry 목록 선형 순회 → 카운터 |
| 등록 전에 WRITABLE이 먼저 drain될 수 있으므로(등록→recheck 창) drain된 WRITABLE을 보류했다 등록 시 재생한다 | Rust: never-reused context counter + 보류 재생 |
| 재전송은 같은 packet을 그대로 보내고, 다시 BACKPRESSURED면 새 토큰으로 계속 대기한다 | 모든 binding 공통, 계약 (b) |

### 2.3 completion drain과 wake

| 규칙 | 근거 |
|---|---|
| 이벤트가 없을 때 timeout-0 poll을 반복하거나 `yield`/`setImmediate`/`call_soon`으로 재예약하지 않는다. 실제 wake 소스(blocking `zlink_poller_wait`, 또는 application이 등록한 public poller)에서 깨운다 | Rust: 실행기 spin이 "301k msg/s"로 보였지만 CPU 100% 점유였고, reactor 스레드 + public Poller 구동으로 259k(포팅 전 172k). Node·Python·Go·Java·.NET 모두 같은 spin 제거 |
| drain owner는 한 곳(public poller 또는 runtime owner)이며 NO_DATA까지 비운다. REQUEST completion과 WRITABLE이 섞여도 각자 대기자에게 간다 | 계약 (e). C REQREP 러너: `completion_id_out = NULL`인 send에도 Core가 토큰을 등록하므로 REQUEST drain이 WRITABLE record를 건너뛰어야 한다 |
| POLLOUT은 socket 전체 level hint다. 토큰이 있는 대상의 WRITABLE record가 없는데 POLLOUT만 계속 오면 POLLOUT 관심을 내리고 POLLCOMPLETION으로 기다린다 | C DEALER_DEALER 러너 spin 방지 |
| runtime owner 스레드는 필요할 때만(토큰이 생긴 뒤) 시작하고 idle이면 종료한다 | Rust reactor 100 ms idle 종료, Go bounded blocking poll |
| EINTR은 종료가 아니다. 종료 결과(ETERM/ESHUTDOWN)에서만 대기자를 실패시킨다 | .NET: EINTR/EBUSY에서 모든 대기자를 실패시켜 이후 WRITABLE의 payload가 버려졌던 결함 |

### 2.4 수신·reply 경로

| 규칙 | 근거 |
|---|---|
| REQ/REP server는 받은 native `Message`를 그대로 reply에 넘긴다(`Message → bytes → Message` 왕복 금지) | C++·.NET·Java·Node direct reply, Python·Go·Rust는 `zlink_msg_copy` clone. Java RR SENDSEND TCP +19.5%, WSS +11.3% |
| 수신 wrapper·native header는 재사용하고 payload 존재 여부를 캐시한다 | C++ DD TLS 비율 89.24% → 93.23%, .NET PUB/SUB 68.05% → 84.44%, Node routed frame pool 64B 79.8 → 111.9 Kmsg/s |
| 이미 native `msg_t`를 소유한 part는 임시 arena·복사 없이 Core에 넘긴다 | Java request zero-copy 7.2~21.1% |
| 기본 wrapper 생성자는 곧 덮어쓸 native storage를 초기화하지 않는다 | C++ C 대비 84.56% → 95.50% |

### 2.5 오류·종료 경계 (성능 항목은 아니지만 리뷰마다 발견됨)

- WRITABLE `send_result == TERMINAL`은 typed 실패로 전달한다(ENOENT → NotFound, ESHUTDOWN/ETERM →
  Terminated). 하나의 결과로 뭉개면(Go: 전부 NotAdmitted) 원인을 잃는다.
- ROUTER/STREAM의 route 없는 RID는 `NOT_CONNECTED` 즉시 실패이며 토큰이 없다(ROUTER는
  MANDATORY 양수일 때, 기본값).
- close/context 종료 시 대기 토큰·snapshot·native 자원을 한 번만 해제하고 예약된 pump callback을
  취소한다(Node: close 뒤 남는 immediate/timer handle).

## 3. 언어별 적용 현황

"유효"는 0.17.0 계약에서도 그대로 쓰는 기법, "적용(0.17.0 리뷰)"은 2026-09-04 리뷰 커밋에서
새로 적용된 항목이다. 수치는 DEALER_ROUTER tcp 1024B, duration 3s, runs 1(다른 job과 병행 측정이라
절대값은 참고용, 전/후 비율이 근거).

| 언어 | 유효한 기존 최적화 | 적용(0.17.0 리뷰) | 전 → 후 |
|---|---|---|---|
| C | native baseline, direct reply | REQREP drain의 stray WRITABLE 처리, DEALER_DEALER POLLOUT spin 방지, 전송당 payload 복사 제거(multi helper), STREAM session retained packet 제거 | 442k → 444k(핫패스 불변) |
| C++ | 생성자 초기화 생략, in-place 수신, payload 캐시, 즉시 완료 경로의 self-reference 생략, direct reply | TERMINAL/native -1 typed 매핑, poller 재진입 lease, close 순서, context term wake, POLLOUT 빈 drain 제거 | 697k → 775k |
| .NET | thread-local wrapper pool, source-generated import, stack + ArrayPool staging, direct reply | snapshot을 거절 시점으로, pump spin 제거, ownership rollback, EINTR 관용, ESHUTDOWN 매핑, HashSet 할당 제거 | 267k ↔ 281k(동률) |
| Java | native part 직접 전달, template·scratch 재사용, bounded pool, direct reply | runtime owner는 POLLCOMPLETION만 대기(spin 제거), terminal typed 매핑, 즉시 성공 경로의 Pending/Future/map 할당 제거, REQUEST 경로 직렬화 제거 | 리뷰 커밋 참고 |
| Node | routed frame pool, identity cache, inline 8-part staging, direct reply builder | 성공 경로의 entry/Promise/map 등록 제거, zero-timeout pump spin 제거, POLLOUT 선형 순회 → 카운터, close 시 handle 취소, ESHUTDOWN 매핑 | 28k(포팅 후) → 145k(포팅 전 153k) |
| Python | owner-aware native clone, completion을 asyncio thread에서 dispatch(3.08배), nested Task·sleep 제거 | 성공 경로 bytes 이중 복사 제거, event loop spin 제거(blocking poller + wake FD), O(n) 해제 → O(1) | 7.9k → 20k |
| Go | `zlink_msg_copy` clone submit | 즉시 성공 send의 poller/goroutine 생성 제거, drain spin → bounded blocking poll, terminal 원인 보존 | 81.5k → 190k |
| Rust | `try_clone` reply | 실행기 spin → reactor 스레드 + public Poller 구동, lazy 등록(보류 재생), 전송당 poller/복사/할당 제거, submit gate RwLock, REQUEST spin 제거 | 172k(포팅 전) → 259k |

## 4. 기각한 후보 (다시 시도하지 않을 것)

- public wrapper, Future/Task/CompletionStage, callback userdata의 pool 재사용. `GCHandle` 재사용은
  늦은 completion이 새 operation을 완료시키는 ABA 위험(.NET).
- .NET direct 2-part native submit: SENDSEND TCP 14~16%, REQ/REP 8~41% 회귀.
- Node TSFN STREAM payload pool: mutex와 반환 비용.
- Python: multipart outbound를 한 native 호출로 합치기(allocator corruption), 중앙 round-robin
  scheduler, 측정 tuple 사전 생성, direct `Received` pool(+2.81%), `POLLIN | POLLCOMPLETION`
  조합(drain 안정성 저하), GIL을 유지하는 `PyDLL` 호출(교착).
- 실행기 turn마다 timeout-0 poll로 진행을 흉내 내는 방식 전부(spin). 측정값이 좋아 보여도 CPU
  100%이며 다른 스레드의 진행을 빼앗는다.
- 인위적인 in-flight 상한이나 2단계 측정으로 backpressure를 우회하는 러너 변경
  ([PERF_MULTI_TEST_POLICY.md](PERF_MULTI_TEST_POLICY.md) §1.2, §5.1 위반).

## 5. 적용 절차

1. 대상 binding의 `perf/run_benchmarks.sh --pattern DEALER_ROUTER --transports tcp --msg-sizes 1024 --duration 3 --runs 1`로 "전"을 기록한다. 같은 조건의 C 값을 같은 시각에 잰다.
2. §2 체크리스트를 성공 경로 → 거절 경로 → drain/wake → 수신 순으로 코드에서 판정하고 표로 남긴다.
3. 수정마다 public API·ownership contract test와 sleep 없는 회귀 테스트(5회 반복)를 추가한다.
4. "후"를 같은 명령으로 재고, single(PAIR·DEALER_ROUTER·PUBSUB, tcp·inproc) + multi(clients 8, 1024·65536, DEALER_DEALER·DEALER_ROUTER_SENDSEND·PUBSUB) 스모크가 status complete·0 없음인지 확인한다.
5. 5% 이상 개선된 항목만 "효과 확인"으로 이 문서 §3에 올리고, 수치·report 경로를 함께 적는다.
   러너 자체 변경(scheduler, drain, fairness)의 효과는 library 최적화와 합산하지 않는다.
