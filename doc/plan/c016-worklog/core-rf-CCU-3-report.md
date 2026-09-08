# CCU-3 — review-CCU-2 차단 3건·경고 3건 해소 (Core, 0.17.4)

작성 2026-09-08. 담당 job CCU-3. worktree `~/project/zlink-work/ccu2`(base `wip/0.17.3-all2` = 1a79625d3d,
그 위에 감독관이 D-H1 스펙 2파일 + `REVIEW-NOTE-D-H1.md`를 반영해 둔 상태).
패치 `~/project/zlink-work/all-artifacts/CCU-3.patch` — CCU-2 + CCU-3 누적, `core/**`만 담았다
(감독관이 넣은 `core/doc/spec/**`·`REVIEW-NOTE-D-H1.md`와 worklog는 내 변경이 아니므로 제외).
커밋하지 않았다. 공개 인터페이스 무변경(`git diff HEAD -- core/include core/src/libzlink.vers` 비어 있음).

## 0. 결과 요약

| 항목 | 상태 |
|---|---|
| B-CCU2-3 이중 소유 | **해소** — registry 복제 상태 8개 + queue record 2개 전부 삭제, applied plan 단일 소유 |
| B-CCU2-1 allocation 예외 | **해소** — attach 경로 힙 할당 0(고정 배열), ctx 경계에 bad_alloc→ENOMEM/…→EFAULT backstop |
| B-CCU2-2 공표 순서 | **해소** — pipe `_hwm` 적용을 plan·generation 기록 앞으로, recalc mutex를 전체에 걸침 |
| W-CCU2-1 테스트 | **해소** — 신규 `unittest_auto_hwm_incremental_plan` 7 케이스(증분 vs 전체 비트 비교 포함) |
| W-CCU2-2 주석 | **해소** — 헤더·구현 주석을 실제 동작(비포화도 성공, D-H1 참조)으로 정정 |
| W-CCU2-3 hotpath | 감독관 지시로 재측정 생략(데이터패스 무변경, 병합 게이트에서 재측정) |
| 검증 | dev 빌드 OK / 대상 suite until-fail:5 32×5 PASS / 전체 ctest **211/211 PASS** / TSan 9/9 PASS·report 0 / CCU 4000 PASS 212.7 kops |

S-CCU2-1의 plan descriptor 리팩터로 갈 필요는 없었다. 이유는 §1 마지막 단락.

## 1. B-CCU2-3 — 이중 소유 제거 (가장 중요)

**원칙**: "마지막 planning pass가 결정한 것"의 소유자는 `auto_hwm_context_plan_t` 레코드 **하나**다.
registry는 그 사실을 복제하지 않는다. 증분 경로는 그 레코드 하나를 읽고, 계산하고, 같은 레코드에 되쓴다.

### 새 상태 목록 — 전(CCU-2) / 후(CCU-3)

| 위치 | CCU-2 | CCU-3 |
|---|---|---|
| `physical_queue_record_t` | `plan_member`, `plan_maximum_bytes` (2개) | **0개** — 둘 다 삭제 |
| `ctx_physical_queue_registry_t` | `_plan_direction_count`, `_plan_send_count`, `_plan_receive_count`, `_plan_auto_direction_count`, `_plan_manual_reserved_bytes`, `_plan_max_queue_id`, `_plan_auto_role`, `_plan_extendable` (8개) | **0개** — 전부 삭제 |
| `auto_hwm_context_plan_t` (= plan 레코드 자신) | — | `application_auto_role`, `application_auto_direction_count`, `max_planned_queue_id` (3개, `auto_hwm_policy.hpp:66-78`) |
| 규칙 | 일치 guard(4개 필드 비교) + detach invalidation flag + `_plan_extendable` 재계산 | **없음** |

합계 **10개 복제 상태 → 3개 파생 불가 필드**, 그리고 그 3개도 plan 레코드 안에 있어 소유자가 하나다.

- `application_auto_direction_count`: 자동 방향 수. `active_directional_queue_count`(=자동+수동)에서 파생 불가.
- `application_auto_role`: 자동 방향이 공유하는 role. **role이 섞이면 `auto_hwm_role_none`**을 넣는다 —
  "uniform 여부" 불리언을 따로 두지 않고 role 필드 하나가 그 사실도 표현한다(규칙 1개).
- `max_planned_queue_id`: water-filling 나머지가 배정되는 stable queue-ID prefix의 끝. remainder cutoff.

