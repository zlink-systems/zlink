# review-CCU-3 — review-CCU-2 차단 3건 해소 재검증

검토 대상은 worktree `/home/hep7hep7/project/zlink-work/ccu2`의 미커밋 diff와
`/home/hep7hep7/project/zlink-work/all-artifacts/CCU-3.patch`
(SHA-256 `b33ce3c4d5aa46fe1a6748f5e580a79fc721d445c36083cadffc8771fbe69816`)이다.
artifact에는 `core/**` 13파일과 신규
`core/tests/unittest/unittest_auto_hwm_incremental_plan.cpp`가 들어 있다. 정적 읽기와 grep만
사용했고 소스·스펙·테스트 수정, 빌드·실행, 커밋은 하지 않았다.

## 결론

**재검증은 미통과다.** B-CCU2-1의 attach-path 할당 예외, B-CCU2-2의 새 generation 조기 공표,
B-CCU2-3의 registry/queue 이중 상태는 각각 해소됐다. 그러나 증분 성공 뒤 기존 방향의 목표 인하를
수행할 debounce 재계산을 예약하는 호출이 없다. 따라서 채택된 D-H1 계약의 수렴 단계가 실행되지
않으며, 기존 방향은 다음 unrelated topology/option 변경까지 이전 목표를 계속 사용한다. 또한 신규
테스트 대부분은 의도와 달리 증분 경로에 진입하지 않고 full fallback을 비교한다.

| 검증 항목 | 판정 | 요약 |
|---|---|---|
| 1. B-3 단일 소유 | **해소** | 새 지속 필드는 plan의 3개뿐이고 registry/queue 복제 상태·flag는 제거됐다. 증분은 context의 applied plan 하나를 복사해 계산한 뒤 같은 소유자에 기록한다. detach와 queue-ID 반례는 보수적 full fallback으로 닫힌다 |
| 2. B-1 allocation 경계 | **해소** | 정책/handle 배열은 각각 고정 길이 2이고 증분 함수에 컨테이너 할당이 없다. `bad_alloc→ENOMEM`, 기타 예외→`EFAULT` backstop도 full pass와 같다 |
| 3. B-2 직렬화·공표 | **해소** | recalc mutex가 registry 확장, pipe `_hwm` 적용, plan/generation 기록 전부를 덮고 기록은 적용 뒤다. 확인한 중첩 lock에는 새 역순이 없다 |
| 4. 신규 7 tests | **부분** | 7개가 등록됐지만 앞 6개 중 증분을 의도한 케이스도 socket 생성이 pending generation을 올려 full fallback한다. 비교 field와 `total_applied` 완화 범위도 불완전하며 동시 attach에는 시작 barrier가 없다 |
| 5. W-1/W-2·범위·플랫폼 | **부분** | W-1은 위 테스트 공백 때문에 미해소. W-2의 포화 전용 설명은 없어졌지만 “debounced replan” 주석은 실제 호출과 불일치한다. public header/export 변경과 새 platform API는 없다 |

## 1. B-CCU2-3 — 단일 소유

### 지속 상태와 증분의 읽기/쓰기

- 새 plan 지속 필드는 `application_auto_role`, `application_auto_direction_count`,
  `max_planned_queue_id` 세 개뿐이다
  (`core/src/runtime/core/auto_hwm_policy.hpp:68-78`). 생성자와 full finalize가 세 필드를 모두
  초기화한다(`auto_hwm_policy.cpp:156-176`, `:346-365`).
- `physical_queue_record_t`에는 `plan_member`, `plan_maximum_bytes`가 없고
  (`ctx_physical_queue_registry.cpp:144-180`), registry data member에도 CCU-2의 `_plan_*` 8필드가
  없다(`ctx_physical_queue_registry.hpp:172-185`). 해당 이름과 `extension_t::added`를 grep한 결과도
  0건이다.
- 증분 진입은 `_auto_hwm_state_sync` 아래 `_auto_hwm.applied_plan()` 하나를 지역 plan으로 복사한다
  (`ctx_auto_hwm_recalc.cpp:132-147`). registry extension은 이 plan만 입력으로 읽고 같은 객체에
  aggregate와 세 필드를 되쓴다(`ctx_physical_queue_registry.cpp:958-1119`). 마지막 publish도
  `_auto_hwm.record_applied_plan(plan, generation)` 한 곳이다
  (`ctx_auto_hwm_recalc.cpp:191-193`). registry나 queue record에 별도 membership/extendable guard는
  남지 않았다.
