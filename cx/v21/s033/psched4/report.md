HEAD 471d0a8c7502d0d04cb160bece2bfae5c347134b, branch perf/1639-cpp-sched-drain에서 수정했다. Node·Channel의 concurrent application 판정은 host::owner_kind_t로 바꿨다. Spot·Actor는 gated로 판정한다. 빌드·테스트·성능 측정은 실행하지 않았다.

Record의 optional owner_kind를 생산자가 지정하고, owner_queue_t가 처음 받은 kind를 유지하도록 했다. 기존 ready list의 원소는 key와 optional kind를 함께 가진 ready_owner_t로 바꿨다. Callback은 key와 확정된 kind를 받고, make_application_owner_work의 capture와 drain_application_owner 인자로 kind를 전달한다. Queue가 먼저 소비·삭제된 뒤에도 ready 알림의 kind가 보존된다. Callback에서 mailbox를 다시 조회하는 대안은 추가 lock과 queue 삭제 경합을 만들므로 쓰지 않았다. 별도 concurrent bool을 저장하지 않았다.

service_mailbox.cpp:140은 kind 없는 application record를 std::invalid_argument로 거부한다. Prepare·admitted callback과 queue 변경 전에 거부하며 release 빌드에도 적용한다. service_mailbox.cpp:154는 같은 queue에 다른 kind가 들어오면 std::invalid_argument로 거부한다. Infrastructure는 nullopt를 명시한다. 두 record factory의 kind 인자는 필수이며 기본값이 없다. Typed 판정 함수에서 정의되지 않은 enum 값도 std::invalid_argument로 거부한다. 없는 queue를 independent take하면 nullptr를 반환하며 owner 문자열로 종류를 추측하지 않는다.

아래 R은 framework/languages/cpp/framework/src/runtime/, T는 framework/languages/cpp/tests/Zlink.Framework.UnitTests/, U는 framework/languages/cpp/tests/support/다. 변경 전 위치는 HEAD 기준이고 변경 후 위치는 현재 파일 기준이다.

| 결정 또는 사용 지점 | 변경 전 | 변경 후 | 판정 소유자 수 |
|---|---|---|---|
| kind를 concurrent/gated로 분류 | R/mesh/service_mailbox.cpp:232의 owner 문자열 비교 | R/mesh/service_mailbox.cpp:239의 owner_kind_t switch | 1 → 1 |
| enqueue의 ready 재게시 | R/mesh/service_mailbox.cpp:164에서 문자열 함수 호출 | R/mesh/service_mailbox.cpp:171에서 queue.owner_kind 전달 | 자체 분류 0 → 0 |
| independent record claim 허용 | R/mesh/service_mailbox.cpp:247에서 문자열 함수 호출 | R/mesh/service_mailbox.cpp:269에서 queue.owner_kind 전달 | 자체 분류 0 → 0 |
| submit_owner의 executor 경로 선택 | R/mesh/mesh_node_host_service.cpp:3415에서 문자열 함수 호출 | R/mesh/mesh_node_host_service.cpp:3417에서 ready callback의 kind 전달 | 자체 분류 0 → 0 |
| drain의 application gate 사용 | R/mesh/mesh_node_host_service.cpp:3947에서 문자열 함수 호출 | R/mesh/mesh_node_host_service.cpp:3953에서 전달받은 kind 사용 | 자체 분류 0 → 0 |

분류 규칙의 판정 위치는 수정 전 1곳, 수정 후 1곳이다. 사용 지점은 전후 모두 4곳이다. 문자열 판정 함수 1개를 제거하고 typed 판정 함수 1개로 교체했다. application_owner의 key 생성은 그대로 유지했다.

| 운영 코드의 추가·삭제 수 | 추가 | 삭제 | 내역 |
|---|---:|---:|---|
| 선언된 kind 저장 필드 | 3 | 0 | record.owner_kind, owner_queue_t.owner_kind, ready_owner_t.second |
| closure의 kind capture | 1 | 0 | make_application_owner_work의 기존 closure |
| interface의 인자 | 7 | 1 | factory 2개·ready callback·work 생성·drain·route_received에 각 1개 추가, concurrent_application_owner의 string_view 인자 1개를 typed 인자 1개로 교체 |
| concurrent 실행 flag | 0 | 0 | 기존 drain의 지역 concurrent는 함수 결과를 계속 사용 |
| queue·lock·timer | 0 | 0 | 기존 ready list의 원소 타입만 변경 |

