# core-rf R6R8-A 적용 결과

worktree: `~/project/zlink-work/r6r8` (detached from main 482d7bca80). 커밋하지 않음.

## 결과 (수치)
- dev 빌드(`JOBS=4 scripts/build-core.sh dev`): 성공, `hotpath_bench` 포함.
- `ctest --test-dir core/build-dev -R 'registry|hwm|auto|dist|router|pubsub|dealer|physical|queue'` 5회: 매회 38/38 통과, 0 실패.
- hotpath_gate (release-gate 트리, `core/build-gate`, `hotpath_bench`만 빌드, `flock PERF_LOCK` 하에 valgrind 1회, 측정 시작 load average 5.44/6.92/6.61):
  - `dealer_dealer_inproc`: 3423.533(기준) → 3381.305(측정), ratio 0.9877, PASS
  - `router_router_tcp`: 2972.532(기준) → 2981.840(측정), ratio 1.0031, PASS

## 변경 파일
- `core/src/runtime/core/session_base.cpp` — R6 #7: `// raw_socket has been removed` 잔재 주석 삭제(코드 변경 없음).
- `core/src/runtime/core/ctx_physical_queue_registry.hpp` — R6 #3: `private` 헬퍼 `physical_queue_record_t *find_locked (const physical_queue_handle_t &) const` 선언 추가.
- `core/src/runtime/core/ctx_physical_queue_registry.cpp` — R6 #3: 7곳(`bind_application_pipe_queue`, `unbind_application_pipe_endpoint`, `sample_application_pipe_queue`, `plan_application_queues`, `record_endpoint_policy`, `refresh_application_hwm_if_drained`, `current_accounted_bytes`)의 `known == _directions.end () || known->second.get () != X.get ()` 반복 검사를 `find_locked (X)` 호출로 교체. 각 호출부는 기존과 동일하게 `_sync` 보유 중에만 호출되며, 락 순서·추가 조건(lane/endpoint_refs 등)은 그대로 둠.
- `core/src/runtime/sockets/internal/dist.cpp` — R8 bundle A: `dist_t::has_pipe`를 수작업 index 비교에서 `_pipes.contains (pipe_)` 호출로 교체(`lb_t::pipe_terminated`와 동일 관용구로 통일).
- `core/src/runtime/sockets/router/router_send_path.cpp` — R8 bundle B: `xsend_routed`의 6개 `router_debug::enabled ()` fprintf 블록을 익명 네임스페이스의 이름 붙은 헬퍼(`trace_xsend_routed_enter/draining/pipe_not_writable/no_out_pipe/selected/write_failed`)로 추출. 각 호출부는 원래 위치·순서 그대로 한 줄 호출로 대체. 문자열·인자·조건(`router_debug::enabled ()` 게이팅) 100% 동일.

## 설계 비교와 선택 이유
- R6 #3: (a) 헬퍼 함수로 통합 vs (b) 매크로. 매크로는 디버깅 시 원본 위치를 가려 posddd 원칙(규칙 수 줄이기, 가독성)에 반해 헬퍼 함수를 선택. 반환값을 `physical_queue_record_t *`로 해 호출부가 추가 조건과 자연스럽게 `&&`/`||`로 결합 가능하게 함(락 보유 전제·시그니처는 브리프 지시대로 불변).
- R8 bundle A: `dist_t::has_pipe`는 `array_t::contains()`와 완전히 동일한 의미(직접 확인: `array.hpp:74` 구현이 `index >= 0 && index < size && items[index] == item` — 원본 `has_pipe`의 "claimed_index 범위 밖이면 false, 아니면 포인터 비교"와 동치)이므로 재구현 제거, 로컬 재구현 유지 방향은 기각(중복 방지 원칙).
- R8 bundle B: 순수 추출만 수행(디버그 fprintf 블록 6곳 → 이름 붙은 헬퍼). 대안으로 "쓰기 실행부(468-584행) 전체를 별도 함수로 뽑기"도 검토했으나 이는 hot-path 로직 이동을 수반해 브리프의 "pure extraction" 범위를 벗어나므로 채택하지 않음(아래 "멈춘 지점" 참고).

## R6 #2 (policy_class) — 적용 안 함, 인벤토리 정정
브리프 조건("확인 필요")에 따라 repo 전체 재검색: `core/src/runtime/sockets/common/socket_base_monitor.cpp:117`에서 `_auto_hwm_socket_plan.policy_class`를 읽어 공개 모니터 스냅샷 필드 `out_->auto_hwm_policy_class`(`core/include/zlink/eventing/api.h:103`)로 노출하고 있음. 이 필드는 python/go/rust/node/cpp 바인딩과 `bindings/cpp/tests/contract/test_cpp_contract_monitor.cpp`가 소비. R6 인벤토리의 "repo 전체 0건" 판정은 오탐 — **죽은 필드가 아니며 공개 계약(`core/include/**`) 영향이 있어 이번 job 범위 밖**. 코드 변경하지 않음.

## 실행한 테스트와 남은 실패
- ctest 패턴 5회 전부 38/38 통과, 실패 없음.
- hotpath_gate 2셀 PASS(위 수치).
- TSan 별도 실행 안 함(pipe/engine/mailbox/mutex 미변경, registry는 순수 내부 리팩터 — 락 순서·타입 불변이라 대상 아님).

## 스펙 재확인
- ctx_physical_queue_registry: 07-core-source-layout / 계약 동작(락 순서, application/completion 레인 판정) 변경 없음 — `find_locked`는 기존 2줄 비교의 문자 그대로의 대체이며 반환 시맨틱 동일. 어느 문장도 다른 동작이 되지 않음.
- dist.cpp: `has_pipe` 반환값 동치 확인(위 설계 비교). 어느 문장도 다른 동작이 되지 않음.
- router_send_path.cpp: 추출된 각 헬퍼는 원래 `if (router_debug::enabled ()) { fprintf(...); }` 블록과 문자열·인자·게이팅 조건이 동일. 함수 반환/제어 흐름 변경 없음. 어느 문장도 다른 동작이 되지 않음.

## 변경 분류
B(기존 결함 정리/중복 제거, 계약 영향 없음) — 4건 모두(R6 #7, R6 #3, R8 bundle A, R8 bundle B).

## 멈춘 지점
- R6 #2는 적용하지 않음(위 "인벤토리 정정" 참고, 공개 계약 영향 D로 재분류 필요 — 인벤토리 문서 수정 권장).
- R8 bundle B: 디버그 블록만의 순수 추출로는 `xsend_routed`가 355행 → 약 331행으로 줄었으나 250행 미만 목표에는 못 미침. 나머지 초과분을 줄이려면 쓰기 실행/에러 처리 로직(468-584행)까지 별도 함수로 옮겨야 하는데 이는 "pure extraction(디버그 블록만)" 범위를 벗어나는 추가 설계 판단(별도 job/리뷰 필요)이라 이번 job에서는 진행하지 않음.
