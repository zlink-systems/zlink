# review-CCU-5 — review-CCU-4 차단 2건 해소 최종 재검증

검토 대상은 worktree `/home/hep7hep7/project/zlink-work/ccu2`의 미커밋 변경과
`/home/hep7hep7/project/zlink-work/all-artifacts/CCU-5.patch`
(SHA-256 `20e99483215ac0962b09073f1af706daa4356ea05456eb61aac329945e557bf1`)이다.
Artifact의 13개 source/CMake/test 변경은 현재 worktree에 역적용 가능한 동일 변경이다
(`git apply --check --reverse` 성공). 다만 worktree에는 artifact에 없는 Auto-HWM 스펙 한·영문 변경
2개와 `REVIEW-NOTE-D-H1.md`도 있다. 스펙 변경은 지정 입력으로 읽었고 note는 대상 diff에 포함하지
않았다. 지정대로 정적 읽기와 diff 확인만 수행했으며 소스·스펙·테스트 수정, 빌드·실행, commit은
하지 않았다.

## 결론

**B-CCU4-1과 B-CCU4-2는 해소됐다.** 수렴 의무는 plan 수치가 아니라 이미 존재하던 deadline 하나가
소유하며, full pass 완료만 이를 해제한다. 증분 plan 기록은 deadline을 보존하므로 같은 wait에
합류하는 attach가 timer를 다시 무장하지 않는다. Option/socket 생성/detach 요청은 기존 request
generation과 trailing deadline을 그대로 사용하므로 기존 debounce 의미도 유지된다.

신규 차단은 없다. 다만 timer wake 횟수를 직접 관측하지 않는 테스트, 짧은 실제 시간에 의존하는
두 테스트, 현재 구현과 반대인 source comment를 신규 W로 기록한다.

| 검증 항목 | 판정 | 근거와 요약 |
|---|---|---|
| 1. `recalc_due()` 조건 | **해소** | deadline 존재 및 경과만 검사한다(`core/src/runtime/core/ctx_auto_hwm_state.cpp:331-340`). Plan의 `applied > planned`는 더 이상 조건이 아니다 |
| 1. full pass의 debounce 해제 | **해소** | `clear_debounce()` 호출은 full pass plan 기록 직후 한 곳뿐이다(`core/src/runtime/core/ctx_auto_hwm_recalc.cpp:302-310`; 전체 호출 검색도 이 한 곳). 증분 기록 경로 `:227-234`에는 없다 |
| 1. deferred shrink 뒤 반복 | **해소** | full pass가 deadline을 지우면 높은 applied 값이 남아도 `recalc_due()`가 거짓이다(`ctx_auto_hwm_state.cpp:265-272,331-340`; 스펙 `06-auto-hwm.ko.md:475-480`) |
| 1. 진행 중 요청의 wait 보존 | **해소** | 새 request generation을 이번 pass가 소비하지 못하면 `_recalc_pending`이 남고(`ctx_auto_hwm_state.cpp:317-327`), `clear_debounce()`가 deadline을 보존한다(`:265-272`) |
| 1. 기존 request debounce 의미 | **해소** | 일반 `schedule()`은 매 요청마다 generation을 올리고 deadline을 다시 계산한다(`ctx_auto_hwm_state.cpp:239-249`). Socket 생성, option, manual HWM, detach 호출도 기존 일반 경로를 유지한다(`ctx.cpp:271-274`, `ctx_options.cpp:128-130`, `socket_base_api.cpp:706-714,2021-2022`) |
| 1. 상태 소유와 규칙 수 | **해소** | request 장부는 pending/generation, full-pass 의무는 deadline이 각각 소유한다. CCU-5가 새 persistent field를 추가하지 않았고 `recalc_due()` 조건은 하나로 줄었다 |
| 2. 증분 성공 후 무장 | **해소** | 증분 성공은 plan 기록 후 convergence만 예약한다(`ctx_auto_hwm_recalc.cpp:227-234`, `socket_base.cpp:333-341`). `record_applied_plan()`은 request 장부와 budget generation만 바꾸고 deadline을 건드리지 않는다(`ctx_auto_hwm_state.cpp:298-328`) |
| 2. 연속 attach의 timer 무장 | **코드 해소 / 테스트 W** | deadline이 이미 있으면 `arm_debounce()`가 그대로 `false`이고(`ctx_auto_hwm_state.cpp:252-263`), caller는 runtime timer를 호출하지 않는다(`ctx_auto_hwm_recalc.cpp:142-157`). 다만 generation 등식은 wake 횟수를 직접 증명하지 않는다(W-CCU5-1) |
| 2. 두 attach의 arm 경쟁 | **해소** | plan extension·record는 recalc mutex로 직렬화되고(`ctx_auto_hwm_recalc.cpp:167-171,227-234`), 각 arm은 같은 state mutex 안에서 실행된다(`:128-134`). Record가 deadline을 지우지 않으므로 가능한 교차 순서 모두 첫 arm 하나만 `true`다 |
| 2. 긴 burst의 주기별 full pass | **스펙 합치** | 후속 attach가 deadline을 밀지 않아 첫 attach 기준 debounce마다 수렴할 수 있다. D-H1은 기존 방향 인하가 “option 변경과 같은 debounce 재계산 경로”에서 기록되고 attach가 동기 full pass를 하지 않을 것을 요구한다(`06-auto-hwm.ko.md:156-162`). 마지막 attach 기준 trailing-edge를 요구하지 않으므로 모순으로 판정하지 않았다 |
| 3. attach별 증분 진입 | **해소** | 비포화 test가 첫 attach equality와 이후 각 attach의 `applied > planned`를 각각 검사한다(`unittest_auto_hwm_incremental_plan.cpp:157-183`) |
| 3. fallback 완전 동일 | **해소** | 비교 helper가 공개 snapshot의 plan 소유 field 전부와 fallback의 applied equality를 검사한다(`unittest_auto_hwm_incremental_plan.cpp:16-74`). 빈 plan 첫 attach는 완화를 끄고(`:135-146`), unlimited manual·mixed role·completed pair도 `false`로 강제한다(`:220-285`) |
| 3. barrier 동시 출발 | **해소** | 두 작업 모두 `std::launch::async`로 시작해 ready를 올리고 같은 `go`를 기다린 뒤 connect loop에 진입한다(`unittest_auto_hwm_incremental_plan.cpp:470-496`) |
| 3. deferred-shrink 반복 test | **기능 반례는 적합 / 타이밍 W** | full pass 뒤 `applied > planned`와 generation 불변을 검사한다(`unittest_auto_hwm_incremental_plan.cpp:395-407`). 그러나 600 ms sleep은 timer callback 처리 완료와 동기화하지 않는다(W-CCU5-2) |
| 4. 상태·옵션·timer | **해소** | CCU-4→CCU-5에는 새 field, option, control-runtime task가 없다. 기존 deadline의 해제 연산만 분리했다 |
| 4. public/ABI | **해소** | `core/include/**`와 `core/src/libzlink.vers` diff는 0줄이다 |
| 4. 주석 정합 | **경고** | 핵심 주석 일부가 아직 CCU-4 동작을 설명한다(W-CCU5-3) |
| 4. Windows/macOS | **정적 해소** | CCU-5 production 변경은 기존 mutex, 정수형과 control-runtime API만 사용한다. Test 추가 API는 표준 C++ `atomic/future/thread/chrono`다. 플랫폼 build는 금지 조건에 따라 실행하지 않았다 |

