# C++ bindings 스펙 재정렬 — Phase 0 선행 확인 결과 (STOP) — 2026-08-23

> 지배 문서: `doc/plan/cpp-routed-async-contract-issue.ko.md` §0·§3,
> `bindings/doc/spec/async-coroutine-policy.ko.md`(2026-08-23 개정),
> 직전 로그: `log/2026-08-23-cpp-routed-send-improvement.md`(D2~D5)
> 브랜치 / HEAD: `codex/bindings-0.12.0-performance` / `a6aab918cf`
> commit·push 하지 않았다. **코드 변경 없음** — Phase 0에서 중단했다.

## 0. 요약

이슈 문서 §3의 권고 1은 "라이브러리 소유 스레드를 제거하고 **재시도는 Core
이벤트 콜백에서 구동**한다"이며, 그 선행 확인 항목으로 **(a) Core 콜백
컨텍스트 내 send 재진입 계약**과 **(b) 스레드 없는 timeout 처리 방식**을
지정했다.

두 항목을 조사한 결과:

| 항목 | 판정 |
|---|---|
| (a) 콜백 내 send 재진입 | **금지 / 구조적으로 불가** — Core 공개 계약이 명시적으로 금지하고, 금지를 무시하더라도 Core의 per-handle send-sequence gate 때문에 실패한다 |
| (b) 스레드 없는 timeout | **가능** — Core가 `zlink_timer_*` 공개 API(Core 소유 `zlink-timer` 스레드)를 제공한다. 바인딩 타이머 스레드는 제거할 수 있다 |

(a)가 부정이므로 계획서의 지시대로 **Phase 0에서 중단**했다. Phase 1~2(구현·검증)는
수행하지 않았다. 진행하려면 Core 측 계약 결정이 선행되어야 하며 이는 본 작업 범위를
넘는다.

## 1. Phase 0-(a) — 콜백 내 send 재진입 계약

### 1.1 문서 근거: 공개 계약이 명시적으로 금지한다

`core/include/zlink/socket/api.h:156-168`, `zlink_routed_send_ready_handler`:

> The callback **must not block or submit on the socket**. It should only
> schedule binding-owned work. Replacing the handler from this socket's own
> readiness callback fails with errno=EDEADLK.

공개 스펙(`doc/site/docs/spec/core/socket/README.ko.md`)도 같은 취지다:

> Event는 재시도 시점일 뿐 성공 보장이 아니므로 **callback은 blocking submit을
> 실행하지 않고 binding scheduler가 같은 key로 `DONTWAIT` 재시도하게 한다.**

두 문서의 강도는 다르다 — 헤더는 submit 자체를 금지하고, 스펙 본문은 *blocking*
submit을 금지하면서 재시도 주체를 "binding scheduler"로 지정한다. 즉 스펙 본문은
DONTWAIT 재시도를 콜백에서 하는 것을 명시적으로 허용하지도 금지하지도 않는 반면,
헤더는 무조건 금지한다. **이 불일치 자체가 Core 측이 정리해야 할 항목이다.**

### 1.2 실측 1 — 단순 경로에서는 데드락하지 않는다

일회용 프로브(`p0_reentry.c`, DEALER→DEALER/tcp, SNDHWM 4 KiB, 별도 drain 스레드,
wake-version lost-wake 가드 포함). 콜백 안에서 `zlink_dealer_send_transport_pair_part`를
`DONTWAIT`로 호출한다:

```
recvd=195094
main_tid=127841112596224 cb_tid=127841068893888     ← 콜백은 Core async-mailbox 스레드
sent=198562 main_backpressured=1653
cb_calls=1654 cb_submit_ok=1438 cb_submit_bp=0 cb_submit_err=0 wedged=0
wake_version=1654 lost_wake_recovered=215
clean-exit
```

20만 건 중 park 1,653건, 그중 1,438건을 **Core 콜백 안에서 인라인 재시도로 해소**했고
데드락·오류·유실 0건이었다. 즉 단일 송신 스레드 + 콜백이라는 단순 구성에서는 동작한다.

### 1.3 실측 2 — 그러나 caller와 동시에 돌면 무너진다 (결정적)

