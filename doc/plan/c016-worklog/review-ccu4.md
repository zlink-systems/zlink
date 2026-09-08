# review-CCU-4 — review-CCU-3 차단 1건·경고 3건 해소 재검증

검토 대상은 worktree `/home/hep7hep7/project/zlink-work/ccu2`의 미커밋 변경과
`/home/hep7hep7/project/zlink-work/all-artifacts/CCU-4.patch`
(SHA-256 `e5162e9096d2c051db330b13e86a2b993059688cc1b66187c4ead69d20697f73`)이다.
artifact에는 source/CMake 12파일과 신규
`core/tests/unittest/unittest_auto_hwm_incremental_plan.cpp`가 들어 있다. Worktree에는 artifact에서
제외됐다고 CCU-4 보고서가 밝힌 Auto-HWM 스펙 한·영문 변경과 `REVIEW-NOTE-D-H1.md`도 존재한다
(`core-rf-CCU-4-report.md:3-6`). 지정된 대로 정적 읽기와 diff 확인만 수행했으며 소스·스펙·테스트
수정, 빌드·실행, commit은 하지 않았다.

## 결론

**재검증은 미통과다.** B-CCU3-1을 고치기 위해 `total_applied_hwm_bytes >
total_planned_hwm_bytes`를 수렴 의무로 재사용했지만, 이 부등식은 스펙 §4의 정상적인 deferred
shrink도 나타낸다. Queue가 새 목표보다 많은 byte를 보관하면 full pass 뒤에도 부등식이 남아
`recalc_due()`가 계속 참이다. 또한 증분 plan을 기록할 때마다 기존 deadline을 0으로 지우므로,
연속 attach마다 runtime timer를 다시 무장한다. CCU-4 보고서의 “첫 무장만 timer를 깨운다”는
규칙은 구현되지 않았다.

| 검증 항목 | 판정 | 요약 |
|---|---|---|
| 1(a) 증분 뒤 debounce full pass | **부분** | 증분 성공은 항상 수렴 deadline을 무장한다. 다만 full pass 실행은 deadline 시점에 `applied > planned`가 유지될 때뿐이며, 수렴 의무와 timer 무장 상태가 정확히 한 번 소비된다는 보장은 없다 |
| 1(b) full pass 뒤 자가 해제 | **부분** | 빈 queue에서는 full pass가 `applied == planned`를 만들어 조건이 꺼진다. Queue가 drain 중이면 꺼지지 않는다 |
| 1(c) deferred shrink 반복 방지 | **미해소 — B-CCU4-1** | 합법적인 deferred shrink에서도 `recalc_due()`가 계속 참이다. Control runtime의 장주기 자동 재예약 때문에 queue가 계속 drain되지 않으면 full pass도 계속 반복한다 |
| 1(d) fast-path generation guard | **해소** | `arm_debounce()`는 `pending_generation`을 올리지 않으며 다음 attach의 `pending == last_applied` guard를 유지한다 |
| 2. timer 최초 무장·자가 재무장 | **미해소 — B-CCU4-2** | 증분 publish가 매번 deadline을 0으로 지워 순차 attach도 매번 runtime timer를 다시 무장한다. 두 attach가 publish와 무장을 교차하면 둘 다 최초 무장으로 판정될 수도 있다 |
| 2. ctx 종료 중 task 수명 | **해소** | stopped 공표, task ID 제거, 실행 중 callback 대기가 기존 lock 순서로 연결돼 해제 뒤 callback 실행은 막힌다 |
| 2. mailbox wake 계약 | **부분** | Auto-HWM은 socket mailbox 대신 control runtime CV를 사용하므로 mailbox 소유권 위반은 없다. 그러나 실제 `schedule_task_after()`와 broadcast가 attach마다 실행돼 “전이에서 한 번만 알림”이라는 목표와 맞지 않는다 |
| 3. W-CCU3-1 증분 진입 증명 | **부분** | socket 생성 요청을 `settle()`로 소진하고 generation `+1`을 검사한다. `applied > planned`는 비포화 test 전체에서 한 번만 요구해 각 증분 가능 attach의 진입을 고정하지 않는다 |
| 3. W-CCU3-2 비교·완화 범위 | **부분** | 누락됐던 plan field와 강제 fallback 케이스의 완전 동일 비교는 보강됐다. 다만 첫 attach도 `extension_expected=true`로 전달돼 실제 증분 불가 단계까지 완화를 적용한다 |
| 3. W-CCU3-3 동시 시작 | **부분** | generation 정확히 `+8`은 보강됐다. Main thread가 barrier 진입 전에 자기 ready를 대신 올려서 두 실행 주체의 동시 출발은 여전히 보장하지 않는다 |
| 3. 수렴 polling 상한 | **해소** | 해당 test는 debounce를 100 ms로 낮추고 5,000 ms까지 기다려 충분한 여유가 있다. 기본 3,000 ms를 5,000 ms만 기다리는 구조는 아니다 |
| 3. inproc pair mailbox 구동 | **해소** | `zlink_recv(DONTWAIT)`의 최초 read가 EAGAIN이면 command를 무조건 처리한 뒤 다시 읽으므로 bind command의 두 번째 attach를 결정적으로 적용한다 |
| 4. 주석·공개 범위·플랫폼 | **부분** | public header와 export는 무변경이고 새 OS API도 없다. 그러나 timer 1회 무장과 full pass 자가 해제를 설명하는 주석은 위 두 반례 때문에 실제 동작과 다르다 |

