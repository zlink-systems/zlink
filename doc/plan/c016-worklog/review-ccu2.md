# review-CCU-2 — attach 증분 Auto-HWM plan 차단 검증

검토 시각 2026-09-08, 대상은 `~/project/zlink-work/all-artifacts/CCU-2.patch`의 9개 파일이다.
18:25 KST에 감독관이 D-H1을 채택하면서 같은 worktree의 한·영 Auto-HWM 스펙 2개와
`REVIEW-NOTE-D-H1.md`가 별도로 추가됐다. 이 리뷰는 그 결정을 최신 계약으로 적용하되,
artifact 밖의 스펙 변경을 CCU-2 구현 diff로 세지 않았다. 소스·스펙·테스트는 수정하지 않았다.

## 결론

**차단 검증은 미통과다.** 증분식의 새 방향 목표와 숫자 aggregate는 전체 water-filling과
비트 단위로 일치하고, 폴백·직렬화의 주요 경계도 대체로 맞는다. 그러나 (1) attach fast path에
새로 생긴 `std::vector` 할당 예외가 기존 오류 경계를 우회하고, (2) plan generation을 pipe-local
admission HWM보다 먼저 공표하며, (3) 마지막 plan이라는 같은 사실을 context state와 registry의
10개 새 지속 상태로 중복 보관해 공통 규칙과 POSDDD를 위반한다.
성능/TSan 결과는 이 두 정적 차단을 해소하지 않는다.

| 검증 항목 | 판정 | 요약 |
|---|---|---|
| 1. 증분 확장 정확성 | **부분** | 새 최대 ID의 목표와 `level/remainder/total` 숫자는 동일. 기존 방향을 갱신하지 않으므로 실제 전체 queue plan과는 동일하지 않지만, 이 부분은 감독관이 D-H1로 계약에 채택했다 |
| 2. 폴백 완전성·동시성 | **부분** | role/input/pending/detach/wrap/부족 폴백과 attach 직렬화는 확인. finite manual과 일부 overflow는 보고서 설명과 달리 성공 경로다. allocation 예외와 plan publication 순서는 미해소 |
| 3. TSan·hotpath | **부분** | TSan 8/8 PASS, report 0. hotpath는 4/5 PASS, req/rep 셀 하나가 -5.52%로 대칭 gate FAIL |
| 4. D 항목 범위 | **부분** | D-H1 채택은 확인. 보고서의 발동식과 리팩터 규모 설명은 과소·부정확 |
| 5. POSDDD·scope | **미해소** | public ABI 변경은 없으나 새 flag/state가 명백히 있고 동일 plan 사실을 이중화 |
| 6. 신규 이슈 | **미해소** | B 3건, W 3건, S 1건 |

## 1. 증분식과 전체 재계산의 손 대조

자동 방향이 모두 같은 role이고 자동 방향 수를 `A=N'`, 역할 하한/상한을 `m/M`, 유한 manual
예약 합계를 `R`, 전체 budget을 `B`, `D=B-R`라 두면 충분한 budget에서 전체 planner
(`auto_hwm_policy.cpp:446-500`)는 다음과 같다.

```text
remaining = D - A*m
L = min(M, m + floor(remaining/A))
r = (L == M) ? 0 : remaining mod A
targets = stable queue-ID 앞 r개가 L+1, 나머지가 L
total_planned = R + A*L + r
```

`extend_application_plan()`의 식(`ctx_physical_queue_registry.cpp:1103-1122`)과 정확히 같다.
전체 경로는 queue-ID 정렬 map을 그대로 plan 배열로 만든다(`:848-917`). 모든 자동 방향의 상한이
같으므로 `:456-500`의 반복은 공통 share를 올린 뒤 남은 `r<A` byte를 앞에서 한 개씩 배정한다.
따라서 새 ID가 기존 최대보다 크다는 검사(`:1054-1059`)를 통과한 새 방향은 remainder prefix에
들어가지 않고 정확히 `L`을 받는다.

손 계산은 `m=10`, `M=20`, `R=3`으로 했다. `D`는 manual을 뺀 data budget이다.

