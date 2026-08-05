# zlink Performance Test Policy (통합)

> **적용 범위**: zlink 전체 (core + bindings)
> **Policy Version**: 2.0
> **Date**: 2026-07-18
> **Scope**: zlink 성능 테스트 통합 정책 — 공통 구조, 통합 실행, 비교 스크립트
>
> 본 정책은 `bindings/c/perf`의 C benchmark runner와 in-repo perf 자산이 존재하는 바인딩
> (`bindings/cpp`, `bindings/dotnet`, `bindings/java`, `bindings/rust`,
> `bindings/go`, `bindings/node`, `bindings/python`)에 동일한 기준으로 적용한다.
> 단, 각 언어의 구현 완성도와 지원 패턴 범위는 다를 수 있으므로 실제 parity
> 수준은 언어별로 점검/정렬 대상이 된다.
> **언어별 적용 범위**:
> - **기본 적용**: `core`(C), `bindings/cpp`, `bindings/dotnet`, `bindings/java`, `bindings/rust`, `bindings/go`, `bindings/node`, `bindings/python`
> - perf의 기본 수신 측정 경로는 poller + recv drain 이다.
> - direct message callback 경로의 동기화 비용(TSFN, GIL, mutex 등)은 언어별
>   런타임 메커니즘이 달라 일관된 비교 기준이 불가능하므로 기본 perf 비교 surface에서
>   제외한다. callback 정합성 검증은 `core/tests/integration`에서 수행한다.
> - 예외:
>   `SPOT_PUBSUB`은 MeshNode ready/claim batch 수신 경로를 사용하고,
>   `STREAM`은 raw callback이 아니라 packet handler surface를 기준으로 테스트한다.

---

## 1. 문서 구조

| 문서 | 설명 |
|------|------|
| **PERF_POLICY.md** (본 문서) | 공통 원칙, 디렉터리 구조, RESULT 형식, 결과 저장, 출력 형식, 실패 처리, 환경 변수(공통), 리팩토링 원칙 |
| [PERF_SINGLE_TEST_POLICY.md](PERF_SINGLE_TEST_POLICY.md) | single suite 전용: recv/request-reply 모델, phase, 패턴/transport, single 전용 환경 변수 |
| [PERF_MULTI_TEST_POLICY.md](PERF_MULTI_TEST_POLICY.md) | multi suite 전용: 프로세스 모델, backpressure, throughput/latency 측정, 패턴/transport, multi 전용 환경 변수 |
| [PERF_SPOT_TEST_POLICY.md](PERF_SPOT_TEST_POLICY.md) | Core 10.0.0 Spot 패턴 이름, MeshNode peer 토폴로지, ready/claim 수신과 단계별 실행 |

- 양 suite에 공통으로 적용되는 모든 규칙은 본 문서에서 관리한다.
- 개별 정책 문서는 해당 suite **전용** 규칙만 기술하며, 공통 규칙은 본 문서를 참조한다.
- 각 바인딩의 실행 스크립트 경로는 `perf/single/` 및 `perf/multi/`에 위치한다.

## 1.1 공통 원칙

아래 원칙은 `bindings/c/perf`를 기준으로 모든 bindings perf에 동일하게 적용한다.
`bindings/c/perf`는 바인딩 언어별 perf를 맞출 때의 기준 구현이다. 같은 항목을
측정하는 바인딩 perf는 C perf의 handshake, phase, metric anchor, RESULT 의미를
그대로 따라야 한다. 문서와 구현이 어긋나면 언어별 구현에서 임의로 해석하지 말고
먼저 `bindings/c/perf` 기준을 확인한 뒤 이 정책 문서를 C 기준에 맞게 고친다.
handshake 의미를 바꿔야 할 때는 C perf를 먼저 수정하고, 그 변경을 본 정책과
suite별 정책 문서에 반영한 다음 다른 바인딩으로 옮긴다.

- `bindings/c/perf`는 `doc/guide` 및 `doc/spec/core` 문서에 기술된 public C API만 사용한다.
  내부 헤더나 내부 함수를 직접 호출하지 않는다.
- `bindings/<lang>/perf`는 해당 언어 binding의 public API만 사용한다. binding
  내부/private API, 내부 구현 클래스, native 내부 helper를 직접 호출하지 않는다.
  C를 제외한 바인딩 perf는 `zlink_*` C API나 C FFI 함수를 hot path에서 직접
  호출해 수치를 만들면 안 된다. C API는 해당 바인딩 라이브러리 내부 구현에서만
  사용할 수 있으며, perf는 그 바인딩이 사용자에게 공개한 API를 통해서만
  send/recv/publish/subscribe를 수행한다.
  성능 수치 달성만을 위해 새 public API, native API, raw handle API, zero-copy
  전용 API를 추가해 perf가 그 경로를 타게 하는 것도 우회로 본다. 공개 API
  누락이 별도 버그로 확인되지 않는 한, 성능 개선은 기존 바인딩 공개 API의
  내부 구현을 고치는 방식으로 수행한다.
- single/multi 기본 경로는 모두 context auto-HWM 을 사용한다. `PERF_*_HWM`,
  `PERF_*_SNDHWM`, `PERF_*_RCVHWM`, `PERF_*_SNDBUF`, `PERF_*_RCVBUF`
  override 는 debug 예외 경로이며 allow flag 가 켜진 경우에만 허용한다.
- OOM을 피하려고 공식 성능 시험의 전송률, 동시 요청 수, payload 크기, client 수,
  active duration을 임의로 낮추거나 송신 간격을 추가하지 않는다. 메시지 burst는
  현재 payload 크기를 반영한 context auto-HWM과 공개 API의 backpressure가 제한해야
  한다. `EAGAIN`은 이 흐름 제어가 작동했다는 뜻이므로 송신자는 준비 상태가 회복되면
  즉시 계속한다.
- 예상 메모리가 현재 장비의 가용 메모리를 넘으면 runner는 workload를 줄여서 다른
  시험으로 바꾸지 않는다. 해당 조건을 실행할 수 없는 이유를 `skip` 또는 `fail`로
  남기고 충분한 메모리가 있는 장비에서 같은 조건을 다시 실행한다.
- auto-HWM을 적용했는데도 queue 메모리가 제한 없이 증가하거나, 종료한 phase의
  메시지가 회수되지 않거나, Core가 backpressure를 반환하지 않아 OOM이 발생하면
  성능 시험 조건을 낮추지 않는다. 재현 test를 추가하고 Core 버그로 수정한 뒤 같은
  조건을 다시 실행한다.
- p95/p99 계산용 sample 저장 상한은 메시지 queue나 요청 동시성을 제한하지 않는
  계측 메모리 보호다. 전체 완료 수와 지연 시간 합계는 계속 기록하며, 이 상한을
  전송률 제한이나 송신 간격으로 사용하면 안 된다.
- public API 동작에 문제가 있으면 perf 코드에서 우회하지 않고 버그로
  레포팅한다. 버그레포팅 문서는 doc/bug/perf 아래에 md 파일 형식으로 작성한다.
  버그는 회귀테스트를 작성해서 재현을 확인하고 수정한다. 버그를 우선 수정하고
  이어서 perf 작업을 계속한다.
- 측정 의미는 유지한다.
  - `ready / active`
  - `RESULT` 포맷
  - 실패 의미
- `bindings/c/perf`와 bindings perf는 같은 이름의 metric을 서로 다른 anchor point에서
  계산하면 안 된다. 같은 비교 surface라면 아래 측정 anchor를 동일 의미로
  유지해야 한다.
  - send timestamp 기록 위치
  - ready 만족 판정 위치
  - active 시작/종료 판정 위치
  - 유효 recv 판정 위치
  - throughput count 증가 위치
  - latency sample 채취 위치
  - RESULT line 확정 위치
- bindings perf는 측정 anchor와 결과 의미를 C perf 기준과 동일하게 유지해야 하지만,
  구현 스타일은 언어별 특성에 맞게 작성할 수 있다.
  - 허용: 언어별 async/runtime, 모듈 분리, 타입 시스템 차이
  - 금지: 측정 anchor point 이동, phase 의미 변경, metric 집합 변경, fail/skip/
    unsupported 의미 변경
- `bindings/c/perf`와 `bindings/<lang>/perf`의 공식 실행 스크립트는 동일한 CLI 옵션
  의미와 동일한 결과 출력 포맷을 따라야 한다.
  - 옵션 이름과 의미를 언어별로 바꾸지 않는다.
  - RESULT line 형식과 필수 5개 metric (throughput, bandwidth, latency, latency_p95, latency_p99) 의미를 바꾸지 않는다.
  - 사람이 읽는 테이블 형식과 complete/partial/fail 의미를 바꾸지 않는다.
- perf runner와 benchmark process 사이의 handshake 방식은
  `bindings/c/perf` 구현을 기준 계약으로 고정한다. 모든 binding perf는 C perf와
  같은 stdout/stdin token, 같은 전송 방향, 같은 start/stop 의미, 같은 timeout
  실패 의미를 사용해야 한다. 언어별 runner가 편의를 위해 별도 token을 추가하거나
  C runner가 요구하지 않는 보정 phase를 측정 조건으로 만들면 안 된다.
- bindings perf는 아래 비교 가능성 체크리스트를 함께 만족해야 한다.
  - 같은 pattern/transport 의미를 측정한다.
  - 같은 metric header / wire protocol contract를 사용한다.
  - 같은 필수 5개 metric (throughput, bandwidth, latency, latency_p95, latency_p99)과 같은 RESULT line 의미를 사용한다.
  - 같은 fail/skip/unsupported/complete/partial 의미를 사용한다.
  - hot path가 실제 binding public API를 통과한다.
- bindings perf는 “binding 라이브러리 성능”을 측정해야 한다. 따라서 각 언어
  perf 바이너리는 해당 언어 binding의 public API로 data path를 직접 실행해야
  한다. C 기준 perf 바이너리나 다른 언어의 perf 바이너리를 wrapper로 호출해서
  결과만 중계하는 방식은 정책 위반이며, 그 결과는 비교 대상으로 인정하지
  않는다.
  - 예외: `MULTI_STREAM` client는 zlink binding client API를 측정하는 대상이
    아니라, STREAM server에 붙는 외부 raw transport peer를 재현하는 검증
    인프라다. 따라서 모든 binding perf runner는
    `bindings/c/perf/common/streamclient/perf_stream_client.cpp`에서 만든
    `perf_stream_client` 공용 바이너리를 그대로 사용할 수 있다. 이 예외는
    `MULTI_STREAM` client에만 적용하며, `MULTI_STREAM` server는 각 binding의
    public STREAM server/packet handler surface를 통해 측정해야 한다.
