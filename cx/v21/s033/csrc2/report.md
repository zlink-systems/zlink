요청한 두 수정을 적용했다. Focused 검사는 8개 중 8개 통과했고, 전체 unit/contract는 306개 중 306개 통과, 0개 실패, 0개 skip이다. 같은 rate에서 source user 중앙값은 main 5.361, 변경판 4.204 µs/message다.

기존 csrc/change.diff와 비교해 이번에 바꾼 파일은 app.cpp와 test_cpp_framework_raw_route_port_contract.cpp 두 개다. 기존 나머지 8개 파일의 diff는 그대로다. 범위 검사는 raw/second-pass-scope.json에 있다. 전체 framework/의 측정 main snapshot ce1b779323 대비 diff는 change.diff에 저장했다. 이 머신의 local main ref는 그 snapshot보다 오래되어 작업 범위 밖 변경도 포함하므로 비교 기준으로 쓰지 않았다. 최초 local main ref와의 비교 출력은 raw/local-main-ref.diff에 보존했다. 시작 branch는 git status --short --branch로 확인한 perf/3868-cpp-source-path다. Commit, push, GitHub 작업, Core·binding·spec 수정과 sub-agent 실행은 하지 않았다. 이전 brief는 읽지 않았다.

framework/languages/cpp/framework/src/runtime/host/app.cpp:2642에서 routing_id_async()를 한 번 호출한다. :2643에서 준비 여부를 확인하고, 준비됐으면 :2646에서 값을 읽어 동기 submit()으로 전달한다. 이 경로의 run()과 submit()은 coroutine이 아니다. 미완료이면 :2644에서 mesh, target, parts, identity task를 resume()의 값 인자로 넘긴다. :2630에서 identity를 기다리고 :2631에서 같은 submit()을 호출한 뒤 결과를 기다린다. Coroutine frame과 terminal projection closure가 기존 mesh 수명을 유지하며, mesh_node_service 포인터의 소유 관계는 기존 host service와 같다. 새 flag, counter, lock은 없다.

같은 파일 :2610에서 local/remote를 한 번 판정한다. Local은 :2611의 submit_local_node_send(), remote는 :2613의 send_to_node()에 payload를 한 번 이전한다. :2614의 project_task_terminal()과 :2619의 one_way_native_submit_result()가 두 경로의 결과를 한 번 변환한다. Identity 조회와 제출 결정을 각 분기에 복제하는 대안 대신 이 공통 submit()을 선택했다.

| 경로 | identity 조회·소비 | 제출·완료 인계 |
|---|---|---|
| Ready | app.cpp:2642 → mesh_node_runtime.cpp:4734–4737의 immutable startup descriptor → app.cpp:2647 | app.cpp:2646 → :2610–2613 → :2614–2619 |
| Pending | app.cpp:2642 → mesh_node_runtime.cpp:4740–4741의 lane.run_task → app.cpp:2644–2645에서 frame에 이전 → :2630에서 await | app.cpp:2631–2632 → :2610–2613 → :2614–2619 → :2631의 co_await |

표의 파일은 framework/languages/cpp/framework/src/runtime/host/와 mesh/ 아래다. 이번 app transport의 identity 조회, payload 인계와 결과 변환은 두 경로 중 선택된 한 경로에서 각각 한 번만 수행되며 중복되지 않는다. 기존 local service는 mesh_node_host_service.cpp:3909에서 송신 source RID를 별도로 얻는다. 이것은 local/remote 재판정이 아니라 기존 local dispatch의 source identity이며 이번 수정으로 추가한 조회가 아니다. Remote send_to_node()는 mesh_node_runtime.cpp:4281–4295의 기존 transport 제출을 사용한다.

요청한 동일 mesh state lane 안의 미완료 send 성공 테스트는 현재 코드에서 성립하지 않아 추가하지 않았다. routing_id_async()도 mesh_node_runtime.cpp:4741에서 state_lane_t::run_task()를 호출하며, state_lane.hpp:377의 throw_if_reentrant()는 state_lane.cpp:57–63에서 같은 lane 재진입을 logic_error로 거부한다. Main도 이 검사를 거친다. 따라서 run_task가 재진입 시 절대 throw하지 않는다는 전제는 현재 코드와 다르다. Lane 밖의 미완료 조회는 contention이 있을 때 가능하므로 이번 수정은 그 경우 caller를 run_sync()로 막지 않고 await한다. 그러나 _node 없는 정상 실행 상태에서 성공하는 send도 없다. Remote는 mesh_node_runtime.cpp:4287–4288에서 시작 전 오류로 끝나고 local은 mesh_node_host_service.cpp:3905의 시작된 service 조건을 요구한다. _node는 시작 중 mesh_node_runtime.cpp:932에서 설정되고 종료 때 :1968에서 해제된다. 이를 우회하는 test hook이나 lifecycle 조작은 추가하지 않았다. 미완료 app transport 분기는 실행 테스트로 직접 검증하지 못한 한계가 있다. 근거 원문은 raw/lifetime-lane-evidence.txt에 있다.

