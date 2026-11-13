ready to compile.

기준은 26d5c811c7, branch는 perf/1639-cpp-sched-drain이다. Core verdict 파일이 아직 없다. 빌드·테스트·성능 측정·lock 획득은 모두 0회다. 컴파일 통과나 PR 수락을 뜻하지 않는다. 최종 diff는 change.diff다.

C는 framework/languages/cpp/framework/src/runtime/, T는 framework/languages/cpp/tests/다. S02·S04·S08은 framework/doc/framework/common/spec/server/01-execution/의 공통 spec이다.

0. cdrain·cdrain2와의 차이

코드를 쓰기 전에 남긴 원인·대안·결정 소유 표는 raw/report-before-final.md에 보존했다.

cdrain/report.md:11–13의 send 회귀는 −11.44%, Spot p99는 501.754→7,585.620 µs다. :90–98에서는 application worker 참여와 switches/message가 늘었다. cpath/report.md §6b는 cheap-message CPU·wake·경합 증가를 설명하지만, 개별 변경의 원인을 분리한 실험은 아니라고 명시한다. 따라서 persistent entry 자체가 회귀 원인이었다고 단정하지 않는다.

cdrain/report.md:25–34는 persistent source, take 뒤 추가 wake와 executor로 옮긴 예산을 사용했다. cdrain2/report.md:53–57은 post-take backlog 비교와 active helper 검색을 추가했지만, :9–12·165–170의 send −12.51%, Spot p99 7,538.543 µs와 application CPU 증가를 해소하지 못했다.

이번에는 C/dispatch/offload_executor.cpp:177에서 publication과 idle 전환의 예약만 판단한다. take_drain_locked(:140)에는 wake·worker 증설·backlog 비교·active helper 검색이 없다. Worker 수·상한·idle timeout의 설정은 바꾸지 않았다. 기존 elastic pool의 실행 자원 확보는 publication에서만 한다. 더 많은 worker를 절감 근거로 세지 않는다.

예산의 길이와 deadline 판정은 C/mesh/mesh_node_host_service.cpp:3946과 C/channels/channel_host_service.cpp:390에 유지했다. 최초 take 앞의 시각만 worker가 전달하므로 claim 준비 시간도 기존 예산에 포함된다. Executor에는 owner별 deadline이나 helper 회전 정책이 없다.

Claim·settlement·host batch는 유지했다. C/mesh/service_mailbox.cpp:244는 기존 list node를 독립 claim에 옮기며 payload를 복사하지 않는다. Invoker와 owner drain coroutine도 유지했다. received_t·native reply token은 기존 binding 경로를 사용한다. record마다 executor item을 제출하는 대안은 사용하지 않았다.

성능 통과는 미확인이다. 독립 claim은 record마다 생기므로 claim의 shared block·records vector·record_bytes vector·settled 배열 할당은 base의 prefix당 4회에서 record당 4회가 된다. 이 비용을 줄였다고 주장하지 않는다. 이번 범위에서 claim 제거를 하지 않는 조건에 따른 비용이며, 실제 send·Spot·Core I/O 측정에서 회귀하면 수락 실패다.

1. 변경

C/dispatch/offload_executor.cpp:110은 group·owner가 같은 entry의 중복 제출을 합친다. :140은 최초 take와 W5 후속 take의 queue membership을 함께 갱신한다. :177은 idle·예약·실행 상태를 확인하고 :232가 lock 밖에서 알린다. Receive role은 application idle 자원으로 세지 않는다.

C/mesh/service_mailbox.cpp:244는 FIFO head 하나를 claim으로 옮긴다. 앞 handler의 terminal이나 owner 전체의 drain 종료를 기다리지 않는다. :543의 독립 claim settlement는 owner를 다시 제출하거나 깨우지 않는다. Spot·Actor의 기존 prefix·seal·gate 경로는 유지했다.

C/mesh/mesh_node_host_service.cpp:3419와 C/channels/channel_host_service.cpp:168은 Node·Channel의 ready callback을 persistent source에 연결한다. 기존 coroutine·drain은 초기 claim과 후속 claim을 같은 scheduler take에서 받는다. Source·prepared claim의 수명은 물리 작업 credit보다 먼저 정리한다.

수정한 파일은 16개다.

- framework/languages/cpp/CMakeLists.txt
- C/dispatch/offload_executor.cpp
- C/dispatch/offload_executor.hpp
- C/mesh/service_mailbox.cpp
- C/mesh/service_mailbox.hpp
- C/mesh/mesh_node_host_service.cpp
- C/mesh/mesh_node_host_service.hpp
- C/stateful/public_host_runtime.cpp
- C/stateful/public_host_runtime.hpp
- C/channels/channel_host_service.cpp
- T/Zlink.Framework.UnitTests/test_cpp_framework_scheduler_drain.cpp
- T/Zlink.Framework.UnitTests/test_cpp_framework_mailbox_owner_dispatch.cpp
- T/Zlink.Framework.UnitTests/test_cpp_framework_manual_dispatch_owner.cpp
- T/Zlink.Framework.UnitTests/test_cpp_framework_offload_executor.cpp
- T/support/manual_internal_scheduler.hpp
- T/support/expect_scheduler_rejection.cmake

