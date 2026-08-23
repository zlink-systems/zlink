# Core send_complete 구현 — send_ready 제거, 0.13.0 버전 반영 (2026-08-23)

> 지배 문서: `doc/plan/core-send-completion-design.ko.md` (D1~D10, Q1~Q10 확정본)
> 브랜치 / 시작 HEAD: `codex/bindings-0.12.0-performance` / `a650e68d7f`
> 커밋 없음. 전부 작업 트리에 남긴다.
> 동반 로그: `log/2026-08-23-auto-hwm-retune.md` (GROUP C 상세)

이 로그는 네 묶음(A: send_complete 추가, B: send_ready 제거, C: auto-HWM
재조정, D: 0.13.0 버전)을 각각 분리 가능한 형태로 구현한 결과를 기록한다.

---

## GROUP A — `zlink_send_async` / send completion

### A.1 공개 표면 (추가)

`core/include/zlink/socket/api.h`

```c
typedef enum zlink_send_complete_result_t {
  ZLINK_SEND_ADMITTED = 0, ZLINK_SEND_TIMED_OUT = 201, ZLINK_SEND_TERMINAL = 202
} zlink_send_complete_result_t;
typedef uint64_t zlink_send_op_id_t;
typedef struct zlink_send_complete_event_t { ... } zlink_send_complete_event_t;
typedef void (*zlink_send_complete_handler_fn) (void *, const zlink_send_complete_event_t *, void *);
typedef struct zlink_send_async_options_t { ... } zlink_send_async_options_t;

zlink_submit_result_t  zlink_send_async (void *, zlink_msg_t *, size_t,
                                         const zlink_send_async_options_t *,
                                         zlink_send_op_id_t *);
zlink_handler_result_t zlink_send_complete_handler (void *, zlink_send_complete_handler_fn, void *);
zlink_submit_result_t  zlink_send_async_cancel (void *, zlink_send_op_id_t);
```

`core/include/zlink_enum.h`에 `ZLINK_OPT_SEND_PENDING_MAX_MSGS = 0x303A`,
`ZLINK_OPT_SEND_PENDING_MAX_BYTES = 0x303B`. `src/libzlink.vers`에서
제거 심볼 2개를 빼고 신규 3개를 추가했다.

### A.2 런타임

새 파일 `core/src/runtime/sockets/common/socket_send_complete.cpp`가
admission 기계장치 전부를 소유한다. 상태는
`socket_send_pending_runtime_t`(`socket_runtime.hpp`)로 socket_runtime에 붙였다.

- **레코드 인계**: `send_async_submit()`이 `parts_` 배열을 값으로 복사해
  보관하고 호출자 배열을 `msg_init()`으로 비운다. 이후 close는 Core가 한다.
  실패 경로에서는 복사 전에 반환하므로 소유권이 호출자에게 남는다.
- **target별 FIFO**: `queues`가 `routed_send_target_key_t -> deque<record*>`다.
  plain 소켓(PAIR)은 기본 생성 key 하나를 공유한다. admit 루프는 각 큐의
  head만 본다 — target 내부 head-of-line은 의도된 동작이다.
- **admit**: `try_admit_send_pending()`이 레코드 하나마다
  `begin_public_send_scope(true)`를 한 번 잡고 part를 `DONTWAIT` +
  (마지막이 아니면) `SNDMORE`로 기존 `send_direct_with_retry()`에 넘긴다.
  새 admission 규칙을 만들지 않았다 — 동기 send와 정확히 같은 byte HWM을
  통과한다. 첫 part가 `EAGAIN`이면 아무것도 쓰이지 않았으므로 pending 유지,
  두 번째 이후 part가 실패하면 `rollback_scoped()`로 전체를 되돌린다.
- **구동**: `process_async_mailbox()` 루프에서 기존
  `dispatch_routed_send_ready_events()` 자리에 `drive_send_pending()`을 놓았다.
  wake는 기존 `write_activated` → (구) `enqueue_routed_send_ready(WRITABLE)`
  자리에 `notify_send_pending_writable()`을 놓아 메일박스 command로 그대로
  이어받았다. **새 스레드 0개.**
