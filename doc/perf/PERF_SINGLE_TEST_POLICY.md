# zlink Single Performance Test Policy

> **적용 범위**: zlink 전체 (core + bindings) — single-client 벤치마크
> **Policy Version**: 2.3
> **Date**: 2026-09-07
> **Scope**: `perf/single` 성능 테스트 정책
>
> 본 정책은 `bindings/c/perf`의 single C benchmark와 in-repo single perf 자산이
> 존재하는 바인딩에 동일한 기준으로 적용한다.
> 단, 각 언어의 구현 완성도와 지원 패턴 범위는 다를 수 있으므로 실제 parity
> 수준은 언어별로 점검/정렬 대상이 된다.
>
> 언어별 적용 범위는 [PERF_POLICY.md](PERF_POLICY.md) 상단을 참조한다.
>
> **상위 문서**: [PERF_POLICY.md](PERF_POLICY.md) — 공통 원칙, 디렉터리 구조,
> RESULT 형식, 결과 저장, 출력 형식, 실패 처리, 환경 변수(공통), 리팩토링 원칙
>
> **관련 문서**:
> [PERF_MULTI_TEST_POLICY.md](PERF_MULTI_TEST_POLICY.md)
>
> 본 문서는 single suite **전용** 정책만 기술한다.
> 양 suite에 공통으로 적용되는 규칙은 상위 문서에서 관리한다.
> Spot 성능 시험은 Framework 성능 시험이 소유하며 binding single suite에는 포함하지 않는다.
---

## 1. Single 핵심 정책

| 항목 | 기준 |
|------|------|
| 측정 모델 | ready + active(duration) |
| throughput | one-way는 `active 수신 건수 / active 시간(초)` (msg/s), request-reply는 `active 왕복 완료 수 / active 시간(초)` (ops/s) |
| latency | one-way는 active 구간 수신 payload header timestamp 기반, request-reply는 request 제출부터 reply completion까지의 왕복 시간 기반 (internal ns / external ms) |
| 대표값 | runs > 1일 때 metric별 median |
| 저장 경로 | `perf/results/single/report/` 단일 |

- 목적: 단일 소켓 경로에서 throughput, bandwidth, latency를 측정한다.
- Single suite는 `PAIR`, `PUBSUB`, `DEALER_DEALER`, `DEALER_ROUTER`, `ROUTER_ROUTER`,
  `DEALER_ROUTER_REQREP`, `ROUTER_ROUTER_REQREP` 7개 pattern을 측정한다. request/reply를
  포함한다(D-BP6이 D-BP3을 되돌림).
- 같은 active 구간에서 동일 메시지 집합으로 latency도 함께 집계한다.
- cpu/mem은 single 기본 perf surface와 RESULT 계약에 포함하지 않는다.
- `single`의 공식 lifecycle은 `ready -> active`다.
- size 변경 시마다 별도 프로세스로 실행하여 케이스 간 메트릭 오염을 방지한다.
- ready bool/count를 복사하기 위한 별도 state struct, heap alloc, mutex/cv 계층은
  만들지 않는다.
- 한 줄 요약: `single = ready + active`

### 1.1 실행 모델

이 절은 러너가 반드시 지켜야 할 실행 모델을 정의한다. 여기서 잘못 읽으면 측정값 자체가
무의미해지므로, 오해하기 쉬운 지점을 먼저 못박는다.

#### 1.1.0 동기·비동기가 뜻하는 것과 뜻하지 않는 것

`single`은 **동기 실행 모델**, `multi`는 **비동기 실행 모델**이다. 이 구분이 뜻하는 것은
**진행을 누가 구동하는가** 하나뿐이다.

| | single | multi |
|---|---|---|
| 진행을 구동하는 주체 | 러너가 만든 **전용 OS thread**가 직접 구동한다 | 그 언어의 **비동기 실행 모델**(coroutine, async runtime, event loop, goroutine)이 구동한다 |
| 측정 구간에서 금지 | coroutine, async task, Promise/Future executor, event-loop yield | 없음 (단, PUB/XPUB publish와 raw reply는 multi에서도 synchronous terminal) |

> **오해 금지 — 동기 실행 모델은 "한 건 보내고 응답을 기다린다"는 뜻이 아니다.**
>
> 동기는 *진행을 전용 thread가 직접 구동한다*는 뜻이지, *in-flight를 1로 묶는다*는 뜻이
> 전혀 아니다. 두 suite 모두에서 **응답을 기다린 뒤 다음을 보내는 RTT 전용 루프는
> 금지**다. RTT 루프는 부하 수준을 인위적으로 1로 낮춰 처리량 측정을 무의미하게 만든다.

#### 1.1.1 admission과 reply는 별개의 사건이다

request/reply 러너를 잘못 짜는 원인은 거의 항상 이 하나다. Core 공개 API에서
**제출(admission)이 끝나는 시점과 응답(reply)이 오는 시점은 서로 다른 사건**이다.
`zlink_request_part`를 `DONTWAIT` + `FINAL`로 부르면
(`core/include/zlink/socket/api.h` 의 `zlink_request_part` 주석):

| 반환 | 뜻 | 러너가 할 일 |
|---|---|---|
| `ZLINK_SUBMIT_OK` + nonzero REQUEST completion ID | **제출됐다.** reply timeout이 이 시점에 시작한다 | outstanding을 하나 늘리고 **곧바로 다음 request를 제출한다.** 여기서 reply를 기다리지 않는다 |
| `ZLINK_SUBMIT_BACKPRESSURED` + `EAGAIN` + nonzero WRITABLE 대기 토큰 | 지금은 더 못 받는다. 이것이 **포화 경계**다 | 제출을 멈추고 completion을 진행한다. 그 토큰의 `ZLINK_COMPLETION_WRITABLE`이 오면 **같은 요청을 다시 제출**한다 |
| `ZLINK_SUBMIT_NOT_CONNECTED` | mandatory ROUTER route 없음. 토큰 없음 | 실패로 기록한다 |