- managed runtime 바인딩(Java, .NET 등)은 size마다 프로세스를 재시작하므로,
  런타임 옵션으로 시작 비용을 최소화해야 한다.
  - Java: `-server`, `-XX:TieredStopAtLevel=4`(C2 fully tiered) 등. 5초 이상 측정 윈도우에서는 C1 한정(`=1`)이 hot path JIT 최적화를 막아 routed 패턴 64B 같은 wrapper-bound 조합에서 큰 ratio 손실을 만들었기 때문에 fully tiered를 권장한다. 짧은 startup만 필요한 임베디드/배포용 옵션은 별도이다.
  - .NET: `DOTNET_TieredCompilation=1` (default; tiered + tier-1 promotion 활성화),
    `DOTNET_TC_QuickJitForLoops=1`, `DOTNET_ReadyToRun=1` 등.
    Java 와 같은 이유로 5초 이상 측정 윈도우에서는 tiered compilation 비활성(`=0`)이 hot path JIT
    최적화를 막아 routed/wrapper-bound 패턴 multi 64B 같은 조합에서 큰 ratio 손실을
    만들었기 때문에 tier-1 promotion 활성화를 권장한다. 짧은 startup만 필요한
    임베디드/배포용 옵션은 별도이다.
- backpressure 검증은 기본 perf surface가 아니라 `core/tests/integration`
  으로 분리한다. one-way backpressure 통합 범위는 `DEALER_DEALER`,
  `DEALER_ROUTER`, `ROUTER_ROUTER`, `PUBSUB`, `SPOT_PUBSUB` 이며,
  `STREAM`, echo, `PAIR` 은 제외한다.
- 실제 오류는 즉시 `fail` 처리한다.
- `EAGAIN`은 오류가 아니라 flow-control 상태로 취급한다.
- perf 측정용 I/O 경로는 기본적으로 **recv 모델**과 request-reply completion
  모델만 사용한다.
  - recv: poller `POLLIN` readiness 감지 → 비동기 `zlink_recv()` /
    `zlink_msg_recv()` drain 루프 (react 방식).
  - send (single): blocking send. HWM 도달 시 자연 backpressure.
    단일 프로세스에서 sender/receiver를 구동하므로 nonblocking 제어 불필요.
  - send (multi): nonblocking send + poller `POLLOUT` readiness 감지.
    서버가 N개 클라이언트를 한 poller에서 처리하므로 EAGAIN 시 pending
    deque/flag에 저장하고 POLLOUT에서 재개.
  - poller는 recv readiness와 request-reply completion 감지에 공통 사용하고,
    send backpressure는 suite별로 위 방식을 따른다.
- direct message callback 경로는 perf에서 측정하지 않는다. callback의 동기화
  메커니즘(TSFN, GIL, mutex 등)은 언어별 런타임에 의존하므로 일관된 비교
  기준이 불가능하다.
- request/reply completion 도 recv 모델의 일부로 본다. requester 는 request
  submit 뒤에도 같은 requester socket의 reply 수신/progress 경로를 계속 돌려야
  한다. completion은 requester socket을 수신하지 않아도 되는 별도 channel이
  아니다.
  - single reqrep은 같은 process 안에서 requester submit thread, requester
    progress thread, replier thread를 분리한다. requester progress thread는
    blocking recv/wait 또는 그와 같은 의미의 public completion progress 경로로
    reply completion을 drain한다.
  - multi reqrep은 client process의 active poller loop가 N개 requester socket을
    multiplex한다. socket request/reply surface와 spot request/reply surface는
    이 public poller loop에 `POLLCOMPLETION`을 등록한다.
    `ZLINK_POLLCOMPLETION`은 completion drain 요청이므로 `POLLIN`/`POLLOUT`과
    섞지 않고 completion 대상에 단독으로 등록한다. `MULTI_SPOT_REQREP`
    requester reply completion도 예외 없이 `POLLCOMPLETION` poller가 소유한다.
  suite 정책에 없는 timer, pipe/eventfd wake, `setInterval`, 짧은 sleep,
  busy polling을 hot path에 두면 C perf와 같은 측정 의미가 아니므로 금지한다.
  Node처럼 native completion callback을 JavaScript turn에서 전달하는 binding은
  `POLLCOMPLETION` poller wait가 completion queue를 drain한 뒤 callback 전달을
  위해 event-loop turn을 한 번 양보할 수 있다. 이 양보는 completion progress
  소유권을 가져가는 timer나 별도 pump가 아니어야 하며, poller wait를 대체하면
  안 된다.
- binding public async/request API가 일반 사용자 편의를 위해 내부 progress
  pump를 제공하더라도, perf에서 `POLLCOMPLETION`을 등록한 외부 poller가
  completion을 소유하는 동안에는 그 내부 pump가 같은 completion queue를
  경쟁해서 drain하면 안 된다. 이 경우 binding 내부 구현은 외부 poller 등록을
  감지해 자동 pump를 비활성화하고, completion drain은 perf의 단일 active
  poll loop에 맡겨야 한다.
- 다만 아래 두 예외는 perf 정책 범위에 포함한다.
  - Spot 계열은 `zlink_mesh_node_drain_ready()`와 claim batch로 application 및
    infrastructure record를 읽는다. direct message callback은 사용하지 않는다.
  - `STREAM`은 raw callback을 제외하지만 packet handler를 정식 수신 경로로 본다.
- `MULTI_SPOT_REQREP` completion도 Spot의 infrastructure claim batch에서 읽는다.
  자세한 record 종류와 응답 API는
  [Core 10.0.0 Spot 성능 시험 정책](./PERF_SPOT_TEST_POLICY.md)을 따른다.

### 1.1.1 Metric Header Wire Format

모든 언어에서 동일한 metric header를 사용해야 언어 간 결과 비교가 가능하다.
metric header는 benchmark payload의 선두에 고정 크기로 인코딩된다.

```text
offset  size  type       field         설명
─────────────────────────────────────────────────────────
 0       4    uint32_le  magic         고정값 0x5A4C4E4B ("ZLNK")
 4       4    uint32_le  run_id        실행 식별자
 8       1    uint8      phase         0=warmup, 1=active, 2=cooldown
 9       4    uint32_le  msg_size      메시지 크기 (bytes)
13       8    uint64_le  seq           메시지 시퀀스 번호
21       8    int64_le   sent_ts_ns    송신 시점 (nanoseconds, epoch)
─────────────────────────────────────────────────────────
total: 29 bytes (고정)
```

- 모든 필드는 **little-endian**이다.
- payload가 29바이트 미만이면 metric header를 포함할 수 없으므로 perf 최소
  메시지 크기는 29바이트 이상이어야 한다. 실제 정책 기본값은 64바이트부터 시작.
- header 이후 나머지 바이트는 패딩(0 또는 임의값)이며 측정에 사용하지 않는다.
- 수신 측은 `magic` 필드로 유효 perf 메시지를 판별하고, `phase == 1`(active)인
  메시지만 throughput/latency 집계에 포함한다.

**phase 값과 lifecycle 매핑**:

| phase 값 | 이름 | lifecycle 단계 | 설명 |
|----------|------|---------------|------|
| 0 | warmup | ready 구간 | ready gate 통과 전 송신되는 메시지. 수신 측은 집계에서 제외 |
| 1 | active | active 구간 | 측정 대상. throughput/latency 집계에 포함 |
| 2 | cooldown | active 종료 후 | sender가 duration 만료 후 보내는 종료 신호. 수신 측은 집계에서 제외 |

- sender는 ready gate 통과 전까지 `phase=0`으로 송신하고, active 시작 시
  `phase=1`로 전환하며, duration 만료 시 `phase=2`로 전환한다.
- receiver는 `phase=1` 메시지만 active 집계 후보로 취급한다. warmup/cooldown
  메시지는 어떤 suite/pattern에서도 active 결과에 포함되면 안 된다.
- active 결과 집계 조건은 suite/pattern 정책 문서에 명시된 단일 규칙으로 고정한다.
  C 기준과 모든 bindings는 같은 pattern에서 동일한 active 유효 메시지 의미를
  사용해야 한다.
- latency sample은 내부적으로 nanosecond 단위로 누적하고, RESULT line과
  사람이 읽는 report/table에는 millisecond 단위로 표시한다.

**run_id 생성 규칙**:

- `run_id`는 **1-based benchmark case ordinal** 이다. 값 `0`은 사용하지 않는다.
- 같은 benchmark case의 sender/receiver는 반드시 동일한 `run_id`를 사용한다.
- 한 case의 ready/settle/active/drain 동안 송신되는 모든 perf 메시지는 동일한
  `run_id`를 사용한다.
- 프로세스가 case 1개만 수행하는 경우 `run_id`는 반드시 `1`이다.
- 프로세스가 여러 case를 순차 수행하는 경우 `run_id`는 case마다 `1,2,3,...`
  순서로 증가해야 하며, 협력하는 상대 프로세스도 같은 case ordinal을 사용해야
  한다.
- receiver는 `run_id`가 일치하는 메시지만 유효 perf 메시지로 인식한다. 다른
  `run_id`의 메시지(이전 실행 잔여, 다른 case 잔여 등)는 집계에서 제외한다.

### 1.1.2 Hot Path 및 Ready Gate 규칙

- setup/handshake 단계의 bounded validation 1회는 허용하되, 측정 구간으로
  들어가기 전에 종료되어야 한다.
- hot loop 안에서는 아래를 금지한다.
  - retry budget
  - sleep / yield
  - fallback
  - cap
  - heap alloc
  - 문자열 생성
  - 로그 출력
  - 불필요 복사
- 예외: `bindings/c/perf` 기준 코드가 같은 위치에서
  `perf_socket_poll(NULL, 0, N)`을 사용하는 경우에는, binding perf도 같은 의미의
  bounded idle wait를 둘 수 있다. 우선 public empty-poll API를 사용하고, 해당
  binding에 empty-poll public API가 없으면 public timer/poller 기반 idle helper를
  사용한다. 이 예외는 신호 누락을 덮는 progress fallback이나 completion pump가
  아니어야 하며, C 기준에 없는 위치로 확장하면 안 된다.
- send/recv 버퍼는 루프 밖에서 1회 할당하고 재사용한다.
- 핵심 send/recv loop는 각 패턴 파일 안에서 명시적으로 보여야 한다.
- registry summary/topology query는 global/coarse 상태 확인용으로만 사용한다.
- registry summary는 eventually consistent view이므로 benchmark의 final strict
  start gate로 사용하지 않는다.
- perf 연결 준비/handshake는 pattern별로 나눈다.
  - raw socket client 연결 준비: low-cost monitor event `CONNECTION_READY`
  - runner-barrier raw start gate: `CONNECTION_READY` 확인 뒤 suite별 패턴 표의
    `CLIENT_READY` / `START` orchestration
  - single `SPOT_PUBSUB`: MeshNode와 두 Spot을 만든 뒤 local subscription 설정 완료
  - multi Spot 계열: 모든 peer MeshNode가 hub에 admitted되고 hub Spot generation을
    받은 뒤 `CLIENT_READY` / `START`
- raw socket client 연결 준비는 expected client 수만큼
  `CONNECTION_READY` 수신으로 판정한다.
- multi Spot 계열은 `zlink_mesh_node_peers()`에서 각 hub peer가
  `ZLINK_MESH_PEER_ADMITTED`인지 확인하고, setup용 node request/reply가 완료된 뒤
  `CLIENT_READY`를 출력한다. 별도 control MeshNode나 고정 settle 시간을 사용하지 않는다.
- single과 multi Spot 수신은 `zlink_mesh_node_drain_ready()`와
  `zlink_mesh_claim_recv_batch()`로 처리한다.
