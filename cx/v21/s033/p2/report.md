이번 작업은 차단 상태다. 2단계 구현을 완료하지 않았다. 검증 전 초안에서 계약 위반을 확인해 전부 걷어냈고, worktree는 26d5c811c7과 같은 상태다. change.diff는 0 byte다.

C는 worktree의 framework/languages/cpp/framework/src/runtime/다. S02·S04·S08은 framework/doc/framework/common/spec/server/01-execution/의 해당 공통 spec이다.

0. cdrain과의 차이와 중단 근거

코드를 쓰기 전에 claim·batch·settlement를 없애는 차이를 기록했다. 그 기록은 raw/report-before-review.md에 보존했다. 이후 S04의 wake 계약을 대조하니, 그 차이를 구현한 초안은 허용된 경로가 아니었다.

현재 branch의 ready callback은 C/mesh/mesh_node_host_service.cpp:3359에서 owner 작업을 만들고 :3360에서 executor에 제출한다. C/dispatch/offload_executor.cpp:105는 work item을 넣고 :107은 notify_one을 호출한다. Worker는 :365에서 item을 가져와 :366에서 queue에서 제거한다. C/dispatch/offload_executor.hpp:102의 work 종류는 일회성 함수 두 종류다. 같은 Node owner를 ready 상태로 유지해 여러 worker가 재사용하는 persistent drain entry는 없다.

따라서 head를 꺼낼 때마다 기존 callback을 다시 부르면 N건의 backlog에 대해 최대 N개의 executor item과 알림을 만든다. Claim 할당을 없애도 이 추가 작업을 절감으로 셀 수 없다. S04 3절 :209–218은 아직 예약되지 않은 idle worker의 가용 조건과 알림 병합을 요구하고 record별 dispatch task를 금지한다. S08 4.3절 :243–247도 work item 하나가 여러 record를 처리하도록 정한다. 초안은 이 두 조건을 위반했다.

반대로 callback을 추가로 부르지 않으면 같은 publication의 A와 B 중 A가 동기로 막혔을 때 B를 가져갈 worker가 예약되지 않는다. 이는 S02 2절 :97–104의 독립 Node/Channel 실행 조건을 만족하지 못한다. Handler 종료나 suspension까지 기다리는 방법도 사용하지 않았다.

cdrain/report.md:25–34에는 persistent source 유지, idle worker 등록과 wake 예약, worker budget 이동이 명시되어 있다. 특히 :32는 record별 executor item 제출을 탈락시켰다. 이 scheduler 변경은 현재 branch에 없다. 승인 설계 cpath/report.md:301은 batch publication과 idle wake 예약을 7단계에 둔다. 이번 지시는 wake 규칙과 W5 변경을 금지하고 2단계만 허용한다. 이 경계를 넘지 않고 두 실행 조건을 함께 만족하는 경로를 확인하지 못했으므로 중단했다. Spec 자체가 비어 있는 문제는 아니다.

| 비교 | cdrain의 설명 | 검토한 2단계 초안 | 판정 |
|---|---|---|---|
| wake와 drain | persistent source와 idle wake 예약을 사용한다. cdrain/report.md:25–34 | 기존 ready callback을 head마다 다시 제출한다. C/mesh/mesh_node_host_service.cpp:3360 → C/dispatch/offload_executor.cpp:105·107 | record별 task와 알림이 늘어 허용하지 않았다. |
| mailbox lock | 독립 take 뒤 claim credit을 settle한다. cdrain/report.md:34 | head take 한 번을 남기고 마지막 prefix settlement lock을 없애려 했다. 현재 반환 lock은 C/mesh/service_mailbox.cpp:448이다. | 이 부분의 삭제만으로 wake 계약을 충족하지 못한다. |
| 메시지별 할당 | 독립 record를 single-record claim이 소유한다. cdrain/report.md:34 | claim·배열·host batch와 별도 payload owner를 없애려 했다. | 새 executor item·closure·drain frame 호출 비용을 함께 없애는 근거가 없어 전체 할당 감소를 주장하지 않는다. |

2단계를 재개하려면 persistent owner drain과 idle wake 예약을 갖춘 기준 branch가 필요하거나, 그 scheduler 경계까지 수정 범위에 포함해야 한다. Worker 확대, retry, timeout 변경, coroutine의 callback 치환과 spec 수정은 하지 않았다.

1. 변경

제품 변경은 없다. 초안에서 추가했던 queue-head 이전, terminal의 record 보유, canonical record용 dispatcher 진입과 borrowed native part 처리, 테스트를 모두 제거했다. 기존 사용자 변경은 없었으며, 제거 뒤 git status와 기준 diff를 raw/status-after-prototype-removal.txt 및 change.diff에 기록했다.

unvalidated-prototype.diff와 prototype-scripts.tar.gz는 폐기한 검증 전 초안이다. 적용 가능한 결과나 통과한 구현으로 제시하지 않는다. 실제 시험 프로세스와 build 디렉터리는 만들지 않았다.

2. 결정 소유 표와 개수