reply는 나중에 **completion queue**로 도착한다. 즉 "응답을 받는다"는 것은 함수가 반환하기를
기다리는 일이 아니라 completion을 drain하는 일이다.

#### 1.1.2 requester thread의 실행 형태

전용 requester thread 하나가 **제출과 completion 진행을 교대로** 구동한다. 이것이
"synchronous callback terminal + completion poller"가 뜻하는 실행 형태다.

```text
while now < active_deadline:
    # 1) 포화 경계까지 연속 제출
    while true:
        r = request_submit(DONTWAIT, FINAL)
        if r == SUBMIT_OK:            outstanding += 1; continue
        if r == SUBMIT_BACKPRESSURED: 대기 토큰 기록; break      # 포화 경계
        else:                         실패 기록; break

    # 2) completion 진행 — reply와 WRITABLE을 NO_DATA까지 drain
    for record in completion_drain():
        if record is REPLY:    outstanding -= 1; 집계(record)
        if record is WRITABLE: 그 토큰의 요청을 다시 제출
```

같은 requester thread가 두 단계를 교대로 돈다. 별도 OS thread로 나눠도 된다. 어느 쪽이든
coroutine·executor·event loop를 쓰지 않는다.

동기 모델에서 동시 진행을 늘리는 또 다른 방법은 **전용 requester thread를 여러 개 두는
것**이다. 이 정책은 그것을 허용한다. thread 수를 늘려 부하를 만드는 것과 한 thread에서
연속 제출하는 것 중 무엇을 쓰든, 금지되는 것은 in-flight를 1로 묶는 RTT 루프뿐이다.

#### 1.1.3 고수준 binding의 공개 API가 다른 모양일 때

binding의 공개 request terminal이 admission 결과를 직접 돌려주지 않고 **admission과 reply를
하나의 awaitable로 합쳐** 제공하는 경우가 있다(`async()` 계열). 이때도 결론은 같다.

- **그 awaitable을 기다리지 말고 계속 제출한다.** 반환된 awaitable은 미완료 집합에 넣고,
  완료되는 것부터 drain해 집계한다.
- backpressure는 binding이 내부에서 처리한다. binding의 awaitable terminal은 `DONTWAIT`로
  admission을 **한 번** 시도하고, 거절되면 요청을 붙들고 **정확히 그 대기 토큰의
  `WRITABLE`에서만** 재개한다. binding은 retry timer도 worker thread도 두지 않는다.
  즉 §1.1.1의 경계가 binding 안에서 그대로 지켜진다. 러너가 그 경계를 따로 관측할 필요가
  없고, 관측하려고 `ZLINK_POLLOUT` 같은 aggregate hint를 쓰면 오히려 부정확해진다.
- 러너는 **미완료 개수 상한** 하나만 둔다. 상한이 없으면 wire는 HWM에서 막혀 있는데
  미완료 객체만 메모리에 쌓인다. 상한은 wire를 포화시키기 충분한 값이면 되고, 모든
  binding이 같은 값을 쓰며 report의 Effective Options에 노출한다.
- single에서는 이 awaitable을 executor나 event loop에 맡기지 않는다. 전용 requester
  thread가 직접 completion을 진행시켜 완료시킨다.

#### 1.1.4 수신 경로와 언어별 고정 조건

- 측정 집계가 걸리는 수신 경로는 **recv 모델**(poller `POLLIN` readiness 감지 +
  nonblocking `recv` drain)을 기본으로 한다. callback으로 측정 data delivery를 직접
  집계하는 경로는 single에서 사용하지 않는다.
- raw send는 blocking terminal을 사용하므로 HWM 도달 시 Core가 sender thread를 대기시킨다.
- Go는 역할별 goroutine을 active 구간 전체에서 `runtime.LockOSThread()`로 고정한다.
  이 goroutine은 다른 작업과 OS thread를 공유하지 않는다. Node는 `worker_threads`,
  Python은 `threading.Thread`에서 synchronous terminal과 recv loop를 실행한다. C++은
  `co_await`, .NET은 `Task`, Rust는 Future executor를 single 측정 경로에서 사용하지 않는다.
- `PAIR`, `PUBSUB`, `DEALER_DEALER`, `DEALER_ROUTER`, `ROUTER_ROUTER`는 recv 모델로
  active payload를 집계한다. `DEALER_ROUTER_REQREP`, `ROUTER_ROUTER_REQREP`는 §1.1.1~§1.1.3의
  request-reply completion 모델로 active 왕복 완료를 집계하며, requester submit flow,
  requester reply completion flow, replier recv/reply flow를 같은 process 안에서 동시에
  구동한다.

#### 프로세스/스레드 모델

single은 **단일 프로세스** 안에서 sender와 receiver를 동시에 구동한다. 구현은
sender thread + main recv loop 또는 sender thread + receiver thread 처럼 나뉠 수
있지만, 측정 중 송신과 수신/progress가 동시에 진행되어야 한다.
benchmark process의 setup과 ready 확인도 같은 thread들이 synchronous API로
수행한다. 실행 스크립트가 process를 시작하고 stdout을 읽는 orchestration 방식은
이 규칙의 대상이 아니다.
single 의 기본 패턴은 one-way 측정 surface를 사용한다. request-reply 비용은
기존 one-way 패턴의 mode를 바꾸지 않고 별도 `*_REQREP` 패턴으로 측정한다.

