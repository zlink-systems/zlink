# Core send-completion 통지 계약과 terminal 표면 정규화 설계

> 작성일: 2026-08-23
> 브랜치: `codex/bindings-0.12.0-performance`
> 상태: **설계 문서 (구현 없음)**. 코드 변경·커밋 없음.
>
> 선행 문서
> - `doc/plan/cpp-routed-async-contract-issue.ko.md` (§0 지배 원칙, §3.1 Phase 0, §3.2)
> - `doc/perf/perf/bindings-0.12.0/log/2026-08-23-cpp-spec-realignment.md`
> - `bindings/doc/spec/async-coroutine-policy.ko.md` (2026-08-23 개정본)
>
> **검증 표기 규칙** — 이 문서는 두 종류의 문장을 섞는다.
> `[확인]`은 이 사이클에서 코드를 직접 읽어 확인한 사실이고 파일:라인을 붙인다.
> `[제안]`은 이 문서가 새로 제안하는 설계이며 아직 코드에 존재하지 않는다.
> 시그니처는 전부 **초안**이며 이름·인자·타입은 구현 사이클에서 바뀔 수 있다.

---

## 1. 배경

### 1.1 지금까지의 경로

1. 2026-08-15 `2fb9ced504`가 모든 바인딩에 "async admission" 구조를 도입했다.
   HWM-managed routed send의 유일한 terminal이 `async()`가 되고, 바인딩이
   park queue·deadline timer·retry reactor·dispatcher thread를 소유하게 됐다.