인자는 선언과 정의를 중복 계산하지 않은 interface 기준이며 순증은 6개다. 테스트의 기존 enqueue·independent·queued helper에도 kind 인자를 각각 추가했다. 새 테스트의 bool 인자는 기대 결과이며 runtime 실행 상태가 아니다. Kind 저장은 문자열 형식에 의존하던 판정과 교환한 것이며, 별도 실행 권한이나 중복 판정 상태를 추가하지 않았다. 기존 list node와 record·queue·closure의 크기는 커질 수 있으나 새 heap 할당 지점은 추가하지 않았다. ABI별 크기와 성능은 측정하지 않았다.

생산 경로는 다음과 같다. R/mesh/raw_mesh_node_owner.cpp:3639의 route_received가 kind를 record에 보존한다. :4161의 Node, :4182의 Channel, :4199의 Spot, :4221의 Actor 분기에서 key를 만들 때 같은 typed kind를 지정하고 :4225에서 record로 전달한다. :4124의 logical multicast는 Node이며 :3815의 boundSessionSend는 send.actor를 소유자로 하는 기존 gated queue이므로 Actor로 명시했다. R/mesh/mesh_node_host_service.cpp:2352의 hosted 재게시와 :4060의 local Node 제출은 ready_record_t.owner_kind를 복사한다. R/stateful/public_host_runtime.cpp:9120의 local stateful 게시는 dispatch.owner.owner_kind를 복사한다. R/channels/channel_host_service.cpp:304와 R/client_server/raw_client_server_owner.cpp:509, :660의 정상·거부 Channel record는 Channel을 지정한다. Owner key를 해석하여 kind를 복구하는 코드는 추가하지 않았다.

새 테스트 T/test_cpp_framework_manual_dispatch_owner.cpp:155는 opaque key의 Node·Channel을 concurrent로, node라는 key의 Spot과 channel: 접두사가 있는 Actor를 gated로 확인하도록 작성했다. Queue 삭제 후 ready callback의 kind 보존도 확인한다. 같은 파일 :204의 테스트는 kind 누락과 queue 내 kind 충돌이 prepare·admitted 및 pending 회계에 반영되지 않고 거부되는지 확인한다. 이 테스트들은 작성만 했으며 실행하지 않았다.

기존 assertion과 음성 대조의 기대값·예산·분기는 유지했다. FIFO 비용 reference의 ready 원소에도 kind를 넣었다. std::list가 sentinel을 할당하는 ABI에서도 같은 typed 원소의 이동 비용을 비교하기 위한 변경이며 allocation count·bytes assertion은 그대로다. Stateful fixture는 object kind에서 한 번 정한 owner kind로 key와 record를 함께 만든다. 일반 mailbox 상태 전이 테스트의 임의 key에는 Spot을 명시하여 기존 gated 경로를 유지했다.

소유 계층은 C++ Framework service mailbox와 application owner scheduler다.
스펙 근거는 framework/doc/framework/common/spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md §2, 특히 :97의 Node·Channel gate 부재와 :94의 stateful gate 순서다.
교차언어는 읽기만 했다. .NET Runtime/Execution/ZLinkSerialExecutionQueue.cs:1470은 source 생성 시 maximumConcurrentTurns를 지정하고 ZLinkProcessExecutionPool.cs:443에서 적용한다. Java runtime/internal/service/ZLinkServiceMailboxScheduler.java:24는 owner kind와 key를 별도 값으로 보존하며 runtime/channels/ZLinkHandlerDispatch.java:46은 시작과 terminal을 구분한다. Kotlin zlink-framework-kotlin/build.gradle.kts:13은 Java core를 공유한다. 이번 변경은 C++ key 표현에 대한 의존을 제거하며 공통 실행 계약을 바꾸지 않는다.
변경 분류는 B, 기존 결함 수정이다. Spec 수정과 새 public API는 없다.

수정 범위와 호출 경로 점검에서는 문자열 판정 의존 1건을 고쳤다. 추가 실행 flag·gate 우회 동기화·중복 classifier는 없으며 이월 항목은 없다.