| N' | 비포화 D | L | r | 전체 재계산 target(ID 순) | total=`R+N'L+r` | 증분 숫자 |
|---:|---:|---:|---:|---|---:|---:|
| 1 | 14 | 14 | 0 | 14 | 17 | 17 |
| 2 | 29 | 14 | 1 | 15, 14 | 32 | 32 |
| 3 | 44 | 14 | 2 | 15, 15, 14 | 47 | 47 |
| 4 | 59 | 14 | 3 | 15, 15, 15, 14 | 62 | 62 |
| 5 | 74 | 14 | 4 | 15, 15, 15, 15, 14 | 77 | 77 |

포화는 각 N'에서 `D=20*N'+7`로 계산했다. 모두 `L=20`, `r=0`이고 target은 N'개 모두 20,
`total_planned`는 차례로 23, 43, 63, 83, 103이다. 상한 뒤 남은 7 byte는 배정하지 않으며 양 경로가 같다.

경계도 대체로 안전하다. `minimum*N'`, `level*N'`, remainder와 manual 합산은
`multiply_snapshot_value`/`add_snapshot_value`로 포화 검사를 한다(`:224-242`, `:1108-1124`).
보호 없는 `share*N'`는 `share=floor(remaining/N')`라 `remaining`을 넘지 않는다.
`manual_reserved`는 전체 경로에서 먼저 모아(`auto_hwm_policy.cpp:397-435`) budget에서 빼고,
증분 경로도 `_plan_manual_reserved_bytes`를 빼고 마지막에 다시 더한다.

다만 “모두 하나의 level”은 remainder가 0이 아닐 때 문자 그대로는 틀리며, 전체 **queue plan**도
비트 동일하지 않다. 예를 들어 `m=10, M=20, R=3, B=46`이면 A=2의 기존 target은 `[20,20]`,
A=3의 전체 재계산은 `[15,14,14]`, 합계 46이다. 증분 경로는 붙는 방향만 14로 쓰므로
실제 현재 target 합은 `3+20+20+14=57`인데 snapshot 숫자는 46을 기록한다
(`ctx_physical_queue_registry.cpp:1126-1156`). 이는 원 보고서가 말한 deferred applied 차이가 아니라
기존 방향의 **planned target 자체**가 다른 상태다. 다만 감독관의 18:25 D-H1 결정은 바로 이 편차를
허용하도록 §2의 연결 증가 행을 개정했고, 별도 note에서 planned 합계 불일치도 D-H1 범위로 판정했다.
그러므로 이 리뷰에서는 신규 blocker로 다시 세지 않는다.

## 2. 폴백과 동시성

| 경계 | 구현 근거 | 판정 |
|---|---|---|
| 자동 role 혼합 | 전체 plan이 uniform 여부를 기록(`registry.cpp:943-974`), 새 role 불일치 거절(`:1038-1049`) | 해소 |
| manual | 새 endpoint manual 및 합쳐진 finite manual은 거절(`:1018-1024`, `:1038-1043`). 기존 **finite** manual은 폴백하지 않고 `R`로 정확히 계산. 기존 unlimited는 aggregate invalid라 확장 불가(`:970-974`) | 부분 — 동작은 타당하나 보고서의 “manual 관여 시 폴백”은 부정확 |
| budget 부족 | 이전 plan 부족은 `_plan_extendable=false`, 새 하한 합이 D를 넘으면 거절(`:970-974`, `:1108-1111`) | 해소 |
| overflow | count/plan 산술 overflow는 거절(`:1063-1085`, `:1108-1124`). 새 applied/send/recv aggregate의 뒤쪽 overflow는 flag+insufficient를 기록하고 성공(`:1130-1168`) | 부분 — “overflow면 항상 폴백”은 부정확 |
| 예약된 재계산·입력 변경 | pending과 last-applied가 다르면 거절, profile/memory/core 입력을 전부 비교(`ctx_auto_hwm_recalc.cpp:128-159`) | 해소 |
| detach·queue ID wrap | plan member 최종 해제 시 invalidation(`registry.cpp:1351-1356`), 새 ID가 이전 max 이하면 거절(`:1054-1059`) | 해소 |
| SNDHWM/RCVHWM setter | 성공 뒤 재계산 예약(`socket_base_api.cpp:706-714`), 다음 fast path는 pending generation에서 막힘 | 해소 |
| ctx 종료 | `_auto_hwm_recalc_sync`로 stop/full/증분을 직렬화하고 증분은 record 직전 stopped를 다시 확인(`ctx_auto_hwm_recalc.cpp:126-169`) | 해소 |
| 동시 attach | 동일 recalc mutex로 직렬화. 성공 1회마다 `record_applied_plan()` 한 번, budget generation 한 번 증가(`ctx_auto_hwm_state.cpp:268-298`) | 해소 |
| lock order | 증분 경로는 recalc → (각각 짧은 state/opt/registry)이고 pipe apply는 recalc 해제 뒤 수행. 확인한 경로에서 새 역순 중첩은 없음 | 해소 |