2. 0.12.0 성능 사이클에서 이 구조가 §0 지배 원칙("바인딩은 순수 래퍼, 자체
   스레드·대기열·재시도 정책 불허") 위반이자 실측상 비용만 내는 구조임이
   드러났다 (`cpp-routed-async-contract-issue.ko.md` §2.3~§2.4).
3. Phase 0 선행 확인에서 "재시도를 Core 콜백에서 구동한다"는 대안이
   **구조적으로 불가**로 판정됐다 (§3.1(a)). 근거 셋:
   헤더가 콜백 내 submit을 금지, per-handle send-sequence gate 때문에 동시
   실행 시 69~88%가 `EINVAL`, gate 회피는 Core 콜백 스레드 블로킹을 요구.
4. §3.2에서 소유자가 **동기 terminal 복원**을 채택했고 C++는 이미 반영됐다.

### 1.2 이번 결정이 해결하는 잔여 문제

§3.2는 문제를 우회했을 뿐 두 가지를 남겼다.

- **비동기 완료 표면이 routed send에서 사라졌다.** 코루틴/`Task`/`Future`
  관용을 쓰는 앱은 HWM 대기 동안 caller 스레드를 블로킹하거나 `DONTWAIT` +
  자체 재시도 루프를 직접 짜야 한다. 정책 소유는 앱에 맞지만, *실행 자원*까지
  앱이 만들어야 한다는 뜻은 아니다.
- **`send_ready` 계열 "readiness hint" 표면이 그대로 남아 있다.** 이 표면의
  계약은 "재시도해 볼 가치가 있다는 신호일 뿐 성공 보장이 아니다"
  (`core/include/zlink/socket/api.h:82-84,148-150,163-166` [확인])이며,
  8/15 구조가 이 hint 위에 세워졌다가 실패했다. hint가 남아 있는 한 같은
  구조가 다시 자란다.

이 문서는 그 둘을 **Core 소유 async admission + completion 통지**로 한 번에
정리하는 설계다.

---

## 2. 결정사항 (고정, 소유자 결정)

이 절의 항목은 설계 입력이며 이 문서가 재논의하지 않는다.

| # | 결정 |
|---|---|
| D1 | `send_ready` **"readiness hint" 의미론을 전 표면에서 폐지**한다. routed handler의 WRITABLE/TERMINAL, plain `zlink_send_ready_handler`, 그리고 recv 가능한 형태의 어떤 hint 이벤트도 포함한다. 어떤 소비자도 다시는 "hint 받고 재시도"를 하지 않는다 |
| D2 | 대체물은 **Core 소유 async send admission + COMPLETION 의미론**이다. Core가 pending send의 소유권을 가져가고(진짜 예약), 공간이 생기면 admit하거나 terminal errno/timeout으로 실패시키고, **완료를 통지**한다. 기계장치는 기존 blocking-send 내부 대기와 동일하되 출구가 condvar 대신 통지다 |
| D3 | 완료 통지는 **의미론이 동일한 두 디스패치 위치** 중 소비자가 고른다: (a) async-mailbox 스레드에서 도는 등록 콜백(기본), (b) `ZLINK_POLLCOMPLETION` 등록으로 poller-owned가 된 같은 콜백(poll 루프 소비자용). 중복이 아니라 선택지다 — 이벤트 종류나 API가 두 개가 아니라 같은 콜백의 디스패치 소유권이 다를 뿐이다 (Q10, §13 해결) |
| D4 | **Sync send는 그대로 둔다** (blocking / `SNDTIMEO` / `DONTWAIT`, 전부 Core 소유) |
| D5 | **바인딩은 스레드를 0개 소유한다** (순수 래퍼 원칙). awaitable terminal은 Core completion 콜백에서 완료된다. 금지됐던 것은 *콜백에서 submit하는 것*이었고, 재시도가 사라졌으므로 더 이상 발생하지 않는다 |
| D6 | 규범 terminal 표면: C++ send/publish = `submit()` 블로킹 + `async()` 코루틴 / C++ request = `submit()` 블로킹 반환 + `submit(callback)` + `async()` / .NET = HWM-managed op는 단일 `Async()` / Java·Kotlin·Node·Python·Rust = 단일 `submit()`가 awaitable 반환 / **Go = `Submit(ctx) -> error` 동기 형태** / raw reply는 전 언어 동기 `submit()`. **"suspension terminal 옆에 callback terminal을 두지 않는다"는 기존 규칙은 C++ request에 한해 폐기**한다 |
| D7 | **명명: 대체 계열의 이름은 `send_complete`다.** 기존 두 핸들러(`zlink_send_ready_handler`, `zlink_routed_send_ready_handler`)는 **하나의 계열로 병합**한다 — 완료 payload가 항상 target identity를 싣기 때문에 routed 전용 표면을 따로 둘 이유가 없다. 등록 API는 `zlink_send_complete_handler(socket, handler_fn, userdata)` 하나뿐이고, payload는 `zlink_send_complete_event_t`, 결과 enum은 `ZLINK_SEND_ADMITTED` / `ZLINK_SEND_TIMED_OUT` / `ZLINK_SEND_TERMINAL`이다. "수신 채널"은 별도 이벤트 종류나 recv 함수가 아니라 기존 `ZLINK_POLLCOMPLETION` 등록을 통한 디스패치 위치 이전이다 (Q10, §13 해결 — §5 참고) |
| D8 | **완료 = Core 큐로의 admission이지 peer 전달이 아니다.** 헤더 문서가 이 점을 명시해야 한다 |
| D9 | **버전은 0.13.0으로 올린다.** `VERSION`, core `CMakeLists`, `zlink.h`, 전 바인딩 매니페스트를 0.13.0으로 옮기고, GitHub Actions로 릴리스한다 (`core/v0.13.0` 태그 + `build.yml` dispatch — 0.12.0과 동일 절차) |
| D10 | 언어 관용 우선(language-idiom-first)이 상시 정책이다. Go의 goroutine 블로킹이 Go식 await이고 `ctx`가 취소·deadline을 소유하므로 Go send terminal은 동기 형태를 유지한다 (D6) |

---

## 3. 설계의 물리적 근거 (코드 확인 결과)

설계를 쓰기 전에 "Core가 정말 pending을 소유하고 완료를 구동할 수 있는가"를
확인했다. **가능하다.** 필요한 실행 컨텍스트·큐·타이머가 이미 전부 Core 안에 있다.

### 3.1 Core는 이미 소켓별 비동기 실행 컨텍스트를 가진다 [확인]

`socket_base_t::process_async_mailbox()`
(`core/src/runtime/sockets/common/socket_base_lifecycle.cpp:410-440`)가
`process_commands` → `dispatch_routed_send_ready_events` →
completion drain 순으로 도는 루프이고, 이것은
`start_async_mailbox_processing(io_thread_t*)`
(`socket_base_lifecycle.cpp:227-261`,
`socket_lifecycle_runtime.cpp:274`)로 **Core 소유 I/O 스레드**에 붙는다.
send-ready handler나 routed handler를 등록하면 이 모드가 켜진다
(`socket_base_dispatch.cpp:229-231,264-266`).

즉 **pending send를 admit할 스레드는 이미 있다.** 새 스레드를 만들 필요가 없다.

### 3.2 blocking send의 대기 논리는 그대로 재사용 가능하다 [확인]

`socket_base_t::send`의 blocking 경로
(`core/src/runtime/sockets/common/socket_base_msg.cpp:385-434`)는

```cpp
while (true) {
    rc = process_commands (timeout, false);
    ...
    rc = target_rid_ ? xsend_routed (...) : xsend_pipe (...);
    if (rc == 0) { clear_send_recovery_pending (); break; }
    ...
    mark_send_recovery_pending ();  mailbox->signal ();
    if (timeout > 0) { timeout = end - now; if (timeout <= 0) { errno = EAGAIN; return -1; } }
}
```

이다. 구조가 (i) 재시도 대상 보관, (ii) writable 신호 대기, (iii) 재시도,
(iv) deadline 검사로 이미 나뉘어 있다. **D2가 요구하는 것은 이 루프의
"caller 스레드가 돈다"를 "async mailbox 스레드가 돈다"로, "return -1"을
"completion 통지"로 바꾸는 것**이다. 새 알고리즘이 아니다.

같은 함수가 blocking 중 Core 소유 락을 놓아 주는 처리도 이미 갖고 있다
(`socket_base_msg.cpp:387-397`, `retry_progress_owner_active`). async 경로는
caller 스레드를 아예 붙잡지 않으므로 이 위험 자체가 사라진다.

### 3.3 "part 단위 API" 위험은 배열 제출로만 없앨 수 있다 [확인]

per-handle send-sequence gate
(`core/src/api/socket/part_helper_api.cpp:433-443`)는 핸들 단위·단일 소유
스레드다. 공개 send API가 전부 part 단위이므로 멀티파트 전송은 gate를
여러 호출에 걸쳐 점유한다 — Phase 0가 실측한 EINVAL 69~88%의 원인이다.

**Core가 pending을 소유하려면 애초에 "완전한 레코드"를 한 번에 받아야 한다.**
다행히 배열 규약은 공개 API에 이미 존재한다 [확인]:
`zlink_socket_msg_handler_fn(..., zlink_msg_t *parts_, size_t part_count_, ...)`
(`core/include/zlink/socket/api.h:51-54`),
`zlink_reply_handler_fn` (`:95-98`),
`zlink_multipart_close(zlink_msg_t *parts, size_t part_count)`
(`core/include/zlink/message/api.h:119`).
따라서 배열 제출 API는 새로운 개념이 아니라 **이미 있는 수신측 규약을
송신측에 대칭으로 놓는 것**이다.

### 3.4 이중 채널(콜백/수신)은 Core에 두 개의 선례가 있다 [확인]

| 선례 | 콜백 채널 | 수신 채널 | poller 통합 |
|---|---|---|---|
| Timer | `zlink_timer_handler()` | `zlink_timer_recv()` | `zlink_poller_add_timer()` (`ZLINK_POLLER_SOURCE_TIMER`) |
| Socket monitor | `zlink_socket_monitor_handler()` | `zlink_socket_monitor_recv()` | 별도 핸들 |

(`core/include/zlink/eventing/api.h:290-304`, `:210-213`,
`core/include/zlink_enum.h:291-296`)

**D3의 "동일 의미론 2채널"은 Core의 기존 관용구다.** 새 패턴을 발명하지 않는다.
게다가 monitor는 두 채널을 **상호 배타**로 두고 두 번째 등록에 `EBUSY`를
돌려준다 (`core/doc/spec/core/07-monitoring.en.md:295-296` [확인]) — §5.2가
제안하는 상호 배타 규칙도 이미 있는 계약이다.

### 3.5 "poll 소비자가 자기 스레드에서 완료를 드레인한다"도 이미 있다 [확인]

`ZLINK_POLLCOMPLETION` (`core/include/zlink_enum.h:319`)이 정확히 그 계약이다.

- DEALER/ROUTER에만 등록 가능하고(`core/src/api/core/zlink.cpp:249-253`),
  등록 시 `acquire_completion_poller()`로 **completion 처리 소유권을 poller가
  가져간다**(`zlink.cpp:262-266`).
- 소유권이 poller에게 있는 동안 async mailbox 루프는 드레인을 건너뛴다
  (`socket_base_lifecycle.cpp:417-425`, `_completion_poller_refs == 0` 검사).
- poll한 스레드가 `completion_drain_scope_t` 안에서 reply handler를 실행한다
  (`socket_base_api.cpp:479-492`).

**이것이 send completion 이벤트 채널의 정확한 템플릿이다** (→ §5).

### 3.6 Core 소유 one-shot deadline 기계장치가 둘 있다 [확인]

- `zlink-req-time`: request reply timeout 전용, lazy-start + idle-exit,
  프로세스 전역 1스레드 (`core/src/api/socket/request_timeout_scheduler_internal.cpp:86-89,175-197`).
- `zlink-timer`: 공개 `zlink_timer_*` 백엔드, 전역 deadline multimap
  (`core/src/api/monitoring/timer_scheduler_backend.cpp:145-149`).

pending send의 timeout에 새 스레드가 필요 없다.

---

## 4. C API 설계 (초안)

### 4.1 형태의 선택: "완전한 레코드 인계"

**[제안]** 앱/바인딩은 part를 자기 쪽에서 모아 **완전한 레코드를 한 번의 호출로
Core에 인계**한다. 인계가 수락되면 그 순간 part 소유권은 Core로 넘어간다.

이 형태를 고른 이유:

1. §3.3의 gate 위험을 **구조적으로** 제거한다. 한 호출 안에서 gate를 잡고
   놓으므로 다중 호출 점유가 없다. Phase 0의 EINVAL 실패 모드가 존재할 수 없다.
2. Core가 "예약"을 하려면 예약 대상의 크기를 알아야 한다. byte HWM 회계는
   레코드 전체 바이트 기준이므로 part 스트리밍으로는 예약이 불가능하다.
   (`api.h:80-84` [확인] "The value is a snapshot, **not a reservation**"이
   지금 예약이 아닌 이유가 정확히 이것이다.)
3. 부분 제출 상태의 취소·close·timeout 처리라는 계약 구멍이 사라진다.

**대안(기각): part 누적 API + `commit()`.** gate 점유 시간이 앱 코드 구간까지
늘어나고, 미완성 레코드의 close/term 처리 계약이 필요하며, Phase 0가 확인한
"멀티파트가 gate를 여러 호출에 걸쳐 붙잡는다"가 그대로 남는다.

### 4.2 제출 API — 초안

**계열 이름은 `send_complete`로 고정(D7)**이고, 옛 두 핸들러는 **하나의 계열로
병합**된다. 병합이 성립하는 이유: 완료 이벤트가 **항상** target identity
(`peer_rid` + `transport_pair_id` + `generation`)를 싣기 때문에, routed 소켓용
표면과 plain 소켓용 표면을 나눌 근거가 사라진다. plain 소켓(PAIR/PUB/XPUB/STREAM)의
완료는 같은 필드를 zero 또는 해당 pipe 값으로 채우면 된다.

```c
/* ── 초안 (이름 계열 send_complete는 확정, 필드 형태는 초안) ──────── */

/** 완료 결과. 성공 또는 최종 실패만 전달한다. 재시도 지시는 없다. */
typedef enum zlink_send_complete_result_t
{
    /* Core 송신 큐로의 admission 성공. peer 전달을 뜻하지 않는다. */
    ZLINK_SEND_ADMITTED = 0,
    /* op deadline 만료 */
    ZLINK_SEND_TIMED_OUT = 201,
    /* 최종 실패: 경로 종료, 취소, socket close, ctx term. errno가 사유다 */
    ZLINK_SEND_TERMINAL = 202
} zlink_send_complete_result_t;

/** Core가 pending에 부여하는 프로세스-소켓 로컬 식별자. 0은 무효값. */
typedef uint64_t zlink_send_op_id_t;

/**
 * @brief 송신 완료 이벤트. 콜백 채널과 수신 채널이 이 struct를 공유한다.
 *
 * result == ZLINK_SEND_ADMITTED 는 **Core 송신 큐로의 admission**을 뜻하며
 * peer가 메시지를 받았다는 뜻이 아니다. 전달 확인이 필요하면 request/reply를
 * 쓴다.
 */
typedef struct zlink_send_complete_event_t
{
    /* ── operation identity ── */
    zlink_send_op_id_t op_id;   /* Core 부여. 취소·상관에 사용 */
    void *userdata;             /* 제출 시 넘긴 값 그대로 */

    /* ── target identity (항상 채워진다) ── */
    zlink_routing_id_t peer_rid;
    uint64_t transport_pair_id;
    uint64_t transport_pair_generation;

    /* ── 결과 ── */
    zlink_send_complete_result_t result;
    int terminal_errno;         /* result != ADMITTED 일 때 사유
                                 * ECANCELED=취소/close, ETERM=ctx term,
                                 * 그 외=경로 종료 사유 */
} zlink_send_complete_event_t;

typedef void (*zlink_send_complete_handler_fn) (
  void *subject_, const zlink_send_complete_event_t *event_, void *userdata_);

/** 제출 옵션. 구조체 확장으로 ABI를 지키기 위해 struct_size를 둔다. */
typedef struct zlink_send_async_options_t
{
    uint32_t struct_size;      /* sizeof(zlink_send_async_options_t) */
    uint32_t timeout_ms;       /* 0 = 무제한. Core 소유 deadline */
    void *userdata;            /* 완료 시 그대로 되돌려준다 */
    const zlink_routed_submit_target_t *target; /* routed면 지정, 아니면 NULL */
} zlink_send_async_options_t;

/**
 * @brief 완전한 멀티파트 레코드 하나의 소유권을 Core에 인계한다.
 *
 * 성공(ZLINK_SUBMIT_OK) 시 parts_[0..count) 전부의 소유권이 Core로 넘어가고
 * 호출자는 이후 각 msg를 만지지 않는다(close 포함). 실패 시 소유권은
 * 호출자에게 남는다.
 *
 * 이 호출은 절대 블로킹하지 않는다. HWM이 막혀 있으면 즉시 OK를 돌려주고
 * pending으로 예약된다 - 완료는 통지로 온다.
 *
 * 완료 통지는 즉시 admit된 경우에도 반드시 정확히 한 번 발생한다.
 * ZLINK_SEND_ADMITTED 는 Core 송신 큐로의 admission이며 peer 전달이 아니다.
 */
ZLINK_EXPORT zlink_submit_result_t zlink_send_async (
  void *s_,
  zlink_msg_t *parts_, size_t part_count_,
  const zlink_send_async_options_t *options_,
  zlink_send_op_id_t *op_id_out_);

/** @brief PUB/XPUB 토픽 발행의 async 형태. 완료 계약은 동일하다. */
ZLINK_EXPORT zlink_submit_result_t zlink_publish_async (
  void *subject_, const char *topic_id_,
  zlink_msg_t *parts_, size_t part_count_,
  const zlink_send_async_options_t *options_,
  zlink_send_op_id_t *op_id_out_);

/**
 * @brief 완료 콜백 채널을 설치/교체한다 (D3-a). replace-only.
 *
 * 이 하나가 옛 zlink_send_ready_handler 와 zlink_routed_send_ready_handler
 * 를 모두 대체한다 - 완료 이벤트가 항상 target identity를 싣기 때문이다.
 */
ZLINK_EXPORT zlink_handler_result_t zlink_send_complete_handler (
  void *s_, zlink_send_complete_handler_fn handler_, void *userdata_);

/** @brief 아직 완료되지 않은 pending의 취소를 요청한다. */
ZLINK_EXPORT zlink_submit_result_t zlink_send_async_cancel (
  void *s_, zlink_send_op_id_t op_id_);
```

**결과 enum이 3값으로 줄어든 것에 대해 (D7)**: 취소(`CANCELED`)와 종료
(`TERMINATED`)를 별도 결과값으로 두지 않고 `ZLINK_SEND_TERMINAL` +
`terminal_errno`(`ECANCELED` / `ETERM`)로 구분한다. 이유는 (i) 소비자 관점에서
셋 다 "이 op는 admit되지 않았고 다시 시도해야 한다"로 동일하고, (ii) errno 축이
이미 Core 전반의 사유 표현 방식이며, (iii) 바인딩이 매핑해야 할 값이 줄어든다.
**3값 enum + `terminal_errno`를 그대로 확정한다 (Q9, §13 해결)** — 근거는 request
결과 판별에 이미 쓰이는 것과 같은 형태(값 축소 + errno 축 분리)라는 선례
일치다.

### 4.3 operation identity — id **와** userdata 둘 다

**[제안]** `zlink_send_op_id_t`(Core 부여, 소켓 로컬 단조 증가)와
`void *userdata`(호출자 부여)를 **둘 다** 전달한다.

- `op_id`는 **취소에 필요**하다. 취소는 Core에게 "이 pending"을 가리켜야
  하는데 포인터를 키로 쓰면 재사용/ABA 위험이 있다. 단조 증가 id가 안전하다.
- `userdata`는 **바인딩 상관에 필요**하다. 바인딩은 suspension 상태 객체
  포인터(또는 handle table 슬롯)를 바로 되돌려받는 편이 해시 조회보다 싸다.
- poller-owned dispatch(§5, `ZLINK_POLLCOMPLETION` 등록)에서는 `op_id`가
  특히 중요하다. 콜백은 등록 시점의 `userdata` 하나만 받으므로, op별로
  다른 상태와 상관하려면 이벤트 자신이 `op_id`로 자기를 식별해야 한다.

`zlink_send_op_id_t`는 `zlink_request_seq`와 같은 부류다. 0을 무효값으로 예약해
"제출 실패 시 out 파라미터가 0"이 명확하게 판정된다.

### 4.4 순서 보장 — target별 FIFO

**[제안]**

- **동일 target(= 동일 `peer_rid` + `transport_pair_id` + `generation`)에
  대한 pending은 제출 순서대로 admit되고 그 순서대로 완료 통지된다.**
  즉 admit은 target별 FIFO다. head-of-line은 의도된 동작이다 — 순서가 뒤집히면
  같은 논리 스트림이 wire에서 재배열된다.
- **서로 다른 target 사이에는 순서 보장이 없다.** 각 target이 독립적으로
  풀리기 때문이다.
- **동기 send와 async pending 사이에 별도 규칙은 없다 (Q1, §13 해결).**
  동기냐 비동기냐에 상관없이 걸리는 것은 **같은 HWM 하나**다. Core는 공간이
  생기면 그 target에 쌓인 pending을 제출 순서대로(FIFO) admit하고, 동기
  `submit()`은 그 순간 **같은 target의 다른 생산자와 동등하게 경쟁**한다 —
  동기 submit을 위해 pending 큐를 우회시키거나 `INVALID_STATE`로 거절하는
  특례는 두지 않는다. **target별 순서 보장은 async pending 사이에서만
  성립한다**; 동기 submit이 그 순서에 끼어드는 것은 함정이 아니라 "같은
  HWM을 같이 쓰는 생산자"라는 모델의 당연한 결과이며, 이 사실만 문서화한다.
- **실패 통지 순서**: TERMINAL/TERMINATED가 발생하면 그 target의 pending
  전체를 **제출 순서대로** 실패 통지한다.

DEALER 무-target 제출(`options->target == NULL`)은 제출 시점에
`zlink_select_routed_submit_target`과 동등한 선택을 **Core 내부에서 확정**하고
그 target에 FIFO로 걸린다. 선택을 완료 시점까지 미루면 순서 보장이 불가능하다.

### 4.5 취소

**[제안]** `zlink_send_async_cancel(s, op_id)`는 **요청**이다. 반환값 의미:

| 반환 | 뜻 |
|---|---|
| `ZLINK_SUBMIT_OK` | 취소를 접수했다. 완료 통지가 `CANCELED`로 온다 |
| `ZLINK_SUBMIT_NOT_FOUND` | 그 id의 pending이 없다 — 이미 완료됐거나 없던 id다 |
| `ZLINK_SUBMIT_INVALID_STATE` | admit이 이미 커밋되어 되돌릴 수 없다. 완료는 `OK`로 온다 |

**취소해도 완료 통지는 반드시 한 번 발생한다.** "취소했으니 통지 없음"은
바인딩 suspension을 영원히 매달리게 만든다. Rust `Future` drop, C++
`async_result_t` drop이 취소를 요청하는 현행 계약
(`bindings/doc/spec/async-coroutine-policy.ko.md` C++/Rust 행 [확인])과
맞물리려면 이 규칙이 필수다.

취소된 레코드의 part는 Core가 close한다 (소유권이 이미 넘어갔으므로).

### 4.6 close / ctx term 시 pending 처리 — **fail-fast**

**[제안]** drain이 아니라 **fail**이다.

- `zlink_close(s)`: 접수 시점에 그 소켓의 모든 pending을
  `ZLINK_SEND_TERMINAL`(errno `ECANCELED`)로 완료 통지하고
  part를 close한다. 통지는 close가 반환하기 전에 전부 나간다.
- `zlink_ctx_term`: 동일하되 errno가 `ETERM`이다.
- **`LINGER`는 pending에 적용되지 않는다.** LINGER는 이미 파이프에 admit된
  바이트의 전송 대기 시간이고, pending은 아직 admit되지 않았다. 두 개념을
  섞으면 close가 무한정 늘어난다.

이 선택의 근거 [확인]: Core는 이미 close/term에서 routed TERMINAL을 전부
enqueue하고 dispatch하는 정확히 같은 형태의 처리를 갖고 있다 —
`enqueue_all_routed_send_terminals(_ctx_terminated ? ETERM : ECANCELED)` +
`dispatch_routed_send_ready_events(true)`
(`core/src/runtime/sockets/common/socket_base_lifecycle.cpp:65-66`). 새 개념이 아니라
같은 종결 경로에 pending 실패를 얹는 것이다.

또한 `zlink_close`는 in-flight 콜백이 있으면 `EBUSY`이고, send-ready/monitor
콜백 안에서의 self-close는 콜백 epilogue로 지연된다는 계약이 이미 있다
(`core/include/zlink/socket/api.h:466-479` [확인]). completion 콜백도 같은
계약을 따른다.

**close-drain 옵트인은 두지 않는다 (Q2, §13 해결).** `ZLINK_OPT_SEND_PENDING_LINGER_MS`
류의 옵션은 추가하지 않고 **fail-fast만**을 유일한 계약으로 둔다. 근거는
선례 일치다: `socket_base_lifecycle`의 close/term 경로가 이미 이 fail-fast
형태이고(§4.6 본문 및 `enqueue_all_routed_send_terminals`), `LINGER`는 이미
admit된 바이트에만 적용된다는 기존 계약과 정확히 대칭이다. 옵트인을 열면
"pending도 LINGER 대상인가"라는 두 번째 계약축이 생기고 close 종료 시점이
다시 상대 소비 속도에 종속되는 문제가 재도입되므로, 옵트인 없이 fail-fast
하나로 고정한다.

### 4.7 pending 큐 자체의 백프레셔 — **반드시 bounded**

**[제안]** pending 큐는 **소켓당 bounded**다. unbounded pending은 HWM을
우회하는 무한 버퍼이며, 그 자체가 D2가 없애려는 문제를 다른 층에 재현한다.

- 새 옵션 **`ZLINK_OPT_SEND_PENDING_MAX`** [제안] (다음 빈 번호 `0x303A`;
  현재 최대값이 `ZLINK_OPT_SUBMIT_RETRY_ATTEMPTS = 0x3039`
  (`core/include/zlink_enum.h:108` [확인])).
- 단위: **개수와 바이트 둘 다**를 둔다.
  - `ZLINK_OPT_SEND_PENDING_MAX_MSGS` — 기본 제안 **1024**
  - `ZLINK_OPT_SEND_PENDING_MAX_BYTES` — 기본 제안 **SNDHWM과 동일한 값**
  - 개수만 두면 대형 메시지에서 메모리가 터지고, 바이트만 두면 소형 메시지
    폭주에서 부기 비용이 터진다. Core의 HWM 회계가 이미 바이트 기준이고
    최소 프레임 charge를 갖고 있으므로(`minimum_core_message_charge_bytes`,
    `core/include/zlink/eventing/api.h:173` [확인]) 바이트 축은 그 회계를 재사용한다.
- 초과 시 `zlink_send_async`는 **`ZLINK_SUBMIT_BACKPRESSURED`를 즉시 반환**하고
  part 소유권은 호출자에게 남는다. **여기가 앱이 정책을 소유하는 지점이다** —
  Core는 절대 pending 제출을 블로킹하지 않는다.
- 0 = 무제한은 **제공하지 않는다**. 무제한이 필요하다는 것은 앱이 큐를 스스로
  가져야 한다는 뜻이다.

**한도는 소켓당으로만 둔다 (Q3, §13 해결).** target당 부분 상한은 두지 않는다.
target당 상한이 head-of-line 격리에는 유리하지만, target 수가 동적인
DEALER/ROUTER에서는 총량 상한이 사실상 사라지는 대가를 치른다. 소켓당 단일
한도가 더 단순하고, 소켓 스코프 옵션(`ZLINK_OPT_*`)이라는 기존 선례와도
그대로 맞는다 — `ZLINK_OPT_SEND_PENDING_MAX_MSGS`/`_BYTES`는 다른 `ZLINK_OPT_*`와
마찬가지로 소켓 단위 값이다. target별 격리가 필요한 앱은 §4.4의 target별
FIFO 자체가 head-of-line을 이미 target 단위로 국한하므로 별도 부분 상한
없이도 격리가 성립한다.

### 4.8 timeout — op별 Core 소유 deadline

**[제안]** `zlink_send_async_options_t::timeout_ms`가 **op별** deadline이다.

- **`SNDTIMEO`를 재사용하지 않는다.** [확인] `SNDTIMEO`는 블로킹 caller의
  대기 상한이고 `DONTWAIT` 경로에는 아예 관여하지 않는다
  (`socket_base_msg.cpp:376-434`). async pending에 소켓 전역 값을 강제하면
  op마다 다른 deadline을 줄 수 없다. request가 이미 op별
  `timeout_ms_` 인자를 갖는 형태(`api.h:368-375` [확인])와 대칭이다.
- 실행 주체: `zlink-req-time`과 같은 lazy-start/idle-exit 전역 deadline
  스케줄러를 **재사용**한다 (`request_timeout_scheduler_internal.cpp:86-89,175-197` [확인]).
  새 스레드를 만들지 않는다. 만료 시 pending을 큐에서 제거하고
  `ZLINK_SEND_TIMED_OUT`으로 통지한다.
- `timeout_ms = 0`은 무제한이다. 무제한 pending은 §4.7의 bound와 close 시
  fail-fast로 이미 회수 가능하므로 안전하다.
- **만료와 admit의 경합**: deadline 스케줄러와 admit 스레드가 같은 pending을
  동시에 집을 수 있다. 완료는 **원자적 1회 claim**으로 결정한다 (pending
  엔트리의 상태 CAS). 먼저 claim한 쪽이 결과를 정한다.

### 4.9 즉시 성공 경로

**[제안]** `zlink_send_async`는 HWM에 여유가 있으면 호출 스레드에서 그대로
admit한다. 그래도 **완료 통지는 발생한다.** 두 가지 하위 선택지:

| 선택 | 장점 | 단점 |
|---|---|---|
| (i) 콜백을 **인라인으로** 즉시 호출 | 지연 최소, 스레드 홉 0 | 콜백이 caller 스택에서 돈다 — 재진입 규칙이 caller에도 적용된다 |
| (ii) 항상 통지 채널을 거친다 | 컨텍스트가 균일 | fast path에도 스레드 홉 1회 — 0.12.0에서 이미 실측된 비용 문제 |

**(i)을 권고한다.** §2.4의 실측(admission 부기가 `async()` 비용의 절반 이상)이
균일성보다 fast path를 지키는 쪽을 지지한다. 대신 계약에 명시한다:
**"완료 콜백은 `zlink_send_async` 호출 안에서 인라인으로 실행될 수 있다."**
바인딩은 이 경우 suspension을 "이미 완료된 상태"로 만들어야 한다 (C++
`async_result_t`의 `await_ready() == true` 경로, Rust `Poll::Ready`).

**인라인 fast-path 콜백을 유지한다 (Q4, §13 해결).** 별도 우회 신호
(`op_id_out_ = 0`이면 이벤트 없음 등)는 두지 않는다. 실측 근거(§2.4, admission
부기가 `async()` 비용의 절반 이상)가 계약 균일성보다 fast path 보존을
지지하고, 두 갈래 계약이 주는 이득보다 인지 비용이 크다. 그 결과로 생기는
비대칭(§5의 poller-owned dispatch 소비자는 즉시 admit이어도 여전히
`zlink_poller_wait()`를 거쳐야 콜백이 실행된다)은 받아들인다 — 이는 "callback
채널은 caller 스레드 인라인일 수 있고, poller-owned dispatch 채널은 항상
poller_wait 호출 시점에 실행된다"는 §5의 디스패치 위치 차이의 자연스러운
결과이지 별도 계약이 아니다.

---

## 5. 완료 이벤트 채널 설계 (D3-b)

### 5.1 monitor 스트림 vs 기존 `ZLINK_POLLCOMPLETION` 재사용 — **POLLCOMPLETION 재사용을 채택 (Q10, §13 해결)**

**결론 먼저: 새 채널을 만들지 않는다.** D3-b의 "수신 가능한 이벤트(poll/recv
루프 소비자용)"는 별도 완료 큐 + 전용 recv API가 아니라, request/reply
completion이 오늘 실제로 쓰는 **기존 `ZLINK_POLLCOMPLETION` 패턴을 그대로
재사용**한다. "콜백 채널 vs 수신 채널"의 실체는 **"콜백을 어디서 실행하느냐"의
차이일 뿐**이다 — async-mailbox 스레드에서 도느냐(기본), 아니면
`zlink_poller_wait()`를 부른 스레드에서 도느냐(POLLCOMPLETION 등록 시). 등록
API도 이벤트 struct도 새로 만들지 않는다: `zlink_send_complete_handler`
하나만 있고, `ZLINK_POLLCOMPLETION`으로 소켓을 등록하면 **같은 콜백**의
디스패치 소유권이 poller로 넘어간다.