bash scripts/format/format.sh --check cpp는 종료 코드 0으로 통과했다. git diff --check도 통과했다. 최종 diff를 줄별로 읽고 이름·선언·인자 순서·include·callback 인자 수를 대조했다. Drain source의 pending/take/run/failed virtual override와 manual scheduler adapter는 기존 signature를 유지한다. 정적 검사에서 괄호 불일치 0건이며 기존 assertion 7,084개가 새 kind 전달 인자를 제외하고 유지됨을 대조했다. CMake 등록, expect_scheduler_rejection.cmake, mailbox_owner_dispatch 테스트와 manual_internal_scheduler.hpp는 HEAD와 동일하다. 로그는 format-check.log와 static-review.log다. 남은 검증은 PM의 컴파일과 관련 테스트·기존 음성 대조 실행이다. Runtime 테스트 통과를 주장하지 않는다.

변경 파일과 모든 변경 블록의 시작 위치는 다음과 같다. 아래 목록에는 factory 인자와 callback signature의 기계적 변경도 포함한다.

R/channels/channel_host_service.cpp:166, 304을 변경했다.

R/client_server/raw_client_server_owner.cpp:509, 660을 변경했다.

R/mesh/mesh_node_host_service.cpp:2128, 2133, 2145, 2352, 3416, 3420, 3424, 3427, 3935, 3953, 4060을 변경했다.

R/mesh/mesh_node_host_service.hpp:161, 198을 변경했다.

R/mesh/raw_mesh_node_owner.cpp:1847, 2838, 2892, 3027, 3639, 3643, 3815, 4124, 4144, 4161, 4182, 4184, 4199, 4201, 4221, 4225을 변경했다.

R/mesh/service_mailbox.cpp:140, 154, 170, 176, 199, 234, 239, 241, 269, 297, 573, 628, 672을 변경했다.

R/mesh/service_mailbox.hpp:169, 193, 205, 215, 223, 330, 344, 347, 372, 456, 468을 변경했다.

R/stateful/public_host_runtime.cpp:9120을 변경했다.

T/test_cpp_framework_actor_maintenance_join.cpp:3611, 4608, 4619, 4627, 4631을 변경했다.

T/test_cpp_framework_claim_dispatch_cost.cpp:116, 347, 386, 445, 491, 515, 552, 656, 747, 808, 861, 885을 변경했다.

T/test_cpp_framework_client_server_runtime.cpp:455을 변경했다.

T/test_cpp_framework_execution.cpp:1693을 변경했다.

T/test_cpp_framework_handler_entry_permit.cpp:105, 109, 156, 270을 변경했다.

T/test_cpp_framework_instance_spot_activation.cpp:1406을 변경했다.

T/test_cpp_framework_local_enqueue_allocations.cpp:798, 929, 1188을 변경했다.

T/test_cpp_framework_m6a_runtime.cpp:1652, 1677, 1691, 1756, 1762, 1765, 1769, 1778, 1781, 1784, 1787, 1813, 1818, 1831, 1840, 1843을 변경했다.

T/test_cpp_framework_m6b_runtime.cpp:1921, 2037, 2126, 4132, 4682, 5569, 5636, 6076, 6261, 6285, 6448, 6532, 7038, 7200, 7209, 9653을 변경했다.

T/test_cpp_framework_m6c_runtime.cpp:4721, 4961을 변경했다.

T/test_cpp_framework_mailbox_fifo_cost.cpp:29, 58, 62, 66, 80, 115, 137을 변경했다.

T/test_cpp_framework_maintenance_failure.cpp:98을 변경했다.

T/test_cpp_framework_manual_dispatch_owner.cpp:30, 155, 250, 257을 변경했다.

T/test_cpp_framework_mesh_node_vertical.cpp:687, 850, 894을 변경했다.

T/test_cpp_framework_receive_allocations.cpp:62, 371, 397을 변경했다.

T/test_cpp_framework_receive_dispatch_cost.cpp:178을 변경했다.

T/test_cpp_framework_receive_record.cpp:345, 564, 582을 변경했다.

T/test_cpp_framework_relocation_seal_contract.cpp:205, 359, 1597, 1647, 1677, 1724, 2246, 2920, 3490, 3577, 3612을 변경했다.

T/test_cpp_framework_reply_submit_claim.cpp:125, 144, 221, 229, 290, 295, 330, 378, 456, 594, 626을 변경했다.

T/test_cpp_framework_route_payload_ownership.cpp:587, 811을 변경했다.

T/test_cpp_framework_scheduler_drain.cpp:20, 151, 158, 188, 258, 279, 302, 343, 365, 371, 375, 475, 482을 변경했다.

T/test_cpp_framework_send_allocations.cpp:925을 변경했다.

U/stateful_ingress_fixture.hpp:25, 32을 변경했다.

change.diff는 위 HEAD에 대한 git diff HEAD 전체이며 이번 수정만 담았다.