| 결정 | 소유 조항 | 현재 판정 위치 | 합칠 소유자와 현재 결과 |
|---|---|---|---|
| Node/Channel 실행 소유 | Framework FIFO. S02 2절 :97–104 | C/mesh/service_mailbox.cpp:212·235·511 | FIFO head take 한 곳이 목표다. 실제 3→3이며 요구한 3→1을 달성하지 못했다. |
| ready membership과 last-take/enqueue | Framework scheduler. S04 3절 :190–218 | C/mesh/service_mailbox.cpp:133·456·559 | record가 남은 동안 유지되는 ready owner를 scheduler가 사용해야 한다. 변경하지 않았다. |
| idle 등록·wake 예약 | Framework scheduler. S04 3절 :202–218 | C/dispatch/offload_executor.cpp:102·107·335·346 | 기존 scheduler 경계가 소유해야 한다. 이번 범위에서는 수정하지 않았다. |
| W5 | shared worker drain. S08 4.3절 :224 | C/mesh/mesh_node_host_service.cpp:3879 | 기존 budget을 유지했다. executor로 옮기지 않았다. |
| payload와 native reply token | canonical record와 binding. 설계 3절 :109 | C/mesh/service_mailbox.hpp:261의 received 저장소, C/mesh/mesh_record_dispatcher.cpp:61 | binding의 native capability를 유지했다. 새 reply 권한을 만들지 않았다. |
| Spot/Actor gate와 relocation prefix | 기존 stateful owner. S02 2·3절 | C/mesh/service_mailbox.cpp:225·322·456 및 기존 serial queue | 그대로 유지했다. |

교차언어 대조는 읽기만 했다. .NET의 Runtime/Spots/ZLinkMeshNodeRouteDispatcher.cs:220·247은 기존 claim turn에서 invocation을 시작하고 비동기 결과를 따로 보유한다. Java의 runtime/channels/ZLinkHandlerDispatch.java:16·59는 queue와 claim 완료 연결을 사용한다. Kotlin은 zlink-framework-kotlin/build.gradle.kts:13에서 Java core runtime을 공유한다. 이 차이로 C++의 per-record executor task를 정당화하지 않았다.

변경 분류는 기존 C++ 실행 경로의 계약 적응 A와 중복 소유 제거 B가 목표였다. 폐기한 초안의 record별 executor 재제출은 C에 해당하며 전달 결과에 포함하지 않았다. 새로운 spec 동작 D는 제안하지 않았다.

K는 한 prefix의 record 수다. 정적으로 확인한 기존 할당 요청은 claim shared block 1회, records vector 1회, record_bytes vector 1회, settled 배열 1회로 4/K회/message다(C/mesh/service_mailbox.cpp:240·244·245·265). Host batch는 TLS 저장소가 없거나 보유 중일 때 추가로 할당한다(C/stateful/public_host_runtime.cpp:796–802). 정상 record별 prefix settlement RMW는 :429의 exchange와 :434의 fetch_sub 두 곳이다. 실제 최종 변경은 없어 이 수는 모두 전후 동일하다. 전체 allocation·native heap·lock 호출 수를 계측한 값은 아니다.

3. 테스트와 로그

빌드 0회, C++ test 0회, 음성 대조 0회, stress 반복 0회다. 따라서 pass 0·fail 0은 미실행을 뜻한다. 통과 판정이 아니다. Core gate 파일이 없어서 build slot과 measure lock을 획득하지 않았다.

Count, FIFO take, A 동기 block 중 B 진행, last-take/enqueue, queued/running shutdown, shutdown 뒤 reply, Spot/Actor gate와 lifetime 음성 대조는 초안에만 작성했다. 전부 미검증이며 worktree에 남기지 않았다. Full non-install suite와 stress-ng 부하 20회 로그는 없다.

요구된 bash scripts/format/format.sh --check cpp도 gate 전의 검사 금지 때문에 실행하지 않았다. 초안에 clang-format 쓰기만 실행한 기록은 raw/format-write.log에 있다. 이를 format check 통과로 세지 않는다. Gate 확인과 실행 횟수는 raw/gate-status.jsonl에 보존했다.

4. 같은 rate의 target CPU

Base 26d5c811c7과 change의 3회 측정은 모두 미실행이다. User+kernel CPU µs/message와 설계 예상 0.8–1.4 µs/message 절감을 비교할 근거가 없다. csrc의 measure.py를 조정한 초안은 prototype-scripts.tar.gz에 보존했으며 실행하지 않았다.

5. Spot p99와 send throughput

1024-byte Spot request p99와 send-saturation throughput의 base/change 3회 측정은 없다. 중앙값과 run-to-run spread도 없다. 성능 회귀나 성능 통과를 판정하지 않는다. 이 작업은 구현과 필수 검증을 완료하지 못했으므로 PR 통과 상태가 아니다.

6. 리팩토링 점검

| 항목 | 현재 위치 | 최종 제거 여부 |
|---|---|---|
| prefix shared claim | C/mesh/service_mailbox.cpp:240 | 제거하지 않았다. |
| records·record_bytes·settled·remaining | C/mesh/service_mailbox.cpp:244·245·265·266 | 제거하지 않았다. |
| prefix settlement RMW와 마지막 반환 lock | C/mesh/service_mailbox.cpp:429·434·448 | 제거하지 않았다. |
| host batch와 retained flags | C/stateful/public_host_runtime.cpp:716·734·796 | 제거하지 않았다. |
| claim→host batch handoff | C/stateful/public_host_runtime.cpp:8535·8545 | 제거하지 않았다. |
| mesh dispatcher·route dispatcher·reply closure | C/mesh/mesh_record_dispatcher.cpp:32·53·61 | 제거하지 않았다. 3단계 객체 변경도 수행하지 않았다. |
| receive_record→route received wrapper | C/mesh/mesh_record_dispatcher.cpp:69 | 제거하지 않았다. |
| native payload를 보유하는 중간 wrapper | C/protocol/service_wire_codec.cpp:3383·3403 | 제거하지 않았다. |
| record별 재제출의 위험 | C/mesh/mesh_node_host_service.cpp:3360, C/dispatch/offload_executor.cpp:105·107 | 초안에서 발견해 적용을 취소했다. |

점검 9항목, 제품 수정 0항목이다. 실행 배타 판정은 3→3이고, 메시지별 할당도 최종 전후 동일하다. Queue, flag, timer, lock이나 coroutine 변경을 남기지 않았다.