- `setup_connected_pair()` 같은 helper는 raw socket client 의 `CONNECTION_READY`
  counting 만 캡슐화한 경우에만 허용된다.
- `wait_ready()` 같은 helper는 허용한다. 단:
  - raw socket client 연결 준비에서는 `CONNECTION_READY` counting 만 수행해야 한다.
  - runner-barrier raw start gate 에서는 `CONNECTION_READY` 확인 뒤 suite별 패턴
    표의 `CLIENT_READY` / `START` orchestration 을 수행해야 한다.
  - single SPOT_PUBSUB에서는 MeshNode와 subscription 설정 완료만 확인한다.
  - multi Spot 계열에서는 peer admission과 hub Spot generation setup만 확인한다.
  - delivery-ready event, 별도 서비스 이벤트 스트림, snapshot polling 을 helper 뒤에
    숨기면 안 된다.

### 1.1.3 Runner Handshake Wire Contract

아래 stdout/stdin token은 `bindings/c/perf`가 사용하는 benchmark orchestration
계약이다. 모든 binding perf runner와 benchmark process는 같은 token과 방향을
사용한다.

| token | 방향 | 의미 |
|-------|------|------|
| `READY,<endpoint>` | server stdout → runner | server data endpoint bind 완료 |
| `CLIENT_READY,<msg_size>` | client stdout → runner | client가 해당 size 실행 준비 완료 |
| `START,<msg_size>` | runner stdin → server/client | 해당 size active 실행 시작 |
| `PHASE_ACTIVE,<msg_size>` | runner stdin → client | C runner 호환용 one-way 보조 token. active gate가 아니며 benchmark process가 필수 조건으로 요구하면 안 됨 |
| `CLIENT_DONE,<msg_size>` | client stdout → runner | client가 해당 size RESULT 출력까지 완료 |
| `STOP` | runner stdin → server/client | 실패, timeout, 정리 요청 |
| `UNSUPPORTED,...` / `SKIP,...` | process stdout → runner | 해당 조합 제외 |
| `RESULT,...` | process stdout → runner | 측정 결과 |

공통 규칙:

- `READY`는 endpoint bind 완료만 의미한다. raw data path가 측정 가능한 상태라는
  뜻으로 해석하지 않는다.
- raw multi 패턴 중 C 기준에서 runner barrier를 쓰는 패턴은 process 내부에서
  C 기준 ready gate를 만족한 뒤 `CLIENT_READY`를 출력한다. runner는
  `CLIENT_READY,<msg_size>`를 본 뒤 server와 client stdin에 같은
  `START,<msg_size>`를 보낸다. 이 규칙은 suite별 패턴 표에서
  `CLIENT_READY` / `START`를 명시한 패턴에만 적용한다.
- C 기준에서 runner `START`를 쓰지 않는 echo/STREAM 패턴에 언어별 runner가
  `CLIENT_READY` / `START` barrier를 새로 추가하면 안 된다.
- `START,<msg_size>`는 C 기준에서 runner barrier를 쓰는 패턴의 active 시작
  token이다.
  C runner는 일부 one-way 경로에서 하위 호환을 위해 `PHASE_ACTIVE,<msg_size>`도
  client stdin으로 보낼 수 있다. 이 token은 C 기준에서 active gate가 아니며,
  benchmark process가 `START` 외에 `PHASE_ACTIVE` 수신을 필수 조건으로 요구하면
  안 된다. 다른 바인딩 runner가 C runner보다 강한 phase 조건을 추가하는 것도
  금지한다.
- `STOP`은 runner orchestration 정리 명령이다. data-plane phase 종료 신호가
  필요한 패턴은 suite 정책에 정의된 wire-level stop token을 사용한다.
- multi Spot 계열 client는 N개 peer의 admission과 setup request/reply가 끝난 뒤
  `CLIENT_READY,<msg_size>`를 출력한다. runner가 server와 client에 같은
  `START,<msg_size>`를 전달하면 active 구간을 시작한다.
- C perf handshake에 없는 언어별 ready token, 별도 ack, 추가 quorum, 별도 warmup
  start 명령은 정책 위반이다. 특정 바인딩 public API 부족으로 C handshake를
  구현할 수 없으면 binding public API를 보강하거나 해당 perf 조합을
  `UNSUPPORTED`로 처리한다. reflection, private/internal API, native handle 우회로
  C handshake를 흉내 내면 안 된다.
- 이미 공식 runner의 supported/default matrix에 들어간 pattern/transport 조합에서
  C 기준 handshake와 다른 구현을 발견했을 때는 그 조합을 default 목록에서 빼거나
  `SKIP` / `UNSUPPORTED`로 바꾸어 비교를 통과시켜서는 안 된다. 이는 수정이 아니라
  회귀 은폐다. 해당 조합은 C 기준 handshake에 맞게 구현을 고치고, 고치기 전에는
  `fail`로 드러나야 한다.
- `UNSUPPORTED`는 정책상 지원하지 않는 pattern/transport 조합이나, 아직 공식
  supported/default matrix에 올리지 않은 새 조합을 도입하기 전의 상태를 표현할 때만
  쓴다. C 기준과 불일치하는 기존 구현을 숨기는 용도로 쓰면 안 된다.
- suite별 정책 문서는 pattern별 low-cost ready gate event를 명시해야 한다.
  perf는 그 표에 없는 추가 precondition(`FILTER_APPLIED`, delivery-ready exact count,
  quorum 완화, 보정용 handshake 단계)을 두지 않는다.
- perf start gate 구현에서 아래를 금지한다.
  - `sleep`/`msleep`/고정 지연
    - 예외: `single PUBSUB`은 ready gate 통과 직후
      bounded post-ready settle을 반드시 수행한다. 이는 추가 ready gate가
      아니라 패턴 전용의 고정 안정화 절차이며, C 기준과 bindings 전체에 동일
      의미로 적용해야 한다.
  - monitor snapshot polling
  - ad-hoc retry loop
- perf lifecycle에서 아래와 같은 **벤치 단계**를 새로 만들지 않는다.
  - `preflight`
  - `prime`
  - `settle`
    - 예외: `single PUBSUB` post-ready settle은 ready를
      대체하는 별도 phase가 아니라, 해당 패턴의 전달 준비를 정렬하기 위한
      bounded 안정화 절차로만 허용한다.
  - `stable`
  - `quiet`
  - `quiescent`
  - `idle drain`
    - 예외: `single` recv one-way 패턴은 active 종료 후 bounded idle drain을
      반드시 수행한다. 이 절차의 의미는 "deadline 이전에 송신되어
      queue/in-flight에 남아 있던 메시지를 추가 recv로 비운다"는 운영 절차다.
      active 결과 집계는 suite/pattern 문서가 정의한 active 유효 메시지 규칙에만
      종속된다. 이 idle drain은 single recv one-way 공통 계약으로 C 기준과 bindings
      전체에 동일하게 적용해야 하며, 다른 ad-hoc drain/settle 단계로 확장하면
      안 된다.
  - `completion drain`
    - 예외: `single` request-reply 패턴은 active 종료 후 bounded completion drain을
      반드시 수행한다. 이 절차는 deadline 이전에 제출된 request의 reply completion만
      정리하는 운영 절차이며, active deadline 이후 새 request를 제출하거나 완료 수를
      늘리는 별도 phase가 아니다.
- 위 단계가 이미 존재하지만 실제로는 “ready 이벤트 하나 기다리기” 또는
  “phase 종료 후 남은 메시지 정리”를 우회적으로 표현한 것뿐이라면, 새 단계로
  유지하지 말고 삭제하거나 기존 `ready -> active` 흐름에 흡수한다.
- raw socket client 의 ready gate event 는
  [`doc/guide/06-monitoring.ko.md`](../../core/doc/guide/06-monitoring.ko.md)의
  raw socket monitoring 절을 단일 기준으로 따른다.
- SPOT_PUBSUB 계열은 별도 서비스 이벤트 스트림을 사용하지 않으며, perf-ready 는
  suite별 benchmark barrier protocol 로만 정의한다.
- monitor event rename:
  - raw socket ready event 는 `CONNECTION_READY` 이다.
- routing 검증이 필요한 패턴(예: ROUTER)은 monitor-ready 이후
  단발성 self-check 1회만 수행하고, 실패 시 즉시 fail 처리한다.
- registry/bootstrap/query/summary 조회는 measurement phase 밖에서만 수행한다.

### 1.1.4 오류 가시성 원칙

- setup/configuration 실패는 즉시 fail로 보고한다. 오류를 삼키거나
  무시하는 패턴(빈 catch, silent ignore wrapper 등)은 허용하지 않는다.
- best-effort 경로(stop token 전송 등)에서 발생한 오류는 이후 코드가
  오류 상태를 읽을 수 있도록 보존한다. 오류를 버리지 않는다.
- 오류 처리 로직은 함수/메서드 단위로 모아 하나의 처리 경로로 관리한다.
  오퍼레이션마다 동일한 동작(fail 반환)의 처리 블록을 반복 나열하지 않는다.
- 조건부 오류 분기(예: `EAGAIN`/`EINTR` → retry, 그 외 → fail)가 필요한
  경우에는 해당 처리를 개별로 유지한다.

## 1.2 Binary And Runner Responsibilities

perf 구조는 다음 두 책임으로 분리한다. 이 분리는 `bindings/c/perf`와 bindings perf에
동일하게 적용한다.

### 1.2.1 바이너리 책임

- 바이너리는 **단일 측정 케이스**만 수행한다.
- 단일 측정 케이스의 최소 단위는 `pattern/transport/size/run` 이다.
- 바이너리는 입력 조건에 따라 해당 케이스의 ready/active를 수행하고,
  `RESULT,...` line과 필요한 제어 신호만 stdout으로 출력한다.
- 바이너리는 다음 책임을 가지지 않는다.
  - 여러 pattern 순회
  - 여러 transport 순회
  - 여러 size 반복 실행 orchestration
  - runs > 1 집계
  - markdown table 포맷팅
  - median/최종 report 저장
- 바이너리 내부에서 측정 hot path를 흐리게 하는 report formatting, 문자열 조합,
  동적 집계 컨테이너 orchestration 로직을 추가하면 안 된다.
- perf 기본 HWM sizing은 context auto-HWM 을 따른다. perf가 여는 benchmark
  socket은 역할별 예외 없이 같은 context budget 아래에서 core 계산값을 사용한다.
  기본 경로에서 `SNDHWM`, `RCVHWM`을 숫자로 직접 고정하지 않는다.
- perf 기본 OS socket buffer sizing은 `SNDBUF=-1`, `RCVBUF=-1`이다.
  기본 경로는 OS 기본 buffer와 TCP 자동 조정에 맡기며, `SNDBUF`, `RCVBUF`를
  숫자로 직접 고정하지 않는다.