| 축 | monitor 스트림에 얹기 | 기존 `ZLINK_POLLCOMPLETION` 재사용 |
|---|---|---|
| payload | `zlink_monitor_event_t`는 `event/value/routing_id/addr[256]×2/...`로 고정 (`eventing/api.h:46-63` [확인]). `op_id`+`userdata`+`result`를 넣을 자리가 없다 | `zlink_send_complete_event_t`(§4.2)가 콜백 인자로 그대로 전달된다. 새 payload 타입이 필요 없다 |
| 유실 | **공개 monitor는 명시적 lossy다** — `lossy = event_version_ <= 3` (`socket_base_monitor.cpp:281`)이고 `zlink_socket_monitor_open`은 3을 넘긴다 (`monitor_socket_api.cpp:142`). 큐가 차면 레코드를 버린다 (`socket_monitor_runtime.cpp:251-262`) [확인]. **완료 유실 = suspension 영구 정지** | `ZLINK_POLLCOMPLETION`은 큐가 아니라 **디스패치 소유권 이전**이다 (§3.5). pending 자체가 §4.7로 bounded이고 그 pending 각각이 정확히 한 번 콜백을 유발하므로 **유실이 구조적으로 불가능**하다 — request/reply completion이 이미 이 무손실 성질로 운영 중이다 |
| 비용 | 이벤트마다 512바이트 주소 문자열 복사 | 콜백 직접 호출. 새 큐/새 struct 복사가 없다 |
| 소유 | monitor 미등록 앱은 완료를 못 받는다. monitor 등록을 완료 수신의 전제로 만드는 것은 층이 뒤섞인다 | 독립. `zlink_send_complete_handler` 등록 자체가 전제다 |
| 선례 | — | request/reply completion이 이미 이 형태로 운영 중이다 (§3.5) — **새 개념이 아니라 기존 메커니즘의 대상 확장** |