### 파일:행 근거

| 무엇 | 어디 |
|---|---|
| plan 필드 3개 선언(주석 포함) | `core/src/runtime/core/auto_hwm_policy.hpp:66-78` |
| 기본값 초기화 | `core/src/runtime/core/auto_hwm_policy.cpp:171-173` |
| `auto_hwm_context_finalize()`가 매 pass마다 리셋 | `core/src/runtime/core/auto_hwm_policy.cpp:360-362` |
| 전체 재계산이 plan에 직접 publish (registry에 사본 없음) | `core/src/runtime/core/ctx_physical_queue_registry.cpp:918-955` |
| 증분 경로가 plan **하나만** 읽고 되씀 | `core/src/runtime/core/ctx_physical_queue_registry.cpp:958-1127` |
| "확장 가능" 판정이 전부 plan 필드에서 나옴(별도 flag 없음) | 〃 `:971-981` |
| detach 무효화가 별도 flag 없이 pending generation guard에 흡수 | `core/src/runtime/core/ctx_auto_hwm_recalc.cpp:135-146` |
| 삭제된 `release_endpoint()` 무효화 | (CCU-2의 `_plan_extendable=false` 제거, 현 `registry.cpp:1268-1306`에 없음) |
| 삭제된 `plan_member`/`plan_maximum_bytes`/`extension_t::added` | 코드에 존재하지 않음(`grep -n '_plan_\|plan_member\|plan_maximum_bytes'` = 0건) |

**detach 무효화를 flag 없이 처리한 근거**: 종료 중인 pipe는 `socket_base_api.cpp:2022`
(`pipe_terminated`)에서 `schedule_auto_hwm_recalculate()`를 먼저 부르고, registry endpoint 은퇴는
그 뒤 `pipe_t::~pipe_t()`/`pipe_transport.cpp:372` → `retire_physical_queue_endpoints()`에서 일어난다.
`schedule`은 `_pending_generation`을 올리므로 증분 경로의 `pending != last_applied` guard가
**방향이 plan에서 빠지기 전에** 이미 fast path를 막는다. 별도 invalidation flag는 중복이었다.

**"이미 plan에 든 방향" 판별도 규칙 하나로 합쳤다**: CCU-2는 `plan_member` 비트로 inproc pair의
두 번째 endpoint를 구분했다. 이제는 `queue_id > max_planned_queue_id`만 본다 — 그보다 큰 ID는
plan이 모르는 새 방향이고, 그 이하는 (member든 아니든) 전체 재계산만이 구분할 수 있으므로 폴백한다
(`registry.cpp:1000-1005`). 대가는 완성된 inproc pair의 두 번째 attach가 항상 전체 재계산이라는 것이다
(=CCU-2 이전 동작과 같음, 회귀 아님). 이 경로는 신규 테스트
`test_completed_inproc_pair_plan_matches_full_recalculation`이 덮는다.

**S-CCU2-1로 가지 않은 이유**: 리뷰가 요구한 "plan 사실 한 벌·폴백 규칙 하나"는 위로 충족된다.
S-CCU2-1의 descriptor 리팩터(2~4 engineer-day)는 D-H1 편차 자체를 없애기 위한 것인데, D-H1은
사용자가 D-B269로 채택하고 스펙 §2 행이 이미 개정되어 있으므로 0.17.4에서 지불할 이유가 없다.
감독관 지시(D-B277)도 "단일 소유로 B-3이 풀리면 그대로"였다.

## 2. B-CCU2-1 — allocation 예외

- `socket_base_t::auto_hwm_extend_plan_for_attached_pipe()`가 `std::vector` 대신 **고정 배열
  `physical_queue_endpoint_policy_t policies[2]`**를 쓴다(`socket_base.cpp:313-334`).
  `physical_queue_endpoint_policy_t` 복사는 `shared_ptr` 참조수 증가뿐이라 할당이 없다.
- `extend_application_plan()`도 `physical_queue_handle_t extensions[extension_capacity]`
  (`registry.cpp:980`, capacity 상수 `registry.cpp:228-230`)만 쓰고 컨테이너를 만들지 않는다.
  policy 개수가 capacity를 넘으면 진입 자체를 거절한다(`:967-968`, `:980`).