## 1. B-CCU3-1 재검증

### 증분 성공과 generation

증분 attach는 plan을 pipe에 적용한 뒤 state에 기록한다
(`core/src/runtime/core/ctx_auto_hwm_recalc.cpp:227-234`). 성공한 호출자는 이어서
`schedule_auto_hwm_convergence()`를 호출한다
(`core/src/runtime/sockets/common/socket_base.cpp:333-341`). 이 함수의 `arm_debounce()`는 deadline만
바꾸고 `pending_generation`을 변경하지 않는다
(`core/src/runtime/core/ctx_auto_hwm_state.cpp:252-267`; generation을 올리는 함수는 `schedule()`의
`:239-250`). 따라서 다음 증분 진입의 `pending_generation == last_applied_generation` guard
(`ctx_auto_hwm_recalc.cpp:173-187`)는 유지된다. 1(d)는 **해소**다.

Deadline이 지나고 plan의 applied 합계가 planned 합계보다 크면 task는 full pass를 호출한다
(`ctx_auto_hwm_state.cpp:325-341`, `ctx_auto_hwm_recalc.cpp:322-344`). 빈 queue에서는 full planner가 새
목표를 적용할 때 `applied_hwm`도 즉시 바꾸고 합계를 다시 읽는다
(`ctx_physical_queue_registry.cpp:915-946`, `:1156-1168`). 이 경우 full pass가 기록한 plan은
`applied == planned`이고 조건이 꺼진다. 1(a)·1(b)는 이 범위에서 성립한다.

### B-CCU4-1 — deferred shrink를 수렴 예약으로 오인한다

스펙 §4는 HWM을 낮출 때 queue 보관량이 새 목표보다 많으면 admission 목표만 즉시 낮추고,
snapshot의 applied 값은 보관량이 목표 이하로 drain된 뒤에 낮추도록 요구한다
(`core/doc/spec/core/systems/06-auto-hwm.ko.md:475-480`). 구현도 `current_accounted_bytes > target`이면
`planned_hwm`만 쓰고 `applied_hwm`은 이전 값으로 둔다
(`ctx_physical_queue_registry.cpp:1156-1168`). Full planner는 이 높은 applied 값을 합산해 plan에
다시 기록한다(`:915-946`).

따라서 다음 정적 반례가 성립한다.

1. 기존 queue가 새 target보다 많은 byte를 보관한 상태에서 증분 attach가 `applied > planned`를
   기록하고 convergence deadline을 무장한다.
2. Deadline에 full pass가 모든 `planned_hwm`을 새 target으로 기록한다. 보관량이 target보다 크므로
   기존 queue의 `applied_hwm`은 낮아지지 않는다.
3. Full pass가 기록한 context plan도 계속 `total_applied_hwm_bytes >
   total_planned_hwm_bytes`다. `record_applied_plan()`은 deadline을 0으로 만들지만
   (`ctx_auto_hwm_state.cpp:293-323`), `recalc_due()`는 deadline 0에서 위 부등식만으로 다시 참이 된다
   (`:325-341`).
4. Control runtime은 callback을 호출하기 전에 task를 `interval_ms` 뒤에 자동 재예약한다
   (`core/src/runtime/core/control_runtime.cpp:225-247`). Auto-HWM task의 interval은 `UINT_MAX`다
   (`ctx_auto_hwm_recalc.cpp:58-63`). Queue가 계속 drain되지 않으면 이 장주기 tick마다 같은 full pass가
   반복된다.