**판정: monitor에 얹지 않고, 이미 있는 `ZLINK_POLLCOMPLETION` 디스패치
소유권 이전 메커니즘을 send completion에도 그대로 적용한다.** monitor는
관측 채널이고 완료는 제어 흐름이므로 섞지 않는다는 판단(무손실 논거)은
유지하되, 대안이 "전용 completion 큐를 새로 설계"가 아니라 "이미 무손실로
검증된 기존 메커니즘을 재사용"이라는 점이 이번 해결로 명확해졌다.

### 5.2 표면 — 새 API 없음. `ZLINK_POLLCOMPLETION` 등록이 곧 두 번째 채널이다 [제안]

```c
/* ── 초안. 등록 API와 payload struct는 §4.2의 것을 그대로 쓴다 ──────── */

/*
 * D3-b의 "수신 채널"은 별도 recv() 함수가 아니다. 다음이 계약의 전부다:
 *
 *   1. zlink_send_complete_handler(s, handler_fn, userdata) 로 콜백을 등록한다.
 *      (등록 전이면 완료가 갈 곳이 없다 — 기존 send_ready_handler와 동일한 전제.)
 *   2. 그 소켓을 poller에 ZLINK_POLLCOMPLETION 으로 등록하면
 *      (zlink_poller_add(poller, s, NULL, ZLINK_POLLCOMPLETION | ...))
 *      완료 디스패치 소유권이 async-mailbox 스레드에서 poller로 넘어간다
 *      (acquire_completion_poller(), zlink.cpp:262-266 과 동일 경로).
 *   3. 이후 handler_fn 은 async-mailbox 스레드가 아니라
 *      zlink_poller_wait() 를 부른 스레드에서, completion_drain_scope_t 안에서
 *      실행된다 — reply handler가 오늘 실행되는 것과 정확히 같은 자리.
 *
 * 새 이벤트 종류(ZLINK_EVENT_SEND_COMPLETE), 새 poller 비트, 새 recv 함수는
 * 두지 않는다. "callback 디스패치 vs event-loop 디스패치"는
 * "async-mailbox 소유 vs poller 소유"라는 하나의 축이다.
 */
```

**Q8과의 상호작용 — POLLCOMPLETION 등록 범위를 넓혀야 한다.** [확인]
`ZLINK_POLLCOMPLETION`은 현재 **DEALER/ROUTER에만** 등록 가능하다
(`core/src/api/core/zlink.cpp:249-253`) — request/reply completion이 그
두 소켓 타입에만 존재했기 때문이다. Q8(§13)에서 `zlink_send_async`/
`zlink_send_complete_handler`를 **PAIR/STREAM을 포함한 전 send-capable
소켓**으로 열기로 했으므로, 그 소켓 타입들도 `send_complete` 콜백을
poller-owned dispatch로 받을 수 있어야 계약이 일관된다. 따라서
`zlink.cpp:249-253`의 소켓 타입 검사를 **"reply completion이 있거나 send
completion이 있는 소켓"** 기준으로 넓혀야 한다 — 이 문서의 결론이며
Phase 1 구현 항목(§12.3)에 명시한다. (마이그레이션 계획에 반영 완료.)

**계약 [제안]**

- `zlink_send_complete_handler`는 등록 API가 **하나뿐**이다. "콜백 채널"과
  "수신 채널"은 같은 등록의 **디스패치 위치**만 다르다.
- **상호 배타는 소켓 단위로 강제된다 (Q7, §13 해결)**: `ZLINK_POLLCOMPLETION`
  등록 여부가 디스패치 소유권을 결정하고, 미등록이면 async-mailbox 스레드가
  기본으로 소유한다. 이미 있는 `_completion_poller_refs` 검사
  (`socket_base_lifecycle.cpp:417-425` [확인])를 그대로 재사용하므로 새
  상태 기계가 필요 없다. monitor의 콜백/수신 상호배타 `EBUSY` 규칙
  (`core/doc/spec/core/07-monitoring.en.md:295-296`, §3.4)과 같은 모양이다.
- 상관: 소비자는 `event.op_id`(그리고 원하면 `userdata`)로 자기 제출과
  맞춘다. poll 루프 앱의 전형적 형태:

  ```c
  zlink_send_complete_handler (sock, on_send_complete, NULL);
  zlink_poller_add (poller, sock, NULL, ZLINK_POLLIN | ZLINK_POLLCOMPLETION);
  /* ... */
  zlink_send_async (sock, parts, n, &opts, &op_id);
  my_table_insert (op_id, my_state);
  /* ... poller_wait 가 내부에서 on_send_complete 를 이 스레드에서 호출한다 ... */
  ```
- 배치 드레인은 성능상 필수다. `ZLINK_POLLCOMPLETION`은 이미 한 번의
  소유권 획득 안에서 여러 완료를 드레인한다 (`drain_request_completions()`,
  `socket_base_api.cpp:483-492` [확인]) — send completion도 같은 드레인
  루프를 공유한다.
- **이벤트 유실 없음**: pending 수가 §4.7로 bounded이고 각 pending은 정확히
  한 번 콜백을 유발하므로 producer가 드레인 능력을 넘길 수 없다. 소비자가
  poller_wait를 호출하지 않으면 pending 상한이 먼저 차서 `zlink_send_async`가
  `BACKPRESSURED`를 반환한다 — 백프레셔가 올바른 층으로 전파된다.

### 5.3 payload 확정 (초안)

`zlink_send_complete_event_t` (§4.2)를 그대로 쓴다. async-mailbox 디스패치와
poller-owned 디스패치가 **같은 콜백 시그니처, 같은 struct**를 쓰는 것이
D3의 "동일 의미론"을 타입 수준에서 강제한다. 필드 확장이 필요해지면 Core가
이미 쓰는 `_v2` 함수 추가 방식을 따른다 (`zlink_router_recv_part_v2`,
`api.h:441-450` [확인]).

---

## 6. 콜백 컨텍스트 계약 (D5 / 질문 C)

### 6.1 실행 스레드 [제안, §3.1 근거]

| 상황 | 완료 콜백이 도는 스레드 |
|---|---|
| 즉시 admit (§4.9-(i)), `ZLINK_POLLCOMPLETION` 미등록 | **`zlink_send_async`를 호출한 스레드**, 인라인 |
| HWM 해소 후 admit, `ZLINK_POLLCOMPLETION` 미등록 | **Core async mailbox I/O 스레드** (`process_async_mailbox`) |
| `ZLINK_POLLCOMPLETION` 등록 상태(§5, Q10) | 위 두 상황 모두 **`zlink_poller_wait()`를 호출한 스레드**로 대체된다 — 디스패치 소유권이 poller로 넘어갔기 때문이다 |
| deadline 만료 | **Core deadline 스케줄러 스레드** (`zlink-req-time` 계열) |
| 취소 | 취소 요청 스레드 또는 위 중 하나 (경합 claim 결과에 따름) |
| close / ctx term | **close/term을 호출한 스레드** (반환 전 전부 통지) |

즉 **하나의 고정 스레드를 약속하지 않는다.** 약속하는 것은 다음 셋이다.

1. **하나의 op에 대해 완료 콜백은 정확히 한 번 실행된다.**
2. **같은 target의 완료 콜백은 제출 순서대로 실행된다** (§4.4).
3. **완료 콜백 실행 중에는 그 소켓의 다른 완료 콜백이 동시 실행되지 않는다**
   (소켓당 직렬). 이는 `dispatch_routed_send_ready_events`가 이미 갖는 형태다
   (`socket_base_dispatch.cpp:369-407`, `socket_callback_scope_t` [확인]).

### 6.2 콜백이 해도 되는 일 / 하면 안 되는 일 [제안]

**허용 (완료 전달만)**
- suspension 완료: promise/future/`std::coroutine_handle` resume 준비,
  `Task` `SetResult`, channel send, `CompletionStage.complete`
- 앱 소유 자료구조에 enqueue
- `zlink_msg_*` (Core가 아직 소유하지 않은 자기 메시지에 한해)

**금지**
- **모든 send/publish/request 계열 API 호출** — `zlink_send_async` 포함
- 블로킹 (뮤텍스 대기, I/O, condvar)
- 같은 소켓의 `zlink_close` (지연 close 계약으로 흡수되지만 의존하지 말 것)
- 완료 핸들러 자기 자신의 교체

### 6.3 이것이 강제 가능한가 — **부분적으로 가능. 정직하게 서술한다** [확인 기반]

| 항목 | 강제 수단 | 판정 |
|---|---|---|
| 콜백 안에서 send 재진입 | Core는 이미 콜백 디스패치 중 TLS 스코프를 세운다 (`socket_send_ready_dispatch_scope_t`, `socket_dispatch_bridge.cpp:171-192`; `completion_drain_scope_t`, `socket_base_api.cpp:648-668` [확인]). 같은 방식으로 completion 디스패치 스코프를 세우고 send 진입점에서 검사해 `EDEADLK`를 반환할 수 있다 | **런타임 강제 가능** [제안] |
| 핸들러 자기 교체 | 이미 같은 방식으로 `EDEADLK`를 반환하는 선례가 있다 (`api.h:139-141,167-168` [확인]) | **강제 가능** |
| 블로킹 | 강제 불가. 임의 사용자 코드가 뮤텍스를 잡는 것을 Core가 막을 방법이 없다 | **문서화만** |
| 무거운 작업 | 강제 불가 | **문서화만** |

**중요한 계약 개선점**: D1이 재시도를 없앴으므로 **"콜백에서 submit하면 안
된다"가 이제 진짜로 지킬 수 있는 규칙이 된다.** Phase 0에서 이 규칙이 문제였던
이유는 규칙이 나쁜 게 아니라 **규칙을 지키면 할 일(재시도)이 없어졌기**
때문이다. 이제 재시도를 Core가 하므로 콜백은 순수 완료 전달로 충분하다.
헤더와 스펙 본문의 강도 불일치(Phase 0 §1.1이 지적한
"헤더는 submit 금지 / 스펙은 blocking submit만 금지" [확인])도
**"완료 전달 외 금지, send 재진입은 `EDEADLK`"** 하나로 통일된다.

**"Core 스레드에서 사용자 코드가 돈다" 문제**는 남는다. 이것은 두 가지로 완화된다.
1. 콜백 계약이 "완료 전달만"이므로 실행 시간이 상수에 가깝다.
2. 그것도 싫은 소비자를 위해 **poller-owned dispatch(§5, `ZLINK_POLLCOMPLETION`
   등록)가 존재한다** — 이 소비자는 완료 콜백을 자기 `zlink_poller_wait()`
   호출 스레드에서 실행하므로 Core의 async-mailbox 스레드에서 사용자 코드가
   아예 돌지 않는다. D3의 두 디스패치 위치는 스타일 선택이 아니라 **이
   문제의 해답**이다.

### 6.4 바인딩 측 귀결 (D5 검증)

- 바인딩은 스레드 0개. 완료 콜백 → suspension 완료 → 이후 실행 모델
  연결(코루틴 재개 스케줄링)은 framework/앱 몫이다.
- Phase 0가 필요했던 `outbound_record_attempt_mutex`
  (`bindings/cpp/src/Runtime/Sockets/socket_callback_state.hpp:32` [확인])는
  **불필요해진다.** 배열 제출(§4.1)이 한 호출에서 gate를 잡고 놓으므로
  바인딩이 직렬화를 흉내 낼 이유가 없다.
- 콜백 안에서 바인딩 뮤텍스를 잡을 필요가 사라지므로 Phase 0 §1.4의 liveness
  위험(Core async-mailbox 스레드가 앱 스레드에 막힘)이 소멸한다.

---

## 7. 계약 요약 (순서·취소·close·timeout)

