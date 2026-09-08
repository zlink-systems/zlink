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
  [bindings-library-performance-improvement-plan-core-0.17.0.ko.md](perf/bindings-0.17.0/bindings-library-performance-improvement-plan-core-0.17.0.ko.md)를 따른다.
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

### 3.1 0.17.2 캠페인 적용 현황 (2026-09-07~08, `doc/perf/perf/bindings-0.17.0/`)

측정은 고정 Core 0.17.2, `tcp`, clients 100, 5 sizes(64~65536), duration 5, C와 paired. 판정 수치는
계획서 §9.x.2와 `decisions.ko.md` D-BP1~D-BP32. **"러너 정합"은 라이브러리 개선이 아니라 측정
의미를 C와 맞춘 것이며 library 효과와 합산하지 않는다.** 이 표는 "무엇을 이미 했고 무엇이
안 됐는지"를 언어 간에 교차 적용하기 위한 것이다.

| 언어 | binding 라이브러리 채택 | 러너 정합 적용 | 비용 지도 결론 | `tcp` 판정(6 pattern) |
|---|---|---|---|---|
| C++ | 즉시 admission SEND의 completion bundle·waiter map 생략(D-BP 러너 pass 1, `p3pin`); completion-owner 이관(requester를 public poller `POLLCOMPLETION`에) | RTT loop 제거(전 binding), 상한 제거·C turn 구조(`31c5e4f7f0`), relay 수신-송신 결합(`e0862e1e5c`), client echo drain(`33f63ae89d`) | **지배 항목 없음** — 요청당 잔여 Ir C 6.4k vs C++ 13.6k, 차이가 380~830 Ir짜리 10개에 퍼짐; coroutine frame은 516 Ir(격차의 7%); C에 없는 요청당 할당 5개(≈408 Ir); Core I/O 연산 2.2~2.9배 유발(922 Ir)(D-BP26) | 통과 4(DD 97.6·PUBSUB 95.3·SS 91.9/91.7), 보류 2(REQREP 72.6/75.8) |
| .NET | (pass 2, 0.17.1) reply submit closure→struct −352 B/op, pump closure 지연 | relay 결합(`fb3f37191d`), client echo drain(검토 완료·회귀 검증 티켓 대기, D-BP34 예정) | **지배 항목 둘** — send builder 177.7 ns·80.5 B/msg(격차 35%), message helper P/Invoke 전환 137.5 ns(27%); GC 0.3%로 무관(D-BP31). 둘 다 **공개 API 계약**: builder는 공개 operation 객체, close는 terminal 시점. 전환 10→9 후보는 효과 없음(D-BP32) | 미달 6(DD 62.5·PUBSUB 61.0·DR SS 63.9·RR SS **69.5(3-run, 0.46%p 부족)**·DR REQREP 60.5·RR REQREP 63.1; 목표 70). 전 pattern 60~70%로 평탄 = D-BP31의 두 지배 항목(builder·P/Invoke)이 pattern 무관하게 걸린다는 뜻. tcp 6/6 완료 |
| Java | — | relay: `submit_sync()`가 SNDTIMEO 1 s 만료 시 BACKPRESSURED를 throw해 4096 B에서 결정적 실패 → async submit + 단일 pending reply FIFO(`611b022b37`); TIMEOUTS 계측(`3cc840d82e`); teardown 창 C 정합(`ad89fb599a`) | 미작성. 65536 B만 무너지는 **크기 의존형**: DD 53.7%, DR REQREP 10.9%·RR REQREP 11.2%(latency 14.5~15.6x; 64~4096 B는 52~59%로 평탄). PUBSUB 65536 B는 138%이므로 수신 경로가 아니다. 정적 분석(`log/2026-09-08-java-reqrep-64k-analysis.ko.md`): 크기 분기는 코드가 아니라 **byte HWM**(1 MiB → 65536 B는 소켓당 16개)이고, REQREP client 루프에 **admission backpressure gate가 없어**(C `blocked_out` 없음, binding이 retained 제출을 알리지 않음) retained 대기가 latency에 계상된다. SENDSEND는 CAS gate가 있어 정상. JNI는 없다(FFM). POLLOUT gate는 **기각**(POLLOUT은 admission credit이 아닌 backpressure 회복 edge — `core/doc/spec/core/05-polling.ko.md:54-68`; 4096 B에서 0건, 65536 B에서 99%). 진단: 소켓당 미완료 최대 2 → 쌓임이 아니라 **turn당 17.5 ms 왕복 비용**(4096 B 0.61 ms). `snd_pending_bytes` 65,664(1 MiB 창의 6%)인데 POLLOUT 99% → 65536 B의 실효 admission 창을 byte HWM이 정하지 않는다 — Java request terminal(retained+WRITABLE 회복)과 Core admission의 상호작용, 다음 캠페인(0.17.4 뒤) 첫 조사 항목(`log/2026-09-08-java-reqrep-64k-gate.ko.md`) | PUBSUB 통과 93.2; DR SS 통과 73.2(3-run); RR SS 통과 84.9(throughput; latency 한도 초과, 4096 B teardown 간헐 실패 1/4 — client가 admission 대기 중 수신을 멈추는 cadence 결함); DD 72.0·DR REQREP 46.3·RR REQREP 47.6 미달(REQREP 둘 다 65536 B 10.9~11.2%) — tcp 6/6 완료 |
| Node | **pass 1(0.17.3-alpha)**: 2-part unrouted receive의 N-API envelope·snapshot 객체 3개 제거 → DD 64 B +4.6%, alloc 608→512 B/msg(`log/2026-09-08-node-cost-map.ko.md`) | relay 결합(`d744799803`); client echo drain + teardown 창 C 정합(`93bcf7156a`, D-BP34) | **지도(alpha, 64 B)**: DD 1,753 ns/msg(C 584) — recv(native 수신+JS materialization) CPU 75%, 격차의 ≥62.7%; DR REQREP 7,193 ns/op(C 2,617) — Promise/completion과 routed receive/reply도 각각 20%↑(계약 경계). 전 pattern 25~43%로 평탄. **초 단위 latency**가 DD(1.3 s)·DR SS(64~4096 B 1,900~2,500x)·RR SS(256~4096 B 500~1,600x)에 걸쳐 있고 REQREP은 3~16x — cadence 조사(astra)는 server drain 제한·timestamp 재사용을 배제했고 원인 미확정. recv materialization 비용·실제 queue 점유·active drain 경계로 분리해 볼 것 | DD 43.1·PUBSUB 29.6·DR SS 33.7·RR SS 35.5·DR REQREP 32.5·RR REQREP 29.7 미달(목표 60) — tcp 6/6 완료(SS는 echo drain 적용 값) |
| Go | — | REQREP 상한 제거·C turn 구조(`add837942b`, 0.17.2 동시 multipart로 재개); relay는 이미 동기 제출; 단방향 drain·echo drain·probe 단일 제출(C turn 모델, 커밋) | 미작성. REQREP 작은 크기 latency ~3x(Node도 동일) → **async terminal 왕복 고정 비용** 후보 | PUBSUB 39.8·DD 31.5·DR SS 50.1(5-run 경계, 목표 53)·DR REQREP 67.7·RR REQREP 67.1 미달; RR SS 65536 B `차단`(server shutdown). DD 64 B 14%·latency 7x = turn 구조의 제출당 goroutine 비용 후보 |
| Rust | — | relay 결합(`fb3f37191d`) | 미작성. **4096 B만** latency가 초 단위(DR SS 2,226x·RR SS 1,154x; 64~1024 B는 0.6~1.8x, 65536 B 1.6~5x) → 크기 하나에 걸린 client 대기 구조 후보(위 교차 현상). 65536 B 처리량 21~38%도 별도 | DD 60.5·PUBSUB 89.0·DR SS 73.5·RR SS 63.8·DR REQREP 54.3·RR REQREP 55.1 미달(목표 95/85) — tcp 6/6 완료. REQREP 비율이 0.17.0 3-run(62~72%)보다 낮은 건 Rust 절대값(64 B 206K, 09-07과 동일)이 아니라 C 기준 상승(351K) 때문 |
| Python | — | relay 결합(`fb3f37191d`) | 미작성. 전 pattern 14~24%로 **평탄**(크기·pattern 무관한 고정 per-message 비용 = 인터프리터·GIL·asyncio 경계) — 세부 지도 전에 "메시지당 Python 함수 호출 수"를 먼저 셀 것(pass 2에서 7→? 이미 줄였음). 65536 B만 36~52%로 C 대비 상대 개선 | DD 15.9·PUBSUB 34.0·DR SS 24.2·RR SS 23.3·DR REQREP 14.2·RR REQREP 15.7 미달(목표 60) — tcp 6/6 완료 |

