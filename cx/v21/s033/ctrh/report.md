# C++ target 수신 역할 인계 구현

`/root/worktree/zlink-3867x`에서 승인된 target-only 첫 대안을 구현했다. 사용자가 지정한 branch는 `perf/3867-cpp-receive-handoff`이며 기준은 `00f9963cc9`다. git 명령은 실행하지 않았다. 새 worktree에서 모든 target 빌드, unit·contract 299개와 전체 C++ format 검사를 통과했다. 남은 실패는 없다. 성능 측정은 감독자가 수행한다.

소유 계층은 Framework의 실행 자원 배치다.

명세 근거는 S08 `08-messaging-hot-path.ko.md` §4.4, S01 `01-submit-and-completion.ko.md` §11, S02 `02-handler-turn-and-execution-gate.ko.md` §1–3·§13이다.

교차언어 대조 결과, .NET `ZLinkProcessExecutionPool.cs:389–402,578–592`는 같은 잠금에서 idle worker를 제거하고 Receiving으로 배정한 뒤 이전 자원이 application 작업을 실행한다. Java `ZLinkJavaRawMeshNode.java:5385`는 전용 receive executor를 사용하고 `ZLinkProcessExecutionLanes.java:70`에서 application 작업을 별도로 게시한다. Kotlin은 같은 Java runtime을 사용한다. Node `node-raw-mesh-backend.ts:1800–1830`는 event loop에서 수신을 진행한다. 공통 계약은 유지하며 C++의 선택적 배치만 바꿨다.

변경 분류는 A(기존 계약 적응)다. 선택 사항인 인계가 없던 기존 게시 경로를 기존 결함으로 분류하지 않았다. 명세를 수정하지 않았다.

`node_receive_role_t::rotate`는 기존 receive turn, claim, permit 정리와 관리를 끝내고 기존 ready batch를 반환한다. `run`까지 반환된 뒤 executor가 한 번 배정을 판단한다. 기존 잠금 안에서 idle 목록의 worker 하나를 제거하고 그 worker의 role slot을 배정한다. 배정된 worker는 application queue보다 그 role을 먼저 실행한다. 성공하면 이전 자원이 기존 `make_application_owner_work`와 `drain_application_owner`로 들어간다. 실패하면 역할을 유지하며 기존 `notify_application_ready → submit_owner → try_submit_internal` 경로를 쓴다. 다른 ready owner를 먼저 게시한 뒤 선택한 owner drain을 실행한다. Record는 기존 owner queue에서 take하며 이 배치를 첫 실행 turn에 적용한다. Suspend 뒤 재개는 기존 scheduler와 gate가 소유한다.

기존 idle 수 집계를 worker별 idle 목록으로 교체했다. 별도 idle 수를 함께 유지하지 않는다. 다음 readiness는 run stack에서 receive 역할 객체로 옮겼다. 기존 ready batch와 queue를 재사용하며 새 mailbox, pending 표, generation, retry, timer를 추가하지 않았다. 인계 배정 자체는 worker를 생성하거나 시작을 기다리지 않는다. 기존 application 게시의 worker 확장 정책과 상한은 유지한다.

배정 시 기존 active 집계에 다음 receiver의 수명을 포함하고 이전 자원의 active 항목은 application 작업으로 이어 쓴다. 따라서 배정 직후 stop이 시작해도 application drain이 먼저 끝났다고 판정하지 않는다. 기존 stop 순서인 application drain, receive role drain, unbind를 유지한다. 비동기 drain의 물리 수명도 기존 retained work가 소유한다.

## 검증

RED는 핵심 executor header·구현, host 구현과 기존 검사가 모두 같은 SHA-256인 `/root/worktree/zlink-3867h`의 기존 실행 파일로 확인했다. `red-provenance.json`에 소스와 실행 파일의 해시를 기록했다. 기존 128건 단일 배달, blocked handler와 독립 owner·completion 진행은 통과했지만 `handler_on_receive_thread=0`이었다. 새 조건인 1을 요구한 검사는 exit 1로 실패했다. 이 실행 파일을 이번 변경의 GREEN 근거로 사용하지 않는다. 새 unit 검사도 기존 인터페이스에 배정 상태와 회전 반환이 없어 컴파일 RED가 됐다.

Focused 검사 네 개가 모두 통과했다. `offload_executor` 검사는 idle 배정, idle 없음, blocked application과 독립 작업, 배정 직후 stop, 예약된 receiver의 application 배제와 기존 worker 수 유지를 확인했다. 실제 target 검사는 수신한 자원에서 handler를 시작하고 다른 자원에서 후속 수신을 진행했다. 막힌 handler 앞에서 128건을 모두 수신한 뒤 FIFO로 한 번씩 배달했으며 다른 owner와 completion도 먼저 진행했다. 준비된 회전 3회에서 시도 3회와 성공 2회를 관찰했고 종료 뒤 실행 중인 receive 역할은 0이었다.

Idle이 없는 target 검사는 준비된 회전 1회, 시도 1회, 성공 0회였으며 다른 worker에서 handler를 실행했다. 배정 직후 통지 전 stop 검사는 admitted record 1건을 한 번 배달하고 역할과 소유 thread를 모두 drain했다. Handler가 이미 실행 중일 때 stop하는 경우도 같은 target 검사에서 통과했다. 모든 handler는 receive 역할 밖이며 state lane-current가 비어 있음을 확인했다.

