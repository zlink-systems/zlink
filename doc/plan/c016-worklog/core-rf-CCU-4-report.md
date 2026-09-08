# CCU-4 — review-CCU-3 차단 1건·경고 3건 해소 (Core, 0.17.4)

작성 2026-09-08. 담당 job CCU-4. worktree `~/project/zlink-work/ccu2`.
패치 `~/project/zlink-work/all-artifacts/CCU-4.patch` — CCU-2+CCU-3+CCU-4 누적, `core/**` 13파일
(감독관이 넣은 `core/doc/spec/**`·`REVIEW-NOTE-D-H1.md`는 제외). 커밋하지 않았다.
공개 인터페이스 무변경(`git diff HEAD -- core/include core/src/libzlink.vers` 0줄).

## 0. 결과 요약

| 항목 | 상태 |
|---|---|
| B-CCU3-1 수렴 미예약 | **해소** — 증분 성공 뒤 debounce를 무장하고, 수렴 의무를 plan 자신이 표현 |
| W-CCU3-1 증분 미진입 | **해소** — 테스트가 pending을 소진한 뒤 attach하고, 증분 진입을 공개 지표로 assert |
| W-CCU3-2 비교 범위 | **해소** — plan 소유 field 전부 비교, `total_applied` 완화는 증분 케이스에만 |
| W-CCU3-3 동시성 | **해소** — 시작 barrier + generation "attach당 정확히 +1" |
| 헤더/구현 주석 | **해소** — remainder prefix가 `level+1`이라는 사실과 실제 수렴 호출을 명시 |
| 검증 | 대상 suite+신규 until-fail:5 32×5 PASS / 전체 ctest **211/211** / TSan **9/9, report 0** / CCU 4000 **203.2 kops PASS**(idle) |

## 1. B-CCU3-1 — 수렴 replan 예약

### 규칙

새 규칙은 **한 문장**이다.

> **증분이 plan을 바꿨으면 debounce 마감을 무장한다. 그 replan이 필요한지는 plan 자신이 말한다 —
> `total_applied_hwm_bytes > total_planned_hwm_bytes`이면 아직 낮추지 못한 목표가 남아 있다는 뜻이다.**

핵심은 **수렴 의무를 요청 카운터가 아니라 plan에 담았다**는 것이다. 그래서
`pending_generation`을 올리지 않고, 증분 fast path의 guard(`pending == last_applied`)는 그대로다.
"예약 사유가 증분이면 허용" 같은 사유 구분도, 새 flag·state도 없다.

- (a) **N개 연속 attach가 fast path 유지**: 증분은 요청을 만들지 않으므로 guard가 계속 통과한다.
- (b) **마지막 attach 뒤 debounce 1회로 수렴**: 무장된 마감이 지나면 `recalc_due()`가 plan의
  `applied > planned`를 보고 전체 재계산을 돌린다. 전체 pass는 모든 목표를 새 level로 낮추므로
  `applied == planned`가 되어 조건이 스스로 사라진다(반복 없음).
- (c) **사이에 option/socket 생성/detach가 오면 full fallback**: 그것들은 여전히
  `schedule_auto_hwm_recalculate()`로 `pending_generation`을 올리므로 guard가 막는다.

`_recalc_deadline_ms == 0`은 이미 `record_applied_plan()`이 "대기 중인 wake-up 없음"을 표현하려고
쓰던 값이라, 그 의미를 그대로 재사용했다(새 상태 0개).

### 파일:행

| 무엇 | 어디 |
|---|---|
| 증분 성공 뒤 수렴 무장 호출 | `core/src/runtime/sockets/common/socket_base.cpp:334-341` |
| `ctx_t::schedule_auto_hwm_convergence()` | `core/src/runtime/core/ctx_auto_hwm_recalc.cpp:119-158` |
| `ctx_auto_hwm_state_t::arm_debounce()` — 요청은 만들지 않고 마감만 이동 | `core/src/runtime/core/ctx_auto_hwm_state.cpp:252-267` |
| `recalc_due()`가 plan에서 수렴 의무를 읽음 | `core/src/runtime/core/ctx_auto_hwm_state.cpp:325-341` |
| 선언·주석 | `core/src/runtime/core/ctx_auto_hwm_state.hpp:36-48`, `core/src/runtime/core/ctx.hpp:124-132` |