- **timeout**: `zlink-req-time` 전역 lazy-start/idle-exit 스케줄러
  (`request_timeout::schedule`)를 재사용한다. 만료와 admit의 경합은 레코드의
  `claimed` 플래그 1회 CAS로 결정한다. **새 스레드 0개.**
- **취소**: `send_async_cancel()`이 `claimed`를 확인해 이미 admit 중이면
  `EBUSY` → 공개 API가 `ZLINK_SUBMIT_INVALID_STATE`로 매핑한다. 그 외에는
  즉시 `TERMINAL`/`ECANCELED`로 완료시킨다. **취소해도 완료는 정확히 1회.**
- **close / ctx term**: `fail_all_send_pending()` +
  `dispatch_send_completions(true)`를 `finish_close_handoff()`,
  `finish_deferred_close_after_async_quiesced()`, `process_stop()`의 기존
  종결 지점에 얹었다. close는 통지를 전부 내보낸 뒤 반환한다. LINGER 미적용.
- **bounded pending**: `options.send_pending_max_msgs`(기본 1024),
  `send_pending_max_bytes`(기본 `ZLINK_HWM_BYTES_DFLT`). 초과 시 즉시
  `BACKPRESSURED`이고 소유권은 호출자에게 남는다. `0`은 무제한이 아니라
  거부값이다.
- **인라인 fast path (Q4)**: `send_async_submit()`이 끝에서
  `drive_send_pending()` + `dispatch_send_completions_if_local()`을 호출한다.
  여유가 있으면 스레드 홉 0회로 admit되고 완료 콜백이 호출 스택에서 돈다.

### A.3 콜백 계약 강제

`socket_send_ready_dispatch_scope_t`를 `socket_send_complete_dispatch_scope_t`로
바꾸고 `dispatching_any()`를 추가했다. `send_async_submit()`은 이 스코프 안이면
`EDEADLK`를 반환한다. 핸들러 자기 교체도 같은 스코프로 `EDEADLK`다.
소켓당 직렬성은 기존 `socket_callback_scope_t`가 그대로 보장한다.

### A.4 `ZLINK_POLLCOMPLETION` 확장

`core/src/api/core/zlink.cpp`의 등록 소켓 타입 검사를 "reply completion이
있거나(DEALER/ROUTER) send completion이 있는 소켓"으로 넓혔다
(`zlink::socket_type_supports_send_completion()`). 등록 시
`acquire_completion_poller()`가 reply completion과 send completion **둘 다의**
디스패치 소유권을 poller wait 스레드로 옮긴다:

- `socket_base_lifecycle.cpp`의 async mailbox 루프는 owner gate 안에서만
  `dispatch_send_completions(false)`를 부른다 (`_completion_poller_refs == 0`).
- `get_events()` / `get_events_for_poller()`의 `ZLINK_POLLCOMPLETION` 분기가
  `drive_send_pending()` + `drain_send_completions()`를 함께 돌린다.
- 그 밖의 모든 완료 지점은 `dispatch_send_completions_if_local()`을 거쳐
  poller가 소유 중이면 디스패치하지 않는다. 상호배타는 소켓 단위다 (Q7).

---

## GROUP B — send_ready 제거

제거된 공개 심볼: `zlink_send_ready_handler`, `zlink_routed_send_ready_handler`.
제거된 공개 타입: `zlink_send_ready_handler_fn`,
`zlink_routed_send_ready_handler_fn`, `zlink_routed_send_ready_event_t`.
제거된 enum/enumerator: `zlink_routed_send_ready_state_t`,
`ZLINK_ROUTED_SEND_WRITABLE`, `ZLINK_ROUTED_SEND_TERMINAL`.

내부 정리:

- `socket_dispatch_bridge_t`에서 hint 핸들러 3중 슬롯(seqlock),
  `send_ready_armed`, coalescing 맵(`routed_send_ready_pending`/`_terminal`)을
  전부 삭제했다. 남은 것은 `send_recovery_pending_flag` /
  `send_recovery_ready_flag` 둘뿐이고 `has_out()`이 계속 이 축을 쓴다.