framework/languages/cpp/tests/Zlink.Framework.ContractTests/test_cpp_framework_raw_route_port_contract.cpp:1219–1220에서 HWM 16의 multipart 호출만 제거했다. 남긴 HWM 2048 호출 바로 앞에 #4118의 Core/binding backpressure 문제 때문에 작은 HWM case를 실행하지 않는다는 한 줄 주석을 넣었다. 함수 본문과 다른 검사는 유지했다. Core·binding은 수정하지 않았다.

빌드는 기존 csrc/build-change와 csrc/build-tests를 재사용했다. linux-ninja-release의 Release, -O3 -DNDEBUG이며 세 build 디렉터리의 compile_commands.json에서 -g 계열 옵션이 없음을 확인했다. 모든 C++ 빌드는 bare /root/jobs/fx/cx/cpp-build-slot.sh로 실행했다. 빌드 로그는 raw/build-change-j3-1.log와 raw/build-tests-j3-1.log이며 모두 exit 0이다. 설정·바이너리 hash는 raw/build-provenance.json, 메모리 기록은 각 build의 -status.json에 있다.

검사는 /root/jobs/fx/cx/measure-shared.sh 안에서 stress-ng --cpu 8 --cpu-load 80을 유지하며 실행했다. Focused는 envelope allocations, raw route port contract, route payload ownership, send allocations, request wire identity, service wire codec, generated service wire codec, route-client channel routing의 8개다. 새 테스트는 없다. 전체 검사는 ctest -L 'unit|contract'를 한 번 실행했다. Compiler를 실행하는 negative fixture도 build slot을 사용하도록 기존 generated CTest 명령의 wrapper와 log 경로만 바꿨다. Assertion, timeout, label과 compiler 인자는 그대로다. raw/test-compiler-wrapper.json에 변경 명령이 있다.

| 검사 | 결과 | 원시 로그 |
|---|---|---|
| Focused, 부하 상태 | 8/8 통과, 9.0초 | raw/test-focused.log, raw/test-focused.xml, raw/test-focused-status.json |
| 전체 unit/contract, 부하 상태 | 306/306 통과, 0 실패, 0 skip, 236.1초 | raw/test-all.log, raw/test-all.xml, raw/test-all-status.json |
| Format, 기존 변경 파일 10개 | clang-format-18 --dry-run --Werror 통과 | raw/clang-format-check.log, raw/format-status.json |
| Diff 공백 검사 | git diff --check 통과 | raw/diff-check.log |

CTest 내부 로그는 raw/test-focused-ctest-artifacts/와 raw/test-all-ctest-artifacts/에 보존했다. Stress와 메모리 원문은 raw/test-*-stress.log, raw/test-*-memory.json에 있다. Focused 최소 MemAvailable은 24.32 GiB, 전체 검사는 23.70 GiB다. 실패 상세는 raw/test-summary.json에 있다.

알려진 #4119의 test_cpp_framework_location_runtime는 이번 전체 실행에서 통과했다. Main에서도 실패한 이력이 있다는 사용자 지시를 유지하며, 이번 통과를 #4119가 수정됐다는 근거로 사용하지 않는다. 관련 코드와 테스트는 수정하거나 재실행하지 않았다.

같은 rate CPU는 csrc/scripts의 measure.py, runner_common.sh, collect.py, summarize.py를 재사용했다. csrc2/scripts의 변경은 출력·빌드 경로와 실행 대상 축소이며 raw/script-adaptations.diff에 있다. 조건 B로 source CPU 0–7, target CPU 8–15, payload 1 KiB, CSEND_PACE=136889, 64건 단위 sleep_until을 유지했다. 준비 구간은 30초·10 segments, active는 20초, CPU capture는 12초다. Perf는 켜지 않았다. Main→변경판, 변경판→main, main→변경판 순서로 각각 3회 실행했다. Saturation과 binding 재측정은 하지 않았다.

