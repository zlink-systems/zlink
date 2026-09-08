# CCU-5 — review-CCU-4 차단 2건 해소 (Core, 0.17.4)

작성 2026-09-08. 담당 job CCU-5. worktree `~/project/zlink-work/ccu2`.
패치 `~/project/zlink-work/all-artifacts/CCU-5.patch` — CCU-2~CCU-5 누적, `core/**` 13파일.
커밋하지 않았다. 공개 인터페이스 무변경(`git diff HEAD -- core/include core/src/libzlink.vers` 0줄).

## 0. 결과 요약

| 항목 | 상태 |
|---|---|
| B-CCU4-1 `applied > planned`가 수렴 지표로 부적합·장주기 반복 | **해소** — 지표 자체를 없앴다. 수렴 의무 = deadline |
| B-CCU4-2 증분 record가 deadline을 지워 attach마다 timer 재무장 | **해소** — deadline 해제는 full pass 완료 지점 한 곳 |
| W-CCU3-1 잔여 | **해소** — 비포화 테스트가 **각 attach마다** 증분 진입을 assert |
| W-CCU3-2 잔여 | **해소** — 첫 attach는 확장 불가이므로 `extension_expected=false`로 완전 동일 요구 |
| W-CCU3-3 잔여 | **해소** — 두 스레드 모두 같은 `go` 뒤에 첫 attach 시작 |
| 검증 | 신규 unittest **10/10** / 대상 suite+신규 until-fail:5 **32×5** / 전체 **211/211** / TSan **9/9, report 0** / CCU 4000 **209.1 kops PASS**(idle) |

## 1. 규칙 — 전후

### 전 (CCU-4)

| 관심사 | 표현 |
|---|---|
| 수렴이 필요한가 | plan의 `total_applied_hwm_bytes > total_planned_hwm_bytes` |
| 언제 도는가 | `recalc_due()` = 마감 경과 && (미처리 요청 \|\| 위 부등식) |
| 마감 해제 | `record_applied_plan()` — full·증분 **양쪽** |

두 결함이 여기서 나왔다. 부등식은 "증분 planner가 기존 방향을 방문하지 않았다"와
"full planner가 목표를 이미 기록했지만 queue가 정상 drain 중이다"(스펙 §4 deferred shrink)를
구분하지 못하고, 증분의 record가 마감을 0으로 만들어 다음 증분이 매번 "첫 무장"이 됐다.

### 후 (CCU-5) — 한 문장

> **수렴 의무는 deadline 자체다.**
> 무장된 deadline이 곧 "full pass가 하나 밀려 있다"이고, full pass가 그 하나를 정확히 한 번 해제한다.

| 관심사 | 표현 | 코드 |
|---|---|---|
| 수렴이 필요한가 | `_recalc_deadline_ms != 0` | `ctx_auto_hwm_state.cpp:331-341` |
| 언제 도는가 | `recalc_due()` = 마감이 있고 지났다 — **그것뿐** | 〃 |
| 무장 | `arm_debounce()` — 마감이 0일 때만 세우고 `true`(=timer 1회 wake) | `ctx_auto_hwm_state.cpp:252-263` |
| 마감 해제 | `clear_debounce()` — **full pass 완료 지점 한 곳** | `ctx_auto_hwm_state.cpp:265-272`, 호출 `ctx_auto_hwm_recalc.cpp:305-310` |
| plan 기록 | `record_applied_plan()`은 요청 장부(`_recalc_pending`)만 다루고 마감을 건드리지 않는다 | `ctx_auto_hwm_state.cpp:318-327` |

plan의 숫자에서 아무것도 추론하지 않으므로 CCU-4가 넣었던 `_applied_plan` 참조가 `recalc_due()`에서
사라졌다. 새 상태·플래그는 여전히 0이다(마감 필드는 원래 있던 `_recalc_deadline_ms`다).

## 2. B-CCU4-1 — deferred shrink 반례가 닫히는 이유 (코드)

리뷰의 반례는 "queue가 보관량 때문에 `applied_hwm`을 못 낮춰 full pass 뒤에도
`total_applied > total_planned`가 남고, `recalc_due()`가 계속 참이라 장주기 tick마다 같은 pass가 반복된다"였다.

이제 `recalc_due()`는 그 숫자를 읽지 않는다(`ctx_auto_hwm_state.cpp:331-341`). 흐름은 이렇다.

1. 증분 attach → `schedule_auto_hwm_convergence()` → `arm_debounce()`가 마감을 세운다
   (`ctx_auto_hwm_recalc.cpp:119-158`).
2. 마감에 task가 깨어 `recalc_due()` 참 → `auto_hwm_recalculate_now()`.
3. 그 pass가 plan을 기록하고 **`clear_debounce()`로 마감을 0으로 만든다**
   (`ctx_auto_hwm_recalc.cpp:305-310`). 보관량 때문에 `applied_hwm`이 안 내려가 `applied > planned`가
   남아도 마감이 0이므로 `recalc_due()`는 거짓이다 → **재실행 없음**.
4. 그 사이 새 요청이 들어왔다면 `record_applied_plan()`이 `_recalc_pending`을 남기고,
   `clear_debounce()`는 그 경우 마감을 지우지 않는다(`ctx_auto_hwm_state.cpp:265-272`) — 요청은 유실되지 않는다.