- `enqueue_routed_send_ready*` / `dispatch_routed_send_ready_events` /
  `enqueue_all_routed_send_terminals` 호출부 전부를
  `notify_send_pending_writable` / `fail_send_pending_for_pipe` /
  `fail_send_pending_for_target` / `fail_all_send_pending` /
  `dispatch_send_completions`로 교체했다 (`socket_base_api.cpp`,
  `socket_base_routing.cpp`, `router.cpp`, `stream.cpp`).
- `command_t::routed_send_ready` → `command_t::send_pending`.
- `arm_send_ready_notification`은 hint 콜백을 깨우는 것이 유일한 목적이었으므로
  삭제했고, `arm_send_ready_after_backpressure`는
  `arm_send_recovery_after_backpressure`로 이름만 바꿔 유지했다(메일박스 signal
  기능이 blocking send와 request 경로에 계속 필요하다).

**`SEND_FLOW_PAUSED`/`RESUMED` monitor 이벤트와
`ZLINK_MONITOR_EVENT_FLAG_SEND_FLOW_WRITABLE` 비트는 그대로 유지했다** (D 결정
§9). 이 축은 원격 flow-state 관측이며 readiness hint가 아니다.

grep 증거:

```
$ cd core && grep -rn "send_ready\|ROUTED_SEND_" include/ src/ tests/
$ echo $?
1
```

### B.1 Core 테스트 재작성

11개 파일을 completion 계약으로 옮겼다. 공통 헬퍼
`core/tests/testutil_send_complete.hpp`를 새로 두었다.

| 파일 | 처리 |
|---|---|
| `test_socket_with_handler.cpp` | `test_raw_socket_send_ready_contracts` → `..._send_complete_contracts`. PAIR/DEALER/ROUTER/STREAM은 OK, PUB/XPUB/SUB은 `ENOTSUP` |
| `test_router_mandatory_hwm.cpp` | readiness 격리 테스트를 completion 격리로 재작성. A를 backpressure → 레코드 예약 → B는 인라인 ADMITTED(격리 증명) → A 드레인 시 A만 ADMITTED → `disconnect_rid` 시 A의 예약 레코드가 `TERMINAL/ENOTCONN` → close 시 B가 `TERMINAL/ECANCELED`. self-close 배치 테스트는 두 target을 backpressure시켜 pending을 만든 뒤 `ctx_shutdown`으로 구동 |
| `test_routed_submit_target.cpp` | same-RID handover 테스트가 backpressure 직후 exact target에 레코드를 예약하고, handover 뒤 그 레코드의 `TERMINAL`과 pair identity를 단언. terminal 사유는 admit 시도(`EHOSTUNREACH`)와 pipe termination(`ENOTCONN`) 중 먼저 알아챈 쪽이 정하므로 둘 다 허용한다 |
| `test_stream_send_blocking_wakeup.cpp` | WRITABLE 관측 → 실제 레코드의 ADMITTED 완료, TERMINAL 관측 → 예약 레코드의 TERMINAL. `..._send_ready_pollout_share_recovery_axis` → `test_stream_pollout_tracks_send_recovery_axis`(POLLOUT 절반만 남음) |
| `test_zmp_request_reply.cpp` | `ignore_routed_ready` 시그니처 교체, reply backpressure 재시도 루프의 readiness 게이트를 완료 드레인 기반으로 교체. `test_router_exact_request_to_dealer_completes_on_async_owner`의 "writable 배리어"는 exact target 선택 자체가 이미 transport admission을 증명하므로 삭제했고, 같은 테스트에 붙어 있던 terminal-callback self-close 단언은 pending operation이 있어야 성립하므로 `test_router_mandatory_hwm`의 terminal-batch self-close 테스트(레코드를 예약한 뒤 구동)로 일원화했다 |
| `test_monitor_perf_contract.cpp` | send-ready 콜백 self-close 2건을 completion 콜백 self-close로 재작성(backpressure → 예약 → 드레인 → 완료에서 self-close). startup-failure 테스트는 핸들러 1개 기준으로 축소 |
| `unittest_socket_runtime.cpp` | dispatch scope 테스트를 `socket_send_complete_dispatch_scope_t` + `dispatching_any()`로 이관. 사라진 bridge 핸들러 테스트 2개는 `test_socket_dispatch_bridge_tracks_send_recovery_edges`와 `test_socket_send_pending_runtime_starts_empty`로 대체 |
| `test_stream_threadsafe.cpp`, `test_stream_socket.cpp`, `test_reconnect_options.cpp`, `test_thread_safe_contract_policy.cpp` | 핸들러 시그니처·이름 교체. `test_reconnect_options`의 hint 카운터는 관측 대상이 사라졌으므로 삭제하고, 실제 관측치(첫 target attach 뒤 DONTWAIT submit이 통과한다)는 그대로 유지 |