- socket의 기존 `_auto_hwm_context_plan` 복사본은 새 세 필드의 소비자가 아니다. 검색 결과 새
  세 필드의 증분 판정 read는 registry extension에만 있으며, socket monitor는 기존
  `enabled/profile`만 읽는다(`socket_base_monitor.cpp:121-122`). 따라서 증분 결정의 두 번째
  소유자로 작동하지 않는다.

### detach 무효화와 종료 경로

표준 pipe 종료는 하나의 순서를 공유한다. 최종 ack가 먼저 socket의 `pipe_terminated()`를 호출하고
(`core/src/runtime/core/pipe_transport.cpp:321-328`), 그 callback은
`schedule_auto_hwm_recalculate()`를 호출한다
(`core/src/runtime/sockets/common/socket_base_api.cpp:2016-2022`). 물리 endpoint retirement는 callback
뒤 inbound cleanup 또는 pipe destructor에서 수행된다
(`pipe_transport.cpp:330-372`, `pipe.cpp:339-350`, `:407-418`).

- 정상 close와 context term은 attached pipe에 `terminate(false)`를 보낸 뒤 위 최종 ack 경로로
  수렴한다(`socket_base_lifecycle.cpp:1398-1425`). context term은 그 전에 recalc task를 중단하며
  (`ctx.cpp:187-203`), stop은 recalc mutex를 획득한 뒤 stopped를 기록한다
  (`ctx_auto_hwm_recalc.cpp:65-77`). 그러므로 schedule이 stopped 때문에 no-op인 최종 teardown에는
  이후 증분 진입 자체가 없다(`:135-137`).
- pair reject는 관련 pipe에 `terminate(false)`를 호출한다
  (`socket_base_api.cpp:443-471`). ROUTER handover/reject의 실제 종료 대상도
  `terminate(true)`를 거쳐 같은 pipe 종료 callback으로 수렴한다
  (`sockets/router/router_admission.cpp:419-439`). standby로만 강등되는 pipe는 endpoint를 은퇴하지
  않으므로 invalidation 대상이 아니다.
- attach extension의 첫 generation guard 뒤 detach schedule이 겹칠 수는 있다. 그러나 두 연산은
  시간상 겹치므로 extension을 먼저 선형화할 수 있고, `record_applied_plan()`은 자신이 읽은 generation과
  더 최신 pending generation이 다르면 최신 요청을 소비하지 않는다
  (`ctx_auto_hwm_state.cpp:287-297`). detach가 extension 시작 전에 끝났다면 schedule이 retirement보다
  먼저이므로 `pending != last_applied` guard가 확실히 full fallback시킨다
  (`ctx_auto_hwm_recalc.cpp:138-145`).
- destructor의 직접 retirement backstop은 rejected/partially constructed pipe도 다루지만
  (`pipe.cpp:339-349`), socket plan에 들어간 attached pipe는 위 sink callback을 거친다. plan에 들어가기
  전 파괴된 pipe에는 무효화할 plan membership이 없다.

### `queue_id > max_planned_queue_id` 반례

- ID allocator는 live ID를 재사용하지 않고 `UINT64_MAX`에서 1로 wrap한 뒤 빈 ID만 고른다
  (`ctx_physical_queue_registry.cpp:383-395`). wrap 뒤 낮아진 ID는 `<= max`라 증분을 거절한다
  (`:1000-1005`). 성능상 full fallback일 뿐 잘못 확장하지 않는다.
- inproc pair 두 번째 attach는 이미 full plan이 덮은 동일한 두 physical queue ID를 다시 제시하므로
  같은 비교에서 full fallback한다. 이 비교는 membership bit 없이도 보수적으로 안전하다.
- 동시 생성된 두 pipe가 ID 순서와 반대로 attach되면 높은 ID가 먼저 max를 올리고 낮은 ID는
  fallback한다. 순서대로 attach되면 둘 다 직렬 증분할 수 있다. recalc mutex가 두 extension 전체를
  직렬화하므로 같은 max를 동시에 읽는 경우는 없다(`ctx_auto_hwm_recalc.cpp:126-130`).

따라서 B-CCU2-3의 **plan 상태 이중 소유 문제는 해소**됐다. CCU-2 대비 규칙 수는 registry/record의
10개 복제 상태와 membership/extendable invalidation 규칙에서 plan의 3개 필드와 보수적 ID 비교
하나로 줄었다.

## 2. B-CCU2-1 — attach 할당과 예외 경계