Core의 part-sequence gate는 **핸들 단위 · 단일 소유 스레드**다
(`core/src/api/socket/part_helper_api.cpp:433-443`):

```cpp
while (state->send.active && state->send.owner_thread != current_thread) {
    if (!aggregate_send_mode_active ()) {
        errno = EINVAL;
        return -1;
    }
    state->cv.wait (lock);
}
```

멀티파트 submit은 첫 part부터 FINAL까지 이 gate를 점유한다. 공개 send API는 전부
part 단위이고(`api.h:341-475`) 배열을 한 번에 넣는 원자적 submit API는 **없다** —
즉 멀티파트 전송은 본질적으로 gate를 여러 호출에 걸쳐 붙잡는다.

프로브 `p0_collide.c`: caller 스레드가 2-part `DONTWAIT` 전송을 반복하고, 콜백이
같은 방식으로 인라인 재시도한다.

| caller 스레드 수 | 콜백 호출 | 콜백 OK | 콜백 BACKPRESSURED | 콜백 **EINVAL** |
|---:|---:|---:|---:|---:|
| 1 | 7,826 | 2,311 | 107 | **5,407 (69%)** |
| 4 | 3,985 | 464 | 1 | **3,519 (88%)** |

**caller 스레드가 단 하나여도 콜백 인라인 재시도의 69%가 `ZLINK_SUBMIT_INVALID_ARGUMENT`
(EINVAL)로 실패한다.** EINVAL은 BACKPRESSURED(재시도 가능)도 TERMINAL(종료)도 아니다.
그리고 §1.5대로 WRITABLE은 edge-triggered라 다음 이벤트가 보장되지 않으므로,
이 실패는 곧 **완료 유실(lost completion)**이다.

`aggregate_send_mode`는 탈출구가 아니다. 내부 전용(공개 헤더에 없음)이고, 켜면
EINVAL 대신 `state->cv.wait(lock)`으로 **블로킹**한다 — 콜백이 절대 해서는 안 되는 동작이다.

### 1.4 왜 바인딩이 스레드로 이 문제를 우회하고 있었는지

바인딩은 이미 같은 gate 때문에 자체 뮤텍스로 직렬화하고 있다
(`bindings/cpp/src/Runtime/Sockets/socket_callback_state.hpp:29-32`, 주석 그대로):

```cpp
// Every binding-owned outbound record on this native socket takes this
// gate only for one complete native submit attempt.  It is deliberately
// released before routed readiness is awaited or user code is resumed.
std::mutex outbound_record_attempt_mutex;
```

따라서 콜백에서 인라인 재시도를 하려면 **이 바인딩 뮤텍스를 잡아야 한다**. 그러면
Core async-mailbox 스레드가 임의의 애플리케이션 스레드가 쥔 뮤텍스에서 블로킹한다.
이는 (i) 헤더의 "must not block" 위반이고, (ii) liveness 위험이다 — 그 스레드가 바로
`process_commands`로 credit을 풀어 주는 스레드이므로, gate를 쥔 채 credit을 기다리는
caller와 서로를 막을 수 있다.

Core는 **blocking send**에 대해서는 이 위험을 이미 인지하고 처리해 두었다
(`core/src/runtime/sockets/common/socket_base_msg.cpp:387-397`):

```cpp
// A mailbox-owned command path re-enters this socket to release byte
// credit. It needs the same retry handoff as a send-ready callback;
// otherwise a blocking public send can prevent the command that makes it
// writable from running.
const bool retry_progress_owner_active =
  send_ready_handler_active () || async_mailbox_owns_commands ();
```

routed handler를 등록하면 async mailbox가 켜지므로(`socket_base_dispatch.cpp:262-272`)
`async_mailbox_owns_commands()`가 참이 되고, Core는 blocking 재시도 중
`public_api_sync`를 놓아 준다. **그러나 Core가 놓아 줄 수 있는 것은 Core 소유 락뿐이며,
바인딩 소유 뮤텍스는 놓아 줄 수 없다.** 여기가 설계가 막히는 지점이다.