**raw one-way 패턴** (PAIR, PUBSUB, DEALER_DEALER, DEALER_ROUTER, ROUTER_ROUTER):
```text
+--------------------------------------+
| process                              |
| sender flow       recv/progress flow |
| blocking send --> poller POLLIN      |
| continuous        recv drain         |
|                   metric collect     |
+--------------------------------------+
```
- sender flow: synchronous blocking send를 연속 수행한다. HWM 도달 시 Core가
  sender thread를 대기시킨다.
- recv/progress flow: recv 루프 안에서 throughput/latency 집계를 수행한다.
  recv loop 구조와 active 집계 anchor는 모든 raw one-way 패턴과 바인딩에서
  동일 의미로 유지해야 한다.

**raw request-reply 패턴** (DEALER_ROUTER_REQREP, ROUTER_ROUTER_REQREP):
```text
+------------------------------------------------+
| process                                        |
| requester thread: request submit <-> progress  |
|                   metric collect               |
| replier thread: recv request -> reply          |
+------------------------------------------------+
```
- request submit flow는 ready gate 통과 후 requester socket으로 request를 가능한
  만큼 연속 제출한다. submit 이 backpressure 를 만나면 새 request 제출을 멈추고
  completion progress 쪽이 reply를 drain할 수 있게 한다.
- requester progress flow는 같은 requester socket에서 requester progress loop를
  수행한다. 이 loop는 blocking recv/wait 또는 그와 같은 의미의 public completion
  progress 경로로 reply completion을 drain한다. 진행 주체는 반드시 그 역할의 전용
  OS thread이며, coroutine, async task 또는 Future executor로 completion을 진행하지
  않는다(§ 1.1.2). binding의 공개 terminal이 admission과 reply를 하나의 awaitable로
  합쳐 제공하면 § 1.1.3의 규칙을 따른다 — awaitable을 기다리지 말고 계속 제출하고,
  완료되는 것부터 drain한다. 같은 handle 동시 사용은 public handle concurrency 계약을
  따른다.
- replier thread는 request를 받은 뒤 source routing id와 request sequence로 reply를
  되돌려 보낸다.
- throughput은 active 구간에서 reply까지 완료된 왕복 수로 계산한다.
- latency는 request 제출 시각부터 해당 reply completion까지의 시간으로 계산한다.

**공통**:
- raw one-way single에는 `EAGAIN` 기반 pending 관리, send-ready handler 등
  multi에서 사용하는 backpressure 메커니즘을 적용하지 않는다. one-way sender는
  blocking send terminal이 돌려주는 backpressure만 사용하며, in-flight 메시지 수를
  코드로 관리하거나 상한으로 고정하지 않는다
  ([PERF_POLICY.md § 7.2](PERF_POLICY.md)).
- single reqrep은 request API의 public backpressure 결과를 사용해 submit flow를
  멈추고, completion progress가 pending reply를 drain한 뒤 다시 submit한다.
  inflight request 수를 코드로 관리하거나 상한으로 고정하지 않는다. request는
  backpressure(`EAGAIN`/`BACKPRESSURED`)를 만날 때까지 연속 제출하며, HWM은 send
  admission queue를 제한한다. reply를 기다리는 request 수는 실제 admission과
  completion 속도로 정해지며, **응답을 받아야 다음 request를 보내는 1:1 ping-pong으로
  직렬화하지 않는다.**
- single reqrep **유효 집계**: throughput과 latency는 active 측정 구간 안에서
  reply까지 완료된 왕복만 계산한다. 측정 종료(deadline) 시점에 응답이 오지 않은
  outstanding request는 완료 왕복이 아니므로 집계에서 제외한다. deadline 이후의
  bounded drain은 완료 카운트를 늘리지 않는 정리 동작이다. 각 (pattern, transport,
  size) 케이스는 별도 프로세스로 실행되므로, 남은 outstanding은 프로세스 종료로
  자연 정리된다. 별도 cancel/close 로직을 요구하지 않는다.
- one-way 송신이 transient 오류(`EAGAIN`/`EINTR`/`ETIMEDOUT`)를 만나면 **1 ms 대기한
  뒤 같은 sequence를 새 `sent_ts_ns`로 다시 stamp해 재제출한다.** sleep 없는 즉시
  반복(busy retry)과 1 ms를 넘는 backoff는 모두 금지한다. 이 1 ms 대기는
  [PERF_POLICY.md § 1.1.2](PERF_POLICY.md)의 hot loop sleep 금지에 대한 명시적
  예외이며, blocking send terminal이 `PERF_SINGLE_SNDTIMEO_MS` 만료로 되돌아온
  경우의 bounded 복구 절차다. 정상 흐름 제어(HWM backpressure)는 sleep 없이 Core가
  sender thread를 대기시켜 처리한다.
- latency sample 계산, percentile sample 축적은 recv 루프 내에서 처리한다.
- phase 종료 후에는 bounded idle drain을 반드시 수행한다. 이 절차는 "deadline 이전에
  송신되어 queue/in-flight에 남아 있던 메시지를 추가 recv로 비운다"는 의미이며,
  active 결과 집계는 본 문서가 정의한 active 유효 메시지 조건을 계속 따른다.
- idle drain은 single recv one-way 공통 계약이다. 특정 one-way 패턴이나 특정
  binding만 더 길거나 다른 의미의 종료 drain을 두면 안 된다.