`core/tests/CMakeLists.txt`의 테스트 이름 4개도 `send_complete`로 갱신했다.

---

## GROUP C — auto-HWM 재조정

상세는 `log/2026-08-23-auto-hwm-retune.md`. 요약:

- profile 표: Compact 2%/64 MiB/32 KiB/512 KiB, LowLatency 3%/256 MiB/32 KiB/2 MiB,
  Balanced 5%/512 MiB/64 KiB/1 MiB, Throughput 8%/1024 MiB/128 KiB/8 MiB.
  STREAM 열은 유지(어떤 profile도 data 상한이 STREAM 상한 아래로 내려가지 않음).
- 산식: `budget = min(percent × resolved_memory,
  max(fixed_cap, active_directional_queue_count × per_queue_minimum))`.
- 계산 위치: `plan_make`는 queue count 0(=고정 cap)으로 seed하고,
  `auto_hwm_context_finalize`가 pass-1에서 queue count를 다 센 직후 data budget을
  나누기 전에 재계산한다. registry가 finalize 후 덮어쓰는 값과 동일하다.
- 문서: `core/doc/spec/core/01-context.{ko,en}.md`에 고정 cap 열과 산식 추가.

---

## GROUP D — 0.13.0

`VERSION`을 0.13.0으로 올리고 `scripts/local-package/sync-version.py --write`를
돌렸다(47개 파일). 이 스크립트가 관리하지 않는 곳은 손으로 맞췄다.

- packaging: `core/packaging/debian/{changelog,zlink.dsc}`,
  `core/packaging/redhat/zlink.spec`,
  `core/packaging/nuget/{package.config,package.nuspec,package.targets}`
  (`0.12.0` → `0.13.0`, `0_12_0` → `0_13_0`).
- 산문 참조: `core/doc/spec/core/07-monitoring.{ko,en}.md`, 각 바인딩 README·
  doc.go·ffi.rs·source_layout 테스트 등 `44cbf3b803`이 다뤘던 목록 그대로.
- `CHANGELOG.md`의 `## [Unreleased]`에 breaking/added/fixed/changed 기재.
- `doc/perf/**`와 `doc/plan/**`은 이력 문서이므로 0.12.0 표기를 그대로 두었다.

게이트:

- `scripts/local-package/sync-version.py --check` → **PASS** (0 changed).
  참고로 작업 시작 시점에는 `framework/**`와 `scripts/local-package/*/fixtures/**`
  7개 파일이 0.11.1로 남아 드리프트 상태였고, `--write`가 함께 정리했다.
- `core/tests/contract/check_public_surface.py` → **PASS**
  (`functions=121 exports match, removed identifiers absent`).
  SONAME은 `0`으로 유지된다(하드코딩). 심볼 제거가 있으므로 ABI 호환은 깨진다.

---

## 설계 문서와의 차이 (deviation)

설계 문서 §4.2의 시그니처는 명시적으로 **초안**이며, 계약은 동일하게 두고 아래를
조정했다. 각 항목은 supervisor 확인 대상이다.