## 3. 신규 B/W/S

### B-CCU2-1 — attach의 allocation 예외가 runtime 경계를 탈출한다

`socket_base_t::auto_hwm_extend_plan_for_attached_pipe()`는 새 `std::vector`에 두 번 `push_back`한다
(`socket_base.cpp:320-328`). 이 코드는 try/catch 밖이다. 기존 전체 재계산은 전체 planning과 vector
할당을 `bad_alloc → ENOMEM`, 그 외 예외를 `EFAULT`로 변환한다
(`ctx_auto_hwm_recalc.cpp:217-250`). attach caller는 반환값만 보고 fallback할 뿐 예외를 받지 않으며
(`socket_base_api.cpp:319-326`, `:645-649`), command dispatch에도 예외 경계가 없다
(`object.cpp:64-105`, `io_thread.cpp:54-70`). 메모리 압박에서 worker/public 호출 경계 밖으로 예외가
빠져나가 process termination 또는 이미 공개된 attach의 반쪽 상태가 될 수 있다. **채택 차단**이다.

### B-CCU2-2 — 새 generation을 pipe-local admission HWM보다 먼저 공표한다

증분은 registry 확장 뒤 state에 새 plan과 `budget_generation`을 먼저 기록하고
(`ctx_auto_hwm_recalc.cpp:161-169`), recalc mutex를 놓고 반환한 다음 붙는 pipe의 실제 writer `_hwm`을
적용한다(`socket_base.cpp:331-340`, `pipe.cpp:986-1005`). 반면 full path는 모든 socket/pipe에 plan을
적용한 다음에만 record한다(`ctx_auto_hwm_recalc.cpp:230-243`). Context snapshot은 state에서 generation과
plan을 복사한다(`ctx_auto_hwm_recalc.cpp:307-330`, `ctx_auto_hwm_state.cpp:306-338`). 따라서 동시 snapshot은
새 generation과 “적용된” 합계를 보면서 새 pipe의 admission은 아직 이전 `_hwm`을 쓰는 상태를 관찰할 수 있다.

동시 attach도 recalc/record까지만 직렬화되고 각 pipe apply는 mutex 밖이므로, attach B가 다음 generation을
기록한 뒤 attach A가 이전 generation의 pipe 적용을 끝내는 순서도 가능하다. 데이터 레이스가 아니라
공표 선형화 순서의 의미적 race라 TSan 통과로 해소되지 않는다. 최신 스펙 §3:330-333의 동일 generation
snapshot과 §4:477-480의 admission 목표 적용을 깨므로 **채택 차단**이다.

### B-CCU2-3 — 금지된 새 상태와 동일 plan 사실의 이중 소유

원 보고서의 “새 옵션·플래그·상태·타이머 0”은 사실이 아니다. queue record에
`plan_member`, `plan_maximum_bytes` 2개(`registry.cpp:161-185`), registry에 count 4개,
manual reserve, max ID, role, `_plan_extendable`의 8개(`registry.hpp:175-189`)를 추가했다.
이 가운데 `plan_maximum_bytes`는 쓰기만 하고 읽지 않으며, `extension_t::added`도 읽지 않는다.
registry의 count/manual/role은 이미 `_auto_hwm.applied_plan()`이 소유한 같은 plan 사실을 복제하고,
일치 여부 guard와 detach invalidation flag를 새 규칙으로 요구한다(`registry.cpp:998-1003`, `:1351-1355`).

