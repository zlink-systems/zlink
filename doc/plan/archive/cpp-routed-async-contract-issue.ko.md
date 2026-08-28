# Bindings routed async admission 구조 설계 이슈

> 작성일: 2026-08-23
>
> 발단: bindings 0.12.0 성능 개선 작업 중 DEALER_DEALER/tcp 측정
> (`doc/perf/perf/bindings-0.12.0/log/2026-08-23-cpp-dealer-dealer-ceiling.md`,
> `2026-08-23-cpp-routed-send-improvement.md`)
>
> 상태: 원칙 확정, 최종 설계 확정(2026-08-23, 소유자 결정, §3.2). 구현 변경은
> 별도 사이클에서 진행한다. 스펙(문서)은 이 결정일에 개정했다
> (`bindings/doc/spec/async-coroutine-policy.ko.md`/`.en.md`,
> `bindings/doc/spec/README.ko.md`/`.en.md`).

## 0. 지배 원칙 (소유자 결정)

**Bindings 라이브러리는 순수하게 Core 라이브러리의 래퍼로 동작한다.**
성능 최적화를 위한 캐싱 정도는 허용하지만, 그 이상은 하지 않는다.

- 허용: 값 캐싱, 상태 스냅샷 캐싱, 할당 재사용(풀) 같은 무정책(policy-free) 최적화
- 불허: 자체 스레드, 대기열·스케줄러, 재시도·백프레셔 정책, deadline 관리 —
  이런 정책과 실행 자원의 소유권은 어플리케이션에 있다

## 1. 현재 구조 (원칙 위반)

2026-08-15 커밋 `2fb9ced504` "bindings: align byte HWM and async admission
contracts"가 모든 바인딩에 "async admission" 구조를 도입했다.

- DEALER/ROUTER send의 공개 터미널을 `async()` 전용으로 변경. 그 이전에는
  PAIR와 동일한 동기 `bool submit()`으로 전송했다 (계약에서 동기 터미널이
  이 커밋으로 제거됨).
- C++ 기준, 라이브러리가 자체 스레드를 소유한다:
  routed admission reactor 1, publish admission reactor 1,
  async continuation dispatcher 워커 2 (`routed_admission_state.cpp:753`,
  `publish_admission_state.cpp:29`, `async_continuation_dispatcher.cpp:106`).
- 대기열, deadline 타이머, 재시도 정책이 전부 라이브러리 내부에 있다.

바인딩별 이식 형태: C++·Rust는 전용 OS 스레드(`bindings/rust/src/internal/routed_admission.rs`),
.NET은 공유 스레드풀+Timer(`RoutedAdmissionScheduler.cs`), Java/Node/Go는 각
런타임 방식으로 동일 계약을 구현. 즉 이 구조는 C++만의 문제가 아니라 8/15
계약 자체의 문제다.

## 2. 이 구조가 정당화되지 않는 이유 (실증)

### 2.1 감시는 Core가 이미 한다

Core는 `zlink_routed_send_ready_handler`로 WRITABLE/TERMINAL 이벤트를 push한다
(`core/include/zlink/socket/api.h:66-94`). 바인딩 reactor 스레드는 감시를 하지
않는다 — Core 콜백이 깨워주면 재시도를 실행하고 deadline 만료를 처리할 뿐이다.
스레드의 존재 이유는 "콜백 재진입 회피 + 타이머"라는 구현 편의다.

### 2.2 크레딧(예약) 방식이 아니다

송신 admission은 snapshot 방식이다. Core 헤더가 명시한다:
"The value is a snapshot, **not a reservation**. A later exact submit can
report backpressure ... if the pipe state changed." (`api.h:83`)

WRITABLE 이벤트는 상태 전이 통지일 뿐 송신자 몫의 예약이 아니므로, 깨어나서
재시도해도 다시 거절될 수 있다. 따라서 라이브러리 reactor가 할 수 있는 일은
"깨면 재시도, 실패면 다시 대기" — 어플리케이션이 `EAGAIN`을 받고 writable
이벤트를 기다렸다 재시도하는 것과 의미론적으로 동일하다. **라이브러리 흡수가
앱 대비 추가로 보장하는 것이 없다.** byte-credit 예약이 존재하는 곳은 수신측
decoder(inbound)뿐이다.

### 2.3 백프레셔 가설 실측 기각

- 64B에서 4,782,515건 send 중 HWM park 0건. HWM을 64 MiB로 올려도 C++
  throughput ±2% 불변(C는 +29%). C++는 HWM 도달 전에 송신측 비용으로 제한된다.