- request-reply 패턴의 종료 drain은 active deadline 이전에 제출된 request의
  bounded completion drain만 허용한다. active deadline 이후 새 request를 제출해서
  완료 수를 늘리면 안 된다.

### 1.2 실행 계약 불변식

- `single`의 최소 측정 단위는 `pattern/transport/size/run` 이다.
- runner는 size마다 perf 바이너리를 **다시 실행**해야 한다.
- 하나의 perf 바이너리 프로세스가 여러 size를 내부 루프로 순회하면 정책 위반이다.
- perf 바이너리는 해당 size 케이스를 측정하고 `RESULT` line만 출력한다.
- size 반복 실행, runs 집계, markdown table 출력, 결과 파일 저장은 runner 책임이다.
- single 리팩토링은 위 책임 분리를 유지해야 하며, 변경 시 자동 검증(test)도
  함께 갱신해야 한다.

### 1.3 금지 단계/개념

`single`에서 아래 단계/개념은 새로 만들지 않는다.

- `preflight`
- `prime`
- `settle`
- `stable`
- `quiet`
- `expected_ready_count > 1`

위 항목이 이미 존재하지만 실제로는 ready 이벤트 하나 대기하거나 phase 종료를
우회적으로 표현한 것뿐이면 삭제한다.

> 공통 금지 단계(`quiescent` 등)는 [PERF_POLICY.md § 1.1](PERF_POLICY.md) 참조.

### 1.4 Poller wait timeout 및 shutdown 신호

single 패턴은 같은 process 안에서 sender thread + receiver(main) thread
조합으로 구성되는 경우가 많다. receiver 가 sender 의 phase 종료를
감지하는 방법은 multi 와 동일한 **wire-level stop token** 패턴을 사용한다.

| 항목 | 규칙 |
|------|------|
| receiver poller wait timeout | **`-1`** (signal-driven wait) |
| sender 가 active phase 종료 후 | **stop token (`__zlink_perf_stop__`) 한 번 blocking send** (deadline 무시) |
| receiver | `-1` poller wait → recv → `is_stop_token(...)` 검사 → 종료 |
| `std::atomic<bool> sender_done` + 짧은 polling (1–10 ms) | **금지**. 기존 코드는 wire-level stop token 패턴으로 마이그레이션한다 |

이 규칙의 의도는 multi 측 정책 ([PERF_MULTI_TEST_POLICY § 1.3.1](PERF_MULTI_TEST_POLICY.md))
과 동일한 idiom을 single 에도 적용하여 cross-platform / cross-binding
shutdown 패턴을 wire 레벨로 통일하는 것이다. 별도 fd / eventfd / pipe /
cancellation token / signal helper 를 도입하지 않는다.

drain 의 자연스러운 처리:
- sender 가 stop token 보내기 직전까지의 in-flight 메시지는 wire 위에서
  먼저 도달하므로 receiver 가 차례대로 소비한 뒤 마지막으로 stop token
  을 만난다. 별도 deadline 기반 drain loop 없이 phase 종료가 처리된다.
- 이 소비는 종료 정리이며 집계 규칙을 바꾸지 않는다. active 집계에 들어가는
  메시지는 § 2.1의 active 유효 메시지 규칙을 만족한 것뿐이다. active deadline
  이후에 recv한 tail 메시지는 stop token 도착 전이라도 집계에서 제외한다.

---

## 2. Phase 규칙

```text
[single phase]:
raw pattern      = [ready] -> [active(duration)] -> [idle_drain] -> [done]
PUBSUB            = [ready] -> [post_ready_settle] -> [active(duration)] -> [idle_drain] -> [done]
```

| Phase | 방식 | 기본값 | 환경 변수 |
|-------|------|--------|-----------|
| ready | event-based | raw socket `CONNECTION_READY` | `PERF_CONNECT_READY_TIMEOUT_MS` 계열 timeout |
| post-ready settle | bounded stabilization | PUBSUB만 수행 | `PERF_SINGLE_PUBSUB_READY_SETTLE_MS` |
| active | time-based | 5s | `PERF_SINGLE_DURATION_SECONDS` |
| idle drain | bounded recv drain | recv one-way 전체 수행 | `PERF_SINGLE_RCVTIMEO_MS` 계열 timeout bound |
| completion drain | bounded completion drain | request-reply 전체 수행 | `PERF_SINGLE_RCVTIMEO_MS` 계열 timeout bound |

### 2.0.1 Single 패턴별 handshake 고정

아래 표는 `bindings/c/perf` single runner와 benchmark binary가 사용하는
패턴별 handshake 계약이다. 다른 바인딩 single perf는 같은 ready source,
active 시작 조건, 종료 신호를 사용해야 한다.