core/·bindings/·spec은 바꾸지 않았다. Commit·push·GitHub 작업은 하지 않았다.

2. 결정 소유 표와 개수

아래 개수는 hosted Node·Channel의 판정 위치다. Gated·standalone 경로와 shutdown broadcast는 제외한다. 수정 전 위치는 기준 revision의 위치다.

| 결정 | 소유 조항 | 수정 전 | 수정 후 |
|---|---|---|---|
| 독립 record 실행 소유 | S02 §2:97–104, S08 §4.3 | C/mesh/service_mailbox.cpp:212·235·511의 owner 배타 판정 3곳 | 같은 파일 :244의 head claim 1곳 |
| persistent ready membership | S04 §3:190–218 | 같은 파일 :133·456·559의 enqueue·settlement·drain 종료 3곳 | C/dispatch/offload_executor.cpp:110·140의 등록·take 2곳 |
| 추가 worker wake 자격 | S04 §3:209–218 | 같은 파일의 internal·public·role 제출 :107·168·180, 3곳 | :177의 예약 판정 1곳. 전달은 :232에서 한다. |
| W5 예산 | S08 §4.3 | C/mesh/mesh_node_host_service.cpp:3879, C/channels/channel_host_service.cpp:384, 2곳 | 각각 :3946·390, 2곳. 기존 10 ms다. |
| permit 반환 | S04 §3, S08 W3 | C/dispatch/application_job_queue.hpp의 handler_entry_t 1곳 | 같은 소유자 1곳 |

수정 전/후 규칙 수는 12→7이다. 이는 위 판정 위치의 합이며 전체 저장소 규칙 수가 아니다. 새 worker phase는 기존 scalar idle 수와 공통 ready CV를 대체한다. 준비 중인 thread도 같은 예약 상태를 사용한다. 별도 owner-running 수·generation·backlog 비교 상태·timer·record notification queue는 추가하지 않았다. Claim의 list는 원래 FIFO node의 저장 위치를 옮긴 것이며 복제한 대기열이 아니다.

N건이 이미 게시된 한 owner burst, 사용 가능한 fixed worker W개를 검사한다. 이번 상한은 누적 executor item 1개, wake W회 이하다. N=1,024·W=4이면 item/message=1/1,024, wake/message≤4/1,024다. record별 제출 음성 대조는 N개 item, record별 알림 음성 대조는 N회 이상의 notify를 발생시킨다. 현재 base의 prefix drain은 K와 publication·terminal 시점에 따라 개수가 달라지므로, base 실측을 N개였다고 적지 않는다. 실제 base/change 수치는 미측정이다. 여러 publication이 중간에 queue를 비우는 경우를 이 단일 burst 상한과 혼동하지 않는다.

교차언어 대조는 읽기만 했다. .NET Runtime/Execution/ZLinkProcessExecutionPool.cs:348–397·438–449는 pool이 idle·ready를 소유하고 application.Run을 lock 밖에서 호출한다. Java runtime/channels/ZLinkHandlerDispatch.java:16–63은 serial queue 진입과 비동기 terminal을 구별한다. Kotlin zlink-framework-kotlin/build.gradle.kts:13은 Java core runtime을 공유한다. 공통 계약을 변경하지 않았고 명시적으로 승인된 C++ 범위만 수정했다.

소유 계층은 C++ Framework mailbox와 application executor다.
근거 조항은 S02 §2, S04 §3, S08 §4.3이다.
교차언어 결과는 위 읽기 전용 대조이며 다른 언어의 구현을 변경하지 않았다.
변경 분류는 A, 승인된 공통 실행 계약에 대한 C++ 적응이다.

3. 테스트와 음성 대조

실행한 runtime 테스트는 0회다. Pass 0·fail 0은 미실행이며 통과 수가 아니다. Full unit·contract non-install suite와 stress-ng 부하 20회도 실행하지 않았다. 첫 빌드와 실행은 PM 검증 대상이다.

새 test_cpp_framework_scheduler_drain은 linux-ninja-release preset의 기존 TEST_PROBES=ON·INSTALL_FRAMEWORK=OFF 구성에서 등록한다. 실제 executor queue 삽입의 누적 counter를 사용하며 worker CV의 notify_one을 link wrapper로 센다. Counter는 probe가 꺼진 측정 빌드에는 없다.