### 1.5 부수 확인: WRITABLE은 순수 edge-triggered

`socket_dispatch_bridge.cpp:131-155`가 같은 key의 pending WRITABLE을 coalesce하고,
TERMINAL은 identity당 정확히 1회다. 주기적 재통지·타이머·re-arm이 없다. 상태 변화가
없으면 바인딩은 **다시 호출되지 않는다.** 따라서 "실패하면 다음 이벤트에 다시 하면 된다"는
복구 전략은 성립하지 않는다.

### 1.6 (a) 판정

**콜백 내 send 재진입은 금지이며, 금지를 무시해도 현 Core ABI에서는 동작하지 않는다.**
근거 세 가지가 독립적으로 성립한다:

1. 공개 헤더가 명시적으로 금지한다 (`api.h:166`).
2. per-handle send-sequence gate 때문에 caller와 동시 실행 시 대부분 EINVAL로 실패하고,
   WRITABLE이 edge-triggered라 복구 경로가 없다 (실측 69~88%).
3. gate를 피하려면 바인딩 뮤텍스를 잡아야 하는데, 그것은 Core 콜백 스레드를 블로킹하는
   것이며 헤더 위반이자 liveness 위험이다.

## 2. Phase 0-(b) — 스레드 없는 timeout 처리

이쪽은 **긍정**이다. Core는 바인딩이 스레드를 만들지 않고 쓸 수 있는 시간 기반 콜백을
공개 API로 제공한다 (`core/include/zlink/eventing/api.h:290-304`):

```c
typedef void (*zlink_timer_handler_fn) (void *timer_, uint64_t fire_count_, void *userdata_);
ZLINK_EXPORT void *zlink_timer_new (void);
ZLINK_EXPORT zlink_config_result_t zlink_timer_start (void *timer_, uint64_t interval_ns_,
                                                      uint64_t repeat_count_);
ZLINK_EXPORT zlink_handler_result_t zlink_timer_handler (void *timer_,
                                                         zlink_timer_handler_fn handler_,
                                                         void *userdata_);
```

- 실행 주체는 **Core 소유 프로세스 전역 스레드** `zlink-timer`
  (`core/src/api/monitoring/timer_scheduler_backend.cpp:145-149`). 모든 타이머가 하나의
  deadline multimap을 공유하므로(`:83`) 바인딩 타이머 N개가 바인딩 스레드 0개를 쓴다.
- `repeat_count_ = 1`이 순수 one-shot deadline이다.
- 소켓 I/O 스레드가 아니다 — `timer_api.cpp:66` 주석: *"Generic timers stay on the
  timer scheduler backend and do not run on socket I/O threads."*
- 바인딩 선례가 이미 있다: `bindings/python/src/zlink/_runtime/eventing/timer.py`.

평가한 다른 후보는 모두 기각됐다:

- **(a) `SNDTIMEO` 위임 — 불가.** 타임아웃 콜백이 없고, 이미 블로킹한 caller 스레드의
  대기 상한일 뿐이다. 만료 시 `errno=EAGAIN` 반환뿐
  (`socket_base_msg.cpp:385-434`). DONTWAIT 경로에서는 아예 관여하지 않는다.
- **(b) routed 이벤트에 lazy deadline 검사 — 불가 단독.** §1.5대로 이벤트가 순수
  edge-triggered라 "다음 이벤트"가 오지 않을 수 있다. 타임아웃이 영원히 발화하지 않는
  워크로드가 존재한다.
- **참고:** routed **request**(reply 대기)의 timeout은 이미 Core 소유다.
  `zlink_dealer_request_part(..., timeout_ms_, handler_, ...)`가 Core의 detached
  `zlink-req-time` 스레드에 등록되고, 만료 시 `ZLINK_REQUEST_TIMED_OUT`으로 reply
  handler가 호출된다(`request_timeout_scheduler_internal.cpp:86-89,186`,
  `socket_request_reply_internal.cpp:140-150`). 바인딩이 request 쪽에 자체 reaper를
  둘 이유는 없다.

### 2.1 (b) 판정과 계약 함의