- benchmark는 메시지 크기별로 context
  `ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES`를 현재 테스트 메시지 크기와 같은 값으로
  설정한다. raw socket `ZLINK_OPT_AUTO_HWM_MSG_UNIT_BYTES`는 저수준 socket별
  override로만 유지하며, MeshNode 또는 Spot handle에는 설정하지 않는다. Spot
  서비스 핸들에 이 공통 옵션을 설정하려는 코드는 정책 위반이고, C API에서는
  `EINVAL` 실패로 처리된다. C perf runner는 일반 패턴의 결과 행 뒤에 runtime
  snapshot에서 실제 수집한 `Auto-HWM detail` 표를 붙이고,
  `Size(B)`, `MsgUnit(B)`, `Scope`, `ScopeCount`를 적용 HWM과 함께 보여야 한다.
  MeshNode와 Spot 계열은 RESULT 계약을 우선하며 내부 socket snapshot을 perf의
  공개 계약으로 사용하지 않는다.
- one-way pattern에서도 이 규칙을 유지한다. 실제 traffic 방향상 한쪽 값만 더
  중요하더라도 기본 bench surface는 auto 계산 결과를 그대로 본다.
- perf는 throughput/bandwidth/latency 중심의 기본 surface만 유지한다. cpu/mem,
  queue, debug, probe 기반 RESULT surface는 기본 perf에 두지 않는다.

### 1.2.2 Runner 책임

- runner(`run_benchmarks*.sh/.ps1`, `run_comparison.py`)는 전체 suite orchestration을 담당한다.
- runner 책임:
  - pattern/transport/size/run 순회
  - 프로세스 시작/종료 및 READY 대기
  - cooldown 적용
  - RESULT line 수집/파싱
  - runs > 1 median 집계
  - markdown table 출력
  - 결과 파일 저장 및 complete/partial 판정
- 사람용 출력 형식과 결과 파일 구조는 runner에서만 관리한다.
- policy 변경으로 출력 형식이나 완료 판정 로직이 바뀌면, 바이너리가 아니라
  runner를 우선 수정한다.

### 1.2.3 구조 불변식

- `pattern/transport/size/run` 단위 측정 의미를 바이너리 밖 runner가 조합한다.
- 바이너리는 “한 케이스 실행 + RESULT line 출력”을 넘는 orchestration 책임을
  가져서는 안 된다.
- runner 리팩토링은 이 책임 분리를 유지해야 하며, 관련 자동 검증(test)도 함께
  갱신해야 한다.

## 1.3 패턴 해석 규칙

- echo
  - 왕복 완료 의미를 유지한다.
  - `*_SENDSEND` 패턴은 public send/recv API로 echo를 만든다.
  - `*_REQREP` 패턴은 public request/reply API와 completion poller로 왕복을 만든다.
  - send 역할과 recv 역할 정책을 둘 다 적용한다.
- one-way send
  - recv 정책은 없다.
  - send 정책만 적용한다.
- one-way recv
  - send 정책은 없다.
  - recv 정책만 적용한다.
- `PUBSUB`, `SPOT_PUBSUB`
  - publisher/server는 one-way send다.
  - subscriber/client는 one-way recv다.
- `MULTI_SPOT_PUBSUB`, `MULTI_SPOT_REQREP`, `MULTI_SPOT_SENDSEND`
  - `clients`는 peer MeshNode 수다.
  - client child process 하나가 MeshNode 하나와 entry Spot 하나를 만든다.
  - 모든 peer는 hub MeshNode에만 연결한다.
  - 정확한 메시지 흐름과 generation 교환은
    [Core 10.0.0 Spot 성능 시험 정책](./PERF_SPOT_TEST_POLICY.md)을 따른다.
- `MULTI_DEALER_ROUTER`
  - 기존 send/send echo 패턴의 호환 이름이다.
  - 새 문서와 새 구현에서는 같은 의미를 `MULTI_DEALER_ROUTER_SENDSEND` 로
    부른다.
- `MULTI_ROUTER_ROUTER`
  - 기존 send/send echo 패턴의 호환 이름이다.
  - 새 문서와 새 구현에서는 같은 의미를 `MULTI_ROUTER_ROUTER_SENDSEND` 로
    부른다.
- `MULTI_DEALER_ROUTER_REQREP`
  - multi suite 의 raw socket request/reply 패턴이다.
  - client(dealer requester) 는 public request API로 request를 제출하고,
    server(router replier) 는 request를 읽은 뒤 public reply API로 응답한다.
  - client completion은 public poller `POLLCOMPLETION` 경로로만 진행한다.
- `MULTI_ROUTER_ROUTER_REQREP`
  - multi suite 의 route-aware raw socket request/reply 패턴이다.
  - 양쪽 모두 routing identity가 있는 socket을 사용하되, 왕복 의미는
    `MULTI_DEALER_ROUTER_REQREP` 와 같다.

---

## 2. 디렉터리 구조

### 2.0 C perf reference

> 아래 경로는 `bindings/c/perf/` 기준이다. 정책 문서 자체는 `doc/perf/`에 위치한다.

```text
bindings/c/perf/
├── run_benchmarks.sh / .ps1                # single 전용 실행 스크립트 (Linux/Windows)
├── run_benchmarks_multi.sh / .ps1          # multi 전용 실행 스크립트 (Linux/Windows)
├── run_comparison.py                       # 통합 비교/실행 스크립트
├── single/                                 # single 벤치마크 소스
├── multi/                                  # multi 벤치마크 소스
└── results/                                # 결과 저장 루트
    ├── single/
    │   └── report/                         # 결과 레포트
    └── multi/
        └── report/                         # 결과 레포트
```

### 2.0.1 bindings (바인딩 라이브러리)

각 바인딩은 동일한 책임 분리를 따른다. 다만 언어와 빌드 시스템에 따라
entrypoint와 소스 하위 디렉터리 위치는 조금 다를 수 있다.

```text
perf/                                       # bindings/<lang>/perf/
├── run_benchmarks.sh / .ps1                # single 공식 entrypoint
├── run_benchmarks_multi.sh / .ps1          # multi 공식 entrypoint
├── single/                                 # single 벤치마크 소스
├── multi/                                  # multi 벤치마크 소스
└── results/                                # 결과 저장 루트 (C 기준과 동일 구조)
    ├── single/
    │   └── report/
    └── multi/
        └── report/
```

- C binding처럼 runner를 `bindings/<lang>/perf/` 루트에 두고, 실제 소스는
  `single/src/`, `multi/src/`, `single/common/`, `multi/common/` 아래에
  두는 구조도 정책 준수 구조로 본다.
- 중요한 점은 경로 이름이 아니라 책임 분리다.
  - runner: pattern/transport/size/run orchestration
  - pattern source: 패턴별 측정 로직
  - common: 공통 계측/출력/보조 인프라

### 2.0.2 소스 파일 위치 및 명명 규칙

#### 소스 위치 규칙

- 모든 벤치마크 소스는 해당 suite의 디렉터리에 위치해야 한다:
  - single: `perf/single/`
  - multi: `perf/multi/`
- 언어별 빌드 시스템이 하위 프로젝트를 요구하는 경우 `perf/single/` 또는 `perf/multi/` 안에 프로젝트 디렉터리를 둔다 (예: .NET의 `Zlink.BindingBench/`, Java의 Gradle/Maven 모듈).
- 벤치마크 소스를 언어 메인 프로젝트의 테스트 트리(예: `src/test/`)에 두지 않는다.

#### 명명 접두어 규칙

- 모든 벤치마크 소스 파일은 **`perf_`** 접두어를 사용한다 (PascalCase 언어는 `Perf` 접두어).
- 기본 패턴: `perf_<pattern>` — 각 언어의 명명 컨벤션을 적용한다.
- perf의 공식 측정 surface는 recv 중심 모델이다. request/reply echo를 추가할
  때는 기존 pattern의 mode나 API 의미를 바꾸지 않고 별도 pattern 이름으로 분리한다.
  handler 또는 completion callback은 측정 data delivery surface로 분리하지 않는다.
- **예외**: 공통 유틸리티 헤더는 `perf_` 접두어 없이 명명할 수 있다 (예: `bench_common.hpp`, `perf_common.hpp`).
- 상세 파일명 규칙은 개별 정책 문서를 참조한다:
  - Single: [PERF_SINGLE_TEST_POLICY.md § 10.1](PERF_SINGLE_TEST_POLICY.md)
  - Multi: [PERF_MULTI_TEST_POLICY.md § 11.1](PERF_MULTI_TEST_POLICY.md)

#### 소스 위치 테이블

| 언어 | single 소스 위치 | multi 소스 위치 | 공통 유틸리티 |
|------|-----------------|----------------|-------------|
| C binding reference | `perf/single/src/` | `perf/multi/src/` | `perf/single/common/`, `perf/multi/common/`, `perf/common/streamclient/` |
| C++ binding | `perf/single/` | `perf/multi/` | `perf_dispatch.hpp` |
| .NET | `perf/single/Zlink.BindingBench/` | `perf/multi/<project>/` 또는 `perf/single/Zlink.BindingBench/` 내 multi role entrypoint | `PerfCommon.cs` |
| Java | `perf/single/<project>/` | `perf/multi/<project>/` 또는 `perf/single/<project>/` 내 multi role entrypoint | `PerfUtil.java` |
| Rust | `perf/single/` | `perf/multi/` | `perf/common/` |
| Go | `perf/single/` | `perf/multi/` | `perf/common/` |
| Node | `perf/single/` | `perf/multi/` | `perf/common/` |
| Python | `perf/single/` | `perf/multi/` | `perf/common/` |

- 컴파일 언어 바인딩(C++/.NET/Java)은 소스 트리 분리 대신 단일 runner에서 `--multi-server`/`--multi-client` role 분기를 제공해도 된다. 이 경우에도 결과 형식, 운영 모드, server/client 프로세스 모델은 동일하게 준수해야 한다.

### 2.0.3 STREAM 소켓 테스트 모델 (공통 필수)

- **STREAM 계열은 multi suite에서만 테스트한다.** single suite에서는 STREAM 소켓 테스트를 수행하지 않는다.
- STREAM 계열(`MULTI_STREAM`)은 반드시 **zlink STREAM server(bind only)** +
  **raw transport client(connect)** 모델로 측정한다.
- zlink STREAM 소켓의 client `connect()` 경로를 벤치마크 클라이언트로 사용하지 않는다.
- STREAM 테스트에서 server를 DEALER/ROUTER/PUBSUB 등 non-STREAM 소켓으로 대체하면 정책 위반이며 결과는 무효다.
- 모델 위반/불일치 구현은 정책 위반으로 간주하며, 해당 코드 경로를 삭제한 뒤 정책 모델로 재구현해야 한다.
- 모델 위반 구현에서 나온 결과는 `UNSUPPORTED`/`SKIP`으로 우회할 수 없으며 정책 산출물로 인정하지 않는다.
- STREAM multi 측정에서는 각 size마다 `connect_ok == target clients`(100%)를 충족해야 하며, 미달 시 반드시 `fail`로 처리한다.
- `MULTI_STREAM`은 **packet semantics**를 측정한다. server는 `zlink_stream_packet_handler()`
  를 사용해 packet 단위로 수신해야 하며, raw recv chunk 경계를 결과 의미로
  노출하면 안 된다.
- 다만 packet semantics를 보존하는 범위의 fast path는 허용한다. 예를 들어 idle connection에서 단일 transport chunk가 이미 정확히 1개의 완전한 packet framing을 포함하면, 내부 조립 단계를 모두 반복할 필요는 없다.