반대로 `resolved_direction_t`/`resolve_direction()`을 full path와 incremental path가 함께 호출하는
구조(`registry.cpp:244-296`, `:882-885`, `:1038-1043`)는 두 endpoint policy의 merge 규칙을 실제로
한곳에 모았으므로 그 한 항목은 해소됐다. 그러나 이 국소 중복 제거보다 새 plan 복제 상태와
동기화 규칙이 더 많이 추가됐다.

이는 공통 규칙의 “새 옵션·플래그·규칙 추가 금지” 및 POSDDD의 단일 설계 결정·단일 소유자
(`posddd.ko.md:195-207`, `:262-264`, `:305-309`)에 정면으로 어긋난다. 수정 전은 full planner와 plan
표현이 각각 하나였지만, 수정 후는 full/incremental planner 둘, plan 사실 두 벌, invalidation/fallback
규칙이 생겼다. 규칙 수가 줄지 않았다. **채택 차단**이다.

### W-CCU2-1 — 신규 fast path 집중 회귀 테스트가 없다

artifact의 9개 변경 파일에는 테스트가 없다. 기존 `test_ctx_options.cpp:795-817`은 첫 attach의 generation
증가만 확인하고, N'=1..5 remainder, 포화↔비포화, finite/unlimited manual, role mix, option/detach race,
ID wrap을 직접 검증하지 않는다. 지시된 `core/tests/scale_test_*` 파일은 저장소에 존재하지 않았다.

### W-CCU2-2 — 선언·주석과 구현이 반대다

header와 구현 주석은 “기존과 확장 plan이 모두 role maximum인 경우만 성공”이라고 한다
(`registry.hpp:137-145`, `registry.cpp:995-997`). 실제 `_plan_extendable`은 saturation을 검사하지 않고
비포화도 성공시킨다(`registry.cpp:970-974`, `:1103-1168`). POSDDD의 “옆 주석이 새 동작을 정확히
설명” 규칙(`posddd.ko.md:652-666`)에도 맞지 않는다.

### W-CCU2-3 — hotpath 대칭 gate 1셀이 실패했다

Release+LTO binary를 idle/lock 조건에서 한 번 측정한 결과 `dealer_router_reqrep_inproc`이 기준보다
5.52% 적은 instruction으로 대칭 ±5% 범위를 0.52%p 벗어났다. 감소 방향이므로 성능 회귀의 증거는
아니고 패치가 message hot path를 직접 바꾸지도 않지만, 저장소 gate의 공식 판정은 FAIL이다.
gate 자체는 지시의 `bindings/c/perf`가 아니라 `core/tests/perf/hotpath_gate.py`와 CTest가 소유한다.
반복 측정으로 덮지 않았으며, base binary 대조나 승인된 reference 처리 없이는 gate 미해소로 남긴다.

### S-CCU2-1 — D 없는 O(1) 설계는 plan descriptor 하나가 소유해야 한다

후속 리팩터를 한다면 queue마다 level을 복제하지 말고 registry가 immutable plan descriptor
`{generation, role별 level, remainder cutoff}` 하나를 소유하고 admission이 generation-aware target을
파생하는 안이 맞다. role 하나의 level만으로는 mixed role, manual reserve, queue-ID remainder prefix를
표현하지 못한다. 현재 `planned_hwm`/apply/aggregate 관련 참조는 core source/test 15개 파일 50곳이라,
현실적 범위는 production 8~12파일 약 400~800 LOC, 집중 test 3~5개 약 200~400 LOC와 hotpath gate,
대략 2~4 engineer-day다. 0.17.4의 작은 보정으로 볼 크기가 아니다.

## 4. D-H1 범위 재판정

정확한 비포화 조건은 원 보고서의 단순 `N*role최대 > budget`가 아니라 다음이다.

```text
A * roleMaximum + finiteManualReserved > effectiveBudget(all directional Q)
```