### 성능 — debounce 타이머를 attach마다 재무장하지 않는다

첫 구현은 attach마다 `control_runtime::schedule_task_after()`를 불렀고, CCU 4000에서
**150.9 kops**로 떨어졌다(같은 창의 수렴 미예약 대조 197.0). 4000번의 타이머 재무장 비용이다.

고친 규칙: **마감을 처음 세우는 무장만 runtime 타이머를 깨우고, 이후 attach는 마감만 옮긴다.
일찍 깬 task는 남은 시간만큼 자기를 다시 건다.** 표준 debounce이며 상태를 늘리지 않는다
(`arm_debounce()`가 "직전에 마감이 없었다"를 반환, `ctx_auto_hwm_recalc.cpp:135-147`;
task 자가 재무장 `:324-356`; 남은 시간 `ctx_auto_hwm_state.cpp:269-276`).

| 빌드 | 시작 load1 | zlink kops | asio kops |
|---|---:|---:|---:|
| CCU-3(수렴 예약 없음, 차단 상태) | 4.71 | 212.7 | 323.0 |
| CCU-4 초안(attach마다 타이머 재무장) | 0.94 | **150.9** | — |
| 같은 초안에서 수렴 호출만 제거한 A/B | 3.46 | 197.0 | — |
| **CCU-4 최종**(타이머 1회 무장 + task 자가 재무장) | 1.32 | **203.2** | 335.9 |

최종본은 수렴 예약을 지키면서 CCU-2/CCU-3와 같은 대역(202.8 / 212.7)으로 돌아왔다.
모든 측정은 `flock PERF_LOCK`, `pgrep -c -x ninja` 0, `--size 64 --ccu 4000 --duration 3`.
최종 행은 `--runs 2`이고 idle 창(load1 1.32)에서 쟀다.

## 2. W-CCU3-1 — 테스트가 증분 경로에 실제로 진입한다

`settle(ctx)` = `zlink_ctx_auto_hwm_recalculate()`로 **socket 생성이 만든 요청을 먼저 소진**한 뒤
`zlink_connect()`로 attach한다(`unittest_auto_hwm_incremental_plan.cpp:95-101`, `:126-141`).

증분 진입의 공개 근거 두 가지를 assert한다.

1. `budget_generation`이 attach당 **정확히 +1**(`:129-138`) — 증분이든 fallback이든 plan을 하나만 기록한다.
2. 비포화 구간에서 attach 직후 **`total_applied > total_planned`**
   (`test_extension_runs_and_matches_full_recalculation_unsaturated`, `:171-197`).
   전체 재계산은 빈 queue에서 항상 `applied == planned`를 남기므로, 이 부등식은 증분 경로로만 도달한다.
   테스트는 5회 attach 중 최소 1회 관측을 요구한다(`:194-196`).

수렴·폴백 전용 케이스도 새로 넣었다.

- `test_extension_converges_on_the_debounce_without_further_calls` (`:281-317`): debounce 100 ms,
  4회 attach 뒤 **snapshot 읽기 외 어떤 API도 부르지 않고** `applied == planned`가 되는 것을 폴링으로 확인 —
  (b)의 고정.
- `test_planning_request_between_attaches_forces_full_recalculation` (`:319-357`): 사이에 socket 생성,
  그리고 SNDHWM 변경을 넣으면 다음 attach가 `applied == planned`(전체 재계산)로 끝나는 것을 확인 — (c)의 고정.
- `test_completed_inproc_pair_falls_back_to_full_recalculation` (`:252-279`): bind한 peer가 자기
  mailbox를 돌려 두 번째 endpoint attach가 실제로 일어나게 `zlink_recv(..., DONTWAIT)`로 구동한 뒤
  폴백 결과를 검증한다.

## 3. W-CCU3-2 — 비교 field와 완화 범위