| 항목 | 계약 [제안] |
|---|---|
| **완료의 의미 (D8)** | `ZLINK_SEND_ADMITTED` = **Core 송신 큐로의 admission**. peer가 받았다는 뜻이 **아니다**. 전달 확인이 필요하면 request/reply를 쓴다. 헤더 doc comment에 명시한다 |
| 제출 단위 | 완전한 멀티파트 레코드 1개. 성공 시 part 소유권이 Core로 이전 |
| 제출 블로킹 | 절대 없음. pending 상한 초과 시 즉시 `BACKPRESSURED` |
| 완료 횟수 | op당 정확히 1회. 성공·타임아웃·취소·terminal·종료 모두 포함 |
| 순서 | target별 FIFO는 **async pending 사이에서만** 보장(admit·완료 통지 모두). target 간 무보장. 동기 send는 같은 HWM을 두고 그 target의 다른 생산자와 동등 경쟁 — 특례 없음(Q1) |
| 취소 | 요청 의미론. 접수되면 `CANCELED`로 완료. 이미 커밋이면 `INVALID_STATE` 반환 후 `OK`로 완료 |
| timeout | op별 `timeout_ms`. Core 전역 deadline 스케줄러가 구동. 0=무제한 |
| close | 모든 pending을 `TERMINATED`(`ECANCELED`)로 즉시 실패. LINGER 미적용. drain 옵트인 없음(Q2) |
| ctx term | 동일, errno `ETERM` |
| pending 상한 | **소켓당** msgs/bytes 이중 상한. target당 부분 상한 없음(Q3). 무제한 없음 |
| 완료 채널 | 콜백 하나(`zlink_send_complete_handler`). "수신 채널"은 새 API가 아니라 `ZLINK_POLLCOMPLETION` 등록으로 같은 콜백의 디스패치 소유권이 poller로 넘어가는 것(Q10). 소켓 단위 상호배타(Q7) |
| 콜백 계약 | 완료 전달만. send 재진입은 `EDEADLK`. 소켓당 직렬. 즉시 admit 시 인라인 실행 허용(Q4) |
| 유실 | 없음. 완료는 pending 1건당 정확히 1회 콜백이므로 pending 상한이 곧 상한 |
| 대상 소켓 | PAIR/PUB/XPUB/DEALER/ROUTER/STREAM 전체 (Q8) |

---

## 8. 기존 표면 처분 표 (질문 D)

### 8.1 Core 표면

| 표면 | 위치 [확인] | 성격 | 처분 [제안] |
|---|---|---|---|
| `zlink_send_ready_handler_fn` | `core/include/zlink/socket/api.h:63` | readiness hint 콜백 타입 | **제거** → `zlink_send_complete_handler_fn` |
| `zlink_send_ready_handler()` | `api.h:152-154` | hint 등록 (PAIR/PUB/XPUB/DEALER/ROUTER/STREAM) | **제거** → `zlink_send_complete_handler()` (D7 병합) |
| `zlink_routed_send_ready_state_t` (`WRITABLE`/`TERMINAL`) | `api.h:65-69` | hint 상태 enum | **제거** → `zlink_send_complete_result_t` |
| `zlink_routed_send_ready_event_t` | `api.h:71-78` | hint payload | **제거** → `zlink_send_complete_event_t` (target identity를 그대로 승계) |
| `zlink_routed_send_ready_handler_fn` / `_handler()` | `api.h:93-94`, `:170-172` | routed hint 등록 | **제거.** 별도 routed 계열을 두지 않는다 — 완료 payload가 항상 target identity를 실으므로 위의 단일 계열이 흡수한다 (D7) |
| `zlink_select_routed_submit_target()` | `api.h:182-184` | "예약 아님" 스냅샷 선택 | **유지, 문구 개정.** exact-target 지정 자체는 async 제출에도 필요하다(`options->target`). 헤더의 "not a reservation ... can report backpressure" 문구는 유지하되 "binding-owned pending state에 넣으라"는 안내(`api.h:178-181`)를 **삭제**한다 — 그 pending은 이제 Core 것이다 |
| `zlink_routed_submit_target_t` | `api.h:80-86` | target 값 identity | **유지** |
| `socket_dispatch_bridge` edge coalescing (`enqueue_routed_send_ready` / `take_routed_send_ready` / `routed_send_ready_pending` / `_terminal`) | `core/src/runtime/sockets/common/socket_dispatch_bridge.cpp:131-171` | hint 합치기 | **제거.** 완료 큐는 coalescing하지 않는다 — op마다 1건이 필수다 |
| `dispatch_routed_send_ready_events()` | `socket_base_dispatch.cpp:369-407` | hint 디스패치 루프 | **제거하고 completion 디스패치로 대체.** 콜백 스코프·close 처리 구조는 재사용 |
| `enqueue_all_routed_send_terminals()` | `socket_base_dispatch.cpp:355-367` | close/term 시 전량 TERMINAL | **재사용.** pending 전량 실패 통지로 의미 전환 (§4.6) |
| `arm_send_ready_notification` / `arm_send_ready_after_backpressure` / `mark_send_recovery_pending` / `_ready` | `socket_dispatch_bridge.cpp:52-95`, `socket_base_msg.cpp:294-381` | 내부 send-recovery 엣지 | **내부 유지.** 공개 hint의 근원이지만 blocking send(D4)와 async admit 구동에 그대로 필요하다. 공개 표면만 사라진다 |
| `ZLINK_POLLOUT` (socket) | `zlink_enum.h:315` | level-triggered 관측 | **유지** (→ §11) |
| `ZLINK_EVENT_SEND_FLOW_PAUSED` / `_RESUMED` | `zlink_enum.h:243-244,264-265` | 원격 receive-flow 전이 관측 (monitor) | **유지** (→ §9) |
| `ZLINK_EVENT_FLOW_STATE_STALE` | `zlink_enum.h:245,266` | 같은 가족의 세 번째 이벤트 (stale/중복 프레임 거부) | **유지.** hint와 무관 |
| `ZLINK_MONITOR_EVENT_FLAG_SEND_FLOW_WRITABLE` | `eventing/api.h:28` | RESUMED 시 로컬 writability 합성 판정 | **유지, 문구 강화** (→ §9.3) |

### 8.2 바인딩 소비자 (언어별 인벤토리)

앞선 인벤토리 조사 결과 [확인]. 라인 수는 삭제 규모의 근사치다.

| 언어 | 제거 대상 | 규모 | 자체 스레드 | 현재 routed send terminal |
|---|---|---|---|---|
| **C++** | 없음 (이미 반전 완료). `socket.cpp:46-80,590-598`의 `send_ready` 트램폴린과 `socket_callback_state.hpp:22-24,33`, 그리고 `set_send_ready_handler` 재노출 6개소(`Contracts/Sockets/*.hpp`)만 정리 | ~60 L | **0** | `submit() -> void` |
| **.NET** | `RoutedAdmissionScheduler.cs`(1143 L), `PublisherAdmissionScheduler.cs`(693 L), `SocketKernel.Callbacks.cs:78`, p/invoke 3개(`NativeMethods.Socket.cs:96,100,104`) | **1836 L** | `Threading.Timer` + `ThreadPool` | `Async()` 전용 |
| **Java** | `RoutedAdmission.java`(1206 L), `PublisherAdmission.java`(653 L), `StreamAdmission.java`(611 L), `SocketCore.java:288`, `Native.java:93,97,101,667,679,691` | **2470 L** | 전용 executor + 공유 ScheduledExecutor | `CompletionStage submit()` |
| **Node** | `routed_admission.ts`(588 L), `publisher_admission.ts`(201 L), `addon_routed_admission.cc`(601 L), `addon_core.cc`의 send_ready 슬롯 8개(:25,159-180,1272-1361,1403-1413,3280-3297) | **~1390 L** | OS 스레드 0 (TSFN + `setTimeout`) | `Promise submit()` |
| **Python** | `routed_async.py`(846 L, `threading.Thread` :199-223 포함), `socket_base.py:258-259,560-566,638-661`, `ffi.py:248-250` | **~900 L** | `threading.Thread` | `Awaitable submit()` |
| **Rust** | `internal/routed_admission.rs`(444 L, 전역 deadline reactor 스레드 :406-408), `routed_async.rs`(710 L), `socket_inner_runtime.rs:28,301-319,657-685,1003-1011,1038`, `ffi.rs:38-40,56-60,496-501,575-585`, `on_send_ready` 계약 6개소 | **~1200 L** | 전역 reactor 스레드 | `impl Future submit()` |
| **Go** | `routed_admission.go`(581 L, goroutine :106), `socket_send_ready.go`(35 L) | **~616 L** | goroutine | `Submit() <-chan error` |
| **C** | 없음 (헤더 선언만). 벤치 shim `c/bench/with_zmq/std_compat/zlink.h:217,1267`은 ZeroMQ 호환용으로 무관 | 0 | 0 | `Submit()` |

총 제거 규모 **약 8,400 L**. 이 코드는 D1/D2 없이도 §3.2 결정으로 이미 삭제
예정이었으므로, `send_ready` 제거로 인한 **추가** 파괴는 사실상 없다.

동반 제거 테스트 [확인]: `dotnet/tests/Zlink.Tests/test_routed_async_admission.cs`,
`test_publisher_async_admission.cs`, `java/.../RoutedAdmissionTest.java` 외 4개,
`node/tests/routed_async_admission.test.ts`,
`python/tests/test_routed_async_contract.py`,
`rust/tests/routed_async_tests.rs`, `go/routed_async_test.go`.
심볼 allowlist 갱신 필요: `go/tests/raw-core11-allowlist.json:97,103,149-150`,
`rust/tests/optimization_guard_tests.rs:34-35`,
`python/tests/test_native_contract.py:44-45`,
`dotnet/.../NativeMethods.Core.cs:86-91`.

### 8.3 Core 테스트 파괴 표면 [확인]

`zlink_send_ready_handler` / `zlink_routed_send_ready_handler` /
`zlink_select_routed_submit_target`를 참조하는 Core 테스트 11개:

`core/tests/integration/`의 `test_routed_submit_target.cpp`,
`test_router_mandatory_hwm.cpp`, `test_socket_with_handler.cpp`,
`test_stream_send_blocking_wakeup.cpp`, `test_stream_socket.cpp`,
`test_stream_threadsafe.cpp`, `test_thread_safe_contract_policy.cpp`,
`test_zmp_request_reply.cpp`, `test_reconnect_options.cpp`,
`monitoring/test_monitor_perf_contract.cpp`;
`core/tests/unittest/unittest_socket_runtime.cpp`.

`zlink_select_routed_submit_target`만 쓰는 테스트는 그대로 통과하고,
readiness handler를 쓰는 테스트는 completion 계약 테스트로 재작성한다.

---

## 9. `SEND_FLOW_PAUSED` / `SEND_FLOW_RESUMED` 처분 판정

### 9.1 판정: **유지** — readiness hint가 아니라 wire 프로토콜 상태 관측이다

### 9.2 근거

0. **결정적 근거: Core 안에서 관측과 readiness가 이미 물리적으로 분리되어 있다.**
   [확인] `core/src/runtime/core/pipe.cpp:1329-1343` (`pipe_t::process_flow_state`):

   ```cpp
   if (transition != flow_state_no_transition)
       _sink->flow_state_applied (this, transition == flow_state_transition_paused,
                                  epoch_, actual_writable);   /* → monitor event */
   if (notify)
       _sink->write_activated (this);                          /* → readiness edge */
   ```

   `flow_state_applied()`가 `SEND_FLOW_*` monitor event를 만드는 **유일한**
   emit 경로이고(`core/src/runtime/sockets/common/socket_base_flow_state.cpp:332-349`),
   실제 재시도를 깨우는 경로는 **완전히 별개의 호출** `write_activated()` →
   `enqueue_routed_send_ready(..., ZLINK_ROUTED_SEND_WRITABLE, ...)`
   (`socket_base_api.cpp:290-299`)다.

   즉 **D1이 제거하는 hint는 `write_activated` 쪽 라인이고, `SEND_FLOW_*`는
   그 옆 라인이다.** monitor를 하나도 열지 않은 소켓도 send는 정상적으로 재개된다
   (`socket_base_t::event()`가 mask 0이면 즉시 반환,
   `socket_base_monitor.cpp:607-608`). 두 축이 서로에게 필요하지 않다는 사실이
   "이건 hint가 아니다"의 가장 강한 증거다.

   Core 주석도 같은 말을 한다 (`pipe.cpp:1334-1337` [확인]):
   > "Observation never gates the send path: this call happens after the
   > transition already committed, off the per-message write/read path."