#### Wire Protocol

STREAM 계열 벤치마크는 `zlink_stream_packet_handler()`가 해석하는 packet framing
프로토콜로 통일한다.

```text
┌──────────────┬──────────────┬──────────────┬──────────────┐
│ 2B hdr size  │ 4B body size │ header bytes │ body bytes   │
│  (big-endian)│  (big-endian)│              │              │
└──────────────┴──────────────┴──────────────┴──────────────┘
```

- **client**: 모든 STREAM 패턴과 모든 binding perf runner에서 동일한 공통 raw
  client(`bindings/c/perf/common/streamclient/perf_stream_client.cpp`)를 사용하며,
  `[2B header size][4B body size][header][body]` 형식으로 송신한다.
  수신(echo)도 동일한 framing으로 읽는다.
- **server**: zlink STREAM 소켓으로 bind한 뒤,
  `zlink_stream_packet_handler()`로 packet 단위 수신을 처리한다.
- perf는 raw `STREAM` callback mode를 별도 테스트하지 않는다.
- metric header는 `body`의 선두에 둔다. 즉 perf의 `msg_size`는 `body` 크기를
  뜻하며, `header`는 STREAM packet framing 검증이나 protocol 보조 정보에만
  사용할 수 있고 throughput / latency / bandwidth 집계 기준에는 포함하지 않는다.

#### Server Packet Delivery

STREAM 소켓은 raw TCP 데이터를 수신하므로, 하나의 transport read가 packet
경계와 일치하지 않을 수 있다. perf server는 이 내부 조립을 직접 구현하는 대신,
`zlink_stream_packet_handler()`가 완성한 packet delivery를 기준으로 echo해야
한다.

```text
packet handler delivery:
  1. transport fragment 수신
  2. core가 packet framing 해석
  3. packet handler callback(source_rid, header, body) 호출
  4. server는 callback에서 동일 source_rid로 echo send
```

| 항목 | 규칙 |
|------|------|
| packet 전달 단위 | `source_rid`별 완성 packet delivery |
| framing | `[2B header size][4B body size][header][body]` |
| echo 시점 | packet handler가 완성 packet 1개를 전달하면 즉시 해당 `source_rid`로 echo send |
| partial 처리 | transport fragment 경계와 packet 경계 불일치는 core packet handler 구현이 흡수 |
| connection 정리 | connection 종료 시 해당 `source_rid` state는 core가 정리 |
| 테스트 대상 | raw callback이 아니라 packet handler delivery |

- client의 wire protocol을 packet framing으로 통일하는 이유:
  `STREAM` 테스트는 raw fragment가 아니라 packet receive surface를 측정하려는
  목적이기 때문이다.
- `STREAM` metric 집계는 `body` 선두의 perf metric header를 기준으로 수행한다.
  `header` bytes는 active 유효 메시지 판정과 latency 계산의 anchor로 사용하지
  않는다.
- 이 프로토콜은 multi suite에 적용된다. single suite에서는 STREAM 테스트를 수행하지 않는다.
- legacy callback-named / len32be-named STREAM 패턴은 삭제 대상이다. public
  policy surface에서는 `STREAM` / `MULTI_STREAM`만 사용한다.

### 2.1 결과 저장 규칙

| 항목 | 규칙 |
|------|------|
| 파일명 형식 | `perf_<lang>_<suite>_<platform>_YYYYMMDD_HHMMSS[_<tag>].txt` |
| 날짜 디렉터리 | 사용하지 않음 (파일명에 날짜/시간 포함) |
| `<lang>` | `c`, `cpp`, `dotnet`, `java`, `rust`, `go`, `node`, `python` |
| `<suite>` | `single`, `multi` |
| `<platform>` | `linux`, `windows`, `macos` |
| `<tag>` | `--results-tag` 옵션으로 지정 (선택) |

### 2.2 보존 정책

#### 파일 수 기반 정리

| 디렉터리 | 최대 파일 수 | 초과 시 처리 |
|-----------|-------------|-------------|
| `report/` | 기본 100 | 파일명 사전순 기준 오래된 파일 삭제 |

- `single/`과 `multi/` 각각 독립적으로 적용한다.
- single 엔진은 최대 파일 수를 100으로 하드코딩한다 (`PERF_RESULTS_MAX_FILES` 미참조).
- multi 엔진은 `PERF_RESULTS_MAX_FILES` 환경 변수를 읽는다 (기본 100).

#### 시간 기반 정리 (C perf reference 전용)

| 환경 변수 | 기본값 | 동작 |
|-----------|--------|------|
| `PERF_RESULTS_RETENTION_DAYS` | 90 | 결과 디렉터리 중 `YYYYMMDD` 형식 이름이 기준일보다 오래된 디렉터리를 삭제 |

- `bindings/c/perf/run_benchmarks.sh` 실행 시 자동 적용된다.
- bindings 스크립트에는 적용되지 않는다.

### 2.3 저장 단위

- 스크립트 1회 실행 = 1개 결과 파일. 실행에서 측정된 모든 패턴/transport/size 조합의 결과가 하나의 파일에 기록된다.
  - **예외**: multi에서 nofile/memory guard로 모든 패턴이 skip되면 결과 파일 없이 `exit 0`한다.

---

## 3. 실행 스크립트

### 3.1 개별 실행

| suite | C 기준 스크립트 | bindings 스크립트 | 정책 문서 |
|-------|--------------|-------------------|-----------|
| single | `bindings/c/perf/run_benchmarks.sh` / `.ps1` | `bindings/<binding>/perf/run_benchmarks.sh` / binding-local 실행기 | [PERF_SINGLE_TEST_POLICY.md](PERF_SINGLE_TEST_POLICY.md) |
| multi | `bindings/c/perf/run_benchmarks_multi.sh` / `.ps1` | `bindings/<binding>/perf/run_benchmarks_multi.sh` / binding-local 실행기 | [PERF_MULTI_TEST_POLICY.md](PERF_MULTI_TEST_POLICY.md) |

```bash
# C 기준 single만 실행
bindings/c/perf/run_benchmarks.sh --pattern PAIR

# C 기준 multi만 실행
bindings/c/perf/run_benchmarks_multi.sh --pattern MULTI_STREAM

# bindings 실행 (예: cpp)
bindings/cpp/perf/run_benchmarks.sh --pattern PAIR
bindings/cpp/perf/run_benchmarks_multi.sh --pattern MULTI_STREAM
```

각 스크립트의 상세 옵션은 개별 정책 문서의 섹션 5를 참조한다.

> **정책 준수 실행기**: 아래 스크립트가 각 suite의 유일한 정책 준수 실행기이다. C 기준은 single=`bindings/c/perf/run_benchmarks.sh`, multi=`bindings/c/perf/run_benchmarks_multi.sh`를 사용하고, 다른 bindings는 각 언어 디렉터리의 `bindings/<binding>/perf/run_benchmarks*.sh` 또는 그와 동등한 binding-local 실행기를 사용한다. 내부 실행 엔진과 공식 entrypoint 사이의 호출 체인은 구현 세부이며, 정책은 공식 entrypoint와 책임 분리만 규정한다.
>
> | suite | C 기준 | bindings |
> |-------|------|----------|
> | single | `bindings/c/perf/run_benchmarks.sh` | `bindings/<binding>/perf/run_benchmarks.sh` 또는 동등한 binding-local 실행기 |
> | multi | `bindings/c/perf/run_benchmarks_multi.sh` | `bindings/<binding>/perf/run_benchmarks_multi.sh` 또는 동등한 binding-local 실행기 |
>
> C 기준은 single/multi 스크립트가 같은 디렉터리에 있으므로 `_multi` 접미어로 구분한다. 다른 bindings는 언어별 디렉터리에서 policy-compliant 실행기를 직접 제공해야 한다.

### 3.2 Smoke 테스트

perf smoke 테스트는 전체 패턴과 전체 transport를 대상으로 하되,
메시지 크기를 64B 하나로 고정하여 빠르게 전 경로의 정상 동작을 검증하는
실행이다. 성능 수치 자체보다 **모든 패턴/transport 조합이 fail 없이
통과하는지**를 확인하는 것이 목적이다.

- perf 코드나 실행 스크립트, 정책 문서를 수정한 뒤에는 **single + multi
  smoke 테스트를 모두 실행해야 한다**.
- smoke 실행은 반드시 각 suite의 공식 entrypoint를 사용한다.
  - single: `run_benchmarks.sh` / `.ps1`
  - multi: `run_benchmarks_multi.sh` / `.ps1`
- perf 문맥에서 smoke 테스트는 "`--pattern ALL` + `--msg-sizes 64`로 해당
  suite의 전체 패턴을 64B 크기 하나로 실행하는 검증"을 뜻한다.

```bash
# C 기준 smoke (single)
bindings/c/perf/run_benchmarks.sh --pattern ALL --msg-sizes 64

# C 기준 smoke (multi)
bindings/c/perf/run_benchmarks_multi.sh --pattern ALL --msg-sizes 64

# bindings smoke (예: cpp)
bindings/cpp/perf/run_benchmarks.sh --pattern ALL --msg-sizes 64
bindings/cpp/perf/run_benchmarks_multi.sh --pattern ALL --msg-sizes 64
```

- single과 multi 각각 실행한다.
- `--msg-sizes 64`로 64B 단일 크기만 측정한다.
- `--pattern ALL`로 해당 suite의 전체 패턴을 순회한다.
- transport는 기본값(전체)을 사용한다.
- smoke 통과 기준: 전 조합이 `fail` 없이 완료 (`status=complete`).
- 리팩토링 단계마다 single/multi smoke를 실행하여 기본 경로를 검증한다.
- 모든 리팩토링이 마무리된 뒤 최종 성능 검증 단계에서 full perf를 실행한다.
- 신규 바인딩 추가, CI 검증 시에도 full perf 전에 smoke를 먼저 실행한다.

### 3.3 통합 실행

실행 엔트리포인트는 공식 스크립트다 (§ 3.1 정책 준수 실행기 테이블 참조).
- C 기준 single: `bindings/c/perf/run_benchmarks.sh` / `.ps1`
- C 기준 multi: `bindings/c/perf/run_benchmarks_multi.sh` / `.ps1`
- bindings: `bindings/<binding>/perf/run_benchmarks*.sh` 또는 동등한 binding-local 실행기
- 단일 실행에서 single/multi를 혼합하지 않는다.

```bash
# C 기준: single만 실행
bindings/c/perf/run_benchmarks.sh --pattern ALL

# C 기준: multi만 실행
bindings/c/perf/run_benchmarks_multi.sh --pattern ALL

# C 기준: 특정 패턴만
bindings/c/perf/run_benchmarks.sh --pattern PAIR,PUBSUB

# C 기준: single 실행
bindings/c/perf/run_benchmarks.sh --pattern PAIR

# C 기준: 태그 추가
bindings/c/perf/run_benchmarks.sh --results-tag v1.5.0

# bindings: 동일한 옵션 체계 적용 (예: java)
bindings/java/perf/run_benchmarks.sh --pattern ALL
bindings/java/perf/run_benchmarks_multi.sh --pattern ALL
```

### 3.4 통합 실행 옵션

