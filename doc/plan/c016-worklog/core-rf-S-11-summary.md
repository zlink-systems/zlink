# S-11 — `pipe_t::_in_active` 소유권 통일 (기존 결함 B)

> 2026-09-07. worktree `~/project/zlink-work/s11` (detached `baaa68d67b` = main, S-1/S-2/S-4/S-9/S-10 포함). **커밋하지 않음.**
> 원본 데이터: `.../scratchpad/S-11/` (`tsan-before-*.log`, `tsan-after-*.log`, `cg_s11.out`, `with_stream*.log`),
> 벤치 결과 `~/project/zlink-work/s11/bindings/c/bench/with_stream/results/20260907_034709`(1차) 및 이후 재측정 2건.

## 1. 결과 (수치)

| 지표 | before (`baaa68d67b`) | after | 판정 |
|---|---|---|---|
| TSan `_in_active` race (`process_activate_read` ↔ `read_internal`) | **보고됨** (`pipe.cpp:2751` 읽기 vs `:1196` 쓰기) | **사라짐** | 완료 조건 충족 |
| TSan `test_two_poller_wake` 전체 | 경고 3건, exit 66 | **경고 0건, exit 0** | 완전 clean |
| callgrind 축소 셀 Ir/msg (S-A §0 방법, CCU 20 / 1024 B / 15 s) | S-1 값 **9,887** | **9,474** | **−4.2 %**(악화 없음) |
| `process_activate_read` 호출/msg | 2.000 | **1.999** | 동일 — wake 진리표 불변의 실측 확인 |
| `mailbox_t::send` 호출/msg | 2.003 | **2.002** | 동일 |
| `stream_t::xread_activated` 호출/msg | 0.999 | **0.999** | 동일 |
| `pthread_mutex_*` 호출/msg | 33.10 | **32.48** | 동일 수준 |

Ir/msg 계산식은 S-1과 동일하다: `(server I refs − 3,453,365 idle) / METRIC recv_msgs`.
같은 식을 S-1의 `cg_after.out`에 적용하면 9,839이므로, 표기 기준 9,887 대비도 동일 식 기준 9,839 대비도 개선이다.

### with_stream (CCU 1000, runs 1)

| size | zlink kops | asio kops | 비 | Phase 0 기준(243.0 등) | load avg |
|---|---:|---:|---:|---|---|
| 64 B | **266.5** | 333.7 | 0.799 | 268.9 대비 −0.9 % | 2.40 |
| 1024 B | **256.2** | 312.6 | 0.820 | 243.0 대비 **+5.4 %** | 2.72 |
| 65536 B | **32.8** | 40.0 | 0.821 | 30.4 대비 **+7.9 %** | 2.83 |

mismatch 전 셀 0. **1차 일괄 실행(20260907_034709)의 64 B·1024 B 값(221.3 / 175.5)은 폐기한다** — 같은 실행의 asio도
64 B에서 202.5(기준 322.0)로 함께 무너졌고 system peak CPU 100 %가 찍혔다. 다른 job의 빌드와 겹친 잡음이다.
크기별 재측정(위 표, asio가 각각 333.7 / 312.6으로 기준 복귀)이 실제 값이며 S-1(287.9 / 258.9 / 32.5)과 같은 수준이다.
이 머신의 load avg가 S-1 측정 시(0.45)보다 상시 높아(2.4~2.9) 절대값 비교는 신뢰구간이 넓다. **부하에 무관한
callgrind Ir/msg가 판정의 근거다.**

## 2. race의 정체 — "한쪽만 잠그는 잠금"이 아니라 **서로 다른 배타 도메인 두 개**

TSan 스택(`tsan-before-test_two_poller_wake.log`)이 원인을 그대로 보여준다. 같은 PAIR socket에 앱 스레드 둘:

| 스레드 | 경로 | 배타 수단 |
|---|---|---|
| T20 (poller) | `zlink_poller_wait` → `get_events_internal` → `process_commands` → `process_activate_read` (`_in_active` 읽기) | `receive.sync` (`socket_base_lifecycle.cpp:538` `scoped_lock_t receive_owner (receive.sync)`) |
| T21 (recv) | `zlink_recv_part` → `socket_base_t::recv` → `receive_once_guarded` → `pair_t::xrecv` → `read_internal` (`_in_active` 쓰기) | **public receive lease** (`try_acquire_public_receive_lease()`, lock-free) |