| 패턴 | process 구조 | ready gate | active 시작 | 종료 |
|------|--------------|------------|-------------|------|
| `PAIR` | 단일 process 안 sender thread + recv/progress flow | raw socket `CONNECTION_READY` | ready gate 통과 직후 `phase=active` | sender가 wire stop token 송신, recv/progress flow는 idle drain 후 종료 |
| `DEALER_DEALER` | 단일 process 안 sender thread + recv/progress flow | raw socket `CONNECTION_READY` | ready gate 통과 직후 `phase=active` | sender가 wire stop token 송신, recv/progress flow는 idle drain 후 종료 |
| `DEALER_ROUTER` | 단일 process 안 sender thread + recv/progress flow | raw socket `CONNECTION_READY`, routing self-check는 단발성 1회만 허용 | ready gate와 self-check 통과 직후 `phase=active` | sender가 wire stop token 송신, recv/progress flow는 idle drain 후 종료 |
| `ROUTER_ROUTER` | 단일 process 안 sender thread + recv/progress flow | raw socket `CONNECTION_READY`, routing self-check는 단발성 1회만 허용 | ready gate와 self-check 통과 직후 `phase=active` | sender가 wire stop token 송신, recv/progress flow는 idle drain 후 종료 |
| `DEALER_ROUTER_REQREP` | 단일 process 안 requester thread + replier thread. requester submit/progress는 같은 thread에서 교대하거나 별도 OS thread로 분리 가능 | raw socket `CONNECTION_READY`, routing self-check는 단발성 1회만 허용 | ready gate와 self-check 통과 직후 `phase=active` | requester는 새 request 제출을 멈추고 bounded completion drain 후 종료 |
| `ROUTER_ROUTER_REQREP` | 단일 process 안 requester thread + replier thread. requester submit/progress는 같은 thread에서 교대하거나 별도 OS thread로 분리 가능 | raw socket `CONNECTION_READY`, routing self-check는 단발성 1회만 허용 | ready gate와 self-check 통과 직후 `phase=active` | requester는 새 request 제출을 멈추고 bounded completion drain 후 종료 |
| `PUBSUB` | 단일 process 안 publisher thread + subscriber drain | raw socket `CONNECTION_READY` 후 bounded post-ready settle | post-ready settle 완료 직후 `phase=active` | publisher가 wire stop token 송신, subscriber는 idle drain 후 종료 |

- single suite에는 runner stdin/stdout `READY` / `CLIENT_READY` / `START`
  orchestration을 만들지 않는다. single handshake는 같은 process 내부의 ready
  gate와 wire stop token으로 닫힌다.
- `PUBSUB`의 post-ready settle은 C 기준에 있는 bounded 절차다. 이를 다른 패턴으로
  확장하거나 sleep 기반 별도 ready gate로 재해석하면 안 된다.
- `setup_connected_pair()`는 내부적으로 low-cost monitoring ready gate를
  캡슐화한 helper인 경우에만 허용된다. 별도/독자적인 start gate 규칙으로
  취급하지 않는다.
- C binding처럼 `PAIR`, `DEALER_DEALER`, `PUBSUB`의 active recv/send 골격이
  거의 같은 경우에는 `single/common`으로 skeleton을 올릴 수 있다. 이때도
  패턴 파일에는 각 패턴이 실제로 사용하는 zlink API 호출과 ready 규칙이
  그대로 보여야 한다.
- pattern별 low-cost ready event는 single 측정의 공식 start gate다. benchmark
  시작 전 준비 판정은 monitoring event로 해결하고, perf 파일 안의 커스텀
  handshake loop, sleep, monitor snapshot polling으로 대체하지 않는다.
- 예외: `PUBSUB`은 ready gate 통과 후 bounded post-ready settle을 수행한다.
- 패턴 파일에서는 공통 helper를 통해 `wait_*ready*()` 형태로 감싸도 된다.
  이 경우에도 ready source는 반드시 위 표의 pattern contract 와 일치해야 한다.
- active에서만 throughput/latency를 계산한다.
- `single`은 별도 settle/prime phase를 두지 않는다.
- 단, 아래 절차는 본 문서에 정의된 의미로 반드시 수행한다.
  - post-ready settle: `PUBSUB`에서 수행한다.
  - idle drain: recv one-way 패턴 공통으로 수행한다.
- latency sample은 내부적으로 nanosecond 단위로 누적하고, RESULT line과
  사람이 읽는 report/table에는 millisecond 단위로 표시한다.
- 다음 size는 별도 프로세스로 다시 시작한다.
- monitor-ready 이후 필요한 protocol self-check는 단발성 검증 1회만
  허용하며, `PUBSUB` 예외를 제외한 sleep 기반 보정은 금지한다.
- C 기준 코드가 같은 위치에서 `perf_socket_poll(NULL, 0, N)`을 쓰는 idle wait는
  sleep 기반 보정이 아니다. binding perf는 public empty-poll API나 public
  timer/poller 기반 idle helper로 그 의미를 표현할 수 있으며, C 기준에 없는
  progress fallback으로 확장하면 안 된다.

### 2.1 Header 기반 집계 (필수)

active 구간 집계는 payload에 기록된 metric header를 기준으로만 수행한다.

- 공식 single 측정의 non-STREAM application wire shape는
  **2-part `[payload, empty]`** 다. ROUTER routing identity는 application part가
  아니므로 ROUTER 계열에도 세 번째 application part는 없다.
- `--part-count 1`은 direct-send 비교를 위한 명시적 진단 실행이며, 공식 2-part
  baseline과 섞어 비교하지 않는다.

- decode 실패 메시지: 집계 제외
- `magic`, `phase`, `msg_size` 검증 실패 메시지: 집계 제외
- `run_id` 불일치 메시지는 집계 제외한다.
- 수신 프레임의 실제 byte 길이가 기대 payload 크기와 다른 메시지는 집계에서
  제외한다. 이 불일치는 실패가 아니라 집계 제외 사유이며 러너를 중단시키지 않는다.