- socket은 `physical_queue_endpoint_policy_t policies[2]`를 사용한다
  (`core/src/runtime/sockets/common/socket_base.cpp:313-333`). 두 원소의 `shared_ptr` 복사는 참조수만
  변경하며 이 코드에는 vector growth가 없다.
- registry도 `physical_queue_handle_t extensions[2]`만 사용하고 입력이 2개를 넘으면 변경 전에
  거절한다(`ctx_physical_queue_registry.cpp:228-230`, `:967-980`). 이후에는 map lookup, 고정 record
  갱신과 정수 계산뿐이다.
- ctx backstop은 registry extension 전체를 감싸 `std::bad_alloc`을 `ENOMEM`, 나머지를 `EFAULT`로
  변환한다(`ctx_auto_hwm_recalc.cpp:168-181`). 이는 full pass의 변환과 같다(`:239-272`).
- false 결과는 두 attach 호출부 모두 즉시 `auto_hwm_recalculate_now()`로 fallback한다
  (`socket_base_api.cpp:316-328`, `:636-650`). extension이 policy 한쪽을 먼저 기록했거나 끝부분 aggregate
  overflow에서 target을 먼저 쓴 경우에도 full planner가 현재 attached pipe 전체의 policy와 registry
  target/aggregate를 다시 만든다(`ctx_physical_queue_registry.cpp:838-945`).

따라서 B-CCU2-1은 **해소**다. 공개 attach 자체의 기존 allocation까지 0이라는 뜻은 아니고, 이번에
추가한 증분 plan 경로가 새 heap allocation을 만들지 않는다는 범위로 판정했다.

## 3. B-CCU2-2 — recalc mutex, lock order, snapshot

`_auto_hwm_recalc_sync`는 함수 진입 `ctx_auto_hwm_recalc.cpp:130`에서 잡혀 반환까지 유지된다. 그 안에서
registry 확장(`:173-175`), attaching pipe의 `_hwm` 적용(`:186-189`), context plan과 budget generation
기록(`:191-193`)이 이 순서로 완료된다. full pass도 같은 mutex 아래 모든 socket/pipe 적용 뒤 기록한다
(`:196-265`). 따라서 새 generation을 관찰한 snapshot에는 attaching pipe의 admission HWM이 이미
적용되어 있다.

정적 lock 순서는 다음과 같다.

- 증분: `recalc → state`(해제), `recalc → opt`(해제), `recalc → registry`(해제),
  `recalc → pipe _out_sync → registry`(해제), `recalc → socket _auto_hwm_sync`(해제),
  `recalc → state`.
- full: `recalc → state`, `recalc → slot`, `recalc → opt`, `recalc → registry`, 그 뒤 socket별
  `recalc → socket _auto_hwm_sync → monitor sync → pipe _out_sync → registry`
  (`ctx_auto_hwm_recalc.cpp:198-265`, `socket_base.cpp:269-301`, `pipe.cpp:986-1005`).
- option setter는 `socket _auto_hwm_sync → monitor sync → pipe _out_sync`를 사용하지만 이들을 모두
  해제한 뒤 schedule/recalc를 호출한다
  (`socket_base_api.cpp:683-714`, `socket_base_lifecycle.cpp:1455-1469`). detach도 socket-local receive,
  pair, monitor lock을 해제한 뒤 schedule하고 registry retirement는 callback 반환 뒤다
  (`socket_base_api.cpp:1964-2022`, `pipe_transport.cpp:321-372`). 확인한 경로에는
  `registry → pipe`나 `socket lock → recalc`가 중첩된 새 역순이 없다.

Snapshot의 `budget_generation`, `total_planned_hwm_bytes`, `total_applied_hwm_bytes`는 하나의
`_auto_hwm_state_sync` 구간에서 같은 `_applied_plan`으로 복사된다
(`ctx_auto_hwm_recalc.cpp:347-350`, `ctx_auto_hwm_state.cpp:306-338`). 새 plan record가 pipe 적용 뒤에만
publish되므로 B-CCU2-2가 지적한 “새 generation + 이전 pipe HWM” 조합은 닫혔다.

다만 snapshot이 state plan을 복사한 뒤 registry live counter를 별도 lock으로 sampling하고
`active_directional_queue_count`를 덮어쓰는 기존 구조는 남아 있다
(`ctx_auto_hwm_recalc.cpp:347-352`, `:380-383`). 이 순서는 HEAD에도 동일하므로 이번 수정이 만든
회귀로 세지 않았지만, 스펙 §3의 “한 generation의 registry view”를 더 강하게 검증할 후속 항목으로
S-CCU3-1에 남긴다.