`receive_once_guarded`의 lock-free 빠른 경로는 `record_scope_`가 없는 단일 frame recv에서 **`receive.sync`를 잡지 않는다**
(`socket_base_msg.cpp:61-66`). lease는 주석대로 *async mailbox owner*를 배제하도록 설계됐고, **같은 socket의 command
owner(=poller 스레드가 직접 돌리는 `process_commands`)는 배제하지 않는다.** 즉 `_in_active`를 만지는 두 경로는
서로 다른 배타 도메인에 있고, 어떤 잠금 조합으로도 겹치지 않는다. `_out_sync`는 애초에 이 멤버와 무관했다(S-1 §2(b)).
따라서 "한쪽에 잠금을 더 잡는" 수정은 존재하지 않는다 — 멤버 자체가 순서를 가져야 한다.

## 3. 설계 비교와 선택 이유 — (B) 채택, (A) 기각

### (A) receive lease 단독 소유 — **기각**

command owner는 `_in_active`를 건드리지 않고 원자 "활성화 요청" 플래그만 남기고, 실제 전이와 `read_activated()`를
수신 스레드가 다음 진입에서 적용하는 안.

기각 이유 두 가지.
1. **계약 위험(POLLIN level)**: `read_activated()`는 socket의 active receive partition(fq/route table)을 바꾸고,
   `ZLINK_POLLIN` level은 그 뒤 `xhas_in()`이 읽는다. 이 갱신을 수신 스레드로 미루면 **아직 partition에 들어가지 않은
   pipe 상태에서 poller가 level을 계산**하게 되어 05-polling의 level 규칙이 깨진다(lost wake). command owner가
   `receive.sync` 아래에서 partition을 갱신하는 현재 순서가 바로 그 level의 근거다.
2. **POSDDD 역행**: 없애야 할 상태를 없애지 않고 "활성화 요청" 플래그라는 **새 상태와 새 적용 규칙**을 추가한다.
   S-1이 (a)에서 같은 이유로 기각한 방향과 동형이다.

### (B) `std::atomic` 승격 — **채택**

`_in_active`는 **payload를 나르지 않는다.** frame 자체는 inbound ypipe의 자체 release/acquire로 건너오고,
`_in_active`는 "큐가 비어 보였다"는 힌트일 뿐이며 **틀린 값은 자기치유된다**: false로 잘못 보이면 reader가
`_in_pipe->check_read()` 한 번을 더 하고(`pipe.cpp:1034,1148`) 복구하며, true로 잘못 보이면 command owner가
wake를 한 번 덜 보내는 것이 아니라 `_head_reclassify_wake` 분기로 떨어질 뿐이다. 그래서 **접근 자체에 순서를 주면 끝**이고,
새 상태·새 규칙·새 제어점이 0이다. acquire/release는 x86-64에서 평범한 mov라 비용도 0이다(§1 Ir/msg가 확인).

### 함께 처리한 축: `_state` 승격 + `_state_active` 미러 삭제 (**상태 −1**)

S-1이 후속으로 남긴 항목이며 (B)와 정확히 같은 축이다. `_state`도 `check_read`/`read_internal`/`process_activate_read`가
**잠금 없이 읽고** `transition_to_inactive_state_unlocked()`만 `_out_sync` 아래에서 쓴다 — `_in_active`와 같은 노출도다.

- `lifecycle_state_t _state` → `std::atomic<lifecycle_state_t> _state`. 쓰기는 전이 헬퍼 **한 곳뿐**임을 확인했고
  (`grep '_state ='` 결과 전이 헬퍼 외 0건) 거기서 release store 한 번으로 바꿨다.
- `std::atomic<bool> _state_active` 미러 **삭제**. 불변식 `_state_active == (_state == active)`가 정확했으므로
  `is_lifecycle_active()`는 `_state.load(acquire) == active`가 됐다(호출처 40여 곳 무변경, 읽는 값 동일).
  reader가 보던 release/acquire 짝은 그대로이고(전이 헬퍼의 release → `is_lifecycle_active`의 acquire) 오히려 강해졌다.