- 유효 header 메시지만 throughput 카운트와 latency 샘플에 포함
- single one-way active 유효 메시지 규칙은 "`phase == active` 이고, **수신 시각이
  active deadline 이전인** 유효 header 메시지"로 고정한다. 판정에 쓰는 시각은
  recv 루프가 그 메시지를 처리한 monotonic 시각이며 payload의 `sent_ts_ns`가
  아니다. active deadline 이후에 recv한 메시지는 stop token 도착 여부와 무관하게
  집계에서 제외하고 종료 정리로만 소비한다. 이 규칙은 모든 one-way 패턴과 모든
  binding에 같은 의미로 적용한다.
- single request-reply active 유효 완료 규칙은 "`phase == active` 인 request가
  active window 안에서 제출되고, **reply completion도 active deadline 전에 끝난**
  왕복"으로 고정한다. 판정에 쓰는 시각은 completion을 처리한 monotonic 시각이며,
  같은 시각으로 왕복 latency를 산출한다. deadline 뒤 bounded completion drain은
  outstanding 정리만 수행하며 throughput과 latency를 늘리지 않는다.

즉, throughput과 latency는 동일한 유효 메시지(또는 유효 완료) 집합을 사용한다.

---

## 3. 유효성 판정 (single 전용)

> 상태 분류(success / unsupported / skip / fail), retry 금지, UNSUPPORTED 오용 금지
> 등 공통 실패 처리 정책은 [PERF_POLICY.md § 7](PERF_POLICY.md) 참조.

### 3.1 완료 판정

```text
expected_result_lines = (요청된 전체 조합 수 - unsupported 수 - skip 수) * 5
actual_result_lines   = 성공적으로 출력된 RESULT 라인 수
status                = (expected_result_lines == actual_result_lines) ? "complete" : "partial"
```

| status | 조건 |
|--------|------|
| complete | `expected_result_lines == actual_result_lines` |
| partial | `expected_result_lines != actual_result_lines` |

- C single perf에서 전체 기본 full matrix가 `complete`로 끝난 경우에는 같은
  결과 파일을 `bindings/c/perf/baseline/`에도 저장하여 다음 회귀 비교 기준으로
  사용한다. partial, smoke, 특정 패턴/transport/size만 실행한 결과는 baseline
  갱신 대상이 아니다. 단, 사용자가 transport/size를 명시했더라도 그 값이 suite
  기본 full matrix와 정확히 같으면 full matrix로 본다.
- partial이어도 결과 파일은 저장한다.

### 3.2 UNSUPPORTED 판정 (single 엔진 특성)

- single 실행 엔진은 stdout `UNSUPPORTED` 토큰만 인식한다.
- stderr `protocol not supported` 기반 자동 분류는 지원하지 않는다
  (multi 엔진에서만 지원).

---

## 4. 결과 저장 (single 전용)

> 파일명 형식(`perf_<lang>_<suite>_<platform>_YYYYMMDD_HHMMSS[_<tag>].txt`),
> 저장 경로(`<suite>/report/`), 보존 정책(최대 100파일) 등 공통 규칙은
> [PERF_POLICY.md § 2.1–2.3, § 4.3](PERF_POLICY.md) 참조.

결과 파일에는 아래가 순서대로 기록된다.

1. `## Effective Options (start)` — 불릿 목록 형식 (lang, suite, runs, patterns, transports, msg_sizes, pin_cpu)
2. 패턴/트랜스포트별 실행 로그 및 테이블
3. `## Auto-HWM Detail` — 모든 single 패턴에서 benchmark process 가 노출한
   실제 socket HWM snapshot table. auto-HWM 정보가 없는 socket 은 생략할 수
   있다.
4. `## Effective Options (result)` — 불릿 목록 형식
5. `## Result Data` — 성공한 조합이 있을 때만 기록한다. 성공한 조합의
   `RESULT,current,...` 라인만 넣고, `UNSUPPORTED`, `SKIP`, `FAIL` 토큰은
   이 섹션에 넣지 않는다.
6. Completion (`status`, `expected_result_lines`, `actual_result_lines`)
7. `Saved result file: ... (status=...)`

실패가 있으면 C single 기준과 같이 `## Failures` 섹션을 `## Auto-HWM Detail`
앞에 기록한다. 실패 섹션은 아래 형식을 사용한다.

```text
## Failures
- PAIR current tcp 64B: timeout
```

- `Effective Options`에는 `lang`과 `suite` 항목이 반드시 포함되어야 한다.
- single 엔진은 최대 파일 수를 100으로 하드코딩한다 (`PERF_RESULTS_MAX_FILES` 미참조).
- single runner는 payload size를 context
  `ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES`로 설정한다.

---

## 5. 실행 방법

> 정책 준수 실행기 목록과 통합 실행 옵션은
> [PERF_POLICY.md § 3](PERF_POLICY.md) 참조.
>
> 수정 후 검증은 single smoke를 포함해야 하며, smoke 정의와 실행 규칙은
> [PERF_POLICY.md § 3.2](PERF_POLICY.md)를 따른다.

### 5.1 CLI 옵션