- 그래도 호출 경계에 기존 전체 재계산과 **같은 변환**을 두었다: `bad_alloc → ENOMEM`,
  그 외 → `EFAULT`, 두 경우 모두 `false`를 돌려 caller가 전체 재계산으로 폴백한다
  (`ctx_auto_hwm_recalc.cpp:163-180`). 부분 확장 뒤 폴백해도 안전한 이유는 전체 재계산이
  registry에서 모든 aggregate를 새로 만들기 때문이며, 그 근거를 주석에 적었다.

즉 attach 경로에서 예외가 나올 원인 자체를 없앴고, backstop까지 두어 worker/public 호출 경계 밖으로
예외가 새지 않는다.

## 3. B-CCU2-2 — 공표 순서

`ctx_t::auto_hwm_extend_plan_for_attach()`(`ctx_auto_hwm_recalc.cpp:119-186`)가

1. `_auto_hwm_recalc_sync`를 **함수 전체**에 걸어 잡고(`:130`),
2. registry 확장(`:174-176`),
3. **`socket_->apply_extended_auto_hwm_plan(plan, pipe_)`로 pipe `_hwm`을 먼저 적용**(`:182`,
   구현 `socket_base.cpp:336-374`, `pipe_->apply_physical_queue_hwm_plan()`이 첫 줄),
4. 그 뒤에야 `_auto_hwm.record_applied_plan(plan, generation)`(`:184-185`)

순으로 진행한다. 전체 pass(`:230-260`: 모든 socket에 apply → record)와 같은 순서다.
mutex를 apply와 record에 함께 걸었으므로 동시 attach가 서로의 apply와 generation 사이에
끼어들 수 없다 — attach B의 generation이 기록되기 전에 attach A의 apply는 이미 끝나 있다.
따라서 §3의 "한 `budget_generation`에 속한 일관된 view"와 §4의 admission 목표 적용을 깨지 않는다.
신규 테스트 `test_concurrent_attach_keeps_generation_monotonic`이 이 직렬화를 공개 표면에서 확인한다.

lock 순서: `_auto_hwm_recalc_sync → pipe _out_sync`(3단계)는 전체 pass의
`_auto_hwm_recalc_sync → socket _auto_hwm_sync → monitor sync → pipe _out_sync`의 부분집합이라
새 역순 중첩이 없다. TSan 9/9 report 0으로 확인했다(§6).

## 4. W-CCU2-2 — 주석 정정

- 헤더 선언 주석을 "포화일 때만 성공"에서 실제 동작으로 고쳤다: 자동 방향이 한 role을 공유하면
  water-filling이 하나의 level을 주므로 그 level의 closed form으로 **비포화에서도** 성공하고,
  이미 붙은 방향의 목표 인하는 debounce가 공표한다(D-H1, §2 연결 증가 행 참조).
  `ctx_physical_queue_registry.hpp:136-152`.
- 구현 주석도 같은 내용으로 맞췄다: `registry.cpp:962-966`(단일 소유), `:1043-1051`(water-filling
  closed form과 remainder), `:1084-1088`(무엇을 미루는지와 그 근거 절).

## 5. W-CCU2-1 — 신규 테스트

`core/tests/unittest/unittest_auto_hwm_incremental_plan.cpp` (신규, CMakeLists 등록 `:17`).
공개 표면만 쓴다(`zlink_connect`/`zlink_bind`, `zlink_ctx_auto_hwm_recalculate`,
`zlink_ctx_get_auto_hwm_budget_snapshot`). 핵심은 `assert_plan_equal()`:
**한 번의 attach가 남긴 snapshot과, 같은 상태에서 강제 전체 재계산이 남긴 snapshot을 필드 단위로 비교**한다
(budget, total_planned, manual_reserved, 방향/송신/수신 수, unlimited manual 수, flags).
`total_applied`는 D-H1이 허용하는 지연 필드이므로 `증분 ≥ 전체`만 요구하고, 전체 재계산 뒤에는
`planned == applied`가 성립함을 함께 검증한다 — 즉 "planned 합계 = 실제 목표 합"이 debounce/강제
재계산 시점에 회복된다는 것을 테스트가 고정한다.