1. **이 이벤트가 보고하는 것은 로컬 HWM이 아니라 원격 peer의
   receive-flow 프로토콜 상태다.** [확인] 정식 스펙
   (`core/doc/spec/core/05-events.ko.md:35-48`):
   > paired DEALER/ROUTER socket은 **peer의 receive-flow 상태**를 monitor event
   > 3개로 보고한다. ... 이 socket의 application pipe 하나에서 **peer 상태가
   > 실제로 PAUSED와 RUNNING 사이를 오갈 때, 그 전이를 pipe에 적용한 뒤에만**
   > 발생한다.

   `WRITABLE`/`TERMINAL` hint는 "로컬에서 재시도해 볼 만하다"는 신호였지만,
   `SEND_FLOW_*`는 "상대가 flow-state 프레임으로 PAUSED/RUNNING을 선언했고
   그것을 적용했다"는 **사실 기록**이다. `value`는 flow epoch,
   식별자는 `routing_id` + `transport_pair_id` + `transport_pair_generation`이다.

2. **스펙이 readiness 해석을 명시적으로 부정한다.** [확인]
   같은 파일 `:50-52`:
   > byte HWM, transport wait, termination 같은 다른 원인이 계속 pipe를 막고
   > 있으면 `ZLINK_MONITOR_EVENT_FLAG_SEND_FLOW_WRITABLE`이 없다. 따라서
   > **RESUMED event만으로 다음 send가 수락된다고 보장하지 않는다.**

3. **재시도에 쓰는 소비자가 하나도 없다.** [확인] 전 언어 grep 결과
   `SEND_FLOW_WRITABLE` / `SEND_FLOW_PAUSED` / `SEND_FLOW_RESUMED`의 소비자는
   전부 **enum 값 패리티 테스트와 상수 미러**뿐이다:
   - `bindings/python/src/zlink/contracts/eventing/codes.py:32`,
     `python/tests/test_enums.py:75`, `test_flow_state_parity.py:262`
   - `bindings/rust/src/contracts/eventing/monitor.rs:350`,
     `rust/tests/monitor_tests.rs:48-58`,
     `rust/src/runtime/eventing/monitor.rs:64,80`
   - `bindings/java/.../MonitorEventFlags.java:24`,
     `java/.../MonitorContractTest.java:72-83`
   - `bindings/go/internal/native/monitor.go:119`
   - `core/tests/integration/test_flow_state_c_api.cpp:350-440,520-580`,
     `bindings/cpp/tests/contract/test_cpp_contract_flow_state.cpp:107-134`

   행동 테스트가 있는 언어는 둘뿐이고 그마저 전부 단언만 한다:
   `bindings/go/monitor_test.go:105-170` (ROUTER pause → DEALER monitor가
   `IsSendFlowPaused()` + pair id 일치 확인 → resume → `IsSendFlowResumed()`),
   `bindings/dotnet/tests/Zlink.Tests/test_flow_state.cs:78-104` (같은 형태).
   **두 테스트 모두 RESUMED 뒤에 send를 하지 않는다.** Core 테스트도 send는
   `ZLINK_DONTWAIT` 재시도 루프와 metric 폴링으로 구동하고 이벤트로 구동하지
   않는다. perf 하네스는 이 이벤트를 **전혀 참조하지 않는다** (`doc/perf/**`
   전체 grep 0건); perf가 쓰는 것은 `flow_*` 스냅샷 카운터뿐이다.

   **`SEND_FLOW_RESUMED`를 받고 send를 재시도하는 코드는 저장소 어디에도
   없다.** 반면 `zlink_routed_send_ready_handler`는 6개 언어의 admission
   reactor가 정확히 그 용도로 소비하고 있었다 (§8.2). 대비가 결정적이다.

4. **관측 축으로 실제 값이 있다.** [확인] `zlink_monitor_status_t`가 ABI 4에서
   이 축의 누적 통계를 노출한다 (`core/include/zlink/eventing/api.h:183-199`):
   `flow_paused_connections`, `flow_pause_applied_total`,
   `flow_resume_applied_total`, `flow_state_stale_total`,
   `flow_pause_duration_ms`. 이벤트를 없애면 스냅샷 카운터만 남아 전이 시점을
   알 수 없어진다. 모니터링 제품 표면이 실제로 얇아진다.