| 옵션 | 설명 | 기본값 |
|------|------|--------|
| `--pattern NAME` | 패턴 목록 (쉼표 구분) | `ALL` |
| `--runs N` | 조합별 반복 횟수 | 1 |
| `--duration N` | active 구간 시간(초) | 5 |
| `--build-dir PATH` | 빌드 디렉터리 | 자동 탐색 |
| `--results-dir PATH` | 결과 루트 디렉터리 | `bindings/c/perf/results` |
| `--results-tag NAME` | 결과 파일명 태그 | 없음 |
| `--output PATH` | 콘솔 출력 tee 파일 | 없음 |
| `--pin-cpu` | CPU pinning | off |
| `--io-threads N` | context I/O threads | 환경/기본값 |
| `--msg-sizes LIST` | 메시지 크기 목록 | 정책 기본값 |
| `--transports LIST` | transport 목록 | 패턴 기본값 |
| `--hwm N` | debug 전용 송수신 HWM 공통 fallback. `PERF_SINGLE_ALLOW_MANUAL_SOCKET_OVERRIDES=1` 필요 | auto-HWM |
| `--send-hwm N` | debug 전용 송신 HWM 우선값. `PERF_SINGLE_ALLOW_MANUAL_SOCKET_OVERRIDES=1` 필요 | `--hwm` |
| `--recv-hwm N` | debug 전용 수신 HWM 우선값. `PERF_SINGLE_ALLOW_MANUAL_SOCKET_OVERRIDES=1` 필요 | `--hwm` |
| `--buf SIZE` | debug 전용 송수신 OS buffer 공통 override. `PERF_SINGLE_ALLOW_MANUAL_SOCKET_OVERRIDES=1` 필요 | `-1` |
| `--sndbuf SIZE` | debug 전용 송신 OS buffer override. `PERF_SINGLE_ALLOW_MANUAL_SOCKET_OVERRIDES=1` 필요 | `--buf` |
| `--rcvbuf SIZE` | debug 전용 수신 OS buffer override. `PERF_SINGLE_ALLOW_MANUAL_SOCKET_OVERRIDES=1` 필요 | `--buf` |
| `--auto-hwm-profile NAME` | context auto-HWM profile (`compact`, `low_latency`, `balanced`, `throughput`) | `balanced` |

### 5.2 바이너리 직접 실행

```bash
<binary> <lib_name> <transport> <size>
# 예시
./core/build/linux-x64/bin/perf_pair current tcp 1024
```

---

## 6. Pattern & Transport Matrix

### 6.1 지원 패턴

공식 `--pattern ALL`은 아래 7개 패턴을 정확히 선택한다.

- PAIR
- PUBSUB
- DEALER_DEALER
- DEALER_ROUTER
- ROUTER_ROUTER
- DEALER_ROUTER_REQREP
- ROUTER_ROUTER_REQREP

> STREAM 계열(STREAM)은 single suite에서 테스트하지 않는다.

#### recv mode 지원 범위

| 패턴 | 허용 mode |
|------|-----------|
| PAIR | `recv` |
| PUBSUB | `recv` |
| DEALER_DEALER | `recv` |
| DEALER_ROUTER | `recv` |
| ROUTER_ROUTER | `recv` |
| DEALER_ROUTER_REQREP | `request-reply` |
| ROUTER_ROUTER_REQREP | `request-reply` |

정책:

- single 전 패턴의 테스트 mode는 recv이다.
- 기존 `DEALER_ROUTER`, `ROUTER_ROUTER`의 mode를 바꾸거나 옵션으로 request-reply를
  섞으면 안 된다. request-reply는 별도 `*_REQREP` 패턴으로만 측정한다.

#### ready gate 기준

single의 send/recv 시작 가능 여부는 공식 raw socket monitor event로 판정한다.
perf는 추가 precondition(`FILTER_APPLIED`, quorum 완화)을 두지 않는다. 아래 contract
이후 메시징이 불가능하면 perf 우회가 아니라 core 버그로 보고 수정한다.

| 패턴 | 송신 시작 기준 | 수신 시작 기준 |
|------|----------------|----------------|
| PAIR | `CONNECTION_READY` | `CONNECTION_READY` |
| PUBSUB | `CONNECTION_READY` | `CONNECTION_READY` |
| DEALER_DEALER | `CONNECTION_READY` | `CONNECTION_READY` |
| DEALER_ROUTER | `CONNECTION_READY` | `CONNECTION_READY` |
| ROUTER_ROUTER | `CONNECTION_READY` | `CONNECTION_READY` |
| DEALER_ROUTER_REQREP | `CONNECTION_READY` | `CONNECTION_READY` |
| ROUTER_ROUTER_REQREP | `CONNECTION_READY` | `CONNECTION_READY` |

- single policy 는 `event.value` 와 `snapshot.ready_count` gate 를 금지한다.
- single policy 는 delivery-ready event gate 도 사용하지 않는다.
- `PUBSUB` 은 `CONNECTION_READY` 뒤에 subscription 전파 안정화를 위한 bounded
  post-ready settle 1회를 반드시 수행한다.

#### 패턴 방향 분류

| 방향 | 패턴 | throughput 단위 |
|------|------|----------------|
| one-way (단방향) | PAIR, PUBSUB, DEALER_DEALER, DEALER_ROUTER, ROUTER_ROUTER | `msg/s` |

> **구현 참고**: single runner는 **Kmsg/s** 단위로 출력하고, bandwidth는
> `throughput × size / 1,000,000`으로 계산한다. single에는 왕복(`ops/s`) 패턴이 없다.

### 6.2 표준 메시지 크기

`[64, 256, 1024, 65536, 131072, 262144]`

### 6.3 transport

| 패턴군 | transport |
|--------|-----------|
| PAIR / PUBSUB / DEALER / ROUTER | tcp, tls, ws, wss, inproc, ipc (Windows: ipc 제외) |

---

## 7. Environment Variables (single 전용)

> 공통 환경 변수(`PERF_DEBUG`, `PERF_IO_THREADS`, `PERF_MSG_SIZES`,
> `PERF_TRANSPORTS`, `PERF_TASKSET`, `PERF_FAIL_FAST`,
> `PERF_DISABLE_RESOURCE_METRICS`, `PERF_MAX_SOCKETS`)는
> [PERF_POLICY.md § 8](PERF_POLICY.md) 참조.