1. **`zlink_publish_async`를 추가하지 않았다.** 설계 문서 §4.2/§12.1은 이 함수를
   목록에 넣지만, 이번 작업 지시의 GROUP A는 대상 소켓을
   "PAIR/STREAM/DEALER/ROUTER"로 명시했다(PUB/XPUB 제외). 지시를 따라
   `zlink_send_async` 하나만 열었고, `zlink_send_complete_handler`도 같은 집합에서만
   성공한다. PUB/XPUB/SUB은 `ENOTSUP`이다. → PUB/XPUB async publish가 필요하면
   별도 작업으로 열어야 한다.
2. **STREAM 레코드는 1 part로 제한**했다. STREAM은 프레임 경계가 없는 raw 바이트
   스트림이라 "완전한 멀티파트 레코드"라는 개념이 없다. `part_count_ != 1`은
   `ZLINK_SUBMIT_NOT_SUPPORTED`.
3. **DEALER의 target 미지정 제출은 제출 시점에 선택을 확정**한다(설계 §4.4 그대로).
   그 시점에 선택 가능한 pipe가 없으면 pending으로 예약하지 않고
   `ZLINK_SUBMIT_NOT_CONNECTED`를 반환한다. 순서 보장을 정의하려면 target이
   먼저 정해져야 하므로 "target 미정 pending"은 두지 않았다.
4. **완료 핸들러 미등록 시 `zlink_send_async`는 `EINVAL`로 실패**한다. 설계 문서는
   "등록 전이면 완료가 갈 곳이 없다"고만 적었고 반환값을 정하지 않았다.
   조용히 성공시키면 op가 영원히 미완료로 남으므로 거부로 확정했다.
5. **취소 반환값 매핑**: 설계표의 `ZLINK_SUBMIT_INVALID_STATE`(이미 커밋)는
   내부 `EBUSY`로 신호하고 공개 API에서 매핑한다. `NOT_FOUND`는 `ENOENT`.
6. **`zlink_send_async`는 같은 핸들에 진행 중인 part 시퀀스가 있으면 `EINVAL`**로
   거절한다. 설계 문서는 이 상호작용을 다루지 않았다. 한 호출 안에서 gate를
   잡고 놓는다는 §4.1의 목적을 유지하려면 진행 중인 시퀀스와 섞일 수 없다.
7. **콜백 재진입 금지는 "이 소켓"이 아니라 "임의 소켓"** 기준이다
   (`dispatching_any()`). 설계 §6.2가 금지하는 것은 콜백 안에서의 submit 자체이고,
   다른 소켓으로 우회하면 같은 문제(Core 콜백 스레드에서의 재시도 루프)가
   재현되므로 넓게 잡았다.
8. **내부 식별자에서도 `send_ready`를 전부 제거**했다. 설계 §8.1은
   `arm_send_ready_notification` 등을 "내부 유지"로 표시했지만, 작업 지시의
   grep 증거 요구(`core/include`·`core/src`에 `send_ready` 심볼 0건)를 만족시키기
   위해 `send_recovery` 계열로 개명했다. 동작은 동일하다.
9. **`zlink_send_op_id_t`를 `typedef uint64_t`로 두었다** — 설계 초안 그대로다.
   공개 표면 게이트가 이 형태를 TYPE으로 인식하는 것을 확인했다.

---

## 범위 밖에서 발견해 고친 것 (pre-existing regression)

**단일 part exact-transport-pair ROUTER submit이 항상 `EHOSTUNREACH`로 실패했다.**

`5d2bf1e84f`가 `router_send_path.cpp`의 `xsend_routed`에서
`if (scoped_pipe)`를 `if (scoped_pipe && _more_out)`로 바꿨다. exact pair를
해석하면 `out_pipe`가 의도적으로 `NULL`이 되는데, 단일 part 레코드는
`_more_out == false`이므로 if/else 사슬이 `else if (_mandatory)`(= "경로 없음")로
떨어졌다. ROUTER는 `_mandatory`가 기본 true다.