| 옵션 | 설명 | 기본값 |
|------|------|--------|
| `--pattern NAME` | 측정할 패턴 (쉼표 구분) | 전체 |
| `--runs N` | 반복 횟수 | 1 (Windows multi `.ps1`: 3) |
| `--build-dir PATH` | 빌드 디렉터리 경로 | 자동 탐색 |
| `--reuse-build` | 기존 빌드 재사용 (configure/build 생략) | off |
| `--clean-build` | 빌드 디렉터리 삭제 후 클린 빌드 | off (기본은 증분 빌드) |
| `--pin-cpu` | CPU pinning (Linux: taskset, Windows: processor affinity) | off |
| `--output PATH` | stdout tee 출력 | stdout만 |
| `--results-dir PATH` | 결과 저장 루트 override | `perf/results` |
| `--results-tag NAME` | 결과 파일명 태그 | 없음 |
| `--msg-sizes LIST` | 메시지 크기 목록 | suite별 기본값 |
| `--transports LIST` | transport 목록 | suite별 기본값 |

- suite별 고유 옵션(`--clients` 등)은 개별 스크립트 호출 시 전달한다.
- official perf runner의 기본 동작은 **현재 소스 기준 최신 벤치마크 산출물**을 사용하도록 configure/build를 수행하는 것이다. `--reuse-build`는 stale build 사용을 명시적으로 허용하는 유일한 opt-out이며, 이 플래그 없이 이전 산출물/스크립트를 그대로 실행하는 runner는 정책 위반이다.
- 결과 의미에 직접 영향을 주는 기본값(`clients`, `stream clients`, `server/client io_threads`, `hwm`, `stream hwm`)은 baseline/full-run 계약의 일부다. 기본값을 변경하면 문서와 runner help, 예시, baseline 비교 기준을 같은 변경에서 함께 갱신해야 한다.
- multi suite의 기본 context I/O thread 수는 server/client 모두 `4`다. C, .NET, Java 등 binding perf runner는 별도 override가 없으면 server process와 client process 양쪽에 같은 값 `4`를 적용해야 한다. Python multi perf는 예외적으로 기본값 `1`을 사용한다. Python callback은 GIL 때문에 동시에 실행되지 않으므로 native I/O thread를 기본 `4`로 열면 측정 처리량보다 thread 경합과 CPU 포화가 먼저 커진다. Python runner는 Effective Options에 실제 값을 반드시 기록하고, `--io-threads 4` 또는 `PERF_IO_THREADS=4`로 C 기준 리소스 조건을 명시 실행할 수 있어야 한다.
- multi 패턴은 각 size 케이스를 실행할 때 해당 payload size를 context auto-HWM message unit으로 설정해야 한다. 이 값은 payload 최대 크기 제한이 아니라 HWM 예산을 메시지 슬롯 수로 환산하기 위한 기준 단위다. size별 msg unit 설정이 빠지면 C perf와 HWM/버퍼 조건이 달라져 결과 비교가 무효가 된다.

---

## 4. 결과 파일 형식 (공통)

### 4.1 파일 구조

결과는 `report/`에 사람이 읽을 수 있는 형식으로 저장한다.

#### report/ (결과 레포트)

```text
## Effective Options (start)
- lang: c
- suite: single
- runs: 1
- patterns: PAIR, SPOT_PUBSUB
- transports: tcp, tls, ws, wss
- msg_sizes: 64, 256, 1024, 65536, 131072, 262144
- pin_cpu: off

===============================================================================

## PATTERN: PAIR (one-way)

### Transport: tcp
| Size     |       Throughput | Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) |
|----------|------------------|-----------|---------------|---------------|---------------|
| 64B      |   523.40 Kmsg/s  | 33.5 MB/s |   0.01235 ms  |   0.01820 ms  |   0.02140 ms  |
| 1024B    |   120.30 Kmsg/s  | 123.2 MB/s|   0.05210 ms  |   0.07055 ms  |   0.09210 ms  |
```

- **실행 옵션 헤더 + TABLE**을 저장한다.
- `## Effective Options (start)` / `## Effective Options (result)` 섹션은 실행 시 사용된 옵션을 불릿 목록으로 출력한다. report/ 파일과 stdout 모두에 포함해야 한다.
- `lang` 항목은 필수이며, 실행한 바인딩을 기록한다 (`c`, `cpp`, `dotnet`, `java`, `rust`, `go`, `node`, `python`).
- `suite` 항목은 필수이며, `single` 또는 `multi`를 기록한다.

### 4.2 RESULT line 형식

```text
RESULT,<lib>,<pattern>,<transport>,<size>,<metric>,<value>
```

| metric | 설명 | 필수 |
|--------|------|------|
| `throughput` | echo 패턴: 왕복 완료 수 (`ops/s`, 1 op = send + recv response 1회 완료), one-way 패턴: 단방향 수신 수 (`msg/s`) | MUST |
| `bandwidth` | 네트워크 전송량 (MB/s) — echo 패턴(single request-reply + multi echo): `throughput × size × 2 / 1,000,000`, one-way 패턴(single one-way + multi one-way): `throughput × size / 1,000,000` | MUST |
| `latency` | 레이턴시 (internal ns, external ms) | MUST |
| `latency_p95` | 레이턴시 95th percentile (internal ns, external ms) | MUST |
| `latency_p99` | 레이턴시 99th percentile (internal ns, external ms) | MUST |
- throughput 단위는 패턴의 메시지 흐름 방향에 따라 결정된다. echo(왕복) 패턴은 `ops/s`, one-way(단방향) 패턴은 `msg/s`. 상세 분류는 개별 정책 문서 섹션 8.1을 참조한다.
- bandwidth는 throughput 단위가 다른 패턴 간에도 실제 데이터 처리량으로 직접 비교할 수 있는 공통 지표이다. 상세 계산은 개별 정책 문서 섹션 8.3을 참조한다.
- 기본 perf surface와 RESULT 계약에는 cpu/mem 계열 메트릭을 포함하지 않는다.
- 상세 META 키 및 패턴별 측정 방식은 개별 정책 문서를 참조한다.

### 4.3 저장 규칙

결과는 항상 `<suite>/report/`에 저장된다 (complete/partial 무관).

- 파일명 형식: `perf_<lang>_<suite>_<platform>_YYYYMMDD_HHMMSS[_<tag>].txt`
- 완료 판정 기준: `expected_result_lines == actual_result_lines`이다.
  `expected_result_lines`는 unsupported와 skip을 제외한 요청 조합 수에 필수
  RESULT metric 5개를 곱한 값이고, `actual_result_lines`는 성공적으로 출력된
  필수 RESULT 라인 수이다.
- C perf에서 전체 기본 full matrix가 `complete`로 끝난 경우에는 같은 결과
  파일을 `bindings/c/perf/baseline/`에도 저장하여 다음 회귀 비교 기준으로
  사용한다. partial, smoke, 특정 패턴/transport/size만 실행한 결과는 baseline
  갱신 대상이 아니다. 단, 사용자가 transport/size를 명시했더라도 그 값이 suite
  기본 full matrix와 정확히 같으면 full matrix로 본다.

---

## 5. 출력 형식 (공통)

### 5.1 스크립트 결과 테이블

> **구현 필수**: 모든 실행 스크립트는 RESULT line 외에 아래 형식의 사람이 읽을 수 있는 테이블을 **반드시 stdout에 출력하고, 결과 파일에도 TABLE 영역으로 기록**해야 한다. RESULT line만 출력하고 테이블을 생략하면 안 된다.

```text
## Effective Options (start)
- lang: c
- suite: single
- runs: 1
- patterns: PAIR, SPOT_PUBSUB
- transports: tcp, tls, ws, wss
- msg_sizes: 64, 256, 1024, 65536, 131072, 262144
- pin_cpu: off

===============================================================================

## PATTERN: PAIR (one-way)

### Transport: tcp
| Size     |       Throughput | Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) |
|----------|------------------|-----------|---------------|---------------|---------------|
| 64B      |   523.40 Kmsg/s  | 33.5 MB/s |  0.01235 ms  |  0.01820 ms  |  0.02140 ms  |
| 1024B    |   312.50 Kmsg/s  | 320.0 MB/s|  0.01844 ms  |  0.02755 ms  |  0.03310 ms  |


===============================================================================

## PATTERN: MULTI_DEALER_DEALER (one-way)

### Transport: tcp
| Size     |       Throughput |  Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) |
|----------|------------------|------------|---------------|---------------|---------------|
| 64B      |   150.00 Kmsg/s  |   9.6 MB/s |      0.05 ms  |      0.06 ms  |      0.08 ms  |
| 1024B    |   120.30 Kmsg/s  | 123.2 MB/s |      0.05 ms  |      0.07 ms  |      0.09 ms  |
```

- **패턴 간 구분선**: 패턴이 바뀔 때 `===============================================================================` 구분선을 출력한다 (첫 번째 패턴 앞에는 출력하지 않음).

| 컬럼 | 단위 | 비고 |
|------|------|------|
| Throughput | echo: `Kops/s`, one-way: `Kmsg/s` | 패턴 방향별 단위 — 개별 정책 문서 섹션 8.1 참조 |
| Bandwidth | `MB/s` | 네트워크 전송량 — 개별 정책 문서 섹션 8.3 참조 |
| Lat.Mean / Lat.P95 / Lat.P99 | `ms` (밀리초, external display) | 평균/95th/99th |

### 5.2 진행 로그

벤치마크 실행 중 **사이즈별 결과 테이블 행을 즉시 출력**하여 진행 상황과 측정 데이터를 동시에 제공한다.

#### 출력 규칙

| 항목 | 규칙 |
|------|------|
| 테이블 header | transport당 1회 출력 (header + separator) |
| 결과 행 | 사이즈별 결과 확정 즉시 출력 |
| `runs=1` | `run N/M:` 및 `median:` 레이블 없이 테이블만 출력 |
| `runs>1` | `run N/M:` 레이블 + 각 run 테이블 + `median:` 최종 테이블 |
| median 테이블 | 모든 run 완료 후 metric별 median 값으로 구성 |
| 실패 행 | metric 컬럼에 `FAIL` 표시 |
| 미지원 행 | metric 컬럼에 `UNSUPPORTED` 표시 |
| cooldown 표시 | `[cooldown Nms]`, `[transport cooldown Nms]` (multi) |
| 실패 표시 | `(failures=N) Done` |
| 조건 | 항상 출력 (`PERF_DEBUG`와 무관) |

- 컬럼 순서 및 형식은 § 5.1 결과 테이블과 동일하다.
- 바이너리 stderr는 stdout 결과에 통합하지 않지만, multi 엔진(`run_comparison.py`)은 stderr에서 `protocol not supported` 문자열을 감지하여 `unsupported` 자동 분류에 활용한다 (§ 7.4 참조).

상세 형식은 suite별 정책 문서를 참조한다:
- Single: [PERF_SINGLE_TEST_POLICY.md § 6.3](PERF_SINGLE_TEST_POLICY.md)
- Multi: [PERF_MULTI_TEST_POLICY.md § 6.3](PERF_MULTI_TEST_POLICY.md)