**바인딩의 deadline 타이머 스레드는 `zlink_timer_*` 위임으로 제거 가능하다.** 문서화된
timeout 계약(만료 시점에 ETIMEDOUT으로 완료)은 그대로 지킬 수 있으며, "다음 이벤트나
caller 상호작용까지 미뤄진다"는 열화 fallback을 받아들일 필요가 없다.

단, 이것은 **timeout 발화**만 해결한다. 타임아웃된 operation을 완료 처리하는 것은 Core
submit이 필요 없으므로(코루틴 재개뿐) `zlink-timer` 스레드에서 수행해도 gate 문제가
없다 — 개정된 스펙의 "완료가 발생한 컨텍스트에서 재개"에 부합한다. 반면 **재시도 실행**은
§1의 이유로 이 스레드에서도 할 수 없다(전역 공유 스레드를 블로킹하면 프로세스 내 모든
타이머가 멈춘다).

## 3. 남은 설계 공간 (Core 측 결정 입력)

현 Core ABI에서 routed admission 재시도를 **실행**할 수 있는 컨텍스트는 두 가지뿐이다:
caller 스레드, 또는 바인딩 소유 스레드. Core 콜백 스레드는 §1로 배제된다.

Core 측 결정이 필요한 선택지:

1. **원자적 멀티파트 submit API 추가** — 예: 파트 배열을 한 번의 호출로 제출하는
   `zlink_dealer_send_transport_pair_message_array(...)`. 이러면 per-handle
   send-sequence gate를 여러 호출에 걸쳐 점유할 일이 없어지고, 콜백이 바인딩 뮤텍스
   없이 논블로킹으로 재시도할 수 있다. **§1의 세 근거 중 2·3을 동시에 없애는 유일한
   선택지**이며, 헤더 문구 완화(1)만 추가로 필요하다.
2. **헤더 계약 완화만** — `api.h:166`을 "must not block; a single DONTWAIT exact submit
   is permitted"로 바꾼다. 그러나 gate 문제(근거 2·3)가 남으므로 **이것만으로는 부족하다.**
3. **현 구조 유지** — 재시도 실행을 바인딩 소유 스레드에 남긴다. 지배 원칙 §0("자체 스레드
   불허")과 정면으로 충돌하므로, 원칙에 예외를 명문화하거나 §4의 방향을 택해야 한다.
4. **동기 터미널 복원 재검토** — 이슈 문서 §3 권고 3이 별도 논의로 남긴 항목. routed send의
   유일한 터미널이 `async()`가 아니게 되면 admission 기계장치 자체의 필요성이 줄어든다.
   §2.2~§2.4의 실증(park 400만 건 중 0~19건, 부기가 비용의 58%)이 이 방향을 지지한다.

부분 적용 가능한 항목(Core 결정과 무관하게 지금 할 수 있는 것):

- **`async_continuation_dispatcher` 워커 2개 제거** — 이것은 재시도 실행과 무관한 순수
  completion executor로, 스펙이 명시적으로 금지한 형태다(이슈 문서 §3). 완료가 발생한
  컨텍스트에서 재개하도록 바꾸면 §1의 제약과 충돌하지 않는다. **다만 재개된 코루틴이
  사용자 코드로 진입하므로, 그 컨텍스트가 Core 콜백 스레드일 때는 결국 §1과 같은
  "Core 스레드에서 임의 사용자 코드 실행" 문제가 된다** — 별도 판단이 필요하다.
- **deadline 타이머의 `zlink_timer_*` 위임** — §2. 단독으로 적용 가능하다.

## 4. 상태

- 코드 변경 없음. `bindings/cpp/src`의 `std::thread` 5개소는 그대로다
  (`routed_admission_state.cpp:155,753`, `publish_admission_state.cpp:29,83`,
  `async_continuation_dispatcher.cpp:106`).
- 일회용 프로브 3개(`p0_reentry.c`, `p0_contend.c`, `p0_collide.c`)는 scratchpad에만
  있고 저장소에 넣지 않았다.
- contract 테스트·sanitizer·perf 재측정은 수행하지 않았다 (변경이 없으므로 무의미).
- commit·push 하지 않았다.