- 발생한 park는 전부 1 ms 미만 edge-wake, deadline 만료 0건. wake 유실 없음.

### 2.4 비용과 결함 표면

- 이 기계장치 때문에 HWM에 걸리지 않는 워크로드도 메시지당 admission 부기를
  냈다. rdtsc 분해: `async()` 1,882 ns/msg 중 Core 제출 502 ns, 나머지가 부기.
  계약 내 완화(D2~D5) 후에도 1,079 ns/msg (C는 ~370 ns/msg).
- 도입 8일 만에 발견된 직렬화 결함: 모든 send가 HWM 상태와 무관하게 reactor
  스레드로 우회, 메시지당 스레드 홉 2회로 ~9.8k msg/s 고정 상한
  (수정: `dbad74ddd2`). 이 결함은 이 스레드 구조 없이는 존재할 수 없는 종류다.
- single suite DEALER_DEALER는 이 구조 도입 후 한 번도 측정된 적이 없어
  0.12.0 사이클에서야 발견됐다. Rust 등 형제 바인딩에 동일 결함이 복제되어
  있을 가능성이 있다 (각 바인딩 측정 전 점검 필요).
- perf 파생 문제: 계획서가 public API 일반 경로 측정을 요구하는데 DEALER
  send의 유일한 터미널이 `async()`라서 하네스가 코루틴을 강제로 사용한다
  (`perf_dealer_dealer.cpp:86`). perf 정책(`PERF_POLICY.md`)은 코루틴을
  요구하지 않는다 — 강제는 바인딩 계약에서 온다. 헤더 스스로 "deliberately
  exposes only async()"라고 명시한다 (`operation_contracts.hpp:213`).

## 3. 스펙 판정과 권고 (2026-08-23 확정)

`bindings/doc/spec/async-coroutine-policy.ko.md`를 검토한 결과, **스펙 자체가
이미 지배 원칙과 일치한다**:

- 비동기 완료 표면(언어 관용 suspension 객체) 제공은 바인딩 책임이 맞다.
  C++는 `co_await op.async()`가 canonical이다.
- "bindings 라이브러리는 coroutine executor나 scheduler를 직접 소유하지
  않는다", "coroutine 실행, event loop 연결, handler dispatcher 연결은
  framework가 맡는다"고 명시되어 있다.

따라서 이 이슈는 **계약 결함이 아니라 구현의 스펙 위반**이다:

- `async_continuation_dispatcher`의 워커 스레드 2개는 바인딩이 소유한
  executor/scheduler로, 스펙이 명시적으로 금지한 형태다.
- admission reactor 전용 스레드는 완료 표면 제공에 필수가 아니다. Core가
  WRITABLE/TERMINAL 이벤트를 push하므로 재시도는 그 콜백에서 구동할 수 있다.
- 스펙의 "standalone coroutine은 binding completion thread에서 재개될 수
  있다 / admission retry queue는 binding이 계속 소유한다" 문구가 구현 확대의
  통로가 되었다. 이 문구는 "완료가 발생한 컨텍스트(Core 이벤트 콜백)에서
  재개될 수 있다"로 명확화한다 — 바인딩이 스레드를 만들라는 뜻이 아니다.

권고:

1. **구현을 스펙에 재정렬 (전 바인딩)**: 라이브러리 소유 스레드(reactor,
   dispatcher 워커)를 제거한다. 재시도는 Core 이벤트 콜백에서 구동하고,
   suspension 재개는 완료가 발생한 컨텍스트에서 수행하며, 이후 실행 모델
   연결은 framework/앱의 몫이다. admission retry queue *상태*의 소유는 스펙
   허용 범위이므로 유지하되 최소화한다. 선행 확인: Core 콜백 컨텍스트 내
   send 재진입 계약과 timeout(deadline)의 스레드 없는 처리 방식.
   → **§3.1에서 선행 확인 완료. 재진입이 불가로 판정되어 이 권고는 현
   Core ABI에서 그대로 실행할 수 없다.**
2. **스펙 한 줄 명확화**: 위 "binding completion thread" 문구를 바인딩
   스레드 생성 금지가 명확하도록 수정한다 (ko/en 동시).
3. **동기 터미널은 이번에 다루지 않는다**: 스펙이 managed routed builder의
   blocking terminal을 금지하므로, 복원 여부는 별도 스펙 개정 논의로 남긴다
   (스펙의 "routed builder가 아닌 one-shot 즉시 submit은 제거 범위가 아니다"
   문구가 여지를 남김).