| 작성한 검사 | 같은 oracle의 음성 대조 | 현재 상태 |
|---|---|---|
| T/Zlink.Framework.UnitTests/test_cpp_framework_scheduler_drain.cpp:163의 1,024건 burst. item=1·wake≤4, FIFO take·정확히 한 번 처리 | --negative-items는 실제 internal item을 record마다 제출한다. --negative-wakes는 예약 없이 같은 worker에 반복 알림을 보낸다. | 양쪽 미실행 |
| :226의 Node·Channel A 동기 block 중 B가 다른 worker에서 완료 | --negative-terminal은 A terminal 뒤에만 다음 entry를 게시한다. | 양쪽 미실행 |
| :260의 scheduled entry, running handler, queued 64건 shutdown·permit 반환 | 기존 terminal·capture 수명 검사와 함께 실행할 대상이다. | 미실행 |
| :308의 owner별 1,000건 publication/last-take 경쟁·FIFO·중복 검사 | record별 제출·terminal 대조와 함께 실행할 대상이다. | 미실행 |
| :359의 Spot·Actor gate 직렬화와 독립 worker 진행 | --negative-spot-gate·--negative-actor-gate가 gate를 거치지 않고 같은 B를 executor에 넣는다. | 양쪽 미실행 |

음성 대조 CTest는 T/support/expect_scheduler_rejection.cmake를 통해 oracle의 exit 1만 수락한다. Exception의 exit 2·crash·timeout은 음성 대조 성공으로 처리하지 않는다.

T/Zlink.Framework.UnitTests/test_cpp_framework_mailbox_owner_dispatch.cpp:214는 실제 native Node ingress에서 A가 막힌 동안 같은 Node의 128건이 완료되는지 검사한다. 기존 Node 직렬 실행·handler 진입 FIFO assertion은 S02 §2와 충돌하여 정확히 한 번 처리로 바꾸었다. Mailbox take FIFO는 새 검사에서 확인한다.

T/Zlink.Framework.UnitTests/test_cpp_framework_manual_dispatch_owner.cpp:558은 다중 worker의 pipeline 진입 순서를 S02 §2에 맞게 구별한다. 수동 single-worker의 기존 순서, permit·scope·reply·terminal 수명 assertion은 유지한다. Filter 해제 후 1번 handler가 마지막에 진입한다는 barrier도 유지했다. 기존 test_cpp_framework_execution.cpp와 test_cpp_framework_shared_serial_turn.cpp의 Spot·Actor gate 검사는 수정하지 않았다. 기존 gate 검사의 구현 제거 음성 대조를 실행했다고 주장하지 않는다.

bash scripts/format/format.sh --check cpp는 통과했다. 로그는 raw/format-check-budget-final.log다. 최종 diff를 줄별로 다시 읽고 14개 변경 C++ 파일의 (), [], {}를 셌다. 불일치는 0개다. 변경 구간의 호출·선언 후보 625개의 인자 inventory는 raw/call-arguments-budget-final.tsv에 있다. Owner drain 12개, source start 5개, host dispatch 5개, Channel drain 5개와 각 기본 인자를 대조했다. raw/call-review-budget-final.log·raw/budget-boundary-review.diff는 이 검토의 근거이며 컴파일을 대체하지 않는다.

4. 측정

측정은 0회다. Base 26d5c811c7과 change의 Spot request p99 1024, send-saturation 1024 throughput, target Core I/O TID별 schedstat µs/message의 3회 중앙값과 spread는 없다. 성능 통과·회귀 없음 판정은 하지 않는다.

verdict 생성 후 테스트가 통과해야 측정할 수 있다. 측정은 지시된 flock -o·MemAvailable 12 GiB·source 0–7/target 8–15 조건을 따른다. 중앙값이 run-to-run spread를 넘어 악화하면 실패다. 현재 claim 비용 증가와 지속 publication의 worker·경합 비용도 그 판정 대상이다.

5. 리팩토링 점검

| 점검한 경로 | 이번에 합치거나 제거한 것 |
|---|---|
| ready callback: C/mesh/mesh_node_host_service.cpp:3419, C/channels/channel_host_service.cpp:168 | 두 기존 publication adapter를 같은 persistent source API에 연결했다. Node·Channel terminal과 drain 종료의 재게시를 제거했다. |
| executor submit: C/dispatch/offload_executor.cpp:110 | group·owner당 queued entry를 하나로 합친다. Gated owner의 기존 internal submit은 유지한다. |
| wake: 같은 파일 :177·232 | internal·public·receive-role의 무조건 notify 3곳을 한 자격 판정과 한 전달 지점으로 합쳤다. |
| drain: 같은 파일 :140·158, C/mesh/mesh_node_host_service.cpp:3922, C/channels/channel_host_service.cpp:383 | 최초·후속 take가 같은 membership 판정을 사용한다. 기존 Node coroutine과 Channel loop를 유지하고 Channel의 중복 rejection drain을 같은 loop에 합쳤다. |

성능 2건, 결정 소유 1건, 불필요 코드 1건을 점검하여 4건을 수정했다. 별도로 넘긴 구현 결함은 0건이다. Claim 비용과 성능 수락은 미검증이다. Burst의 설계 상한은 record별 제출 대조 N item·N wake에서 persistent entry 1 item·W wake 이하이며 실측값이 아니다.

작업 폴더와 worktree 합계는 1 GiB 미만이다. Build 디렉터리와 시험 프로세스를 만들지 않았다. Raw 출력은 raw/에 보존했다. 남은 일은 PM의 첫 컴파일, 음성 대조 포함 focused tests, full non-install suite, 지정 부하 20회와 조건부 측정이다.