**교차 적용 후보 (효과가 확인된 것을 다른 언어에)**
- C++의 "즉시 admission SEND는 completion bundle을 만들지 않는다"(§2.1)는 Java·Node에 이미 있다(§3). .NET·Go·Rust·Python에 같은 경로가 있는지 비용 지도로 확인할 것.
- .NET의 reply closure→struct(−352 B/op)는 closure를 쓰는 Node·Python reply 경로에 후보.
- 러너: relay 수신-송신 결합과 client echo drain은 **7개 전부**에 필요했다(D-BP24). Java·Go만 처음부터 C 모델이었다.
- **STREAM(Core 0.17.3-alpha, D-BP37)**: C++ 96.7·.NET 95.8·Java 103.4 통과, Go 65.9 경계, Node 35.1 미달, **Rust 20.4·Python 19.1 미달** — Rust만 크기 무관 ~88 K ops/s(메시지당 ~11 µs 고정 비용)로 다른 pattern(54~89%)과 동떨어져 있어 Rust STREAM 서버 수신·packet 경로가 첫 지도 대상.
- 러너 집계: 다중 run의 대표 RESULT는 metric별 median 한 줄(D-BP35). .NET Multi는 마지막 run 값, Go는 중복 raw를 내고 있었다 — 새 언어 러너를 볼 때 "5-run인데 RESULT가 5줄인가/마지막 값인가"를 먼저 확인할 것.
- **Node의 초 단위 latency는 Single(client 1·동기 API)에서도 그대로다**(sg1: PAIR 1,179x·DR 2,104x·RR 3,268x, 처리량은 54~58%; REQREP은 12/11%·360x). 100 client·async terminal과 무관한 **수신 경로 자체**의 문제 → Node 비용 지도가 찾은 `recv`(native 수신 + JS materialization 75%)와 같은 자리. Node Single one-way 처리량(54~58%)이 Multi(30~43%)보다 높으므로 Multi 격차에는 100 client·async terminal 성분이 추가로 있다.
- **초 단위 latency 이상은 언어 하나의 문제가 아니다** (0.17.2 tcp 1-run, 같은 셀의 다른 크기는 정상): Node DD 64~4096 B(1.2 s), Node RR SS 256·1024·4096 B(C 대비 500~1,600x), Rust DR SS 4096 B만(2,226x, 64~1024 B는 0.6~1.4x), Java SS 일부. C·C++·.NET에는 없다. 크기 하나만 튀고 처리량은 정상이므로 처리량 격차와 분리해 다룬다 — client 수신 cadence 조사(astra)가 Java에서 "admission pending + poll(0)에 수신 없음 → admission 신호만 대기"를 찾았고 Node는 미설명. Rust도 같은 client 대기 구조인지 먼저 볼 것.

