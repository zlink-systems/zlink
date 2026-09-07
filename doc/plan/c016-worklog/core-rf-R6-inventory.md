# Phase 3 인벤토리 R6 — session_base / ctx_physical_queue_registry / auto_hwm / transport_pair_policy / flow_state_frame

입력: `core/src/runtime/core/{session_base.cpp,session_base.hpp}`(main HEAD 3586f0eb17), `session_base_pipe_io.cpp`(r3 workt리 버전, trace 제거 후), `ctx_physical_queue_registry.{cpp,hpp}`, `transport_pair_policy.hpp`, `flow_state_frame.hpp`, `auto_hwm_policy.{cpp,hpp}`, `ctx_auto_hwm_recalc.cpp`, `ctx_auto_hwm_state.{cpp,hpp}`. 읽기 전용, 빌드 안 함.

| # | 분류 | file:line | 관찰 | 제안 | 반경 | 계약 | 성능 |
|---|---|---|---|---|---|---|---|
| 1 | 1(dead) | `core/src/runtime/core/ctx_physical_queue_registry.cpp:669-735` (`commit_decoder_frame`) | 로컬 `bool oversize = false;`가 어떤 경로에서도 `true`로 대입되지 않음 → `*oversize_admission_out_`은 항상 `false`. 유일한 호출자 `pipe.cpp:1479` `oversize = oversize \|\| registry_oversize;`는 상수 폴딩됨(= 원래 `oversize`와 동일). application 레인은 주석대로 "reservation carries metadata only" 라서 의도된 것으로 보이나, completion 레인 분기(703-719행)에서도 `oversize`를 세팅하는 코드가 전혀 없어 이 출력 파라미터 자체가 사문화된 것으로 보임 | 확인 필요 — commit_decoder_frame의 oversize_admission_out_ 존재 이유(과거엔 세팅했었는지 git blame 확인) 후, 죽은 경로면 파라미터 제거 또는 `_oversize_message_admission_count` 집계에 실제로 연결 | 파일 2 (ctx_physical_queue_registry.cpp, pipe.cpp) | D(있음) — 05-connection-memory/06-auto-hwm이 소유한 oversize admission 카운트에 영향 가능 | 없음(현재도 상수이므로 제거해도 无변화 추정, 그러나 실측 확인 전엔 위험으로 표기) |
| 2 | 1(dead output field) | `core/src/runtime/core/auto_hwm_policy.hpp:78` `auto_hwm_socket_plan_t::policy_class` / `auto_hwm_policy.cpp:333` 대입 | `auto_hwm_socket_plan_prepare()`가 매 소켓 플랜마다 `auto_hwm_policy_class_for_role()`을 호출해 `policy_class`를 채우지만, repo 전체에서 이 필드를 읽는 코드가 0건(`grep -rn "policy_class"` → 정의/대입만 존재) | `auto_hwm_policy_class_for_role` 호출·필드 대입 제거, 또는 실제 소비처(예: 진단/스냅샷 노출)가 계획되어 있었다면 연결 | 파일 2 (auto_hwm_policy.hpp/.cpp) | 없음 (외부 관측 지점 없음 — public snapshot 구조체에 노출 안 됨) | 없음(연산 자체가 미미하지만 auto_hwm 재계산 경로마다 실행되는 죽은 계산 제거는 이득) |
| 3 | 3(얕은 모듈/반복 판정) | `core/src/runtime/core/ctx_physical_queue_registry.cpp` — `known == _directions.end () \|\| known->second.get () != direction_.get ()` 패턴이 7곳(`bind_application_pipe_queue`, `unbind_application_pipe_endpoint`, `sample_application_pipe_queue`, `record_endpoint_policy`, `current_accounted_bytes`, `refresh_application_hwm_if_drained`, `plan_application_queues`)에서 반복 | `_sync` 보유 상태에서 direction 핸들 유효성을 확인하는 `physical_queue_record_t *find_locked(direction_)` helper로 통합 | 파일 1 (내부 refactor만, 시그니처 불변) | 없음 | 없음(락 보유 중 동일 비교, no-op 대체) |
| 4 | 3(긴 함수) | `core/src/runtime/core/ctx_physical_queue_registry.cpp:748-904` (`plan_application_queues`, ~157행) 및 `auto_hwm_policy.cpp:343-529` (`auto_hwm_context_finalize`, ~187행) | 250행 임계는 아직 안 넘었으나 각각 입력 수집→역할 병합→예산 분배 3단계가 한 함수에 섞여 있음. 이번 라운드에선 그대로 두되 다음 확장(새 role 추가 등) 시 분리 후보로 표기 | 확인 필요 — 지금은 분리 이득보다 diff 반경 위험이 큼. 별도 apply-job 없이 관찰만 기록 | — | — | — |
| 5 | 4(소유권/이름) | `core/src/runtime/core/ctx_physical_queue_registry.hpp:159` `mutable recursive_mutex_t _sync;` | 이름은 "recursive"지만, 실제 코드 경로 확인 결과 `snapshot()`이 `_sync`를 잡은 채로 `sample_application_pipe_queue()`(자체적으로 `_sync`를 다시 잡음, 468-507행)를 호출하지 않도록 명시적으로 분리되어 있음(1136행 주석 "Do not hold the registry mutex while sampling a pipe"). `reset_metrics()`도 `snapshot()` 완료 후 별도로 `_sync`를 잡음. 즉 현재 유일한 재진입 경로가 안 보임 — recursive가 실제로 필요한지 미검증 | 확인 필요 — 모든 `scoped_lock_t (_sync)` 호출부가 서로 중첩되는 경로(콜스택)가 있는지 caller 그래프로 검증 후 plain mutex로 강등 가능한지 판단. fast_mutex/recursive 오분류 전례가 있어 신중히 | 파일 1(mutex 타입만) — 그러나 검증 범위는 registry를 호출하는 pipe.cpp 전체 | D(있음) — 락 semantics 변경은 교착 위험, 반드시 실측/스트레스 테스트 필요 | 위험(recursive_mutex_t가 plain mutex_t보다 무거우면 이득, 아니면 없음 — 실측 필요) |
| 6 | 5(이름-개념 불일치, 경미) | `core/src/runtime/core/ctx_physical_queue_registry.cpp:598-667` `reserve_decoder_frame` 내부의 `final_oversize` / `commit_decoder_frame`의 안 쓰이는 `oversize` — 이름은 "oversize 여부"를 나타내지만 #1과 결합해서 보면 reserve 단계의 판정(650행)과 commit 단계의 판정(pipe.cpp:3797)이 서로 다른 기준(HWM 초과 vs in-flight=0)으로 "oversize"란 이름을 재사용 | 확인 필요 — 두 "oversize" 개념이 실제로 같은 것을 뜻하는지 05-connection-memory 스펙과 대조 | 조사만, 코드 변경 없음 | D — 이름 통일이 문서·계약 재정의를 수반할 수 있음 | 없음 |
| 7 | 1(잔재 주석) | `core/src/runtime/core/session_base.cpp:399` `// raw_socket has been removed` | 제거된 개념(raw_socket)을 가리키는 주석만 남아 있고 그 자리엔 다음 로직(pending 처리)만 있음 — 정보 없는 잔재 주석 | 주석 삭제(코드 변경 없음, 텍스트만) | 파일 1, 1행 | 없음 | 없음 |
| 8 | 확인 필요 | `core/src/runtime/core/session_base.hpp:52-57` `push_msg`/`pull_msg`가 `virtual`인데 `session_base_t`에 오버라이드가 하나도 없음(`grep -rn "::push_msg\|::pull_msg" core/src`로 재확인 필요 — 이번 세션에선 서브클래스 존재 여부까지 확인 못함) | 오버라이드 관계 확인 후 virtual 제거 여부 결정 | 미상 | 미상 | 미상 |