여기서 A는 자동 방향 수이고 effective budget은 전체 계획 방향 Q에 따라 달라질 수 있다
(`auto_hwm_policy.cpp:98-110`, `:438-450`). 실제 기존 target 편차는 grow 전후 level 또는 remainder
prefix가 바뀔 때만 생긴다. 양쪽 모두 상한 포화이거나 effective-cap의 minimum floor 구간에서 level이
그대로면 편차가 없다. Balanced/STREAM, finite manual 없음, fixed cap 512 MiB라면 상한 128 KiB 기준
4097번째 방향부터 비포화하고 8192 방향의 minimum floor까지가 관련 구간이다. 일반 data 역할의
상한 1 MiB이면 513번째 방향부터라 STREAM만의 문제가 아니다.

감독관의 18:25 D-H1 채택으로 최신 §2:156-162는 “붙는 방향 즉시, 기존 방향은 debounce”를 계약으로
명시한다. 따라서 그 자체는 해소된 D다. 다만 `total_planned_hwm_bytes`를 새 level 숫자로 쓰면서 실제
기존 `planned_hwm` 합과 다른 문제까지 note가 명시적으로 D-H1로 받아들였다는 전제에서만 채택 가능하다.

## 5. 범위·플랫폼·검증

- 공개 `core/include/**`, `core/src/libzlink.vers` diff는 없다. 새 OS API도 없어 Windows/macOS의 명백한
  컴파일 비호환은 정적으로 찾지 못했다. 해당 플랫폼 빌드는 실행하지 않았다.
- runtime 소유 계층은 Core의 Auto-HWM planner/physical registry/pipe이며, 수정 위치 자체는
  `06-auto-hwm.ko.md:545-554`의 소유 표와 맞는다.
- 변경 분류: 성능 원인은 **B(기존 결함)**, 기존 방향 debounce는 채택된 **D-H1**, 이 리뷰의
  B-CCU2-1/2는 신규 회귀, B-CCU2-3은 범위/POSDDD 위반이다.
- 수정 전/후 규칙 수: **1 full plan/1 plan state → full+incremental 2경로/중복 plan state+invalidation 규칙**.

### 실행 결과

| 검증 | 구성·명령 요약 | 결과 |
|---|---|---|
| TSan build | GCC 13.3, RelWithDebInfo, LTO off, `-fsanitize=thread`, JOBS=4 | PASS. `atomic_thread_fence` TSan 미지원 warning은 있었음 |
| TSan tests | `setarch x86_64 -R`, suppression 없음, ctx lifecycle/options/attach/auto-HWM 8개, `-j1` | **8/8 PASS, TSan report 0, 3.36 s** |
| Release+LTO build | `JOBS=4 scripts/build-core.sh release-gate` | PASS |
| hotpath 5셀 | 시작 load1 0.76, ninja 0, available 10668 MiB, `PERF_LOCK` 아래 CTest | **4/5 PASS, gate FAIL** |

| hotpath cell | reference | measured | ratio | 판정 |
|---|---:|---:|---:|---|
| dealer_dealer_inproc | 3230.922 | 3098.853 | 0.9591 | PASS |
| dealer_router_reqrep_inproc | 16455.383 | 15546.832 | 0.9448 | **FAIL** |
| pair_inproc | 2348.457 | 2431.726 | 1.0355 | PASS |
| router_router_tcp | 2972.532 | 3020.074 | 1.0160 | PASS |
| stream_tcp | 13969.806 | 13895.794 | 0.9947 | PASS |

TSan 대상: `test_ctx_destroy`, `test_ctx_term_fixed_rid_handover`, `test_ctx_options`,
`test_inproc_pending_connect_rejected_at_attach`, `unittest_auto_hwm_physical_attempt`,
`unittest_ctx_lifecycle`, `unittest_ctx_runtime`, `unittest_auto_hwm_policy`.

리뷰어가 소스·스펙·테스트를 변경하지 않았고 커밋도 하지 않았다. 18:25에 별도 반영된 감독관의
스펙 2파일 변경은 위 D-H1 입력으로만 읽었다. 남은 실패는 B-CCU2-1/2/3과 hotpath 1셀 gate FAIL이다.

차단 항목 수 / 채택 가능 여부: 3 / 채택 불가