즉 즉시 busy loop는 아니지만, 수렴 의무는 full pass 뒤 스스로 사라지지 않으며 영구 보관된
message가 있으면 재계산도 장주기로 계속 반복한다. 더 근본적으로, `applied > planned`는 “증분 planner가
기존 방향을 방문하지 않았다”와 “full planner가 목표를 이미 기록했지만 queue가 정상적으로 drain
중이다”라는 서로 다른 사실을 구분하지 못한다. B-CCU3-1은 **미해소**이며 이 반례를
**B-CCU4-1**로 기록한다.

## 2. Timer 무장, 경쟁과 수명

### B-CCU4-2 — 첫 무장만 runtime을 깨운다는 규칙이 구현되지 않았다

`arm_debounce()`는 `_recalc_deadline_ms == 0`일 때만 `true`를 반환하고
(`ctx_auto_hwm_state.cpp:252-267`), 호출자는 그때만 `schedule_task_after()`를 호출한다
(`ctx_auto_hwm_recalc.cpp:128-157`). 이 두 함수만 보면 CCU-4 보고서의 규칙과 일치한다.

그러나 각 증분 성공은 무장보다 먼저 `record_applied_plan(plan, recalc_generation)`을 호출한다
(`ctx_auto_hwm_recalc.cpp:227-234`). 증분 guard가 통과했다면 `recalc_generation ==
pending_generation`이고, `record_applied_plan()`은 이전 convergence deadline이 이미 있어도 무조건
`_recalc_deadline_ms = 0`으로 만든다(`ctx_auto_hwm_state.cpp:312-322`). 다음
`schedule_auto_hwm_convergence()`는 다시 idle로 판정한다. 따라서 순차 attach에서도 다음 순서가 매번
반복된다.

```text
incremental record -> deadline = 0 -> arm_debounce() = true
                   -> schedule_task_after() -> control-runtime CV broadcast
```

`schedule_task_after()`는 기존 schedule node를 다시 배치하고 CV를 broadcast한다
(`control_runtime.cpp:138-155`). 따라서 보고서의 “이후 attach는 마감만 옮긴다”와 “attach당 timer
재무장을 하지 않는다”는 설명(`core-rf-CCU-4-report.md:52-60`)은 실제 코드와 다르다.

두 thread도 state mutex 때문에 `arm_debounce()` 자체를 동시에 실행하지는 않는다. 하지만 extension의
record와 convergence 무장이 하나의 recalc/state 임계구역이 아니다. 첫 attach가 무장한 뒤 두 번째
attach가 record로 deadline을 0으로 만들면 둘 다 `wake_needed=true`를 얻을 수 있다. Control runtime의
단일 schedule node와 `_sync`가 마지막 재배치를 직렬화하므로 wake 유실이나 손상은 없지만, “첫
무장 한 번”은 보장되지 않는다. 이를 **B-CCU4-2**로 기록한다.

Task가 deadline보다 일찍 실행됐을 때 남은 시간을 계산해 다시 예약하는 경로 자체는 맞다
(`ctx_auto_hwm_state.cpp:269-275`, `ctx_auto_hwm_recalc.cpp:322-355`). 다만 위 deadline 초기화 때문에
연속 attach의 일반 경로가 이 자가 재무장 규칙을 사용하지 않는다.

### 종료 수명과 mailbox 계약

종료는 recalc mutex를 먼저 획득해 실행 중 full pass와 합류한 뒤 state mutex 아래 stopped를 기록하고
task ID를 지운다. 이어 `remove_task()`가 실행 중 callback이 끝날 때까지 기다린다
(`ctx_auto_hwm_recalc.cpp:65-84`, `control_runtime.cpp:114-135`). Callback이 남은 시간 재예약을 결정한 뒤
종료가 task를 제거한 경쟁도 `schedule_task_after()`가 없는 ID를 `EINVAL`로 거절하고 callback 종료를
기다리는 순서로 끝난다(`control_runtime.cpp:138-155`). 해제된 ctx에서 callback이 실행되는 정적 경로는
찾지 못했다.

Auto-HWM timer는 socket mailbox가 아니라 control runtime의 mutex/CV와 단일 schedule node가 소유한다.
따라서 mailbox command의 소비자를 둘로 만들거나 socket mailbox 신호를 빼앗는 변경은 없고, 스펙
§3.3의 단일 command owner 및 command drain 규칙(`11-synchronization-model.ko.md:131-155`)과 충돌하지
않는다. 다만 §3.3의 “깨울 이유가 생긴 전이에서 한 번 알린다”는 원칙에 대응시킨 CCU-4의 자체
무장 규칙은 B-CCU4-2 때문에 충족하지 않는다.