측정에만 flock -o /root/jobs/fx/cx/measure.lock의 배타 lock을 잡았다. 시작 전 load1 ≤ 4, MemAvailable ≥ 12 GiB를 확인했다. 전체 process /proc stat의 utime·stime 차이를 CLK_TCK=100으로 환산했다. Source는 capture completed 차이, target은 received 차이로 나누었다. 식은 Δticks / CLK_TCK × 1,000,000 / Δmessages다. 비용 열별 중앙값을 계산했다. 단위는 µs/message다.

| 구현 | 회차 | msg/s | Source user | Source kernel | Target user | Target kernel |
|---|---:|---:|---:|---:|---:|---:|
| main | 1 | 136,888.6 | 5.361 | 3.850 | 9.777 | 3.891 |
| main | 2 | 136,879.3 | 5.396 | 3.882 | 9.695 | 3.865 |
| main | 3 | 136,888.6 | 5.300 | 3.816 | 9.609 | 3.810 |
| main | 중앙값 | 136,888.6 | 5.361 | 3.850 | 9.695 | 3.865 |
| 변경판 | 1 | 136,888.6 | 4.362 | 3.122 | 8.906 | 2.959 |
| 변경판 | 2 | 136,888.6 | 4.204 | 3.023 | 8.627 | 2.811 |
| 변경판 | 3 | 136,888.6 | 4.180 | 2.948 | 8.469 | 2.845 |
| 변경판 | 중앙값 | 136,888.6 | 4.204 | 3.023 | 8.627 | 2.845 |

이번 변경판 source user 중앙값은 main보다 21.58% 낮다. 기존 csrc 변경판 3.958 대비 차이는 +0.246 µs/message다. 서로 다른 세션의 3회 중앙값이므로 이 차이를 이번 두 수정만의 인과 효과나 통계적 유의성으로 해석하지 않는다. Bench는 _node가 있는 ready identity 경로를 사용한다.

원시 결과는 raw/paced/{main,change}-{1,2,3}/에 있다. 각 실행의 commands.json, results.json, target-stats.json, capture/before.json, capture/after.json, capture/samples.json과 stdout·stderr를 보존했다. 측정 시작 조건과 binary·native package hash는 raw/measure-wait.jsonl과 raw/measure-session.json에 있다. 메모리는 raw/memory-measure.jsonl에 있다. 여섯 실행 모두 source·target 오류, abandoned, rejected, header validation failure, contamination과 drain-bound가 0이고 제출·완료·수신 수가 같다. 요청 rate 오차는 1% 이내다. 집계 결과는 raw/summary.json이며 정상 6개를 수락하고 잘못된 입력 10개를 모두 거부한 결과도 포함한다.

소유 계층은 Framework app transport의 논리 target 선택과 task 결과 전달이다. Physical admission·retry와 payload 보관은 기존 Core/binding 소유다.

Spec 근거는 02-channel-messaging §2·8, 01-submit-and-completion §10·11과 POSDDD의 한 결정·한 소유자 원칙이다. Contract나 spec은 바꾸지 않았다.

교차언어 대조는 기존 csrc/report.md의 결정 소유 표와 .NET ZLinkManagedMeshNode.cs:3413, Node raw-service-mesh-runtime.ts:641의 기존 Node send 제출을 확인했다. Kotlin은 Java runtime을 공유한다. 이번 변경은 C++의 기존 비동기 identity 계약을 복원하며 다른 언어의 사용자 동작을 바꾸지 않는다. 교차언어 suite는 실행하지 않았다.

변경 분류는 B다. 기존 비동기 identity 조회를 동기 대기로 바꾼 결함의 수정과 #4118에 맞춘 테스트 범위 수정이다.

리팩토링 점검은 app transport의 identity 소유, local/remote 판정, 결과 변환 세 항목에서 중복 0개다. 수정 전/후 판정 소유 위치 수는 identity 1→1, target 선택 1→1, 결과 변환 1→1로 합계 3→3이다. 준비 여부 구현 분기는 요청에 따라 0→1이며 별도 runtime 상태는 없다. 고친 항목은 동기 identity 대기 1개이며, 이 범위에서 별도로 넘긴 리팩토링 항목은 0개다. 기존 local service의 source RID 조회는 유지했다.

최종 binary hash는 측정 manifest와 모두 일치하고 main 바이너리는 기존 csrc 측정 당시와도 같다. 여섯 실행의 자식 process는 모두 종료됐다. 측정 중 최소 MemAvailable은 25.46 GiB였고 최종 csrc2 크기는 42.6 MiB로 10 GB 미만이다. 원시 출력은 삭제하지 않았다. 테스트가 만든 Server/.run 자료는 raw/full-suite-engine-server-run/으로 옮겼다. 최종 검사는 raw/final-artifacts.json과 raw/final-git-status.txt에 있다.