**Single (runs=1):**
```text
  > Benchmarking current for PAIR...
    Testing tcp:
      | Size     |       Throughput |  Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) |
      |----------|------------------|------------|---------------|---------------|---------------|
      | 64B      |   523.40 Kmsg/s  | 33.5 MB/s  |  0.01235 ms  |  0.01820 ms  |  0.02140 ms  |
      | 256B     |   480.12 Kmsg/s  | 122.9 MB/s |  0.01420 ms  |  0.02030 ms  |  0.02410 ms  |
    Testing tcp: Done
```

**Multi (runs=3):**
```text
  > Benchmarking current for MULTI_DEALER_DEALER...
    Testing tcp | 64B,256B:
      run 1/3:
        | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) |
        |----------|------------------|--------------|---------------|---------------|---------------|
        | 64B      |    121.98 Kmsg/s |    15.61 MB/s |      0.81 ms  |      1.01 ms  |      1.26 ms  |
        | 256B     |    ...
      [cooldown 3000ms]
      run 2/3:
        ...
      [cooldown 3000ms]
      run 3/3:
        ...
      median:
        | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) |
        |----------|------------------|--------------|---------------|---------------|---------------|
        | 64B      |    ...
        | 256B     |    ...
    Testing tcp: Done
```

### 5.3 완료와 실패 요약

`## Result Data`는 성공한 조합이 있을 때만 출력한다. 이 섹션에는 성공한
조합의 `RESULT,current,...` 라인만 기록한다. `UNSUPPORTED`, `SKIP`, `FAIL`
토큰은 사람이 읽는 진행 테이블, Completion 카운트, `## Skips`, `## Failures`로
표현하고 Result Data에는 넣지 않는다.

Completion 뒤에는 필요한 경우 `## Skips`, `## Failures` 순서로 요약을 출력한다.
마지막에는 저장된 결과 파일 경로와 status를 출력한다. single suite는 C single
출력 순서에 맞춰 `## Failures`를 auto-HWM detail 앞에 둘 수 있다.

```text
## Completion
- status: partial
- expected_result_lines: 10
- actual_result_lines: 5

## Skips
- MULTI_STREAM: memory_guard_clients=10000

## Failures
- PAIR current ipc 64B: timeout
- MULTI_STREAM current wss 65536B: no_data

Saved result file: ... (status=partial)
```

---

## 6. 리소스 메트릭 수집

- 이번 정책의 기본 perf surface와 RESULT 계약에는 cpu/mem 계열 메트릭을 포함하지 않는다.
- cpu/mem 수집이 필요하면 별도 진단 작업으로 분리하고, 기본 runner/binary/output 계약과 섞지 않는다.

---

## 7. 실패 처리 정책 (공통 필수)

### 7.1 Retry(재시도) 금지

벤치마크 실행 스크립트 및 바이너리에 **retry/재시도 로직을 구현하지 않는다**.

| 항목 | 규칙 |
|------|------|
| 스크립트 레벨 재시도 | 금지 — 실패한 pattern/transport/size 조합을 자동으로 다시 실행하지 않는다 |
| 바이너리 내부 재시도 | 금지 — send/recv 실패 시 자동 재시도하지 않는다. `EAGAIN`은 pending 상태로 기록하고 이후 `PollOut` readiness에서 재개할 수 있으나, 동일 호출 흐름에서의 즉시 retry loop는 금지한다 |
| 환경 변수 | `PERF_MULTI_ATTEMPTS`, `PERF_MULTI_STREAM_ATTEMPTS` 및 레거시 `PERF_MULTI_ATTEMPTS`, `PERF_MULTI_STREAM_ATTEMPTS`는 **삭제 대상**이다. 구현에 존재하면 제거해야 한다 |

- **이유**: 재시도는 실패 원인을 숨긴다. 벤치마크 실패는 라이브러리 또는 환경의 실제 문제를 반영하며, 재시도로 통과시키면 회귀가 감지되지 않는다.

### 7.2 Inflight/Outstanding 옵션 금지

벤치마크 바이너리 및 스크립트에 **inflight, outstanding, in-flight 제한 옵션**을 두지 않는다.

| 항목 | 규칙 |
|------|------|
| CLI 옵션 | `--inflight`, `--outstanding`, `--max-in-flight` 등 inflight 깊이를 조절하는 옵션을 제공하지 않는다 |
| 환경 변수 | `PERF_INFLIGHT`, `PERF_MULTI_INFLIGHT`, `PERF_OUTSTANDING` 등 inflight 관련 환경 변수는 **삭제 대상**이다. 구현에 존재하면 제거해야 한다 |
| 하드코딩 flow control | `outstanding_limit`, `window_exhausted` 등 send/recv 차이 기반의 인위적 흐름 제어는 제거한다. 기본 경로는 auto-HWM이 제공하는 send 큐 backpressure를 사용한다 |

- **이유**: inflight 제한은 벤치마크 결과를 인위적으로 왜곡한다. 라이브러리의 실제 처리 능력을 측정해야 하며, 벤치마크 인프라가 추가 병목을 도입하면 안 된다.
- one-way 패턴에서는 응답이 없으므로 outstanding 개념 자체가 성립하지 않는다.
- echo 패턴에서는 클라이언트 측 per-socket pending 제어(1:1 send-recv)와 소켓 HWM이 자연 backpressure를 제공하므로 별도의 outstanding 제한이 불필요하다.

### 7.3 실패 시 대응 절차

| 단계 | 행동 |
|------|------|
| 1. 실패 기록 | 실패한 조합을 `## Failures` 섹션에 기록하고 결과 파일에 `status=partial`로 저장한다 |
| 2. 원인 파악 | 로그, 종료 코드, timeout 여부를 확인하여 실패 원인을 빠르게 파악한다 |
| 3-a. 벤치마크 코드 이슈 | 벤치마크 구현 버그이면 수정 후 재실행한다 |
| 3-b. core zlink 라이브러리 이슈 | 라이브러리 자체 결함이면 벤치마크 코드에서 우회(workaround)하지 않는다. 재현 가능한 회귀 테스트를 먼저 추가하고, core 버그를 수정한 뒤 perf 작업을 계속 진행한다 |
| 3-c. 환경 이슈 | OS 리소스(fd limit, port 고갈 등)이면 환경을 수정한 뒤 재실행한다 |

- 재시도로 문제를 숨기지 않는다. 실패는 반드시 원인을 파악한 뒤 근본 원인을 해결해야 한다.
- core 라이브러리 버그를 발견하면 "perf만 통과시키는 우회"를 금지한다. 반드시
  재현 테스트를 추가해 버그를 고정한 뒤, core 수정과 함께 해결해야 한다.

### 7.4 UNSUPPORTED 오용 금지

정책 문서(§10.3 / §11.3)에 **정의된 transport**가 실행 시 실패하면 반드시 `fail`로 보고해야 한다. `UNSUPPORTED`로 보고하여 실패를 숨기는 것을 **금지**한다.

| 상황 | 올바른 상태 | 설명 |
|------|------------|------|
| 정의된 transport가 정상 동작 | `success` | RESULT line 출력 |
| 정의된 transport가 실패 (timeout, crash, no_data 등) | `fail` | 원인 파악 후 수정 필요 |
| 정책에 정의되지 않은 transport 조합 | `unsupported` | 결과 제외 |
| stderr에 `protocol not supported` 포함 | `unsupported` | 런타임에서 지원되지 않는 transport 자동 감지 (multi 엔진만 지원, single 엔진은 stdout 토큰만 사용) |
| 플랫폼 제약으로 실행 불가 (예: Windows에서 ipc) | `skip` | reason 명시 필수 |

- `UNSUPPORTED`는 **정책에 정의되지 않은** pattern-transport 조합에만 사용한다.
- **stderr 기반 자동 분류**: 바이너리 stderr에 `protocol not supported` 문자열이 포함되면 multi 실행 엔진(`run_comparison.py`)이 해당 조합을 `unsupported`로 자동 분류한다. single 엔진(`single/run_comparison.py`)은 stderr 문자열 기반 분류를 수행하지 않으며 stdout `UNSUPPORTED` 토큰만 인식한다.
- 실패를 `UNSUPPORTED`로 위장하면 회귀(regression)가 감지되지 않으므로 엄격히 금지한다.

### 7.5 공통화 경계 원칙

perf 구현의 공통화 기준은 **코드 중복 제거 자체가 아니라 benchmark 의미 보존과
전체 복잡도 감소**다. 공통 helper는 지원 인프라를 숨길 수는 있지만, 패턴의
측정 의미, I/O 모델, routing/backpressure semantics를 숨겨서는 안 된다.

#### 공통화 권장 대상

아래 항목은 공통 헤더/공통 소스/공통 runner helper로 수렴하는 것을 권장한다.

| 항목 | 설명 |
|------|------|
| CLI 인자 파싱 | `argc`/`argv` 해석, 옵션 추출 |
| 환경 변수 해석 | `resolve_bench_msg_sizes`, `resolve_multi_bench_settings` 등 |
| 표 출력 포맷 | markdown table, `Effective Options`, failure summary |
| TLS 설정 | `setup_tls_client`, `setup_tls_server` |
| Context RAII | `ctx_guard_t` 등 리소스 관리 wrapper |
| 타이머/스톱워치 | `stopwatch_t`, 시간 측정 유틸리티 |
| Monitor 유틸리티 | raw socket client `CONNECTION_READY` counting helper, multi SPOT_PUBSUB / multi SPOT_REQREP / multi SPOT_SENDSEND=`READY/START` barrier helper |
| transport 가용성 검사 | `transport_available()` |
| 공통 cleanup | socket / monitor / context close helper |

위 항목은 **벤치 의미를 바꾸지 않는 지원 인프라**로 분류한다.

#### 공통 구현 강제 대상

아래 항목은 구현체마다 중복 정의하지 말고, 공통 모듈의 단일 구현을 사용해야 한다.

| 항목 | 설명 |
|------|------|
| RESULT line 포맷팅/출력 | `RESULT,<lib>,<pattern>,...` 형식과 필드 순서를 단일 구현으로 유지 |
| metric header encode/decode | `magic`, `run_id`, `phase`, `msg_size`, `seq`, `sent_ts_ns` 처리 |
| phase별 유효 샘플 판정 | active 구간 구분과 유효 header 판정 |
| throughput 계산 | 유효 수신 건수 기반 계산 |
| bandwidth 계산 | throughput와 payload size 기반 계산 |
| latency 샘플 집계 | header timestamp 기반 샘플 수집 |
| p95/p99 계산 | latency 분포 대표값 계산 |
| 메트릭 보정/출력 계약 | latency triplet 보정, metric naming, RESULT line 계약 유지 |

이 항목들은 결과 해석과 조합 간 비교 가능성을 결정하는 **측정 계약**이므로
선택적 공통화가 아니라 **공통 구현 강제 대상**으로 본다.

#### 공통화 허용 기준