## 1. B-CCU4-1 상세 판정

`recalc_due()`는 `_recalc_deadline_ms != 0 && now_ms_ >= _recalc_deadline_ms`뿐이다
(`ctx_auto_hwm_state.cpp:331-340`). Full planner가 deferred shrink 때문에
`total_applied_hwm_bytes > total_planned_hwm_bytes`를 다시 기록해도 그 숫자는 수렴 의무를 만들지 않는다.
Full pass는 plan publish가 성공한 뒤 state mutex 아래에서 `record_applied_plan()`과
`clear_debounce()`를 연속 수행한다(`ctx_auto_hwm_recalc.cpp:293-310`). 미처리 request가 없으면 deadline은
0이 되고 이후 장주기 task tick도 재계산하지 않는다. 따라서 review-CCU-4의 영구 deferred-shrink
반례는 닫혔다.

Full pass 실행 중 option/socket/detach request가 들어오면 `schedule()`이 새 generation,
`_recalc_pending=true`, 새 deadline을 함께 기록한다(`ctx_auto_hwm_state.cpp:239-249`). 진행 중 pass는 이전
generation으로 publish하므로 pending을 소비하지 못하고(`:317-327`), `clear_debounce()`도 그 request의
deadline을 지우지 않는다(`:265-272`). 반대로 pass가 현재 generation까지 처리했으면 pending과 deadline을
각각 record와 full-pass 완료 지점에서 해제한다. Request 장부와 wait 소유를 분리했지만 같은 사실을 두
상태로 중복한 것이 아니라 서로 다른 두 사실을 각각 한 곳에서 소비한다.

## 2. B-CCU4-2 상세 판정

증분 성공의 record는 `_recalc_pending`, `_last_applied_generation`, `_budget_generation`만 다루고
deadline을 변경하지 않는다(`ctx_auto_hwm_state.cpp:298-328`). 이후 `arm_debounce()`는 state mutex 아래
deadline 0→armed 전이에서만 `true`를 반환한다(`ctx_auto_hwm_recalc.cpp:128-134`,
`ctx_auto_hwm_state.cpp:252-263`). 두 attach의 extension과 record는 `_auto_hwm_recalc_sync`로 직렬화되고,
record/arm이 교차해도 어느 record도 armed deadline을 지우지 않는다. 따라서 같은 wait에서는
`schedule_task_after()` 호출이 코드상 한 번이다.