| 케이스 | 무엇을 덮나 |
|---|---|
| `..._unsaturated` | 비포화 water level. 홀수 budget(1 MiB+3)으로 remainder가 0이 아닌 구간, 자동 방향 2·4·6·8·10 |
| `..._across_saturation` | 포화(role 최대) → 비포화 전환을 자동 방향 2→12에서 넘어감 |
| `..._with_finite_manual` | 유한 manual 예약이 level 분모 밖·budget 안이라는 계산 |
| `test_unlimited_manual_falls_back_to_full_recalculation` | unlimited manual → aggregate invalid → 증분 거절, 폴백 결과가 정확 |
| `test_mixed_roles_fall_back_to_full_recalculation` | ROUTER(routed)+DEALER(peer_queue) role 혼합 → 거절, 폴백 결과가 정확 |
| `test_completed_inproc_pair_plan_matches_full_recalculation` | 완성된 inproc pair의 두 번째 endpoint(= `queue_id <= max_planned_queue_id`) 폴백 |
| `test_concurrent_attach_keeps_generation_monotonic` | 두 스레드 동시 attach 8회 — generation 단조 증가·누락 없음, 최종 plan이 전체 재계산과 일치 |

**미커버 1건 — queue ID wrap**: `allocate_queue_id_unlocked()`가 `UINT64_MAX`에서 1로 되감는 경우다.
공개 API로 2^64개의 queue ID를 소모할 방법이 없고(방향 하나당 ID 1개), 내부 훅을 새로 뚫는 것은
"새 규칙·상태 추가 금지"에 걸린다. 코드 경로 자체는 `registry.cpp:1000-1005`의
`queue_id <= max_planned_queue_id → 폴백` 한 줄이며, wrap이 나면 새 ID가 작아져 이 분기로 들어가
**전체 재계산으로 안전하게 떨어진다**(정확성 손실 없음, 성능만 원래대로). 리뷰의 지적도 폴백 존재
확인이었고 이미 "해소"로 판정돼 있다.

## 6. 검증 결과

| 검증 | 명령 | 결과 |
|---|---|---|
| dev 빌드 | `JOBS=4 scripts/build-core.sh dev` (ninja 0·available 10.6 GB 확인 후) | OK |
| 신규 unittest | `ctest -R unittest_auto_hwm_incremental_plan` | **7/7 PASS** |
| 대상 suite + 신규 | `ctest -R 'auto_hwm|hwm|attach|stream|ctx' --repeat until-fail:5` | **32/32 × 5회 PASS** |
| 전체 | `ctest -E hotpath_gate` | **211/211 PASS** |
| TSan | 기존 `core/build-tsan`(GCC 13.3, RelWithDebInfo, LTO off, `-fsanitize=thread`) 재빌드 후 `setarch x86_64 -R ctest -j1`, 억제 없음. 리뷰어 8개 suite + 신규 unittest | **9/9 PASS, `WARNING: ThreadSanitizer` 0건** |
| Release lib | `JOBS=4 scripts/build-core.sh release --lib-only` | OK |
| with_stream CCU 4000 | `--stack zlink,asio --size 64 --ccu 4000 --duration 3 --runs 1`, `flock PERF_LOCK`, ninja 0 | **zlink 212.7 kops PASS** (asio 323.0). 시작 load1 4.71 — 다른 트랙이 동시에 돌던 창이라 idle 기준(1.5)은 못 맞췄다. 판정용이 아니라 PASS 유지 확인용이고 두 스택을 같은 창에서 쟀다 |
| hotpath 5셀 | 감독관 지시로 생략 | — |

참고: CCU-2에서 잰 같은 셀은 202.8 kops였고 이번은 212.7 kops다. 두 측정 모두 부하가 있는 창이라
차이를 개선으로 읽지 않는다.

## 7. 변경 분류

- 성능 결함 제거: **B(기존 결함)**.
- 기존 방향의 목표 인하 지연: **D-H1**(사용자 채택, 스펙 §2 개정 완료 — 이 patch에는 스펙 변경이 없다).
- CCU-3의 세 차단 해소는 전부 **CCU-2 자체의 회귀·범위 위반 수정**이며 새 계약 편차를 만들지 않는다.
- 새 옵션·플래그·타이머: 0. 새 규칙: CCU-2 대비 **감소**(일치 guard·invalidation flag·member 비트 3종 삭제,
  "ID가 plan의 최대보다 큰가" 한 규칙으로 대체).

## 8. 남은 것

- hotpath 5셀(감독관이 병합 게이트에서 수행).
- `--ccu 1000 --size all --runs 3` before/after 표(측정 창 경쟁으로 미실행, CCU-2 보고서 §7과 동일 항목).
- queue ID wrap의 실행 커버리지(§5 사유).