재현(수정 전, 이 작업과 무관한 기존 동기 API):

```c
zlink_select_routed_submit_target (router, &rid, &target);  /* OK */
zlink_send_part_transport_pair (router, &target.peer_rid,
    target.transport_pair_id, target.transport_pair_generation,
    &part, ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL);
/* → ZLINK_SUBMIT_NOT_CONNECTED, errno == EHOSTUNREACH */
```

수정: `else if (_mandatory)` → `else if (!scoped_pipe && _mandatory)`. 경로를
아예 못 찾은 경우에만 unreachable이다. `scoped_pipe && _more_out` 분기(멀티파트
선검사)는 그대로 둔다.

이 회귀가 `test_router_mandatory_hwm` / `test_routed_submit_target` 계열의
기존 실패를 설명한다. `zlink_send_async`의 ROUTER 경로가 정확히 같은 코드를
쓰므로 고치지 않으면 GROUP A의 ROUTER 대상이 동작하지 않는다.

---

## 테스트 결과

### 빌드

`cmake -S core -B core/build -DBUILD_TESTS=ON -DZLINK_BUILD_TESTS=ON
-DCMAKE_BUILD_TYPE=Release` → 전체 빌드 **성공, 에러 0, 경고 0**.

> 환경 주의: 이 호스트(WSL2, 16 논리코어, 11 GiB RAM)에서 `-j16`은 정적
> `libzlink.a`를 링크하는 테스트 바이너리 76개가 동시에 뜨면서 메모리를
> 소진해 사실상 정지한다(load 190+). `-j4`로 낮추면 정상 진행한다.

### Core 테스트 스위트 (`ctest`, 91개)

**83 passed / 8 failed. 새로 생긴 실패 0건.** 8건 전부 이번 변경 이전부터
실패하던 것이고, 그중 5건은 `git worktree`로 뜬 **HEAD(`a650e68d7f`) 원본
트리에서 같은 실패를 직접 재현해 확인**했다.

| # | 테스트 | 결과 | 판정 |
|---|---|---|---|
| 10 | `test_ctx_destroy` | SEGFAULT (`test_ctx_term_with_open_socket_monitors`) | **기존 실패 — baseline 재현 확인** |
| 14 | `test_multi_socket_contract_regressions` | SEGFAULT | **기존 실패 — baseline 재현 확인** |
| 33 | `test_zmp_request_reply` | 38 tests / 1 failure (`test_blocking_generic_dealer_recv_remains_on_transport_pipe:949`) | **기존 실패 — baseline과 실패 건수·위치 완전 동일** |
| 35 | `test_router_concurrent_routed_recv` | 4 tests / 1 failure (`test_router_routed_recv_serializes_fq_with_pipe_termination:739`) | **기존 실패 — 작업 지시가 명시한 기지 실패** |
| 36 | `test_router_mandatory_hwm` | `..._isolated_by_exact_target_and_terminal_cause:169 Expected 0 Was 204` | **기존 실패 — baseline이 같은 테스트의 이전 버전에서 `:137 Expected 0 Was 204`로 동일 실패.** `drain_one_part()`의 `zlink_recv_part`가 204를 반환하는 문제이며 completion 계약과 무관 |
| 38 | `test_flow_state_c_api` | `-j4` 병렬 실행 시 Timeout | **부하 flake — 단독 실행 시 3.59초 Passed** |
| 39 | `test_routed_submit_target` | 6 tests / 3 failures | **기존 실패이며 이번 변경으로 개선됨.** baseline은 5개 중 4개 실패 + glibc fatal abort. 현재는 ROUTER exact 테스트 2개(`..._rejects_stale_generation`, `..._after_same_rid_handover`)가 **새로 통과**하고 abort도 사라졌다. 남은 3건(`..._peer_churn`, `test_dealer_exact_target_keeps_blocked_a_isolated_from_b`, `test_dealer_exact_multipart_failure_rolls_back_only_target_a`)은 baseline에서도 실패한다 |
| 61 | `test_stream_threadsafe_socket_runtime_reads` | SEGFAULT | **기존 실패 — baseline에서 3회 연속 동일 SEGFAULT 재현 확인** |