4. **허용되는 최적화의 예**: C1(콜백 상태 캐싱), C3(풀 재사용), D3(target
   상태 캐싱) 같은 무정책 캐싱·재사용은 지배 원칙 범위 안이며 유지한다.

wire protocol과 C API는 변경하지 않는다.

## 3.1 선행 확인 결과 (2026-08-23, Phase 0)

권고 1이 지정한 두 선행 확인 항목을 조사했다. 상세 근거와 실측은
`doc/perf/perf/bindings-0.12.0/log/2026-08-23-cpp-spec-realignment.md`.

### (a) Core 콜백 컨텍스트 내 send 재진입 — **금지 / 구조적으로 불가**

세 근거가 독립적으로 성립한다.

1. **공개 계약이 명시적으로 금지한다.** `core/include/zlink/socket/api.h:166`,
   `zlink_routed_send_ready_handler`: *"The callback must not block or submit
   on the socket. It should only schedule binding-owned work."*
   (공개 스펙 본문은 *blocking* submit만 금지하고 재시도 주체를 "binding
   scheduler"로 지정한다 — 헤더와 강도가 다르며, 이 불일치 자체가 Core 측
   정리 대상이다.)
2. **per-handle send-sequence gate 때문에 실제로 실패한다.**
   `core/src/api/socket/part_helper_api.cpp:433-443`의 gate는 핸들 단위·단일
   소유 스레드이고, 공개 send API가 전부 part 단위라 멀티파트 submit이 gate를
   여러 호출에 걸쳐 점유한다. 실측: caller 스레드 1개만 있어도 콜백 인라인
   재시도의 **69%**, 4개면 **88%**가 `ZLINK_SUBMIT_INVALID_ARGUMENT`(EINVAL)로
   실패한다. EINVAL은 BACKPRESSURED도 TERMINAL도 아니고 WRITABLE은 순수
   edge-triggered(`socket_dispatch_bridge.cpp:131-155`)라 복구 경로가 없다 —
   곧 완료 유실이다.
3. **gate를 피하려면 Core 콜백 스레드를 블로킹해야 한다.** 바인딩은 이미 같은
   이유로 `outbound_record_attempt_mutex`
   (`bindings/cpp/src/Runtime/Sockets/socket_callback_state.hpp:32`)로
   직렬화한다. 콜백이 이 뮤텍스를 잡으면 Core async-mailbox 스레드가 애플리케이션
   스레드에 막히는데, 그 스레드가 곧 credit을 푸는 `process_commands` 주체다.
   Core는 blocking send에 대해 이 위험을 이미 처리해 두었으나
   (`socket_base_msg.cpp:387-397`) Core가 놓아 줄 수 있는 것은 Core 소유 락뿐이다.

단순 구성(송신 스레드 1개, 콜백 인라인 재시도)에서는 데드락 없이 동작한다는 것도
실측으로 확인했다(20만 건 중 park 1,653건, 인라인 해소 1,438건, 오류 0). 즉
문제는 데드락이 아니라 **동시성 하에서의 EINVAL 실패와 복구 불가**다.

### (b) 스레드 없는 timeout — **가능**

Core가 `zlink_timer_*` 공개 API를 제공한다
(`core/include/zlink/eventing/api.h:290-304`). 실행 주체는 Core 소유 프로세스
전역 스레드 `zlink-timer`(`core/src/api/monitoring/timer_scheduler_backend.cpp:145-149`)이며
모든 타이머가 하나의 deadline multimap을 공유하므로 바인딩 타이머 N개가 바인딩
스레드 0개를 쓴다. `repeat_count_=1`이 one-shot deadline이다. 바인딩 선례:
`bindings/python/src/zlink/_runtime/eventing/timer.py`.

기각된 후보: `SNDTIMEO` 위임(타임아웃 콜백이 없고 블로킹 caller의 대기 상한일
뿐), routed 이벤트 lazy deadline 검사 단독(이벤트가 edge-triggered라 만료가
영원히 발화하지 않을 수 있음). 참고로 routed **request**의 reply timeout은 이미
Core 소유다(`zlink-req-time` 스레드 → `ZLINK_REQUEST_TIMED_OUT`).

즉 **deadline 타이머 스레드는 Core 결정과 무관하게 지금 제거할 수 있다.**

### 판정과 후속

권고 1은 (a) 때문에 현 Core ABI에서 실행 불가다. 재시도를 **실행**할 수 있는
컨텍스트는 caller 스레드 또는 바인딩 소유 스레드뿐이다. Core 측 결정 선택지:

1. **원자적 멀티파트 submit API 추가**(파트 배열 1회 호출) — 근거 2·3을 동시에
   없애는 유일한 선택지. 헤더 문구 완화만 추가로 필요하다.
2. **헤더 계약 완화만** — 근거 2·3이 남아 **불충분**하다.
3. **현 구조 유지** — §0 원칙에 예외를 명문화해야 한다.
4. **동기 터미널 복원 재검토**(권고 3) — §2.3~§2.4 실증이 지지하는 방향.

Core 결정과 무관하게 선행 가능한 항목: deadline 타이머의 `zlink_timer_*` 위임,
그리고 `async_continuation_dispatcher` 워커 제거(단, 재개 컨텍스트가 Core 콜백
스레드가 되는 경우 "Core 스레드에서 사용자 코드 실행" 문제가 남아 별도 판단 필요).

## 3.2 최종 설계 확정 (2026-08-23)

§3.1의 판정과 후속 선택지 중 **선택지 4 (동기 터미널 복원 재검토, 권고 3)**를
소유자가 채택했다. §2.3~§2.4의 실측(park 0건, HWM을 64 MiB로 올려도 처리량
불변, admission 기계장치로 인한 메시지당 부기 비용, 8일 만에 드러난 직렬화
결함)이 이 선택을 지지한다. 이로써 §3 권고 3 "동기 터미널은 이번에 다루지
않는다"는 이 개정으로 **대체**된다 — 동기 터미널을 이번에 복원한다.

최종 설계는 다음과 같다.

| 축 | 결정 |
|---|---|
| Routed **send**(DEALER/ROUTER, request 아님) | 동기 `submit()` terminal을 복원한다. Core send를 그대로 감싼다 |
| HWM 대기·재개·timeout | 전부 Core 소유다: blocking 모드는 Core 내부에서 대기하다 Core 신호로 재개, `SNDTIMEO`가 대기 상한, `DONTWAIT`은 즉시 `EAGAIN` |
| 백프레셔 정책 | 어플리케이션이 소유한다 |
| "HWM-managed routed는 async 전용" 규칙 | 폐지 |
| binding-side admission 기계장치(park queue, WRITABLE-callback 재시도, deadline timer, dispatcher thread) | 폐지 |
| Request→reply 비동기 완료 표면 | 유지. Core가 이미 완료를 구동하는 지점(reply handler callback)이 있으므로 그대로 둔다. 재개는 완료가 발생한 컨텍스트에서 일어난다. request timeout은 이미 Core 소유(`ZLINK_REQUEST_TIMED_OUT`) |
| 바인딩 소유 스레드 | 0개. §0 지배 원칙 그대로 |
| 새 C API | 없음. wire protocol과 기존 C API는 변경하지 않는다 |

이 결정은 §3.1(a)가 지적한 "Core 콜백 컨텍스트 내 send 재진입 금지" 문제를
**우회**한다 — 동기 `submit()`은 애초에 콜백 컨텍스트에서 재진입할 필요가 없다.
caller 스레드가 Core 안에서 직접 blocking/SNDTIMEO/DONTWAIT를 수행하므로, per-handle
send-sequence gate로 인한 EINVAL 실패(§3.1(a)2)와 콜백 스레드 블로킹 문제(§3.1(a)3)가
애초에 발생하지 않는다. Core 측 멀티파트 submit API 확장(§3.1 후속 선택지 1)은
더 이상 이 설계의 전제조건이 아니다.

바인딩 스펙 반영은 `bindings/doc/spec/async-coroutine-policy.ko.md`/`.en.md`와
`bindings/doc/spec/README.ko.md`/`.en.md` (`##### Retained credit과 routed 완료` 절)에서
수행했다. 언어별 스펙(`bindings/doc/spec/<lang>/README.*.md`)의 구현 반영은 후속
per-language 작업으로 남긴다.

## 4. 이번 성능 사이클의 처리

- 계약 변경 금지 원칙에 따라 계약 내 완화(D2~D5)까지만 반영한다.
- DEALER_DEALER/tcp 판정은 현 계약의 수치로 기록하되, 이 문서를 근거로
  "계약 구조 기인 비용"임을 행 설명에 남긴다.
- 이후 바인딩(.NET, Java, Node, Go, Rust)의 routed 측정 전에 동일 직렬화
  결함 여부를 먼저 점검한다.
- 이 문서는 진행 시트(`doc/perf/perf/bindings-0.12.0/progress.ko.md`)에서
  링크한다.