다만 `test_extension_converges_on_the_debounce_without_further_calls`의
`before + N + 1` 등식(`unittest_auto_hwm_incremental_plan.cpp:313-340`)이 직접 증명하는 것은 attach plan
N회와 convergence full pass 1회다. `schedule_task_after()`는 같은 task의 단일 schedule node를
deschedule/reinsert하고 매번 CV를 깨운다(`core/src/runtime/core/control_runtime.cpp:138-155`). 그러므로
가상의 N회 재무장도 마지막 schedule 하나에서 full pass 1회만 만들 수 있어 이 등식만으로 N회
timer wake를 배제할 수 없다. 1회 무장 판정의 근거는 테스트가 아니라 위 state-mutex 코드다.

긴 burst에서 후속 arm이 deadline을 밀지 않는 것은 trailing-edge option request와 다른 coalescing
정책이다. 하지만 D-H1은 구체적인 deadline 재설정 시점을 계약하지 않고, 기존 방향의 인하가 debounce
task/full planner를 통해 기록될 것과 attach에서 synchronous full pass를 하지 않을 것만 정한다
(`06-auto-hwm.ko.md:156-162`). 첫 attach 기준 window마다 full pass가 실행되는 현재 동작은 이 문구와
모순되지 않는다.

## 3. W 잔여와 테스트 안정성

- W-CCU4-1은 해소됐다. 비포화 test는 증분 가능한 2~5번째 attach 각각에
  `applied > planned`를 요구한다(`unittest_auto_hwm_incremental_plan.cpp:169-180`).
- W-CCU4-2는 해소됐다. Helper는 빈 plan의 첫 attach에서 완화를 끄고(`:135-146`), 명시적인 fallback
  세 범주는 applied까지 완전 동일을 요구한다(`:220-285`).
- W-CCU4-3은 해소됐다. 두 async worker가 모두 barrier에 도착한 뒤 같은 release를 받는다(`:470-496`).
- `test_extension_converges_on_the_debounce_without_further_calls`는 5 attach가 100 ms 안에 끝난다는
  실제 시간 가정이 있다(`:294-340`). Process suspension이나 심한 부하로 deadline이 burst 중 지나면
  현재의 의도된 주기 수렴도 full pass를 둘 이상 수행할 수 있어 정확한 generation 등식이 실패한다.
- `test_deferred_shrink_does_not_keep_replanning`의 6배 대기는 600 ms wall-clock sleep뿐이다(`:358-407`).
  Control runtime이 그 안에 예정 wake를 실제 처리했다는 handshake가 없어 결함 구현을 거짓 통과시킬 수
  있다. 또한 task가 due를 관측한 뒤 recalc mutex를 기다리는 순간 synchronous `settle()`이 먼저 full
  pass를 끝내면, 이미 admitted된 callback이 뒤이어 한 번 더 publish해 generation 불변 검사가
  비결정적으로 실패할 수 있다(`ctx_auto_hwm_recalc.cpp:326-348`). 기능 반례는 맞지만 timing-independent
  회귀 증명은 아니다.

## 4. 신규 B/W/S

| ID | 등급 | 내용 |
|---|---|---|
| W-CCU5-1 | **W** | `budget_generation == before + N + 1`은 full pass 1회만 관측하며 동일 task의 `schedule_task_after()`/CV wake N회를 배제하지 못한다. 코드 정적 판정은 1회 무장을 보장하지만 보고서와 test comment의 계측 주장은 과도하다 |
| W-CCU5-2 | **W** | 두 100 ms debounce test가 실제 시간과 scheduler 진행에 의존한다. Burst 중 deadline 경과 및 due 관측과 synchronous `settle()`의 경쟁 때문에 false failure/false pass 가능성이 있다 |
| W-CCU5-3 | **W** | `ctx_auto_hwm_recalc.cpp:142-145`는 후속 attach가 deadline을 옮기고 task가 마지막 deadline으로 재무장한다고 쓰지만 구현은 deadline을 그대로 둔다. `ctx_physical_queue_registry.cpp:1080-1086`, `socket_base.cpp:336-340`, `ctx.hpp:126-132`도 plan 수치가 수렴 의무를 나타낸다고 써서 deadline 단독 소유와 어긋난다 |
| S-CCU3-1 | **S(유지)** | state plan 복사와 registry live sampling 사이 generation 일관성은 이번 차단 해소 범위 밖이다 |

변경 분류: B-CCU4-1/2 해소는 채택된 D-H1 구현의 **B 기존 결함 수정**이다. 신규 W는 test 증명력과
주석 정합 문제이며 새 계약이 필요한 D 또는 상위 계층 우회 C는 아니다.

차단 항목 수 / 채택 가능 여부: 0 / 채택 가능