테스트로 고정: `test_deferred_shrink_does_not_keep_replanning`
(`core/tests/unittest/unittest_auto_hwm_incremental_plan.cpp:352-407`). PAIR pair의 queue를 EAGAIN까지
채운 뒤 방향을 늘려 level을 낮추고, full pass 직후 `applied > planned`가 **실제로 남아 있음**을 확인한 다음,
debounce 6배 시간 동안 `budget_generation`이 **전혀 증가하지 않음**을 확인한다.

## 3. B-CCU4-2 — 타이머 무장 횟수 = 연속 attach N에 대해 1

`record_applied_plan()`에서 `_recalc_deadline_ms = 0`을 제거했으므로
(`ctx_auto_hwm_state.cpp:318-327`), 증분이 plan을 기록해도 진행 중인 wait는 그대로 남는다.
`arm_debounce()`는 마감이 0이 아니면 **아무것도 하지 않고 `false`**를 돌려주고
(`ctx_auto_hwm_state.cpp:252-263`), 호출자는 그때 runtime timer를 건드리지 않는다
(`ctx_auto_hwm_recalc.cpp:143-150`). 따라서 한 wait에 몇 개의 attach가 합류하든 wake는 한 번이다.

**공개 표면 계측**: `test_extension_converges_on_the_debounce_without_further_calls`
(`unittest_auto_hwm_incremental_plan.cpp:281-350`)가 debounce 100 ms에서 5회 연속 attach 후

- burst 직후 `budget_generation == before + 5` (attach당 plan 1개, 그 외 publish 없음),
- 수렴 뒤 `budget_generation == before + 5 + **1**` — **전체 burst에 대해 full pass가 정확히 1회**,
- 이어서 debounce 5배를 더 기다려도 generation 불변(해제된 wait는 되살아나지 않는다)

를 검사한다. "무장 N회"였다면 pass가 여러 번 기록되어 이 등식이 깨진다. 즉 timer 무장 횟수 1은
generation 증가량으로 관측된다.

부수 효과 하나를 명시한다: 후속 무장이 마감을 **뒤로 밀지 않으므로**, 긴 burst에서는 첫 attach로부터
debounce 뒤에 full pass가 한 번 돌고, 그 뒤 attach가 다시 wait를 연다. 즉 수렴이 debounce 주기마다
전진하며 무한정 미뤄지지 않는다 — CCU-4의 "마지막 attach 기준"보다 오히려 수렴 보장이 강하다.

## 4. 경고 잔여분

- **W-CCU3-1**: `test_extension_runs_and_matches_full_recalculation_unsaturated`
  (`:171-206`)가 **각 attach마다** 판정한다 — 첫 attach는 확장할 water level이 없으므로
  `applied == planned`를 요구하고, 2번째부터 5번째까지는 `applied > planned`를 **매번** 요구한다.
  fallback은 이 부등식을 만들 수 없으므로 각 단계의 증분 진입이 고정된다.
- **W-CCU3-2**: `compare_settled_attach_steps()`(`:126-149`)가 attach 직전 snapshot의
  `active_directional_queue_count`로 "확장 가능한 plan이 있었는지"를 보고, 없으면
  `extension_expected=false`로 넘겨 **applied까지 완전 동일**을 요구한다. 첫 topology attach에 완화가
  적용되던 문제가 사라졌다.
- **W-CCU3-3**: 두 작업 모두 `std::async`로 띄우고, main은 `ready == 2`를 기다린 뒤 `go`를 세운다
  (`:441-470`). 이제 두 스레드가 같은 신호 뒤에 첫 attach를 시작한다. generation은 여전히
  `before + 8` 정확히 검사한다.

## 5. 검증 결과

| 검증 | 결과 |
|---|---|
| dev 빌드(JOBS=4, ninja 0 확인 후) | OK |
| 신규 unittest | **10/10 PASS** |
| 대상 suite + 신규 `--repeat until-fail:5` | **32/32 × 5 PASS** |
| 전체 `ctest -E hotpath_gate` | **211/211 PASS** |
| TSan(기존 `core/build-tsan` 재빌드, 억제 없음, `setarch x86_64 -R`, `-j1`, 9 suite) | **9/9 PASS, `WARNING: ThreadSanitizer` 0건** |
| with_stream CCU 4000 64 B `--runs 2` | **zlink 209.1 kops PASS**(asio 333.5), idle(load1 1.08), `flock PERF_LOCK`, ninja 0 |

CCU 4000 추이: CCU-2 202.8 → CCU-3 212.7(부하 창) → CCU-4 203.2 → **CCU-5 209.1**. 같은 대역이다.

## 6. 변경 분류

- B-CCU4-1/2는 CCU-4가 넣은 수렴 메커니즘의 **자체 결함 수정**이며 새 계약 편차가 없다.
  스펙 §2의 "기존 방향 인하는 debounce 재계산 경로가 기록"은 이제 deadline 하나로 보장되고,
  §4의 deferred shrink 상태를 수렴 의무로 오독하지 않는다.
- 새 옵션·플래그·타이머·지속 상태: **0**. `clear_debounce()`가 늘고 `recalc_due()`의 조건은 3개에서
  1개로 줄었다(규칙 수 감소).

## 7. 남은 것

- hotpath 5셀(merge gate).
- S-CCU3-1(state plan 복사와 registry live sampling 사이 generation 일관성) — HEAD에도 동일, 범위 밖.
- queue ID wrap 실행 커버리지(공개 API로 도달 불가).