- 공통화의 목적은 코드 이동이 아니라 복잡도 감소여야 한다.
- single/multi가 같은 메트릭/출력 계약을 쓰도록 runner surface를 공통화한다.
- helper가 없어도 각 패턴의 측정 의미를 몇 문장으로 설명할 수 있어야 한다.
- helper는 설정, 출력, 정리, 계측 인프라를 감싸는 용도로만 사용한다.
- C binding의 raw single one-way 패턴처럼 측정 골격이 거의 같은 경우에는,
  `single/common`에 recv/send skeleton을 두고 각 패턴 파일에는 아래 항목만
  남기는 구성을 권장한다.
  - 어떤 zlink send/recv API를 쓰는지
  - ready gate와 setup이 어떻게 다른지
  - topic, routing metadata, probe 같은 패턴 전용 규칙

#### 반드시 인라인 유지할 코어 로직

아래 로직은 각 benchmark 소스 파일 안에서 명시적으로 드러나야 한다.

- 해당 패턴이 사용하는 **send/recv API 호출** (어떤 zlink API를 쓰는지)
- routing frame 조립/해석
- `EAGAIN` 이후 pending flag / pending deque 처리 전략 (패턴별 backpressure
  방식이 다를 때)
- `recv + poller` 실행 경로가 명시적으로 드러나는지
- ready gate 통과 이후 benchmark를 시작하는 실제 조건
- 소켓/handle 생성 및 연결 방식

즉, 파일 하나만 읽어도 해당 패턴이 **어떤 zlink API를 어떻게 사용하는지**
이해할 수 있어야 한다.

#### template policy 예외

동일 구조의 echo/relay 패턴에서 send/recv API 호출만 다르고 phase 제어,
poller event loop, latency 집계 등의 **공통 골격이 95% 이상 동일**한 경우,
아래 조건을 모두 만족하면 template header로 공통 골격을 추출할 수 있다.

- 각 패턴 파일이 **policy struct**로 send/recv API 호출을 명시적으로 정의한다.
  policy struct를 보면 해당 패턴이 어떤 zlink API를 쓰는지 즉시 알 수 있어야
  한다.
- 소켓/handle 생성, 연결, monitor-ready gate는 패턴 파일에 인라인으로 남긴다.
- template은 C++ template instantiation으로 컴파일 시 inline되어 **런타임
  비용이 0**이어야 한다. function pointer, virtual dispatch, `std::function`
  기반 간접 호출은 허용하지 않는다.
- template header 내부에 pattern별 분기(`if`/`switch`)가 없어야 한다.
  패턴 차이는 오직 template parameter(policy struct)로만 표현한다.
- 구조가 다른 패턴(one-way publish, stream framing, deque backpressure 등)을
  같은 template에 억지로 끼워 넣지 않는다.

이 예외는 **동일 코드 복붙을 줄여 유지보수 비용을 낮추기 위한 것**이며,
서로 다른 측정 구조를 하나의 추상화에 합치는 용도가 아니다.

#### 과도한 공통화 판정 기준

아래 중 하나라도 만족하면 과도한 공통화로 간주하고 분리한다.

- helper/template 안에 pattern별 분기(`if`/`switch`)가 늘어나 새 패턴 추가 시
  helper를 계속 수정해야 한다.
- backpressure, routing, phase 의미가 helper 내부로 숨어 파일만 봐서는 설명이
  안 된다.
- 공통화 이후 변경 증폭이 줄지 않고 오히려 여러 패턴이 한 helper에 결합된다.
- 구조가 다른 패턴을 하나의 template/helper에 합치기 위해 조건부 로직이
  추가된다.

#### STREAM client 예외 (검증 인프라)

`bindings/c/perf/common/streamclient/`의 STREAM raw/multi client 코드는
**벤치마크 대상 라이브러리 자체가 아니라 검증 인프라**로 간주한다. STREAM
client는 zlink socket client가 아니라 외부 raw TCP/TLS/WS/WSS peer 역할을
하므로, 언어별 binding public API 사용 원칙의 예외다.

- STREAM client 공통 구현은 `common/streamclient/`에 모아둘 수 있다.
- C++, .NET, Java, Rust, Go, Node, Python 등 모든 binding perf runner는
  `perf_stream_client` 공용 바이너리를 wrapper/symlink로 연결해 사용할 수 있다.
  이것은 C perf 결과를 중계하는 우회가 아니라 STREAM server 측 binding surface를
  동일한 raw peer 조건으로 검증하기 위한 정책상 허용된 구조다.
- STREAM server는 이 예외에 포함되지 않는다. 각 binding의 STREAM server
  benchmark는 해당 binding의 public server/packet handler surface를 사용해야 한다.
- 이 예외는 STREAM 계열 client 인프라에만 적용한다.
- STREAM 계열은 multi suite에서만 테스트하므로 single suite에는 해당 없다.

---

## 7.6 리팩토링 원칙 (공통)

> 참조: [`AGENTS.md`](../../AGENTS.md)

perf 벤치마크 코드와 실행 인프라를 리팩토링할 때는 아래 원칙을 공통으로
적용한다. `bindings/c/perf/README.md`는 사용 방법만 설명하며, 설계/리팩토링 기준은
본 정책 문서가 source of truth다.

### 7.6.1 성능 비회귀 우선

- 구조 변경은 single/multi 기준 성능을 저하시켜서는 안 된다.
- 각 리팩토링 단계는 single + multi smoke 테스트로 기본 경로가 깨지지 않았는지
  확인한 뒤 다음 단계로 진행한다.
- 모든 리팩토링 단계가 끝난 뒤 최종 검증으로 full single + multi perf 실행을
  수행해 기준선 비회귀를 확인한다.
- 코드 품질이 개선되더라도 throughput/latency가 회귀하면 해당 변경을 수용하지
  않는다.

### 7.6.2 복잡도 감소가 목적이다

- 리팩토링은 코드를 옮기는 작업이 아니라 전체 복잡도를 줄이는 작업이어야 한다.
- 얕은 wrapper, pass-through 계층, config flag 기반 분기로 간접비만 늘리는
  구조를 제거한다.
- 각 계층은 단순 위임이 아니라 서로 다른 추상화를 제공해야 한다.

### 7.6.3 깊은 모듈과 명확한 ownership

- 넓은 호출 표면의 작은 함수 다발보다, 좁은 인터페이스와 풍부한 내부를 가진
  모듈을 선호한다.
- 소켓, 컨텍스트, 타이머, 파일 디스크립터 등 모든 리소스는 정확히 하나의
  authoritative close owner를 가져야 한다.
- lifecycle, ownership, invariant는 몇 문장으로 설명 가능해야 한다.

### 7.6.4 정보 은닉

- benchmark 바이너리는 라이브러리 내부 구조에 의존하지 않아야 한다.
- 패턴별 측정 의미와 프로세스 관리, 결과 포맷팅, 파일 I/O 같은 메커니즘을
  분리한다.
- phase machinery나 transport 내부를 패턴 수준 측정 코드에 노출하지 않는다.

### 7.6.5 retry / workaround / 인위적 흐름 제어 금지

- 스크립트와 바이너리에 retry 로직을 넣지 않는다.
- inflight/outstanding 제한 옵션으로 측정 의미를 왜곡하지 않는다.
- 실패를 `UNSUPPORTED`나 우회 로직으로 숨기지 않는다.
- 실패는 실제 신호로 취급하고 근본 원인을 수정한다.

### 7.6.6 죽은 코드와 레거시 옵션 정리

- 미사용 코드, retry 관련 변수, inflight 관련 변수, orphan helper는
  리팩토링 과정에서 제거한다.
- compatibility shim, `_unused` 류의 이름 변경, `// removed` 주석으로
  잔존물을 남기지 않는다.

### 7.6.7 구조로 오용을 방지한다

- 정책 문서나 런타임 검사에만 의존하지 말고 타입과 API 설계로 오용을 막는다.
- 예: RAII guard, enum-typed phase state, compile-time pattern/transport
  validation.

### 7.6.8 변경 증폭을 줄인다

- 새 pattern 추가는 새 소스 파일과 transport matrix entry 정도로 끝나야 한다.
- 새 transport 추가가 pattern-level 코드를 건드리게 만들면 경계가 잘못된 것이다.
- 한 곳의 변경이 여러 곳을 강제로 수정하게 만들면 추상화를 다시 설계해야 한다.

### 7.6.9 단계별 게이트

- 각 리팩토링 단계는 아래 게이트를 통과해야 한다.
  1. 기능 게이트: `run_test_lanes.sh`
  2. 성능 게이트: single + multi smoke 테스트 통과
  3. hot-path 게이트: 측정 경로에 새 lock/alloc/log 없음
- 현재 단계 게이트를 통과하기 전에는 다음 단계를 시작하지 않는다.
- 모든 리팩토링 단계가 끝난 뒤에는 최종 성능 게이트로 full single + multi perf를
  실행해 회귀가 없는지 확인한다.

---

## 8. 환경 변수 (공통)

| 변수 | 설명 | 기본값 |
|------|------|--------|
| `PERF_DEBUG` | 디버그 로그 | unset |
| `PERF_IO_THREADS` | context I/O threads. single 기본값은 모든 패턴에서 1이며, SPOT_PUBSUB 계열 예외를 두지 않는다. multi 기본값은 server/client 모두 4다. Python multi는 GIL 기반 callback 경합을 피하기 위해 기본값 1을 사용하되, 이 변수를 설정하면 명시값을 따른다. | suite별 기본값 |
| `PERF_MSG_SIZES` | 테스트 size 목록이며 runner가 size별 case로 나누어 실행한다. single 기본값은 `64,256,1024,65536,131072,262144`, multi 기본값은 `64,256,1024,4096,65536,131072`, multi STREAM 기본값은 `64,256,1024,65536`이다. | suite/패턴별 기본값 |
| `PERF_TRANSPORTS` | 테스트 transport 목록 | suite/패턴별 기본값 |
| `PERF_TASKSET` | CPU pinning (`1`로 활성화, Linux: taskset, Windows: processor affinity) | 0 |
| `PERF_FAIL_FAST` | 실패 시 즉시 중단 | 0 |
| `PERF_MAX_SOCKETS` | context max sockets | auto |
| `PERF_DISABLE_RESOURCE_METRICS` | 리소스 메트릭(CPU/메모리) 수집 비활성화 (`1`로 활성화) | 0 |
| `PERF_STREAM_NON_TCP_CLIENTS_MAX` | STREAM 계열 non-tcp transport의 최대 client cap | 10000 |
| `PERF_RESULTS_MAX_FILES` | report/ 디렉터리 최대 파일 수 (multi 전용) | 100 |
| `PERF_MULTI_LATENCY_SAMPLE_CAP` | multi sampler 하나가 보관하는 percentile sample 상한 | 65536 |
| `PERF_SINGLE_LATENCY_SAMPLE_CAP` | single sampler 하나가 보관하는 percentile sample 상한 | 1000000 |

- 위 환경 변수는 C 기준과 모든 바인딩에서 동일하게 적용된다 (단, `PERF_RESULTS_MAX_FILES`는 multi 엔진만 참조하며, single 엔진은 100 하드코딩).
- suite별 고유 환경 변수는 개별 정책 문서를 참조한다:
  - Single: [PERF_SINGLE_TEST_POLICY.md § 11](PERF_SINGLE_TEST_POLICY.md)
  - Multi: [PERF_MULTI_TEST_POLICY.md § 12](PERF_MULTI_TEST_POLICY.md)