single perf의 기본 `PERF_IO_THREADS`는 모든 언어와 모든 패턴에서 `1`이다.
`--io-threads` 또는 `PERF_IO_THREADS`를 명시한 실행은 기준 비교가 아니라 의도적인
진단 실행으로 기록해야 한다.

### 7.1 phase/timeout

| 변수 | 설명 | 기본값 |
|------|------|--------|
| `PERF_SINGLE_DURATION_SECONDS` | active 구간 시간(초) | 5 |
| `PERF_SINGLE_TIMEOUT_SECONDS` | 프로세스 timeout(초) | `max(30, duration*6+15)` |
| `PERF_TRANSPORT_TRANSITION_MS` | transport 전환 후 다음 케이스를 시작하기 전 runner 레벨 대기(ms). 이전 transport의 소켓 정리가 다음 측정에 섞이지 않게 하기 위한 대기이며, benchmark phase가 아니다 | 3000 |

### 7.2 hwm/timeout

| 변수 | 설명 | 기본값 |
|------|------|--------|
| `PERF_SINGLE_HWM` | debug 전용 소켓 HWM 공통 fallback. `PERF_SINGLE_ALLOW_MANUAL_SOCKET_OVERRIDES=1` 일 때만 사용 | 비활성 |
| `PERF_SINGLE_SNDHWM` | debug 전용 송신 HWM 우선값 | `PERF_SINGLE_HWM` |
| `PERF_SINGLE_RCVHWM` | debug 전용 수신 HWM 우선값 | `PERF_SINGLE_HWM` |
| `PERF_SINGLE_SNDBUF` | debug 전용 `SNDBUF` override | 비활성 |
| `PERF_SINGLE_RCVBUF` | debug 전용 `RCVBUF` override | 비활성 |
| `PERF_SINGLE_ALLOW_MANUAL_SOCKET_OVERRIDES` | single runner 의 수동 HWM/SNDBUF/RCVBUF override 허용 플래그 | 0 |
| `PERF_SINGLE_SNDTIMEO_MS` | 송신 타임아웃(ms) | 200 |
| `PERF_SINGLE_RCVTIMEO_MS` | 수신 타임아웃(ms) | 200 |
| `PERF_SINGLE_PUBSUB_RCVTIMEO_MS` | PUBSUB 수신 타임아웃(ms) | `PERF_SINGLE_RCVTIMEO_MS` |
| `PERF_SINGLE_PUBSUB_READY_SETTLE_MS` | PUBSUB post-ready settle(ms) | 1000 |
| `PERF_SINGLE_LATENCY_SAMPLE_CAP` | percentile 계산에 보관할 최대 sample 수. `0`이면 sample을 보관하지 않는다 | 1,000,000 |
| `PERF_SINGLE_PUBSUB_XPUB_NODROP` | PUBSUB의 `ZLINK_XPUB_NODROP` 기본값 | (바이너리별) |

- percentile sample을 하나도 보관하지 않은 경우(`PERF_SINGLE_LATENCY_SAMPLE_CAP=0`
  또는 유효 sample 0개)의 `latency_p95`·`latency_p99` 보고 값과 percentile 보간식은
  [PERF_POLICY.md § 1.1](PERF_POLICY.md)의 공통 규칙을 따른다.
- backpressure 검증은 `core/tests/integration`로 분리한다. one-way 통합 범위는
  `DEALER_DEALER`, `DEALER_ROUTER`, `ROUTER_ROUTER`, `PUBSUB` 이며, `STREAM`, echo,
  `PAIR` 은 제외한다.

---

## 8. 변경 이력

- **v2.3 (2026-09-07)**
  - request/reply 모델을 single suite에서 제외했던 개정(D-BP3)을 **철회**하고
    `DEALER_ROUTER_REQREP`, `ROUTER_ROUTER_REQREP`를 공식 패턴으로 되돌렸다 (D-BP6).
    §1.1을 실행 모델 중심으로 다시 써서 동기 실행 모델이 in-flight 1을 뜻하지
    않는다는 점과 admission·reply가 별개 사건이라는 점을 명시했다.
  - one-way active 유효 메시지 규칙의 기준 시각을 "수신 monotonic 시각 < active
    deadline"으로 확정하고, wire 프레임 길이 불일치를 집계 제외 사유로 명문화했다.
  - one-way 송신의 transient 재시도를 "1 ms 대기 + `sent_ts_ns` 재stamp"로 고정했다.
  - 개정 조항은 개정 이후의 측정에 적용하며, 이전에 완결된 paired 판정을 소급
    무효화하지 않는다 (D-BP4).
- **v2.2 (2026-08-28)**
  - Spot 성능 시험을 Framework suite 소유로 이전하고 binding single 공식 목록을 7개로 고정했다.
- **v2.1 (2026-07-18)**
  - latency sample 기본 상한을 명시했다.
- **v2.0 (2026-04-07)**
  - 공통 정책을 [PERF_POLICY.md](PERF_POLICY.md)로 통합, 중복 제거
  - single 전용 내용만 유지
- **v1.9 (2026-03-21)**
  - 공통 원칙 및 바인딩 parity 기준 정렬
- **v1.6 (2026-03-03)**
  - baseline/mode/trend/gate 정책 제거
  - 결과 저장 구조를 `report/` 단일 경로로 정리
  - active 동시 측정 모델(throughput + latency) 명시
  - header decode/검증 성공 메시지만 집계하는 규칙 명문화
  - 드레인/재시도 미사용 정책 명시