`assert_plan_equal()`(`:19-79`)이 비교하는 field: `configured_memory_limit_bytes`,
`runtime_memory_limit_bytes`, `resolved_memory_limit_bytes`, `configured_core_budget_bytes`,
`effective_core_budget_bytes`, `total_planned_hwm_bytes`, `manual_reserved_hwm_bytes`,
`active_directional_queue_count`, `active_send_queue_count`, `active_receive_queue_count`,
`unlimited_manual_queue_count`, `flags` — plan 소유 field 전부다.

`total_applied_hwm_bytes`는 인자 `extension_expected_`로 갈린다(`:66-76`).
증분이 가능한 케이스만 `증분 ≥ 전체`로 완화하고, **폴백이 강제되는 케이스(unlimited manual, role mix,
완성된 inproc pair)는 완전 동일을 요구**한다. 어느 경우든 전체 재계산 자신의 `planned == applied`도 확인한다.

## 4. W-CCU3-3 — 동시 attach

`test_concurrent_attach_records_one_plan_per_attach` (`:359-416`):

- socket 8개를 먼저 만들고 `settle()`로 생성 요청을 전부 소진해, 이후 publish는 attach뿐이게 만든다.
- `std::atomic<int> ready` + `std::atomic<bool> go`로 **두 스레드를 같은 지점에서 출발**시킨다(`:381-397`).
- `budget_generation`을 `before + 8` **정확히**로 검사한다(`:404-406`). debounce 3000 ms이고 다른
  요청이 없으므로 다른 publish가 끼어들 수 없다 — 직렬화와 누락 없음의 증거가 된다.
- 마지막에 전체 재계산과 plan을 비교한다.

## 5. 주석 정정

- `ctx_physical_queue_registry.hpp:137-158`: "one level"이 아니라 **`level`과, 나머지를 받는
  stable-ID prefix의 `level + 1`**임을 적고, attach하는 방향이 그 prefix 위에 있으므로 정확히 `level`을
  받는다는 논리를 명시했다. "debounced replan"이 실제로 무엇인지도
  `ctx_t::schedule_auto_hwm_convergence`로 이름을 박았다.
- `ctx_physical_queue_registry.cpp:1043-1053`(water-filling 나머지), `:1084-1092`(무엇을 미루고 그 사실이
  `applied > planned`로 남아 `recalc_due()`가 읽는다)도 같은 내용으로 맞췄다.

## 6. 검증 결과

| 검증 | 결과 |
|---|---|
| dev 빌드(JOBS=4, ninja 0·메모리 확인 후) | OK |
| 신규 unittest | **9/9 PASS** |
| 대상 suite + 신규 `--repeat until-fail:5` | **32/32 × 5 PASS** |
| 전체 `ctest -E hotpath_gate` | **211/211 PASS** |
| TSan(기존 `core/build-tsan` 재빌드, 억제 없음, `setarch x86_64 -R`, `-j1`, 9 suite) | **9/9 PASS, `WARNING: ThreadSanitizer` 0건** |
| with_stream CCU 4000 64 B | **zlink 203.2 kops PASS**(asio 335.9), idle(load1 1.32), `flock PERF_LOCK`, ninja 0 |
| hotpath 5셀 | 감독관 지시로 생략(merge gate) |

## 7. 변경 분류

- B-CCU3-1 해소는 **채택된 D-H1 계약의 수렴 단계를 실제로 실행하게 만든 것**이며 새 계약 편차가 아니다.
  스펙 §2가 요구하는 "기존 방향 인하는 debounce 재계산 경로가 기록"이 이제 코드로 보장된다.
- 새 옵션·플래그·타이머·지속 상태: **0**. 새 method 3개(`schedule_auto_hwm_convergence`,
  `arm_debounce`, `debounce_remaining_ms`)는 모두 기존 debounce 상태(`_recalc_deadline_ms`)와
  기존 plan field 위에서 동작한다.

## 8. 남은 것

- hotpath 5셀(merge gate).
- S-CCU3-1(state plan 복사와 registry live sampling 사이 generation 일관성) — 이번 범위 밖, HEAD에도 동일.
- queue ID wrap 실행 커버리지(공개 API로 도달 불가, CCU-3 보고서 §5 사유와 동일).