## 4. 신규 차단 — debounce 수렴이 예약되지 않는다

### B-CCU3-1 — successful extension은 기존 방향 인하를 영구히 미룬다

채택된 Auto-HWM 스펙 §2는 연결 증가로 목표가 감소하면 attaching 방향은 즉시 기록하고, 기존 방향의
목표 인하는 option 변경과 같은 **debounce 재계산 경로가 기록**한다고 명시한다
(`core/doc/spec/core/systems/06-auto-hwm.ko.md:156-162`). 구현은 새 방향만 갱신한다고 명시하며
(`ctx_physical_queue_registry.cpp:1079-1088`), 성공하면 pipe apply와 plan record 뒤 곧바로 반환한다
(`ctx_auto_hwm_recalc.cpp:186-193`). 이 성공 구간 어디에도 `schedule_auto_hwm_recalculate()` 호출이 없다.

호출부도 성공 시 아무 작업을 하지 않고, false일 때만 동기 full replan을 수행한다
(`socket_base_api.cpp:323-326`, `:645-649`). `record_applied_plan()`은 pending generation을 새로 만들지
않고 현재 plan을 기록할 뿐이다(`ctx_auto_hwm_state.cpp:268-298`). 저장소 전체 schedule 호출은 socket
생성, option 변경, detach와 extension 실패/비활성 attach 경계뿐이며 successful extension 뒤의
debounce 예약은 없다.

따라서 비포화 구간에서 한 socket에 연결을 연속 추가해 증분이 성공하면 기존 방향의
`planned_hwm`/pipe `_hwm`은 자동으로 새 level에 수렴하지 않는다. 우연한 다음 socket 생성, option 변경,
detach가 있어야 full pass가 발생한다. 이는 D-H1이 허용한 “debounce까지의 지연”이 아니라 종료 시점이
없는 지연이며 **채택 차단**이다.

## 5. 테스트 재검증

신규 executable과 7개 `RUN_TEST` 등록은 확인했다
(`core/tests/unittest/CMakeLists.txt:17`,
`core/tests/unittest/unittest_auto_hwm_incremental_plan.cpp:264-277`). 그러나 W-CCU2-1은 완전히
해소되지 않았다.

### W-CCU3-1 — 대부분의 비교가 증분 경로를 실행하지 않는다

`compare_attach_steps()`는 반복마다 새 socket을 만든 뒤 connect한다
(`unittest_auto_hwm_incremental_plan.cpp:75-100`). 그런데 socket 생성은 항상
`schedule_auto_hwm_recalculate()`로 pending generation을 올린다(`core/src/runtime/core/ctx.cpp:232-274`).
이어지는 attach extension은 `pending_generation != last_applied_generation`이면 즉시 false다
(`ctx_auto_hwm_recalc.cpp:138-145`). 따라서 기본 3000 ms debounce 안에 실행되는 unsaturated,
saturation 전이, finite manual 케이스의 각 “extended” snapshot은 실제로 attach caller의 동기 full
fallback 결과다. unlimited manual과 mixed role도 같은 helper를 쓰므로 어느 거절 조건 때문에
fallback했는지 관찰하지 못한다. completed inproc pair는 의도대로 fallback 대상이지만 증분 대조가 아니다.

동시 attach 케이스는 socket 8개를 먼저 생성하므로 첫 attach는 확실히 full fallback하고, 그 full pass가
동시에 attach된 다른 pipe까지 수집하는 timing에 따라 나머지도 ID guard로 fallback할 수 있다
(`unittest_auto_hwm_incremental_plan.cpp:221-258`). fast-path 진입을 직접 확인하는 hook/assert가 없어
“증분 vs full” 회귀 테스트라는 완료 조건을 충족하지 않는다.

### W-CCU3-2 — 비교 field와 D-H1 완화 범위가 넓다

`assert_plan_equal()`은 effective budget, planned/manual/count/flags만 비교하고
`configured_memory_limit_bytes`, `runtime_memory_limit_bytes`, `resolved_memory_limit_bytes`,
`configured_core_budget_bytes`는 비교하지 않는다
(`unittest_auto_hwm_incremental_plan.cpp:13-43`). “plan 소유 field 전부”라는 주석과 맞지 않는다.