관련 스위트는 전부 그린이다:

```
$ ctest -R "unittest_auto_hwm_policy|unittest_socket_runtime|contract_public_surface|contract_c_header_mirror|test_socket_with_handler|test_stream_send_blocking_wakeup|test_reconnect_options|test_stream_socket|test_thread_safe_contract_policy|test_monitor_perf_contract|test_stream_threadsafe$"
100% tests passed, 0 tests failed out of 17
```

### 게이트

```
$ python3 core/tests/contract/check_public_surface.py . core/build/lib/libzlink.so
PUBLIC SURFACE CONTRACT: PASS
functions=121 exports match, removed identifiers absent

$ python3 core/tests/contract/check_c_header_mirror.py .        # (contract_c_header_mirror)
(출력 없음, exit 0)

$ python3 scripts/local-package/sync-version.py --check
Core/binding version 0.13.0 verified (0 changed file(s))

$ cd core && grep -rn "send_ready\|ROUTED_SEND_" include/ src/ tests/ ; echo $?
1
```

### send_async 기능 테스트 (throwaway)

`scratchpad/func_send_async.c` — 공개 C API만 사용하는 독립 프로그램,
`libzlink.so`에 직접 링크. **30개 체크 전부 통과 (2회 연속).**

| 경로 | 확인한 것 |
|---|---|
| 인라인 admit | 여유가 있으면 `zlink_send_async`가 반환하기 전에 완료 콜백이 이미 실행됐고 `op_id`가 부여된다. 레코드가 peer에 도달한다 |
| HWM-full pending | 동기 send로 byte HWM을 채운 뒤 submit → `ZLINK_SUBMIT_OK`이고 완료는 아직 없음. peer 드레인 후 그 `op_id`로 `ADMITTED` 완료 |
| pending 상한 | `ZLINK_OPT_SEND_PENDING_MAX_MSGS = 2` → 3번째 submit이 정확히 `BACKPRESSURED` |
| 취소 | `OK` 접수 → `TERMINAL`/`ECANCELED` 1회 → 같은 id 재취소는 `NOT_FOUND` |
| timeout | `timeout_ms = 200`으로 park → `TIMED_OUT` 1회 |
| 콜백 재진입 | 완료 콜백 안에서 `zlink_send_async` → `EDEADLK` |
| close-with-pendings | 3건 park 후 `zlink_close` → 반환 시점에 이미 3건 전부 `TERMINAL`/`ECANCELED` |
| POLLCOMPLETION | ROUTER + `ZLINK_POLLCOMPLETION` 등록 → submit 시 인라인 콜백 **없음**(poller가 소유) → `zlink_poller_wait`가 그 스레드에서 `ADMITTED` 디스패치 → 레코드가 peer에 도달 |
| 미지원 소켓 | PUB의 `zlink_send_complete_handler`가 `ENOTSUP` |

### 구현 중 잡은 자체 결함 3건

1. **use-after-free (crash)**: `send_async_submit()`이 pending mutex를 놓은 뒤에도
   `record->op_id`를 읽었다. mutex를 놓는 순간 admit 루프나 deadline 스레드가
   이미 완료·해제했을 수 있다. op id를 락 안에서 지역 변수로 복사하도록 고쳤다.
2. **poller 소유 중 인라인 디스패치**: `send_async_submit()`이 완료를
   무조건 디스패치해 `ZLINK_POLLCOMPLETION` 등록 상태에서도 콜백이 caller
   스레드에서 돌았다. `dispatch_send_completions_if_local()`을 도입해
   `_completion_poller_refs != 0`이면 건너뛰도록 했다.