의도적으로 idle 목록이 비어 있어도 배정 성공을 반환하게 바꾼 실행은 `!handed_off` assertion에서 거부됐다. `negative-test.log`의 종료 값은 -6이다. 검사 직후 원래 executor 소스를 복원했다. 단순 게시나 성공 반환만으로 독점 배정이 됐다고 판단하는 오류를 검사가 구별한다.

최종 검증 결과는 다음과 같다.

- `cmake --build --preset linux-ninja-release --parallel 3 -- -k 0`: 모든 target 빌드 성공. 마지막 정리 변경까지 반영해 재연결을 완료했다. 빌드는 지정된 shared measure lock과 build-slot 스크립트 안에서 실행했다.
- 최종 focused 네 검사: 4/4 통과, 0.15초. `final-focused-tests.log`에 실제 target의 thread·배정·drain 관찰값을 보존했다.
- `ctest --preset linux-ninja-release -L '^(unit|contract)$'`: 299/299 통과. unit 229개, contract 70개이며 전체 176.77초였다. `request_observer_lifetime`, `local_enqueue_allocations`, 기존 mailbox 단일 배달 검사를 포함한다. 최종 전체 suite는 한 번 실행했다.
- `scripts/format/format.sh --check cpp`: 통과.
- 의도적 잘못된 배정을 거부한 뒤 원본을 복원한 executor 검사도 다시 통과했다. 최종 빌드 산출물에는 정상 구현이 들어 있다.

남은 test 실패는 없다. `changes.diff`에 여섯 파일의 diff를, `changed-files.json`에 수정 전후 소스 해시를, `green-artifacts.json`에 최종 library와 두 검사 실행 파일의 해시를 보존했다.

배포된 Core와 C++ binding은 모두 1.23.0이다. 같은 release preset에 기존 package 경로 `/root/.cache/zlink/core/1.23.0/linux-x64`와 `/root/.cache/zlink/zlink-cpp/1.23.0`를 지정했다. Core와 binding 소스는 수정하지 않았으며 Core C header를 직접 읽지 않았다. benchmark, GitHub 작업, 문서 동기화, 다른 모델과 sub-agent 실행은 하지 않았다.

공유 빌드 경합에서 MemAvailable이 10.00GiB까지 내려간 표본을 확인했다. 이 작업의 컴파일 프로세스만 SIGTERM으로 중단했다. 12GiB 하한을 전 구간에서 지켰다는 주장은 하지 않는다. 이후 두 build slot이 겹치지 않게 추가 잠금을 사용하고, `--parallel 3` 명령 아래 CMake compile pool을 2로 제한했다. runtime의 worker 한도는 변경하지 않았다.

수정 전/후 규칙 수는 자원 배정 1→1이며 queue, gate, claim, permit, owner 순서, Yield, terminal, shutdown의 판정자도 각각 1→1이다.

리팩토링 점검: 성능 1건, POSDDD 0건, 불필요 코드 1건 발견. 고친 항목은 target owner drain의 필수 자원 전달 배치와 local drain의 불필요한 std::function 변환이다. 넘긴 항목은 없다.

| 결정 | 수정 전 소유자 | 수정 후 소유자 |
|---|---|---|
| 수신·application 자원 배정과 빈 자원 판정 | `offload_executor`의 idle 수와 worker loop | 같은 executor의 잠금, idle 목록과 worker role slot. 1→1 |
| 초기 receive 배정과 후속 인계 | 같은 executor의 role 게시와 worker loop. 후속 인계 없음 | 같은 `assign_idle_role_locked`와 worker loop. 초기 미배정 role은 기존 queue 사용. 배정 소유자 1→1 |
| 회전, poller, cursor와 다음 readiness | `node_receive_role_t::run/rotate`와 역할 객체, run의 readiness 지역 변수 | 같은 역할 객체. readiness도 역할 객체에 보존. 1→1 |
| ready owner 목록과 FIFO claim | `service_mailbox`의 ready batch, begin drain과 take | 같은 batch, begin drain과 take. 1→1 |
| application 문맥, gate, permit, owner 순서와 Yield | 기존 `drain_application_owner`, dispatcher와 serial execution 경로 | 같은 진입점과 소유자. 각각 1→1 |
| admitted 작업과 receive의 drain 수명 | executor의 active·role active, retained work와 host stop | 같은 집계가 예약된 receiver와 이전 자원의 application 작업도 포함. 같은 stop 순서. 1→1 |
| source 완료 배정과 논리 terminal | 기존 process completion dispatcher와 operation registry | 같은 소유자. 각각 1→1 |
| 물리 completion, 재제출, reply 전송과 errno | 기존 binding·Core | 같은 소유자. 각각 1→1 |

변경 파일은 다음 여섯 개다. 경로 기준은 `framework/languages/cpp/`다.

- `framework/src/runtime/dispatch/offload_executor.hpp`
- `framework/src/runtime/dispatch/offload_executor.cpp`
- `framework/src/runtime/mesh/mesh_node_host_service.cpp`
- `tests/Zlink.Framework.UnitTests/test_cpp_framework_offload_executor.cpp`
- `tests/Zlink.Framework.UnitTests/test_cpp_framework_mailbox_owner_dispatch.cpp`
- `CMakeLists.txt`