**언어별 격차의 성격이 다르다** — C++는 퍼진 비용(개별 후보 무의미), .NET은 두 항목이 지배하나 공개 API에 묶임, Node·Go는 latency 이상이 throughput과 별개로 있음. 비용 지도 없이 후보를 고르면 pass 2가 오히려 나빠진 C++ 사례(D-BP21)를 반복한다. **pass 전에 지도부터.**

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
- **(0.17.2 캠페인 추가)** C++: `_continuation_weak` fallback 제거·lifetime 없는 슬롯 할당 제거(pass 2 — DR 72.87→69.35%로 악화, 도달 불가 경로를 throw로 바꾸는 내부 계약 축소), REQUEST completion 중복 합류 상태 제거(pass 1 — 악화), REQUEST에 SEND의 ID 0 즉시 완료 적용(request terminal 계약 위반), entry/result 통합(caller detach 수명), RID snapshot을 빌린 pointer로(exact-target 재제출 보존), route lookup·pipe cache(Core 소유), WRITABLE capture 즉시 재제출(Core 계약: `NO_DATA`까지 비운 뒤 재제출, `socket/README.ko.md:1080`).
- **(0.17.2 캠페인 추가)** .NET: send builder 80.5 B/msg 제거(공개 operation 객체 자체), close를 다음 init에 합침(ownership release가 terminal 계약), GC 감소를 가설로 쓰는 것(GC pause 0.3%). 유일 계약 유지 후보 전환 10→9는 효과 0(D-BP32).
- **(0.17.2 캠페인 추가)** 러너: pending 수 상한으로 relay 누적 막기(§5 위반 — 답은 "앞 admission을 기다린 뒤 다음 제출"), `send_drain_timeout`·deadline 늘리기, 중앙값이 정상이라고 개별 run 이상을 무시하기(D-BP29 — C도 튄다는 걸 중앙값이 가렸다).
- 인위적인 in-flight 상한이나 2단계 측정으로 backpressure를 우회하는 러너 변경
  ([PERF_MULTI_TEST_POLICY.md](PERF_MULTI_TEST_POLICY.md) §1.2, §5.1 위반).

## 5. 적용 절차

1. 대상 binding의 `perf/run_benchmarks.sh --pattern DEALER_ROUTER --transports tcp --msg-sizes 1024 --duration 3 --runs 1`로 "전"을 기록한다. 같은 조건의 C 값을 같은 시각에 잰다.
2. §2 체크리스트를 성공 경로 → 거절 경로 → drain/wake → 수신 순으로 코드에서 판정하고 표로 남긴다.
3. 수정마다 public API·ownership contract test와 sleep 없는 회귀 테스트(5회 반복)를 추가한다.
4. "후"를 같은 명령으로 재고, single(PAIR·DEALER_ROUTER·PUBSUB, tcp·inproc) + multi(clients 8, 1024·65536, DEALER_DEALER·DEALER_ROUTER_SENDSEND·PUBSUB) 스모크가 status complete·0 없음인지 확인한다.
5. 5% 이상 개선된 항목만 "효과 확인"으로 이 문서 §3에 올리고, 수치·report 경로를 함께 적는다.
   러너 자체 변경(scheduler, drain, fairness)의 효과는 library 최적화와 합산하지 않는다.