5. **전달 경로가 유실 가능하므로 애초에 hint로 쓸 수 없다.** [확인]
   `SEND_FLOW_*`는 monitor 핸들(`zlink_socket_monitor_open` / `_handler` /
   `_recv`, `eventing/api.h:203-213`)로 나가는 관측 스트림이고, bounded FIFO
   `std::deque`에 쌓인다(`socket_monitor_runtime.cpp:243-264`). 그리고
   **공개 monitor는 명시적으로 lossy다**: `lossy = event_version_ <= 3`
   (`socket_base_monitor.cpp:281`)이고 `zlink_socket_monitor_open`은 버전 3을
   넘긴다(`monitor_socket_api.cpp:142`). 큐가 차면 레코드를 **버린다**
   (`socket_monitor_runtime.cpp:251-262`).

   게다가 **RESUMED가 아예 발생하지 않는 정상 경로가 있다**:
   paused 상태로 pair가 종료되면 gauge만 풀고 RESUMED를 내지 않는다
   (`socket_base_flow_state.cpp:352-375`, 주석: *"This is a lifecycle release,
   not a resume"*).

   **이 둘 때문에 RESUMED로 재시도를 구동하는 소비자는 두 경로 모두에서
   영구 정지한다.** 즉 이 이벤트는 hint로 쓸 수 *없는* 물건이고, 아무도 그렇게
   쓰지 않는 이유가 이것이다. 반대로 완료 통지가 monitor에 있으면 안 되는
   이유(§5.1)도 정확히 같은 사실이다.

6. **emit 조건 자체가 "적용된 상태 전이"다.** [확인] emit은 정확히 한 곳
   (`socket_base_flow_state.cpp:332-349`)이고 두 개의 생산 경로에서 불린다:
   (a) flow-state 프레임이 stale/중복 검사를 통과하고 실제 PAUSED↔RUNNING
   전이를 만들었을 때 (`pipe.cpp:1329-1332`, 소켓 소유 스레드),
   (b) pair admission 시점에 pending 상태를 적용해 전이가 발생했을 때
   (`socket_base_api.cpp:285-288`). 두 경로 모두 **전이가 커밋된 뒤**에 부른다.
   payload는 `value` = flow epoch, 식별자는 routing_id + pair id + generation이다.
   hint라면 "적용 여부와 무관하게 재시도해 보라"를 쏘는 것이 맞다.

### 9.3 단, 한 조각은 hint의 잔재다 — `ZLINK_MONITOR_EVENT_FLAG_SEND_FLOW_WRITABLE`

[확인] `core/include/zlink/eventing/api.h:24-28`:
> Set on `ZLINK_EVENT_SEND_FLOW_RESUMED` when clearing the remote pause left the
> pipe **actually writable**.

이 비트만은 "원격 상태 보고"가 아니라 **로컬 writability 합성 판정**이다.
성격상 D1이 폐지하는 hint와 같은 부류다. 다만:

- 이 비트를 재시도에 쓰는 소비자가 **없다** (§9.2-3).
- 제거하면 monitor event flags의 비트 배치가 바뀌어 4개 언어의 상수 미러와
  패리티 테스트를 건드려야 하는데, 얻는 것이 "hint 오용 가능성 차단"뿐이다.

**결정: 비트는 유지하고 문서 문구만 강화한다 (Q5, §13 해결).** "이 flag는
진단용이며 send 재시도의 트리거로 사용해서는 안 된다. 송신 완료가 필요하면
`zlink_send_async`를 쓰라"를 헤더와 `05-events.*.md`에 명시한다. 비트를
제거하는 쪽(흔적도 남기지 않기)은 채택하지 않는다 — 얻는 것이 "hint 오용
가능성 차단"뿐인데 4개 언어의 비트 배치·패리티 테스트를 건드리는 비용이
크다.

### 9.4 곁가지로 발견한 문서/구현 불일치 (이번 범위 밖, 기록만)

[확인] `core/doc/spec/core/07-monitoring.en.md:340-343`은 monitor 큐가 가득 차면
*"aggregates identical high-frequency events and prioritizes connection-state
events"* 한다고 서술하지만, 실제 구현
(`core/src/api/monitoring/socket_monitor_runtime.cpp:251-262`)은 **집계 없이
단순 drop**한다. `enqueue_worker_event`에 conflate/dedup 로직이 없다.

이 설계와 직접 관련은 없으나 monitor 계약 문서의 정확성 문제이므로 별도
항목으로 남긴다. 이 문서의 §5.1이 "monitor는 유실 가능하므로 완료 통지를
얹지 않는다"고 판단한 근거는 코드 쪽(=drop)이며, 스펙 문구가 코드에 맞춰
정정되면 그 판단이 더 명확해진다.

---

## 10. 언어별 종결자 규범 표 (D6)

| 언어 | send | publish | request | raw reply | 근거/변경 |
|---|---|---|---|---|---|
| **C** | `zlink_send_part*` (동기) + `zlink_send_async` | `zlink_publish_part` + `zlink_publish_async` | `zlink_dealer_request_part` 등 | `zlink_*_reply_part` | C ABI는 builder 정책 미적용 |
| **C++** | `submit()` 블로킹 **+ `async()` 코루틴** | `submit()` **+ `async()`** | `submit()` 블로킹 반환 **+ `submit(callback)` + `async()`** | `submit()` 동기 | `async()` **재추가**. 현재 sync 기반(`operation_contracts.hpp:203,217-239`)에 얹는다. request의 3-terminal은 D6이 명시적으로 기존 금지를 폐기한 부분 |
| **.NET** | `Async()` 단일 | `Async()` | `Async()` | `Submit()` 동기 | HWM-managed op는 `Async()` 하나. 현재 형태 유지, 내부만 Core 위임으로 교체 |
| **Java** | `submit()` → `CompletionStage<Void>` | 동일 | `submit()` → `CompletionStage<List<Message>>` | `submit()` → `void` | 표면 불변, 구현이 `RoutedAdmission` → Core completion |
| **Kotlin** | `submit()` (Java 표면) | 동일 | 동일 | 동일 | canonical: `submit().await()` |
| **Node** | `submit()` → `Promise<void>` | 동일 | `submit()` → `Promise<Message[]>` | `submit()` → `void` | 표면 불변 |
| **Python** | `submit()` → awaitable | 동일 | `submit()` → awaitable | `submit()` → `None` | 표면 불변 |
| **Rust** | `submit()` → `impl Future<Output=Result<(),SubmitError>>` | 동일 | `submit()` → `impl Future<Output=Result<Vec<Message>,ZlinkError>>` | `submit()` → `Result` | 표면 불변. Future drop = `zlink_send_async_cancel` |
| **Go** | **`Submit(ctx) -> error` (동기)** | 동일 | `Submit(ctx)` → completion channel | `Submit()` → `error` | **D6/D10 변경.** 현재의 `<-chan error` 채널 반환(`dealer_router_request.go:349` [확인])을 **동기 `error` 반환으로 되돌린다.** goroutine 블로킹이 Go식 await이므로 채널을 감싸 반환하는 것은 관용에 어긋난다. `ctx`가 취소와 deadline을 소유한다: `ctx.Done()` → `zlink_send_async_cancel`, `ctx.Deadline()` → `options.timeout_ms` |

**핵심 변화**: 다섯 언어(.NET·Java/Kotlin·Node·Python·Rust)의 **공개 표면은
그대로**이고 그 아래 8,400 L의 admission 기계장치가 Core 위임으로 대체된다.
표면이 바뀌는 곳은 둘이다:
- **C++**: 표면이 늘어난다 (`async()` 재추가, request `submit(callback)` 추가).
- **Go**: send terminal 반환 타입이 `<-chan error` → `error`로 좁아진다.
  이것은 Go 사용자 코드를 깨는 변경이므로 Go 스펙
  (`bindings/doc/spec/go/README.{ko,en}.md:140-224` [확인])과 예제를 함께 고친다.

**언어 관용 우선 정책(D10)의 귀결**: "모든 언어가 같은 모양"이 목표가 아니다.
같은 것은 **Core 계약**이고, 각 언어는 자기 관용으로 그 계약을 노출한다.
Go의 동기 `Submit(ctx)`와 Rust의 `Future` 반환 `submit()`이 같은
`zlink_send_async` + completion 위에 서는 것이 정상이다.

**스펙 개정 필요 사항** [확인]:
- `bindings/doc/spec/async-coroutine-policy.{ko,en}.md` — "routed send는 동기
  `submit()`" 절을 "동기 + Core-completion awaitable 병존"으로 재개정.
  "request builder에 callback terminal을 canonical suspension terminal과 함께
  노출하지 않는다" 규칙에 **C++ 예외**를 명문화.
- `bindings/doc/spec/README.{ko,en}.md` `##### Retained credit과 routed 완료`
  (`:1302-1332`).
- 언어별 `bindings/doc/spec/<lang>/README.{ko,en}.md` 9쌍.
- 특히 java `:722`, dotnet `:287-291`, rust `:514`는 현재 **구현이 만족하지
  못하는 스레드-프리 주장을 이미 하고 있다** [확인]. 이 작업이 그 주장을
  참으로 만든다.

---

## 11. 앱 소유 정책을 위해 남는 것 (질문 F)

### 11.1 `DONTWAIT` — [확인] 그대로 존재

`ZLINK_SEND_FLAGS_DONTWAIT` (`core/include/zlink_enum.h:301`)와 모든 part send
API의 `flags_` 인자. D4에 의해 불변이다.

### 11.2 POLLOUT 관측 — **존재하지만 granularity가 부족하다**

**존재하는 것** [확인]:
- `ZLINK_OPT_EVENTS` → `zlink_getsockopt`로 현재 readiness 비트
  (`zlink_option.cpp:101`, `options_owner.cpp:63`).
- `zlink_poll` / `zlink_poller_*`의 `ZLINK_POLLOUT`.
- **level-triggered이다**: `recv_internal.cpp:448-449`가 매 폴에서
  `socket->transport_has_out()`을 새로 질의한다. edge-triggered hint와 달리
  "놓치면 끝"이 아니다.

**부족한 것** [확인]:

| 소켓 | `xhas_out()` 실제 동작 | 문제 |
|---|---|---|
| ROUTER | `router_send_path.cpp:416-421`: `if (!_mandatory) return true;` | **`ROUTER_MANDATORY`가 꺼져 있으면 항상 `true`.** POLLOUT이 정보를 전혀 주지 않는다 |
| DEALER | `dealer.cpp:235-238`: `_lb.has_out()` | **소켓 집계**. 어떤 파이프 하나라도 쓸 수 있으면 참. 앱이 보내려는 **그 target**의 writability가 아니다 |
| ROUTER (per-pipe) | `router_recv_path.cpp:517-526`이 `check_hwm() && !remote_flow_blocks_next_message()`로 **정확한 per-pipe 판정을 이미 계산한다** | 이 값이 **공개 표면으로 나오지 않는다** |

즉 **"직접 백프레셔 정책을 짜겠다"는 앱에게 POLLOUT-equivalent 관측은
routed 소켓에서 실질적으로 없다.** 그동안 그 자리를 `zlink_routed_send_ready_handler`
(edge hint)가 메우고 있었고, D1이 그것을 제거한다. §11.3에서 이 간극을
새 질의 API 없이 `zlink_send_async`의 기존 계약(DONTWAIT류 프로브)으로
메우는 결론을 내린다 (Q6, §13 해결).

### 11.3 최소 답안 — **새 질의 API는 추가하지 않는다 (Q6, §13 해결)**

**`zlink_routed_target_writable` 같은 별도 질의 함수는 두지 않는다.** 소유자
판단: 불필요한 복잡성이다. §11.2가 지적한 "직접 백프레셔 정책을 짜려는 앱에게
target별 writability 관측이 없다"는 간극은, 새 질의 API가 아니라 **이미
있는 `DONTWAIT` 프로브가 그 질의 역할을 겸한다**는 사실로 메워진다.

- `zlink_send_async(sock, parts, n, &opts, &op_id)`를 호출하면 그 자체가
  즉시(블로킹 없이) 결과를 알려준다: 여유가 있으면 그 자리에서 admit되고
  (§4.9), 여유가 없으면 pending으로 예약되거나(기본) §4.7의 상한에 걸려
  `BACKPRESSURED`를 반환한다. **"이 target이 지금 받을 수 있는가"를 알고
  싶은 앱은 곧 보낼 그 레코드로 `zlink_send_async`를 호출하는 것 자체가
  질의다** — 성공하면 답은 "그렇다"이고 그 호출이 곧 제출이므로 별도
  질의-후-제출의 TOCTOU 틈도 없다.
- 순수 관측만 원하고 제출까지는 원치 않는 극히 드문 경우에도, 별도 함수를
  새로 만드는 비용(헤더 표면 증가, ROUTER/DEALER 구현, 스펙·바인딩 반영)이
  DONTWAIT 프로브로 이미 얻는 것 이상의 가치를 주지 않는다고 판단한다.
- §11.2가 지적한 `ZLINK_OPT_EVENTS`/`POLLOUT`의 낮은 granularity(ROUTER
  비-mandatory 시 항상 `true`, DEALER는 소켓 집계) 문제는 여전히 사실로
  남지만, 이 문서의 결론은 그 간극을 새 API가 아니라 `zlink_send_async`의
  기존 계약으로 메운다는 것이다.

이 절의 `zlink_config_result_t zlink_routed_target_writable(...)` 초안은
**채택하지 않는다.** §12(마이그레이션)·§14(제안 목록)에서도 제외한다.

---

## 12. 마이그레이션 계획 (질문 E)

### 12.1 파괴 표면 (정확히)

**제거되는 공개 C 심볼 2개**: `zlink_send_ready_handler`,
`zlink_routed_send_ready_handler`.
**제거되는 공개 타입 3개**: `zlink_send_ready_handler_fn`,
`zlink_routed_send_ready_handler_fn`, `zlink_routed_send_ready_event_t`.
**제거되는 공개 enum 1개 + enumerator 2개**: `zlink_routed_send_ready_state_t`,
`ZLINK_ROUTED_SEND_WRITABLE`, `ZLINK_ROUTED_SEND_TERMINAL`.

**추가되는 공개 심볼 (초안)**: `zlink_send_async`, `zlink_publish_async`,
`zlink_send_complete_handler`, `zlink_send_async_cancel`.
`zlink_send_complete_recv`와 `zlink_routed_target_writable`은 **추가하지
않는다** (각각 Q10, Q6, §13 해결 — §5.2, §11.3).
**추가 타입**: `zlink_send_op_id_t`, `zlink_send_complete_result_t`,
`zlink_send_complete_event_t`, `zlink_send_complete_handler_fn`,
`zlink_send_async_options_t`.
**추가 매크로/enumerator**: `ZLINK_SEND_ADMITTED` / `ZLINK_SEND_TIMED_OUT` /
`ZLINK_SEND_TERMINAL`,
`ZLINK_OPT_SEND_PENDING_MAX_MSGS`, `ZLINK_OPT_SEND_PENDING_MAX_BYTES`.
`ZLINK_EVENT_SEND_COMPLETE`와 `ZLINK_POLLSENDCOMPLETE`는 **추가하지 않는다**
— 수신 채널은 새 이벤트 종류/poller 비트가 아니라 기존 `ZLINK_POLLCOMPLETION`
등록으로 실현된다 (Q10, §13 해결). 다만 `ZLINK_POLLCOMPLETION`의 등록
가능 소켓 타입 검사(`zlink.cpp:249-253`)는 Q8(§13 해결, PAIR/STREAM 개방)에
맞춰 넓혀야 한다 (§5.2, §12.3 Phase 1).

**순 결과**: 옛 표면 6개(함수 2 + 타입 3 + enum 1)가 사라지고 **`send_complete`
계열 하나**가 그 자리를 대신한다 (D7). routed 전용/plain 전용으로 갈라져 있던
readiness 표면이 하나로 줄어드는 것이 이 변경의 표면 측 순이익이다.

**게이트 영향** [확인]: `core/tests/contract/check_public_surface.py`는
정식 스펙(`core/doc/spec/core/**` ko/en 동일 C 블록)과 헤더 클로저와
`libzlink` 동적 export를 **정확 일치**로 검사한다. 따라서 위 변경은
**ko/en 스펙 문서 수정이 헤더 수정과 동시에** 이뤄져야 빌드가 통과한다.
`core/tests/CMakeLists.txt:910`.

**ABI 영향**: 제거 심볼이 있으므로 SONAME 호환이 깨진다.

### 12.2 버전: **0.13.0 (D9, 소유자 결정 — 확정)**

버전은 **0.13.0**으로 올린다. 이 절은 권고가 아니라 결정 사항의 기록이다.

**바꿔야 하는 곳**
- `VERSION`
- core `CMakeLists.txt` (프로젝트 버전 + SONAME)
- `core/include/zlink/common.h`의 `ZLINK_VERSION_MINOR` (현재 12 [확인],
  `common.h:11`)
- 전 바인딩 매니페스트: `bindings/dotnet/**/*.csproj`·NuGet 메타,
  `bindings/java` Gradle/Maven, `bindings/node/package.json`,
  `bindings/python/pyproject.toml`, `bindings/rust/Cargo.toml`,
  `bindings/go` 버전 상수, `bindings/cpp`·`bindings/c` CMake
- 패키징 메타데이터: Debian(`core/packaging/debian/changelog`), RPM, NuGet —
  `check_public_surface.py` §5가 CMake 버전·SONAME과의 일치를 검사한다 [확인]

**릴리스 절차 (0.12.0과 동일)**
1. `core/v0.13.0` 태그
2. `build.yml` workflow dispatch (GitHub Actions)

**정합성 근거** [확인]: `core/v0.12.0` 태그가 이미 히스토리에 존재하고
(2026-08-23), 공개 C 표면에서 심볼이 **제거**되므로 SONAME 호환이 깨진다.
0.x에서 breaking change의 정식 표현은 minor 증가다. 즉 D9는 semver 규율과
일치하며, 0.12.0을 재사용했다면 "같은 버전, 다른 ABI"가 되어 바인딩 8종의
버전 판별이 깨졌을 것이다.

`CHANGELOG.md`의 `## [Unreleased]` 섹션 [확인]에 이 변경(`send_ready` 계열
제거 + `send_complete` 계열 추가)을 breaking으로 기재한다.

### 12.3 순서

**Phase 1 — Core (additive)**
1. 정식 스펙 ko/en에 새 C 블록 추가 (`core/doc/spec/core/socket/*.md`,
   `05-events.*.md`, `07-monitoring.*.md`).
2. 헤더 + 구현: pending 레코드 큐(소켓당, §4.7), admit 루프(async mailbox 통합),
   deadline 위임, 콜백 디스패치, 취소. PAIR/PUB/XPUB/DEALER/ROUTER/STREAM
   전체에 `zlink_send_async`/`zlink_publish_async`/`zlink_send_complete_handler`를
   연다 (Q8, §13 해결).
3. `ZLINK_POLLCOMPLETION` 등록 가능 소켓 타입 검사(`zlink.cpp:249-253`)를
   "reply completion 또는 send completion이 있는 소켓" 기준으로 넓힌다
   (Q8/Q10 상호작용, §5.2, §13 해결). `zlink_routed_target_writable`은
   추가하지 않는다 (Q6, §13 해결).
4. Core 계약 테스트 신규: 완료 1회성, target FIFO, 취소 경합, close fail-fast,
   pending 상한 → `BACKPRESSURED`, 콜백/POLLCOMPLETION 디스패치 소유권
   상호배타, 콜백 내 send `EDEADLK`.
5. 이 시점까지 `send_ready`는 **살아 있다** — 롤백 가능 지점.

**Phase 2 — Core (removal) + 버전**
6. `send_ready` 6개 표면 제거, `socket_dispatch_bridge`의 hint coalescing 제거,
   `zlink_select_routed_submit_target` 헤더 문구 개정.
7. Core 테스트 11개 재작성/삭제 (§8.3).
8. 0.13.0 버전 반영 (§12.2의 전체 목록: `VERSION`, core `CMakeLists`,
   `common.h`, 전 바인딩 매니페스트, Debian/RPM/NuGet 메타), `CHANGELOG.md`
   breaking 항목 기재, `check_public_surface` 통과 확인.
9. `SEND_FLOW_*` 문서 문구 강화 (§9.3): 헤더와 `05-events.{ko,en}.md`에
   "진단용이며 재시도 트리거로 쓰지 말 것" 명시. 함께
   `07-monitoring.{ko,en}.md:340-343`의 aggregate-on-full 서술을 실제 구현
   (drop-on-full)에 맞게 정정 (§9.4).

**Phase 3 — C++**
10. 현재 sync 기반 위에 `async()` 재추가 (send/publish),
    request에 `submit()` 블로킹 + `submit(callback)` 추가.
11. `socket.cpp`의 `send_ready` 트램폴린과 `set_send_ready_handler` 재노출
    6개소 제거, `socket_callback_state.hpp`의 attempt mutex 제거.
12. C++ 계약 테스트 + DEALER_DEALER perf 재측정 (0.12.0 사이클 baseline 대비).

**Phase 4 — 6개 언어** (독립 병렬 가능, 각 언어 완결)
13. .NET → Java/Kotlin → Node → Python → Rust → Go.
    각 언어: admission 모듈 삭제 → 자체 스레드/타이머 제거 →
    terminal을 Core completion에 연결 → 심볼 allowlist 갱신 →
    admission 테스트 삭제, completion 계약 테스트 추가.
    **순서 근거**: .NET/Java가 규모가 가장 커서(1836/2470 L) 먼저 패턴을 확립하고,
    Go가 가장 작아서 마지막 검증에 적합하다.
    **Go만 추가 작업**: send terminal 반환 타입을 `<-chan error` → `error`로
    좁히고(D6/D10) 스펙·예제·테스트를 함께 고친다. 다른 다섯 언어는 표면 불변이다.

**Phase 5 — 스펙 재개정**
14. `async-coroutine-policy.{ko,en}.md` 재개정 (§10),
    `bindings/doc/spec/README.{ko,en}.md:1302-1332`,
    언어별 README 9쌍, `bindings/doc/reference/{cpp,node,rust}/*` 6개 [확인].
15. `doc/plan/cpp-routed-async-contract-issue.ko.md`에 §3.3으로 이 결정 링크.

**Phase 6 — 전체 스모크**
16. 전 언어 contract + 통합 스위트, sanitizer(TSan 필수 — 새 pending 큐가
    3개 스레드 컨텍스트에서 접근된다), cross-language interop,
    perf 재측정(DEALER_DEALER single/multi, PUB/SUB, REQ/REP 전 언어).

### 12.4 롤백 지점

Phase 1 종료 시점(추가만 완료, 제거 전)이 유일한 안전 롤백 지점이다.
Phase 2 이후는 8개 언어가 동시에 깨지므로 전진만 가능하다.

---

## 13. 열린 질문 — **해결됨 (소유자 결정, 2026-08-23)**

이 절은 원래 열린 질문 목록이었다. 아래 Q1~Q10 전부 소유자 판단으로
닫혔다. 각 항목은 판단 근거를 남기기 위해 질문 형태를 그대로 유지하고
"해결"란에 한 줄 결론과 근거를 적는다. §4·§5·§7·§11·§12의 본문은 이
결론에 맞춰 이미 갱신했다 — 이 표는 기록용이며 새 결정을 만들지 않는다.

| # | 질문 | 문서 위치 | 해결 |
|---|---|---|---|
| **Q1** | 같은 target에 pending이 있을 때 동기 `submit()`의 앞지르기를 허용할 것인가, `INVALID_STATE`로 거절할 것인가 | §4.4 | **특례 없음.** 동기냐 비동기냐에 상관없이 걸리는 것은 같은 HWM 하나다 — Core는 공간이 생기면 target별 stored pending을 FIFO로 admit하고, 동기 submit은 그 순간 같은 target의 다른 생산자와 동등하게 경쟁한다. target별 순서 보장은 **async pending 사이에서만** 성립한다 (소유자: "동기냐 비동기냐에 상관없이 HWM이 걸리는 것" — 거절 규칙 없음) |
| **Q2** | close 시 pending drain 옵트인(`SEND_PENDING_LINGER_MS`)을 제공할 것인가 | §4.6 | **옵트인 없음, fail-fast만.** 선례: `socket_base_lifecycle`의 close/term fail-fast 형태와 정확히 같은 모양이며, `LINGER`는 이미 admit된 바이트에만 적용된다는 기존 계약과 대칭이다 |
| **Q3** | pending 상한을 소켓당만 둘 것인가, target당 부분 상한도 둘 것인가 | §4.7 | **소켓당 상한만.** 더 단순하고, 소켓 스코프 옵션(`ZLINK_OPT_*`)이라는 기존 선례와 일치한다 |
| **Q4** | 즉시 성공 시 수신 채널 소비자도 이벤트를 거치게 할 것인가(대칭) 아니면 우회 신호를 줄 것인가(비대칭·빠름) | §4.9 | **인라인 fast-path 콜백 유지.** 실측(부기 비용이 `async()` 비용의 절반 이상)이 균일성보다 fast path 보존을 지지한다 |
| **Q5** | `ZLINK_MONITOR_EVENT_FLAG_SEND_FLOW_WRITABLE` 비트를 제거할 것인가 | §9.3 | **유지, 문구만 강화.** 헤더/스펙에 "진단용, 재시도 트리거로 쓰지 말 것"을 명시한다 |
| **Q6** | `zlink_routed_target_writable`을 이번 범위에 포함할 것인가 | §11.3 | **포함하지 않음.** 불필요한 복잡성이다 — 기존 `DONTWAIT` 프로브(즉 `zlink_send_async` 자체의 즉시 응답)가 그 질의 역할을 겸한다 (소유자: 불필요한 복잡성) |
| **Q7** | 완료 콜백과 수신 채널의 상호배타를 소켓 단위로 강제할 것인가, op 단위로 선택하게 할 것인가 | §5.2 | **소켓 단위.** 선례: monitor의 기존 콜백/수신 상호배타 `EBUSY` 규칙과 같은 모양이다 |
| **Q8** | `zlink_send_async`를 raw PAIR/STREAM에도 열 것인가, routed(DEALER/ROUTER) + PUB/XPUB로 제한할 것인가 | §4.2 | **PAIR/STREAM 포함 전면 개방.** 단일 `send_complete` 계열로 병합한 것의 자연스러운 귀결이다. 부수 결론: `ZLINK_POLLCOMPLETION`의 등록 가능 소켓 타입 검사(`zlink.cpp:249-253`, 현재 DEALER/ROUTER 전용)를 이 확장에 맞춰 넓혀야 한다 (§5.2) |
| **Q9** | 완료 결과 enum을 D7의 3값(`ADMITTED`/`TIMED_OUT`/`TERMINAL`)으로 유지하고 취소·close·ctx term을 `terminal_errno`로 구분할 것인가, 별도 결과값을 더 둘 것인가 | §4.2 | **3값 유지.** 선례: request 결과 판별이 이미 이 형태(값 축소 + errno 축 분리)다 |
| **Q10** | 완료 통지를 monitor 스트림에 얹을 것인가, 별도 완료 큐(수신 API)를 새로 만들 것인가 | §5.2 | **둘 다 아니다 — monitor에도 얹지 않고 새 큐도 만들지 않는다.** 기존 `ZLINK_POLLCOMPLETION` 패턴(`core/doc/spec/core/06-polling.ko.md:140-150` [확인]이 서술하는, request reply completion이 오늘 쓰는 바로 그 메커니즘)을 그대로 재사용한다. `ZLINK_POLLCOMPLETION`으로 소켓을 등록하면 `zlink_poller_wait()`가 이미 등록된 `zlink_send_complete_handler` 콜백을 poller 호출 스레드에서 디스패치한다. 새 채널이 아니라 "async-mailbox 디스패치 vs poller-owned 디스패치"라는 하나의 축이며, 무손실 논거(POLLCOMPLETION은 큐가 아니라 디스패치 소유권 이전이므로 유실이 구조적으로 불가능하다)는 유지된다. §5(완료 이벤트 채널 설계)를 이 방향으로 다시 썼다 |

---

## 14. 이 문서가 확인한 것과 제안한 것

**코드로 확인한 것 (설계 전제)**
- Core는 소켓당 비동기 실행 컨텍스트(async mailbox on io_thread)를 이미 갖는다.
- blocking send 대기 루프는 재사용 가능한 구조로 이미 분리되어 있다.
- part 배열 규약은 수신측 공개 API에 이미 존재한다.
- "콜백 + 수신"의 이중 채널은 timer/monitor에 이미 두 선례가 있다.
- "poll 소비자가 자기 스레드에서 완료를 드레인"은 `ZLINK_POLLCOMPLETION`으로
  이미 구현되어 있다.
- Core 소유 one-shot deadline 스케줄러가 둘 있다.
- `SEND_FLOW_*`는 원격 flow-state 관측이며 재시도에 쓰는 소비자가 0이다.
  Core 안에서 관측(`flow_state_applied`)과 readiness(`write_activated`)가
  이미 별개의 호출로 분리되어 있고, 공개 monitor가 lossy이며 종료 시 RESUMED가
  생략되므로 애초에 hint로 쓸 수 없는 채널이다.
- routed 소켓의 POLLOUT은 실질적으로 정보를 주지 못한다(ROUTER 비-mandatory는
  항상 true, DEALER는 소켓 집계).
- 바인딩 6종에 약 8,400 L의 admission 기계장치와 5종의 자체 스레드가 있다.

**소유자가 고정한 것 (재논의 없음)**
- D1~D6 (§2), 그리고 D7 `send_complete` 계열 명명과 두 핸들러의 단일 계열 병합,
  D8 "완료 = admission이지 peer 전달이 아님", D9 0.13.0 버전과 릴리스 절차,
  D10 언어 관용 우선 정책과 Go의 동기 `Submit(ctx) -> error`.
- §13 Q1~Q10 전부 (2026-08-23 소유자 결정으로 해결·기록됨).

**이 문서가 제안한 것 (미구현)**
- `zlink_send_async` / `zlink_publish_async` 배열 제출 API와 그 계약 전부
  (identity, 순서, 취소, close, timeout, pending 상한), PAIR/PUB/XPUB/
  DEALER/ROUTER/STREAM 전체 대상(Q8).
- `zlink_send_complete_event_t`의 구체 필드 배치와 `zlink_send_complete_handler`
  단일 등록 API. "수신 채널"은 새 API가 아니라 `ZLINK_POLLCOMPLETION` 등록을
  통한 디스패치 위치 이전이며(Q10), 그 등록 가능 소켓 타입 검사도 Q8에 맞춰
  넓혀야 한다.
- `ZLINK_OPT_SEND_PENDING_MAX_*`.
- 완료 콜백 컨텍스트 계약과 `EDEADLK` 강제.
- 6-Phase 마이그레이션 순서.

**이 문서가 명시적으로 채택하지 않은 것**
- `zlink_send_complete_recv`(전용 완료 큐 수신 API), `ZLINK_EVENT_SEND_COMPLETE`,
  `ZLINK_POLLSENDCOMPLETE` — Q10 해결로 기존 `ZLINK_POLLCOMPLETION` 재사용에
  자리를 내줬다.
- `zlink_routed_target_writable` — Q6 해결로 불필요하다고 판단했다.
- close-drain 옵트인(`ZLINK_OPT_SEND_PENDING_LINGER_MS`) — Q2 해결.
- pending 상한의 target당 부분 상한 — Q3 해결.