- 63곳의 기계적 변환은 **필요 없었다**: `std::atomic<T>`의 암묵 변환·대입 연산자로 기존 `_state != active` /
  `_state = x` 문법이 그대로 컴파일된다. 다만 **핫 경로 3곳**(`check_read`, `probe_normalized_head_kind`,
  `read_internal`, `process_activate_read`)은 `_state`를 두 번 비교하므로 seq_cst load 2회가 되지 않도록
  진입부에서 `const lifecycle_state_t state = _state.load(acquire)` 스냅샷 1회로 접었다. 이 축약이 Ir/msg −4 %의 본체다.

## 4. 변경 파일

| 파일 | 내용 |
|---|---|
| `core/src/runtime/core/pipe.hpp` | `_in_active` → `std::atomic<bool>`(소유 주석 포함), `_state` → `std::atomic<lifecycle_state_t>`, **`_state_active` 멤버 삭제** |
| `core/src/runtime/core/pipe.cpp` | `_in_active` 10개 접근점을 explicit acquire/release로, `_state` 핫 경로 4곳 스냅샷 1회, `is_lifecycle_active()`/`transition_to_inactive_state_unlocked()` 갱신, `_state_active` 초기화·store 삭제, `process_activate_read` 주석을 실제 소유 구조로 정정 |

`core/include/**`·`libzlink.vers`·계약 테스트·옵션 어느 것도 건드리지 않았다. 변경은 두 파일 45+/32− 이다.

## 5. 실행한 테스트

| 대상 | 결과 |
|---|---|
| `ctest -R 'wake\|poll\|stream\|pipe\|mailbox\|fq\|dealer'` (build-dev) × **5회** | 4/5 전부 통과. 4회차에 `test_close_completion_poller_release` 1건 실패 — **pre-existing 간헐**(`diag-close-completion-poller-release.md`: ab-old(캠페인 이전)에서 4/50, main에서 더 낮음). 이 diff와 무관 |
| lost-wake 세트(`test_wake_invariants`·`test_two_poller_wake`·`test_wake_invariant_hwm_lwm_shrink`·`test_wake_invariant_completion_owner`·`test_stream_packet_progress`·`test_stream_send_blocking_wakeup`) `--repeat until-fail:10` | **2회 실행, 두 번 다 100 % 통과 (0 실패)** |
| TSan(`core/build-tsan`, `setarch $(uname -m) -R`) 4개 바이너리 before/after | 아래 |

### TSan before → after

| 바이너리 | before | after |
|---|---|---|
| `test_two_poller_wake` | 3건 (**`_in_active` @ `pipe.cpp:2751`**, `notify_receive_progress_locked`, `receive_once_guarded`) | **0건, exit 0** |
| `test_wake_invariants` | 10건 | 10건 → `ypipe<command_t,16>::check_read` 9 + `receive_once_guarded` 1 (**`_in_active` 없음**) |
| `test_stream_packet_progress` | 9건 | 9건 → `ypipe` 8 + `receive_once_guarded` 1 (**`_in_active` 없음**) |
| `test_stream_multiclient_delivery` | — | 15건, 전부 `ypipe<command_t,16>::check_read` (**`_in_active` 없음**) |

남은 두 부류는 S-1 §4가 이미 pre-existing으로 분류한 것과 같다: (1) mailbox cpipe의 의도된 lock-free 패턴,
(2) `receive_once_guarded`. **(2)는 §2에서 밝힌 lease/`sync` 이중 배타 도메인 그 자체**이며 `_in_active` 하나가 아니라
socket receive 상태 전반의 문제다 — §8에 D 항목으로 남긴다.

## 6. 재확인한 스펙 문장 (문장 단위 대조)

이 diff는 **어떤 값을 언제 쓰는가만** 바꾸고 **무엇을 언제 하는가는 바꾸지 않는다.** 진리표가 같다는 것은 문장 대조뿐
아니라 §1의 callgrind 호출 횟수(`process_activate_read` 1.999/msg, `mailbox_t::send` 2.002/msg,
`xread_activated` 0.999/msg — 전부 S-1과 동일)로도 실측 확인됐다.

### `core/doc/spec/core/05-polling.ko.md` §3 표 raw socket 행 (49행)
> "| raw socket | complete record를 수신할 수 있음 | submit 재시도 가치가 있음(socket 전체 집계). 읽지 않은 `ZLINK_COMPLETION_WRITABLE` record가 있는 동안 level로 유지 | socket별 receive mode 적용. close된 등록 socket은 `ZLINK_POLLERR` 1회 |"

