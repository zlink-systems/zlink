# Core task4m 병합·재검증

상태: 병합·재검증 완료. 모든 요청 기능 게이트 green, 남은 기능 실패 없음.

## 병합 결과

- 기준 HEAD: `e378deecd0`, detached 유지. `git apply --3way`로 B를 적용하고 선언/reconnect 충돌을 수동 해결했다.
- `core/src/runtime/sockets/common/socket_base_api.cpp:1729`: B의 DISCONNECTED identity 정규화는 A와 같아 중복 변경을 남기지 않았다. A의 connect/bind pipe half 공유 ID와 TCP/IPC attempt ID는 유지했다.
- `core/src/runtime/sockets/common/socket_base_api.cpp:1743,1918`, `socket_base_lifecycle.cpp:1372`: B의 pipe termination 안에서 직접 `connect_internal(..., false)` 호출은 버리고 A의 `reconnect_inproc` self-command를 유지했다. command owner 안의 reconnect 재진입을 피하고 기존 reconnect 옵션 조건도 보존한다.
- `core/src/runtime/sockets/common/socket_endpoint_runtime.cpp:116,125`: B의 endpoint 반환형 `erase_pipe`는 버렸다. A의 `endpoint_for_pipe` 조회와 기존 삭제 API로 connect intent를 판정한다. 명시 disconnect의 map 제거는 유지해 자동 reconnect와 구분한다.
- `core/src/runtime/sockets/common/socket_base_endpoint.cpp:426,951`: B의 1-lane attach/explicit disconnect 임시 peer executor 설치·quiesce를 채택했다. 영구 owner를 추가하지 않고 기존 executor 수명 관리를 사용한다.
- `core/src/runtime/sockets/common/socket_base_endpoint.cpp:1042`: B의 bound inproc endpoint matching pipe no-delay 종료 및 lifetime pin 아래 양방향 ack drain을 채택했다. B의 최대 20×10 ms drain 범위를 늘리지 않았다.
- `core/tests/integration/test_socket_disconnect_progress_without_app_poll.cpp:601`: 제공된 공개 C API 테스트 12 case를 그대로 추가하고 CMake에 등록했다.

소유 계층: Core inproc endpoint/pipe lifecycle 및 command owner.
Spec 조항: `core/doc/spec/core/05-polling.ko.md:72-81`; `core/doc/spec/core/socket/README.ko.md:825-827,851-861`; `core/doc/spec/core/06-monitoring.ko.md:69-77,517-533`.
교차언어 대조: Framework runtime 변경 없음. 모든 언어가 사용하는 공통 Core C API 경로를 수정하며 binding/Framework 코드는 수정하지 않았다.
변경 분류: B — 기존 Core 결함 수정 병합. 아래 기존 계약 공백은 별도 판단 대상으로 보존한다.

## 변경 파일

- core/src/runtime/sockets/common/socket_base.hpp
- core/src/runtime/sockets/common/socket_base_endpoint.cpp
- core/src/runtime/sockets/common/socket_endpoint_runtime.cpp
- core/src/runtime/sockets/common/socket_runtime.hpp
- core/tests/CMakeLists.txt
- core/tests/integration/test_socket_disconnect_progress_without_app_poll.cpp

## 게이트

`core/build-task4m`: RelWithDebInfo, ENABLE_LTO=OFF, ZLINK_BUILD_TESTS=ON, BUILD_TESTS=ON, build -j3. Configure/build exit 0.

| 검증 | 최종 결과 |
|---|---|
| test_socket_disconnect_progress_without_app_poll | 5/5 green (각 12 Unity cases) |
| test_monitor_connection_identity | 5/5 green (각 11 Unity cases) |
| 신규 두 테스트 반복 합계 | 10/10 실행 green, 55.25 s |
| test_ctx_term_fixed_rid_handover | green, 0.79 s |
| test_router_reciprocal_handover_lanes | green, 7.03 s |
| test_zmp_request_reply_receive_transaction | green, 1.89 s |
| test_flow_state_paired | green, 2.84 s |
| test_completion_pipe_budget_is_fair_and_stale_requeue_is_fenced | 포함 suite test_phase3_request_reply_contract green, 12.50 s |
| 지정 회귀 suite 합계 | 5/5 green, 25.06 s |
| 전체 ctest -j2 -E hotpath_gate --output-on-failure | 145/145 green, 0 failures, 205.05 s |
| git diff --check / --cached --check / HEAD --check | green |
| hotpath_gate | 실행 제외, 감독자 Release+LTO 별도 판정 |

전체 gate label: integration 95, unittest 28, regression 25 (label은 중복될 수 있음).
변경은 허용된 6개 파일에만 있다. 기존 core/build·core/build-dev symlink는 사용하지 않았다. detached HEAD를 유지했고 commit/push/reset/checkout/stash를 실행하지 않았다. 3-way 적용 및 충돌 해결 결과는 index에 staged 상태로 남아 있다.

로그 위치: `/home/hep7hep7/project/zlink-work/c016/`의 `core-task4m-configure.log`, `core-task4m-build.log`, `core-task4m-new-tests.log`, `core-task4m-targeted.log`, `core-task4m-full.log`, `core-task4m-progress.md`.
전체 Unity 출력: `core/build-task4m/Testing/Temporary/LastTest.log`.
남은 기능 실패: 없음.

## BLOCKERS

작업 4 요약의 다음 두 항목을 그대로 인용한다.

1. **spec gap — 사용자 결정 필요:** terminal edge를 소비하기 전 같은 socket에서 즉시 `disconnect(endpoint) -> connect(endpoint)` 했을 때, 새 connect가 이전 physical terminal을 반드시 기다리는지 또는 old/new overlap을 허용하는지 명시가 없다. 선택지: (A) disconnect 반환 전에 logical/physical terminal을 직렬화해 다음 connect를 항상 새 pipe로 admit, (B) overlap을 허용하고 default REJECT/HANDOVER 정책에 따라 request 결과가 달라질 수 있음을 계약화. 현재 Core는 transport timing에 따른 overlap을 허용하며 HANDOVER만 결정적으로 새 reply를 완료한다.
2. **공개 관찰 한계:** request/reply가 어느 physical `connection_id`를 사용했는지 직접 반환하는 공개 C API가 없다(`connection_id`는 send target으로 사용 금지, reply token은 opaque). 직접 귀속이 필수라면 monitor/request completion 계약 확장 여부를 별도 결정해야 한다. 현재 테스트는 old terminal 이후 유일한 live replacement로만 추론한다.

`hotpath_gate`는 요청에 따라 제외하며 감독자가 Release+LTO에서 별도 판정한다.