3. **close 중 잘못된 terminal 사유**: close가 접수되면 admit 루프의
   `begin_public_send_scope()`가 `ESHUTDOWN`으로 실패하는데, 이를 route 실패로
   취급해 pending을 `ESHUTDOWN`으로 종료시켰다. `ESHUTDOWN`/`ETERM`은 pending을
   유지하도록 바꿔 close/term 경로가 `ECANCELED`/`ETERM`을 확정하게 했다.
   반대로 route 실패(`EHOSTUNREACH` 등)는 **terminal로 유지**한다 — 죽은 exact
   target을 계속 재시도하면 mailbox 스레드가 매 wake마다 public send scope를
   다시 잡아 동시 `zlink_close`가 lifecycle gate를 영원히 못 얻는다(실측으로
   `EBUSY` 확인).


---

## 바인딩 파괴 목록 (후속 작업용)

`bindings/`는 이번 범위 밖이다(그룹 D의 버전 매니페스트 편집 제외). 제거된
Core 심볼을 참조해 **컴파일/게이트가 깨지는 파일**은 다음과 같다.

**벤더링된 Core 헤더 사본 (재동기화 필요, 4개 바인딩)**
- `bindings/c/include/zlink/socket/api.h`, `bindings/c/include/zlink_errno.h`
- `bindings/cpp/include/zlink/socket/api.h`, `bindings/cpp/include/zlink_errno.h`
- `bindings/go/include/zlink/socket/api.h`, `bindings/go/include/zlink_errno.h`
- `bindings/rust/include/zlink/socket/api.h`, `bindings/rust/include/zlink_errno.h`

**.NET**
- `src/Zlink/Runtime/Messaging/RoutedAdmissionScheduler.cs`
- `src/Zlink/Runtime/Native/NativeMethods.Socket.cs`,
  `src/Zlink/Runtime/Native/NativeMethods.Core.cs` (심볼 allowlist)
- `src/Zlink/Runtime/Sockets/SocketKernel.Callbacks.cs`

**Java**
- `runtime/nativeapi/Native.java`, `runtime/sockets/SocketCore.java`
  (`RoutedAdmission.java` / `PublisherAdmission.java` / `StreamAdmission.java`는
  이 심볼을 직접 부르지는 않지만 같은 구조 위에 서 있어 함께 삭제 대상)

**Node**
- `native/src/addon_core.cc`, `native/src/addon_routed_admission.cc`
- `tests/source_layout.test.ts`, `dist-tools/tests/source_layout.test.js`

**Python**
- `src/zlink/_native/ffi.py`, `src/zlink/_runtime/messaging/routed_async.py`,
  `src/zlink/_runtime/sockets/socket_base.py`, `tests/test_native_contract.py`

**Rust**
- `src/runtime/native/ffi.rs`, `src/internal/routed_admission.rs`,
  `src/runtime/sockets/socket/socket_inner_runtime.rs`,
  `tests/optimization_guard_tests.rs`

**Go**
- `internal/native/socket_send_ready.go`, `internal/native/routed_admission.go`
- `tests/raw-core11-allowlist.json` — `coreVersion`은 0.13.0으로 갱신했지만
  `headers[].sha256`은 **갱신하지 않았다**. 이 파일이 해시하는 대상은
  `bindings/go/include/**`의 벤더링 헤더이고, 그 헤더는 아직 Core 0.13.0
  표면으로 재동기화되지 않았다. 헤더를 다시 벤더링하는 Go 후속 작업에서
  해시와 `publicSymbols`를 함께 갱신해야 한다.

**스펙 문서 (Phase 5)**
- `bindings/doc/spec/README.{ko,en}.md`,
  `bindings/doc/spec/async-coroutine-policy.{ko,en}.md`,
  언어별 `bindings/doc/spec/<lang>/README.{ko,en}.md`,
  `bindings/doc/reference/{dotnet,go,java,node,rust}/03-sockets.{ko,en}.md`.

**C++ 작업 트리 주의**: `bindings/cpp` 아래에는 이번 작업과 무관한 미커밋
동기 submit 재정렬 변경이 있다. 이번 작업은 그 트리를 건드리지 않았고
(버전 문자열 제외), 위의 벤더링 헤더 재동기화는 그 작업과 충돌할 수 있다.