**동일.** `POLLIN` level은 `get_events_internal` → `xhas_in()`이 결정하고, 그 경로는 변경 파일에 없다.
`read_activated()` 호출 여부의 진리표는 그대로다: `_state ∈ {active, waiting_for_delimiter}` 이고
(`!_in_active` 였거나, `_in_active` 이면서 재분류 marker가 armed→idle 전이에 성공)일 때 정확히 1회.
바뀐 것은 그 두 술어를 **읽는 방식**(plain → acquire load)뿐이며, 술어의 값 집합·평가 순서·분기 구조는 비트 단위로 같다.

> "여러 peer를 가진 raw socket의 `ZLINK_POLLOUT`은 socket 전체의 집계 readiness다." (54행)

**동일.** 송신 방향(`_out_active`, `write_activated`, `flush_unlocked`)은 한 줄도 건드리지 않았다.

### `core/doc/spec/core/socket/README.ko.md` §3 (63행)
> "Core가 application에 처리할 항목이 있음을 알리는 경로는 poller readiness와 pull receive다. Core는 application notification callback을 호출하지 않는다."

**동일.** 알림 경로의 개수·종류·시점을 바꾸지 않았다.

> "| 일반 DATA | `ZLINK_POLLIN` | socket 종류에 맞는 `*_recv_part()` |" (67행)

**동일.** readiness와 꺼내는 함수의 짝을 바꾸지 않았다.

### `core/doc/spec/core/socket/README.ko.md` §6 / `08-stream.ko.md` §5 recv 문장
> "`ZLINK_RECV_FLAGS_DONTWAIT` 호출에 데이터가 없으면 `ZLINK_RECV_NO_DATA`와 `EAGAIN`을 반환한다."
> "성공하면 수신 part의 소유권이 호출자에게 이전되며 호출자는 `zlink_msg_close(part_out_)`를 정확히 한 번 호출해야 한다."
> "RAW 수신 record는 단일 part이며, 성공 시 `*has_more_out_`은 `ZLINK_PART_FINAL`이다."

**전부 동일.** `read_internal`의 반환값·`msg_` 소유권 이전·`errno` 설정은 손대지 않았다.
`_in_active.store(false)`가 일어나는 지점(`ypipe_read_empty` / `_in_pipe->read()` 실패)과 그때의 반환값은 전과 같다.

### `systems/04-thread-safety`·`02-threading-model` 소유 계층
`_in_active`/`_state`의 **소유자를 옮기지 않았다.** 값을 쓰는 주체(수신 lease와 command owner)와 시점은 그대로이고,
두 주체가 이미 서로 다른 배타 도메인에 있다는 **기존 사실에 메모리 순서를 부여**했을 뿐이다.

**어느 문장도 다른 동작이 되지 않았다.**

## 7. 변경 분류

**B (기존 결함 수정).** S-1 §7이 남긴 pre-existing 결함이며, 캠페인 이전(S-2 base)에도 TSan이 같은 경합을 보고했다.
부수적으로 중복 상태 `_state_active` 1개를 삭제했다(POSDDD: 상태 수 감소, 규칙 수 불변).

## 8. 멈춘 지점 / 후속

- **D (spec gap 후보, 이번 job 범위 밖)**: `receive_once_guarded`의 lock-free public lease와 `process_commands`의
  `receive.sync`가 **같은 socket receive 상태를 서로 배제하지 않는다**(§2). `_in_active`는 그 상태 중 하나였을 뿐이고,
  TSan에 남은 `receive_once_guarded` 경고가 같은 구조를 가리킨다. 다중 peer socket에서는 fq active partition 자체가
  같은 노출도를 갖는다(이번 PAIR 재현에서는 fq를 쓰지 않아 드러나지 않았다). lease가 배제해야 하는 대상에
  **command owner를 포함시킬지**는 04-thread-safety의 소유 계층 문장과 성능 예산(lease의 존재 이유)이 함께 걸린
  결정이라 별도 job이 필요하다.
- with_stream 1차 일괄 실행은 외부 부하로 폐기하고 크기별로 재측정했다(§1). 측정 규칙상 before는 재지 않았다.