## 3. W-CCU3-1/2/3과 신규 test

### W-CCU3-1 — 부분 해소

`settle()`은 socket 생성이 올린 pending request를 동기 full pass로 소진한다
(`core/tests/unittest/unittest_auto_hwm_incremental_plan.cpp:96-102`). 각 attach 전후 snapshot으로
`budget_generation == before + 1`도 정확히 검사한다(`:107-119`). 이 두 보강은 해소됐다.

그러나 generation `+1`은 증분과 동기 full fallback이 모두 plan을 한 번 기록하므로 증분 진입 자체를
구분하지 못한다. 구분 가능한 `total_applied > total_planned`는 비포화 test의 5 attach 중 한 번이라도
관측되면 통과한다(`:150-172`). Saturation 전이, finite manual 및 concurrent test는
`extension_expected=true`를 넘기지만 `applied >= full-applied`만 검사한다(`:61-69`, `:126-140`,
`:179-202`, `:361-408`). 따라서 증분 가능하다고 선언한 각 단계가 실제 fast path를 사용했는지는
고정하지 않는다.

자율 수렴 test도 polling 전에 `applied > planned`를 먼저 요구하지 않는다(`:282-311`). 모든 attach가
full fallback으로 끝나 처음부터 equality인 회귀가 생겨도 loop를 건너뛰고 통과할 수 있다. 이 test는
빈 queue만 사용하므로 B-CCU4-1의 deferred shrink 반례도 다루지 않는다. W-CCU3-1은 **부분 해소**다.

### W-CCU3-2 — 부분 해소

`assert_plan_equal()`은 이전에 빠졌던 configured/runtime/resolved memory limit와 configured core
budget을 포함해 plan 소유 field를 비교한다(`:22-60`). Unlimited manual, mixed role, completed inproc
pair에는 `extension_expected=false`를 넘겨 applied까지 완전 동일을 요구한다(`:65-69`, `:208-273`). 이
범위는 해소됐다.

다만 `compare_settled_attach_steps()`는 첫 topology attach에도 `extension_expected=true`를 그대로
전달한다(`:126-140`). 초기 plan은 `application_auto_direction_count == 0`이므로 extension이 명시적으로
거절되고 full fallback한다(`core/src/runtime/core/ctx_physical_queue_registry.cpp:970-978`). 그 단계에도
`attached.total_applied >= replanned.total_applied` 완화가 적용된다. 완화 범위가 증분 가능 케이스에만
한정됐다는 설명은 정확하지 않으므로 W-CCU3-2는 **부분 해소**다.

### W-CCU3-3 — barrier는 여전히 동시 출발을 보장하지 않는다

Generation을 `before + 8`과 정확히 비교하는 보강은 확인했다
(`unittest_auto_hwm_incremental_plan.cpp:398-405`). 그러나 barrier에서 worker만
`connect_range()` 안의 `ready.fetch_add(1)` 뒤 기다린다(`:375-389`). Main thread는 자기 작업이 barrier에
도착하기 전에 바깥에서 `ready.fetch_add(1)`을 수행하고 즉시 `go=true`로 바꾼 뒤에야
`connect_range(0)`을 호출한다(`:390-395`). Main의 실제 lambda는 그 안에서 ready를 다시 올려 3을 만들고,
이미 true인 go를 통과한다. 한 thread만 기다리는 gate이므로 두 connect loop의 동시 출발을 보장하지
않는다. 정확한 generation 검사는 해소됐지만 경쟁 coverage는 **미해소**다.

### Polling과 completed inproc pair

자율 수렴 test의 debounce는 기본 3,000 ms가 아니라 100 ms이고 polling 상한은 5,000 ms다
(`:282-308`). 정적 시간 여유는 충분하다. 이 판정은 test 실행 결과를 재검증했다는 뜻이 아니며, 리뷰
제약에 따라 실행하지 않았다.

Completed inproc pair test의 `zlink_recv(..., ZLINK_DONTWAIT)`는 최초 read가 EAGAIN이면 timeout 0
분기에서 `process_commands(0, false)`를 반드시 호출하고 다시 읽는다
(`core/src/runtime/sockets/common/socket_base_msg.cpp:669-696`). `process_commands()`는 mailbox를
EAGAIN까지 비우며 각 command를 적용한다(`socket_base_lifecycle.cpp:487-527`). Bind command는
`process_bind()`에서 `attach_pipe()`를 호출한다(`:1389-1396`). 따라서 해당 PAIR test의 두 번째 endpoint
attach 구동은 정적으로 결정적이다.