또한 모든 호출에서 `extended.total_applied_hwm_bytes >= replanned.total_applied_hwm_bytes`만 요구한다
(`:44-52`). 이 helper는 증분 목표 인하뿐 아니라 unlimited manual, mixed role, completed inproc pair처럼
반드시 full fallback해야 하는 케이스에도 그대로 쓰인다(`:149-216`). D-H1 완화는 “증분이 실제 성공했고
기존 방향 목표가 낮아진 경우”에만 허용돼야 하므로 현재 assertion은 fallback의 applied aggregate
오류를 가릴 수 있다.

### W-CCU3-3 — 동시 attach 검증이 결정적이지 않다

두 작업을 `std::async`와 현재 thread에서 시작하지만 동시 출발 barrier가 없다
(`unittest_auto_hwm_incremental_plan.cpp:233-247`). 한쪽이 네 attach를 마친 뒤 다른 쪽이 시작해도
통과한다. generation도 “정확히 attach당 1회”라는 주석과 달리 `>= before + 8`만 검사한다
(`:249-255`); periodic debounce/full fallback의 추가 publish를 허용하므로 직렬화와 누락 없음의 증거가
아니다. 결과 자체가 timing에 따라 실패할 가능성보다는, 경쟁 구간을 실행했다는 보장이 없는
coverage 비결정성 문제다.

## 6. W-CCU2-2 주석, 범위와 플랫폼

- 포화일 때만 성공한다는 이전 header 설명은 제거됐다. 그러나 새 header는 모든 자동 방향이 “one
  level”이라고 쓰는데 remainder가 있으면 stable-ID prefix는 `level+1`, 나머지는 `level`이다
  (`ctx_physical_queue_registry.hpp:142-147`, 구현 `ctx_physical_queue_registry.cpp:1050-1073`). 더구나
  “debounced replan republishes it”은 B-CCU3-1 때문에 현재 사실이 아니다. W-CCU2-2는 **부분 해소**다.
- `git diff HEAD -- core/include core/src/libzlink.vers`는 비어 있다. 공개 header·export/ABI 변경은 없다.
- 새 production 코드는 고정 C++ 배열, `std::shared_ptr`, 기존 mutex/error API만 쓴다. 신규 test의
  `std::future`, `std::async`, `std::to_string`도 저장소의 다른 Windows/macOS 대상 test와 source에서
  이미 사용하는 표준 API다. 정적으로 명백한 Windows/macOS 컴파일 비호환은 찾지 못했다. 해당
  플랫폼 build는 리뷰 제한 때문에 실행하지 않았다.
- W-CCU2-3 hotpath 1셀은 이번 보고서 입력상 merge gate로 이관됐고, 읽기 전용 지시 때문에 재측정하지
  않았다. 해소 증거는 이번 artifact에 없다.

## 7. 신규 B/W/S와 제안

| ID | 등급 | 내용 |
|---|---|---|
| B-CCU3-1 | **B** | successful incremental attach가 기존 방향 인하를 위한 debounce full replan을 예약하지 않아 D-H1 수렴 계약을 위반 |
| W-CCU3-1 | **W** | 신규 비교 test 대부분이 socket-create pending generation 때문에 증분이 아니라 full fallback을 실행 |
| W-CCU3-2 | **W** | plan input field 비교 누락, `total_applied >= full` 완화를 fallback 케이스까지 적용 |
| W-CCU3-3 | **W** | concurrent attach에 start barrier와 정확한 publish 판정이 없어 경쟁 실행이 결정적이지 않음 |
| S-CCU3-1 | **S** | state plan 복사와 registry live sampling 사이의 기존 generation 일관성도 별도 contract test/선형화 검토 필요 |

B-CCU3-1의 단순 대안은 성공 뒤 기존 schedule API를 호출하는 것이지만, 현재
`pending != last_applied` guard 때문에 그 다음 burst attach부터 동기 full fallback이 되어 CCU 목적을
잃는다. 다른 대안은 “deferred shrink가 예약된 generation”과 detach/input invalidation을 plan 소유자
안에서 구분해 연속 증분을 허용하는 것이다. 후자는 상태/규칙을 함부로 늘리면 B-CCU2-3을 재발시킨다.
두 대안을 성능과 단일 소유 조건으로 다시 비교하고, 수렴 의무와 invalidation 이유를 한 소유자가
표현하도록 설계해야 한다(POSDDD `doc/principal/dev/posddd.ko.md:195-207`, `:262-264`, `:305-309`).

변경 분류: 기존 성능 결함은 **B**, 기존 방향 인하 지연은 채택된 **D-H1**이나 현재 구현의 무기한
미수렴은 신규 **B-CCU3-1**이다.

차단 항목 수 / 채택 가능 여부: 1 / 채택 불가