## 확인했으나 문제 없음(오탐 방지용 기록)
- `session_base_t::hiccuped()`가 `zlink_assert (false)`인 것은 dead code가 아님 — `pipe_t::process_hiccup()`은 피어(소켓측) 파이프의 `_sink`(=socket_base_t)에서만 notify가 발생하고, 세션이 자신의 `_pipe->hiccup()`을 호출해도 그 알림은 피어 쪽 sink로 감. 정상 동작.
- `transport_pair_policy.hpp`의 `request_correlation_*`, `completion_socket_buffer`는 `pipe.cpp`/`socket_base_endpoint.cpp`/유닛테스트에서 모두 살아있는 참조 확인.
- `flow_state_frame.hpp`는 pipe.cpp, socket_base_flow_state.cpp 등에서 활성 사용.
- `ctx_auto_hwm_state.hpp`의 `recalc_task_id`/`clear_recalc_task_id`/`_budget_generation`/`_measurement_epoch`는 모두 `ctx_auto_hwm_recalc.cpp`에서 소비됨.
- S-A/S-B가 지적한 `account_inbound_frame` 문제(RMW-per-message)는 `pipe.cpp`에 위치 — 이번 R6 파일 목록에는 없어 범위 밖으로 기록만 남김(다른 라운드에 인계).

## 항목 요약
- 총 8항목: 분류1(dead) 3, 분류3(얕은모듈/중복) 2, 분류4(소유권) 1, 분류5(이름불일치) 1, 확인필요(오버라이드) 1.
- 확신도: 항목 3, 7만 즉시 적용 가능(로우 리스크). 나머지는 "확인 필요" 또는 D(계약영향) 표기.

## 적용 job 묶음 제안
- **묶음 A (안전, ~20분)**: #7 (주석 삭제) + #3 (registry 내부 `find_locked` helper로 7곳 통합, 시그니처·락 순서 불변) — 파일: `ctx_physical_queue_registry.cpp`, `session_base.cpp`. 순서: #7 → #3.
- **묶음 B (조사 후 결정, ~1h)**: #1 (commit_decoder_frame oversize 출력 사문화 여부 git blame + 05-connection-memory 대조) → 확정되면 파라미터 제거. 파일: `ctx_physical_queue_registry.{cpp,hpp}`, `pipe.cpp`(범위 밖 파일이므로 별도 조율 필요).
- **묶음 C (조사만, 코드변경 없음, 별도 job으로 격리)**: #5 (`_sync` recursive_mutex_t 필요성 검증 — caller 콜스택 전수 확인) + #8 (push_msg/pull_msg virtual 오버라이드 확인) + #2 (`policy_class` 죽은 필드 제거, auto_hwm_policy.{cpp,hpp} 로컬) — #2만 즉시 적용 가능, #5/#8은 확인 결과에 따라 별도 apply-job으로 분리.

보고서 경로: `doc/plan/c016-worklog/core-rf-R6-inventory.md`