## 4. 주석, public 범위와 플랫폼

- `core/include/**`와 `core/src/libzlink.vers`의 diff는 0줄이다. Public header, export와 ABI 변경은 없다.
- 새 production 경로는 기존 mutex, `uint64_t`, `UINT_MAX`와 control runtime API만 사용한다. Test의
  `std::atomic`, `std::future`, `std::thread`, `std::chrono`, `std::to_string`은 표준 C++ API다. 정적으로
  Windows/macOS 전용 컴파일 비호환 API는 찾지 못했다. 해당 플랫폼 build는 실행 금지 때문에 확인하지
  않았다.
- Remainder prefix가 `level + 1`이고 새 stable ID가 `level`을 받는다는 주석은 구현
  (`ctx_physical_queue_registry.cpp:1050-1074`)과 일치한다.
- 반면 `recalc_due()`의 “full pass 뒤 조건이 사라져 반복하지 않는다”
  (`ctx_auto_hwm_state.cpp:329-340`), `arm_debounce()`와 선언의 “후속 arm은 deadline만 옮기고 task가
  재무장한다”(`:252-267`, `ctx_auto_hwm_state.hpp:37-47`), convergence caller의 같은 설명
  (`ctx_auto_hwm_recalc.cpp:142-145`)은 B-CCU4-1/2 반례와 맞지 않는다. 주석 정합은 **부분 해소**다.

## 5. 신규 B/W/S와 대안 비교

| ID | 등급 | 내용 |
|---|---|---|
| B-CCU4-1 | **B** | `applied > planned`가 미방문 방향과 정상 deferred shrink를 구분하지 못해 full pass 뒤에도 `recalc_due()`가 계속 참이고 장주기 재계산이 반복됨 |
| B-CCU4-2 | **B** | 증분 record가 기존 deadline을 0으로 지워 연속 attach마다 runtime timer를 다시 무장하며, 두 attach의 record/arm 교차에서도 최초 무장이 중복될 수 있음 |
| W-CCU4-1 | **W** | 증분 진입의 `applied > planned`를 비포화 test 전체에서 한 번만 확인하고 자율 수렴 test는 그 사전조건을 검사하지 않아 경로별 회귀를 놓침 |
| W-CCU4-2 | **W** | 첫 attach는 extension 불가인데도 `extension_expected=true` 완화를 적용함 |
| W-CCU4-3 | **W** | Main thread가 실제 connect loop의 barrier 진입 전에 ready를 대신 올려 동시 출발을 보장하지 못함 |
| S-CCU3-1 | **S(유지)** | state plan 복사와 registry live sampling의 generation 일관성은 이번 patch에서도 범위 밖으로 남음 |

수렴 예약에는 두 대안이 있다.

| 대안 | 장점 | 문제 |
|---|---|---|
| 일반 `schedule()`로 pending generation을 올림 | 요청과 실행의 기존 규칙을 그대로 사용하고 deferred shrink와 혼동하지 않음 | 다음 attach가 모두 full fallback해 CCU fast path 목적을 잃음 |
| 기존 deadline을 한 번만 소비되는 convergence 의무로 사용 | 새 persistent field 없이 burst 중 fast path와 timer 1회 무장을 유지할 수 있음 | plan publish가 convergence deadline을 지우지 않게 하고, task가 deadline을 claim/clear하는 경계를 한 곳에서 정의해야 함 |

두 번째 대안이 결과 규칙 수가 적다. `total_applied_hwm_bytes`는 스펙이 정한 snapshot/deferred-shrink
의미만 유지하고, “full pass 한 번 실행” 의무는 기존 deadline의 무장→claim→해제 한 상태 전이로
소유하게 해야 한다. 현재 구현은 한 부등식에 두 의미를 겹치고, deadline의 “timer가 무장됨” 의미를
plan publish가 함께 지워 규칙이 분산됐다.

변경 분류: B-CCU4-1/2는 채택된 D-H1을 구현하는 과정에서 생긴 **B 기존 결함**이다. 새 계약이 필요한
D 항목이나 상위 계층 우회 C로 판정하지 않는다.

차단 항목 수 / 채택 가능 여부: 2 / 채택 불가
