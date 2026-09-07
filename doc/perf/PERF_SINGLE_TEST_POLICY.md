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
| throughput | `active 수신 건수 / active 시간(초)` (msg/s) |
| latency | active 구간 수신 payload header timestamp 기반 (internal ns / external ms) |
| 대표값 | runs > 1일 때 metric별 median |
| 저장 경로 | `perf/results/single/report/` 단일 |

- 목적: 단일 소켓 경로에서 throughput, bandwidth, latency를 측정한다.
- **Single suite는 one-way 5 pattern(`PAIR`, `PUBSUB`, `DEALER_DEALER`,
  `DEALER_ROUTER`, `ROUTER_ROUTER`)만 측정한다. request/reply 모델은 single suite에서
  제외하고 [PERF_MULTI_TEST_POLICY.md](PERF_MULTI_TEST_POLICY.md)의 multi suite에서만
  측정·판정한다.** 근거: single 실행 모델은 전용 OS thread + synchronous API이고
  측정 구간의 async terminal을 금지하는데, 각 binding이 공개한 request terminal은
  reply까지 블로킹하는 동기형과 비동기형 둘뿐이고
  [async-coroutine-policy](../../bindings/doc/spec/async-coroutine-policy.ko.md)가
  request callback terminal을 제공하지 않는다고 못박고 있어, "동기 실행 모델을
  유지한 채 backpressure 경계까지 연속 제출"을 만족하는 구현이 공개 계약에
  존재하지 않는다. request/reply 부하 모델은 multi 정책이 규정한 비동기 연속
  제출로만 측정한다. (결정: D-BP3)
- 같은 active 구간에서 동일 메시지 집합으로 latency도 함께 집계한다.
- cpu/mem은 single 기본 perf surface와 RESULT 계약에 포함하지 않는다.
- `single`의 공식 lifecycle은 `ready -> active`다.
- size 변경 시마다 별도 프로세스로 실행하여 케이스 간 메트릭 오염을 방지한다.
- ready bool/count를 복사하기 위한 별도 state struct, heap alloc, mutex/cv 계층은
  만들지 않는다.
- 한 줄 요약: `single = ready + active`

### 1.1 I/O 모델

- 측정 집계가 걸리는 수신 경로는 **recv 모델**(poller `POLLIN` readiness 감지 +
  nonblocking `recv` drain) 을 기본으로 한다. callback으로 측정 data delivery를
  직접 집계하는 경로는 single 에서 사용하지 않는다.
- **single 실행 모델은 전용 OS thread + synchronous API다.** 동시에 진행하는
  sender/receiver 역할은 전용 OS thread에서 구동한다. raw send는 blocking
  terminal을 사용하므로 HWM 도달 시 Core가 sender thread를 대기시킨다.
  측정 구간에는 coroutine, async task, Promise/Future executor, event-loop yield를
  사용하지 않는다.
- Go는 역할별 goroutine을 active 구간 전체에서 `runtime.LockOSThread()`로 고정한다.
  이 goroutine은 다른 작업과 OS thread를 공유하지 않는다. Node는
  `worker_threads`, Python은 `threading.Thread`에서 synchronous terminal과 recv
  loop를 실행한다. C++은 `co_await`, .NET은 `Task`, Rust는 Future executor를
  single 측정 경로에서 사용하지 않는다.
- `PAIR`, `PUBSUB`, `DEALER_DEALER`, `DEALER_ROUTER`,
  `ROUTER_ROUTER` 는 이 recv 모델로 active payload를 집계한다. single suite의
  공식 패턴은 이 5개뿐이며, request-reply completion 모델을 쓰는 패턴은 없다(§1).

#### 프로세스/스레드 모델

single은 **단일 프로세스** 안에서 sender와 receiver를 동시에 구동한다. 구현은
sender thread + main recv loop 또는 sender thread + receiver thread 처럼 나뉠 수
있지만, 측정 중 송신과 수신/progress가 동시에 진행되어야 한다.
benchmark process의 setup과 ready 확인도 같은 thread들이 synchronous API로
수행한다. 실행 스크립트가 process를 시작하고 stdout을 읽는 orchestration 방식은
이 규칙의 대상이 아니다.
single 의 모든 패턴은 one-way 측정 surface를 사용한다. request-reply 비용은
single에서 측정하지 않고 multi suite의 `MULTI_*_REQREP` 패턴으로 측정한다(§1).

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

**공통**:
- raw one-way single에는 `EAGAIN` 기반 pending 관리, send-ready handler 등
  multi에서 사용하는 backpressure 메커니즘을 적용하지 않는다. one-way sender는
  blocking send terminal이 돌려주는 backpressure만 사용하며, in-flight 메시지 수를
  코드로 관리하거나 상한으로 고정하지 않는다
  ([PERF_POLICY.md § 7.2](PERF_POLICY.md)).
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

즉, throughput과 latency는 동일한 유효 메시지 집합을 사용한다.

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

공식 `--pattern ALL`은 아래 5개 패턴을 정확히 선택한다.

- PAIR
- PUBSUB
- DEALER_DEALER
- DEALER_ROUTER
- ROUTER_ROUTER

> STREAM 계열(STREAM)은 single suite에서 테스트하지 않는다.
> request/reply 계열(`DEALER_ROUTER_REQREP`, `ROUTER_ROUTER_REQREP`)도 single suite에서
> 테스트하지 않는다. multi suite의 `MULTI_DEALER_ROUTER_REQREP`,
> `MULTI_ROUTER_ROUTER_REQREP`에서만 측정한다(§1, D-BP3).

#### recv mode 지원 범위

| 패턴 | 허용 mode |
|------|-----------|
| PAIR | `recv` |
| PUBSUB | `recv` |
| DEALER_DEALER | `recv` |
| DEALER_ROUTER | `recv` |
| ROUTER_ROUTER | `recv` |

정책:

- single 전 패턴의 테스트 mode는 recv이다.
- 기존 `DEALER_ROUTER`, `ROUTER_ROUTER`의 mode를 바꾸거나 옵션으로 request-reply를
  섞으면 안 된다. single에 request-reply 패턴을 다시 추가하지 않는다.

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
  - request/reply 모델(`DEALER_ROUTER_REQREP`, `ROUTER_ROUTER_REQREP`)을 single suite에서
    제외하고 공식 패턴을 one-way 5개로 고정했다 (D-BP3).
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
